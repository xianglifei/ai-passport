// main/pet_ui.c —— 卡卡应用场景实现(主界面)。
//
// 布局(240x320):
//   顶部:名字牌"Kaka"(中文卡卡待字体管线) + 右上电池
//   中部:卡卡 160x160,脚踩草地
//   底部:4 个活动 dock 槽(喂食/刷毛/晒太阳/散步),选中白边高亮
// 动画:呼吸(sit_a/sit_b 800ms 交替)、每 4s 眨眼 240ms;需求气泡按模型刷新。
#include "pet_ui.h"

#include "lvgl.h"
#include "pet_art.h"
#include "pet_font.h"

// —— 配色(与像素资产调色板同族) ——
#define COL_SKY      lv_color_hex(0x9AD7F5)
#define COL_GRASS    lv_color_hex(0x6FC24B)
#define COL_GRASS_DK lv_color_hex(0x4E9E33)
#define COL_PANEL    lv_color_hex(0xFFF6E8)
#define COL_ACCENT   lv_color_hex(0xF5A02E)
#define COL_INK      lv_color_hex(0x35231B)
#define COL_WHITE    lv_color_hex(0xFFFFFF)
#define CREAM_HEX    lv_color_hex(0xFBEFDD)

#define DOCK_COUNT PET_DOCK_COUNT
#define DOCK_SLOT_W 52
#define DOCK_SLOT_H 52
#define DOCK_Y      246
#define DOCK_GAP    8

static lv_obj_t *s_scr;
static lv_obj_t *s_cat;
static lv_obj_t *s_bubble;       // 需求气泡容器
static lv_obj_t *s_bubble_icon;  // 气泡内图标
static lv_obj_t *s_battery_txt;
static lv_obj_t *s_slots[DOCK_COUNT];
static int s_selected;
static uint32_t s_celebrate_until;  // 庆祝演出截止时刻(单调毫秒)
static bool s_hearts_spawned;       // 本轮庆祝是否已撒过爱心
static lv_obj_t *s_toast;           // 顶部胶囊提示(到时自删)
static uint32_t s_toast_until;
static bool s_night;                // 夜晚屏模式(电池文字配色用)

static void parts_reset(void);  // 定义在粒子段落,先供 pet_ui_init 使用

// dock 槽位:与 pet_activity_t 1..4 一一对应
static const lv_image_dsc_t *const DOCK_ICONS[DOCK_COUNT] = {
    &kaka_art_icon_feed,   // 喂食
    &kaka_art_icon_brush,  // 刷毛
    &kaka_art_icon_sun,    // 晒太阳
    &kaka_art_icon_walk,   // 散步
};

// dock 中文名(与生成脚本 SYMS 保持同步)
static const char *const DOCK_NAMES[DOCK_COUNT] = {
    "\u5582\u98df",        // 喂食
    "\u5237\u6bdb",        // 刷毛
    "\u6652\u592a\u9633",  // 晒太阳
    "\u6563\u6b65",        // 散步
};

