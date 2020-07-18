/*
 * Copyright (c) 2019 TAOS Data, Inc. <jhtao@taosdata.com>
 *
 * This program is free software: you can use, redistribute, and/or modify
 * it under the terms of the GNU Affero General Public License, version 3
 * or later ("AGPL"), as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "os.h"

#include "tlog.h"
#include "trpc.h"
#include "tscProfile.h"
#include "tscSecondaryMerge.h"
#include "tscUtil.h"
#include "tsclient.h"
#include "tsocket.h"
#include "tscSQLParser.h"
#include "tutil.h"
#include "tnote.h"

extern void tscSetNextLaunchTimer(SSqlStream *pStream, SSqlObj *pSql);
static void tscProcessFetchRow(SSchedMsg *pMsg);
static void tscAsyncQueryRowsForNextVnode(void *param, TAOS_RES *tres, int numOfRows);

static void tscProcessAsyncRetrieveImpl(void *param, TAOS_RES *tres, int numOfRows, void (*fp)());

/*
 * Proxy function to perform sequentially query&retrieve operation.
 * If sql queries upon a super table and two-stage merge procedure is not involved (when employ the projection
 * query), it will sequentially query&retrieve data for all vnodes
 */
static void tscAsyncFetchRowsProxy(void *param, TAOS_RES *tres, int numOfRows);
static void tscAsyncFetchSingleRowProxy(void *param, TAOS_RES *tres, int numOfRows);

// TODO return the correct error code to client in tscQueueAsyncError
void taos_query_a(TAOS *taos, const char *sqlstr, void (*fp)(void *, TAOS_RES *, int), void *param) {
  STscObj *pObj = (STscObj *)taos;
  if (pObj == NULL || pObj->signature != pObj) {
    tscError("bug!!! pObj:%p", pObj);
    globalCode = TSDB_CODE_DISCONNECTED;
    tscQueueAsyncError(fp, param);
    return;
  }

  int32_t sqlLen = strlen(sqlstr);
  if (sqlLen > tsMaxSQLStringLen) {
    tscError("sql string too long");
    tscQueueAsyncError(fp, param);
    return;
  }

  taosNotePrintTsc(sqlstr);

  SSqlObj *pSql = (SSqlObj *)calloc(1, sizeof(SSqlObj));
  if (pSql == NULL) {
    tscError("failed to malloc sqlObj");
    tscQueueAsyncError(fp, param);
    return;
  }

  SSqlCmd *pCmd = &pSql->cmd;
  SSqlRes *pRes = &pSql->res;

  pSql->signature = pSql;
  pSql->pTscObj = pObj;
  pSql->fp = fp;
  pSql->param = param;

  if (TSDB_CODE_SUCCESS != tscAllocPayload(pCmd, TSDB_DEFAULT_PAYLOAD_SIZE)) {
    tscError("failed to malloc payload");
    tfree(pSql);
    tscQueueAsyncError(fp, param);
    return;
  }

  pSql->sqlstr = malloc(sqlLen + 1);
  if (pSql->sqlstr == NULL) {
    tscError("%p failed to malloc sql string buffer", pSql);
    tscQueueAsyncError(fp, param);
    free(pCmd->payload);
    free(pSql);
    return;
  }

  pRes->qhandle = 0;
  pRes->numOfRows = 1;

  strtolower(pSql->sqlstr, sqlstr);
  tscDump("%p pObj:%p, Async SQL: %s", pSql, pObj, pSql->sqlstr);

  int32_t code = tsParseSql(pSql, true);
  if (code == TSDB_CODE_ACTION_IN_PROGRESS) return;

  if (code != TSDB_CODE_SUCCESS) {
    pSql->res.code = (uint8_t)code;
    tscQueueAsyncRes(pSql);
    return;
  }

  tscDoQuery(pSql);
}

