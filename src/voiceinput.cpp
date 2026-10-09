/*
 * fcitx5-voiceinput: press a hotkey, speak, and the recognized text is
 * committed into the focused application.
 *
 * The addon itself stays thin: it watches the trigger key, shows status
 * via the input panel, spawns the Python backend (recording + Silero VAD
 * + SenseVoice ASR) and commits the final text.
 *
 * SPDX-License-Identifier: MIT
 */
#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <fcitx-config/enum.h>
#include <fcitx-config/iniparser.h>
#include <fcitx-config/option.h>
#include <fcitx/action.h>
#include <fcitx/addonfactory.h>
#include <fcitx/addoninstance.h>
#include <fcitx/addonmanager.h>
#include <fcitx/event.h>
#include <fcitx/inputcontext.h>
#include <fcitx/inputcontextmanager.h>
#include <fcitx/inputpanel.h>
#include <fcitx/instance.h>
#include <fcitx/statusarea.h>
#include <fcitx/text.h>
#include <fcitx/userinterface.h>
#include <fcitx/userinterfacemanager.h>
#include <fcitx-utils/capabilityflags.h>
#include <fcitx-utils/event.h>
#include <fcitx-utils/i18n.h>
#include <fcitx-utils/key.h>

extern char **environ;

namespace fcitx {

enum class VoiceLanguage { Auto, Zh, En, Ja, Ko, Yue };
FCITX_CONFIG_ENUM_NAME(VoiceLanguage, "Auto", "Zh", "En", "Ja", "Ko", "Yue");

const char *languageCode(VoiceLanguage lang) {
    switch (lang) {
    case VoiceLanguage::Zh:
        return "zh";
    case VoiceLanguage::En:
        return "en";
    case VoiceLanguage::Ja:
        return "ja";
    case VoiceLanguage::Ko:
        return "ko";
    case VoiceLanguage::Yue:
        return "yue";
    default:
        return "auto";
    }
}

enum class VoiceModel {
    SenseVoiceSmall,
    ParaformerZh,
    WhisperBase,
    FireRedAsrLarge,
};
FCITX_CONFIG_ENUM_NAME(VoiceModel, "SenseVoiceSmall", "ParaformerZh",
                       "WhisperBase", "FireRedASRLarge");

const char *modelCode(VoiceModel model) {
    switch (model) {
    case VoiceModel::ParaformerZh:
        return "paraformer-zh";
    case VoiceModel::WhisperBase:
        return "whisper-base";
    case VoiceModel::FireRedAsrLarge:
        return "fire-red-asr-large";
    default:
        return "sense-voice-small";
    }
}

// Model sub-directories, order matches the VoiceModel enum. Keep in sync
// with MODEL_REGISTRY in backend/voice_backend.py.
constexpr const char *kVoiceModelDirnames[] = {
    "sherpa-onnx-sense-voice-zh-en-ja-ko-yue-2024-07-17",
    "sherpa-onnx-paraformer-zh-2023-09-14",
    "sherpa-onnx-whisper-base",
    "sherpa-onnx-fire-red-asr-large-zh_en-2025-02-16",
};

// True when the model directory exists and holds at least one .onnx file,
// mirroring what the backend's find_offline_model() accepts.
bool voiceModelCached(size_t index, const std::string &modelDir) {
    const char *xdg = std::getenv("XDG_DATA_HOME");
    std::string base = (xdg && *xdg) ? xdg : std::string(std::getenv("HOME") ?
                                                             std::getenv("HOME") :
                                                             "") +
                                       "/.local/share";
    std::string dir = modelDir.empty()
                          ? base + "/fcitx5-voiceinput/models/" +
                                kVoiceModelDirnames[index]
                          : modelDir + "/" + kVoiceModelDirnames[index];
    DIR *d = ::opendir(dir.c_str());
    if (!d) {
        return false;
    }
    bool cached = false;
    while (dirent *ent = ::readdir(d)) {
        const size_t len = strlen(ent->d_name);
        if (len > 5 && strcmp(ent->d_name + len - 5, ".onnx") == 0) {
            cached = true;
            break;
        }
    }
    ::closedir(d);
    return cached;
}

// Annotation that labels every model in the configtool combo box with its
// live cache status, probed on disk each time fcitx5-configtool opens the
// page (dumpDescription runs on every DBus GetConfig).
struct VoiceModelStatusAnnotation {
    // Points at the ModelDir option of the same config struct; only
    // dereferenced much later, when dumpDescription runs.
    const Option<std::string> *modelDirOption = nullptr;

