// main/app_ui.c —— GLM 额度仪表盘:深底卡片风,一屏呈现 5 小时窗口/周限额的
// 已用百分比、剩余比例条、重置时刻与倒计时;页脚承载轮询状态,右上角状态点+电量。
#include "app_ui.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "lvgl.h"
#include "quota_model.h"

static const char *TAG = "app_ui";

// ---------------------------------------------------------------------------
// 视觉基调:沿用本仓库用户应用的深底浅字;额度充足=青,偏低=金,告急=红。
// ---------------------------------------------------------------------------
#define COL_BG       0x0E1116
#define COL_PANEL    0x1A2028
#define COL_PANEL_HI 0x262F3B
#define COL_BORDER   0x2A323E
#define COL_TEXT     0xF2F4F8
#define COL_SUB      0x8C96A5
#define COL_WORK     0xFF5A3C
#define COL_REST     0x27C2A4
#define COL_EMPTY    0x222833
#define COL_GOLD     0xFFC857
#define COL_WHITE    0xFFFFFF
#define COL_BLUE     0x4C8DFF

// 与 assets/fonts/gen_app_fonts.sh 的字符清单保持一致;开机自检逐字核对。
static const char APP_GLYPH_SET[] =
    "额度小时周限额刷新连接未配置更新下次失败重试长按返回热点手机浏览器"
    "打开正在联网等待稍可设置入启址络错误效法解超长接口拒绝"
    "一二三四五六日响应析客户端初始失败";

LV_FONT_DECLARE(app_font_16);
LV_FONT_DECLARE(app_font_20);

// 中文子集字体不含 LVGL 图标符号,按官方字体指南挂 Montserrat fallback。
static lv_font_t s_font_16_sym;
static lv_font_t s_font_20_sym;

static lv_obj_t *s_scr;                 // 当前屏幕
static lv_obj_t *s_battery;             // 顶层电量标签

// ---- 仪表盘控件(仅当前页有效;切页统一在 new_screen 里清空) ----
static lv_obj_t *s_net_dot;
typedef struct {
    lv_obj_t *panel;
    lv_obj_t *reset_time;               // "14:52 刷新"
    lv_obj_t *num;                      // 已用百分比大数字
    lv_obj_t *bar_bg;                   // 剩余段(暗色)
    lv_obj_t *bar_fill;                 // 已用段(亮色,自左向右生长)
} card_t;
static card_t s_card[2];
static lv_obj_t *s_footer;
static lv_obj_t *s_nokey_hint;
static bool s_dash_nokey;
static quota_data_t s_data;             // 最近一次数据(UI 持副本)
static bool s_from_snapshot;            // 数据来自开机快照(数字显灰)

static lv_obj_t *s_setup_l1;
static lv_obj_t *s_setup_l2;
static lv_obj_t *s_setup_l3;
static lv_obj_t *s_toast;
static uint32_t s_toast_until;          // lv_tick 毫秒;0 = 无提示

// ---------------------------------------------------------------------------
// 小工具
// ---------------------------------------------------------------------------
static lv_obj_t *make_label(lv_obj_t *parent, const char *txt,
                            const lv_font_t *font, lv_color_t color) {
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, txt);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, color, 0);
    return label;
}

