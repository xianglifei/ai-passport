// main/pet_scenes.h —— 卡卡的四个互动场景(喂食/刷毛/晒太阳/散步)。
//
// 线程约定:所有函数在持有 bsp_lvgl_lock() 的上下文调用(app_tick /
// scene_anim 均在 LVGL 任务内,input 任务自行加锁)。屏幕经
// pet_ui_install_screen 注册,粒子复用 pet_ui 的共享系统。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "pet_model.h"

// 进入活动场景(整屏重建)。battery_soc 供右上角电量显示。
void pet_scenes_show(pet_activity_t a, const pet_model_t *m, int battery_soc);

// 当前是否有活动场景在展示。
bool pet_scenes_active(void);

// 一次交互的演出(吃一口/刷一下/扑跳/唤醒);r 为模型返回的结果,
// now_ms 为单调毫秒(用于演出计时)。
void pet_scenes_interact(pet_interact_t r, uint32_t now_ms);

// 80ms 动画心跳:帧循环、粒子伴生位、视差滚动、扑跳姿态。
void pet_scenes_anim(const pet_model_t *m, uint32_t now_ms);
