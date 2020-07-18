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
#include "tscUtil.h"
#include "hash.h"
#include "taosmsg.h"
#include "tcache.h"
#include "tkey.h"
#include "tmd5.h"
#include "tscJoinProcess.h"
#include "tscProfile.h"
#include "tscSecondaryMerge.h"
#include "tschemautil.h"
#include "tsclient.h"
#include "tsqldef.h"
#include "ttimer.h"
#include "tast.h"

/*
 * the detailed information regarding metric meta key is:
 * fullmetername + '.' + tagQueryCond + '.' + tableNameCond + '.' + joinCond +
 * '.' + relation + '.' + [tagId1, tagId2,...] + '.' + group_orderType
 *
 * if querycond/tablenameCond/joinCond is null, its format is:
 * fullmetername + '.' + '(nil)' + '.' + '(nil)' + relation + '.' + [tagId1,
 * tagId2,...] + '.' + group_orderType
 */
void tscGetMetricMetaCacheKey(SQueryInfo* pQueryInfo, char* str, uint64_t uid) {
  int32_t         index = -1;
  SMeterMetaInfo* pMeterMetaInfo = tscGetMeterMetaInfoByUid(pQueryInfo, uid, &index);

  int32_t len = 0;
  char    tagIdBuf[128] = {0};
  for (int32_t i = 0; i < pMeterMetaInfo->numOfTags; ++i) {
    len += sprintf(&tagIdBuf[len], "%d,", pMeterMetaInfo->tagColumnIndex[i]);
  }

  STagCond* pTagCond = &pQueryInfo->tagCond;
  assert(len < tListLen(tagIdBuf));

  const int32_t maxKeySize = TSDB_MAX_TAGS_LEN;  // allowed max key size

  SCond* cond = tsGetMetricQueryCondPos(pTagCond, uid);

  char join[512] = {0};
  if (pTagCond->joinInfo.hasJoin) {
    sprintf(join, "%s,%s", pTagCond->joinInfo.left.meterId, pTagCond->joinInfo.right.meterId);
  }

  // estimate the buffer size
  size_t tbnameCondLen = pTagCond->tbnameCond.cond != NULL ? strlen(pTagCond->tbnameCond.cond) : 0;
  size_t redundantLen = 20;

  size_t bufSize = strlen(pMeterMetaInfo->name) + tbnameCondLen + strlen(join) + strlen(tagIdBuf);
  if (cond != NULL && cond->cond != NULL) {
    bufSize += strlen(cond->cond);
  }

  bufSize = (size_t)((bufSize + redundantLen) * 1.5);
  char* tmp = calloc(1, bufSize);

  int32_t keyLen = snprintf(tmp, bufSize, "%s,%s,%s,%d,%s,[%s],%d", pMeterMetaInfo->name,
                            ((cond != NULL && cond->cond != NULL) ? cond->cond : NULL), (tbnameCondLen > 0 ? pTagCond->tbnameCond.cond : NULL),
                            pTagCond->relType, join, tagIdBuf, pQueryInfo->groupbyExpr.orderType);

  assert(keyLen <= bufSize);

  if (keyLen < maxKeySize) {
    strcpy(str, tmp);
  } else {  // using md5 to hash
    MD5_CTX ctx;
    MD5Init(&ctx);

    MD5Update(&ctx, (uint8_t*)tmp, keyLen);
    char* pStr = base64_encode(ctx.digest, tListLen(ctx.digest));
    strcpy(str, pStr);
    free(pStr);
  }

  free(tmp);
}

SCond* tsGetMetricQueryCondPos(STagCond* pTagCond, uint64_t uid) {
  for (int32_t i = 0; i < TSDB_MAX_JOIN_TABLE_NUM; ++i) {
    if (uid == pTagCond->cond[i].uid) {
      return &pTagCond->cond[i];
    }
  }

  return NULL;
}

void tsSetMetricQueryCond(STagCond* pTagCond, uint64_t uid, const char* str) {
  size_t len = strlen(str);
  if (len == 0) {
    return;
  }

  SCond* pDest = &pTagCond->cond[pTagCond->numOfTagCond];
  pDest->uid = uid;
  pDest->cond = strdup(str);

  pTagCond->numOfTagCond += 1;
}

bool tscQueryOnMetric(SSqlCmd* pCmd) {
  SQueryInfo* pQueryInfo = tscGetQueryInfoDetail(pCmd, 0);

  return ((pQueryInfo->type & TSDB_QUERY_TYPE_STABLE_QUERY) == TSDB_QUERY_TYPE_STABLE_QUERY) &&
         (pCmd->msgType == TSDB_MSG_TYPE_QUERY);
}

bool tscQueryMetricTags(SQueryInfo* pQueryInfo) {
  for (int32_t i = 0; i < pQueryInfo->fieldsInfo.numOfOutputCols; ++i) {
    if (tscSqlExprGet(pQueryInfo, i)->functionId != TSDB_FUNC_TAGPRJ) {
      return false;
    }
  }

  return true;
}

bool tscIsSelectivityWithTagQuery(SSqlCmd* pCmd) {
  bool    hasTags = false;
  int32_t numOfSelectivity = 0;

  SQueryInfo* pQueryInfo = tscGetQueryInfoDetail(pCmd, 0);

  for (int32_t i = 0; i < pQueryInfo->fieldsInfo.numOfOutputCols; ++i) {
    int32_t functId = tscSqlExprGet(pQueryInfo, i)->functionId;
    if (functId == TSDB_FUNC_TAG_DUMMY) {
      hasTags = true;
      continue;
    }

    if ((aAggs[functId].nStatus & TSDB_FUNCSTATE_SELECTIVITY) != 0) {
      numOfSelectivity++;
    }
  }

  if (numOfSelectivity > 0 && hasTags) {
    return true;
  }

  return false;
}

void tscGetDBInfoFromMeterId(char* meterId, char* db) {
  char* st = strstr(meterId, TS_PATH_DELIMITER);
  if (st != NULL) {
    char* end = strstr(st + 1, TS_PATH_DELIMITER);
    if (end != NULL) {
      memcpy(db, meterId, (end - meterId));
      db[end - meterId] = 0;
      return;
    }
  }

  db[0] = 0;
}

SVnodeSidList* tscGetVnodeSidList(SMetricMeta* pMetricmeta, int32_t vnodeIdx) {
  if (pMetricmeta == NULL) {
    tscError("illegal metricmeta");
    return 0;
  }

  if (pMetricmeta->numOfVnodes == 0 || pMetricmeta->numOfMeters == 0) {
    return 0;
  }

  if (vnodeIdx < 0 || vnodeIdx >= pMetricmeta->numOfVnodes) {
    int32_t vnodeRange = (pMetricmeta->numOfVnodes > 0) ? (pMetricmeta->numOfVnodes - 1) : 0;
    tscError("illegal vnodeIdx:%d, reset to 0, vnodeIdx range:%d-%d", vnodeIdx, 0, vnodeRange);

    vnodeIdx = 0;
  }

  return (SVnodeSidList*)(pMetricmeta->list[vnodeIdx] + (char*)pMetricmeta);
}

SMeterSidExtInfo* tscGetMeterSidInfo(SVnodeSidList* pSidList, int32_t idx) {
  if (pSidList == NULL) {
    tscError("illegal sidlist");
    return 0;
  }

  if (idx < 0 || idx >= pSidList->numOfSids) {
    int32_t sidRange = (pSidList->numOfSids > 0) ? (pSidList->numOfSids - 1) : 0;

    tscError("illegal sidIdx:%d, reset to 0, sidIdx range:%d-%d", idx, 0, sidRange);
    idx = 0;
  }
  
  assert(pSidList->pSidExtInfoList[idx] >= 0);
  
  return (SMeterSidExtInfo*)(pSidList->pSidExtInfoList[idx] + (char*)pSidList);
}

bool tscIsTwoStageMergeMetricQuery(SQueryInfo* pQueryInfo, int32_t tableIndex) {
  if (pQueryInfo == NULL) {
    return false;
  }

  SMeterMetaInfo* pMeterMetaInfo = tscGetMeterMetaInfoFromQueryInfo(pQueryInfo, tableIndex);
  if (pMeterMetaInfo == NULL) {
    return false;
  }
  
  // for select query super table, the metricmeta can not be null in any cases.
  if (pQueryInfo->command == TSDB_SQL_SELECT && UTIL_METER_IS_SUPERTABLE(pMeterMetaInfo)) {
    assert(pMeterMetaInfo->pMetricMeta != NULL);
  }
  
  if (pMeterMetaInfo->pMetricMeta == NULL) {
    return false;
  }
  
  if ((pQueryInfo->type & TSDB_QUERY_TYPE_FREE_RESOURCE) == TSDB_QUERY_TYPE_FREE_RESOURCE) {
    return false;
  }

  // for ordered projection query, iterate all qualified vnodes sequentially
  if (tscNonOrderedProjectionQueryOnSTable(pQueryInfo, tableIndex)) {
    return false;
  }

  if (((pQueryInfo->type & TSDB_QUERY_TYPE_STABLE_SUBQUERY) != TSDB_QUERY_TYPE_STABLE_SUBQUERY) &&
      pQueryInfo->command == TSDB_SQL_SELECT) {
    return UTIL_METER_IS_SUPERTABLE(pMeterMetaInfo);
  }

  return false;
}

bool tscIsProjectionQueryOnSTable(SQueryInfo* pQueryInfo, int32_t tableIndex) {
  SMeterMetaInfo* pMeterMetaInfo = tscGetMeterMetaInfoFromQueryInfo(pQueryInfo, tableIndex);
  
  /*
   * In following cases, return false for non ordered project query on super table
   * 1. failed to get metermeta from server; 2. not a super table; 3. limitation is 0;
   * 4. show queries, instead of a select query
   */
  if (pMeterMetaInfo == NULL || !UTIL_METER_IS_SUPERTABLE(pMeterMetaInfo) ||
      pQueryInfo->command == TSDB_SQL_RETRIEVE_EMPTY_RESULT || pQueryInfo->exprsInfo.numOfExprs == 0) {
    return false;
  }
  
  // only query on tag, not a projection query
  if (tscQueryMetricTags(pQueryInfo)) {
    return false;
  }
  
  // for project query, only the following two function is allowed
  for (int32_t i = 0; i < pQueryInfo->fieldsInfo.numOfOutputCols; ++i) {
    int32_t functionId = tscSqlExprGet(pQueryInfo, i)->functionId;
    if (functionId != TSDB_FUNC_PRJ && functionId != TSDB_FUNC_TAGPRJ && functionId != TSDB_FUNC_TAG &&
        functionId != TSDB_FUNC_TS && functionId != TSDB_FUNC_ARITHM) {
      return false;
    }
  }
  
  return true;
}

bool tscNonOrderedProjectionQueryOnSTable(SQueryInfo* pQueryInfo, int32_t tableIndex) {
  if (!tscIsProjectionQueryOnSTable(pQueryInfo, tableIndex)) {
    return false;
  }
  
  // order by column exists, not a non-ordered projection query
  return pQueryInfo->order.orderColId < 0;
}

bool tscOrderedProjectionQueryOnSTable(SQueryInfo* pQueryInfo, int32_t tableIndex) {
  if (!tscIsProjectionQueryOnSTable(pQueryInfo, tableIndex)) {
    return false;
  }
  
  // order by column exists, a non-ordered projection query
  return pQueryInfo->order.orderColId >= 0;
}

bool tscProjectionQueryOnTable(SQueryInfo* pQueryInfo) {
  for (int32_t i = 0; i < pQueryInfo->fieldsInfo.numOfOutputCols; ++i) {
    int32_t functionId = tscSqlExprGet(pQueryInfo, i)->functionId;
    if (functionId != TSDB_FUNC_PRJ && functionId != TSDB_FUNC_TS) {
      return false;
    }
  }

  return true;
}

