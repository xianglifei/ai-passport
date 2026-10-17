#!/usr/bin/env bash
# 重新生成卡卡的声音采样(main/snd/*.pcm)。
# 来源:assets/snd/src/ 下的 CC0 猫叫 WAV(来源与许可证见 assets/snd/README.md)。
# 处理:去首尾静音 → 响度归一(-14 LUFS,峰 -3dB,玩具喇叭要足够响)→ 首尾 8ms 淡入淡出防咔哒
#       → 16 kHz 单声道 16bit 裸 PCM(little-endian,直入 flash)。
# 用法:./gen_sounds.sh
set -euo pipefail

HERE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
OUT="${HERE}/../../main/snd"
mkdir -p "${OUT}"

# 角色分派(按时长/响度/亮度人工挑选,依据见 assets/snd/README.md)
# 平行数组:macOS 自带 bash 3.2 无关联数组
NAMES=(meow_greet meow_happy meow_sweet meow_hungry meow_sleepy meow_squeak meow_trill)
FILES=("Cat Meows - Food Time 7.wav" "Cat Meows - Food Time 1.wav" "Cat Meows - Food Time 2.wav" \
       "Cat Meows - Food Time 4.wav" "Cat Meows - Food Time 11.wav" "Cat Meows - Tiny Tito.wav" \
       "Cat Meows - Trevor.wav")

for idx in "${!NAMES[@]}"; do
  name="${NAMES[$idx]}"
  src="${HERE}/src/${FILES[$idx]}"
  [[ -f "${src}" ]] || { echo "缺源文件: ${src}" >&2; exit 1; }

  # 第一步:裁静音 + 重采样到 16k 单声道(中间格式,便于量时长)
  tmp="$(mktemp -t kaka_snd).wav"
  ffmpeg -v error -y -i "${src}" \
    -af "silenceremove=start_periods=1:start_threshold=-40dB,areverse,silenceremove=start_periods=1:start_threshold=-40dB,areverse" \
    -ar 16000 -ac 1 -c:a pcm_s16le "${tmp}"

  dur="$(ffprobe -v quiet -show_entries format=duration -of csv=p=0 "${tmp}")"
  fade_st="$(python3 -c "print(max(0, float('$dur') - 0.008))")"

  # 第二步:响度归一 + 防咔哒淡入淡出 → 裸 PCM
  ffmpeg -v error -y -i "${tmp}" \
    -af "loudnorm=I=-14:TP=-3:LRA=7,afade=t=in:d=0.008,afade=t=out:st=${fade_st}:d=0.008" \
    -ar 16000 -ac 1 -c:a pcm_s16le -f s16le "${OUT}/${name}.pcm"
  rm -f "${tmp}"
  echo "${name}.pcm  <-  ${FILES[$idx]}  (${dur}s)"
done
echo "已生成 main/snd/*.pcm"
