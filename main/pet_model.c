// main/pet_model.c —— 卡卡养成核心模型实现(纯逻辑,无 ESP-IDF 依赖)。
#include "pet_model.h"

#include <string.h>

// 状态统一夹在 [PET_STAT_MIN, PET_STAT_MAX];收益另算,衰减只到下限。
static uint8_t stat_clamp(int v) {
    if (v < PET_STAT_MIN) return PET_STAT_MIN;
    if (v > PET_STAT_MAX) return PET_STAT_MAX;
    return (uint8_t)v;
}

// 大于 85 后收益减半(防无脑连点,与计划一致)
static uint8_t gain(uint8_t current, uint8_t amount) {
    int add = (current > 85) ? (amount / 2) : amount;
    return stat_clamp((int)current + add);
}

static uint32_t rng_next(pet_model_t *m) {
    // xorshift32:确定性伪随机,host 测试可复现
    uint32_t x = m->rng ? m->rng : 0x9E3779B9u;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    m->rng = x;
    return x;
}

void pet_model_init(pet_model_t *m) {
    memset(m, 0, sizeof(*m));
    m->fullness = 70;
    m->clean = 70;
    m->happy = 80;
    m->rng = 0x1A2B3C4Du;
}

void pet_model_set_stats(pet_model_t *m, uint8_t full, uint8_t clean, uint8_t happy) {
    m->fullness = stat_clamp(full);
    m->clean = stat_clamp(clean);
    m->happy = stat_clamp(happy);
}

static void decay_pass(pet_model_t *m, uint32_t delta_ms) {
    if (delta_ms == 0) return;
    // 饱食
    m->full_acc += delta_ms;
    while (m->full_acc >= PET_DECAY_FULL_MS) {
        m->full_acc -= PET_DECAY_FULL_MS;
        if (m->fullness > PET_STAT_MIN) m->fullness--;
    }
    // 干净
    m->clean_acc += delta_ms;
    while (m->clean_acc >= PET_DECAY_CLEAN_MS) {
        m->clean_acc -= PET_DECAY_CLEAN_MS;
        if (m->clean > PET_STAT_MIN) m->clean--;
    }
    // 心情
    m->happy_acc += delta_ms;
    while (m->happy_acc >= PET_DECAY_HAPPY_MS) {
        m->happy_acc -= PET_DECAY_HAPPY_MS;
        if (m->happy > PET_STAT_MIN) m->happy--;
    }
}

void pet_model_cold_boot(pet_model_t *m) {
    // 读档后统一补 6 小时等效衰减;衰减被封顶在下限,卡卡只是"有点想你"
    decay_pass(m, PET_COLD_BOOT_MS);
    m->idle_ms = 0;
}

bool pet_model_should_meow(pet_model_t *m) {
    if (m->idle_ms < PET_IDLE_MEOW_MIN_MS) return false;
    if (m->meow_roll_acc < PET_IDLE_MEOW_ROLL_MS) return false;
    m->meow_roll_acc = 0;
    return (rng_next(m) % PET_IDLE_MEOW_CHANCE) == 0;
}

void pet_model_tick(pet_model_t *m, uint32_t now_ms) {
    uint32_t delta = now_ms - m->last_ms;  // 无符号回绕安全(单调时钟)
    m->last_ms = now_ms;
    if (delta > 60000u) delta = 60000u;  // 防 first-tick 巨额 delta,60s 封顶

    decay_pass(m, delta);
    m->idle_ms += delta;
    m->meow_roll_acc += delta;

    // 活动计时:晒太阳/散步到时自动结束并结算
    if (m->activity == PET_ACTIVITY_SUN || m->activity == PET_ACTIVITY_WALK) {
        m->activity_ms += delta;
        uint32_t dur = (m->activity == PET_ACTIVITY_SUN) ? PET_SUN_DURATION_MS
                                                         : PET_WALK_DURATION_MS;
        if (m->activity_ms >= dur) {
            pet_model_finish_activity(m);
        }
    }
}

bool pet_model_start_activity(pet_model_t *m, pet_activity_t a) {
    if (m->activity != PET_ACTIVITY_NONE) return false;
    if (a == PET_ACTIVITY_NONE) return false;
    m->activity = a;
    m->activity_ms = 0;
    m->brush_strokes_this = 0;
    m->feed_fish_this = 0;
    m->idle_ms = 0;
    return true;
}

pet_interact_t pet_model_interact(pet_model_t *m) {
    m->idle_ms = 0;
    switch (m->activity) {
    case PET_ACTIVITY_FEED:
        if (m->fullness >= 95) {
            // 礼貌拒喂:活动就此结束
            pet_model_finish_activity(m);
            return PET_INTERACT_REFUSED;
        }
        m->fullness = gain(m->fullness, 10);
        m->feed_fish++;
        m->feed_fish_this++;
        if (m->feed_fish_this >= 3) {
            // 一顿吃 3 条鱼,吃饱收尾
            pet_model_finish_activity(m);
            return PET_INTERACT_ENDED;
        }
        return PET_INTERACT_FED;
    case PET_ACTIVITY_BRUSH:
        m->brush_strokes_this++;
        m->brush_strokes++;
        m->clean = gain(m->clean, 8);
        if (m->brush_strokes_this % PET_BRUSH_SHINE_STROKES == 0) {
            m->happy = gain(m->happy, 5);  // 发亮小奖励,活动圆满结束
            pet_model_finish_activity(m);
            return PET_INTERACT_SHINE;
        }
        return PET_INTERACT_BRUSHED;
    case PET_ACTIVITY_SUN:
        // 确定键 = 提前唤醒,晒太阳就此结束(结算在 finish 内统一做)
        pet_model_finish_activity(m);
        return PET_INTERACT_WOKE;
    case PET_ACTIVITY_WALK:
        return PET_INTERACT_JUMPED;  // 扑跳:小开心
    default:
        return PET_INTERACT_IDLE;
    }
}

void pet_model_finish_activity(pet_model_t *m) {
    switch (m->activity) {
    case PET_ACTIVITY_SUN:
        m->happy = gain(m->happy, 40);
        m->sun_count++;
        break;
    case PET_ACTIVITY_WALK:
        m->happy = gain(m->happy, 35);
        m->walk_count++;
        break;
    default:
        break;  // 喂食/刷毛的收益已按次结算
    }
    m->activity = PET_ACTIVITY_NONE;
    m->activity_ms = 0;
    m->brush_strokes_this = 0;
    m->feed_fish_this = 0;
    m->idle_ms = 0;
}

pet_want_t pet_model_want(const pet_model_t *m) {
    // 找最低且低于 WANT 线的状态;并列时按 饱食>干净>心情 的优先级
    if (m->fullness <= m->clean && m->fullness <= m->happy && m->fullness < PET_STAT_WANT)
        return PET_WANT_FOOD;
    if (m->clean <= m->happy && m->clean < PET_STAT_WANT)
        return PET_WANT_BRUSH;
    if (m->happy < PET_STAT_WANT)
        return PET_WANT_WALK;
    return PET_WANT_NONE;
}
