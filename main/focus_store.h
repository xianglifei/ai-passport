// main/focus_store.h —— 番茄会话本地缓冲:NVS 环形暂存,在线批量上报后清空。
// 纯逻辑部分(增删/序列化)主机测试覆盖(tests/test_focus_store.c);NVS IO 为薄层。
// 记录时间戳一律为**真 UTC 毫秒**(记录点已折算,直接是上传线格式)。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define FOCUS_STORE_CAP 200          // 满则丢弃新会话(约够数周断网),上报侧单批上限 2000
#define FOCUS_TYPE_WORK 1
#define FOCUS_TYPE_REST 2

typedef struct {
    uint8_t type;                    // FOCUS_TYPE_*
    uint64_t start_ms;               // 真 UTC 毫秒
    uint64_t end_ms;
} focus_rec_t;

typedef struct {
    uint16_t count;                  // 待上报条数
    focus_rec_t recs[FOCUS_STORE_CAP];
} focus_store_t;

void focus_store_init(focus_store_t *s);
// 追加一条;缓冲满返回 false(调用方丢日志即可,会话本身不受影响)。
bool focus_store_add(focus_store_t *s, uint8_t type,
                     uint64_t start_ms, uint64_t end_ms);
// 上报成功后移除最旧 n 条。
void focus_store_drop_oldest(focus_store_t *s, uint16_t n);

// 序列化(版本头+逐字段小端),buf 至少 FOCUS_STORE_BLOB_SIZE;返回实际长度。
#define FOCUS_STORE_BLOB_SIZE (4 + FOCUS_STORE_CAP * 24)
size_t focus_store_serialize(const focus_store_t *s, uint8_t *buf, size_t len);
// 反序列化;非法数据返回 false 且 s 被重置为空。
bool focus_store_deserialize(focus_store_t *s, const uint8_t *buf, size_t len);
// NVS 读写薄层在 app_store.c(app_store_load_focus_buf / app_store_save_focus_buf)。
