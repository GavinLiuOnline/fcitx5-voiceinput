#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""fcitx5-voiceinput backend.

Records microphone audio, performs endpoint detection with Silero VAD and
transcribes speech locally with a selectable sherpa-onnx offline model
(SenseVoice / Paraformer / Whisper / FireRedASR).

Subcommands:
  doctor    Check dependencies / download & verify models / test load.
  session   One voice input session for the fcitx5 addon.

Session protocol (stdout):
  PARTIAL\\n<draft text>\\n     live draft of the current phrase (preedit)
  COMMIT\\n<final text>\\n     final text of one VAD segment (committed)
  NEWLINE\\n                    line break requested via Enter (no payload)
  STATUS\\n<message>\\n        panel note (model download/loading)
  ERROR\\n<message>\\n          on failure
  DONE\\n                       session finished
Diagnostics go to stderr.
"""

import argparse
import fcntl
import os
import queue
import re
import shutil
import signal
import subprocess
import sys
import tarfile
import threading
import time
import traceback
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path

import numpy as np

SAMPLE_RATE = 16000
CHUNK_SAMPLES = 4096

DATA_HOME = Path(os.environ.get("XDG_DATA_HOME", str(Path.home() / ".local/share")))
DEFAULT_MODEL_DIR = DATA_HOME / "fcitx5-voiceinput" / "models"
BASE_URL = "https://github.com/k2-fsa/sherpa-onnx/releases/download/asr-models"
VAD_URL = f"{BASE_URL}/silero_vad.onnx"

# Offline recognition models selectable in the fcitx5 configuration panel.
# `type` selects the sherpa_onnx.OfflineRecognizer factory; every tarball
# comes from the official sherpa-onnx asr-models GitHub release.
MODEL_REGISTRY = {
    "sense-voice-small": {
        "dirname": "sherpa-onnx-sense-voice-zh-en-ja-ko-yue-2024-07-17",
        "type": "sense_voice",
        "size": "约 230 MB",
        # Prefer per-file HF downloads (~239 MB) over the ~1 GB tarball.
        "hf_repo": "csukuangfj/sherpa-onnx-sense-voice-zh-en-ja-ko-yue-2024-07-17",
    },
    "paraformer-zh": {
        "dirname": "sherpa-onnx-paraformer-zh-2023-09-14",
        "type": "paraformer",
        "size": "约 230 MB",
    },
    "whisper-base": {
        "dirname": "sherpa-onnx-whisper-base",
        "type": "whisper",
        "size": "约 145 MB",
    },
    "fire-red-asr-large": {
        "dirname": "sherpa-onnx-fire-red-asr-large-zh_en-2025-02-16",
        "type": "fire_red",
        "size": "约 1.2 GB",
    },
}
MODEL_CHOICES = tuple(MODEL_REGISTRY)
DEFAULT_MODEL_ID = "sense-voice-small"

# Streaming model used only for the live preedit draft (small bilingual
# zh-en zipformer); the final committed text always comes from SenseVoice.
STREAM_DIRNAME = "sherpa-onnx-streaming-zipformer-small-bilingual-zh-en-2023-02-16"
STREAM_TARBALL_URL = f"{BASE_URL}/{STREAM_DIRNAME}.tar.bz2"

# Ordered download sources: upstream first, then mirrors.
HF_BASES = (
    "https://huggingface.co",
    "https://hf-mirror.com",      # mirror for users in Chinese mainland
)

# Set VOICEINPUT_MIRROR to a gh proxy prefix to speed up GitHub downloads,
# e.g. export VOICEINPUT_MIRROR=https://ghfast.top
MIRROR = os.environ.get("VOICEINPUT_MIRROR", "").rstrip("/")

LANG_CHOICES = ("auto", "zh", "en", "ja", "ko", "yue")


class _HTTP308RedirectHandler(urllib.request.HTTPRedirectHandler):
    """urllib does not follow 308 (used by hf-mirror); map it to 302."""

    http_error_308 = urllib.request.HTTPRedirectHandler.http_error_302


_opener = urllib.request.build_opener(_HTTP308RedirectHandler)


def _open_following_redirects(req: urllib.request.Request, timeout: int):
    """urllib raises HTTPError 308 instead of following it; follow manually."""
    url = req.full_url
    for _ in range(5):
        try:
            return _opener.open(req, timeout=timeout)
        except urllib.error.HTTPError as e:
            if e.code == 308 and e.headers.get("Location"):
                url = urllib.parse.urljoin(url, e.headers["Location"])
                req = urllib.request.Request(url, headers=dict(req.headers))
                continue
            raise
    raise IOError("重定向次数过多")


def _download_once(url: str, dest: Path, on_progress=None) -> None:
    part = dest.with_suffix(dest.suffix + ".part")
    part.parent.mkdir(parents=True, exist_ok=True)
    have = part.stat().st_size if part.is_file() else 0
    headers = {"User-Agent": "fcitx5-voiceinput"}
    append = False
    if have:
        headers["Range"] = f"bytes={have}-"
        append = True
    req = urllib.request.Request(url, headers=headers)
    with _open_following_redirects(req, 60) as resp:
        if append and resp.status != 206:
            # Server ignored the Range request: restart from scratch.
            append = False
            have = 0
        total = int(resp.headers.get("Content-Length") or 0)
        if total:
            total += have
        mode = "ab" if append else "wb"
        done = have
        last_pct = -1
        with open(part, mode) as f:
            while True:
                chunk = resp.read(256 * 1024)
                if not chunk:
                    break
                f.write(chunk)
                done += len(chunk)
                if total:
                    pct = done * 100 // total
                    if pct != last_pct:
                        last_pct = pct
                        print(f"\r    {pct:3d}%  {done / 1e6:7.1f} / "
                              f"{total / 1e6:.1f} MB",
                              end="", file=sys.stderr, flush=True)
                        if on_progress:
                            on_progress(pct)
    print(file=sys.stderr)
    if total and done < total:
        raise IOError(f"连接中断于 {done / 1e6:.1f}/{total / 1e6:.1f} MB")
    part.rename(dest)


def download(url: str, dest: Path, attempts: int = 5, on_progress=None) -> None:
    if MIRROR and url.startswith("https://github.com/"):
        url = MIRROR + "/" + url
    print(f"    下载 {url}", file=sys.stderr)
    last_err = None
    for _ in range(attempts):
        try:
            _download_once(url, dest, on_progress)
            return
        except Exception as e:  # noqa: BLE001
            last_err = e
            time.sleep(1)
    raise last_err


def download_first(filename: str, dest: Path, sources,
                   on_progress=None) -> bool:
    """Try each source URL base until one succeeds."""
    for base in sources:
        try:
            download(f"{base}/{filename}", dest, on_progress=on_progress)
            return True
        except Exception as e:  # noqa: BLE001
            print(f"    源 {base} 失败: {e}", file=sys.stderr)
    return False


def _child_die_with_parent():
    """arecord/pw-record must never outlive this backend (SIGKILL safety)."""
    try:
        import ctypes
        ctypes.CDLL("libc.so.6", use_errno=True).prctl(1, signal.SIGTERM)
    except Exception:  # noqa: BLE001
        pass


class Recorder:
    """Unified microphone source: 16 kHz mono s16 with a poll-able fd."""

    def __init__(self):
        self.proc = None
        self._stream = None
        self._wav_check = False  # strip possible WAV header (pw-record)
        self._stripped = False
        if shutil.which("arecord"):
            self.name = "arecord"
            cmd = ["arecord", "-q", "-t", "raw", "-f", "S16_LE",
                   "-c", "1", "-r", str(SAMPLE_RATE), "-"]
            self.proc = subprocess.Popen(
                cmd, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                preexec_fn=_child_die_with_parent)
            self.fd = self.proc.stdout.fileno()
        elif shutil.which("pw-record"):
            self.name = "pw-record"
            cmd = ["pw-record", "--format", "s16",
                   "--rate", str(SAMPLE_RATE), "--channels", "1", "-"]
            self.proc = subprocess.Popen(
                cmd, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                preexec_fn=_child_die_with_parent)
            self.fd = self.proc.stdout.fileno()
            self._wav_check = True
        else:
            try:
                import sounddevice as sd
            except ImportError:
                raise RuntimeError(
                    "未找到录音工具：请安装 alsa-utils (arecord)，"
                    "pipewire-utils (pw-record)，或 pip3 install --user sounddevice")
            self.name = "sounddevice"
            r, w = os.pipe()
            self._wfd = w

            def callback(indata, frames, time_info, status):
                try:
                    os.write(w, bytes(indata))
                except OSError:
                    pass

            self._stream = sd.RawInputStream(
                samplerate=SAMPLE_RATE, blocksize=1024, dtype="int16",
                channels=1, callback=callback)
            self._stream.start()
            self.fd = r

    def read(self, nbytes: int) -> bytes:
        """Blocking read of raw s16le bytes (call after fd is readable)."""
        data = os.read(self.fd, nbytes)
        if self._wav_check and not self._stripped:
            while len(data) < 44:
                data += os.read(self.fd, 44 - len(data))
            self._stripped = True
            if data[:4] == b"RIFF":
                data = data[44:]
        return data

    def close(self):
        if self._stream:
            try:
                self._stream.stop()
                self._stream.close()
            except Exception:  # noqa: BLE001
                pass
            self._stream = None
        if self.proc:
            if self.proc.poll() is None:
                try:
                    self.proc.terminate()
                    self.proc.wait(timeout=2)
                except Exception:  # noqa: BLE001
                    self.proc.kill()
            self.proc = None


def make_vad(vad_path: Path, silence_ms: int):
    import sherpa_onnx

    cfg = sherpa_onnx.VadModelConfig()
    cfg.silero_vad.model = str(vad_path)
    cfg.silero_vad.threshold = 0.5
    cfg.silero_vad.min_speech_duration = 0.25
    cfg.silero_vad.min_silence_duration = max(0.2, silence_ms / 1000.0)
    cfg.silero_vad.window_size = 512
    cfg.sample_rate = SAMPLE_RATE
    return sherpa_onnx.VoiceActivityDetector(cfg, buffer_size_in_seconds=120)


def drain_segments(vad):
    """Pull all completed speech segments from the VAD buffer."""
    segments = []
    while not vad.empty():
        seg = vad.front
        if callable(seg):
            seg = seg()
        samples = getattr(seg, "samples", None)
        if samples is None and isinstance(seg, tuple):
            samples = seg[-1]
        segments.append(np.asarray(samples, dtype=np.float32))
        vad.pop()
    return segments


_CJK_TIGHT = re.compile(
    r"(?<=[\u3040-\u30ff\u3400-\u9fff\uf900-\ufaff\u3000-\u303f])\s+"
    r"(?=[\u3040-\u30ff\u3400-\u9fff\uf900-\ufaff\u3000-\u303f])")


# SenseVoice appends sentence-final punctuation to every recognized
# segment; strip it so pause-based segmentation does not end each
# phrase with a full stop the user never dictated.
_SEGMENT_TRAILING_PUNCT = "。．，,、！!？?；;：:….．"


def _tighten_cjk(text: str) -> str:
    """Streaming models emit spaces between tokens; CJK reads better tight."""
    return _CJK_TIGHT.sub("", text)


def emit_status(msg: str) -> None:
    """Send a panel status note to the fcitx5 addon ("" clears it)."""
    print("STATUS", flush=True)
    print(msg, flush=True)


_notify_id = 0


def notify_desktop(body: str, sticky: bool = False) -> None:
    """Show a desktop notification, replacing the previous one.

    Best effort: silently no-op without a session bus (uses gdbus, which
    ships with every glib-based desktop). Progress updates reuse the same
    replaces_id so the notification does not spam the tray. sticky=True
    keeps the notification until dismissed (used for success/failure).
    """
    global _notify_id
    if not shutil.which("gdbus"):
        return
    try:
        out = subprocess.run(
            ["gdbus", "call", "--session",
             "--dest", "org.freedesktop.Notifications",
             "--object-path", "/org/freedesktop/Notifications",
             "--method", "org.freedesktop.Notifications.Notify",
             "fcitx5-voiceinput", str(_notify_id), "audio-input-microphone",
             "语音输入模型下载", body, "[]", "{}",
             "0" if sticky else "8000"],
            capture_output=True, text=True, timeout=5)
        if out.returncode != 0:
            print(f"[voice] 通知发送失败: {out.stderr.strip()}", file=sys.stderr)
            return
        m = re.search(r"uint32\s+(\d+)", out.stdout)
        if m and m.group(1) != "0":
            _notify_id = int(m.group(1))
    except Exception as e:  # noqa: BLE001
        print(f"[voice] 通知发送失败: {e}", file=sys.stderr)


def make_progress_notifier(prefix: str):
    """One desktop notification per 20% step, all replacing each other."""
    state = [-1]

    def on_progress(pct: int) -> None:
        if pct // 20 > state[0] // 20 or pct == 100:
            state[0] = pct
            notify_desktop(f"{prefix}… {pct}%")

    return on_progress


def model_entry(model_id: str) -> dict:
    return MODEL_REGISTRY.get(model_id, MODEL_REGISTRY[DEFAULT_MODEL_ID])


def _pick_model_file(base: Path, pattern: str):
    cands = sorted(base.glob(pattern))
    files = [p for p in cands if "int8" in p.name] or cands
    return files[0] if files else None


def find_offline_model(model_id: str, model_dir: Path):
    """Resolve the onnx files of the selected model; None if incomplete."""
    entry = model_entry(model_id)
    base = model_dir / entry["dirname"]
    if not base.is_dir():
        return None
    kind = entry["type"]
    if kind in ("sense_voice", "paraformer"):
        need = {"model": _pick_model_file(base, "model*.onnx"),
                "tokens": base / "tokens.txt"}
    elif kind == "whisper":
        need = {"encoder": _pick_model_file(base, "*encoder*.onnx"),
                "decoder": _pick_model_file(base, "*decoder*.onnx"),
                "tokens": _pick_model_file(base, "*tokens*.txt")}
    else:  # fire_red
        need = {"encoder": _pick_model_file(base, "*encoder*.onnx"),
                "decoder": _pick_model_file(base, "*decoder*.onnx"),
                "tokens": base / "tokens.txt"}
    if any(v is None or not Path(v).is_file() for v in need.values()):
        return None
    need = {k: str(v) for k, v in need.items()}
    need["kind"] = kind
    return need


def ensure_vad(model_dir: Path) -> bool:
    vad_path = model_dir / "silero_vad.onnx"
    if vad_path.is_file() and vad_path.stat().st_size < 500_000:
        print("    检测到不完整的 VAD 模型，重新下载...", file=sys.stderr)
        vad_path.unlink()
    if not vad_path.is_file():
        try:
            download(VAD_URL, vad_path)
        except Exception as e:  # noqa: BLE001
            print(f"    ✘ 下载 VAD 模型失败: {e}", file=sys.stderr)
            return False
    return True


def prepare_offline_model(model_id: str, model_dir: Path,
                          on_progress=None) -> bool:
    """Serialize model preparation across processes (doctor + sessions)."""
    model_dir.mkdir(parents=True, exist_ok=True)
    lock_fd = open(model_dir / ".download.lock", "w")
    try:
        try:
            fcntl.flock(lock_fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError:
            # Someone else is downloading. If the model is meanwhile
            # complete on disk (and VAD is present), use it directly
            # instead of waiting for the lock.
            if (find_offline_model(model_id, model_dir) is not None
                    and (model_dir / "silero_vad.onnx").is_file()):
                fcntl.flock(lock_fd, fcntl.LOCK_UN)
                lock_fd.close()
                return True
            emit_status("识别模型正在后台下载，请稍候…")
            fcntl.flock(lock_fd, fcntl.LOCK_EX)
            emit_status("")
        return _prepare_offline_locked(model_id, model_dir, on_progress)
    finally:
        fcntl.flock(lock_fd, fcntl.LOCK_UN)
        lock_fd.close()


def spawn_detached_download(model_id: str, model_dir: Path) -> None:
    """Start a detached `download` child, unless one is already running.

    Double-fork so the downloader is reparented to init and survives the
    session being killed (Esc / hotkey / focus loss must not abort the
    download).
    """
    model_dir.mkdir(parents=True, exist_ok=True)
    probe = os.open(model_dir / ".download.lock", os.O_WRONLY | os.O_CREAT)
    try:
        try:
            fcntl.flock(probe, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError:
            return  # a download is already running; nothing to spawn
        fcntl.flock(probe, fcntl.LOCK_UN)
    finally:
        os.close(probe)
    try:
        pid = os.fork()
    except OSError:
        return
    if pid == 0:
        try:
            os.setsid()
            if os.fork() == 0:
                argv = [sys.executable, "-u", str(Path(__file__).resolve()),
                        "download", "--model", model_id]
                os.execvp(argv[0], argv)
        except BaseException:  # noqa: BLE001
            pass
        os._exit(127)
    try:
        os.waitpid(pid, 0)
    except ChildProcessError:
        pass


def wait_model_ready(model_id: str, model_dir: Path,
                     timeout_s: int = 1800) -> bool:
    """Poll until the model + VAD are complete on disk (or timeout)."""
    deadline = time.monotonic() + timeout_s
    while True:
        if (find_offline_model(model_id, model_dir) is not None
                and (model_dir / "silero_vad.onnx").is_file()):
            return True
        if time.monotonic() > deadline:
            return False
        time.sleep(2)


def _prepare_offline_locked(model_id: str, model_dir: Path,
                            on_progress=None) -> bool:
    """Make sure the selected offline model + VAD exist, downloading if not."""
    if not ensure_vad(model_dir):
        return False
    if find_offline_model(model_id, model_dir):
        return True
    entry = model_entry(model_id)
    model_dir.mkdir(parents=True, exist_ok=True)
    emit_status(f"正在下载识别模型（{entry['size']}），请稍候…")
    print(f"[voice] 开始下载模型 {entry['dirname']}", file=sys.stderr,
          flush=True)
    hf_repo = entry.get("hf_repo")
    if hf_repo:
        # Preferred path: per-file downloads, much smaller than the tarball.
        base = model_dir / entry["dirname"]
        base.mkdir(parents=True, exist_ok=True)
        int8 = base / "model.int8.onnx"
        tokens = base / "tokens.txt"
        if int8.is_file() and int8.stat().st_size < 100_000_000:
            print("    检测到不完整的识别模型，重新下载...", file=sys.stderr)
            int8.unlink()
        if not int8.is_file():
            if not download_first(f"{hf_repo}/resolve/main/model.int8.onnx",
                                  int8, HF_BASES, on_progress=on_progress):
                print("    回退到完整压缩包...", file=sys.stderr)
            elif not tokens.is_file():
                download_first(f"{hf_repo}/resolve/main/tokens.txt",
                               tokens, HF_BASES)
    if not find_offline_model(model_id, model_dir):
        archive = model_dir / (entry["dirname"] + ".tar.bz2")
        if not archive.is_file() or archive.stat().st_size < 1_000_000:
            try:
                download(f"{BASE_URL}/{entry['dirname']}.tar.bz2", archive,
                         on_progress=on_progress)
            except Exception as e:  # noqa: BLE001
                print(f"    ✘ 下载识别模型失败: {e}", file=sys.stderr)
                emit_status("模型下载失败，请检查网络")
                return False
        emit_status("正在解压模型…")
        print("    解压中...", file=sys.stderr)
        try:
            with tarfile.open(archive, "r:bz2") as tar:
                tar.extractall(model_dir)
        except Exception as e:  # noqa: BLE001
            print(f"    ✘ 解压失败: {e}", file=sys.stderr)
            emit_status("模型解压失败，详见系统日志")
            return False
    ok = find_offline_model(model_id, model_dir) is not None
    emit_status("" if ok else "模型下载失败，详见系统日志")
    return ok


def load_offline_recognizer(sherpa_onnx, model_id: str, model_dir: Path,
                            lang: str, itn: bool):
    """Load the selected offline model with its matching factory."""
    files = find_offline_model(model_id, model_dir)
    if files is None:
        raise RuntimeError(
            f"模型文件不完整: {model_entry(model_id)['dirname']}")
    kind = files["kind"]
    common = dict(tokens=files["tokens"], num_threads=2)
    lang = "" if lang == "auto" else lang
    if kind == "sense_voice":
        return sherpa_onnx.OfflineRecognizer.from_sense_voice(
            model=files["model"], use_itn=itn, language=lang, **common)
    if kind == "paraformer":
        return sherpa_onnx.OfflineRecognizer.from_paraformer(
            paraformer=files["model"], **common)
    if kind == "whisper":
        return sherpa_onnx.OfflineRecognizer.from_whisper(
            encoder=files["encoder"], decoder=files["decoder"],
            language=lang, task="transcribe", **common)
    return sherpa_onnx.OfflineRecognizer.from_fire_red_asr(
        encoder=files["encoder"], decoder=files["decoder"], **common)


def find_streaming_model(model_dir: Path):
    """Locate streaming zipformer onnx files (prefer int8 variants)."""
    base = model_dir / STREAM_DIRNAME
    if not base.is_dir():
        return None

    def pick(prefix):
        cands = sorted(base.glob(prefix))
        files = [p for p in cands if "int8" in p.name] or cands
        return str(files[0]) if files else None

    encoder = pick("encoder*.onnx")
    decoder = pick("decoder*.onnx")
    joiner = pick("joiner*.onnx")
    tokens = base / "tokens.txt"
    if not (encoder and decoder and joiner and tokens.is_file()):
        return None
    return {"encoder": encoder, "decoder": decoder,
            "joiner": joiner, "tokens": str(tokens)}


def load_online_recognizer(sherpa_onnx, model_dir: Path):
    """Streaming recognizer for the live preedit draft (optional)."""
    files = find_streaming_model(model_dir)
    if files is None:
        return None
    try:
        return sherpa_onnx.OnlineRecognizer.from_transducer(
            encoder=files["encoder"],
            decoder=files["decoder"],
            joiner=files["joiner"],
            tokens=files["tokens"],
            num_threads=1,
            sample_rate=SAMPLE_RATE,
            feature_dim=80,
            decoding_method="greedy_search",
        )
    except Exception as e:  # noqa: BLE001
        print(f"[voice] 流式模型加载失败: {e}", file=sys.stderr, flush=True)
        return None


def cmd_session(args) -> int:
    flush_mode = 0  # 1 = flush pending audio (SIGUSR1), 2 = flush + newline

    def on_sigint(signum, frame):
        raise KeyboardInterrupt

    def on_usr(signum, frame):
        # Enter / newline requests are flag-based (no exceptions) so they
        # are safe at any point in the main loop.
        nonlocal flush_mode
        flush_mode = 1 if signum == signal.SIGUSR1 else 2

    signal.signal(signal.SIGINT, on_sigint)
    signal.signal(signal.SIGUSR1, on_usr)
    signal.signal(signal.SIGUSR2, on_usr)

    model_dir = Path(args.model_dir) if args.model_dir else DEFAULT_MODEL_DIR
    vad_model = model_dir / "silero_vad.onnx"

    rec = None
    reader_t = None
    chunks = queue.Queue()
    if args.file is None:
        # Capture from t=0: a reader thread buffers mic audio in memory
        # while the models load (~1-3 s), so the first words are never lost.
        try:
            rec = Recorder()
        except RuntimeError as e:
            print(f"ERROR\n{e}", flush=True)
            return 1
        print(f"[voice] recorder={rec.name}", file=sys.stderr, flush=True)

        def _reader():
            try:
                while True:
                    data = rec.read(CHUNK_SAMPLES * 2)
                    if not data:
                        break
                    chunks.put(data)
            except (OSError, ValueError):
                pass

        reader_t = threading.Thread(target=_reader, daemon=True)
        reader_t.start()

    try:
        import sherpa_onnx
    except ImportError:
        if rec:
            rec.close()
        print("ERROR\n缺少 sherpa-onnx，请执行: pip3 install --user sherpa-onnx",
              flush=True)
        return 1

    try:
        # First use of a freshly selected model: hand the download to a
        # detached background process, so it survives this session (Esc /
        # hotkey / focus change must not abort it). Mic audio keeps
        # buffering while the session waits for the model to appear; the
        # downloader reports its own progress as desktop notifications.
        if find_offline_model(args.model, model_dir) is None:
            spawn_detached_download(args.model, model_dir)
            emit_status("正在后台下载模型，请稍候…（结束录音不会中断下载）")
            if not wait_model_ready(args.model, model_dir):
                notify_desktop("模型下载失败，请检查网络后重试", sticky=True)
                if rec:
                    rec.close()
                print("ERROR\n模型下载失败，请检查网络或手动运行 doctor",
                      flush=True)
                return 1
            emit_status("")
        emit_status("正在加载识别模型…")
        recognizer = load_offline_recognizer(
            sherpa_onnx, args.model, model_dir, args.lang, args.itn)
        emit_status("")
    except KeyboardInterrupt:
        if rec:
            rec.close()
        print("DONE", flush=True)
        return 0
    except Exception as e:  # noqa: BLE001
        if rec:
            rec.close()
        print(f"ERROR\n加载识别模型失败: {e}", flush=True)
        return 1

    vad = make_vad(vad_model, args.silence_ms)

    # Streaming recognizer for the live preedit draft. It only speaks
    # zh/en, so other languages fall back to per-phrase commits; the
    # final committed text always comes from SenseVoice.
    online = None
    ostream = None
    if args.preedit and args.file is None and args.lang in ("auto", "zh", "en"):
        online = load_online_recognizer(sherpa_onnx, model_dir)
        if online is not None:
            ostream = online.create_stream()
        else:
            print("[voice] 无流式预览模型，跳过实时预编辑（运行 doctor 下载）",
                  file=sys.stderr, flush=True)

    def emit_segment(seg) -> "str | None":
        """Recognize one VAD segment and stream the final text out.

        Returns the final text ("" when nothing was recognized) or None on
        a fatal recognition error.
        """
        audio = np.asarray(seg, dtype=np.float32)
        if audio.size < SAMPLE_RATE // 10:  # ignore sub-0.1s blips
            return ""
        try:
            stream = recognizer.create_stream()
            stream.accept_waveform(SAMPLE_RATE, audio)
            recognizer.decode_stream(stream)
            text = stream.result.text.strip()
        except Exception as e:  # noqa: BLE001
            print(f"ERROR\n识别失败: {e}", flush=True)
            return None
        text = text.rstrip(_SEGMENT_TRAILING_PUNCT).rstrip()
        # Always emit, even when empty, so the addon can drop any draft
        # preedit still on screen for this segment.
        print("COMMIT", flush=True)
        print(text, flush=True)
        return text

    def flush_vad() -> bool:
        vad.flush()
        for seg in drain_segments(vad):
            if emit_segment(seg) is None:
                return False
        return True

    if args.file:
        # Debug mode: run VAD + ASR over a wav file instead of the mic.
        import wave

        with wave.open(args.file, "rb") as wav:
            assert wav.getframerate() == SAMPLE_RATE, "需要 16kHz wav"
            frames = wav.getnframes()
            raw = wav.readframes(frames)
        audio = np.frombuffer(raw, dtype=np.int16).astype(np.float32) / 32768.0
        for i in range(0, audio.size, CHUNK_SAMPLES):
            vad.accept_waveform(audio[i:i + CHUNK_SAMPLES])
            for seg in drain_segments(vad):
                if emit_segment(seg) is None:
                    return 1
        if not flush_vad():
            return 1
    else:
        last_partial_at = 0.0
        last_draft = ""

        try:
            # Continuous dictation: buffered audio feeds the VAD (whole
            # phrases -> COMMIT) and the streaming model (draft -> PARTIAL)
            # until SIGINT (space released / hotkey pressed again). There is
            # no idle timeout: session length is fully user-controlled.
            while True:
                if flush_mode:
                    # Enter pressed: commit the pending segment now and
                    # keep listening; mode 2 also inserts a line break
                    # (a newline cannot travel as a COMMIT payload).
                    enter_newline = flush_mode == 2
                    flush_mode = 0
                    if not flush_vad():
                        return 1
                    if enter_newline:
                        print("NEWLINE", flush=True)
                    last_draft = ""
                    if ostream is not None:
                        ostream = online.create_stream()
                    continue
                try:
                    data = chunks.get(timeout=0.2)
                except queue.Empty:
                    if (rec is not None and rec.proc is not None
                            and rec.proc.poll() is not None
                            and chunks.empty()):
                        print("ERROR\n录音进程已退出", flush=True)
                        return 1
                    continue
                chunk = np.frombuffer(
                    data, dtype=np.int16).astype(np.float32) / 32768.0
                vad.accept_waveform(chunk)
                if ostream is not None:
                    ostream.accept_waveform(SAMPLE_RATE, chunk)
                for seg in drain_segments(vad):
                    text = emit_segment(seg)
                    if text is None:
                        return 1
                    if not text:
                        print("[voice] 忽略空识别段（噪声）",
                              file=sys.stderr, flush=True)
                    last_draft = ""
                    if ostream is not None:
                        # Restart the draft so the preedit only shows the
                        # new phrase; finals come from SenseVoice.
                        ostream = online.create_stream()
                if ostream is not None:
                    try:
                        if online.is_ready(ostream):
                            online.decode_stream(ostream)
                            draft = _tighten_cjk(
                                online.get_result(ostream)).strip()
                            if (draft and draft != last_draft
                                    and time.monotonic() - last_partial_at >= 0.3):
                                last_partial_at = time.monotonic()
                                last_draft = draft
                                print("PARTIAL", flush=True)
                                print(draft, flush=True)
                    except Exception as e:  # noqa: BLE001
                        print(f"[voice] 流式解码失败: {e}",
                              file=sys.stderr, flush=True)
                        ostream = None
        except KeyboardInterrupt:
            # Trigger key pressed again: flush trailing audio and finish.
            try:
                flush_vad()
            except Exception:  # noqa: BLE001
                pass
        finally:
            if rec is not None:
                try:
                    rec.close()
                except Exception:  # noqa: BLE001
                    pass
            if reader_t is not None:
                reader_t.join(timeout=1.0)

    print("DONE", flush=True)
    return 0


def check_resources() -> bool:
    ok = True
    if sys.version_info < (3, 8):
        print("    ✘ 需要 Python >= 3.8", file=sys.stderr)
        ok = False
    try:
        import numpy  # noqa: F401
    except ImportError:
        print("    ✘ 缺少 numpy: pip3 install --user numpy", file=sys.stderr)
        ok = False
    try:
        import sherpa_onnx  # noqa: F401

        print(f"    sherpa-onnx 版本 {sherpa_onnx.__version__}", file=sys.stderr)
    except ImportError:
        print("    ✘ 缺少 sherpa-onnx: pip3 install --user sherpa-onnx",
              file=sys.stderr)
        ok = False
    if not (shutil.which("arecord") or shutil.which("pw-record")):
        print("    ✘ 未找到 arecord / pw-record，请安装 alsa-utils 或 pipewire-utils",
              file=sys.stderr)
        ok = False
    else:
        print(f"    录音工具: arecord={bool(shutil.which('arecord'))}, "
              f"pw-record={bool(shutil.which('pw-record'))}", file=sys.stderr)
    return ok


def prepare_streaming_model(model_dir: Path) -> bool:
    """Download the live-preedit streaming model (optional, recommended)."""
    if find_streaming_model(model_dir):
        return True
    archive = model_dir / (STREAM_DIRNAME + ".tar.bz2")
    if not archive.is_file() or archive.stat().st_size < 1_000_000:
        try:
            download(STREAM_TARBALL_URL, archive)
        except Exception as e:  # noqa: BLE001
            print(f"    ✘ 下载失败（不影响整句上屏）: {e}", file=sys.stderr)
            return False
    print("    解压中...", file=sys.stderr)
    try:
        with tarfile.open(archive, "r:bz2") as tar:
            tar.extractall(model_dir)
    except Exception as e:  # noqa: BLE001
        print(f"    ✘ 解压失败: {e}", file=sys.stderr)
        return False
    return find_streaming_model(model_dir) is not None


def verify_models(model_dir: Path, model_id: str) -> bool:
    ok = True
    entry = model_entry(model_id)
    files = find_offline_model(model_id, model_dir)
    if files:
        for k, v in files.items():
            if k == "kind":
                continue
            print(f"    ✔ 识别模型 {k} "
                  f"({Path(v).stat().st_size / 1e6:.1f} MB)", file=sys.stderr)
    else:
        print(f"    ✘ 识别模型文件不完整: {entry['dirname']}", file=sys.stderr)
        ok = False
    vad_path = model_dir / "silero_vad.onnx"
    if vad_path.is_file() and vad_path.stat().st_size >= 100_000:
        print(f"    ✔ 语音检测模型 Silero VAD "
              f"({vad_path.stat().st_size / 1e6:.1f} MB)", file=sys.stderr)
    else:
        print(f"    ✘ 语音检测模型缺失或损坏: {vad_path}", file=sys.stderr)
        ok = False
    if ok:
        try:
            make_vad(vad_path, 800)
        except Exception as e:  # noqa: BLE001
            print(f"    ✘ VAD 模型加载失败（文件可能损坏）: {e}", file=sys.stderr)
            ok = False
    if find_streaming_model(model_dir):
        print("    ✔ 流式预览模型（实时预编辑）", file=sys.stderr)
    else:
        print("    ⚠ 流式预览模型未就绪（实时预编辑不可用）", file=sys.stderr)
    return ok


def cmd_download(args) -> int:
    """Download one recognition model, or no-op when already complete.

    Used by the addon to prefetch a model right after the user picks it in
    fcitx5-configtool, and by users who want to pre-download from the CLI.
    Progress is reported as desktop notifications, so it stays visible even
    when launched in the background from fcitx5.
    """
    # No session protocol here; route STATUS notes and progress to stderr.
    sys.stdout = sys.stderr
    model_dir = Path(args.model_dir) if args.model_dir else DEFAULT_MODEL_DIR
    if find_offline_model(args.model, model_dir) is not None:
        print(f"模型已缓存: {args.model}", file=sys.stderr)
        return 0
    entry = model_entry(args.model)
    notify_desktop(f"开始下载 {args.model}（{entry['size']}）…")
    if not prepare_offline_model(args.model, model_dir,
                                 on_progress=make_progress_notifier(
                                     f"下载 {args.model}")):
        notify_desktop("模型下载失败，请检查网络后重试", sticky=True)
        return 1
    notify_desktop("模型下载完成，可以开始语音输入了", sticky=True)
    print(f"模型就绪: {args.model}", file=sys.stderr)
    return 0


def cmd_doctor(args) -> int:
    model_dir = Path(args.model_dir) if args.model_dir else DEFAULT_MODEL_DIR
    entry = model_entry(args.model)

    print("[1/6] 检查本地资源", file=sys.stderr, flush=True)
    if not check_resources():
        print("DOCTOR FAIL", file=sys.stderr)
        return 1

    print(f"[2/6] 准备识别模型 ({entry['dirname']}，{entry['size']})",
          file=sys.stderr, flush=True)
    print("[3/6] 准备语音检测模型 (Silero VAD)", file=sys.stderr, flush=True)
    if not prepare_offline_model(args.model, model_dir):
        print("DOCTOR FAIL", file=sys.stderr)
        return 1

    print("[4/6] 准备流式预览模型 (实时预编辑用)", file=sys.stderr, flush=True)
    if not prepare_streaming_model(model_dir):
        print("    ⚠ 已跳过：实时预编辑不可用，整句上屏不受影响",
              file=sys.stderr)

    print("[5/6] 校验模型文件", file=sys.stderr, flush=True)
    if not verify_models(model_dir, args.model):
        print("DOCTOR FAIL", file=sys.stderr)
        return 1

    print(f"[6/6] 试加载识别模型 ({args.model})", file=sys.stderr, flush=True)
    try:
        import sherpa_onnx

        recognizer = load_offline_recognizer(
            sherpa_onnx, args.model, model_dir, "auto", True)
        stream = recognizer.create_stream()
        silence = np.zeros(SAMPLE_RATE, dtype=np.float32)
        stream.accept_waveform(SAMPLE_RATE, silence)
        recognizer.decode_stream(stream)
    except Exception as e:  # noqa: BLE001
        print(f"    ✘ 加载失败: {e}", file=sys.stderr)
        print("DOCTOR FAIL", file=sys.stderr)
        return 1

    print(f"DOCTOR OK\n模型目录: {model_dir}", file=sys.stderr)
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description="fcitx5-voiceinput backend")
    sub = parser.add_subparsers(dest="cmd", required=True)

    p_doc = sub.add_parser("doctor", help="检查依赖 / 下载模型 / 验证加载")
    p_doc.add_argument("--model-dir", default=None)
    p_doc.add_argument("--model", default=DEFAULT_MODEL_ID,
                       choices=MODEL_CHOICES)

    p_dl = sub.add_parser("download",
                          help="下载指定识别模型（已缓存则跳过）")
    p_dl.add_argument("--model-dir", default=None)
    p_dl.add_argument("--model", default=DEFAULT_MODEL_ID,
                      choices=MODEL_CHOICES)

    p_sess = sub.add_parser("session", help="执行一次语音输入会话")
    p_sess.add_argument("--model", default=DEFAULT_MODEL_ID,
                        choices=MODEL_CHOICES)
    p_sess.add_argument("--lang", default="auto", choices=LANG_CHOICES)
    p_sess.add_argument("--silence-ms", type=int, default=800)
    p_sess.add_argument("--itn", action="store_true")
    p_sess.add_argument("--preedit", action="store_true",
                        help="启用实时预编辑草稿（需流式预览模型）")
    p_sess.add_argument("--model-dir", default=None)
    p_sess.add_argument("--file", default=None,
                        help="调试: 用 16k 单声道 wav 文件代替麦克风输入")

    args = parser.parse_args()
    try:
        if args.cmd == "doctor":
            return cmd_doctor(args)
        if args.cmd == "download":
            return cmd_download(args)
        return cmd_session(args)
    except KeyboardInterrupt:
        print("ERROR\n已取消", flush=True)
        return 1
    except Exception as e:  # noqa: BLE001
        # Last-resort guard: the fcitx5 addon expects a clean
        # COMMIT/ERROR protocol line pair (or EOF) on stdout, never a crash.
        print(f"ERROR\n{type(e).__name__}: {e}", flush=True)
        traceback.print_exc()
        return 1


if __name__ == "__main__":
    sys.exit(main())
