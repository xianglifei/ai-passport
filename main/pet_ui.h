// main/pet_ui.h —— 卡卡应用的 LVGL 场景层。
//
// 线程约定:本模块所有函数都必须在持有 bsp_lvgl_lock() 的上下文调用
// (app_tick 运行在 LVGL 任务内,天然持锁;input 任务需自行加锁)。
// 场景自建于本文件,不复用基线 demo 的 ui_pixel 外壳(仓库强制 UI 重设计规则)。
#pragma once

#include <stdint.h>

#include "lvgl.h"
#include "pet_model.h"

#define PET_DOCK_COUNT 4  // 喂食/刷毛/晒太阳/散步

// 应用初始化时调用一次:仅初始化内部状态。
void pet_ui_init(void);

// 显示主界面(会整体重建屏幕)。
void pet_ui_show_home(const pet_model_t *m);

// dock 选中项 0..3(喂食/刷毛/晒太阳/散步);越界值会被夹紧。
void pet_ui_select(int index);
int pet_ui_selected(void);

// 电池电量百分比 0..100;-1 = 读取失败(优雅降级为不显示)。
void pet_ui_set_battery(int soc);

// 周期刷新:idle 帧动画、需求气泡、心情演出。now_ms 为单调毫秒。
void pet_ui_tick(const pet_model_t *m, uint32_t now_ms);

// 屏幕注册表:主界面与活动场景(pet_scenes)共用;安装新屏会删除旧屏并
// 清空主界面控件指针,然后 lv_screen_load。
void pet_ui_install_screen(lv_obj_t *scr);

// 活动结束回到主界面后的庆祝演出:开心帧 + 爱心雨约 2s。
// why=PET_INTERACT_REFUSED 时只给一个害羞爱心(卡卡很有礼貌)。
void pet_ui_celebrate(pet_interact_t why, uint32_t now_ms);

// 顶部胶囊提示(静音开关等),约 1.6s 自动消失。text 为中文文案。
void pet_ui_toast(const char *text);

// 夜晚屏(睡前仪式/自动休眠):月亮星星 + 睡着的卡卡 + 晚安文案。
void pet_ui_show_night(void);

// —— 共享粒子(主界面庆祝与活动场景共用,父对象为当前屏幕) ——
// 爱心:从 (x,y) 上升消散(图像精灵)。
void pet_ui_spawn_heart(int x, int y);
// 方块粒子:6x6 小方块,up=1 向上飘,否则向下落;颜色自定(碎屑/毛絮)。
void pet_ui_spawn_chunk(int x, int y, lv_color_t color, bool up);
// 80ms 心跳推进粒子;屏幕被 install_screen 重建时粒子表自动清空。
void pet_ui_particles_tick(void);
