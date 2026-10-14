// main/pet_font.c —— 中文字体审计实现(LVGL 锁内调用)。
#include "pet_font.h"

#include "esp_log.h"

static const char *TAG = "kaka_font";

const lv_font_t *pet_font_cjk16(void) {
    return &app_font_16;
}

const lv_font_t *pet_font_cjk20(void) {
    return &app_font_20;
}

// UTF-8 解码器:返回码点,*adv 为该字符字节数;非法序列返回 0xFFFD。
static uint32_t utf8_next(const char *s, size_t *adv) {
    const unsigned char *p = (const unsigned char *)s;
    if (p[0] < 0x80) {
        *adv = 1;
        return p[0];
    }
    if ((p[0] & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80) {
        *adv = 2;
        return ((uint32_t)(p[0] & 0x1F) << 6) | (p[1] & 0x3F);
    }
    if ((p[0] & 0xF0) == 0xE0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
        *adv = 3;
        return ((uint32_t)(p[0] & 0x0F) << 12) | ((uint32_t)(p[1] & 0x3F) << 6) |
               (p[2] & 0x3F);
    }
    *adv = 1;
    return 0xFFFD;
}

static bool font_has(const lv_font_t *font, uint32_t cp) {
    lv_font_glyph_dsc_t dsc;
    return lv_font_get_glyph_dsc(font, &dsc, cp, 0) && !dsc.is_placeholder;
}

bool pet_font_verify(void) {
    bool ok = true;
    const char *s = PET_GLYPH_SET;
    while (*s) {
        size_t adv;
        uint32_t cp = utf8_next(s, &adv);
        s += adv;
        if (!font_has(&app_font_16, cp) || !font_has(&app_font_20, cp)) {
            ESP_LOGE(TAG, "字体缺字: U+%04X", cp);
            ok = false;
        }
    }
    // 已知缺字反例:龙部生僻字,任何一份字库都不该有它
    if (font_has(&app_font_20, 0x9F98)) {
        ESP_LOGE(TAG, "缺字反例 U+9F98 竟然存在,审计逻辑可疑");
        ok = false;
    }
    if (ok) {
        ESP_LOGI(TAG, "字体审计通过");
    }
    return ok;
}
