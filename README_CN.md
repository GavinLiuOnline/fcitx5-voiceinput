# fcitx5-voiceinput

[English](README.md)

基于 [fcitx5](https://github.com/fcitx/fcitx5) 的语音输入法插件：按下快捷键说话，
识别出的文字直接上屏到当前应用。设计参考 `@deepseek-ai/dsh-experimental-voice-input-bundle`。

所有识别**完全本地运行**：[SenseVoiceSmall (INT8)](https://github.com/k2-fsa/sherpa-onnx)
负责语音识别，Silero VAD 负责自动断句。不依赖任何云服务，音频不出本机。

## 功能特性

- **热键触发** — 默认 `Ctrl+Alt+V`，可在 `fcitx5-configtool` 中自定义
- **边说边出字** — 说话过程中流式模型实时显示草稿（预编辑），每停顿一句
  立即由 SenseVoice 精修并上屏，录音不中断；再次按快捷键手动结束。
  分段上屏时会去掉模型自动附加的句末标点，不会每段强制加句号
- **多语言** — 自动识别 / 中 / 英 / 日 / 韩 / 粤语（SenseVoice）
- **焦点感知** — 录音时切换到其他窗口或输入框会自动停止录音；
  在同一输入框内移动光标不会中断
- **智能数字转换（ITN）** — “三点五” → “3.5”
- **状态反馈** — 输入面板实时显示录音/识别状态，输入法工具栏出现麦克风按钮
- **准备步骤自检** — `voice_backend.py doctor` 依次完成：检查本地资源 /
  准备识别与检测模型 / 准备流式预览模型 / 校验模型文件 / 加载模型

## 架构

```
┌─────────────────────────── fcitx5 ───────────────────────────┐
│  voiceinput 插件 (C++)                                        │
│  • PreInputMethod 事件钩子监听触发热键                          │
│  • 启动 Python 后端，通过管道读取结果                            │
│  • 通过 commitString 把文字提交到焦点应用                       │
└───────────────┬───────────────────────────────────────────────┘
                │ stdout: PARTIAL 草稿→预编辑 / COMMIT 文本→上屏
                │         ERROR\n<原因> / DONE
┌───────────────▼───────────────────────────────────────────────┐
│  voice_backend.py (Python)                                    │
│  • 录音 16kHz 单声道（arecord / pw-record / sounddevice）       │
│  • Silero VAD 自动断句（sherpa-onnx）                          │
│  • SenseVoiceSmall INT8 离线识别（sherpa-onnx）                │
└───────────────────────────────────────────────────────────────┘
```

## 环境要求

- Linux + fcitx5 >= 5.0（在 Ubuntu 22.04 / fcitx5 5.0.14 上开发测试）
- 构建依赖：`libfcitx5core-dev libfcitx5config-dev libfcitx5utils-dev`、cmake、g++
- Python 3.8+，及 `sherpa-onnx`、`numpy`
- 麦克风

## 安装

### 从安装包安装

从 [Releases](../../releases) 页面下载 `fcitx5-voiceinput_<版本>_amd64.deb`
或 snap 包（推送 `v*` 标签后 CI 会自动构建两者并发布）：

```bash
# deb（Ubuntu 22.04+）
sudo apt install ./fcitx5-voiceinput_1.0.0_amd64.deb
pip3 install --user sherpa-onnx    # ASR 引擎（apt 不打包，需手动装一次）

# snap（classic 安装器 snap）
sudo snap install fcitx5-voiceinput --classic
fcitx5-voiceinput.install-addon    # 把插件复制进系统 fcitx5 目录
```

### 从源码安装

```bash
git clone <本仓库>
cd fcitx5-voiceinput
./scripts/install.sh
```

脚本会自动安装构建依赖、编译并安装插件、安装中文翻译，然后运行 `doctor`
下载模型（约 290 MB：SenseVoice INT8 + Silero VAD + 流式预览模型）。

<details>
<summary>手动安装</summary>

```bash
pip3 install --user sherpa-onnx numpy
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
sudo cmake --install build       # 插件 → /usr/lib/x86_64-linux-gnu/fcitx5/
python3 backend/voice_backend.py doctor
fcitx5 -r                        # 重启 fcitx5
```
</details>

## 使用方法

1. 焦点放在任意输入框，按 **`Ctrl+Alt+V`**（默认），输入面板显示 🎤 录音中…
2. 连续说话即可——说话时草稿实时显示在光标处（预编辑），每停顿一句立即
   精修上屏，录音继续不断开
3. 再次按快捷键结束，结尾未停顿的内容会自动补上屏

配置入口：**fcitx5-configtool → 附加组件 → 语音输入**

| 选项 | 默认值 | 说明 |
| --- | --- | --- |
| 触发快捷键 | `Ctrl+Alt+V` | 开始/结束一次语音输入 |
| 识别语言 | 自动识别 | auto / zh / en / ja / ko / yue |
| 识别模型 | SenseVoice Small | 见下方"识别模型" |
| 空闲自动停止（秒，无新文字时计时） | 6 | 连续该秒数无新文字自动结束录音；每出现新文字（草稿或上屏）重新计时 |
| 录音中的按键 | — | `Esc` 结束；`Enter` 换行；`Backspace` 逐字删除；可直接键入英文、数字、标点符号 |
| 断句停顿（毫秒，停顿后草稿上屏） | 800 | 说话停顿超过该时长，当前草稿立即转为正式文本 |
| 智能数字转换 | 开 | 口述数字转为阿拉伯数字 |
| 实时预编辑 | 开 | 说话时流式显示草稿（中/英） |
| Python / 后端脚本 / 模型目录 | — | 高级路径配置 |

### 识别模型

在 fcitx5-configtool 的下拉框中选择，每个选项实时标注本地缓存状态
（已缓存 / 未缓存）。**切换到未缓存的模型会立即在后台下载**，第一次录音
无需等待；若下载完成前就开始说话，录音会先缓冲、模型现场下载，不丢字。
下载在**独立的后台进程**中进行，结束录音、切换焦点都不会中断下载。
下载进度以**桌面通知**显示（每 20% 原位刷新，完成/失败各有提示），
录音中触发的下载同时会在输入面板状态行显示等待提示：

| 模型 | 语言 | 体积 | 说明 |
| --- | --- | --- | --- |
| SenseVoice Small（默认） | 中/英/日/韩/粤 | 约 230 MB | 综合最佳，支持 ITN |
| Paraformer 中文 | 中 | 约 230 MB | 阿里达摩院，中文快速 |
| Whisper Base | 多语言 | 约 145 MB | OpenAI Whisper，CPU 上较慢 |
| FireRed ASR Large | 中/英 | 约 1.2 GB | 小红书，高精度大模型 |

也可命令行预下载：`python3 backend/voice_backend.py download --model paraformer-zh`

## 发布版本

推送 `v*` 标签即触发 release 工作流：自动构建 deb（CPack）与 snap
（snapcraft）并附到 GitHub Release：

```bash
git tag v1.0.0
git push origin v1.0.0
```

## 命令行调试

```bash
# 完整自检 + 模型下载
python3 backend/voice_backend.py doctor

# 用 16kHz 单声道 wav 文件代替麦克风测试识别
python3 backend/voice_backend.py session --file test.wav --itn

# GitHub 下载慢？设置代理前缀
export VOICEINPUT_MIRROR=https://ghfast.top
```

## 常见问题

- **安装后附加组件里没有语音输入** — 重启 fcitx5（`fcitx5 -r`），
  用 `fcitx5-diagnose` 检查；组件名显示为“语音输入 / Voice Input”。
- **录音失败/没有声音** — 确认安装了 `alsa-utils` 或 `pipewire-utils`，
  且麦克风是默认输入源（可用 `pavucontrol` 调整）。
- **模型下载失败** — 设置 `VOICEINPUT_MIRROR`（GitHub 代理），或手动从
  [HuggingFace](https://huggingface.co/csukuangfj/sherpa-onnx-sense-voice-zh-en-ja-ko-yue-2024-07-17)
  下载 `model.int8.onnx`、`tokens.txt`，连同 `silero_vad.onnx` 放到
  `~/.local/share/fcitx5-voiceinput/models/`。

## 许可证

MIT。模型版权归各自项目所有
（[SenseVoice](https://github.com/FunAudioLLM/SenseVoice)、
[Silero VAD](https://github.com/snakers4/silero-vad)），
通过 [sherpa-onnx](https://github.com/k2-fsa/sherpa-onnx) 分发。
