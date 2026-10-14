// main/pet_nvs.h —— 卡卡存档的 NVS 胶水(薄封装,不含业务逻辑)。
//
// 命名空间 "kaka_pet";键:"stats" blob / "mute" u8。
// 任何失败都不致命:设备继续运行,只是不持久化。
#pragma once

#include <stdbool.h>

#include "pet_model.h"

// 初始化 NVS(含 NO_FREE_PAGES/NEW_VERSION_FOUND 的擦除重试);失败返回 false。
bool pet_nvs_init(void);

// 读取存档到 m;无存档或损坏返回 false(此时应走 cold_boot 流程)。
bool pet_nvs_load(pet_model_t *m);

// 保存存档;失败仅记录,不阻断游戏。
bool pet_nvs_save(const pet_model_t *m);

bool pet_nvs_mute(void);          // 当前是否静音(默认不静音)
void pet_nvs_set_mute(bool on);   // 静音开关持久化
