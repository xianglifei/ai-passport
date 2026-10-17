// main/pet_scenes.c —— 四个互动场景实现。
//
// 场景是"演出层":模型(pet_model)决定状态与结算,这里只负责把它画出来。
// 喂食:奶白厨房 + 碗 + 三条鱼计数;刷毛:蜜桃房间 + 五枚爪印进度;
// 晒太阳:暖色午后 + 蝴蝶绕飞 + Zzz;散步:卷轴街道 + 视差 + 扑跳。
#include "pet_scenes.h"

#include <math.h>

#include "lvgl.h"
#include "pet_art.h"
#include "pet_audio.h"
#include "pet_font.h"
#include "pet_ui.h"

// —— 配色 ——
#define COL_CREAM_BG  lv_color_hex(0xFFF3DF)
#define COL_FLOOR_WARM lv_color_hex(0xF2DFC0)
#define COL_PEACH_BG  lv_color_hex(0xFFE7D6)
#define COL_SUNBG     lv_color_hex(0xFFDCA8)
#define COL_SUNGROUND lv_color_hex(0xE8C893)
#define COL_ROAD      lv_color_hex(0x9AA0A8)
#define COL_ROAD_DASH lv_color_hex(0xE8E8E0)
#define COL_WHITE     lv_color_hex(0xFFFFFF)
#define COL_INK       lv_color_hex(0x35231B)

static pet_activity_t s_scene = PET_ACTIVITY_NONE;
static uint32_t s_last_now;  // 最近一次 anim/interact 的单调毫秒

// 场景控件(cat 所有场景共用;其余按场景复用指针,切换时整屏重建)
static lv_obj_t *s_cat;
static lv_obj_t *s_bowl;        // 喂食:碗
static lv_obj_t *s_fish[3];     // 喂食:剩余鱼计数图标
static lv_obj_t *s_brush;       // 刷毛:刷子
static lv_obj_t *s_paws[5];     // 刷毛:进度爪印
static lv_obj_t *s_butterfly;   // 晒太阳/散步:蝴蝶
static lv_obj_t *s_zzz;         // 晒太阳:Zzz
static lv_obj_t *s_collar;      // 散步:项圈徽标
static lv_obj_t *s_houses[3];   // 散步:视差房子
static lv_obj_t *s_trees[2];    // 散步:视差树
static lv_obj_t *s_clouds[2];   // 散步:云
static lv_obj_t *s_dashes[4];   // 散步:路面虚线

// 演出计时(80ms 心跳推进)
static uint32_t s_eat_until;      // 喂食:咀嚼动画截止
static uint32_t s_brush_until;    // 刷毛:刷子扫动截止
static uint32_t s_jump_until;     // 散步:扑跳姿态截止
static uint8_t s_eaten;           // 本次已吃几条(与 fish 图标联动)
static uint8_t s_strokes_shown;   // 已点亮的爪印数
static uint32_t s_poke_until;     // 戳一戳:微反馈截止
static uint32_t s_poke_base_x;    // 戳一戳:猫的原位 x(结束时归位)
static bool s_poke_active;        // 戳一戳结束后需要一次归位

static lv_obj_t *sc_rect(lv_obj_t *parent, int32_t x, int32_t y,
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

static lv_obj_t *sc_screen(lv_color_t bg) {
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, bg, 0);
    lv_obj_set_style_radius(scr, 0, 0);
    lv_obj_set_style_border_width(scr, 0, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    return scr;
}

static lv_obj_t *sc_img(lv_obj_t *parent, const lv_image_dsc_t *src,
                        int32_t x, int32_t y) {
    lv_obj_t *img = lv_image_create(parent);
    lv_image_set_src(img, src);
    lv_obj_set_pos(img, x, y);
    return img;
}

static void sc_battery(lv_obj_t *scr, int soc) {
    lv_obj_t *lbl = lv_label_create(scr);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl, COL_INK, 0);
    lv_label_set_text_fmt(lbl, "%d%%", soc > 100 ? 100 : (soc < 0 ? 0 : soc));
    lv_obj_align(lbl, LV_ALIGN_TOP_RIGHT, -8, 10);
}

