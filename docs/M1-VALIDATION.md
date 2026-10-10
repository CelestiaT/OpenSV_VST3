# M1 验证清单 —— headless VST3 空壳在 MuseScore 中过通路

前置：本仓库构建产物 `build/plugin/OpenSVVst3_artefacts/Release/VST3/OpenSV.vst3`。

## 0. 构建前提（关键发现，docs/PLAN.md F-10）

MuseScore master（本 fork）中 **`MUSE_MODULE_VST` 默认为 OFF**
（`muse/framework/cmake/MuseDeclareOptions.cmake:128`），OFF 时 VST 宿主走
`muse/framework/stubs/vst` 存根 ⇒ 插件根本不会被扫描/加载。
配置 MuseScore 时必须显式打开：

```bash
cmake -S . -B build -G Ninja \
    -DMUSE_MODULE_VST=ON \
    -DCMAKE_BUILD_TYPE=Release
```

（`MUSE_MODULE_EXTENSIONS`、`MUSE_MODULE_AUDIOPLUGINS` 默认已为 ON，无需处理。）

## 1. 部署插件

```bash
mkdir -p ~/.vst3
cp -a build/plugin/OpenSVVst3_artefacts/Release/VST3/OpenSV.vst3 ~/.vst3/
```

## 2. MuseScore 内检查项

1. 编辑 →  preferences → 插件：确认 `~/.vst3` 在扫描路径中；
2. 重新扫描插件 → 列表出现 **OpenSV**（无报错即 F-2 通路成立）；
3. 新建谱 → 某轨音色面板把输出切到 VST → 选 OpenSV；
4. **无 GUI**：不应弹出/可点开任何插件窗口（F-8，`hasEditor()==false` 生效）；
5. 混音台/面板出现参数列表：`Voice / Gender / Breathiness / Vibber /
   Attenuation / Test Tone (M1) / Sync Now`（宿主自动渲染 ⇒ M4 面板可行性）；
6. 播放任意带音符的谱：应听到 **正弦试听音**（音高跟随音符），
   证明 mpe→VST3 noteOn/noteOff 采样级通路（F-3）；
7. **播放头对时探针**：从终端启动 MuseScore，观察日志（见 §3），
   核对 `samplePos` 与谱面播放位置（风险 R2 的判定数据）；
   - 若 `samplePos=-1` 或恒定 0：宿主位置不可用 ⇒ 记录，M2 改用
     累计块数 + noteOn 事件位置兜底；
   - 若数值 = 工程绝对位置（seek 后立刻跳变正确）⇒ 架构成立，最理想；
8. 关闭 → 保存工程 → 重开：参数值从 mscz 恢复（state 往返）；
9. 拖动任一参数 → 日志中确认插件收到（M4 的"零 GUI 控制面"成立）。

## 3. 探针日志位置

```
~/.local/share/OpenSV/logs/vst3.log
```

每秒一行（仅当有音频活动）：
`transport: samplePos=… ppq=… bpm=… playing=… activeNotes=… blocks=… syncRequests=…`

## 4. 判定与回写

- 全过 → PLAN.md 勾选 M1，进入 M2（ProjectRenderer 接入取样路径）；
- 步骤 2/3 失败 → 优先排查 `MUSE_MODULE_VST=ON` 是否生效、扫描路径；
- 步骤 7 异常不阻塞 M1 判定（有兜底方案），但必须记录数值特征。

## 5. 已知临时性内容（M2/M3 会重构，勿视为缺陷）

- Sync Now 用布尔轮询近似 trigger；
- `Voice` 参数只有占位项（M3 扫描声库后接管）；
- 正弦试听由 `Test Tone (M1)` 开关控制，M2 接入引擎后默认关闭。
