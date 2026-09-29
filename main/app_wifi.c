#include "app_wifi.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "app_store.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"

static const char *TAG = "app_wifi";
static const char *NS = "focus_app";
static const char *KEY_SSID = "wifi_ssid";
static const char *KEY_PASS = "wifi_pass";

#define STA_CONNECT_TIMEOUT_MS 25000
#define AP_LIFETIME_MS 300000
#define AP_MAX_CONNECTIONS 1

typedef struct {
    esp_event_handler_instance_t wifi_evt;
    esp_event_handler_instance_t ip_evt;
    esp_netif_t *sta_netif;
    esp_netif_t *ap_netif;
    httpd_handle_t httpd;
    bool wifi_started;
    bool sntp_running;
    uint32_t retry;
    uint64_t connecting_since_ms;
    uint64_t ap_since_ms;
    volatile bool sync_flag;
    app_wifi_state_t state;
    char ssid[33];
    char pass[65];
    char ap_name[24];
} wifi_ctx_t;

static wifi_ctx_t s_w;

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data);

static bool load_creds(void) {
    nvs_handle_t handle;
    if (nvs_open(NS, NVS_READONLY, &handle) != ESP_OK) return false;
    size_t len = sizeof(s_w.ssid);
    bool ok = nvs_get_str(handle, KEY_SSID, s_w.ssid, &len) == ESP_OK && s_w.ssid[0];
    len = sizeof(s_w.pass);
    ok = ok && nvs_get_str(handle, KEY_PASS, s_w.pass, &len) == ESP_OK;
    nvs_close(handle);
    return ok;
}

static void save_creds(const char *ssid, const char *pass) {
    nvs_handle_t handle;
    if (nvs_open(NS, NVS_READWRITE, &handle) != ESP_OK) {
        ESP_LOGW(TAG, "凭据保存失败:NVS 不可用");
        return;
    }
    nvs_set_str(handle, KEY_SSID, ssid);
    nvs_set_str(handle, KEY_PASS, pass);
    nvs_commit(handle);
    nvs_close(handle);
}

static void on_sync(struct timeval *tv) {
    (void)tv;
    s_w.sync_flag = true;
}

static void start_sntp(void) {
    if (s_w.sntp_running) return;
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(
        2, ESP_SNTP_SERVER_LIST("ntp.aliyun.com", "pool.ntp.org"));
    cfg.sync_cb = on_sync;
    cfg.start = true;
    if (esp_netif_sntp_init(&cfg) != ESP_OK) {
        ESP_LOGW(TAG, "SNTP 启动失败");
        return;
    }
    s_w.sntp_running = true;
    ESP_LOGI(TAG, "SNTP 对时开始");
}

// httpd 的 /save 在 httpd 任务里回调;自建一次性任务延迟收尾,避免占用 httpd。
static void finish_provision_task(void *arg) {
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(1500));   // 让响应先送达手机
    app_wifi_finish_provision();
    vTaskDelete(NULL);
}

// HTML 属性值转义:SSID 可能含引号/&/< 等。
static void html_escape(const char *src, char *dst, size_t dst_len) {
    size_t o = 0;
    for (size_t i = 0; src[i] && o + 1 < dst_len; i++) {
        const char *rep = NULL;
        switch (src[i]) {
        case '"': rep = "&quot;"; break;
        case '&': rep = "&amp;"; break;
        case '<': rep = "&lt;"; break;
        case '>': rep = "&gt;"; break;
        default: dst[o++] = src[i]; continue;
        }
        for (const char *r = rep; *r && o + 1 < dst_len; r++) dst[o++] = *r;
    }
    dst[o] = 0;
}

static esp_err_t form_handler(httpd_req_t *req) {
    // 动态渲染:预填已存 SSID;Key 只提示"已保存",不回显内容。
    // 属性值一律加引号;no-store + 随机版本 URL,防止手机浏览器用缓存的旧页面。
    static char page[1600];
    char ssid_esc[33 * 6] = "";
    html_escape(s_w.ssid, ssid_esc, sizeof(ssid_esc));
    snprintf(page, sizeof(page),
        "<!doctype html><html><head><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<title>GLM Quota Setup</title></head>"
        "<body style=\"font-family:sans-serif;max-width:420px;margin:36px auto\">"
        "<h3>GLM 额度 · 配网</h3>"
        "<form action=\"/save\" method=\"post\">"
        "<p>Wi-Fi 名称<br>"
        "<input name=\"s\" value=\"%s\" autocomplete=\"off\" "
        "style=\"width:100%%;font-size:18px\"></p>"
        "<p>Wi-Fi 密码<br>"
        "<input name=\"p\" type=\"password\" autocomplete=\"off\" "
        "style=\"width:100%%;font-size:18px\"></p>"
        "<p>智谱 API Key%s<br>"
        "<input name=\"k\" type=\"text\" placeholder=\"%s\" "
        "autocomplete=\"off\" spellcheck=\"false\" "
        "style=\"width:100%%;font-size:18px\"></p>"
        "<p><button style=\"font-size:18px;padding:10px 28px\">保存并连接</button></p>"
        "</form></body></html>",
        ssid_esc,
        s_w.ssid[0] ? "(已保存)" : "",
        app_store_has_key() ? "已保存,留空则不修改" : "32位十六进制.8位字母数字");
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, page, HTTPD_RESP_USE_STRLEN);
}

