#include "heatmap_store.h"

#include <string.h>

void hm_init(hm_store_t *store) {
    if (!store) return;
    memset(store, 0, sizeof(*store));
    store->version = HM_BLOB_VERSION;
    store->capacity = HM_DAYS;
    for (int i = 0; i < HM_DAYS; i++) {
        store->days[i].day_index = HM_DAY_NONE;
    }
}

hm_day_t *hm_day_find(hm_store_t *store, uint16_t day_index) {
    if (!store || day_index == HM_DAY_NONE) return NULL;
    for (int i = 0; i < HM_DAYS; i++) {
        if (store->days[i].day_index == day_index) return &store->days[i];
    }
    return NULL;
}

hm_day_t *hm_day_acquire(hm_store_t *store, uint16_t day_index) {
    if (!store || day_index == HM_DAY_NONE) return NULL;
    hm_day_t *slot = hm_day_find(store, day_index);
    if (slot) return slot;
    // 先找空槽;没有就挤掉 day_index 最小(最旧)的记录。
    slot = NULL;
    uint16_t oldest = 0xFFFF;
    for (int i = 0; i < HM_DAYS; i++) {
        if (store->days[i].day_index == HM_DAY_NONE) {
            slot = &store->days[i];
            break;
        }
        if (store->days[i].day_index < oldest) {
            oldest = store->days[i].day_index;
            slot = &store->days[i];
        }
    }
    memset(slot, 0, sizeof(*slot));
    slot->day_index = day_index;
    return slot;
}

void hm_mark_bucket(hm_day_t *day, uint16_t bucket, uint8_t kind_mask) {
    if (!day || kind_mask == 0) return;
    if (bucket >= HM_CELLS_PER_DAY) return;
    day->cells[bucket / 4] |= (uint8_t)(kind_mask << (2 * (bucket % 4)));
}

void hm_mark_span(hm_day_t *day, uint16_t start_min, uint16_t end_min,
                  uint8_t kind_mask) {
    if (!day || kind_mask == 0) return;
    if (end_min <= start_min) return;
    uint16_t first = (uint16_t)(start_min / HM_MINUTES_PER_BUCKET);
    uint16_t last = (uint16_t)((end_min - 1) / HM_MINUTES_PER_BUCKET);
    if (first >= HM_CELLS_PER_DAY) return;
    if (last >= HM_CELLS_PER_DAY) last = HM_CELLS_PER_DAY - 1;
    for (uint16_t b = first; b <= last; b++) {
        hm_mark_bucket(day, b, kind_mask);
    }
}

bool hm_cell_has(const hm_day_t *day, uint16_t bucket, uint8_t kind_mask) {
    if (!day || kind_mask == 0 || bucket >= HM_CELLS_PER_DAY) return false;
    return (day->cells[bucket / 4] >> (2 * (bucket % 4))) & kind_mask;
}

int hm_count_cells(const hm_day_t *day, uint8_t kind_mask) {
    if (!day || kind_mask == 0) return 0;
    int count = 0;
    for (int b = 0; b < HM_CELLS_PER_DAY; b++) {
        if (hm_cell_has(day, (uint16_t)b, kind_mask)) count++;
    }
    return count;
}

static void put16(uint8_t *buf, uint16_t v) {
    buf[0] = (uint8_t)(v & 0xFF);
    buf[1] = (uint8_t)(v >> 8);
}

static uint16_t get16(const uint8_t *buf) {
    return (uint16_t)(buf[0] | (buf[1] << 8));
}

bool hm_serialize(const hm_store_t *store, uint8_t *buf, size_t len) {
    if (!store || !buf) return false;
    if (len < HM_BLOB_SIZE) return false;
    buf[0] = HM_BLOB_VERSION;
    buf[1] = 0;
    put16(buf + 2, HM_DAYS);
    uint8_t *p = buf + 4;
    for (int i = 0; i < HM_DAYS; i++) {
        const hm_day_t *day = &store->days[i];
        put16(p, day->day_index);
        put16(p + 2, day->work_sessions);
        put16(p + 4, day->rest_sessions);
        memcpy(p + 6, day->cells, HM_CELL_BYTES);
        p += 6 + HM_CELL_BYTES;
    }
    return true;
}

bool hm_deserialize(hm_store_t *store, const uint8_t *buf, size_t len) {
    if (!store || !buf) return false;
    hm_init(store);
    if (len < HM_BLOB_SIZE) return false;
    if (buf[0] != HM_BLOB_VERSION) return false;
    if (get16(buf + 2) != HM_DAYS) return false;
    const uint8_t *p = buf + 4;
    for (int i = 0; i < HM_DAYS; i++) {
        hm_day_t *day = &store->days[i];
        day->day_index = get16(p);
        day->work_sessions = get16(p + 2);
        day->rest_sessions = get16(p + 4);
        memcpy(day->cells, p + 6, HM_CELL_BYTES);
        p += 6 + HM_CELL_BYTES;
    }
    return true;
}