static void sc_reset_state(void) {
    s_cat = s_bowl = s_brush = s_butterfly = s_zzz = s_collar = NULL;
    for (int i = 0; i < 3; i++) { s_fish[i] = NULL; s_houses[i] = NULL; }
    for (int i = 0; i < 5; i++) s_paws[i] = NULL;
    for (int i = 0; i < 2; i++) s_trees[i] = NULL;
    for (int i = 0; i < 4; i++) s_dashes[i] = NULL;
    s_eat_until = s_brush_until = s_jump_until = 0;
    s_eaten = 0;
    s_strokes_shown = 0;
    s_poke_until = 0;
    s_poke_active = false;
}

bool pet_scenes_active(void) {
    return s_scene != PET_ACTIVITY_NONE;
}

// 换屏/删屏前必调:指针指向的对象随旧屏释放,不清就成了悬空指针。
void pet_scenes_stop(void) {
    s_scene = PET_ACTIVITY_NONE;
    sc_reset_state();
}

// —— 各场景搭建 ——
static void build_feed(lv_obj_t *scr, const pet_model_t *m) {
    sc_rect(scr, 0, 260, 240, 60, COL_FLOOR_WARM, 0);
    for (int i = 0; i < 3; i++) {
        s_fish[i] = sc_img(scr, &kaka_art_icon_feed, 12 + i * 40, 12);
        // 已饱时进场景就把鱼置灰:一眼看懂"现在吃不下",不用按了才知道
        if (m->fullness >= 95) {
            lv_obj_set_style_img_opa(s_fish[i], 90, 0);
        }
    }
    s_bowl = sc_img(scr, &kaka_art_prop_bowl, 12, 224);
    s_cat = sc_img(scr, &kaka_art_cat_eat0, 72, 96);
}

static void build_brush(lv_obj_t *scr) {
    sc_rect(scr, 0, 260, 240, 60, COL_FLOOR_WARM, 0);
    for (int i = 0; i < 5; i++) {
        s_paws[i] = sc_img(scr, &kaka_art_prop_paw, 32 + i * 40, 14);
        lv_obj_set_style_img_opa(s_paws[i], 70, 0);  // 未点亮暗淡
    }
    s_cat = sc_img(scr, &kaka_art_cat_sit_a, 40, 84);
    s_brush = sc_img(scr, &kaka_art_prop_brush, 128, 110);
    lv_obj_add_flag(s_brush, LV_OBJ_FLAG_HIDDEN);
}

static void build_sun(lv_obj_t *scr) {
    sc_img(scr, &kaka_art_prop_sun, 136, 14);
    sc_rect(scr, 0, 252, 240, 68, COL_SUNGROUND, 0);
    s_cat = sc_img(scr, &kaka_art_cat_lie_a, 40, 100);
    s_butterfly = sc_img(scr, &kaka_art_prop_butterfly, 60, 80);
    // Zzz 锚在猫头顶右侧(躺姿头朝左,头顶约在场景 (48..95,156)):
    // 从 (88,134) 漂起,贴着脑袋,不再远远挂在右上角
    s_zzz = sc_img(scr, &kaka_art_prop_zzz, 88, 134);
}

