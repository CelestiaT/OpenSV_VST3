# M2 使用指南 —— OpenSV VST3 在 MuseScore 里出声

## 前提
- 宿主：官方 Nightly AppImage（4.7，VST3 可用；Surge XT 能弹编辑器 ⇒ X11-embed 路径畅通）。
- 插件已部署：`~/.vst3/OpenSV.vst3`（指向
  `OPSV_VST3/build/plugin/OpenSVVst3_artefacts/Release/VST3/OpenSV.vst3` 的软链，
  重新构建后无需重新链接）。

## 编辑器面板（A 方案，2026-10-10 起）
插件现在提供自绘的小型原生面板（**不是** OpenSV 那套 standalone GUI）：
- Voice bank 下拉：自动扫描当前声库所在目录 + `scanDirs` 里的目录（`*.nofs`）；
  `+ folder` 按钮增加扫描目录并持久化；
- Dictionary `[...]` 选 clf-data 目录、Language、Syllable（MIDI 采集模式每音符唱什么）；
- Gender / Breathiness / Vibber / Attenuation 滑杆绑定 VST3 参数（宿主自动化可见）；
- Commit & Render / Reload config 按钮 + 渲染状态行（notes/RENDERING/audio/错误）；
- 可调整大小（440×520 起）。面板的每次修改立即写回 config.json（绝对路径规范化）。

MuseScore 4.7 对无编辑器插件也会弹空壳对话框（其 QML bug：`VstEditorDialog.qml:
floating` 属性缺失 WARN，无害），有了真编辑器后该窗口即为可用面板。

## 声库配置（面板代写，也可手编；1 秒轮询热加载）
`~/.config/opensv-vst3/config.json`
（JUCE 9 在 Linux 将 `userApplicationDataDirectory` 映射为 `$XDG_CONFIG_HOME`，
实测 `XDG_CONFIG_HOME` 环境变量在 JUCE 内不稳定，测试请用 `OPENSV_CONFIG=/abs/path`
覆盖整个 config 文件路径）：
```json
{
  "voice": "/home/casuki/Projects/MuseScore/voices/AVER_v200.nofs",
  "dict":  "/home/casuki/Projects/SVFlatEditor/SV Flat/Synthesizer V Studio Basic Origin/clf-data",
  "language": "japanese",
  "lyric": "la",
  "scanDirs": [ "/home/casuki/Projects/MuseScore/voices" ]
}
```
- `voice`：`.nofs` 文件（标准声库 = `.svpk` 解包：`unzip -j xxx.svpk voice.nofs`）；
  换 `KASANE_TETO_AI_104.nofs` 即切 Teto。相对路径会被 setConfig 强制转为绝对。
- `language`：`japanese` / `english` / `mandarin` / `cantonese` / `spanish`。

## 在 MuseScore 里的操作流程（M2 语义）
1. 轨道输出切到 VST3 → **OpenSV**；
2. 确保谱上有音符（M2 忽略谱面歌词，唱 config.lyric 音节）；
3. **播放一遍**——这一遍是"采集"（听到正弦占位音）；
4. **停止播放**（或面板 Commit & Render / 推 Sync Now 参数）触发后台渲染，
   渲染时长 ≈ 曲长/4，状态行显示 RENDERING，日志每秒更新；
5. **再播放一遍**——真实人声，按 `projectTimeSamples` 与播放头绝对对齐，可 seek/暂停。

一旦引擎出声，正弦占位自动停止（Test Tone 参数只在无引擎音频时生效）。

## 验证/调试
- 无 GUI 冒烟（CI 可用，含 config 持久化 round-trip）：
  ```bash
  ninja -C build OpenSVEngineSmoke
  OPENSV_CONFIG=/tmp/cfg.json \
    ./build/OpenSVEngineSmoke_artefacts/Release/opensv-engine-smoke \
    voices/AVER_v200.nofs "<clf-data>" /tmp/smoke.wav
  ```
- 插件运行日志：`~/.config/OpenSV/logs/vst3.log`
  （已改线程安全异步落盘：音频线程行不再丢失；每行带时间戳）。

## 已知边界（M3/M4）
- 音符靠"播放一遍"采集 → QML 桥直接导出音符+歌词+精确 ms；
- 歌词固定音节 → 桥导出后每音符带 lyric/phoneme；
- 整曲重渲（ProjectRenderer 自带 phrase 缓存，声库/词典不变时只重推理受影响乐句）；
- Flat 声库（.sfpk）解包路径未接入。
