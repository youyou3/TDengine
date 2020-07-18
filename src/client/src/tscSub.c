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

#include "shash.h"
#include "taos.h"
#include "tlog.h"
#include "trpc.h"
#include "tsclient.h"
#include "tsocket.h"
#include "ttime.h"
#include "ttimer.h"
#include "tutil.h"
#include "tscUtil.h"
#include "tcache.h"
#include "tscProfile.h"

typedef struct SSubscriptionProgress {
  int64_t uid;
  TSKEY key;
} SSubscriptionProgress;

typedef struct SSub {
  void *                  signature;
  char                    topic[32];
  int64_t                 lastSyncTime;
  int64_t                 lastConsumeTime;
  TAOS *                  taos;
  void *                  pTimer;
  SSqlObj *               pSql;
  int                     interval;
  TAOS_SUBSCRIBE_CALLBACK fp;
  void *                  param;
  int                     numOfMeters;
  SSubscriptionProgress * progress;
} SSub;


static int tscCompareSubscriptionProgress(const void* a, const void* b) {
  const SSubscriptionProgress* x = (const SSubscriptionProgress*)a;
  const SSubscriptionProgress* y = (const SSubscriptionProgress*)b;
  if (x->uid > y->uid) return 1;
  if (x->uid < y->uid) return -1;
  return 0;
}

TSKEY tscGetSubscriptionProgress(void* sub, int64_t uid) {
  if (sub == NULL)
    return 0;

  SSub* pSub = (SSub*)sub;
  for (int s = 0, e = pSub->numOfMeters; s < e;) {
    int m = (s + e) / 2;
    SSubscriptionProgress* p = pSub->progress + m;
    if (p->uid > uid)
      e = m;
    else if (p->uid < uid)
      s = m + 1;
    else
      return p->key;
  }

  return 0;
}

void tscUpdateSubscriptionProgress(void* sub, int64_t uid, TSKEY ts) {
  if( sub == NULL)
    return;

  SSub* pSub = (SSub*)sub;
  for (int s = 0, e = pSub->numOfMeters; s < e;) {
    int m = (s + e) / 2;
    SSubscriptionProgress* p = pSub->progress + m;
    if (p->uid > uid)
      e = m;
    else if (p->uid < uid)
      s = m + 1;
    else {
      if (ts >= p->key) p->key = ts;
      break;
    }
  }
}


static SSub* tscCreateSubscription(STscObj* pObj, const char* topic, const char* sql) {
  SSub* pSub = calloc(1, sizeof(SSub));
  if (pSub == NULL) {
    globalCode = TSDB_CODE_CLI_OUT_OF_MEMORY;
    tscError("failed to allocate memory for subscription");
    return NULL;
  }

  char* sqlstr = NULL;
  SSqlObj* pSql = calloc(1, sizeof(SSqlObj));
  if (pSql == NULL) {
    globalCode = TSDB_CODE_CLI_OUT_OF_MEMORY;
    tscError("failed to allocate SSqlObj for subscription");
    goto failed;
  }

  pSql->signature = pSql;
  pSql->pTscObj = pObj;

  sqlstr = (char*)malloc(strlen(sql) + 1);
  if (sqlstr == NULL) {
    tscError("failed to allocate sql string for subscription");
    goto failed;
  }
  strcpy(sqlstr, sql);
  strtolower(sqlstr, sqlstr);
  pSql->sqlstr = sqlstr;

  tsem_init(&pSql->rspSem, 0, 0);
  tsem_init(&pSql->emptyRspSem, 0, 1);

  SSqlRes *pRes = &pSql->res;
  pRes->numOfRows = 1;
  pRes->numOfTotal = 0;

  pSql->pSubscription = pSub;
  pSub->pSql = pSql;
  pSub->signature = pSub;
  strncpy(pSub->topic, topic, sizeof(pSub->topic));
  pSub->topic[sizeof(pSub->topic) - 1] = 0;
  return pSub;

failed:
  if (sqlstr != NULL) {
    free(sqlstr);
  }
  if (pSql != NULL) {
    free(pSql);
  }
  free(pSub);
  return NULL;
}


static void tscProcessSubscriptionTimer(void *handle, void *tmrId) {
  SSub *pSub = (SSub *)handle;
  if (pSub == NULL || pSub->pTimer != tmrId) return;

  TAOS_RES* res = taos_consume(pSub);
  if (res != NULL) {
    pSub->fp(pSub, res, pSub->param, 0);
  }

  taosTmrReset(tscProcessSubscriptionTimer, pSub->interval, pSub, tscTmr, &pSub->pTimer);
}