static void build_walk(lv_obj_t *scr, const pet_model_t *m) {
    // 局部随机源:由存档计数混合而成,每次散步配色都不同且可复现
    uint32_t r = m->feed_fish ^ (m->walk_count << 7) ^ (m->brush_strokes << 3) ^
                 (m->sun_count << 11) ^ m->rng;
    r ^= r << 13; r ^= r >> 17; r ^= r << 5;
    // 天空底部小路 + 草
    static const uint32_t skies[3] = { 0x9AD7F5, 0xB7E3F2, 0xF3D9C4 };  // 晴/薄云/黄昏
    lv_obj_set_style_bg_color(scr, lv_color_hex(skies[r % 3]), 0);
    sc_rect(scr, 0, 216, 240, 30, lv_color_hex(0x7CC95E), 0);
    sc_rect(scr, 0, 246, 240, 74, COL_ROAD, 0);
    for (int i = 0; i < 4; i++) {
        s_dashes[i] = sc_rect(scr, i * 70, 292, 36, 6, COL_ROAD_DASH, 2);
    }
    // 房子(视差层,anim 里滚动):从 5 色调色板里挑 3 个不重样的
    static const uint32_t palette[5] = { 0xF7B1A5, 0xA8C8F0, 0xF5D98A,
                                         0xC9B8E8, 0xBFE3C0 };
    for (int i = 0; i < 3; i++) {
        r ^= r << 13; r ^= r >> 17; r ^= r << 5;
        s_houses[i] = sc_rect(scr, 0, 136, 64, 80,
                              lv_color_hex(palette[(r + i) % 5]), 4);
    }
    // 树 + 云(树的绿也换两种)
    r ^= r << 13; r ^= r >> 17; r ^= r << 5;
    uint32_t leaf = (r & 1) ? 0x5FAE4A : 0x4E9E5C;
    for (int i = 0; i < 2; i++) {
        s_trees[i] = sc_rect(scr, 0, 168, 36, 48, lv_color_hex(leaf), 18);
    }
    for (int i = 0; i < 2; i++) {
        s_clouds[i] = sc_rect(scr, 0, 26 + i * 38, 52, 18, COL_WHITE, 9);
    }
    s_collar = sc_img(scr, &kaka_art_prop_collar, 10, 10);
    s_cat = sc_img(scr, &kaka_art_cat_walk0, 56, 92);
    // 蝴蝶随机从左或右侧登场
    s_butterfly = sc_img(scr, &kaka_art_prop_butterfly, (r & 2) ? 20 : 180, 70);
}

void pet_scenes_show(pet_activity_t a, const pet_model_t *m, int battery_soc) {
    s_scene = a;
    sc_reset_state();

    lv_obj_t *scr;
    switch (a) {
    case PET_ACTIVITY_FEED:
        scr = sc_screen(COL_CREAM_BG);
        break;
    case PET_ACTIVITY_BRUSH:
        scr = sc_screen(COL_PEACH_BG);
        break;
    case PET_ACTIVITY_SUN:
        scr = sc_screen(COL_SUNBG);
        break;
    case PET_ACTIVITY_WALK:
        scr = sc_screen(lv_color_hex(0x9AD7F5));  // 底色,build_walk 会随机覆盖
        break;
    default:
        s_scene = PET_ACTIVITY_NONE;
        return;
    }
    pet_ui_install_screen(scr);  // 删旧屏 + 清粒子表
    sc_battery(scr, battery_soc);

    switch (a) {
    case PET_ACTIVITY_FEED:
        build_feed(scr, m);  // m 判断已饱时开局置灰鱼
        pet_audio_play(SND_WATER);
        break;
    case PET_ACTIVITY_BRUSH:
        build_brush(scr);
        break;
    case PET_ACTIVITY_SUN:
        build_sun(scr);
        pet_audio_play(SND_PURR);
        pet_audio_play(SND_BIRDS);
        break;
    case PET_ACTIVITY_WALK:
        build_walk(scr, m);  // m 混出本次配色
        pet_audio_play(SND_BELL);
        pet_audio_play(SND_WIND);
        break;
    default:
        break;
    }
    // 成长阶段与主界面同款:幼猫/少年缩小,脚底对齐
    if (s_cat) {
        lv_image_set_pivot(s_cat, 80, 160);
        uint8_t st = pet_ui_stage();
        lv_image_set_scale(s_cat, st == 0 ? 205 : (st == 1 ? 230 : 256));
    }
}

void pet_scenes_poke(uint32_t now_ms) {
    if (!s_cat) return;
    s_poke_until = now_ms + 450;
    s_poke_base_x = (uint32_t)lv_obj_get_x(s_cat);
    s_poke_active = true;
}

