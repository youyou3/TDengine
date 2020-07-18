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
#include "tscSQLParser.h"
#include "ttime.h"
#include "ttimer.h"
#include "tutil.h"

#include "taosmsg.h"
#include "tscUtil.h"
#include "tsclient.h"

#include "tscProfile.h"

static void tscProcessStreamQueryCallback(void *param, TAOS_RES *tres, int numOfRows);
static void tscProcessStreamRetrieveResult(void *param, TAOS_RES *res, int numOfRows);
void tscSetNextLaunchTimer(SSqlStream *pStream, SSqlObj *pSql);
static void tscSetRetryTimer(SSqlStream *pStream, SSqlObj *pSql, int64_t timer);

static int64_t getDelayValueAfterTimewindowClosed(SSqlStream* pStream, int64_t launchDelay) {
  return taosGetTimestamp(pStream->precision) + launchDelay - pStream->stime - 1;
}

static bool isProjectStream(SQueryInfo* pQueryInfo) {
  for (int32_t i = 0; i < pQueryInfo->fieldsInfo.numOfOutputCols; ++i) {
    SSqlExpr *pExpr = tscSqlExprGet(pQueryInfo, i);
    if (pExpr->functionId != TSDB_FUNC_PRJ) {
      return false;
    }
  }

  return true;
}

static int64_t tscGetRetryDelayTime(int64_t slidingTime, int16_t prec) {
  float retryRangeFactor = 0.3;

  // change to ms
  if (prec == TSDB_TIME_PRECISION_MICRO) {
    slidingTime = slidingTime / 1000;
  }

  int64_t retryDelta = (int64_t)tsStreamCompRetryDelay * retryRangeFactor;
  retryDelta = ((rand() % retryDelta) + tsStreamCompRetryDelay) * 1000L;

  if (slidingTime < retryDelta) {
    return slidingTime;
  } else {
    return retryDelta;
  }
}

static void tscProcessStreamLaunchQuery(SSchedMsg *pMsg) {
  SSqlStream *pStream = (SSqlStream *)pMsg->ahandle;
  SSqlObj *   pSql = pStream->pSql;

  pSql->fp = tscProcessStreamQueryCallback;
  pSql->param = pStream;
  
  SQueryInfo *pQueryInfo = tscGetQueryInfoDetail(&pSql->cmd, 0);
  SMeterMetaInfo *pMeterMetaInfo = tscGetMeterMetaInfoFromQueryInfo(pQueryInfo, 0);

  int code = tscGetMeterMeta(pSql, pMeterMetaInfo);
  pSql->res.code = code;

  if (code == TSDB_CODE_ACTION_IN_PROGRESS) return;

  if (code == 0 && UTIL_METER_IS_SUPERTABLE(pMeterMetaInfo)) {
    code = tscGetMetricMeta(pSql, 0);
    pSql->res.code = code;

    if (code == TSDB_CODE_ACTION_IN_PROGRESS) return;
  }

  tscTansformSQLFunctionForSTableQuery(pQueryInfo);

  // failed to get meter/metric meta, retry in 10sec.
  if (code != TSDB_CODE_SUCCESS) {
    int64_t retryDelayTime = tscGetRetryDelayTime(pStream->slidingTime, pStream->precision);
    tscError("%p stream:%p,get metermeta failed, retry in %" PRId64 "ms", pStream->pSql, pStream, retryDelayTime);
  
    tscSetRetryTimer(pStream, pSql, retryDelayTime);
    return;
  }

  if ((UTIL_METER_IS_SUPERTABLE(pMeterMetaInfo) 
      && ( pMeterMetaInfo->pMeterMeta  == NULL 
        || pMeterMetaInfo->pMetricMeta == NULL 
        || pMeterMetaInfo->pMetricMeta->numOfMeters == 0 
        || pMeterMetaInfo->pMetricMeta->numOfVnodes == 0)) 
      || (!(UTIL_METER_IS_SUPERTABLE(pMeterMetaInfo))  && (pMeterMetaInfo->pMeterMeta  == NULL))) {
    tscTrace("%p no table in metricmeta, no launch query", pSql);
    tscClearMeterMetaInfo(pMeterMetaInfo, false);
    tscSetNextLaunchTimer(pStream, pSql);
    return;
  }

  tscTrace("%p stream:%p start stream query on:%s", pSql, pStream, pMeterMetaInfo->name);
  tscProcessSql(pStream->pSql);

  tscIncStreamExecutionCount(pStream);
}

