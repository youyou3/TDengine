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
#include "taosmsg.h"

#include "tcache.h"
#include "tscUtil.h"
#include "tsclient.h"
#include "ttypes.h"

#include "textbuffer.h"
#include "tscSecondaryMerge.h"
#include "tschemautil.h"
#include "tsocket.h"

static void tscSetLocalQueryResult(SSqlObj *pSql, const char *val, const char *columnName, size_t valueLength);

static int32_t getToStringLength(const char *pData, int32_t length, int32_t type) {
  char buf[512] = {0};

  int32_t len = 0;
  int32_t MAX_BOOL_TYPE_LENGTH = 5;  // max(strlen("true"), strlen("false"));
  switch (type) {
    case TSDB_DATA_TYPE_BINARY:
      return length;
    case TSDB_DATA_TYPE_NCHAR:
      return length;
    case TSDB_DATA_TYPE_DOUBLE: {
      double dv = 0;
      dv = GET_DOUBLE_VAL(pData);
      len = sprintf(buf, "%lf", dv);
      if (strncasecmp("nan", buf, 3) == 0) {
        len = 4;
      }
    } break;
    case TSDB_DATA_TYPE_FLOAT: {
      float fv = 0;
      fv = GET_FLOAT_VAL(pData);
      len = sprintf(buf, "%f", fv);
      if (strncasecmp("nan", buf, 3) == 0) {
        len = 4;
      }
    } break;
    case TSDB_DATA_TYPE_TIMESTAMP:
    case TSDB_DATA_TYPE_BIGINT:
      len = sprintf(buf, "%" PRId64 "", *(int64_t *)pData);
      break;
    case TSDB_DATA_TYPE_BOOL:
      len = MAX_BOOL_TYPE_LENGTH;
      break;
    default:
      len = sprintf(buf, "%d", *(int32_t *)pData);
      break;
  };
  return len;
}

/*
 * we need to convert all data into string, so we need to sprintf all kinds of
 * non-string data into string, and record its length to get the right
 * maximum length. The length may be less or greater than its original binary length:
 * For example:
 * length((short) 1) == 1, less than sizeof(short)
 * length((uint64_t) 123456789011) > 12, greater than sizsof(uint64_t)
 */
static int32_t tscMaxLengthOfTagsFields(SSqlObj *pSql) {
  SMeterMeta *pMeta = tscGetMeterMetaInfo(&pSql->cmd, 0, 0)->pMeterMeta;

  if (pMeta->meterType == TSDB_METER_METRIC || pMeta->meterType == TSDB_METER_OTABLE ||
      pMeta->meterType == TSDB_METER_STABLE) {
    return 0;
  }

  char *   pTagValue = tsGetTagsValue(pMeta);
  SSchema *pTagsSchema = tsGetTagSchema(pMeta);

  int32_t len = getToStringLength(pTagValue, pTagsSchema[0].bytes, pTagsSchema[0].type);

  pTagValue += pTagsSchema[0].bytes;
  for (int32_t i = 1; i < pMeta->numOfTags; ++i) {
    int32_t tLen = getToStringLength(pTagValue, pTagsSchema[i].bytes, pTagsSchema[i].type);
    if (len < tLen) {
      len = tLen;
    }

    pTagValue += pTagsSchema[i].bytes;
  }

  return len;
}

