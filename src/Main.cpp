// =============================================================================
// Main.cpp  —  application entry, object wiring, and the main window (all on the message thread)
// =============================================================================
//
// ROLE
//   This is the "composition root" for MiniDAWLab: it creates Transport, Session,
//   `RecorderService` (optional Phase 4 capture; not wired to UI yet), and `PlaybackEngine`,
//   connects them to juce::AudioDeviceManager, and shows the one window. It does
//   not implement playback math or file decoding — that lives in the engine / session / io layers.
//
// STARTUP ORDER (see initialise) — read before changing tear order
//   1. transport, session, recorderService, playbackEngine  (non-owning: engine points at
//      transport+session+recorder; recorder does not own Transport/Session)
//   2. deviceManager.initialiseWithDefaultDevices  —  **1 in / 2 out** when possible; falls back
//      to output-only (0, 2) with a log if the system cannot open an input (playback still works).
//   3. addAudioCallback(playbackEngine)  —  the engine will now receive audioDeviceIO* calls
//   4. main window with TransportControlsContent  —  UI can load files and send Transport commands
//
// SHUTDOWN ORDER (see shutdown) — JUCE: remove callback before closing device, then release objects
//   1. destroy main window
//   2. removeAudioCallback(playbackEngine)
//   3. closeAudioDevice
//   4. destroy playbackEngine, then recorderService, then session, transport
//
// THREADING
//   juce::JUCEApplication::initialise / shutdown and all UI (buttons, file chooser, paint) are
//   the [Message thread] in a desktop JUCE app. The audio path is only in PlaybackEngine’s
//   callback, which we do not call from here.
//
// NESTED TYPES
//   TransportControlsContent  —  buttons + timeline ruler + lane; FileChooser → read playhead, Session add-clip.
//   MainWindow  —  juce::DocumentWindow shell around the content.
//
// Method bodies in this file add plain-language notes next to start/stop order and the async
// file dialog path so the composition root is navigable, not just listed.
// `TransportControlsContent` also hosts the recording coordinator (`numpadRecordToggled`); the
// **shortcut** to invoke it is handled only in `MainWindow` as a single `KeyListener` on the
// window (JUCE key dispatch walks from focus → parent; content-only listeners are not always hit).
// =============================================================================

#include <JuceHeader.h>
#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "domain/Session.h"
#include "domain/AudioClip.h"
#include "engine/CountInClickOutput.h"
#include "engine/PlaybackEngine.h"
#include "engine/RecorderService.h"
#include "io/AudioFileLoader.h"
#include "io/MonoWavFileWriter.h"
#include "transport/Transport.h"
#include "ui/TimelineRulerView.h"
#include "ui/TimelineViewportModel.h"
#include "ui/TrackLanesView.h"
#include "ui/InspectorView.h"
#include "audio/AudioDeviceInfo.h"
#include "audio/LatencySettingsStore.h"
#include "ui/LatencySettingsView.h"
#include "io/ProjectAudioImport.h"

#include <algorithm>
#include <memory>
#include <optional>
#include <vector>

namespace
{
    // Temporary: show last key in a small local label (transport row). Leave `false` in normal use.
    constexpr bool kShowKeyDiagnostic = false;

    // Temporary: log + on-screen line for keys reaching MainWindow::routeShortcut. Leave `false` in
    // normal use (no extra layout row; `if constexpr` strips the UI).
    constexpr bool kShowShortcutDiagnostics = false;

    [[nodiscard]] juce::String hex8(const juce::uint32 x)
    {
        return juce::String::toHexString(x).toUpperCase();
    }

    // [ShortcutDiag] Lines are parseable tokens for correlating WM/JUCE conversions with matchers.
    void logShortcutRouterKey(const juce::KeyPress& key)
    {
        if (!kShowShortcutDiagnostics)
        {
            return;
        }
        const int kc = key.getKeyCode();
        const int lowWord = kc & 0xffff;
        const juce_wchar tc = key.getTextCharacter();
        const juce::uint32 kcU = static_cast<juce::uint32>(kc);
        const juce::uint32 tcU = static_cast<juce::uint32>(static_cast<juce::uint16>(tc));
        const juce::uint32 lowU = static_cast<juce::uint32>(lowWord) & 0xffffu;

        const juce::ModifierKeys mods = key.getModifiers();
        const juce::uint32 modRaw = static_cast<juce::uint32>(mods.getRawFlags());

        const int canonNp1 = juce::KeyPress::numberPad1;
        const int canonMul = juce::KeyPress::numberPadMultiply;
        const juce::uint32 canonNp1U = static_cast<juce::uint32>(canonNp1);
        const juce::uint32 canonMulU = static_cast<juce::uint32>(canonMul);

        juce::String msg;
        msg += "[ShortcutDiag] ";
        msg += "keyCode=";
        msg += juce::String(kc);
        msg += " (0x";
        msg += hex8(kcU);
        msg += ") lowWord=";
        msg += juce::String(lowWord);
        msg += " (0x";
        msg += hex8(lowU);
        msg += ") textChar=";
        msg += juce::String(static_cast<int>(tcU & 0xffffu));
        msg += " (0x";
        msg += hex8(tcU);
        msg += ") np1Canon=";
        msg += juce::String(canonNp1);
        msg += " (0x";
        msg += hex8(canonNp1U);
        msg += ") mulCanon=";
        msg += juce::String(canonMul);
        msg += " (0x";
        msg += hex8(canonMulU);
        msg += ") modShift=";
        msg += mods.isShiftDown() ? juce::String("Y") : juce::String("n");
        msg += " modCtrl=";
        msg += mods.isCtrlDown() ? juce::String("Y") : juce::String("n");
        msg += " modAlt=";
        msg += mods.isAltDown() ? juce::String("Y") : juce::String("n");
        msg += " modCmd=";
        msg += mods.isCommandDown() ? juce::String("Y") : juce::String("n");
        msg += " modRaw=0x";
        msg += hex8(modRaw);
        msg += " desc=\"";
        msg += key.getTextDescription();
        msg += "\"";
        juce::Logger::writeToLog(msg);
    }

    // Single-line caption for temporary on-screen shortcut diagnostic (transport area).
    [[nodiscard]] juce::String makeShortcutDiagVisibleCaption(const juce::KeyPress& key)
    {
        const int kc = key.getKeyCode();
        const int lowWord = kc & 0xffff;
        const juce_wchar tc = key.getTextCharacter();
        const auto kcU = static_cast<juce::uint32>(kc);
        const auto tcU = static_cast<juce::uint32>(static_cast<juce::uint16>(tc));
        const auto lowU = static_cast<juce::uint32>(lowWord) & 0xffffu;

        juce::String cap;
        cap << "[ShortcutDiag ui] ";
        cap << "keyCode=" << juce::String(kc) << " (0x" << hex8(kcU) << ") ";
        cap << "lowWord=" << juce::String(lowWord) << " (0x" << hex8(lowU) << ") ";
        cap << "textChar=" << juce::String(static_cast<int>(tcU & 0xffffu)) << " (0x"
            << hex8(tcU) << ") ";
        cap << "desc=\"" << key.getTextDescription() << "\"";
        return cap;
    }

    // JUCE (Windows) uses e.g. VK_* | 0x10000 for numpad keys; `KeyPress::numberPadMultiply` matches
    // that, but some paths deliver VK_MULTIPLY (0x6A) without the high bit — match both.
    [[nodiscard]] bool isNumpadMultiplyKey(const juce::KeyPress& k) noexcept
    {
        if (k == juce::KeyPress::numberPadMultiply)
        {
            return true;
        }
        constexpr int kVkMultiply = 0x6A; // winuser.h VK_MULTIPLY
        if ((k.getKeyCode() & 0xffff) == kVkMultiply)
        {
            return true;
        }
        return false;
    }

    // Jump-to-left-locator shortcut: matches numpad 1, top-row "1", and (Windows) numpad-with-NumLock-off
    // which is often delivered as End (VK_END 0x23), e.g. raw keyCode 0x10023, desc "end".
    // For now the dedicated End key (navigation cluster) also triggers the same action.
    [[nodiscard]] bool isNumpad1Shortcut(const juce::KeyPress& k) noexcept
    {
        if (k == juce::KeyPress::numberPad1)
        {
            return true;
        }

        const int canonNumpad1Code = juce::KeyPress::numberPad1;
        const int raw = k.getKeyCode();
        if (raw == canonNumpad1Code)
        {
            return true;
        }
        // Same virtual key as JUCE’s numpad 1, but high bits may differ (platform extended flags).
        if ((raw & 0xffff) == (canonNumpad1Code & 0xffff))
        {
            return true;
        }

        constexpr int kVkNumpad1 = 0x61; // winuser.h VK_NUMPAD1
        if (((raw & 0xffff) == kVkNumpad1) || raw == kVkNumpad1)
        {
            return true;
        }

        if (k.getTextCharacter() == juce_wchar{ '1' })
        {
            return true;
        }

        constexpr int kAsciiDigit1 = 49; // 0x31 main-row
        if (((raw & 0xffff) == kAsciiDigit1) || raw == kAsciiDigit1)
        {
            return true;
        }

        // NumLock off: numpad "1" is often VK_END (0x23); JUCE may use extended keyCode e.g. 0x10023.
        if (k == juce::KeyPress::endKey)
        {
            return true;
        }
        const int canonEnd = juce::KeyPress::endKey;
        if (raw == canonEnd)
        {
            return true;
        }
        if ((raw & 0xffff) == (canonEnd & 0xffff))
        {
            return true;
        }
        constexpr int kVkEnd = 0x23; // winuser.h VK_END
        if (((raw & 0xffff) == kVkEnd) || raw == kVkEnd)
        {
            return true;
        }
        constexpr int kObservedEndExtended = 0x10023; // e.g. numpad-1-as-End on Windows/JUCE (65571 decimal)
        if (raw == kObservedEndExtended)
        {
            return true;
        }

        return false;
    }

