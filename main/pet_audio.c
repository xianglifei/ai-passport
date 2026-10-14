// main/pet_audio.c —— 卡卡声音系统实现:配方驱动的过程式合成器。
//
// 每个声音是一份 recipe:三段频率路径(f_start→f_mid→f_end)、分段包络、
// 颤音、噪声混合(气声/摩擦)或音符序列(铃铛/琶音/摇篮曲)。worker 任务
// 逐样本合成到 512 样本块再 bsp_audio_write,单芯片 FPU 足够实时。
#include "pet_audio.h"

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "bsp_audio.h"

static const char *TAG = "kaka_audio";

#define SAMPLE_HZ 16000
#define CHUNK 512
#define PLAY_QUEUE_DEPTH 4
#define TWO_PI 6.28318530718f

// —— 配方 ——
enum { KIND_TONE, KIND_NOTES, KIND_NOISE };

typedef struct {
    uint8_t kind;
    uint16_t dur_ms;    // 单次时长(音符序列为序列总长外的整体节拍)
    uint16_t f_start, f_mid, f_end;  // TONE 三段频率
    uint8_t seg_a, seg_b;            // 包络分段点(百分比)
    uint8_t vib_hz, vib_pct;         // 颤音
    uint8_t noise_pct;               // 噪声混合(气声)
    uint8_t amp;                     // 总幅度 0..255 → 映射 ±(amp*32)
    uint8_t am_hz;                   // 噪声/音调的调幅(咀嚼/呼噜)
    uint8_t repeat;                  // 整体重复次数(间隔 110ms)
    uint8_t notes_n;                 // NOTES:音符数
    uint16_t note_f[6];              // NOTES:频率(0=休止)
    uint16_t note_ms[6];             // NOTES:时长
} recipe_t;

// 便捷默认:TONE,无颤音无噪声,幅度 200
#define TONE_BASE(repeat_v) \
    .kind = KIND_TONE, .seg_a = 25, .seg_b = 70, .amp = 200, .repeat = repeat_v

static const recipe_t RECIPES[] = {
    [SND_MEOW_GREET] = { TONE_BASE(1), .dur_ms = 450,
                         .f_start = 550, .f_mid = 880, .f_end = 440,
                         .vib_hz = 6, .vib_pct = 8 },
    [SND_MEOW_HAPPY] = { TONE_BASE(2), .dur_ms = 300,
                         .f_start = 700, .f_mid = 1050, .f_end = 600,
                         .vib_hz = 7, .vib_pct = 10 },
    [SND_MEOW_SWEET] = { TONE_BASE(1), .dur_ms = 550, .amp = 160,
                         .f_start = 500, .f_mid = 700, .f_end = 450,
                         .vib_hz = 7, .vib_pct = 12 },
    [SND_MEOW_HUNGRY] = { TONE_BASE(1), .dur_ms = 700,
                          .f_start = 350, .f_mid = 280, .f_end = 560,
                          .vib_hz = 5, .vib_pct = 10, .seg_a = 40, .seg_b = 75 },
    [SND_MEOW_SLEEPY] = { TONE_BASE(1), .dur_ms = 800, .amp = 140,
                          .f_start = 480, .f_mid = 380, .f_end = 300,
                          .vib_hz = 4, .vib_pct = 6 },
    [SND_SQUEAK] = { TONE_BASE(1), .dur_ms = 120,
                     .f_start = 900, .f_mid = 1400, .f_end = 1100 },
    [SND_TRILL] = { TONE_BASE(1), .dur_ms = 400,
                    .f_start = 600, .f_mid = 900, .f_end = 700,
                    .vib_hz = 14, .vib_pct = 25 },
    [SND_YAWN] = { TONE_BASE(1), .dur_ms = 900, .amp = 170,
                   .f_start = 600, .f_mid = 350, .f_end = 200,
                   .noise_pct = 60, .seg_a = 35, .seg_b = 80 },
    [SND_MUNCH] = { .kind = KIND_NOISE, .dur_ms = 330, .amp = 190,
                    .am_hz = 9 },
    [SND_PURR] = { .kind = KIND_TONE, .dur_ms = 1200, .amp = 170,
                   .f_start = 55, .f_mid = 55, .f_end = 55,
                   .noise_pct = 45, .am_hz = 26, .seg_a = 10, .seg_b = 90 },
    [SND_BOING] = { TONE_BASE(1), .dur_ms = 220,
                    .f_start = 160, .f_mid = 420, .f_end = 240,
                    .vib_hz = 10, .vib_pct = 20 },
    [SND_FOOTSTEP] = { .kind = KIND_NOISE, .dur_ms = 60, .amp = 90 },
    [SND_BELL] = { .kind = KIND_NOTES, .dur_ms = 0, .amp = 190,
                   .notes_n = 2,
                   .note_f = { 1985, 2640 }, .note_ms = { 120, 380 } },
    [SND_SPARKLE] = { .kind = KIND_NOTES, .dur_ms = 0, .amp = 180,
                      .notes_n = 3,
                      .note_f = { 1568, 2093, 2637 }, .note_ms = { 70, 70, 120 } },
    [SND_WATER] = { .kind = KIND_NOISE, .dur_ms = 500, .amp = 150, .seg_b = 40 },
    [SND_HEART] = { .kind = KIND_NOTES, .dur_ms = 0, .amp = 170,
                    .notes_n = 2,
                    .note_f = { 1046, 1568 }, .note_ms = { 90, 220 } },
    [SND_BUBBLE] = { TONE_BASE(1), .dur_ms = 90,
                     .f_start = 300, .f_mid = 900, .f_end = 900, .amp = 170 },
    [SND_MENU_TICK] = { TONE_BASE(1), .dur_ms = 40, .amp = 150,
                        .f_start = 900, .f_mid = 900, .f_end = 900 },
    [SND_SELECT] = { .kind = KIND_NOTES, .dur_ms = 0, .amp = 180,
                     .notes_n = 2,
                     .note_f = { 784, 1175 }, .note_ms = { 80, 160 } },
    [SND_BIRDS] = { .kind = KIND_NOTES, .dur_ms = 0, .amp = 120,
                    .notes_n = 6,
                    .note_f = { 2400, 2800, 2300, 0, 2600, 3000 },
                    .note_ms = { 80, 60, 90, 120, 70, 60 } },
    [SND_WIND] = { .kind = KIND_NOISE, .dur_ms = 1500, .amp = 80, .am_hz = 1 },
    [SND_LULLABY] = { .kind = KIND_NOTES, .dur_ms = 0, .amp = 150,
                      .notes_n = 5,
                      .note_f = { 392, 440, 523, 587, 523 },
                      .note_ms = { 300, 300, 300, 300, 600 } },
    [SND_STRETCH] = { TONE_BASE(1), .dur_ms = 600, .amp = 160,
                      .f_start = 300, .f_mid = 500, .f_end = 350,
                      .noise_pct = 30 },
};