static int32_t tscSetValueToResObj(SSqlObj *pSql, int32_t rowLen) {
  SSqlRes *pRes = &pSql->res;

  // one column for each row
  SQueryInfo* pQueryInfo = tscGetQueryInfoDetail(&pSql->cmd, 0);
  
  SMeterMetaInfo *pMeterMetaInfo = tscGetMeterMetaInfoFromQueryInfo(pQueryInfo, 0);
  SMeterMeta *    pMeta = pMeterMetaInfo->pMeterMeta;

  /*
   * tagValueCnt is to denote the number of tags columns for meter, not metric. and is to show the column data.
   * for meter, which is created according to metric, the value of tagValueCnt is not 0, and the numOfTags must be 0.
   * for metric, the value of tagValueCnt must be 0, but the numOfTags is not 0
   */

  int32_t numOfRows = pMeta->numOfColumns;
  int32_t totalNumOfRows = numOfRows + pMeta->numOfTags;

  if (UTIL_METER_IS_SUPERTABLE(pMeterMetaInfo)) {
    numOfRows = pMeta->numOfColumns + pMeta->numOfTags;
  }

  tscInitResObjForLocalQuery(pSql, totalNumOfRows, rowLen);
  SSchema *pSchema = tsGetSchema(pMeta);

  for (int32_t i = 0; i < numOfRows; ++i) {
    TAOS_FIELD *pField = tscFieldInfoGetField(pQueryInfo, 0);
    strncpy(pRes->data + tscFieldInfoGetOffset(pQueryInfo, 0) * totalNumOfRows + pField->bytes * i, pSchema[i].name,
            TSDB_COL_NAME_LEN);

    char *type = tDataTypeDesc[pSchema[i].type].aName;

    pField = tscFieldInfoGetField(pQueryInfo, 1);
    strncpy(pRes->data + tscFieldInfoGetOffset(pQueryInfo, 1) * totalNumOfRows + pField->bytes * i, type, pField->bytes);

    int32_t bytes = pSchema[i].bytes;
    if (pSchema[i].type == TSDB_DATA_TYPE_NCHAR) {
      bytes = bytes / TSDB_NCHAR_SIZE;
    }

    pField = tscFieldInfoGetField(pQueryInfo, 2);
    *(int32_t *)(pRes->data + tscFieldInfoGetOffset(pQueryInfo, 2) * totalNumOfRows + pField->bytes * i) = bytes;

    pField = tscFieldInfoGetField(pQueryInfo, 3);
    if (i >= pMeta->numOfColumns && pMeta->numOfTags != 0) {
      strncpy(pRes->data + tscFieldInfoGetOffset(pQueryInfo, 3) * totalNumOfRows + pField->bytes * i, "tag",
              strlen("tag") + 1);
    }
  }

  if (UTIL_METER_IS_SUPERTABLE(pMeterMetaInfo)) {
    return 0;
  }

  // the following is handle display tags value for meters created according to metric
  char *pTagValue = tsGetTagsValue(pMeta);
  for (int32_t i = numOfRows; i < totalNumOfRows; ++i) {
    // field name
    TAOS_FIELD *pField = tscFieldInfoGetField(pQueryInfo, 0);
    strncpy(pRes->data + tscFieldInfoGetOffset(pQueryInfo, 0) * totalNumOfRows + pField->bytes * i, pSchema[i].name,
            TSDB_COL_NAME_LEN);

    // type name
    pField = tscFieldInfoGetField(pQueryInfo, 1);
    char *type = tDataTypeDesc[pSchema[i].type].aName;
    strncpy(pRes->data + tscFieldInfoGetOffset(pQueryInfo, 1) * totalNumOfRows + pField->bytes * i, type, pField->bytes);

    // type length
    int32_t bytes = pSchema[i].bytes;
    pField = tscFieldInfoGetField(pQueryInfo, 2);
    if (pSchema[i].type == TSDB_DATA_TYPE_NCHAR) {
      bytes = bytes / TSDB_NCHAR_SIZE;
    }

    *(int32_t *)(pRes->data + tscFieldInfoGetOffset(pQueryInfo, 2) * totalNumOfRows + pField->bytes * i) = bytes;

    // tag value
    pField = tscFieldInfoGetField(pQueryInfo, 3);
    char *target = pRes->data + tscFieldInfoGetOffset(pQueryInfo, 3) * totalNumOfRows + pField->bytes * i;

    if (isNull(pTagValue, pSchema[i].type)) {
      sprintf(target, "%s", TSDB_DATA_NULL_STR);
    } else {
      switch (pSchema[i].type) {
        case TSDB_DATA_TYPE_BINARY:
          /* binary are not null-terminated string */
          strncpy(target, pTagValue, pSchema[i].bytes);
          break;
        case TSDB_DATA_TYPE_NCHAR:
          taosUcs4ToMbs(pTagValue, pSchema[i].bytes, target);
          break;
        case TSDB_DATA_TYPE_FLOAT: {
          float fv = 0;
          fv = GET_FLOAT_VAL(pTagValue);
          sprintf(target, "%f", fv);
        } break;
        case TSDB_DATA_TYPE_DOUBLE: {
          double dv = 0;
          dv = GET_DOUBLE_VAL(pTagValue);
          sprintf(target, "%lf", dv);
        } break;
        case TSDB_DATA_TYPE_TINYINT:
          sprintf(target, "%d", *(int8_t *)pTagValue);
          break;
        case TSDB_DATA_TYPE_SMALLINT:
          sprintf(target, "%d", *(int16_t *)pTagValue);
          break;
        case TSDB_DATA_TYPE_INT:
          sprintf(target, "%d", *(int32_t *)pTagValue);
          break;
        case TSDB_DATA_TYPE_BIGINT:
          sprintf(target, "%" PRId64 "", *(int64_t *)pTagValue);
          break;
        case TSDB_DATA_TYPE_BOOL: {
          char *val = (*((int8_t *)pTagValue) == 0) ? "false" : "true";
          sprintf(target, "%s", val);
          break;
        }
        default:
          break;
      }
    }

    pTagValue += pSchema[i].bytes;
  }

  return 0;
}