    // Numpad * or (for laptops) top-row * character (e.g. Shift+8) — not used for all keys with `*`
    // in text outside this narrow set (handled only at MainWindow).
    [[nodiscard]] bool isRecordToggleShortcut(const juce::KeyPress& k) noexcept
    {
        if (isNumpadMultiplyKey(k))
        {
            return true;
        }
        if (k.getTextCharacter() == juce_wchar{ '*' })
        {
            return true;
        }
        return false;
    }

    // Unmodified Space → play/pause toggle (not recording). Ctrl/Cmd/Alt+Space are ignored here.
    [[nodiscard]] bool isSpacePlayPauseShortcut(const juce::KeyPress& k) noexcept
    {
        const juce::ModifierKeys m = k.getModifiers();
        if (m.isCommandDown() || m.isCtrlDown() || m.isAltDown())
        {
            return false;
        }
        if (k.getKeyCode() == 32 || k.getTextCharacter() == juce_wchar{ ' ' })
        {
            return true;
        }
        return false;
    }

    [[nodiscard]] juce::File makeUniqueTakeWavInProjectAudioDir(const juce::File& audioDir)
    {
        const juce::String t = juce::Time::getCurrentTime().formatted("%Y%m%d_%H%M%S");
        juce::File f = audioDir.getChildFile("take_" + t + ".wav");
        if (!f.existsAsFile())
        {
            return f;
        }
        for (int i = 1; i < 10000; ++i)
        {
            f = audioDir.getChildFile("take_" + t + "_" + juce::String(i) + ".wav");
            if (!f.existsAsFile())
            {
                return f;
            }
        }
        return audioDir.getChildFile("take_" + t + "_9999.wav");
    }

    // Offline split (after cycle OD finalize): independent mono 24‑bit WAVs in `Audio/`.
    [[nodiscard]] juce::File makeUniqueCyclePassWavInProjectAudioDir(
        const juce::File& audioDir,
        const juce::String& batchStamp,
        const int sliceIndex)
    {
        juce::File f = audioDir.getChildFile(
            juce::String("cycle_pass_") + batchStamp + "_" + juce::String(sliceIndex) + ".wav");
        if (!f.existsAsFile())
        {
            return f;
        }
        for (int i = 1; i < 10000; ++i)
        {
            f = audioDir.getChildFile(juce::String("cycle_pass_") + batchStamp + "_"
                                      + juce::String(sliceIndex) + "_" + juce::String(i) + ".wav");
            if (!f.existsAsFile())
            {
                return f;
            }
        }
        return audioDir.getChildFile(
            juce::String("cycle_pass_") + batchStamp + "_" + juce::String(sliceIndex)
            + "_collision.wav");
    }

    // Defer-and-retry cleanup: the continuous WAV may briefly remain locked by Windows after the
    // recorder writer is closed (AV scan / indexer / kernel handle release latency). One synchronous
    // attempt + up to 1 s of message-thread retries (no audio-thread work, no sleeps), then
    // rename to a `_debug_cycle_continuous_…` sibling as last resort.
    class DeferredCycleMasterDeleter : private juce::Timer
    {
    public:
        static void schedule(juce::File f)
        {
            std::unique_ptr<DeferredCycleMasterDeleter> p(new DeferredCycleMasterDeleter(std::move(f)));
            p->startTimer(kRetryIntervalMs);
            liveInstances().push_back(std::move(p));
        }

    private:
        explicit DeferredCycleMasterDeleter(juce::File f) noexcept : file_(std::move(f)) {}

        void timerCallback() override
        {
            ++attempts_;
            if (!file_.existsAsFile())
            {
                retire();
                return;
            }
            if (file_.deleteFile())
            {
                juce::Logger::writeToLog(
                    "[Rec] cycle split: deleted continuous master WAV (deferred attempt "
                    + juce::String(attempts_) + ", " + file_.getFileName() + ").");
                retire();
                return;
            }
            if (attempts_ >= kMaxAttempts)
            {
                const juce::File dbg = file_.getSiblingFile(
                    "_debug_cycle_continuous_" + file_.getFileName());
                if (dbg.existsAsFile())
                {
                    (void)dbg.deleteFile();
                }
                const bool renamed = file_.moveFileTo(dbg);
                if (!renamed)
                {
                    juce::Logger::writeToLog(
                        "[Rec] cycle split WARNING: continuous master could not be deleted or renamed: "
                        + file_.getFullPathName());
                }
                else
                {
                    juce::Logger::writeToLog(
                        "[Rec] cycle split: continuous master kept as debug file "
                        + dbg.getFullPathName());
                }
                retire();
            }
        }

        void retire()
        {
            stopTimer();
            DeferredCycleMasterDeleter* self = this;
            juce::MessageManager::callAsync([self]() {
                auto& v = liveInstances();
                v.erase(std::remove_if(v.begin(), v.end(),
                                       [self](const std::unique_ptr<DeferredCycleMasterDeleter>& x) {
                                           return x.get() == self;
                                       }),
                        v.end());
            });
        }

        static std::vector<std::unique_ptr<DeferredCycleMasterDeleter>>& liveInstances() noexcept
        {
            static std::vector<std::unique_ptr<DeferredCycleMasterDeleter>> v;
            return v;
        }

        juce::File file_;
        int attempts_ = 0;
        static constexpr int kMaxAttempts = 20;     // 20 * 50 ms = 1 s
        static constexpr int kRetryIntervalMs = 50;
    };

    inline void scheduleCycleContinuousMasterCleanup(const juce::File& continuousWav)
    {
        if (continuousWav == juce::File() || !continuousWav.existsAsFile())
        {
            return;
        }
        if (continuousWav.deleteFile())
        {
            juce::Logger::writeToLog(
                "[Rec] cycle split: deleted continuous master WAV (" + continuousWav.getFileName()
                + ").");
            return;
        }
        DeferredCycleMasterDeleter::schedule(continuousWav);
    }

    // First-time Save As: abort with a non-empty message if we cannot write without clobbering.
    // `projectFile` = `<projectFolder>/<projectName>.dalproj`.
    [[nodiscard]] juce::String firstTimeSaveConflictMessage(
        const juce::File& projectFolder,
        const juce::File& projectFile)
    {
        if (projectFile.existsAsFile())
        {
            return "A project file already exists at:\n" + projectFile.getFullPathName()
                   + "\n\nChoose a different name or delete the existing file first.";
        }
        if (projectFolder.exists() && !projectFolder.isDirectory())
        {
            return "Cannot create the project folder; a file already exists at:\n"
                   + projectFolder.getFullPathName();
        }
        if (projectFolder.isDirectory())
        {
            juce::Array<juce::File> files;
            projectFolder.findChildFiles(files, juce::File::findFiles, false);
            for (const auto& c : files)
            {
                const juce::String n = c.getFileName();
                if (n.endsWithIgnoreCase(".dalproj") || n.endsWithIgnoreCase(".mdlproj"))
                {
                    if (!(c == projectFile))
                    {
                        return "The project folder already contains a different project file:\n"
                               + c.getFullPathName()
                               + "\n\nChoose a different folder or name.";
                    }
                }
            }
        }
        return {};
    }
} // namespace

namespace
{
class AudioSettingsDialogContent final : public juce::Component
{
public:
    AudioSettingsDialogContent(juce::AudioDeviceManager& dm,
                               LatencySettingsStore& latencyStore,
                               PlaybackEngine& playbackEngine)
        : selector_(dm, 0, 2, 2, 2, false, false, false, false)
        , latencyView_(latencyStore, playbackEngine)
    {
        addAndMakeVisible(selector_);
        addAndMakeVisible(latencyView_);
        setSize(640, 680);
    }

    void resized() override
    {
        constexpr int kGapBelowSelectorPx = 10;
        auto area = getLocalBounds();
        const int w = area.getWidth();
        const int topY = area.getY();

        // AudioDeviceSelectorComponent ends resized() by setSize(w, intrinsicHeight). Lay it out
        // with enough vertical slack first so internal controls measure correctly; then tighten
        // its bounds to that height so we do not leave a tall empty band above the latency panel.
        const int provisionalH = juce::jmax(1, area.getHeight() - kGapBelowSelectorPx);
        selector_.setBounds(area.getX(), topY, w, provisionalH);
        const int selectorH = juce::jmax(1, selector_.getHeight());
        selector_.setBounds(area.getX(), topY, w, selectorH);

        const int latencyY = topY + selectorH + kGapBelowSelectorPx;
        const int latencyH = juce::jmax(1, area.getBottom() - latencyY);
        latencyView_.setBounds(area.getX(), latencyY, w, latencyH);
    }

    [[nodiscard]] LatencySettingsView& getLatencyPane() noexcept { return latencyView_; }

private:
    juce::AudioDeviceSelectorComponent selector_;
    LatencySettingsView latencyView_;
};
} // namespace