bool tscIsPointInterpQuery(SQueryInfo* pQueryInfo) {
  for (int32_t i = 0; i < pQueryInfo->exprsInfo.numOfExprs; ++i) {
    SSqlExpr* pExpr = tscSqlExprGet(pQueryInfo, i);
    if (pExpr == NULL) {
      return false;
    }

    int32_t functionId = pExpr->functionId;
    if (functionId == TSDB_FUNC_TAG) {
      continue;
    }

    if (functionId != TSDB_FUNC_INTERP) {
      return false;
    }
  }
  return true;
}

bool tscIsTWAQuery(SQueryInfo* pQueryInfo) {
  for (int32_t i = 0; i < pQueryInfo->exprsInfo.numOfExprs; ++i) {
    SSqlExpr* pExpr = tscSqlExprGet(pQueryInfo, i);
    if (pExpr == NULL) {
      continue;
    }

    int32_t functionId = pExpr->functionId;
    if (functionId == TSDB_FUNC_TWA) {
      return true;
    }
  }

  return false;
}

void tscClearInterpInfo(SQueryInfo* pQueryInfo) {
  if (!tscIsPointInterpQuery(pQueryInfo)) {
    return;
  }

  pQueryInfo->interpoType = TSDB_INTERPO_NONE;
  tfree(pQueryInfo->defaultVal);
}

int32_t tscCreateResPointerInfo(SSqlRes* pRes, SQueryInfo* pQueryInfo) {
  if (pRes->tsrow == NULL) {
    int32_t numOfColumns = pQueryInfo->exprsInfo.numOfExprs;
    assert(numOfColumns >= pQueryInfo->fieldsInfo.numOfOutputCols);
    
    pRes->numOfCols = numOfColumns;
  
    pRes->tsrow = calloc(POINTER_BYTES, numOfColumns);
    pRes->buffer = calloc(POINTER_BYTES, numOfColumns);
  
    // not enough memory
    if (pRes->tsrow == NULL || (pRes->buffer == NULL && pRes->numOfCols > 0)) {
      tfree(pRes->tsrow);
      tfree(pRes->buffer);
    
      pRes->code = TSDB_CODE_CLI_OUT_OF_MEMORY;
      return pRes->code;
    }
  }

  return TSDB_CODE_SUCCESS;
}

void tscDestroyResPointerInfo(SSqlRes* pRes) {
  // free all buffers containing the multibyte string
  if (pRes->buffer != NULL) {
    for (int i = 0; i < pRes->numOfCols; i++) {
      tfree(pRes->buffer[i]);
    }
    
    pRes->numOfCols = 0;
  }
  
  tfree(pRes->pRsp);
  tfree(pRes->tsrow);
  
  tfree(pRes->pGroupRec);
  tfree(pRes->pColumnIndex);
  tfree(pRes->buffer);
  
  pRes->data = NULL;  // pRes->data points to the buffer of pRsp, no need to free
}

void tscFreeSqlCmdData(SSqlCmd* pCmd) {
  pCmd->pDataBlocks = tscDestroyBlockArrayList(pCmd->pDataBlocks);
  tscFreeSubqueryInfo(pCmd);
}

/*
 * this function must not change the pRes->code value, since it may be used later.
 */
void tscFreeResData(SSqlObj* pSql) {
  SSqlRes* pRes = &pSql->res;
  
  pRes->row = 0;
  
  pRes->rspType = 0;
  pRes->rspLen = 0;
  pRes->row = 0;
  
  pRes->numOfRows = 0;
  pRes->numOfTotal = 0;
  pRes->numOfTotalInCurrentClause = 0;
  
  pRes->numOfGroups = 0;
  pRes->precision = 0;
  pRes->qhandle = 0;
  
  pRes->offset = 0;
  pRes->useconds = 0;
  
  tscDestroyLocalReducer(pSql);
  
  tscDestroyResPointerInfo(pRes);
}

void tscFreeSqlResult(SSqlObj* pSql) {
  tfree(pSql->res.pRsp);
  pSql->res.row = 0;
  pSql->res.numOfRows = 0;
  pSql->res.numOfTotal = 0;

  pSql->res.numOfGroups = 0;
  tfree(pSql->res.pGroupRec);

  tscDestroyLocalReducer(pSql);

  tscDestroyResPointerInfo(&pSql->res);
  tfree(pSql->res.pColumnIndex);
}

void tscFreeSqlObjPartial(SSqlObj* pSql) {
  if (pSql == NULL || pSql->signature != pSql) {
    return;
  }

  SSqlCmd* pCmd = &pSql->cmd;
  STscObj* pObj = pSql->pTscObj;

  int32_t cmd = pCmd->command;
  if (cmd < TSDB_SQL_INSERT || cmd == TSDB_SQL_RETRIEVE_METRIC || cmd == TSDB_SQL_RETRIEVE_EMPTY_RESULT ||
      cmd == TSDB_SQL_METRIC_JOIN_RETRIEVE) {
    tscRemoveFromSqlList(pSql);
  }

  pCmd->command = 0;

  // pSql->sqlstr will be used by tscBuildQueryStreamDesc
  if (pObj->signature == pObj) {
    pthread_mutex_lock(&pObj->mutex);
    tfree(pSql->sqlstr);
    pthread_mutex_unlock(&pObj->mutex);
  }

  tscFreeSqlResult(pSql);
  tfree(pSql->pSubs);
  pSql->numOfSubs = 0;

  tscFreeSqlCmdData(pCmd);
}

void tscFreeSqlObj(SSqlObj* pSql) {
  if (pSql == NULL || pSql->signature != pSql) return;

  tscTrace("%p start to free sql object", pSql);
  tscFreeSqlObjPartial(pSql);

  pSql->signature = NULL;
  pSql->fp = NULL;
  
  SSqlCmd* pCmd = &pSql->cmd;

  memset(pCmd->payload, 0, (size_t)pCmd->allocSize);
  tfree(pCmd->payload);

  pCmd->allocSize = 0;

  if (pSql->fp == NULL) {
    tsem_destroy(&pSql->rspSem);
    tsem_destroy(&pSql->emptyRspSem);
  }
  free(pSql);
}

void tscDestroyDataBlock(STableDataBlocks* pDataBlock) {
  if (pDataBlock == NULL) {
    return;
  }

  tfree(pDataBlock->pData);
  tfree(pDataBlock->params);

  // free the refcount for metermeta
  taosRemoveDataFromCache(tscCacheHandle, (void**)&(pDataBlock->pMeterMeta), false);
  tfree(pDataBlock);
}

SParamInfo* tscAddParamToDataBlock(STableDataBlocks* pDataBlock, char type, uint8_t timePrec, short bytes,
                                   uint32_t offset) {
  uint32_t needed = pDataBlock->numOfParams + 1;
  if (needed > pDataBlock->numOfAllocedParams) {
    needed *= 2;
    void* tmp = realloc(pDataBlock->params, needed * sizeof(SParamInfo));
    if (tmp == NULL) {
      return NULL;
    }
    pDataBlock->params = (SParamInfo*)tmp;
    pDataBlock->numOfAllocedParams = needed;
  }

  SParamInfo* param = pDataBlock->params + pDataBlock->numOfParams;
  param->idx = -1;
  param->type = type;
  param->timePrec = timePrec;
  param->bytes = bytes;
  param->offset = offset;

  ++pDataBlock->numOfParams;
  return param;
}

SDataBlockList* tscCreateBlockArrayList() {
  const int32_t DEFAULT_INITIAL_NUM_OF_BLOCK = 16;

  SDataBlockList* pDataBlockArrayList = calloc(1, sizeof(SDataBlockList));
  if (pDataBlockArrayList == NULL) {
    return NULL;
  }
  pDataBlockArrayList->nAlloc = DEFAULT_INITIAL_NUM_OF_BLOCK;
  pDataBlockArrayList->pData = calloc(1, POINTER_BYTES * pDataBlockArrayList->nAlloc);
  if (pDataBlockArrayList->pData == NULL) {
    free(pDataBlockArrayList);
    return NULL;
  }

  return pDataBlockArrayList;
}

void tscAppendDataBlock(SDataBlockList* pList, STableDataBlocks* pBlocks) {
  if (pList->nSize >= pList->nAlloc) {
    pList->nAlloc = (pList->nAlloc) << 1U;
    pList->pData = realloc(pList->pData, POINTER_BYTES * (size_t)pList->nAlloc);

    // reset allocated memory
    memset(pList->pData + pList->nSize, 0, POINTER_BYTES * (pList->nAlloc - pList->nSize));
  }

  pList->pData[pList->nSize++] = pBlocks;
}

void* tscDestroyBlockArrayList(SDataBlockList* pList) {
  if (pList == NULL) {
    return NULL;
  }

  for (int32_t i = 0; i < pList->nSize; i++) {
    tscDestroyDataBlock(pList->pData[i]);
  }

  tfree(pList->pData);
  tfree(pList);

  return NULL;
}

int32_t tscCopyDataBlockToPayload(SSqlObj* pSql, STableDataBlocks* pDataBlock) {
  SSqlCmd* pCmd = &pSql->cmd;
  assert(pDataBlock->pMeterMeta != NULL);

  pCmd->numOfTablesInSubmit = pDataBlock->numOfMeters;

  assert(pCmd->numOfClause == 1);
  SMeterMetaInfo* pMeterMetaInfo = tscGetMeterMetaInfo(pCmd, pCmd->clauseIndex, 0);

  // set the correct metermeta object, the metermeta has been locked in pDataBlocks, so it must be in the cache
  if (pMeterMetaInfo->pMeterMeta != pDataBlock->pMeterMeta) {
    strcpy(pMeterMetaInfo->name, pDataBlock->meterId);
    taosRemoveDataFromCache(tscCacheHandle, (void**)&(pMeterMetaInfo->pMeterMeta), false);

    pMeterMetaInfo->pMeterMeta = taosTransferDataInCache(tscCacheHandle, (void**)&pDataBlock->pMeterMeta);
  } else {
    assert(strncmp(pMeterMetaInfo->name, pDataBlock->meterId, tListLen(pDataBlock->meterId)) == 0);
  }

  /*
   * the submit message consists of : [RPC header|message body|digest]
   * the dataBlock only includes the RPC Header buffer and actual submit messsage body, space for digest needs
   * additional space.
   */
  int ret = tscAllocPayload(pCmd, pDataBlock->nAllocSize + sizeof(STaosDigest));
  if (TSDB_CODE_SUCCESS != ret) {
    return ret;
  }

  memcpy(pCmd->payload, pDataBlock->pData, pDataBlock->nAllocSize);

  /*
   * the payloadLen should be actual message body size
   * the old value of payloadLen is the allocated payload size
   */
  pCmd->payloadLen = pDataBlock->nAllocSize - tsRpcHeadSize;

  assert(pCmd->allocSize >= pCmd->payloadLen + tsRpcHeadSize + sizeof(STaosDigest));
  return TSDB_CODE_SUCCESS;
}

void tscFreeUnusedDataBlocks(SDataBlockList* pList) {
  /* release additional memory consumption */
  for (int32_t i = 0; i < pList->nSize; ++i) {
    STableDataBlocks* pDataBlock = pList->pData[i];
    pDataBlock->pData = realloc(pDataBlock->pData, pDataBlock->size);
    pDataBlock->nAllocSize = (uint32_t)pDataBlock->size;
  }
}

/**
 * create the in-memory buffer for each table to keep the submitted data block
 * @param initialSize
 * @param rowSize
 * @param startOffset
 * @param name
 * @param dataBlocks
 * @return
 */