static int32_t tscBuildMeterSchemaResultFields(SSqlObj *pSql, int32_t numOfCols, int32_t typeColLength,
                                               int32_t noteColLength) {
  int32_t  rowLen = 0;
  SSqlCmd *pCmd = &pSql->cmd;
  pCmd->numOfCols = numOfCols;

  SQueryInfo* pQueryInfo = tscGetQueryInfoDetail(pCmd, 0);
  pQueryInfo->order.order = TSQL_SO_ASC;

  tscFieldInfoSetValue(&pQueryInfo->fieldsInfo, 0, TSDB_DATA_TYPE_BINARY, "Field", TSDB_COL_NAME_LEN);
  rowLen += TSDB_COL_NAME_LEN;

  tscFieldInfoSetValue(&pQueryInfo->fieldsInfo, 1, TSDB_DATA_TYPE_BINARY, "Type", typeColLength);
  rowLen += typeColLength;

  tscFieldInfoSetValue(&pQueryInfo->fieldsInfo, 2, TSDB_DATA_TYPE_INT, "Length", sizeof(int32_t));
  rowLen += sizeof(int32_t);

  tscFieldInfoSetValue(&pQueryInfo->fieldsInfo, 3, TSDB_DATA_TYPE_BINARY, "Note", noteColLength);
  rowLen += noteColLength;
  
  //set the sqlexpr part
  SColumnIndex index = {0};
  pQueryInfo->fieldsInfo.pSqlExpr[0] = tscSqlExprInsert(pQueryInfo, 0, TSDB_FUNC_TS_DUMMY, &index, TSDB_DATA_TYPE_BINARY, TSDB_COL_NAME_LEN, TSDB_COL_NAME_LEN);
  pQueryInfo->fieldsInfo.pSqlExpr[1] = tscSqlExprInsert(pQueryInfo, 1, TSDB_FUNC_TS_DUMMY, &index, TSDB_DATA_TYPE_BINARY, typeColLength, typeColLength);
  pQueryInfo->fieldsInfo.pSqlExpr[2] = tscSqlExprInsert(pQueryInfo, 2, TSDB_FUNC_TS_DUMMY, &index, TSDB_DATA_TYPE_INT, sizeof(int32_t), sizeof(int32_t));
  pQueryInfo->fieldsInfo.pSqlExpr[3] = tscSqlExprInsert(pQueryInfo, 3, TSDB_FUNC_TS_DUMMY, &index, TSDB_DATA_TYPE_BINARY, noteColLength, noteColLength);

  return rowLen;
}

