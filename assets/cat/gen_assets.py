#!/usr/bin/env python3
"""卡卡(Kaka)像素资产生成器 —— 确定性绘制橘猫全套动画帧与道具精灵。

设计约定:
  - 所有精灵在"逻辑像素"网格上手工绘制(40x40 猫 / 小道具自然尺寸),
    导出时最近邻放大 4 倍,得到设备上的块状像素(chunky)画面。
  - 全部精灵共享一个 16 色 RGBA 调色板,索引 0 恒为透明。
  - 输出三样东西:
      PNG 预览(接触表,供人审阅形象)
      单帧 PNG(assets/cat/png/,透明底)
      C 源码(main/pet_art.c,LVGL I4 索引格式,palette+索引面,stride 4 字节对齐)
  - 无随机数:同一次运行永远生成同一份资产(可复现)。

用法:
  python3 assets/cat/gen_assets.py            # 生成预览 + PNG + C 源码
  python3 assets/cat/gen_assets.py --png-only # 只刷新预览,便于调形象
"""

from __future__ import annotations

import argparse
import os
from pathlib import Path

try:
    from PIL import Image, ImageDraw
except ImportError as exc:  # pragma: no cover
    raise SystemExit("需要 Pillow: python3 -m pip install --user pillow") from exc

# ---------------------------------------------------------------------------
# 调色板:全部精灵共用。索引 0 = 透明。
# 颜色按"轮廓/橘猫皮毛/五官/道具"分区,改色只改这里。
# ---------------------------------------------------------------------------

TRANSP, OUTLINE = 0, 1
ORANGE, ORANGE_DK, ORANGE_DP = 2, 3, 4          # 橘猫三阶橙
CREAM, WHITE = 5, 6                             # 奶白口鼻/胸脯, 高光
EYE_DK, GOLD = 7, 8                             # 眼睛与金色点缀
PINK_NOSE, PINK_IN, BLUSH = 9, 10, 11           # 鼻头/耳内/腮红
SUN_Y, SKY_BL, LEAF_G = 12, 13, 14              # 道具:太阳黄/蓝/绿叶
RED = 15                                        # 项圈/食碗红

PALETTE_RGB = {
    TRANSP:  (0, 0, 0, 0),
    OUTLINE: (53, 35, 27, 255),     # 深棕轮廓
    ORANGE:  (245, 160, 46, 255),   # 主毛色
    ORANGE_DK: (217, 124, 27, 255), # 虎斑纹/阴影
    ORANGE_DP: (184, 94, 18, 255),  # 深条纹
    CREAM:  (251, 239, 221, 255),
    WHITE:  (255, 255, 255, 255),
    EYE_DK: (36, 24, 18, 255),
    GOLD:   (255, 204, 77, 255),
    PINK_NOSE: (242, 109, 156, 255),
    PINK_IN:  (248, 175, 196, 255),
    BLUSH:  (255, 158, 158, 255),
    SUN_Y:  (255, 213, 74, 255),
    SKY_BL: (78, 168, 222, 255),
    LEAF_G: (107, 191, 89, 255),
    RED:    (224, 82, 82, 255),
}

# ---------------------------------------------------------------------------
# 网格绘制原语。grid: list[list[int]],g[y][x],越界自动忽略。
# ---------------------------------------------------------------------------