int32_t tscCreateDataBlock(size_t initialSize, int32_t rowSize, int32_t startOffset, const char* name,
                           SMeterMeta* pMeterMeta, STableDataBlocks** dataBlocks) {
  STableDataBlocks* dataBuf = (STableDataBlocks*)calloc(1, sizeof(STableDataBlocks));
  if (dataBuf == NULL) {
    tscError("failed to allocated memory, reason:%s", strerror(errno));
    return TSDB_CODE_CLI_OUT_OF_MEMORY;
  }

  dataBuf->nAllocSize = (uint32_t)initialSize;
  dataBuf->headerSize = startOffset; // the header size will always be the startOffset value, reserved for the subumit block header
  if (dataBuf->nAllocSize <= dataBuf->headerSize) {
    dataBuf->nAllocSize = dataBuf->headerSize*2;
  }
  
  dataBuf->pData = calloc(1, dataBuf->nAllocSize);
  dataBuf->ordered = true;
  dataBuf->prevTS = INT64_MIN;

  dataBuf->rowSize = rowSize;
  dataBuf->size = startOffset;
  dataBuf->tsSource = -1;

  strncpy(dataBuf->meterId, name, TSDB_METER_ID_LEN);

  /*
   * The metermeta may be released since the metermeta cache are completed clean by other thread
   * due to operation such as drop database. So here we add the reference count directly instead of invoke
   * taosGetDataFromCache, which may return NULL value.
   */
  dataBuf->pMeterMeta = taosGetDataFromExists(tscCacheHandle, pMeterMeta);
  assert(initialSize > 0 && pMeterMeta != NULL && dataBuf->pMeterMeta != NULL);

  *dataBlocks = dataBuf;
  return TSDB_CODE_SUCCESS;
}

int32_t tscGetDataBlockFromList(void* pHashList, SDataBlockList* pDataBlockList, int64_t id, int32_t size,
                                int32_t startOffset, int32_t rowSize, const char* tableId, SMeterMeta* pMeterMeta,
                                STableDataBlocks** dataBlocks) {
  *dataBlocks = NULL;

  STableDataBlocks** t1 = (STableDataBlocks**)taosGetDataFromHashTable(pHashList, (const char*)&id, sizeof(id));
  if (t1 != NULL) {
    *dataBlocks = *t1;
  }

  if (*dataBlocks == NULL) {
    int32_t ret = tscCreateDataBlock((size_t)size, rowSize, startOffset, tableId, pMeterMeta, dataBlocks);
    if (ret != TSDB_CODE_SUCCESS) {
      return ret;
    }

    taosAddToHashTable(pHashList, (const char*)&id, sizeof(int64_t), (char*)dataBlocks, POINTER_BYTES);
    tscAppendDataBlock(pDataBlockList, *dataBlocks);
  }

  return TSDB_CODE_SUCCESS;
}

int32_t tscMergeTableDataBlocks(SSqlObj* pSql, SDataBlockList* pTableDataBlockList) {
  SSqlCmd* pCmd = &pSql->cmd;

  void* pVnodeDataBlockHashList = taosInitHashTable(128, taosGetDefaultHashFunction(TSDB_DATA_TYPE_BIGINT), false);
  SDataBlockList* pVnodeDataBlockList = tscCreateBlockArrayList();

  for (int32_t i = 0; i < pTableDataBlockList->nSize; ++i) {
    STableDataBlocks* pOneTableBlock = pTableDataBlockList->pData[i];

    STableDataBlocks* dataBuf = NULL;
    int32_t           ret =
        tscGetDataBlockFromList(pVnodeDataBlockHashList, pVnodeDataBlockList, pOneTableBlock->vgid, TSDB_PAYLOAD_SIZE,
                                tsInsertHeadSize, 0, pOneTableBlock->meterId, pOneTableBlock->pMeterMeta, &dataBuf);
    if (ret != TSDB_CODE_SUCCESS) {
      tscError("%p failed to prepare the data block buffer for merging table data, code:%d", pSql, ret);
      taosCleanUpHashTable(pVnodeDataBlockHashList);
      tscDestroyBlockArrayList(pVnodeDataBlockList);
      return ret;
    }

    int64_t destSize = dataBuf->size + pOneTableBlock->size;
    if (dataBuf->nAllocSize < destSize) {
      while (dataBuf->nAllocSize < destSize) {
        dataBuf->nAllocSize = dataBuf->nAllocSize * 1.5;
      }

      char* tmp = realloc(dataBuf->pData, dataBuf->nAllocSize);
      if (tmp != NULL) {
        dataBuf->pData = tmp;
        memset(dataBuf->pData + dataBuf->size, 0, dataBuf->nAllocSize - dataBuf->size);
      } else {  // failed to allocate memory, free already allocated memory and return error code
        tscError("%p failed to allocate memory for merging submit block, size:%d", pSql, dataBuf->nAllocSize);

        taosCleanUpHashTable(pVnodeDataBlockHashList);
        tfree(dataBuf->pData);
        tscDestroyBlockArrayList(pVnodeDataBlockList);

        return TSDB_CODE_CLI_OUT_OF_MEMORY;
      }
    }

    SShellSubmitBlock* pBlocks = (SShellSubmitBlock*)pOneTableBlock->pData;
    sortRemoveDuplicates(pOneTableBlock);

    char* e = (char*)pBlocks->payLoad + pOneTableBlock->rowSize*(pBlocks->numOfRows-1);
    
    tscTrace("%p meterId:%s, sid:%d rows:%d sversion:%d skey:%" PRId64 ", ekey:%" PRId64, pSql, pOneTableBlock->meterId, pBlocks->sid,
             pBlocks->numOfRows, pBlocks->sversion, GET_INT64_VAL(pBlocks->payLoad), GET_INT64_VAL(e));

    pBlocks->sid = htonl(pBlocks->sid);
    pBlocks->uid = htobe64(pBlocks->uid);
    pBlocks->sversion = htonl(pBlocks->sversion);
    pBlocks->numOfRows = htons(pBlocks->numOfRows);

    memcpy(dataBuf->pData + dataBuf->size, pOneTableBlock->pData, pOneTableBlock->size);

    dataBuf->size += pOneTableBlock->size;
    dataBuf->numOfMeters += 1;
  }

  tscDestroyBlockArrayList(pTableDataBlockList);

  // free the table data blocks;
  pCmd->pDataBlocks = pVnodeDataBlockList;

  tscFreeUnusedDataBlocks(pCmd->pDataBlocks);
  taosCleanUpHashTable(pVnodeDataBlockHashList);

  return TSDB_CODE_SUCCESS;
}

void tscCloseTscObj(STscObj* pObj) {
  pObj->signature = NULL;
  SSqlObj* pSql = pObj->pSql;
  globalCode = pSql->res.code;

  taosTmrStopA(&(pObj->pTimer));
  tscFreeSqlObj(pSql);

  pthread_mutex_destroy(&pObj->mutex);
  tscTrace("%p DB connection is closed", pObj);
  tfree(pObj);
}

bool tscIsInsertOrImportData(char* sqlstr) {
  int32_t index = 0;

  do {
    SSQLToken t0 = tStrGetToken(sqlstr, &index, false, 0, NULL);
    if (t0.type != TK_LP) {
      return t0.type == TK_INSERT || t0.type == TK_IMPORT;
    }
  } while (1);
}

int tscAllocPayload(SSqlCmd* pCmd, int size) {
  assert(size > 0);

  if (pCmd->payload == NULL) {
    assert(pCmd->allocSize == 0);

    pCmd->payload = (char*)malloc(size);
    if (pCmd->payload == NULL) return TSDB_CODE_CLI_OUT_OF_MEMORY;
    pCmd->allocSize = size;
  } else {
    if (pCmd->allocSize < size) {
      char* b = realloc(pCmd->payload, size);
      if (b == NULL) return TSDB_CODE_CLI_OUT_OF_MEMORY;
      pCmd->payload = b;
      pCmd->allocSize = size;
    }
  }

  memset(pCmd->payload, 0, pCmd->allocSize);
  assert(pCmd->allocSize >= size);

  return TSDB_CODE_SUCCESS;
}

static void ensureSpace(SFieldInfo* pFieldInfo, int32_t size) {
  if (size > pFieldInfo->numOfAlloc) {
    int32_t oldSize = pFieldInfo->numOfAlloc;

    int32_t newSize = (oldSize <= 0) ? 8 : (oldSize << 1);
    while (newSize < size) {
      newSize = (newSize << 1);
    }

    if (newSize > TSDB_MAX_COLUMNS) {
      newSize = TSDB_MAX_COLUMNS;
    }

    int32_t inc = newSize - oldSize;

    pFieldInfo->pFields = realloc(pFieldInfo->pFields, newSize * sizeof(TAOS_FIELD));
    memset(&pFieldInfo->pFields[oldSize], 0, inc * sizeof(TAOS_FIELD));

//    pFieldInfo->pOffset = realloc(pFieldInfo->pOffset, newSize * sizeof(int16_t));
//    memset(&pFieldInfo->pOffset[oldSize], 0, inc * sizeof(int16_t));

    pFieldInfo->pVisibleCols = realloc(pFieldInfo->pVisibleCols, newSize * sizeof(bool));
    memset(&pFieldInfo->pVisibleCols[oldSize], 0, inc * sizeof(bool));

    pFieldInfo->pSqlExpr = realloc(pFieldInfo->pSqlExpr, POINTER_BYTES*newSize);
    pFieldInfo->pExpr = realloc(pFieldInfo->pExpr, POINTER_BYTES*newSize);
  
    memset(&pFieldInfo->pSqlExpr[oldSize], 0, inc * POINTER_BYTES);
    memset(&pFieldInfo->pExpr[oldSize], 0, inc * POINTER_BYTES);
  
    pFieldInfo->numOfAlloc = newSize;
  }
}

static void evic(SFieldInfo* pFieldInfo, int32_t index) {
  if (index < pFieldInfo->numOfOutputCols) {
    memmove(&pFieldInfo->pFields[index + 1], &pFieldInfo->pFields[index],
            sizeof(pFieldInfo->pFields[0]) * (pFieldInfo->numOfOutputCols - index));
    
    memmove(&pFieldInfo->pVisibleCols[index + 1], &pFieldInfo->pVisibleCols[index],
            sizeof(pFieldInfo->pVisibleCols[0]) * (pFieldInfo->numOfOutputCols - index));
    
    memmove(&pFieldInfo->pSqlExpr[index + 1], &pFieldInfo->pSqlExpr[index],
            sizeof(pFieldInfo->pSqlExpr[0]) * (pFieldInfo->numOfOutputCols - index));
  
    memmove(&pFieldInfo->pExpr[index + 1], &pFieldInfo->pExpr[index],
            sizeof(pFieldInfo->pExpr[0]) * (pFieldInfo->numOfOutputCols - index));
  }
}

static void setValueImpl(TAOS_FIELD* pField, int8_t type, const char* name, int16_t bytes) {
  pField->type = type;
  strncpy(pField->name, name, TSDB_COL_NAME_LEN);
  pField->bytes = bytes;
}

void tscFieldInfoSetValFromSchema(SFieldInfo* pFieldInfo, int32_t index, SSchema* pSchema) {
  ensureSpace(pFieldInfo, pFieldInfo->numOfOutputCols + 1);
  evic(pFieldInfo, index);

  TAOS_FIELD* pField = &pFieldInfo->pFields[index];
  setValueImpl(pField, pSchema->type, pSchema->name, pSchema->bytes);
  pFieldInfo->numOfOutputCols++;
}

void tscFieldInfoSetValFromField(SFieldInfo* pFieldInfo, int32_t index, TAOS_FIELD* pField) {
  ensureSpace(pFieldInfo, pFieldInfo->numOfOutputCols + 1);
  evic(pFieldInfo, index);

  memcpy(&pFieldInfo->pFields[index], pField, sizeof(TAOS_FIELD));
  pFieldInfo->pVisibleCols[index] = true;
  pFieldInfo->numOfOutputCols++;
}

