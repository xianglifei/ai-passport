// main/main.c —— 双应用:GLM 额度监视 + 番茄钟(简化版),上下键翻页。
//
// 额度页(默认):两张限额卡(已用百分比/比例条/重置时刻)+ 页脚轮询状态 +
// 标题行状态点(网络 + 番茄后台计时)。每 5 分钟拉取,失败 60 秒重试,
// 断线自动重连(30 秒周期),在线常驻。
// 番茄页:↑ 直接开始工作 25 分,↓ 直接开始休息 10 分,OK 单击=开始工作;
// 倒计时中长按 OK 放弃(不入库);长按 ↑/↓ 翻页,计时后台继续。
// 完成的会话记录真 UTC 毫秒时间戳,NVS 暂存,在线批量上报 Let's farm
// (/sync,幂等去重),上报成功才清本地;放弃的会话不记录。
//
// 按键语义(上下键优先服务当前页;额度页无页内用途,单击即翻页):
//   额度页  ↑/↓ 单击或长按 = 番茄页;OK 单击 = 立即更新;OK 长按 = 配网
//   番茄页  ↑ = 工作 / ↓ = 休息 / OK = 工作(空闲);长按 OK = 放弃(倒计时);
//           ↑/↓/OK 单击 = 开始休息(完成横幅);长按 ↑/↓ = 翻页(全状态)
//   配网页  OK = 返回;长按 DOWN = 重开热点
//
// 线程模型:按键回调只入队;input 任务持 bsp_lvgl_lock 处理输入;
// app_tick 跑在 LVGL 任务内(免锁);Wi-Fi/HTTPS/NVS/上报在 work 任务
// (8KB 栈)。配额拉取与番茄上报各自带标志位交付结果。
#include "app_audio.h"
#include "app_clock.h"
#include "app_store.h"
#include "app_ui.h"
#include "app_wifi.h"
#include "bsp_battery.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "bsp_pins.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "focus_model.h"
#include "focus_store.h"
#include "focus_sync.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "quota_fetch.h"
#include "quota_model.h"
#include <stdio.h>
#include <string.h>

// 本机私有凭据(可选):main/credentials.h 被 .gitignore 排除,不存在时为空。
#if __has_include("credentials.h")
#include "credentials.h"
#endif
#ifndef GLM_API_KEY_DEFAULT
#define GLM_API_KEY_DEFAULT ""
#endif
#ifndef GLM_SYNC_TOKEN_DEFAULT
#define GLM_SYNC_TOKEN_DEFAULT ""
#endif

static const char *TAG = "quota_app";

#define TICK_PERIOD_MS 250
#define CLOCK_SAVE_INTERVAL_MS 30000
#define BATTERY_INTERVAL_MS 30000
#define POLL_INTERVAL_MS (5U * 60U * 1000U)   // 配额正常轮询周期
#define FETCH_RETRY_MS (60U * 1000U)          // 拉取失败后的重试间隔
#define WIFI_RETRY_MS (30U * 1000U)           // Wi-Fi 彻底失败后的重连周期
#define SYNC_RETRY_MS (5U * 60U * 1000U)      // 会话上报失败后的重试间隔
#define FOCUS_SNAP_MAX 100                    // 单次上报批量(服务端上限 2000)
#define INPUT_QUEUE_DEPTH 8
#define WORK_QUEUE_DEPTH 8
#define KEY_MAX_LEN 96
#define TOKEN_MAX_LEN 160
// 设备无联网前不设本地时间;SNTP 对时结果按 UTC+8 折算(与 app_wifi 约定一致)。
#define APP_TZ_OFFSET_SEC (8 * 3600)

typedef enum {
    PAGE_DASH = 0,
    PAGE_FOCUS,
    PAGE_SETUP,
} app_page_t;

typedef enum {
    WORK_WIFI_STA = 1,      // 用已存凭据连 Wi-Fi
    WORK_WIFI_PROVISION,    // 开配网热点
    WORK_WIFI_STOP,         // 停止 Wi-Fi/SNTP
    WORK_FETCH,             // 拉取一次配额
    WORK_SYNC_UPLOAD,       // 上报一批番茄会话(快照在 s_app.focus_snap)
} work_type_t;

