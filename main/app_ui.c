#include "app_ui.h"

#include "app_clock.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "lvgl.h"

static const char *TAG = "app_ui";

// ---------------------------------------------------------------------------
// 视觉基调:深底浅字,工作=番茄红,休息=青绿,庆祝点缀金色。
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

// 与 assets/fonts/gen_app_fonts.sh 的字符清单保持一致;开机自检逐字核对。
static const char APP_GLYPH_SET[] =
    "番茄钟工作休息分钟开始专注放松一下热力图选择确定完成今日第个吧"
    "月周换天返回设置时间保存取消微调快调小时中长按放弃完毕二三四五六日"
    "于对连接已功失败等待机热点正重试浏览打开输入家里网密码页在请上址";

LV_FONT_DECLARE(app_font_16);
LV_FONT_DECLARE(app_font_20);

// 中文子集字体不含 LVGL 图标符号,按官方字体指南挂 Montserrat fallback。
static lv_font_t s_font_16_sym;
static lv_font_t s_font_20_sym;

static lv_obj_t *s_scr;                 // 当前屏幕
static lv_obj_t *s_battery;             // 顶层电量标签

// 各页控件(仅当前页的指针有效;切页统一在 new_screen 里清空)
static lv_obj_t *s_home_cards[3];
static lv_obj_t *s_count_arc;
static lv_obj_t *s_count_time;
static lv_obj_t *s_count_finish;
static lv_obj_t *s_heat_date;
static lv_obj_t *s_heat_stats;
static lv_obj_t *s_time_label;
static lv_obj_t *s_wifi_l1;
static lv_obj_t *s_wifi_l2;
static lv_obj_t *s_wifi_l3;
static lv_obj_t *s_toast;
static uint32_t s_toast_until;          // lv_tick 毫秒;0 = 无提示

// 热力图 288 个格子共享样式:每格只挂 2~3 个共享样式,不再各自持有本地属性,
// 这是 48KB 内存池撑不起 288 个对象导致重启的根因修复。
static lv_style_t s_style_cell_base;
static lv_style_t s_style_cell_empty;
static lv_style_t s_style_cell_work;
static lv_style_t s_style_cell_rest;
static lv_style_t s_style_cell_mixed;
static lv_style_t s_style_cell_today;

static const char *const WEEKDAY_ZH[] = { "一", "二", "三", "四", "五", "六", "日" };
#define SCALE_BASE 256                  // LVGL transform_scale 的 100%

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
    s_count_arc = s_count_time = s_count_finish = NULL;
    s_heat_date = s_heat_stats = s_time_label = NULL;
    s_wifi_l1 = s_wifi_l2 = s_wifi_l3 = NULL;
    for (int i = 0; i < 3; i++) s_home_cards[i] = NULL;
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
// 首页
// ---------------------------------------------------------------------------
static void home_card_style(lv_obj_t *card, bool selected, lv_color_t accent) {
    lv_obj_set_style_bg_color(card, selected ? lv_color_hex(COL_PANEL_HI)
                                             : lv_color_hex(COL_PANEL), 0);
    lv_obj_set_style_border_width(card, selected ? 2 : 1, 0);
    lv_obj_set_style_border_color(card, selected ? accent
                                                 : lv_color_hex(COL_BORDER), 0);
}

static void home_refresh(uint32_t sel) {
    home_card_style(s_home_cards[0], sel == APP_UI_HOME_WORK, lv_color_hex(COL_WORK));
    home_card_style(s_home_cards[1], sel == APP_UI_HOME_REST, lv_color_hex(COL_REST));
    home_card_style(s_home_cards[2], sel == APP_UI_HOME_HEATMAP, lv_color_hex(COL_GOLD));
}

static void home_card(lv_obj_t *parent, uint32_t slot, int y, int h,
                      bool has_dot, lv_color_t dot_color,
                      const char *main_txt, const char *sub_txt) {
    lv_obj_t *card = make_panel(parent, 16, y, 208, h, lv_color_hex(COL_PANEL));
    s_home_cards[slot] = card;
    if (has_dot) {
        lv_obj_t *dot = make_circle(card, 10, dot_color);
        lv_obj_set_pos(dot, 16, h / 2 - 5);
    }
    lv_obj_t *main_label = make_label(card, main_txt, &app_font_20,
                                      lv_color_hex(COL_TEXT));
    lv_obj_align(main_label, LV_ALIGN_LEFT_MID, 36, -9);
    if (sub_txt) {
        lv_obj_t *sub_label = make_label(card, sub_txt, &app_font_16,
                                         lv_color_hex(COL_SUB));
        lv_obj_align(sub_label, LV_ALIGN_LEFT_MID, 36, 13);
    }
}

