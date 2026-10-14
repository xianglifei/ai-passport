// main/pet_nvs.c —— 卡卡存档的 NVS 胶水实现。
#include "pet_nvs.h"

#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "pet_store.h"

static const char *TAG = "kaka_store";

#define NVS_NAMESPACE "kaka_pet"
#define NVS_KEY_STATS "stats"
#define NVS_KEY_MUTE  "mute"

bool pet_nvs_init(void) {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        // 常规恢复路径:擦除后重试一次(仓库 app_store 同款模式)
        ESP_LOGW(TAG, "NVS 需要擦除重建 (%s), 旧存档丢失", esp_err_to_name(err));
        err = nvs_flash_erase();
        if (err == ESP_OK) err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS 初始化失败: %s —— 继续运行但不持久化", esp_err_to_name(err));
        return false;
    }
    return true;
}

bool pet_nvs_load(pet_model_t *m) {
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return false;
    uint8_t buf[PET_STORE_BLOB_SIZE];
    size_t len = sizeof(buf);
    esp_err_t err = nvs_get_blob(h, NVS_KEY_STATS, buf, &len);
    nvs_close(h);
    if (err != ESP_OK) return false;
    if (!pet_store_deserialize(m, buf, len)) {
        ESP_LOGW(TAG, "存档损坏(版本/长度/数值非法), 重新开始");
        return false;
    }
    return true;
}

bool pet_nvs_save(const pet_model_t *m) {
    uint8_t buf[PET_STORE_BLOB_SIZE];
    size_t len = pet_store_serialize(m, buf, sizeof(buf));
    if (len == 0) return false;
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return false;
    esp_err_t err = nvs_set_blob(h, NVS_KEY_STATS, buf, len);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) ESP_LOGW(TAG, "存档写入失败: %s", esp_err_to_name(err));
    return err == ESP_OK;
}

bool pet_nvs_mute(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return false;
    uint8_t v = 0;
    nvs_get_u8(h, NVS_KEY_MUTE, &v);  // 无键 = 未静音
    nvs_close(h);
    return v != 0;
}

void pet_nvs_set_mute(bool on) {
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return;
    esp_err_t err = nvs_set_u8(h, NVS_KEY_MUTE, on ? 1 : 0);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) ESP_LOGW(TAG, "静音开关保存失败: %s", esp_err_to_name(err));
}
