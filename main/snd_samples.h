// main/snd_samples.h —— 真实猫叫采样(CC0)的嵌入符号声明。
//
// pcm 文件由 assets/snd/gen_sounds.sh 生成到 main/snd/,经 CMake
// EMBED_FILES 嵌进 flash .rodata;符号名规则:路径与点线替换为下划线。
// 数据为 16kHz 单声道 16bit 小端裸 PCM,用 ld_i16() 逐字节读,
// 不假设对齐(嵌入符号无对齐保证)。
#pragma once

#include <stdint.h>

extern const uint8_t _binary_meow_greet_pcm_start[];
extern const uint8_t _binary_meow_greet_pcm_end[];
extern const uint8_t _binary_meow_happy_pcm_start[];
extern const uint8_t _binary_meow_happy_pcm_end[];
extern const uint8_t _binary_meow_sweet_pcm_start[];
extern const uint8_t _binary_meow_sweet_pcm_end[];
extern const uint8_t _binary_meow_hungry_pcm_start[];
extern const uint8_t _binary_meow_hungry_pcm_end[];
extern const uint8_t _binary_meow_sleepy_pcm_start[];
extern const uint8_t _binary_meow_sleepy_pcm_end[];
extern const uint8_t _binary_meow_squeak_pcm_start[];
extern const uint8_t _binary_meow_squeak_pcm_end[];
extern const uint8_t _binary_meow_trill_pcm_start[];
extern const uint8_t _binary_meow_trill_pcm_end[];