static void tscProcessStreamTimer(void *handle, void *tmrId) {
  SSqlStream *pStream = (SSqlStream *)handle;
  if (pStream == NULL) return;
  if (pStream->pTimer != tmrId) return;
  pStream->pTimer = NULL;

  pStream->numOfRes = 0;  // reset the numOfRes.
  SSqlObj *pSql = pStream->pSql;
  SQueryInfo* pQueryInfo = tscGetQueryInfoDetail(&pSql->cmd, 0);
  tscTrace("%p add into timer", pSql);

  if (isProjectStream(pQueryInfo)) {
    /*
     * pQueryInfo->etime, which is the start time, does not change in case of
     * repeat first execution, once the first execution failed.
     */
    pQueryInfo->stime = pStream->stime;  // start time

    pQueryInfo->etime = taosGetTimestamp(pStream->precision);  // end time
    if (pQueryInfo->etime > pStream->etime) {
      pQueryInfo->etime = pStream->etime;
    }
  } else {
    pQueryInfo->stime = pStream->stime - pStream->interval;
    pQueryInfo->etime = pStream->stime - 1;
  }

  // launch stream computing in a new thread
  SSchedMsg schedMsg;
  schedMsg.fp = tscProcessStreamLaunchQuery;
  schedMsg.ahandle = pStream;
  schedMsg.thandle = (void *)1;
  schedMsg.msg = NULL;
  taosScheduleTask(tscQhandle, &schedMsg);
}

static void tscProcessStreamQueryCallback(void *param, TAOS_RES *tres, int numOfRows) {
  SSqlStream *pStream = (SSqlStream *)param;
  if (tres == NULL || numOfRows < 0) {
    int64_t retryDelay = tscGetRetryDelayTime(pStream->slidingTime, pStream->precision);
    tscError("%p stream:%p, query data failed, code:%d, retry in %" PRId64 "ms", pStream->pSql, pStream, numOfRows,
             retryDelay);

    SMeterMetaInfo* pMeterMetaInfo = tscGetMeterMetaInfo(&pStream->pSql->cmd, 0, 0);
    tscClearMeterMetaInfo(pMeterMetaInfo, true);
  
    tscSetRetryTimer(pStream, pStream->pSql, retryDelay);
    return;
  }

  taos_fetch_rows_a(tres, tscProcessStreamRetrieveResult, param);
}

static void tscSetTimestampForRes(SSqlStream *pStream, SSqlObj *pSql) {
  SSqlRes *pRes = &pSql->res;

  int64_t  timestamp = *(int64_t *)pRes->data;
  int64_t actualTimestamp = pStream->stime - pStream->interval;

  if (timestamp != actualTimestamp) {
    // reset the timestamp of each agg point by using start time of each interval
    *((int64_t *)pRes->data) = actualTimestamp;
    tscWarn("%p stream:%p, timestamp of points is:%" PRId64 ", reset to %" PRId64 "", pSql, pStream, timestamp, actualTimestamp);
  }
}