static void tscAsyncFetchRowsProxy(void *param, TAOS_RES *tres, int numOfRows) {
  if (tres == NULL) {
    return;
  }

  SSqlObj *pSql = (SSqlObj *)tres;
  SSqlRes *pRes = &pSql->res;
  SSqlCmd *pCmd = &pSql->cmd;

  if (numOfRows == 0) {
    if (hasMoreVnodesToTry(pSql)) { // sequentially retrieve data from remain vnodes.
      tscTryQueryNextVnode(pSql, tscAsyncQueryRowsForNextVnode);
    } else {
      /*
       * all available virtual node has been checked already, now we need to check
       * for the next subclause queries
       */
      if (pCmd->clauseIndex < pCmd->numOfClause - 1) {
        tscTryQueryNextClause(pSql, tscAsyncQueryRowsForNextVnode);
        return;
      }

      /*
       * 1. has reach the limitation
       * 2. no remain virtual nodes to be retrieved anymore
       */
      (*pSql->fetchFp)(param, pSql, 0);
    }
    
    return;
  }
  
  // local reducer has handle this situation during super table non-projection query.
  if (pCmd->command != TSDB_SQL_RETRIEVE_METRIC) {
    pRes->numOfTotalInCurrentClause += pRes->numOfRows;
  }

  (*pSql->fetchFp)(param, tres, numOfRows);
}

// actual continue retrieve function with user-specified callback function
static void tscProcessAsyncRetrieveImpl(void *param, TAOS_RES *tres, int numOfRows, void (*fp)()) {
  SSqlObj *pSql = (SSqlObj *)tres;
  if (pSql == NULL) {  // error
    tscError("sql object is NULL");
    return;
  }

  SSqlCmd *pCmd = &pSql->cmd;
  SSqlRes *pRes = &pSql->res;

  if ((pRes->qhandle == 0 || numOfRows != 0) && pCmd->command < TSDB_SQL_LOCAL) {
    if (pRes->qhandle == 0) {
      tscError("qhandle is NULL");
    } else {
      pRes->code = numOfRows;
    }

    tscQueueAsyncError(pSql->fetchFp, param);
    return;
  }

  pSql->fp = fp;
  if (pCmd->command != TSDB_SQL_RETRIEVE_METRIC && pCmd->command < TSDB_SQL_LOCAL) {
    pCmd->command = (pCmd->command > TSDB_SQL_MGMT) ? TSDB_SQL_RETRIEVE : TSDB_SQL_FETCH;
  }
  tscProcessSql(pSql);
}

/*
 * retrieve callback for fetch rows proxy.
 * The below two functions both serve as the callback function of query virtual node.
 * query callback first, and then followed by retrieve callback
 */
static void tscAsyncQueryRowsForNextVnode(void *param, TAOS_RES *tres, int numOfRows) {
  // query completed, continue to retrieve
  tscProcessAsyncRetrieveImpl(param, tres, numOfRows, tscAsyncFetchRowsProxy);
}

void tscAsyncQuerySingleRowForNextVnode(void *param, TAOS_RES *tres, int numOfRows) {
  // query completed, continue to retrieve
  tscProcessAsyncRetrieveImpl(param, tres, numOfRows, tscAsyncFetchSingleRowProxy);
}

void taos_fetch_rows_a(TAOS_RES *taosa, void (*fp)(void *, TAOS_RES *, int), void *param) {
  SSqlObj *pSql = (SSqlObj *)taosa;
  if (pSql == NULL || pSql->signature != pSql) {
    tscError("sql object is NULL");
    globalCode = TSDB_CODE_DISCONNECTED;
    tscQueueAsyncError(fp, param);
    return;
  }

  SSqlRes *pRes = &pSql->res;
  SSqlCmd *pCmd = &pSql->cmd;

  if (pRes->qhandle == 0) {
    tscError("qhandle is NULL");
    tscQueueAsyncError(fp, param);
    return;
  }

  // user-defined callback function is stored in fetchFp
  pSql->fetchFp = fp;
  pSql->fp = tscAsyncFetchRowsProxy;

  pSql->param = param;
  tscResetForNextRetrieve(pRes);

  if (pCmd->command != TSDB_SQL_RETRIEVE_METRIC && pCmd->command < TSDB_SQL_LOCAL) {
    pCmd->command = (pCmd->command > TSDB_SQL_MGMT) ? TSDB_SQL_RETRIEVE : TSDB_SQL_FETCH;
  }

  tscProcessSql(pSql);
}

