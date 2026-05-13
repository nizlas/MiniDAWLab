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
//   1. transport, session, recorderService, pluginInsertHost, experimentalInstrumentHost, playbackEngine
//      (`playbackEngine` points at transport+session+recorder+pluginHost+experimentalInstrument;
//      recorder does not own Transport/Session)
//   2. deviceManager.initialiseWithDefaultDevices  —  **1 in / 2 out** when possible; falls back
//      to output-only (0, 2) with a log if the system cannot open an input (playback still works).
//   3. addAudioCallback(playbackEngine)  —  the engine will now receive audioDeviceIO* calls
//   4. main window with TransportControlsContent  —  UI can load files and send Transport commands
//
// SHUTDOWN ORDER (see shutdown) — JUCE: remove callback before closing device, then release objects
//   1. destroy main window
//   2. removeAudioCallback(playbackEngine)
//   3. closeAudioDevice
//   4. destroy playbackEngine, then experimentalInstrumentHost, then pluginInsertHost, then recorderService, then session, transport
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

#include <cmath>

#include "domain/Session.h"
#include "domain/SessionHistory.h"
#include "domain/AudioClip.h"
#include "domain/PlacedClip.h"
#include "engine/CountInClickOutput.h"
#include "engine/PlaybackEngine.h"
#include "engine/RecorderService.h"
#include "plugins/PluginInsertHost.h"
#include "plugins/ExperimentalInstrumentHost.h"
#include "instruments/InstrumentTrackController.h"
#include "plugins/InsertSlotId.h"
#include "plugins/PluginDiscovery.h"
#include "plugins/Vst3ChildProcessScan.h"
#include "io/AudioFileLoader.h"
#include "io/MonoWavFileWriter.h"
#include "transport/Transport.h"
#include "ui/TimelineRulerView.h"
#include "ui/TimelineViewportModel.h"
#include "ui/ClipWaveformView.h"
#include "ui/TrackLanesView.h"
#include "ui/TrackHeaderView.h"
#include "ui/CollapsibleSideStrip.h"
#include "ui/InspectorView.h"
#include "audio/AudioDeviceInfo.h"
#include "audio/LatencySettingsStore.h"
#include "ui/LatencySettingsView.h"
#include "ui/experimental/ExperimentalMidiEditorWindow.h"
#include "ui/TransportShortcutKeys.h"
#include "ui/TimelineClipEventChrome.h"
#include "ui/experimental/ExperimentalMidiPatternPlayer.h"
#include "io/ProjectAudioImport.h"
#include "io/AudioWaveformCache.h"
#include "io/ProjectFile.h"
#include "diagnostics/UndoDiagnosticConfig.h"
#include "diagnostics/UndoDiagnosticFileLog.h"

#include <algorithm>
#include <atomic>
#include <memory>
#include <optional>
#include <thread>
#include <type_traits>
#include <vector>

namespace
{
    // Temporary: show last key in a small local label (transport row). Leave `false` in normal use.
    constexpr bool kShowKeyDiagnostic = false;

    // Temporary: log + on-screen line for keys reaching MainWindow::routeShortcut. Leave `false` in
    // normal use (no extra layout row; `if constexpr` strips the UI).
    constexpr bool kShowShortcutDiagnostics = false;

    [[nodiscard]] juce::String instrumentVst3InsertBlockedMessage()
    {
        return juce::String{
            "This plugin appears to be an instrument. MiniDAWLab does not support instrument inserts on audio tracks yet."};
    }

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