// "/" 永远 302 到带随机版本号的地址:手机浏览器不会复用缓存的旧表单页。
static esp_err_t root_handler(httpd_req_t *req) {
    static char target[40];
    snprintf(target, sizeof(target), "/form?v=%u", (unsigned)esp_random());
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", target);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static void url_decode(const char *src, char *dst, size_t dst_len) {
    size_t o = 0;
    for (size_t i = 0; src[i] && o + 1 < dst_len; i++) {
        char c = src[i];
        if (c == '%' && src[i + 1] && src[i + 2]) {
            char hex[3] = { src[i + 1], src[i + 2], 0 };
            dst[o++] = (char)strtol(hex, NULL, 16);
            i += 2;
        } else if (c == '+') {
            dst[o++] = ' ';
        } else {
            dst[o++] = c;
        }
    }
    dst[o] = 0;
}

static esp_err_t save_handler(httpd_req_t *req) {
    // POST 表单体与 query 同构(s=..&p=..&k=..),复用同一解析器。
    // 之前用 GET 时用户手机浏览器两次把 k 字段丢在 URL 外,POST 走请求体绕开它。
    char query[512] = {0};
    char raw[160] = {0};
    char ssid[33] = {0}, pass[65] = {0}, key[96] = {0};

    size_t content_len = req->content_len;
    if (content_len == 0 || content_len >= sizeof(query)) {
        ESP_LOGW(TAG, "save 表单体异常: len=%u", (unsigned)content_len);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
        return ESP_OK;
    }
    size_t received = 0;
    while (received < content_len) {
        int ret = httpd_req_recv(req, query + received, content_len - received);
        if (ret <= 0) {
            ESP_LOGW(TAG, "save 表单体读取失败");
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "recv failed");
            return ESP_OK;
        }
        received += (size_t)ret;
    }
    query[received] = 0;

    if (httpd_query_key_value(query, "s", raw, sizeof(raw)) != ESP_OK) {
        ESP_LOGW(TAG, "save 缺 ssid: body_len=%u", (unsigned)received);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing ssid");
        return ESP_OK;
    }
    url_decode(raw, ssid, sizeof(ssid));
    bool have_pass = httpd_query_key_value(query, "p", raw, sizeof(raw)) == ESP_OK;
    if (have_pass) url_decode(raw, pass, sizeof(pass));
    // Key 选填:留空/缺省 = 保留已存 Key(只改 Wi-Fi 时不丢 Key)。
    bool k_found = httpd_query_key_value(query, "k", raw, sizeof(raw)) == ESP_OK;
    if (k_found) url_decode(raw, key, sizeof(key));
    bool have_key = k_found && key[0] != 0;
    if (!ssid[0] || strlen(ssid) > 32) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad ssid");
        return ESP_OK;
    }
    // 诊断日志只记长度与在场性,绝不打印凭据/Key 内容。
    ESP_LOGI(TAG, "save 表单: body_len=%u pass=%s k=%s (k_len=%u)",
             (unsigned)received,
             have_pass ? (pass[0] ? "有值" : "空") : "缺",
             k_found ? (have_key ? "有值" : "空") : "缺",
             (unsigned)(k_found ? strlen(key) : 0));
    // 密码留空 = 保留已存密码(与 Key 同规则;防止误覆盖)。
    if (have_pass && pass[0]) {
        strlcpy(s_w.pass, pass, sizeof(s_w.pass));
    }
    if (have_key) {
        if (app_store_save_key(key) != ESP_OK) {
            ESP_LOGW(TAG, "API Key 保存失败(NVS 不可用?)");
        } else {
            ESP_LOGI(TAG, "API Key 已更新(长度 %d)", (int)strlen(key));
        }
    }
    save_creds(ssid, s_w.pass);
    strlcpy(s_w.ssid, ssid, sizeof(s_w.ssid));
    ESP_LOGI(TAG, "收到新凭据 ssid=%s", s_w.ssid);
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    if (have_key) {
        httpd_resp_send(req, "已保存,设备正在连接,结果请看设备屏幕",
                        HTTPD_RESP_USE_STRLEN);
    } else {
        // 说真话:手机上没收到 Key 多半是浏览器拿了缓存的旧页面。
        httpd_resp_send(req,
            "Wi-Fi 已保存,但没有收到 API Key(可能是缓存的旧页面)。"
            "请刷新本页重试;若仍失败,重启浏览器再打开 192.168.4.1",
            HTTPD_RESP_USE_STRLEN);
    }
    xTaskCreate(finish_provision_task, "wifi_fin", 4096, NULL, 4, NULL);
    return ESP_OK;
}

