// main/pet_font.h —— 卡卡应用的中文字体接入与开机缺字审计。
//
// 字体来源:assets/fonts/gen_app_fonts.sh(Noto Sans SC OFL 1.1 子集)。
// 仓库字体清单要求:新增文案必须同步更新 PET_GLYPH_SET 与生成脚本的
// SYMS 并重新生成;开机逐字审计不通过时,中文显示状态一律记 Unverified。
#pragma once

#include <stdbool.h>

#include "lvgl.h"

LV_FONT_DECLARE(app_font_16);
LV_FONT_DECLARE(app_font_20);

// 应用全部中文文案(与 gen_app_fonts.sh 的 SYMS 一一对应,多出的字仅供审计)
#define PET_GLYPH_SET "\u5361\u5582\u98df\u5237\u6bdb\u6652\u592a\u9633" \
                      "\u6563\u6b65\u9759\u97f3\u5f00\u5173\u7761\u7740\u4e86" \
                      "\u665a\u5b89\u91cf\u9ad8\u4e2d\u56de\u5fc6\u7b2c\u6b21" \
                      "\u957f\u5927\u60f3\u4f60\u600e\u4e48\u73a9\u6362\u59cb" \
                      "\u6309\u89c9\u54aa\u56e2\u5b50\u5e03\u4e01\u7164\u7403\u6a58" \
                      "\u4e0a\u4e0b\u5403\u9971\u6539\u540d"

// 开机逐字审计:PET_GLYPH_SET 的每个码点在 16/20 两份字库里都必须
// 非占位;另含一个已知缺字反例(0x9F98),防止审计空洞通过。
// 全部通过返回 true;缺字打 ESP_LOGE 并返回 false(LVGL 锁内调用)。
bool pet_font_verify(void);

// 纯中文文案用的字体句柄。
const lv_font_t *pet_font_cjk16(void);
const lv_font_t *pet_font_cjk20(void);
