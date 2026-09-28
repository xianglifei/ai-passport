// tests/test_focus_model.c —— 番茄钟状态机 + 墙钟换算主机测试。
// 纯 C,不依赖 ESP-IDF;由 tools/validate.sh --static 编译运行。
#include "focus_model.h"
#include "app_clock.h"

#include <assert.h>
#include <stdio.h>

static void test_init_is_home(void) {
    focus_model_t model;
    focus_model_init(&model, 1000);
    assert(model.state == FOCUS_STATE_HOME);
    assert(focus_model_remaining_sec(&model, 2000) == 0);
    assert(focus_model_tick(&model, 5000) == FOCUS_EVENT_NONE);
    // HOME 下放弃/跳过都无效(启动才合法)。
    assert(!focus_model_abort(&model));
    assert(!focus_model_skip_celebrate(&model));
}

static void test_work_completes_into_celebrate(void) {
    focus_model_t model;
    focus_model_init(&model, 0);
    assert(focus_model_start_work(&model, 1000));
    assert(model.state == FOCUS_STATE_WORK_RUNNING);
    assert(model.duration_sec == FOCUS_WORK_SEC);
    // 刚启动:剩 1500 秒(向上取整)。
    assert(focus_model_remaining_sec(&model, 1000) == FOCUS_WORK_SEC);
    assert(focus_model_remaining_sec(&model, 1001) == FOCUS_WORK_SEC);
    // 中途:剩 1499。
    assert(focus_model_remaining_sec(&model, 2000) == FOCUS_WORK_SEC - 1);
    assert(focus_model_tick(&model, 1500999) == FOCUS_EVENT_NONE);
    // deadline = 1000 + 25*60*1000 = 1501000。
    assert(focus_model_tick(&model, 1501000) == FOCUS_EVENT_WORK_DONE);
    assert(model.state == FOCUS_STATE_CELEBRATE);
    assert(model.celebrate_end_ms == 1501000 + FOCUS_CELEBRATE_MS);
    // 庆祝期超时自动回首页。
    assert(focus_model_tick(&model, 1501000 + FOCUS_CELEBRATE_MS - 1) ==
           FOCUS_EVENT_NONE);
    assert(focus_model_tick(&model, 1501000 + FOCUS_CELEBRATE_MS) ==
           FOCUS_EVENT_CELEBRATE_DONE);
    assert(model.state == FOCUS_STATE_HOME);
}

static void test_celebrate_skip_by_key(void) {
    focus_model_t model;
    focus_model_init(&model, 0);
    focus_model_start_work(&model, 0);
    focus_model_tick(&model, FOCUS_WORK_SEC * 1000ULL);
    assert(model.state == FOCUS_STATE_CELEBRATE);
    assert(focus_model_skip_celebrate(&model));
    assert(model.state == FOCUS_STATE_HOME);
}

static void test_rest_completes_straight_home(void) {
    focus_model_t model;
    focus_model_init(&model, 0);
    assert(focus_model_start_rest(&model, 500));
    assert(model.state == FOCUS_STATE_REST_RUNNING);
    assert(model.duration_sec == FOCUS_REST_SEC);
    assert(focus_model_remaining_sec(&model, 500) == FOCUS_REST_SEC);
    assert(focus_model_tick(&model, 500 + FOCUS_REST_SEC * 1000ULL - 1) ==
           FOCUS_EVENT_NONE);
    assert(focus_model_tick(&model, 500 + FOCUS_REST_SEC * 1000ULL) ==
           FOCUS_EVENT_REST_DONE);
    assert(model.state == FOCUS_STATE_HOME);
}

static void test_abort_from_running_only(void) {
    focus_model_t model;
    focus_model_init(&model, 0);
    assert(!focus_model_abort(&model));
    focus_model_start_work(&model, 0);
    assert(focus_model_abort(&model));
    assert(model.state == FOCUS_STATE_HOME);
    assert(focus_model_start_rest(&model, 0));
    assert(focus_model_abort(&model));
    assert(model.state == FOCUS_STATE_HOME);
    // 庆祝态要走 skip,不走 abort。
    focus_model_start_work(&model, 0);
    focus_model_tick(&model, FOCUS_WORK_SEC * 1000ULL);
    assert(!focus_model_abort(&model));
    assert(focus_model_skip_celebrate(&model));
}

static void test_full_cycle_sequences(void) {
    focus_model_t model;
    focus_model_init(&model, 0);
    uint64_t now = 100;
    assert(focus_model_start_work(&model, now));
    now += FOCUS_WORK_SEC * 1000ULL;
    assert(focus_model_tick(&model, now) == FOCUS_EVENT_WORK_DONE);
    now += FOCUS_CELEBRATE_MS;
    assert(focus_model_tick(&model, now) == FOCUS_EVENT_CELEBRATE_DONE);
    assert(focus_model_start_rest(&model, now));
    now += FOCUS_REST_SEC * 1000ULL;
    assert(focus_model_tick(&model, now) == FOCUS_EVENT_REST_DONE);
    // 一个完整来回后可以继续开下一个工作。
    assert(focus_model_start_work(&model, now));
}

