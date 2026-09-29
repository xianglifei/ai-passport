// main/main.c —— GLM 额度:智谱 Coding Plan 配额监视(5 小时窗口 + 周限额)。
//
// 单屏仪表盘:两张限额卡(剩余/比例条/重置时刻/倒计时)+ 页脚轮询状态 +
// 标题行套餐徽章与网络状态点。每 5 分钟自动拉取一次,连上 Wi-Fi 与对时成功
// 后立即拉取;拉取失败 60 秒后重试,Wi-Fi 掉线自动重连(30 秒周期)。
//
// 按键语义:
//   仪表盘  OK 单击 = 立即更新;OK 长按 = 配网设置(热点 + 表单:Wi-Fi/Key)
//   配网页  OK = 返回;长按 DOWN = 重开热点
//
// 线程模型:按键回调只入队;input 任务持 bsp_lvgl_lock 处理输入;
// app_tick 跑在 LVGL 任务内(免锁);Wi-Fi/HTTPS/NVS 在 work 任务
// (8KB 栈,TLS 握手开销大)。拉取结果经标志位交给 app_tick 上屏。
//
// 时间:开机连 Wi-Fi + SNTP(UTC+8 折算本地);配额的重置时刻是绝对时间戳,
// 没有准确时钟就显示占位,不做手动对时(本应用数据本来就依赖网络)。
#include "app_store.h"
#include "app_ui.h"
#include "app_wifi.h"
#include "bsp_battery.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "bsp_pins.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "quota_fetch.h"
#include "quota_model.h"
#include <stdio.h>
#include <string.h>

// 本机私有默认 Key(可选):main/credentials.h 被 .gitignore 排除,不存在时为空。
#if __has_include("credentials.h")
#include "credentials.h"
#endif
#ifndef GLM_API_KEY_DEFAULT
#define GLM_API_KEY_DEFAULT ""
#endif

static const char *TAG = "quota_app";

#define TICK_PERIOD_MS 250
#define CLOCK_SAVE_INTERVAL_MS 30000
#define BATTERY_INTERVAL_MS 30000
#define POLL_INTERVAL_MS (5U * 60U * 1000U)   // 正常轮询周期
#define FETCH_RETRY_MS (60U * 1000U)          // 拉取失败后的重试间隔
#define WIFI_RETRY_MS (30U * 1000U)           // Wi-Fi 彻底失败后的重连周期
#define INPUT_QUEUE_DEPTH 8
#define WORK_QUEUE_DEPTH 8
#define KEY_MAX_LEN 96
// 设备无联网前不设本地时间;SNTP 对时结果按 UTC+8 折算(与 app_wifi 约定一致)。
#define APP_TZ_OFFSET_SEC (8 * 3600)

typedef enum {
    PAGE_DASH = 0,
    PAGE_SETUP,
} app_page_t;

typedef enum {
    WORK_WIFI_STA = 1,      // 用已存凭据连 Wi-Fi
    WORK_WIFI_PROVISION,    // 开配网热点
    WORK_WIFI_STOP,         // 停止 Wi-Fi/SNTP
    WORK_FETCH,             // 拉取一次配额
} work_type_t;

typedef struct {
    work_type_t type;
} work_item_t;

typedef struct {
    bsp_btn_t btn;
    bsp_btn_ev_t event;
} input_event_t;

typedef struct {
    uint64_t boot_epoch;        // 本地墙钟秒;0 = 尚未 SNTP 对时
    uint64_t clock_saved_ms;    // 上次续存时的单调毫秒

    app_page_t page;

    QueueHandle_t work_queue;

    char api_key[KEY_MAX_LEN];

    // 拉取调度与结果(worker 写结果,app_tick 消费;单写单读,标志位交付)
    bool fetch_inflight;
    uint64_t next_fetch_ms;     // 单调毫秒;0 = 尚未排期
    bool result_ready;          // worker 写完一批结果
    bool result_ok;
    quota_data_t result;
    bool result_live;           // result 是实时数据(否则为开机快照,显示灰色)
    char fetch_err[48];
    bool last_ok;               // 上一次拉取是否成功(页脚样式)
    uint64_t fetched_wall;      // 最近一次成功拉取的本地墙钟秒
    uint64_t wifi_retry_at_ms;  // Wi-Fi FAILED 后下次重连时刻;0=未排期
    uint64_t setup_done_at;     // 配网成功后自动回仪表盘的时刻;0=未定时
} app_t;

static app_t s_app;
static QueueHandle_t s_input_queue;
static volatile bool s_input_ready;
static app_wifi_state_t s_last_wifi_state = APP_WIFI_IDLE;

