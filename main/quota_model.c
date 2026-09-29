// main/quota_model.c —— 配额响应解析与派生计算,见 quota_model.h。
#include "quota_model.h"

#include <stdio.h>
#include <string.h>

#include "app_clock.h"
#include "cJSON.h"

// 实测取值:unit=3 & number=5 → 5 小时窗口;unit=6 & number=1 → 每周。
// 官方未将此接口写入公开文档,字段口径以实测为准(见 zhipu-glm-plan-usage 技能)。
#define UNIT_HOUR 3
#define UNIT_WEEK 6

static uint32_t num_u32(const cJSON *obj, const char *key) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!cJSON_IsNumber(item)) return 0;
    if (item->valuedouble < 0.0) return 0;
    return (uint32_t)item->valuedouble;
}

static uint64_t num_u64(const cJSON *obj, const char *key) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!cJSON_IsNumber(item) || item->valuedouble < 0.0) return 0;
    return (uint64_t)item->valuedouble;
}

// usage/currentValue/remaining 缺一不致命:remaining 字段缺失时按 total-used 兜底。
static void fill_window(quota_window_t *w, const cJSON *item) {
    uint32_t total = num_u32(item, "usage");
    if (total == 0) {
        memset(w, 0, sizeof(*w));
        return;
    }
    uint32_t used = num_u32(item, "currentValue");
    if (used > total) used = total;
    uint32_t remaining;
    const cJSON *rem = cJSON_GetObjectItemCaseSensitive(item, "remaining");
    if (cJSON_IsNumber(rem) && rem->valuedouble >= 0.0) {
        remaining = (uint32_t)rem->valuedouble;
        if (remaining > total) remaining = total;
    } else {
        remaining = total - used;
    }
    w->valid = true;
    w->total = total;
    w->used = used;
    w->remaining = remaining;
    w->reset_ms = num_u64(item, "nextResetTime");
}

bool quota_parse_json(const char *body, quota_data_t *out) {
    memset(out, 0, sizeof(*out));
    if (!body) return false;

    cJSON *root = cJSON_Parse(body);
    if (!root) return false;

    const cJSON *success = cJSON_GetObjectItemCaseSensitive(root, "success");
    bool ok = cJSON_IsBool(success) && cJSON_IsTrue(success);
    const cJSON *data = cJSON_GetObjectItemCaseSensitive(root, "data");
    const cJSON *limits = data ? cJSON_GetObjectItemCaseSensitive(data, "limits")
                               : NULL;
    if (ok && cJSON_IsArray(limits)) {
        const cJSON *level = cJSON_GetObjectItemCaseSensitive(data, "level");
        if (cJSON_IsString(level) && level->valuestring) {
            strncpy(out->level, level->valuestring, sizeof(out->level) - 1);
        }
        const cJSON *item = NULL;
        cJSON_ArrayForEach(item, limits) {
            int unit = (int)num_u64(item, "unit");
            int number = (int)num_u64(item, "number");
            if (unit == UNIT_HOUR && number == 5) {
                fill_window(&out->win5h, item);
            } else if (unit == UNIT_WEEK && number == 1) {
                fill_window(&out->week, item);
            }
        }
        out->valid = out->win5h.valid || out->week.valid;
    }
    cJSON_Delete(root);
    return out->valid;
}

uint32_t quota_remaining_bps(const quota_window_t *w) {
    if (!w || !w->valid || w->total == 0) return 0;
    return (uint32_t)((uint64_t)w->remaining * 10000U / w->total);
}

void quota_fmt_reset_time(uint64_t now_sec, uint64_t reset_ms,
                          char *buf, size_t len) {
    static const char *const WEEKDAY[] = { "一", "二", "三", "四", "五", "六", "日" };
    if (len == 0) return;
    if (!reset_ms) {
        snprintf(buf, len, "--:--");
        return;
    }
    uint64_t reset_sec = reset_ms / 1000U;
    uint16_t now_day = app_clock_day_index(now_sec);
    uint16_t reset_day = app_clock_day_index(reset_sec);
    uint32_t minute = app_clock_minute_of_day(reset_sec);
    if (reset_day == now_day) {
        snprintf(buf, len, "%02u:%02u", (unsigned)(minute / 60U),
                 (unsigned)(minute % 60U));
    } else {
        snprintf(buf, len, "周%s %02u:%02u",
                 WEEKDAY[app_clock_weekday_mon0(reset_day)],
                 (unsigned)(minute / 60U), (unsigned)(minute % 60U));
    }
}
