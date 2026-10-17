# Sound sample sources and licensing

**English** · [简体中文](README.zh_CN.md)

## Source

The meow voice samples come from the "Micro Pack - Cat Meows" in the GitHub
repository [lavenderdotpet/CC0-Public-Domain-Sounds](https://github.com/lavenderdotpet/CC0-Public-Domain-Sounds)
(by Benjamin Burnes / ben_burnes).

## License

The whole pack is dedicated to the **public domain (CC0)**: free to use,
modify and redistribute, no permission needed. Attribution is encouraged
but not required — credit given here gladly.

## Role assignment

Seven clips were picked from 14 candidates by duration / loudness (RMS) /
brightness (zero-crossing rate):

| Firmware sound | Sample file | Why |
| --- | --- | --- |
| SND_MEOW_GREET | Cat Meows - Food Time 7.wav  | 0.90 s, medium loudness, clear and friendly |
| SND_MEOW_HAPPY | Cat Meows - Food Time 1.wav  | 1.67 s, longest and brightest — happy chatter |
| SND_MEOW_SWEET | Cat Meows - Food Time 2.wav  | softest (-37 dB) |
| SND_MEOW_HUNGRY | Cat Meows - Food Time 4.wav | loudest (-27 dB), insistent |
| SND_MEOW_SLEEPY | Cat Meows - Food Time 11.wav | shortest and darkest |
| SND_SQUEAK | Cat Meows - Tiny Tito.wav   | tiny kitten voice |
| SND_TRILL | Cat Meows - Trevor.wav          | bright and short |

The yawn (SND_YAWN) and the whole effect layer (munch, purr, bell, …)
remain procedural — samples and effects complement each other.

## Regenerating

Original WAVs are kept in `src/`. After changing the recipe or the source
material, run:

```bash
./gen_sounds.sh   # trim silence → normalize (-18 LUFS) → 16k mono PCM → main/snd/
```

Requires ffmpeg. New files must be synced across `gen_sounds.sh`'s
assignment table, the `EMBED_FILES` list in `main/CMakeLists.txt`, and the
symbol declarations in `main/snd_samples.h`.
