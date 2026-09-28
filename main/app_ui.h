// main/app_ui.h —— 番茄钟 LVGL 界面:首页/倒计时/完工仪式/热力图/时间设置/Wi-Fi 对时。
// 所有函数必须在 LVGL 就绪后调用:LVGL 任务内(如 lv_timer 回调)或
// 持有 bsp_lvgl_lock() 时。页面互相切换时由本模块负责删除旧屏。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "heatmap_store.h"

#define APP_UI_HOME_WORK 0
#define APP_UI_HOME_REST 1
#define APP_UI_HOME_HEATMAP 2

// 初始化字体(含符号 fallback)、字形覆盖自检、电量挂件与首页。
void app_ui_init(void);

// 首页三张卡的选择态;sel 取 APP_UI_HOME_*。
void app_ui_show_home(uint32_t sel);
void app_ui_home_select(uint32_t sel);

// 倒计时页;work=true 工作(番茄色),false 休息(青色)。
// has_clock=false 时不显示"完成于"行(时钟未设置)。
void app_ui_show_countdown(bool work, uint32_t total_sec, bool has_clock);
// 每 250ms 调用:刷新剩余时间/进度环;finish_min 为"完成于 HH:MM"的分钟数,
// 传 -1 表示不显示。
void app_ui_countdown_update(uint32_t remain_sec, uint32_t total_sec,
                             int32_t finish_min);

// 完工仪式页;tomato_count 为今日已完成的工作次数(>=1)。
void app_ui_show_celebrate(uint16_t tomato_count);

// 热力图页(12 列 × 24 行,每天 288 个 5 分钟格)。
// day 可为 NULL(该天无记录);is_today 时 today_bucket(0..287)描白框,
// 非今天传 0xFFFF。clock_unset=true 时显示"请先设置时间"引导页。
void app_ui_show_heatmap(uint16_t day_index, const hm_day_t *day,
                         bool is_today, uint16_t today_bucket, bool clock_unset);

// 时间设置页;forced=true 表示时钟从未设置,不允许取消。
void app_ui_show_timeset(uint32_t minute_of_day, bool forced);
void app_ui_timeset_update(uint32_t minute_of_day);

// Wi-Fi 对时页:三行说明/状态 + 底部提示;刷新用 wifi_update。
void app_ui_show_wifi(const char *line1, const char *line2, const char *line3,
                      const char *hint);
void app_ui_wifi_update(const char *line1, const char *line2, const char *line3);

// 电量(0..100),-1 不可用。挂在顶层图层,任何页面右上角常显。
void app_ui_set_battery(int soc);

// 页面中下部的短暂提示条(约 1.6 秒);需先进入目标页面。
void app_ui_toast(const char *text);

// 主循环每帧调用(LVGL 任务内):处理提示条过期等 UI 内部事务。
void app_ui_tick(void);