void pet_scenes_interact(pet_interact_t r, uint32_t now_ms) {
    s_last_now = now_ms;
    switch (s_scene) {
    case PET_ACTIVITY_FEED:
        if (r == PET_INTERACT_FED || r == PET_INTERACT_ENDED) {
            s_eat_until = now_ms + 700;
            s_eaten++;
            if (s_eaten < 3 && s_fish[3 - s_eaten]) {
                lv_obj_add_flag(s_fish[3 - s_eaten], LV_OBJ_FLAG_HIDDEN);
            } else if (s_eaten == 3 && s_fish[0]) {
                lv_obj_add_flag(s_fish[0], LV_OBJ_FLAG_HIDDEN);
            }
            pet_ui_spawn_chunk(40, 236, lv_color_hex(0xE0A25E), false);
            pet_ui_spawn_chunk(56, 240, lv_color_hex(0xF2DFC0), false);
            pet_audio_play(SND_MUNCH);
        } else if (r == PET_INTERACT_REFUSED) {
            // 礼貌拒喂:摇头(复用戳一戳演出)+ 置灰鱼 + 明说"吃饱了",
            // 让孩子一眼看懂是猫饱了,而不是按键失灵
            pet_scenes_poke(now_ms);
            for (int i = 0; i < 3; i++) {
                if (s_fish[i]) lv_obj_set_style_img_opa(s_fish[i], 90, 0);
            }
            pet_ui_toast("\u5403\u9971\u4e86!");  // 吃饱了!
            pet_ui_spawn_heart(60, 160);
            pet_audio_play(SND_MEOW_SWEET);
        }
        break;
    case PET_ACTIVITY_BRUSH:
        if (r == PET_INTERACT_BRUSHED || r == PET_INTERACT_SHINE) {
            s_brush_until = now_ms + 600;
            if (s_strokes_shown < 5 && s_paws[s_strokes_shown]) {
                lv_obj_set_style_img_opa(s_paws[s_strokes_shown], 255, 0);
                s_strokes_shown++;
            }
            lv_obj_clear_flag(s_brush, LV_OBJ_FLAG_HIDDEN);
            pet_ui_spawn_chunk(120, 150, lv_color_hex(0xFFF6E8), true);
            pet_ui_spawn_chunk(96, 180, lv_color_hex(0xF5A02E), true);
            pet_audio_play(SND_SPARKLE);
            if (r == PET_INTERACT_SHINE) {
                pet_audio_play(SND_MEOW_SWEET);
            }
        }
        break;
    case PET_ACTIVITY_SUN:
        if (r == PET_INTERACT_WOKE) {
            pet_audio_play(SND_YAWN);
            pet_audio_play(SND_STRETCH);
        }
        break;
    case PET_ACTIVITY_WALK:
        if (r == PET_INTERACT_JUMPED) {
            s_jump_until = now_ms + 600;
            pet_audio_play(SND_BOING);
            pet_ui_spawn_heart(90, 130);
        }
        break;
    default:
        break;
    }
}

// —— 80ms 动画心跳 ——
static void anim_feed(uint32_t now) {
    if (now < s_eat_until) {
        // 咀嚼:eat0→1→2→1 循环
        static const lv_image_dsc_t *const seq[4] = {
            &kaka_art_cat_eat1, &kaka_art_cat_eat2, &kaka_art_cat_eat1,
            &kaka_art_cat_eat0
        };
        lv_image_set_src(s_cat, seq[(now / 120) % 4]);
    } else {
        lv_image_set_src(s_cat, &kaka_art_cat_eat0);
    }
}

static void anim_brush(uint32_t now) {
    if (now < s_brush_until) {
        // 刷子从头扫到尾(600ms),猫眯眼享受
        uint32_t k = s_brush_until - now;  // 剩余毫秒
        int y = 110 + (int)(230 - 110) * (1 - k / 600.0f);
        lv_obj_set_pos(s_brush, 118, y);
        lv_image_set_src(s_cat, &kaka_art_cat_squint);
    } else {
        if (s_brush) lv_obj_add_flag(s_brush, LV_OBJ_FLAG_HIDDEN);
        lv_image_set_src(s_cat, ((now / 800) % 2) ? &kaka_art_cat_sit_b
                                                  : &kaka_art_cat_sit_a);
    }
}

