**简体中文** · [English](README.md)

# 卡卡 Kaka —— AI Passport 电子宠物猫

给 5 岁孩子的离线电子宠物游戏:孩子用 上 / 下 / 确认 三键照顾一只橘猫
"卡卡"。明快像素风,全部美术由脚本确定性生成,全部声音由代码合成,
零版权素材、零联网。

分支:`feature/kaka-pet`(基于上游 `FoloToy/ai-passport` main)。

## 玩法

| 按键 | 语境 | 行为 |
| --- | --- | --- |
| 上 / 下 短按 | 主界面 | 切换活动:喂食 / 刷毛 / 晒太阳 / 散步 |
| 确认 短按 | 主界面 | 开始选中的活动 |
| 确认 短按 | 活动中 | 场景内互动:再吃一条鱼 / 再刷一下 / 提前唤醒 / 跳跃扑蝶 |
| 确认 长按 | 任意 | 睡前仪式:夜晚屏 + 摇篮曲,任意键唤醒 |
| 上 长按 | 任意 | 家长控制:静音开 / 关(持久化) |

- **喂食**:三条鱼一组;吃饱后卡卡会礼貌拒喂(闻一闻、推碗、害羞爱心)。
- **刷毛**:每按一下刷一下,连刷五下毛发发亮收尾。
- **晒太阳**:8 秒暖阳小憩(蝴蝶绕飞、Zzz),可按确认提前唤醒。
- **散步**:12 秒横向卷轴街道(视差滚动),按确认扑跳。
- **轻养成**:饱食 / 干净 / 心情三项,缓慢下降且**下限 15**——卡卡永远
  不会生病,最低只是想你;头顶气泡提示想要什么。重新上电统一按 6 小时
  等效衰减补进度(设备无 RTC 电池,不假装知道关了多久)。
- **声音**:喵声"配音"(问候 / 开心 / 撒娇 / 饿了 / 困倦 / 颤音…) +
  全场景音效(咀嚼、呼噜、铃铛、闪光、鸟鸣、风、摇篮曲…),全部由
  配方合成器生成;空闲时卡卡会随机喵。
- **功耗**:60 秒无操作背光降到 30%,5 分钟进夜晚屏,任意键恢复。

## 结构

```
main/pet_model.c    纯逻辑:状态/衰减/活动 FSM(host 可测)
main/pet_store.c    存档序列化(host 可测);main/pet_nvs.c 为 NVS 胶水
main/pet_ui.c       主界面/夜晚屏/粒子/庆祝;main/pet_scenes.c 四个活动场景
main/pet_audio.c    两层声音系统(喵声配音 + 音效,过程式合成)
main/pet_art.c      像素资产(LVGL I4,由 assets/cat/gen_assets.py 生成)
assets/fonts/       中文字体子集(Noto Sans SC OFL 1.1,gen_app_fonts.sh)
tests/test_pet_*.c  host 逻辑测试(挂进 tools/validate.sh --static)
```

## 构建与验证

沿用仓库统一门禁(需 ESP-IDF 5.5.3):

```bash
./tools/validate.sh --static    # 仓库检查 + host 测试
./tools/validate.sh --firmware  # 固件构建 + 合并镜像校验
```

改像素形象:`python3 assets/cat/gen_assets.py`(先 `--png-only` 调预览);
改中文文案:同步更新 `assets/fonts/gen_app_fonts.sh` 的 SYMS 与
`main/pet_font.h` 的 `PET_GLYPH_SET` 后重新生成。