static lv_obj_t *make_rect(lv_obj_t *parent, int32_t x, int32_t y,
                           int32_t w, int32_t h, lv_color_t color,
                           int32_t radius) {
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_bg_color(obj, color, 0);
    lv_obj_set_style_radius(obj, radius, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    return obj;
}

// LVGL 屏幕必须以 NULL 为父节点创建;否则删除旧屏会连带删掉它
static lv_obj_t *make_screen(lv_color_t bg) {
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, bg, 0);
    lv_obj_set_style_radius(scr, 0, 0);
    lv_obj_set_style_border_width(scr, 0, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    return scr;
}

void pet_ui_init(void) {
    s_scr = NULL;
    s_cat = NULL;
    s_bubble = NULL;
    s_bubble_icon = NULL;
    s_battery_txt = NULL;
    for (int i = 0; i < DOCK_COUNT; i++) s_slots[i] = NULL;
    s_selected = 0;
    s_celebrate_until = 0;
    s_hearts_spawned = false;
    parts_reset();  // 旧屏删除时粒子对象已随之释放,只清登记表
}

void pet_ui_install_screen(lv_obj_t *scr) {
    if (s_scr) {
        lv_obj_delete(s_scr);  // 整屏删除,同时清掉所有控件
    }
    pet_ui_init();  // 清空主界面控件指针(庆祝状态也随之复位)
    s_scr = scr;
    s_toast = NULL;  // 提示挂在旧屏上,已随之删除
    lv_screen_load(scr);
}

// —— 粒子系统:爱心(图像)与方块(碎屑/毛絮),生命周期由 particles_tick 推进 ——
#define PART_MAX 24
#define PART_HEART 1
#define PART_CHUNK 2

typedef struct {
    lv_obj_t *obj;
    int16_t vx, vy;      // 每帧(80ms)位移,像素
    uint16_t life;       // 剩余帧数
    uint16_t max_life;
    uint8_t kind;
} part_t;

static part_t s_parts[PART_MAX];
static uint32_t s_now_cache;  // 最近一次 tick 的单调毫秒

static void parts_reset(void) {
    for (int i = 0; i < PART_MAX; i++) s_parts[i].obj = NULL;
}

static int part_slot(void) {
    for (int i = 0; i < PART_MAX; i++) {
        if (s_parts[i].obj == NULL) return i;
    }
    return -1;
}

void pet_ui_spawn_heart(int x, int y) {
    int i = part_slot();
    if (i < 0) return;
    lv_obj_t *img = lv_image_create(s_scr);
    lv_image_set_src(img, &kaka_art_prop_heart);
    lv_obj_set_pos(img, x, y);
    s_parts[i] = (part_t){ .obj = img, .vx = 0, .vy = -2,
                           .life = 22, .max_life = 22, .kind = PART_HEART };
}

void pet_ui_spawn_chunk(int x, int y, lv_color_t color, bool up) {
    int i = part_slot();
    if (i < 0) return;
    lv_obj_t *c = make_rect(s_scr, x, y, 6, 6, color, 1);
    s_parts[i] = (part_t){ .obj = c, .vx = (x % 2 ? 1 : -1),
                           .vy = up ? -3 : 2, .life = 12, .max_life = 12,
                           .kind = PART_CHUNK };
}

void pet_ui_particles_tick(void) {
    for (int i = 0; i < PART_MAX; i++) {
        part_t *p = &s_parts[i];
        if (!p->obj) continue;
        lv_obj_set_pos(p->obj, lv_obj_get_x(p->obj) + p->vx,
                       lv_obj_get_y(p->obj) + p->vy);
        p->life--;
        lv_opa_t opa = (lv_opa_t)(255 * p->life / p->max_life);
        if (p->kind == PART_HEART) {
            lv_obj_set_style_img_opa(p->obj, opa, 0);
        } else {
            lv_obj_set_style_bg_opa(p->obj, opa, 0);
        }
        if (p->life == 0) {
            lv_obj_delete(p->obj);
            p->obj = NULL;
        }
    }
}

void pet_ui_celebrate(pet_interact_t why, uint32_t now_ms) {
    s_celebrate_until = now_ms + 2000;
    s_hearts_spawned = false;
    if (why == PET_INTERACT_REFUSED) {
        // 礼貌拒喂:只给一颗害羞爱心,短演出
        pet_ui_spawn_heart(112, 140);
        s_celebrate_until = now_ms + 900;
    }
}

int pet_ui_selected(void) {
    return s_selected;
}

static void restyle_slots(void) {
    for (int i = 0; i < DOCK_COUNT; i++) {
        bool sel = (i == s_selected);
        lv_obj_set_style_border_width(s_slots[i], sel ? 3 : 0, 0);
        lv_obj_set_style_border_color(s_slots[i], COL_WHITE, 0);
        lv_obj_set_style_bg_color(s_slots[i],
                                  sel ? lv_color_hex(0xFFD9A0) : COL_PANEL, 0);
        lv_obj_set_y(s_slots[i], sel ? DOCK_Y - 4 : DOCK_Y);  // 选中微抬升
    }
}

void pet_ui_select(int index) {
    if (index < 0) index = 0;
    if (index > DOCK_COUNT - 1) index = DOCK_COUNT - 1;
    s_selected = index;
    if (s_slots[0]) restyle_slots();
}

void pet_ui_show_home(const pet_model_t *m) {
    (void)m;
    s_night = false;

    // 先注册新屏(会删掉旧屏/场景屏),再把主界面控件挂上去
    lv_obj_t *scr = make_screen(COL_SKY);
    pet_ui_install_screen(scr);
    // 天空与草地
    make_rect(s_scr, 0, 228, 240, 6, COL_GRASS_DK, 0);
    make_rect(s_scr, 0, 234, 240, 86, COL_GRASS, 0);
    make_rect(s_scr, 0, 228, 240, 6, COL_GRASS_DK, 0);
    make_rect(s_scr, 0, 234, 240, 86, COL_GRASS, 0);

    // 名字牌(中文,走 app_font_20 子集)
    lv_obj_t *plate = make_rect(s_scr, 80, 8, 80, 30, COL_ACCENT, 8);
    lv_obj_t *name = lv_label_create(plate);
    lv_label_set_text(name, "\u5361\u5361");  // 卡卡
    lv_obj_set_style_text_font(name, pet_font_cjk20(), 0);
    lv_obj_set_style_text_color(name, COL_WHITE, 0);
    lv_obj_center(name);

    // 电池(右上)
    s_battery_txt = lv_label_create(s_scr);
    lv_obj_set_style_text_font(s_battery_txt, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_battery_txt, COL_INK, 0);
    lv_label_set_text(s_battery_txt, "--");
    lv_obj_align(s_battery_txt, LV_ALIGN_TOP_RIGHT, -8, 10);

    // 卡卡(160x160 居中,脚踩草地)
    s_cat = lv_image_create(s_scr);
    lv_image_set_src(s_cat, &kaka_art_cat_sit_a);
    lv_obj_set_pos(s_cat, 40, 68);

    // 需求气泡(挂在卡卡头上,内容随模型刷新)
    s_bubble = make_rect(s_scr, 158, 46, 48, 48, COL_WHITE, 24);
    s_bubble_icon = lv_image_create(s_bubble);
    lv_obj_add_flag(s_bubble, LV_OBJ_FLAG_HIDDEN);

    // dock(槽 + 中文名,家长可读;孩子看图标)
    int x0 = (240 - (DOCK_COUNT * DOCK_SLOT_W + (DOCK_COUNT - 1) * DOCK_GAP)) / 2;
    for (int i = 0; i < DOCK_COUNT; i++) {
        int x = x0 + i * (DOCK_SLOT_W + DOCK_GAP);
        s_slots[i] = make_rect(s_scr, x, DOCK_Y, DOCK_SLOT_W, DOCK_SLOT_W,
                               COL_PANEL, 10);
        lv_obj_t *icon = lv_image_create(s_slots[i]);
        lv_image_set_src(icon, DOCK_ICONS[i]);
        lv_obj_center(icon);

        lv_obj_t *label = lv_label_create(s_scr);
        lv_label_set_text(label, DOCK_NAMES[i]);
        lv_obj_set_style_text_font(label, pet_font_cjk16(), 0);
        lv_obj_set_style_text_color(label, COL_WHITE, 0);
        lv_obj_set_pos(label, x + DOCK_SLOT_W / 2 - 24, DOCK_Y + DOCK_SLOT_W + 2);
        lv_obj_set_size(label, 48, 18);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    }
    restyle_slots();
}

void pet_ui_set_battery(int soc) {
    if (!s_battery_txt) return;
    if (soc < 0) {
        lv_label_set_text(s_battery_txt, "--");
        return;
    }
    if (soc > 100) soc = 100;
    lv_label_set_text_fmt(s_battery_txt, "%d%%", soc);
    // 低电量变红提醒家长充电;夜晚屏用奶白字
    lv_color_t color = s_night ? CREAM_HEX : COL_INK;
    if (soc <= 15) color = lv_color_hex(0xE05252);
    lv_obj_set_style_text_color(s_battery_txt, color, 0);
}

void pet_ui_toast(const char *text) {
    if (!s_scr) return;
    if (s_toast) {
        lv_obj_delete(s_toast);
        s_toast = NULL;
    }
    lv_obj_t *cap = make_rect(s_scr, 60, 44, 120, 34, COL_PANEL, 17);
    lv_obj_t *lbl = lv_label_create(cap);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, pet_font_cjk16(), 0);
    lv_obj_set_style_text_color(lbl, COL_INK, 0);
    lv_obj_center(lbl);
    s_toast = cap;
    s_toast_until = s_now_cache + 1600;
}

