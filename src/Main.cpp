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
//   1. removeAudioCallback(playbackEngine)  —  first, so audio never runs during window teardown
//   2. destroy main window (runtime coordinators / plugin hosts tear down with audio silent)
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
#include "diagnostics/CrashDumpHandler.h"
#include "diagnostics/StabilityDiagnosticLog.h"
#include "diagnostics/StabilityScenarioRunner.h"
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
#include "instruments/ProxyRenderScheduler.h"

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

    // Stability C1: minidump on unhandled exception. Installed before any other startup work so
    // even early-initialisation crashes produce a dump. Not installed in the OOP scan worker
    // above: scan-worker crashes are contained by design and must not fill the dump folder.
    installCrashDumpHandler();

    // Hidden verification flag (never exposed in UI): crash deliberately so the dump/breadcrumb/
    // symbolization pipeline can be checked end to end. See scripts/symbolize-crash.ps1.
    if (commandLine.contains("--stability-crash-test"))
    {
        writeLastOperationBreadcrumb("stability crash test: triggering intentional crash");
        triggerIntentionalCrashForStabilityTest();
        return; // not reached
    }

    // Stability C2: `--stability-*` scenario flags. Parsed before the window exists; the scenario
    // starts via callAsync below once startup is complete. A parse error fails fast with exit
    // code 1 (matching the runner's FAIL behavior) so wrapper scripts see bad invocations.
    juce::String stabilityParseError;
    const StabilityScenarioRequest stabilityRequest
        = parseStabilityScenarioFromCommandLine(getCommandLineParameterArray(),
                                                stabilityParseError);
    if (stabilityParseError.isNotEmpty())
    {
        appendStabilityRunLine("RESULT: FAIL invalid stability arguments: " + stabilityParseError);
        setApplicationReturnValue(1);
        quit();
        return;
    }
    if (stabilityRequest.isActive())
    {
        setStabilityTestModeActive(true);
        appendStabilityRunLine("stability test mode active (prompts auto-answer deterministically)");
    }

    writeLastOperationBreadcrumb("app startup begin");

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

    // Stability Slice 3: publish-before-destroy support. After PluginInsertHost publishes a realtime
    // map that dropped live plugin instances, drain the in-flight audio callback before the instances
    // are destroyed. Hook is cleared in shutdown() before the engine is torn down.
    pluginInsertHost_->setRealtimeDrainAfterPublish([enginePtr = playbackEngine.get()] {
        (void)enginePtr->waitForAudioCallbackExit(250.0);
    });

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

    // P1E: application-owned proxy render scheduler (one low-priority worker). Created before
    // the window so the content view can attach its production engine; posted completions run
    // via MessageManager::callAsync. Owned HERE (application scope, sibling of Session) — never
    // by a UI component.
    proxyRenderScheduler_ = std::make_unique<proxy_render::ProxyRenderScheduler>(
        [](std::function<void()> fn) { juce::MessageManager::callAsync(std::move(fn)); });

    mainWindow = createMainWindow(
        getApplicationName(),
        *transport,
        *session,
        *pluginInsertHost_,
        deviceManager,
        *recorderService,
        *countInOutput_,
        *latencySettingsStore,
        *playbackEngine,
        *proxyRenderScheduler_);

    // ".dalproj" on the command line (Explorer double-click / shell "open"): load it through the
    // normal project pipeline once the window is up. `getCommandLineParameterArray()` keeps quoted
    // paths with spaces as one argument and handles Unicode. Missing files surface as a non-fatal
    // alert inside `loadProjectFromFile`.
    const juce::StringArray cliArgs = getCommandLineParameterArray();
    bool commandLineProjectOpenQueued = false;
    for (const auto& arg : cliArgs)
    {
        // Stability C2: scenario project paths belong to the runner, not the normal open path
        // (the runner loads them itself, possibly repeatedly).
        if (stabilityRequest.isActive())
        {
            break;
        }
        if (arg.startsWith("-"))
        {
            continue;
        }
        if (!(arg.endsWithIgnoreCase(".dalproj") || arg.endsWithIgnoreCase(".mdlproj")))
        {
            continue;
        }
        const juce::File projectFile = juce::File::isAbsolutePath(arg)
                                           ? juce::File(arg)
                                           : juce::File::getCurrentWorkingDirectory().getChildFile(arg);
        commandLineProjectOpenQueued = true;
        juce::MessageManager::callAsync([this, projectFile] {
            if (mainWindow != nullptr)
            {
                mainWindow->openProjectFileFromCommandLine(projectFile);
            }
        });
        break;
    }

    // Stability Slice 5: offer autosave recovery once the window is up. Queued after the
    // command-line open above, so an explicitly requested project always wins (the coordinator
    // only logs a breadcrumb in that case and keeps the autosave for the next plain startup).
    // Stability test mode reuses the "command-line open queued" path: the recovery prompt is
    // skipped deterministically (breadcrumb logged, autosave kept for the next plain startup).
    const bool skipRecoveryPrompt = commandLineProjectOpenQueued || stabilityRequest.isActive();
    juce::MessageManager::callAsync([this, skipRecoveryPrompt] {
        if (mainWindow != nullptr)
        {
            mainWindow->offerAutosaveRecoveryOnStartup(skipRecoveryPrompt);
        }
    });

    // Stability C2: start the scenario after the recovery check above (both are queued in order).
    if (stabilityRequest.isActive())
    {
        juce::MessageManager::callAsync([this, stabilityRequest] {
            if (mainWindow != nullptr)
            {
                mainWindow->startStabilityScenario(stabilityRequest);
            }
        });
    }

    // SPIKE-01 (P0/P1A validation spike; removable): hidden diagnostic panel for the
    // authoritative plugin-state capture measurements. Normal startup is unchanged when the
    // flag is absent — this is the only product-path reference to the spike scaffolding.
    // SPIKE-01B-M: `--spike01-auto=<plan>` additionally runs a scripted unattended
    // measurement plan inside the panel (see Spike01StateCapturePanel::buildAutoPlan for the
    // full set of supported plan ids).
    if (commandLine.contains("--spike01-state-capture"))
    {
        juce::String spike01AutoPlan;
        for (const auto& arg : cliArgs)
        {
            if (arg.startsWith("--spike01-auto="))
            {
                spike01AutoPlan = arg.fromFirstOccurrenceOf("=", false, false).trim();
            }
        }
        juce::MessageManager::callAsync([this, spike01AutoPlan] {
            if (mainWindow != nullptr)
            {
                mainWindow->startSpike01StateCaptureProbe(spike01AutoPlan);
            }
        });
    }

    writeLastOperationBreadcrumb("app startup complete");
}

