#!/usr/bin/env bash
# 重新生成番茄钟应用的中文子集字库(app_font_16/app_font_20)。
# 来源字体:Noto Sans SC Regular(SIL OFL 1.1),SubsetOTF/SC。
# 用法:./gen_app_fonts.sh /path/to/NotoSansSC-Regular.otf
set -euo pipefail

FONT="${1:?用法: $0 /path/to/NotoSansSC-Regular.otf}"
HERE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
LV_FONT_CONV_VERSION=1.5.3

# 应用全部固定文案的汉字清单(ASCII/·/！走 --range)。
# 新增页面文案后同步更新此清单与 main/app_ui.c 的 APP_GLYPH_SET,再重新生成。
SYMS="番茄钟工作休息分钟开始专注放松一下热力图选择确定完成今日第个吧月周换天返回设置时间保存取消微调快调小时中长按放弃完毕二三四五六日于对连接已功失败等待机热点正重试浏览打开输入家里网密码页在请上址"
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