void app_ui_show_home(uint32_t sel) {
    new_screen();
    lv_obj_t *title = make_label(s_scr, "番茄钟", &app_font_20,
                                 lv_color_hex(COL_TEXT));
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 16, 10);

    home_card(s_scr, APP_UI_HOME_WORK, 44, 86, true, lv_color_hex(COL_WORK),
              "工作 25 分钟", "开始专注");
    home_card(s_scr, APP_UI_HOME_REST, 136, 86, true, lv_color_hex(COL_REST),
              "休息 10 分钟", "放松一下");
    home_card(s_scr, APP_UI_HOME_HEATMAP, 228, 54, false, lv_color_hex(0),
              "热力图", NULL);
    static const uint32_t LEGEND[] = { COL_EMPTY, COL_WORK, COL_REST, COL_WORK, COL_REST };
    for (int i = 0; i < 5; i++) {
        lv_obj_t *sq = make_panel(s_home_cards[APP_UI_HOME_HEATMAP],
                                  136 + i * 14, 22, 10, 10,
                                  lv_color_hex(LEGEND[i]));
        lv_obj_set_style_radius(sq, 2, 0);
    }

    lv_obj_t *hint = make_label(s_scr, LV_SYMBOL_UP LV_SYMBOL_DOWN " 选择   OK 确定",
                                &s_font_16_sym, lv_color_hex(COL_SUB));
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -10);

    home_refresh(sel);
    load_screen();
}

void app_ui_home_select(uint32_t sel) {
    if (!s_home_cards[0]) return;
    home_refresh(sel);
}

