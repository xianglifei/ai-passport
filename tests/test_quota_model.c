// tests/test_quota_model.c —— 配额数据模型主机测试:JSON 解析/比例/倒计时/重置钟面。
#include "quota_model.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "app_clock.h"

// 实测接口响应(2026-09-29,字段与真实返回一致,数值脱敏)。
static const char *REAL_BODY =
    "{"
    "\"code\":200,\"msg\":\"操作成功\","
    "\"data\":{\"limits\":["
    "{\"type\":\"CREDIT_LIMIT\",\"unit\":3,\"number\":5,\"usage\":12000,"
    "\"currentValue\":485,\"remaining\":11515,\"percentage\":4,"
    "\"nextResetTime\":1790658074125},"
    "{\"type\":\"CREDIT_LIMIT\",\"unit\":6,\"number\":1,\"usage\":60000,"
    "\"currentValue\":7349,\"remaining\":52651,\"percentage\":12,"
    "\"nextResetTime\":1790775412984}"
    "],\"level\":\"pro\"},"
    "\"success\":true}";

static void test_parse_real_shape(void) {
    quota_data_t d;
    assert(quota_parse_json(REAL_BODY, &d));
    assert(d.valid);
    assert(strcmp(d.level, "pro") == 0);
    assert(d.win5h.valid);
    assert(d.win5h.total == 12000);
    assert(d.win5h.used == 485);
    assert(d.win5h.remaining == 11515);
    assert(d.win5h.reset_ms == 1790658074125ULL);
    assert(d.week.valid);
    assert(d.week.total == 60000);
    assert(d.week.used == 7349);
    assert(d.week.remaining == 52651);
    assert(d.week.reset_ms == 1790775412984ULL);
}

static void test_parse_missing_remaining_falls_back(void) {
    const char *body =
        "{\"data\":{\"limits\":[{\"unit\":3,\"number\":5,\"usage\":1000,"
        "\"currentValue\":250}],\"level\":\"lite\"},\"success\":true}";
    quota_data_t d;
    assert(quota_parse_json(body, &d));
    assert(d.win5h.valid);
    assert(d.win5h.remaining == 750);
    assert(d.win5h.reset_ms == 0);
    assert(!d.week.valid);
    assert(strcmp(d.level, "lite") == 0);
}

static void test_parse_rejects_bad_payloads(void) {
    quota_data_t d;
    assert(!quota_parse_json(NULL, &d));
    assert(!quota_parse_json("not json", &d));
    assert(!quota_parse_json("{\"code\":401,\"success\":false,\"msg\":\"验证不正确\"}", &d));
    assert(!quota_parse_json("{\"success\":true,\"data\":{}}", &d));
    assert(!quota_parse_json("{\"success\":true}", &d));
    // usage=0 的窗口视为不可用,但不拖垮整体。
    assert(quota_parse_json(
        "{\"data\":{\"limits\":[{\"unit\":3,\"number\":5,\"usage\":0},"
        "{\"unit\":6,\"number\":1,\"usage\":100,\"currentValue\":1,"
        "\"remaining\":99,\"nextResetTime\":123}]},\"success\":true}", &d));
    assert(!d.win5h.valid);
    assert(d.week.valid);
    assert(d.valid);
    // 空串 out 恒被重置。
    memset(&d, 0x5A, sizeof(d));
    assert(!quota_parse_json("", &d));
    assert(!d.valid && !d.win5h.valid && !d.week.valid);
}

static void test_remaining_bps(void) {
    quota_window_t w = { .valid = true, .total = 12000, .remaining = 12000 };
    assert(quota_remaining_bps(&w) == 10000);
    w.remaining = 6000;
    assert(quota_remaining_bps(&w) == 5000);
    w.remaining = 1;
    assert(quota_remaining_bps(&w) == 0);        // 整数截断
    w.remaining = 0;
    assert(quota_remaining_bps(&w) == 0);
    quota_window_t bad = { .valid = false, .total = 0, .remaining = 100 };
    assert(quota_remaining_bps(&bad) == 0);
    assert(quota_remaining_bps(NULL) == 0);
}

static void test_fmt_reset_time(void) {
    char buf[32];
    // 2026-09-28 是周一;本地口径的墙钟秒直接用 civil 工具构造。
    uint16_t mon = app_clock_days_from_civil(2026, 9, 28);
    assert(app_clock_weekday_mon0(mon) == 0);
    uint64_t noon = app_clock_wall_from_hm(mon, 12, 0);

    quota_fmt_reset_time(noon, 0, buf, sizeof(buf));
    assert(strcmp(buf, "--:--") == 0);
    // 同日 14:52 → 只显示钟点。
    quota_fmt_reset_time(noon, (uint64_t)app_clock_wall_from_hm(mon, 14, 52) * 1000ULL,
                         buf, sizeof(buf));
    assert(strcmp(buf, "14:52") == 0);
    // 次日(周二)00:00 → 带星期前缀。
    quota_fmt_reset_time(noon, (uint64_t)app_clock_wall_from_hm(mon + 1, 0, 0) * 1000ULL,
                         buf, sizeof(buf));
    assert(strcmp(buf, "周二 00:00") == 0);
    // 下周一(7 天后)。
    quota_fmt_reset_time(noon, (uint64_t)app_clock_wall_from_hm(mon + 7, 8, 5) * 1000ULL,
                         buf, sizeof(buf));
    assert(strcmp(buf, "周一 08:05") == 0);
}

int main(void) {
    test_parse_real_shape();
    test_parse_missing_remaining_falls_back();
    test_parse_rejects_bad_payloads();
    test_remaining_bps();
    test_fmt_reset_time();
    printf("test_quota_model: all passed\n");
    return 0;
}
