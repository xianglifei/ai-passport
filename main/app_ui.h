// main/app_ui.h —— GLM 额度 LVGL 界面:单屏仪表盘(5 小时窗口/周限额/状态)
// + 配网设置页。所有函数必须在 LVGL 就绪后调用:LVGL 任务内(如 lv_timer 回调)
// 或持有 bsp_lvgl_lock() 时。页面互相切换时由本模块负责删除旧屏。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "quota_model.h"

// 网络状态点(标题行右上,电量左侧):颜色语义见实现。
typedef enum {
    APP_UI_NET_OFFLINE = 0,   // 红:失败/未配置
    APP_UI_NET_CONNECTING,    // 琥珀:连接中
    APP_UI_NET_ONLINE,        // 青:在线
    APP_UI_NET_AP,            // 蓝:配网热点开启
} app_ui_net_t;

// 初始化字体(含符号 fallback)、字形覆盖自检、电量挂件与仪表盘。
void app_ui_init(void);

// 仪表盘:两张限额卡(数值先占位)+ 状态点 + 页脚。has_key=false 时改为
// "未配置 API Key" 引导页(仍是一屏,长按 OK 进配置)。
void app_ui_show_dash(bool has_key);

// 有新数据(实时或开机快照):整卡重绘数字/比例条;from_snapshot=true 时数字
// 先用灰色,首次实时拉取成功后恢复主题色。
void app_ui_dash_set_quota(const quota_data_t *d, bool from_snapshot);

// 网络状态点与页脚前缀联动;页脚正文用 set_footer。
void app_ui_dash_set_net(app_ui_net_t net);
// 页脚一行文本(由 main 组装:上次更新/下次轮询/失败原因等),state=true 红色。
void app_ui_dash_set_footer(const char *text, bool alert);

// 每秒调用(墙钟推进后):重算两张卡的"刷新时刻/倒计时"相对文案。
void app_ui_dash_tick(uint64_t wall_sec);

// 配网设置页:三行说明/状态 + 底部提示;刷新用 setup_update。
void app_ui_show_setup(const char *line1, const char *line2, const char *line3,
                       const char *hint);
void app_ui_setup_update(const char *line1, const char *line2, const char *line3);

// 电量(0..100),-1 不可用。挂在顶层图层,任何页面右上角常显。
void app_ui_set_battery(int soc);

// 页面中下部的短暂提示条(约 1.6 秒);需先进入目标页面。
void app_ui_toast(const char *text);

// 主循环每帧调用(LVGL 任务内):处理提示条过期等 UI 内部事务。
void app_ui_tick(void);