static int32_t tscProcessDescribeTable(SSqlObj *pSql) {
  SQueryInfo* pQueryInfo = tscGetQueryInfoDetail(&pSql->cmd, 0);
  
  assert(tscGetMeterMetaInfoFromQueryInfo(pQueryInfo, 0)->pMeterMeta != NULL);

  const int32_t NUM_OF_DESCRIBE_TABLE_COLUMNS = 4;
  const int32_t TYPE_COLUMN_LENGTH = 16;
  const int32_t NOTE_COLUMN_MIN_LENGTH = 8;

  int32_t note_field_length = tscMaxLengthOfTagsFields(pSql);
  if (note_field_length == 0) {
    note_field_length = NOTE_COLUMN_MIN_LENGTH;
  }

  int32_t rowLen =
      tscBuildMeterSchemaResultFields(pSql, NUM_OF_DESCRIBE_TABLE_COLUMNS, TYPE_COLUMN_LENGTH, note_field_length);
  tscFieldInfoCalOffset(pQueryInfo);
  return tscSetValueToResObj(pSql, rowLen);
}

// todo add order support
static int tscBuildMetricTagProjectionResult(SSqlObj *pSql) {
  // the result structure has been completed in sql parse, so we
  // only need to reorganize the results in the column format
  SSqlCmd *       pCmd = &pSql->cmd;
  SSqlRes *       pRes = &pSql->res;
  SQueryInfo *pQueryInfo = tscGetQueryInfoDetail(pCmd, 0);
  
  SMeterMetaInfo *pMeterMetaInfo = tscGetMeterMetaInfoFromQueryInfo(pQueryInfo, 0);

  SMetricMeta *pMetricMeta = pMeterMetaInfo->pMetricMeta;
  SSchema *    pSchema = tsGetTagSchema(pMeterMetaInfo->pMeterMeta);

  int32_t vOffset[TSDB_MAX_COLUMNS] = {0};

  for (int32_t f = 1; f < pMeterMetaInfo->numOfTags; ++f) {
    int16_t tagColumnIndex = pMeterMetaInfo->tagColumnIndex[f - 1];
    if (tagColumnIndex == -1) {
      vOffset[f] = vOffset[f - 1] + TSDB_METER_NAME_LEN;
    } else {
      vOffset[f] = vOffset[f - 1] + pSchema[tagColumnIndex].bytes;
    }
  }

  int32_t totalNumOfResults = pMetricMeta->numOfMeters;
  int32_t rowLen = tscGetResRowLength(pQueryInfo);

  tscInitResObjForLocalQuery(pSql, totalNumOfResults, rowLen);

  int32_t rowIdx = 0;
  for (int32_t i = 0; i < pMetricMeta->numOfVnodes; ++i) {
    SVnodeSidList *pSidList = (SVnodeSidList *)((char *)pMetricMeta + pMetricMeta->list[i]);

    for (int32_t j = 0; j < pSidList->numOfSids; ++j) {
      SMeterSidExtInfo *pSidExt = tscGetMeterSidInfo(pSidList, j);
      
      for (int32_t k = 0; k < pQueryInfo->fieldsInfo.numOfOutputCols; ++k) {
        SColIndexEx *pColIndex = &tscSqlExprGet(pQueryInfo, k)->colInfo;
        int16_t      offsetId = pColIndex->colIdx;

        assert((pColIndex->flag & TSDB_COL_TAG) != 0);

        char *      val = pSidExt->tags + vOffset[offsetId];
        TAOS_FIELD *pField = tscFieldInfoGetField(pQueryInfo, k);

        memcpy(pRes->data + tscFieldInfoGetOffset(pQueryInfo, k) * totalNumOfResults + pField->bytes * rowIdx, val,
               (size_t)pField->bytes);
      }
      rowIdx++;
    }
  }

  return 0;
}