void pet_ui_show_night(void) {
    lv_obj_t *scr = make_screen(lv_color_hex(0x1B2A4A));
    pet_ui_install_screen(scr);
    s_night = true;

    // 星星(固定几颗,足够夜的氛围)
    static const int16_t stars[6][2] = {
        {30, 40}, {86, 24}, {150, 52}, {206, 30}, {58, 84}, {190, 96}
    };
    for (int i = 0; i < 6; i++) {
        make_rect(s_scr, stars[i][0], stars[i][1], 4, 4,
                  i % 2 ? COL_WHITE : lv_color_hex(0xFFF2B8), 1);
    }
    lv_obj_t *moon = lv_image_create(s_scr);
    lv_image_set_src(moon, &kaka_art_prop_moon);
    lv_obj_set_pos(moon, 168, 16);

    lv_obj_t *sleeping = lv_image_create(s_scr);
    lv_image_set_src(sleeping, &kaka_art_cat_sleep);
    lv_obj_set_pos(sleeping, 40, 104);

    lv_obj_t *good = lv_label_create(s_scr);
    lv_label_set_text(good, "\u665a\u5b89");  // 晚安
    lv_obj_set_style_text_font(good, pet_font_cjk20(), 0);
    lv_obj_set_style_text_color(good, CREAM_HEX, 0);
    lv_obj_align(good, LV_ALIGN_TOP_MID, 0, 14);

    lv_obj_t *zzz = lv_label_create(s_scr);
    lv_label_set_text(zzz, "\u5361\u5361\u7761\u7740\u4e86");  // 卡卡睡着了
    lv_obj_set_style_text_font(zzz, pet_font_cjk16(), 0);
    lv_obj_set_style_text_color(zzz, lv_color_hex(0x9FB4D8), 0);
    lv_obj_align(zzz, LV_ALIGN_BOTTOM_MID, 0, -22);

    s_battery_txt = lv_label_create(s_scr);
    lv_obj_set_style_text_font(s_battery_txt, &lv_font_montserrat_14, 0);
    lv_label_set_text(s_battery_txt, "");
    lv_obj_align(s_battery_txt, LV_ALIGN_TOP_RIGHT, -8, 10);
}