    bool skipDescription() { return false; }
    bool skipSave() { return false; }
    void dumpDescription(RawConfig &config) const {
        const std::string modelDir =
            modelDirOption ? modelDirOption->value() : std::string();
        for (size_t i = 0; i < std::size(kVoiceModelDirnames); i++) {
            std::string label = _(_VoiceModel_Names[i]);
            label += voiceModelCached(i, modelDir) ? _(" · cached")
                                                   : _(" · not cached");
            config.setValueByPath("EnumI18n/" + std::to_string(i), label);
        }
    }
    static std::string toString(VoiceModel value) {
        return _(_VoiceModel_Names[static_cast<size_t>(value)]);
    }
};

enum class TriggerMode {
    HotkeyToggle,
    HoldSpace,
};
FCITX_CONFIG_ENUM_NAME_WITH_I18N(TriggerMode, "HotkeyToggle", "HoldSpace");

FCITX_CONFIGURATION(
    VoiceInputConfig,
    KeyListOption triggerKey{
        this, "TriggerKey", _("Trigger Key"), {Key("Control+Alt+V")},
        KeyListConstrain({KeyConstrainFlag::AllowModifierOnly})};
    OptionWithAnnotation<TriggerMode, TriggerModeI18NAnnotation> triggerMode{
        this, "TriggerMode", _("Trigger Mode"), TriggerMode::HoldSpace};
    // fcitx-config has no double marshalling, so the hold duration is an
    // int in milliseconds.
    Option<int, IntConstrain> holdMs{
        this, "HoldMs", _("Hold Space Milliseconds to Start"), 2500,
        IntConstrain(500, 10000)};
    Option<VoiceLanguage> language{this, "Language", _("Recognition Language"),
                                   VoiceLanguage::Auto};
    OptionWithAnnotation<VoiceModel, VoiceModelStatusAnnotation> model{
        this, "Model", _("Recognition Model"), VoiceModel::SenseVoiceSmall,
        NoConstrain<VoiceModel>(), DefaultMarshaller<VoiceModel>(),
        VoiceModelStatusAnnotation{&modelDir}};
    Option<int, IntConstrain> silenceMs{this, "SilenceMs",
                                        _("Sentence Pause (ms; draft commits on pause)"),
                                        800, IntConstrain(200, 5000)};
    Option<bool> useItn{this, "UseITN", _("Inverse Text Normalization"), true};
    Option<bool> livePreedit{this, "LivePreedit",
                             _("Live Preedit (text while speaking)"), true};
    Option<std::string> python{this, "Python", _("Python Interpreter"),
                               "python3"};
    Option<std::string> backend{this, "Backend", _("Backend Script"),
                                VOICEINPUT_BACKEND_PATH};
    Option<std::string> modelDir{this, "ModelDir", _("Model Directory"), ""};);

class VoiceInput final : public AddonInstance {
public:
    explicit VoiceInput(Instance *instance) : instance_(instance) {
        // Make _() resolve the addon's own catalog from its locale dir.
        registerDomain(FCITX_GETTEXT_DOMAIN, VOICEINPUT_LOCALEDIR);
        reloadConfig();

        keyHandler_ = instance_->watchEvent(
            EventType::InputContextKeyEvent, EventWatcherPhase::PreInputMethod,
            [this](Event &event) {
                auto &keyEvent = static_cast<KeyEvent &>(event);
                if (config_.triggerMode.value() == TriggerMode::HoldSpace) {
                    handleHoldSpace(keyEvent);
                    return;
                }
                if (keyEvent.isRelease()) {
                    return;
                }
                if (!keyEvent.key().checkKeyList(config_.triggerKey.value())) {
                    if (recording_ && !interrupted_) {
                        editWhileRecording(keyEvent);
                    }
                    return;
                }
                trigger(keyEvent.inputContext());
                keyEvent.filterAndAccept();
            });

        icCreatedHandler_ = instance_->watchEvent(
            EventType::InputContextCreated, EventWatcherPhase::Default,
            [this](Event &event) {
                auto &created =
                    static_cast<InputContextCreatedEvent &>(event);
                created.inputContext()->statusArea().addAction(
                    StatusGroup::AfterInputMethod, &action_);
            });

        // Focus left the field being dictated to (another window, another
        // input box, or the desktop): stop the recording. Moving the text
        // cursor inside the same field does not generate a focus change, so
        // it never interrupts a session.
        focusHandler_ = instance_->watchEvent(
            EventType::InputContextFocusOut, EventWatcherPhase::Default,
            [this](Event &event) {
                if (!recording_) {
                    return;
                }
                auto &focusEvent = static_cast<FocusOutEvent &>(event);
                if (focusEvent.inputContext()->uuid() == icUuid_) {
                    cancelSession();
                }
            });

        action_.setIcon("audio-input-microphone");
        action_.setShortText(_("Voice Input"));
        action_.connect<SimpleAction::Activated>(
            [this](InputContext *ic) { trigger(ic); });
        instance_->userInterfaceManager().registerAction(&action_);
    }

    ~VoiceInput() override {
        cancelSession();
        instance_->userInterfaceManager().unregisterAction(&action_);
    }

    void reloadConfig() override {
        const std::string prevModel =
            configLoaded_ ? modelCode(config_.model.value()) : std::string();
        readAsIni(config_, "conf/fcitx5-voiceinput.conf");
        if (!configLoaded_) {
            // First load (addon startup), not a user edit.
            configLoaded_ = true;
            return;
        }
        // A different model was picked in fcitx5-configtool: prefetch it
        // in the background right away, so the first dictation with the
        // new model doesn't have to wait for a download. The backend is a
        // no-op when the model is already complete on disk.
        const auto cur = modelCode(config_.model.value());
        if (cur != prevModel) {
            spawnModelDownload(cur);
        }
    }