// ---------------------------------------------------------------------------
// 倒计时:圆环进度 + 居中大数字 + 完成时刻
// ---------------------------------------------------------------------------
void app_ui_show_countdown(bool work, uint32_t total_sec, bool has_clock) {
    (void)total_sec;
    new_screen();
    uint32_t accent = work ? COL_WORK : COL_REST;

    // 背景光晕(低透明度同色圆),让页面不显得"平"。
    lv_obj_t *glow = make_circle(s_scr, 204, lv_color_hex(accent));
    lv_obj_set_style_bg_opa(glow, 26, 0);
    lv_obj_align(glow, LV_ALIGN_CENTER, 0, -6);

    // 进度环:剩余量从满环逐渐消退。
    s_count_arc = lv_arc_create(s_scr);
    lv_obj_set_size(s_count_arc, 176, 176);
    lv_obj_align(s_count_arc, LV_ALIGN_CENTER, 0, -6);
    lv_arc_set_rotation(s_count_arc, 0);
    lv_arc_set_bg_angles(s_count_arc, 0, 360);
    lv_arc_set_range(s_count_arc, 0, 100);
    lv_arc_set_value(s_count_arc, 100);
    lv_obj_remove_style(s_count_arc, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(s_count_arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_color(s_count_arc, lv_color_hex(COL_EMPTY), LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_count_arc, 10, LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_count_arc, lv_color_hex(accent), LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(s_count_arc, 10, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(s_count_arc, true, LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(s_count_arc, true, LV_PART_INDICATOR);

    lv_obj_t *pill = make_panel(s_scr, 45, 22, 150, 38, lv_color_hex(accent));
    lv_obj_set_style_radius(pill, 19, 0);
    lv_obj_t *pill_label = make_label(pill, work ? "工作中" : "休息中",
                                      &app_font_20, lv_color_hex(COL_WHITE));
    lv_obj_center(pill_label);

    s_count_time = make_label(s_scr, "00:00", &lv_font_montserrat_48,
                              lv_color_hex(COL_TEXT));
    lv_obj_align(s_count_time, LV_ALIGN_CENTER, 0, -6);

    s_count_finish = make_label(s_scr, "", &app_font_16, lv_color_hex(COL_SUB));
    lv_obj_align(s_count_finish, LV_ALIGN_CENTER, 0, 108);
    if (!has_clock) lv_label_set_text(s_count_finish, " ");

    lv_obj_t *hint = make_label(s_scr, "长按 OK 放弃", &app_font_16,
                                lv_color_hex(COL_SUB));
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -12);

    load_screen();
}

void app_ui_countdown_update(uint32_t remain_sec, uint32_t total_sec,
                             int32_t finish_min) {
    if (!s_count_time) return;
    lv_label_set_text_fmt(s_count_time, "%02u:%02u",
                          (unsigned)(remain_sec / 60U),
                          (unsigned)(remain_sec % 60U));
    uint32_t pct = total_sec ? remain_sec * 100U / total_sec : 0;
    if (pct > 100U) pct = 100U;
    lv_arc_set_value(s_count_arc, (int32_t)pct);
    if (finish_min >= 0) {
        lv_label_set_text_fmt(s_count_finish, "完成于 %02u:%02u",
                              (unsigned)finish_min / 60U,
                              (unsigned)finish_min % 60U);
    }
}

// ---------------------------------------------------------------------------
// 完工仪式
// ---------------------------------------------------------------------------
static void anim_scale_cb(void *var, int32_t v) {
    lv_obj_set_style_transform_scale((lv_obj_t *)var, v, 0);
}

static void anim_opa_cb(void *var, int32_t v) {
    lv_obj_set_style_opa((lv_obj_t *)var, (lv_opa_t)v, 0);
}

static void make_ring(int y_offset, uint32_t delay) {
    lv_obj_t *ring = make_circle(s_scr, 120, lv_color_hex(COL_GOLD));
    lv_obj_set_style_bg_opa(ring, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ring, 3, 0);
    lv_obj_set_style_border_color(ring, lv_color_hex(COL_GOLD), 0);
    lv_obj_align(ring, LV_ALIGN_CENTER, 0, y_offset);
    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, ring);
    lv_anim_set_exec_cb(&anim, anim_scale_cb);
    lv_anim_set_values(&anim, SCALE_BASE, SCALE_BASE * 3);
    lv_anim_set_duration(&anim, 1100);
    lv_anim_set_delay(&anim, delay);
    lv_anim_set_repeat_count(&anim, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&anim);
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, ring);
    lv_anim_set_exec_cb(&anim, anim_opa_cb);
    lv_anim_set_values(&anim, 220, 0);
    lv_anim_set_duration(&anim, 1100);
    lv_anim_set_delay(&anim, delay);
    lv_anim_set_repeat_count(&anim, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&anim);
}

void app_ui_show_celebrate(uint16_t tomato_count) {
    new_screen();
    make_ring(-40, 0);
    make_ring(-40, 550);

    lv_obj_t *tomato = make_circle(s_scr, 84, lv_color_hex(COL_WORK));
    lv_obj_align(tomato, LV_ALIGN_CENTER, 0, -40);
    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, tomato);
    lv_anim_set_exec_cb(&anim, anim_scale_cb);
    lv_anim_set_values(&anim, SCALE_BASE - 24, SCALE_BASE + 24);
    lv_anim_set_duration(&anim, 600);
    lv_anim_set_playback_duration(&anim, 600);
    lv_anim_set_repeat_count(&anim, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&anim);

    lv_obj_t *title = make_label(s_scr, "专注完成！", &app_font_20,
                                 lv_color_hex(COL_TEXT));
    lv_obj_align(title, LV_ALIGN_CENTER, 0, 64);

    lv_obj_t *sub = make_label(s_scr, "今日第 1 个番茄", &app_font_16,
                               lv_color_hex(COL_GOLD));
    lv_obj_align(sub, LV_ALIGN_CENTER, 0, 98);
    if (tomato_count > 0) {
        lv_label_set_text_fmt(sub, "今日第 %u 个番茄", (unsigned)tomato_count);
    }

    lv_obj_t *tail = make_label(s_scr, "去休息一下吧", &app_font_16,
                                lv_color_hex(COL_SUB));
    lv_obj_align(tail, LV_ALIGN_BOTTOM_MID, 0, -34);

    load_screen();
}

// ---------------------------------------------------------------------------
// 热力图
// ---------------------------------------------------------------------------
static void cell_styles_init(void) {
    lv_style_init(&s_style_cell_base);
    lv_style_set_pad_all(&s_style_cell_base, 0);
    lv_style_set_radius(&s_style_cell_base, 2);
    lv_style_set_border_width(&s_style_cell_base, 0);
    lv_style_set_bg_opa(&s_style_cell_base, LV_OPA_COVER);

    lv_style_init(&s_style_cell_empty);
    lv_style_set_bg_color(&s_style_cell_empty, lv_color_hex(COL_EMPTY));
    lv_style_init(&s_style_cell_work);
    lv_style_set_bg_color(&s_style_cell_work, lv_color_hex(COL_WORK));
    lv_style_init(&s_style_cell_rest);
    lv_style_set_bg_color(&s_style_cell_rest, lv_color_hex(COL_REST));
    lv_style_init(&s_style_cell_mixed);
    lv_style_set_bg_color(&s_style_cell_mixed, lv_color_hex(COL_GOLD));

    lv_style_init(&s_style_cell_today);
    lv_style_set_border_width(&s_style_cell_today, 1);
    lv_style_set_border_color(&s_style_cell_today, lv_color_hex(COL_TEXT));
    lv_style_set_radius(&s_style_cell_today, 2);
}

void app_ui_show_heatmap(uint16_t day_index, const hm_day_t *day,
                         bool is_today, uint16_t today_bucket, bool clock_unset) {
    new_screen();
    lv_obj_t *title = make_label(s_scr, "热力图", &app_font_20,
                                 lv_color_hex(COL_TEXT));
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 16, 10);

    if (clock_unset) {
        lv_obj_t *tip = make_label(s_scr, "请先设置时间", &app_font_20,
                                   lv_color_hex(COL_TEXT));
        lv_obj_align(tip, LV_ALIGN_CENTER, 0, -20);
        lv_obj_t *hint = make_label(s_scr, "OK 设置时间", &app_font_16,
                                    lv_color_hex(COL_SUB));
        lv_obj_align(hint, LV_ALIGN_CENTER, 0, 24);
        load_screen();
        return;
    }

    int y = 0, m = 0, d = 0;
    app_clock_civil(day_index, &y, &m, &d);
    s_heat_date = make_label(s_scr, "", &app_font_16, lv_color_hex(COL_SUB));
    lv_label_set_text_fmt(s_heat_date, "%d月%d日 周%s", m, d,
                          WEEKDAY_ZH[app_clock_weekday_mon0(day_index)]);
    lv_obj_align(s_heat_date, LV_ALIGN_TOP_RIGHT, -16, 14);

    int work_cells = day ? hm_count_cells(day, HM_KIND_WORK) : 0;
    int rest_cells = day ? hm_count_cells(day, HM_KIND_REST) : 0;
    s_heat_stats = make_label(s_scr, "", &app_font_16, lv_color_hex(COL_TEXT));
    lv_label_set_text_fmt(s_heat_stats, "专注 %d 分 · 休息 %d 分",
                          work_cells * 5, rest_cells * 5);
    lv_obj_align(s_heat_stats, LV_ALIGN_TOP_LEFT, 16, 36);

    // 12 列(每列 1 小时) × 24 行 = 288 格;x=19, y=56 起。
    const int cell_w = 15, cell_h = 8, gap_x = 2, gap_y = 1;
    const int x0 = 19, y0 = 56;
    for (int b = 0; b < HM_CELLS_PER_DAY; b++) {
        int col = b % 12;
        int row = b / 12;
        bool has_work = day && hm_cell_has(day, (uint16_t)b, HM_KIND_WORK);
        bool has_rest = day && hm_cell_has(day, (uint16_t)b, HM_KIND_REST);
        const lv_style_t *bg = &s_style_cell_empty;
        if (has_work && has_rest) bg = &s_style_cell_mixed;
        else if (has_work) bg = &s_style_cell_work;
        else if (has_rest) bg = &s_style_cell_rest;
        lv_obj_t *cell = lv_obj_create(s_scr);
        lv_obj_set_pos(cell, x0 + col * (cell_w + gap_x),
                       y0 + row * (cell_h + gap_y));
        lv_obj_set_size(cell, cell_w, cell_h);
        lv_obj_add_style(cell, &s_style_cell_base, 0);
        lv_obj_add_style(cell, bg, 0);
        if (is_today && (uint16_t)b == today_bucket) {
            lv_obj_add_style(cell, &s_style_cell_today, 0);
        }
    }

    // 内存诊断:288 个对象的页面是最吃 LVGL 池的地方,构建后留档。
    lv_mem_monitor_t mon;
    lv_mem_monitor(&mon);
    ESP_LOGI(TAG, "热力图构建完成: LVGL池 used=%u%% frag=%u%%, 空闲堆=%u",
             (unsigned)mon.used_pct, (unsigned)mon.frag_pct,
             (unsigned)esp_get_free_heap_size());

    lv_obj_t *hint = make_label(s_scr, LV_SYMBOL_UP LV_SYMBOL_DOWN " 换天   OK 返回",
                                &s_font_16_sym, lv_color_hex(COL_SUB));
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -10);

    load_screen();
}

