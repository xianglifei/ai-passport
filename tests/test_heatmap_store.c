// tests/test_heatmap_store.c —— 热力图数据模型主机测试:染色/统计/环形淘汰/序列化。
#include "heatmap_store.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_init_empty(void) {
    hm_store_t store;
    hm_init(&store);
    assert(store.version == HM_BLOB_VERSION);
    assert(store.capacity == HM_DAYS);
    for (int i = 0; i < HM_DAYS; i++) {
        assert(store.days[i].day_index == HM_DAY_NONE);
    }
    assert(hm_day_find(&store, 100) == NULL);
}

static void test_mark_bucket_bits_independent(void) {
    hm_store_t store;
    hm_init(&store);
    hm_day_t *day = hm_day_acquire(&store, 100);
    assert(day && day->day_index == 100);
    hm_mark_bucket(day, 0, HM_KIND_WORK);
    hm_mark_bucket(day, 1, HM_KIND_REST);
    hm_mark_bucket(day, 2, HM_KIND_WORK | HM_KIND_REST);
    assert(hm_cell_has(day, 0, HM_KIND_WORK));
    assert(!hm_cell_has(day, 0, HM_KIND_REST));
    assert(hm_cell_has(day, 1, HM_KIND_REST));
    assert(!hm_cell_has(day, 1, HM_KIND_WORK));
    assert(hm_cell_has(day, 2, HM_KIND_WORK) && hm_cell_has(day, 2, HM_KIND_REST));
    // 重复染色幂等;越界安全。
    hm_mark_bucket(day, 0, HM_KIND_WORK);
    assert(hm_count_cells(day, HM_KIND_WORK) == 2);
    hm_mark_bucket(day, 288, HM_KIND_WORK);
    hm_mark_bucket(day, 999, HM_KIND_REST);
    assert(day->cells[HM_CELL_BYTES - 1] == 0);
}

static void test_mark_span_clamps(void) {
    hm_store_t store;
    hm_init(&store);
    hm_day_t *day = hm_day_acquire(&store, 7);
    // [0,5) → bucket 0;[5,10) → bucket 1;[8,11) 跨 bucket 1..2。
    hm_mark_span(day, 0, 5, HM_KIND_WORK);
    assert(hm_cell_has(day, 0, HM_KIND_WORK));
    assert(!hm_cell_has(day, 1, HM_KIND_WORK));
    hm_mark_span(day, 8, 11, HM_KIND_REST);
    assert(hm_cell_has(day, 1, HM_KIND_REST) && hm_cell_has(day, 2, HM_KIND_REST));
    // 末尾越界截断到最后一格。
    hm_mark_span(day, 1439, 2000, HM_KIND_WORK);
    assert(hm_cell_has(day, 287, HM_KIND_WORK));
    // 空区间/无效参数。
    hm_mark_span(day, 10, 10, HM_KIND_WORK);
    hm_mark_span(day, 20, 5, HM_KIND_WORK);
    hm_mark_span(day, 0, 5, 0);
    assert(hm_count_cells(day, HM_KIND_WORK) == 2);
    // find 已存在的天,acquire 不会另开槽。
    assert(hm_day_find(&store, 7) == day);
    assert(hm_day_acquire(&store, 7) == day);
}

static void test_eviction_keeps_recent_days(void) {
    hm_store_t store;
    hm_init(&store);
    for (uint16_t d = 0; d < HM_DAYS; d++) {
        hm_day_t *day = hm_day_acquire(&store, d);
        hm_mark_bucket(day, 0, HM_KIND_WORK);
    }
    // 写满:day 0..29 全在。
    for (uint16_t d = 0; d < HM_DAYS; d++) {
        assert(hm_day_find(&store, d) != NULL);
    }
    // 再来一天:挤掉最旧的 day 0。
    hm_day_t *day = hm_day_acquire(&store, 30);
    assert(day && day->day_index == 30);
    assert(hm_day_find(&store, 0) == NULL);
    assert(hm_day_find(&store, 1) != NULL);
    // 再来一天:继续挤掉当前最旧的 day 1。
    hm_day_t *day31 = hm_day_acquire(&store, 31);
    assert(day31->day_index == 31);
    assert(hm_day_find(&store, 1) == NULL);
    assert(hm_day_find(&store, 2) != NULL);
    assert(hm_day_find(&store, 30) != NULL);
}

