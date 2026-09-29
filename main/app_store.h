// main/app_store.h —— NVS 持久化封装:智谱 API Key + 墙钟续存 + 配额快照。
// 只做薄 IO;解析与校验在 quota_model(纯逻辑,主机测试覆盖)。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "focus_store.h"
#include "quota_model.h"

// 初始化默认 NVS 分区;必要时擦除重试。失败时设备仍可运行(仅无持久化)。
bool app_store_init(void);

// 墙钟续存:保存"保存时刻的本地墙钟秒 + 当时的单调毫秒"。
// 无记录返回 false(clock 未设置过)。
bool app_store_load_clock(uint64_t *wall_epoch, uint64_t *saved_boot_ms);
int app_store_save_clock(uint64_t wall_epoch, uint64_t saved_boot_ms);

// 智谱 API Key(NVS 明文保存,仅存本机;与 Wi-Fi 密码同等安全级别)。
// 无记录返回 false。buf 至少 64 字节(Key 形如 32 位十六进制 + '.' + 8 位以上)。
bool app_store_load_key(char *buf, size_t len);
int app_store_save_key(const char *key);
// 是否已存 Key(配网页据此显示占位提示;每次读 NVS,调用方无需缓存)。
bool app_store_has_key(void);

// 最近一次成功拉取的配额快照(开机先展示旧值,联网后覆盖)。
// saved_wall_sec 为拉取成功时的本地墙钟秒;无记录返回 false。
bool app_store_load_quota(quota_data_t *out, uint64_t *saved_wall_sec);
int app_store_save_quota(const quota_data_t *d, uint64_t wall_sec);

// Let's farm 同步 token(30 天过期)。无记录返回 false。
bool app_store_load_sync_token(char *buf, size_t len);
int app_store_save_sync_token(const char *token);

// 番茄会话缓冲(见 focus_store.h;读写即整块序列化)。
bool app_store_load_focus_buf(focus_store_t *s);
int app_store_save_focus_buf(const focus_store_t *s);