// —— 合成器状态(worker 内单线程访问) ——
typedef struct {
    const recipe_t *r;
    uint32_t t;              // 当前重复内的样本序号
    uint32_t total;          // 单次总样本数
    uint32_t gap_left;       // 重复间隔剩余样本
    uint8_t rep_left;        // 剩余重复次数
    uint32_t noise_lfsr;     // 噪声移位寄存器
    float phase;             // 振荡器相位
} synth_t;

static synth_t s_syn;
static QueueHandle_t s_play_q;
static TaskHandle_t s_task;
static volatile bool s_muted;
static bool s_ok;

static float synth_noise(synth_t *st) {
    // 16bit LFSR → [-1,1]
    uint32_t l = st->noise_lfsr;
    l ^= l << 13;
    l ^= l >> 17;
    l ^= l << 5;
    st->noise_lfsr = l ? l : 0xA5A5A5A5u;
    return ((float)(int16_t)l / 32768.0f);
}

// 包络:三段折线 {(0,0.15),(seg_a,1),(seg_b,0.9),(100,0)}
static float synth_env(const recipe_t *r, float pct) {
    if (pct < r->seg_a / 100.0f) {
        return 0.15f + 0.85f * (pct / (r->seg_a / 100.0f));
    }
    if (pct < r->seg_b / 100.0f) {
        float k = (pct - r->seg_a / 100.0f) /
                  ((r->seg_b - r->seg_a) / 100.0f);
        return 1.0f - 0.1f * k;
    }
    float k = (pct - r->seg_b / 100.0f) / ((100.0f - r->seg_b) / 100.0f);
    return 0.9f * (1.0f - k);
}

// 三段频率路径
static float synth_freq(const recipe_t *r, float pct) {
    if (pct < r->seg_a / 100.0f) {
        float k = pct / (r->seg_a / 100.0f);
        return (float)r->f_start + ((float)r->f_mid - r->f_start) * k;
    }
    float k = (pct - r->seg_a / 100.0f) / ((100.0f - r->seg_a) / 100.0f);
    return (float)r->f_mid + ((float)r->f_end - r->f_mid) * k;
}

// NOTES:当前时刻落在哪个音符,返回其频率(0=休止)
static float synth_note_freq(const synth_t *st, float pct) {
    const recipe_t *r = st->r;
    float acc = 0;
    float total_ms = 0;
    for (int i = 0; i < r->notes_n; i++) total_ms += r->note_ms[i];
    float t_ms = pct * total_ms;
    for (int i = 0; i < r->notes_n; i++) {
        acc += r->note_ms[i];
        if (t_ms < acc) return (float)r->note_f[i];
    }
    return 0;
}

