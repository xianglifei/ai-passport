#include "app_store.h"

#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "app_store";
static const char *NS = "focus_app";
static const char *KEY_HM = "hm";
static const char *KEY_CLOCK = "clock";

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

bool app_store_load_heatmap(hm_store_t *store) {
    hm_init(store);
    nvs_handle_t handle;
    if (nvs_open(NS, NVS_READONLY, &handle) != ESP_OK) return false;
    uint8_t buf[HM_BLOB_SIZE];
    size_t len = sizeof(buf);
    esp_err_t err = nvs_get_blob(handle, KEY_HM, buf, &len);
    nvs_close(handle);
    if (err != ESP_OK || len != HM_BLOB_SIZE) return false;
    return hm_deserialize(store, buf, len);
}

int app_store_save_heatmap(const uint8_t *blob, size_t len) {
    if (!blob || len != HM_BLOB_SIZE) return ESP_ERR_INVALID_ARG;
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = nvs_set_blob(handle, KEY_HM, blob, len);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "热力图写入失败: %s", esp_err_to_name(err));
    }
    return err;
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
