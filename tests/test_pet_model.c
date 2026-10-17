// tests/test_pet_model.c —— 卡卡模型 host 逻辑测试(编译 main/pet_model.c)。
//
// 用统一的单调仿真时钟 sim_now 推进,保证所有测试块之间时间基点严格递增,
// 避免 delta 回绕被 60s 封顶后"碰巧通过"。
#include <assert.h>
#include <stdio.h>

#include "pet_model.h"

static uint32_t sim_now = 1;  // 从 1 开始,避免 0 被当作"未初始化"

static void sim_advance(pet_model_t *m, uint32_t ms, uint32_t step) {
    for (uint32_t t = 0; t < ms; t += step) {
        sim_now += step;
        pet_model_tick(m, sim_now);
    }
}

static void test_init_and_decay(void) {
    pet_model_t m;
        pet_model_init(&m);
    m.last_ms = sim_now;  // 同步到仿真时钟,避免首个 tick 的 60s 封顶 delta
    assert(m.fullness == 70 && m.clean == 70 && m.happy == 80);
    assert(m.activity == PET_ACTIVITY_NONE);

    // 12 分钟:饱食应 -1(70 → 69)
    sim_advance(&m, PET_DECAY_FULL_MS, 1000);
    assert(m.fullness == 69);

    // 再走 30 分钟(累计 42min):干净 -2(42/20min)、心情 -1(30min)、饱食 -3(42/12min)
    sim_advance(&m, PET_DECAY_HAPPY_MS, 1000);
    assert(m.clean == 68);
    assert(m.happy == 79);
    assert(m.fullness == 67);
}

static void test_decay_floor(void) {
    pet_model_t m;
        pet_model_init(&m);
    m.last_ms = sim_now;  // 同步到仿真时钟,避免首个 tick 的 60s 封顶 delta
    pet_model_set_stats(&m, PET_STAT_MIN, PET_STAT_MIN + 1, PET_STAT_MIN + 1);
    // 连续走 10 天:三状态全部钉在下限,卡卡永远不会惨
    sim_advance(&m, 10u * 24u * 60u * 60u * 1000u, 1000);
    assert(m.fullness == PET_STAT_MIN);
    assert(m.clean == PET_STAT_MIN);
    assert(m.happy == PET_STAT_MIN);
}

static void test_cold_boot(void) {
    pet_model_t m;
    pet_model_init(&m);
    // 6h 等效:饱食 -30、干净 -18、心情 -12;掉得多 → 思念等级 2
    uint8_t miss = pet_model_cold_boot(&m);
    assert(m.fullness == 40);
    assert(m.clean == 52);
    assert(m.happy == 68);
    assert(miss == 2);

    // 中等缺席:饱食 30 → 15(掉 15),思念等级 1
    pet_model_set_stats(&m, 30, 70, 80);
    miss = pet_model_cold_boot(&m);
    assert(m.fullness == PET_STAT_MIN);
    assert(miss == 1);

    // 低状态再冷启动也只到下限:没怎么掉,不播重逢
    pet_model_set_stats(&m, 20, 20, 20);
    miss = pet_model_cold_boot(&m);
    assert(m.fullness == PET_STAT_MIN);
    assert(m.clean == PET_STAT_MIN);
    assert(m.happy == PET_STAT_MIN);
    assert(miss == 0);
}

static void test_feed_and_refuse(void) {
    pet_model_t m;
    pet_model_init(&m);
    assert(pet_model_start_activity(&m, PET_ACTIVITY_FEED));
    // 活动中不能再开新活动
    assert(!pet_model_start_activity(&m, PET_ACTIVITY_WALK));

    // >85 后收益减半:80 → 90 → 95,第 3 条鱼吃饱自动收尾
    assert(pet_model_interact(&m) == PET_INTERACT_FED);
    assert(m.fullness == 80);
    assert(pet_model_interact(&m) == PET_INTERACT_FED);
    assert(m.fullness == 90);
    assert(pet_model_interact(&m) == PET_INTERACT_ENDED);
    assert(m.fullness == 95);
    assert(m.feed_fish == 3);
    assert(m.activity == PET_ACTIVITY_NONE);

    // 已饱再喂:礼貌拒喂,不加不减,也不退出场景(由 UI 数连续拒喂温柔收尾)
    assert(pet_model_start_activity(&m, PET_ACTIVITY_FEED));
    assert(pet_model_interact(&m) == PET_INTERACT_REFUSED);
    assert(m.fullness == 95);
    assert(m.feed_fish == 3);
    assert(m.activity == PET_ACTIVITY_FEED);
    pet_model_finish_activity(&m);
}