typedef struct {
    work_type_t type;
    uint16_t a;             // WORK_SYNC_UPLOAD:快照条数
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
    char sync_token[TOKEN_MAX_LEN];

    // 番茄:状态机 + 会话缓冲 + 上传快照(worker 只见快照,不碰主缓冲)
    focus_model_t model;
    focus_store_t focus;
    uint64_t session_start_wall;    // 本次会话起始本地墙钟秒;0=未在计时
    focus_rec_t focus_snap[FOCUS_SNAP_MAX];
    uint16_t focus_snap_n;
    bool sync_inflight;
    bool sync_ready;                // worker 写完一批上报结果
    focus_sync_result_t sync_result;
    uint64_t sync_retry_at_ms;      // 上报失败后下次重试;0=未排期

    // 配额拉取调度与结果(worker 写结果,app_tick 消费;标志位交付)
    bool fetch_inflight;
    uint64_t next_fetch_ms;     // 单调毫秒;0 = 尚未排期
    bool result_ready;
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
static void request_sync_upload(void);

// 番茄状态机 → UI 形态
static app_ui_focus_mode_t focus_ui_mode(void) {
    switch (s_app.model.state) {
    case FOCUS_STATE_WORK_RUNNING: return APP_UI_FOCUS_WORK;
    case FOCUS_STATE_REST_RUNNING: return APP_UI_FOCUS_REST;
    case FOCUS_STATE_CELEBRATE: return APP_UI_FOCUS_DONE;
    default: return APP_UI_FOCUS_IDLE;
    }
}

// 额度页标题行的番茄后台计时指示点(只在额度页可见)。
static void update_focus_dot(void) {
    bool running = s_app.model.state == FOCUS_STATE_WORK_RUNNING ||
                   s_app.model.state == FOCUS_STATE_REST_RUNNING;
    app_ui_dash_set_focus_dot(running,
                              s_app.model.state == FOCUS_STATE_WORK_RUNNING);
}

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
    update_focus_dot();
    // 配网页可能刚换了 Key/刚连上网:回仪表盘即安排一次拉取(在线才会真正发出)。
    if (has_key) s_app.next_fetch_ms = now_ms();
}

static void go_focus(void) {
    s_app.page = PAGE_FOCUS;
    app_ui_show_focus(focus_ui_mode(), s_app.focus.count);
}

