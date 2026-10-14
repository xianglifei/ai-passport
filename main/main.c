// main/main.c —— 卡卡(Kaka)电子宠物猫应用壳。
//
// 按键语义(全局):
//   上/下 短按  主界面:切换 dock 活动项(喂食/刷毛/晒太阳/散步)
//   确定 短按   主界面:开始选中活动;活动中:场景内交互(M4 接入演出)
//   确定 长按   睡前仪式(M7 接入;当前占位打日志)
//   上   长按   静音开/关(NVS 记忆;BSP 长按阈值为 500ms)
//
// 线程模型(沿用本仓库 feature 应用惯例):
//   - 按键回调跑在共享 esp_timer 任务:只入队,不做任何别的事;
//   - input 任务:持 LVGL 锁分发按键;
//   - app_tick 是 250ms lv_timer,跑在 LVGL 任务内(天然持锁);
//   - 慢操作(NVS 保存)走 worker 任务队列,绝不占按键回调/LVGL 任务;
//   - 单核 C3:模型 -> UI 用"先写数据后读"即可,无需原子量。
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lvgl.h"

#include "bsp_battery.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "pet_audio.h"
#include "pet_font.h"
#include "pet_model.h"
#include "pet_nvs.h"
#include "pet_scenes.h"
#include "pet_ui.h"

static const char *TAG = "kaka";

#define INPUT_QUEUE_DEPTH 8
#define WORK_QUEUE_DEPTH 4
#define AUTOSAVE_TICKS 240   // 250ms x 240 = 60s 自动存档
#define BATTERY_TICKS 20     // 250ms x 20  = 5s 电池刷新
#define DIM_AFTER_MS 60000u      // 无操作 60s:背光降到 30%
#define NIGHT_AFTER_MS 300000u   // 无操作 5min:进夜晚屏(自动睡)

typedef struct {
    bsp_btn_t btn;
    bsp_btn_ev_t event;
} input_event_t;

typedef enum {
    WORK_SAVE,
} work_item_t;

static void enqueue_work(work_item_t item);  // 供 enter_night 等早于定义使用

typedef enum {
    APP_AWAKE,   // 正常游玩
    APP_NIGHT,   // 夜晚屏(睡前仪式/自动休眠),任意键唤醒
} app_mode_t;

typedef struct {
    pet_model_t model;
    app_mode_t mode;
    int selected;             // dock 当前选中项(0..PET_DOCK_COUNT-1)
    bool muted;
    bool scene_refused;       // 最近一次场景交互是否为礼貌拒喂(回家庆祝用)
    bool dimmed;              // 60s 无操作已降背光
    uint32_t last_input_ms;   // 最近一次按键时刻
    QueueHandle_t input_queue;
    QueueHandle_t work_queue;
} app_t;

static app_t s_app;

// —— 输入:按键回调(esp_timer 任务)只入队 ——
static void on_key(bsp_btn_t btn, bsp_btn_ev_t event, void *user) {
    (void)user;
    input_event_t e = { btn, event };
    xQueueSend(s_app.input_queue, &e, 0);
}

// —— 输入分发(持 LVGL 锁) ——
static void enter_night(void) {
    s_app.mode = APP_NIGHT;
    // 活动进行中睡着:先按"提前结束"结算,再进夜晚
    if (s_app.model.activity != PET_ACTIVITY_NONE) {
        pet_model_finish_activity(&s_app.model);
    }
    pet_ui_show_night();
    pet_ui_set_battery(bsp_battery_soc());
    pet_audio_play(SND_LULLABY);
    bsp_display_backlight(8);
    enqueue_work(WORK_SAVE);
    ESP_LOGI(TAG, "晚安:卡卡睡着了");
}

static void wake_up(void) {
    s_app.mode = APP_AWAKE;
    s_app.dimmed = false;
    pet_ui_show_home(&s_app.model);
    pet_ui_set_battery(bsp_battery_soc());
    pet_audio_play(SND_STRETCH);
    pet_audio_play(SND_MEOW_GREET);
    bsp_display_backlight(100);
    ESP_LOGI(TAG, "早上好:卡卡醒了");
}

