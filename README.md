# fcitx5-voiceinput

[中文说明](README_CN.md)

A [fcitx5](https://github.com/fcitx/fcitx5) addon that adds voice input to
your desktop: press a hotkey, speak, and the recognized text is committed
into the focused application — inspired by
`@deepseek-ai/dsh-experimental-voice-input-bundle`.

Everything runs **locally**: [SenseVoiceSmall (INT8)](https://github.com/k2-fsa/sherpa-onnx)
for speech recognition and Silero VAD for endpoint detection. No cloud
service, no audio ever leaves your machine.

## Features

- **Hotkey triggered** — default `Ctrl+Alt+V`, fully rebindable via
  `fcitx5-configtool`
- **Live dictation** — a streaming model shows a draft of your speech as
  preedit text while you talk; every phrase (Silero VAD) is then finalized
  with SenseVoice and committed at the pause. Press the hotkey again to
  stop manually. Model-appended sentence-final punctuation is stripped
  when a segment commits, so no full stop is forced after every pause
- **Multilingual** — auto / zh / en / ja / ko / yue (SenseVoice)
- **Focus-aware** — switching to another window or input field while
  dictating stops the recording automatically; moving the cursor inside
  the same field never interrupts it
- **Inverse text normalization** — spoken numbers become digits ("三点五" → "3.5")
- **Status feedback** — recording/recognizing states shown in the fcitx5
  input panel; a microphone button appears in the input method toolbar
- **Preparation doctor** — `voice_backend.py doctor` checks resources,
  downloads and verifies models (SenseVoice, Silero VAD and a streaming
  zipformer used for the live draft), and test-loads SenseVoice

## Architecture

```
┌─────────────────────────── fcitx5 ───────────────────────────┐
│  voiceinput addon (C++)                                       │
│  • trigger hotkey (PreInputMethod watcher)                    │
│  • spawns backend, reads result over a pipe                   │
│  • commits text into the focused app (commitString)           │
└───────────────┬───────────────────────────────────────────────┘
                │ stdout: PARTIAL draft → preedit / COMMIT text → committed
                │         ERROR\n<msg> / DONE
┌───────────────▼───────────────────────────────────────────────┐
│  voice_backend.py (Python)                                    │
│  • records 16 kHz mono (arecord / pw-record / sounddevice)    │
│  • Silero VAD endpoint detection (sherpa-onnx)                │
│  • SenseVoiceSmall INT8 offline ASR (sherpa-onnx)             │
└───────────────────────────────────────────────────────────────┘
```

## Requirements

- Linux with fcitx5 >= 5.0 (tested on Ubuntu 22.04 / fcitx5 5.0.14)
- `libfcitx5core-dev libfcitx5config-dev libfcitx5utils-dev`, cmake, g++
- Python 3.8+ with `sherpa-onnx` and `numpy`
- A microphone

## Install

### From packages

Grab `fcitx5-voiceinput_<version>_amd64.deb` or the snap from the
[Releases](../../releases) page (CI builds both for every `v*` tag).

```bash
# deb (Ubuntu 22.04+)
sudo apt install ./fcitx5-voiceinput_1.0.0_amd64.deb
pip3 install --user sherpa-onnx    # ASR engine (not packaged by apt)

# snap (classic installer snap)
sudo snap install fcitx5-voiceinput --classic
fcitx5-voiceinput.install-addon    # copies the addon into system fcitx5
```

### From source

```bash
git clone <this repo>
cd fcitx5-voiceinput
./scripts/install.sh
```

The script installs build dependencies, builds the addon, installs it
system-wide, installs the zh_CN translation, then runs `doctor` to download
the models (~290 MB: SenseVoice INT8 + Silero VAD + streaming draft model).

<details>
<summary>Manual install</summary>

```bash
pip3 install --user sherpa-onnx numpy
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
sudo cmake --install build       # addon → /usr/lib/x86_64-linux-gnu/fcitx5/
python3 backend/voice_backend.py doctor
fcitx5 -r                        # restart fcitx5
```
</details>

## Usage

1. Focus any text field and press **`Ctrl+Alt+V`** (default). The input panel
   shows 🎤 Recording…
2. Speak continuously — a live draft appears at the cursor (preedit) while
   you talk; every time you pause briefly, the phrase is finalized and
   committed, then recording continues.
3. Press the hotkey again (or release Space in hold-space mode) to stop; the
   trailing words are flushed first. There is no recording time limit — the
   session lasts exactly as long as you keep it open.

Options are under **fcitx5-configtool → Addons → Voice Input**:

| Option | Default | Description |
| --- | --- | --- |
| Trigger Mode | Hold space | `Hotkey toggle`: press the hotkey to start/stop. `Hold space`: hold **Space** for the configured duration to start, release to stop |
| Trigger Key | `Ctrl+Alt+V` | Hotkey to start/stop a session (hotkey mode; also works as a fallback in hold-space mode) |
| Hold Space Milliseconds to Start | 1000 | Hold-space mode: milliseconds to hold Space before recording starts (500-10000); a shorter press types a normal space |
| Recognition Language | Auto | auto / zh / en / ja / ko / yue |
| Recognition Model | SenseVoice Small | See "Recognition models" below |
| Keys while recording | — | `Esc` finish; `Enter` insert a newline; `BackSpace` delete the last committed character; letters/digits/punctuation are typed straight into the text field |
| Sentence Pause (ms; draft commits on pause) | 800 | Pause length that turns the live draft into committed text |
| Inverse Text Normalization | On | Convert spoken numbers to digits |
| Live Preedit | On | Streaming draft shown while speaking (zh/en) |
| Python / Backend / Model Directory | — | Advanced paths |

### Recognition models

Pick one in the fcitx5-configtool combo box — every entry is labeled with
its live cache status ("cached" / "not cached"). Switching to a model that
is not cached **starts a background download immediately**, so the first
dictation with it doesn't wait. If you start talking before the download
finishes, mic audio is buffered meanwhile — nothing is lost. Downloads run
in a **detached background process**: ending a dictation (Esc, hotkey or
focus change) never aborts them. Progress shows as **desktop
notifications** (updated in place every 20%, plus a final success/failure
notice); a download triggered mid-dictation also shows a waiting note on
the input-panel status line:

| Model | Languages | Size | Notes |
| --- | --- | --- | --- |
| SenseVoice Small (default) | zh/en/ja/ko/yue | ~230 MB | Best all-round, ITN support |
| Paraformer zh | zh | ~230 MB | Alibaba, fast Chinese ASR |
| Whisper Base | multilingual | ~145 MB | OpenAI Whisper, slower on CPU |
| FireRed ASR Large | zh/en | ~1.2 GB | High accuracy |

Pre-download via CLI: `python3 backend/voice_backend.py download --model paraformer-zh`

## Releasing

Pushing a `v*` tag runs the release workflow, which builds the deb (CPack)
and the snap (snapcraft) and attaches them to a GitHub Release:

```bash
git tag v1.0.0
git push origin v1.0.0
```

## CLI debugging

```bash
# Full self-check + model download
python3 backend/voice_backend.py doctor

# Recognize a 16 kHz mono wav file instead of the microphone
python3 backend/voice_backend.py session --file test.wav --itn

# Slow GitHub connection? Use a proxy prefix for github.com downloads
export VOICEINPUT_MIRROR=https://ghfast.top
```

## Troubleshooting

- **Addon missing after install** — restart fcitx5 (`fcitx5 -r`) and check
  `fcitx5-diagnose`; the addon appears as "Voice Input / 语音输入" in
  fcitx5-configtool → Addons.
- **No sound / recorder error** — ensure `alsa-utils` or `pipewire-utils` is
  installed and your mic is the default source (`pavucontrol`).
- **Model download fails** — set `VOICEINPUT_MIRROR` (GitHub proxy) or
  manually place `model.int8.onnx`, `tokens.txt` (from
  [HuggingFace](https://huggingface.co/csukuangfj/sherpa-onnx-sense-voice-zh-en-ja-ko-yue-2024-07-17))
  and `silero_vad.onnx` in `~/.local/share/fcitx5-voiceinput/models/`.

## License

MIT. Models are distributed under their own licenses
([SenseVoice](https://github.com/FunAudioLLM/SenseVoice),
[Silero VAD](https://github.com/snakers4/silero-vad)) via
[sherpa-onnx](https://github.com/k2-fsa/sherpa-onnx).
