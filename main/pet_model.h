// main/pet_model.h —— 卡卡养成核心模型(纯逻辑,可脱离 ESP-IDF/LVGL 做 host 测试)。
//
// 设计要点(与项目计划一致):
//   - 三状态 0..100,下限 PET_STAT_MIN(卡卡永远不会"惨",最低只是想你);
//   - 衰减以毫秒累积,注入 now_ms,模型内自己算 delta(要求调用者 now 单调);
//   - 活动状态机:idle → 四种活动,活动中每次"确定"= 一次交互,晒太阳/散步到时自动结束;
//   - 收益递减:>85 后收益减半;饱食 >=95 时礼貌拒喂;
//   - 空闲喵叫:20s 后每秒掷一次确定性 xorshift,命中则"喵"并重置空闲计时。
#pragma once

#include <stdint.h>
#include <stdbool.h>

#define PET_STAT_MIN 15     // 三状态公共下限
#define PET_STAT_WANT 40    // 低于此值出现需求气泡
#define PET_STAT_MAX 100

// 衰减速率:每扣 1 点所需的毫秒数
#define PET_DECAY_FULL_MS (12u * 60u * 1000u)   // 饱食:12 分钟/点
#define PET_DECAY_CLEAN_MS (20u * 60u * 1000u)  // 干净:20 分钟/点
#define PET_DECAY_HAPPY_MS (30u * 60u * 1000u)  // 心情:30 分钟/点

// 冷启动(重新上电)等效衰减:统一按 6 小时计(设备无 RTC 电池,关机时长不可知)
#define PET_COLD_BOOT_MS (6u * 60u * 60u * 1000u)

// 活动时长(到时自动结束)
#define PET_SUN_DURATION_MS (8u * 1000u)    // 晒太阳 8s
#define PET_WALK_DURATION_MS (12u * 1000u)  // 散步 12s
#define PET_BRUSH_SHINE_STROKES 5           // 连刷 5 下毛发发亮

// 空闲喵叫
#define PET_IDLE_MEOW_MIN_MS (20u * 1000u)  // 空闲多久后开始有机会喵
#define PET_IDLE_MEOW_ROLL_MS 1000u         // 掷骰间隔
#define PET_IDLE_MEOW_CHANCE 5              // 每秒命中概率的倒数(约 1/25 → 均值 ~25s)

// 成长阶段:按累计照顾量(吃鱼+刷毛+晒太阳+散步)推导,不单独存档
#define PET_GROWTH_T1 50    // 幼猫 → 少年(约一个周末的陪伴量)
#define PET_GROWTH_T2 150   // 少年 → 成年
#define PET_MILESTONE_STEP 10  // 累计每满 10 次,UI 演出一次小庆祝

typedef enum {
    PET_ACTIVITY_NONE = 0,
    PET_ACTIVITY_FEED,    // 喂食
    PET_ACTIVITY_BRUSH,   // 刷毛
    PET_ACTIVITY_SUN,     // 晒太阳
    PET_ACTIVITY_WALK,    // 散步
} pet_activity_t;

typedef enum {
    PET_WANT_NONE = 0,
    PET_WANT_FOOD,   // 想吃鱼
    PET_WANT_BRUSH,  // 想刷毛
    PET_WANT_WALK,   // 想散步(心情低)
    PET_WANT_PLAY,   // 无需求,UI 随机挑可爱气泡(蝴蝶/毛线球)
} pet_want_t;

// 活动内一次"确定"交互的结果,UI/声音据此演出
typedef enum {
    PET_INTERACT_IDLE = 0,   // 不在活动中(忽略)
    PET_INTERACT_FED,        // 吃了一口鱼
    PET_INTERACT_REFUSED,    // 礼貌拒喂(已饱;不结束活动,UI 演出摇头/害羞)
    PET_INTERACT_BRUSHED,    // 刷了一下
    PET_INTERACT_SHINE,      // 第 5 下:毛发发亮(活动就此结束)
    PET_INTERACT_WOKE,       // 晒太阳中被确定键提前唤醒
    PET_INTERACT_JUMPED,     // 散步中扑跳
    PET_INTERACT_ENDED,      // 交互后活动自动收尾(FED 第 3 条后)
} pet_interact_t;

typedef struct {
    // 状态
    uint8_t fullness;  // 饱食
    uint8_t clean;     // 干净
    uint8_t happy;     // 心情
    // 活动状态机
    pet_activity_t activity;
    uint8_t brush_strokes_this;   // 本次刷毛活动已刷几下
    uint8_t feed_fish_this;       // 本次喂食已吃几条鱼
    uint32_t activity_ms;         // 当前活动已进行毫秒
    // 互动计数(家长向统计)
    uint32_t feed_fish;           // 累计吃掉的鱼
    uint32_t brush_strokes;       // 累计刷毛下数
    uint32_t walk_count;          // 累计散步次数
    uint32_t sun_count;           // 累计晒太阳次数
    // 内部计时(不序列化的部分放最后,便于区分)
    uint32_t last_ms;             // 上次 tick 的 now_ms
    uint32_t full_acc, clean_acc, happy_acc;  // 衰减毫秒累积器
    uint32_t idle_ms;             // 距上次交互/喵叫
    uint32_t meow_roll_acc;       // 喵叫掷骰累积
    uint32_t rng;                 // xorshift32 状态(seed 后非零)
    // 瞬态(不序列化):某项累计刚好跨过里程碑台阶时置为对应活动,
    // 由 UI 取走(pet_model_take_milestone)后清零
    pet_activity_t milestone;
} pet_model_t;

void pet_model_init(pet_model_t *m);
// 冷启动:按 PET_COLD_BOOT_MS 等效衰减一次(读档后调用;封顶到下限)。
// 返回"思念等级" 0..2:断电期间状态掉得越多越大,供 UI 播重逢演出。
uint8_t pet_model_cold_boot(pet_model_t *m);
// 心跳:now_ms 单调递增;内部含衰减、活动计时、自动结束、空闲喵叫判定
void pet_model_tick(pet_model_t *m, uint32_t now_ms);

bool pet_model_start_activity(pet_model_t *m, pet_activity_t a);
pet_interact_t pet_model_interact(pet_model_t *m);
void pet_model_finish_activity(pet_model_t *m);

pet_want_t pet_model_want(const pet_model_t *m);
// 空闲喵叫判定(tick 内部推进;此函数供测试断言与 UI 查询)
bool pet_model_should_meow(pet_model_t *m);

// 成长阶段 0 幼猫 / 1 少年 / 2 成年:由累计照顾量推导(无需存档)
uint8_t pet_model_growth_stage(const pet_model_t *m);
// 取走待演出的里程碑(累计某项刚跨过 PET_MILESTONE_STEP 的倍数);
// 返回 PET_ACTIVITY_NONE 表示没有。取走即清零。
pet_activity_t pet_model_take_milestone(pet_model_t *m);

// —— 测试辅助:直接改状态(仅 host 测试与读档恢复用) ——
void pet_model_set_stats(pet_model_t *m, uint8_t full, uint8_t clean, uint8_t happy);