static void test_growth_and_milestone(void) {
    pet_model_t m;
    pet_model_init(&m);
    assert(pet_model_growth_stage(&m) == 0);  // 新档:幼猫

    // 19 轮"吃 3 条 + 刷 5 下"(每轮照顾量 +8,全程累计 152):
    // 成长跨过 50(第 7 轮)与 150(第 19 轮)两个台阶;
    // 里程碑在累计值每满 10 的那一次交互上置位,取走即清零。
    for (int round = 1; round <= 19; round++) {
        pet_model_set_stats(&m, 70, 70, 80);
        uint32_t fish_before = m.feed_fish;
        pet_model_start_activity(&m, PET_ACTIVITY_FEED);
        pet_model_interact(&m);
        pet_model_interact(&m);
        pet_model_interact(&m);  // 第 3 条:ENDED
        // 一轮 3 条,整十数可能落在其中任何一条上(如第 10 条是某轮第 1 条)
        bool fish_crossed = m.feed_fish / PET_MILESTONE_STEP >
                            fish_before / PET_MILESTONE_STEP;
        pet_activity_t hit = pet_model_take_milestone(&m);
        assert(hit == (fish_crossed ? PET_ACTIVITY_FEED : PET_ACTIVITY_NONE));
        assert(m.milestone == PET_ACTIVITY_NONE);  // 取走即清零

        pet_model_start_activity(&m, PET_ACTIVITY_BRUSH);
        for (int i = 0; i < 5; i++) pet_model_interact(&m);
        hit = pet_model_take_milestone(&m);
        if (m.brush_strokes % 10 == 0) {
            assert(hit == PET_ACTIVITY_BRUSH);
        } else {
            assert(hit == PET_ACTIVITY_NONE);
        }

        uint32_t care = m.feed_fish + m.brush_strokes;
        uint8_t expect = care >= PET_GROWTH_T2 ? 2 : (care >= PET_GROWTH_T1 ? 1 : 0);
        assert(pet_model_growth_stage(&m) == expect);
    }
    assert(m.feed_fish == 57);
    assert(m.brush_strokes == 95);
    assert(pet_model_growth_stage(&m) == 2);  // 152 ≥ 150:成年
}

static void test_brush_shine(void) {
    pet_model_t m;
    pet_model_init(&m);
    pet_model_set_stats(&m, 70, 30, 50);
    assert(pet_model_start_activity(&m, PET_ACTIVITY_BRUSH));
    for (int i = 1; i <= 4; i++) {
        assert(pet_model_interact(&m) == PET_INTERACT_BRUSHED);
        assert(m.brush_strokes == (uint32_t)i);
    }
    // 第 5 下发亮并给心情小奖励,活动圆满结束
    assert(pet_model_interact(&m) == PET_INTERACT_SHINE);
    assert(m.happy == 55);
    // 干净 +8/下 × 5 = 40 → 70
    assert(m.clean == 70);
    assert(m.activity == PET_ACTIVITY_NONE);

    // 再刷一轮:重新开始,第 5 下再次发亮
    assert(pet_model_start_activity(&m, PET_ACTIVITY_BRUSH));
    for (int i = 0; i < 4; i++) pet_model_interact(&m);
    assert(pet_model_interact(&m) == PET_INTERACT_SHINE);
    pet_model_finish_activity(&m);
}