static uint64_t now_ms(void) {
    return (uint64_t)esp_timer_get_time() / 1000ULL;
}

static uint64_t wall_sec(void) {
    return s_app.boot_epoch ? s_app.boot_epoch + now_ms() / 1000ULL : 0;
}

static void enqueue_work(work_type_t type) {
    if (!s_app.work_queue) return;
    work_item_t item = { .type = type };
    xQueueSend(s_app.work_queue, &item, 0);
}

// ---------------------------------------------------------------------------
// 页面
// ---------------------------------------------------------------------------
static app_ui_net_t map_net(app_wifi_state_t st);

static void go_dash(void) {
    // Key 可能在配网页刚被写入,每次回仪表盘前重读(单键 NVS 读,开销微小)。
    app_store_load_key(s_app.api_key, sizeof(s_app.api_key));
    bool has_key = s_app.api_key[0] != 0;
    s_app.page = PAGE_DASH;
    app_ui_show_dash(has_key);
    if (s_app.result.valid) {
        // 切页返回:重绘最近一次数据(快照灰显,实时数据带主题色)。
        app_ui_dash_set_quota(&s_app.result, !s_app.result_live);
    }
    app_ui_dash_set_net(map_net(app_wifi_state()));
    // 配网页可能刚换了 Key/刚连上网:回仪表盘即安排一次拉取(在线才会真正发出)。
    if (has_key) s_app.next_fetch_ms = now_ms();
}

static void enter_setup(void) {
    s_app.page = PAGE_SETUP;
    s_last_wifi_state = APP_WIFI_IDLE;
    s_app.setup_done_at = 0;
    app_ui_show_setup("正在开启热点...", "", "", "OK 返回 · 长按 DOWN 重开热点");
    enqueue_work(WORK_WIFI_STOP);
    enqueue_work(WORK_WIFI_PROVISION);
}

// ---------------------------------------------------------------------------
// 网络状态 → 状态点
// ---------------------------------------------------------------------------
static app_ui_net_t map_net(app_wifi_state_t st) {
    switch (st) {
    case APP_WIFI_CONNECTED: return APP_UI_NET_ONLINE;
    case APP_WIFI_CONNECTING: return APP_UI_NET_CONNECTING;
    case APP_WIFI_AP_MODE:
    case APP_WIFI_AP_CONNECTED: return APP_UI_NET_AP;
    default: return APP_UI_NET_OFFLINE;
    }
}

// 页脚一行:最近更新时刻 + 下次轮询倒计时 / 失败原因 / 等待联网。
static void footer_refresh(app_wifi_state_t st) {
    if (s_app.page != PAGE_DASH) return;
    char text[64];

    if (st == APP_WIFI_AP_MODE || st == APP_WIFI_AP_CONNECTED) {
        app_ui_dash_set_footer("配网热点已开启", false);
        return;
    }
    if (st == APP_WIFI_FAILED) {
        app_ui_dash_set_footer("Wi-Fi 未连接,稍后重连", true);
        return;
    }
    if (st != APP_WIFI_CONNECTED) {
        app_ui_dash_set_footer("等待联网...", false);
        return;
    }
    if (s_app.fetch_inflight) {
        app_ui_dash_set_footer("正在更新...", false);
        return;
    }
    if (!s_app.last_ok && s_app.fetch_err[0]) {
        snprintf(text, sizeof(text), "失败:%s", s_app.fetch_err);
        app_ui_dash_set_footer(text, true);
        return;
    }
    if (!s_app.boot_epoch) {
        app_ui_dash_set_footer("等待联网...", false);
        return;
    }
    if (!s_app.fetched_wall) {
        app_ui_dash_set_footer("已连接,等待更新", false);
        return;
    }
    uint32_t t = s_app.fetched_wall % 86400U;
    uint64_t remain_ms = s_app.next_fetch_ms > now_ms()
                             ? s_app.next_fetch_ms - now_ms() : 0;
    uint32_t next_s = (uint32_t)(remain_ms / 1000ULL);
    snprintf(text, sizeof(text), "更新 %02u:%02u · 下次 %02u:%02u",
             (unsigned)(t / 3600U), (unsigned)((t % 3600U) / 60U),
             (unsigned)(next_s / 60U), (unsigned)(next_s % 60U));
    app_ui_dash_set_footer(text, false);
}