class Grid:
    def __init__(self, w: int, h: int):
        self.w, self.h = w, h
        self.g = [[TRANSP] * w for _ in range(h)]

    def px(self, x: int, y: int, c: int) -> None:
        if 0 <= x < self.w and 0 <= y < self.h:
            self.g[y][x] = c

    def get(self, x: int, y: int) -> int:
        if 0 <= x < self.w and 0 <= y < self.h:
            return self.g[y][x]
        return TRANSP

    def rect(self, x: int, y: int, w: int, h: int, c: int) -> None:
        for yy in range(y, y + h):
            for xx in range(x, x + w):
                self.px(xx, yy, c)

    def hline(self, x0: int, x1: int, y: int, c: int) -> None:
        for x in range(min(x0, x1), max(x0, x1) + 1):
            self.px(x, y, c)

    def vline(self, x: int, y0: int, y1: int, c: int) -> None:
        for y in range(min(y0, y1), max(y0, y1) + 1):
            self.px(x, y, c)

    def disc(self, cx: float, cy: float, r: float, c: int) -> None:
        for y in range(int(cy - r) - 1, int(cy + r) + 2):
            for x in range(int(cx - r) - 1, int(cx + r) + 2):
                if (x - cx) ** 2 + (y - cy) ** 2 <= r * r + 0.1:
                    self.px(x, y, c)

    def ell(self, cx: float, cy: float, rx: float, ry: float, c: int) -> None:
        for y in range(int(cy - ry) - 1, int(cy + ry) + 2):
            for x in range(int(cx - rx) - 1, int(cx + rx) + 2):
                if ((x - cx) / max(rx, 0.01)) ** 2 + ((y - cy) / max(ry, 0.01)) ** 2 <= 1.04:
                    self.px(x, y, c)

    def tri(self, p1, p2, p3, c: int) -> None:
        """整数三角形(半平面法),顶点为 (x, y) 元组。"""
        (x1, y1), (x2, y2), (x3, y3) = p1, p2, p3
        minx, maxx = min(x1, x2, x3), max(x1, x2, x3)
        miny, maxy = min(y1, y2, y3), max(y1, y2, y3)
        d = (y2 - y3) * (x1 - x3) + (x3 - x2) * (y1 - y3)

        def side(x, y):
            a = (y2 - y3) * (x - x3) + (x3 - x2) * (y - y3)
            b = (y3 - y1) * (x - x3) + (x1 - x3) * (y - y3)
            cc = d - a - b
            return (a >= 0 and b >= 0 and cc >= 0) or (a <= 0 and b <= 0 and cc <= 0)

        for y in range(miny, maxy + 1):
            for x in range(minx, maxx + 1):
                if side(x, y):
                    self.px(x, y, c)

    def stamp(self, src: "Grid", ox: int, oy: int) -> None:
        for y in range(src.h):
            for x in range(src.w):
                c = src.get(x, y)
                if c != TRANSP:
                    self.px(x + ox, y + oy, c)

    def outline(self, oc: int = OUTLINE) -> None:
        """在剪影外扩 1px 轮廓(贴纸感)。只补空像素,不改已画的。"""
        adds = []
        for y in range(self.h):
            for x in range(self.w):
                if self.get(x, y) != TRANSP:
                    continue
                for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                    if self.get(x + dx, y + dy) not in (TRANSP,):
                        adds.append((x, y))
                        break
        for x, y in adds:
            self.px(x, y, oc)


# ---------------------------------------------------------------------------
# 橘猫卡卡 —— 正面坐姿(主界面 idle)
# ---------------------------------------------------------------------------


def cat_front(eyes: str = "open", breath: int = 0, tail_phase: int = 0,
              mouth: str = "smile", blush: bool = True) -> Grid:
    """正面坐姿。eyes: open/blink/happy/squint;breath 0/1 呼吸;
    tail_phase 0/1 尾巴轻摆。"""
    g = Grid(40, 40)
    dy = 1 if breath else 0  # 呼吸时整体下沉 1px(身体变高会顶格,改下沉)

    # 耳朵(先画,压在头下面)
    g.tri((9, 13 + dy), (12, 5 + dy), (17, 11 + dy), ORANGE)
    g.tri((23, 11 + dy), (28, 5 + dy), (31, 13 + dy), ORANGE)
    g.tri((11, 12 + dy), (13, 7 + dy), (15, 11 + dy), PINK_IN)
    g.tri((25, 11 + dy), (27, 7 + dy), (29, 12 + dy), PINK_IN)

    # 头(大圆脸)
    g.ell(19, 15 + dy, 11.4, 10.2, ORANGE)
    # 身体(梨形)
    g.ell(19, 28 + dy, 8.6, 9.4, ORANGE)

    # 虎斑:额头 M 纹 + 两侧颊纹
    for x in (15, 19, 23):
        g.vline(x, 6 + dy, 9 + dy, ORANGE_DK)
    g.vline(15, 6 + dy, 7 + dy, ORANGE_DP)
    g.vline(23, 6 + dy, 7 + dy, ORANGE_DP)
    g.vline(9, 14 + dy, 16 + dy, ORANGE_DK)
    g.vline(30, 14 + dy, 16 + dy, ORANGE_DK)

    # 奶白口鼻(两团圆)与胸脯
    g.disc(16, 20 + dy, 3.2, CREAM)
    g.disc(23, 20 + dy, 3.2, CREAM)
    g.ell(19, 28 + dy, 5.4, 6.4, CREAM)

    # 眼睛
    ye = 15 + dy
    if eyes == "open":
        for ex in (12, 25):
            g.rect(ex, ye - 1, 3, 4, EYE_DK)
            g.px(ex, ye - 1, WHITE)   # 高光
            g.px(ex, ye + 1, GOLD)    # 眼底反光
    elif eyes == "blink":  # ⌒ 弧线闭眼
        g.px(11, ye + 1, EYE_DK)
        g.hline(12, 13, ye, EYE_DK)
        g.px(14, ye + 1, EYE_DK)
        g.px(24, ye + 1, EYE_DK)
        g.hline(25, 26, ye, EYE_DK)
        g.px(27, ye + 1, EYE_DK)
    elif eyes == "happy":  # ^ ^ 笑眼
        for ex in (12, 25):
            g.px(ex, ye + 1, EYE_DK)
            g.px(ex + 1, ye, EYE_DK)
            g.px(ex + 2, ye + 1, EYE_DK)
    elif eyes == "squint":  # > < 满足眯眼
        g.px(11, ye - 1, EYE_DK)
        g.px(12, ye, EYE_DK)
        g.px(13, ye - 1, EYE_DK)
        g.px(25, ye - 1, EYE_DK)
        g.px(26, ye, EYE_DK)
        g.px(27, ye - 1, EYE_DK)

    # 鼻子 + 嘴
    g.rect(18, 18 + dy, 3, 1, PINK_NOSE)
    if mouth == "smile":
        g.px(17, 20 + dy, EYE_DK)
        g.hline(18, 20, 21 + dy, EYE_DK)
        g.px(21, 20 + dy, EYE_DK)
    elif mouth == "open":  # 开口笑
        g.rect(18, 20 + dy, 3, 2, EYE_DK)
        g.px(19, 21 + dy, PINK_NOSE)

    # 腮红
    if blush:
        g.hline(8, 10, 19 + dy, BLUSH)
        g.hline(28, 30, 19 + dy, BLUSH)

    # 前爪(奶白,带趾线)
    for px_ in (14, 23):
        g.ell(px_ + 1.5, 36 + dy, 3.2, 2.2, CREAM)
        g.vline(px_ + 1, 35 + dy, 36 + dy, ORANGE_DK)

    # 尾巴:从右侧绕到身前,尾尖相位轻摆
    tail = [(27, 30), (29, 31), (30, 33), (29, 35), (26, 36), (22, 36)]
    if tail_phase:
        tail = [(27, 30), (29, 32), (30, 34), (28, 36), (25, 37), (21, 37)]
    for i, (tx, ty) in enumerate(tail):
        g.disc(tx, ty + dy, 1.9, ORANGE)
    for tx, ty in (tail[2], tail[4]):
        g.px(tx, ty + dy, ORANGE_DK)
    g.disc(tail[-1][0], tail[-1][1] + dy, 1.9, ORANGE_DK)  # 尾尖深色

    g.outline()

    # 胡须(轮廓外补画,1px 奶白)
    for y in (17 + dy, 19 + dy):
        g.hline(4, 7, y, CREAM)
        g.hline(32, 35, y, CREAM)
    return g


