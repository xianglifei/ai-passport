// main/focus_sync.h —— 番茄会话上报:Let's farm /sync 客户端(仅工作任务调用)。
// 服务端:POST {records:[{type:'work'|'rest',start,end}]},毫秒为真 UTC,
// (user,type,start,end) 主键 INSERT OR IGNORE 幂等——重发安全。
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "focus_store.h"

typedef enum {
    FOCUS_SYNC_OK = 0,       // HTTP 200,数据已入库
    FOCUS_SYNC_ERR_TOKEN,    // 401:token 缺失/过期,需重新注入
    FOCUS_SYNC_ERR_NET,      // DNS/TCP/TLS/超时
    FOCUS_SYNC_ERR_REJECT,   // 其他 HTTP 状态或响应异常
} focus_sync_result_t;

// 上报一批会话。recs 须为**真 UTC 毫秒**(记录点已折算);token 为 Bearer 凭证。
// 返回 OK 时调用方才可清掉这批记录。
focus_sync_result_t focus_sync_upload(const char *token,
                                      const focus_rec_t *recs, size_t n);