static void tscProcessStreamRetrieveResult(void *param, TAOS_RES *res, int numOfRows) {
  SSqlStream *    pStream = (SSqlStream *)param;
  SSqlObj *       pSql = (SSqlObj *)res;
  SMeterMetaInfo *pMeterMetaInfo = tscGetMeterMetaInfo(&pSql->cmd, 0, 0);

  if (pSql == NULL || numOfRows < 0) {
    int64_t retryDelayTime = tscGetRetryDelayTime(pStream->slidingTime, pStream->precision);
    tscError("%p stream:%p, retrieve data failed, code:%d, retry in %" PRId64 "ms", pSql, pStream, numOfRows, retryDelayTime);
    tscClearMeterMetaInfo(pMeterMetaInfo, true);
  
    tscSetRetryTimer(pStream, pStream->pSql, retryDelayTime);
    return;
  }

  if (numOfRows > 0) { // when reaching here the first execution of stream computing is successful.
    pStream->numOfRes += numOfRows;
    SQueryInfo* pQueryInfo = tscGetQueryInfoDetail(&pSql->cmd, 0);
    
    for(int32_t i = 0; i < numOfRows; ++i) {
      TAOS_ROW row = taos_fetch_row(res);
      tscTrace("%p stream:%p fetch result", pSql, pStream);
      if (isProjectStream(pQueryInfo)) {
        pStream->stime = *(TSKEY *)row[0];
      } else {
        tscSetTimestampForRes(pStream, pSql);
      }

      // user callback function
      (*pStream->fp)(pStream->param, res, row);
    }

    // actually only one row is returned. this following is not necessary
    taos_fetch_rows_a(res, tscProcessStreamRetrieveResult, pStream);
  } else {  // numOfRows == 0, all data has been retrieved
    pStream->useconds += pSql->res.useconds;

    SQueryInfo* pQueryInfo = tscGetQueryInfoDetail(&pSql->cmd, 0);
    
    if (pStream->numOfRes == 0) {
      if (pQueryInfo->interpoType == TSDB_INTERPO_SET_VALUE || pQueryInfo->interpoType == TSDB_INTERPO_NULL) {
        SSqlRes *pRes = &pSql->res;

        /* failed to retrieve any result in this retrieve */
        pSql->res.numOfRows = 1;
        void *row[TSDB_MAX_COLUMNS] = {0};
        char  tmpRes[TSDB_MAX_BYTES_PER_ROW] = {0};

        void *oldPtr = pSql->res.data;
        pSql->res.data = tmpRes;
  
        for (int32_t i = 1; i < pQueryInfo->fieldsInfo.numOfOutputCols; ++i) {
          int16_t     offset = tscFieldInfoGetOffset(pQueryInfo, i);
          TAOS_FIELD *pField = tscFieldInfoGetField(pQueryInfo, i);

          assignVal(pSql->res.data + offset, (char *)(&pQueryInfo->defaultVal[i]), pField->bytes, pField->type);
          row[i] = pSql->res.data + offset;
        }

        tscSetTimestampForRes(pStream, pSql);
        row[0] = pRes->data;

        //            char result[512] = {0};
        //            taos_print_row(result, row, pQueryInfo->fieldsInfo.pFields, pQueryInfo->fieldsInfo.numOfOutputCols);
        //            tscPrint("%p stream:%p query result: %s", pSql, pStream, result);
        tscTrace("%p stream:%p fetch result", pSql, pStream);

        // user callback function
        (*pStream->fp)(pStream->param, res, row);

        pRes->numOfRows = 0;
        pRes->data = oldPtr;
      } else if (isProjectStream(pQueryInfo)) {
        /* no resuls in the query range, retry */
        // todo set retry dynamic time
        int32_t retry = tsProjectExecInterval;
        tscError("%p stream:%p, retrieve no data, code:%d, retry in %" PRId64 "ms", pSql, pStream, numOfRows, retry);

        tscSetRetryTimer(pStream, pStream->pSql, retry);
        return;
      }
    } else {
      if (isProjectStream(pQueryInfo)) {
        pStream->stime += 1;
      }
    }

    tscTrace("%p stream:%p, query on:%s, fetch result completed, fetched rows:%d", pSql, pStream, pMeterMetaInfo->name,
             pStream->numOfRes);

    // release the metric/meter meta information reference, so data in cache can be updated
    tscClearMeterMetaInfo(pMeterMetaInfo, false);
    tscSetNextLaunchTimer(pStream, pSql);
  }
}

static void tscSetRetryTimer(SSqlStream *pStream, SSqlObj *pSql, int64_t timer) {
  SQueryInfo* pQueryInfo = tscGetQueryInfoDetail(&pSql->cmd, 0);
  int64_t delay = getDelayValueAfterTimewindowClosed(pStream, timer);
  
  if (isProjectStream(pQueryInfo)) {
    int64_t now = taosGetTimestamp(pStream->precision);
    int64_t etime = now > pStream->etime ? pStream->etime : now;

    if (pStream->etime < now && now - pStream->etime > tsMaxRetentWindow) {
      /*
       * current time window will be closed, since it too early to exceed the maxRetentWindow value
       */
      tscTrace("%p stream:%p, etime:%" PRId64 " is too old, exceeds the max retention time window:%" PRId64 ", stop the stream",
               pStream->pSql, pStream, pStream->stime, pStream->etime);
      // TODO : How to terminate stream here
      if (pStream->callback) {
        // Callback function from upper level
        pStream->callback(pStream->param);
      }
      taos_close_stream(pStream);
      return;
    }
  
    tscTrace("%p stream:%p, next start at %" PRId64 ", in %" PRId64 "ms. delay:%" PRId64 "ms qrange %" PRId64 "-%" PRId64 "", pStream->pSql, pStream,
             now + timer, timer, delay, pStream->stime, etime);
  } else {
    tscTrace("%p stream:%p, next start at %" PRId64 ", in %" PRId64 "ms. delay:%" PRId64 "ms qrange %" PRId64 "-%" PRId64 "", pStream->pSql, pStream,
             pStream->stime, timer, delay, pStream->stime - pStream->interval, pStream->stime - 1);
  }

  pSql->cmd.command = TSDB_SQL_SELECT;

  // start timer for next computing
  taosTmrReset(tscProcessStreamTimer, timer, pStream, tscTmr, &pStream->pTimer);
}

