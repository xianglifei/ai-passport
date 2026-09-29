#include "app_audio.h"

#include <math.h>

#include "bsp_audio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *TAG = "app_audio";

#define CHIME_RATE 16000
#define CHUNK_SAMPLES 512
#define FADE_MS 8

// E5 → G5 → C6 琶音,最后一音拉长。
static const struct { float hz; uint32_t ms; } CHIME_NOTES[] = {
    { 659.25f, 140 }, { 783.99f, 140 }, { 1046.50f, 340 },
};
#define CHIME_NOTE_COUNT (sizeof(CHIME_NOTES) / sizeof(CHIME_NOTES[0]))

typedef enum {
    WORK_CHIME = 1,
} work_item_t;

static QueueHandle_t s_queue;
static bool s_ready;

// 单个音符:按 chunk 生成带 8ms 淡入淡出的正弦并写入。
static bool play_note(float hz, uint32_t ms) {
    uint32_t total = (uint64_t)CHIME_RATE * ms / 1000U;
    uint32_t fade = (uint64_t)CHIME_RATE * FADE_MS / 1000U;
    float step = 2.0f * 3.14159265f * hz / CHIME_RATE;
    int16_t buf[CHUNK_SAMPLES];
    float phase = 0.0f;
    uint32_t done = 0;
    while (done < total) {
        uint32_t n = total - done < CHUNK_SAMPLES ? total - done : CHUNK_SAMPLES;
        for (uint32_t i = 0; i < n; i++) {
            float amp = 9000.0f;
            if (done + i < fade) amp = 9000.0f * (done + i) / fade;
            if (total - (done + i) < fade) amp = 9000.0f * (total - done - i) / fade;
            buf[i] = (int16_t)(sinf(phase) * amp);
            phase += step;
            if (phase >= 2.0f * 3.14159265f) phase -= 2.0f * 3.14159265f;
        }
        if (bsp_audio_write(buf, (size_t)n * sizeof(int16_t)) != ESP_OK) {
            ESP_LOGW(TAG, "音频写入失败,中止音效");
            return false;
        }
        done += n;
    }
    return true;
}

static void audio_task(void *arg) {
    (void)arg;
    work_item_t item;
    for (;;) {
        if (xQueueReceive(s_queue, &item, portMAX_DELAY) != pdTRUE) continue;
        if (item == WORK_CHIME) {
            for (uint32_t i = 0; i < CHIME_NOTE_COUNT; i++) {
                if (!play_note(CHIME_NOTES[i].hz, CHIME_NOTES[i].ms)) break;
            }
        }
    }
}

bool app_audio_init(void) {
    if (bsp_audio_init() != ESP_OK) {
        ESP_LOGW(TAG, "音频初始化失败,音效禁用");
        return false;
    }
    if (bsp_audio_set_format(CHIME_RATE, 16, 1) != ESP_OK) {
        ESP_LOGW(TAG, "音频格式设置失败,音效禁用");
        return false;
    }
    bsp_audio_set_volume(75);
    s_queue = xQueueCreate(2, sizeof(work_item_t));
    if (!s_queue || xTaskCreate(audio_task, "app_audio", 4096, NULL, 4, NULL) != pdPASS) {
        ESP_LOGW(TAG, "音频任务创建失败,音效禁用");
        return false;
    }
    s_ready = true;
    return true;
}

void app_audio_play_chime(void) {
    if (!s_ready || !s_queue) return;
    work_item_t item = WORK_CHIME;
    (void)xQueueSend(s_queue, &item, 0);
}
