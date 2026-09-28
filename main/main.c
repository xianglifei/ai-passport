// main/main.c —— 番茄专注:工作 25 分钟 / 休息 10 分钟 / 24h 五分钟热力图。
//
// 按键语义(全局一致):
//   上/下 单击   首页=移动选中项;热力图=切换查看日;时间设置=±5 分钟;倒计时=无动作
//   上 长按      时间设置=快调 +1 小时
//   下 长按      时间设置=Wi-Fi 对时(配网页)
//   确定  单击   首页=进入选中项;热力图=返回(时钟未设置时进入设置);设置=保存;Wi-Fi=返回
//   确定  长按   倒计时=放弃回首页;热力图=进入时间设置;设置=取消(时钟已设置时)
//   庆祝页       任意按键跳过;6 秒后自动回首页
//
// 线程模型:按键回调只入队;input 任务持 bsp_lvgl_lock 处理输入;
// app_tick 跑在 LVGL 任务内(免锁);NVS 写入与 Wi-Fi 操作在 focus_work 任务;
// 音效有独立任务。Wi-Fi 状态/对时结果由 app_tick 轮询,不在 Wi-Fi 任务碰 UI。
//
// 时间:开机有凭据自动连 Wi-Fi + SNTP(UTC+8 折算本地),无凭据/失败走手动设置;
// 设备没有 RTC 后备电池,断电重启后若对时不可用需重新手动对时。
#include "app_audio.h"
#include "app_clock.h"
#include "app_store.h"
#include "app_ui.h"
#include "app_wifi.h"
#include "bsp_battery.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "bsp_pins.h"
#include "focus_model.h"
#include "heatmap_store.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lvgl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "focus_app";

#define TICK_PERIOD_MS 250
#define CLOCK_SAVE_INTERVAL_MS 30000
#define BATTERY_INTERVAL_MS 30000
#define INPUT_QUEUE_DEPTH 8
#define WORK_QUEUE_DEPTH 8
#define TIMESET_DEFAULT_MIN (12U * 60U)
// 设备无联网前用户手设的本地时间;SNTP 对时结果按 UTC+8 折算(见 app_wifi.c)。
#define APP_TZ_OFFSET_SEC (8 * 3600)

typedef enum {
    PAGE_HOME = 0,
    PAGE_COUNTDOWN,
    PAGE_CELEBRATE,
    PAGE_HEATMAP,
    PAGE_TIMESET,
    PAGE_WIFI,
} app_page_t;

typedef enum {
    WORK_SAVE_HM = 1,       // 写热力图 blob(item.blob)
    WORK_SAVE_CLOCK,        // 写时钟(item.a=epoch, item.b=boot_ms)
    WORK_WIFI_STA,          // 用已存凭据连 Wi-Fi
    WORK_WIFI_PROVISION,    // 开配网热点
    WORK_WIFI_STOP,         // 停止 Wi-Fi/SNTP
} work_type_t;

typedef struct {
    work_type_t type;
    uint8_t *blob;          // WORK_SAVE_HM:malloc 的序列化结果,worker 负责 free
    uint64_t a;
    uint64_t b;
} work_item_t;

typedef struct {
    bsp_btn_t btn;
    bsp_btn_ev_t event;
} input_event_t;

typedef struct {
    focus_model_t model;
    hm_store_t hm;
    uint64_t boot_epoch;        // 本地墙钟秒;0 = 从未设置
    uint64_t clock_saved_ms;    // 上次续存时的单调毫秒
    uint16_t anchor_day;        // 固件构建日期,首次设置时间的日期锚点

    // 倒计时染色:会话推进时给经过的 5 分钟格上色
    bool mark_active;
    uint16_t mark_day;
    uint16_t mark_bucket;

    app_page_t page;
    uint32_t home_sel;
    uint16_t view_day;          // 热力图正在查看的日索引
    uint32_t timeset_min;       // 时间设置页的当前值(分钟)
    bool timeset_forced;        // 时钟从未设置:不允许取消

    QueueHandle_t work_queue;
} app_t;

static app_t s_app;
static QueueHandle_t s_input_queue;
static volatile bool s_input_ready;
static app_wifi_state_t s_last_wifi_state = APP_WIFI_IDLE;
static uint64_t s_wifi_done_at;         // 对时成功后自动回首页的时刻;0=未定时