// ---------------------------------------------------------------------------
// 时间设置
// ---------------------------------------------------------------------------
void app_ui_show_timeset(uint32_t minute_of_day, bool forced) {
    new_screen();
    lv_obj_t *title = make_label(s_scr, "设置时间", &app_font_20,
                                 lv_color_hex(COL_TEXT));
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 26);

    s_time_label = make_label(s_scr, "00:00", &lv_font_montserrat_48,
                              lv_color_hex(COL_TEXT));
    lv_obj_align(s_time_label, LV_ALIGN_CENTER, 0, -34);

    lv_obj_t *hint1 = make_label(s_scr, LV_SYMBOL_UP LV_SYMBOL_DOWN " 一次 5 分钟",
                                 &s_font_16_sym, lv_color_hex(COL_SUB));
    lv_obj_align(hint1, LV_ALIGN_CENTER, 0, 34);
    lv_obj_t *hint2 = make_label(s_scr, "长按 " LV_SYMBOL_UP " 快调 1 小时",
                                 &s_font_16_sym, lv_color_hex(COL_SUB));
    lv_obj_align(hint2, LV_ALIGN_CENTER, 0, 60);
    lv_obj_t *hint3 = make_label(s_scr, "长按 " LV_SYMBOL_DOWN " Wi-Fi 对时",
                                 &s_font_16_sym, lv_color_hex(COL_SUB));
    lv_obj_align(hint3, LV_ALIGN_CENTER, 0, 86);
    lv_obj_t *hint4 = make_label(s_scr, forced ? "OK 保存" : "OK 保存 · 长按 OK 取消",
                                 &s_font_16_sym, lv_color_hex(COL_SUB));
    lv_obj_align(hint4, LV_ALIGN_CENTER, 0, 112);

    app_ui_timeset_update(minute_of_day);
    load_screen();
}