// 请求一次拉取(仪表盘 OK / 调度到期共用);不在线时给出提示。
static void request_fetch(bool manual) {
    if (!s_app.api_key[0]) {
        if (manual) app_ui_toast("未配置 API Key");
        return;
    }
    if (app_wifi_state() != APP_WIFI_CONNECTED) {
        if (manual) app_ui_toast("Wi-Fi 未连接");
        return;
    }
    if (s_app.fetch_inflight) return;
    s_app.fetch_inflight = true;
    s_app.fetch_err[0] = 0;
    enqueue_work(WORK_FETCH);
    footer_refresh(APP_WIFI_CONNECTED);
}

// worker 的拉取结果上屏(在 LVGL 任务内调用)。
static void consume_fetch_result(void) {
    if (!s_app.result_ready) return;
    s_app.result_ready = false;
    s_app.fetch_inflight = false;
    if (s_app.result_ok) {
        s_app.last_ok = true;
        s_app.fetched_wall = wall_sec();
        s_app.next_fetch_ms = now_ms() + POLL_INTERVAL_MS;
        quota_data_t d = s_app.result;
        s_app.result_live = true;
        app_store_save_quota(&d, s_app.fetched_wall);
        app_ui_dash_set_quota(&d, false);
    } else {
        s_app.last_ok = false;
        s_app.next_fetch_ms = now_ms() + FETCH_RETRY_MS;
        ESP_LOGW(TAG, "拉取失败: %s", s_app.fetch_err);
    }
    footer_refresh(app_wifi_state());
}

// ---------------------------------------------------------------------------
// 输入
// ---------------------------------------------------------------------------
static void handle_dash_input(bsp_btn_t btn, bsp_btn_ev_t ev) {
    if (ev != BSP_BTN_CLICK && ev != BSP_BTN_LONG) return;
    if (btn == BSP_BTN_OK) {
        if (ev == BSP_BTN_CLICK) request_fetch(true);
        else enter_setup();
    }
}

static void handle_setup_input(bsp_btn_t btn, bsp_btn_ev_t ev) {
    if (btn == BSP_BTN_OK && (ev == BSP_BTN_CLICK || ev == BSP_BTN_LONG)) {
        // 只有热点还开着才需要收摊;正在连接/已在线时保持不动(联网是核心功能)。
        app_wifi_state_t st = app_wifi_state();
        if (st == APP_WIFI_AP_MODE || st == APP_WIFI_AP_CONNECTED) {
            enqueue_work(WORK_WIFI_STOP);
            if (app_wifi_has_creds()) enqueue_work(WORK_WIFI_STA);
        }
        go_dash();      // 内部会重读 Key,配网页写入的新 Key 直接生效
    } else if (btn == BSP_BTN_DOWN && ev == BSP_BTN_LONG) {
        enqueue_work(WORK_WIFI_STOP);
        enqueue_work(WORK_WIFI_PROVISION);
    }
}