static void handle_input(input_event_t *e) {
    s_app.last_input_ms = (uint32_t)(esp_timer_get_time() / 1000);
    if (s_app.mode == APP_NIGHT) {
        wake_up();  // 任意键唤醒,本键不穿透
        return;
    }
    if (s_app.dimmed) {  // 打盹降光后,任意操作立即恢复
        s_app.dimmed = false;
        bsp_display_backlight(100);
    }
    if (e->btn == BSP_BTN_UP && e->event == BSP_BTN_LONG) {
        // 家长控制:静音切换
        s_app.muted = !s_app.muted;
        pet_audio_set_muted(s_app.muted);
        pet_nvs_set_mute(s_app.muted);
        pet_ui_toast(s_app.muted ? "\u9759\u97f3 \u5f00"    // 静音 开
                                 : "\u9759\u97f3 \u5173");  // 静音 关
        pet_audio_play(SND_BUBBLE);
        ESP_LOGI(TAG, "%s", s_app.muted ? "静音开" : "静音关");
        return;
    }
    if (e->btn == BSP_BTN_OK && e->event == BSP_BTN_LONG) {
        enter_night();  // 睡前仪式
        return;
    }
    // 活动进行中:场景内交互
    if (s_app.model.activity != PET_ACTIVITY_NONE) {
        if (e->btn == BSP_BTN_OK && e->event == BSP_BTN_CLICK) {
            pet_interact_t r = pet_model_interact(&s_app.model);
            s_app.scene_refused = (r == PET_INTERACT_REFUSED);
            pet_scenes_interact(r, (uint32_t)(esp_timer_get_time() / 1000));
        }
        return;
    }
    if (e->event != BSP_BTN_CLICK) return;
    switch (e->btn) {
    case BSP_BTN_UP:
        s_app.selected = (s_app.selected + PET_DOCK_COUNT - 1) % PET_DOCK_COUNT;
        pet_ui_select(s_app.selected);
        pet_audio_play(SND_MENU_TICK);
        break;
    case BSP_BTN_DOWN:
        s_app.selected = (s_app.selected + 1) % PET_DOCK_COUNT;
        pet_ui_select(s_app.selected);
        pet_audio_play(SND_MENU_TICK);
        break;
    case BSP_BTN_OK: {
        pet_activity_t a = (pet_activity_t)(s_app.selected + 1);
        if (pet_model_start_activity(&s_app.model, a)) {
            pet_audio_play(SND_SELECT);
            s_app.scene_refused = false;
            pet_scenes_show(a, &s_app.model, bsp_battery_soc());
        }
        break;
    }
    default:
        break;
    }
}

static void input_task(void *arg) {
    (void)arg;
    input_event_t e;
    for (;;) {
        if (xQueueReceive(s_app.input_queue, &e, portMAX_DELAY) != pdTRUE) continue;
        if (bsp_lvgl_lock(500)) {
            handle_input(&e);
            bsp_lvgl_unlock();
        }
    }
}

// —— 慢操作:worker 任务 ——
static void worker_task(void *arg) {
    (void)arg;
    work_item_t item;
    for (;;) {
        if (xQueueReceive(s_app.work_queue, &item, portMAX_DELAY) != pdTRUE) continue;
        switch (item) {
        case WORK_SAVE:
            pet_nvs_save(&s_app.model);
            break;
        default:
            break;
        }
    }
}

static void enqueue_work(work_item_t item) {
    xQueueSend(s_app.work_queue, &item, 0);
}