static int tscBuildMetricTagSqlFunctionResult(SSqlObj *pSql) {
  SSqlCmd *pCmd = &pSql->cmd;
  SSqlRes *pRes = &pSql->res;

  SQueryInfo* pQueryInfo = tscGetQueryInfoDetail(pCmd, 0);
  
  SMetricMeta *pMetricMeta = tscGetMeterMetaInfoFromQueryInfo(pQueryInfo, 0)->pMetricMeta;
  int32_t      totalNumOfResults = 1;  // count function only produce one result
  int32_t      rowLen = tscGetResRowLength(pQueryInfo);

  tscInitResObjForLocalQuery(pSql, totalNumOfResults, rowLen);

  int32_t rowIdx = 0;
  for (int32_t i = 0; i < totalNumOfResults; ++i) {
    for (int32_t k = 0; k < pQueryInfo->fieldsInfo.numOfOutputCols; ++k) {
      SSqlExpr *pExpr = tscSqlExprGet(pQueryInfo, i);

      if (pExpr->colInfo.colIdx == -1 && pExpr->functionId == TSDB_FUNC_COUNT) {
        TAOS_FIELD *pField = tscFieldInfoGetField(pQueryInfo, k);

        memcpy(pRes->data + tscFieldInfoGetOffset(pQueryInfo, i) * totalNumOfResults + pField->bytes * rowIdx,
               &pMetricMeta->numOfMeters, sizeof(pMetricMeta->numOfMeters));
      } else {
        tscError("not support operations");
        continue;
      }
    }
    rowIdx++;
  }

  return 0;
}

static int tscProcessQueryTags(SSqlObj *pSql) {
  SSqlCmd *pCmd = &pSql->cmd;
  
  SQueryInfo* pQueryInfo = tscGetQueryInfoDetail(pCmd, 0);
  
  SMeterMeta *pMeterMeta = tscGetMeterMetaInfoFromQueryInfo(pQueryInfo, 0)->pMeterMeta;
  if (pMeterMeta == NULL || pMeterMeta->numOfTags == 0 || pMeterMeta->numOfColumns == 0) {
    strcpy(pCmd->payload, "invalid table");
    pSql->res.code = TSDB_CODE_INVALID_TABLE;
    return pSql->res.code;
  }

  SSqlExpr *pExpr = tscSqlExprGet(pQueryInfo, 0);
  if (pExpr->functionId == TSDB_FUNC_COUNT) {
    return tscBuildMetricTagSqlFunctionResult(pSql);
  } else {
    return tscBuildMetricTagProjectionResult(pSql);
  }
}

static void tscProcessCurrentUser(SSqlObj *pSql) {
  SQueryInfo* pQueryInfo = tscGetQueryInfoDetail(&pSql->cmd, 0);
  
  SSqlExpr* pExpr = tscSqlExprGet(pQueryInfo, 0);
  tscSetLocalQueryResult(pSql, pSql->pTscObj->user, pExpr->aliasName, TSDB_USER_LEN);
}

static void tscProcessCurrentDB(SSqlObj *pSql) {
  char db[TSDB_DB_NAME_LEN + 1] = {0};
  extractDBName(pSql->pTscObj->db, db);
  
  // no use db is invoked before.
  if (strlen(db) == 0) {
    setNull(db, TSDB_DATA_TYPE_BINARY, TSDB_DB_NAME_LEN);
  }
  
  SQueryInfo* pQueryInfo = tscGetQueryInfoDetail(&pSql->cmd, 0);
  
  SSqlExpr* pExpr = tscSqlExprGet(pQueryInfo, 0);
  tscSetLocalQueryResult(pSql, db, pExpr->aliasName, TSDB_DB_NAME_LEN);
}

static void tscProcessServerVer(SSqlObj *pSql) {
  const char* v = pSql->pTscObj->sversion;
  SQueryInfo* pQueryInfo = tscGetQueryInfoDetail(&pSql->cmd, 0);
  
  SSqlExpr* pExpr = tscSqlExprGet(pQueryInfo, 0);
  tscSetLocalQueryResult(pSql, v, pExpr->aliasName, tListLen(pSql->pTscObj->sversion));
}

static void tscProcessClientVer(SSqlObj *pSql) {
  SQueryInfo* pQueryInfo = tscGetQueryInfoDetail(&pSql->cmd, 0);
  
  SSqlExpr* pExpr = tscSqlExprGet(pQueryInfo, 0);
  tscSetLocalQueryResult(pSql, version, pExpr->aliasName, strlen(version));
}

