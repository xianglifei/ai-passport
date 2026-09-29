// main/app_store.c —— NVS 薄封装,见 app_store.h。
// 命名空间沿用 "focus_app"(与番茄专注分支同源,Key 不同不冲突)。
#include "app_store.h"

#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "app_store";
static const char *NS = "focus_app";
static const char *KEY_CLOCK = "clock";
static const char *KEY_API = "glm_key";
static const char *KEY_QUOTA = "glm_quota";

bool app_store_init(void) {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS 需要重建(擦除默认分区后重试): %s", esp_err_to_name(err));
        esp_err_t erase_err = nvs_flash_erase();
        if (erase_err != ESP_OK) {
            ESP_LOGE(TAG, "NVS 擦除失败: %s", esp_err_to_name(erase_err));
            return false;
        }
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS 初始化失败: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

bool app_store_load_clock(uint64_t *wall_epoch, uint64_t *saved_boot_ms) {
    nvs_handle_t handle;
    if (nvs_open(NS, NVS_READONLY, &handle) != ESP_OK) return false;
    uint8_t buf[16];
    size_t len = sizeof(buf);
    esp_err_t err = nvs_get_blob(handle, KEY_CLOCK, buf, &len);
    nvs_close(handle);
    if (err != ESP_OK || len != sizeof(buf)) return false;
    uint64_t epoch, boot_ms;
    memcpy(&epoch, buf, 8);
    memcpy(&boot_ms, buf + 8, 8);
    if (epoch == 0) return false;
    if (wall_epoch) *wall_epoch = epoch;
    if (saved_boot_ms) *saved_boot_ms = boot_ms;
    return true;
}

int app_store_save_clock(uint64_t wall_epoch, uint64_t saved_boot_ms) {
    uint8_t buf[16];
    memcpy(buf, &wall_epoch, 8);
    memcpy(buf + 8, &saved_boot_ms, 8);
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = nvs_set_blob(handle, KEY_CLOCK, buf, sizeof(buf));
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "时钟续存失败: %s", esp_err_to_name(err));
    }
    return err;
}

bool app_store_load_key(char *buf, size_t len) {
    if (!buf || len < 2) return false;
    nvs_handle_t handle;
    if (nvs_open(NS, NVS_READONLY, &handle) != ESP_OK) return false;
    size_t size = len;
    esp_err_t err = nvs_get_str(handle, KEY_API, buf, &size);
    nvs_close(handle);
    return err == ESP_OK && buf[0] != 0;
}

int app_store_save_key(const char *key) {
    if (!key || !key[0]) return ESP_ERR_INVALID_ARG;
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = nvs_set_str(handle, KEY_API, key);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Key 保存失败: %s", esp_err_to_name(err));
    }
    return err;
}

bool app_store_has_key(void) {
    nvs_handle_t handle;
    if (nvs_open(NS, NVS_READONLY, &handle) != ESP_OK) return false;
    size_t size = 0;
    esp_err_t err = nvs_get_str(handle, KEY_API, NULL, &size);
    nvs_close(handle);
    return err == ESP_OK && size > 1;    // size 含 NUL;>1 即非空串
}

// 快照按内存布局整块存取:同版本固件读写,字段增减由 size 校验兜底。
bool app_store_load_quota(quota_data_t *out, uint64_t *saved_wall_sec) {
    nvs_handle_t handle;
    if (nvs_open(NS, NVS_READONLY, &handle) != ESP_OK) return false;
    uint8_t buf[sizeof(quota_data_t) + 8];
    size_t len = sizeof(buf);
    esp_err_t err = nvs_get_blob(handle, KEY_QUOTA, buf, &len);
    nvs_close(handle);
    if (err != ESP_OK || len != sizeof(buf)) return false;
    memcpy(out, buf, sizeof(*out));
    memcpy(saved_wall_sec, buf + sizeof(*out), 8);
    return out->valid;
}

int app_store_save_quota(const quota_data_t *d, uint64_t wall_sec) {
    if (!d || !d->valid) return ESP_ERR_INVALID_ARG;
    uint8_t buf[sizeof(quota_data_t) + 8];
    memcpy(buf, d, sizeof(*d));
    memcpy(buf + sizeof(*d), &wall_sec, 8);
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = nvs_set_blob(handle, KEY_QUOTA, buf, sizeof(buf));
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "配额快照写入失败: %s", esp_err_to_name(err));
    }
    return err;
}
