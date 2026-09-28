// main/heatmap_store.h —— 24 小时热力图数据模型:每天 288 个 5 分钟格子,
// 每格 2 bit(bit0=工作,bit1=休息),可同时置位;保留最近 HM_DAYS 天。
// 纯逻辑 + 显式序列化(NVS blob 由 app_store 落盘),主机测试覆盖
// tests/test_heatmap_store.c。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HM_DAYS 30                     // 保留天数(环形,挤掉最旧)
#define HM_CELLS_PER_DAY 288           // 24h × 每小时 12 格(5 分钟)
#define HM_MINUTES_PER_BUCKET 5
#define HM_CELL_BYTES (HM_CELLS_PER_DAY / 4)  // 72 字节/天
#define HM_KIND_WORK 0x1
#define HM_KIND_REST 0x2
#define HM_DAY_NONE 0xFFFF             // 空槽标记
#define HM_BLOB_VERSION 1

typedef struct {
    uint16_t day_index;                // 自 1970-01-01(本地时区)的天数
    uint16_t work_sessions;            // 当天完成的工作会话数(仪式达成才 +1)
    uint16_t rest_sessions;
    uint8_t cells[HM_CELL_BYTES];
} hm_day_t;

typedef struct {
    uint8_t version;
    uint16_t capacity;
    hm_day_t days[HM_DAYS];
} hm_store_t;

// 序列化后的 blob 字节数(固定,含 4 字节头)。
#define HM_BLOB_SIZE (4U + (uint32_t)HM_DAYS * (2U + 2U + 2U + HM_CELL_BYTES))

void hm_init(hm_store_t *store);

// 无该天记录返回 NULL。
hm_day_t *hm_day_find(hm_store_t *store, uint16_t day_index);
// 取当天记录槽;不存在则复用"最旧/空闲"槽。永远返回非 NULL。
hm_day_t *hm_day_acquire(hm_store_t *store, uint16_t day_index);

// 给 bucket(0..287)或按分钟区间 [start_min, end_min) 染色;越界自动截断。
void hm_mark_bucket(hm_day_t *day, uint16_t bucket, uint8_t kind_mask);
void hm_mark_span(hm_day_t *day, uint16_t start_min, uint16_t end_min,
                  uint8_t kind_mask);
bool hm_cell_has(const hm_day_t *day, uint16_t bucket, uint8_t kind_mask);
// 统计含 kind_mask 任一位的格子数(乘 5 即分钟数)。
int hm_count_cells(const hm_day_t *day, uint8_t kind_mask);

// 显式小端序列化;buf 不足返回 false。
bool hm_serialize(const hm_store_t *store, uint8_t *buf, size_t len);
// 校验版本/长度;失败时把 store 恢复为空并返回 false。
bool hm_deserialize(hm_store_t *store, const uint8_t *buf, size_t len);
