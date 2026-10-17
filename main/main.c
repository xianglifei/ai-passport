// main/main.c —— 卡卡(Kaka)电子宠物猫应用壳。
//
// 按键语义(全局):
//   上/下 短按  主界面:切换 dock 活动项;场景内:戳一戳(眯眼微反馈)
//   确定 短按   主界面:开始选中活动;活动中:场景内交互
//   确定 长按   睡前仪式(夜晚屏 + 摇篮曲,任意键唤醒)
//   上   长按   家长控制:音量循环 高→中→静音(NVS 记忆)
//   下   长按   回忆相册(累计成就 + 玩法说明,任意键退出)
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
#include <stdio.h>

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
#define REFUSE_STREAK_MAX 3      // 连续拒喂第 3 次:卡卡吃好了,温柔收尾

typedef struct {
    bsp_btn_t btn;
    bsp_btn_ev_t event;
} input_event_t;

typedef enum {
    WORK_SAVE,
} work_item_t;

static void enqueue_work(work_item_t item);  // 供 enter_night 等早于定义使用

typedef enum {
    APP_NAMING,  // 首次开机:从预设名里挑选(上/下 换,OK 定)
    APP_AWAKE,   // 正常游玩
    APP_NIGHT,   // 夜晚屏(睡前仪式/自动休眠),任意键唤醒
    APP_ALBUM,   // 回忆相册(静态页),任意键退出
} app_mode_t;