    [[nodiscard]] juce::String undoDiagSnapPtr(const SessionSnapshot* p)
    {
        if (p == nullptr)
        {
            return "null";
        }
        return "0x" + juce::String::toHexString(reinterpret_cast<juce::pointer_sized_int>(p));
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

    /// MIDI runtime clip: same outer chrome sequence as placed audio clips (ClipWaveformView); label only inside.
    void paintRuntimeMidiClipEventBlock(juce::Graphics& g,
                                        juce::Rectangle<float> eb,
                                        bool selected)
    {
        using namespace mini_daw::timeline_clip_chrome;
        paintEventChromeBody(g, eb, midiLaneEventBodyFill());
        if (selected)
        {
            paintEventChromeSelectionOverlay(g, eb);
        }
        g.setColour(juce::Colour(0xff242a33));
        g.setFont(11.5f);
        g.drawFittedText(
            juce::String("MIDI 1"),
            clipEventLabelBounds(eb).toNearestInt(),
            juce::Justification::centredLeft,
            1);
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
    void initialise(const juce::String& commandLine) override
    {
        const juce::StringArray workerArgv = mini_daw::rawScanWorkerArgsFromCommandLine(commandLine);
        if (mini_daw::isVst3RawScanWorkerArgv(workerArgv))
        {
            vst3OopWorkerMode_ = true;
            const int rc = mini_daw::runVst3RawScanWorkerMain(workerArgv);
            setApplicationReturnValue(rc);
            quit();
            return;
        }

        // Domain objects first: the engine only holds references; safe because we create them
        // in dependency order and tear down in reverse in shutdown.
        transport = std::make_unique<Transport>();
        session = std::make_unique<Session>();
        // Phase 4: owned by the app; `PlaybackEngine` gets a non-owning pointer for input push
        // (only while `isRecording()`; see `PlaybackEngine` gate). Input channel count comes from
        // device init below.
        recorderService = std::make_unique<RecorderService>();
        countInOutput_ = std::make_unique<CountInClickOutput>();
        pluginInsertHost_ = std::make_unique<PluginInsertHost>();
        experimentalInstrumentHost_ = std::make_unique<ExperimentalInstrumentHost>();
        playbackEngine = std::make_unique<PlaybackEngine>(*transport,
                                                           *session,
                                                           recorderService.get(),
                                                           countInOutput_.get(),
                                                           pluginInsertHost_.get(),
                                                           experimentalInstrumentHost_.get());

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
            *pluginInsertHost_,
            *experimentalInstrumentHost_,
            deviceManager,
            *recorderService,
            *countInOutput_,
            *latencySettingsStore,
            *playbackEngine);
    }

    // [Message thread] Reverse of initialise; see file header.
    void shutdown() override
    {
        if (vst3OopWorkerMode_)
        {
            vst3OopWorkerMode_ = false;
            return;
        }

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
        experimentalInstrumentHost_.reset();
        pluginInsertHost_.reset();
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
                                     private juce::Timer,
                                     public collapsible_side_strip::Host
    {
    private:
        enum class InsertPickerMode
        {
            AddPre,
            AddPost,
        };

        static constexpr int kInspectorMaxW = 360;
        static constexpr int kInspectorDefaultW = 90;

        [[nodiscard]] int getSideStripWidth() const noexcept override { return inspectorCurrentWidth_; }

        void setSideStripWidth(int w) noexcept override { inspectorCurrentWidth_ = w; }

        [[nodiscard]] int getSideStripMaxWidth() const noexcept override { return kInspectorMaxW; }

        [[nodiscard]] int getSideStripDefaultWidth() const noexcept override { return kInspectorDefaultW; }

        void sideStripLayoutChanged() override { resized(); }

    public:
        TransportControlsContent(Transport& transportIn,
                                 Session& sessionIn,
                                 PluginInsertHost& pluginInsertHostIn,
                                 ExperimentalInstrumentHost& experimentalInstrumentHostIn,
                                 juce::AudioDeviceManager& deviceManagerIn,
                                 RecorderService& recorderIn,
                                 CountInClickOutput& countInClicksIn,
                                 LatencySettingsStore& latencyStoreIn,
                                 PlaybackEngine& playbackEngineIn)
            : transport(transportIn)
            , session(sessionIn)
            , pluginHost_(pluginInsertHostIn)
            , experimentalInstrumentHost_(experimentalInstrumentHostIn)
            , instrumentTrackController_(experimentalInstrumentHostIn)
            , deviceManager(deviceManagerIn)
            , recorder_(recorderIn)
            , countInClicks_(countInClicksIn)
            , latencyStore_(latencyStoreIn)
            , playbackEngine_(playbackEngineIn)
            , sessionHistory_{}
            , timelineViewport_()
            , audioWaveformCache_()
            , rulerView(
                  sessionIn,
                  transportIn,
                  deviceManagerIn,
                  timelineViewport_,
                  [this]() {
                      return recorder_.isRecording() || isCountInActive();
                  })
            , trackLanesView(
                  sessionIn,
                  transportIn,
                  timelineViewport_,
                  deviceManagerIn,
                  recorderIn,
                  latencyStoreIn,
                  audioWaveformCache_)
            , inspectorView_(sessionIn)
            , inspectorResizeSplitter_(*this)
            , inspectorCollapsedKnob_(*this)
            , instrumentMidiEventLane_(*this, instrumentTrackController_)
        {
            experimentalInstrumentHostIn.setDrumNamePhaseCAudioProbeShouldSkip([this] {
                return transport.readPlaybackIntentForUi() == PlaybackIntent::Playing || recorder_.isRecording()
                    || isCountInActive();
            });
            experimentalInstrumentHostIn.setOnPluginDrumNamesDiscovered(
                [this](const std::map<int, juce::String>& discovered) {
                    juce::PluginDescription d{};
                    const juce::String pluginId
                        = experimentalInstrumentHost_.getLastLoadedPluginDescription(d)
                              ? d.createIdentifierString()
                              : juce::String{};
                    instrumentTrackController_.mergeAutoPluginDrumLabels(discovered, pluginId);
                    ExperimentalInstrumentHost::appendInstrumentHostLogLine(
                        "drum-track: mergeAutoPluginDrumLabels source=afterEditorOpen keys="
                        + juce::String(static_cast<int>(discovered.size())));
                });
            trackLanesView.setStructuralTimelineEditBlockedPredicate([this]() {
                return recorder_.isRecording() || isCountInActive();
            });
            setWantsKeyboardFocus(true);
            audioWaveformCache_.setOnPyramidReady([this](const AudioClip*) { trackLanesView.repaint(); });
            {
                TrackHeaderCallbacks hdrCb;
                // Selecting the instrument row makes it the UI-active track and clears any audio
                // header active highlight (mutex; no `Session` change — see `setHeaderActiveSuppressProvider`).
                hdrCb.onActivateName = [this] {
                    instrumentTrackController_.setActive(true);
                    trackLanesView.repaint();
                };
                hdrCb.onTogglePower = [this]() -> bool {
                    instrumentTrackController_.setPowerOn(!instrumentTrackController_.isPowerOn());
                    instrumentTrackController_.setActive(true);
                    trackLanesView.repaint();
                    return true;
                };
                hdrCb.onToggleMute = [this] {
                    instrumentTrackController_.setMuted(!instrumentTrackController_.isMuted());
                    instrumentTrackController_.setActive(true);
                    trackLanesView.repaint();
                };
                hdrCb.onToggleArm = nullptr;
                hdrCb.onShowContextMenu =
                    [this](TrackHeaderView& header, const juce::MouseEvent&) {
                    instrumentTrackController_.setActive(true);
                    trackLanesView.repaint();

                    constexpr int kRescanInstrumentPluginDescription = 101;
                    const bool has = experimentalInstrumentHost_.hasInstrument();
                    const juce::File instBundle(experimentalInstrumentHost_.getLastLoadedVst3OriginalPath());
                    const bool canRescanDesc =
                        has && instBundle.exists() && !experimentalOopScanBusy_.load();

                    juce::PopupMenu menu;
                    juce::PopupMenu::Item rescanItem;
                    rescanItem.itemID = kRescanInstrumentPluginDescription;
                    rescanItem.text = "Rescan instrument plugin description";
                    rescanItem.isEnabled = canRescanDesc;
                    menu.addItem(rescanItem);

                    juce::Component::SafePointer<TransportControlsContent> safeThis(this);
                    menu.showMenuAsync(
                        juce::PopupMenu::Options().withTargetComponent(&header),
                        [safeThis, kRescanInstrumentPluginDescription](const int result) {
                            if (safeThis == nullptr || result == 0)
                            {
                                return;
                            }
                            if (result == kRescanInstrumentPluginDescription)
                            {
                                safeThis->runExperimentalInstrumentPluginDescriptionRescan();
                            }
                        });
                };
                hdrCb.onOpenInstrumentEditor = [this] { experimentalInstrumentHost_.openNativeEditor(); };

                instrumentTrackHeader_ = std::make_unique<TrackHeaderView>(
                    [this]() -> TrackHeaderModel {
                        TrackHeaderModel m;
                        m.name = juce::String("Groove Agent SE");
                        m.subtitle = {};
                        m.active = instrumentTrackController_.isActive();
                        m.armed = false;
                        m.muted = instrumentTrackController_.isMuted();
                        m.off = !instrumentTrackController_.isPowerOn();
                        m.powerInteractable = true;
                        m.muteInteractable = true;
                        m.armInteractable = false;
                        m.instrumentEditorAvailable = experimentalInstrumentHost_.hasInstrument();
                        return m;
                    },
                    std::move(hdrCb),
                    kInvalidTrackId,
                    std::nullopt);
            }
            timelineViewport_.setOnVisibleRangeChanged([this] {
                rulerView.repaint();
                trackLanesView.repaint();
                repaintInstrumentTrackRow();
            });
            addClipButton.onClick = [this] { addClipAtPlayheadClicked(); };
            addTrackButton.onClick = [this] {
                juce::PopupMenu menu;
                menu.addItem(1, "Add Audio Track");
                juce::PopupMenu instrMenu;
                instrMenu.addItem(100, "Groove Agent SE");
                instrMenu.addItem(
                    juce::PopupMenu::Item("HALion Sonic (not validated yet)").setEnabled(false));
                menu.addSubMenu("Add Instrument Track", instrMenu);
                juce::Component::SafePointer<TransportControlsContent> safeThis(this);
                menu.showMenuAsync(
                    juce::PopupMenu::Options().withTargetComponent(&addTrackButton),
                    [safeThis](int result) {
                        if (safeThis == nullptr || result == 0)
                        {
                            return;
                        }
                        if (result == 1)
                        {
                            safeThis->session.addTrack();
                            safeThis->syncViewportFromSession();
                            safeThis->trackLanesView.syncTracksFromSession();
                            safeThis->inspectorView_.refreshFromSession();
                            return;
                        }
                        if (result == 100)
                        {
                            safeThis->onAddGrooveAgentInstrumentTrackFromMenu();
                        }
                    });
            };
            saveProjectButton.onClick = [this] { saveProjectClicked(); };
            loadProjectButton.onClick = [this] { loadProjectClicked(); };
            playPauseButton.onClick = [this] { togglePlayPauseFromUi(); };
            // Stop: "playback off + playhead to start" when idle; if recording, finalize/commit first
            // so RecorderService is never left recording while transport is Stopped.
            stopButton.onClick = [this] { stopOrSeekFromStopButton(); };
            audioSettingsButton.onClick = [this] { showAudioSettingsDialog(); };
            helpButton.onClick = [this] { showHelpMenuPopup(); };

            constexpr int kEditToolRadioGroup = 90421;
            pointerToolButton_.setClickingTogglesState(true);
            pointerToolButton_.setToggleState(true, juce::dontSendNotification);
            pointerToolButton_.setRadioGroupId(kEditToolRadioGroup);
            splitToolButton_.setClickingTogglesState(true);
            splitToolButton_.setRadioGroupId(kEditToolRadioGroup);
            pointerToolButton_.onClick = [this] {
                currentEditTool_ = EditTool::Pointer;
                trackLanesView.repaint();
            };
            splitToolButton_.onClick = [this] {
                currentEditTool_ = EditTool::Split;
                trackLanesView.repaint();
            };

            addAndMakeVisible(addClipButton);
            addAndMakeVisible(addTrackButton);
            addAndMakeVisible(saveProjectButton);
            addAndMakeVisible(loadProjectButton);
            addAndMakeVisible(playPauseButton);
            addAndMakeVisible(stopButton);
            addAndMakeVisible(audioSettingsButton);
            addAndMakeVisible(helpButton);
            addAndMakeVisible(pointerToolButton_);
            addAndMakeVisible(splitToolButton_);
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
            addAndMakeVisible(inspectorResizeSplitter_);
            addAndMakeVisible(rulerView);
            addAndMakeVisible(trackLanesView);
            addAndMakeVisible(*instrumentTrackHeader_);
            addAndMakeVisible(instrumentMidiEventLane_);
            instrumentTrackHeader_->setVisible(false);
            instrumentMidiEventLane_.setVisible(false);
            refreshExperimentalInstrumentUi();
            addAndMakeVisible(inspectorCollapsedKnob_);
            inspectorCollapsedKnob_.setVisible(false);
            pluginHost_.setUndoRecorder(
                this,
                [](void* ctx, const juce::String& label, const PluginUndoStepSides& sides) {
                    auto* const self = static_cast<TransportControlsContent*>(ctx);
                    const std::shared_ptr<const SessionSnapshot> snap
                        = self->session.loadSessionSnapshotForAudioThread();
                    if (snap == nullptr)
                    {
                        return;
                    }
                    PluginUndoStepSides copy = sides;
                    self->sessionHistory_.record(label, snap, snap, std::move(copy), std::nullopt);
                });
            pluginHost_.setEditorShortcutCallbacks({
                [this] { invokeUndoFromWindowShortcut(); },
                [this] { invokeRedoFromWindowShortcut(); } });
            trackLanesView.setTrackHeaderPluginHost(
                { [this](const TrackId tid) {
                      showVst3PluginPickerForTrack(tid, InsertPickerMode::AddPost, this);
                  },
                  [this](const TrackId tid) { pluginHost_.openNativeEditor(tid); },
                  [this](const TrackId tid) { pluginHost_.openGenericParamsEditor(tid); },
                  [this](const TrackId tid) { pluginHost_.removePlugin(tid); } });
            inspectorView_.setInspectorPluginHost({
                [this](const TrackId tid) { return pluginHost_.hasAnyInsertOnTrack(tid); },
                [this](const TrackId tid) {
                    std::vector<InspectorInsertRow> rows;
                    rows.reserve(8);
                    for (const auto& rv : pluginHost_.getInsertRowsForTrack(tid))
                    {
                        InspectorInsertRow ir;
                        ir.slotId = rv.slotId;
                        ir.stage = rv.stage;
                        ir.displayName = rv.displayName;
                        rows.push_back(std::move(ir));
                    }
                    return rows;
                },
                [this](const TrackId tid, const InsertStage st) {
                    showVst3PluginPickerForTrack(
                        tid,
                        st == InsertStage::Pre ? InsertPickerMode::AddPre : InsertPickerMode::AddPost,
                        &inspectorView_);
                },
                [this](const TrackId tid, const InsertSlotId sid) { pluginHost_.openNativeEditor(tid, sid); },
                [this](const TrackId tid, const InsertSlotId sid) { pluginHost_.removeInsert(tid, sid); },
                [this](const TrackId tid, const InsertSlotId sid, const InsertStage st, const int gap) {
                    pluginHost_.moveInsertToStageAtGap(tid, sid, st, gap);
                },
                [this](const TrackId tid, const InsertSlotId sid, const int gapIndex) {
                    pluginHost_.reorderInsertWithinStage(tid, sid, gapIndex);
                } });
            trackLanesView.setOnDeleteTrackRequested([this](const TrackId tid) {
                if (recorder_.isRecording() || isCountInActive())
                {
                    return;
                }
                if (tid == kInvalidTrackId)
                {
                    return;
                }
                executeUndoableSessionEdit(
                    "Delete track",
                    [this, tid]() -> bool {
                        const std::shared_ptr<const SessionSnapshot> snap
                            = session.loadSessionSnapshotForAudioThread();
                        if (snap == nullptr || snap->findTrackIndexById(tid) < 0)
                        {
                            return false;
                        }
                        pluginHost_.evictPluginForTrackNoUndo(tid);
                        session.removeTrack(tid);
                        syncViewportFromSession();
                        trackLanesView.syncTracksFromSession();
                        rulerView.repaint();
                        trackLanesView.repaint();
                        inspectorView_.refreshFromSession();
                        return true;
                    });
            });
            trackLanesView.setOnUndoableClipMoveRequested(
                [this](const PlacedClipId clipId,
                       const std::int64_t newStart,
                       const std::optional<TrackId> destTrack) -> bool {
                    if (recorder_.isRecording() || isCountInActive())
                    {
                        return false;
                    }
                    if (clipId == kInvalidPlacedClipId)
                    {
                        return false;
                    }
                    bool committed = false;
                    executeUndoableSessionEdit(
                        "Move clip",
                        [this, clipId, newStart, destTrack, &committed]() -> bool {
                            const std::shared_ptr<const SessionSnapshot> snapBefore
                                = session.loadSessionSnapshotForAudioThread();
                            if (snapBefore == nullptr)
                            {
                                return false;
                            }
                            bool found = false;
                            for (int ti = 0; ti < snapBefore->getNumTracks(); ++ti)
                            {
                                const Track& tr = snapBefore->getTrack(ti);
                                for (int ci = 0; ci < tr.getNumPlacedClips(); ++ci)
                                {
                                    if (tr.getPlacedClip(ci).getId() == clipId)
                                    {
                                        found = true;
                                        break;
                                    }
                                }
                                if (found)
                                {
                                    break;
                                }
                            }
                            if (!found)
                            {
                                return false;
                            }
                            if (destTrack.has_value())
                            {
                                if (*destTrack == kInvalidTrackId
                                    || snapBefore->findTrackIndexById(*destTrack) < 0)
                                {
                                    return false;
                                }
                                session.moveClipToTrack(clipId, newStart, *destTrack);
                            }
                            else
                            {
                                session.moveClip(clipId, newStart);
                            }
                            const std::shared_ptr<const SessionSnapshot> snapAfter
                                = session.loadSessionSnapshotForAudioThread();
                            if (snapAfter == snapBefore)
                            {
                                return false;
                            }
                            syncViewportFromSession();
                            trackLanesView.syncTracksFromSession();
                            rulerView.repaint();
                            trackLanesView.repaint();
                            inspectorView_.refreshFromSession();
                            committed = true;
                            return true;
                        });
                    return committed;
                });
            trackLanesView.setOnUndoableClipTrimRequested(
                [this](const PlacedClipId clipId, const ClipTrimEdge edge, const std::int64_t newVal) -> bool {
                    if (recorder_.isRecording() || isCountInActive())
                    {
                        return false;
                    }
                    if (clipId == kInvalidPlacedClipId)
                    {
                        return false;
                    }
                    bool committed = false;
                    executeUndoableSessionEdit(
                        "Trim clip",
                        [this, clipId, edge, newVal, &committed]() -> bool {
                            const std::shared_ptr<const SessionSnapshot> snapBefore
                                = session.loadSessionSnapshotForAudioThread();
                            if (snapBefore == nullptr)
                            {
                                return false;
                            }
                            bool found = false;
                            for (int ti = 0; ti < snapBefore->getNumTracks(); ++ti)
                            {
                                const Track& tr = snapBefore->getTrack(ti);
                                for (int ci = 0; ci < tr.getNumPlacedClips(); ++ci)
                                {
                                    if (tr.getPlacedClip(ci).getId() == clipId)
                                    {
                                        found = true;
                                        break;
                                    }
                                }
                                if (found)
                                {
                                    break;
                                }
                            }
                            if (!found)
                            {
                                return false;
                            }
                            if (edge == ClipTrimEdge::Left)
                            {
                                session.setClipLeftEdgeTrim(clipId, newVal);
                            }
                            else
                            {
                                session.setClipRightEdgeVisibleLength(clipId, newVal);
                            }
                            const std::shared_ptr<const SessionSnapshot> snapAfter
                                = session.loadSessionSnapshotForAudioThread();
                            if (snapAfter == snapBefore)
                            {
                                return false;
                            }
                            syncViewportFromSession();
                            trackLanesView.syncTracksFromSession();
                            rulerView.repaint();
                            trackLanesView.repaint();
                            inspectorView_.refreshFromSession();
                            committed = true;
                            return true;
                        });
                    return committed;
                });
            trackLanesView.setActiveEditToolProvider([this]() { return currentEditTool_; });
            trackLanesView.setOnUndoableClipSplitRequested(
                [this](const PlacedClipId clipId,
                       const std::int64_t splitSample,
                       const bool clipWasSelected) {
                    if (recorder_.isRecording() || isCountInActive())
                    {
                        return;
                    }
                    if (clipId == kInvalidPlacedClipId)
                    {
                        return;
                    }
                    std::optional<std::pair<PlacedClipId, PlacedClipId>> splitIds;
                    executeUndoableSessionEdit(
                        "Split clip",
                        [this, clipId, splitSample, &splitIds]() -> bool {
                            const std::shared_ptr<const SessionSnapshot> snapBefore
                                = session.loadSessionSnapshotForAudioThread();
                            if (snapBefore == nullptr)
                            {
                                return false;
                            }
                            bool found = false;
                            for (int ti = 0; ti < snapBefore->getNumTracks(); ++ti)
                            {
                                const Track& tr = snapBefore->getTrack(ti);
                                for (int ci = 0; ci < tr.getNumPlacedClips(); ++ci)
                                {
                                    if (tr.getPlacedClip(ci).getId() == clipId)
                                    {
                                        found = true;
                                        break;
                                    }
                                }
                                if (found)
                                {
                                    break;
                                }
                            }
                            if (!found)
                            {
                                return false;
                            }
                            const auto maybe = session.splitClip(clipId, splitSample);
                            if (!maybe.has_value())
                            {
                                return false;
                            }
                            const std::shared_ptr<const SessionSnapshot> snapAfter
                                = session.loadSessionSnapshotForAudioThread();
                            if (snapAfter == snapBefore)
                            {
                                return false;
                            }
                            splitIds = *maybe;
                            syncViewportFromSession();
                            trackLanesView.syncTracksFromSession();
                            rulerView.repaint();
                            trackLanesView.repaint();
                            inspectorView_.refreshFromSession();
                            return true;
                        });
                    if (!clipWasSelected || !splitIds.has_value())
                    {
                        return;
                    }
                    const PlacedClipId rightId = splitIds->second;
                    const std::shared_ptr<const SessionSnapshot> snap
                        = session.loadSessionSnapshotForAudioThread();
                    if (snap == nullptr)
                    {
                        return;
                    }
                    for (int ti = 0; ti < snap->getNumTracks(); ++ti)
                    {
                        const Track& tr = snap->getTrack(ti);
                        for (int ci = 0; ci < tr.getNumPlacedClips(); ++ci)
                        {
                            if (tr.getPlacedClip(ci).getId() == rightId)
                            {
                                trackLanesView.selectPlacedClipOnTrack(tr.getId(), rightId);
                                return;
                            }
                        }
                    }
                });
            // UI-only mutex with the experimental instrument header. Audio headers paint inactive
            // when the instrument row is the UI-active row; clicking any audio header clears it.
            // No `Session` change — `Session::activeTrackId_` semantics for Add Clip etc. unchanged.
            trackLanesView.setHeaderActiveSuppressProvider(
                [this] { return instrumentTrackController_.isActive(); });
            trackLanesView.setOnAudioHeaderActivated(
                [this] { instrumentTrackController_.setActive(false); });
            deviceManager.addChangeListener(this);
            updatePlayPauseButtonFromTransport();
            startTimerHz(10);
            syncViewportFromSession();
            syncInstrumentClipTimelineFromDevice();
            playbackEngine_.setInstrumentTrackController(&instrumentTrackController_);
        }

        ~TransportControlsContent() override
        {
            audioWaveformCache_.setOnPyramidReady({});
            deviceManager.removeChangeListener(this);
            cancelCountIn();
            experimentalMidiEditorWindow_.reset();
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

        /// I3h: same transport / seek / cycle / record entry points as the main window, for the
        /// experimental MIDI editor (toolbar + piano-roll ruler). Does not duplicate transport state.
        [[nodiscard]] ExperimentalMidiTransportCommands makeMidiEditorTransportCommands()
        {
            ExperimentalMidiTransportCommands c;
            c.transport = &transport;
            c.onTogglePlayPause = [this] { invokePlayPauseToggleFromWindowShortcut(); };
            c.onStop = [this] { stopOrSeekFromStopButton(); };
            c.onToggleRecord = [this] { invokeRecordToggleFromWindowShortcut(); };
            c.onJumpToLeftLocator = [this] { invokeJumpToLeftLocatorFromWindowShortcut(); };
            c.onToggleCycle = [this] {
                if (recorder_.isRecording() || isCountInActive())
                {
                    juce::Logger::writeToLog("[Cycle] MIDI editor toggle ignored (recording or count-in)");
                    return;
                }
                transport.requestCycleEnabled(!transport.readCycleEnabledForUi());
                juce::Logger::writeToLog(juce::String{"[Cycle] "}
                                         + (transport.readCycleEnabledForUi() ? "on" : "off"));
            };
            c.isUiInputBlockedByRecording = [this]() {
                return recorder_.isRecording() || isCountInActive();
            };
            return c;
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
                if (experimentalMidiEditorWindow_ != nullptr)
                {
                    experimentalMidiEditorWindow_->notifyExternalTransportSeek(L);
                }
                return;
            }
            juce::Logger::writeToLog("[Shortcut] numpad1 ignored: no valid locator range");
        }

        void invokeDeleteSelectedPlacedClipFromWindowShortcut()
        {
            if (recorder_.isRecording() || isCountInActive())
            {
                return;
            }
            const std::optional<std::pair<TrackId, PlacedClipId>> sel
                = trackLanesView.getAggregatedSelectedClip();
            if (!sel.has_value())
            {
                return;
            }
            const TrackId tid = sel->first;
            const PlacedClipId pid = sel->second;
            const std::shared_ptr<const SessionSnapshot> snap
                = session.loadSessionSnapshotForAudioThread();
            const int ti = (snap != nullptr) ? snap->findTrackIndexById(tid) : -1;
            if (ti < 0)
            {
                return;
            }
            const Track& tr = snap->getTrack(ti);
            bool found = false;
            for (int i = 0; i < tr.getNumPlacedClips(); ++i)
            {
                if (tr.getPlacedClip(i).getId() == pid)
                {
                    found = true;
                    break;
                }
            }
            if (!found)
            {
                return;
            }
            executeUndoableSessionEdit(
                "Delete event",
                [this, tid, pid]() -> bool {
                    session.removePlacedClip(tid, pid);
                    trackLanesView.notifyPlacedClipRemoved(tid, pid);
                    syncViewportFromSession();
                    trackLanesView.syncTracksFromSession();
                    rulerView.repaint();
                    trackLanesView.repaint();
                    inspectorView_.refreshFromSession();
                    return true;
                });
        }

        void invokeCopySelectedClipFromWindowShortcut()
        {
            const std::optional<std::pair<TrackId, PlacedClipId>> sel
                = trackLanesView.getAggregatedSelectedClip();
            if (!sel.has_value())
            {
                return;
            }
            const std::shared_ptr<const SessionSnapshot> snap
                = session.loadSessionSnapshotForAudioThread();
            if (snap == nullptr)
            {
                return;
            }
            const int tIdx = snap->findTrackIndexById(sel->first);
            if (tIdx < 0)
            {
                return;
            }
            const Track& tr = snap->getTrack(tIdx);
            for (int i = 0; i < tr.getNumPlacedClips(); ++i)
            {
                const PlacedClip& p = tr.getPlacedClip(i);
                if (p.getId() != sel->second)
                {
                    continue;
                }
                InternalClipPasteboard pb;
                pb.material = p.getMaterial();
                pb.leftTrimSamples = p.getLeftTrimSamples();
                pb.visibleLengthSamples = p.getEffectiveLengthSamples();
                pb.materialWindowStartSamples = p.getMaterialWindowStartSamples();
                pb.materialWindowEndExclusiveSamples = p.getMaterialWindowEndExclusiveSamples();
                clipPasteboard_ = std::move(pb);
                return;
            }
        }

        void invokePasteClipFromWindowShortcut()
        {
            if (recorder_.isRecording() || isCountInActive())
            {
                return;
            }
            if (!clipPasteboard_.has_value())
            {
                return;
            }
            const TrackId target = session.getActiveTrackId();
            if (target == kInvalidTrackId)
            {
                return;
            }
            const std::shared_ptr<const SessionSnapshot> snap
                = session.loadSessionSnapshotForAudioThread();
            if (snap == nullptr || snap->findTrackIndexById(target) < 0)
            {
                return;
            }
            const InternalClipPasteboard pb = *clipPasteboard_;
            if (pb.material == nullptr || pb.visibleLengthSamples <= 0)
            {
                return;
            }
            executeUndoableSessionEdit(
                "Paste clip",
                [this, target, pb]() -> bool {
                    const juce::Result r = session.addPlacedClipFromExistingMaterial(
                        pb.material,
                        transport.readPlayheadSamplesForUi(),
                        pb.leftTrimSamples,
                        pb.visibleLengthSamples,
                        target,
                        pb.materialWindowStartSamples,
                        pb.materialWindowEndExclusiveSamples);
                    if (!r.wasOk())
                    {
                        return false;
                    }
                    syncViewportFromSession();
                    trackLanesView.syncTracksFromSession();
                    trackLanesView.selectFrontPlacedClipOnTrack(target);
                    rulerView.repaint();
                    trackLanesView.repaint();
                    inspectorView_.refreshFromSession();
                    return true;
                });
        }

        void invokeUndoFromWindowShortcut()
        {
            if constexpr (undo_diagnostic::kUndoDiag)
            {
                writeUndoDiagnosticLogLine(
                    "[UndoDiag] invokeUndoFromWindowShortcut entered undoSize="
                    + juce::String(sessionHistory_.undoStackSize()) + " redoSize="
                    + juce::String(sessionHistory_.redoStackSize()));
            }
            if (recorder_.isRecording() || isCountInActive())
            {
                if constexpr (undo_diagnostic::kUndoDiag)
                {
                    writeUndoDiagnosticLogLine(
                        "[UndoDiag] invokeUndo bail: recordingOrCountIn recording="
                        + juce::String(recorder_.isRecording() ? "Y" : "n")
                        + " countIn=" + juce::String(isCountInActive() ? "Y" : "n"));
                }
                return;
            }
            if (trackLanesView.isClipEditGestureInProgress())
            {
                if constexpr (undo_diagnostic::kUndoDiag)
                {
                    writeUndoDiagnosticLogLine("[UndoDiag] invokeUndo bail: gestureInProgress");
                }
                return;
            }
            pluginHost_.flushOpenEditorParameterUndoSteps();
            const std::optional<SessionHistoryRestoreBundle> bundle = sessionHistory_.popUndo();
            if (!bundle.has_value() || bundle->timelineSnapshot == nullptr)
            {
                if constexpr (undo_diagnostic::kUndoDiag)
                {
                    writeUndoDiagnosticLogLine(
                        "[UndoDiag] invokeUndo bail: emptyOrNullBundle hasValue="
                        + juce::String(bundle.has_value() ? "Y" : "n") + " timelineNull="
                        + juce::String(
                            (bundle.has_value() && bundle->timelineSnapshot == nullptr) ? "Y" : "n"));
                }
                return;
            }
            {
                const std::shared_ptr<const SessionSnapshot> live
                    = session.loadSessionSnapshotForAudioThread();
                const std::int64_t curL = live ? live->getLeftLocatorSamples() : 0;
                const std::int64_t curR = live ? live->getRightLocatorSamples() : 0;
                const std::shared_ptr<const SessionSnapshot> restoredWithLocators
                    = SessionSnapshot::withLocators(*bundle->timelineSnapshot, curL, curR);
                session.restoreSessionSnapshotForUndo(restoredWithLocators);
                if constexpr (undo_diagnostic::kUndoDiag)
                {
                    writeUndoDiagnosticLogLine(
                        "[UndoDiag] invokeUndo restored timeline="
                        + undoDiagSnapPtr(bundle->timelineSnapshot.get()) + " liveBeforeRestore="
                        + undoDiagSnapPtr(live.get()));
                }
            }
            if (bundle->pluginSides.has_value())
            {
                pluginHost_.importChain(bundle->pluginSides->trackId, bundle->pluginSides->before);
            }
            if (bundle->instrumentSides.has_value())
            {
                if constexpr (undo_diagnostic::kUndoDiag)
                {
                    if (experimentalMidiEditorWindow_ != nullptr)
                    {
                        const auto preId = experimentalMidiEditorWindow_->getBoundInstrumentClipId();
                        writeUndoDiagnosticLogLine(
                            "[UndoDiag] invokeUndo pre instrument apply storedEditorClipId="
                            + (preId.has_value() ? juce::String(static_cast<juce::int64>(*preId))
                                                 : juce::String("none")));
                    }
                }
                const std::vector<ProjectFileExperimentalInstrumentTrackV1>& mus
                    = bundle->isRedo ? bundle->instrumentSides->after : bundle->instrumentSides->before;
                instrumentTrackController_.applyExperimentalInstrumentMusicalUndoBlock(mus);
                rebindExperimentalMidiEditorAfterInstrumentMusicalUndo();
            }
            refreshAfterSessionSnapshotRestore();
            if constexpr (undo_diagnostic::kUndoDiag)
            {
                const auto liveNow = session.loadSessionSnapshotForAudioThread();
                writeUndoDiagnosticLogLine(
                    "[UndoDiag] invokeUndo complete liveNow=" + undoDiagSnapPtr(liveNow.get()) + " undoSize="
                    + juce::String(sessionHistory_.undoStackSize()) + " redoSize="
                    + juce::String(sessionHistory_.redoStackSize()));
            }
        }

        void invokeRedoFromWindowShortcut()
        {
            if constexpr (undo_diagnostic::kUndoDiag)
            {
                writeUndoDiagnosticLogLine(
                    "[UndoDiag] invokeRedoFromWindowShortcut entered undoSize="
                    + juce::String(sessionHistory_.undoStackSize()) + " redoSize="
                    + juce::String(sessionHistory_.redoStackSize()));
            }
            if (recorder_.isRecording() || isCountInActive())
            {
                if constexpr (undo_diagnostic::kUndoDiag)
                {
                    writeUndoDiagnosticLogLine(
                        "[UndoDiag] invokeRedo bail: recordingOrCountIn recording="
                        + juce::String(recorder_.isRecording() ? "Y" : "n")
                        + " countIn=" + juce::String(isCountInActive() ? "Y" : "n"));
                }
                return;
            }
            if (trackLanesView.isClipEditGestureInProgress())
            {
                if constexpr (undo_diagnostic::kUndoDiag)
                {
                    writeUndoDiagnosticLogLine("[UndoDiag] invokeRedo bail: gestureInProgress");
                }
                return;
            }
            const std::optional<SessionHistoryRestoreBundle> bundle = sessionHistory_.popRedo();
            if (!bundle.has_value() || bundle->timelineSnapshot == nullptr)
            {
                if constexpr (undo_diagnostic::kUndoDiag)
                {
                    writeUndoDiagnosticLogLine(
                        "[UndoDiag] invokeRedo bail: emptyOrNullBundle hasValue="
                        + juce::String(bundle.has_value() ? "Y" : "n") + " timelineNull="
                        + juce::String(
                            (bundle.has_value() && bundle->timelineSnapshot == nullptr) ? "Y" : "n"));
                }
                return;
            }
            {
                const std::shared_ptr<const SessionSnapshot> live
                    = session.loadSessionSnapshotForAudioThread();
                const std::int64_t curL = live ? live->getLeftLocatorSamples() : 0;
                const std::int64_t curR = live ? live->getRightLocatorSamples() : 0;
                const std::shared_ptr<const SessionSnapshot> restoredWithLocators
                    = SessionSnapshot::withLocators(*bundle->timelineSnapshot, curL, curR);
                session.restoreSessionSnapshotForUndo(restoredWithLocators);
                if constexpr (undo_diagnostic::kUndoDiag)
                {
                    writeUndoDiagnosticLogLine(
                        "[UndoDiag] invokeRedo restored timeline="
                        + undoDiagSnapPtr(bundle->timelineSnapshot.get()) + " liveBeforeRestore="
                        + undoDiagSnapPtr(live.get()));
                }
            }
            if (bundle->pluginSides.has_value())
            {
                pluginHost_.importChain(bundle->pluginSides->trackId, bundle->pluginSides->after);
            }
            if (bundle->instrumentSides.has_value())
            {
                if constexpr (undo_diagnostic::kUndoDiag)
                {
                    if (experimentalMidiEditorWindow_ != nullptr)
                    {
                        const auto preId = experimentalMidiEditorWindow_->getBoundInstrumentClipId();
                        writeUndoDiagnosticLogLine(
                            "[UndoDiag] invokeRedo pre instrument apply storedEditorClipId="
                            + (preId.has_value() ? juce::String(static_cast<juce::int64>(*preId))
                                                 : juce::String("none")));
                    }
                }
                const std::vector<ProjectFileExperimentalInstrumentTrackV1>& mus
                    = bundle->isRedo ? bundle->instrumentSides->after : bundle->instrumentSides->before;
                instrumentTrackController_.applyExperimentalInstrumentMusicalUndoBlock(mus);
                rebindExperimentalMidiEditorAfterInstrumentMusicalUndo();
            }
            refreshAfterSessionSnapshotRestore();
            if constexpr (undo_diagnostic::kUndoDiag)
            {
                const auto liveNow = session.loadSessionSnapshotForAudioThread();
                writeUndoDiagnosticLogLine(
                    "[UndoDiag] invokeRedo complete liveNow=" + undoDiagSnapPtr(liveNow.get()) + " undoSize="
                    + juce::String(sessionHistory_.undoStackSize()) + " redoSize="
                    + juce::String(sessionHistory_.redoStackSize()));
            }
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
            instrumentTrackController_.syncShellWithHostState();
            auto area = getLocalBounds().reduced(8);
            auto row = area.removeFromTop(32);
            if (kShowKeyDiagnostic)
            {
                keyDiagLabel_.setBounds(row.removeFromRight(300).reduced(2, 0));
            }
            constexpr int kCountInLabelWidth = 140;
            countInStatusLabel_.setBounds(row.removeFromRight(kCountInLabelWidth).reduced(4, 0));
            const int buttonWidth = juce::jmax(48, row.getWidth() / 8);

            addClipButton.setBounds(row.removeFromLeft(buttonWidth).reduced(2));
            addTrackButton.setBounds(row.removeFromLeft(buttonWidth).reduced(2));
            saveProjectButton.setBounds(row.removeFromLeft(buttonWidth).reduced(2));
            loadProjectButton.setBounds(row.removeFromLeft(buttonWidth).reduced(2));
            playPauseButton.setBounds(row.removeFromLeft(buttonWidth).reduced(2));
            stopButton.setBounds(row.removeFromLeft(buttonWidth).reduced(2));
            audioSettingsButton.setBounds(row.removeFromLeft(buttonWidth).reduced(2));
            helpButton.setBounds(row.removeFromLeft(buttonWidth).reduced(2));
            auto toolRow = area.removeFromTop(28);
            constexpr int kToolButtonW = 80;
            pointerToolButton_.setBounds(toolRow.removeFromLeft(kToolButtonW).reduced(2, 2));
            splitToolButton_.setBounds(toolRow.removeFromLeft(kToolButtonW).reduced(2, 2));
            if constexpr (kShowShortcutDiagnostics)
            {
                if (shortcutDiagLabel_ != nullptr)
                {
                    shortcutDiagLabel_->setBounds(area.removeFromTop(28));
                }
            }
            constexpr int kTimelineRulerHeight = 20;
            const int lanesBandTop = area.getY() + kTimelineRulerHeight;
            const int lanesBandHeight = juce::jmax(0, area.getHeight() - kTimelineRulerHeight);
            if (inspectorCurrentWidth_ > 0)
            {
                auto inspectorStrip = area.removeFromLeft(inspectorCurrentWidth_);
                const int splitW = juce::jmin(collapsible_side_strip::kSplitterWidth, inspectorStrip.getWidth());
                const int contentW = juce::jmax(0, inspectorStrip.getWidth() - splitW);
                inspectorView_.setBounds(
                    inspectorStrip.getX(),
                    inspectorStrip.getY(),
                    contentW,
                    inspectorStrip.getHeight());
                inspectorView_.setVisible(true);
                inspectorResizeSplitter_.setBounds(
                    inspectorStrip.getX() + contentW,
                    inspectorStrip.getY(),
                    splitW,
                    inspectorStrip.getHeight());
                inspectorResizeSplitter_.setVisible(true);
                inspectorCollapsedKnob_.setVisible(false);
            }
            else
            {
                if (area.getX() > 0)
                {
                    area.setLeft(0);
                }
                inspectorView_.setBounds(area.getX(), lanesBandTop, 0, lanesBandHeight);
                inspectorView_.setVisible(true);
                inspectorResizeSplitter_.setBounds(0, 0, 0, 0);
                inspectorResizeSplitter_.setVisible(false);
            }
            auto timelineRow = area.removeFromTop(kTimelineRulerHeight);
            timelineRow.removeFromLeft(TrackLanesView::kTrackHeaderWidth);    
            rulerView.setBounds(timelineRow);
            if (instrumentTrackController_.hasInstrumentTrack())
            {
                // Match audio lane row height: with N session tracks, each row in the band would be
                // H/(N+1) if the instrument row shares the band evenly — same height as each audio header.
                const int H = juce::jmax(0, area.getHeight());
                const int nAudio = session.getNumTracks();
                const int instH = (nAudio <= 0) ? H : juce::jmax(1, H / (nAudio + 1));
                auto instRow = area.removeFromBottom(instH);
                const int leftW = juce::jmin(TrackLanesView::kTrackHeaderWidth, instRow.getWidth());
                auto headerCol = instRow.removeFromLeft(leftW);
                if (instrumentTrackHeader_ != nullptr)
                {
                    instrumentTrackHeader_->setBounds(headerCol);
                    instrumentTrackHeader_->setVisible(true);
                    instrumentTrackHeader_->toFront(false);
                }
                instrumentMidiEventLane_.setBounds(instRow);
                instrumentMidiEventLane_.setVisible(true);
            }
            else
            {
                if (instrumentTrackHeader_ != nullptr)
                {
                    instrumentTrackHeader_->setVisible(false);
                }
                instrumentMidiEventLane_.setVisible(false);
            }
            trackLanesView.setBounds(area);
            if (inspectorCurrentWidth_ == 0)
            {
                const int knobX = trackLanesView.getBounds().getX();
                const int knobY = trackLanesView.getBounds().getCentreY()
                                  - collapsible_side_strip::kCollapsedKnobHeight / 2;
                inspectorCollapsedKnob_.setBounds(
                    knobX,
                    knobY,
                    collapsible_side_strip::kCollapsedKnobWidth,
                    collapsible_side_strip::kCollapsedKnobHeight);
                inspectorCollapsedKnob_.setVisible(true);
                inspectorCollapsedKnob_.toFront(false);
            }
        }

        void wireMidiEditorForOpenClip(InstrumentMidiClip* clip)
        {
            jassert(clip != nullptr);
            experimentalMidiEditorWindow_->bindExternalPattern(&clip->pattern,
                                                               clip,
                                                               &instrumentTrackController_,
                                                               &session,
                                                               &transport,
                                                               &deviceManager,
                                                               &timelineViewport_,
                                                               clip->name);
            experimentalMidiEditorWindow_->bindTransportCommands(makeMidiEditorTransportCommands());
            experimentalMidiEditorWindow_->setInstrumentMusicalUndoUi(
                [this](const juce::String& lab, std::function<bool()> m) {
                    executeUndoableInstrumentEdit(lab, std::move(m));
                },
                [this](const juce::String& lab, std::vector<ProjectFileExperimentalInstrumentTrackV1> before) {
                    commitInstrumentMusicalUndoPair(lab, std::move(before));
                },
                [this] { return instrumentTrackController_.buildExperimentalInstrumentMusicalUndoBlock(); },
                [this] { invokeUndoFromWindowShortcut(); },
                [this] { invokeRedoFromWindowShortcut(); });
            experimentalMidiEditorWindow_->syncInstrumentStateFromHost();
        }

        /// Clip-bound editor is invalid (e.g. clip removed by undo); same scratch wiring as toolbar scratch.
        void detachMidiEditorToScratchAfterMissingInstrumentClip(const juce::String& reasonForUser)
        {
            if (experimentalMidiEditorWindow_ == nullptr)
            {
                return;
            }
            experimentalMidiEditorWindow_->unbindExternalPattern();
            experimentalMidiEditorWindow_->bindTransportCommands(makeMidiEditorTransportCommands());
            experimentalMidiEditorWindow_->setInstrumentMusicalUndoUi(
                std::function<void(const juce::String&, std::function<bool()>)>{},
                std::function<void(const juce::String&, std::vector<ProjectFileExperimentalInstrumentTrackV1>)>{},
                std::function<std::vector<ProjectFileExperimentalInstrumentTrackV1>()>{},
                [this] { invokeUndoFromWindowShortcut(); },
                [this] { invokeRedoFromWindowShortcut(); });
            experimentalMidiEditorWindow_->syncInstrumentStateFromHost();
            if (reasonForUser.isNotEmpty())
            {
                juce::AlertWindow::showMessageBoxAsync(
                    juce::AlertWindow::InfoIcon, "MIDI editor", reasonForUser);
            }
        }

        void rebindExperimentalMidiEditorAfterInstrumentMusicalUndo()
        {
            if (experimentalMidiEditorWindow_ == nullptr)
            {
                return;
            }
            const std::optional<std::uint64_t> idOpt = experimentalMidiEditorWindow_->getBoundInstrumentClipId();
            if (!idOpt.has_value() || *idOpt == 0)
            {
                if constexpr (undo_diagnostic::kUndoDiag)
                {
                    writeUndoDiagnosticLogLine(
                        "[UndoDiag] rebindMidiAfterInstrumentUndo skip: no stored clip id");
                }
                return;
            }
            const InstrumentMidiClipId clipId = static_cast<InstrumentMidiClipId>(*idOpt);
            if constexpr (undo_diagnostic::kUndoDiag)
            {
                writeUndoDiagnosticLogLine("[UndoDiag] rebindMidiAfterInstrumentUndo clipId="
                                           + juce::String(static_cast<juce::int64>(clipId)));
            }
            InstrumentMidiClip* const clip = instrumentTrackController_.getClipById(clipId);
            if (clip == nullptr)
            {
                if constexpr (undo_diagnostic::kUndoDiag)
                {
                    writeUndoDiagnosticLogLine(
                        "[UndoDiag] rebindMidiAfterInstrumentUndo clip missing -> detach scratch");
                }
                detachMidiEditorToScratchAfterMissingInstrumentClip(
                    "The MIDI clip being edited is no longer available after undo.\n\n"
                    "The editor was switched to scratch mode.");
                return;
            }
            instrumentTrackController_.setSelectedClipId(clipId);
            wireMidiEditorForOpenClip(clip);
            if constexpr (undo_diagnostic::kUndoDiag)
            {
                writeUndoDiagnosticLogLine(
                    "[UndoDiag] rebindMidiAfterInstrumentUndo ok patternPtr="
                    + juce::String::formatted("%p", (void*)&clip->pattern) + " timelineNotes="
                    + juce::String((int)clip->pattern.timelineNotes.size()));
            }
        }

        void openMidiEditorForInstrumentClip(const InstrumentMidiClipId clipId)
        {
            InstrumentMidiClip* clip = instrumentTrackController_.getClipById(clipId);
            if (clip == nullptr)
            {
                return;
            }
            instrumentTrackController_.setSelectedClipId(clipId);
            if (experimentalMidiEditorWindow_ == nullptr)
            {
                experimentalMidiEditorWindow_
                    = std::make_unique<ExperimentalMidiEditorWindow>(experimentalInstrumentHost_);
            }
            wireMidiEditorForOpenClip(clip);
            experimentalMidiEditorWindow_->setVisible(true);
            experimentalMidiEditorWindow_->toFront(true);
        }

        [[nodiscard]] TimelineViewportModel& getTimelineViewport() noexcept { return timelineViewport_; }

        void syncInstrumentClipTimelineFromDevice() noexcept
        {
            double sr = 48000.0;
            if (juce::AudioIODevice* d = deviceManager.getCurrentAudioDevice())
            {
                const double r = d->getCurrentSampleRate();
                if (r > 0.0 && std::isfinite(r))
                {
                    sr = r;
                }
            }
            instrumentTrackController_.setTimelineSampleRate(sr);
        }

        private:
        void refreshExperimentalMidiEditorInstrumentUiIfOpen()
        {
            if (experimentalMidiEditorWindow_ != nullptr)
            {
                experimentalMidiEditorWindow_->syncInstrumentStateFromHost();
            }
        }

        /// Repaint sibling **TrackHeaderView** + MIDI lane (same row layout as `TrackLanesView`).
        void repaintInstrumentTrackRow()
        {
            if (instrumentTrackHeader_ != nullptr)
            {
                instrumentTrackHeader_->repaint();
            }
            instrumentMidiEventLane_.repaint();
        }

        /// MIDI clip lane only (same role as `ClipWaveformView`). Header is a sibling `TrackHeaderView`.
        struct InstrumentMidiEventLane final : public juce::Component,
                                               private juce::ChangeListener,
                                               private juce::Timer
        {
            static constexpr bool kLogInstrumentLane = false;

            InstrumentMidiEventLane(TransportControlsContent& ownerIn,
                                    InstrumentTrackController& controllerIn) noexcept
                : owner_(ownerIn)
                , controller_(controllerIn)
            {
                controller_.addChangeListener(this);
                startTimerHz(20);
            }

            ~InstrumentMidiEventLane() override
            {
                stopTimer();
                controller_.removeChangeListener(this);
            }

            void changeListenerCallback(juce::ChangeBroadcaster*) override
            {
                owner_.repaintInstrumentTrackRow();
                owner_.refreshExperimentalMidiEditorInstrumentUiIfOpen();
            }

            void timerCallback() override
            {
                if (owner_.transport.readPlaybackIntentForUi() == PlaybackIntent::Playing)
                {
                    repaint();
                }
            }

            void paint(juce::Graphics& g) override
            {
                const auto lane = getLocalBounds();

                const juce::Colour laneBg = getLookAndFeel().findColour(juce::ResizableWindow::backgroundColourId)
                                              .darker(0.2f);
                g.setColour(laneBg);
                g.fillRect(lane);
                g.setColour(laneBg.darker(0.12f));
                g.drawVerticalLine(lane.getX(), (float)lane.getY(), (float)lane.getBottom());

                const auto laneContent = getLaneContentBounds();
                if (laneContent.isEmpty())
                {
                    return;
                }

                for (const auto& up : controller_.getClips())
                {
                    const auto* c = up.get();
                    if (c == nullptr)
                    {
                        continue;
                    }
                    const auto eb = getEventBoundsForClip(*c, laneContent);
                    if (eb.isEmpty())
                    {
                        continue;
                    }
                    const bool sel = (c->id == controller_.getSelectedClipId());
                    if (kLogInstrumentLane)
                    {
                        juce::Logger::writeToLog("instrument-lane: paint clip id=" + juce::String((juce::int64)c->id)
                                                 + " selected=" + juce::String(sel ? "true" : "false") + " eventBounds="
                                                 + eb.toString());
                    }

                    paintRuntimeMidiClipEventBlock(g, eb.toFloat(), sel);
                }
            }

            void mouseDown(const juce::MouseEvent& e) override
            {
                const auto pos = e.getPosition();
                if (kLogInstrumentLane)
                {
                    juce::Logger::writeToLog("instrument-lane: mouseDown x=" + juce::String(pos.x) + " y="
                                             + juce::String(pos.y));
                }

                if (!getLocalBounds().contains(pos))
                {
                    return;
                }

                if (auto* clip = hitTestClipAtEvent(e.position))
                {
                    controller_.setSelectedClipId(clip->id);
                    if (kLogInstrumentLane)
                    {
                        juce::Logger::writeToLog("instrument-lane: hit clip id=" + juce::String((juce::int64)clip->id)
                                                 + " selected=true");
                    }
                }
                else
                {
                    controller_.clearClipSelection();
                    if (kLogInstrumentLane)
                    {
                        juce::Logger::writeToLog("instrument-lane: no hit");
                    }
                }

                repaint();
            }

            void mouseDoubleClick(const juce::MouseEvent& e) override
            {
                if (auto* clip = hitTestClipAtEvent(e.position))
                {
                    controller_.setSelectedClipId(clip->id);
                    owner_.openMidiEditorForInstrumentClip(clip->id);
                    repaint();
                }
            }

        private:
            [[nodiscard]] juce::Rectangle<int> getLaneContentBounds() const
            {
                return getLocalBounds().reduced(8, 6);
            }

            [[nodiscard]] juce::Rectangle<int> getEventBoundsForClip(const InstrumentMidiClip& c,
                                                                     juce::Rectangle<int> laneContent) const
            {
                using namespace mini_daw::timeline_clip_chrome;
                const auto band = laneContent.toFloat().reduced(0.0f, kEventVerticalMargin);
                TimelineViewportModel& vp = owner_.getTimelineViewport();
                const double spp = vp.getSamplesPerPixel();
                if (spp > 0.0 && std::isfinite(spp) && c.lengthSamples > 0)
                {
                    const std::int64_t visStart = vp.getVisibleStartSamples();
                    const float originX = band.getX();
                    const std::int64_t len = juce::jmax(std::int64_t{1}, c.lengthSamples);
                    const float x0 = TimelineRulerView::sessionSampleToLocalX(
                        c.startSamples, originX, visStart, spp);
                    const float x1 = TimelineRulerView::sessionSampleToLocalX(
                        c.startSamples + len, originX, visStart, spp);
                    float left = juce::jmin(x0, x1);
                    float right = juce::jmax(x0, x1);
                    constexpr float minW = 40.0f;
                    if (right - left < minW)
                    {
                        const float mid = 0.5f * (left + right);
                        left = mid - minW * 0.5f;
                        right = mid + minW * 0.5f;
                    }
                    left = juce::jlimit(band.getX(), band.getRight(), left);
                    right = juce::jlimit(band.getX(), band.getRight(), right);
                    if (right <= band.getX() + 0.5f || left >= band.getRight() - 0.5f)
                    {
                        return {};
                    }
                    const int y = juce::roundToInt(band.getY());
                    const int h = juce::jmax(1, juce::roundToInt(band.getHeight()));
                    return { juce::roundToInt(left), y, juce::jmax(1, juce::roundToInt(right - left)), h };
                }

                const int laneCW = juce::jmax(1, juce::roundToInt(band.getWidth()));
                const float s = (float)c.laneStartFractionPermille / 1000.f;
                const float e = (float)c.laneEndFractionPermille / 1000.f;
                const float span = juce::jlimit(0.02f, 1.f, e - s);
                int w = juce::roundToInt((float)laneCW * span);
                w = juce::jmax(40, juce::jmin(w, laneCW));
                const int minX0 = juce::roundToInt(band.getX());
                const int maxX0 = juce::roundToInt(band.getRight()) - w;
                if (maxX0 < minX0)
                {
                    return {};
                }
                const int avail = juce::jmax(0, maxX0 - minX0);
                const int x0 = minX0 + (avail > 0 ? juce::roundToInt(s * (float)avail) : 0);
                const int clampedX0 = juce::jlimit(minX0, maxX0, x0);
                const int y = juce::roundToInt(band.getY());
                const int h = juce::jmax(1, juce::roundToInt(band.getHeight()));
                return { clampedX0, y, w, h };
            }

            [[nodiscard]] InstrumentMidiClip* hitTestClipAtEvent(juce::Point<float> pos) const
            {
                const auto laneContent = getLaneContentBounds();
                if (!laneContent.contains(pos.toInt()))
                {
                    return nullptr;
                }

                for (const auto& up : controller_.getClips())
                {
                    auto* c = up.get();
                    if (c == nullptr)
                    {
                        continue;
                    }

                    if (getEventBoundsForClip(*c, laneContent).contains(pos.toInt()))
                    {
                        return c;
                    }
                }

                return nullptr;
            }

            TransportControlsContent& owner_;
            InstrumentTrackController& controller_;
        };

        struct InternalClipPasteboard
        {
            std::shared_ptr<const AudioClip> material;
            std::int64_t leftTrimSamples = 0;
            std::int64_t visibleLengthSamples = 0;
            std::int64_t materialWindowStartSamples = 0;
            std::int64_t materialWindowEndExclusiveSamples = 0;
        };

        std::optional<InternalClipPasteboard> clipPasteboard_;

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
            syncInstrumentClipTimelineFromDevice();
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

        void showHelpMenuPopup()
        {
            juce::PopupMenu menu;
            constexpr int kUndoBehaviorMenuItemId = 1;
            menu.addItem(kUndoBehaviorMenuItemId, "Undo Behavior...");
            juce::Component::SafePointer<TransportControlsContent> safeThis(this);
            menu.showMenuAsync(
                juce::PopupMenu::Options().withTargetComponent(&helpButton),
                [safeThis, kUndoBehaviorMenuItemId](const int result) {
                    if (safeThis == nullptr)
                    {
                        return;
                    }
                    if (result != kUndoBehaviorMenuItemId)
                    {
                        return;
                    }
                    safeThis->showUndoBehaviorDialog();
                });
        }

        void showUndoBehaviorDialog()
        {
            juce::AlertWindow::showMessageBoxAsync(
                juce::AlertWindow::InfoIcon,
                "Undo Behavior",
                undoBehaviorHelpBodyText());
        }

        [[nodiscard]] static juce::String undoBehaviorHelpBodyText()
        {
            return juce::String(
                "Undo will restore previous session/timeline states, such as:\n"
                "  - clip moves\n"
                "  - clip trims\n"
                "  - split clips\n"
                "  - pasted clips\n"
                "  - deleted events\n"
                "  - deleted tracks\n"
                "  - track mute / off / fader changes\n"
                "  - locator / range edits\n"
                "\n"
                "Undo will NOT automatically delete or restore external files on disk.\n"
                "\n"
                "For safety:\n"
                "  - recorded audio files in the project Audio/ folder remain on disk\n"
                "  - imported audio files copied into Audio/ remain on disk\n"
                "  - undoing a recording or import placement may remove the timeline event,\n"
                "    but not the underlying audio file\n"
                "  - cleanup of unused files will be a separate future command\n"
                "    (e.g. \"Clean Unused Media\")\n"
                "\n"
                "Note: undo/redo is not implemented yet. This dialog explains the planned behavior.");
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

        // [Message thread] After `Session::restoreSessionSnapshotForUndo` / redo: playhead, cycle
        // toggle, and L/R locator samples are not session-undo state for this pass (`withLocators`
        // reapplies live locators on purpose). Clear clip UI gestures / caches so stale drag ghosts
        // and raster fingerprints cannot survive restore.
        void refreshAfterSessionSnapshotRestore()
        {
            if constexpr (undo_diagnostic::kUndoDiag)
            {
                writeUndoDiagnosticLogLine(
                    "[UndoDiag] refreshAfterSessionSnapshotRestore: cancel UI for snapshot restore");
            }
            trackLanesView.cancelAllClipGesturesAndTransientUiState();
            if (cycleSessionTrackId_ != kInvalidTrackId)
            {
                const std::shared_ptr<const SessionSnapshot> snap
                    = session.loadSessionSnapshotForAudioThread();
                if (snap == nullptr || snap->findTrackIndexById(cycleSessionTrackId_) < 0)
                {
                    if constexpr (undo_diagnostic::kUndoDiag)
                    {
                        writeUndoDiagnosticLogLine(
                            "[UndoDiag] cycleSessionTrackId cleared (track missing from snapshot)");
                    }
                    cycleSessionTrackId_ = kInvalidTrackId;
                }
            }
            syncViewportFromSession();
            trackLanesView.syncTracksFromSession();
            rulerView.repaint();
            trackLanesView.repaint();
            inspectorView_.refreshFromSession();
            if constexpr (undo_diagnostic::kUndoDiag)
            {
                writeUndoDiagnosticLogLine("[UndoDiag] refreshAfterSessionSnapshotRestore complete");
            }
        }

        // [Message thread] Undo-1: mutator must return false when no session mutation occurred
        // (e.g. paste `Result::fail`). `Session::removePlacedClip` / `removeTrack` always allocate a
        // new `SessionSnapshot` instance even on semantic no-op, so we only wrap paths that
        // pre-validate the target id (delete event / delete track) or use `Result::wasOk()` (paste).
        template <typename F>
        void executeUndoableSessionEdit(const juce::String& label, F&& mutator)
        {
            static_assert(std::is_invocable_r_v<bool, F>);
            std::shared_ptr<const SessionSnapshot> before = session.loadSessionSnapshotForAudioThread();
            if (before == nullptr)
            {
                if constexpr (undo_diagnostic::kUndoDiag)
                {
                    writeUndoDiagnosticLogLine("[UndoDiag] executeUndoableSessionEdit skip: null before label=\""
                                             + label + "\"");
                }
                return;
            }
            if constexpr (undo_diagnostic::kUndoDiag)
            {
                writeUndoDiagnosticLogLine("[UndoDiag] executeUndoableSessionEdit mutator run label=\"" + label
                                           + "\" before=" + undoDiagSnapPtr(before.get()) + " undoSize="
                                           + juce::String(sessionHistory_.undoStackSize()) + " redoSize="
                                           + juce::String(sessionHistory_.redoStackSize()));
            }
            if (!mutator())
            {
                if constexpr (undo_diagnostic::kUndoDiag)
                {
                    writeUndoDiagnosticLogLine(
                        "[UndoDiag] executeUndoableSessionEdit mutator=false label=\"" + label + "\"");
                }
                return;
            }
            std::shared_ptr<const SessionSnapshot> after = session.loadSessionSnapshotForAudioThread();
            if (after == nullptr)
            {
                if constexpr (undo_diagnostic::kUndoDiag)
                {
                    writeUndoDiagnosticLogLine("[UndoDiag] executeUndoableSessionEdit skip: null after label=\""
                                               + label + "\"");
                }
                return;
            }
            const SessionSnapshot* const afterPtrForDiag = after.get();
            sessionHistory_.record(label, std::move(before), std::move(after));
            if constexpr (undo_diagnostic::kUndoDiag)
            {
                writeUndoDiagnosticLogLine("[UndoDiag] executeUndoableSessionEdit after record label=\"" + label
                                           + "\" after=" + undoDiagSnapPtr(afterPtrForDiag) + " undoSize="
                                           + juce::String(sessionHistory_.undoStackSize()) + " redoSize="
                                           + juce::String(sessionHistory_.redoStackSize()));
            }
        }

        template <typename F>
        void executeUndoableInstrumentEdit(const juce::String& label, F&& mutator)
        {
            static_assert(std::is_invocable_r_v<bool, F>);
            std::shared_ptr<const SessionSnapshot> snap = session.loadSessionSnapshotForAudioThread();
            if (snap == nullptr)
            {
                if constexpr (undo_diagnostic::kUndoDiag)
                {
                    writeUndoDiagnosticLogLine(
                        "[UndoDiag] executeUndoableInstrumentEdit skip: null snap label=\"" + label + "\"");
                }
                return;
            }
            std::vector<ProjectFileExperimentalInstrumentTrackV1> beforeMusical
                = instrumentTrackController_.buildExperimentalInstrumentMusicalUndoBlock();
            if (beforeMusical.empty())
            {
                if constexpr (undo_diagnostic::kUndoDiag)
                {
                    writeUndoDiagnosticLogLine("[UndoDiag] executeUndoableInstrumentEdit skip: empty before label=\""
                                               + label + "\"");
                }
                return;
            }
            if constexpr (undo_diagnostic::kUndoDiag)
            {
                writeUndoDiagnosticLogLine("[UndoDiag] executeUndoableInstrumentEdit mutator run label=\"" + label
                                           + "\"");
            }
            if (!mutator())
            {
                if constexpr (undo_diagnostic::kUndoDiag)
                {
                    writeUndoDiagnosticLogLine(
                        "[UndoDiag] executeUndoableInstrumentEdit mutator=false label=\"" + label + "\"");
                }
                return;
            }
            std::vector<ProjectFileExperimentalInstrumentTrackV1> afterMusical
                = instrumentTrackController_.buildExperimentalInstrumentMusicalUndoBlock();
            if (afterMusical.empty())
            {
                if constexpr (undo_diagnostic::kUndoDiag)
                {
                    writeUndoDiagnosticLogLine("[UndoDiag] executeUndoableInstrumentEdit skip: empty after label=\""
                                               + label + "\"");
                }
                return;
            }
            if (experimentalInstrumentTracksMusicalUndoEqual(beforeMusical, afterMusical))
            {
                if constexpr (undo_diagnostic::kUndoDiag)
                {
                    writeUndoDiagnosticLogLine(
                        "[UndoDiag] executeUndoableInstrumentEdit skip: musicalEqual label=\"" + label + "\"");
                }
                return;
            }
            sessionHistory_.record(label,
                                   snap,
                                   snap,
                                   std::nullopt,
                                   InstrumentUndoStepSides{ std::move(beforeMusical), std::move(afterMusical) });
            if constexpr (undo_diagnostic::kUndoDiag)
            {
                writeUndoDiagnosticLogLine("[UndoDiag] executeUndoableInstrumentEdit recorded label=\"" + label
                                           + "\" undoSize=" + juce::String(sessionHistory_.undoStackSize())
                                           + " redoSize=" + juce::String(sessionHistory_.redoStackSize()));
            }
        }

        void commitInstrumentMusicalUndoPair(
            const juce::String& label,
            std::vector<ProjectFileExperimentalInstrumentTrackV1> beforeMusical)
        {
            if (beforeMusical.empty())
            {
                return;
            }
            std::shared_ptr<const SessionSnapshot> snap = session.loadSessionSnapshotForAudioThread();
            if (snap == nullptr)
            {
                return;
            }
            std::vector<ProjectFileExperimentalInstrumentTrackV1> afterMusical
                = instrumentTrackController_.buildExperimentalInstrumentMusicalUndoBlock();
            if (afterMusical.empty() || experimentalInstrumentTracksMusicalUndoEqual(beforeMusical, afterMusical))
            {
                return;
            }
            sessionHistory_.record(label,
                                   snap,
                                   snap,
                                   std::nullopt,
                                   InstrumentUndoStepSides{ std::move(beforeMusical), std::move(afterMusical) });
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
                    return;
                }

                juce::AudioIODevice* const device = deviceManager.getCurrentAudioDevice();
                if (device == nullptr)
                {
                    juce::AlertWindow::showMessageBoxAsync(
                        juce::AlertWindow::WarningIcon,
                        "Audio",
                        "No active audio device. Cannot validate sample rate for load.");
                    return;
                }

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
                    juce::AlertWindow::showMessageBoxAsync(
                        juce::AlertWindow::WarningIcon,
                        "Could not import audio",
                        importRes.getErrorMessage());
                    return;
                }
                executeUndoableSessionEdit("Import clip", [&]() -> bool {
                    const juce::Result loadResult = session.addClipFromFileAtPlayhead(
                        pathToUse, sampleRate, startSampleOnTimeline);
                    if (!loadResult.wasOk())
                    {
                        juce::AlertWindow::showMessageBoxAsync(
                            juce::AlertWindow::WarningIcon,
                            "Could not open file",
                            loadResult.getErrorMessage());
                        return false;
                    }
                    // New **front** clip is on the active track; playhead/transport are unchanged.
                    syncViewportFromSession();
                    trackLanesView.syncTracksFromSession();
                    rulerView.repaint();
                    trackLanesView.repaint();
                    inspectorView_.refreshFromSession();
                    return true;
                });
            });
        }

