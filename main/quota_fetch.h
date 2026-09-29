// main/quota_fetch.h —— 智谱套餐限额拉取(HTTPS)。
// 阻塞式网络 IO,只允许在工作任务里调用(见 main.c 的 worker 约定)。
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "quota_model.h"

// 拉取一次限额并解析进 *out。key 原样放入 Authorization 头(实测不带 Bearer)。
// 成功返回 true;失败返回 false 并把简短原因写进 errbuf(不含 Key 本身)。
// 每次调用内部完整走 TLS 握手 → 请求 → 读响应 → 释放,无跨调用状态。
bool quota_fetch_now(const char *key, quota_data_t *out,
                     char *errbuf, size_t errlen);