// —— 周期心跳(LVGL 任务内,天然持锁) ——
static void app_tick(lv_timer_t *timer) {
    (void)timer;
    static uint32_t tick_count;
    tick_count++;

    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    pet_model_tick(&s_app.model, now_ms);

    if (s_app.mode == APP_NIGHT) {
        return;  // 夜晚屏静止:不衰减演出、不喵叫(模型照常走,速率很慢)
    }

    // 背光分级:60s 无操作降到 30%,5min 进夜晚屏
    uint32_t idle = now_ms - s_app.last_input_ms;
    if (!s_app.dimmed && idle >= DIM_AFTER_MS && idle < NIGHT_AFTER_MS) {
        s_app.dimmed = true;
        bsp_display_backlight(30);
    }
    if (idle >= NIGHT_AFTER_MS) {
        enter_night();
        return;
    }

    // 场景收尾:模型活动已结束(吃完/刷亮/晒完/走完/拒喂)→ 回家庆祝
    if (pet_scenes_active() && s_app.model.activity == PET_ACTIVITY_NONE) {
        pet_ui_show_home(&s_app.model);
        pet_ui_celebrate(s_app.scene_refused ? PET_INTERACT_REFUSED
                                             : PET_INTERACT_ENDED,
                         now_ms);
        enqueue_work(WORK_SAVE);  // 活动边界存档
    } else {
        pet_ui_tick(&s_app.model, now_ms);
    }

    // 空闲喵叫:按需求状态挑"台词"
    if (!s_app.muted && !pet_scenes_active() &&
        pet_model_should_meow(&s_app.model)) {
        pet_want_t want = pet_model_want(&s_app.model);
        pet_sound_t s = SND_MEOW_GREET;
        if (want == PET_WANT_FOOD) {
            s = SND_MEOW_HUNGRY;
        } else if (want == PET_WANT_WALK || want == PET_WANT_BRUSH) {
            s = SND_MEOW_SWEET;
        } else {
            switch (tick_count % 3) {
            case 0: s = SND_MEOW_GREET; break;
            case 1: s = SND_MEOW_HAPPY; break;
            default: s = SND_TRILL; break;
            }
        }
        pet_audio_play(s);
    }

    if (tick_count % BATTERY_TICKS == 0) {
        pet_ui_set_battery(bsp_battery_soc());
    }
    if (tick_count % AUTOSAVE_TICKS == 0) {
        enqueue_work(WORK_SAVE);
    }
}

// —— 80ms 动画心跳:场景帧循环/视差 + 共享粒子(LVGL 任务内) ——
static void scene_anim_tick(lv_timer_t *timer) {
    (void)timer;
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    pet_scenes_anim(&s_app.model, now_ms);
    pet_ui_particles_tick();
}

// —— 启动 ——
void app_main(void) {
    // 存档:读到就补冷启动衰减(设备无 RTC 电池,关机时长不可知,统一按 6h 等效);
    // 读不到就走全新初始状态。
    pet_nvs_init();
    bool restored = pet_nvs_load(&s_app.model);
    if (restored) {
        pet_model_cold_boot(&s_app.model);
        ESP_LOGI(TAG, "存档已恢复并补 6h 冷启动衰减");
    } else {
        pet_model_init(&s_app.model);
        ESP_LOGI(TAG, "新档开始:卡卡等着你");
    }
    s_app.muted = pet_nvs_mute();
    s_app.selected = 0;
    s_app.mode = APP_AWAKE;
    s_app.last_input_ms = (uint32_t)(esp_timer_get_time() / 1000);

    if (bsp_display_init() != ESP_OK) {
        ESP_LOGE(TAG, "显示初始化失败");
        return;
    }
    if (bsp_lvgl_init() == NULL) {
        ESP_LOGE(TAG, "LVGL 初始化失败");
        return;
    }
    bsp_display_backlight(100);
    bsp_battery_init();          // 非致命:失败时界面显示 "--"
    if (!pet_audio_init()) {     // 非致命:失败时游戏无声
        ESP_LOGW(TAG, "声音系统不可用");
    }
    pet_audio_set_muted(s_app.muted);

    s_app.input_queue = xQueueCreate(INPUT_QUEUE_DEPTH, sizeof(input_event_t));
    s_app.work_queue = xQueueCreate(WORK_QUEUE_DEPTH, sizeof(work_item_t));
    xTaskCreate(input_task, "kaka_input", 4096, NULL, 5, NULL);
    xTaskCreate(worker_task, "kaka_work", 4096, NULL, 4, NULL);

    if (bsp_lvgl_lock(1000)) {
        pet_font_verify();  // 开机逐字审计(结果记录在日志,交付时核对)
        pet_ui_init();
        pet_ui_show_home(&s_app.model);
        pet_ui_set_battery(bsp_battery_soc());
        bsp_lvgl_unlock();
    }

    bsp_button_init(on_key, NULL);
    lv_timer_create(app_tick, 250, NULL);
    lv_timer_create(scene_anim_tick, 80, NULL);
    ESP_LOGI(TAG, "卡卡就绪");
}