static lv_obj_t *make_panel(lv_obj_t *parent, int x, int y, int w, int h,
                            lv_color_t bg) {
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_bg_color(obj, bg, 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_radius(obj, 16, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    return obj;
}

static lv_obj_t *make_circle(lv_obj_t *parent, int d, lv_color_t color) {
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_set_size(obj, d, d);
    lv_obj_set_style_radius(obj, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(obj, color, 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    return obj;
}

static void new_screen(void) {
    if (s_scr) {
        lv_obj_delete(s_scr);           // 同时清理其对象的动画
        s_scr = NULL;
    }
    s_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_scr, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);
    s_toast = NULL;
    s_net_dot = s_footer = s_nokey_hint = NULL;
    memset(s_card, 0, sizeof(s_card));
    s_setup_l1 = s_setup_l2 = s_setup_l3 = NULL;
}

static void load_screen(void) {
    lv_screen_load(s_scr);
}

// ---------------------------------------------------------------------------
// 字体:符号 fallback + 开机字形自检
// ---------------------------------------------------------------------------
static void app_fonts_init(void) {
    s_font_16_sym = app_font_16;
    s_font_16_sym.fallback = &lv_font_montserrat_14;
    s_font_20_sym = app_font_20;
    s_font_20_sym.fallback = &lv_font_montserrat_20;
}

static uint32_t utf8_next(const char **p) {
    const unsigned char *s = (const unsigned char *)(*p);
    uint32_t cp;
    if (*s < 0x80) {
        cp = *s;
        *p += 1;
    } else if ((*s & 0xE0) == 0xC0) {
        cp = (uint32_t)(*s & 0x1F);
        *p += 1;
        cp = (cp << 6) | (*(*p)++ & 0x3F);
    } else {
        cp = (uint32_t)(*s & 0x0F);
        *p += 1;
        cp = (cp << 6) | (*(*p)++ & 0x3F);
        cp = (cp << 6) | (*(*p)++ & 0x3F);
    }
    return cp;
}

// 文档要求:不得靠隐藏缺字方框假装修复;开机即核对全部用字并记录缺口。
static void verify_glyphs(const lv_font_t *font) {
    lv_font_glyph_dsc_t dsc;
    for (const char *p = APP_GLYPH_SET; *p;) {
        uint32_t cp = utf8_next(&p);
        if (!lv_font_get_glyph_dsc(font, &dsc, cp, 0) || dsc.is_placeholder) {
            ESP_LOGE(TAG, "字体缺字: U+%04lX", (unsigned long)cp);
        }
    }
}

// ---------------------------------------------------------------------------
// 限额卡:标题行(名称+重置时刻)/已用百分比大数字/两段式比例条。
// 卡内三条横向分带(标题 8-26 / 数字 30-78 / 比例条 86-94),互不侵入;
// 重置时刻已在标题行右上,不再放倒计时文案。
// 比例条两态强对比:左亮段=已用(随用量生长),右暗段=剩余,同色系不同明度。
static void card_build(uint32_t slot, int y, const char *title) {
    card_t *c = &s_card[slot];
    lv_obj_t *panel = make_panel(s_scr, 12, y, 216, 110, lv_color_hex(COL_PANEL));
    c->panel = panel;

    lv_obj_t *t = make_label(panel, title, &app_font_16, lv_color_hex(COL_SUB));
    lv_obj_set_pos(t, 12, 8);

    c->reset_time = make_label(panel, "--:--", &app_font_16, lv_color_hex(COL_SUB));
    lv_obj_align(c->reset_time, LV_ALIGN_TOP_RIGHT, -12, 8);

    // 已用百分比是用户主看项。
    c->num = make_label(panel, "--", &lv_font_montserrat_48, lv_color_hex(COL_SUB));
    lv_obj_set_pos(c->num, 12, 30);

    c->bar_bg = make_panel(panel, 12, 86, 192, 8, lv_color_hex(COL_EMPTY));
    lv_obj_set_style_radius(c->bar_bg, 4, 0);
    c->bar_fill = make_panel(c->bar_bg, 0, 0, 0, 8, lv_color_hex(COL_REST));
    lv_obj_set_style_radius(c->bar_fill, 4, 0);
}

// 剩余比例 → 主题色:充足=青、偏低=金、告急=红;快照数据一律灰。
static uint32_t color_for(uint32_t bps) {
    if (bps >= 5000) return COL_REST;
    if (bps >= 2000) return COL_GOLD;
    return COL_WORK;
}

// 单卡整绘:大数字=已用百分比;比例条亮段=已用、暗段=剩余;无效数据显示占位。
static void card_update(uint32_t slot, const quota_window_t *w) {
    card_t *c = &s_card[slot];
    if (!c->panel) return;

    if (!w || !w->valid) {
        lv_label_set_text(c->num, "--");
        lv_obj_set_style_text_color(c->num, lv_color_hex(COL_SUB), 0);
        lv_obj_set_style_bg_color(c->bar_bg, lv_color_hex(COL_EMPTY), 0);
        lv_obj_set_size(c->bar_fill, 0, 8);
        return;
    }

    uint32_t bps = quota_remaining_bps(w);
    uint32_t used_bps = 10000U - bps;
    uint32_t color = s_from_snapshot ? COL_SUB : color_for(bps);

    lv_label_set_text_fmt(c->num, "%lu%%", (unsigned long)(used_bps / 100U));
    lv_obj_set_style_text_color(c->num, lv_color_hex(color), 0);

    // 同一主题色两种明度:暗段=剩余,亮段=已用,对比一眼可辨。
    lv_obj_set_style_bg_color(c->bar_bg, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(c->bar_bg, 45, 0);
    lv_obj_set_size(c->bar_fill, (int32_t)(192U * used_bps / 10000U), 8);
    lv_obj_set_style_bg_color(c->bar_fill, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(c->bar_fill, LV_OPA_COVER, 0);
}

// 重置时刻每秒重算;时钟未同步(wall_sec==0)时保持占位。
static void card_update_times(uint32_t slot, const quota_window_t *w,
                              uint64_t wall_sec) {
    card_t *c = &s_card[slot];
    if (!c->panel) return;
    if (!w || !w->valid || !w->reset_ms || wall_sec == 0) {
        lv_label_set_text(c->reset_time, "--:--");
        return;
    }
    char buf[24];
    quota_fmt_reset_time(wall_sec, w->reset_ms, buf, sizeof(buf));
    lv_label_set_text_fmt(c->reset_time, "%s 刷新", buf);
}

void app_ui_show_dash(bool has_key) {
    new_screen();
    s_dash_nokey = !has_key;

    // 套餐名不再做徽章(曾在小屏上与标题重叠),改由页脚前缀展示。
    lv_obj_t *title = make_label(s_scr, "GLM 额度", &app_font_20,
                                 lv_color_hex(COL_TEXT));
    lv_obj_set_pos(title, 14, 10);

    s_net_dot = make_circle(s_scr, 10, lv_color_hex(COL_SUB));
    lv_obj_align(s_net_dot, LV_ALIGN_TOP_RIGHT, -64, 14);

    if (s_dash_nokey) {
        s_nokey_hint = make_label(s_scr, "未配置 API Key", &app_font_20,
                                  lv_color_hex(COL_TEXT));
        lv_obj_align(s_nokey_hint, LV_ALIGN_CENTER, 0, -22);
        lv_obj_t *h2 = make_label(s_scr, "长按 OK 打开配网", &s_font_16_sym,
                                  lv_color_hex(COL_SUB));
        lv_obj_align(h2, LV_ALIGN_CENTER, 0, 22);
        load_screen();
        return;
    }

    card_build(0, 48, "5 小时限额");
    card_build(1, 178, "周限额");

    s_footer = make_label(s_scr, "等待联网...", &s_font_16_sym,
                          lv_color_hex(COL_SUB));
    lv_obj_set_pos(s_footer, 14, 298);

    // 已有数据(开机快照/切页返回)立即整绘。
    card_update(0, &s_data.win5h);
    card_update(1, &s_data.week);
    if (s_data.valid) {
        card_update_times(0, &s_data.win5h, 0);
        card_update_times(1, &s_data.week, 0);
    }
    load_screen();
}

void app_ui_dash_set_quota(const quota_data_t *d, bool from_snapshot) {
    s_data = *d;
    s_from_snapshot = from_snapshot;
    if (!s_card[0].panel) return;       // 不在仪表盘(如配网页),切回时再绘

    card_update(0, &s_data.win5h);
    card_update(1, &s_data.week);
}

void app_ui_dash_set_net(app_ui_net_t net) {
    if (!s_net_dot) return;
    uint32_t color = COL_SUB;
    switch (net) {
    case APP_UI_NET_ONLINE: color = COL_REST; break;
    case APP_UI_NET_CONNECTING: color = COL_GOLD; break;
    case APP_UI_NET_OFFLINE: color = COL_WORK; break;
    case APP_UI_NET_AP: color = COL_BLUE; break;
    }
    lv_obj_set_style_bg_color(s_net_dot, lv_color_hex(color), 0);
}

void app_ui_dash_set_footer(const char *text, bool alert) {
    if (!s_footer) return;
    lv_label_set_text(s_footer, text);
    lv_obj_set_style_text_color(s_footer,
                                lv_color_hex(alert ? COL_WORK : COL_SUB), 0);
}

void app_ui_dash_tick(uint64_t wall_sec) {
    if (!s_card[0].panel) return;
    if (!s_data.valid) return;
    card_update_times(0, &s_data.win5h, wall_sec);
    card_update_times(1, &s_data.week, wall_sec);
}

// ---------------------------------------------------------------------------
// 配网设置页
// ---------------------------------------------------------------------------
void app_ui_show_setup(const char *line1, const char *line2, const char *line3,
                       const char *hint) {
    new_screen();
    lv_obj_t *title = make_label(s_scr, "配网设置", &app_font_20,
                                 lv_color_hex(COL_TEXT));
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 30);

    s_setup_l1 = make_label(s_scr, line1, &app_font_20, lv_color_hex(COL_TEXT));
    lv_obj_align(s_setup_l1, LV_ALIGN_CENTER, 0, -44);
    s_setup_l2 = make_label(s_scr, line2, &app_font_20, lv_color_hex(COL_GOLD));
    lv_obj_align(s_setup_l2, LV_ALIGN_CENTER, 0, -6);
    s_setup_l3 = make_label(s_scr, line3, &app_font_16, lv_color_hex(COL_SUB));
    lv_obj_align(s_setup_l3, LV_ALIGN_CENTER, 0, 38);

    lv_obj_t *hint_label = make_label(s_scr, hint, &s_font_16_sym,
                                      lv_color_hex(COL_SUB));
    lv_obj_align(hint_label, LV_ALIGN_BOTTOM_MID, 0, -14);

    load_screen();
}

void app_ui_setup_update(const char *line1, const char *line2,
                         const char *line3) {
    if (!s_setup_l1) return;
    lv_label_set_text(s_setup_l1, line1);
    lv_label_set_text(s_setup_l2, line2);
    lv_label_set_text(s_setup_l3, line3);
}

// ---------------------------------------------------------------------------
// 电量 / 提示 / 帧 tick
// ---------------------------------------------------------------------------
void app_ui_set_battery(int soc) {
    if (!s_battery) return;
    if (soc < 0) lv_label_set_text(s_battery, "-- %");
    else lv_label_set_text_fmt(s_battery, "%d %%", soc);
}

void app_ui_toast(const char *text) {
    if (!s_scr) return;
    s_toast = make_panel(s_scr, 60, 150, 120, 40, lv_color_hex(COL_REST));
    lv_obj_set_style_radius(s_toast, 20, 0);
    lv_obj_set_style_bg_opa(s_toast, 230, 0);
    lv_obj_t *label = make_label(s_toast, text, &app_font_20,
                                 lv_color_hex(COL_WHITE));
    lv_obj_center(label);
    s_toast_until = lv_tick_get() + 1600;
}

void app_ui_tick(void) {
    if (s_toast && s_toast_until && lv_tick_get() >= s_toast_until) {
        lv_obj_delete(s_toast);
        s_toast = NULL;
        s_toast_until = 0;
    }
}

// ---------------------------------------------------------------------------
// 初始化
// ---------------------------------------------------------------------------
void app_ui_init(void) {
    app_fonts_init();
    verify_glyphs(&app_font_16);
    verify_glyphs(&app_font_20);

    s_battery = make_label(lv_layer_top(), "-- %", &app_font_16,
                           lv_color_hex(COL_SUB));
    lv_obj_align(s_battery, LV_ALIGN_TOP_RIGHT, -14, 12);

    memset(&s_data, 0, sizeof(s_data));
    app_ui_show_dash(true);
    ESP_LOGI(TAG, "仪表盘构建完成: 空闲堆=%u", (unsigned)esp_get_free_heap_size());
}