void tscFieldInfoUpdateVisible(SFieldInfo* pFieldInfo, int32_t index, bool visible) {
  if (index < 0 || index >= pFieldInfo->numOfOutputCols) {
    return;
  }

  bool oldVisible = pFieldInfo->pVisibleCols[index];
  pFieldInfo->pVisibleCols[index] = visible;

  if (oldVisible != visible) {
    if (!visible) {
      pFieldInfo->numOfHiddenCols += 1;
    } else {
      if (pFieldInfo->numOfHiddenCols > 0) {
        pFieldInfo->numOfHiddenCols -= 1;
      }
    }
  }
}

void tscFieldInfoSetValue(SFieldInfo* pFieldInfo, int32_t index, int8_t type, const char* name, int16_t bytes) {
  ensureSpace(pFieldInfo, pFieldInfo->numOfOutputCols + 1);
  evic(pFieldInfo, index);

  TAOS_FIELD* pField = &pFieldInfo->pFields[index];
  setValueImpl(pField, type, name, bytes);

  pFieldInfo->pVisibleCols[index] = true;
  pFieldInfo->numOfOutputCols++;
  
  pFieldInfo->pExpr[index] = NULL;
  pFieldInfo->pSqlExpr[index] = NULL;
}

void tscFieldInfoSetExpr(SFieldInfo* pFieldInfo, int32_t index, SSqlExpr* pExpr) {
  assert(index >= 0 && index < pFieldInfo->numOfOutputCols);
  pFieldInfo->pSqlExpr[index] = pExpr;
}

void tscFieldInfoSetBinExpr(SFieldInfo* pFieldInfo, int32_t index, SSqlFunctionExpr* pExpr) {
  assert(index >= 0 && index < pFieldInfo->numOfOutputCols);
  pFieldInfo->pExpr[index] = pExpr;
}

void tscFieldInfoUpdateBySqlFunc(SQueryInfo* pQueryInfo) {
  for(int32_t i = 0; i < pQueryInfo->fieldsInfo.numOfOutputCols; ++i) {
    TAOS_FIELD* field = tscFieldInfoGetField(pQueryInfo, i);
    
    SSqlExpr* pExpr = pQueryInfo->fieldsInfo.pSqlExpr[i];
    if (pExpr == NULL) {
      continue;
    }
    
    field->type = pExpr->resType;
    field->bytes = pExpr->resBytes;
  }
}

void tscFieldInfoCalOffset(SQueryInfo* pQueryInfo) {
  SSqlExprInfo* pExprInfo = &pQueryInfo->exprsInfo;
  pExprInfo->pExprs[0]->offset = 0;
  
  for (int32_t i = 1; i < pExprInfo->numOfExprs; ++i) {
    pExprInfo->pExprs[i]->offset = pExprInfo->pExprs[i - 1]->offset + pExprInfo->pExprs[i - 1]->resBytes;
  }
}

void tscFieldInfoCopy(SFieldInfo* src, SFieldInfo* dst, const int32_t* indexList, int32_t size) {
  if (src == NULL) {
    return;
  }

  if (size <= 0) {
    *dst = *src;
    tscFieldInfoCopyAll(dst, src);
  } else {  // only copy the required column
    for (int32_t i = 0; i < size; ++i) {
      assert(indexList[i] >= 0 && indexList[i] <= src->numOfOutputCols);
      tscFieldInfoSetValFromField(dst, i, &src->pFields[indexList[i]]);
      dst->pVisibleCols[i] = src->pVisibleCols[indexList[i]];
      dst->pSqlExpr[i] = src->pSqlExpr[indexList[i]];
      dst->pExpr[i] = src->pExpr[indexList[i]];
    }
  }
}

void tscFieldInfoCopyAll(SFieldInfo* dst, SFieldInfo* src) {
  *dst = *src;

  dst->pFields = malloc(sizeof(TAOS_FIELD) * dst->numOfAlloc);
  dst->pVisibleCols = malloc(sizeof(bool) * dst->numOfAlloc);
  dst->pSqlExpr = malloc(POINTER_BYTES * dst->numOfAlloc);
  dst->pExpr = malloc(POINTER_BYTES * dst->numOfAlloc);

  memcpy(dst->pFields, src->pFields, sizeof(TAOS_FIELD) * dst->numOfOutputCols);
  memcpy(dst->pVisibleCols, src->pVisibleCols, sizeof(bool) * dst->numOfOutputCols);
  memcpy(dst->pSqlExpr, src->pSqlExpr, POINTER_BYTES * dst->numOfOutputCols);
  memcpy(dst->pExpr, src->pExpr, POINTER_BYTES * dst->numOfOutputCols);
}

TAOS_FIELD* tscFieldInfoGetField(SQueryInfo* pQueryInfo, int32_t index) {
  if (index >= pQueryInfo->fieldsInfo.numOfOutputCols) {
    return NULL;
  }

  return &pQueryInfo->fieldsInfo.pFields[index];
}

int32_t tscNumOfFields(SQueryInfo* pQueryInfo) { return pQueryInfo->fieldsInfo.numOfOutputCols; }

int16_t tscFieldInfoGetOffset(SQueryInfo* pQueryInfo, int32_t index) {
  if (index >= pQueryInfo->exprsInfo.numOfExprs) {
    return 0;
  }

  return pQueryInfo->exprsInfo.pExprs[index]->offset;
}

int32_t tscFieldInfoCompare(SFieldInfo* pFieldInfo1, SFieldInfo* pFieldInfo2) {
  assert(pFieldInfo1 != NULL && pFieldInfo2 != NULL);

  if (pFieldInfo1->numOfOutputCols != pFieldInfo2->numOfOutputCols) {
    return pFieldInfo1->numOfOutputCols - pFieldInfo2->numOfOutputCols;
  }

  for (int32_t i = 0; i < pFieldInfo1->numOfOutputCols; ++i) {
    TAOS_FIELD* pField1 = &pFieldInfo1->pFields[i];
    TAOS_FIELD* pField2 = &pFieldInfo2->pFields[i];

    if (pField1->type != pField2->type || pField1->bytes != pField2->bytes ||
        strcasecmp(pField1->name, pField2->name) != 0) {
      return 1;
    }
  }

  return 0;
}

int32_t tscGetResRowLength(SQueryInfo* pQueryInfo) {
  if (pQueryInfo->exprsInfo.numOfExprs <= 0) {
    return 0;
  }
  
  int32_t size = 0;
  for(int32_t i = 0; i < pQueryInfo->exprsInfo.numOfExprs; ++i) {
    size += pQueryInfo->exprsInfo.pExprs[i]->resBytes;
  }
  
  return size;
}

void tscClearFieldInfo(SFieldInfo* pFieldInfo) {
  if (pFieldInfo == NULL) {
    return;
  }

  tfree(pFieldInfo->pFields);
  tfree(pFieldInfo->pVisibleCols);
  tfree(pFieldInfo->pSqlExpr);
  
  for(int32_t i = 0; i < pFieldInfo->numOfOutputCols; ++i) {
    SSqlFunctionExpr* pExpr = pFieldInfo->pExpr[i];
    if (pExpr != NULL) {
      tSQLBinaryExprDestroy(&pExpr->binExprInfo.pBinExpr, NULL);
      tfree(pExpr->binExprInfo.pReqColumns);
      tfree(pExpr);
    }
  }
  
  tfree(pFieldInfo->pExpr);
  memset(pFieldInfo, 0, sizeof(SFieldInfo));
}

static void _exprCheckSpace(SSqlExprInfo* pExprInfo, int32_t size) {
  if (size > pExprInfo->numOfAlloc) {
    uint32_t oldSize = pExprInfo->numOfAlloc;

    uint32_t newSize = (oldSize <= 0) ? 8 : (oldSize << 1U);
    while (newSize < size) {
      newSize = (newSize << 1U);
    }

    if (newSize > TSDB_MAX_COLUMNS) {
      newSize = TSDB_MAX_COLUMNS;
    }

    int32_t inc = newSize - oldSize;

    pExprInfo->pExprs = realloc(pExprInfo->pExprs, newSize * sizeof(SSqlExpr));
    memset(&pExprInfo->pExprs[oldSize], 0, inc * sizeof(SSqlExpr));

    pExprInfo->numOfAlloc = newSize;
  }
}

static void _exprEvic(SSqlExprInfo* pExprInfo, int32_t index) {
  if (index < pExprInfo->numOfExprs) {
    memmove(&pExprInfo->pExprs[index + 1], &pExprInfo->pExprs[index],
            sizeof(pExprInfo->pExprs[0]) * (pExprInfo->numOfExprs - index));
  }
}

SSqlExpr* tscSqlExprInsertEmpty(SQueryInfo* pQueryInfo, int32_t index, int16_t functionId) {
  SSqlExprInfo* pExprInfo = &pQueryInfo->exprsInfo;

  _exprCheckSpace(pExprInfo, pExprInfo->numOfExprs + 1);
  _exprEvic(pExprInfo, index);
  
  SSqlExpr* pExpr = calloc(1, sizeof(SSqlExpr));
  pExpr->functionId = functionId;
  
  pExprInfo->numOfExprs++;
  pExprInfo->pExprs[index] = pExpr;
  return pExpr;
}

SSqlExpr* tscSqlExprInsert(SQueryInfo* pQueryInfo, int32_t index, int16_t functionId, SColumnIndex* pColIndex,
                           int16_t type, int16_t size, int16_t interSize) {
  SMeterMetaInfo* pMeterMetaInfo = tscGetMeterMetaInfoFromQueryInfo(pQueryInfo, pColIndex->tableIndex);

  SSqlExprInfo* pExprInfo = &pQueryInfo->exprsInfo;

  _exprCheckSpace(pExprInfo, pExprInfo->numOfExprs + 1);
  _exprEvic(pExprInfo, index);

  SSqlExpr* pExpr = calloc(1, sizeof(SSqlExpr));
  pExprInfo->pExprs[index] = pExpr;
  
  pExpr->functionId = functionId;
  int16_t numOfCols = pMeterMetaInfo->pMeterMeta->numOfColumns;

  // set the correct column index
  if (pColIndex->columnIndex == TSDB_TBNAME_COLUMN_INDEX) {
    pExpr->colInfo.colId = TSDB_TBNAME_COLUMN_INDEX;
  } else {
    SSchema* pSchema = tsGetColumnSchema(pMeterMetaInfo->pMeterMeta, pColIndex->columnIndex);
    pExpr->colInfo.colId = pSchema->colId;
  }

  // tag columns require the column index revised.
  if (pColIndex->columnIndex >= numOfCols) {
    pColIndex->columnIndex -= numOfCols;
    pExpr->colInfo.flag = TSDB_COL_TAG;
  } else {
    if (pColIndex->columnIndex != TSDB_TBNAME_COLUMN_INDEX) {
      pExpr->colInfo.flag = TSDB_COL_NORMAL;
    } else {
      pExpr->colInfo.flag = TSDB_COL_TAG;
    }
  }

  pExpr->colInfo.colIdx = pColIndex->columnIndex;
  pExpr->resType = type;
  pExpr->resBytes = size;
  pExpr->interResBytes = interSize;
  pExpr->uid = pMeterMetaInfo->pMeterMeta->uid;

  pExprInfo->numOfExprs++;
  return pExpr;
}

SSqlExpr* tscSqlExprUpdate(SQueryInfo* pQueryInfo, int32_t index, int16_t functionId, int16_t srcColumnIndex,
                           int16_t type, int16_t size) {
  SMeterMetaInfo* pMeterMetaInfo = tscGetMeterMetaInfoFromQueryInfo(pQueryInfo, 0);
  SSqlExprInfo*   pExprInfo = &pQueryInfo->exprsInfo;
  if (index > pExprInfo->numOfExprs) {
    return NULL;
  }

  SSqlExpr* pExpr = pExprInfo->pExprs[index];

  pExpr->functionId = functionId;

  pExpr->colInfo.colIdx = srcColumnIndex;
  pExpr->colInfo.colId = tsGetColumnSchema(pMeterMetaInfo->pMeterMeta, srcColumnIndex)->colId;

  pExpr->resType = type;
  pExpr->resBytes = size;

  return pExpr;
}