void app_ui_timeset_update(uint32_t minute_of_day) {
    if (!s_time_label) return;
    lv_label_set_text_fmt(s_time_label, "%02u:%02u",
                          (unsigned)(minute_of_day / 60U),
                          (unsigned)(minute_of_day % 60U));
}

// ---------------------------------------------------------------------------
// Wi-Fi 对时
// ---------------------------------------------------------------------------
void app_ui_show_wifi(const char *line1, const char *line2, const char *line3,
                      const char *hint) {
    new_screen();
    lv_obj_t *title = make_label(s_scr, "Wi-Fi 对时", &app_font_20,
                                 lv_color_hex(COL_TEXT));
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 30);

    s_wifi_l1 = make_label(s_scr, line1, &app_font_20, lv_color_hex(COL_TEXT));
    lv_obj_align(s_wifi_l1, LV_ALIGN_CENTER, 0, -44);
    s_wifi_l2 = make_label(s_scr, line2, &app_font_20, lv_color_hex(COL_GOLD));
    lv_obj_align(s_wifi_l2, LV_ALIGN_CENTER, 0, -6);
    s_wifi_l3 = make_label(s_scr, line3, &app_font_16, lv_color_hex(COL_SUB));
    lv_obj_align(s_wifi_l3, LV_ALIGN_CENTER, 0, 38);

    lv_obj_t *hint_label = make_label(s_scr, hint, &s_font_16_sym,
                                      lv_color_hex(COL_SUB));
    lv_obj_align(hint_label, LV_ALIGN_BOTTOM_MID, 0, -14);

    load_screen();
}

void app_ui_wifi_update(const char *line1, const char *line2,
                        const char *line3) {
    if (!s_wifi_l1) return;
    lv_label_set_text(s_wifi_l1, line1);
    lv_label_set_text(s_wifi_l2, line2);
    lv_label_set_text(s_wifi_l3, line3);
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
    cell_styles_init();
    verify_glyphs(&app_font_16);
    verify_glyphs(&app_font_20);

    s_battery = make_label(lv_layer_top(), "-- %", &app_font_16,
                           lv_color_hex(COL_SUB));
    lv_obj_align(s_battery, LV_ALIGN_TOP_RIGHT, -14, 12);

    app_ui_show_home(APP_UI_HOME_WORK);
}