        void showVst3PluginPickerForTrack(const TrackId trackId,
                                          InsertPickerMode mode,
                                          juce::Component* anchor)
        {
            if (trackId == kInvalidTrackId)
            {
                return;
            }
            juce::FileSearchPath combined = mini_daw::getStandardVst3SearchPaths();
            const juce::FileSearchPath userPaths = mini_daw::loadUserVst3SearchPaths();
            for (int i = 0; i < userPaths.getNumPaths(); ++i)
            {
                combined.add(userPaths[i], -1);
            }
            auto scan = mini_daw::scanForVst3Plugins(combined);

            juce::PopupMenu menu;
            juce::PopupMenu discovered;
            if (scan.entries.empty())
            {
                discovered.addItem(
                    juce::PopupMenu::Item("(no VST3 plugins found)").setEnabled(false));
            }
            else
            {
                constexpr int kFoundBase = 1000;
                for (size_t i = 0; i < scan.entries.size(); ++i)
                {
                    const auto& en = scan.entries[i];
                    juce::PopupMenu::Item item(
                        en.support == mini_daw::PluginPickerSupport::SupportedCandidate
                            ? en.displayName
                            : en.displayName + "  (" + en.unsupportedReason + ")");
                    item.itemID = static_cast<int>(kFoundBase + static_cast<int>(i));
                    item.setEnabled(en.support == mini_daw::PluginPickerSupport::SupportedCandidate);
                    discovered.addItem(item);
                }
            }
            menu.addSubMenu("Discovered VST3 plugins", discovered);
            menu.addSeparator();
            menu.addItem(1, "Add VST3 Folder...");
            menu.addItem(2, "Load Specific VST3...");

            juce::Component* const target = (anchor != nullptr) ? anchor : this;
            juce::Component::SafePointer<TransportControlsContent> safeThis(this);
            std::vector<mini_daw::PluginDiscoveryEntry> entries = std::move(scan.entries);
            menu.showMenuAsync(
                juce::PopupMenu::Options().withTargetComponent(target),
                [safeThis, trackId, mode, entries = std::move(entries), anchor](const int result) {
                    if (safeThis == nullptr || result == 0)
                    {
                        return;
                    }
                    if (result == 1)
                    {
                        safeThis->beginAddVst3FolderForTrack(trackId, anchor, mode);
                        return;
                    }
                    if (result == 2)
                    {
                        safeThis->beginLoadVst3ForTrack(trackId, mode);
                        return;
                    }
                    constexpr int kFoundBase = 1000;
                    const size_t idx = static_cast<size_t>(result - kFoundBase);
                    if (idx >= entries.size())
                    {
                        return;
                    }
                    if (mini_daw::classifyVst3Candidate(entries[idx].file.getFileNameWithoutExtension())
                        == mini_daw::PluginPickerSupport::UnsupportedInstrument)
                    {
                        juce::AlertWindow::showMessageBoxAsync(
                            juce::AlertWindow::WarningIcon, "VST3", instrumentVst3InsertBlockedMessage());
                        safeThis->inspectorView_.refreshFromSession();
                        return;
                    }
                    const InsertStage stage
                        = (mode == InsertPickerMode::AddPre) ? InsertStage::Pre : InsertStage::Post;
                    const juce::Result r
                        = safeThis->pluginHost_.addInsertFromVst3File(trackId, stage, entries[idx].file);
                    if (!r.wasOk())
                    {
                        juce::AlertWindow::showMessageBoxAsync(
                            juce::AlertWindow::WarningIcon, "VST3", r.getErrorMessage());
                    }
                    safeThis->inspectorView_.refreshFromSession();
                });
        }