// ---------------------------------------------------------------------------
// MiniDAWLabApplication — process-wide singleton, owns top-level subsystems
// ---------------------------------------------------------------------------
class MiniDAWLabApplication : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override { return "Danielssons Audio Lab"; }

    const juce::String getApplicationVersion() override
    {
        return { ProjectInfo::versionString };
    }

    bool moreThanOneInstanceAllowed() override { return true; }

    // [Message thread] Wires the stack described in the file header. jassert on empty init error
    // in debug: some audio output must open for playback; input is optional (see device init).
    void initialise(const juce::String&) override
    {
        // Domain objects first: the engine only holds references; safe because we create them
        // in dependency order and tear down in reverse in shutdown.
        transport = std::make_unique<Transport>();
        session = std::make_unique<Session>();
        // Phase 4: owned by the app; `PlaybackEngine` gets a non-owning pointer for input push
        // (only while `isRecording()`; see `PlaybackEngine` gate). Input channel count comes from
        // device init below.
        recorderService = std::make_unique<RecorderService>();
        countInOutput_ = std::make_unique<CountInClickOutput>();
        playbackEngine = std::make_unique<PlaybackEngine>(
            *transport, *session, recorderService.get(), countInOutput_.get());

        // JUCE: open audio before we register the engine. Restore saved `audio-device.xml` if present
        // (Stage 2); else pick defaults. Prefer **1 input, 2 outputs**; fall back to output-only.
        juce::String audioInitError;
        {
            const juce::File settingsFile = mini_daw::getAudioSettingsFile();
            const auto saved = mini_daw::loadAudioSettingsXmlIfAny(settingsFile);
            audioInitError = deviceManager.initialise(1, 2, saved.get(), true);
            if (audioInitError.isNotEmpty())
            {
                juce::Logger::writeToLog(
                    juce::String{"[Audio] 1-in/2-out (with saved state) not available: "} + audioInitError
                    + " — retrying output-only (0 in / 2 out). Input capture disabled until a suitable device exists.");
                audioInitError = deviceManager.initialiseWithDefaultDevices(0, 2);
            }
        }
        jassert(audioInitError.isEmpty());
        juce::ignoreUnused(audioInitError);
        juce::Logger::writeToLog(
            juce::String{"[Audio]\n"} + mini_daw::describeActiveAudioDeviceMultiLine(deviceManager));

        latencySettingsStore = std::make_unique<LatencySettingsStore>(
            deviceManager, mini_daw::getLatencySettingsFile());
        latencySettingsStore->loadFromFile();
        latencySettingsStore->refreshFromCurrentDevice();
        playbackEngine->setPlaybackOffsetSamples(latencySettingsStore->getCurrentPlaybackOffsetSamples());

        // After this line, the audio thread can call our PlaybackEngine; keep UI after so we do
        // not paint or load files before the device exists.
        deviceManager.addAudioCallback(playbackEngine.get());

        mainWindow = std::make_unique<MainWindow>(
            getApplicationName(),
            *transport,
            *session,
            deviceManager,
            *recorderService,
            *countInOutput_,
            *latencySettingsStore,
            *playbackEngine);
    }

    // [Message thread] Reverse of initialise; see file header.
    void shutdown() override
    {
        // Window first so no UI code runs while we tear down audio (matches JUCE’s typical order).
        mainWindow.reset();

        if (playbackEngine != nullptr)
        {
            // Unregister *before* closing the device so the engine is never called after destroy.
            deviceManager.removeAudioCallback(playbackEngine.get());
        }

        deviceManager.closeAudioDevice();

        // After callback removal, drop engine (it held `recorderService.get()` for audio thread) then
        // the recorder, then the rest. `RecorderService` is independent of Transport/Session.
        playbackEngine.reset();
        countInOutput_.reset();
        recorderService.reset();
        session.reset();
        transport.reset();
    }

    void systemRequestedQuit() override { quit(); }