int32_t  tscSqlExprNumOfExprs(SQueryInfo* pQueryInfo) {
  return pQueryInfo->exprsInfo.numOfExprs;
}

void addExprParams(SSqlExpr* pExpr, char* argument, int32_t type, int32_t bytes, int16_t tableIndex) {
  if (pExpr == NULL || argument == NULL || bytes == 0) {
    return;
  }

  // set parameter value
  // transfer to tVariant from byte data/no ascii data
  tVariantCreateFromBinary(&pExpr->param[pExpr->numOfParams], argument, bytes, type);

  pExpr->numOfParams += 1;
  assert(pExpr->numOfParams <= 3);
}

SSqlExpr* tscSqlExprGet(SQueryInfo* pQueryInfo, int32_t index) {
  if (pQueryInfo->exprsInfo.numOfExprs <= index) {
    return NULL;
  }

  return pQueryInfo->exprsInfo.pExprs[index];
}

void* tscSqlExprDestroy(SSqlExpr* pExpr) {
  if (pExpr == NULL) {
    return NULL;
  }
  
  for(int32_t i = 0; i < tListLen(pExpr->param); ++i) {
    tVariantDestroy(&pExpr->param[i]);
  }
  
  tfree(pExpr);
  
  return NULL;
}

/*
 * NOTE: Does not release SSqlExprInfo here.
 */
void tscSqlExprInfoDestroy(SSqlExprInfo* pExprInfo) {
  if (pExprInfo->numOfAlloc == 0) {
    return;
  }
  
  for(int32_t i = 0; i < pExprInfo->numOfExprs; ++i) {
    tscSqlExprDestroy(pExprInfo->pExprs[i]);
  }
  
  tfree(pExprInfo->pExprs);
  
  pExprInfo->numOfAlloc = 0;
  pExprInfo->numOfExprs = 0;
}


void tscSqlExprCopy(SSqlExprInfo* dst, const SSqlExprInfo* src, uint64_t tableuid, bool deepcopy) {
  if (src == NULL) {
    return;
  }

  *dst = *src;

  dst->pExprs = calloc(dst->numOfAlloc, POINTER_BYTES);
  
  int16_t num = 0;
  for (int32_t i = 0; i < src->numOfExprs; ++i) {
    if (src->pExprs[i]->uid == tableuid) {
      
      if (deepcopy) {
        dst->pExprs[num] = calloc(1, sizeof(SSqlExpr));
        *dst->pExprs[num] = *src->pExprs[i];
      } else {
        dst->pExprs[num] = src->pExprs[i];
      }
      
      num++;
    }
  }

  dst->numOfExprs = num;
  
  if (deepcopy) {
    for (int32_t i = 0; i < dst->numOfExprs; ++i) {
      for (int32_t j = 0; j < src->pExprs[i]->numOfParams; ++j) {
        tVariantAssign(&dst->pExprs[i]->param[j], &src->pExprs[i]->param[j]);
      }
    }
  }

}

static void clearVal(SColumnBase* pBase) {
  memset(pBase, 0, sizeof(SColumnBase));

  pBase->colIndex.tableIndex = -2;
  pBase->colIndex.columnIndex = -2;
}

static void _cf_ensureSpace(SColumnBaseInfo* pcolList, int32_t size) {
  if (pcolList->numOfAlloc < size) {
    int32_t oldSize = pcolList->numOfAlloc;

    int32_t newSize = (oldSize <= 0) ? 8 : (oldSize << 1);
    while (newSize < size) {
      newSize = (newSize << 1);
    }

    if (newSize > TSDB_MAX_COLUMNS) {
      newSize = TSDB_MAX_COLUMNS;
    }

    int32_t inc = newSize - oldSize;

    pcolList->pColList = realloc(pcolList->pColList, newSize * sizeof(SColumnBase));
    memset(&pcolList->pColList[oldSize], 0, inc * sizeof(SColumnBase));

    pcolList->numOfAlloc = newSize;
  }
}

static void _cf_evic(SColumnBaseInfo* pcolList, int32_t index) {
  if (index < pcolList->numOfCols) {
    memmove(&pcolList->pColList[index + 1], &pcolList->pColList[index],
            sizeof(SColumnBase) * (pcolList->numOfCols - index));

    clearVal(&pcolList->pColList[index]);
  }
}

SColumnBase* tscColumnBaseInfoGet(SColumnBaseInfo* pColumnBaseInfo, int32_t index) {
  if (pColumnBaseInfo == NULL || pColumnBaseInfo->numOfCols < index) {
    return NULL;
  }

  return &pColumnBaseInfo->pColList[index];
}

void tscColumnBaseInfoUpdateTableIndex(SColumnBaseInfo* pColList, int16_t tableIndex) {
  for (int32_t i = 0; i < pColList->numOfCols; ++i) {
    pColList->pColList[i].colIndex.tableIndex = tableIndex;
  }
}

// todo refactor
SColumnBase* tscColumnBaseInfoInsert(SQueryInfo* pQueryInfo, SColumnIndex* pColIndex) {
  SColumnBaseInfo* pcolList = &pQueryInfo->colList;

  // ignore the tbname column to be inserted into source list
  if (pColIndex->columnIndex < 0) {
    return NULL;
  }

  int16_t col = pColIndex->columnIndex;

  int32_t i = 0;
  while (i < pcolList->numOfCols) {
    if (pcolList->pColList[i].colIndex.columnIndex < col) {
      i++;
    } else if (pcolList->pColList[i].colIndex.tableIndex < pColIndex->tableIndex) {
      i++;
    } else {
      break;
    }
  }

  SColumnIndex* pIndex = &pcolList->pColList[i].colIndex;
  if ((i < pcolList->numOfCols && (pIndex->columnIndex > col || pIndex->tableIndex != pColIndex->tableIndex)) ||
      (i >= pcolList->numOfCols)) {
    _cf_ensureSpace(pcolList, pcolList->numOfCols + 1);
    _cf_evic(pcolList, i);

    pcolList->pColList[i].colIndex = *pColIndex;
    pcolList->numOfCols++;
  }

  return &pcolList->pColList[i];
}

void tscColumnFilterInfoCopy(SColumnFilterInfo* dst, const SColumnFilterInfo* src) {
  assert(src != NULL && dst != NULL);

  assert(src->filterOnBinary == 0 || src->filterOnBinary == 1);
  if (src->lowerRelOptr == TSDB_RELATION_INVALID && src->upperRelOptr == TSDB_RELATION_INVALID) {
    assert(0);
  }

  *dst = *src;
  if (dst->filterOnBinary) {
    size_t len = (size_t)dst->len + 1;
    char*  pTmp = calloc(1, len);
    dst->pz = (int64_t)pTmp;
    memcpy((char*)dst->pz, (char*)src->pz, (size_t)len);
  }
}

void tscColumnBaseCopy(SColumnBase* dst, const SColumnBase* src) {
  assert(src != NULL && dst != NULL);

  *dst = *src;

  if (src->numOfFilters > 0) {
    dst->filterInfo = calloc(1, src->numOfFilters * sizeof(SColumnFilterInfo));

    for (int32_t j = 0; j < src->numOfFilters; ++j) {
      tscColumnFilterInfoCopy(&dst->filterInfo[j], &src->filterInfo[j]);
    }
  } else {
    assert(src->filterInfo == NULL);
  }
}

void tscColumnBaseInfoCopy(SColumnBaseInfo* dst, const SColumnBaseInfo* src, int16_t tableIndex) {
  if (src == NULL) {
    return;
  }

  *dst = *src;
  dst->pColList = calloc(1, sizeof(SColumnBase) * dst->numOfAlloc);

  int16_t num = 0;
  for (int32_t i = 0; i < src->numOfCols; ++i) {
    if (src->pColList[i].colIndex.tableIndex == tableIndex || tableIndex < 0) {
      dst->pColList[num] = src->pColList[i];

      if (dst->pColList[num].numOfFilters > 0) {
        dst->pColList[num].filterInfo = calloc(1, dst->pColList[num].numOfFilters * sizeof(SColumnFilterInfo));

        for (int32_t j = 0; j < dst->pColList[num].numOfFilters; ++j) {
          tscColumnFilterInfoCopy(&dst->pColList[num].filterInfo[j], &src->pColList[i].filterInfo[j]);
        }
      }

      num += 1;
    }
  }

  dst->numOfCols = num;
}

void tscColumnBaseInfoDestroy(SColumnBaseInfo* pColumnBaseInfo) {
  if (pColumnBaseInfo == NULL) {
    return;
  }

  assert(pColumnBaseInfo->numOfCols <= TSDB_MAX_COLUMNS);

  for (int32_t i = 0; i < pColumnBaseInfo->numOfCols; ++i) {
    SColumnBase* pColBase = &(pColumnBaseInfo->pColList[i]);

    if (pColBase->numOfFilters > 0) {
      for (int32_t j = 0; j < pColBase->numOfFilters; ++j) {
        assert(pColBase->filterInfo[j].filterOnBinary == 0 || pColBase->filterInfo[j].filterOnBinary == 1);

        if (pColBase->filterInfo[j].filterOnBinary) {
          free((char*)pColBase->filterInfo[j].pz);
          pColBase->filterInfo[j].pz = 0;
        }
      }
    }

    tfree(pColBase->filterInfo);
  }

  tfree(pColumnBaseInfo->pColList);
}

void tscColumnBaseInfoReserve(SColumnBaseInfo* pColumnBaseInfo, int32_t size) {
  _cf_ensureSpace(pColumnBaseInfo, size);
}

/*
 * 1. normal name, not a keyword or number
 * 2. name with quote
 * 3. string with only one delimiter '.'.
 *
 * only_one_part
 * 'only_one_part'
 * first_part.second_part
 * first_part.'second_part'
 * 'first_part'.second_part
 * 'first_part'.'second_part'
 * 'first_part.second_part'
 *
 */
static int32_t validateQuoteToken(SSQLToken* pToken) {
  pToken->n = strdequote(pToken->z);
  strtrim(pToken->z);
  pToken->n = (uint32_t)strlen(pToken->z);

  int32_t k = tSQLGetToken(pToken->z, &pToken->type);

  if (pToken->type == TK_STRING) {
    return tscValidateName(pToken);
  }

  if (k != pToken->n || pToken->type != TK_ID) {
    return TSDB_CODE_INVALID_SQL;
  }
  return TSDB_CODE_SUCCESS;
}