static int64_t getLaunchTimeDelay(const SSqlStream* pStream) {
  int64_t delayDelta = (int64_t)(pStream->slidingTime * tsStreamComputDelayRatio);
  
  int64_t maxDelay =
      (pStream->precision == TSDB_TIME_PRECISION_MICRO) ? tsMaxStreamComputDelay * 1000L : tsMaxStreamComputDelay;
  
  if (delayDelta > maxDelay) {
    delayDelta = maxDelay;
  }
  
  int64_t remainTimeWindow = pStream->slidingTime - delayDelta;
  if (maxDelay > remainTimeWindow) {
    maxDelay = (remainTimeWindow / 1.5);
  }
  
  int64_t currentDelay = (rand() % maxDelay);  // a random number
  currentDelay += delayDelta;
  assert(currentDelay < pStream->slidingTime);
  
  return currentDelay;
}


void tscSetNextLaunchTimer(SSqlStream *pStream, SSqlObj *pSql) {
  int64_t timer = 0;
  
  SQueryInfo* pQueryInfo = tscGetQueryInfoDetail(&pSql->cmd, 0);
  if (isProjectStream(pQueryInfo)) {
    /*
     * for project query, no mater fetch data successfully or not, next launch will issue
     * more than the sliding time window
     */
    timer = pStream->slidingTime;
    if (pStream->stime > pStream->etime) {
      tscTrace("%p stream:%p, stime:%" PRId64 " is larger than end time: %" PRId64 ", stop the stream", pStream->pSql, pStream,
               pStream->stime, pStream->etime);
      // TODO : How to terminate stream here
      if (pStream->callback) {
        // Callback function from upper level
        pStream->callback(pStream->param);
      }
      taos_close_stream(pStream);
      return;
    }
  } else {
    pStream->stime += pStream->slidingTime;
    if ((pStream->stime - pStream->interval) >= pStream->etime) {
      tscTrace("%p stream:%p, stime:%" PRId64 " is larger than end time: %" PRId64 ", stop the stream", pStream->pSql, pStream,
               pStream->stime, pStream->etime);
      // TODO : How to terminate stream here
      if (pStream->callback) {
        // Callback function from upper level
        pStream->callback(pStream->param);
      }
      taos_close_stream(pStream);
      return;
    }
    
    timer = pStream->stime - taosGetTimestamp(pStream->precision);
    if (timer < 0) {
      timer = 0;
    }
  }

  timer += getLaunchTimeDelay(pStream);
  
  if (pStream->precision == TSDB_TIME_PRECISION_MICRO) {
    timer = timer / 1000L;
  }

  tscSetRetryTimer(pStream, pSql, timer);
}