bool app_wifi_init(void) {
    memset(&s_w, 0, sizeof(s_w));
    s_w.state = APP_WIFI_IDLE;

    if (esp_netif_init() != ESP_OK ||
        esp_event_loop_create_default() != ESP_OK) {
        ESP_LOGW(TAG, "netif/event 初始化失败,Wi-Fi 不可用");
        return false;
    }
    s_w.sta_netif = esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    if (esp_wifi_init(&cfg) != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_init 失败,Wi-Fi 不可用");
        return false;
    }
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL, &s_w.wifi_evt));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL, &s_w.ip_evt));
    esp_wifi_set_storage(WIFI_STORAGE_RAM);

    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_w.ap_name, sizeof(s_w.ap_name), "GLMQuota-%02X%02X",
             mac[4], mac[5]);
    return load_creds();
}

bool app_wifi_has_creds(void) {
    return s_w.ssid[0] != 0;
}

const char *app_wifi_ap_name(void) {
    return s_w.ap_name;
}

const char *app_wifi_ssid(void) {
    return s_w.ssid;
}

app_wifi_state_t app_wifi_state(void) {
    return s_w.state;
}

bool app_wifi_poll_sync(uint64_t *utc_sec) {
    if (!s_w.sync_flag) return false;
    s_w.sync_flag = false;
    if (utc_sec) *utc_sec = (uint64_t)time(NULL);
    return true;
}

void app_wifi_tick(uint64_t now_ms) {
    // CONNECTED 是本应用的常态,不再有"对完即停"的 25s 收摊;
    // 只有 CONNECTING 卡住才判失败,由 main 定期重试。
    if (s_w.state == APP_WIFI_CONNECTING && s_w.connecting_since_ms &&
        now_ms - s_w.connecting_since_ms > STA_CONNECT_TIMEOUT_MS) {
        ESP_LOGW(TAG, "连接 %s 超时未拿到 IP", s_w.ssid);
        s_w.state = APP_WIFI_FAILED;
    }
    if ((s_w.state == APP_WIFI_AP_MODE || s_w.state == APP_WIFI_AP_CONNECTED) &&
        s_w.ap_since_ms && now_ms - s_w.ap_since_ms > AP_LIFETIME_MS) {
        ESP_LOGW(TAG, "配网热点超时未完成,自动关闭");
        if (s_w.httpd) { httpd_stop(s_w.httpd); s_w.httpd = NULL; }
        app_wifi_stop();
        s_w.state = s_w.ssid[0] ? APP_WIFI_FAILED : APP_WIFI_IDLE;
    }
}

bool app_wifi_start_sta(void) {
    if (!s_w.ssid[0]) return false;
    if (s_w.state == APP_WIFI_CONNECTING || s_w.state == APP_WIFI_CONNECTED) {
        return true;
    }
    if (s_w.sntp_running) { esp_netif_sntp_deinit(); s_w.sntp_running = false; }
    if (s_w.httpd) { httpd_stop(s_w.httpd); s_w.httpd = NULL; }

    esp_wifi_set_mode(WIFI_MODE_STA);
    wifi_config_t cfg = {0};
    strlcpy((char *)cfg.sta.ssid, s_w.ssid, sizeof(cfg.sta.ssid));
    strlcpy((char *)cfg.sta.password, s_w.pass, sizeof(cfg.sta.password));
    esp_wifi_set_config(WIFI_IF_STA, &cfg);
    s_w.retry = 0;
    s_w.state = APP_WIFI_CONNECTING;
    s_w.connecting_since_ms = (uint64_t)esp_timer_get_time() / 1000ULL;
    if (!s_w.wifi_started) {
        if (esp_wifi_start() != ESP_OK) {
            s_w.state = APP_WIFI_FAILED;
            return false;
        }
        s_w.wifi_started = true;
    } else {
        esp_wifi_connect();
    }
    ESP_LOGI(TAG, "开始连接 %s", s_w.ssid);
    return true;
}