    // Fire-and-forget `voice_backend.py download --model <id>`. Double
    // fork so the worker is reparented to init and never becomes a zombie
    // of the fcitx5 process.
    void spawnModelDownload(const std::string &code) {
        pid_t pid = ::fork();
        if (pid < 0) {
            return;
        }
        if (pid == 0) {
            ::setsid();
            const auto python = config_.python.value();
            const auto backend = config_.backend.value();
            const pid_t worker = ::fork();
            if (worker == 0) {
                ::execlp(python.c_str(), python.c_str(), "-u",
                         backend.c_str(), "download", "--model", code.c_str(),
                         static_cast<char *>(nullptr));
                _exit(127);
            }
            _exit(0);
        }
        int status = 0;
        ::waitpid(pid, &status, 0);
    }

    // Expose the configuration to fcitx5-configtool (DBus GetConfig).
    const Configuration *getConfig() const override { return &config_; }

    void setConfig(const RawConfig &raw) override {
        config_.load(raw);
        safeSaveAsIni(config_, "conf/fcitx5-voiceinput.conf");
    }

private:
    InputContext *findIc() const {
        return instance_->inputContextManager().findByUUID(icUuid_);
    }

    void showStatus(const std::string &message) {
        auto *ic = findIc();
        if (!ic) {
            return;
        }
        auto &panel = ic->inputPanel();
        panel.setAuxUp(Text(message));
        ic->updateUserInterface(UserInterfaceComponent::InputPanel);
    }

    void scheduleTick() {
        tickTimer_ = instance_->eventLoop().addTimeEvent(
            CLOCK_MONOTONIC, now(CLOCK_MONOTONIC) + 1000000, 100000,
            [this](EventSourceTime *, uint64_t) {
                if (!recording_) {
                    return false;
                }
                const auto elapsed =
                    (now(CLOCK_MONOTONIC) - startTime_) / 1000000;
                // The session has no time limit - it ends when the user
                // releases space / presses the hotkey again. The only
                // watchdog is for a backend that ignores SIGINT while
                // finalizing (model hang): never lock the panel forever.
                if (interrupted_ && elapsed > 120) {
                    showStatus(_("Voice backend timeout"));
                    cancelSession();
                    return false;
                }
                showStatus(recordingStatus());
                scheduleTick();
                return false;
            });
    }

    // Panel status line: a backend note (model download/loading) takes
    // precedence; otherwise show elapsed recording time.
    std::string recordingStatus() const {
        if (!statusNote_.empty()) {
            return std::string("🎤 ") + statusNote_;
        }
        const auto elapsed =
            (now(CLOCK_MONOTONIC) - startTime_) / 1000000;
        return std::string("🎤 ") + _("Recording") + " " +
               std::to_string(elapsed) + "s";
    }

    void trigger(InputContext *ic) {
        if (!recording_) {
            startSession(ic);
            return;
        }
        if (!interrupted_) {
            finalizeRecording();
            return;
        }
        // Third press: backend is stuck, hard cancel.
        showStatus(_("Voice input cancelled"));
        cancelSession();
    }

    // Second trigger-key press (or Escape): stop recording and transcribe
    // the remaining audio.
    void finalizeRecording() {
        interrupted_ = true;
        // The tick keeps running until the backend exits; without this it
        // would overwrite the panel with a still-counting "Recording Ns",
        // which looks exactly like "released but still recording".
        statusNote_ = _("Recognizing...");
        showStatus(recordingStatus());
        ::kill(child_, SIGINT);
    }

    // Editing keys while dictating:
    //   Escape    end the session (same as trigger key)
    //   Return    insert a newline (after flushing the pending phrase)
    //   BackSpace delete the last committed character
    // Any other simple printable key is typed straight into the text field,
    // so English letters, digits and punctuation can be mixed in.
    void editWhileRecording(KeyEvent &keyEvent) {
        const auto &key = keyEvent.key();
        if (key.check(Key("Escape"))) {
            finalizeRecording();
            keyEvent.filterAndAccept();
            return;
        }
        if (key.check(Key("Return"))) {
            ::kill(child_, SIGUSR2);
            keyEvent.filterAndAccept();
            return;
        }
        if (key.check(Key("BackSpace"))) {
            if (auto *ic = keyEvent.inputContext()) {
                ic->forwardKey(Key("BackSpace"));
            }
            keyEvent.filterAndAccept();
            return;
        }
        if (key.isSimple()) {
            const auto ch = Key::keySymToUnicode(key.sym());
            if (ch >= 0x20 && ch != 0x7f) {
                if (auto *ic = keyEvent.inputContext()) {
                    ic->commitString(Key::keySymToUTF8(key.sym()));
                }
                keyEvent.filterAndAccept();
                return;
            }
        }
    }