void taos_fetch_row_a(TAOS_RES *taosa, void (*fp)(void *, TAOS_RES *, TAOS_ROW), void *param) {
  SSqlObj *pSql = (SSqlObj *)taosa;
  if (pSql == NULL || pSql->signature != pSql) {
    tscError("sql object is NULL");
    globalCode = TSDB_CODE_DISCONNECTED;
    tscQueueAsyncError(fp, param);
    return;
  }

  SSqlRes *pRes = &pSql->res;
  SSqlCmd *pCmd = &pSql->cmd;

  if (pRes->qhandle == 0) {
    tscError("qhandle is NULL");
    tscQueueAsyncError(fp, param);
    return;
  }

  pSql->fetchFp = fp;
  pSql->param = param;
  
  if (pRes->row >= pRes->numOfRows) {
    tscResetForNextRetrieve(pRes);
    pSql->fp = tscAsyncFetchSingleRowProxy;
    
    if (pCmd->command != TSDB_SQL_RETRIEVE_METRIC && pCmd->command < TSDB_SQL_LOCAL) {
      pCmd->command = (pCmd->command > TSDB_SQL_MGMT) ? TSDB_SQL_RETRIEVE : TSDB_SQL_FETCH;
    }
    
    tscProcessSql(pSql);
  } else {
    SSchedMsg schedMsg;
    schedMsg.fp = tscProcessFetchRow;
    schedMsg.ahandle = pSql;
    schedMsg.thandle = pRes->tsrow;
    schedMsg.msg = NULL;
    taosScheduleTask(tscQhandle, &schedMsg);
  }
}

void tscAsyncFetchSingleRowProxy(void *param, TAOS_RES *tres, int numOfRows) {
  SSqlObj *pSql = (SSqlObj *)tres;
  SSqlRes *pRes = &pSql->res;
  SSqlCmd *pCmd = &pSql->cmd;

  SQueryInfo *pQueryInfo = tscGetQueryInfoDetail(pCmd, pCmd->clauseIndex);
  
  if (numOfRows == 0) {
    if (hasMoreVnodesToTry(pSql)) {     // sequentially retrieve data from remain vnodes.
      tscTryQueryNextVnode(pSql, tscAsyncQuerySingleRowForNextVnode);
    } else {
      /*
       * 1. has reach the limitation
       * 2. no remain virtual nodes to be retrieved anymore
       */
      (*pSql->fetchFp)(pSql->param, pSql, NULL);
    }
    return;
  }
  
  for (int i = 0; i < pCmd->numOfCols; ++i){
    SSqlExpr* pExpr = pQueryInfo->fieldsInfo.pSqlExpr[i];
    if (pExpr != NULL) {
      pRes->tsrow[i] = TSC_GET_RESPTR_BASE(pRes, pQueryInfo, i) + pExpr->resBytes * pRes->row;
    } else {
      //todo add
    }
  }
  
  pRes->row++;

  (*pSql->fetchFp)(pSql->param, pSql, pSql->res.tsrow);
}

void tscProcessFetchRow(SSchedMsg *pMsg) {
  SSqlObj *pSql = (SSqlObj *)pMsg->ahandle;
  SSqlRes *pRes = &pSql->res;
  SSqlCmd *pCmd = &pSql->cmd;
  
  SQueryInfo *pQueryInfo = tscGetQueryInfoDetail(pCmd, pCmd->clauseIndex);

  for (int i = 0; i < pCmd->numOfCols; ++i) {
    SSqlExpr* pExpr = pQueryInfo->fieldsInfo.pSqlExpr[i];
    if (pExpr != NULL) {
      pRes->tsrow[i] = TSC_GET_RESPTR_BASE(pRes, pQueryInfo, i) + pExpr->resBytes * pRes->row;
    } else {
      //todo add
    }
  }
  
  pRes->row++;
  (*pSql->fetchFp)(pSql->param, pSql, pRes->tsrow);
}

