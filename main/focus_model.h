// main/focus_model.h —— 番茄钟状态机:工作 25 分钟 / 休息 10 分钟。
// 纯逻辑,不依赖 ESP-IDF/LVGL,主机测试覆盖(tests/test_focus_model.c)。
// 时间基准:调用方传入的单调毫秒(设备上为 esp_timer,主机上为测试注入值)。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define FOCUS_WORK_SEC (25U * 60U)
#define FOCUS_REST_SEC (10U * 60U)
#define FOCUS_CELEBRATE_MS 6000U

typedef enum {
    FOCUS_STATE_HOME = 0,        // 首页:选择工作/休息/热力图
    FOCUS_STATE_WORK_RUNNING,    // 工作倒计时
    FOCUS_STATE_REST_RUNNING,    // 休息倒计时
    FOCUS_STATE_CELEBRATE,       // 工作完成后的庆祝仪式(定时自动回首页)
} focus_state_t;

typedef enum {
    FOCUS_EVENT_NONE = 0,
    FOCUS_EVENT_WORK_DONE,       // 工作 25 分钟走完 -> 进入 CELEBRATE
    FOCUS_EVENT_REST_DONE,       // 休息 10 分钟走完 -> 回 HOME
    FOCUS_EVENT_CELEBRATE_DONE,  // 庆祝超时 -> 回 HOME
} focus_event_t;

typedef struct {
    focus_state_t state;
    uint64_t deadline_ms;        // 倒计时截止(单调毫秒);非运行态为 0
    uint64_t celebrate_end_ms;   // CELEBRATE 状态的自动结束时刻
    uint32_t duration_sec;       // 当前会话总时长(用于进度显示)
} focus_model_t;

void focus_model_init(focus_model_t *model, uint64_t now_ms);

bool focus_model_start_work(focus_model_t *model, uint64_t now_ms);
bool focus_model_start_rest(focus_model_t *model, uint64_t now_ms);
// 长按 OK 放弃当前会话;HOME/CELEBRATE 下无效。
bool focus_model_abort(focus_model_t *model);
// 庆祝页任意按键跳过。
bool focus_model_skip_celebrate(focus_model_t *model);

// 周期调用(约 4Hz)。产生会话完成/庆祝超时事件并自行迁移状态。
focus_event_t focus_model_tick(focus_model_t *model, uint64_t now_ms);

// 剩余秒数(向上取整);非运行态返回 0。
uint32_t focus_model_remaining_sec(const focus_model_t *model, uint64_t now_ms);
