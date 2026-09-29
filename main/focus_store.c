// main/focus_store.c —— 番茄会话缓冲,见 focus_store.h。
// 纯逻辑,不依赖 ESP-IDF(主机测试编译本文件);NVS 读写薄层在 app_store.c。
#include "focus_store.h"

#include <string.h>

// blob 布局:u16 版本 + u16 count + count × {u8 type + u8 保留 + u64 start + u64 end}
// 逐字段小端写入,不依赖结构体填充。
#define FOCUS_BUF_VERSION 1

void focus_store_init(focus_store_t *s) {
    memset(s, 0, sizeof(*s));
}

bool focus_store_add(focus_store_t *s, uint8_t type,
                     uint64_t start_ms, uint64_t end_ms) {
    if (s->count >= FOCUS_STORE_CAP) return false;
    focus_rec_t *r = &s->recs[s->count++];
    r->type = type;
    r->start_ms = start_ms;
    r->end_ms = end_ms;
    return true;
}

void focus_store_drop_oldest(focus_store_t *s, uint16_t n) {
    if (n >= s->count) {
        s->count = 0;
        return;
    }
    memmove(s->recs, s->recs + n, (size_t)(s->count - n) * sizeof(s->recs[0]));
    s->count = (uint16_t)(s->count - n);
}

static void put_u16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static uint16_t get_u16(const uint8_t *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}

static void put_u64(uint8_t *p, uint64_t v) {
    for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i));
}

static uint64_t get_u64(const uint8_t *p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) v = (v << 8) | p[i];
    return v;
}

size_t focus_store_serialize(const focus_store_t *s, uint8_t *buf, size_t len) {
    size_t need = 4 + (size_t)s->count * 24;
    if (len < need) return 0;
    put_u16(buf, FOCUS_BUF_VERSION);
    put_u16(buf + 2, s->count);
    uint8_t *p = buf + 4;
    for (uint16_t i = 0; i < s->count; i++) {
        p[0] = s->recs[i].type;
        p[1] = 0;
        put_u64(p + 2, s->recs[i].start_ms);
        put_u64(p + 10, s->recs[i].end_ms);
        p += 24;
    }
    return need;
}

bool focus_store_deserialize(focus_store_t *s, const uint8_t *buf, size_t len) {
    focus_store_init(s);
    if (len < 4 || get_u16(buf) != FOCUS_BUF_VERSION) return false;
    uint16_t count = get_u16(buf + 2);
    if (count > FOCUS_STORE_CAP || len < 4 + (size_t)count * 24) return false;
    const uint8_t *p = buf + 4;
    for (uint16_t i = 0; i < count; i++) {
        s->recs[i].type = p[0];
        s->recs[i].start_ms = get_u64(p + 2);
        s->recs[i].end_ms = get_u64(p + 10);
        p += 24;
    }
    s->count = count;
    return true;
}