void tscProcessAsyncRes(SSchedMsg *pMsg) {
  SSqlObj *pSql = (SSqlObj *)pMsg->ahandle;
  STscObj *pTscObj = pSql->pTscObj;
  SSqlCmd *pCmd = &pSql->cmd;
  SSqlRes *pRes = &pSql->res;

  void *taosres = pSql;

  // pCmd may be released, so cache pCmd->command
  int cmd = pCmd->command;
  int code = pRes->code ? -pRes->code : pRes->numOfRows;

  if ((tscKeepConn[cmd] == 0 || (code != TSDB_CODE_SUCCESS && code != TSDB_CODE_ACTION_IN_PROGRESS)) &&
      pSql->pStream == NULL) {
    if (pSql->thandle) taosAddConnIntoCache(tscConnCache, pSql->thandle, pSql->ip, pSql->vnode, pTscObj->user);

    pSql->thandle = NULL;
  }

  // in case of async insert, restore the user specified callback function
  bool shouldFree = tscShouldFreeAsyncSqlObj(pSql);

  if (cmd == TSDB_SQL_INSERT) {
    assert(pSql->fp != NULL);
    pSql->fp = pSql->fetchFp;
  }

  (*pSql->fp)(pSql->param, taosres, code);

  if (shouldFree) {
    tscTrace("%p Async sql is automatically freed in async res", pSql);
    tscFreeSqlObj(pSql);
  }
}

void tscProcessAsyncError(SSchedMsg *pMsg) {
  void (*fp)() = pMsg->ahandle;

  (*fp)(pMsg->thandle, NULL, -1);
}

void tscQueueAsyncError(void(*fp), void *param) {
  SSchedMsg schedMsg;
  schedMsg.fp = tscProcessAsyncError;
  schedMsg.ahandle = fp;
  schedMsg.thandle = param;
  schedMsg.msg = NULL;
  taosScheduleTask(tscQhandle, &schedMsg);
}

void tscQueueAsyncRes(SSqlObj *pSql) {
  if (pSql == NULL || pSql->signature != pSql) {
    tscTrace("%p SqlObj is freed, not add into queue async res", pSql);
    return;
  } else {
    tscError("%p add into queued async res, code:%d", pSql, pSql->res.code);
  }

  SSchedMsg schedMsg;
  schedMsg.fp = tscProcessAsyncRes;
  schedMsg.ahandle = pSql;
  schedMsg.thandle = (void *)1;
  schedMsg.msg = NULL;
  taosScheduleTask(tscQhandle, &schedMsg);
}

void tscProcessAsyncFree(SSchedMsg *pMsg) {
  SSqlObj *pSql = (SSqlObj *)pMsg->ahandle;
  tscTrace("%p sql is freed", pSql);
  taos_free_result(pSql);
}

void tscQueueAsyncFreeResult(SSqlObj *pSql) {
  tscTrace("%p sqlObj put in queue to async free", pSql);

  SSchedMsg schedMsg;
  schedMsg.fp = tscProcessAsyncFree;
  schedMsg.ahandle = pSql;
  schedMsg.thandle = (void *)1;
  schedMsg.msg = NULL;
  taosScheduleTask(tscQhandle, &schedMsg);
}