bool app_wifi_start_provision(void) {
    // 热点开在已存名称上,重复进入配网前先收干净。
    if (s_w.wifi_started) { esp_wifi_stop(); s_w.wifi_started = false; }
    if (s_w.sntp_running) { esp_netif_sntp_deinit(); s_w.sntp_running = false; }
    if (s_w.httpd) { httpd_stop(s_w.httpd); s_w.httpd = NULL; }
    if (!s_w.ap_netif) s_w.ap_netif = esp_netif_create_default_wifi_ap();

    esp_wifi_set_mode(WIFI_MODE_AP);
    wifi_config_t ap = {0};
    strlcpy((char *)ap.ap.ssid, s_w.ap_name, sizeof(ap.ap.ssid));
    ap.ap.max_connection = AP_MAX_CONNECTIONS;
    ap.ap.authmode = WIFI_AUTH_OPEN;
    esp_wifi_set_config(WIFI_IF_AP, &ap);
    if (esp_wifi_start() != ESP_OK) {
        s_w.state = APP_WIFI_FAILED;
        return false;
    }
    s_w.wifi_started = true;
    s_w.ap_since_ms = (uint64_t)esp_timer_get_time() / 1000ULL;
    s_w.state = APP_WIFI_AP_MODE;

    httpd_config_t httpd_cfg = HTTPD_DEFAULT_CONFIG();
    if (httpd_start(&s_w.httpd, &httpd_cfg) == ESP_OK) {
        httpd_uri_t uri_root = { .uri = "/", .method = HTTP_GET, .handler = root_handler };
        httpd_uri_t uri_form = { .uri = "/form", .method = HTTP_GET, .handler = form_handler };
        httpd_uri_t uri_save = { .uri = "/save", .method = HTTP_POST, .handler = save_handler };
        httpd_register_uri_handler(s_w.httpd, &uri_root);
        httpd_register_uri_handler(s_w.httpd, &uri_form);
        httpd_register_uri_handler(s_w.httpd, &uri_save);
        ESP_LOGI(TAG, "配网热点 %s 就绪", s_w.ap_name);
    } else {
        ESP_LOGW(TAG, "HTTP 服务启动失败");
    }
    return true;
}

bool app_wifi_finish_provision(void) {
    if (s_w.httpd) { httpd_stop(s_w.httpd); s_w.httpd = NULL; }
    if (s_w.wifi_started) { esp_wifi_stop(); s_w.wifi_started = false; }
    if (s_w.ap_netif) {
        esp_netif_destroy(s_w.ap_netif);
        s_w.ap_netif = NULL;
    }
    ESP_LOGI(TAG, "配网结束,转入 STA 连接");
    return app_wifi_start_sta();
}

bool app_wifi_stop(void) {
    if (s_w.sntp_running) { esp_netif_sntp_deinit(); s_w.sntp_running = false; }
    if (s_w.httpd) { httpd_stop(s_w.httpd); s_w.httpd = NULL; }
    if (s_w.wifi_started) {
        esp_wifi_stop();
        s_w.wifi_started = false;
    }
    s_w.state = APP_WIFI_IDLE;
    return true;
}

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data) {
    (void)arg; (void)data;
    if (base == WIFI_EVENT) {
        switch (id) {
        case WIFI_EVENT_STA_START:
            esp_wifi_connect();
            break;
        case WIFI_EVENT_STA_DISCONNECTED:
            if (s_w.state == APP_WIFI_CONNECTING && s_w.retry < 3) {
                // 连不上:前 3 次立刻重试,拖满 25s 超时后转 FAILED。
                s_w.retry++;
                esp_wifi_connect();
            } else if (s_w.state == APP_WIFI_CONNECTING) {
                s_w.state = APP_WIFI_FAILED;
                ESP_LOGW(TAG, "连接失败(重试 %u 次)", (unsigned)s_w.retry);
            } else if (s_w.state == APP_WIFI_CONNECTED) {
                // 在线时掉线(路由重启/信号丢失):立即转入重连,
                // 4 次内回不去则经 25s 守护转 FAILED,main 周期重试。
                ESP_LOGW(TAG, "在线掉线,自动重连");
                s_w.state = APP_WIFI_CONNECTING;
                s_w.retry = 0;
                s_w.connecting_since_ms = (uint64_t)esp_timer_get_time() / 1000ULL;
                esp_wifi_connect();
            }
            break;
        case WIFI_EVENT_AP_STACONNECTED:
            s_w.state = APP_WIFI_AP_CONNECTED;
            break;
        default:
            break;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        s_w.state = APP_WIFI_CONNECTED;
        s_w.connecting_since_ms = 0;
        start_sntp();
    }
}