# ---------------------------------------------------------------------------
# 侧面视图(走路/吃/躺/扑)基件
# ---------------------------------------------------------------------------


def _side_head(g: Grid, cx: float, cy: float, eyes: str = "open",
               ear_back: bool = False) -> None:
    """朝左的侧头。ear_back=True 时耳朵后压(扑跳)。"""
    g.disc(cx, cy, 6.6, ORANGE)
    # 耳朵
    if ear_back:
        g.tri((int(cx) + 1, int(cy) - 5), (int(cx) + 4, int(cy) - 8), (int(cx) + 5, int(cy) - 3), ORANGE)
        g.tri((int(cx) - 3, int(cy) - 5), (int(cx) - 1, int(cy) - 8), (int(cx) + 1, int(cy) - 4), ORANGE)
    else:
        g.tri((int(cx) - 4, int(cy) - 4), (int(cx) - 2, int(cy) - 9), (int(cx) + 1, int(cy) - 5), ORANGE)
        g.tri((int(cx) + 1, int(cy) - 5), (int(cx) + 4, int(cy) - 8), (int(cx) + 5, int(cy) - 2), ORANGE)
        g.tri((int(cx) - 2, int(cy) - 5), (int(cx) - 1, int(cy) - 7), (int(cx) + 0, int(cy) - 5), PINK_IN)
    # 口鼻
    g.ell(cx - 4.6, cy + 1.6, 3.4, 2.8, CREAM)
    g.px(int(cx) - 7, int(cy) + 1, PINK_NOSE)  # 鼻尖
    # 眼睛
    if eyes == "open":
        g.rect(int(cx) - 3, int(cy) - 2, 2, 3, EYE_DK)
        g.px(int(cx) - 3, int(cy) - 2, WHITE)
    else:
        g.hline(int(cx) - 3, int(cx) - 2, int(cy) - 1, EYE_DK)
    # 颊纹
    g.vline(int(cx) + 3, int(cy) - 1, int(cy) + 2, ORANGE_DK)


