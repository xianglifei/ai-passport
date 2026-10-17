# 声音采样来源与许可证

**简体中文** · [English](README.md)

## 来源

喵声层采样来自 GitHub 仓库 [lavenderdotpet/CC0-Public-Domain-Sounds](https://github.com/lavenderdotpet/CC0-Public-Domain-Sounds)
的 "Micro Pack - Cat Meows"(作者 Benjamin Burnes / ben_burnes)。

## 许可证

整包为 **CC0(公有领域贡献)**,可自由使用、修改、再分发,无需授权。
作者鼓励(但不要求)署名 —— 特此致谢。

## 角色分派

按"时长 / 响度(RMS) / 亮度(过零率)"三指标从 14 条候选中挑 7 条:

| 固件声音 | 采样文件 | 挑选依据 |
| --- | --- | --- |
| SND_MEOW_GREET 问候 | Cat Meows - Food Time 7.wav  | 0.90s,中等响度,清晰友好 |
| SND_MEOW_HAPPY 开心 | Cat Meows - Food Time 1.wav  | 1.67s 最长最亮,连珠炮式絮叨 |
| SND_MEOW_SWEET 撒娇 | Cat Meows - Food Time 2.wav  | 最轻(-37dB),软 |
| SND_MEOW_HUNGRY 饿了 | Cat Meows - Food Time 4.wav | 最响(-27dB),急切 |
| SND_MEOW_SLEEPY 困倦 | Cat Meows - Food Time 11.wav | 最短最沉(亮度最低) |
| SND_SQUEAK 抗议短叫 | Cat Meows - Tiny Tito.wav   | 小奶猫,音色最细 |
| SND_TRILL 颤音 | Cat Meows - Trevor.wav          | 明亮短促 |

打哈欠(SND_YAWN)与全部效果层(咀嚼/呼噜/铃铛/…)仍为代码合成,
采样与效果听感互补。

## 再生成

`src/` 留存原始 WAV;改配方或换素材后跑:

```bash
./gen_sounds.sh   # 裁静音 → 响度归一(-18 LUFS) → 16k 单声道 PCM → main/snd/
```

需要 ffmpeg。新文件要同步改 `gen_sounds.sh` 的分派表、`main/CMakeLists.txt`
的 EMBED_FILES 与 `main/snd_samples.h` 的符号声明。