    // Hold-space trigger. A usable KeyRelease cannot be relied on (some
    // frontends swallow it entirely), so the key state is tracked through
    // the event stream:
    //   - auto-repeat presses carry KeyState::Repeat when the client
    //     advertises ReportKeyRepeat and mean the key is still held;
    //   - a flag-less press within 70ms of the previous one is still the
    //     same hold (humans cannot type that fast);
    //   - any other non-repeat press means the previous press already
    //     ended, so its pending space is emitted immediately - typing
    //     never waits for the hold threshold;
    //   - without the repeat flag at all, activity fallback applies:
    //     0.9s without any space event counts as released;
    //   - while recording, only key activity refreshes a watchdog; 0.7s
    //     of silence (or a real release event) stops the session;
    //   - only a bare space is intercepted: Ctrl+Space etc. pass through;
    //   - a preedit / candidate panel (pinyin) passes the space through so
    //     the input method keeps its "space selects candidate" behavior;
    //   - the deferred start aborts when the key already went up, so a
    //     release racing the threshold cannot spawn a ghost session;
    //   - during a finalizing session every space is swallowed - a hold
    //     armed then would fire a stray recording after the session ends.
    static constexpr uint64_t kProbeUs = 500000;      // activity check period
    static constexpr uint64_t kSilenceUs = 900000;    // "released" threshold
    static constexpr uint64_t kWatchdogUs = 700000;   // release-lost window
    static constexpr uint64_t kFlaglessRepeatUs = 70000;

    // A SIGBUS was observed when startSession() ran directly inside an
    // sd-event time callback, so IC-touching work is deferred to the next
    // loop iteration via a zero-delay time event. The event source is owned
    // by the unique_ptr returned from addTimeEvent(): destroying it
    // disables the source so the callback never runs (verified at runtime
    // against the installed libFcitx5Utils). holdDefer_ must therefore keep
    // it alive until it fired; replacing a previous, already-fired one-shot
    // is harmless.
    // IMPORTANT: addTimeEvent's usec is an ABSOLUTE moment on the given
    // clock (sd-event semantics), NOT a delay from now - verified at
    // runtime with /tmp/fcitx_timeprobe.cpp (a past moment like 1e6 us
    // fires immediately). Every relative delay must be expressed as
    // now() + delay. deferHold exploits this: 0 lies in the past, so the
    // callback runs on the next dispatch - a proper "defer" semantic.
    void deferHold(std::function<void()> action) {
        holdDefer_ = instance_->eventLoop().addTimeEvent(
            CLOCK_MONOTONIC, 0, 0,
            [this, action = std::move(action)](EventSourceTime *, uint64_t) {
                action();
                return false;
            });
    }

    void holdEmitSpace() {
        auto *ic = instance_->inputContextManager().findByUUID(holdIcUuid_);
        fprintf(stderr, "[voiceinput] hold: emitting space (ic=%p)\n",
                static_cast<void *>(ic));
        if (ic) {
            ic->forwardKey(Key(FcitxKey_space));
        }
    }

    void cancelHold(bool emitSpace) {
        holdTimer_.reset();
        holdProbe_.reset();
        if (emitSpace) {
            holdEmitSpace();
        }
    }

    void armHoldProbe() {
        holdProbe_ = instance_->eventLoop().addTimeEvent(
            CLOCK_MONOTONIC, now(CLOCK_MONOTONIC) + kProbeUs, 100000,
            [this](EventSourceTime *, uint64_t) {
                holdProbe_.reset();
                if (!holdTimer_) {
                    return false;
                }
                const auto idle = now(CLOCK_MONOTONIC) - lastSpaceAt_;
                if (idle <= kSilenceUs) {
                    armHoldProbe();  // repeats keep arriving: still holding
                    return false;
                }
                fprintf(stderr,
                        "[voiceinput] hold: silent for %llums, short press\n",
                        static_cast<unsigned long long>(idle / 1000));
                deferHold([this] { cancelHold(true); });
                return false;
            });
    }

    void armHoldWatchdog() {
        holdWatchdog_ = instance_->eventLoop().addTimeEvent(
            CLOCK_MONOTONIC, now(CLOCK_MONOTONIC) + kWatchdogUs, 100000,
            [this](EventSourceTime *, uint64_t) {
                // Self-reset before any re-arm: assigning over the source
                // whose callback is still running would destroy it mid-use.
                holdWatchdog_.reset();
                if (!recording_ || !holdStarted_) {
                    return false;
                }
                if (now(CLOCK_MONOTONIC) - lastSpaceAt_ <= kWatchdogUs) {
                    armHoldWatchdog();
                    return false;
                }
                fprintf(stderr,
                        "[voiceinput] hold: recording key released, "
                        "finalizing\n");
                deferHold([this] {
                    if (recording_ && holdStarted_ && !interrupted_) {
                        finalizeRecording();
                    }
                });
                return false;
            });
    }

