#include "app_clock.h"

#include "heatmap_store.h"   // 5 分钟格常量

uint64_t app_clock_wall_sec(uint64_t boot_epoch, uint64_t now_boot_ms) {
    return boot_epoch + now_boot_ms / 1000ULL;
}

uint16_t app_clock_day_index(uint64_t wall_sec) {
    return (uint16_t)(wall_sec / 86400ULL);
}

uint32_t app_clock_minute_of_day(uint64_t wall_sec) {
    return (uint32_t)((wall_sec % 86400ULL) / 60ULL);
}

uint16_t app_clock_bucket(uint32_t minute_of_day) {
    if (minute_of_day >= 1440U) return HM_CELLS_PER_DAY - 1;
    return (uint16_t)(minute_of_day / HM_MINUTES_PER_BUCKET);
}

// Howard Hinnant civil_from_days:天索引 → y/m/d。
void app_clock_civil(uint16_t day_index, int *year, int *month, int *day) {
    int64_t z = (int64_t)day_index + 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    uint32_t doe = (uint32_t)(z - era * 146097);                      // [0, 146096]
    uint32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t y = (int64_t)yoe + era * 400;
    uint32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);           // [0, 365]
    uint32_t mp = (5 * doy + 2) / 153;                                // [0, 11]
    uint32_t d = doy - (153 * mp + 2) / 5 + 1;                        // [1, 31]
    uint32_t m = mp + (mp < 10 ? 3 : -9);                             // [1, 12]
    if (m <= 2) y += 1;
    if (year) *year = (int)y;
    if (month) *month = (int)m;
    if (day) *day = (int)d;
}

int app_clock_weekday_mon0(uint16_t day_index) {
    // 1970-01-01(第 0 天)是周四:周一为 0 的星期 = (day + 3) % 7。
    return (int)((day_index + 3U) % 7U);
}

// Howard Hinnant days_from_civil:civil 的逆运算。
uint16_t app_clock_days_from_civil(int y, int m, int d) {
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);                        // [0, 399]
    unsigned doy = (unsigned)(153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + (unsigned)d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;            // [0, 146096]
    int64_t z = era * 146097 + (int64_t)doe - 719468;
    return (uint16_t)z;
}

uint64_t app_clock_wall_from_hm(uint16_t day_index, uint32_t hour, uint32_t minute) {
    if (hour > 23U) hour = 23U;
    if (minute > 59U) minute = 59U;
    return (uint64_t)day_index * 86400ULL + hour * 3600ULL + minute * 60ULL;
}
