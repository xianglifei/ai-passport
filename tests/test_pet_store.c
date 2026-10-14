// tests/test_pet_store.c —— 卡卡存档序列化 host 测试(编译 main/pet_store.c)。
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "pet_store.h"

static void test_roundtrip(void) {
    pet_model_t m, out;
    pet_model_init(&m);
    pet_model_set_stats(&m, 42, 77, 88);
    m.feed_fish = 1234;
    m.brush_strokes = 5678;
    m.walk_count = 9;
    m.sun_count = 10;
    m.rng = 0xDEADBEEFu;

    uint8_t buf[PET_STORE_BLOB_SIZE + 8];  // 故意给大,验证只写 BLOB_SIZE
    memset(buf, 0xAA, sizeof(buf));
    assert(pet_store_serialize(&m, buf, sizeof(buf)) == PET_STORE_BLOB_SIZE);
    assert(buf[0] == PET_STORE_BLOB_VERSION);
    assert(buf[1] == 42 && buf[2] == 77 && buf[3] == 88);

    pet_model_init(&out);
    assert(pet_store_deserialize(&out, buf, PET_STORE_BLOB_SIZE));
    assert(out.fullness == 42 && out.clean == 77 && out.happy == 88);
    assert(out.feed_fish == 1234);
    assert(out.brush_strokes == 5678);
    assert(out.walk_count == 9);
    assert(out.sun_count == 10);
    assert(out.rng == 0xDEADBEEFu);
}

static void test_rejects_bad_input(void) {
    pet_model_t m, keep;
    pet_model_init(&m);
    keep = m;

    uint8_t buf[PET_STORE_BLOB_SIZE];
    pet_model_t src;
    pet_model_init(&src);
    pet_store_serialize(&src, buf, sizeof(buf));

    // 容量不足:序列化返回 0
    assert(pet_store_serialize(&src, buf, PET_STORE_BLOB_SIZE - 1) == 0);

    // 长度不足:拒绝
    assert(!pet_store_deserialize(&m, buf, PET_STORE_BLOB_SIZE - 1));
    // 版本不符:拒绝
    uint8_t bad[PET_STORE_BLOB_SIZE];
    memcpy(bad, buf, sizeof(bad));
    bad[0] = PET_STORE_BLOB_VERSION + 1;
    assert(!pet_store_deserialize(&m, bad, sizeof(bad)));
    // 状态越界:拒绝
    memcpy(bad, buf, sizeof(bad));
    bad[1] = 5;  // 低于下限 15
    assert(!pet_store_deserialize(&m, bad, sizeof(bad)));
    memcpy(bad, buf, sizeof(bad));
    bad[2] = 200;  // 高于上限
    assert(!pet_store_deserialize(&m, bad, sizeof(bad)));

    // 失败后模型保持不变
    assert(memcmp(&m, &keep, sizeof(m)) == 0);
}

int main(void) {
    test_roundtrip();
    test_rejects_bad_input();
    printf("pet_store: all tests passed\n");
    return 0;
}