    void handleHoldSpace(KeyEvent &keyEvent) {
        const auto &key = keyEvent.key();
        const bool isPlainSpace =
            key.check(Key(FcitxKey_space)) &&
            (key.states() & KeyState::SimpleMask) == 0;
        if (!isPlainSpace) {
            if (!keyEvent.isRelease() && holdTimer_ && key.isSimple()) {
                // Typing resumed before the probe ran: flush the pending
                // space of the previous short press right away.
                cancelHold(true);
            }
            if (!keyEvent.isRelease() &&
                key.checkKeyList(config_.triggerKey.value())) {
                trigger(keyEvent.inputContext());
                keyEvent.filterAndAccept();
                return;
            }
            if (!keyEvent.isRelease() && recording_ && !interrupted_) {
                editWhileRecording(keyEvent);
            }
            return;
        }
        const auto nowUs = now(CLOCK_MONOTONIC);
        const bool isRepeat =
            keyEvent.origKey().states().test(KeyState::Repeat) ||
            key.states().test(KeyState::Repeat);
        fprintf(stderr,
                "[voiceinput] space: rel=%d rep=%d state=%s\n",
                keyEvent.isRelease() ? 1 : 0, isRepeat ? 1 : 0,
                recording_ ? (holdStarted_ ? "recording" : "rec-hotkey")
                           : (holdTimer_ ? "waiting" : "idle"));
        // Physical space state as of this event. The deferred start consults
        // it so a release that slipped past the threshold never spawns a
        // ghost session.
        spaceDown_ = !keyEvent.isRelease();

        if (keyEvent.isRelease()) {
            if (holdTimer_) {
                // Short press, and releases do arrive: fastest path.
                keyEvent.filterAndAccept();
                fprintf(stderr, "[voiceinput] space: act=emit\n");
                cancelHold(true);
                return;
            }
            if (recording_ && holdStarted_ && !interrupted_) {
                keyEvent.filterAndAccept();
                holdWatchdog_.reset();
                fprintf(stderr, "[voiceinput] space: act=finalize\n");
                finalizeRecording();
            } else {
                fprintf(stderr, "[voiceinput] space: act=rel-pass\n");
            }
            return;
        }

        // Bare space press.
        if (!recording_ && !holdPendingStart_ && keyEvent.inputContext()) {
            auto &panel = keyEvent.inputContext()->inputPanel();
            if (panel.candidateList() || !panel.preedit().empty() ||
                !panel.clientPreedit().empty()) {
                // A preedit / candidate window belongs to the input
                // method (e.g. pinyin: space picks the highlighted
                // candidate). Never hijack that - and drop any hold that
                // was still pending.
                fprintf(stderr, "[voiceinput] space: act=composing\n");
                cancelHold(false);
                return;
            }
        }
        if (holdPendingStart_) {
            // Threshold fired and startSession is queued: swallow repeats
            // so they do not restart the hold timer meanwhile.
            keyEvent.filterAndAccept();
            lastSpaceAt_ = nowUs;
            fprintf(stderr, "[voiceinput] space: act=pending\n");
            return;
        }
        if (recording_ && holdStarted_ && !interrupted_) {
            // Key activity proves the key is still held; silence (0.7s)
            // or a real release event ends the session.
            keyEvent.filterAndAccept();
            lastSpaceAt_ = nowUs;
            armHoldWatchdog();
            fprintf(stderr, "[voiceinput] space: act=keepalive\n");
            return;
        }
        if (recording_) {
            // Hotkey session: the space is dictated text. A finalizing
            // (interrupted_) session must not arm a hold - its threshold
            // would fire a stray recording after the session ended.
            keyEvent.filterAndAccept();
            if (!interrupted_) {
                fprintf(stderr, "[voiceinput] space: act=dictate\n");
                editWhileRecording(keyEvent);
            } else {
                fprintf(stderr, "[voiceinput] space: act=swallow\n");
            }
            return;
        }
        if (holdTimer_) {
            if (isRepeat || nowUs - lastSpaceAt_ < kFlaglessRepeatUs) {
                // Still the same hold: a flagged repeat, or a flag-less
                // repeat too fast to be a human keypress.
                keyEvent.filterAndAccept();
                lastSpaceAt_ = nowUs;
                fprintf(stderr, "[voiceinput] space: act=hold\n");
                return;
            }
            // A fresh non-repeat press: the previous hold already ended
            // without a release event - it was a short press, emit its
            // pending space now.
            keyEvent.filterAndAccept();
            fprintf(stderr, "[voiceinput] space: act=flush\n");
            cancelHold(true);
        } else {
            keyEvent.filterAndAccept();
        }
        // A brand new pending hold.
        if (auto *ic = keyEvent.inputContext()) {
            holdIcUuid_ = ic->uuid();
        }
        lastSpaceAt_ = nowUs;
        fprintf(stderr, "[voiceinput] space: act=arm\n");
        holdTimer_ = instance_->eventLoop().addTimeEvent(
            CLOCK_MONOTONIC,
            nowUs +
                static_cast<uint64_t>(config_.holdMs.value()) * 1000,
            0,
            [this](EventSourceTime *, uint64_t) {
                holdTimer_.reset();
                holdProbe_.reset();
                if (recording_ || holdPendingStart_) {
                    return false;
                }
                if (now(CLOCK_MONOTONIC) - lastSpaceAt_ > kSilenceUs) {
                    // The key went up before the threshold and no release
                    // event ever arrived: emit the pending space.
                    deferHold([this] { holdEmitSpace(); });
                    return false;
                }
                fprintf(stderr, "[voiceinput] hold: threshold reached\n");
                // Heavy IC work (capability changes, posix_spawn, IO
                // source, panel updates) must not run inside the timer
                // dispatch - defer it to the next loop iteration.
                holdPendingStart_ = true;
                deferHold([this] {
                    holdPendingStart_ = false;
                    if (!spaceDown_ || recording_) {
                        // A release slipped in between the threshold and
                        // this deferred start: never spawn a ghost session
                        // for an already-released key.
                        fprintf(stderr,
                                "[voiceinput] hold: start aborted "
                                "(spaceDown=%d recording=%d)\n",
                                spaceDown_ ? 1 : 0, recording_ ? 1 : 0);
                        return;
                    }
                    auto *ic = instance_->inputContextManager().findByUUID(
                        holdIcUuid_);
                    if (!ic || !ic->hasFocus()) {
                        return;
                    }
                    startSession(ic);
                    if (recording_) {
                        // Session belongs to the held space key: enable
                        // the release watchdog (release events may never
                        // arrive).
                        holdStarted_ = true;
                        lastSpaceAt_ = now(CLOCK_MONOTONIC);
                        armHoldWatchdog();
                    }
                });
                return false;
            });
        armHoldProbe();
    }

