#!/usr/bin/env bash
# 重新生成卡卡应用的中文子集字库(app_font_16/app_font_20)。
# 来源字体:Noto Sans SC Regular(SIL OFL 1.1),SubsetOTF/SC,同目录留档。
# 用法:./gen_app_fonts.sh(缺省用本目录 NotoSansSC-Regular.otf,可显式传路径)
set -euo pipefail

FONT="${1:-$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)/NotoSansSC-Regular.otf}"
HERE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
LV_FONT_CONV_VERSION=1.5.3

# 应用全部固定文案的汉字清单(ASCII 走 --range)。
# 新增页面文案后同步更新此清单与 main/pet_font.h 的 PET_GLYPH_SET,再重新生成。
SYMS="卡喂食刷毛晒太阳散步静音开关睡着了晚安"
RANGE="0x20-0x7E"

for size in 16 20; do
    npx -y "lv_font_conv@${LV_FONT_CONV_VERSION}" \
        --font "${FONT}" \
        --range "${RANGE}" \
        --symbols "${SYMS}" \
        --size "${size}" --bpp 4 --format lvgl --no-compress \
        --lv-font-name "app_font_${size}" --lv-include lvgl.h \
        --output "${HERE}/app_font_${size}.c"
done
echo "已生成 app_font_16.c / app_font_20.c"