typedef struct {
    pet_model_t model;
    app_mode_t mode;
    int selected;             // dock 当前选中项(0..PET_DOCK_COUNT-1)
    uint8_t level;            // 音量档 0 静音/1 中/2 高(NVS "vol")
    int8_t name_idx;          // 名字索引(NVS "name";-1 = 首次开机未命名)
    uint8_t stage;            // 成长阶段 0/1/2(由累计照顾量推导)
    uint8_t refuse_streak;    // 喂食场景连续礼貌拒喂计数
    bool reunion;             // 断电重逢演出待播(读档且掉状态明显)
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
    pet_scenes_stop();  // 删屏前清场景登记表,否则动画心跳对悬空指针 UAF
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
    uint32_t now = s_app.last_input_ms;

    if (s_app.mode == APP_NAMING) {  // 首次开机:上/下 换名,OK 定下
        if (e->event != BSP_BTN_CLICK) return;
        switch (e->btn) {
        case BSP_BTN_UP:
            s_app.name_idx = (s_app.name_idx + PET_NAME_COUNT - 1) % PET_NAME_COUNT;
            pet_ui_name_picker_select((uint8_t)s_app.name_idx);
            pet_audio_play(SND_MENU_TICK);
            break;
        case BSP_BTN_DOWN:
            s_app.name_idx = (s_app.name_idx + 1) % PET_NAME_COUNT;
            pet_ui_name_picker_select((uint8_t)s_app.name_idx);
            pet_audio_play(SND_MENU_TICK);
            break;
        case BSP_BTN_OK:
            pet_nvs_set_name((uint8_t)s_app.name_idx);
            pet_ui_set_name((uint8_t)s_app.name_idx);
            pet_audio_play(SND_SELECT);
            s_app.mode = APP_AWAKE;
            pet_ui_show_home(&s_app.model);
            pet_ui_set_battery(bsp_battery_soc());
            pet_audio_play(SND_MEOW_GREET);
            ESP_LOGI(TAG, "命名完成:%s", PET_NAMES[s_app.name_idx]);
            break;
        default:
            break;
        }
        return;
    }
    if (s_app.mode == APP_ALBUM) {  // 回忆页:OK 长按改名,其余任意键退出
        if (e->btn == BSP_BTN_OK && e->event == BSP_BTN_LONG) {
            s_app.mode = APP_NAMING;
            pet_ui_show_name_picker();
            pet_audio_play(SND_SELECT);
            return;
        }
        s_app.mode = APP_AWAKE;
        pet_ui_show_home(&s_app.model);
        pet_ui_set_battery(bsp_battery_soc());
        pet_audio_play(SND_BUBBLE);
        return;
    }
    if (s_app.mode == APP_NIGHT) {
        wake_up();  // 任意键唤醒,本键不穿透
        return;
    }
    if (s_app.dimmed) {  // 打盹降光后,任意操作立即恢复
        s_app.dimmed = false;
        bsp_display_backlight(100);
    }
    if (e->btn == BSP_BTN_UP && e->event == BSP_BTN_LONG) {
        // 家长控制:音量循环 高→中→静音→高
        s_app.level = (uint8_t)((s_app.level + 2) % 3);
        pet_audio_set_level(s_app.level);
        pet_nvs_set_volume(s_app.level);
        pet_ui_toast(s_app.level == 2 ? "\u97f3\u91cf \u9ad8"        // 音量 高
                        : s_app.level == 1 ? "\u97f3\u91cf \u4e2d"   // 音量 中
                                           : "\u9759\u97f3 \u5f00"); // 静音 开
        pet_audio_play(SND_BUBBLE);
        ESP_LOGI(TAG, "音量档 %u", s_app.level);
        return;
    }
    if (e->btn == BSP_BTN_OK && e->event == BSP_BTN_LONG) {
        enter_night();  // 睡前仪式
        return;
    }
    if (e->btn == BSP_BTN_DOWN && e->event == BSP_BTN_LONG) {
        if (s_app.model.activity == PET_ACTIVITY_NONE) {  // 活动中不打断
            s_app.mode = APP_ALBUM;
            pet_ui_show_album(&s_app.model, bsp_battery_soc());
            pet_audio_play(SND_SELECT);
        }
        return;
    }
    // 活动进行中:场景内交互
    if (s_app.model.activity != PET_ACTIVITY_NONE) {
        if (e->event != BSP_BTN_CLICK) return;
        if (e->btn == BSP_BTN_OK) {
            pet_interact_t r = pet_model_interact(&s_app.model);
            s_app.scene_refused = (r == PET_INTERACT_REFUSED);
            if (r == PET_INTERACT_REFUSED) {
                // 礼貌拒喂不退出场景;连续第 3 次说明真的吃不下了,温柔收尾
                if (++s_app.refuse_streak >= REFUSE_STREAK_MAX) {
                    s_app.refuse_streak = 0;
                    pet_model_finish_activity(&s_app.model);
                    pet_audio_play(SND_YAWN);
                }
            } else {
                s_app.refuse_streak = 0;
            }
            pet_scenes_interact(r, now);
        } else {
            // 戳一戳:场景内的上/下不再是死键
            pet_scenes_poke(now);
            pet_audio_play(SND_SQUEAK);
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

// 里程碑提示:"第 N 次 XX"(N 为刚跨过的累计值)
static void milestone_toast(pet_activity_t a) {
    uint32_t n = 0;
    const char *what = NULL;
    switch (a) {
    case PET_ACTIVITY_FEED:  n = s_app.model.feed_fish;     what = "\u5582\u98df"; break;   // 喂食
    case PET_ACTIVITY_BRUSH: n = s_app.model.brush_strokes; what = "\u5237\u6bdb"; break;   // 刷毛
    case PET_ACTIVITY_SUN:   n = s_app.model.sun_count;     what = "\u6652\u592a\u9633"; break;  // 晒太阳
    case PET_ACTIVITY_WALK:  n = s_app.model.walk_count;    what = "\u6563\u6b65"; break;   // 散步
    default: return;
    }
    char buf[28];
    snprintf(buf, sizeof(buf), "\u7b2c%lu\u6b21%s",  // 第N次XX
             (unsigned long)n, what);
    pet_ui_toast(buf);
}

// —— 周期心跳(LVGL 任务内,天然持锁) ——
static void app_tick(lv_timer_t *timer) {
    (void)timer;
    static uint32_t tick_count;
    tick_count++;

    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    pet_model_tick(&s_app.model, now_ms);

    if (s_app.mode != APP_AWAKE) {
        if (s_app.mode == APP_ALBUM &&
            now_ms - s_app.last_input_ms >= DIM_AFTER_MS) {
            // 回忆页没人看了:自己回家,交回正常的降光/入睡流程
            s_app.mode = APP_AWAKE;
            pet_ui_show_home(&s_app.model);
            pet_ui_set_battery(bsp_battery_soc());
        }
        return;  // 命名/夜晚/回忆:静止页,不衰减演出、不喵叫(模型照常走)
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

    // 场景收尾:模型活动已结束(吃完/刷亮/晒完/走完/吃好了)→ 回家庆祝。
    // 先停场景再换屏:旧屏删除后场景指针即悬空,不停会在 80ms 心跳里 UAF。
    if (pet_scenes_active() && s_app.model.activity == PET_ACTIVITY_NONE) {
        pet_scenes_stop();
        // 成长阶段:跨过台阶先换渲染/音高再进新屏,回家的就是大一号的猫
        uint8_t stage = pet_model_growth_stage(&s_app.model);
        if (stage != s_app.stage) {
            s_app.stage = stage;
            pet_ui_set_stage(stage);
            pet_audio_set_stage(stage);
            pet_ui_toast("\u957f\u5927\u4e86!");  // 长大了!
            pet_audio_play(SND_MEOW_HAPPY);
        }
        pet_ui_show_home(&s_app.model);
        pet_ui_celebrate(s_app.scene_refused ? PET_INTERACT_REFUSED
                                             : PET_INTERACT_ENDED,
                         now_ms);
        // 里程碑:累计某项刚满 10 的倍数,加演小庆祝
        pet_activity_t hit = pet_model_take_milestone(&s_app.model);
        if (hit != PET_ACTIVITY_NONE) {
            milestone_toast(hit);
            pet_audio_play(SND_SPARKLE);
        }
        enqueue_work(WORK_SAVE);  // 活动边界存档
    } else {
        pet_ui_tick(&s_app.model, now_ms);
    }

    // 空闲喵叫:按需求状态挑"台词"
    if (s_app.level != 0 && !pet_scenes_active() &&
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
    pet_ui_particles_tick(now_ms);
}

// —— 启动 ——
void app_main(void) {
    // 存档:读到就补冷启动衰减(设备无 RTC 电池,关机时长不可知,统一按 6h 等效);
    // 掉状态明显时安排一场"重逢"演出。读不到就走全新初始状态。
    pet_nvs_init();
    bool restored = pet_nvs_load(&s_app.model);
    uint8_t miss = 0;
    if (restored) {
        miss = pet_model_cold_boot(&s_app.model);
        ESP_LOGI(TAG, "存档已恢复并补 6h 冷启动衰减(思念等级 %u)", miss);
    } else {
        pet_model_init(&s_app.model);
        ESP_LOGI(TAG, "新档开始:小小一只等着你");
    }
    int8_t stored_name = pet_nvs_name();
    // 未命名时索引归 0(与命名屏初始显示"卡卡"一致):
    // 若留 -1,首次按上键会 (−1+5)%6=4 直接跳"煤球",显示与索引错一格
    s_app.name_idx = stored_name < 0 ? 0 : stored_name;
    s_app.level = pet_nvs_volume();
    s_app.stage = pet_model_growth_stage(&s_app.model);
    s_app.reunion = restored && miss > 0;
    s_app.selected = 0;
    s_app.mode = (stored_name < 0) ? APP_NAMING : APP_AWAKE;
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
    pet_audio_set_level(s_app.level);
    pet_audio_set_stage(s_app.stage);

    s_app.input_queue = xQueueCreate(INPUT_QUEUE_DEPTH, sizeof(input_event_t));
    s_app.work_queue = xQueueCreate(WORK_QUEUE_DEPTH, sizeof(work_item_t));
    xTaskCreate(input_task, "kaka_input", 4096, NULL, 5, NULL);
    xTaskCreate(worker_task, "kaka_work", 4096, NULL, 4, NULL);

    if (bsp_lvgl_lock(1000)) {
        pet_font_verify();  // 开机逐字审计(结果记录在日志,交付时核对)
        pet_ui_init();
        pet_ui_set_name(s_app.name_idx < 0 ? 0 : (uint8_t)s_app.name_idx);
        pet_ui_set_stage(s_app.stage);
        if (s_app.mode == APP_NAMING) {
            pet_ui_show_name_picker();  // 首次开机:先起个名字
        } else {
            pet_ui_show_home(&s_app.model);
            pet_ui_set_battery(bsp_battery_soc());
            if (s_app.reunion) {  // 断电重逢:开心帧 + 爱心雨 + "想你了"
                s_app.reunion = false;
                pet_ui_reunion((uint32_t)(esp_timer_get_time() / 1000));
                pet_ui_toast("\u60f3\u4f60\u4e86");  // 想你了
                pet_audio_play(SND_MEOW_GREET);
            }
        }
        // lv_timer 链表非线程安全:必须在持锁上下文创建,否则与 LVGL
        // 任务的 lv_timer_handler 并发遍历链表构成数据竞争。
        lv_timer_create(app_tick, 250, NULL);
        lv_timer_create(scene_anim_tick, 80, NULL);
        bsp_lvgl_unlock();
    } else {
        ESP_LOGE(TAG, "LVGL 锁获取失败,界面不可用");
    }

    bsp_button_init(on_key, NULL);
    ESP_LOGI(TAG, "卡卡就绪");
}
