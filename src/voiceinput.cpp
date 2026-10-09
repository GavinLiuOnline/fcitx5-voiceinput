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
FCITX_CONFIG_ENUM_NAME_WITH_I18N(VoiceModel, "SenseVoiceSmall",
                                 "ParaformerZh", "WhisperBase",
                                 "FireRedASRLarge");

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

FCITX_CONFIGURATION(
    VoiceInputConfig,
    KeyListOption triggerKey{
        this, "TriggerKey", _("Trigger Key"), {Key("Control+Alt+V")},
        KeyListConstrain({KeyConstrainFlag::AllowModifierOnly})};
    Option<VoiceLanguage> language{this, "Language", _("Recognition Language"),
                                   VoiceLanguage::Auto};
    OptionWithAnnotation<VoiceModel, VoiceModelI18NAnnotation> model{
        this, "Model", _("Recognition Model"), VoiceModel::SenseVoiceSmall};
    Option<int, IntConstrain> maxSeconds{
        this, "MaxSeconds", _("Auto-stop Idle Seconds (no new text)"), 6,
        IntConstrain(3, 120)};
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
                if (keyEvent.isRelease()) {
                    return;
                }
                if (!keyEvent.key().checkKeyList(config_.triggerKey.value())) {
                    if (recording_ && !interrupted_) {
                        const auto &key = keyEvent.key();
                        // Editing keys while dictating:
                        //   Escape    end the session (same as trigger key)
                        //   Return    insert a newline (after flushing the
                        //             pending phrase)
                        //   BackSpace delete the last committed character
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
                        // Any other simple printable key is typed straight
                        // into the text field, so English letters, digits
                        // and punctuation can be mixed into dictation.
                        if (key.isSimple()) {
                            const auto ch = Key::keySymToUnicode(key.sym());
                            if (ch >= 0x20 && ch != 0x7f) {
                                if (auto *ic = keyEvent.inputContext()) {
                                    ic->commitString(
                                        Key::keySymToUTF8(key.sym()));
                                }
                                keyEvent.filterAndAccept();
                                return;
                            }
                        }
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
        readAsIni(config_, "conf/fcitx5-voiceinput.conf");
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
            CLOCK_MONOTONIC, 1000000, 100000,
            [this](EventSourceTime *, uint64_t) {
                if (!recording_) {
                    return false;
                }
                const auto elapsed =
                    (now(CLOCK_MONOTONIC) - startTime_) / 1000000;
                if (interrupted_ &&
                    elapsed > static_cast<uint64_t>(config_.maxSeconds.value() + 5)) {
                    showStatus(_("Voice backend timeout"));
                    cancelSession();
                    return false;
                }
                if (interrupted_) {
                    return false;
                }
                showStatus(recordingStatus());
                scheduleTick();
                return false;
            });
    }

    // Panel status line: a backend note (model download/loading) takes
    // precedence; otherwise show elapsed time + idle auto-stop countdown.
    std::string recordingStatus() const {
        if (!statusNote_.empty()) {
            return std::string("🎤 ") + statusNote_;
        }
        const auto elapsed =
            (now(CLOCK_MONOTONIC) - startTime_) / 1000000;
        return std::string("🎤 ") + _("Recording") + " " +
               std::to_string(elapsed) + "s (" +
               std::to_string(idleRemain_) + "s " + _("idle auto stop") + ")";
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
        showStatus(_("Recognizing..."));
        ::kill(child_, SIGINT);
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
        idleRemain_ = config_.maxSeconds.value();
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
        args.push_back("--max-ms");
        args.push_back(std::to_string(config_.maxSeconds.value() * 1000));
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
                   _("press trigger key to stop") + ")");
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
    //   TICK\n<sec>\n      seconds left before the idle auto-stop
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
                case PayloadKind::Tick:
                    idleRemain_ = std::atoi(line.c_str());
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
            } else if (line == "TICK") {
                expectPayload_ = true;
                payloadKind_ = PayloadKind::Tick;
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
            CLOCK_MONOTONIC, 2500000, 100000,
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
    std::unique_ptr<HandlerTableEntry<EventHandler>> keyHandler_;
    std::unique_ptr<HandlerTableEntry<EventHandler>> icCreatedHandler_;
    std::unique_ptr<HandlerTableEntry<EventHandler>> focusHandler_;
    SimpleAction action_;
    std::unique_ptr<EventSourceIO> ioSource_;
    std::unique_ptr<EventSourceTime> tickTimer_;
    std::unique_ptr<EventSourceTime> errorTimer_;
    ICUUID icUuid_{};
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
    enum class PayloadKind { Commit, Preedit, Error, Tick, Status };
    PayloadKind payloadKind_ = PayloadKind::Commit;
    int idleRemain_ = 30;
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