int tscUpdateSubscription(STscObj* pObj, SSub* pSub) {
  int code = (uint8_t)tsParseSql(pSub->pSql, false);
  if (code != TSDB_CODE_SUCCESS) {
    tscError("failed to parse sql statement: %s", pSub->topic);
    return 0;
  }

  SSqlCmd* pCmd = &pSub->pSql->cmd;
  if (pCmd->command != TSDB_SQL_SELECT) {
    tscError("only 'select' statement is allowed in subscription: %s", pSub->topic);
    return 0;
  }

  SMeterMetaInfo *pMeterMetaInfo = tscGetMeterMetaInfo(pCmd, 0, 0);
  int numOfMeters = 0;
  if (!UTIL_METER_IS_NOMRAL_METER(pMeterMetaInfo)) {
    SMetricMeta* pMetricMeta = pMeterMetaInfo->pMetricMeta;
    for (int32_t i = 0; i < pMetricMeta->numOfVnodes; i++) {
      SVnodeSidList *pVnodeSidList = tscGetVnodeSidList(pMetricMeta, i);
      numOfMeters += pVnodeSidList->numOfSids;
    }
  }

  SSubscriptionProgress* progress = (SSubscriptionProgress*)calloc(numOfMeters, sizeof(SSubscriptionProgress));
  if (progress == NULL) {
    tscError("failed to allocate memory for progress: %s", pSub->topic);
    return 0;
  }

  if (UTIL_METER_IS_NOMRAL_METER(pMeterMetaInfo)) {
    numOfMeters = 1;
    int64_t uid = pMeterMetaInfo->pMeterMeta->uid;
    progress[0].uid = uid;
    progress[0].key = tscGetSubscriptionProgress(pSub, uid);
  } else {
    SMetricMeta* pMetricMeta = pMeterMetaInfo->pMetricMeta;
    numOfMeters = 0;
    for (int32_t i = 0; i < pMetricMeta->numOfVnodes; i++) {
      SVnodeSidList *pVnodeSidList = tscGetVnodeSidList(pMetricMeta, i);
      for (int32_t j = 0; j < pVnodeSidList->numOfSids; j++) {
        SMeterSidExtInfo *pMeterInfo = tscGetMeterSidInfo(pVnodeSidList, j);
        int64_t uid = pMeterInfo->uid;
        progress[numOfMeters].uid = uid;
        progress[numOfMeters++].key = tscGetSubscriptionProgress(pSub, uid);
      }
    }
    qsort(progress, numOfMeters, sizeof(SSubscriptionProgress), tscCompareSubscriptionProgress);
  }

  free(pSub->progress);
  pSub->numOfMeters = numOfMeters;
  pSub->progress = progress;

  pSub->lastSyncTime = taosGetTimestampMs();

  return 1;
}


static int tscLoadSubscriptionProgress(SSub* pSub) {
  char buf[TSDB_MAX_SQL_LEN];
  sprintf(buf, "%s/subscribe/%s", dataDir, pSub->topic);

  FILE* fp = fopen(buf, "r");
  if (fp == NULL) {
    tscTrace("subscription progress file does not exist: %s", pSub->topic);
    return 1;
  }

  if (fgets(buf, sizeof(buf), fp) == NULL) {
    tscTrace("invalid subscription progress file: %s", pSub->topic);
    fclose(fp);
    return 0;
  }

  for (int i = 0; i < sizeof(buf); i++) {
    if (buf[i] == 0)
      break;
    if (buf[i] == '\r' || buf[i] == '\n') {
      buf[i] = 0;
      break;
    }
  }
  if (strcmp(buf, pSub->pSql->sqlstr) != 0) {
    tscTrace("subscription sql statement mismatch: %s", pSub->topic);
    fclose(fp);
    return 0;
  }

  if (fgets(buf, sizeof(buf), fp) == NULL || atoi(buf) < 0) {
    tscTrace("invalid subscription progress file: %s", pSub->topic);
    fclose(fp);
    return 0;
  }

  int numOfMeters = atoi(buf);
  SSubscriptionProgress* progress = calloc(numOfMeters, sizeof(SSubscriptionProgress));
  for (int i = 0; i < numOfMeters; i++) {
    if (fgets(buf, sizeof(buf), fp) == NULL) {
      fclose(fp);
      free(progress);
      return 0;
    }
    int64_t uid, key;
    sscanf(buf, "%" SCNd64 ":%" SCNd64, &uid, &key);
    progress[i].uid = uid;
    progress[i].key = key;
  }

  fclose(fp);

  qsort(progress, numOfMeters, sizeof(SSubscriptionProgress), tscCompareSubscriptionProgress);
  pSub->numOfMeters = numOfMeters;
  pSub->progress = progress;
  tscTrace("subscription progress loaded, %d tables: %s", numOfMeters, pSub->topic);
  return 1;
}