def _side_body(g: Grid, cx: float, cy: float, rx: float, ry: float) -> None:
    g.ell(cx, cy, rx, ry, ORANGE)
    # 背部虎斑(3 道)
    for i in range(3):
        x = int(cx) - 4 + i * 4
        g.vline(x, int(cy - ry) + 1, int(cy - ry) + 3, ORANGE_DK)
    # 胸口奶白
    g.ell(cx - rx + 2.5, cy + 1, 2.4, 3.4, CREAM)


def _tail_up(g: Grid, x0: int, y0: int, sway: int, rings: bool = True) -> None:
    """竖起的高尾巴,sway 摆动相位(-1/0/1)。"""
    pts = [(x0, y0), (x0 + 2, y0 - 3), (x0 + 2 + sway, y0 - 6), (x0 + 1 + sway, y0 - 8)]
    for tx, ty in pts:
        g.disc(tx, ty, 1.9, ORANGE)
    if rings:
        g.px(pts[1][0], pts[1][1], ORANGE_DK)
        g.px(pts[2][0], pts[2][1], ORANGE_DK)
    g.disc(pts[-1][0], pts[-1][1], 1.9, ORANGE_DK)


def cat_walk(phase: int) -> Grid:
    """侧面走路 4 帧:双腿对角交替,远侧腿用深橙拉开前后层次。"""
    g = Grid(40, 40)
    bob = 1 if phase in (1, 3) else 0  # 身体起伏
    _side_body(g, 22, 21 + bob, 11, 7)
    _side_head(g, 10, 15 + bob)
    _tail_up(g, 31, 17 + bob, sway=(phase % 2))

    def leg(x: int, swing: int, far: bool) -> None:
        color = ORANGE_DK if far else ORANGE
        g.rect(x - 1, 26 + bob, 3, 4, color)          # 大腿
        g.rect(x - 1 + swing, 30 + bob, 3, 4, color)  # 小腿随摆动偏移
        g.ell(x + swing, 34 + bob, 2.2, 1.4,
              ORANGE_DP if far else CREAM)

    near_swing = (-2, 0, 2, 0)[phase]
    far_swing = (2, 0, -2, 0)[phase]
    leg(11, far_swing, far=True)    # 远前腿(先画,被近腿压住)
    leg(25, far_swing, far=True)    # 远后腿
    leg(15, near_swing, far=False)  # 近前腿
    leg(29, near_swing, far=False)  # 近后腿
    g.outline()
    return g


def cat_eat(stage: int) -> Grid:
    """侧面吃鱼 3 帧:0 低头咬/1 抬头嚼/2 深低头。碗由场景单独贴。"""
    g = Grid(40, 40)
    _side_body(g, 23, 21, 11, 7)
    if stage == 1:  # 抬头嚼
        _side_head(g, 11, 14)
        g.rect(6, 15, 3, 2, EYE_DK)  # 张嘴嚼
        g.px(7, 16, PINK_NOSE)
    else:  # 低头,口鼻朝下接近碗
        hy = 19 if stage == 0 else 20  # 头心 y
        g.disc(10, hy, 6.6, ORANGE)
        g.tri((5, hy - 4), (7, hy - 9), (10, hy - 5), ORANGE)      # 耳
        g.tri((10, hy - 5), (13, hy - 8), (14, hy - 3), ORANGE)
        g.tri((7, hy - 5), (8, hy - 7), (9, hy - 5), PINK_IN)
        g.ell(7, hy + 5, 3.4, 2.8, CREAM)                            # 朝下的口鼻
        g.px(5, hy + 4, PINK_NOSE)
        g.hline(6, 9, hy - 3, EYE_DK)                                # 眯眼享受
        g.hline(8, 10, hy - 6, BLUSH)
    # 前腿支撑
    g.rect(12, 26, 3, 9, ORANGE)
    g.rect(16, 26, 3, 9, ORANGE)
    g.rect(27, 26, 3, 9, ORANGE)
    g.rect(30, 26, 3, 9, ORANGE)
    for lx in (13, 17, 28, 31):
        g.ell(lx, 35, 2.4, 1.6, CREAM)
    _tail_up(g, 32, 18, sway=stage % 2)
    g.outline()
    return g