    void startSession(InputContext *ic) {
        if (recording_) {
            return;
        }
        errorShown_ = false;
        expectPayload_ = false;
        payloadKind_ = PayloadKind::Commit;
        hasPartial_ = false;
        lastByte_ = 0;
        statusNote_.clear();

        int pipefd[2];
        if (::pipe2(pipefd, O_CLOEXEC) != 0) {
            showStatus(_("Failed to create pipe"));
            return;
        }

        std::vector<std::string> args;
        args.push_back(config_.python.value());
        args.push_back("-u");
        args.push_back(config_.backend.value());
        args.push_back("session");
        args.push_back("--lang");
        args.push_back(languageCode(config_.language.value()));
        args.push_back("--model");
        args.push_back(modelCode(config_.model.value()));
        args.push_back("--silence-ms");
        args.push_back(std::to_string(config_.silenceMs.value()));
        if (config_.useItn.value()) {
            args.push_back("--itn");
        }
        if (config_.livePreedit.value()) {
            args.push_back("--preedit");
        }
        const auto &modelDir = config_.modelDir.value();
        if (!modelDir.empty()) {
            args.push_back("--model-dir");
            args.push_back(modelDir);
        }

        std::vector<char *> argv;
        argv.reserve(args.size() + 1);
        for (auto &arg : args) {
            argv.push_back(const_cast<char *>(arg.c_str()));
        }
        argv.push_back(nullptr);

        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_adddup2(&actions, pipefd[1],
                                         STDOUT_FILENO);

        posix_spawnattr_t attr;
        posix_spawnattr_init(&attr);
        sigset_t mask;
        sigemptyset(&mask);
        posix_spawnattr_setsigmask(&attr, &mask);

        pid_t pid = -1;
        const int rc = ::posix_spawnp(&pid, args[0].c_str(), &actions, &attr,
                                      argv.data(), environ);
        posix_spawnattr_destroy(&attr);
        posix_spawn_file_actions_destroy(&actions);
        ::close(pipefd[1]);

        if (rc != 0) {
            ::close(pipefd[0]);
            showStatus(std::string("⚠ ") + _("Failed to launch backend") +
                       ": " + std::strerror(rc));
            return;
        }

        child_ = pid;
        outFd_ = pipefd[0];
        ::fcntl(outFd_, F_SETFL, ::fcntl(outFd_, F_GETFL) | O_NONBLOCK);
        recording_ = true;
        interrupted_ = false;
        buffer_.clear();
        icUuid_ = ic->uuid();
        startTime_ = now(CLOCK_MONOTONIC);

        if (auto *icNow = findIc()) {
            savedCaps_ = icNow->capabilityFlags();
            savedPreeditEnabled_ = icNow->isPreeditEnabled();
            fprintf(stderr, "[voiceinput] ic caps=0x%llx preeditEnabled=%d\n",
                    static_cast<unsigned long long>(savedCaps_.toInteger()),
                    savedPreeditEnabled_ ? 1 : 0);
            if (!savedCaps_.test(CapabilityFlag::Preedit)) {
                // Some frontends (e.g. GTK apps via XIM) can render inline
                // preedit but never advertise the capability bit, which
                // would push the draft into the panel popup instead of the
                // text cursor. Force-enable it for the session duration.
                icNow->setCapabilityFlags(savedCaps_ | CapabilityFlag::Preedit);
                capsForced_ = true;
            }
            if (!savedPreeditEnabled_) {
                // InputContext::updatePreedit() also gates on this flag;
                // XIM may have disabled it for this client.
                icNow->setEnablePreedit(true);
            }
        }

        ioSource_ = instance_->eventLoop().addIOEvent(
            outFd_, IOEventFlag::In,
            [this](EventSourceIO *, int fd, IOEventFlags flags) {
                return ioCallback(fd, flags);
            });
        showStatus(std::string("🎤 ") + _("Recording") + " 0s (" +
                   (config_.triggerMode.value() == TriggerMode::HoldSpace
                        ? _("release Space to stop")
                        : _("press trigger key to stop")) +
                   ")");
        scheduleTick();
    }