static void tscSetSlidingWindowInfo(SSqlObj *pSql, SSqlStream *pStream) {
  int64_t minIntervalTime = tsMinIntervalTime;
  
  SQueryInfo* pQueryInfo = tscGetQueryInfoDetail(&pSql->cmd, 0);
  
  if (pQueryInfo->intervalTime < minIntervalTime) {
    tscWarn("%p stream:%p, original sample interval:%ld too small, reset to:%" PRId64 "", pSql, pStream,
            pQueryInfo->intervalTime, minIntervalTime);
    pQueryInfo->intervalTime = minIntervalTime;
  }

  pStream->interval = pQueryInfo->intervalTime;  // it shall be derived from sql string

  if (pQueryInfo->slidingTime == 0) {
    pQueryInfo->slidingTime = pQueryInfo->intervalTime;
  }

  int64_t minSlidingTime = tsMinSlidingTime;

  if (pQueryInfo->slidingTime == -1) {
    pQueryInfo->slidingTime = pQueryInfo->intervalTime;
  } else if (pQueryInfo->slidingTime < minSlidingTime) {
    tscWarn("%p stream:%p, original sliding value:%" PRId64 " too small, reset to:%" PRId64 "", pSql, pStream,
        pQueryInfo->slidingTime, minSlidingTime);

    pQueryInfo->slidingTime = minSlidingTime;
  }

  if (pQueryInfo->slidingTime > pQueryInfo->intervalTime) {
    tscWarn("%p stream:%p, sliding value:%" PRId64 " can not be larger than interval range, reset to:%" PRId64 "", pSql, pStream,
            pQueryInfo->slidingTime, pQueryInfo->intervalTime);

    pQueryInfo->slidingTime = pQueryInfo->intervalTime;
  }

  pStream->slidingTime = pQueryInfo->slidingTime;
  
  pQueryInfo->intervalTime = 0; // clear the interval value to avoid the force time window split by query processor
  pQueryInfo->slidingTime = 0;
}

static int64_t tscGetStreamStartTimestamp(SSqlObj *pSql, SSqlStream *pStream, int64_t stime) {
  SQueryInfo* pQueryInfo = tscGetQueryInfoDetail(&pSql->cmd, 0);
  
  if (isProjectStream(pQueryInfo)) {
    // no data in table, flush all data till now to destination meter, 10sec delay
    pStream->interval = tsProjectExecInterval;
    pStream->slidingTime = tsProjectExecInterval;

    if (stime != 0) {  // first projection start from the latest event timestamp
      assert(stime >= pQueryInfo->stime);
      stime += 1;  // exclude the last records from table
    } else {
      stime = pQueryInfo->stime;
    }
  } else {             // timewindow based aggregation stream
    if (stime == 0) {  // no data in meter till now
      stime = ((int64_t)taosGetTimestamp(pStream->precision) / pStream->interval) * pStream->interval;
      tscWarn("%p stream:%p, last timestamp:0, reset to:%" PRId64 "", pSql, pStream, stime);
    } else {
      int64_t newStime = (stime / pStream->interval) * pStream->interval;
      if (newStime != stime) {
        tscWarn("%p stream:%p, last timestamp:%" PRId64 ", reset to:%" PRId64 "", pSql, pStream, stime, newStime);
        stime = newStime;
      }
    }
  }

  return stime;
}

static int64_t tscGetLaunchTimestamp(const SSqlStream *pStream) {
  int64_t timer = pStream->stime - taosGetTimestamp(pStream->precision);
  if (timer < 0) timer = 0;

  int64_t startDelay =
      (pStream->precision == TSDB_TIME_PRECISION_MICRO) ? tsStreamCompStartDelay * 1000L : tsStreamCompStartDelay;
  
  timer += getLaunchTimeDelay(pStream);
  timer += startDelay;
  
  return (pStream->precision == TSDB_TIME_PRECISION_MICRO) ? timer / 1000L : timer;
}

static void setErrorInfo(STscObj* pObj, int32_t code, char* info) {
  if (pObj == NULL) {
    return;
  }

  SSqlCmd* pCmd = &pObj->pSql->cmd;

  pObj->pSql->res.code = code;
  
  if (info != NULL) {
    strncpy(pCmd->payload, info, pCmd->payloadLen);
  }
}

