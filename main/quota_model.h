// main/quota_model.h —— 智谱 GLM Coding Plan 配额:响应解析 + 派生计算。
// 纯逻辑,不依赖 ESP-IDF/LVGL,主机测试覆盖(tests/test_quota_model.c,
// cJSON 用 tests/cjson/ 下的 vendored 副本;设备端用 IDF json 组件,同源同版)。
//
// 数据来源:GET /api/monitor/usage/quota/limit(Bearer Key 放 Authorization 头)。
// 实测(2026-09):limits 数组里 unit=3 & number=5 是 5 小时滚动窗口,
// unit=6 & number=1 是每周限额;usage=总额度,currentValue=已用,
// remaining=剩余,nextResetTime=下次重置(epoch 毫秒)。
//
// 时间口径:与 app_clock 一致,"纪元"一律是本地墙钟语义(设备上 main 已把
// UTC 折算 UTC+8)。parse 后调用方须把 nextResetTime 按同一偏移折算,
// 再传给本模块的格式化函数。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    bool valid;             // 该窗口取到了可用数据(total>0)
    uint32_t total;         // usage:窗口总额度(积分)
    uint32_t used;          // currentValue:已用
    uint32_t remaining;     // remaining;接口缺失时按 total-used 兜底
    uint64_t reset_ms;      // 下次重置,本地墙钟口径(quota_fetch 已折算
                            // UTC+8;0 = 接口未给)。与 app_clock 纪元同帧。
} quota_window_t;

typedef struct {
    bool valid;             // limits 可读且至少一个窗口有效
    char level[16];         // 套餐名(如 "pro"),缺失为空串
    quota_window_t win5h;   // 5 小时窗口
    quota_window_t week;    // 每周窗口
} quota_data_t;

// 解析限额接口的 JSON 响应。out 恒被重置;limits 缺失/不可读返回 false。
// 单个窗口字段缺失不算整体失败(窗口置 invalid)。
bool quota_parse_json(const char *body, quota_data_t *out);

// 剩余比例,基点(万分比)0..10000(total=0 返回 0)。整数运算,进度条/百分比共用。
uint32_t quota_remaining_bps(const quota_window_t *w);

// 重置时刻的钟面:"HH:MM";若不在 now_sec 所在的自然日,带星期前缀 "周X HH:MM"。
void quota_fmt_reset_time(uint64_t now_sec, uint64_t reset_ms,
                          char *buf, size_t len);