void tscSaveSubscriptionProgress(void* sub) {
  SSub* pSub = (SSub*)sub;

  char path[256];
  sprintf(path, "%s/subscribe", dataDir);
  if (access(path, 0) != 0) {
    mkdir(path, 0777);
  }

  sprintf(path, "%s/subscribe/%s", dataDir, pSub->topic);
  FILE* fp = fopen(path, "w+");
  if (fp == NULL) {
    tscError("failed to create progress file for subscription: %s", pSub->topic);
    return;
  }

  fputs(pSub->pSql->sqlstr, fp);
  fprintf(fp, "\n%d\n", pSub->numOfMeters);
  for (int i = 0; i < pSub->numOfMeters; i++) {
    int64_t uid = pSub->progress[i].uid;
    TSKEY key = pSub->progress[i].key;
    fprintf(fp, "%" PRId64 ":%" PRId64 "\n", uid, key);
  }

  fclose(fp);
}

TAOS_SUB *taos_subscribe(TAOS *taos, int restart, const char* topic, const char *sql, TAOS_SUBSCRIBE_CALLBACK fp, void *param, int interval) {
  STscObj* pObj = (STscObj*)taos;
  if (pObj == NULL || pObj->signature != pObj) {
    globalCode = TSDB_CODE_DISCONNECTED;
    tscError("connection disconnected");
    return NULL;
  }

  SSub* pSub = tscCreateSubscription(pObj, topic, sql);
  if (pSub == NULL) {
    return NULL;
  }
  pSub->taos = taos;

  if (restart) {
    tscTrace("restart subscription: %s", topic);
  } else {
    tscLoadSubscriptionProgress(pSub);
  }

  if (!tscUpdateSubscription(pObj, pSub)) {
    taos_unsubscribe(pSub, 1);
    return NULL;
  }

  pSub->interval = interval;
  if (fp != NULL) {
    tscTrace("asynchronize subscription, create new timer", topic);
    pSub->fp = fp;
    pSub->param = param;
    taosTmrReset(tscProcessSubscriptionTimer, interval, pSub, tscTmr, &pSub->pTimer);
  }

  return pSub;
}

void taos_free_result_imp(SSqlObj* pSql, int keepCmd);

TAOS_RES *taos_consume(TAOS_SUB *tsub) {
  SSub *pSub = (SSub *)tsub;
  if (pSub == NULL) return NULL;

  tscSaveSubscriptionProgress(pSub);

  SSqlObj* pSql = pSub->pSql;
  SSqlRes *pRes = &pSql->res;

  if (pSub->pTimer == NULL) {
    int64_t duration = taosGetTimestampMs() - pSub->lastConsumeTime;
    if (duration < (int64_t)(pSub->interval)) {
      tscTrace("subscription consume too frequently, blocking...");
      taosMsleep(pSub->interval - (int32_t)duration);
    }
  }

  for (int retry = 0; retry < 3; retry++) {
    tscRemoveFromSqlList(pSql);

    if (taosGetTimestampMs() - pSub->lastSyncTime > 10 * 60 * 1000) {
      tscTrace("begin meter synchronization");
      char* sqlstr = pSql->sqlstr;
      pSql->sqlstr = NULL;
      taos_free_result_imp(pSql, 0);
      pSql->sqlstr = sqlstr;
      taosClearDataCache(tscCacheHandle);
      if (!tscUpdateSubscription(pSub->taos, pSub)) return NULL;
      tscTrace("meter synchronization completed");
    } else {
      SQueryInfo* pQueryInfo = tscGetQueryInfoDetail(&pSql->cmd, 0);
      
      uint16_t type = pQueryInfo->type;
      taos_free_result_imp(pSql, 1);
      pRes->numOfRows = 1;
      pRes->numOfTotal = 0;
      pRes->qhandle = 0;
      pSql->thandle = NULL;
      pSql->cmd.command = TSDB_SQL_SELECT;
      pQueryInfo->type = type;

      tscGetMeterMetaInfo(&pSql->cmd, 0, 0)->vnodeIndex = 0;
    }

    tscDoQuery(pSql);
    if (pRes->code != TSDB_CODE_NOT_ACTIVE_TABLE) {
      break;
    }
    // meter was removed, make sync time zero, so that next retry will
    // do synchronization first
    pSub->lastSyncTime = 0;
  }

  if (pRes->code != TSDB_CODE_SUCCESS) {
    tscError("failed to query data, error code=%d", pRes->code);
    tscRemoveFromSqlList(pSql);
    return NULL;
  }

  pSub->lastConsumeTime = taosGetTimestampMs();
  return pSql;
}

void taos_unsubscribe(TAOS_SUB *tsub, int keepProgress) {
  SSub *pSub = (SSub *)tsub;
  if (pSub == NULL || pSub->signature != pSub) return;

  if (pSub->pTimer != NULL) {
    taosTmrStop(pSub->pTimer);
  }

  if (keepProgress) {
    tscSaveSubscriptionProgress(pSub);
  } else {
    char path[256];
    sprintf(path, "%s/subscribe/%s", dataDir, pSub->topic);
    remove(path);
  }

  tscFreeSqlObj(pSub->pSql);
  free(pSub->progress);
  memset(pSub, 0, sizeof(*pSub));
  free(pSub);
}