TAOS_STREAM *taos_open_stream(TAOS *taos, const char *sqlstr, void (*fp)(void *param, TAOS_RES *, TAOS_ROW row),
                              int64_t stime, void *param, void (*callback)(void *)) {
  STscObj *pObj = (STscObj *)taos;
  if (pObj == NULL || pObj->signature != pObj) return NULL;

  SSqlObj *pSql = (SSqlObj *)calloc(1, sizeof(SSqlObj));
  if (pSql == NULL) {
    setErrorInfo(pObj, TSDB_CODE_CLI_OUT_OF_MEMORY, NULL);
    return NULL;
  }

  pSql->signature = pSql;
  pSql->pTscObj = pObj;
  SSqlCmd *pCmd = &pSql->cmd;
  SSqlRes *pRes = &pSql->res;
  int ret = tscAllocPayload(pCmd, TSDB_DEFAULT_PAYLOAD_SIZE);
  if (TSDB_CODE_SUCCESS != ret) {
    setErrorInfo(pObj, ret, NULL);
    free(pSql);
    return NULL;
  }

  pSql->sqlstr = strdup(sqlstr);
  if (pSql->sqlstr == NULL) {
    setErrorInfo(pObj, TSDB_CODE_CLI_OUT_OF_MEMORY, NULL);

    tfree(pSql);
    return NULL;
  }

  tsem_init(&pSql->rspSem, 0, 0);
  tsem_init(&pSql->emptyRspSem, 0, 1);

  SSqlInfo SQLInfo = {0};
  tSQLParse(&SQLInfo, pSql->sqlstr);

  tscCleanSqlCmd(&pSql->cmd);
  ret = tscAllocPayload(&pSql->cmd, TSDB_DEFAULT_PAYLOAD_SIZE);
  if (TSDB_CODE_SUCCESS != ret) {
    setErrorInfo(pObj, ret, NULL);
    tscError("%p open stream failed, sql:%s, code:%d", pSql, sqlstr, TSDB_CODE_CLI_OUT_OF_MEMORY);
    tscFreeSqlObj(pSql);
    return NULL;
  }
  
  pSql->cmd.inStream = 1;  // 1 means sql in stream, allowed the sliding clause.
  pRes->code = tscToSQLCmd(pSql, &SQLInfo);
  SQLInfoDestroy(&SQLInfo);

  if (pRes->code != TSDB_CODE_SUCCESS) {
    setErrorInfo(pObj, pRes->code, pCmd->payload);

    tscError("%p open stream failed, sql:%s, reason:%s, code:%d", pSql, sqlstr, pCmd->payload, pRes->code);
    tscFreeSqlObj(pSql);
    return NULL;
  }

  SSqlStream *pStream = (SSqlStream *)calloc(1, sizeof(SSqlStream));
  if (pStream == NULL) {
    setErrorInfo(pObj, TSDB_CODE_CLI_OUT_OF_MEMORY, NULL);

    tscError("%p open stream failed, sql:%s, reason:%s, code:%d", pSql, sqlstr, pCmd->payload, pRes->code);
    tscFreeSqlObj(pSql);
    return NULL;
  }

  SQueryInfo* pQueryInfo = tscGetQueryInfoDetail(pCmd, 0);
  SMeterMetaInfo* pMeterMetaInfo = tscGetMeterMetaInfoFromQueryInfo(pQueryInfo, 0);

  pStream->fp = fp;
  pStream->callback = callback;
  pStream->param = param;
  pStream->pSql = pSql;
  pStream->precision = pMeterMetaInfo->pMeterMeta->precision;

  pStream->ctime = taosGetTimestamp(pStream->precision);
  pStream->etime = pQueryInfo->etime;

  pSql->pStream = pStream;
  tscAddIntoStreamList(pStream);

  tscSetSlidingWindowInfo(pSql, pStream);
  pStream->stime = tscGetStreamStartTimestamp(pSql, pStream, stime);

  int64_t starttime = tscGetLaunchTimestamp(pStream);
  taosTmrReset(tscProcessStreamTimer, starttime, pStream, tscTmr, &pStream->pTimer);

  tscTrace("%p stream:%p is opened, query on:%s, interval:%" PRId64 ", sliding:%" PRId64 ", first launched in:%" PRId64 ", sql:%s", pSql,
           pStream, pMeterMetaInfo->name, pStream->interval, pStream->slidingTime, starttime, sqlstr);

  return pStream;
}

void taos_close_stream(TAOS_STREAM *handle) {
  SSqlStream *pStream = (SSqlStream *)handle;

  SSqlObj *pSql = (SSqlObj *)atomic_exchange_ptr(&pStream->pSql, 0);
  if (pSql == NULL) {
    return;
  }

  /*
   * stream may be closed twice, 1. drop dst table, 2. kill stream
   * Here, we need a check before release memory
   */
  if (pSql->signature == pSql) {
    tscRemoveFromStreamList(pStream, pSql);

    taosTmrStopA(&(pStream->pTimer));
    tscTrace("%p stream:%p is closed", pSql, pStream);
    tscFreeSqlObj(pSql);
    pStream->pSql = NULL;

    tfree(pStream);
  }
}