        void refreshExperimentalInstrumentUi()
        {
            instrumentTrackController_.syncShellWithHostState();
            const bool showInst = instrumentTrackController_.hasInstrumentTrack();
            if (instrumentTrackHeader_ != nullptr)
            {
                instrumentTrackHeader_->setVisible(showInst);
                if (showInst)
                {
                    instrumentTrackHeader_->repaint();
                }
            }
            instrumentMidiEventLane_.setVisible(showInst);

            if (experimentalMidiEditorWindow_ != nullptr)
            {
                experimentalMidiEditorWindow_->syncInstrumentStateFromHost();
            }
        }

        void onAddGrooveAgentInstrumentTrackFromMenu()
        {
            if (!experimentalInstrumentHost_.hasInstrument())
            {
                juce::AlertWindow::showMessageBoxAsync(
                    juce::AlertWindow::InfoIcon,
                    "Instrument track",
                    "Load Groove Agent SE from cached OOP description first.");
                return;
            }
            if (!experimentalInstrumentHost_.getInstrumentNameForUi().containsIgnoreCase("Groove Agent"))
            {
                juce::AlertWindow::showMessageBoxAsync(
                    juce::AlertWindow::InfoIcon,
                    "Instrument track",
                    "Load Groove Agent SE from cached OOP description first.");
                return;
            }
            if (!instrumentTrackController_.tryAddGrooveAgentInstrumentTrackShell())
            {
                juce::AlertWindow::showMessageBoxAsync(
                    juce::AlertWindow::InfoIcon,
                    "Instrument track",
                    "An instrument track row is already shown.");
                return;
            }
            syncInstrumentClipTimelineFromDevice();
            refreshExperimentalInstrumentUi();
            resized();
        }