// 在持有 bsp_lvgl_lock 时调用
static void handle_input(bsp_btn_t btn, bsp_btn_ev_t ev) {
    switch (s_app.page) {
    case PAGE_DASH: handle_dash_input(btn, ev); break;
    case PAGE_SETUP: handle_setup_input(btn, ev); break;
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
// 工作任务:Wi-Fi 状态机与 HTTPS 拉取都在这里(阻塞 IO 不碰 LVGL)
// ---------------------------------------------------------------------------
static void worker_task(void *arg) {
    (void)arg;
    work_item_t item;
    for (;;) {
        if (xQueueReceive(s_app.work_queue, &item, portMAX_DELAY) != pdTRUE) continue;
        switch (item.type) {
        case WORK_WIFI_STA:
            app_wifi_start_sta();
            break;
        case WORK_WIFI_PROVISION:
            app_wifi_start_provision();
            break;
        case WORK_WIFI_STOP:
            app_wifi_stop();
            break;
        case WORK_FETCH: {
            quota_data_t d;
            char err[48] = {0};
            bool ok = quota_fetch_now(s_app.api_key, &d, err, sizeof(err));
            if (ok) {
                s_app.result = d;       // 失败时保留上次好数据,不覆盖
            }
            strlcpy(s_app.fetch_err, err, sizeof(s_app.fetch_err));
            s_app.result_ok = ok;
            s_app.result_ready = true;      // app_tick 消费后清 inflight
            break;
        }
        }
    }
}

// ---------------------------------------------------------------------------
// 配网设置页状态文案(app_tick 轮询刷新)
// ---------------------------------------------------------------------------
static void setup_page_refresh(void) {
    const char *l1 = "", *l2 = "", *l3 = "";
    switch (app_wifi_state()) {
    case APP_WIFI_AP_MODE:
        l1 = "等待手机连接热点:";
        l2 = app_wifi_ap_name();
        l3 = "手机浏览器打开 192.168.4.1";
        break;
    case APP_WIFI_AP_CONNECTED:
        l1 = "已连接热点";
        l2 = app_wifi_ap_name();
        l3 = "手机浏览器打开 192.168.4.1";
        break;
    case APP_WIFI_CONNECTING:
        l1 = "正在连接 Wi-Fi...";
        l2 = app_wifi_ssid();
        break;
    case APP_WIFI_CONNECTED:
        l1 = "已连接";
        l2 = app_wifi_ssid();
        break;
    case APP_WIFI_FAILED:
        l1 = "连接失败,可重试";
        l2 = "长按 DOWN 重开热点";
        break;
    default:
        l1 = "Wi-Fi 未连接";
        break;
    }
    app_ui_setup_update(l1, l2, l3);
}

// ---------------------------------------------------------------------------
// LVGL tick(在 LVGL 任务内运行,免锁)
// ---------------------------------------------------------------------------
static void app_tick(lv_timer_t *timer) {
    (void)timer;
    uint64_t now = now_ms();
    static uint64_t s_last_sec_tick;
    app_ui_tick();

    // ---- Wi-Fi:对时事件 / 状态变化 / 守护 ----
    app_wifi_tick(now);
    uint64_t utc = 0;
    if (app_wifi_poll_sync(&utc)) {
        uint64_t local = utc + APP_TZ_OFFSET_SEC;
        uint64_t new_epoch = local - now / 1000ULL;
        ESP_LOGI(TAG, "SNTP 对时: 校正 %lld 秒",
                 (long long)(new_epoch - s_app.boot_epoch));
        s_app.boot_epoch = new_epoch;
        s_app.clock_saved_ms = now;
        app_store_save_clock(local, now);
        // 时钟就绪:重置时刻可以显示了;并对时完尽快拉一次。
        if (s_app.page == PAGE_DASH) app_ui_dash_tick(wall_sec());
        if (!s_app.next_fetch_ms || !s_app.fetched_wall) {
            s_app.next_fetch_ms = now;      // 到期即拉
        }
    }
    app_wifi_state_t st = app_wifi_state();
    if (st != s_last_wifi_state) {
        s_last_wifi_state = st;
        app_ui_dash_set_net(map_net(st));
        footer_refresh(st);
        if (s_app.page == PAGE_SETUP) setup_page_refresh();
        if (st == APP_WIFI_CONNECTED && s_app.next_fetch_ms == 0) {
            s_app.next_fetch_ms = now;      // 首次上线:立即拉取
        }
        if (st == APP_WIFI_FAILED) {
            s_app.wifi_retry_at_ms = now + WIFI_RETRY_MS;
        }
    }
    if (st == APP_WIFI_FAILED && s_app.wifi_retry_at_ms &&
        now >= s_app.wifi_retry_at_ms) {
        s_app.wifi_retry_at_ms = 0;
        enqueue_work(WORK_WIFI_STA);
    }

    // ---- 拉取调度 ----
    consume_fetch_result();
    if (!s_app.fetch_inflight && s_app.api_key[0] && s_app.next_fetch_ms &&
        now >= s_app.next_fetch_ms && s_app.page != PAGE_SETUP) {
        request_fetch(false);
    }

    // ---- 每秒:相对文案(倒计时/页脚下次倒计时) ----
    if (now - s_last_sec_tick >= 1000) {
        s_last_sec_tick = now;
        if (s_app.page == PAGE_DASH && s_app.boot_epoch) {
            app_ui_dash_tick(wall_sec());
        }
        if (s_app.page == PAGE_DASH && st == APP_WIFI_CONNECTED &&
            s_app.fetched_wall && !s_app.fetch_inflight && s_app.last_ok) {
            footer_refresh(st);
        }
    }

    // ---- 配网成功自动回仪表盘(保持在线,不停 Wi-Fi) ----
    if (s_app.page == PAGE_SETUP && st == APP_WIFI_CONNECTED &&
        s_app.boot_epoch) {
        if (!s_app.setup_done_at) {
            s_app.setup_done_at = now + 1200;
        } else if (now >= s_app.setup_done_at) {
            s_app.setup_done_at = 0;
            go_dash();
        }
    }

    if (s_app.boot_epoch && now - s_app.clock_saved_ms >= CLOCK_SAVE_INTERVAL_MS) {
        app_store_save_clock(s_app.boot_epoch + now / 1000ULL, now);
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
    ESP_LOGI(TAG, "GLM 额度 启动(每 %u 分钟拉取一次套餐限额)",
             (unsigned)(POLL_INTERVAL_MS / 60000U));

    memset(&s_app, 0, sizeof(s_app));

    if (!app_store_init()) {
        ESP_LOGW(TAG, "NVS 不可用,凭据与快照不会被保存");
    }
    uint64_t saved_epoch = 0, saved_boot_ms = 0;
    if (app_store_load_clock(&saved_epoch, &saved_boot_ms)) {
        s_app.boot_epoch = saved_epoch;
        ESP_LOGI(TAG, "恢复时钟: epoch=%llu", (unsigned long long)saved_epoch);
    }
    app_store_load_key(s_app.api_key, sizeof(s_app.api_key));
    // 本机内置默认 Key(credentials.h,gitignored;无此文件时为空串,块被优化掉):
    // 覆盖空值或已知无效的测试 Key,写入 NVS 持久化(一次性迁移)。
    static const char *const TEST_KEY = "0123456789abcdef0123456789abcdef.ABCD1234";
    if (GLM_API_KEY_DEFAULT[0] &&
        (!s_app.api_key[0] || strcmp(s_app.api_key, TEST_KEY) == 0)) {
        strlcpy(s_app.api_key, GLM_API_KEY_DEFAULT, sizeof(s_app.api_key));
        app_store_save_key(s_app.api_key);
        ESP_LOGI(TAG, "写入内置默认 Key(长度 %d)", (int)strlen(s_app.api_key));
    }
    quota_data_t snapshot = {0};
    uint64_t snapshot_wall = 0;
    if (app_store_load_quota(&snapshot, &snapshot_wall)) {
        s_app.result = snapshot;
        s_app.fetched_wall = snapshot_wall;
        ESP_LOGI(TAG, "载入配额快照");
    }

    // Wi-Fi 子系统先于 UI 决策初始化:开机进仪表盘还是配网页要看已存凭据。
    bool has_wifi_creds = app_wifi_init();
    ESP_LOGI(TAG, "%s Wi-Fi 凭据", has_wifi_creds ? "已存" : "无");

    if (bsp_display_init() != ESP_OK || !bsp_lvgl_init()) {
        ESP_LOGE(TAG, "显示/LVGL 初始化失败,无法继续。"
                      "检查 SPI 接线(MOSI=%d SCLK=%d CS=%d DC=%d BL=%d)",
                 BSP_LCD_MOSI, BSP_LCD_SCLK, BSP_LCD_CS, BSP_LCD_DC, BSP_LCD_BL);
        return;
    }
    bsp_display_backlight(100);

    if (bsp_battery_init() != ESP_OK) {
        ESP_LOGW(TAG, "电量计不可用,右上角显示 -- ");
    }

    if (bsp_lvgl_lock(1000)) {
        app_ui_init();
        bool has_key = s_app.api_key[0] != 0;
        if (has_wifi_creds && has_key) {
            s_app.page = PAGE_DASH;
            app_ui_show_dash(has_key);
            if (snapshot.valid) {
                app_ui_dash_set_quota(&snapshot, true);
            }
        } else {
            enter_setup();              // 凭据或 Key 缺失:开机即配网
        }
        app_ui_set_battery(bsp_battery_soc());
        bsp_lvgl_unlock();
    }

    s_input_queue = xQueueCreate(INPUT_QUEUE_DEPTH, sizeof(input_event_t));
    if (s_input_queue &&
        xTaskCreate(input_task, "quota_input", 3072, NULL, 5, NULL) == pdPASS) {
        esp_err_t err = bsp_button_init(on_key, NULL);
        if (err != ESP_OK) ESP_LOGE(TAG, "按键初始化失败: %s", esp_err_to_name(err));
    } else {
        ESP_LOGE(TAG, "输入任务创建失败");
    }

    s_app.work_queue = xQueueCreate(WORK_QUEUE_DEPTH, sizeof(work_item_t));
    if (s_app.work_queue) {
        if (xTaskCreate(worker_task, "quota_work", 8192, NULL, 4, NULL) != pdPASS) {
            ESP_LOGE(TAG, "工作任务创建失败,无法联网取数");
        }
    }

    // Wi-Fi 常驻:有凭据开机即连并对时;本应用联网就是核心功能,连上不停。
    if (has_wifi_creds) {
        enqueue_work(WORK_WIFI_STA);
    }

    // 开机无凭据/无 Key 时 enter_setup 只切了页;队列此时才就绪,补发开热点。
    if (s_app.page == PAGE_SETUP) {
        enqueue_work(WORK_WIFI_STOP);
        enqueue_work(WORK_WIFI_PROVISION);
    }

    lv_timer_create(app_tick, TICK_PERIOD_MS, NULL);
    s_input_ready = true;
    ESP_LOGI(TAG, "就绪");
}