// 开始一个番茄会话;时钟未同步也能计时,只是该会话不入库。
static void start_focus(bool work) {
    bool ok = work ? focus_model_start_work(&s_app.model, now_ms())
                   : focus_model_start_rest(&s_app.model, now_ms());
    if (!ok) return;
    s_app.session_start_wall = wall_sec();
    s_app.page = PAGE_FOCUS;
    app_ui_show_focus(work ? APP_UI_FOCUS_WORK : APP_UI_FOCUS_REST,
                      s_app.focus.count);
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

// 请求一次配额拉取(仪表盘 OK / 调度到期共用);不在线时给出提示。
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
// 番茄:会话入库与上报
// ---------------------------------------------------------------------------
// 完成的会话入库(只记录完整完成的;放弃不入库)。时间戳折算为真 UTC 毫秒,
// 与网页端同格式——设备墙钟是 UTC+8 口径,入库前减回。
static void record_session(uint8_t type, uint64_t start_wall) {
    uint64_t end_wall = wall_sec();
    if (!start_wall || !end_wall || end_wall <= start_wall) {
        ESP_LOGW(TAG, "时钟未同步,本次会话不入库");
        return;
    }
    uint64_t utc_start = (start_wall - APP_TZ_OFFSET_SEC) * 1000ULL;
    uint64_t utc_end = (end_wall - APP_TZ_OFFSET_SEC) * 1000ULL;
    if (!focus_store_add(&s_app.focus, type, utc_start, utc_end)) {
        ESP_LOGW(TAG, "会话缓冲已满(%u 条),本条丢弃",
                 (unsigned)FOCUS_STORE_CAP);
        return;
    }
    app_store_save_focus_buf(&s_app.focus);
    ESP_LOGI(TAG, "会话入库: type=%u 时长 %llu 秒, 待传 %u 条", type,
             (unsigned long long)(end_wall - start_wall),
             (unsigned)s_app.focus.count);
    request_sync_upload();
}

// 快照最旧的至多 FOCUS_SNAP_MAX 条交 worker;期间新增的留在缓冲下一批传。
// 只在 LVGL 任务内调用(单写者);worker 只读快照,不碰主缓冲。
static void request_sync_upload(void) {
    if (s_app.sync_inflight || s_app.focus.count == 0) return;
    s_app.focus_snap_n = s_app.focus.count < FOCUS_SNAP_MAX
                             ? s_app.focus.count
                             : FOCUS_SNAP_MAX;
    memcpy(s_app.focus_snap, s_app.focus.recs,
           (size_t)s_app.focus_snap_n * sizeof(focus_rec_t));
    s_app.sync_inflight = true;
    work_item_t item = { .type = WORK_SYNC_UPLOAD, .a = s_app.focus_snap_n };
    xQueueSend(s_app.work_queue, &item, 0);
}

// worker 的上报结果消费(在 LVGL 任务内调用);成功才清快照,失败择时重试。
static void consume_sync_result(void) {
    if (!s_app.sync_ready) return;
    s_app.sync_ready = false;
    s_app.sync_inflight = false;
    if (s_app.sync_result == FOCUS_SYNC_OK) {
        focus_store_drop_oldest(&s_app.focus, s_app.focus_snap_n);
        app_store_save_focus_buf(&s_app.focus);
        ESP_LOGI(TAG, "会话上报成功,剩余待传 %u 条", (unsigned)s_app.focus.count);
    } else {
        s_app.sync_retry_at_ms = now_ms() + SYNC_RETRY_MS;
        ESP_LOGW(TAG, "会话上报失败(%d),%u 条待重试",
                 (int)s_app.sync_result, (unsigned)s_app.focus.count);
    }
    if (s_app.page == PAGE_FOCUS) app_ui_focus_set_pending(s_app.focus.count);
}

// ---------------------------------------------------------------------------
// 输入
// ---------------------------------------------------------------------------
static void handle_dash_input(bsp_btn_t btn, bsp_btn_ev_t ev) {
    if (ev != BSP_BTN_CLICK && ev != BSP_BTN_LONG) return;
    if (btn == BSP_BTN_UP || btn == BSP_BTN_DOWN) {
        go_focus();                             // 额度页无页内用途:上下键即翻页
        return;
    }
    if (btn == BSP_BTN_OK) {
        if (ev == BSP_BTN_CLICK) request_fetch(true);
        else enter_setup();
    }
}

static void handle_focus_input(bsp_btn_t btn, bsp_btn_ev_t ev) {
    // 全状态通用:长按 ↑/↓ 翻回额度页,计时后台继续。
    if (ev == BSP_BTN_LONG && (btn == BSP_BTN_UP || btn == BSP_BTN_DOWN)) {
        go_dash();
        return;
    }
    focus_state_t st = s_app.model.state;
    if (st == FOCUS_STATE_HOME) {
        if (ev != BSP_BTN_CLICK) return;
        if (btn == BSP_BTN_UP) start_focus(true);
        else if (btn == BSP_BTN_DOWN) start_focus(false);
        else if (btn == BSP_BTN_OK) start_focus(true);      // OK=默认开始工作
    } else if (st == FOCUS_STATE_WORK_RUNNING ||
               st == FOCUS_STATE_REST_RUNNING) {
        if (btn == BSP_BTN_OK && ev == BSP_BTN_LONG) {
            focus_model_abort(&s_app.model);                // 放弃:不入库
            s_app.session_start_wall = 0;
            app_ui_show_focus(APP_UI_FOCUS_IDLE, s_app.focus.count);
        }
    } else if (st == FOCUS_STATE_CELEBRATE) {
        if (btn == BSP_BTN_OK && ev == BSP_BTN_LONG) {
            if (focus_model_skip_celebrate(&s_app.model)) {
                app_ui_show_focus(APP_UI_FOCUS_IDLE, s_app.focus.count);
            }
        } else if (ev == BSP_BTN_CLICK) {
            if (focus_model_skip_celebrate(&s_app.model)) start_focus(false);
        }
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
    case PAGE_FOCUS: handle_focus_input(btn, ev); break;
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
// 工作任务:Wi-Fi 状态机与 HTTPS(配额/上报)都在这里(阻塞 IO 不碰 LVGL)
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
        case WORK_SYNC_UPLOAD:
            s_app.sync_result = focus_sync_upload(
                s_app.sync_token, s_app.focus_snap, item.a);
            s_app.sync_ready = true;
            break;
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
        // 首次上线顺手把攒着的会话传上去。
        if (st == APP_WIFI_CONNECTED && s_app.focus.count) {
            request_sync_upload();
        }
    }
    if (st == APP_WIFI_FAILED && s_app.wifi_retry_at_ms &&
        now >= s_app.wifi_retry_at_ms) {
        s_app.wifi_retry_at_ms = 0;
        enqueue_work(WORK_WIFI_STA);
    }

    // ---- 配额拉取调度 ----
    consume_fetch_result();
    if (!s_app.fetch_inflight && s_app.api_key[0] && s_app.next_fetch_ms &&
        now >= s_app.next_fetch_ms && s_app.page != PAGE_SETUP) {
        request_fetch(false);
    }

    // ---- 番茄:状态机推进 / 会话入库 / 上报调度 / 页面刷新 ----
    focus_event_t fev = focus_model_tick(&s_app.model, now);
    if (fev == FOCUS_EVENT_WORK_DONE) {
        record_session(FOCUS_TYPE_WORK, s_app.session_start_wall);
        app_audio_play_chime();
        if (s_app.page == PAGE_FOCUS) {
            app_ui_show_focus(APP_UI_FOCUS_DONE, s_app.focus.count);
        }
    } else if (fev == FOCUS_EVENT_REST_DONE) {
        record_session(FOCUS_TYPE_REST, s_app.session_start_wall);
        s_app.session_start_wall = 0;
        if (s_app.page == PAGE_FOCUS) {
            app_ui_show_focus(APP_UI_FOCUS_IDLE, s_app.focus.count);
            app_ui_toast("休息完毕");
        }
    } else if (fev == FOCUS_EVENT_CELEBRATE_DONE) {
        if (s_app.page == PAGE_FOCUS) {
            app_ui_show_focus(APP_UI_FOCUS_IDLE, s_app.focus.count);
        }
    }
    if (s_app.page == PAGE_FOCUS &&
        (s_app.model.state == FOCUS_STATE_WORK_RUNNING ||
         s_app.model.state == FOCUS_STATE_REST_RUNNING)) {
        uint32_t remain = focus_model_remaining_sec(&s_app.model, now);
        int32_t finish_min = -1;
        if (s_app.boot_epoch && remain > 0) {
            uint64_t finish_wall =
                s_app.boot_epoch + (now + (uint64_t)remain * 1000ULL) / 1000ULL;
            finish_min = (int32_t)app_clock_minute_of_day(finish_wall);
        }
        app_ui_focus_update(remain, s_app.model.duration_sec, finish_min);
    }
    consume_sync_result();
    if (!s_app.sync_inflight && s_app.focus.count &&
        s_app.sync_retry_at_ms && now >= s_app.sync_retry_at_ms) {
        s_app.sync_retry_at_ms = 0;
        request_sync_upload();
    }

    // ---- 每秒:相对文案(重置时刻/页脚下次倒计时/番茄指示点) ----
    if (now - s_last_sec_tick >= 1000) {
        s_last_sec_tick = now;
        if (s_app.page == PAGE_DASH && s_app.boot_epoch) {
            app_ui_dash_tick(wall_sec());
        }
        if (s_app.page == PAGE_DASH && st == APP_WIFI_CONNECTED &&
            s_app.fetched_wall && !s_app.fetch_inflight && s_app.last_ok) {
            footer_refresh(st);
        }
        if (s_app.page == PAGE_DASH) update_focus_dot();
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
    ESP_LOGI(TAG, "双应用启动:GLM 额度 + 番茄钟(↑↓ 翻页)");

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

    focus_model_init(&s_app.model, 0);
    if (app_store_load_focus_buf(&s_app.focus) && s_app.focus.count) {
        ESP_LOGI(TAG, "载入待传会话 %u 条", (unsigned)s_app.focus.count);
    }
    app_store_load_sync_token(s_app.sync_token, sizeof(s_app.sync_token));
    if (!s_app.sync_token[0] && GLM_SYNC_TOKEN_DEFAULT[0]) {
        // 本机内置同步 token(credentials.h):一次性写入 NVS。
        strlcpy(s_app.sync_token, GLM_SYNC_TOKEN_DEFAULT, sizeof(s_app.sync_token));
        app_store_save_sync_token(s_app.sync_token);
        ESP_LOGI(TAG, "写入内置同步 token(长度 %d)", (int)strlen(s_app.sync_token));
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
    app_audio_init();       // 失败不致命:音效禁用,番茄照常

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
