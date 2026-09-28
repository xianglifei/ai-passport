// main/app_store.h —— NVS 持久化封装:热力图 blob + 墙钟续存。
// 只做薄 IO;数据结构与校验在 heatmap_store(纯逻辑,主机测试覆盖)。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "heatmap_store.h"

// 初始化默认 NVS 分区;必要时擦除重试。失败时设备仍可运行(仅无持久化)。
bool app_store_init(void);

// 读热力图。无历史/数据无效时把 store 置为空表并返回 false(不算错误)。
bool app_store_load_heatmap(hm_store_t *store);
// 写入已序列化的热力图 blob(调用方先用 hm_serialize 生成)。
int app_store_save_heatmap(const uint8_t *blob, size_t len);

// 墙钟续存:保存"保存时刻的本地墙钟秒 + 当时的单调毫秒"。
// 无记录返回 false(clock 未设置过)。
bool app_store_load_clock(uint64_t *wall_epoch, uint64_t *saved_boot_ms);
int app_store_save_clock(uint64_t wall_epoch, uint64_t saved_boot_ms);