def cat_lie(sleep: bool = False, breath: int = 0) -> Grid:
    """侧躺蜷成团(breath 0/1 呼吸;sleep 闭眼版)。"""
    g = Grid(40, 40)
    ry = 7.4 + (0.6 if breath else 0)
    g.ell(21, 28, 13.4, ry, ORANGE)
    # 背部虎斑
    for i in range(4):
        g.vline(12 + i * 5, int(28 - ry) + 1, int(28 - ry) + 3, ORANGE_DK)
    # 头枕在身体前上
    g.disc(11, 23, 6.4, ORANGE)
    g.tri((6, 20), (8, 15), (12, 19), ORANGE)
    g.tri((12, 19), (15, 16), (16, 21), ORANGE)
    g.tri((8, 19), (9, 17), (10, 19), PINK_IN)
    g.ell(6, 25, 3.2, 2.6, CREAM)      # 口鼻
    g.px(4, 24, PINK_NOSE)
    if sleep:
        g.px(6, 20, EYE_DK)            # > < 安睡眼
        g.px(7, 21, EYE_DK)
        g.px(8, 20, EYE_DK)
    else:
        g.rect(7, 20, 2, 3, EYE_DK)
        g.px(7, 20, WHITE)
    g.hline(8, 10, 18, BLUSH)
    # 尾巴绕到身前
    for tx, ty in ((32, 30), (28, 33), (22, 34), (15, 34), (9, 33)):
        g.disc(tx, ty, 1.9, ORANGE)
    g.disc(9, 33, 1.9, ORANGE_DK)
    g.px(28, 33, ORANGE_DK)
    g.px(22, 34, ORANGE_DK)
    g.outline()
    return g


def cat_crouch_frame(tail_phase: int) -> Grid:
    """扑跳前压低身子,只有尾巴在摆(蓄力感)。"""
    g = Grid(40, 40)
    g.ell(22, 27, 11, 5.4, ORANGE)          # 压低的身体
    g.disc(30, 23, 4.4, ORANGE)             # 微微抬起的臀部
    _side_head(g, 9, 24, eyes="open")
    # 收拢的短腿
    g.rect(10, 29, 3, 5, ORANGE)
    g.rect(14, 30, 3, 4, ORANGE)
    g.rect(28, 29, 3, 5, ORANGE)
    g.rect(31, 30, 3, 4, ORANGE)
    for lx in (11, 15, 29, 32):
        g.ell(lx, 34, 2.2, 1.4, CREAM)
    _tail_up(g, 33, 20, sway=tail_phase * 2 - 1)
    g.outline()
    return g


def cat_leap() -> Grid:
    """腾空扑跳(朝左):前高后低的弧形身体,前伸后蹬,尾巴接在臀上。"""
    g = Grid(40, 40)
    # 弧形身体:三段椭圆叠出前高后低的跃姿曲线
    g.ell(15, 13, 6.4, 4.6, ORANGE)
    g.ell(22, 15, 7.4, 4.8, ORANGE)
    g.ell(29, 17, 5.6, 4.4, ORANGE)
    for i in range(3):  # 背部虎斑沿弧线
        g.vline(15 + i * 5, 10 + (1 if i == 2 else 0), 12 + (1 if i == 2 else 0), ORANGE_DK)
    _side_head(g, 8, 9, eyes="open", ear_back=True)
    g.ell(11, 16, 3.0, 2.4, CREAM)  # 胸口奶白
    # 前腿:前下方伸
    g.rect(2, 16, 6, 2, ORANGE)
    g.ell(2, 17, 2.2, 1.4, CREAM)
    g.rect(5, 19, 5, 2, ORANGE)
    g.ell(5, 20, 2.2, 1.4, CREAM)
    # 后腿:大腿 + 后蹬小腿
    g.ell(29, 18, 4.2, 3.2, ORANGE)
    g.rect(31, 20, 6, 2, ORANGE)
    g.ell(36, 21, 2.2, 1.4, CREAM)
    # 尾巴:从臀上方顺势向后上飘(首圆叠进身体)
    for tx, ty in ((32, 13), (34, 11), (36, 9), (38, 7)):
        g.disc(tx, ty, 1.7, ORANGE)
    g.disc(38, 7, 1.7, ORANGE_DK)
    g.outline()
    return g


# ---------------------------------------------------------------------------
# 道具精灵
# ---------------------------------------------------------------------------


def prop_fish() -> Grid:
    g = Grid(16, 10)
    g.ell(7, 4, 5.4, 3.4, SKY_BL)
    g.tri((11, 1), (15, 4), (11, 7), SKY_BL)
    g.px(4, 3, EYE_DK)
    g.px(4, 4, WHITE)
    g.hline(2, 5, 6, WHITE)
    g.outline()
    return g


def prop_bowl() -> Grid:
    g = Grid(20, 12)
    g.ell(9, 4, 8.4, 3.4, CREAM)     # 碗口奶白沿
    g.ell(9, 5, 6.4, 2.2, RED)       # 碗口内壁
    g.ell(9, 6, 7.4, 4.4, RED)       # 碗身
    g.rect(3, 6, 13, 4, RED)
    g.hline(4, 14, 9, ORANGE_DK)
    g.hline(5, 13, 3, WHITE)          # 高光
    g.outline()
    return g