static void anim_sun(uint32_t now) {
    // 慢呼吸
    lv_image_set_src(s_cat, ((now / 1600) % 2) ? &kaka_art_cat_lie_b
                                               : &kaka_art_cat_lie_a);
    // 蝴蝶绕飞(李萨如)
    int bx = 60 + (int)(60.0f * sinf(now / 900.0f));
    int by = 70 + (int)(26.0f * sinf(now / 470.0f));
    lv_obj_set_pos(s_butterfly, bx, by);
    // Zzz 每 2.5s 漂起一次:贴着头顶向右上方散开
    uint32_t ph = now % 2500;
    if (ph < 1400) {
        lv_obj_clear_flag(s_zzz, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(s_zzz, 88 + (int)(ph / 200), 134 - (int)(ph / 160));
    } else {
        lv_obj_add_flag(s_zzz, LV_OBJ_FLAG_HIDDEN);
    }
}

static void anim_walk(uint32_t now) {
    // 扑跳姿态优先;否则 4 帧走路循环
    if (now < s_jump_until) {
        lv_image_set_src(s_cat, &kaka_art_cat_leap);
        lv_obj_set_y(s_cat, 92 - 26);
    } else {
        static const lv_image_dsc_t *const seq[4] = {
            &kaka_art_cat_walk0, &kaka_art_cat_walk1,
            &kaka_art_cat_walk2, &kaka_art_cat_walk3
        };
        lv_image_set_src(s_cat, seq[(now / 150) % 4]);
        lv_obj_set_y(s_cat, 92);
        if ((now % 300) < 80) pet_audio_play(SND_FOOTSTEP);  // 轻脚步
    }
    // 视差:世界向右滚(猫面朝左前进)。层速 云<房<树<路面。
    for (int i = 0; i < 2; i++) {
        lv_obj_set_x(s_clouds[i], (int)((now / 55 + (uint32_t)i * 150) % 340) - 60);
    }
    for (int i = 0; i < 3; i++) {
        lv_obj_set_x(s_houses[i], (int)((now / 30 + (uint32_t)i * 96) % 340) - 70);
    }
    for (int i = 0; i < 2; i++) {
        lv_obj_set_x(s_trees[i], (int)((now / 24 + (uint32_t)i * 170 + 50) % 340) - 70);
    }
    for (int i = 0; i < 4; i++) {
        lv_obj_set_x(s_dashes[i], (int)((now / 15 + (uint32_t)i * 70) % 300) - 40);
    }
    // 蝴蝶在猫前方扑闪
    lv_obj_set_pos(s_butterfly,
                   20 + (int)(40.0f * sinf(now / 800.0f)),
                   60 + (int)(20.0f * sinf(now / 400.0f)));
}

void pet_scenes_anim(const pet_model_t *m, uint32_t now_ms) {
    (void)m;
    if (s_scene == PET_ACTIVITY_NONE || !s_cat) return;
    s_last_now = now_ms;
    if (now_ms < s_poke_until) {  // 戳一戳优先:眯眼 + 左右小幅摆动
        lv_image_set_src(s_cat, &kaka_art_cat_squint);
        lv_obj_set_x(s_cat, (int32_t)(s_poke_base_x + (((now_ms / 90) % 2) ? 2 : -2)));
        return;
    }
    if (s_poke_active) {  // 摆动结束,一次性归位
        lv_obj_set_x(s_cat, (int32_t)s_poke_base_x);
        s_poke_active = false;
    }
    switch (s_scene) {
    case PET_ACTIVITY_FEED: anim_feed(now_ms); break;
    case PET_ACTIVITY_BRUSH: anim_brush(now_ms); break;
    case PET_ACTIVITY_SUN: anim_sun(now_ms); break;
    case PET_ACTIVITY_WALK: anim_walk(now_ms); break;
    default: break;
    }
}