    bool ioCallback(int fd, IOEventFlags flags) {
        char buf[8192];
        bool eof = false;
        while (true) {
            const ssize_t n = ::read(fd, buf, sizeof(buf));
            if (n > 0) {
                buffer_.append(buf, n);
                continue;
            }
            if (n == 0) {
                eof = true;
                break;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                break;
            }
            eof = true;
            break;
        }
        processLines();
        if (!eof && !(flags & IOEventFlag::Hup)) {
            return true;
        }
        finishSession();
        return false;
    }

    void finishSession() {
        ioSource_.reset();
        tickTimer_.reset();
        holdWatchdog_.reset();
        holdStarted_ = false;
        recording_ = false;

        // Reap the child.
        if (child_ > 0) {
            int status = 0;
            for (int i = 0; i < 40; ++i) {
                const auto r = ::waitpid(child_, &status, WNOHANG);
                if (r == child_ || (r < 0 && errno != EINTR)) {
                    break;
                }
                struct timespec ts = {0, 25 * 1000 * 1000};
                nanosleep(&ts, nullptr);
            }
            child_ = -1;
        }
        if (outFd_ >= 0) {
            ::close(outFd_);
            outFd_ = -1;
        }

        clearPreedit();
        restoreCaps();
        if (errorShown_) {
            // Error message is already on screen.
        } else if (!hasPartial_) {
            showTransient(_("No speech recognized"));
        } else {
            // Clear the "Recording/Recognizing" status line once done.
            resetPanel();
        }
        buffer_.clear();
    }

    // Undo the preedit overrides from startSession.
    void restoreCaps() {
        if (capsForced_ || !savedPreeditEnabled_) {
            if (auto *ic = findIc()) {
                if (capsForced_) {
                    ic->setCapabilityFlags(savedCaps_);
                }
                if (!savedPreeditEnabled_) {
                    ic->setEnablePreedit(false);
                }
            }
            capsForced_ = false;
        }
        savedCaps_ = CapabilityFlags{};
        savedPreeditEnabled_ = true;
    }

    void cancelSession() {
        ioSource_.reset();
        tickTimer_.reset();
        errorTimer_.reset();
        holdWatchdog_.reset();
        holdStarted_ = false;
        recording_ = false;
        clearPreedit();
        restoreCaps();
        if (child_ > 0) {
            ::kill(child_, SIGKILL);
            int status = 0;
            ::waitpid(child_, &status, 0);
            child_ = -1;
        }
        if (outFd_ >= 0) {
            ::close(outFd_);
            outFd_ = -1;
        }
        buffer_.clear();
    }

    // Streaming line protocol from the backend:
    //   PARTIAL\n<text>\n  draft of the current phrase -> show as preedit
    //   COMMIT\n<text>\n   final text of one VAD segment -> drop the
    //                      preedit and commit into the application
    //   STATUS\n<msg>\n    panel note (model download/loading); "" clears
    //   ERROR\n<msg>\n     fatal error -> show message
    //   NEWLINE\n          line break requested via Enter (no payload)
    //   DONE\n             session finished
    void processLines() {
        auto pos = buffer_.find('\n');
        while (pos != std::string::npos) {
            std::string line = buffer_.substr(0, pos);
            buffer_.erase(0, pos + 1);
            if (expectPayload_) {
                expectPayload_ = false;
                while (!line.empty() && line.back() == '\r') {
                    line.pop_back();
                }
                switch (payloadKind_) {
                case PayloadKind::Error:
                    errorShown_ = true;
                    showTransient("⚠ " + line);
                    break;
                case PayloadKind::Preedit:
                    updatePreedit(line);
                    break;
                case PayloadKind::Commit:
                    // Commit BEFORE dropping the preedit. Clients reliably
                    // handle "commit while composing, then preedit reset"
                    // (this is the order fcitx5's own engines use), while a
                    // commit right after preedit-done can be swallowed.
                    if (!line.empty()) {
                        commitText(line);
                    }
                    clearPreedit();
                    break;
                case PayloadKind::Status:
                    statusNote_ = line;
                    showStatus(recordingStatus());
                    break;
                }
            } else if (line == "PARTIAL") {
                expectPayload_ = true;
                payloadKind_ = PayloadKind::Preedit;
            } else if (line == "COMMIT") {
                expectPayload_ = true;
                payloadKind_ = PayloadKind::Commit;
            } else if (line == "STATUS") {
                expectPayload_ = true;
                payloadKind_ = PayloadKind::Status;
            } else if (line == "NEWLINE") {
                commitText("\n");
            } else if (line == "ERROR") {
                expectPayload_ = true;
                payloadKind_ = PayloadKind::Error;
            }
            pos = buffer_.find('\n');
        }
    }

