// main/pet_store.h —— 卡卡存档序列化(纯逻辑,host 可测)。
//
// blob 布局(小端,v1):
//   u8 version | u8 full | u8 clean | u8 happy
//   u32 feed_fish | u32 brush_strokes | u32 walk_count | u32 sun_count | u32 rng
// 共 24 字节。NVS 读写胶水见 pet_nvs.h。
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "pet_model.h"

#define PET_STORE_BLOB_VERSION 1
#define PET_STORE_BLOB_SIZE 24

// 序列化到 buf(容量至少 PET_STORE_BLOB_SIZE);返回写入字节数,容量不足返回 0。
size_t pet_store_serialize(const pet_model_t *m, uint8_t *buf, size_t cap);

// 反序列化;版本不符/长度不足/状态越界一律拒绝并返回 false(此时 m 不变)。
bool pet_store_deserialize(pet_model_t *m, const uint8_t *buf, size_t len);
