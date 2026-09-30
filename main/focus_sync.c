// main/focus_sync.c —— /sync 上报客户端,见 focus_sync.h。
// 只允许在工作任务里调用(阻塞 TLS,复用证书包,feixing.io 直连已真机验证)。
#include "focus_sync.h"

#include <stdlib.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"

static const char *TAG = "focus_sync";
static const char *SYNC_URL = "https://pomodoro-api.feixing.io/sync";

#define SYNC_TIMEOUT_MS 15000
// 200 条 ≈ 14KB JSON,堆上临时分配,不占常驻 RAM。
#define JSON_BUF_SIZE (FOCUS_STORE_CAP * 72 + 32)

focus_sync_result_t focus_sync_upload(const char *token,
                                      const focus_rec_t *recs, size_t n,
                                      uint16_t *uploaded) {
    if (uploaded) *uploaded = 0;
    if (!token || !token[0]) return FOCUS_SYNC_ERR_TOKEN;
    if (n == 0) return FOCUS_SYNC_OK;

    char *body = malloc(JSON_BUF_SIZE);
    if (!body) {
        ESP_LOGW(TAG, "JSON 缓冲分配失败");
        return FOCUS_SYNC_ERR_NET;
    }
    size_t off = (size_t)snprintf(body, JSON_BUF_SIZE, "{\"records\":[");
    uint16_t appended = 0;
    for (size_t i = 0; i < n && off < JSON_BUF_SIZE - 96; i++) {
        off += (size_t)snprintf(
            body + off, JSON_BUF_SIZE - off, "%s{\"type\":\"%s\",\"start\":%llu,\"end\":%llu}",
            i ? "," : "", recs[i].type == FOCUS_TYPE_WORK ? "work" : "rest",
            (unsigned long long)recs[i].start_ms,
            (unsigned long long)recs[i].end_ms);
        appended++;
    }
    off += (size_t)snprintf(body + off, JSON_BUF_SIZE - off, "]}");

    esp_http_client_config_t cfg = {
        .url = SYNC_URL,
        .method = HTTP_METHOD_POST,
        .timeout_ms = SYNC_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 1024,
        .keep_alive_enable = false,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        free(body);
        return FOCUS_SYNC_ERR_NET;
    }
    esp_http_client_set_header(client, "Authorization", token);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, body, (int)off);

    focus_sync_result_t result;
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);   // 失败时也可能已收到状态码
    if (err != ESP_OK && status <= 0) {
        ESP_LOGW(TAG, "上报网络失败: %s", esp_err_to_name(err));
        result = FOCUS_SYNC_ERR_NET;
    } else if (status == 200) {
        // 只认实际写入请求体的条数;若因截断少于 n,余下由调用方再发一批。
        if (uploaded) *uploaded = appended;
        ESP_LOGI(TAG, "上报成功: %u/%u 条", (unsigned)appended, (unsigned)n);
        result = FOCUS_SYNC_OK;
    } else if (status == 401) {
        ESP_LOGW(TAG, "同步凭证过期(401),需重新注入 token");
        result = FOCUS_SYNC_ERR_TOKEN;
    } else {
        ESP_LOGW(TAG, "上报被拒: HTTP %d", status);
        result = FOCUS_SYNC_ERR_REJECT;
    }
    esp_http_client_cleanup(client);
    free(body);
    return result;
}