static void test_sessions_counters(void) {
    hm_store_t store;
    hm_init(&store);
    hm_day_t *day = hm_day_acquire(&store, 500);
    day->work_sessions = 3;
    day->rest_sessions = 2;
    assert(day->work_sessions == 3);
}

static void test_serialize_roundtrip(void) {
    hm_store_t a, b;
    hm_init(&a);
    hm_init(&b);
    hm_day_t *d1 = hm_day_acquire(&a, 1000);
    hm_day_t *d2 = hm_day_acquire(&a, 2000);
    hm_mark_span(d1, 9 * 60, 9 * 60 + 25, HM_KIND_WORK);
    hm_mark_span(d2, 18 * 60, 18 * 60 + 10, HM_KIND_REST);
    d1->work_sessions = 1;
    d2->rest_sessions = 1;

    uint8_t buf[HM_BLOB_SIZE + 16];
    memset(buf, 0xAA, sizeof(buf));
    assert(hm_serialize(&a, buf, sizeof(buf)));
    // 长度必须精确受控,尾部允许填充但头部结构确定。
    assert(hm_deserialize(&b, buf, HM_BLOB_SIZE));
    hm_day_t *r1 = hm_day_find(&b, 1000);
    hm_day_t *r2 = hm_day_find(&b, 2000);
    assert(r1 && r2);
    assert(r1->work_sessions == 1);
    assert(memcmp(r1->cells, d1->cells, HM_CELL_BYTES) == 0);
    assert(memcmp(r2->cells, d2->cells, HM_CELL_BYTES) == 0);
    assert(hm_count_cells(r1, HM_KIND_WORK) == 5);   // 25 分钟 = 5 格
    assert(hm_count_cells(r2, HM_KIND_REST) == 2);   // 10 分钟 = 2 格
    // 未写入的天仍是空槽。
    assert(hm_day_find(&b, 3000) == NULL);
}

static void test_deserialize_rejects_garbage(void) {
    hm_store_t store;
    hm_init(&store);
    hm_day_t *day = hm_day_acquire(&store, 1);
    hm_mark_bucket(day, 3, HM_KIND_WORK);
    uint8_t buf[HM_BLOB_SIZE];
    assert(hm_serialize(&store, buf, sizeof(buf)));
    buf[0] = 99;  // 版本错
    assert(!hm_deserialize(&store, buf, HM_BLOB_SIZE));
    assert(store.days[0].day_index == HM_DAY_NONE);  // 失败后回到空表
    assert(hm_serialize(&store, buf, sizeof(buf)));
    buf[2] = 0x01;  // capacity 错(小端低位)
    assert(!hm_deserialize(&store, buf, HM_BLOB_SIZE));
    assert(!hm_deserialize(&store, buf, 10));        // 长度不足
    assert(!hm_deserialize(&store, NULL, HM_BLOB_SIZE));
}

static void test_blob_size_matches_layout(void) {
    // 头 4 字节 + 每天 6 字节元数据 + 72 字节格子。
    assert(HM_BLOB_SIZE == 4U + 30U * 78U);
    assert(HM_BLOB_SIZE == 2344U);
}

int main(void) {
    test_init_empty();
    test_mark_bucket_bits_independent();
    test_mark_span_clamps();
    test_eviction_keeps_recent_days();
    test_sessions_counters();
    test_serialize_roundtrip();
    test_deserialize_rejects_garbage();
    test_blob_size_matches_layout();
    printf("test_heatmap_store: PASS\n");
    return 0;
}
