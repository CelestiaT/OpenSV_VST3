# OPSV_VST3 —— OpenSV 歌唱合成引擎 × MuseScore 集成计划

> 上游谱系：本仓库 fork 自 [cubeww/OpenSV](https://github.com/cubeww/OpenSV)（经
> [niko-at-chalupa/OpenSV](https://github.com/niko-at-chalupa/OpenSV) 的 niko 分支线，
> 基线 commit `84ef139`），并携带针对官方声库的三处引擎修复（commit `2614296`，
> 与成果包 `patch/upstream-local.diff` +301/−26 逐字节一致）。
> 远端约定：`upstream` = cubeww/OpenSV，`niko` = niko 分支，`celestia-local` = 本地工作库。

---

## 1. 目标与非目标

### 目标
- 在 MuseScore Studio（本项目上级目录的 MScore fork，v5.0-dev）播放时，用 **OpenSV 声库**
  演唱乐谱中带歌词（音素）的音符，听到的是真实人声而非记谱示意音。
- **零修改 MuseScore 源码**：只用其现成的两条通道——
  VST3 音频插件宿主（`MUSE_MODULE_VST`）与 QML/JS 扩展插件系统（`MuseScore 3.0` API）。
- **OpenSV 插件端完全 headless**：不向宿主提供任何 GUI（其 JUCE 界面不符合我们的审美要求）。
  全部用户交互放在 MuseScore 原生 QML dialog 插件里，观感与 MuseScore 设置页一致。

### 非目标
- 不做实时逐块（block-by-block）流式合成——沿用 OpenSV 已有的"后台整曲/乐句渲染 +
  三重缓冲"模型（`PreviewEngine`/`ProjectRenderer`），播放端按宿主位置取样。
- 不分发声库、权重、加密容器（.dnni v3）；只用用户自备的明文 v1/v2 资源。不做任何 DRM 规避。
- 不改 mpe 事件管线传歌词（曾称 F1 方案）——工程桥旁路后该方案作废，仅作远期可选项。

---

## 2. 已核实的关键事实（源码证据）

| # | 事实 | 证据 |
|---|------|------|
| F-1 | MuseScore Linux 默认强制 xcb，原生 Wayland 需 `MU_QT_QPA_PLATFORM=wayland` | `MScore/src/app/main.cpp:105-110` |
| F-2 | VST3 宿主链完整：扫描 `.vst3`、加载、混音台挂载 | `muse/framework/vst/internal/vstpluginsscanner.cpp`、`src/playback/internal/knownaudiopluginsconfigurator.cpp` |
| F-3 | 宿主向插件送**采样精确**的 noteOn/noteOff（mpe → VST3 事件） | `muse/framework/vst/internal/synth/vstsynthesiser.cpp`、`vstsequencer.cpp` |
| F-4 | 宿主**不填** ppq/tempo，但**填** `projectTimeSamples`（工程绝对采样位置）| `muse/framework/vst/internal/vstaudioclient.cpp:268`；`vstsequencer.cpp:369 result.ppqPosition=0` |
| F-5 | mpe `NoteEvent` 无歌词字段，宿主不会把乐谱歌词发给插件 | `muse/framework/mpe/events.h`（grep 无 lyric） |
| F-6 | QML 插件可读歌词与音符：`Chord.lyrics` → `Lyrics.text` | `src/engraving/api/v1/elements.h:1629` |
| F-7 | QML 插件可起外部进程：`QProcess` 已注册（`MsProcess`） | `src/engraving/api/v1/qmlpluginapi.cpp:208` |
| F-8 | 仅当插件自带 editor 时宿主才嵌入其 GUI；无 editor 则不出现 | `muse/framework/vst/qml/Muse/Vst/VstEditorDialog.qml` |
| F-9 | OpenSV GUI 的实时播放 = 后台线程整工程渲染 + front/middle/back 三重缓冲 + phrase/voice/dict 三级缓存 | `OPSV_VST3/src/audio/PreviewEngine.h`、`ProjectRenderer.h` |

**推论（架构级决策）**：
- 歌词/音素不走 MIDI 管线，走**工程桥文件**（§5）；时间对齐以 `projectTimeSamples` 为基准，
  一切乐谱位置在导出侧换算成**绝对毫秒**，插件端不需要理解节拍网格。
- 插件 UI = VST3 参数（宿主自动渲染的通用控件）+ MuseScore dialog 插件（桥的管理者）。

---

## 3. 总体架构

```
┌───────────────────────────── MuseScore (不改源码) ─────────────────────────────┐
│                                                                                 │
│  乐谱 (Score/Chord/Note/Lyrics)                                                  │
│        ▲                                                                        │
│        │ MuseScore 3.0 API (QML/JS)                                             │
│  ┌─────┴──────────────┐          ┌───────────────────────────────┐              │
│  │ C4: QML 桥插件      │──写────▶│  桥文件 bridge.json (v1)       │              │
│  │ "OpenSV Bridge"     │◀─读─────│  状态文件 status.json          │              │
│  │ dialog, 原生主题     │ QProcess│  ~/.cache/opensv-bridge/<hash>/│              │
│  └────────────────────┘          └───────────────┬───────────────┘              │
│         混音台面板: 参数控件自动渲染▲                │ FileWatcher/Timer 轮询        │
└──────────────────────────────────┼────────────────┼──────────────────────────────┘
                                   │                ▼
                    ┌──────────────┴────────────────────────────┐
                    │ C2: OpenSV.vst3  (headless, 本仓库产物)     │
                    │  ├─ VST3 参数表 (声库/表情/调教)             │
                    │  ├─ BridgeLoader (JSON → sv::Project)      │
                    │  ├─ RenderWorker (复用 ProjectRenderer)     │
                    │  └─ PlaybackBuffer (三重缓冲, 按             │
                    │     processContext.projectTimeSamples 取样) │
                    └───────────────────────────────────────────┘
```

组件命名：
- **C1 `libopensv`**：把 `src/core` + `src/synthesis` + `src/audio/ProjectRenderer`
  重构成静态库，剥离 JUCE 设备管理依赖（CLI、GUI、插件三方共用，消除复制）。
- **C2 `OpenSVVst3`**：JUCE `juce_add_plugin` 产物，`hasEditor=false`。
- **C3 桥协议**：`bridge.json` / `status.json` 文件契约（§5）。
- **C4 `opensv-bridge.qml`**：MuseScore dialog 插件，负责导出、声库选择、状态展示。
- **C5 测试床**：headless 渲染一致性对比（与 `opensv-cli` 逐样本差）。

---

## 4. 里程碑与任务分解

### M0 仓库与开发闭环（0.5 天）✅ 本计划落地时已完成大半
- [x] clone cubeww/OpenSV 历史，建立 `upstream`/`niko` remote，分支 `feat/vst3`
- [x] 本地引擎修复固化为 commit `2614296`
- [x] `third_party/JUCE` submodule 就位（pin `be29c81492`，6.0.8+4638，JUCE 8 系）
- [ ] `CMakeLists.txt` 拆出 `libopensv` OBJECT/STATIC 目标（不含 `src/ui`、`src/app`）
- [ ] CI/本地脚本：`build.sh`（Ninja Release）+ `smoke.sh`（opensv-cli 渲染非静音阈值判定，
      直接移植成果包 `host/opensv-smoke.sh`）
- [ ] 在 GitHub 上对 cubeww/OpenSV 使用 Fork 按钮建个人仓库后：
      `git remote set-url origin <你的 fork>` → `git push -u origin feat/vst3 --tags`
      （只有 GitHub 的 Fork 动作才能显示官方 "forked from" 徽章；本地 remote 与
      README 说明是谱系的第二重表达）

### M1 VST3 空壳验证（1 天）——先证通路，再上真引擎
交付：`OpenSV.vst3`（正弦/静音 synth，3 个演示参数，无 editor）。
验收清单（全部在 MuseScore 里人工过一遍，任何一项不过须先解决再前进）：
- [ ] 把 `OpenSV.vst3` 放入 `~/.vst3`（或首选项添加扫描路径），设置 → 音频插件 扫描通过
- [ ] 混音台把某轨输出路由到该插件，播放时收到 noteOn/noteOff
- [ ] 插件 UI 入口不可见/不可点（F-8），宿主不崩溃
- [ ] 宿主显示参数列表（gender/breathiness/voice 索引），调动能触发
      `setParameter` 且插件日志可见
- [ ] **关键**：插件内记录 `processContext.projectTimeSamples`，
      与宿主播放位置一致；拖动播放头后位置立即正确（F-4 成立性实测）
- [ ] 重启 MuseScore 后参数/状态从 mscz 恢复（`getState/setState`）

### M2 libopensv 抽取 + 插件内核（2–3 天）
- [ ] `libopensv`：`Project`/`ProjectFile`（工程模型与 .svp 序列化）、
      `VoiceDatabase`/`PhonemeDictionary`、`ProjectRenderer`（三级缓存）全部进静态库；
      去除对 `AudioDeviceManager` 的依赖（它本来也只属于 `PreviewEngine`）
- [ ] 插件 `AudioProcessor`：
  - `processBlock`：从 front slot 按 `projectTimeSamples - trackStartSample` 取样输出；
    越界静音；句边界 5–10ms 等功率交叉淡化（`PreviewEngine::renderPlayback` 的移植）
  - 渲染线程：直接继承 `juce::Thread` 语义 —— 照搬 `PreviewEngine` 的
    `requestedRevision/completedRevision/middleSlot` 协议（F-9），不重新发明
  - `setNumChannels`/`prepareToPlay`：48k 固定内部采样率或跟随宿主 spec（首选跟随宿主，
    `ProjectRenderer::render(project, sampleRate, ...)` 已参数化）
- [ ] 音源数据源：M2 阶段先接受 **JUCE 插件内的 MIDI note 收集 + 桥 JSON** 双源，
      桥 JSON 为准，MIDI 仅用于"桥缺失时发元音 `a` 的兜底"与对齐校验
- 验收：C5 测试——同一工程分别经 `opensv-cli` 与插件内部渲染，输出 PCM 差 < 1e-6

### M3 桥协议与加载器（1–2 天）
- [ ] `BridgeLoader`：`std::atomic<uint64_t>` 文件指纹（mtime+size+内容 hash），
      `juce::Timer` 500ms 轮询（插件内不引 QFileSystemWatcher）；加载即 revision++
- [ ] 声库发现：扫描配置的声库根目录（`~/.local/share/opensv-voices` + 桥内显式路径），
      `.nofs` magic `0xF580` 校验（成果包 host/opensv_host.py 有现成判据），
      以 StringList 参数暴露；找不到声库 → status.json 记错，静音但不停摆
- [ ] 写 `status.json`（渲染中/进度/完成/错误），见 §5.3

### M4 QML 桥插件（2–3 天）
交付：`opensv-bridge.qml`（dialog 型，命名空间同 lilyrics 插件结构）。
- [ ] 数据导出：遍历活动谱表（用户选 part）的 Segment→Chord→Note + `chord.lyrics[]`，
      tick→ms 用 tempo map（`Score.tempoz`/timeSig 遍历，具体 API 名以
      `src/engraving/api/v1` 为准）；写 `bridge.json` 到
      `~/.cache/opensv-bridge/<mscz 路径 hash>/`
- [ ] 触发：对话框按钮"同步到 OpenSV"；后续增强：快捷键 + 变更指纹对比提示
- [ ] 声库/词典设置：目录选择用插件 `FileDialog`，路径写入 bridge.json（桥是唯一真源，
      插件 state 只存桥文件路径）
- [ ] 状态面板：`QProcess` 跑 `cat status.json`（F-7）→ 原生 ProgressBar/文案
      （渲染中 61s 级别的等待必须可见）
- [ ] 音素编辑（P2 增强）：直接读写 `chord.lyrics[n].text` 回写乐谱
- 验收：MuseScore 改谱 → 点同步 → 数秒后播放即人声；编辑响应只重渲受影响乐句
      （观察 statistics.reusedPhrases）

### M5 打磨与健壮性（2–3 天）
- [ ] 长工程首渲：分块渲染优先"播放头附近"乐句（lookahead 队列），其余按序
- [ ] 内存预算：phrase 缓存上限可配置（宿主进程内运行，尊重 `RenderStatistics`），
      全曲 buffer 251s×2ch×f32@48k ≈ 96MB，RAM 目标增量 < 1.2GB（对照实测引擎峰值 929MB）
- [ ] 时间签名/速度变化：由 tempo map 换算吸收（导出侧完成），插件零感知
- [ ] 播放头 seek、循环区间：projectTimeSamples 天然覆盖；验证 M1 清单可回放
- [ ] 声库路径跨机迁移：state 存相对路径 + 声库名，加载失败在 status 里点名提示
- [ ] 文档：`docs/BUILD.md`（Linux 构建、部署到 ~/.vst3、MuseScore 设置）
      `docs/BRIDGE-PROTOCOL.md`（§5 正式版）

### M6 远期可选
- 多轨/合唱（bridge 数组化，M5 后自然支持）
- 波形/频谱可视化（宿主 VST3 UI 扩展协议 `IImageRecovery` 或干脆画在 QML 面板里，
  用数据 sidecar——推荐后者，保持 headless 原则）
- 上游化讨论：若 engine 侧修复对 cubeww 有价值，发 PR；插件层留本仓库
- F1（宿主 mpe 传歌词）**仅当**想要"零手动同步"时重启评估——需要改 MScore，
  与"不改源码"目标冲突，默认搁置

---

## 5. 桥协议 v1（草案，M3 冻结）

### 5.1 路径约定
```
~/.cache/opensv-bridge/<scoreKey>/bridge.json    # 乐谱→SV 工程（唯一真源）
~/.cache/opensv-bridge/<scoreKey>/status.json    # 插件→QML 面板状态
scoreKey = sha256(abs(mscz 路径))[0:16]
```

### 5.2 bridge.json
```json
{
  "schema": 1,
  "generatedMs": 1728528000000,
  "score": { "name": "demo.mscz", "fingerprint": "…" },
  "voice": {
    "name": "teto",
    "nofsPath": "/home/u/.local/share/opensv-voices/teto.nofs",
    "dictDir":  "/home/u/.local/share/opensv-voices/clf-data",
    "language": "japanese"
  },
  "sampleRate": 48000,
  "phrases": [
    {
      "id": "p0",
      "startMs": 0.0, "endMs": 3.21,
      "notes": [
        { "startMs": 0.12, "endMs": 0.55, "midi": 62, "velocity": 0.8,
          "phonemes": ["t", "e", "t", "o"], "lyricText": "てと" }
      ]
    }
  ]
}
```
要点：绝对毫秒；声库语言决定 phoneset 前缀（`japanese-romaji-phones` 等，
见引擎修复 commit）；`phonemes` 为空则回落 `lyricText` 走词典 G2P；
phrase 划分规则 = 间隔 > 2 个八分音符的连唱段（与 GUI 现有行为对齐）。

### 5.3 status.json
```json
{ "state": "rendering|ready|error", "revision": 7, "progress": 0.42,
  "renderedPhrases": 12, "reusedPhrases": 9, "error": null }
```

---

## 6. VST3 参数表（初版）

| Param ID | 名称 | 类型 | 说明 |
|---|---|---|---|
| 1 | Voice | StringList | 扫描到的声库；桥的 `voice.name` 可覆盖 |
| 2 | Gender | float -0.5..0.5 | 引擎已有参数 |
| 3 | Breathiness | float 0..1 | 引擎已有参数 |
| 4 | Vibber | float 0..1 | 颤音深度 |
| 5 | Attenuation | float 0..1 | 音量（不做 -inf 曲线）|
| 6 | Sync Now | trigger | 请求重载 bridge.json（兜底按钮）|

全部参数变更 → 只标脏受影响 phrase 缓存键，不触发全曲重渲。

---

## 7. 测试矩阵

| 层 | 测试 | 方法 |
|---|---|---|
| 引擎 | bridge→Project 构建正确性 | 单测：golden bridge.json vs 期望 Project（含 phrase 划分）|
| 引擎 | 渲染一致性 | 与 opensv-cli 输出逐样本对比（1e-6）|
| 插件 | headless 冒烟 | M1 清单（宿主实测）|
| 插件 | projectTimeSamples 跟拍 | 录宿主播放位置流，与插件取样指针误差 < 1 block |
| 插件 | seek/循环/变速 | 手工矩阵 |
| 桥 | 节拍换算 | 混合 tempo/timeSig 乐段，导出 ms 与 MuseScore 播放位置对齐（允许 ±10ms）|
| 桥 | 歌词缺失/音素非法 | 回落与静音策略（发 `x`/跳过），不崩溃 |
| e2e | 官方流程 | 打开 demo.mscz→同步→播放→录制混音输出→非静音阈值+时长匹配 |

---

## 8. 风险登记

| 风险 | 等级 | 缓解 |
|---|---|---|
| 宿主 VST 通路对本 fork 构建默认开关未确认 | 中 | M0 末尾先构建一次，跑 M1；若 OFF，`-DMUSE_MODULE_VST=ON` |
| projectTimeSamples 语义（工程起点 vs 设备时钟）有出入 | 中 | M1 用正弦+日志先校准；轨道偏移可配 |
| 编辑→重渲风暴（连续改谱）| 中 | revision 节流（合并 300ms 内多次请求），phrase 缓存 |
| 长曲 RAM 压力 | 中 | buffer 预算 + phrase cache 上限 + 分块渲染（M5）|
| tempo map 换算 API 细节（`tempoz` 行为）| 低 | 用 Segment.ticks→ms 官方 API，golden 测试兜底 |
| JUCE 主版本跟随上游（6.0.8-4638 = JUCE 8）插件 API 变动 | 低 | submodule pin 住；升级单独 PR |
| 许可与分发边界 | 低 | 引擎代码 GPL 系（保留上游出处）；**不分发**声库/权重/加密容器 |

---

## 9. 目录规划（本仓库内新增）

```
OPSV_VST3/
├── src/{core,synthesis,audio,cli,app,ui}   # 上游 + 本地修复（尽量不动 src/ui）
├── plugin/                                  # C2: OpenSVVst3（新）
│   ├── CMakeLists.txt                       # juce_add_plugin(VST3)
│   ├── processor.{h,cpp}
│   ├── bridge/BridgeLoader.{h,cpp}
│   └── playback/StreamingBuffer.{h,cpp}     # 从 PreviewEngine 协议移植
├── libopensv/                               # C1: 共享静态库目标（cmake 层重构）
├── bridge/                                  # schema.json + 样例 golden 数据
├── qml/opensv-bridge.qml                    # C4（同一仓库管理，安装到 MuseScore 插件目录）
├── tests/{unit,e2e}/
├── docs/{PLAN.md,BUILD.md,BRIDGE-PROTOCOL.md}
└── third_party/JUCE                         # submodule pin
```

## 10. 参考坐标
- 上游引擎：`cubeww/OpenSV@84ef139`（本仓库 `niko` 线）
- 成果包（历史证据）：`../opensv-engine-port-2026-10-09.tar.xz`
  （smoke/性能数据/声库判据可复用）
- MuseScore 侧证据文件：§2 表中所有路径（`../MScore/` 下）
- 渲染性能基线：`T = 0.14s + 0.244·L`（≈4.1× 实时），全曲 251s → 61.3s，RSS 929MB

## 11. 总工作量估计
M1–M4 关键路径 ≈ **8–12 个有效工作日**；M5 收尾 +3–5 天。
每个里程碑末尾都有可运行的工件（空壳→可听→自动同步→原生面板），随时可停在任一节点
交付对应能力。
