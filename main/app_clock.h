// main/app_clock.h —— 本地墙钟换算:开机纪元 + 单调毫秒 → 天索引/分钟/日期。
// 纯逻辑,主机测试覆盖(tests/test_focus_model.c 的 clock 部分)。
//
// 设备没有 RTC 后备电池,也没有联网对时:app 层把"用户手动设置的本地时间"
// 折算成 boot_epoch(NVS 周期续存),运行期用单调时钟推进。所有"纪元"都是
// 本地时区语义,不做时区换算。
#pragma once

#include <stdint.h>

// 本地墙钟秒 = boot_epoch + (now_boot_ms / 1000)。
uint64_t app_clock_wall_sec(uint64_t boot_epoch, uint64_t now_boot_ms);

// 自 1970-01-01(本地)的天数。
uint16_t app_clock_day_index(uint64_t wall_sec);

// 当天第几分钟(0..1439)。
uint32_t app_clock_minute_of_day(uint64_t wall_sec);

// 天索引 → 公历日期(民用历,含 1900/2100 等世纪规则)。
void app_clock_civil(uint16_t day_index, int *year, int *month, int *day);

// 公历日期 → 天索引(civil 的逆运算)。
uint16_t app_clock_days_from_civil(int year, int month, int day);

// 星期,0=周一 … 6=周日(1970-01-01 是周四)。
int app_clock_weekday_mon0(uint16_t day_index);

// 由用户设置的时/分构造"当天该时刻"的本地墙钟秒(供时间设置页保存)。
// hour/min 越界会被夹取到合法范围。
uint64_t app_clock_wall_from_hm(uint16_t day_index, uint32_t hour, uint32_t minute);