def prop_brush() -> Grid:
    """横向木柄刷:木刷背 + 三排金色鬃毛针朝下。"""
    g = Grid(16, 12)
    g.ell(9, 5, 4.6, 3.4, ORANGE_DP)   # 木刷背
    g.hline(6, 12, 2, CREAM)           # 背板高光
    for x in (7, 9, 11):               # 鬃毛针朝下
        g.vline(x, 8, 11, GOLD)
    g.rect(1, 4, 5, 3, CREAM)          # 手柄
    g.hline(1, 5, 4, WHITE)
    g.outline()
    return g


def prop_sun(face: bool = True) -> Grid:
    g = Grid(24, 24)
    c = 11
    for a in range(8):  # 光芒
        import math
        x0 = c + 8 * math.cos(a * math.pi / 4)
        y0 = c + 8 * math.sin(a * math.pi / 4)
        x1 = c + 10.6 * math.cos(a * math.pi / 4)
        y1 = c + 10.6 * math.sin(a * math.pi / 4)
        g.disc(x0, y0, 1.2, SUN_Y)
        g.disc(x1, y1, 0.9, SUN_Y)
    g.disc(c, c, 6.6, SUN_Y)
    g.disc(c - 1, c - 1, 4.4, GOLD)
    if face:
        g.px(9, 10, EYE_DK)
        g.px(13, 10, EYE_DK)
        g.px(9, 12, EYE_DK)
        g.px(13, 12, EYE_DK)
        g.hline(10, 12, 13, EYE_DK)  # 微笑
    g.outline()
    return g


def prop_butterfly() -> Grid:
    g = Grid(12, 10)
    g.ell(3, 3, 2.8, 2.6, SKY_BL)
    g.ell(3, 7, 2.4, 2.0, PINK_IN)
    g.ell(8, 3, 2.8, 2.6, PINK_IN)
    g.ell(8, 7, 2.4, 2.0, SKY_BL)
    g.rect(5, 3, 2, 5, EYE_DK)
    g.px(5, 2, GOLD)
    g.px(6, 2, GOLD)
    g.outline()
    return g


def prop_heart() -> Grid:
    g = Grid(9, 8)
    pat = [
        ".##...##.",
        ".##...##.",
        "#########",
        "#########",
        ".#######.",
        "..#####..",
        "...###...",
        "....#....",
    ]
    for y, row in enumerate(pat):
        for x, ch in enumerate(row):
            if ch == "#":
                g.px(x + 0, y, RED)
    # 高光
    g.px(2, 1, WHITE)
    g.outline()
    return g


def prop_zzz() -> Grid:
    g = Grid(16, 14)
    z3 = [
        "###",
        "..#",
        ".#.",
        "#..",
        "###",
    ]

    def stamp_z(ox: int, oy: int, scale: int, c: int) -> None:
        for y, row in enumerate(z3):
            for x, ch in enumerate(row):
                if ch == "#":
                    g.rect(ox + x * scale, oy + y * scale, scale, scale, c)
    stamp_z(9, 0, 1, SKY_BL)
    stamp_z(5, 4, 1, SKY_BL)
    stamp_z(0, 9, 2, SKY_BL)
    g.outline()
    return g


def prop_moon() -> Grid:
    g = Grid(14, 14)
    g.disc(7, 6, 6.2, SUN_Y)
    for y in range(g.h):  # 右上挖出月牙缺口
        for x in range(g.w):
            if (x - 10.5) ** 2 + (y - 3) ** 2 <= 4.9 ** 2:
                g.px(x, y, TRANSP)
    g.outline()
    return g


def prop_yarn() -> Grid:
    g = Grid(12, 12)
    g.disc(5, 6, 4.6, PINK_IN)
    g.hline(2, 8, 5, PINK_NOSE)
    g.vline(5, 3, 9, PINK_NOSE)
    g.hline(9, 11, 10, PINK_NOSE)  # 线头
    g.outline()
    return g


def prop_sparkle() -> Grid:
    g = Grid(9, 9)
    g.disc(4, 4, 1.2, WHITE)
    g.hline(0, 8, 4, GOLD)
    g.vline(4, 0, 8, GOLD)
    g.px(4, 4, WHITE)
    return g


def prop_paw() -> Grid:
    g = Grid(12, 12)
    g.ell(5, 8, 3.8, 3.0, ORANGE_DK)
    g.disc(2, 4, 1.5, ORANGE_DK)
    g.disc(5, 2, 1.5, ORANGE_DK)
    g.disc(8, 4, 1.5, ORANGE_DK)
    g.outline()
    return g