int32_t tscValidateName(SSQLToken* pToken) {
  if (pToken->type != TK_STRING && pToken->type != TK_ID) {
    return TSDB_CODE_INVALID_SQL;
  }

  char* sep = strnchr(pToken->z, TS_PATH_DELIMITER[0], pToken->n, true);
  if (sep == NULL) {  // single part
    if (pToken->type == TK_STRING) {
      pToken->n = strdequote(pToken->z);
      strtrim(pToken->z);
      pToken->n = (uint32_t)strlen(pToken->z);

      int len = tSQLGetToken(pToken->z, &pToken->type);

      // single token, validate it
      if (len == pToken->n) {
        return validateQuoteToken(pToken);
      } else {
        sep = strnchr(pToken->z, TS_PATH_DELIMITER[0], pToken->n, true);
        if (sep == NULL) {
          return TSDB_CODE_INVALID_SQL;
        }

        return tscValidateName(pToken);
      }
    } else {
      if (isNumber(pToken)) {
        return TSDB_CODE_INVALID_SQL;
      }
    }
  } else {  // two part
    int32_t oldLen = pToken->n;
    char*   pStr = pToken->z;

    if (pToken->type == TK_SPACE) {
      strtrim(pToken->z);
      pToken->n = (uint32_t)strlen(pToken->z);
    }

    pToken->n = tSQLGetToken(pToken->z, &pToken->type);
    if (pToken->z[pToken->n] != TS_PATH_DELIMITER[0]) {
      return TSDB_CODE_INVALID_SQL;
    }

    if (pToken->type != TK_STRING && pToken->type != TK_ID) {
      return TSDB_CODE_INVALID_SQL;
    }

    if (pToken->type == TK_STRING && validateQuoteToken(pToken) != TSDB_CODE_SUCCESS) {
      return TSDB_CODE_INVALID_SQL;
    }

    int32_t firstPartLen = pToken->n;

    pToken->z = sep + 1;
    pToken->n = oldLen - (sep - pStr) - 1;
    int32_t len = tSQLGetToken(pToken->z, &pToken->type);
    if (len != pToken->n || (pToken->type != TK_STRING && pToken->type != TK_ID)) {
      return TSDB_CODE_INVALID_SQL;
    }

    if (pToken->type == TK_STRING && validateQuoteToken(pToken) != TSDB_CODE_SUCCESS) {
      return TSDB_CODE_INVALID_SQL;
    }

    // re-build the whole name string
    if (pStr[firstPartLen] == TS_PATH_DELIMITER[0]) {
      // first part do not have quote do nothing
    } else {
      pStr[firstPartLen] = TS_PATH_DELIMITER[0];
      memmove(&pStr[firstPartLen + 1], pToken->z, pToken->n);
      pStr[firstPartLen + sizeof(TS_PATH_DELIMITER[0]) + pToken->n] = 0;
    }
    pToken->n += (firstPartLen + sizeof(TS_PATH_DELIMITER[0]));
    pToken->z = pStr;
  }

  return TSDB_CODE_SUCCESS;
}

void tscIncStreamExecutionCount(void* pStream) {
  if (pStream == NULL) {
    return;
  }

  SSqlStream* ps = (SSqlStream*)pStream;
  ps->num += 1;
}

bool tscValidateColumnId(SMeterMetaInfo* pMeterMetaInfo, int32_t colId) {
  if (pMeterMetaInfo->pMeterMeta == NULL) {
    return false;
  }

  if (colId == -1 && UTIL_METER_IS_SUPERTABLE(pMeterMetaInfo)) {
    return true;
  }

  SSchema* pSchema = tsGetSchema(pMeterMetaInfo->pMeterMeta);
  int32_t  numOfTotal = pMeterMetaInfo->pMeterMeta->numOfTags + pMeterMetaInfo->pMeterMeta->numOfColumns;

  for (int32_t i = 0; i < numOfTotal; ++i) {
    if (pSchema[i].colId == colId) {
      return true;
    }
  }

  return false;
}

void tscTagCondCopy(STagCond* dest, const STagCond* src) {
  memset(dest, 0, sizeof(STagCond));

  if (src->tbnameCond.cond != NULL) {
    dest->tbnameCond.cond = strdup(src->tbnameCond.cond);
  }

  dest->tbnameCond.uid = src->tbnameCond.uid;

  memcpy(&dest->joinInfo, &src->joinInfo, sizeof(SJoinInfo));

  for (int32_t i = 0; i < src->numOfTagCond; ++i) {
    if (src->cond[i].cond != NULL) {
      dest->cond[i].cond = strdup(src->cond[i].cond);
    }

    dest->cond[i].uid = src->cond[i].uid;
  }

  dest->relType = src->relType;
  dest->numOfTagCond = src->numOfTagCond;
}

void tscTagCondRelease(STagCond* pCond) {
  free(pCond->tbnameCond.cond);
  for (int32_t i = 0; i < pCond->numOfTagCond; ++i) {
    free(pCond->cond[i].cond);
  }

  memset(pCond, 0, sizeof(STagCond));
}

void tscGetSrcColumnInfo(SSrcColumnInfo* pColInfo, SQueryInfo* pQueryInfo) {
  SMeterMetaInfo* pMeterMetaInfo = tscGetMeterMetaInfoFromQueryInfo(pQueryInfo, 0);
  SSchema*        pSchema = tsGetSchema(pMeterMetaInfo->pMeterMeta);

  for (int32_t i = 0; i < pQueryInfo->exprsInfo.numOfExprs; ++i) {
    SSqlExpr* pExpr = tscSqlExprGet(pQueryInfo, i);
    pColInfo[i].functionId = pExpr->functionId;

    if (TSDB_COL_IS_TAG(pExpr->colInfo.flag)) {
      SSchema* pTagSchema = tsGetTagSchema(pMeterMetaInfo->pMeterMeta);
      int16_t  actualTagIndex = pMeterMetaInfo->tagColumnIndex[pExpr->colInfo.colIdx];

      pColInfo[i].type = (actualTagIndex != -1) ? pTagSchema[actualTagIndex].type : TSDB_DATA_TYPE_BINARY;
    } else {
      pColInfo[i].type = pSchema[pExpr->colInfo.colIdx].type;
    }
  }
}

void tscSetFreeHeatBeat(STscObj* pObj) {
  if (pObj == NULL || pObj->signature != pObj || pObj->pHb == NULL) {
    return;
  }

  SSqlObj* pHeatBeat = pObj->pHb;
  assert(pHeatBeat == pHeatBeat->signature);

  // to denote the heart-beat timer close connection and free all allocated resources
  SQueryInfo* pQueryInfo = tscGetQueryInfoDetail(&pHeatBeat->cmd, 0);
  pQueryInfo->type = TSDB_QUERY_TYPE_FREE_RESOURCE;
}

bool tscShouldFreeHeatBeat(SSqlObj* pHb) {
  assert(pHb == pHb->signature);

  SQueryInfo* pQueryInfo = tscGetQueryInfoDetail(&pHb->cmd, 0);
  return pQueryInfo->type == TSDB_QUERY_TYPE_FREE_RESOURCE;
}

void tscCleanSqlCmd(SSqlCmd* pCmd) {
  pCmd->pDataBlocks = tscDestroyBlockArrayList(pCmd->pDataBlocks);
  tscFreeSubqueryInfo(pCmd);

  uint32_t allocSize = pCmd->allocSize;
  char*    allocPtr = pCmd->payload;

  memset(pCmd, 0, sizeof(SSqlCmd));

  // restore values
  pCmd->allocSize = allocSize;
  pCmd->payload = allocPtr;
}

/*
 * the following three kinds of SqlObj should not be freed
 * 1. SqlObj for stream computing
 * 2. main SqlObj
 * 3. heartbeat SqlObj
 *
 * If res code is error and SqlObj does not belong to above types, it should be
 * automatically freed for async query, ignoring that connection should be kept.
 *
 * If connection need to be recycled, the SqlObj also should be freed.
 */
bool tscShouldFreeAsyncSqlObj(SSqlObj* pSql) {
  if (pSql == NULL || pSql->signature != pSql || pSql->fp == NULL) {
    return false;
  }

  STscObj* pTscObj = pSql->pTscObj;
  if (pSql->pStream != NULL || pTscObj->pHb == pSql) {
    return false;
  }

  int32_t command = pSql->cmd.command;
  if (pTscObj->pSql == pSql) {
    /*
     * in case of taos_connect_a query, the object should all be released, even it is the
     * master sql object. Otherwise, the master sql should not be released
     */
    if (command == TSDB_SQL_CONNECT && pSql->res.code != TSDB_CODE_SUCCESS) {
      return true;
    }

    return false;
  }

  if (command == TSDB_SQL_INSERT) {
    SSqlCmd* pCmd = &pSql->cmd;

    /*
     * in case of multi-vnode insertion, the object should not be released until all
     * data blocks have been submit to vnode.
     */
    SDataBlockList* pDataBlocks = pCmd->pDataBlocks;
    SQueryInfo*     pQueryInfo = tscGetQueryInfoDetail(&pSql->cmd, 0);

    SMeterMetaInfo* pMeterMetaInfo = tscGetMeterMetaInfoFromQueryInfo(pQueryInfo, 0);
    assert(pQueryInfo->numOfTables == 1 || pQueryInfo->numOfTables == 2);

    if (pDataBlocks == NULL || pMeterMetaInfo->vnodeIndex >= pDataBlocks->nSize) {
      tscTrace("%p object should be release since all data blocks have been submit", pSql);
      return true;
    } else {
      return false;
    }
  } else {
    return tscKeepConn[command] == 0 ||
           (pSql->res.code != TSDB_CODE_ACTION_IN_PROGRESS && pSql->res.code != TSDB_CODE_SUCCESS);
  }
}

/**
 *
 * @param pCmd
 * @param clauseIndex denote the index of the union sub clause, usually are 0, if no union query exists.
 * @param tableIndex  denote the table index for join query, where more than one table exists
 * @return
 */
SMeterMetaInfo* tscGetMeterMetaInfo(SSqlCmd* pCmd, int32_t clauseIndex, int32_t tableIndex) {
  if (pCmd == NULL || pCmd->numOfClause == 0) {
    return NULL;
  }

  assert(clauseIndex >= 0 && clauseIndex < pCmd->numOfClause);

  SQueryInfo* pQueryInfo = tscGetQueryInfoDetail(pCmd, clauseIndex);
  return tscGetMeterMetaInfoFromQueryInfo(pQueryInfo, tableIndex);
}

SMeterMetaInfo* tscGetMeterMetaInfoFromQueryInfo(SQueryInfo* pQueryInfo, int32_t tableIndex) {
  assert(pQueryInfo != NULL);

  if (pQueryInfo->pMeterInfo == NULL) {
    assert(pQueryInfo->numOfTables == 0);
    return NULL;
  }

  assert(tableIndex >= 0 && tableIndex <= pQueryInfo->numOfTables && pQueryInfo->pMeterInfo != NULL);

  return pQueryInfo->pMeterInfo[tableIndex];
}

SQueryInfo* tscGetQueryInfoDetail(SSqlCmd* pCmd, int32_t subClauseIndex) {
  assert(pCmd != NULL && subClauseIndex >= 0 && subClauseIndex < TSDB_MAX_UNION_CLAUSE);

  if (pCmd->pQueryInfo == NULL || subClauseIndex >= pCmd->numOfClause) {
    return NULL;
  }

  return pCmd->pQueryInfo[subClauseIndex];
}

int32_t tscGetQueryInfoDetailSafely(SSqlCmd* pCmd, int32_t subClauseIndex, SQueryInfo** pQueryInfo) {
  int32_t ret = TSDB_CODE_SUCCESS;

  *pQueryInfo = tscGetQueryInfoDetail(pCmd, subClauseIndex);

  while ((*pQueryInfo) == NULL) {
    if ((ret = tscAddSubqueryInfo(pCmd)) != TSDB_CODE_SUCCESS) {
      return ret;
    }

    (*pQueryInfo) = tscGetQueryInfoDetail(pCmd, subClauseIndex);
  }

  return TSDB_CODE_SUCCESS;
}

SMeterMetaInfo* tscGetMeterMetaInfoByUid(SQueryInfo* pQueryInfo, uint64_t uid, int32_t* index) {
  int32_t k = -1;

  for (int32_t i = 0; i < pQueryInfo->numOfTables; ++i) {
    if (pQueryInfo->pMeterInfo[i]->pMeterMeta->uid == uid) {
      k = i;
      break;
    }
  }

  if (index != NULL) {
    *index = k;
  }

  assert(k != -1);
  return tscGetMeterMetaInfoFromQueryInfo(pQueryInfo, k);
}

int32_t tscAddSubqueryInfo(SSqlCmd* pCmd) {
  assert(pCmd != NULL);

  size_t s = pCmd->numOfClause + 1;
  char*  tmp = realloc(pCmd->pQueryInfo, s * POINTER_BYTES);
  if (tmp == NULL) {
    return TSDB_CODE_CLI_OUT_OF_MEMORY;
  }

  pCmd->pQueryInfo = (SQueryInfo**)tmp;

  SQueryInfo* pQueryInfo = calloc(1, sizeof(SQueryInfo));
  pQueryInfo->msg = pCmd->payload;  // pointer to the parent error message buffer

  pCmd->pQueryInfo[pCmd->numOfClause++] = pQueryInfo;
  return TSDB_CODE_SUCCESS;
}