static void test_sun_walk_auto_finish(void) {
    pet_model_t m;
        pet_model_init(&m);
    m.last_ms = sim_now;  // 同步到仿真时钟,避免首个 tick 的 60s 封顶 delta
    pet_model_set_stats(&m, 70, 70, 50);

    // 晒太阳:3s 后被确定键提前唤醒 → 自动结算 +40
    assert(pet_model_start_activity(&m, PET_ACTIVITY_SUN));
    sim_advance(&m, 3000, 250);
    assert(pet_model_interact(&m) == PET_INTERACT_WOKE);
    assert(m.activity == PET_ACTIVITY_NONE);
    assert(m.happy == 90);
    assert(m.sun_count == 1);

    // 晒太阳:8s 自动结束(tick 内结算),90 + 40 夹到 100
    assert(pet_model_start_activity(&m, PET_ACTIVITY_SUN));
    sim_advance(&m, PET_SUN_DURATION_MS + 500, 250);
    assert(m.activity == PET_ACTIVITY_NONE);
    assert(m.sun_count == 2);
    assert(m.happy == 100);

    // 散步:扑跳一次,12s 自动结束;心情已满,结算不再涨
    assert(pet_model_start_activity(&m, PET_ACTIVITY_WALK));
    assert(pet_model_interact(&m) == PET_INTERACT_JUMPED);
    sim_advance(&m, PET_WALK_DURATION_MS + 1000, 250);
    assert(m.activity == PET_ACTIVITY_NONE);
    assert(m.walk_count == 1);
    assert(m.happy == 100);

    // 压低心情后散步:结算 +35(50 → 85)
    pet_model_set_stats(&m, 70, 70, 50);
    assert(pet_model_start_activity(&m, PET_ACTIVITY_WALK));
    sim_advance(&m, PET_WALK_DURATION_MS + 500, 250);
    assert(m.activity == PET_ACTIVITY_NONE);
    assert(m.happy == 85);
    assert(m.walk_count == 2);
}

static void test_want_bubble(void) {
    pet_model_t m;
    pet_model_init(&m);
    assert(pet_model_want(&m) == PET_WANT_NONE);  // 70/70/80 无需求

    pet_model_set_stats(&m, 38, 60, 50);
    assert(pet_model_want(&m) == PET_WANT_FOOD);

    pet_model_set_stats(&m, 60, 35, 50);
    assert(pet_model_want(&m) == PET_WANT_BRUSH);

    pet_model_set_stats(&m, 60, 70, 30);
    assert(pet_model_want(&m) == PET_WANT_WALK);

    // 并列最低:按 饱食 > 干净 > 心情 优先
    pet_model_set_stats(&m, 30, 30, 90);
    assert(pet_model_want(&m) == PET_WANT_FOOD);
    pet_model_set_stats(&m, 90, 30, 30);
    assert(pet_model_want(&m) == PET_WANT_BRUSH);
}

static void test_idle_meow(void) {
    pet_model_t m;
        pet_model_init(&m);
    m.last_ms = sim_now;  // 同步到仿真时钟,避免首个 tick 的 60s 封顶 delta
    // 空闲 19s:一次都不喵
    int meows = 0;
    sim_advance(&m, PET_IDLE_MEOW_MIN_MS - 1000, 250);
    for (uint32_t t = 0; t < 60; t++) {
        sim_advance(&m, 1000, 250);
        if (pet_model_should_meow(&m)) meows++;
    }
    // 60 秒窗口、1/25 概率/秒(期望 ~2.4):确定性 rng 下应落在 [1, 12]
    printf("idle meows in 60s window: %d\n", meows);
    assert(meows >= 1 && meows <= 12);

    // 互动会重置空闲计时:喂一口后立刻进入新一轮等待
    pet_model_start_activity(&m, PET_ACTIVITY_FEED);
    pet_model_interact(&m);
    assert(m.idle_ms == 0);
    pet_model_finish_activity(&m);
}

static void test_first_tick_guard(void) {
    pet_model_t m;
    pet_model_init(&m);
    // 首个 tick delta 被 60s 封顶:60s < 12min,状态一点不掉
    pet_model_tick(&m, 10u * 60u * 60u * 1000u);
    assert(m.fullness == 70 && m.clean == 70 && m.happy == 80);
}

int main(void) {
    test_init_and_decay();
    test_decay_floor();
    test_cold_boot();
    test_feed_and_refuse();
    test_growth_and_milestone();
    test_brush_shine();
    test_sun_walk_auto_finish();
    test_want_bubble();
    test_idle_meow();
    test_first_tick_guard();
    printf("pet_model: all tests passed\n");
    return 0;
}