def prop_collar() -> Grid:
    """红项圈 + 大金铃铛(散步挂绳意象)。"""
    g = Grid(16, 12)
    for y in (3, 4):
        g.hline(2, 13, y, RED)         # 项圈带
    g.px(1, 4, RED)
    g.px(14, 4, RED)
    g.disc(7, 8, 2.9, GOLD)            # 大金铃
    g.px(7, 7, EYE_DK)                 # 铃口
    g.px(6, 9, EYE_DK)
    g.px(5, 7, WHITE)                  # 高光
    g.outline()
    return g


# ---------------------------------------------------------------------------
# 帧清单
# ---------------------------------------------------------------------------

SCALE = 4       # 逻辑像素 → 屏幕像素倍率(猫与场景道具)
ICON_SCALE = 2  # dock 图标倍率(塞进 56px 图标槽)


def build_frames() -> list[tuple[str, Grid, int]]:
    """返回 (名字, 网格, 导出倍率) 列表;顺序即接触表顺序。"""
    cats = {
        # 主界面 idle
        "cat_sit_a": cat_front("open", breath=0, tail_phase=0),
        "cat_sit_b": cat_front("open", breath=1, tail_phase=1),
        "cat_blink": cat_front("blink", breath=0, tail_phase=0),
        "cat_happy": cat_front("happy", breath=1, tail_phase=0, mouth="open"),
        "cat_squint": cat_front("squint", breath=1, tail_phase=1),
        # 走路
        "cat_walk0": cat_walk(0),
        "cat_walk1": cat_walk(1),
        "cat_walk2": cat_walk(2),
        "cat_walk3": cat_walk(3),
        # 吃
        "cat_eat0": cat_eat(0),
        "cat_eat1": cat_eat(1),
        "cat_eat2": cat_eat(2),
        # 躺/睡
        "cat_lie_a": cat_lie(sleep=False, breath=0),
        "cat_lie_b": cat_lie(sleep=False, breath=1),
        "cat_sleep": cat_lie(sleep=True, breath=0),
        # 扑跳
        "cat_crouch": cat_crouch_frame(0),
        "cat_crouch2": cat_crouch_frame(1),
        "cat_leap": cat_leap(),
    }
    props = {
        "prop_fish": prop_fish(),
        "prop_bowl": prop_bowl(),
        "prop_brush": prop_brush(),
        "prop_sun": prop_sun(),
        "prop_butterfly": prop_butterfly(),
        "prop_heart": prop_heart(),
        "prop_zzz": prop_zzz(),
        "prop_moon": prop_moon(),
        "prop_yarn": prop_yarn(),
        "prop_sparkle": prop_sparkle(),
        "prop_paw": prop_paw(),
        "prop_collar": prop_collar(),
    }
    out = [(n, g, SCALE) for n, g in cats.items()]
    out += [(n, g, SCALE) for n, g in props.items()]
    # dock 图标:同名道具的 ×2 小版,槽位内显示
    out += [
        ("icon_feed", props["prop_fish"], ICON_SCALE),
        ("icon_brush", props["prop_brush"], ICON_SCALE),
        ("icon_sun", props["prop_sun"], ICON_SCALE),
        ("icon_walk", props["prop_paw"], ICON_SCALE),
    ]
    return out


# ---------------------------------------------------------------------------
# 导出:PNG 预览 + 单帧 PNG + C 源码(LVGL I4)
# ---------------------------------------------------------------------------

SKY_BG = (168, 220, 247)     # 预览底色(仅预览用)


def grid_to_image(g: Grid, scale: int, bg=None) -> Image.Image:
    img = Image.new("RGBA", (g.w * scale, g.h * scale),
                    bg + (255,) if bg else (0, 0, 0, 0))
    for y in range(g.h):
        for x in range(g.w):
            c = g.get(x, y)
            if c != TRANSP:
                img.putpixel((x, y), PALETTE_RGB[c]) if scale == 1 else img.paste(
                    PALETTE_RGB[c], (x * scale, y * scale, (x + 1) * scale, (y + 1) * scale))
    return img


def emit_pngs(frames: list, out_dir: Path) -> None:
    out_dir.mkdir(parents=True, exist_ok=True)
    for name, g, scale in frames:
        grid_to_image(g, scale).save(out_dir / f"{name}.png")