static void synth_start(synth_t *st, pet_sound_t s) {
    memset(st, 0, sizeof(*st));
    if (s <= SND_NONE || s >= (pet_sound_t)(sizeof(RECIPES) / sizeof(RECIPES[0]))) {
        st->r = NULL;
        return;
    }
    st->r = &RECIPES[s];
    st->noise_lfsr = 0x1234ABCDu;
    if (st->r->kind == KIND_NOTES) {
        uint32_t total_ms = 0;
        for (int i = 0; i < st->r->notes_n; i++) total_ms += st->r->note_ms[i];
        st->total = total_ms * SAMPLE_HZ / 1000;
    } else {
        st->total = (uint32_t)st->r->dur_ms * SAMPLE_HZ / 1000;
    }
    st->rep_left = st->r->repeat ? (uint8_t)(st->r->repeat - 1) : 0;
}

// 生成一个样本;声音全部结束返回 false
static bool synth_next(synth_t *st, int16_t *out) {
    if (!st->r) return false;
    if (st->gap_left > 0) {  // 重复间隔
        st->gap_left--;
        *out = 0;
        return true;
    }
    if (st->t >= st->total) {  // 本轮结束
        if (st->rep_left > 0) {
            st->rep_left--;
            st->t = 0;
            st->phase = 0;
            st->gap_left = 110 * SAMPLE_HZ / 1000;
            *out = 0;
            return true;
        }
        return false;
    }
    const recipe_t *r = st->r;
    float pct = (float)st->t / (float)st->total;
    float sample = 0;
    if (r->kind == KIND_NOISE) {
        sample = synth_noise(st) * synth_env(r, pct);
        if (r->am_hz) {  // 咀嚼/风的调幅
            sample *= 0.55f + 0.45f * sinf(TWO_PI * r->am_hz * pct *
                                           (r->dur_ms / 1000.0f));
        }
    } else {
        float f = (r->kind == KIND_NOTES) ? synth_note_freq(st, pct)
                                          : synth_freq(r, pct);
        if (f <= 0) {
            sample = 0;  // 休止符
        } else {
            if (r->vib_hz) {
                f *= 1.0f + (r->vib_pct / 100.0f) *
                                sinf(TWO_PI * r->vib_hz * st->t / SAMPLE_HZ);
            }
            st->phase += f / SAMPLE_HZ;
            if (st->phase >= 1.0f) st->phase -= 1.0f;
            float tone = sinf(TWO_PI * st->phase);
            if (r->am_hz) {  // 呼噜的低频调幅
                tone *= 0.5f + 0.5f * sinf(TWO_PI * r->am_hz * st->t / SAMPLE_HZ);
            }
            sample = tone * synth_env(r, pct);
            if (r->noise_pct) {  // 气声
                sample = sample * (1.0f - r->noise_pct / 255.0f) +
                        synth_noise(st) * (r->noise_pct / 255.0f) * 0.5f;
            }
        }
    }
    st->t++;
    int32_t v = (int32_t)(sample * r->amp * 32);
    if (v > 32767) v = 32767;
    if (v < -32767) v = -32767;
    *out = (int16_t)v;
    return true;
}

static void audio_task(void *arg) {
    (void)arg;
    static int16_t chunk[CHUNK];
    pet_sound_t want;
    for (;;) {
        if (xQueueReceive(s_play_q, &want, portMAX_DELAY) != pdTRUE) continue;
        if (s_muted || !s_ok) continue;  // 静音:直接消费掉
        synth_start(&s_syn, want);
        while (s_ok) {
            uint32_t n = 0;
            while (n < CHUNK) {
                if (!synth_next(&s_syn, &chunk[n])) goto sound_done;
                n++;
            }
            bsp_audio_write(chunk, sizeof(int16_t) * CHUNK);
        }
    sound_done:;
    }
}

bool pet_audio_init(void) {
    if (bsp_audio_init() != ESP_OK) {
        ESP_LOGW(TAG, "音频初始化失败,游戏将无声运行");
        return false;
    }
    if (bsp_audio_set_format(SAMPLE_HZ, 16, 1) != ESP_OK) {
        ESP_LOGW(TAG, "音频格式设置失败");
        return false;
    }
    bsp_audio_set_volume(70);
    s_play_q = xQueueCreate(PLAY_QUEUE_DEPTH, sizeof(pet_sound_t));
    if (s_play_q == NULL) return false;
    if (xTaskCreate(audio_task, "kaka_audio", 4096, NULL, 4, &s_task) != pdPASS) {
        return false;
    }
    s_ok = true;
    return true;
}

void pet_audio_play(pet_sound_t s) {
    if (!s_ok || s == SND_NONE) return;
    // 最新优先:丢弃未播的旧请求,保证手感
    xQueueReset(s_play_q);
    xQueueSend(s_play_q, &s, 0);
}

void pet_audio_set_muted(bool muted) {
    s_muted = muted;
}
