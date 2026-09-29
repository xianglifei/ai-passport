// main/quota_fetch.c —— esp_http_client + 证书包拉取限额响应,见 quota_fetch.h。
#include "quota_fetch.h"

#include <stdio.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "quota_model.h"

// 实测响应 <1KB;留足余量,超长截断交由 cJSON 解析失败兜底。
#define RESP_BUF_SIZE 2048
#define FETCH_TIMEOUT_MS 12000

static const char *TAG = "quota_fetch";
static const char *MONITOR_URL =
    "https://open.bigmodel.cn/api/monitor/usage/quota/limit";

// esp_http_client 事件里只做追加,成败判定放在轮询返回之后。
typedef struct {
    char buf[RESP_BUF_SIZE];
    size_t len;
    bool overflow;
} resp_buf_t;

static esp_err_t on_http_event(esp_http_client_event_t *evt) {
    resp_buf_t *resp = (resp_buf_t *)evt->user_data;
    if (evt->event_id != HTTP_EVENT_ON_DATA || !resp || !evt->data) return ESP_OK;
    if (resp->len + evt->data_len >= sizeof(resp->buf)) {
        resp->overflow = true;
        return ESP_OK;
    }
    memcpy(resp->buf + resp->len, evt->data, evt->data_len);
    resp->len += evt->data_len;
    return ESP_OK;
}

// 本地时区偏移(UTC+8):与 main/app_wifi 的 SNTP 折算约定一致。
// 接口的 nextResetTime 是 UTC 纪元毫秒;应用内部时间口径是本地墙钟,
// 差 8 小时会把"3 小时后刷新"显示成"即将刷新",此处统一折算。
#define APP_TZ_OFFSET_SEC (8 * 3600)

bool quota_fetch_now(const char *key, quota_data_t *out,
                     char *errbuf, size_t errlen) {
    static resp_buf_t resp;               // 静态区:不让 2KB 缓冲占任务栈
    memset(&resp, 0, sizeof(resp));
    if (errbuf && errlen) errbuf[0] = 0;

    esp_http_client_config_t cfg = {
        .url = MONITOR_URL,
        .method = HTTP_METHOD_GET,
        .timeout_ms = FETCH_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler = on_http_event,
        .user_data = &resp,
        .buffer_size = 1024,              // TLS 收包缓冲(不含响应体累积)
        .keep_alive_enable = false,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        snprintf(errbuf, errlen, "网络错误");
        return false;
    }
    esp_http_client_set_header(client, "Authorization", key);
    esp_http_client_set_header(client, "Content-Type", "application/json");

    esp_err_t err = esp_http_client_perform(client);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "请求失败: %s", esp_err_to_name(err));
        snprintf(errbuf, errlen, "网络错误");
        esp_http_client_cleanup(client);
        return false;
    }
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (status == 401) {
        snprintf(errbuf, errlen, "Key 无效(401)");
        return false;
    }
    if (status != 200) {
        snprintf(errbuf, errlen, "HTTP %d", status);
        return false;
    }
    if (resp.overflow) {
        snprintf(errbuf, errlen, "响应超长");
        return false;
    }
    resp.buf[resp.len] = 0;
    if (!quota_parse_json(resp.buf, out)) {
        // 接口对无效 Key 有时返回 200 + success:false,而非 401。
        if (strstr(resp.buf, "\"success\":false")) {
            snprintf(errbuf, errlen, "Key 无效,接口拒绝");
        } else {
            snprintf(errbuf, errlen, "响应无法解析");
        }
        return false;
    }
    // 重置时刻折算到本地墙钟口径(见文件头注释);此后全应用不再做时区换算。
    uint64_t tz_ms = (uint64_t)APP_TZ_OFFSET_SEC * 1000ULL;
    if (out->win5h.reset_ms) out->win5h.reset_ms += tz_ms;
    if (out->week.reset_ms) out->week.reset_ms += tz_ms;
    return true;
}
