// tests/test_focus_store.c —— 番茄会话缓冲主机测试:增删/序列化回环/容量与非法数据。
#include "focus_store.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_add_and_drop(void) {
    focus_store_t s;
    focus_store_init(&s);
    assert(s.count == 0);
    assert(focus_store_add(&s, FOCUS_TYPE_WORK, 1000, 2000));
    assert(focus_store_add(&s, FOCUS_TYPE_REST, 3000, 4000));
    assert(s.count == 2);
    assert(s.recs[0].type == FOCUS_TYPE_WORK && s.recs[0].start_ms == 1000);
    assert(s.recs[1].type == FOCUS_TYPE_REST && s.recs[1].end_ms == 4000);

    focus_store_drop_oldest(&s, 1);
    assert(s.count == 1 && s.recs[0].type == FOCUS_TYPE_REST);
    focus_store_drop_oldest(&s, 5);            // 超量清空
    assert(s.count == 0);
}

static void test_capacity(void) {
    focus_store_t s;
    focus_store_init(&s);
    for (int i = 0; i < FOCUS_STORE_CAP; i++) {
        assert(focus_store_add(&s, FOCUS_TYPE_WORK, i, i + 1));
    }
    assert(s.count == FOCUS_STORE_CAP);
    assert(!focus_store_add(&s, FOCUS_TYPE_WORK, 999, 1000));   // 满则拒收
    assert(s.count == FOCUS_STORE_CAP);
}

static void test_serialize_roundtrip(void) {
    focus_store_t s, back;
    focus_store_init(&s);
    assert(focus_store_add(&s, FOCUS_TYPE_WORK,
                           1790646623064ULL, 1790648023064ULL));
    assert(focus_store_add(&s, FOCUS_TYPE_REST,
                           1790648023064ULL, 1790648623064ULL));
    uint8_t buf[FOCUS_STORE_BLOB_SIZE];
    size_t len = focus_store_serialize(&s, buf, sizeof(buf));
    assert(len == 4 + 2 * 24);
    assert(focus_store_deserialize(&back, buf, len));
    assert(back.count == 2);
    assert(back.recs[0].start_ms == 1790646623064ULL);
    assert(back.recs[1].type == FOCUS_TYPE_REST);
    // 短缓冲拒绝
    assert(focus_store_serialize(&s, buf, len - 1) == 0);
}

static void test_deserialize_rejects_garbage(void) {
    focus_store_t s;
    uint8_t buf[64] = {0};
    assert(!focus_store_deserialize(&s, buf, 2));              // 太短
    buf[0] = 0x7F;                                             // 版本不符
    buf[1] = 0x00;
    assert(!focus_store_deserialize(&s, buf, sizeof(buf)));
    buf[0] = 0x01;                                             // 版本对,数量超容
    buf[2] = 0xD1;                                             // 200+1? 0xD1=209
    buf[3] = 0x00;
    assert(!focus_store_deserialize(&s, buf, sizeof(buf)));
    // 声明 2 条但数据不够
    buf[2] = 0x02;
    assert(!focus_store_deserialize(&s, buf, 4 + 24));
}

int main(void) {
    test_add_and_drop();
    test_capacity();
    test_serialize_roundtrip();
    test_deserialize_rejects_garbage();
    printf("test_focus_store: all passed\n");
    return 0;
}