static void doClearSubqueryInfo(SQueryInfo* pQueryInfo) {
  tscTagCondRelease(&pQueryInfo->tagCond);
  tscClearFieldInfo(&pQueryInfo->fieldsInfo);

  tscSqlExprInfoDestroy(&pQueryInfo->exprsInfo);
  memset(&pQueryInfo->exprsInfo, 0, sizeof(pQueryInfo->exprsInfo));

  tscColumnBaseInfoDestroy(&pQueryInfo->colList);
  memset(&pQueryInfo->colList, 0, sizeof(pQueryInfo->colList));

  pQueryInfo->tsBuf = tsBufDestory(pQueryInfo->tsBuf);

  tfree(pQueryInfo->defaultVal);
}

void tscClearSubqueryInfo(SSqlCmd* pCmd) {
  for (int32_t i = 0; i < pCmd->numOfClause; ++i) {
    SQueryInfo* pQueryInfo = tscGetQueryInfoDetail(pCmd, i);
    doClearSubqueryInfo(pQueryInfo);
  }
}

void tscFreeSubqueryInfo(SSqlCmd* pCmd) {
  if (pCmd == NULL || pCmd->numOfClause == 0) {
    return;
  }

  for (int32_t i = 0; i < pCmd->numOfClause; ++i) {
    char* addr = (char*)pCmd - offsetof(SSqlObj, cmd);

    SQueryInfo* pQueryInfo = tscGetQueryInfoDetail(pCmd, i);

    doClearSubqueryInfo(pQueryInfo);
    tscRemoveAllMeterMetaInfo(pQueryInfo, (const char*)addr, false);
    tfree(pQueryInfo);
  }

  pCmd->numOfClause = 0;
  tfree(pCmd->pQueryInfo);
}

SMeterMetaInfo* tscAddMeterMetaInfo(SQueryInfo* pQueryInfo, const char* name, SMeterMeta* pMeterMeta,
                                    SMetricMeta* pMetricMeta, int16_t numOfTags, int16_t* tags) {
  void* pAlloc = realloc(pQueryInfo->pMeterInfo, (pQueryInfo->numOfTables + 1) * POINTER_BYTES);
  if (pAlloc == NULL) {
    return NULL;
  }

  pQueryInfo->pMeterInfo = pAlloc;
  pQueryInfo->pMeterInfo[pQueryInfo->numOfTables] = calloc(1, sizeof(SMeterMetaInfo));

  SMeterMetaInfo* pMeterMetaInfo = pQueryInfo->pMeterInfo[pQueryInfo->numOfTables];
  assert(pMeterMetaInfo != NULL);

  if (name != NULL) {
    assert(strlen(name) <= TSDB_METER_ID_LEN);
    strcpy(pMeterMetaInfo->name, name);
  }

  pMeterMetaInfo->pMeterMeta = pMeterMeta;
  pMeterMetaInfo->pMetricMeta = pMetricMeta;
  pMeterMetaInfo->numOfTags = numOfTags;

  if (tags != NULL) {
    memcpy(pMeterMetaInfo->tagColumnIndex, tags, sizeof(pMeterMetaInfo->tagColumnIndex[0]) * numOfTags);
  }

  pQueryInfo->numOfTables += 1;
  return pMeterMetaInfo;
}

SMeterMetaInfo* tscAddEmptyMeterMetaInfo(SQueryInfo* pQueryInfo) {
  return tscAddMeterMetaInfo(pQueryInfo, NULL, NULL, NULL, 0, NULL);
}

void doRemoveMeterMetaInfo(SQueryInfo* pQueryInfo, int32_t index, bool removeFromCache) {
  if (index < 0 || index >= pQueryInfo->numOfTables) {
    return;
  }

  SMeterMetaInfo* pMeterMetaInfo = tscGetMeterMetaInfoFromQueryInfo(pQueryInfo, index);

  tscClearMeterMetaInfo(pMeterMetaInfo, removeFromCache);
  free(pMeterMetaInfo);

  int32_t after = pQueryInfo->numOfTables - index - 1;
  if (after > 0) {
    memmove(&pQueryInfo->pMeterInfo[index], &pQueryInfo->pMeterInfo[index + 1], after * POINTER_BYTES);
  }

  pQueryInfo->numOfTables -= 1;
}

void tscRemoveAllMeterMetaInfo(SQueryInfo* pQueryInfo, const char* address, bool removeFromCache) {
  tscTrace("%p deref the metric/meter meta in cache, numOfTables:%d", address, pQueryInfo->numOfTables);

  int32_t index = pQueryInfo->numOfTables;
  while (index >= 0) {
    doRemoveMeterMetaInfo(pQueryInfo, --index, removeFromCache);
  }

  tfree(pQueryInfo->pMeterInfo);
}

void tscClearMeterMetaInfo(SMeterMetaInfo* pMeterMetaInfo, bool removeFromCache) {
  if (pMeterMetaInfo == NULL) {
    return;
  }

  taosRemoveDataFromCache(tscCacheHandle, (void**)&(pMeterMetaInfo->pMeterMeta), removeFromCache);
  taosRemoveDataFromCache(tscCacheHandle, (void**)&(pMeterMetaInfo->pMetricMeta), removeFromCache);
}

void tscResetForNextRetrieve(SSqlRes* pRes) {
  if (pRes == NULL) {
    return;
  }

  pRes->row = 0;
  pRes->numOfRows = 0;
}

SSqlObj* createSubqueryObj(SSqlObj* pSql, int16_t tableIndex, void (*fp)(), void* param, SSqlObj* pPrevSql) {
  SSqlCmd*        pCmd = &pSql->cmd;
  SMeterMetaInfo* pMeterMetaInfo = tscGetMeterMetaInfo(pCmd, pCmd->clauseIndex, tableIndex);

  SSqlObj* pNew = (SSqlObj*)calloc(1, sizeof(SSqlObj));
  if (pNew == NULL) {
    tscError("%p new subquery failed, tableIndex:%d, vnodeIndex:%d", pSql, tableIndex, pMeterMetaInfo->vnodeIndex);
    return NULL;
  }

  pNew->pTscObj = pSql->pTscObj;
  pNew->signature = pNew;

  pNew->sqlstr = strdup(pSql->sqlstr);
  if (pNew->sqlstr == NULL) {
    tscError("%p new subquery failed, tableIndex:%d, vnodeIndex:%d", pSql, tableIndex, pMeterMetaInfo->vnodeIndex);

    free(pNew);
    return NULL;
  }

  memcpy(&pNew->cmd, pCmd, sizeof(SSqlCmd));

  pNew->cmd.command = TSDB_SQL_SELECT;
  pNew->cmd.payload = NULL;
  pNew->cmd.allocSize = 0;

  pNew->cmd.pQueryInfo = NULL;
  pNew->cmd.numOfClause = 0;
  pNew->cmd.clauseIndex = 0;

  if (tscAddSubqueryInfo(&pNew->cmd) != TSDB_CODE_SUCCESS) {
    tscFreeSqlObj(pNew);
    return NULL;
  }

  SQueryInfo* pNewQueryInfo = tscGetQueryInfoDetail(&pNew->cmd, 0);
  SQueryInfo* pQueryInfo = tscGetQueryInfoDetail(pCmd, pCmd->clauseIndex);

  memcpy(pNewQueryInfo, pQueryInfo, sizeof(SQueryInfo));

  memset(&pNewQueryInfo->colList, 0, sizeof(pNewQueryInfo->colList));
  memset(&pNewQueryInfo->fieldsInfo, 0, sizeof(SFieldInfo));

  pNewQueryInfo->pMeterInfo = NULL;
  pNewQueryInfo->defaultVal = NULL;
  pNewQueryInfo->numOfTables = 0;
  pNewQueryInfo->tsBuf = NULL;

  tscTagCondCopy(&pNewQueryInfo->tagCond, &pQueryInfo->tagCond);

  if (pQueryInfo->interpoType != TSDB_INTERPO_NONE) {
    pNewQueryInfo->defaultVal = malloc(pQueryInfo->fieldsInfo.numOfOutputCols * sizeof(int64_t));
    memcpy(pNewQueryInfo->defaultVal, pQueryInfo->defaultVal, pQueryInfo->fieldsInfo.numOfOutputCols * sizeof(int64_t));
  }

  if (tscAllocPayload(&pNew->cmd, TSDB_DEFAULT_PAYLOAD_SIZE) != TSDB_CODE_SUCCESS) {
    tscError("%p new subquery failed, tableIndex:%d, vnodeIndex:%d", pSql, tableIndex, pMeterMetaInfo->vnodeIndex);
    tscFreeSqlObj(pNew);
    return NULL;
  }

  tscColumnBaseInfoCopy(&pNewQueryInfo->colList, &pQueryInfo->colList, (int16_t)tableIndex);

  // set the correct query type
  if (pPrevSql != NULL) {
    SQueryInfo* pPrevQueryInfo = tscGetQueryInfoDetail(&pPrevSql->cmd, pPrevSql->cmd.clauseIndex);
    pNewQueryInfo->type = pPrevQueryInfo->type;
  } else {
    pNewQueryInfo->type |= TSDB_QUERY_TYPE_SUBQUERY;  // it must be the subquery
  }

  uint64_t uid = pMeterMetaInfo->pMeterMeta->uid;
  tscSqlExprCopy(&pNewQueryInfo->exprsInfo, &pQueryInfo->exprsInfo, uid, true);

  int32_t numOfOutputCols = pNewQueryInfo->exprsInfo.numOfExprs;

  if (numOfOutputCols > 0) {
    int32_t* indexList = calloc(1, numOfOutputCols * sizeof(int32_t));
    for (int32_t i = 0, j = 0; i < pQueryInfo->exprsInfo.numOfExprs; ++i) {
      SSqlExpr* pExpr = tscSqlExprGet(pQueryInfo, i);
      if (pExpr->uid == uid) {
        indexList[j++] = i;
      }
    }
    
    // create the fields info from the sql functions
    SColumnList columnList = {.num = 0};
  
    // for avg/last/first/histo.. query, the output type is binary not numeric data type
    for(int32_t k = 0; k < numOfOutputCols; ++k) {
      SSqlExpr* pExpr = tscSqlExprGet(pQueryInfo, indexList[k]);
      columnList.ids[0] = (SColumnIndex){.tableIndex = tableIndex, .columnIndex = pExpr->colInfo.colIdx};
      insertResultField(pNewQueryInfo, k, &columnList, pExpr->resBytes, pExpr->resType, pExpr->aliasName, pExpr);
    }

    free(indexList);
  
    //     make sure the the sqlExpr for each fields is correct
// todo handle the agg arithmetic expression
    for(int32_t f = 0; f < pNewQueryInfo->fieldsInfo.numOfOutputCols; ++f) {
      char* name = pNewQueryInfo->fieldsInfo.pFields[f].name;
      for(int32_t k1 = 0; k1 < pNewQueryInfo->exprsInfo.numOfExprs; ++k1) {
        SSqlExpr* pExpr1 = tscSqlExprGet(pNewQueryInfo, k1);
        if (strcmp(name, pExpr1->aliasName) == 0) {
          pNewQueryInfo->fieldsInfo.pSqlExpr[f] = pExpr1;
        }
      }
    }
  
    tscFieldInfoCalOffset(pNewQueryInfo);
  }

  pNew->fp = fp;
  pNew->param = param;

  char key[TSDB_MAX_TAGS_LEN + 1] = {0};
  tscGetMetricMetaCacheKey(pQueryInfo, key, uid);

#ifdef _DEBUG_VIEW
  printf("the metricmeta key is:%s\n", key);
#endif

  char*           name = pMeterMetaInfo->name;
  SMeterMetaInfo* pFinalInfo = NULL;

  if (pPrevSql == NULL) {
    SMeterMeta*  pMeterMeta = taosGetDataFromCache(tscCacheHandle, name);
    SMetricMeta* pMetricMeta = taosGetDataFromCache(tscCacheHandle, key);

    pFinalInfo = tscAddMeterMetaInfo(pNewQueryInfo, name, pMeterMeta, pMetricMeta, pMeterMetaInfo->numOfTags,
                                     pMeterMetaInfo->tagColumnIndex);
  } else {  // transfer the ownership of pMeterMeta/pMetricMeta to the newly create sql object.
    SMeterMetaInfo* pPrevInfo = tscGetMeterMetaInfo(&pPrevSql->cmd, pPrevSql->cmd.clauseIndex, 0);

    SMeterMeta*  pPrevMeterMeta = taosTransferDataInCache(tscCacheHandle, (void**)&pPrevInfo->pMeterMeta);
    SMetricMeta* pPrevMetricMeta = taosTransferDataInCache(tscCacheHandle, (void**)&pPrevInfo->pMetricMeta);

    pFinalInfo = tscAddMeterMetaInfo(pNewQueryInfo, name, pPrevMeterMeta, pPrevMetricMeta, pMeterMetaInfo->numOfTags,
                                     pMeterMetaInfo->tagColumnIndex);
  }

  if (pFinalInfo->pMeterMeta == NULL) {
    tscError("%p new subquery failed for get pMeterMeta is NULL from cache", pSql);
    tscFreeSqlObj(pNew);
    return NULL;
  }

  assert(pNewQueryInfo->numOfTables == 1);
  
  if (UTIL_METER_IS_SUPERTABLE(pMeterMetaInfo)) {
    assert(pFinalInfo->pMetricMeta != NULL);
  }
  
  tscTrace(
      "%p new subquery: %p, tableIndex:%d, vnodeIdx:%d, type:%d, exprInfo:%d, colList:%d,"
      "fieldInfo:%d, name:%s, qrang:%" PRId64 " - %" PRId64 " order:%d, limit:%" PRId64,
      pSql, pNew, tableIndex, pMeterMetaInfo->vnodeIndex, pNewQueryInfo->type, pNewQueryInfo->exprsInfo.numOfExprs,
      pNewQueryInfo->colList.numOfCols, pNewQueryInfo->fieldsInfo.numOfOutputCols, pFinalInfo->name, pNewQueryInfo->stime,
      pNewQueryInfo->etime, pNewQueryInfo->order.order, pNewQueryInfo->limit.limit);
  
  tscPrintSelectClause(pNew, 0);

  return pNew;
}

