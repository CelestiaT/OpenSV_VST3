# M2 使用指南 —— OpenSV VST3 在 MuseScore 里出声

## 前提
- 宿主：官方 Nightly AppImage（4.7，VST3 可用）或自编译 5.0-dev（`-DMUSE_MODULE_VST=ON`）。
- 插件已部署：`~/.vst3/OpenSV.vst3`（当前是指向
  `OPSV_VST3/build/plugin/OpenSVVst3_artefacts/Release/VST3/OpenSV.vst3` 的软链，
  重新构建后无需重新链接）。

## 声库配置（唯一入口，改完自动生效，1 秒内被插件轮询到）
`~/.local/share/opensv-vst3/config.json`：
```json
{
  "voice": "/home/casuki/Projects/MuseScore/voices/AVER_v200.nofs",
  "dict":  "/home/casuki/Projects/SVFlatEditor/SV Flat/Synthesizer V Studio Basic Origin/clf-data",
  "language": "japanese",
  "lyric": "la"
}
```
- `voice`：`.nofs` 文件（标准声库 = `.svpk` 解包：`unzip -j xxx.svpk voice.nofs`）；
  换成 `KASANE_TETO_AI_104.nofs` 即切 Teto。
- `dict`：`clf-data` 目录（G2P 词典，SV Basic 自带）。
- `lyric`：MIDI 收集模式下每个音符唱的音节（M3 桥接入后由乐谱歌词取代）。
- `language`：`japanese` / `english` / `mandarin` / `cantonese` / `spanish`…（对应
  词典与 phoneset 前缀）。

## 在 MuseScore 里的操作流程（M2 语义）
1. 混音台/轨道面板把某轨输出切到 VST3 → **OpenSV**；
2. 确保谱上有音符、轨道有歌词与否均可（M2 忽略歌词，用 config.lyric）；
3. **播放一遍**该段——这一遍是"采集"（听到的是正弦占位或静音）；
4. **停止播放**触发后台渲染（日志 `committed N notes -> render requested`；
   渲染时长 ≈ 曲长/4，全曲 251s 约 60s，日志每秒更新 `RENDERING`）；
5. **再播放一遍**——听到真实人声，位置与宿主播放头绝对对齐（可 seek/暂停，
   因为输出是按 `projectTimeSamples` 从渲染结果里取样）。
6. 播放中途想立即渲染已采集的音符：把参数 **Sync Now** 推一下（等价于停止播放）。

## 验证/调试
- 无 GUI 冒烟（CI 可用）：
  ```bash
  ninja -C build OpenSVEngineSmoke
  ./build/OpenSVEngineSmoke_artefacts/Release/opensv-engine-smoke \
      ../voices/AVER_v200.nofs "<clf-data 路径>" /tmp/smoke.wav
  ```
- 插件运行日志：`~/.local/share/OpenSV/logs/vst3.log`
  （transport 行附带 `engine: audio|RENDERING|idle song=Xs ERR=…`）。
- 安全归一化：引擎原始峰值若 >0.95 会整曲缩到 ≈-1 dBFS（AI 声库全曲峰值实测可达 >1，
  CLI 直接截断写盘是隐患）。

## 已知边界（M3/M4 会解决）
- 音符只能靠"播放一遍"采集 → QML 桥插件将直接导出音符+歌词+精确 ms；
- 歌词固定音节 → 桥导出后每音符带 `lyrics/phonemes`；
- 整曲重渲（phrase 缓存已启用在 ProjectRenderer 内，声库/词典不变时只重推理
  受影响乐句，重渲速度远低于整曲）。