private:
    // [Message thread only] Child component: file chooser, transport buttons, timeline ruler, lane.
    // Holds non-owning refs; MainWindow and application own lifetime. Add path: FileChooser
    // (async) → `Transport::readPlayheadSamplesForUi` once, then `addClipFromFileAtPlayhead`.
    class TransportControlsContent : public juce::Component,
                                     public juce::ChangeListener,
                                     private juce::Timer
    {
    public:
        TransportControlsContent(Transport& transportIn,
                                 Session& sessionIn,
                                 juce::AudioDeviceManager& deviceManagerIn,
                                 RecorderService& recorderIn,
                                 CountInClickOutput& countInClicksIn,
                                 LatencySettingsStore& latencyStoreIn,
                                 PlaybackEngine& playbackEngineIn)
            : transport(transportIn)
            , session(sessionIn)
            , deviceManager(deviceManagerIn)
            , recorder_(recorderIn)
            , countInClicks_(countInClicksIn)
            , latencyStore_(latencyStoreIn)
            , playbackEngine_(playbackEngineIn)
            , timelineViewport_()
            , rulerView(
                  sessionIn,
                  transportIn,
                  deviceManagerIn,
                  timelineViewport_,
                  [this]() {
                      return recorder_.isRecording() || isCountInActive();
                  })
            , trackLanesView(sessionIn, transportIn, timelineViewport_, deviceManagerIn, recorderIn, latencyStoreIn)
            , inspectorView_(sessionIn)
        {
            setWantsKeyboardFocus(true);
            timelineViewport_.setOnVisibleRangeChanged([this] {
                rulerView.repaint();
                trackLanesView.repaint();
            });
            addClipButton.onClick = [this] { addClipAtPlayheadClicked(); };
            addTrackButton.onClick = [this] {
                session.addTrack();
                syncViewportFromSession();
                trackLanesView.syncTracksFromSession();
                inspectorView_.refreshFromSession();
            };
            saveProjectButton.onClick = [this] { saveProjectClicked(); };
            loadProjectButton.onClick = [this] { loadProjectClicked(); };
            playPauseButton.onClick = [this] { togglePlayPauseFromUi(); };
            // Stop: "playback off + playhead to start" when idle; if recording, finalize/commit first
            // so RecorderService is never left recording while transport is Stopped.
            stopButton.onClick = [this] { stopOrSeekFromStopButton(); };
            audioSettingsButton.onClick = [this] { showAudioSettingsDialog(); };

            addAndMakeVisible(addClipButton);
            addAndMakeVisible(addTrackButton);
            addAndMakeVisible(saveProjectButton);
            addAndMakeVisible(loadProjectButton);
            addAndMakeVisible(playPauseButton);
            addAndMakeVisible(stopButton);
            addAndMakeVisible(audioSettingsButton);
            if (kShowKeyDiagnostic)
            {
                addAndMakeVisible(keyDiagLabel_);
                keyDiagLabel_.setFont(juce::FontOptions(11.0f));
                keyDiagLabel_.setJustificationType(juce::Justification::centredLeft);
                keyDiagLabel_.setText("key: —", juce::dontSendNotification);
            }
            if constexpr (kShowShortcutDiagnostics)
            {
                shortcutDiagLabel_ = std::make_unique<juce::Label>();
                shortcutDiagLabel_->setFont(juce::FontOptions(12.0f));
                shortcutDiagLabel_->setJustificationType(juce::Justification::centredLeft);
                shortcutDiagLabel_->setInterceptsMouseClicks(false, false);
                shortcutDiagLabel_->setMinimumHorizontalScale(1.0f);
                shortcutDiagLabel_->setText(
                    "[ShortcutDiag ui] (press a key — same source as routeShortcut logger line)",
                    juce::dontSendNotification);
                addAndMakeVisible(*shortcutDiagLabel_);
            }
            countInStatusLabel_.setFont(juce::FontOptions(12.0f));
            countInStatusLabel_.setJustificationType(juce::Justification::centredLeft);
            addAndMakeVisible(countInStatusLabel_);
            addAndMakeVisible(inspectorView_);
            addAndMakeVisible(rulerView);
            addAndMakeVisible(trackLanesView);
            deviceManager.addChangeListener(this);
            updatePlayPauseButtonFromTransport();
            startTimerHz(10);
            syncViewportFromSession();
        }

        ~TransportControlsContent() override
        {
            deviceManager.removeChangeListener(this);
            cancelCountIn();
            if (cycleRecordingWrapTimer_ != nullptr)
            {
                cycleRecordingWrapTimer_->stopTimer();
            }
        }

        // [Message thread] Invoked only from `MainWindow` shortcut router (not from child
        // `keyPressed` — avoids duplicate `numpadRecordToggled` on one physical keypress).
        void invokeRecordToggleFromWindowShortcut() { numpadRecordToggled(); }
        // [Message thread] Space: when recording, commit (source tag `space`); else same as Play/Pause.
        void invokePlayPauseToggleFromWindowShortcut()
        {
            if (recorder_.isRecording())
            {
                stopRecordingAndCommitFromUi("space");
                return;
            }
            togglePlayPauseTransportOnly();
        }
        void invokeJumpToLeftLocatorFromWindowShortcut()
        {
            if (recorder_.isRecording() || isCountInActive())
            {
                juce::Logger::writeToLog("[Shortcut] numpad1 ignored (recording or count-in)");
                return;
            }
            const std::int64_t L = session.getLeftLocatorSamples();
            const std::int64_t R = session.getRightLocatorSamples();
            if (R > L && R > 0)
            {
                transport.requestSeek(L);
                return;
            }
            juce::Logger::writeToLog("[Shortcut] numpad1 ignored: no valid locator range");
        }
        void setKeyDiagnosticLine(const juce::String& line)
        {
            if (kShowKeyDiagnostic)
            {
                keyDiagLabel_.setText(line, juce::dontSendNotification);
            }
        }

        void setShortcutDiagVisibleCaption(const juce::String& line)
        {
            if constexpr (kShowShortcutDiagnostics)
            {
                if (shortcutDiagLabel_)
                {
                    shortcutDiagLabel_->setText(line, juce::dontSendNotification);
                }
            }
            else
            {
                juce::ignoreUnused(line);
            }
        }

        // [Message thread] Layout: one row of buttons, fixed-height time ruler, then event lane.
        void resized() override
        {
            auto area = getLocalBounds().reduced(8);
            auto row = area.removeFromTop(32);
            if (kShowKeyDiagnostic)
            {
                keyDiagLabel_.setBounds(row.removeFromRight(300).reduced(2, 0));
            }
            constexpr int kCountInLabelWidth = 140;
            countInStatusLabel_.setBounds(row.removeFromRight(kCountInLabelWidth).reduced(4, 0));
            const int buttonWidth = juce::jmax(48, row.getWidth() / 7);

            addClipButton.setBounds(row.removeFromLeft(buttonWidth).reduced(2));
            addTrackButton.setBounds(row.removeFromLeft(buttonWidth).reduced(2));
            saveProjectButton.setBounds(row.removeFromLeft(buttonWidth).reduced(2));
            loadProjectButton.setBounds(row.removeFromLeft(buttonWidth).reduced(2));
            playPauseButton.setBounds(row.removeFromLeft(buttonWidth).reduced(2));
            stopButton.setBounds(row.removeFromLeft(buttonWidth).reduced(2));
            audioSettingsButton.setBounds(row.removeFromLeft(buttonWidth).reduced(2));
            if constexpr (kShowShortcutDiagnostics)
            {
                if (shortcutDiagLabel_ != nullptr)
                {
                    shortcutDiagLabel_->setBounds(area.removeFromTop(28));
                }
            }
            constexpr int kTimelineRulerHeight = 20;
            constexpr int kInspectorWidth = 90;
            auto inspectorCol = area.removeFromLeft(kInspectorWidth).reduced(0, 0);
            inspectorView_.setBounds(inspectorCol);
            auto timelineRow = area.removeFromTop(kTimelineRulerHeight);
            timelineRow.removeFromLeft(TrackLanesView::kTrackHeaderWidth);
            rulerView.setBounds(timelineRow);
            trackLanesView.setBounds(area);
        }

    private:
        struct CountInTimer final : juce::Timer
        {
            explicit CountInTimer(TransportControlsContent& o)
                : owner(o)
            {
            }
            void timerCallback() override { owner.onCountInTimerTick(); }
            TransportControlsContent& owner;
        };
        friend struct CountInTimer;

        struct CycleRecordingWrapTimer final : juce::Timer
        {
            explicit CycleRecordingWrapTimer(TransportControlsContent& o)
                : owner(o)
            {
            }
            void timerCallback() override { owner.onCycleRecordingWrapTimerTick(); }
            TransportControlsContent& owner;
        };
        friend struct CycleRecordingWrapTimer;

        void onCycleRecordingWrapTimerTick()
        {
            if (!cycleRecordingActive_ || !recorder_.isRecording())
            {
                return;
            }
            const std::uint32_t now = transport.readCycleWrapCountForUi();
            if (now != lastSeenWrapCount_)
            {
                numCompletedPasses_ += static_cast<int>(now - lastSeenWrapCount_);
                lastSeenWrapCount_ = now;
            }
        }

        void changeListenerCallback(juce::ChangeBroadcaster* source) override
        {
            juce::ignoreUnused(source);
            // Multi-line device detail is logged once at app init; on change, persist is best-effort.
            mini_daw::trySaveAudioDeviceState(deviceManager, mini_daw::getAudioSettingsFile());
            latencyStore_.refreshFromCurrentDevice();
            latencyStore_.save();
            playbackEngine_.setPlaybackOffsetSamples(latencyStore_.getCurrentPlaybackOffsetSamples());
            if (auto* lv = audioLatencySettingsWeak_.getComponent())
            {
                lv->syncFromStore();
            }
        }

        void showAudioSettingsDialog()
        {
            if (recorder_.isRecording() || isCountInActive())
            {
                juce::AlertWindow::showMessageBoxAsync(
                    juce::AlertWindow::WarningIcon,
                    "Audio Settings",
                    "Audio settings cannot be changed while recording or count-in is active.");
                return;
            }
            if (transport.readPlaybackIntentForUi() == PlaybackIntent::Playing)
            {
                transport.requestPlaybackIntent(PlaybackIntent::Stopped);
                updatePlayPauseButtonFromTransport();
            }
            auto* body = new AudioSettingsDialogContent(deviceManager, latencyStore_, playbackEngine_);
            audioLatencySettingsWeak_ = &body->getLatencyPane();
            body->getLatencyPane().syncFromStore();
            juce::DialogWindow::LaunchOptions opt;
            opt.content.setOwned(body);
            opt.dialogTitle = "Audio Settings";
            opt.dialogBackgroundColour
                = getLookAndFeel().findColour(juce::ResizableWindow::backgroundColourId);
            opt.componentToCentreAround = this;
            opt.escapeKeyTriggersCloseButton = true;
            opt.useNativeTitleBar = true;
            opt.resizable = true;
            opt.launchAsync();
        }

        void timerCallback() override
        {
            updatePlayPauseButtonFromTransport();
            inspectorView_.refreshFromSession();
        }

        // [Message thread] Transport intent: Playing → Paused, else (Stopped or Paused) → Playing.
        // If a take is in progress, the button stops/commits (never Paused+recording).
        void togglePlayPauseTransportOnly()
        {
            if (transport.readPlaybackIntentForUi() == PlaybackIntent::Playing)
            {
                transport.requestPlaybackIntent(PlaybackIntent::Paused);
            }
            else
            {
                transport.requestPlaybackIntent(PlaybackIntent::Playing);
            }
            updatePlayPauseButtonFromTransport();
        }

        void togglePlayPauseFromUi()
        {
            if (isCountInActive())
            {
                cancelCountIn();
                return;
            }
            if (recorder_.isRecording())
            {
                stopRecordingAndCommitFromUi("play_pause");
                return;
            }
            togglePlayPauseTransportOnly();
        }

        // [Message thread] Stop button: normal stop+seek, or end recording and commit (then seek 0).
        void stopOrSeekFromStopButton()
        {
            if (isCountInActive())
            {
                cancelCountIn();
                return;
            }
            if (recorder_.isRecording())
            {
                stopRecordingAndCommitFromUi("stop");
            }
            else
            {
                transport.requestPlaybackIntent(PlaybackIntent::Stopped);
            }
            transport.requestSeek(0);
            updatePlayPauseButtonFromTransport();
        }

        void updatePlayPauseButtonFromTransport()
        {
            const bool playing = transport.readPlaybackIntentForUi() == PlaybackIntent::Playing;
            const juce::String t = playing ? "Pause" : "Play";
            if (t != playPauseButton.getButtonText())
            {
                playPauseButton.setButtonText(t);
            }
        }

        // [Message thread] Seed default arrangement + samples-per-pixel once sample rate is known;
        // clamp the pan window to the current arrangement extent (when ruler width is known).
        void syncViewportFromSession()
        {
            juce::AudioIODevice* const dev = deviceManager.getCurrentAudioDevice();
            if (dev != nullptr)
            {
                const double sr = dev->getCurrentSampleRate();
                if (sr > 0.0)
                {
                    if (session.getStoredArrangementExtentSamples() == 0
                        && session.getContentEndSamples() == 0)
                    {
                        session.setArrangementExtentSamples(
                            (std::int64_t)std::llround(3600.0 * sr));
                    }
                    // Default: 10 pixels per second of session time; visible **length in samples** is
                    // derived as `round(rulerWidthPx * (sr/10))` and grows/shrinks with window width.
                    constexpr double kDefaultPixelsPerSecond = 10.0;
                    timelineViewport_.setSamplesPerPixelIfUnset(sr / kDefaultPixelsPerSecond);
                }
            }
            {
                const double rw = (double)rulerView.getWidth();
                if (rw > 0.0)
                {
                    timelineViewport_.clampToExtent(rw, session.getArrangementExtentSamples());
                }
            }
        }

        // [Message thread] Presents a native file dialog; on success, new clip is placed on the
        // **session** timeline at the current `Transport` playhead (read once, here, not on audio).
        void addClipAtPlayheadClicked()
        {
            if (!session.hasKnownProjectFile())
            {
                juce::AlertWindow::showMessageBoxAsync(
                    juce::AlertWindow::InfoIcon,
                    "Add clip",
                    "Save the project before importing audio.");
                return;
            }
            if (importInFlight_)
            {
                juce::Logger::writeToLog(
                    juce::String("[CLIMPORT] STAGE:ui:ignored_second_add_while_chooser_in_flight"));
                return;
            }
            importInFlight_ = true;

            const auto fileChooserFlags = juce::FileBrowserComponent::openMode
                                          | juce::FileBrowserComponent::canSelectFiles;

            auto chooser = std::make_shared<juce::FileChooser>(
                "Add audio at playhead",
                juce::File{},
                "*.wav;*.aif;*.aiff;*.flac;*.ogg;*.mp3");

            // JUCE: async dialog; the lambda runs on the *message* thread when the user dismisses
            // the picker. We record playhead and decode in this callback — the agreed “at add
            // time” read for placement (not the audio thread).
            chooser->launchAsync(fileChooserFlags, [this, chooser](const juce::FileChooser& fc) {
                juce::ignoreUnused(chooser);
                struct ClearImportInFlight
                {
                    bool& b;
                    explicit ClearImportInFlight(bool& ref) noexcept
                        : b(ref)
                    {
                    }
                    ~ClearImportInFlight() { b = false; }
                } clearImport{importInFlight_};

                const juce::File file = fc.getResult();
                if (!file.existsAsFile())
                {
                    // Cancel or empty selection — not an error, keep the current session.
                    juce::Logger::writeToLog(
                        juce::String("[CLIMPORT] STAGE:ui:chooser_dismissed_cancel_or_no_file"));
                    return;
                }

                juce::AudioIODevice* const device = deviceManager.getCurrentAudioDevice();
                if (device == nullptr)
                {
                    juce::Logger::writeToLog("[CLIMPORT] STAGE:ui:fail no_audio_device");
                    juce::AlertWindow::showMessageBoxAsync(
                        juce::AlertWindow::WarningIcon,
                        "Audio",
                        "No active audio device. Cannot validate sample rate for load.");
                    return;
                }

                juce::Logger::writeToLog(
                    juce::String("[CLIMPORT] STAGE:ui:chooser_ok file=") + file.getFullPathName());
                // Snapshot once: this value becomes `PlacedClip::startSampleOnTimeline` for the
                // new row (see Session / `PHASE_PLAN` add-at-playhead).
                const std::int64_t startSampleOnTimeline = transport.readPlayheadSamplesForUi();

                // Loader must match the *running* device rate (Phase 1 contract).
                const double sampleRate = device->getCurrentSampleRate();

                const juce::File audioDir = mini_daw::getProjectAudioDir(session.getCurrentProjectFolder());
                juce::File pathToUse;
                const juce::Result importRes
                    = mini_daw::importAudioIntoProjectAudioDir(file, audioDir, pathToUse);
                if (!importRes.wasOk())
                {
                    juce::Logger::writeToLog(
                        juce::String{"[CLIMPORT] STAGE:ui:copy_fail reason="} + importRes.getErrorMessage());
                    juce::AlertWindow::showMessageBoxAsync(
                        juce::AlertWindow::WarningIcon,
                        "Could not import audio",
                        importRes.getErrorMessage());
                    return;
                }
                if (file == pathToUse)
                {
                    juce::Logger::writeToLog(
                        juce::String{"[CLIMPORT] STAGE:ui:copy_ok dest=(already in Audio/) "}
                        + pathToUse.getFullPathName());
                }
                else
                {
                    juce::Logger::writeToLog(
                        juce::String{"[CLIMPORT] STAGE:ui:copy_ok dest="} + pathToUse.getFullPathName());
                }

                const juce::Result loadResult
                    = session.addClipFromFileAtPlayhead(pathToUse, sampleRate, startSampleOnTimeline);

                if (!loadResult.wasOk())
                {
                    juce::Logger::writeToLog(
                        juce::String("[CLIMPORT] STAGE:ui:session_add_failed err=") + loadResult.getErrorMessage());
                    juce::AlertWindow::showMessageBoxAsync(
                        juce::AlertWindow::WarningIcon,
                        "Could not open file",
                        loadResult.getErrorMessage());
                }
                else
                {
                    juce::Logger::writeToLog(
                        juce::String("[CLIMPORT] STAGE:ui:sync:begin file=") + pathToUse.getFileName());
                    // New **front** clip is on the active track; playhead/transport are unchanged.
                    syncViewportFromSession();
                    trackLanesView.syncTracksFromSession();
                    trackLanesView.repaint();
                    juce::Logger::writeToLog(
                        juce::String("[CLIMPORT] STAGE:ui:sync:done file=") + pathToUse.getFileName());
                }
            });
        }

        void saveProjectClicked()
        {
            juce::AudioIODevice* const device = deviceManager.getCurrentAudioDevice();
            if (device == nullptr)
            {
                juce::AlertWindow::showMessageBoxAsync(
                    juce::AlertWindow::WarningIcon,
                    "Save project",
                    "No active audio device; cannot include device sample rate in the project file.");
                return;
            }
            const double sampleRate = device->getCurrentSampleRate();

            // Normal save: no chooser. Explicit "Save As" / "New project" is deferred.
            if (session.hasKnownProjectFile())
            {
                const juce::Result r
                    = session.saveProjectToFile(transport, session.getCurrentProjectFile(), sampleRate);
                if (!r.wasOk())
                {
                    juce::AlertWindow::showMessageBoxAsync(
                        juce::AlertWindow::WarningIcon, "Save project", r.getErrorMessage());
                }
                return;
            }

            // First-time save: DAW-style `<Parent>/<ProjectName>/<ProjectName>.dalproj`
            const auto fileChooserFlags = juce::FileBrowserComponent::saveMode
                                          | juce::FileBrowserComponent::canSelectFiles;
            auto chooser = std::make_shared<juce::FileChooser>(
                "Save project as…",
                juce::File{},
                "*.dalproj");
            chooser->launchAsync(fileChooserFlags, [this, chooser, sampleRate](const juce::FileChooser& fc) {
                juce::ignoreUnused(chooser);
                juce::File userPick = fc.getResult();
                if (userPick.getFullPathName().isEmpty())
                {
                    return;
                }
                if (!userPick.hasFileExtension("dalproj"))
                {
                    userPick = userPick.getSiblingFile(
                        userPick.getFileNameWithoutExtension() + ".dalproj");
                }
                const juce::String projectName = userPick.getFileNameWithoutExtension();
                if (projectName.isEmpty())
                {
                    juce::AlertWindow::showMessageBoxAsync(
                        juce::AlertWindow::WarningIcon,
                        "Save project",
                        "Invalid project name.");
                    return;
                }
                const juce::File parentDir = userPick.getParentDirectory();
                const juce::File projectFolder = parentDir.getChildFile(projectName);
                const juce::File projectFile
                    = projectFolder.getChildFile(projectName + ".dalproj");
                {
                    const juce::String conflict = firstTimeSaveConflictMessage(projectFolder, projectFile);
                    if (conflict.isNotEmpty())
                    {
                        juce::AlertWindow::showMessageBoxAsync(
                            juce::AlertWindow::WarningIcon, "Save project", conflict);
                        return;
                    }
                }
                if (!projectFolder.isDirectory() && !projectFolder.createDirectory())
                {
                    juce::AlertWindow::showMessageBoxAsync(
                        juce::AlertWindow::WarningIcon,
                        "Save project",
                        "Could not create the project folder:\n" + projectFolder.getFullPathName());
                    return;
                }
                {
                    const juce::String conflict2
                        = firstTimeSaveConflictMessage(projectFolder, projectFile);
                    if (conflict2.isNotEmpty())
                    {
                        juce::AlertWindow::showMessageBoxAsync(
                            juce::AlertWindow::WarningIcon, "Save project", conflict2);
                        return;
                    }
                }
                const juce::Result r = session.saveProjectToFile(transport, projectFile, sampleRate);
                if (!r.wasOk())
                {
                    juce::AlertWindow::showMessageBoxAsync(
                        juce::AlertWindow::WarningIcon, "Save project", r.getErrorMessage());
                }
            });
        }

        void loadProjectClicked()
        {
            juce::AudioIODevice* const device = deviceManager.getCurrentAudioDevice();
            if (device == nullptr)
            {
                juce::AlertWindow::showMessageBoxAsync(
                    juce::AlertWindow::WarningIcon,
                    "Load project",
                    "No active audio device; cannot match sample rate to decode project clips.");
                return;
            }
            const double sampleRate = device->getCurrentSampleRate();

            const auto fileChooserFlags = juce::FileBrowserComponent::openMode
                                          | juce::FileBrowserComponent::canSelectFiles;
            auto chooser = std::make_shared<juce::FileChooser>(
                "Load project",
                juce::File{},
                "*.dalproj;*.mdlproj");
            chooser->launchAsync(fileChooserFlags, [this, chooser, sampleRate](const juce::FileChooser& fc) {
                juce::ignoreUnused(chooser);
                const juce::File f = fc.getResult();
                if (!f.existsAsFile())
                {
                    return;
                }
                juce::StringArray skipped;
                juce::String infoNote;
                const juce::Result r
                    = session.loadProjectFromFile(transport, f, sampleRate, skipped, infoNote);
                if (!r.wasOk())
                {
                    juce::AlertWindow::showMessageBoxAsync(
                        juce::AlertWindow::WarningIcon, "Load project", r.getErrorMessage());
                    return;
                }
                syncViewportFromSession();
                trackLanesView.syncTracksFromSession();
                inspectorView_.refreshFromSession();
                rulerView.repaint();
                trackLanesView.repaint();
                if (infoNote.isNotEmpty() || skipped.size() > 0)
                {
                    juce::String body;
                    if (infoNote.isNotEmpty())
                    {
                        body = infoNote;
                    }
                    if (skipped.size() > 0)
                    {
                        if (body.isNotEmpty())
                        {
                            body << "\n\n";
                        }
                        body << "Could not load " + juce::String(skipped.size())
                             + (skipped.size() == 1 ? " file:" : " files:") + "\n\n";
                        for (int i = 0; i < skipped.size(); ++i)
                        {
                            body << skipped[i] << (i < skipped.size() - 1 ? "\n" : "");
                        }
                    }
                    juce::AlertWindow::showMessageBoxAsync(
                        juce::AlertWindow::InfoIcon, "Load project (partial or note)", body);
                }
            });
        }

        // [Message thread] One path to end a take: stop transport first, then finalize, then commit.
        // Call only when `recorder_.isRecording()`; no-op if not.
        void stopRecordingAndCommitFromUi(const char* sourceContext)
        {
            if (!recorder_.isRecording())
            {
                return;
            }
            if (sourceContext != nullptr)
            {
                juce::Logger::writeToLog(
                    juce::String{"[Rec] stop/commit source="} + sourceContext);
            }

            const bool commitCycleTakes = cycleRecordingActive_;
            const TrackId cycleTrackId = cycleSessionTrackId_;
            const std::int64_t cycleLocL = cycleSessionLocL_;
            const std::int64_t cycleLocR = cycleSessionLocR_;
            const std::int64_t cycleStart = cycleSessionRecordingStartSample_;
            const double cycleSr = cycleSessionSampleRate_;

            transport.requestPlaybackIntent(PlaybackIntent::Stopped);
            updatePlayPauseButtonFromTransport();
            if (cycleRecordingWrapTimer_ != nullptr)
            {
                cycleRecordingWrapTimer_->stopTimer();
            }

            trackLanesView.clearCycleRecordingPreviewContext();
            cycleRecordingActive_ = false;

            const RecordedTakeResult r = recorder_.stopRecordingAndFinalize();

            if (!r.success)
            {
                numCompletedPasses_ = 0;
                lastSeenWrapCount_ = 0;
                juce::AlertWindow::showMessageBoxAsync(
                    juce::AlertWindow::WarningIcon,
                    "Recording",
                    r.errorMessage.isNotEmpty() ? r.errorMessage : "Could not finalize recording.");
                juce::Logger::writeToLog(
                    juce::String{"[Rec] stop/finalize failed: "} + r.errorMessage);
                return;
            }

            std::uint32_t wrapFinal = transport.readCycleWrapCountForUi();
            if (commitCycleTakes)
            {
                if (wrapFinal != lastSeenWrapCount_)
                {
                    numCompletedPasses_ += static_cast<int>(wrapFinal - lastSeenWrapCount_);
                    lastSeenWrapCount_ = wrapFinal;
                }
            }

            if (r.droppedSampleCount > 0)
            {
                const juce::String w = "Recording overrun: " + juce::String(r.droppedSampleCount)
                                        + (r.droppedSampleCount == 1 ? " sample was" : " samples were")
                                        + " replaced with silence.";
                juce::Logger::writeToLog(juce::String{"[Rec] "} + w);
                juce::AlertWindow::showMessageBoxAsync(
                    juce::AlertWindow::InfoIcon, "Recording", w);
            }

            if (commitCycleTakes)
            {
                std::unique_ptr<AudioClip> loadedClip;
                const auto loadClipResult
                    = AudioFileLoader::loadFromFile(r.takeFile, cycleSr, loadedClip);
                if (!loadClipResult.wasOk() || loadedClip == nullptr)
                {
                    numCompletedPasses_ = 0;
                    lastSeenWrapCount_ = 0;
                    juce::AlertWindow::showMessageBoxAsync(
                        juce::AlertWindow::WarningIcon,
                        "Session",
                        loadClipResult.getErrorMessage().isNotEmpty() ? loadClipResult.getErrorMessage()
                                                                     : "Could not decode recorded WAV.");
                    juce::Logger::writeToLog(
                        juce::String{"[Rec] cycle decode failed: "} + loadClipResult.getErrorMessage());
                    rulerView.repaint();
                    trackLanesView.repaint();
                    return;
                }

                const std::int64_t passLen = cycleLocR - cycleLocL;
                if (passLen <= 0 || cycleSr <= 0.0 || loadedClip->getNumChannels() < 1)
                {
                    numCompletedPasses_ = 0;
                    lastSeenWrapCount_ = 0;
                    juce::AlertWindow::showMessageBoxAsync(
                        juce::AlertWindow::WarningIcon,
                        "Session",
                        "Cycle recording commit failed: invalid loop range or decoded material.");
                    rulerView.repaint();
                    trackLanesView.repaint();
                    return;
                }

                juce::File audioDir = session.getCurrentProjectFolder().getChildFile("Audio");
                if (audioDir.getFullPathName().isEmpty())
                {
                    numCompletedPasses_ = 0;
                    lastSeenWrapCount_ = 0;
                    juce::AlertWindow::showMessageBoxAsync(
                        juce::AlertWindow::WarningIcon, "Session", "Could not resolve project Audio folder.");
                    rulerView.repaint();
                    trackLanesView.repaint();
                    return;
                }
                if (!audioDir.isDirectory() && !audioDir.createDirectory())
                {
                    numCompletedPasses_ = 0;
                    lastSeenWrapCount_ = 0;
                    juce::AlertWindow::showMessageBoxAsync(
                        juce::AlertWindow::WarningIcon,
                        "Session",
                        "Could not create project Audio folder: " + audioDir.getFullPathName());
                    rulerView.repaint();
                    trackLanesView.repaint();
                    return;
                }

                const float* const pcmLive = loadedClip->getAudio().getReadPointer(0);
                const auto decoded = static_cast<std::int64_t>(loadedClip->getNumSamples());
                const std::int64_t totalAvail
                    = juce::jmax<std::int64_t>(std::int64_t{ 0 }, juce::jmin(decoded, r.intendedSampleCount));

                if (totalAvail < 1)
                {
                    numCompletedPasses_ = 0;
                    lastSeenWrapCount_ = 0;
                    loadedClip.reset();
                    juce::AlertWindow::showMessageBoxAsync(
                        juce::AlertWindow::WarningIcon,
                        "Session",
                        "Cycle recording had no usable samples to commit.");
                    rulerView.repaint();
                    trackLanesView.repaint();
                    return;
                }

                std::vector<float> pcmStable(
                    static_cast<size_t>(
                        juce::jmax<std::int64_t>(std::int64_t{ 0 }, totalAvail)));
                for (std::int64_t i = 0; i < totalAvail; ++i)
                {
                    pcmStable[(size_t)i] = pcmLive[i];
                }
                loadedClip.reset();

                const float* const pcm = pcmStable.data();

                const juce::String batchStamp
                    = juce::Time::getCurrentTime().formatted("%Y%m%d_%H%M%S");
                bool allOk = true;
                const juce::File continuousMaster = r.takeFile;
                int sliceFileIndex = 0;

                const std::int64_t recordingPlacementOffsetSamples
                    = latencyStore_.getCurrentRecordingOffsetSamples();

                // `timelinePos` is where the slice is placed on the session timeline. Segment 0
                // sits at the actual recording start (which may be < L, in [L,R), or >= R), while
                // every subsequent wrapped pass sits at L by definition of cycle wrap-around.
                auto writeSliceCommit = [&](const std::int64_t offsetSamples,
                                            std::int64_t sliceLen,
                                            const std::int64_t timelinePosRaw) {
                    std::int64_t timelinePos = timelinePosRaw + recordingPlacementOffsetSamples;
                    std::int64_t wavOff = offsetSamples;
                    std::int64_t sliceUse = sliceLen;

                    if (timelinePos < 0)
                    {
                        const std::int64_t underflow = -timelinePos;
                        timelinePos = 0;
                        wavOff += underflow;
                        sliceUse -= underflow;
                    }

                    if (sliceUse <= 0 || wavOff < 0)
                    {
                        return;
                    }
                    if (wavOff + sliceUse > totalAvail)
                    {
                        sliceUse = totalAvail - wavOff;
                        if (sliceUse <= 0)
                        {
                            allOk = false;
                            return;
                        }
                    }
                    const auto sampleCount = static_cast<int>(sliceUse);
                    const juce::File sliceWav = makeUniqueCyclePassWavInProjectAudioDir(
                        audioDir, batchStamp, sliceFileIndex);

                    ++sliceFileIndex;

                    const juce::Result wrResult = MonoWavFileWriter::writeMono24BitWavSegment(
                        sliceWav, pcm + wavOff, sampleCount, cycleSr);

                    if (!wrResult.wasOk())
                    {
                        allOk = false;
                        juce::Logger::writeToLog(
                            "[Rec] cycle split write failed (" + sliceWav.getFileName()
                            + "): " + wrResult.getErrorMessage());
                        return;
                    }

                    const juce::Result ar = session.addRecordedTakeAtSample(
                        sliceWav,
                        cycleSr,
                        timelinePos,
                        cycleTrackId,
                        sliceUse);
                    if (!ar.wasOk())
                    {
                        allOk = false;
                        juce::Logger::writeToLog(
                            "[Rec] cycle addRecordedTake "
                            + sliceWav.getFileName() + ": " + ar.getErrorMessage());
                    }
                };

                // Variable first-segment placement: sample 0 of the recording corresponds to the
                // playhead at recording-start (cycleStart). Wrap math (R - cycleStart) yields
                // segment 0's natural length only if the recording actually crossed R from the
                // left (i.e. at least one wrap signalled by the audio thread).
                const std::int64_t actualStart = juce::jmax<std::int64_t>(std::int64_t{ 0 }, cycleStart);
                const int wraps = juce::jmax(0, numCompletedPasses_);

                if (actualStart >= cycleLocR || wraps <= 0)
                {
                    // Linear: a single segment placed at actualStart, full take length.
                    // Covers: start >= R (no wrap possible), or start < R but recording stopped
                    // before reaching R (no wrap occurred).
                    writeSliceCommit(std::int64_t{ 0 }, totalAvail, actualStart);
                }
                else
                {
                    // Segment 0 spans recording samples [0, R - actualStart) -> timeline [actualStart, R).
                    const std::int64_t firstSegLen
                        = juce::jmin(cycleLocR - actualStart, totalAvail);
                    writeSliceCommit(std::int64_t{ 0 }, firstSegLen, actualStart);

                    // Subsequent full passes (each of length passLen) are placed at L.
                    const std::int64_t remainingAfterFirst = totalAvail - firstSegLen;
                    const std::int64_t maxAdditionalFullsBySamples
                        = passLen > 0 ? remainingAfterFirst / passLen : std::int64_t{ 0 };
                    const int subsequentFull = static_cast<int>(
                        juce::jmin(static_cast<std::int64_t>(juce::jmax(0, wraps - 1)),
                                   maxAdditionalFullsBySamples));
                    for (int i = 0; i < subsequentFull; ++i)
                    {
                        const std::int64_t off
                            = firstSegLen + static_cast<std::int64_t>(i) * passLen;
                        writeSliceCommit(off, passLen, cycleLocL);
                    }

                    // Final partial (at L), if any samples remain after the last full pass.
                    const std::int64_t partialOffset
                        = firstSegLen + static_cast<std::int64_t>(subsequentFull) * passLen;
                    std::int64_t partialLen = totalAvail - partialOffset;
                    partialLen = juce::jlimit<std::int64_t>(std::int64_t{ 0 }, passLen, partialLen);
                    if (partialLen > 0)
                    {
                        writeSliceCommit(partialOffset, partialLen, cycleLocL);
                    }
                }

                numCompletedPasses_ = 0;
                lastSeenWrapCount_ = 0;

                if (!allOk)
                {
                    juce::AlertWindow::showMessageBoxAsync(
                        juce::AlertWindow::WarningIcon,
                        "Session",
                        "Some cycle takes could not be split or committed (see log).");
                }
                else
                {
                    syncViewportFromSession();
                    scheduleCycleContinuousMasterCleanup(continuousMaster);
                }
            }
            else
            {
                const std::int64_t recordingPlacementOffsetSamples
                    = latencyStore_.getCurrentRecordingOffsetSamples();
                // TODO: If committed start is negative, non-cycle could trim the head of the WAV
                // symmetrically to cycle; v1 only clamps timeline placement to 0.
                const std::int64_t committedStartSamples = juce::jmax<std::int64_t>(
                    std::int64_t{ 0 }, r.recordingStartSample + recordingPlacementOffsetSamples);

                const juce::Result ar = session.addRecordedTakeAtSample(
                    r.takeFile,
                    r.sampleRate,
                    committedStartSamples,
                    r.targetTrackId,
                    r.intendedSampleCount);
                if (!ar.wasOk())
                {
                    juce::AlertWindow::showMessageBoxAsync(
                        juce::AlertWindow::WarningIcon, "Session", ar.getErrorMessage());
                    juce::Logger::writeToLog(
                        juce::String{"[Rec] addRecordedTakeAtSample failed: "} + ar.getErrorMessage());
                }
                else
                {
                    syncViewportFromSession();
                }
            }

            rulerView.repaint();
            trackLanesView.repaint();
        }

        void numpadRecordToggled()
        {
            if (recorder_.isRecording())
            {
                stopRecordingAndCommitFromUi("numpad_*");
                return;
            }
            if (isCountInActive())
            {
                cancelCountIn();
                juce::Logger::writeToLog("[Rec] count-in cancelled (numpad_*)");
                return;
            }

            const TrackId armed = recorder_.getArmedTrackId();
            if (armed == kInvalidTrackId)
            {
                juce::AlertWindow::showMessageBoxAsync(
                    juce::AlertWindow::InfoIcon,
                    "Recording",
                    "Arm a track for recording (use the R control on a track header) first.");
                juce::Logger::writeToLog("[Rec] start blocked: no armed track");
                return;
            }
            if (!session.hasKnownProjectFile())
            {
                juce::AlertWindow::showMessageBoxAsync(
                    juce::AlertWindow::InfoIcon,
                    "Recording",
                    "Save the project before recording.");
                juce::Logger::writeToLog("[Rec] start blocked: project not saved to disk");
                return;
            }
            juce::File projectFile = session.getCurrentProjectFile();
            if (projectFile.getFullPathName().isEmpty())
            {
                juce::Logger::writeToLog("[Rec] start blocked: empty project file path");
                return;
            }
            juce::File audioDir = session.getCurrentProjectFolder().getChildFile("Audio");
            if (audioDir.getFullPathName().isEmpty())
            {
                juce::Logger::writeToLog("[Rec] start blocked: could not build Audio/ path");
                return;
            }
            if (!audioDir.isDirectory() && !audioDir.createDirectory())
            {
                juce::AlertWindow::showMessageBoxAsync(
                    juce::AlertWindow::WarningIcon,
                    "Recording",
                    "Could not create the project Audio folder: " + audioDir.getFullPathName());
                juce::Logger::writeToLog("[Rec] start blocked: createDirectory Audio/ failed");
                return;
            }
            const juce::File takeWav = makeUniqueTakeWavInProjectAudioDir(audioDir);
            juce::AudioIODevice* const dev = deviceManager.getCurrentAudioDevice();
            if (dev == nullptr)
            {
                juce::AlertWindow::showMessageBoxAsync(
                    juce::AlertWindow::WarningIcon, "Audio", "No active audio device.");
                return;
            }
            if (dev->getActiveInputChannels().countNumberOfSetBits() < 1)
            {
                juce::AlertWindow::showMessageBoxAsync(
                    juce::AlertWindow::WarningIcon,
                    "Audio",
                    "No input channel is active. Enable an input in your audio device, then try again.");
                juce::Logger::writeToLog("[Rec] start blocked: no active input channels");
                return;
            }
            const double sr = dev->getCurrentSampleRate();
            if (sr <= 0.0)
            {
                juce::AlertWindow::showMessageBoxAsync(
                    juce::AlertWindow::WarningIcon, "Audio", "Invalid device sample rate.");
                return;
            }

            cycleRecordingActive_ = false;
            const bool cycleOn = transport.readCycleEnabledForUi();
            const std::int64_t locL = session.getLeftLocatorSamples();
            const std::int64_t locR = session.getRightLocatorSamples();
            if (cycleOn && locR > locL && locR > 0)
            {
                cycleRecordingActive_ = true;
                cycleSessionLocL_ = locL;
                cycleSessionLocR_ = locR;
                numCompletedPasses_ = 0;
                // No presnap to L: cycle recording starts at the current playhead. Cycle wraps still
                // happen on the audio thread when playback crosses R from the left.
            }

            BeginRecordingRequest req;
            req.takeFile = takeWav;
            req.targetTrackId = armed;
            // Filled in `completeCountInAndStartRecording` from the playhead at the moment
            // `beginRecording` runs (after 8 count-in clicks + 375 ms silent tail).
            req.recordingStartSample = 0;
            req.sampleRate = sr;
            // Count-in: no `beginRecording` until clicks + post-click delay; Session not touched before.
            startCountInAfterValidation(std::move(req));
        }

        [[nodiscard]] bool isCountInActive() const noexcept { return pendingCountIn_.has_value(); }

        void cancelCountIn()
        {
            if (countInTimer_ != nullptr)
            {
                countInTimer_->stopTimer();
            }
            countInAwaitingPostClickDelay_ = false;
            pendingCountIn_.reset();
            countInClicks_.cancel();
            countInStatusLabel_.setText({}, juce::dontSendNotification);
            if (cycleRecordingActive_)
            {
                cycleRecordingActive_ = false;
                trackLanesView.clearCycleRecordingPreviewContext();
                if (cycleRecordingWrapTimer_ != nullptr)
                {
                    cycleRecordingWrapTimer_->stopTimer();
                }
                numCompletedPasses_ = 0;
                lastSeenWrapCount_ = 0;
            }
            juce::Logger::writeToLog("[Rec] count-in cancelled");
        }

        void onCountInTimerTick()
        {
            if (!pendingCountIn_.has_value())
            {
                if (countInTimer_ != nullptr)
                {
                    countInTimer_->stopTimer();
                }
                return;
            }
            // After the 8th click, one more interval of silence, then `beginRecording` (no 9th click).
            if (countInAwaitingPostClickDelay_)
            {
                countInAwaitingPostClickDelay_ = false;
                if (countInTimer_ != nullptr)
                {
                    countInTimer_->stopTimer();
                }
                completeCountInAndStartRecording();
                return;
            }
            // Cubase-like: tick tock tock tock | tick tock tock tock; 375 ms per step; first click
            // one interval after * so the keydown is not on-mic. Extra 375 ms after last click
            // before arming the recorder (reduces headphone bleed from the final click).
            static constexpr int kClicks = 8;
            ++countInBeat_;
            if (countInBeat_ < 1 || countInBeat_ > kClicks)
            {
                if (countInTimer_ != nullptr)
                {
                    countInTimer_->stopTimer();
                }
                return;
            }
            const bool useTick = (countInBeat_ == 1 || countInBeat_ == 5);
            if (useTick)
            {
                countInClicks_.triggerTick();
            }
            else
            {
                countInClicks_.triggerTock();
            }
            countInStatusLabel_.setText("Count-in: " + juce::String(countInBeat_) + "/"
                                        + juce::String(kClicks),
                                        juce::dontSendNotification);
            if (countInBeat_ == kClicks)
            {
                countInAwaitingPostClickDelay_ = true;
                countInStatusLabel_.setText("Get ready…", juce::dontSendNotification);
            }
        }

        void startCountInAfterValidation(BeginRecordingRequest&& req)
        {
            countInClicks_.prepare(req.sampleRate);
            pendingCountIn_ = std::move(req);
            countInBeat_ = 0;
            countInAwaitingPostClickDelay_ = false;
            if (countInTimer_ == nullptr)
            {
                countInTimer_ = std::make_unique<CountInTimer>(*this);
            }
            // First audible click is after kCountInIntervalMs, not on keydown.
            countInStatusLabel_.setText("Count-in…", juce::dontSendNotification);
            static constexpr int kCountInIntervalMs = 375;
            countInTimer_->startTimer(kCountInIntervalMs);
            juce::Logger::writeToLog(
                "[Rec] count-in started (8 clicks, 375 ms, +375 ms pre-roll before record)");
        }

        void completeCountInAndStartRecording()
        {
            if (!pendingCountIn_.has_value())
            {
                return;
            }
            BeginRecordingRequest req = *pendingCountIn_;
            pendingCountIn_.reset();
            countInStatusLabel_.setText({}, juce::dontSendNotification);

            const bool armedCycleSession = cycleRecordingActive_;
            // For both linear and cycle recording, the timeline start is wherever the playhead is
            // at the moment count-in completes. Cycle splitting (commit) reconstructs per-pass
            // placement from this real start, the locators, and the wrap count.
            req.recordingStartSample = transport.readPlayheadSamplesForUi();
            if (armedCycleSession)
            {
                cycleSessionTrackId_ = req.targetTrackId;
                cycleSessionSampleRate_ = req.sampleRate;
                cycleSessionTakeFile_ = req.takeFile;
                cycleSessionRecordingStartSample_ = req.recordingStartSample;
            }

            if (!recorder_.beginRecording(req))
            {
                if (armedCycleSession)
                {
                    cycleRecordingActive_ = false;
                    trackLanesView.clearCycleRecordingPreviewContext();
                    if (cycleRecordingWrapTimer_ != nullptr)
                    {
                        cycleRecordingWrapTimer_->stopTimer();
                    }
                    numCompletedPasses_ = 0;
                    lastSeenWrapCount_ = 0;
                }
                juce::String err = recorder_.getLastError();
                if (err.isEmpty())
                {
                    err = "beginRecording failed";
                }
                juce::AlertWindow::showMessageBoxAsync(
                    juce::AlertWindow::WarningIcon, "Recording", err);
                juce::Logger::writeToLog(juce::String{"[Rec] beginRecording failed: "} + err);
                return;
            }

            if (armedCycleSession)
            {
                lastSeenWrapCount_ = transport.readCycleWrapCountForUi();
                numCompletedPasses_ = 0;
                trackLanesView.setCycleRecordingPreviewContext(
                    true,
                    cycleSessionLocL_,
                    cycleSessionLocR_,
                    cycleSessionRecordingStartSample_,
                    lastSeenWrapCount_);
                if (cycleRecordingWrapTimer_ == nullptr)
                {
                    cycleRecordingWrapTimer_ = std::make_unique<CycleRecordingWrapTimer>(*this);
                }
                cycleRecordingWrapTimer_->startTimerHz(50);
            }

            transport.requestPlaybackIntent(PlaybackIntent::Playing);
            updatePlayPauseButtonFromTransport();
        }

        Transport& transport;
        Session& session;
        juce::AudioDeviceManager& deviceManager;
        RecorderService& recorder_;
        CountInClickOutput& countInClicks_;
        LatencySettingsStore& latencyStore_;
        PlaybackEngine& playbackEngine_;

        /// When Audio Settings is open; auto-clears when the dialog-owned view is destroyed.
        juce::Component::SafePointer<LatencySettingsView> audioLatencySettingsWeak_;
        std::optional<BeginRecordingRequest> pendingCountIn_;
        int countInBeat_ = 0;
        /// True after the 8th click: next timer fire ends count-in and starts `beginRecording`.
        bool countInAwaitingPostClickDelay_ = false;
        std::unique_ptr<CountInTimer> countInTimer_;
        /// Count-in / recording line (no always-visible audio device debug; use Audio...).
        juce::Label countInStatusLabel_;

        std::unique_ptr<CycleRecordingWrapTimer> cycleRecordingWrapTimer_;
        bool cycleRecordingActive_ = false;
        TrackId cycleSessionTrackId_ = kInvalidTrackId;
        std::int64_t cycleSessionLocL_ = 0;
        std::int64_t cycleSessionLocR_ = 0;
        /// Actual playhead sample at the moment cycle recording begins (after count-in). Used by
        /// the offline split to place segment 0 at its real start position rather than at L.
        std::int64_t cycleSessionRecordingStartSample_ = 0;
        double cycleSessionSampleRate_ = 0.0;
        juce::File cycleSessionTakeFile_;
        std::uint32_t lastSeenWrapCount_ = 0;
        int numCompletedPasses_ = 0;

        /// Set while a file chooser for Add clip is in flight; blocks overlapping Add clip clicks.
        bool importInFlight_ = false;

        juce::TextButton addClipButton{ "Add clip..." };
        juce::TextButton addTrackButton{ "Add track" };
        juce::TextButton saveProjectButton{ "Save Project..." };
        juce::TextButton loadProjectButton{ "Load Project..." };
        juce::TextButton playPauseButton{ "Play" };
        juce::TextButton stopButton{ "Stop" };
        juce::TextButton audioSettingsButton{ "Audio..." };
        juce::Label keyDiagLabel_;

        /// Temporary: last key seen by `MainWindow::routeShortcut` for numpad diagnostics (gated by flag).
        std::unique_ptr<juce::Label> shortcutDiagLabel_;

        /// UI-only: shared x–span for ruler and lanes; never stored in `Session` (see `PHASE_PLAN`).
        TimelineViewportModel timelineViewport_;
        TimelineRulerView rulerView;
        TrackLanesView trackLanesView;
        InspectorView inspectorView_;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TransportControlsContent)
    };

    // [Message thread] Top-level JUCE window: one `juce::KeyListener` on **this** so shortcuts are
    // always visited in `ComponentPeer::handleKeyPress` (including when focus is null → peer root).
    class MainWindow : public juce::DocumentWindow, public juce::KeyListener
    {
    public:
        MainWindow(const juce::String& name,
                   Transport& transport,
                   Session& session,
                   juce::AudioDeviceManager& deviceManager,
                   RecorderService& recorderService,
                   CountInClickOutput& countInClicks,
                   LatencySettingsStore& latencyStore,
                   PlaybackEngine& playbackEngine)
            : DocumentWindow(
                  name,
                  juce::Desktop::getInstance().getDefaultLookAndFeel().findColour(
                      juce::ResizableWindow::backgroundColourId),
                  DocumentWindow::allButtons)
        {
            setUsingNativeTitleBar(true);
            setContentOwned(
                new TransportControlsContent(transport,
                                             session,
                                             deviceManager,
                                             recorderService,
                                             countInClicks,
                                             latencyStore,
                                             playbackEngine),
                true);
            setResizable(true, true);
            setResizeLimits(320, 240, 10000, 10000);
            centreWithSize(640, 400);
            addKeyListener(this);
            if (juce::Component* c = getContentComponent())
            {
                c->setWantsKeyboardFocus(true);
            }
            juce::MessageManager::callAsync([this] {
                if (juce::Component* c = getContentComponent())
                {
                    c->grabKeyboardFocus();
                }
            });
            setVisible(true);
        }

        ~MainWindow() override { removeKeyListener(this); }

        // [Message thread] User clicked the window close: end the application.
        void closeButtonPressed() override
        {
            juce::JUCEApplication::getInstance()->systemRequestedQuit();
        }

        void activeWindowStatusChanged() override
        {
            juce::DocumentWindow::activeWindowStatusChanged();
            if (isActiveWindow())
            {
                juce::MessageManager::callAsync([this] {
                    if (juce::Component* c = getContentComponent())
                    {
                        c->grabKeyboardFocus();
                    }
                });
            }
        }

        bool keyPressed(const juce::KeyPress& key, juce::Component* originating) override
        {
            juce::ignoreUnused(originating);
            if (kShowKeyDiagnostic)
            {
                if (auto* tcc = dynamic_cast<TransportControlsContent*>(getContentComponent()))
                {
                    tcc->setKeyDiagnosticLine(
                        juce::String{"0x" } + juce::String::toHexString((juce::uint32)key.getKeyCode())
                        + " ch=0x" + juce::String::toHexString((juce::uint32)key.getTextCharacter()) + " "
                        + key.getTextDescription());
                }
            }
            return routeShortcut(key);
        }

    private:
        [[nodiscard]] bool routeShortcut(const juce::KeyPress& key)
        {
            logShortcutRouterKey(key);
            if constexpr (kShowShortcutDiagnostics)
            {
                if (auto* tcc = dynamic_cast<TransportControlsContent*>(getContentComponent()))
                {
                    tcc->setShortcutDiagVisibleCaption(makeShortcutDiagVisibleCaption(key));
                }
            }
            if (isRecordToggleShortcut(key))
            {
                if (auto* tcc = dynamic_cast<TransportControlsContent*>(getContentComponent()))
                {
                    tcc->invokeRecordToggleFromWindowShortcut();
                    juce::Logger::writeToLog(
                        juce::String{"[Shortcut] record toggle: "} + key.getTextDescription());
                    return true;
                }
                return false;
            }
            if (isNumpad1Shortcut(key))
            {
                if (auto* tcc = dynamic_cast<TransportControlsContent*>(getContentComponent()))
                {
                    tcc->invokeJumpToLeftLocatorFromWindowShortcut();
                    juce::Logger::writeToLog(juce::String{"[Shortcut] jump to left locator (numpad1 / top-row "
                                                          "1 / VK): "}
                                             + key.getTextDescription());
                    return true;
                }
                return false;
            }
            if (isSpacePlayPauseShortcut(key))
            {
                if (auto* tcc = dynamic_cast<TransportControlsContent*>(getContentComponent()))
                {
                    tcc->invokePlayPauseToggleFromWindowShortcut();
                    juce::Logger::writeToLog(
                        juce::String{"[Shortcut] play/pause: "} + key.getTextDescription());
                    return true;
                }
                return false;
            }
            return false;
        }

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainWindow)
    };

    std::unique_ptr<Transport> transport;
    std::unique_ptr<Session> session;
    // Phase 4: recording capture (not user-wired in this file yet); engine holds non-owning `get()`.
    std::unique_ptr<RecorderService> recorderService;
    /// Count-in metronome clicks to device only; coordinator state lives in `TransportControlsContent`.
    std::unique_ptr<CountInClickOutput> countInOutput_;
    std::unique_ptr<PlaybackEngine> playbackEngine;
    std::unique_ptr<LatencySettingsStore> latencySettingsStore;
    juce::AudioDeviceManager deviceManager;
    std::unique_ptr<MainWindow> mainWindow;
};

// JUCE: generate WinMain / main and the app singleton; DO NOT add another main().
START_JUCE_APPLICATION(MiniDAWLabApplication)
