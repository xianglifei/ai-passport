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
#include "pet_audio.h"
#include "pet_font.h"

#include <math.h>

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
static lv_obj_t *s_bubble_tail[2];  // 气泡尾巴小圆点(随气泡显隐)
static lv_obj_t *s_battery_txt;
static lv_obj_t *s_slots[DOCK_COUNT];
static int s_selected;
static uint32_t s_celebrate_until;  // 庆祝演出截止时刻(单调毫秒)
static bool s_hearts_spawned;       // 本轮庆祝是否已撒过爱心
static lv_obj_t *s_toast;           // 顶部胶囊提示(到时自删)
static uint32_t s_toast_until;
static bool s_night;                // 夜晚屏模式(电池文字配色用)

// 名字与成长阶段(跨屏幕保持;由 main 在开机时设置一次)
static uint8_t s_name_idx;
static uint8_t s_stage;
static lv_obj_t *s_collar;      // 主界面:成年项圈徽标
static lv_obj_t *s_picker_lbl;  // 命名屏:名字标签

// 环境生命:主界面空闲演出(踱步/扑蝴蝶/饿了看碗)
typedef enum {
    IDLE_ACT_NONE = 0,
    IDLE_ACT_STROLL,   // 起身踱个小步
    IDLE_ACT_POUNCE,   // 扑向蝴蝶
    IDLE_ACT_BOWL,     // 蹲着看碗(饿了)
} idle_act_t;
static idle_act_t s_idle_act;
static uint32_t s_idle_started;
static uint32_t s_next_idle_at;
static int s_pounce_x;
static uint32_t s_urng = 0xB5297A4Du;
static lv_obj_t *s_hb_fly;    // 主界面蝴蝶(扑蝴蝶用)
static lv_obj_t *s_hb_bowl;   // 主界面碗(看碗用)

static uint32_t urng_next(void) {
    s_urng ^= s_urng << 13;
    s_urng ^= s_urng >> 17;
    s_urng ^= s_urng << 5;
    return s_urng ? s_urng : (s_urng = 0xB5297A4Du);
}

// 预设名字(字形已全部进入字体子集;索引存 NVS "name")
const char *const PET_NAMES[PET_NAME_COUNT] = {
    "\u5361\u5361",        // 卡卡
    "\u54aa\u54aa",        // 咪咪
    "\u56e2\u5b50",        // 团子
    "\u5e03\u4e01",        // 布丁
    "\u7164\u7403",        // 煤球
    "\u6a58\u5b50",        // 橘子
};

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
    for (int i = 0; i < 2; i++) s_bubble_tail[i] = NULL;
    s_battery_txt = NULL;
    for (int i = 0; i < DOCK_COUNT; i++) s_slots[i] = NULL;
    s_selected = 0;
    s_celebrate_until = 0;
    s_hearts_spawned = false;
    s_collar = NULL;
    s_picker_lbl = NULL;
    s_hb_fly = NULL;
    s_hb_bowl = NULL;
    s_idle_act = IDLE_ACT_NONE;
    s_next_idle_at = 0;  // 下一块屏的第一跳由 tick 重新排(避开过期时刻)
    parts_reset();  // 旧屏删除时粒子对象已随之释放,只清登记表
}

void pet_ui_set_name(uint8_t idx) {
    if (idx >= PET_NAME_COUNT) idx = 0;
    s_name_idx = idx;
}

void pet_ui_set_stage(uint8_t stage) {
    s_stage = stage > 2 ? 2 : stage;
}

uint8_t pet_ui_stage(void) {
    return s_stage;
}