// ---- app_clock ----

static void test_clock_day_and_minute(void) {
    // 1970-01-01 00:00 本地 = day 0,minute 0。
    assert(app_clock_day_index(0) == 0);
    assert(app_clock_minute_of_day(0) == 0);
    // 00:00:59 → 0 分;00:01:00 → 1 分。
    assert(app_clock_minute_of_day(59) == 0);
    assert(app_clock_minute_of_day(60) == 1);
    // 23:59:59 → 1439 分,仍是 day 0。
    assert(app_clock_minute_of_day(86399) == 1439);
    assert(app_clock_day_index(86399) == 0);
    assert(app_clock_day_index(86400) == 1);
    // 已知时刻:2026-01-01 00:00:00 UTC = 1767225600。
    assert(app_clock_day_index(1767225600ULL) ==
           (uint16_t)(1767225600ULL / 86400ULL));
    assert(app_clock_weekday_mon0((uint16_t)(1767225600ULL / 86400ULL)) == 3);
}

static void test_clock_bucket(void) {
    assert(app_clock_bucket(0) == 0);
    assert(app_clock_bucket(4) == 0);      // 第 0..4 分钟 → 第 0 格
    assert(app_clock_bucket(5) == 1);
    assert(app_clock_bucket(1439) == 287);
    assert(app_clock_bucket(1500) == 287); // 越界夹取
}

static void test_clock_civil(void) {
    int y, m, d;
    app_clock_civil(0, &y, &m, &d);
    assert(y == 1970 && m == 1 && d == 1);
    // 2024-02-29(闰年) = day 19782。
    app_clock_civil(19782, &y, &m, &d);
    assert(y == 2024 && m == 2 && d == 29);
    // 2026-09-28(今天) = day 20724。
    app_clock_civil(20724, &y, &m, &d);
    assert(y == 2026 && m == 9 && d == 28);
    // 2000-02-29 = day 11016。
    app_clock_civil(11016, &y, &m, &d);
    assert(y == 2000 && m == 2 && d == 29);
    // 2100-03-01(2100 非闰年) = day 47541。
    app_clock_civil(47541, &y, &m, &d);
    assert(y == 2100 && m == 3 && d == 1);
}

static void test_clock_weekday(void) {
    // 1970-01-01 周四 → 3。
    assert(app_clock_weekday_mon0(0) == 3);
    // 1970-01-05 周一 → 0。
    assert(app_clock_weekday_mon0(4) == 0);
    // 2026-09-28 是周一(第 20724 天) → 0。
    assert(app_clock_weekday_mon0(20724) == 0);
}

static void test_clock_wall_from_hm(void) {
    assert(app_clock_wall_from_hm(0, 0, 0) == 0);
    assert(app_clock_wall_from_hm(0, 9, 30) == 9 * 3600 + 30 * 60);
    assert(app_clock_wall_from_hm(20724, 23, 59) ==
           20724ULL * 86400ULL + 23 * 3600 + 59 * 60);
    // 越界夹取。
    assert(app_clock_wall_from_hm(0, 24, 60) == 23 * 3600 + 59 * 60);
    assert(app_clock_wall_sec(1000, 1500) == 1001);
}

static void test_clock_days_from_civil_roundtrip(void) {
    // civil ↔ days_from_civil 双向往返。
    static const struct { int y, m, d; } CASES[] = {
        {1970, 1, 1}, {2000, 2, 29}, {2024, 2, 29},
        {2026, 9, 28}, {2100, 3, 1}, {2099, 12, 31},
    };
    for (unsigned i = 0; i < sizeof(CASES) / sizeof(CASES[0]); i++) {
        uint16_t day = app_clock_days_from_civil(CASES[i].y, CASES[i].m, CASES[i].d);
        int y = 0, m = 0, d = 0;
        app_clock_civil(day, &y, &m, &d);
        assert(y == CASES[i].y && m == CASES[i].m && d == CASES[i].d);
    }
    assert(app_clock_days_from_civil(1970, 1, 1) == 0);
    assert(app_clock_days_from_civil(2026, 9, 28) == 20724);
}

int main(void) {
    test_init_is_home();
    test_work_completes_into_celebrate();
    test_celebrate_skip_by_key();
    test_rest_completes_straight_home();
    test_abort_from_running_only();
    test_full_cycle_sequences();
    test_clock_day_and_minute();
    test_clock_bucket();
    test_clock_civil();
    test_clock_weekday();
    test_clock_wall_from_hm();
    test_clock_days_from_civil_roundtrip();
    printf("test_focus_model: PASS\n");
    return 0;
}