static const lv_image_dsc_t *bubble_icon_for(pet_want_t want, uint32_t now) {
    switch (want) {
    case PET_WANT_FOOD: return &kaka_art_icon_feed;
    case PET_WANT_BRUSH: return &kaka_art_icon_brush;
    case PET_WANT_WALK: return &kaka_art_icon_walk;
    case PET_WANT_PLAY:
        // 无需求时的可爱气泡:蝴蝶/毛线球按秒轮换
        return (now / 3000) % 2 ? &kaka_art_prop_butterfly : &kaka_art_prop_yarn;
    default: return NULL;
    }
}

void pet_ui_tick(const pet_model_t *m, uint32_t now_ms) {
    if (!s_cat) return;
    s_now_cache = now_ms;

    const lv_image_dsc_t *frame;
    if (now_ms < s_celebrate_until) {
        // 活动圆满结束:开心帧 + 一次爱心雨
        frame = &kaka_art_cat_happy;
        if (!s_hearts_spawned) {
            s_hearts_spawned = true;
            pet_ui_spawn_heart(48, 150);
            pet_ui_spawn_heart(104, 128);
            pet_ui_spawn_heart(160, 150);
            pet_ui_spawn_heart(80, 170);
        }
    } else if (now_ms % 4000 >= 3760) {
        frame = &kaka_art_cat_blink;  // 每 4s 眨眼 240ms
    } else {
        frame = ((now_ms / 800) % 2) ? &kaka_art_cat_sit_b : &kaka_art_cat_sit_a;
    }
    lv_image_set_src(s_cat, frame);

    // 需求气泡:低状态提示;一切正常时偶尔冒可爱气泡
    pet_want_t want = pet_model_want(m);
    if (want == PET_WANT_NONE) {
        // 每 10s 里露脸 3s 的"我在玩"气泡
        if ((now_ms % 10000) < 3000) want = PET_WANT_PLAY;
    }
    const lv_image_dsc_t *icon = bubble_icon_for(want, now_ms);
    if (icon) {
        lv_obj_clear_flag(s_bubble, LV_OBJ_FLAG_HIDDEN);
        lv_image_set_src(s_bubble_icon, icon);
    } else {
        lv_obj_add_flag(s_bubble, LV_OBJ_FLAG_HIDDEN);
    }

    // 顶部胶囊提示到时自删
    if (s_toast && now_ms >= s_toast_until) {
        lv_obj_delete(s_toast);
        s_toast = NULL;
    }
}
