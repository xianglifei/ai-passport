#!/usr/bin/env bash
# 重新生成 GLM 额度应用的中文子集字库(app_font_16/app_font_20)。
# 来源字体:Noto Sans SC Regular(SIL OFL 1.1),SubsetOTF/SC。
# 用法:./gen_app_fonts.sh /path/to/NotoSansSC-Regular.otf
set -euo pipefail

FONT="${1:?用法: $0 /path/to/NotoSansSC-Regular.otf}"
HERE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
LV_FONT_CONV_VERSION=1.5.3

# 应用全部固定文案的汉字清单(ASCII/·/！走 --range)。
# 新增页面文案后同步更新此清单与 main/app_ui.c 的 APP_GLYPH_SET,再重新生成。
SYMS="额度小时周限刷新连接未配置更下次失败重试长按返回热点手机浏览器打开正在联网等待稍可设入启址络错误效法解超口拒绝一二三四五六日响应析客户端初始番茄工分休作完于弃同条翻页毕中息钟切换成步放已后"
RANGE="0x20-0x7E,0xB7,0xFF01"

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