void MiniDAWLabApplication::systemRequestedQuit()
{
    // Stability Slice 5: a dirty project shows the save-before-quit prompt; quitting then resumes
    // from the prompt via JUCEApplication::quit() (which does not re-enter this hook).
    if (mainWindow != nullptr && mainWindow->tryInterceptQuitForUnsavedChanges())
    {
        writeLastOperationBreadcrumb("quit intercepted: unsaved changes prompt shown");
        return;
    }
    quit();
}

// [Message thread] Reverse of initialise; see file header.
void MiniDAWLabApplication::shutdown()
{
    if (vst3OopWorkerMode_)
    {
        vst3OopWorkerMode_ = false;
        return;
    }

    writeLastOperationBreadcrumb("app shutdown begin");

    // Stability Slice 1: remove the audio callback *first* so the audio thread can never touch
    // plugin hosts / instrument runtimes / session state while the window (which owns the
    // runtime coordinators) is being destroyed. removeAudioCallback blocks until any in-flight
    // audio callback has returned.
    if (playbackEngine != nullptr)
    {
        deviceManager.removeAudioCallback(playbackEngine.get());
    }

    // Callback is gone; the drain hook (which references the engine) is no longer needed and must
    // not run once the engine is destroyed below.
    if (pluginInsertHost_ != nullptr)
    {
        pluginInsertHost_->setRealtimeDrainAfterPublish(nullptr);
    }

    // Window next: coordinators and plugin hosts owned by the window tear down with no audio
    // callback running. The content view detaches the proxy-render engine in its destructor
    // (cancel + bounded join + message-thread instance teardown) BEFORE the coordinators die.
    mainWindow.reset();

    // P1E worker join after the window detached its engine; before Session teardown.
    if (proxyRenderScheduler_ != nullptr)
    {
        proxyRenderScheduler_->shutdown();
        proxyRenderScheduler_.reset();
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

    writeLastOperationBreadcrumb("app shutdown complete");
}

// JUCE: generate WinMain / main and the app singleton; DO NOT add another main().
START_JUCE_APPLICATION(MiniDAWLabApplication)