    void commitText(const std::string &text) {
        if (text.empty()) {
            return;
        }
        auto *ic = findIc();
        if (!ic) {
            return;
        }
        std::string out = text;
        // Insert a space when two ASCII words join across segments; UTF-8
        // lead bytes are never alnum so CJK text is concatenated directly.
        if (lastByte_ > 0 &&
            std::isalnum(static_cast<unsigned char>(lastByte_)) != 0 &&
            std::isalnum(static_cast<unsigned char>(out.front())) != 0) {
            out.insert(out.begin(), ' ');
        }
        fprintf(stderr, "[voiceinput] commit '%s' via %s\n", out.c_str(),
                ic->frontend());
        ic->commitString(out);
        lastByte_ = static_cast<signed char>(out.back());
        hasPartial_ = true;
    }

    // Live draft while speaking; an empty string clears it. The draft is
    // always pushed to the client as inline preedit at the text cursor and
    // never rendered in the input panel popup.
    void updatePreedit(const std::string &text) {
        auto *ic = findIc();
        if (!ic) {
            return;
        }
        Text preedit;
        if (!text.empty()) {
            preedit.append(text, TextFormatFlag::Underline);
        }
        auto &panel = ic->inputPanel();
        panel.setClientPreedit(preedit);
        panel.setPreedit(Text());
        ic->updatePreedit();
    }

    void clearPreedit() { updatePreedit(""); }

    void resetPanel() {
        auto *ic = findIc();
        if (!ic) {
            return;
        }
        auto &panel = ic->inputPanel();
        panel.reset();
        ic->updateUserInterface(UserInterfaceComponent::InputPanel);
    }

    void showTransient(const std::string &message) {
        showStatus(message);
        errorTimer_ = instance_->eventLoop().addTimeEvent(
            CLOCK_MONOTONIC, now(CLOCK_MONOTONIC) + 2500000, 100000,
            [this](EventSourceTime *, uint64_t) {
                auto *ic = findIc();
                if (ic) {
                    auto &panel = ic->inputPanel();
                    panel.reset();
                    ic->updateUserInterface(UserInterfaceComponent::InputPanel);
                }
                errorTimer_.reset();
                return false;
            });
    }

    Instance *instance_;
    VoiceInputConfig config_;
    bool configLoaded_ = false;
    std::unique_ptr<HandlerTableEntry<EventHandler>> keyHandler_;
    std::unique_ptr<HandlerTableEntry<EventHandler>> icCreatedHandler_;
    std::unique_ptr<HandlerTableEntry<EventHandler>> focusHandler_;
    SimpleAction action_;
    std::unique_ptr<EventSourceIO> ioSource_;
    std::unique_ptr<EventSourceTime> tickTimer_;
    std::unique_ptr<EventSourceTime> errorTimer_;
    std::unique_ptr<EventSourceTime> holdTimer_;
    std::unique_ptr<EventSourceTime> holdProbe_;
    std::unique_ptr<EventSourceTime> holdWatchdog_;
    std::unique_ptr<EventSourceTime> holdDefer_;
    ICUUID icUuid_{};
    ICUUID holdIcUuid_{};
    uint64_t lastSpaceAt_ = 0;
    // True only for sessions started by holding space (they end on key
    // release / repeat silence); hotkey sessions are unaffected.
    bool holdStarted_ = false;
    // Threshold fired but startSession is still queued via deferHold();
    // swallow leftover space repeats so they do not restart the timer.
    bool holdPendingStart_ = false;
    // Latest known physical state of the space key in HoldSpace mode.
    bool spaceDown_ = false;
    pid_t child_ = -1;
    int outFd_ = -1;
    std::string buffer_;
    uint64_t startTime_ = 0;
    bool recording_ = false;
    bool interrupted_ = false;
    bool errorShown_ = false;
    bool expectPayload_ = false;
    bool hasPartial_ = false;
    signed char lastByte_ = 0;

    // Kind of the payload line announced by the preceding tag line.
    enum class PayloadKind { Commit, Preedit, Error, Status };
    PayloadKind payloadKind_ = PayloadKind::Commit;
    std::string statusNote_;
    CapabilityFlags savedCaps_{};
    bool capsForced_ = false;
    bool savedPreeditEnabled_ = true;
};

} // namespace fcitx

class VoiceInputFactory : public fcitx::AddonFactory {
    fcitx::AddonInstance *create(fcitx::AddonManager *manager) override {
        return new fcitx::VoiceInput(manager->instance());
    }
};

FCITX_ADDON_FACTORY(VoiceInputFactory);