        static juce::String experimentalInstrumentRescanFailureDetail(const mini_daw::Vst3OopScanOutcome outcome,
                                                                      bool successButNoDescriptions)
        {
            if (successButNoDescriptions)
            {
                return "The scan finished but no plugin descriptions were returned.";
            }
            switch (outcome)
            {
            case mini_daw::Vst3OopScanOutcome::Success:
                return {};
            case mini_daw::Vst3OopScanOutcome::ChildCrashedOrFailed:
                return "The scan process exited with an error.";
            case mini_daw::Vst3OopScanOutcome::Timeout:
                return "The scan timed out.";
            case mini_daw::Vst3OopScanOutcome::LaunchFailed:
                return "The scan process could not be started.";
            case mini_daw::Vst3OopScanOutcome::ParseFailed:
                return "The scan result could not be read.";
            default:
                return "Unknown error.";
            }
        }

        static juce::String experimentalInstrumentRescanOutcomeLogTag(const mini_daw::Vst3OopScanOutcome outcome,
                                                                      bool successButNoDescriptions)
        {
            if (successButNoDescriptions)
            {
                return "success_no_descriptions";
            }
            switch (outcome)
            {
            case mini_daw::Vst3OopScanOutcome::Success:
                return "success";
            case mini_daw::Vst3OopScanOutcome::ChildCrashedOrFailed:
                return "child_failed";
            case mini_daw::Vst3OopScanOutcome::Timeout:
                return "timeout";
            case mini_daw::Vst3OopScanOutcome::LaunchFailed:
                return "launch_failed";
            case mini_daw::Vst3OopScanOutcome::ParseFailed:
                return "parse_failed";
            default:
                return "unknown";
            }
        }

