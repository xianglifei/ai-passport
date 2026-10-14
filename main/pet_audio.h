// main/pet_audio.h —— 卡卡声音系统:喵声"配音"层 + 音效层,全部代码合成。
//
// 输出 16kHz/16bit 单声道 PCM,经 bsp_audio_write 分块阻塞写,跑在专属
// 工作任务里;pet_audio_play() 只入队,可在按键回调之外任意上下文调用
// (但按键回调本身按仓库规则不得调用任何慢函数,分发在 input 任务做)。
// 同一时刻只播一个声音:新声音会顶掉队列里还没播的(游戏手感优先)。
#pragma once

#include <stdbool.h>

typedef enum {
    SND_NONE = 0,
    // —— 喵声配音层(卡卡的"台词") ——
    SND_MEOW_GREET,    // 问候喵(上扬)
    SND_MEOW_HAPPY,    // 开心喵(双声清亮)
    SND_MEOW_SWEET,    // 撒娇喵(软糯)
    SND_MEOW_HUNGRY,   // 饿了喵(拖音委屈)
    SND_MEOW_SLEEPY,   // 困倦喵(轻慢)
    SND_SQUEAK,        // 抗议短叫
    SND_TRILL,         // 开心颤音
    SND_YAWN,          // 打哈欠
    // —— 音效层 ——
    SND_MUNCH,         // 咀嚼(三段脆响)
    SND_PURR,          // 呼噜(~1.2s)
    SND_BOING,         // 跳跃弹跳
    SND_FOOTSTEP,      // 脚步(散步轻用)
    SND_BELL,          // 项圈铃铛
    SND_SPARKLE,       // 刷毛闪光
    SND_WATER,         // 倒鱼入碗
    SND_HEART,         // 爱心提示
    SND_BUBBLE,        // 气泡 pop
    SND_MENU_TICK,     // 菜单切换嗒声
    SND_SELECT,        // 选中
    SND_BIRDS,         // 鸟鸣(晒太阳/散步氛围)
    SND_WIND,          // 风声(散步氛围)
    SND_LULLABY,       // 摇篮短句(睡前)
    SND_STRETCH,       // 伸懒腰
} pet_sound_t;

// 初始化 BSP 音频 + 专属播放任务;失败不致命(游戏无声继续)。
bool pet_audio_init(void);

// 非阻塞播放:入队即返回;音频不可用或已静音时静默丢弃。
void pet_audio_play(pet_sound_t s);

// 静音开关(运行时切换;由 main 在 NVS 读出/按键切换时设置)。
void pet_audio_set_muted(bool muted);