// ---------------------------------------------------------------------------
// 固件构建日期(__DATE__ = "Mmm dd yyyy")作为热力图的日期锚点。
// 刷机与首次对时通常相距不远;偏差可在交付说明中核对。
// ---------------------------------------------------------------------------
static uint16_t build_day_index(void) {
    static const char *const MONTHS[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                          "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    char mon[4] = {0};
    int day = 0, year = 0;
    if (sscanf(__DATE__, "%3s %d %d", mon, &day, &year) != 3) {
        return 0;
    }
    int month = 0;
    for (int i = 0; i < 12; i++) {
        if (strcmp(mon, MONTHS[i]) == 0) month = i + 1;
    }
    if (month == 0 || year < 1970) return 0;
    return app_clock_days_from_civil(year, month, day);
}

static uint64_t now_ms(void) {
    return (uint64_t)esp_timer_get_time() / 1000ULL;
}

static uint16_t today_day_index(void) {
    return app_clock_day_index(s_app.boot_epoch + now_ms() / 1000ULL);
}

// ---------------------------------------------------------------------------
// 页面流转
// ---------------------------------------------------------------------------
static void go_home(void) {
    s_app.page = PAGE_HOME;
    app_ui_show_home(s_app.home_sel);
}

static void go_home_with_toast(const char *text) {
    s_app.page = PAGE_HOME;
    app_ui_show_home(s_app.home_sel);
    app_ui_toast(text);
}

static void enter_countdown(bool work) {
    uint64_t now = now_ms();
    bool started = work ? focus_model_start_work(&s_app.model, now)
                        : focus_model_start_rest(&s_app.model, now);
    if (!started) return;
    s_app.mark_active = false;
    s_app.page = PAGE_COUNTDOWN;
    app_ui_show_countdown(work, work ? FOCUS_WORK_SEC : FOCUS_REST_SEC,
                          s_app.boot_epoch != 0);
}

static void rebuild_heatmap(void) {
    uint16_t today = today_day_index();
    if (s_app.view_day > today) s_app.view_day = today;
    uint16_t oldest = (uint16_t)(today >= HM_DAYS - 1 ? today - (HM_DAYS - 1) : 0);
    if (s_app.view_day < oldest) s_app.view_day = oldest;
    hm_day_t *day = hm_day_find(&s_app.hm, s_app.view_day);
    bool is_today = s_app.view_day == today;
    uint16_t bucket = 0xFFFF;
    if (is_today && s_app.boot_epoch) {
        bucket = app_clock_bucket(app_clock_minute_of_day(
            s_app.boot_epoch + now_ms() / 1000ULL));
    }
    app_ui_show_heatmap(s_app.view_day, day, is_today, bucket,
                        s_app.boot_epoch == 0);
}

static void enter_heatmap(void) {
    s_app.page = PAGE_HEATMAP;
    s_app.view_day = s_app.boot_epoch ? today_day_index() : 0;
    rebuild_heatmap();
}

static void enter_timeset(void) {
    s_app.page = PAGE_TIMESET;
    s_app.timeset_forced = s_app.boot_epoch == 0;
    if (s_app.boot_epoch) {
        s_app.timeset_min = app_clock_minute_of_day(
            s_app.boot_epoch + now_ms() / 1000ULL);
    } else {
        s_app.timeset_min = TIMESET_DEFAULT_MIN;
    }
    app_ui_show_timeset(s_app.timeset_min, s_app.timeset_forced);
}

// ---------------------------------------------------------------------------
// Wi-Fi 对时页:状态文本由 app_tick 轮询刷新
// ---------------------------------------------------------------------------
static void wifi_page_refresh(void) {
    const char *l1 = "", *l2 = "", *l3 = "";
    switch (app_wifi_state()) {
    case APP_WIFI_AP_MODE:
        l1 = "请在手机上连接热点:";
        l2 = app_wifi_ap_name();
        l3 = "浏览器打开 192.168.4.1";
        break;
    case APP_WIFI_AP_CONNECTED:
        l1 = "等待打开配网地址:";
        l2 = app_wifi_ap_name();
        l3 = "浏览器打开 192.168.4.1";
        break;
    case APP_WIFI_CONNECTING:
        l1 = "正在连接 Wi-Fi...";
        l2 = app_wifi_ssid();
        break;
    case APP_WIFI_CONNECTED:
        l1 = "已连接,正在对时...";
        l2 = app_wifi_ssid();
        break;
    case APP_WIFI_SYNCED:
        l1 = "对时成功";
        break;
    case APP_WIFI_FAILED:
        l1 = "连接失败,可重试";
        break;
    default:
        l1 = "Wi-Fi 未运行";
        break;
    }
    app_ui_wifi_update(l1, l2, l3);
}

static void enqueue_work(work_type_t type) {
    if (!s_app.work_queue) return;
    work_item_t item = { .type = type };
    xQueueSend(s_app.work_queue, &item, 0);
}

static void enter_wifi_page(void) {
    s_app.page = PAGE_WIFI;
    s_last_wifi_state = APP_WIFI_IDLE;
    s_wifi_done_at = 0;
    app_ui_show_wifi("正在开启热点...", "", "", "DOWN 重试 · OK 返回");
    enqueue_work(WORK_WIFI_PROVISION);
}

// 保存时间:今天取"已设置的日期(避免回退)或构建锚点",时分用用户输入。
static void save_timeset(void) {
    uint64_t now = now_ms();
    uint16_t day = s_app.boot_epoch
                       ? app_clock_day_index(s_app.boot_epoch + now / 1000ULL)
                       : s_app.anchor_day;
    if (day < s_app.anchor_day) day = s_app.anchor_day;
    uint64_t wall_now = app_clock_wall_from_hm(day, s_app.timeset_min / 60U,
                                               s_app.timeset_min % 60U);
    s_app.boot_epoch = wall_now - now / 1000ULL;
    s_app.clock_saved_ms = now;
    if (s_app.work_queue) {
        work_item_t item = { .type = WORK_SAVE_CLOCK, .a = wall_now, .b = now };
        xQueueSend(s_app.work_queue, &item, 0);
    }
    ESP_LOGI(TAG, "时间已设置: day=%u %02u:%02u",
             (unsigned)day, (unsigned)(s_app.timeset_min / 60U),
             (unsigned)(s_app.timeset_min % 60U));
}

// ---------------------------------------------------------------------------
// 染色:会话推进时把经过的 5 分钟格写进当天记录并落盘
// ---------------------------------------------------------------------------
static void queue_save_heatmap(void) {
    if (!s_app.work_queue) return;
    uint8_t *blob = malloc(HM_BLOB_SIZE);
    if (!blob) {
        ESP_LOGW(TAG, "热力图序列化缓冲分配失败,本次不落盘");
        return;
    }
    if (!hm_serialize(&s_app.hm, blob, HM_BLOB_SIZE)) {
        free(blob);
        return;
    }
    work_item_t item = { .type = WORK_SAVE_HM, .blob = blob };
    xQueueSend(s_app.work_queue, &item, 0);
}

static void mark_current_bucket(uint16_t day, uint16_t bucket, uint8_t kind) {
    hm_day_t *day_rec = hm_day_acquire(&s_app.hm, day);
    hm_mark_bucket(day_rec, bucket, kind);
    queue_save_heatmap();
}

static void update_marking(uint64_t now, uint8_t kind) {
    if (!s_app.boot_epoch) return;
    uint64_t wall = s_app.boot_epoch + now / 1000ULL;
    uint16_t day = app_clock_day_index(wall);
    uint16_t bucket = app_clock_bucket(app_clock_minute_of_day(wall));

    if (!s_app.mark_active || day != s_app.mark_day) {
        s_app.mark_active = true;
        s_app.mark_day = day;
        s_app.mark_bucket = bucket;
        mark_current_bucket(day, bucket, kind);
        return;
    }
    if (bucket == s_app.mark_bucket) return;
    hm_day_t *day_rec = hm_day_acquire(&s_app.hm, day);
    for (uint16_t b = (uint16_t)(s_app.mark_bucket + 1); b <= bucket; b++) {
        hm_mark_bucket(day_rec, b, kind);
    }
    s_app.mark_bucket = bucket;
    queue_save_heatmap();
}

// ---------------------------------------------------------------------------
// 输入
// ---------------------------------------------------------------------------
static void handle_home_input(bsp_btn_t btn, bsp_btn_ev_t ev) {
    if (ev != BSP_BTN_CLICK) return;
    if (btn == BSP_BTN_UP) {
        s_app.home_sel = (s_app.home_sel + 2U) % 3U;
        app_ui_home_select(s_app.home_sel);
    } else if (btn == BSP_BTN_DOWN) {
        s_app.home_sel = (s_app.home_sel + 1U) % 3U;
        app_ui_home_select(s_app.home_sel);
    } else if (btn == BSP_BTN_OK) {
        if (s_app.home_sel == APP_UI_HOME_WORK) enter_countdown(true);
        else if (s_app.home_sel == APP_UI_HOME_REST) enter_countdown(false);
        else enter_heatmap();
    }
}

static void handle_heatmap_input(bsp_btn_t btn, bsp_btn_ev_t ev) {
    if (ev == BSP_BTN_LONG && btn == BSP_BTN_OK) {
        enter_timeset();
        return;
    }
    if (ev != BSP_BTN_CLICK) return;
    if (btn == BSP_BTN_OK) {
        if (s_app.boot_epoch) go_home();
        else enter_timeset();
        return;
    }
    if (!s_app.boot_epoch) return;
    uint16_t today = today_day_index();
    uint16_t oldest = (uint16_t)(today >= HM_DAYS - 1 ? today - (HM_DAYS - 1) : 0);
    if (btn == BSP_BTN_UP) {
        if (s_app.view_day > oldest) {
            s_app.view_day--;
            rebuild_heatmap();
        }
    } else if (btn == BSP_BTN_DOWN) {
        if (s_app.view_day < today) {
            s_app.view_day++;
            rebuild_heatmap();
        }
    }
}

static void handle_timeset_input(bsp_btn_t btn, bsp_btn_ev_t ev) {
    if (btn == BSP_BTN_OK) {
        if (ev == BSP_BTN_CLICK) {
            save_timeset();
            go_home();
        } else if (ev == BSP_BTN_LONG && !s_app.timeset_forced) {
            go_home();
        }
        return;
    }
    if (btn == BSP_BTN_UP && ev == BSP_BTN_CLICK) {
        s_app.timeset_min = (s_app.timeset_min + 5U) % 1440U;
        app_ui_timeset_update(s_app.timeset_min);
    } else if (btn == BSP_BTN_DOWN && ev == BSP_BTN_CLICK) {
        s_app.timeset_min = (s_app.timeset_min + 1435U) % 1440U;
        app_ui_timeset_update(s_app.timeset_min);
    } else if (btn == BSP_BTN_UP && ev == BSP_BTN_LONG) {
        s_app.timeset_min = (s_app.timeset_min + 60U) % 1440U;
        app_ui_timeset_update(s_app.timeset_min);
    } else if (btn == BSP_BTN_DOWN && ev == BSP_BTN_LONG) {
        enter_wifi_page();
    }
}

// 在持有 bsp_lvgl_lock 时调用
static void handle_input(bsp_btn_t btn, bsp_btn_ev_t ev) {
    switch (s_app.page) {
    case PAGE_HOME:
        handle_home_input(btn, ev);
        break;
    case PAGE_COUNTDOWN:
        if (ev == BSP_BTN_LONG && btn == BSP_BTN_OK) {
            if (focus_model_abort(&s_app.model)) go_home();
        }
        break;
    case PAGE_CELEBRATE:
        if (ev == BSP_BTN_CLICK || ev == BSP_BTN_DOUBLE || ev == BSP_BTN_LONG) {
            if (focus_model_skip_celebrate(&s_app.model)) go_home();
        }
        break;
    case PAGE_HEATMAP:
        handle_heatmap_input(btn, ev);
        break;
    case PAGE_TIMESET:
        handle_timeset_input(btn, ev);
        break;
    case PAGE_WIFI:
        if (btn == BSP_BTN_OK && (ev == BSP_BTN_CLICK || ev == BSP_BTN_LONG)) {
            enqueue_work(WORK_WIFI_STOP);
            go_home();
        } else if (btn == BSP_BTN_DOWN && ev == BSP_BTN_LONG) {
            enqueue_work(WORK_WIFI_STOP);
            enqueue_work(WORK_WIFI_PROVISION);
        }
        break;
    }
}

// button 回调跑在共享 esp_timer 任务:只入队,立即返回。
static void on_key(bsp_btn_t btn, bsp_btn_ev_t ev, void *user) {
    (void)user;
    if (!s_input_ready || !s_input_queue) return;
    const input_event_t input = { .btn = btn, .event = ev };
    (void)xQueueSend(s_input_queue, &input, 0);
}

static void input_task(void *arg) {
    (void)arg;
    input_event_t input;
    for (;;) {
        if (xQueueReceive(s_input_queue, &input, portMAX_DELAY) == pdTRUE) {
            if (bsp_lvgl_lock(500)) {
                handle_input(input.btn, input.event);
                bsp_lvgl_unlock();
            }
        }
    }
}

// ---------------------------------------------------------------------------
// 工作任务:NVS 写入(热力图 blob 由 tick 序列化好,这里只落盘)
// ---------------------------------------------------------------------------
static void worker_task(void *arg) {
    (void)arg;
    work_item_t item;
    for (;;) {
        if (xQueueReceive(s_app.work_queue, &item, portMAX_DELAY) != pdTRUE) continue;
        if (item.type == WORK_SAVE_HM && item.blob) {
            app_store_save_heatmap(item.blob, HM_BLOB_SIZE);
            free(item.blob);
        } else if (item.type == WORK_SAVE_CLOCK) {
            app_store_save_clock(item.a, item.b);
        } else if (item.type == WORK_WIFI_STA) {
            app_wifi_start_sta();
        } else if (item.type == WORK_WIFI_PROVISION) {
            app_wifi_start_provision();
        } else if (item.type == WORK_WIFI_STOP) {
            app_wifi_stop();
        }
    }
}

// ---------------------------------------------------------------------------
// LVGL tick(在 LVGL 任务内运行,免锁)
// ---------------------------------------------------------------------------
static void app_tick(lv_timer_t *timer) {
    (void)timer;
    uint64_t now = now_ms();
    app_ui_tick();

    // ---- Wi-Fi 守护与对时 ----
    app_wifi_tick(now);
    uint64_t utc = 0;
    if (app_wifi_poll_sync(&utc)) {
        uint64_t local = utc + APP_TZ_OFFSET_SEC;
        uint64_t new_epoch = local - now / 1000ULL;
        ESP_LOGI(TAG, "SNTP 对时: 校正 %lld 秒",
                 (long long)(new_epoch - s_app.boot_epoch));
        s_app.boot_epoch = new_epoch;
        s_app.clock_saved_ms = now;
        if (s_app.work_queue) {
            work_item_t item = { .type = WORK_SAVE_CLOCK, .a = local, .b = now };
            xQueueSend(s_app.work_queue, &item, 0);
        }
        enqueue_work(WORK_WIFI_STOP);       // 对完即停,省电
        if (s_app.page == PAGE_TIMESET) {
            go_home();                      // 时钟已准,无需再手设
        } else if (s_app.page == PAGE_HOME) {
            app_ui_toast("对时成功");
        }
    }
    app_wifi_state_t st = app_wifi_state();
    if (st != s_last_wifi_state) {
        s_last_wifi_state = st;
        if (s_app.page == PAGE_WIFI) wifi_page_refresh();
    }
    if (s_app.page == PAGE_WIFI && st == APP_WIFI_SYNCED) {
        if (!s_wifi_done_at) s_wifi_done_at = now + 1200;
        else if (now >= s_wifi_done_at) go_home();
    }

    // ---- 番茄钟状态机 ----
    focus_event_t ev = focus_model_tick(&s_app.model, now);
    if (ev == FOCUS_EVENT_WORK_DONE) {
        hm_day_t *day = hm_day_acquire(&s_app.hm, today_day_index());
        day->work_sessions++;
        queue_save_heatmap();
        app_audio_play_chime();
        s_app.page = PAGE_CELEBRATE;
        app_ui_show_celebrate(day->work_sessions);
        return;
    }
    if (ev == FOCUS_EVENT_REST_DONE) {
        go_home_with_toast("休息完毕");
        return;
    }
    if (ev == FOCUS_EVENT_CELEBRATE_DONE) {
        go_home();
        return;
    }

    if (s_app.page == PAGE_COUNTDOWN) {
        uint32_t remain = focus_model_remaining_sec(&s_app.model, now);
        int32_t finish_min = -1;
        if (s_app.boot_epoch && remain > 0) {
            uint64_t finish_wall = s_app.boot_epoch +
                                   (now + (uint64_t)remain * 1000ULL) / 1000ULL;
            finish_min = (int32_t)app_clock_minute_of_day(finish_wall);
        }
        app_ui_countdown_update(remain, s_app.model.duration_sec, finish_min);
        if (remain > 0) {
            update_marking(now, s_app.model.state == FOCUS_STATE_WORK_RUNNING
                                    ? HM_KIND_WORK : HM_KIND_REST);
        }
    }

    if (s_app.boot_epoch &&
        now - s_app.clock_saved_ms >= CLOCK_SAVE_INTERVAL_MS) {
        if (s_app.work_queue) {
            work_item_t item = { .type = WORK_SAVE_CLOCK,
                                 .a = s_app.boot_epoch + now / 1000ULL, .b = now };
            xQueueSend(s_app.work_queue, &item, 0);
        }
        s_app.clock_saved_ms = now;
    }

    static uint64_t s_last_batt_ms;
    if (now - s_last_batt_ms >= BATTERY_INTERVAL_MS) {
        s_last_batt_ms = now;
        app_ui_set_battery(bsp_battery_soc());
    }
}

// ---------------------------------------------------------------------------
// 启动
// ---------------------------------------------------------------------------
void app_main(void) {
    ESP_LOGI(TAG, "番茄专注 启动(工作 %u 分钟 / 休息 %u 分钟)",
             (unsigned)(FOCUS_WORK_SEC / 60U), (unsigned)(FOCUS_REST_SEC / 60U));

    memset(&s_app, 0, sizeof(s_app));
    focus_model_init(&s_app.model, 0);
    s_app.anchor_day = build_day_index();
    ESP_LOGI(TAG, "日期锚点(固件构建日): day %u", (unsigned)s_app.anchor_day);

    if (!app_store_init()) {
        ESP_LOGW(TAG, "NVS 不可用,记录与时间不会被保存");
    }
    uint64_t saved_epoch = 0, saved_boot_ms = 0;
    if (app_store_load_clock(&saved_epoch, &saved_boot_ms)) {
        // 软重启:误差 <= 30 秒续存间隔;断电重启:需重新对时。
        s_app.boot_epoch = saved_epoch;
        ESP_LOGI(TAG, "恢复时钟: epoch=%llu (上次保存于运行 %llu 秒)",
                 (unsigned long long)saved_epoch,
                 (unsigned long long)saved_boot_ms / 1000ULL);
    }
    app_store_load_heatmap(&s_app.hm);

    if (bsp_display_init() != ESP_OK || !bsp_lvgl_init()) {
        ESP_LOGE(TAG, "显示/LVGL 初始化失败,无法继续。"
                      "检查 SPI 接线(MOSI=%d SCLK=%d CS=%d DC=%d BL=%d)",
                 BSP_LCD_MOSI, BSP_LCD_SCLK, BSP_LCD_CS, BSP_LCD_DC, BSP_LCD_BL);
        return;
    }
    bsp_display_backlight(100);

    // 电量计先就绪,首屏右上角就能显示真实电量。
    if (bsp_battery_init() != ESP_OK) {
        ESP_LOGW(TAG, "电量计不可用,右上角显示 -- ");
    }

    if (bsp_lvgl_lock(1000)) {
        app_ui_init();
        if (s_app.boot_epoch) {
            s_app.page = PAGE_HOME;
        } else {
            enter_timeset();        // 从未对时:开机先设置时间
        }
        app_ui_set_battery(bsp_battery_soc());
        bsp_lvgl_unlock();
    }

    app_audio_init();

    s_input_queue = xQueueCreate(INPUT_QUEUE_DEPTH, sizeof(input_event_t));
    if (s_input_queue &&
        xTaskCreate(input_task, "focus_input", 3072, NULL, 5, NULL) == pdPASS) {
        esp_err_t err = bsp_button_init(on_key, NULL);
        if (err != ESP_OK) ESP_LOGE(TAG, "按键初始化失败: %s", esp_err_to_name(err));
    } else {
        ESP_LOGE(TAG, "输入任务创建失败");
    }

    s_app.work_queue = xQueueCreate(WORK_QUEUE_DEPTH, sizeof(work_item_t));
    if (s_app.work_queue) {
        if (xTaskCreate(worker_task, "focus_work", 4096, NULL, 4, NULL) != pdPASS) {
            ESP_LOGE(TAG, "工作任务创建失败,记录不会落盘");
        }
    }

    // Wi-Fi 自动对时:有凭据开机即连;无凭据在时间设置页长按 DOWN 配网。
    if (app_wifi_init()) {
        ESP_LOGI(TAG, "已存 Wi-Fi 凭据,后台自动对时");
        enqueue_work(WORK_WIFI_STA);
    } else {
        ESP_LOGI(TAG, "无 Wi-Fi 凭据,可在时间设置页长按 DOWN 配网");
    }

    lv_timer_create(app_tick, TICK_PERIOD_MS, NULL);
    s_input_ready = true;
    ESP_LOGI(TAG, "就绪");
}