        void runExperimentalInstrumentPluginDescriptionRescan()
        {
            if (!experimentalInstrumentHost_.hasInstrument())
            {
                juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::InfoIcon,
                                                       "Experimental instrument",
                                                       "No instrument plugin is loaded.");
                return;
            }
            const juce::File bundle(experimentalInstrumentHost_.getLastLoadedVst3OriginalPath());
            if (!bundle.exists())
            {
                juce::AlertWindow::showMessageBoxAsync(
                    juce::AlertWindow::WarningIcon,
                    "Experimental instrument",
                    "No VST3 bundle path is known for the loaded instrument.");
                return;
            }

            bool expectedBusy = false;
            if (!experimentalOopScanBusy_.compare_exchange_strong(expectedBusy, true))
            {
                mini_daw::writeVst3OopScanDiagnosticLogLine(
                    "parent: instrument description rescan ignored (OOP operation already in progress)");
                return;
            }

            mini_daw::writeVst3OopScanDiagnosticLogLine("rescan requested path=\"" + bundle.getFullPathName() + "\"");

            juce::Component::SafePointer<TransportControlsContent> safeThis(this);
            const juce::File bundleCopy = bundle;
            std::thread([safeThis, bundleCopy] {
                const mini_daw::Vst3OopScanResult scanResult
                    = mini_daw::runVst3OopScanBlocking(bundleCopy, mini_daw::kVst3OopScanReplyTimeoutMs);
                juce::MessageManager::callAsync([safeThis, scanResult, bundleCopy] {
                    if (safeThis == nullptr)
                    {
                        return;
                    }
                    safeThis->experimentalOopScanBusy_.store(false);
                    safeThis->refreshExperimentalInstrumentUi();

                    const bool successNoDesc = scanResult.outcome == mini_daw::Vst3OopScanOutcome::Success
                                               && scanResult.descriptions.empty();
                    const bool ok = scanResult.outcome == mini_daw::Vst3OopScanOutcome::Success
                                    && !scanResult.descriptions.empty();

                    if (!ok)
                    {
                        const juce::String tag = experimentalInstrumentRescanOutcomeLogTag(
                            scanResult.outcome,
                            successNoDesc);
                        mini_daw::writeVst3OopScanDiagnosticLogLine("rescan failed outcome=" + tag);
                        juce::String msg = "Plugin description scan failed. Existing cache and loaded instrument "
                                          "were left unchanged.\n\n";
                        msg << experimentalInstrumentRescanFailureDetail(scanResult.outcome, successNoDesc);
                        juce::AlertWindow::showMessageBoxAsync(
                            juce::AlertWindow::WarningIcon, "Experimental instrument", msg);
                        return;
                    }

                    mini_daw::writeVst3OopScanDiagnosticLogLine(
                        "rescan success descriptionCount=" + juce::String((int)scanResult.descriptions.size())
                        + " v2Updated=yes");
                });
            }).detach();
        }

        void beginAddVst3FolderForTrack(const TrackId trackId,
                                        juce::Component* anchor,
                                        InsertPickerMode mode)
        {
            if (trackId == kInvalidTrackId)
            {
                return;
            }
            if (vst3FolderChooserInFlight_)
            {
                return;
            }
            vst3FolderChooserInFlight_ = true;
            const auto chooserFlags = juce::FileBrowserComponent::openMode
                                      | juce::FileBrowserComponent::canSelectDirectories;
            auto chooser = std::make_shared<juce::FileChooser>("Add VST3 search folder", juce::File{}, "*");
            chooser->launchAsync(
                chooserFlags,
                [this, chooser, trackId, anchor, mode](const juce::FileChooser& fc) {
                    juce::ignoreUnused(chooser);
                    struct ClearFolderChooser
                    {
                        bool& flag;
                        explicit ClearFolderChooser(bool& f) noexcept
                            : flag(f)
                        {
                        }
                        ~ClearFolderChooser() { flag = false; }
                    } clearFlag{ vst3FolderChooserInFlight_ };
                    const juce::File folder = fc.getResult();
                    if (!folder.isDirectory())
                    {
                        return;
                    }
                    juce::FileSearchPath paths = mini_daw::loadUserVst3SearchPaths();
                    paths.add(folder, -1);
                    mini_daw::saveUserVst3SearchPaths(paths);
                    showVst3PluginPickerForTrack(trackId, mode, anchor);
                });
        }

        void beginLoadVst3ForTrack(const TrackId trackId, InsertPickerMode mode)
        {
            if (trackId == kInvalidTrackId)
            {
                return;
            }
            if (vst3ChooserInFlight_)
            {
                return;
            }
            vst3ChooserInFlight_ = true;
            const auto fileChooserFlags = juce::FileBrowserComponent::openMode
                                          | juce::FileBrowserComponent::canSelectFiles;
            auto chooser = std::make_shared<juce::FileChooser>("Load VST3", juce::File{}, "*.vst3");
            chooser->launchAsync(
                fileChooserFlags,
                [this, chooser, trackId, mode](const juce::FileChooser& fc) {
                    juce::ignoreUnused(chooser);
                    struct ClearVst3Chooser
                    {
                        bool& flag;
                        explicit ClearVst3Chooser(bool& f) noexcept
                            : flag(f)
                        {
                        }
                        ~ClearVst3Chooser() { flag = false; }
                    } clearFlag{ vst3ChooserInFlight_ };
                    const juce::File file = fc.getResult();
                    if (!file.exists())
                    {
                        inspectorView_.refreshFromSession();
                        return;
                    }
                    if (mini_daw::classifyVst3Candidate(file.getFileNameWithoutExtension())
                        == mini_daw::PluginPickerSupport::UnsupportedInstrument)
                    {
                        juce::AlertWindow::showMessageBoxAsync(
                            juce::AlertWindow::WarningIcon, "VST3", instrumentVst3InsertBlockedMessage());
                        inspectorView_.refreshFromSession();
                        return;
                    }
                    const InsertStage stage
                        = (mode == InsertPickerMode::AddPre) ? InsertStage::Pre : InsertStage::Post;
                    const juce::Result r = pluginHost_.addInsertFromVst3File(trackId, stage, file);
                    if (!r.wasOk())
                    {
                        juce::AlertWindow::showMessageBoxAsync(
                            juce::AlertWindow::WarningIcon, "VST3", r.getErrorMessage());
                    }
                    inspectorView_.refreshFromSession();
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
                if (experimentalMidiEditorWindow_ != nullptr)
                {
                    experimentalMidiEditorWindow_->snapshotOpenClipViewportFromRoll();
                }
                const juce::Result r = session.saveProjectToFile(
                    transport, session.getCurrentProjectFile(), sampleRate, &pluginHost_, &instrumentTrackController_);
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
                if (experimentalMidiEditorWindow_ != nullptr)
                {
                    experimentalMidiEditorWindow_->snapshotOpenClipViewportFromRoll();
                }
                const juce::Result r
                    = session.saveProjectToFile(transport, projectFile, sampleRate, &pluginHost_, &instrumentTrackController_);
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
                instrumentTrackController_.setTimelineSampleRate(sampleRate);
                const juce::Result r
                    = session.loadProjectFromFile(transport, f, sampleRate, skipped, infoNote, &pluginHost_, &instrumentTrackController_);
                if (!r.wasOk())
                {
                    juce::AlertWindow::showMessageBoxAsync(
                        juce::AlertWindow::WarningIcon, "Load project", r.getErrorMessage());
                    return;
                }
                juce::String experimentalInstrumentAutoloadNote;
                instrumentTrackController_.runPendingGrooveAgentProjectAutoload(
                    experimentalInstrumentHost_, experimentalInstrumentAutoloadNote);
                if (experimentalMidiEditorWindow_ != nullptr)
                {
                    experimentalMidiEditorWindow_->syncInstrumentStateFromHost();
                }
                if (experimentalInstrumentAutoloadNote.isNotEmpty())
                {
                    if (infoNote.isNotEmpty())
                    {
                        infoNote << "\n\n";
                    }
                    infoNote << experimentalInstrumentAutoloadNote;
                }
                sessionHistory_.clear();
                syncViewportFromSession();
                syncInstrumentClipTimelineFromDevice();
                trackLanesView.syncTracksFromSession();
                inspectorView_.refreshFromSession();
                rulerView.repaint();
                trackLanesView.repaint();
                refreshExperimentalInstrumentUi();
                resized();
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
        PluginInsertHost& pluginHost_;
        ExperimentalInstrumentHost& experimentalInstrumentHost_;
        InstrumentTrackController instrumentTrackController_;
        juce::AudioDeviceManager& deviceManager;
        RecorderService& recorder_;
        CountInClickOutput& countInClicks_;
        LatencySettingsStore& latencyStore_;
        PlaybackEngine& playbackEngine_;

        SessionHistory sessionHistory_;

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
        bool vst3ChooserInFlight_ = false;
        bool vst3FolderChooserInFlight_ = false;
        std::atomic<bool> experimentalOopScanBusy_{ false };
        std::unique_ptr<ExperimentalMidiEditorWindow> experimentalMidiEditorWindow_;

        EditTool currentEditTool_ = EditTool::Pointer;

        juce::TextButton addClipButton{ "Add clip..." };
        juce::TextButton addTrackButton{ "Add track" };
        juce::TextButton saveProjectButton{ "Save Project..." };
        juce::TextButton loadProjectButton{ "Load Project..." };
        juce::TextButton playPauseButton{ "Play" };
        juce::TextButton stopButton{ "Stop" };
        juce::TextButton audioSettingsButton{ "Audio..." };
        juce::TextButton helpButton{ "Help..." };
        juce::TextButton pointerToolButton_{ "Pointer" };
        juce::TextButton splitToolButton_{ "Split" };

        std::unique_ptr<TrackHeaderView> instrumentTrackHeader_;
        InstrumentMidiEventLane instrumentMidiEventLane_;
        juce::Label keyDiagLabel_;

        /// Temporary: last key seen by `MainWindow::routeShortcut` for numpad diagnostics (gated by flag).
        std::unique_ptr<juce::Label> shortcutDiagLabel_;

        /// UI-only: shared x–span for ruler and lanes; never stored in `Session` (see `PHASE_PLAN`).
        TimelineViewportModel timelineViewport_;
        AudioWaveformCache audioWaveformCache_;
        TimelineRulerView rulerView;
        TrackLanesView trackLanesView;
        InspectorView inspectorView_;
        collapsible_side_strip::ResizeSplitter inspectorResizeSplitter_;
        collapsible_side_strip::CollapsedKnob inspectorCollapsedKnob_;
        int inspectorCurrentWidth_ = kInspectorDefaultW;

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
                   PluginInsertHost& pluginInsertHost,
                   ExperimentalInstrumentHost& experimentalInstrumentHost,
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
                                             pluginInsertHost,
                                             experimentalInstrumentHost,
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
            if constexpr (undo_diagnostic::kUndoDiag)
            {
                writeUndoDiagnosticLogLine("[UndoDiag] enabled");
            }
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
            if constexpr (undo_diagnostic::kUndoDiag)
            {
                writeUndoDiagnosticLogLine("[UndoDiag] MainWindow::keyPressed desc=\""
                                           + key.getTextDescription() + "\"");
            }
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
            if constexpr (undo_diagnostic::kUndoDiag)
            {
                const bool cmd = key.getModifiers().isCommandDown();
                const bool z = (key.getKeyCode() == 'z' || key.getKeyCode() == 'Z');
                const bool y = (key.getKeyCode() == 'y' || key.getKeyCode() == 'Y');
                const bool undoCombo = cmd && !key.getModifiers().isShiftDown() && z;
                const bool redoCombo = cmd && (y || (key.getModifiers().isShiftDown() && z));
                if (undoCombo || redoCombo)
                {
                    writeUndoDiagnosticLogLine(
                        "[UndoDiag] routeShortcut entered undoRelated desc=\"" + key.getTextDescription()
                        + "\" undoCombo=" + juce::String(undoCombo ? "Y" : "n") + " redoCombo="
                        + juce::String(redoCombo ? "Y" : "n") + ")");
                }
            }
            if constexpr (kShowShortcutDiagnostics)
            {
                if (auto* tcc = dynamic_cast<TransportControlsContent*>(getContentComponent()))
                {
                    tcc->setShortcutDiagVisibleCaption(makeShortcutDiagVisibleCaption(key));
                }
            }

            const bool editorHasFocus
                = (dynamic_cast<juce::TextEditor*>(juce::Component::getCurrentlyFocusedComponent())
                   != nullptr);
            if constexpr (undo_diagnostic::kUndoDiag)
            {
                const bool undoShortcut = key.getModifiers().isCommandDown()
                                          && !key.getModifiers().isShiftDown()
                                          && (key.getKeyCode() == 'z' || key.getKeyCode() == 'Z');
                const bool redoShortcut
                    = key.getModifiers().isCommandDown()
                      && ((key.getKeyCode() == 'y' || key.getKeyCode() == 'Y')
                          || ((key.getKeyCode() == 'z' || key.getKeyCode() == 'Z')
                              && key.getModifiers().isShiftDown()));
                if (editorHasFocus && (undoShortcut || redoShortcut))
                {
                    writeUndoDiagnosticLogLine(
                        "[UndoDiag] routeShortcut blocked: TextEditor focus undoShortcut="
                        + juce::String(undoShortcut ? "Y" : "n") + " redoShortcut="
                        + juce::String(redoShortcut ? "Y" : "n") + " desc=\""
                        + key.getTextDescription() + "\"");
                }
            }
            if (!editorHasFocus)
            {
                if (key.isKeyCode(juce::KeyPress::deleteKey))
                {
                    if (auto* tcc = dynamic_cast<TransportControlsContent*>(getContentComponent()))
                    {
                        tcc->invokeDeleteSelectedPlacedClipFromWindowShortcut();
                        return true;
                    }
                }
                if (key.getModifiers().isCommandDown() && (key.getKeyCode() == 'c' || key.getKeyCode() == 'C'))
                {
                    if (auto* tcc = dynamic_cast<TransportControlsContent*>(getContentComponent()))
                    {
                        tcc->invokeCopySelectedClipFromWindowShortcut();
                        return true;
                    }
                }
                if (key.getModifiers().isCommandDown() && (key.getKeyCode() == 'v' || key.getKeyCode() == 'V'))
                {
                    if (auto* tcc = dynamic_cast<TransportControlsContent*>(getContentComponent()))
                    {
                        tcc->invokePasteClipFromWindowShortcut();
                        return true;
                    }
                }
                if (key.getModifiers().isCommandDown() && !key.getModifiers().isShiftDown()
                    && (key.getKeyCode() == 'z' || key.getKeyCode() == 'Z'))
                {
                    if (auto* tcc = dynamic_cast<TransportControlsContent*>(getContentComponent()))
                    {
                        if constexpr (undo_diagnostic::kUndoDiag)
                        {
                            writeUndoDiagnosticLogLine("[UndoDiag] routeShortcut matched=undo desc=\""
                                                       + key.getTextDescription() + "\"");
                        }
                        tcc->invokeUndoFromWindowShortcut();
                        return true;
                    }
                }
                if (key.getModifiers().isCommandDown()
                    && ((key.getKeyCode() == 'y' || key.getKeyCode() == 'Y')
                        || ((key.getKeyCode() == 'z' || key.getKeyCode() == 'Z')
                            && key.getModifiers().isShiftDown())))
                {
                    if (auto* tcc = dynamic_cast<TransportControlsContent*>(getContentComponent()))
                    {
                        if constexpr (undo_diagnostic::kUndoDiag)
                        {
                            writeUndoDiagnosticLogLine("[UndoDiag] routeShortcut matched=redo desc=\""
                                                       + key.getTextDescription() + "\"");
                        }
                        tcc->invokeRedoFromWindowShortcut();
                        return true;
                    }
                }
            }

            if (midi_transport_shortcuts::isRecordToggleShortcut(key))
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
            if (midi_transport_shortcuts::isJumpToLeftLocatorShortcut(key))
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
            if (midi_transport_shortcuts::isSpacePlayPauseShortcut(key))
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

    bool vst3OopWorkerMode_ = false;

    std::unique_ptr<Transport> transport;
    std::unique_ptr<Session> session;
    // Phase 4: recording capture (not user-wired in this file yet); engine holds non-owning `get()`.
    std::unique_ptr<RecorderService> recorderService;
    /// Count-in metronome clicks to device only; coordinator state lives in `TransportControlsContent`.
    std::unique_ptr<CountInClickOutput> countInOutput_;
    std::unique_ptr<ExperimentalInstrumentHost> experimentalInstrumentHost_;
    std::unique_ptr<PluginInsertHost> pluginInsertHost_;
    std::unique_ptr<PlaybackEngine> playbackEngine;
    std::unique_ptr<LatencySettingsStore> latencySettingsStore;
    juce::AudioDeviceManager deviceManager;
    std::unique_ptr<MainWindow> mainWindow;
};

// JUCE: generate WinMain / main and the app singleton; DO NOT add another main().
START_JUCE_APPLICATION(MiniDAWLabApplication)