void tscAsyncInsertMultiVnodesProxy(void *param, TAOS_RES *tres, int numOfRows) {
  SSqlObj *pSql = (SSqlObj *)param;
  SSqlCmd *pCmd = &pSql->cmd;
  int32_t  code = TSDB_CODE_SUCCESS;

  assert(pCmd->dataSourceType != 0 && pSql->signature == pSql);
  
  int32_t index = 0;
  SQueryInfo* pQueryInfo = tscGetQueryInfoDetail(pCmd, index);
  
  SMeterMetaInfo* pMeterMetaInfo = tscGetMeterMetaInfoFromQueryInfo(pQueryInfo, 0);
  assert(pQueryInfo->numOfTables == 1 || pQueryInfo->numOfTables == 2);
  
  SDataBlockList *pDataBlocks = pCmd->pDataBlocks;
  if (pDataBlocks == NULL || pMeterMetaInfo->vnodeIndex >= pDataBlocks->nSize) {
    // restore user defined fp
    pSql->fp = pSql->fetchFp;
    tscTrace("%p Async insertion completed, destroy data block list", pSql);

    // release data block data
    pCmd->pDataBlocks = tscDestroyBlockArrayList(pCmd->pDataBlocks);

    // all data has been sent to vnode, call user function
    (*pSql->fp)(pSql->param, tres, numOfRows);
  } else {
    do {
      code = tscCopyDataBlockToPayload(pSql, pDataBlocks->pData[pMeterMetaInfo->vnodeIndex++]);
      if (code != TSDB_CODE_SUCCESS) {
        tscTrace("%p prepare submit data block failed in async insertion, vnodeIdx:%d, total:%d, code:%d",
                 pSql, pMeterMetaInfo->vnodeIndex - 1, pDataBlocks->nSize, code);
      }

    } while (code != TSDB_CODE_SUCCESS && pMeterMetaInfo->vnodeIndex < pDataBlocks->nSize);

    // build submit msg may fail
    if (code == TSDB_CODE_SUCCESS) {
      tscTrace("%p async insertion, vnodeIdx:%d, total:%d", pSql, pMeterMetaInfo->vnodeIndex - 1, pDataBlocks->nSize);
      tscProcessSql(pSql);
    }
  }
}

int tscSendMsgToServer(SSqlObj *pSql);

