// main/pet_store.c —— 卡卡存档序列化实现(纯逻辑,无 ESP-IDF 依赖)。
#include "pet_store.h"

static void put_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

static uint32_t get_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

size_t pet_store_serialize(const pet_model_t *m, uint8_t *buf, size_t cap) {
    if (cap < PET_STORE_BLOB_SIZE) return 0;
    buf[0] = PET_STORE_BLOB_VERSION;
    buf[1] = m->fullness;
    buf[2] = m->clean;
    buf[3] = m->happy;
    put_u32(&buf[4], m->feed_fish);
    put_u32(&buf[8], m->brush_strokes);
    put_u32(&buf[12], m->walk_count);
    put_u32(&buf[16], m->sun_count);
    put_u32(&buf[20], m->rng);
    return PET_STORE_BLOB_SIZE;
}

bool pet_store_deserialize(pet_model_t *m, const uint8_t *buf, size_t len) {
    if (len < PET_STORE_BLOB_SIZE) return false;
    if (buf[0] != PET_STORE_BLOB_VERSION) return false;
    uint8_t full = buf[1], clean = buf[2], happy = buf[3];
    // 状态必须落在合法区间,损坏数据不得进入模型
    if (full < PET_STAT_MIN || full > PET_STAT_MAX) return false;
    if (clean < PET_STAT_MIN || clean > PET_STAT_MAX) return false;
    if (happy < PET_STAT_MIN || happy > PET_STAT_MAX) return false;

    pet_model_t tmp = *m;  // 先写副本,失败不动原模型
    tmp.fullness = full;
    tmp.clean = clean;
    tmp.happy = happy;
    tmp.feed_fish = get_u32(&buf[4]);
    tmp.brush_strokes = get_u32(&buf[8]);
    tmp.walk_count = get_u32(&buf[12]);
    tmp.sun_count = get_u32(&buf[16]);
    tmp.rng = get_u32(&buf[20]);
    *m = tmp;
    return true;
}