// 成长阶段的猫:幼猫 80% / 少年 90% / 成年 100%,以脚底中心为锚
static void apply_stage_to_cat(lv_obj_t *cat) {
    if (!cat) return;
    lv_image_set_pivot(cat, 80, 160);  // 素材均为 160x160
    lv_image_set_scale(cat, s_stage == 0 ? 205 : (s_stage == 1 ? 230 : 256));
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

void pet_ui_particles_tick(uint32_t now_ms) {
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
    // 顶部胶囊到时自删:放在 80ms 心跳里,主界面/场景/静态屏都能按时消失
    // (pet_ui_tick 只在主界面跑到这里,场景内的 toast 以前会一直挂着)
    if (s_toast && now_ms >= s_toast_until) {
        lv_obj_delete(s_toast);
        s_toast = NULL;
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
    lv_label_set_text(name, PET_NAMES[s_name_idx]);
    lv_obj_set_style_text_font(name, pet_font_cjk20(), 0);
    lv_obj_set_style_text_color(name, COL_WHITE, 0);
    lv_obj_center(name);

    // 电池(右上)
    s_battery_txt = lv_label_create(s_scr);
    lv_obj_set_style_text_font(s_battery_txt, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_battery_txt, COL_INK, 0);
    lv_label_set_text(s_battery_txt, "--");
    lv_obj_align(s_battery_txt, LV_ALIGN_TOP_RIGHT, -8, 10);

    // 卡卡(160x160 居中,脚踩草地;幼猫/少年按成长阶段缩小,脚底对齐)
    s_cat = lv_image_create(s_scr);
    lv_image_set_src(s_cat, &kaka_art_cat_sit_a);
    lv_obj_set_pos(s_cat, 40, 68);
    apply_stage_to_cat(s_cat);
    if (s_stage == 2) {  // 成年:戴上项圈徽标
        s_collar = lv_image_create(s_scr);
        lv_image_set_src(s_collar, &kaka_art_prop_collar);
        lv_obj_set_pos(s_collar, 98, 148);
    }

    // 环境生命的道具(默认藏起,tick 触发时露脸)
    s_hb_fly = lv_image_create(s_scr);
    lv_image_set_src(s_hb_fly, &kaka_art_prop_butterfly);
    lv_obj_add_flag(s_hb_fly, LV_OBJ_FLAG_HIDDEN);
    s_hb_bowl = lv_image_create(s_scr);
    lv_image_set_src(s_hb_bowl, &kaka_art_prop_bowl);
    lv_obj_set_pos(s_hb_bowl, 10, 196);
    lv_obj_add_flag(s_hb_bowl, LV_OBJ_FLAG_HIDDEN);

    // 需求气泡(挂在卡卡头上,内容随模型刷新):白圆 + 两粒小尾巴圆点,
    // 一眼是"想法"而不是悬空白球;图标缩到 32px 居中 —— 48px 原图直塞
    // 会被圆角裁掉四角,永远只显示一部分
    s_bubble = make_rect(s_scr, 158, 46, 48, 48, COL_WHITE, 24);
    for (int i = 0; i < 2; i++) {  // 尾巴往猫头方向递小两粒
        int d = i == 0 ? 8 : 5;
        s_bubble_tail[i] = make_rect(s_scr, 152 - i * 8, 90 + i * 8, d, d,
                                     COL_WHITE, d / 2);
        lv_obj_add_flag(s_bubble_tail[i], LV_OBJ_FLAG_HIDDEN);
    }
    s_bubble_icon = lv_image_create(s_bubble);
    lv_obj_center(s_bubble_icon);
    lv_image_set_scale(s_bubble_icon, 171);  // 48px -> 32px
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
    apply_stage_to_cat(sleeping);  // 幼猫睡得也小一号

    lv_obj_t *good = lv_label_create(s_scr);
    lv_label_set_text(good, "\u665a\u5b89");  // 晚安
    lv_obj_set_style_text_font(good, pet_font_cjk20(), 0);
    lv_obj_set_style_text_color(good, CREAM_HEX, 0);
    lv_obj_align(good, LV_ALIGN_TOP_MID, 0, 14);

    lv_obj_t *zzz = lv_label_create(s_scr);
    lv_label_set_text_fmt(zzz, "%s\u7761\u7740\u4e86", PET_NAMES[s_name_idx]);
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

// 空闲小剧场:踱步/扑蝴蝶/饿了看碗。进行中返回 true 并自行决定帧与位置。
static bool idle_life_tick(const pet_model_t *m, uint32_t now_ms,
                           const lv_image_dsc_t **frame) {
    if (s_idle_act == IDLE_ACT_NONE) {
        // 触发:开场静默 18~33s,之后每 25~55s 自导自演一次;庆祝时不抢戏
        if (s_next_idle_at == 0) {
            s_next_idle_at = now_ms + 18000 + urng_next() % 15000;
        } else if (now_ms >= s_next_idle_at && now_ms >= s_celebrate_until) {
            s_idle_started = now_ms;
            uint32_t r = urng_next();
            if (pet_model_want(m) == PET_WANT_FOOD && (r & 1)) {
                s_idle_act = IDLE_ACT_BOWL;
                if (s_hb_bowl) lv_obj_clear_flag(s_hb_bowl, LV_OBJ_FLAG_HIDDEN);
                pet_audio_play(SND_MEOW_HUNGRY);
            } else if (r & 2) {
                s_idle_act = IDLE_ACT_POUNCE;
                // 蝴蝶只在左侧天空飞(56..104, 64):右上角是"我在玩"气泡
                // 的固定位置(158..206, 46..94),两片空域必须错开,不叠影
                s_pounce_x = 56 + (int)(urng_next() % 49);
                if (s_hb_fly) {
                    lv_obj_set_pos(s_hb_fly, s_pounce_x, 64);
                    lv_obj_clear_flag(s_hb_fly, LV_OBJ_FLAG_HIDDEN);
                }
                pet_audio_play(SND_TRILL);
            } else {
                s_idle_act = IDLE_ACT_STROLL;
            }
        }
        return false;
    }

    uint32_t t = now_ms - s_idle_started;
    switch (s_idle_act) {
    case IDLE_ACT_STROLL:
        if (t < 2400) {  // 向右踱 34px 再折返,走路四帧
            static const lv_image_dsc_t *const seq[4] = {
                &kaka_art_cat_walk0, &kaka_art_cat_walk1,
                &kaka_art_cat_walk2, &kaka_art_cat_walk3
            };
            *frame = seq[(now_ms / 150) % 4];
            lv_obj_set_x(s_cat,
                         40 + (int)(34.0f * sinf((float)t / 2400.0f * 3.14159f)));
            return true;
        }
        break;
    case IDLE_ACT_POUNCE:
        if (t < 1200) {  // 向蝴蝶扑一小段,抛物线跳
            *frame = &kaka_art_cat_leap;
            int k = (int)(t / 12);  // 0..99
            int target = s_pounce_x - 44;          // 蝴蝶凑到猫的右爪边
            if (target < 16) target = 16;
            if (target > 60) target = 60;
            lv_obj_set_x(s_cat, 40 + (target - 40) * k / 100);
            lv_obj_set_y(s_cat,
                         68 - (int)(26.0f * sinf((float)t / 1200.0f * 3.14159f)));
            return true;
        }
        pet_ui_spawn_heart(s_pounce_x - 20, 96);  // 扑空也很开心
        break;
    case IDLE_ACT_BOWL:
        if (t < 2500) {  // 凑近碗盯着,咽口水
            *frame = &kaka_art_cat_squint;
            lv_obj_set_x(s_cat, 40 - (t > 600 ? 12 : (int)(t / 50)));
            return true;
        }
        break;
    default:
        break;
    }
    // 演出结束:收道具、回原位,安排下一次
    s_idle_act = IDLE_ACT_NONE;
    s_next_idle_at = now_ms + 25000 + urng_next() % 30000;
    if (s_hb_fly) lv_obj_add_flag(s_hb_fly, LV_OBJ_FLAG_HIDDEN);
    if (s_hb_bowl) lv_obj_add_flag(s_hb_bowl, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(s_cat, 40, 68);
    return false;
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
    } else if (idle_life_tick(m, now_ms, &frame)) {
        // 空闲小剧场自己选好了帧与位置
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
        for (int i = 0; i < 2; i++) {
            if (s_bubble_tail[i]) {
                lv_obj_clear_flag(s_bubble_tail[i], LV_OBJ_FLAG_HIDDEN);
            }
        }
        lv_image_set_src(s_bubble_icon, icon);
        // 枢轴挪到素材中心:缩放后绕中心收缩,48x40 的蝴蝶也不会偏
        lv_image_set_pivot(s_bubble_icon, icon->header.w / 2,
                           icon->header.h / 2);
    } else {
        lv_obj_add_flag(s_bubble, LV_OBJ_FLAG_HIDDEN);
        for (int i = 0; i < 2; i++) {
            if (s_bubble_tail[i]) {
                lv_obj_add_flag(s_bubble_tail[i], LV_OBJ_FLAG_HIDDEN);
            }
        }
    }
}

void pet_ui_show_name_picker(void) {
    lv_obj_t *scr = make_screen(COL_SKY);
    pet_ui_install_screen(scr);
    make_rect(s_scr, 0, 228, 240, 6, COL_GRASS_DK, 0);
    make_rect(s_scr, 0, 234, 240, 86, COL_GRASS, 0);

    // 名字牌预览(与主界面同款):上/下 换名,OK 定下
    lv_obj_t *plate = make_rect(s_scr, 60, 8, 120, 34, COL_ACCENT, 8);
    s_picker_lbl = lv_label_create(plate);
    lv_label_set_text(s_picker_lbl, PET_NAMES[s_name_idx]);
    lv_obj_set_style_text_font(s_picker_lbl, pet_font_cjk20(), 0);
    lv_obj_set_style_text_color(s_picker_lbl, COL_WHITE, 0);
    lv_obj_center(s_picker_lbl);

    lv_obj_t *cat = lv_image_create(s_scr);
    lv_image_set_src(cat, &kaka_art_cat_sit_a);
    lv_obj_set_pos(cat, 40, 76);
    apply_stage_to_cat(cat);  // 初见就是小小一只

    lv_obj_t *hint = lv_label_create(s_scr);
    lv_label_set_text(hint, "\u4e0a/\u4e0b \u6362  OK \u5f00\u59cb");  // 上/下 换  OK 开始
    lv_obj_set_style_text_font(hint, pet_font_cjk16(), 0);
    lv_obj_set_style_text_color(hint, COL_WHITE, 0);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -26);
}

void pet_ui_name_picker_select(uint8_t idx) {
    if (idx >= PET_NAME_COUNT) idx = 0;
    s_name_idx = idx;
    if (s_picker_lbl) lv_label_set_text(s_picker_lbl, PET_NAMES[idx]);
}

void pet_ui_show_album(const pet_model_t *m, int battery_soc) {
    // 回忆页 = 家长/查看角落。上半:当前状态三条(报警气泡只有"亮灯",
    // 这里才是"仪表盘");下半:累计成就 + 玩法说明。
    lv_obj_t *scr = make_screen(COL_PANEL);  // 奶白底,像一页小册子
    pet_ui_install_screen(scr);

    lv_obj_t *title = lv_label_create(s_scr);
    lv_label_set_text(title, "\u56de\u5fc6");  // 回忆
    lv_obj_set_style_text_font(title, pet_font_cjk20(), 0);
    lv_obj_set_style_text_color(title, COL_INK, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 6);

    lv_obj_t *batt = lv_label_create(s_scr);
    lv_obj_set_style_text_font(batt, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(batt, COL_INK, 0);
    if (battery_soc < 0) {
        lv_label_set_text(batt, "--");
    } else {
        lv_label_set_text_fmt(batt, "%d%%",
                              battery_soc > 100 ? 100 : battery_soc);
    }
    lv_obj_align(batt, LV_ALIGN_TOP_RIGHT, -8, 8);

    // —— 当前状态:图标 + 进度条;低于照顾线(40)整条变红提示该做什么 ——
    static const struct {
        const lv_image_dsc_t *icon;
        uint8_t v;
        uint32_t color;
    } stats[] = {
        { &kaka_art_icon_feed,  0, 0xF5A02E },  // 饱食:橙(鱼)
        { &kaka_art_icon_brush, 0, 0x6FC24B },  // 干净:绿(刷)
        { &kaka_art_prop_heart, 0, 0xE07BA5 },  // 心情:粉(爱心)
    };
    uint8_t svals[3] = { m->fullness, m->clean, m->happy };
    for (int i = 0; i < 3; i++) {
        int y = 32 + i * 28;
        lv_obj_t *icon = lv_image_create(s_scr);
        lv_image_set_src(icon, stats[i].icon);
        lv_obj_set_pos(icon, 16, y);
        lv_image_set_pivot(icon, 0, 0);
        lv_image_set_scale(icon, 128);  // 48px 素材缩到 24px 显示
        make_rect(s_scr, 52, y + 5, 168, 14, lv_color_hex(0xE8E0D0), 7);
        bool low = svals[i] < PET_STAT_WANT;  // 低于照顾线:红色提醒
        make_rect(s_scr, 52, y + 5,
                  (int32_t)(168 * svals[i] / 100), 14,
                  lv_color_hex(low ? 0xE05252 : stats[i].color), 7);
    }

    make_rect(s_scr, 20, 118, 200, 2, COL_GRASS, 0);

    // —— 累计成就 ——
    static const struct {
        const lv_image_dsc_t *icon;
        const char *name;
    } rows[] = {
        { &kaka_art_icon_feed, "\u5582\u98df" },       // 喂食
        { &kaka_art_icon_brush, "\u5237\u6bdb" },      // 刷毛
        { &kaka_art_icon_sun, "\u6652\u592a\u9633" },  // 晒太阳
        { &kaka_art_icon_walk, "\u6563\u6b65" },       // 散步
    };
    uint32_t counts[] = { m->feed_fish, m->brush_strokes, m->sun_count,
                          m->walk_count };
    for (int i = 0; i < 4; i++) {
        int y = 124 + i * 24;
        lv_obj_t *icon = lv_image_create(s_scr);
        lv_image_set_src(icon, rows[i].icon);
        lv_obj_set_pos(icon, 16, y);
        lv_image_set_pivot(icon, 0, 0);
        lv_image_set_scale(icon, 128);
        lv_obj_t *lbl = lv_label_create(s_scr);
        lv_label_set_text(lbl, rows[i].name);
        lv_obj_set_style_text_font(lbl, pet_font_cjk16(), 0);
        lv_obj_set_style_text_color(lbl, COL_INK, 0);
        lv_obj_set_pos(lbl, 50, y + 3);
        lv_obj_t *val = lv_label_create(s_scr);
        lv_obj_set_style_text_font(val, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(val, COL_ACCENT, 0);
        lv_label_set_text_fmt(val, "%lu", (unsigned long)counts[i]);
        lv_obj_set_pos(val, 192, y + 3);
    }

    // —— 玩法说明(压缩到三行,给状态条腾地方) ——
    lv_obj_t *how = lv_label_create(s_scr);
    lv_label_set_text(how, "\u600e\u4e48\u73a9");  // 怎么玩
    lv_obj_set_style_text_font(how, pet_font_cjk16(), 0);
    lv_obj_set_style_text_color(how, COL_ACCENT, 0);
    lv_obj_set_pos(how, 16, 226);

    static const char *const helps[] = {
        "\u4e0a/\u4e0b: \u6362   OK: \u5f00\u59cb",              // 上/下: 换 OK: 开始
        "OK \u957f\u6309: \u7761\u89c9",                          // OK 长按: 睡觉
        "\u4e0a \u957f\u6309: \u97f3\u91cf   \u4e0b \u957f\u6309: \u56de\u5fc6",  // 上/下 长按
        "OK \u957f\u6309: \u6539\u540d",                          // OK 长按: 改名
    };
    for (int i = 0; i < 4; i++) {
        lv_obj_t *line = lv_label_create(s_scr);
        lv_label_set_text(line, helps[i]);
        lv_obj_set_style_text_font(line, pet_font_cjk16(), 0);
        lv_obj_set_style_text_color(line, COL_INK, 0);
        lv_obj_set_pos(line, 12, 244 + i * 18);
    }
}

void pet_ui_reunion(uint32_t now_ms) {
    s_celebrate_until = now_ms + 3000;
    s_hearts_spawned = false;  // 复用庆祝渲染:开心帧 + 爱心雨
}