void tscMeterMetaCallBack(void *param, TAOS_RES *res, int code) {
  SSqlObj *pSql = (SSqlObj *)param;
  if (pSql == NULL || pSql->signature != pSql) return;

  SSqlCmd *pCmd = &pSql->cmd;
  SSqlRes *pRes = &pSql->res;

  if (pSql->fp == NULL) {
    tscError("%p callBack is NULL!!!", pSql);
    return;
  }

  if (pSql->fp == (void *)1) {
    pSql->fp = NULL;

    if (code != 0) {
      code = abs(code);
      pRes->code = code;
      tscTrace("%p failed to renew meterMeta", pSql);
      tsem_post(&pSql->rspSem);
    } else {
      tscTrace("%p renew meterMeta successfully, command:%d, code:%d, thandle:%p, retry:%d",
          pSql, pSql->cmd.command, pSql->res.code, pSql->thandle, pSql->retry);
  
      SMeterMetaInfo* pMeterMetaInfo = tscGetMeterMetaInfo(&pSql->cmd, 0, 0);
      assert(pMeterMetaInfo->pMeterMeta == NULL);
      
      tscGetMeterMeta(pSql, pMeterMetaInfo);
      code = tscSendMsgToServer(pSql);
      if (code != 0) {
        pRes->code = code;
        tsem_post(&pSql->rspSem);
      }
    }

    return;
  }

  if (code != 0) {
    pRes->code = (uint8_t)abs(code);
    tscQueueAsyncRes(pSql);
    return;
  }

  if (pSql->pStream == NULL) {
    // check if it is a sub-query of super table query first, if true, enter another routine
    SQueryInfo* pQueryInfo = tscGetQueryInfoDetail(pCmd, pCmd->clauseIndex);
  
    if ((pQueryInfo->type & TSDB_QUERY_TYPE_STABLE_SUBQUERY) == TSDB_QUERY_TYPE_STABLE_SUBQUERY) {
      SMeterMetaInfo* pMeterMetaInfo = tscGetMeterMetaInfoFromQueryInfo(pQueryInfo, 0);
      if(pMeterMetaInfo->pMeterMeta == NULL) {
        code = tscGetMeterMeta(pSql, pMeterMetaInfo);
        assert(code == TSDB_CODE_SUCCESS);
      }

      assert(pMeterMetaInfo->pMeterMeta->numOfTags != 0 && pMeterMetaInfo->vnodeIndex >= 0 && pSql->param != NULL);

      SRetrieveSupport *trs = (SRetrieveSupport *)pSql->param;
      SSqlObj *         pParObj = trs->pParentSqlObj;
      
      assert(pParObj->signature == pParObj && trs->subqueryIndex == pMeterMetaInfo->vnodeIndex &&
          pMeterMetaInfo->pMeterMeta->numOfTags != 0);

      tscTrace("%p get metricMeta during super table query successfully", pSql);
      
      code = tscGetMetricMeta(pSql, 0);
      pRes->code = code;

      if (code == TSDB_CODE_ACTION_IN_PROGRESS) return;
    } else {  // normal async query continues
      if (pCmd->isParseFinish) {
        tscTrace("%p resend data to vnode in metermeta callback since sql has been parsed completed", pSql);
        
        SMeterMetaInfo* pMeterMetaInfo = tscGetMeterMetaInfo(pCmd, pCmd->clauseIndex, 0);
        code = tscGetMeterMeta(pSql, pMeterMetaInfo);
        assert(code == TSDB_CODE_SUCCESS);
      
        if (pMeterMetaInfo->pMeterMeta) {
          code = tscSendMsgToServer(pSql);
          if (code == TSDB_CODE_SUCCESS) return;
        }
      } else {
        code = tsParseSql(pSql, false);
        if (code == TSDB_CODE_ACTION_IN_PROGRESS) return;
      }
    }

  } else {  // stream computing
    SMeterMetaInfo *pMeterMetaInfo = tscGetMeterMetaInfo(pCmd, pCmd->clauseIndex, 0);
    code = tscGetMeterMeta(pSql, pMeterMetaInfo);
    pRes->code = code;

    if (code == TSDB_CODE_ACTION_IN_PROGRESS) return;

    if (code == TSDB_CODE_SUCCESS && UTIL_METER_IS_SUPERTABLE(pMeterMetaInfo)) {
      code = tscGetMetricMeta(pSql, pCmd->clauseIndex);
      pRes->code = code;

      if (code == TSDB_CODE_ACTION_IN_PROGRESS) return;
    }
  }

  if (code != TSDB_CODE_SUCCESS) {
    pSql->res.code = code;
    tscQueueAsyncRes(pSql);
    return;
  }

  if (pSql->pStream) {
    /*
     * NOTE:
     * transfer the sql function for super table query before get meter/metric meta,
     * since in callback functions, only tscProcessSql(pStream->pSql) is executed!
     */
    SQueryInfo* pQueryInfo = tscGetQueryInfoDetail(pCmd, pCmd->clauseIndex);
  
    SMeterMetaInfo *pMeterMetaInfo = tscGetMeterMetaInfoFromQueryInfo(pQueryInfo, 0);   
    if ((UTIL_METER_IS_SUPERTABLE(pMeterMetaInfo) 
         && ( pMeterMetaInfo->pMeterMeta  == NULL 
           || pMeterMetaInfo->pMetricMeta == NULL 
           || pMeterMetaInfo->pMetricMeta->numOfMeters == 0 
           || pMeterMetaInfo->pMetricMeta->numOfVnodes == 0)) 
      || (!(UTIL_METER_IS_SUPERTABLE(pMeterMetaInfo))  && (pMeterMetaInfo->pMeterMeta  == NULL)))  {
      tscTrace("%p stream:%p meta is updated, but no table, clear meter meta and set next launch new query, command:%d", pSql, pSql->pStream, pSql->cmd.command);
      tscClearMeterMetaInfo(pMeterMetaInfo, false);
      tscSetNextLaunchTimer(pSql->pStream, pSql);
      return;
    }
    
    tscTrace("%p stream:%p meta is updated, start new query, command:%d", pSql, pSql->pStream, pSql->cmd.command);
    
    tscTansformSQLFunctionForSTableQuery(pQueryInfo);
    tscIncStreamExecutionCount(pSql->pStream);
  } else {
    tscTrace("%p get meterMeta/metricMeta successfully", pSql);
  }

  tscDoQuery(pSql);
}
