// main/app_audio.h —— 完工庆祝音效:ES8311 正弦琶音,独立任务播放。
// 音频写阻塞,绝不能在按键回调或 LVGL 任务里直接调用。
#pragma once

#include <stdbool.h>

// 初始化 codec + I2S + 播放任务。失败不致命:音效被禁用,应用照常运行。
bool app_audio_init(void);

// 请求播放完工琶音(非阻塞;未初始化或队列满时静默丢弃)。
void app_audio_play_chime(void);
