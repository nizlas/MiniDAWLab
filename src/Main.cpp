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
//   1. transport, session, recorderService, pluginInsertHost, playbackEngine
//      (`playbackEngine` points at transport+session+recorder+pluginHost; instrument Groove Agent
//      hosts attach from `TransportControlsContent` afterward).
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
//   4. destroy playbackEngine, then pluginInsertHost, then recorderService, then session, transport
//
// THREADING
//   juce::JUCEApplication::initialise / shutdown and all UI (buttons, file chooser, paint) are
//   the [Message thread] in a desktop JUCE app. The audio path is only in PlaybackEngine’s
//   callback, which we do not call from here.
//
// NESTED TYPES
//   TransportControlsContent lives in app/MainAppWindow.cpp; MainWindow in app/MainWindow.cpp.
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

#include "MiniDAWLabApplication.h"

#include <algorithm>
#include <cmath>

#include "domain/Session.h"
#include "domain/Track.h"
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
#include "ui/PlayheadOverlay.h"
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
#include "io/ProjectAudioImport.h"
#include "io/AudioWaveformCache.h"
#include "io/ProjectFile.h"
#include "diagnostics/UndoDiagnosticConfig.h"
#include "diagnostics/UndoDiagnosticFileLog.h"
#include "diagnostics/ExperimentalPlaybackRoutingLog.h"

#include <algorithm>
#include <atomic>
#include <memory>
#include <optional>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "app/MainWindow.h"

void MiniDAWLabApplication::initialise(const juce::String& commandLine)
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
    playbackEngine = std::make_unique<PlaybackEngine>(*transport,
                                                       *session,
                                                       recorderService.get(),
                                                       countInOutput_.get(),
                                                       pluginInsertHost_.get());

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

    mainWindow = createMainWindow(
        getApplicationName(),
        *transport,
        *session,
        *pluginInsertHost_,
        deviceManager,
        *recorderService,
        *countInOutput_,
        *latencySettingsStore,
        *playbackEngine);
}

// [Message thread] Reverse of initialise; see file header.
void MiniDAWLabApplication::shutdown()
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
    pluginInsertHost_.reset();
    countInOutput_.reset();
    recorderService.reset();
    session.reset();
    transport.reset();
}

// JUCE: generate WinMain / main and the app singleton; DO NOT add another main().
START_JUCE_APPLICATION(MiniDAWLabApplication)