void tscDoQuery(SSqlObj* pSql) {
  SSqlCmd* pCmd = &pSql->cmd;
  void*    fp = pSql->fp;
  
  pSql->res.code = TSDB_CODE_SUCCESS;
  
  if (pCmd->command > TSDB_SQL_LOCAL) {
    tscProcessLocalCmd(pSql);
  } else {
    if (pCmd->command == TSDB_SQL_SELECT) {
      tscAddIntoSqlList(pSql);
    }

    if (pCmd->dataSourceType == DATA_FROM_DATA_FILE) {
      tscProcessMultiVnodesInsertFromFile(pSql);
    } else {
      // pSql may be released in this function if it is a async insertion.
      tscProcessSql(pSql);
      if (NULL == fp) tscProcessMultiVnodesInsert(pSql);
    }
  }
}

int16_t tscGetJoinTagColIndexByUid(STagCond* pTagCond, uint64_t uid) {
  if (pTagCond->joinInfo.left.uid == uid) {
    return pTagCond->joinInfo.left.tagCol;
  } else {
    return pTagCond->joinInfo.right.tagCol;
  }
}

bool tscIsUpdateQuery(STscObj* pObj) {
  if (pObj == NULL || pObj->signature != pObj) {
    globalCode = TSDB_CODE_DISCONNECTED;
    return TSDB_CODE_DISCONNECTED;
  }

  SSqlCmd* pCmd = &pObj->pSql->cmd;
  return ((pCmd->command >= TSDB_SQL_INSERT && pCmd->command <= TSDB_SQL_DROP_DNODE) ||
          TSDB_SQL_USE_DB == pCmd->command)
             ? 1
             : 0;
}

int32_t tscInvalidSQLErrMsg(char* msg, const char* additionalInfo, const char* sql) {
  const char* msgFormat1 = "invalid SQL: %s";
  const char* msgFormat2 = "invalid SQL: syntax error near \"%s\" (%s)";
  const char* msgFormat3 = "invalid SQL: syntax error near \"%s\"";

  const int32_t BACKWARD_CHAR_STEP = 0;

  if (sql == NULL) {
    assert(additionalInfo != NULL);
    sprintf(msg, msgFormat1, additionalInfo);
    return TSDB_CODE_INVALID_SQL;
  }

  char buf[64] = {0};  // only extract part of sql string
  strncpy(buf, (sql - BACKWARD_CHAR_STEP), tListLen(buf) - 1);

  if (additionalInfo != NULL) {
    sprintf(msg, msgFormat2, buf, additionalInfo);
  } else {
    sprintf(msg, msgFormat3, buf);  // no additional information for invalid sql error
  }

  return TSDB_CODE_INVALID_SQL;
}

bool tscHasReachLimitation(SQueryInfo* pQueryInfo, SSqlRes* pRes) {
  assert(pQueryInfo != NULL && pQueryInfo->clauseLimit != 0);
  return (pQueryInfo->clauseLimit > 0 && pRes->numOfTotalInCurrentClause >= pQueryInfo->clauseLimit);
}

char* tscGetErrorMsgPayload(SSqlCmd* pCmd) { return pCmd->payload; }

/**
 *  If current vnode query does not return results anymore (pRes->numOfRows == 0), try the next vnode if exists,
 *  in case of multi-vnode super table projection query and the result does not reach the limitation.
 */
bool hasMoreVnodesToTry(SSqlObj* pSql) {
  SSqlCmd* pCmd = &pSql->cmd;
  SSqlRes* pRes = &pSql->res;

  SQueryInfo* pQueryInfo = tscGetQueryInfoDetail(pCmd, pCmd->clauseIndex);
  
  SMeterMetaInfo* pMeterMetaInfo = tscGetMeterMetaInfoFromQueryInfo(pQueryInfo, 0);
  if (!UTIL_METER_IS_SUPERTABLE(pMeterMetaInfo) || (pMeterMetaInfo->pMetricMeta == NULL)) {
    return false;
  }
  
  int32_t totalVnode = pMeterMetaInfo->pMetricMeta->numOfVnodes;
  return pRes->numOfRows == 0 && tscNonOrderedProjectionQueryOnSTable(pQueryInfo, 0) &&
         (!tscHasReachLimitation(pQueryInfo, pRes)) && (pMeterMetaInfo->vnodeIndex < totalVnode - 1);
}

void tscTryQueryNextVnode(SSqlObj* pSql, __async_cb_func_t fp) {
  SSqlCmd* pCmd = &pSql->cmd;
  SSqlRes* pRes = &pSql->res;

  SQueryInfo* pQueryInfo = tscGetQueryInfoDetail(pCmd, pCmd->clauseIndex);

  /*
   * no result returned from the current virtual node anymore, try the next vnode if exists
   * if case of: multi-vnode super table projection query
   */
  assert(pRes->numOfRows == 0 && tscNonOrderedProjectionQueryOnSTable(pQueryInfo, 0) && !tscHasReachLimitation(pQueryInfo, pRes));

  SMeterMetaInfo* pMeterMetaInfo = tscGetMeterMetaInfoFromQueryInfo(pQueryInfo, 0);
  int32_t         totalVnode = pMeterMetaInfo->pMetricMeta->numOfVnodes;

  while (++pMeterMetaInfo->vnodeIndex < totalVnode) {
    tscTrace("%p current vnode:%d exhausted, try next:%d. total vnode:%d. current numOfRes:%d", pSql,
             pMeterMetaInfo->vnodeIndex - 1, pMeterMetaInfo->vnodeIndex, totalVnode, pRes->numOfTotalInCurrentClause);

    /*
     * update the limit and offset value for the query on the next vnode,
     * according to current retrieval results
     *
     * NOTE:
     * if the pRes->offset is larger than 0, the start returned position has not reached yet.
     * Therefore, the pRes->numOfRows, as well as pRes->numOfTotalInCurrentClause, must be 0.
     * The pRes->offset value will be updated by virtual node, during query execution.
     */
    if (pQueryInfo->clauseLimit >= 0) {
      pQueryInfo->limit.limit = pQueryInfo->clauseLimit - pRes->numOfTotalInCurrentClause;
    }

    pQueryInfo->limit.offset = pRes->offset;

    assert((pRes->offset >= 0 && pRes->numOfRows == 0) || (pRes->offset == 0 && pRes->numOfRows >= 0));
    tscTrace("%p new query to next vnode, vnode index:%d, limit:%" PRId64 ", offset:%" PRId64 ", glimit:%" PRId64, pSql,
             pMeterMetaInfo->vnodeIndex, pQueryInfo->limit.limit, pQueryInfo->limit.offset, pQueryInfo->clauseLimit);

    /*
     * For project query with super table join, the numOfSub is equalled to the number of all subqueries.
     * Therefore, we need to reset the value of numOfSubs to be 0.
     *
     * For super table join with projection query, if anyone of the subquery is exhausted, the query completed.
     */
    pSql->numOfSubs = 0;
    pCmd->command = TSDB_SQL_SELECT;

    tscResetForNextRetrieve(pRes);

    // in case of async query, set the callback function
    void* fp1 = pSql->fp;
    pSql->fp = fp;

    if (fp1 != NULL) {
      assert(fp != NULL);
    }

    int32_t ret = tscProcessSql(pSql);  // todo check for failure

    // in case of async query, return now
    if (fp != NULL) {
      return;
    }

    if (ret != TSDB_CODE_SUCCESS) {
      pSql->res.code = ret;
      return;
    }

    // retrieve data
    assert(pCmd->command == TSDB_SQL_SELECT);
    pCmd->command = TSDB_SQL_FETCH;

    if ((ret = tscProcessSql(pSql)) != TSDB_CODE_SUCCESS) {
      pSql->res.code = ret;
      return;
    }

    // if the result from current virtual node are empty, try next if exists. otherwise, return the results.
    if (pRes->numOfRows > 0) {
      break;
    }
  }

  if (pRes->numOfRows == 0) {
    tscTrace("%p all vnodes exhausted, prj query completed. total res:%d", pSql, totalVnode, pRes->numOfTotal);
  }
}

void tscTryQueryNextClause(SSqlObj* pSql, void (*queryFp)()) {
  SSqlCmd* pCmd = &pSql->cmd;
  SSqlRes* pRes = &pSql->res;

  // current subclause is completed, try the next subclause
  assert(pCmd->clauseIndex < pCmd->numOfClause - 1);

  pCmd->clauseIndex++;
  SQueryInfo* pQueryInfo = tscGetQueryInfoDetail(pCmd, pCmd->clauseIndex);

  pSql->cmd.command = pQueryInfo->command;

  //backup the total number of result first
  int64_t num = pRes->numOfTotal + pRes->numOfTotalInCurrentClause;
  tscFreeResData(pSql);
  
  pRes->numOfTotal = num;
  
  tfree(pSql->pSubs);
  pSql->numOfSubs = 0;
  
  if (pSql->fp != NULL) {
    pSql->fp = queryFp;
    assert(queryFp != NULL);
  }

  tscTrace("%p try data in the next subclause:%d, total subclause:%d", pSql, pCmd->clauseIndex, pCmd->numOfClause);
  if (pCmd->command > TSDB_SQL_LOCAL) {
    tscProcessLocalCmd(pSql);
  } else {
    tscProcessSql(pSql);
  }
}