def emit_contact_sheet(frames: list, path: Path) -> None:
    """接触表:按行排布 + 名字标签,底色天空绿草。"""
    names = [(n, s) for n, _, s in frames]
    cell_w, cell_h = 40 * SCALE, 40 * SCALE
    label_h = 18
    cols = 6
    rows = (len(names) + cols - 1) // cols
    sheet = Image.new("RGB", (cols * cell_w, rows * (cell_h + label_h)),
                      SKY_BG)
    # 草地条
    dr = ImageDraw.Draw(sheet)
    for r in range(rows):
        y0 = r * (cell_h + label_h)
        dr.rectangle([0, y0 + int(cell_h * 0.78), sheet.width, y0 + int(cell_h * 0.82)],
                     fill=(126, 200, 110))
    for i, (name, g, scale) in enumerate(frames):
        cx, cy = divmod(i, cols)[1], divmod(i, cols)[0]
        x0, y0 = cx * cell_w, cy * (cell_h + label_h)
        img = grid_to_image(g, scale)
        sheet.paste(img, (x0 + (cell_w - img.width) // 2,
                          y0 + (cell_h - img.height) // 2), img)
        dr.text((x0 + 6, y0 + cell_h + 1), name, fill=(40, 30, 25))
    sheet.save(path)


def emit_c_source(frames: list, path: Path, header: Path) -> None:
    """生成 main/pet_art.c/.h:LVGL RGB565A8(真机已验证的直通 blit 格式)。

    数据布局(lv_draw_sw_img.c 的 RGB565A8 快路径约定):
      [RGB565 平面, stride = w*2][A8 alpha 平面, stride = w]
    A8 步长必须是 RGB 步长的一半;所有导出宽度为偶数(×2/×4 放大保证)。
    之前的 I4 索引方案会被解码器整图转 ARGB8888(一张 160x160 临时 100KB
    堆,且 lv_color32_t 内存序是 BGRA、调色板字节序易错),真机乱码,弃用。
    """

    def rgb565(rgb) -> int:
        r, g, b, _ = rgb
        return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)

    lines = [
        "// main/pet_art.c —— 卡卡像素资产(LVGL RGB565A8:RGB565 面 + A8 面)。",
        "// 由 assets/cat/gen_assets.py 生成,请勿手改;改形象请改生成器后重新生成。",
        "#include \"pet_art.h\"",
        "",
    ]
    hdr_decl = [
        "// main/pet_art.h —— 卡卡像素资产描述符。",
        "// 由 assets/cat/gen_assets.py 生成,请勿手改。",
        "#pragma once",
        "",
        "#include \"lvgl.h\"",
        "",
    ]
    for name, g, scale in frames:
        w, h = g.w * scale, g.h * scale
        rgb_stride = w * 2
        data = bytearray()
        # RGB565 平面(按调色板展开到导出像素)
        for y in range(g.h):
            row = bytearray()
            for x in range(g.w):
                c = g.get(x, y)
                px = rgb565(PALETTE_RGB[c]) if c != TRANSP else 0
                for _ in range(scale):
                    row += px.to_bytes(2, "little")
            for _ in range(scale):
                data += row
        # A8 alpha 平面(非透明=255)
        for y in range(g.h):
            row = bytearray()
            for x in range(g.w):
                a = 0 if g.get(x, y) == TRANSP else 255
                for _ in range(scale):
                    row.append(a)
            for _ in range(scale):
                data += row

        lines.append(f"static const uint8_t kaka_{name}_data[{len(data)}] = {{")
        for i in range(0, len(data), 16):
            chunk = ", ".join(f"0x{b:02X}" for b in data[i:i + 16])
            lines.append(f"    {chunk},")
        lines.append("};")
        lines.append(f"const lv_image_dsc_t kaka_art_{name} = {{")
        lines.append("    .header = {")
        lines.append("        .magic = LV_IMAGE_HEADER_MAGIC,")
        lines.append("        .cf = LV_COLOR_FORMAT_RGB565A8,")
        lines.append("        .flags = 0,")
        lines.append(f"        .w = {w},")
        lines.append(f"        .h = {h},")
        lines.append(f"        .stride = {rgb_stride},")
        lines.append("    },")
        lines.append(f"    .data_size = {len(data)},")
        lines.append(f"    .data = kaka_{name}_data,")
        lines.append("};")
        lines.append("")
        hdr_decl.append(f"extern const lv_image_dsc_t kaka_art_{name};")

    path.write_text("\n".join(lines), encoding="utf-8")
    header.write_text("\n".join(hdr_decl) + "\n", encoding="utf-8")


# ---------------------------------------------------------------------------


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--png-only", action="store_true",
                    help="只刷新 PNG 预览(调形象用),不写 C 源码")
    args = ap.parse_args()

    root = Path(__file__).resolve().parent
    frames = build_frames()

    emit_contact_sheet(frames, root / "preview_kaka.png")
    emit_pngs(frames, root / "png")
    if not args.png_only:
        main_dir = root.parent.parent / "main"
        emit_c_source(frames, main_dir / "pet_art.c", main_dir / "pet_art.h")
        print(f"C 源码: main/pet_art.c ({len(frames)} 帧)")
    print(f"预览: {root / 'preview_kaka.png'}; 单帧 PNG: {root / 'png'}/")


if __name__ == "__main__":
    main()