static void tscProcessServStatus(SSqlObj *pSql) {
  STscObj* pObj = pSql->pTscObj;
  
  if (pObj->pHb != NULL) {
    if (pObj->pHb->res.code == TSDB_CODE_NETWORK_UNAVAIL) {
      pSql->res.code = TSDB_CODE_NETWORK_UNAVAIL;
      return;
    } else {
      int32_t* data = (int32_t*) pObj->pHb->res.data;
      if (data != NULL) {
        int32_t totalDnode = data[0];
        int32_t onlineDnode = data[1];
        assert(onlineDnode <= totalDnode);
  
        if (onlineDnode < totalDnode) {
          pSql->res.code = TSDB_CODE_NETWORK_UNAVAIL;
          return;
        }
      }
      
    }
  } else {
    if (pSql->res.code == TSDB_CODE_NETWORK_UNAVAIL) {
      return;
    }
  }
  
  SQueryInfo* pQueryInfo = tscGetQueryInfoDetail(&pSql->cmd, 0);
  
  SSqlExpr* pExpr = tscSqlExprGet(pQueryInfo, 0);
  tscSetLocalQueryResult(pSql, "1", pExpr->aliasName, 2);
}

void tscSetLocalQueryResult(SSqlObj *pSql, const char *val, const char *columnName, size_t valueLength) {
  SSqlCmd *pCmd = &pSql->cmd;
  SSqlRes *pRes = &pSql->res;

  pCmd->numOfCols = 1;
  
  SQueryInfo* pQueryInfo = tscGetQueryInfoDetail(pCmd, pCmd->clauseIndex);
  pQueryInfo->order.order = TSQL_SO_ASC;
  
  tscClearFieldInfo(&pQueryInfo->fieldsInfo);
  
  tscFieldInfoSetValue(&pQueryInfo->fieldsInfo, 0, TSDB_DATA_TYPE_BINARY, columnName, valueLength);
  tscInitResObjForLocalQuery(pSql, 1, valueLength);

  TAOS_FIELD *pField = tscFieldInfoGetField(pQueryInfo, 0);
  pQueryInfo->fieldsInfo.pSqlExpr[0] = pQueryInfo->exprsInfo.pExprs[0];
  
  strncpy(pRes->data, val, pField->bytes);
}

int tscProcessLocalCmd(SSqlObj *pSql) {
  SSqlCmd *pCmd = &pSql->cmd;

  if (pCmd->command == TSDB_SQL_CFG_LOCAL) {
    pSql->res.code = (uint8_t)tsCfgDynamicOptions(pCmd->payload);
  } else if (pCmd->command == TSDB_SQL_DESCRIBE_TABLE) {
    pSql->res.code = (uint8_t)tscProcessDescribeTable(pSql);
  } else if (pCmd->command == TSDB_SQL_RETRIEVE_TAGS) {
    pSql->res.code = (uint8_t)tscProcessQueryTags(pSql);
  } else if (pCmd->command == TSDB_SQL_RETRIEVE_EMPTY_RESULT) {
    /*
     * set the qhandle to be 1 in order to pass the qhandle check, and to call partial release function to
     * free allocated resources and remove the SqlObj from sql query linked list
     */
    pSql->res.qhandle = 0x1;
    pSql->res.numOfRows = 0;
  } else if (pCmd->command == TSDB_SQL_RESET_CACHE) {
    taosClearDataCache(tscCacheHandle);
  } else if (pCmd->command == TSDB_SQL_SERV_VERSION) {
    tscProcessServerVer(pSql);
  } else if (pCmd->command == TSDB_SQL_CLI_VERSION) {
    tscProcessClientVer(pSql);
  } else if (pCmd->command == TSDB_SQL_CURRENT_USER) {
    tscProcessCurrentUser(pSql);
  } else if (pCmd->command == TSDB_SQL_CURRENT_DB) {
    tscProcessCurrentDB(pSql);
  } else if (pCmd->command == TSDB_SQL_SERV_STATUS) {
    tscProcessServStatus(pSql);
  } else {
    pSql->res.code = TSDB_CODE_INVALID_SQL;
    tscError("%p not support command:%d", pSql, pCmd->command);
  }

  // keep the code in local variable in order to avoid invalid read in case of async query
  int32_t code = pSql->res.code;

  if (pSql->fp != NULL) {  // callback function
    if (code == 0) {
      (*pSql->fp)(pSql->param, pSql, 0);
    } else {
      tscQueueAsyncRes(pSql);
    }
  }

  return code;
}
