#include "diagnostics/StabilityScenarioRunner.h"

#include "diagnostics/StabilityDiagnosticLog.h"
#include "diagnostics/StabilityInvariants.h"
#include "ui/UiLayoutSettingsStore.h"
#include "ui/experimental/ExperimentalMidiPattern.h"

#include <atomic>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <thread>

#if JUCE_WINDOWS
 #define WIN32_LEAN_AND_MEAN
 #include <windows.h>
 #include <psapi.h> // K32GetProcessMemoryInfo (perf-profile process sample; kernel32 export)
#endif

namespace
{
    std::atomic<bool> gStabilityTestModeActive{ false };

    constexpr int kTimerIntervalMs = 50;
    constexpr int kSettleAfterLoadMs = 1200;
    constexpr int kSettleAfterDeleteOpMs = 400;
    constexpr int kSettleDefaultMs = 250;

    [[nodiscard]] juce::int64 nowMs() noexcept
    {
        return static_cast<juce::int64>(juce::Time::getMillisecondCounterHiRes());
    }
} // namespace

bool isStabilityTestModeActive() noexcept
{
    return gStabilityTestModeActive.load(std::memory_order_relaxed);
}

void setStabilityTestModeActive(const bool active) noexcept
{
    gStabilityTestModeActive.store(active, std::memory_order_relaxed);
}

// -----------------------------------------------------------------------------
// Command-line parsing
// -----------------------------------------------------------------------------

StabilityScenarioRequest parseStabilityScenarioFromCommandLine(const juce::StringArray& args,
                                                               juce::String& errorOut)
{
    errorOut.clear();
    StabilityScenarioRequest req;

    auto fileFromArg = [](const juce::String& a) -> juce::File {
        return juce::File::isAbsolutePath(a)
                   ? juce::File(a)
                   : juce::File::getCurrentWorkingDirectory().getChildFile(a);
    };
    auto nextProjectArg = [&args, &fileFromArg](int& i, juce::File& out) -> bool {
        if (i + 1 >= args.size() || args[i + 1].startsWith("-"))
        {
            return false;
        }
        ++i;
        out = fileFromArg(args[i]);
        return true;
    };
    auto setKind = [&req, &errorOut](const StabilityScenarioKind k) -> bool {
        if (req.kind != StabilityScenarioKind::None)
        {
            errorOut = "multiple --stability-* scenario flags given";
            return false;
        }
        req.kind = k;
        return true;
    };

    for (int i = 0; i < args.size(); ++i)
    {
        const juce::String& a = args[i];
        if (a == "--stability-load-loop")
        {
            if (!setKind(StabilityScenarioKind::LoadLoop)) { return {}; }
            if (!nextProjectArg(i, req.projectA))
            {
                errorOut = "--stability-load-loop requires a project path";
                return {};
            }
        }
        else if (a == "--stability-load-alternate")
        {
            if (!setKind(StabilityScenarioKind::LoadAlternate)) { return {}; }
            if (!nextProjectArg(i, req.projectA) || !nextProjectArg(i, req.projectB))
            {
                errorOut = "--stability-load-alternate requires two project paths";
                return {};
            }
        }
        else if (a == "--stability-delete-loop")
        {
            if (!setKind(StabilityScenarioKind::DeleteLoop)) { return {}; }
            if (!nextProjectArg(i, req.projectA))
            {
                errorOut = "--stability-delete-loop requires a project path";
                return {};
            }
        }
        else if (a == "--stability-open-save-close")
        {
            if (!setKind(StabilityScenarioKind::OpenSaveClose)) { return {}; }
            if (!nextProjectArg(i, req.projectA))
            {
                errorOut = "--stability-open-save-close requires a project path";
                return {};
            }
        }
        else if (a == "--stability-smoke")
        {
            if (!setKind(StabilityScenarioKind::Smoke)) { return {}; }
            if (!nextProjectArg(i, req.projectA))
            {
                errorOut = "--stability-smoke requires a project path";
                return {};
            }
        }
        else if (a == "--stability-mixdown")
        {
            if (!setKind(StabilityScenarioKind::Mixdown)) { return {}; }
            if (!nextProjectArg(i, req.projectA))
            {
                errorOut = "--stability-mixdown requires a project path";
                return {};
            }
        }
        else if (a == "--stability-autosave")
        {
            if (!setKind(StabilityScenarioKind::Autosave)) { return {}; }
            if (!nextProjectArg(i, req.projectA))
            {
                errorOut = "--stability-autosave requires a project path";
                return {};
            }
        }
        else if (a == "--stability-recover-autosave")
        {
            if (!setKind(StabilityScenarioKind::RecoverAutosave)) { return {}; }
            if (!nextProjectArg(i, req.projectA))
            {
                errorOut = "--stability-recover-autosave requires a project path";
                return {};
            }
        }
        else if (a == "--stability-midi-routing")
        {
            if (!setKind(StabilityScenarioKind::MidiRouting)) { return {}; }
            if (!nextProjectArg(i, req.projectA))
            {
                errorOut = "--stability-midi-routing requires a project path";
                return {};
            }
        }
        else if (a == "--stability-midi-track-parity")
        {
            if (!setKind(StabilityScenarioKind::MidiTrackParity)) { return {}; }
            if (!nextProjectArg(i, req.projectA))
            {
                errorOut = "--stability-midi-track-parity requires a project path";
                return {};
            }
        }
        else if (a == "--stability-midi-import-audio")
        {
            if (!setKind(StabilityScenarioKind::MidiImportAudio)) { return {}; }
            if (!nextProjectArg(i, req.projectA))
            {
                errorOut = "--stability-midi-import-audio requires a project path";
                return {};
            }
        }
        else if (a == "--stability-midi-editor-move")
        {
            if (!setKind(StabilityScenarioKind::MidiEditorMoveCrash)) { return {}; }
            if (!nextProjectArg(i, req.projectA))
            {
                errorOut = "--stability-midi-editor-move requires a project path";
                return {};
            }
        }
        else if (a == "--stability-pregain")
        {
            if (!setKind(StabilityScenarioKind::PreGain)) { return {}; }
            if (!nextProjectArg(i, req.projectA))
            {
                errorOut = "--stability-pregain requires a project path";
                return {};
            }
        }
        else if (a == "--stability-pregain-inspector")
        {
            if (!setKind(StabilityScenarioKind::PreGainInspector)) { return {}; }
            if (!nextProjectArg(i, req.projectA))
            {
                errorOut = "--stability-pregain-inspector requires a project path";
                return {};
            }
        }
        else if (a == "--stability-inserts")
        {
            if (!setKind(StabilityScenarioKind::Inserts)) { return {}; }
            if (!nextProjectArg(i, req.projectA))
            {
                errorOut = "--stability-inserts requires a project path";
                return {};
            }
        }
        else if (a == "--stability-header-column")
        {
            if (!setKind(StabilityScenarioKind::HeaderColumn)) { return {}; }
            if (!nextProjectArg(i, req.projectA))
            {
                errorOut = "--stability-header-column requires a project path";
                return {};
            }
        }
        else if (a == "--stability-export-levels")
        {
            if (!setKind(StabilityScenarioKind::ExportLevels)) { return {}; }
            if (!nextProjectArg(i, req.projectA))
            {
                errorOut = "--stability-export-levels requires a project path";
                return {};
            }
        }
        else if (a == "--stability-inspector-panel")
        {
            if (!setKind(StabilityScenarioKind::InspectorPanel)) { return {}; }
            if (!nextProjectArg(i, req.projectA))
            {
                errorOut = "--stability-inspector-panel requires a project path";
                return {};
            }
        }
        else if (a == "--stability-mixer")
        {
            if (!setKind(StabilityScenarioKind::Mixer)) { return {}; }
            if (!nextProjectArg(i, req.projectA))
            {
                errorOut = "--stability-mixer requires a project path";
                return {};
            }
        }
        else if (a == "--stability-organ-dc")
        {
            if (!setKind(StabilityScenarioKind::OrganDc)) { return {}; }
            if (!nextProjectArg(i, req.projectA))
            {
                errorOut = "--stability-organ-dc requires a project path";
                return {};
            }
        }
        else if (a == "--stability-live-midi")
        {
            if (!setKind(StabilityScenarioKind::LiveMidi)) { return {}; }
            if (!nextProjectArg(i, req.projectA))
            {
                errorOut = "--stability-live-midi requires a project path";
                return {};
            }
        }
        else if (a == "--stability-midi-cycle-takes")
        {
            if (!setKind(StabilityScenarioKind::MidiCycleTakes)) { return {}; }
            if (!nextProjectArg(i, req.projectA))
            {
                errorOut = "--stability-midi-cycle-takes requires a project path";
                return {};
            }
        }
        else if (a == "--stability-proxy-recording")
        {
            if (!setKind(StabilityScenarioKind::ProxyRecording)) { return {}; }
            if (!nextProjectArg(i, req.projectA))
            {
                errorOut = "--stability-proxy-recording requires a project path";
                return {};
            }
        }
        else if (a == "--stability-proxy-render-probe")
        {
            if (!setKind(StabilityScenarioKind::ProxyRenderProbe)) { return {}; }
            if (!nextProjectArg(i, req.projectA))
            {
                errorOut = "--stability-proxy-render-probe requires <project> <trackId> <outDir>";
                return {};
            }
            if (i + 2 >= args.size())
            {
                errorOut = "--stability-proxy-render-probe requires <project> <trackId> <outDir>";
                return {};
            }
            req.probeTrackId = static_cast<TrackId>(args[i + 1].getLargeIntValue());
            req.probeOutDir = fileFromArg(args[i + 2].unquoted());
            i += 2;
        }
        else if (a == "--stability-proxy-playback-edges")
        {
            if (!setKind(StabilityScenarioKind::ProxyPlaybackEdges)) { return {}; }
            if (!nextProjectArg(i, req.projectA) || i + 2 >= args.size())
            {
                errorOut = "--stability-proxy-playback-edges requires <project> <trackId> <outDir>";
                return {};
            }
            req.probeTrackId = static_cast<TrackId>(args[i + 1].getLargeIntValue());
            req.probeOutDir = fileFromArg(args[i + 2].unquoted());
            i += 2;
        }
        else if (a == "--stability-perf-profile")
        {
            if (!setKind(StabilityScenarioKind::PerfProfile)) { return {}; }
            if (!nextProjectArg(i, req.projectA))
            {
                errorOut = "--stability-perf-profile requires a project path";
                return {};
            }
        }
        else if (a == "--seconds" || a == "--warmup" || a == "--buffer" || a == "--start-seconds")
        {
            if (i + 1 >= args.size())
            {
                errorOut = a + " requires a number";
                return {};
            }
            ++i;
            if (a == "--seconds") { req.perfSeconds = juce::jlimit(5, 600, args[i].getIntValue()); }
            else if (a == "--warmup") { req.perfWarmupSeconds = juce::jlimit(0, 120, args[i].getIntValue()); }
            else if (a == "--buffer") { req.perfRequestedBufferSize = juce::jlimit(0, 8192, args[i].getIntValue()); }
            else { req.perfStartSeconds = juce::jmax(0.0, args[i].getDoubleValue()); }
        }
        else if (a == "--mixer-open")
        {
            req.perfMixerOpen = true;
        }
        else if (a == "--profile-off")
        {
            req.perfProfilerOff = true;
        }
        else if (a == "--repeat")
        {
            if (i + 1 >= args.size())
            {
                errorOut = "--repeat requires a number";
                return {};
            }
            ++i;
            req.probeRepeat = juce::jlimit(1, 50, args[i].getIntValue());
        }
        else if (a == "--wait-after-prepare")
        {
            if (i + 1 >= args.size())
            {
                errorOut = "--wait-after-prepare requires milliseconds";
                return {};
            }
            ++i;
            req.probeWaitAfterPrepareMs = juce::jlimit(0, 600000, args[i].getIntValue());
        }
        else if (a == "--publish")
        {
            req.probePublish = true;
        }
        else if (a == "--state-blob")
        {
            if (!nextProjectArg(i, req.probeStateBlobOverride))
            {
                errorOut = "--state-blob requires a file path";
                return {};
            }
        }
        else if (a == "--no-readiness")
        {
            req.probeNoReadiness = true;
        }
        else if (a == "--realtime-indication")
        {
            req.probeRealtimeIndication = true;
        }
        else if (a == "--tail-policy-v1")
        {
            req.probeTailPolicyV1 = true;
        }
        else if (a == "--retain-failed")
        {
            req.probeRetainFailed = true;
        }
        else if (a == "--midi")
        {
            if (!nextProjectArg(i, req.midiFile))
            {
                errorOut = "--midi requires a .mid file path";
                return {};
            }
        }
        else if (a == "--iterations")
        {
            if (i + 1 >= args.size())
            {
                errorOut = "--iterations requires a number";
                return {};
            }
            ++i;
            req.iterations = args[i].getIntValue();
            if (req.iterations < 1 || req.iterations > 10000)
            {
                errorOut = "--iterations must be 1..10000 (got \"" + args[i] + "\")";
                return {};
            }
        }
        else if (a == "--format")
        {
            if (i + 1 >= args.size())
            {
                errorOut = "--format requires wav or mp3";
                return {};
            }
            ++i;
            if (args[i].equalsIgnoreCase("mp3")) { req.mixdownMp3 = true; }
            else if (args[i].equalsIgnoreCase("wav")) { req.mixdownMp3 = false; }
            else
            {
                errorOut = "--format must be wav or mp3 (got \"" + args[i] + "\")";
                return {};
            }
        }
        else if (a.startsWith("--stability-") && a != "--stability-crash-test")
        {
            errorOut = "unknown stability flag: " + a;
            return {};
        }
    }
    return req;
}

// -----------------------------------------------------------------------------
// Runner
// -----------------------------------------------------------------------------

StabilityScenarioRunner::StabilityScenarioRunner(StabilityRunnerHooks hooks)
    : hooks_(std::move(hooks))
{
}

StabilityScenarioRunner::~StabilityScenarioRunner()
{
    stopTimer();
}

void StabilityScenarioRunner::start(const StabilityScenarioRequest& request)
{
    runStartMs_ = nowMs();
    invariantFailuresAtStart_ = stability_invariants::getStabilityInvariantFailureCount();
    switch (request.kind)
    {
        case StabilityScenarioKind::LoadLoop: scenarioName_ = "load-loop"; break;
        case StabilityScenarioKind::LoadAlternate: scenarioName_ = "load-alternate"; break;
        case StabilityScenarioKind::DeleteLoop: scenarioName_ = "delete-loop"; break;
        case StabilityScenarioKind::OpenSaveClose: scenarioName_ = "open-save-close"; break;
        case StabilityScenarioKind::Smoke: scenarioName_ = "smoke"; break;
        case StabilityScenarioKind::Mixdown:
            scenarioName_ = request.mixdownMp3 ? "mixdown-mp3" : "mixdown-wav";
            break;
        case StabilityScenarioKind::Autosave: scenarioName_ = "autosave"; break;
        case StabilityScenarioKind::RecoverAutosave: scenarioName_ = "recover-autosave"; break;
        case StabilityScenarioKind::MidiRouting: scenarioName_ = "midi-routing"; break;
        case StabilityScenarioKind::MidiTrackParity: scenarioName_ = "midi-track-parity"; break;
        case StabilityScenarioKind::MidiImportAudio: scenarioName_ = "midi-import-audio"; break;
        case StabilityScenarioKind::MidiEditorMoveCrash: scenarioName_ = "midi-editor-move"; break;
        case StabilityScenarioKind::PreGain: scenarioName_ = "pregain"; break;
        case StabilityScenarioKind::PreGainInspector: scenarioName_ = "pregain-inspector"; break;
        case StabilityScenarioKind::Inserts: scenarioName_ = "inserts"; break;
        case StabilityScenarioKind::HeaderColumn: scenarioName_ = "header-column"; break;
        case StabilityScenarioKind::ExportLevels: scenarioName_ = "export-levels"; break;
        case StabilityScenarioKind::InspectorPanel: scenarioName_ = "inspector-panel"; break;
        case StabilityScenarioKind::OrganDc: scenarioName_ = "organ-dc"; break;
        case StabilityScenarioKind::LiveMidi: scenarioName_ = "live-midi"; break;
        case StabilityScenarioKind::MidiCycleTakes: scenarioName_ = "midi-cycle-takes"; break;
        case StabilityScenarioKind::ProxyRecording: scenarioName_ = "proxy-recording"; break;
        case StabilityScenarioKind::ProxyRenderProbe: scenarioName_ = "proxy-render-probe"; break;
        case StabilityScenarioKind::ProxyPlaybackEdges: scenarioName_ = "proxy-playback-edges"; break;
        case StabilityScenarioKind::Mixer: scenarioName_ = "mixer"; break;
        case StabilityScenarioKind::PerfProfile: scenarioName_ = "perf-profile"; break;
        case StabilityScenarioKind::None: scenarioName_ = "none"; break;
    }

    appendStabilityRunLine("================================================================");
    appendStabilityRunLine("scenario start: " + scenarioName_
                           + " iterations=" + juce::String(request.iterations)
                           + " projectA=" + request.projectA.getFullPathName()
                           + (request.projectB != juce::File{}
                                  ? " projectB=" + request.projectB.getFullPathName()
                                  : juce::String{}));

    if (!request.projectA.existsAsFile())
    {
        finish(false, "project file not found: " + request.projectA.getFullPathName());
        return;
    }
    if (request.kind == StabilityScenarioKind::LoadAlternate && !request.projectB.existsAsFile())
    {
        finish(false, "project file not found: " + request.projectB.getFullPathName());
        return;
    }

    switch (request.kind)
    {
        case StabilityScenarioKind::LoadLoop:
            appendLoadLoopSteps(request.projectA, request.iterations);
            break;
        case StabilityScenarioKind::LoadAlternate:
            appendLoadAlternateSteps(request.projectA, request.projectB, request.iterations);
            break;
        case StabilityScenarioKind::DeleteLoop:
            appendDeleteLoopSteps(request.projectA, request.iterations);
            break;
        case StabilityScenarioKind::OpenSaveClose:
            appendOpenSaveCloseSteps(request.projectA);
            break;
        case StabilityScenarioKind::Smoke:
            appendLoadLoopSteps(request.projectA, 3);
            appendDeleteLoopSteps(request.projectA, 2);
            appendOpenSaveCloseSteps(request.projectA);
            break;
        case StabilityScenarioKind::Mixdown:
            appendMixdownSteps(request.projectA, request.mixdownMp3);
            break;
        case StabilityScenarioKind::Autosave:
            appendAutosaveSteps(request.projectA, /*withRecovery*/ false);
            break;
        case StabilityScenarioKind::RecoverAutosave:
            appendAutosaveSteps(request.projectA, /*withRecovery*/ true);
            break;
        case StabilityScenarioKind::MidiRouting:
            appendMidiRoutingSteps(request.projectA);
            break;
        case StabilityScenarioKind::MidiTrackParity:
            appendMidiTrackParitySteps(request.projectA);
            break;
        case StabilityScenarioKind::MidiImportAudio:
            appendMidiImportAudioSteps(request.projectA, request.midiFile);
            break;
        case StabilityScenarioKind::MidiEditorMoveCrash:
            appendMidiEditorMoveCrashSteps(request.projectA);
            break;
        case StabilityScenarioKind::PreGain:
            appendPreGainSteps(request.projectA);
            break;
        case StabilityScenarioKind::PreGainInspector:
            appendPreGainInspectorSteps(request.projectA);
            break;
        case StabilityScenarioKind::Inserts:
            appendInsertsSteps(request.projectA);
            break;
        case StabilityScenarioKind::HeaderColumn:
            appendHeaderColumnSteps(request.projectA);
            break;
        case StabilityScenarioKind::ExportLevels:
            appendExportLevelsSteps(request.projectA);
            break;
        case StabilityScenarioKind::InspectorPanel:
            appendInspectorPanelSteps(request.projectA);
            break;
        case StabilityScenarioKind::OrganDc:
            appendOrganDcSteps(request.projectA);
            break;
        case StabilityScenarioKind::LiveMidi:
            appendLiveMidiSteps(request.projectA);
            break;
        case StabilityScenarioKind::MidiCycleTakes:
            appendMidiCycleTakesSteps(request.projectA);
            break;
        case StabilityScenarioKind::ProxyRecording:
            appendProxyRecordingSteps(request.projectA);
            break;
        case StabilityScenarioKind::ProxyRenderProbe:
            appendProxyRenderProbeSteps(request);
            break;
        case StabilityScenarioKind::ProxyPlaybackEdges:
            appendProxyPlaybackEdgesSteps(request);
            break;
        case StabilityScenarioKind::Mixer:
            appendMixerSteps(request.projectA);
            break;
        case StabilityScenarioKind::PerfProfile:
            appendPerfProfileSteps(request);
            break;
        case StabilityScenarioKind::None:
            finish(false, "no scenario requested");
            return;
    }

    appendStabilityRunLine("steps queued: " + juce::String(static_cast<int>(steps_.size()))
                           + " (delete-loop iterations expand at plan time)");
    resumeAtMs_ = nowMs() + 500; // Initial settle: let startup async work finish first.
    startTimer(kTimerIntervalMs);
}

void StabilityScenarioRunner::timerCallback()
{
    if (finished_ || nowMs() < resumeAtMs_)
    {
        return;
    }
    if (nextStepIndex_ >= steps_.size())
    {
        finish(true, {});
        return;
    }

    // Copy out the step: plan-steps may insert into steps_ while running.
    const Step step = steps_[nextStepIndex_];
    ++nextStepIndex_;

    appendStabilityRunLine("step begin: " + step.name);
    const juce::int64 t0 = nowMs();
    juce::String failReason;
    bool ok = false;
    ok = step.action ? step.action(failReason) : false;
    const juce::int64 elapsed = nowMs() - t0;
    if (!ok)
    {
        appendStabilityRunLine("step FAIL: " + step.name + " elapsedMs=" + juce::String(elapsed)
                               + (failReason.isNotEmpty() ? " reason: " + failReason
                                                          : juce::String{}));
        finish(false, "step \"" + step.name + "\" failed"
                          + (failReason.isNotEmpty() ? ": " + failReason : juce::String{}));
        return;
    }
    // Stability C3: verify runtime invariants after every successful step. Also fail when any
    // invariant failure was logged from an app-internal call site (delete/undo/load hooks) since
    // the run started - a matrix must never silently pass with invariant failures.
    if (hooks_.verifyInvariants && !hooks_.verifyInvariants("runner:" + step.name))
    {
        appendStabilityRunLine("step INVARIANT FAIL after: " + step.name
                               + " (see stability-invariant.log)");
        finish(false, "invariant check failed after step \"" + step.name + "\"");
        return;
    }
    if (stability_invariants::getStabilityInvariantFailureCount() > invariantFailuresAtStart_)
    {
        appendStabilityRunLine("step INVARIANT FAIL (logged by app call site) after: " + step.name
                               + " (see stability-invariant.log)");
        finish(false, "invariant failure logged during step \"" + step.name + "\"");
        return;
    }

    appendStabilityRunLine("step end ok: " + step.name + " elapsedMs=" + juce::String(elapsed));
    // A step may choose its own settle at run time (e.g. "play the whole loop", whose length is
    // only known after the project loaded) - see `settleOverrideMsForCurrentStep_`.
    const int settleMs = settleOverrideMsForCurrentStep_ > 0 ? settleOverrideMsForCurrentStep_ : step.settleMsAfter;
    settleOverrideMsForCurrentStep_ = -1;
    resumeAtMs_ = nowMs() + settleMs;
}

void StabilityScenarioRunner::finish(const bool pass, const juce::String& reason)
{
    if (finished_)
    {
        return;
    }
    finished_ = true;
    stopTimer();

    if (openSaveCloseCopy_ != juce::File{} && openSaveCloseCopy_.existsAsFile())
    {
        (void)openSaveCloseCopy_.deleteFile();
        appendStabilityRunLine("cleanup: deleted temp project copy "
                               + openSaveCloseCopy_.getFullPathName());
        if (scenarioName_ == "inserts")
        {
            (void)openSaveCloseCopy_.getSiblingFile(openSaveCloseCopy_.getFileNameWithoutExtension() + "_autosave.dalproj").deleteFile();
        }
    }
    if (insertsMissingPluginCopy_ != juce::File{} && insertsMissingPluginCopy_.existsAsFile())
    {
        (void)insertsMissingPluginCopy_.deleteFile();
        (void)insertsMissingPluginCopy_.getSiblingFile(insertsMissingPluginCopy_.getFileNameWithoutExtension() + "_autosave.dalproj").deleteFile();
        appendStabilityRunLine("cleanup: deleted missing-plugin test copy " + insertsMissingPluginCopy_.getFullPathName());
    }
    if (scenarioOutputDir_ != juce::File{} && scenarioOutputDir_.isDirectory())
    {
        (void)scenarioOutputDir_.deleteRecursively();
        appendStabilityRunLine("cleanup: deleted scenario output folder "
                               + scenarioOutputDir_.getFullPathName());
    }

    const juce::int64 totalMs = nowMs() - runStartMs_;
    if (pass)
    {
        appendStabilityRunLine("RESULT: PASS scenario=" + scenarioName_
                               + " totalMs=" + juce::String(totalMs));
        writeLastOperationBreadcrumb("stability scenario PASS: " + scenarioName_);
    }
    else
    {
        appendStabilityRunLine("RESULT: FAIL scenario=" + scenarioName_
                               + " totalMs=" + juce::String(totalMs) + " reason: " + reason);
        writeLastOperationBreadcrumb("stability scenario FAIL: " + scenarioName_ + " - " + reason);
    }

    const int exitCode = pass ? 0 : 1;

    // Shutdown watchdog: scenario runs have shown the app shutdown can hang when the audio
    // callback is wedged inside its processing section (drain "timeout=YES" in
    // track-delete-diag.log; removeAudioCallback then blocks forever). A test run must always
    // terminate with the intended exit code, so a detached thread force-exits after a grace
    // period. When shutdown completes normally the process dies first and this thread with it.
    std::thread([exitCode] {
        std::this_thread::sleep_for(std::chrono::seconds(20));
        appendStabilityRunLine(
            "WARNING: clean shutdown did not complete within 20s - forcing process exit "
            "(see drain timeout=YES lines in track-delete-diag.log)");
#if JUCE_WINDOWS
        ::TerminateProcess(::GetCurrentProcess(), static_cast<UINT>(exitCode));
#else
        std::_Exit(exitCode);
#endif
    }).detach();

    if (auto* app = juce::JUCEApplication::getInstance())
    {
        app->setApplicationReturnValue(exitCode);
        // Direct quit: bypasses the unsaved-changes quit guard on purpose (deterministic exit;
        // scenarios routinely leave the session dirty).
        app->quit();
    }
}

// -----------------------------------------------------------------------------
// Step builders
// -----------------------------------------------------------------------------

// -----------------------------------------------------------------------------
// MIDI cycle takes + layering (`--stability-midi-cycle-takes <project>`)
// -----------------------------------------------------------------------------
void StabilityScenarioRunner::appendMidiCycleTakesSteps(const juce::File& project)
{
    if (hooks_.liveMidiFixtureSetup == nullptr || hooks_.liveMidiInject == nullptr || hooks_.liveMidiSetMonitor == nullptr
        || hooks_.liveMidiSetArm == nullptr || hooks_.liveMidiCapturedNoteCount == nullptr || hooks_.liveMidiCaptureReset == nullptr
        || hooks_.recordToggleLikeKey == nullptr || hooks_.isCountInActive == nullptr || hooks_.isRecordingInProgress == nullptr
        || hooks_.getTransportPlayheadSamples == nullptr || hooks_.liveMidiSummarizeAllClips == nullptr || hooks_.undoStackSize == nullptr
        || hooks_.invokeUndo == nullptr || hooks_.invokeRedo == nullptr || hooks_.loadProjectFromFile == nullptr
        || hooks_.saveProject == nullptr || hooks_.setLocatorsSamples == nullptr || hooks_.getCycleWrapCount == nullptr
        || hooks_.setCycleEnabled == nullptr || hooks_.seekTransportTo == nullptr || hooks_.setPlaybackActive == nullptr
        || hooks_.deleteTopmostMidiClipLikeUi == nullptr || hooks_.getDeviceSampleRate == nullptr
        || hooks_.liveMidiAttachCaptureSink == nullptr)
    {
        steps_.push_back(Step{ "midi-cycle: hooks missing",
                               [](juce::String& failReason) -> bool {
                                   failReason = "midi-cycle hooks not installed";
                                   return false;
                               },
                               0 });
        return;
    }

    steps_.push_back(Step{ "midi-cycle: copy project to sibling test file",
                           [this, project](juce::String& failReason) -> bool {
                               const juce::File copy = project.getSiblingFile(project.getFileNameWithoutExtension() + "-midicycletest.dalproj");
                               (void)copy.deleteFile();
                               if (!project.copyFileTo(copy))
                               {
                                   failReason = "could not copy project to " + copy.getFullPathName();
                                   return false;
                               }
                               openSaveCloseCopy_ = copy;
                               cycleEvidenceDir_ = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("dal-stability-midi-cycle");
                               (void)cycleEvidenceDir_.createDirectory();
                               appendStabilityRunLine("  test copy: " + copy.getFullPathName());
                               return true;
                           },
                           kSettleDefaultMs });
    steps_.push_back(Step{ "midi-cycle: load test copy",
                           [this](juce::String&) -> bool {
                               hooks_.loadProjectFromFile(openSaveCloseCopy_);
                               return true;
                           },
                           kSettleAfterLoadMs });
    // The stored arrangement extent right after the fixture exists: every Cycle run below must
    // leave it exactly there (a project's saved extent may be far beyond the loop range).
    auto storedExtentAtStart = std::make_shared<std::int64_t>(-1);
    steps_.push_back(Step{ "midi-cycle: build fixture (instrument shell + Lower / Pedal routed rows, MIDI Input = All, filters 1/5/6)",
                           [this, storedExtentAtStart](juce::String& failReason) -> bool {
                               if (!hooks_.liveMidiFixtureSetup(liveMidiInstTid_, liveMidiLowerTid_, liveMidiPedalTid_, failReason))
                               {
                                   return false;
                               }
                               if (hooks_.getStoredArrangementExtentSamples)
                               {
                                   *storedExtentAtStart = hooks_.getStoredArrangementExtentSamples();
                               }
                               appendStabilityRunLine("  fixture: inst=" + juce::String((juce::int64)liveMidiInstTid_) + " lower="
                                                      + juce::String((juce::int64)liveMidiLowerTid_) + " pedal="
                                                      + juce::String((juce::int64)liveMidiPedalTid_)
                                                      + " injection=" + (hooks_.liveMidiInjectUsesRealPort && hooks_.liveMidiInjectUsesRealPort()
                                                                             ? "REAL loopback port" : "bus device-thread entry")
                                                      + " stored extent=" + juce::String((juce::int64)*storedExtentAtStart));
                               return true;
                           },
                           600 });

    const auto inject = [this](const juce::MidiMessage& m) { hooks_.liveMidiInject(m); };
    const auto on = [](const int ch, const int note, const int vel) { return juce::MidiMessage::noteOn(ch, note, (juce::uint8)vel); };
    const auto off = [](const int ch, const int note) { return juce::MidiMessage::noteOff(ch, note, (juce::uint8)0); };
    const auto sr = [this]() { return hooks_.getDeviceSampleRate() > 0.0 ? hooks_.getDeviceSampleRate() : 48000.0; };
    const auto secondsToSamples = [sr](const double s) { return (std::int64_t)std::llround(s * sr()); };
    /// Settle until the transport (playing inside [L, R)) reaches `targetSample`, plus a margin.
    const auto settleUntilPlayhead = [this, sr](const std::int64_t targetSample, const int marginMs) {
        const std::int64_t head = hooks_.getTransportPlayheadSamples();
        const double ms = targetSample > head ? (double)(targetSample - head) / sr() * 1000.0 : 0.0;
        settleOverrideMsForCurrentStep_ = (int)juce::jlimit(50.0, 6000.0, ms + (double)marginMs);
    };
    const auto describeClip = [](const StabilityMidiClipSummary& s) {
        juce::String line;
        line << "start=" << juce::String((juce::int64)s.firstClipStartSamples) << " len="
             << juce::String((juce::int64)s.firstClipLengthSamples) << " notes=[";
        for (const auto& n : s.notes)
        {
            line << "(n" << n.note << " v" << n.velocity << " ch" << n.channel << " @" << juce::String((juce::int64)n.startTick)
                 << " d" << juce::String((juce::int64)n.durationTicks) << ")";
        }
        line << "] cc=[";
        for (const auto& c : s.cc)
        {
            line << "(cc" << c.controller << "=" << c.value << " ch" << c.channel << " @" << juce::String((juce::int64)c.tick) << ")";
        }
        line << "] pb=[";
        for (const auto& b : s.pitchBend)
        {
            line << "(" << b.value << " ch" << b.channel << " @" << juce::String((juce::int64)b.tick) << ")";
        }
        line << "]";
        return line;
    };
    const auto hasNote = [](const StabilityMidiClipSummary& s, const int note) {
        for (const auto& n : s.notes)
        {
            if (n.note == note)
            {
                return true;
            }
        }
        return false;
    };
    const auto noteAt = [](const StabilityMidiClipSummary& s, const int note) -> const StabilityMidiClipSummary::Note* {
        for (const auto& n : s.notes)
        {
            if (n.note == note)
            {
                return &n;
            }
        }
        return nullptr;
    };
    const auto hasCc = [](const StabilityMidiClipSummary& s, const int cc, const int value) {
        for (const auto& c : s.cc)
        {
            if (c.controller == cc && c.value == value)
            {
                return true;
            }
        }
        return false;
    };
    /// Preview geometry check against the lane's own viewport: left edge at the expected pass
    /// start, right edge at the overlay's frame position (same value the playhead line draws).
    const auto checkPreview = [this](const juce::String& label, const std::int64_t expectedPassStart,
                                     juce::String& failReason) -> bool {
        if (hooks_.liveMidiTakePreviewGeometry == nullptr || hooks_.playheadDisplaySamples == nullptr)
        {
            appendStabilityRunLine("  preview (" + label + "): geometry hooks missing - skipped");
            return true;
        }
        int x0 = 0, x1 = 0;
        float originX = 0.0f;
        std::int64_t visStart = 0;
        double spp = 0.0;
        if (!hooks_.liveMidiTakePreviewGeometry(liveMidiInstTid_, x0, x1, originX, visStart, spp) || spp <= 0.0)
        {
            failReason = "no running-take preview on the instrument lane (" + label + ")";
            return false;
        }
        const double display = hooks_.playheadDisplaySamples();
        const double expectedX0 = (double)originX + ((double)expectedPassStart - (double)visStart) / spp;
        const double expectedX1 = (double)originX + (display - (double)visStart) / spp;
        const double headNow = (double)hooks_.getTransportPlayheadSamples();
        appendStabilityRunLine("  preview (" + label + "): x0=" + juce::String(x0) + " expected=" + juce::String(expectedX0, 1)
                               + " | x1=" + juce::String(x1) + " expected(display)=" + juce::String(expectedX1, 1)
                               + " | passStart=" + juce::String((juce::int64)expectedPassStart) + " display="
                               + juce::String(display, 0) + " transport=" + juce::String(headNow, 0) + " visStart="
                               + juce::String((juce::int64)visStart) + " spp=" + juce::String(spp, 2));
        if (std::abs((double)x0 - expectedX0) > 1.5)
        {
            failReason = "preview left edge is not at the current pass start (" + label + ")";
            return false;
        }
        if (std::abs((double)x1 - expectedX1) > 1.5)
        {
            failReason = "preview right edge does not follow the playhead frame position (" + label + ")";
            return false;
        }
        if (x1 < x0 - 1)
        {
            failReason = "preview runs ahead of its start (" + label + ")";
            return false;
        }
        return true;
    };
    const auto png = [this](const juce::String& name) {
        if (hooks_.captureArrangementPng)
        {
            const juce::File f = cycleEvidenceDir_.getChildFile(name);
            (void)f.deleteFile(); // a shorter PNG written over a longer one would keep the old tail
            (void)hooks_.captureArrangementPng(f);
        }
    };
    /// Play one full loop from L with the capture sink reset; reports which (channel, note) sounded.
    const auto playLoopAndReport = [this, sr](const juce::String& label) {
        hooks_.setPlaybackActive(false);
        hooks_.seekTransportTo(cycleLocL_);
        hooks_.setCycleEnabled(true);
        juce::Thread::sleep(120);
        hooks_.liveMidiCaptureReset();
        hooks_.setPlaybackActive(true);
        const double loopMs = (double)(cycleLocR_ - cycleLocL_) / sr() * 1000.0;
        settleOverrideMsForCurrentStep_ = (int)juce::jlimit(500.0, 8000.0, loopMs + 400.0);
        appendStabilityRunLine("  playback (" + label + "): one loop from L, " + juce::String(settleOverrideMsForCurrentStep_) + " ms");
    };
    const auto heard = [this](const int ch, const int note) { return hooks_.liveMidiCapturedNoteCount(ch, note, true); };
    const auto reportHeard = [this, heard](const juce::String& label) {
        juce::String line = "  heard (" + label + "): inst ch1 60=" + juce::String(heard(1, 60)) + " 62=" + juce::String(heard(1, 62))
                            + " 64=" + juce::String(heard(1, 64)) + " 65=" + juce::String(heard(1, 65)) + " | Lower ch2 48="
                            + juce::String(heard(2, 48)) + " 50=" + juce::String(heard(2, 50));
        appendStabilityRunLine(line);
    };

    // ---- 1. Loop [2 s, 5 s), start inside it at 2.5 s; arm inst + Lower; Monitor on inst only; Cycle ON.
    steps_.push_back(Step{ "midi-cycle: instrument row MIDI Input = All MIDI inputs, channel 1 (through the Inspector)",
                           [this](juce::String& failReason) -> bool {
                               if (hooks_.activateTrackLikeHeaderClick && hooks_.inspectorChooseMidiInput && hooks_.inspectorChooseMidiInputChannel)
                               {
                                   hooks_.activateTrackLikeHeaderClick(liveMidiInstTid_);
                                   if (!hooks_.inspectorChooseMidiInput("All MIDI inputs") || !hooks_.inspectorChooseMidiInputChannel(1))
                                   {
                                       failReason = "Inspector pick of All MIDI inputs / channel 1 failed on the instrument row";
                                       return false;
                                   }
                               }
                               else if (!hooks_.liveMidiSetTrackInputDevice || !hooks_.liveMidiSetTrackInputDevice(liveMidiInstTid_, {}, {}))
                               {
                                   failReason = "could not configure the instrument row's MIDI input";
                                   return false;
                               }
                               if (hooks_.describeTrackMidiInputFromSession)
                               {
                                   appendStabilityRunLine("  inst input: " + hooks_.describeTrackMidiInputFromSession(liveMidiInstTid_));
                               }
                               return true;
                           },
                           400 });
    steps_.push_back(Step{ "midi-cycle: set loop [2.0 s, 5.0 s), Cycle ON, seek to 2.5 s (inside the loop), arm inst + Lower, Monitor inst",
                           [this, secondsToSamples](juce::String& failReason) -> bool {
                               cycleLocL_ = secondsToSamples(2.0);
                               cycleLocR_ = secondsToSamples(5.0);
                               hooks_.setLocatorsSamples(cycleLocL_, cycleLocR_);
                               hooks_.setCycleEnabled(true);
                               hooks_.setPlaybackActive(false);
                               hooks_.seekTransportTo(secondsToSamples(2.5));
                               hooks_.liveMidiSetArm(liveMidiInstTid_, true);
                               hooks_.liveMidiSetArm(liveMidiLowerTid_, true);
                               hooks_.liveMidiSetMonitor(liveMidiInstTid_, true);
                               hooks_.liveMidiSetMonitor(liveMidiLowerTid_, false);
                               cycleInstClipsBefore_ = (int)hooks_.liveMidiSummarizeAllClips(liveMidiInstTid_).size();
                               cycleLowerClipsBefore_ = (int)hooks_.liveMidiSummarizeAllClips(liveMidiLowerTid_).size();
                               cycleUndoSizeBefore_ = hooks_.undoStackSize();
                               if (!hooks_.isCycleEnabled || !hooks_.isCycleEnabled())
                               {
                                   failReason = "Cycle could not be enabled";
                                   return false;
                               }
                               appendStabilityRunLine("  loop [" + juce::String((juce::int64)cycleLocL_) + ", " + juce::String((juce::int64)cycleLocR_)
                                                      + ") clipsBefore inst=" + juce::String(cycleInstClipsBefore_) + " lower="
                                                      + juce::String(cycleLowerClipsBefore_) + " undo=" + juce::String(cycleUndoSizeBefore_));
                               return true;
                           },
                           400 });
    steps_.push_back(Step{ "midi-cycle: press Record with Cycle ON -> count-in starts (no refusal)",
                           [this](juce::String& failReason) -> bool {
                               hooks_.liveMidiCaptureReset();
                               cycleWrapsAtRecordStart_ = hooks_.getCycleWrapCount();
                               hooks_.recordToggleLikeKey();
                               juce::Thread::sleep(120);
                               if (!hooks_.isCountInActive())
                               {
                                   failReason = "Record did not start the count-in with Cycle on"
                                                + (hooks_.lastRecordStartRefusal ? " (" + hooks_.lastRecordStartRefusal().replace("\n", " / ") + ")"
                                                                                 : juce::String());
                                   return false;
                               }
                               appendStabilityRunLine("  count-in started with Cycle ON; wrap count at start=" + juce::String((int)cycleWrapsAtRecordStart_));
                               return true;
                           },
                           3700 }); // 8 × 375 ms + 375 ms pre-roll
    steps_.push_back(Step{ "midi-cycle: recording started; preview starts exactly at the record boundary (no 40 px floor)",
                           [this, checkPreview, png](juce::String& failReason) -> bool {
                               if (!hooks_.isRecordingInProgress() || hooks_.isCountInActive())
                               {
                                   failReason = "take did not start after the count-in";
                                   return false;
                               }
                               cycleRecordStart_ = hooks_.liveMidiTakeStartSample ? hooks_.liveMidiTakeStartSample() : 0;
                               appendStabilityRunLine("  record start=" + juce::String((juce::int64)cycleRecordStart_) + " playhead="
                                                      + juce::String((juce::int64)hooks_.getTransportPlayheadSamples()) + " wraps="
                                                      + juce::String((int)hooks_.getCycleWrapCount()));
                               if (hooks_.getCycleWrapCount() != cycleWrapsAtRecordStart_)
                               {
                                   failReason = "a wrap happened during the count-in (loop too short for this check)";
                                   return false;
                               }
                               png("cycle-preview-start.png");
                               return checkPreview("start", cycleRecordStart_, failReason);
                           },
                           300 });
    // ---- 2. Pass 0: notes on inst (ch1) + Lower (ch5 → Force 2), sustain + wheel; key 62 held across wrap 1.
    steps_.push_back(Step{ "midi-cycle: pass 0 - ch1 60 on + ch5 48 on (Lower), sustain down, pitch bend 12000",
                           [this, inject, on](juce::String&) -> bool {
                               inject(on(1, 60, 100));
                               inject(on(5, 48, 90));
                               inject(juce::MidiMessage::controllerEvent(1, 64, 127));
                               inject(juce::MidiMessage::pitchWheel(1, 12000));
                               return true;
                           },
                           500 });
    steps_.push_back(Step{ "midi-cycle: pass 0 - release 60 / 48, hold ch1 62 across the first wrap",
                           [this, inject, on, off, settleUntilPlayhead](juce::String&) -> bool {
                               inject(off(1, 60));
                               inject(off(5, 48));
                               inject(on(1, 62, 96));
                               settleUntilPlayhead(cycleLocR_, 250); // wait for wrap 1
                               return true;
                           },
                           1000 });
    steps_.push_back(Step{ "midi-cycle: after wrap 1 - transport wrapped; preview restarts at the LEFT locator (earlier pass dimmed)",
                           [this, checkPreview, png](juce::String& failReason) -> bool {
                               const std::uint32_t wraps = hooks_.getCycleWrapCount() - cycleWrapsAtRecordStart_;
                               appendStabilityRunLine("  wraps since record start=" + juce::String((int)wraps) + " playhead="
                                                      + juce::String((juce::int64)hooks_.getTransportPlayheadSamples()));
                               if (wraps != 1)
                               {
                                   failReason = "expected exactly one wrap by now, saw " + juce::String((int)wraps);
                                   return false;
                               }
                               png("cycle-preview-after-wrap.png");
                               return checkPreview("after wrap 1", cycleLocL_, failReason);
                           },
                           100 });
    // Viewport changes repaint through the coalesced flush, so each PNG is taken in the NEXT step.
    steps_.push_back(Step{ "midi-cycle: zoom in x1.5: the preview keeps its time anchoring under the new viewport",
                           [this, checkPreview](juce::String& failReason) -> bool {
                               if (hooks_.zoomTimelineLikeWheel == nullptr || hooks_.panTimelineBySamples == nullptr)
                               {
                                   appendStabilityRunLine("  zoom/pan hooks missing - skipped");
                                   return true;
                               }
                               hooks_.zoomTimelineLikeWheel(1.5);
                               return checkPreview("zoomed in x1.5", cycleLocL_, failReason);
                           },
                           150 });
    steps_.push_back(Step{ "midi-cycle: PNG (zoomed), then zoom x4 + scroll -0.5 s: still anchored",
                           [this, checkPreview, png, secondsToSamples](juce::String& failReason) -> bool {
                               if (hooks_.zoomTimelineLikeWheel == nullptr || hooks_.panTimelineBySamples == nullptr)
                               {
                                   return true;
                               }
                               png("cycle-preview-zoomed.png");
                               // Zoom further in so the visible span is shorter than the arrangement, then scroll.
                               hooks_.zoomTimelineLikeWheel(4.0);
                               hooks_.panTimelineBySamples(-secondsToSamples(0.5)); // scroll left by half a second (region stays on screen)
                               return checkPreview("zoomed x4 + panned -0.5 s", cycleLocL_, failReason);
                           },
                           150 });
    steps_.push_back(Step{ "midi-cycle: PNG (panned), then restore the viewport",
                           [this, checkPreview, png, secondsToSamples](juce::String& failReason) -> bool {
                               if (hooks_.zoomTimelineLikeWheel == nullptr || hooks_.panTimelineBySamples == nullptr)
                               {
                                   return true;
                               }
                               png("cycle-preview-panned.png");
                               hooks_.panTimelineBySamples(secondsToSamples(0.5));
                               hooks_.zoomTimelineLikeWheel(1.0 / 4.0);
                               hooks_.zoomTimelineLikeWheel(1.0 / 1.5);
                               return checkPreview("restored", cycleLocL_, failReason);
                           },
                           150 });
    steps_.push_back(Step{ "midi-cycle: pass 1 - release 62 (+0.3 s), ch1 64 on + ch5 50 on",
                           [this, inject, on, off](juce::String&) -> bool {
                               inject(off(1, 62));
                               inject(on(1, 64, 100));
                               inject(on(5, 50, 90));
                               return true;
                           },
                           400 });
    steps_.push_back(Step{ "midi-cycle: pass 1 - release 64 / 50; wait for wrap 2",
                           [this, inject, off, settleUntilPlayhead](juce::String&) -> bool {
                               inject(off(1, 64));
                               inject(off(5, 50));
                               settleUntilPlayhead(cycleLocR_, 250);
                               return true;
                           },
                           1000 });
    steps_.push_back(Step{ "midi-cycle: pass 2 - controller-only on inst (pedal release), Lower silent; wait for wrap 3",
                           [this, inject, settleUntilPlayhead](juce::String& failReason) -> bool {
                               const std::uint32_t wraps = hooks_.getCycleWrapCount() - cycleWrapsAtRecordStart_;
                               if (wraps != 2)
                               {
                                   failReason = "expected two wraps by now, saw " + juce::String((int)wraps);
                                   return false;
                               }
                               inject(juce::MidiMessage::controllerEvent(1, 64, 0));
                               settleUntilPlayhead(cycleLocR_, 250);
                               return true;
                           },
                           1000 });
    steps_.push_back(Step{ "midi-cycle: pass 3 (partial) - ch1 65 on, off; Stop inside the pass",
                           [this, inject, on, off, settleUntilPlayhead, secondsToSamples](juce::String& failReason) -> bool {
                               const std::uint32_t wraps = hooks_.getCycleWrapCount() - cycleWrapsAtRecordStart_;
                               if (wraps != 3)
                               {
                                   failReason = "expected three wraps by now, saw " + juce::String((int)wraps);
                                   return false;
                               }
                               inject(on(1, 65, 100));
                               juce::Thread::sleep(250);
                               inject(off(1, 65));
                               settleUntilPlayhead(cycleLocL_ + secondsToSamples(0.8), 0);
                               return true;
                           },
                           300 });
    steps_.push_back(Step{ "midi-cycle: Stop (Record key while recording) -> all passes committed in ONE undo step",
                           [this](juce::String& failReason) -> bool {
                               liveMidiStopPlayhead_ = hooks_.getTransportPlayheadSamples();
                               hooks_.recordToggleLikeKey();
                               juce::Thread::sleep(200);
                               if (hooks_.isRecordingInProgress())
                               {
                                   failReason = "take still active after stop";
                                   return false;
                               }
                               const int undoNow = hooks_.undoStackSize();
                               appendStabilityRunLine("  stop playhead=" + juce::String((juce::int64)liveMidiStopPlayhead_) + " wraps="
                                                      + juce::String((int)(hooks_.getCycleWrapCount() - cycleWrapsAtRecordStart_))
                                                      + " undo before=" + juce::String(cycleUndoSizeBefore_) + " after=" + juce::String(undoNow));
                               if (undoNow != cycleUndoSizeBefore_ + 1)
                               {
                                   failReason = "the recording run must be exactly one undo step";
                                   return false;
                               }
                               return true;
                           },
                           500 });
    // ---- 3. Verify the takes: one clip per pass and row, windows, boundary handling, stack order.
    steps_.push_back(Step{ "midi-cycle: verify takes - 4 passes per row, windows [start,R) [L,R) [L,R) [L,stop), held key / pedal / wheel across wraps",
                           [this, describeClip, hasNote, noteAt, hasCc, sr](juce::String& failReason) -> bool {
                               const auto inst = hooks_.liveMidiSummarizeAllClips(liveMidiInstTid_);
                               const auto lower = hooks_.liveMidiSummarizeAllClips(liveMidiLowerTid_);
                               for (size_t i = (size_t)cycleInstClipsBefore_; i < inst.size(); ++i)
                               {
                                   appendStabilityRunLine("  inst take " + juce::String((int)(i - (size_t)cycleInstClipsBefore_)) + ": " + describeClip(inst[i]));
                               }
                               for (size_t i = (size_t)cycleLowerClipsBefore_; i < lower.size(); ++i)
                               {
                                   appendStabilityRunLine("  lower take " + juce::String((int)(i - (size_t)cycleLowerClipsBefore_)) + ": " + describeClip(lower[i]));
                               }
                               if ((int)inst.size() != cycleInstClipsBefore_ + 4 || (int)lower.size() != cycleLowerClipsBefore_ + 4)
                               {
                                   failReason = "expected 4 new takes on the instrument row and 4 on the Lower row (silent passes included)";
                                   return false;
                               }
                               const StabilityMidiClipSummary& p0 = inst[(size_t)cycleInstClipsBefore_];
                               const StabilityMidiClipSummary& p1 = inst[(size_t)cycleInstClipsBefore_ + 1];
                               const StabilityMidiClipSummary& p2 = inst[(size_t)cycleInstClipsBefore_ + 2];
                               const StabilityMidiClipSummary& p3 = inst[(size_t)cycleInstClipsBefore_ + 3];
                               const std::int64_t tol = (std::int64_t)(sr() * 0.05);
                               const auto windowIs = [tol](const StabilityMidiClipSummary& s, const std::int64_t start, const std::int64_t end) {
                                   return std::abs((long long)(s.firstClipStartSamples - start)) <= tol
                                          && std::abs((long long)(s.firstClipStartSamples + s.firstClipLengthSamples - end)) <= tol;
                               };
                               if (!windowIs(p0, cycleRecordStart_, cycleLocR_) || !windowIs(p1, cycleLocL_, cycleLocR_)
                                   || !windowIs(p2, cycleLocL_, cycleLocR_) || p3.firstClipStartSamples != cycleLocL_
                                   || p3.firstClipLengthSamples <= 0 || p3.firstClipStartSamples + p3.firstClipLengthSamples > cycleLocR_)
                               {
                                   failReason = "pass windows do not match [start,R) [L,R) [L,R) [L,stop)";
                                   return false;
                               }
                               // Pass 0: 60, 62 closed at the pass end, pedal down + released at the end, wheel.
                               const auto* n62p0 = noteAt(p0, 62);
                               const std::int64_t p0EndTick = (std::int64_t)std::llround((double)p0.firstClipLengthSamples / sr() * p0.bpm / 60.0 * p0.ticksPerQuarter);
                               if (!hasNote(p0, 60) || n62p0 == nullptr
                                   || std::abs((long long)(n62p0->startTick + n62p0->durationTicks - p0EndTick)) > 40
                                   || !hasCc(p0, 64, 127) || p0.pitchBend.empty())
                               {
                                   failReason = "pass 0 must hold 60, 62 ending on the pass end, CC64 down and the wheel";
                                   return false;
                               }
                               // Pass 1: 62 continues from tick 0 with velocity 96 / ch1, then 64; pedal + wheel restated at 0.
                               const auto* n62p1 = noteAt(p1, 62);
                               bool pbAtZero = false;
                               for (const auto& b : p1.pitchBend)
                               {
                                   pbAtZero = pbAtZero || (b.tick == 0 && b.value == 12000);
                               }
                               bool ccAtZero = false;
                               for (const auto& c : p1.cc)
                               {
                                   ccAtZero = ccAtZero || (c.tick == 0 && c.controller == 64 && c.value == 127);
                               }
                               if (n62p1 == nullptr || n62p1->startTick != 0 || n62p1->velocity != 96 || n62p1->channel != 1 || !hasNote(p1, 64)
                                   || !pbAtZero || !ccAtZero || hasNote(p1, 60))
                               {
                                   failReason = "pass 1 must continue 62 from tick 0 (velocity / channel kept), hold 64, restate pedal + wheel";
                                   return false;
                               }
                               // Pass 2: controller-only (pedal release), no notes.
                               if (!p2.notes.empty() || !hasCc(p2, 64, 0))
                               {
                                   failReason = "pass 2 must be a controller-only take (pedal release, no notes)";
                                   return false;
                               }
                               // Pass 3: 65 only, partial window.
                               if (!hasNote(p3, 65) || hasNote(p3, 64) || hasNote(p3, 62))
                               {
                                   failReason = "pass 3 must hold only note 65";
                                   return false;
                               }
                               // Lower: 48 / 50 / silent / silent (silent passes still exist as masking takes).
                               const StabilityMidiClipSummary& l0 = lower[(size_t)cycleLowerClipsBefore_];
                               const StabilityMidiClipSummary& l1 = lower[(size_t)cycleLowerClipsBefore_ + 1];
                               const StabilityMidiClipSummary& l2 = lower[(size_t)cycleLowerClipsBefore_ + 2];
                               const StabilityMidiClipSummary& l3 = lower[(size_t)cycleLowerClipsBefore_ + 3];
                               if (!hasNote(l0, 48) || !hasNote(l1, 50) || !l2.notes.empty() || !l3.notes.empty() || l3.firstClipLengthSamples <= 0)
                               {
                                   failReason = "Lower passes must be 48 / 50 / silent full / silent partial";
                                   return false;
                               }
                               if (l0.notes[0].channel != 5)
                               {
                                   failReason = "Lower take must keep its RECEIVED channel 5";
                                   return false;
                               }
                               return true;
                           },
                           200 });
    // ---- 4. Undo / redo of the whole run.
    steps_.push_back(Step{ "midi-cycle: Undo removes every pass on both rows; Redo restores them",
                           [this](juce::String& failReason) -> bool {
                               hooks_.invokeUndo();
                               juce::Thread::sleep(150);
                               const int instAfterUndo = (int)hooks_.liveMidiSummarizeAllClips(liveMidiInstTid_).size();
                               const int lowerAfterUndo = (int)hooks_.liveMidiSummarizeAllClips(liveMidiLowerTid_).size();
                               hooks_.invokeRedo();
                               juce::Thread::sleep(150);
                               const int instAfterRedo = (int)hooks_.liveMidiSummarizeAllClips(liveMidiInstTid_).size();
                               const int lowerAfterRedo = (int)hooks_.liveMidiSummarizeAllClips(liveMidiLowerTid_).size();
                               appendStabilityRunLine("  undo -> inst/lower clips " + juce::String(instAfterUndo) + "/" + juce::String(lowerAfterUndo)
                                                      + "; redo -> " + juce::String(instAfterRedo) + "/" + juce::String(lowerAfterRedo));
                               if (instAfterUndo != cycleInstClipsBefore_ || lowerAfterUndo != cycleLowerClipsBefore_
                                   || instAfterRedo != cycleInstClipsBefore_ + 4 || lowerAfterRedo != cycleLowerClipsBefore_ + 4)
                               {
                                   failReason = "undo/redo of the recording run is not one coherent step";
                                   return false;
                               }
                               return true;
                           },
                           300 });
    // ---- 5. Playback selection: topmost take wins per source track.
    steps_.push_back(Step{ "midi-cycle: play one loop - only the topmost takes sound (inst: 65 from pass 3, rest masked; Lower: masked by its silent passes)",
                           [this, playLoopAndReport](juce::String&) -> bool {
                               playLoopAndReport("full stack");
                               return true;
                           },
                           100 });
    steps_.push_back(Step{ "midi-cycle: verify playback of the full stack",
                           [this, heard, reportHeard](juce::String& failReason) -> bool {
                               hooks_.setPlaybackActive(false);
                               reportHeard("full stack");
                               if (heard(1, 65) < 1 || heard(1, 60) != 0 || heard(1, 62) != 0 || heard(1, 64) != 0
                                   || heard(2, 48) != 0 || heard(2, 50) != 0)
                               {
                                   failReason = "the topmost takes must be the only audible ones (inst 65; Lower nothing)";
                                   return false;
                               }
                               return true;
                           },
                           200 });
    steps_.push_back(Step{ "midi-cycle: delete the topmost take on inst and Lower (pass 3) -> pass 2 wins (controller-only / silent): still nothing but no 65",
                           [this, playLoopAndReport](juce::String& failReason) -> bool {
                               if (!hooks_.deleteTopmostMidiClipLikeUi(liveMidiInstTid_) || !hooks_.deleteTopmostMidiClipLikeUi(liveMidiLowerTid_))
                               {
                                   failReason = "could not delete the topmost take";
                                   return false;
                               }
                               playLoopAndReport("after deleting pass 3");
                               return true;
                           },
                           100 });
    steps_.push_back(Step{ "midi-cycle: verify playback after deleting pass 3",
                           [this, heard, reportHeard](juce::String& failReason) -> bool {
                               hooks_.setPlaybackActive(false);
                               reportHeard("after deleting pass 3");
                               if (heard(1, 65) != 0 || heard(1, 64) != 0 || heard(1, 62) != 0 || heard(2, 50) != 0)
                               {
                                   failReason = "after deleting pass 3 the silent / controller-only pass 2 must mask everything below";
                                   return false;
                               }
                               return true;
                           },
                           200 });
    steps_.push_back(Step{ "midi-cycle: delete pass 2 on both rows -> pass 1 is heard (inst 62 + 64, Lower 50); pass 0 stays masked",
                           [this, playLoopAndReport](juce::String& failReason) -> bool {
                               if (!hooks_.deleteTopmostMidiClipLikeUi(liveMidiInstTid_) || !hooks_.deleteTopmostMidiClipLikeUi(liveMidiLowerTid_))
                               {
                                   failReason = "could not delete the topmost take";
                                   return false;
                               }
                               playLoopAndReport("after deleting pass 2");
                               return true;
                           },
                           100 });
    steps_.push_back(Step{ "midi-cycle: verify playback after deleting pass 2",
                           [this, heard, reportHeard](juce::String& failReason) -> bool {
                               hooks_.setPlaybackActive(false);
                               reportHeard("after deleting pass 2");
                               if (heard(1, 62) < 1 || heard(1, 64) < 1 || heard(2, 50) < 1 || heard(1, 60) != 0 || heard(2, 48) != 0 || heard(1, 65) != 0)
                               {
                                   failReason = "after deleting pass 2 the previous take (pass 1) must be heard and pass 0 stay masked";
                                   return false;
                               }
                               return true;
                           },
                           200 });
    steps_.push_back(Step{ "midi-cycle: Undo the four deletes -> full stack again (only 65 audible)",
                           [this, playLoopAndReport](juce::String& failReason) -> bool {
                               for (int i = 0; i < 4; ++i)
                               {
                                   hooks_.invokeUndo();
                                   juce::Thread::sleep(80);
                               }
                               if ((int)hooks_.liveMidiSummarizeAllClips(liveMidiInstTid_).size() != cycleInstClipsBefore_ + 4
                                   || (int)hooks_.liveMidiSummarizeAllClips(liveMidiLowerTid_).size() != cycleLowerClipsBefore_ + 4)
                               {
                                   failReason = "undo did not restore the deleted takes";
                                   return false;
                               }
                               playLoopAndReport("after undoing the deletes");
                               return true;
                           },
                           100 });
    steps_.push_back(Step{ "midi-cycle: verify playback after undo (65 only), then Redo the deletes and verify pass 1 again",
                           [this, heard, reportHeard, playLoopAndReport](juce::String& failReason) -> bool {
                               hooks_.setPlaybackActive(false);
                               reportHeard("after undo");
                               if (heard(1, 65) < 1 || heard(1, 64) != 0 || heard(1, 62) != 0 || heard(2, 50) != 0)
                               {
                                   failReason = "undo must restore the original audible result";
                                   return false;
                               }
                               for (int i = 0; i < 4; ++i)
                               {
                                   hooks_.invokeRedo();
                                   juce::Thread::sleep(80);
                               }
                               playLoopAndReport("after redoing the deletes");
                               return true;
                           },
                           100 });
    steps_.push_back(Step{ "midi-cycle: verify playback after redo (pass 1 heard); Undo the deletes again for the remaining checks",
                           [this, heard, reportHeard](juce::String& failReason) -> bool {
                               hooks_.setPlaybackActive(false);
                               reportHeard("after redo");
                               if (heard(1, 64) < 1 || heard(2, 50) < 1 || heard(1, 65) != 0)
                               {
                                   failReason = "redo must restore the deleted state's audible result";
                                   return false;
                               }
                               for (int i = 0; i < 4; ++i)
                               {
                                   hooks_.invokeUndo();
                                   juce::Thread::sleep(80);
                               }
                               return (int)hooks_.liveMidiSummarizeAllClips(liveMidiInstTid_).size() == cycleInstClipsBefore_ + 4;
                           },
                           300 });
    // ---- 6. MIDI editor open across delete / undo / move of the topmost take.
    if (hooks_.openMidiEditorOnFirstClipOfTrack && hooks_.closeMidiEditor && hooks_.isMidiEditorOpen && hooks_.moveTopmostMidiClipLikeUi)
    {
        steps_.push_back(Step{ "midi-cycle: open the MIDI editor on the inst row, delete the topmost take, undo, move the topmost take, undo - no crash",
                               [this, secondsToSamples](juce::String& failReason) -> bool {
                                   if (!hooks_.openMidiEditorOnFirstClipOfTrack(liveMidiInstTid_))
                                   {
                                       failReason = "MIDI editor did not open";
                                       return false;
                                   }
                                   juce::Thread::sleep(150);
                                   if (!hooks_.deleteTopmostMidiClipLikeUi(liveMidiInstTid_))
                                   {
                                       failReason = "delete with open editor failed";
                                       return false;
                                   }
                                   juce::Thread::sleep(150);
                                   hooks_.invokeUndo();
                                   juce::Thread::sleep(150);
                                   if (!hooks_.moveTopmostMidiClipLikeUi(liveMidiInstTid_, secondsToSamples(0.1)))
                                   {
                                       failReason = "move with open editor failed";
                                       return false;
                                   }
                                   juce::Thread::sleep(150);
                                   hooks_.invokeUndo();
                                   juce::Thread::sleep(150);
                                   appendStabilityRunLine(juce::String("  editor open after delete/undo/move/undo: ") + (hooks_.isMidiEditorOpen() ? "yes" : "no (closed by the edit - acceptable)"));
                                   hooks_.closeMidiEditor();
                                   return (int)hooks_.liveMidiSummarizeAllClips(liveMidiInstTid_).size() == cycleInstClipsBefore_ + 4;
                               },
                               400 });
    }
    // ---- 7. Save / reload keeps content, windows and stack order; the audible result is the same.
    steps_.push_back(Step{ "midi-cycle: save, reload, verify clip count + stack order survived",
                           [this, describeClip](juce::String& failReason) -> bool {
                               const auto before = hooks_.liveMidiSummarizeAllClips(liveMidiInstTid_);
                               hooks_.saveProject();
                               juce::Thread::sleep(200);
                               hooks_.loadProjectFromFile(openSaveCloseCopy_);
                               juce::Thread::sleep(600);
                               const auto after = hooks_.liveMidiSummarizeAllClips(liveMidiInstTid_);
                               const auto lowerAfter = hooks_.liveMidiSummarizeAllClips(liveMidiLowerTid_);
                               if (after.size() != before.size() || (int)lowerAfter.size() != cycleLowerClipsBefore_ + 4)
                               {
                                   failReason = "clip count changed across save/reload";
                                   return false;
                               }
                               for (size_t i = 0; i < before.size(); ++i)
                               {
                                   if (before[i].firstClipStartSamples != after[i].firstClipStartSamples
                                       || before[i].firstClipLengthSamples != after[i].firstClipLengthSamples
                                       || before[i].notes.size() != after[i].notes.size() || before[i].cc.size() != after[i].cc.size()
                                       || before[i].pitchBend.size() != after[i].pitchBend.size())
                                   {
                                       failReason = "stack order / content changed across save/reload at index " + juce::String((int)i) + ": "
                                                    + describeClip(before[i]) + " vs " + describeClip(after[i]);
                                       return false;
                                   }
                               }
                               if (!hooks_.liveMidiAttachCaptureSink(liveMidiInstTid_, failReason))
                               {
                                   return false;
                               }
                               return true;
                           },
                           kSettleAfterLoadMs });
    steps_.push_back(Step{ "midi-cycle: play one loop after reload",
                           [this, playLoopAndReport](juce::String&) -> bool {
                               playLoopAndReport("after reload");
                               return true;
                           },
                           100 });
    steps_.push_back(Step{ "midi-cycle: verify playback after reload (65 only)",
                           [this, heard, reportHeard](juce::String& failReason) -> bool {
                               hooks_.setPlaybackActive(false);
                               reportHeard("after reload");
                               if (heard(1, 65) < 1 || heard(1, 64) != 0 || heard(1, 62) != 0 || heard(1, 60) != 0 || heard(2, 50) != 0 || heard(2, 48) != 0)
                               {
                                   failReason = "the reloaded project must select the same takes";
                                   return false;
                               }
                               return true;
                           },
                           200 });
    // ---- 8. Offline mixdown and a fresh proxy sequencer select the same events.
    if (hooks_.runMixdownBlocking)
    {
        steps_.push_back(Step{ "midi-cycle: offline WAV mixdown delivers the same selection (65 only)",
                               [this, heard, reportHeard](juce::String& failReason) -> bool {
                                   hooks_.setPlaybackActive(false);
                                   hooks_.setCycleEnabled(true);
                                   hooks_.liveMidiCaptureReset();
                                   const juce::File out = cycleEvidenceDir_.getChildFile("midi-cycle-mixdown.wav");
                                   (void)out.deleteFile();
                                   const juce::Result r = hooks_.runMixdownBlocking(out, false);
                                   appendStabilityRunLine("  mixdown: " + juce::String(r.wasOk() ? "ok" : r.getErrorMessage()) + " size="
                                                          + juce::String((juce::int64)out.getSize()));
                                   reportHeard("offline mixdown");
                                   (void)out.deleteFile();
                                   if (!r.wasOk())
                                   {
                                       failReason = "mixdown failed: " + r.getErrorMessage();
                                       return false;
                                   }
                                   if (heard(1, 65) < 1 || heard(1, 64) != 0 || heard(1, 62) != 0 || heard(1, 60) != 0 || heard(2, 50) != 0 || heard(2, 48) != 0)
                                   {
                                       failReason = "the offline mixdown delivered a different selection than realtime playback";
                                       return false;
                                   }
                                   return true;
                               },
                               300 });
    }
    if (hooks_.proxySequencerNoteOnsForDestination)
    {
        steps_.push_back(Step{ "midi-cycle: a fresh proxy render (P1C snapshot + offline sequencer) selects the same events",
                               [this](juce::String& failReason) -> bool {
                                   const juce::String ons = hooks_.proxySequencerNoteOnsForDestination(liveMidiInstTid_);
                                   appendStabilityRunLine("  proxy sequencer note-ons: " + ons);
                                   const bool ok = ons.contains("1:65@") && !ons.contains("1:64@") && !ons.contains("1:62@")
                                                   && !ons.contains("1:60@") && !ons.contains("2:50@") && !ons.contains("2:48@");
                                   if (!ok)
                                   {
                                       failReason = "the proxy bake selects different events than realtime";
                                       return false;
                                   }
                                   return true;
                               },
                               100 });
    }
    // ---- 9. While a row records, its own earlier takes are silent (no doubling of the live
    //         performance); the live input is still heard; Record alone (not Monitor) decides.
    steps_.push_back(Step{ "midi-cycle: second run - Record on inst again (Monitor on): press Record",
                           [this](juce::String& failReason) -> bool {
                               hooks_.setPlaybackActive(false);
                               hooks_.setCycleEnabled(true);
                               hooks_.seekTransportTo(cycleLocL_);
                               // Make the Lower row audible first (delete its two silent masking takes) so the
                               // run can prove that OTHER source rows into the same instrument keep playing
                               // while the recording row's own clips are silent. Undone after the run.
                               if (!hooks_.deleteTopmostMidiClipLikeUi(liveMidiLowerTid_) || !hooks_.deleteTopmostMidiClipLikeUi(liveMidiLowerTid_))
                               {
                                   failReason = "could not uncover the Lower row's take";
                                   return false;
                               }
                               hooks_.liveMidiSetArm(liveMidiInstTid_, true);
                               hooks_.liveMidiSetArm(liveMidiLowerTid_, false);
                               hooks_.liveMidiSetMonitor(liveMidiInstTid_, true);
                               cycleUndoSizeBefore_ = hooks_.undoStackSize();
                               hooks_.recordToggleLikeKey();
                               juce::Thread::sleep(120);
                               if (!hooks_.isCountInActive())
                               {
                                   failReason = "second Record did not start a count-in";
                                   return false;
                               }
                               return true;
                           },
                           3700 });
    steps_.push_back(Step{ "midi-cycle: second run - recording: play ch1 67 live; the row's own earlier take (65) must NOT sound",
                           [this, inject, on, off, sr](juce::String& failReason) -> bool {
                               if (!hooks_.isRecordingInProgress())
                               {
                                   failReason = "second take did not start";
                                   return false;
                               }
                               hooks_.liveMidiCaptureReset();
                               inject(on(1, 67, 100));
                               juce::Thread::sleep(200);
                               inject(off(1, 67));
                               const double loopMs = (double)(cycleLocR_ - cycleLocL_) / sr() * 1000.0;
                               settleOverrideMsForCurrentStep_ = (int)juce::jlimit(500.0, 8000.0, loopMs + 300.0);
                               return true;
                           },
                           100 });
    steps_.push_back(Step{ "midi-cycle: second run - verify (65 silent while recording, 67 heard live), Stop, Undo the run",
                           [this, heard, reportHeard](juce::String& failReason) -> bool {
                               reportHeard("while recording the second run");
                               const int live67 = heard(1, 67);
                               const int old65 = heard(1, 65);
                               const int lower50 = heard(2, 50);
                               appendStabilityRunLine("  live 67 heard=" + juce::String(live67) + " earlier take 65 heard=" + juce::String(old65)
                                                      + " other row (Lower 50) heard=" + juce::String(lower50));
                               hooks_.recordToggleLikeKey();
                               juce::Thread::sleep(200);
                               if (hooks_.isRecordingInProgress())
                               {
                                   failReason = "second take still active after stop";
                                   return false;
                               }
                               if (old65 != 0)
                               {
                                   failReason = "the recording row's earlier take doubled the live performance";
                                   return false;
                               }
                               if (live67 < 1)
                               {
                                   failReason = "the live note was not heard while recording";
                                   return false;
                               }
                               if (lower50 < 1)
                               {
                                   failReason = "another source row into the same instrument stopped playing during the take";
                                   return false;
                               }
                               const int clipsNow = (int)hooks_.liveMidiSummarizeAllClips(liveMidiInstTid_).size();
                               if (clipsNow <= cycleInstClipsBefore_ + 4 || hooks_.undoStackSize() != cycleUndoSizeBefore_ + 1)
                               {
                                   failReason = "the second run did not add its passes as one undo step";
                                   return false;
                               }
                               // Undo the run and the two Lower deletes (back to the full stacks).
                               for (int i = 0; i < 3; ++i)
                               {
                                   hooks_.invokeUndo();
                                   juce::Thread::sleep(120);
                               }
                               return (int)hooks_.liveMidiSummarizeAllClips(liveMidiInstTid_).size() == cycleInstClipsBefore_ + 4
                                      && (int)hooks_.liveMidiSummarizeAllClips(liveMidiLowerTid_).size() == cycleLowerClipsBefore_ + 4;
                           },
                           300 });
    steps_.push_back(Step{ "midi-cycle: after the run the row's clips play again (65 heard with Monitor still on, nothing recording)",
                           [this, playLoopAndReport](juce::String&) -> bool {
                               playLoopAndReport("after the second run");
                               return true;
                           },
                           100 });
    steps_.push_back(Step{ "midi-cycle: verify playback after the second run",
                           [this, heard, reportHeard](juce::String& failReason) -> bool {
                               hooks_.setPlaybackActive(false);
                               reportHeard("after the second run");
                               if (heard(1, 65) < 1)
                               {
                                   failReason = "clip suppression must end with the take";
                                   return false;
                               }
                               return true;
                           },
                           200 });
    // ---- 10. Simultaneous audio + MIDI cycle recording: the audio slices and the MIDI passes get
    //          the same pass boundaries (same start boundary, locators and stop wrap count), in ONE
    //          undo step. Skipped with a logged reason when no audio input is available.
    if (hooks_.armAudioTrackForRecording && hooks_.audioClipWindowsForTrack && hooks_.listAllTracks)
    {
        auto audioTid = std::make_shared<TrackId>(kInvalidTrackId);
        auto audioBefore = std::make_shared<int>(0);
        auto started = std::make_shared<bool>(false);
        steps_.push_back(Step{ "midi-cycle: combined audio + MIDI cycle take - arm an audio track + inst, seek to L, Record",
                               [this, audioTid, audioBefore, started](juce::String&) -> bool {
                                   for (const StabilityTrackInfo& t : hooks_.listAllTracks())
                                   {
                                       if (t.kindName == "audio" && *audioTid == kInvalidTrackId)
                                       {
                                           *audioTid = t.id;
                                       }
                                   }
                                   if (*audioTid == kInvalidTrackId)
                                   {
                                       appendStabilityRunLine("  no audio track in this project - combined cycle take skipped");
                                       return true;
                                   }
                                   hooks_.setPlaybackActive(false);
                                   hooks_.setCycleEnabled(true);
                                   hooks_.seekTransportTo(cycleLocL_);
                                   *audioBefore = (int)hooks_.audioClipWindowsForTrack(*audioTid).size();
                                   cycleUndoSizeBefore_ = hooks_.undoStackSize();
                                   hooks_.armAudioTrackForRecording(*audioTid);
                                   hooks_.liveMidiSetArm(liveMidiInstTid_, true);
                                   hooks_.liveMidiSetArm(liveMidiLowerTid_, false);
                                   hooks_.recordToggleLikeKey();
                                   juce::Thread::sleep(200);
                                   *started = hooks_.isCountInActive();
                                   appendStabilityRunLine(juce::String("  combined cycle count-in started: ")
                                                          + (*started ? "yes" : "no (audio input unavailable on this device - skipped)"));
                                   if (!*started)
                                   {
                                       hooks_.armAudioTrackForRecording(kInvalidTrackId);
                                   }
                                   return true;
                               },
                               3700 });
        steps_.push_back(Step{ "midi-cycle: combined cycle - pass 0: ch1 67; wait for the wrap",
                               [this, started, inject, on, off, settleUntilPlayhead](juce::String& failReason) -> bool {
                                   if (!*started)
                                   {
                                       return true;
                                   }
                                   if (!hooks_.isRecordingInProgress())
                                   {
                                       failReason = "combined cycle take did not start";
                                       return false;
                                   }
                                   cycleWrapsAtRecordStart_ = hooks_.getCycleWrapCount();
                                   inject(on(1, 67, 100));
                                   juce::Thread::sleep(200);
                                   inject(off(1, 67));
                                   settleUntilPlayhead(cycleLocR_, 250);
                                   return true;
                               },
                               300 });
        steps_.push_back(Step{ "midi-cycle: combined cycle - pass 1: ch1 69; Stop inside pass 1",
                               [this, started, inject, on, off, settleUntilPlayhead, secondsToSamples](juce::String& failReason) -> bool {
                                   if (!*started)
                                   {
                                       return true;
                                   }
                                   if (hooks_.getCycleWrapCount() - cycleWrapsAtRecordStart_ != 1)
                                   {
                                       failReason = "expected one wrap in the combined cycle take";
                                       return false;
                                   }
                                   inject(on(1, 69, 100));
                                   juce::Thread::sleep(200);
                                   inject(off(1, 69));
                                   settleUntilPlayhead(cycleLocL_ + secondsToSamples(0.8), 0);
                                   return true;
                               },
                               300 });
        steps_.push_back(Step{ "midi-cycle: combined cycle - Stop: audio slices and MIDI passes share the pass boundaries, ONE undo step; undo",
                               [this, started, audioTid, audioBefore, sr, describeClip](juce::String& failReason) -> bool {
                                   if (!*started)
                                   {
                                       return true;
                                   }
                                   const int midiBefore = cycleInstClipsBefore_ + 4;
                                   hooks_.recordToggleLikeKey();
                                   juce::Thread::sleep(600);
                                   hooks_.armAudioTrackForRecording(kInvalidTrackId);
                                   const auto audio = hooks_.audioClipWindowsForTrack(*audioTid);
                                   const auto midi = hooks_.liveMidiSummarizeAllClips(liveMidiInstTid_);
                                   const int undoDelta = hooks_.undoStackSize() - cycleUndoSizeBefore_;
                                   juce::String audioLine = "  combined cycle: audio clips " + juce::String(*audioBefore) + "->" + juce::String((int)audio.size()) + " [";
                                   for (size_t i = 0; i < audio.size() && (int)i < (int)audio.size() - *audioBefore; ++i)
                                   {
                                       audioLine << "(start=" << juce::String((juce::int64)audio[i].first) << " len=" << juce::String((juce::int64)audio[i].second) << ")";
                                   }
                                   audioLine << "] midi clips " << midiBefore << "->" << (int)midi.size() << " undo +" << undoDelta;
                                   appendStabilityRunLine(audioLine);
                                   for (size_t i = (size_t)midiBefore; i < midi.size(); ++i)
                                   {
                                       appendStabilityRunLine("  combined cycle midi pass " + juce::String((int)(i - (size_t)midiBefore)) + ": " + describeClip(midi[i]));
                                   }
                                   const int newAudio = (int)audio.size() - *audioBefore;
                                   const int newMidi = (int)midi.size() - midiBefore;
                                   if (newAudio != 2 || newMidi != 2 || undoDelta != 1)
                                   {
                                       failReason = "combined cycle take must add 2 audio slices and 2 MIDI passes in exactly one undo step";
                                       return false;
                                   }
                                   // Audio clips are newest-first; MIDI passes oldest-first. Lengths must be IDENTICAL
                                   // (one engine-acknowledged stop boundary); only the placements differ by their
                                   // deliberate compensations (audio start = raw + latency-store offset).
                                   const std::int64_t audioPass0Len = audio[1].second;
                                   const std::int64_t audioPass1Len = audio[0].second;
                                   const std::int64_t midiPass0Len = midi[(size_t)midiBefore].firstClipLengthSamples;
                                   const std::int64_t midiPass1Len = midi[(size_t)midiBefore + 1].firstClipLengthSamples;
                                   appendStabilityRunLine("  pass lengths: audio " + juce::String((juce::int64)audioPass0Len) + " / " + juce::String((juce::int64)audioPass1Len)
                                                          + " midi " + juce::String((juce::int64)midiPass0Len) + " / " + juce::String((juce::int64)midiPass1Len)
                                                          + " | audio placement offset=" + juce::String((juce::int64)(hooks_.recordingPlacementOffsetSamples ? hooks_.recordingPlacementOffsetSamples() : 0))
                                                          + " | " + (hooks_.lastRecordRunBoundaries ? hooks_.lastRecordRunBoundaries() : juce::String()));
                                   if (audioPass0Len != midiPass0Len || audioPass1Len != midiPass1Len)
                                   {
                                       failReason = "audio slices and MIDI passes do not share the exact pass boundaries";
                                       return false;
                                   }
                                   bool has67 = false, has69 = false;
                                   for (const auto& n : midi[(size_t)midiBefore].notes) { has67 = has67 || n.note == 67; }
                                   for (const auto& n : midi[(size_t)midiBefore + 1].notes) { has69 = has69 || n.note == 69; }
                                   if (!has67 || !has69)
                                   {
                                       failReason = "combined cycle MIDI passes do not hold their notes (67 in pass 0, 69 in pass 1)";
                                       return false;
                                   }
                                   hooks_.invokeUndo();
                                   juce::Thread::sleep(300);
                                   const bool restored = (int)hooks_.audioClipWindowsForTrack(*audioTid).size() == *audioBefore
                                                         && (int)hooks_.liveMidiSummarizeAllClips(liveMidiInstTid_).size() == midiBefore;
                                   if (!restored)
                                   {
                                       failReason = "one Undo did not remove both the audio slices and the MIDI passes";
                                       return false;
                                   }
                                   return true;
                               },
                               400 });
    }
    // ---- 11. Recording past the previous arrangement end (no fixed headroom): MIDI-only, audio-only
    //          and combined runs cross the old end with a running transport; the extent follows the
    //          playhead during the run and keeps only what the result needs afterwards. Cycle runs
    //          above must not have lengthened the project.
    if (hooks_.getStoredArrangementExtentSamples && hooks_.getArrangementExtentSamples && hooks_.lastRecordRunBoundaries)
    {
        auto extentBeforeAll = std::make_shared<std::int64_t>(0);
        auto audioTid = std::make_shared<TrackId>(kInvalidTrackId);
        steps_.push_back(Step{ "past-end: the Cycle runs did not lengthen the project (stored extent unchanged)",
                               [this, extentBeforeAll, storedExtentAtStart](juce::String& failReason) -> bool {
                                   hooks_.setPlaybackActive(false);
                                   hooks_.setCycleEnabled(false);
                                   *extentBeforeAll = hooks_.getStoredArrangementExtentSamples();
                                   appendStabilityRunLine("  stored extent now=" + juce::String((juce::int64)*extentBeforeAll) + " at fixture time="
                                                          + juce::String((juce::int64)*storedExtentAtStart) + " (loop end R=" + juce::String((juce::int64)cycleLocR_) + ")");
                                   // The takes end at R, so the extent may cover exactly up to R when the project was
                                   // shorter than the loop — never beyond that (no reserve, no per-pass growth).
                                   const std::int64_t expected = juce::jmax<std::int64_t>(*storedExtentAtStart, cycleLocR_);
                                   if (*extentBeforeAll != expected)
                                   {
                                       failReason = "a Cycle run changed the stored arrangement extent beyond what the result needs (expected "
                                                    + juce::String((juce::int64)expected) + ")";
                                       return false;
                                   }
                                   return true;
                               },
                               200 });
        auto midiStart = std::make_shared<std::int64_t>(0);
        auto audioClipsBeforePastEnd = std::make_shared<int>(0);
        steps_.push_back(Step{ "past-end: MIDI-only - seek 0.5 s before the end, arm inst, Record",
                               [this, extentBeforeAll, midiStart, secondsToSamples](juce::String& failReason) -> bool {
                                   *midiStart = *extentBeforeAll - secondsToSamples(0.5);
                                   hooks_.seekTransportTo(*midiStart);
                                   hooks_.liveMidiSetArm(liveMidiInstTid_, true);
                                   hooks_.liveMidiSetArm(liveMidiLowerTid_, false);
                                   hooks_.liveMidiSetMonitor(liveMidiInstTid_, false);
                                   cycleUndoSizeBefore_ = hooks_.undoStackSize();
                                   hooks_.recordToggleLikeKey();
                                   juce::Thread::sleep(120);
                                   if (!hooks_.isCountInActive())
                                   {
                                       failReason = "Record did not start";
                                       return false;
                                   }
                                   return true;
                               },
                               3700 });
        steps_.push_back(Step{ "past-end: MIDI-only - note 70 before the old end, block the UI 1.5 s, note 72 after it (transport must keep running)",
                               [this, extentBeforeAll, inject, on, off](juce::String& failReason) -> bool {
                                   if (!hooks_.isRecordingInProgress())
                                   {
                                       failReason = "take did not start";
                                       return false;
                                   }
                                   inject(on(1, 70, 100));
                                   juce::Thread::sleep(200);
                                   inject(off(1, 70));
                                   const std::int64_t headBefore = hooks_.getTransportPlayheadSamples();
                                   juce::Thread::sleep(1500); // no UI timer can run: the engine alone must carry the transport past the end
                                   const std::int64_t headAfter = hooks_.getTransportPlayheadSamples();
                                   appendStabilityRunLine("  transport during the UI stall: " + juce::String((juce::int64)headBefore) + " -> "
                                                          + juce::String((juce::int64)headAfter) + " (old extent " + juce::String((juce::int64)*extentBeforeAll)
                                                          + ", navigable extent now " + juce::String((juce::int64)hooks_.getArrangementExtentSamples()) + ")");
                                   if (headAfter <= *extentBeforeAll || headAfter - headBefore < 48000)
                                   {
                                       failReason = "the transport froze at the old arrangement end while recording";
                                       return false;
                                   }
                                   inject(on(1, 72, 100));
                                   juce::Thread::sleep(200);
                                   inject(off(1, 72));
                                   return true;
                               },
                               400 });
        steps_.push_back(Step{ "past-end: MIDI-only - Stop; clip runs past the old end with correct positions; extent = result end, no reserve",
                               [this, extentBeforeAll, midiStart, sr, describeClip](juce::String& failReason) -> bool {
                                   hooks_.recordToggleLikeKey();
                                   juce::Thread::sleep(250);
                                   const auto clips = hooks_.liveMidiSummarizeAllClips(liveMidiInstTid_);
                                   if ((int)clips.size() != cycleInstClipsBefore_ + 5)
                                   {
                                       failReason = "MIDI-only run past the end did not add exactly one clip";
                                       return false;
                                   }
                                   const StabilityMidiClipSummary& c = clips.back();
                                   const std::int64_t clipEnd = c.firstClipStartSamples + c.firstClipLengthSamples;
                                   const std::int64_t stored = hooks_.getStoredArrangementExtentSamples();
                                   appendStabilityRunLine("  take: " + describeClip(c) + " | end=" + juce::String((juce::int64)clipEnd) + " oldExtent="
                                                          + juce::String((juce::int64)*extentBeforeAll) + " storedExtentNow=" + juce::String((juce::int64)stored)
                                                          + " | " + hooks_.lastRecordRunBoundaries());
                                   if (c.firstClipStartSamples != *midiStart || clipEnd <= *extentBeforeAll + (std::int64_t)sr())
                                   {
                                       failReason = "the take does not start at the record boundary or does not reach past the old end";
                                       return false;
                                   }
                                   // Note 72 was played ~1.9 s after the start: its tick must lie beyond the old end.
                                   bool n72Ok = false;
                                   for (const auto& n : c.notes)
                                   {
                                       if (n.note == 72)
                                       {
                                           const double samples = (double)n.startTick / (double)c.ticksPerQuarter * 60.0 / c.bpm * sr();
                                           n72Ok = c.firstClipStartSamples + (std::int64_t)samples > *extentBeforeAll;
                                       }
                                   }
                                   if (!n72Ok)
                                   {
                                       failReason = "note played after the old end was not placed after it";
                                       return false;
                                   }
                                   if (stored < clipEnd || stored > clipEnd + (std::int64_t)(sr() * 0.5))
                                   {
                                       failReason = "stored extent after Stop must equal the result end (no reserve, no margin)";
                                       return false;
                                   }
                                   *extentBeforeAll = stored;
                                   return true;
                               },
                               300 });
        steps_.push_back(Step{ "past-end: audio-only - arm an audio track, seek 0.3 s before the (new) end, Record 2 s, Stop; length = acknowledged frames",
                               [this, extentBeforeAll, audioTid, audioClipsBeforePastEnd, secondsToSamples](juce::String& failReason) -> bool {
                                   if (!hooks_.armAudioTrackForRecording || !hooks_.audioClipWindowsForTrack || !hooks_.listAllTracks)
                                   {
                                       return true;
                                   }
                                   for (const StabilityTrackInfo& t : hooks_.listAllTracks())
                                   {
                                       if (t.kindName == "audio" && *audioTid == kInvalidTrackId)
                                       {
                                           *audioTid = t.id;
                                       }
                                   }
                                   if (*audioTid == kInvalidTrackId)
                                   {
                                       appendStabilityRunLine("  no audio track - audio-only past-end run skipped");
                                       return true;
                                   }
                                   hooks_.liveMidiSetArm(liveMidiInstTid_, false);
                                   hooks_.armAudioTrackForRecording(*audioTid);
                                   const std::int64_t start = *extentBeforeAll - secondsToSamples(0.3);
                                   hooks_.seekTransportTo(start);
                                   const int before = (int)hooks_.audioClipWindowsForTrack(*audioTid).size();
                                   hooks_.recordToggleLikeKey();
                                   juce::Thread::sleep(120);
                                   if (!hooks_.isCountInActive())
                                   {
                                       appendStabilityRunLine("  audio input unavailable - audio-only past-end run skipped");
                                       hooks_.armAudioTrackForRecording(kInvalidTrackId);
                                       return true;
                                   }
                                   settleOverrideMsForCurrentStep_ = 3700 + 2000;
                                   *audioClipsBeforePastEnd = before; // verified in the next step
                                   return true;
                               },
                               100 });
        steps_.push_back(Step{ "past-end: audio-only - Stop and verify",
                               [this, extentBeforeAll, audioTid, audioClipsBeforePastEnd, sr](juce::String& failReason) -> bool {
                                   if (*audioTid == kInvalidTrackId || !hooks_.isRecordingInProgress())
                                   {
                                       return true;
                                   }
                                   const std::int64_t headBeforeStop = hooks_.getTransportPlayheadSamples();
                                   hooks_.recordToggleLikeKey();
                                   juce::Thread::sleep(400);
                                   hooks_.armAudioTrackForRecording(kInvalidTrackId);
                                   const auto audio = hooks_.audioClipWindowsForTrack(*audioTid);
                                   const juce::String bounds = hooks_.lastRecordRunBoundaries();
                                   appendStabilityRunLine("  audio-only: clips=" + juce::String((int)audio.size()) + " newest start="
                                                          + juce::String((juce::int64)(audio.empty() ? 0 : audio.front().first)) + " len="
                                                          + juce::String((juce::int64)(audio.empty() ? 0 : audio.front().second)) + " headBeforeStop="
                                                          + juce::String((juce::int64)headBeforeStop) + " | " + bounds);
                                   if (audio.empty() || (int)audio.size() != *audioClipsBeforePastEnd + 1)
                                   {
                                       failReason = "audio-only run past the end did not add one clip";
                                       return false;
                                   }
                                   // frames=<stop mono - start mono> in the boundary line must equal the clip length.
                                   const int framesPos = bounds.indexOf("frames=");
                                   const std::int64_t frames = framesPos >= 0 ? bounds.substring(framesPos + 7).getLargeIntValue() : -1;
                                   if (frames != audio.front().second)
                                   {
                                       failReason = "audio clip length differs from the acknowledged captured frame count";
                                       return false;
                                   }
                                   if (headBeforeStop <= *extentBeforeAll)
                                   {
                                       failReason = "audio-only transport froze at the arrangement end";
                                       return false;
                                   }
                                   *extentBeforeAll = hooks_.getStoredArrangementExtentSamples();
                                   return true;
                               },
                               300 });
        steps_.push_back(Step{ "past-end: combined - arm audio + inst, seek 0.3 s before the end, Record 2 s",
                               [this, extentBeforeAll, audioTid, secondsToSamples](juce::String&) -> bool {
                                   if (*audioTid == kInvalidTrackId)
                                   {
                                       return true;
                                   }
                                   hooks_.armAudioTrackForRecording(*audioTid);
                                   hooks_.liveMidiSetArm(liveMidiInstTid_, true);
                                   hooks_.seekTransportTo(*extentBeforeAll - secondsToSamples(0.3));
                                   hooks_.recordToggleLikeKey();
                                   juce::Thread::sleep(120);
                                   if (!hooks_.isCountInActive())
                                   {
                                       appendStabilityRunLine("  combined past-end run could not start - skipped");
                                       hooks_.armAudioTrackForRecording(kInvalidTrackId);
                                       *audioTid = kInvalidTrackId;
                                       return true;
                                   }
                                   settleOverrideMsForCurrentStep_ = 3700 + 1000;
                                   return true;
                               },
                               100 });
        steps_.push_back(Step{ "past-end: combined - play a note, Stop; audio and MIDI lengths identical and past the old end",
                               [this, extentBeforeAll, audioTid, inject, on, off](juce::String& failReason) -> bool {
                                   if (*audioTid == kInvalidTrackId)
                                   {
                                       return true;
                                   }
                                   if (!hooks_.isRecordingInProgress())
                                   {
                                       failReason = "combined past-end take did not start";
                                       return false;
                                   }
                                   inject(on(1, 74, 100));
                                   juce::Thread::sleep(300);
                                   inject(off(1, 74));
                                   juce::Thread::sleep(700);
                                   hooks_.recordToggleLikeKey();
                                   juce::Thread::sleep(400);
                                   hooks_.armAudioTrackForRecording(kInvalidTrackId);
                                   hooks_.liveMidiSetArm(liveMidiInstTid_, false);
                                   const auto audio = hooks_.audioClipWindowsForTrack(*audioTid);
                                   const auto midi = hooks_.liveMidiSummarizeAllClips(liveMidiInstTid_);
                                   if (audio.empty() || midi.empty())
                                   {
                                       failReason = "combined past-end run produced no clips";
                                       return false;
                                   }
                                   const std::int64_t audioLen = audio.front().second;
                                   const std::int64_t midiLen = midi.back().firstClipLengthSamples;
                                   const std::int64_t midiEnd = midi.back().firstClipStartSamples + midiLen;
                                   appendStabilityRunLine("  combined past-end: audio len=" + juce::String((juce::int64)audioLen) + " midi len="
                                                          + juce::String((juce::int64)midiLen) + " midi end=" + juce::String((juce::int64)midiEnd)
                                                          + " oldExtent=" + juce::String((juce::int64)*extentBeforeAll) + " storedNow="
                                                          + juce::String((juce::int64)hooks_.getStoredArrangementExtentSamples()) + " | "
                                                          + hooks_.lastRecordRunBoundaries());
                                   if (audioLen != midiLen || midiEnd <= *extentBeforeAll)
                                   {
                                       failReason = "combined run past the end: lengths differ or the take did not cross the old end";
                                       return false;
                                   }
                                   return true;
                               },
                               300 });
    }
    // ---- 12. Device stopped during a take: Stop must not wait forever; the take is still committed
    //          from the message-thread fallback boundary and the device comes back.
    if (hooks_.closeAudioDeviceForTest && hooks_.restartAudioDeviceForTest && hooks_.lastRecordRunBoundaries)
    {
        steps_.push_back(Step{ "device-stop: MIDI-only take, note, then the audio device is closed while recording",
                               [this, inject, on, off](juce::String& failReason) -> bool {
                                   hooks_.setPlaybackActive(false);
                                   hooks_.setCycleEnabled(false);
                                   hooks_.seekTransportTo(0);
                                   hooks_.liveMidiSetArm(liveMidiInstTid_, true);
                                   hooks_.liveMidiSetMonitor(liveMidiInstTid_, false);
                                   cycleUndoSizeBefore_ = hooks_.undoStackSize();
                                   hooks_.recordToggleLikeKey();
                                   juce::Thread::sleep(120);
                                   if (!hooks_.isCountInActive())
                                   {
                                       failReason = "Record did not start";
                                       return false;
                                   }
                                   settleOverrideMsForCurrentStep_ = 3700 + 400;
                                   return true;
                               },
                               100 });
        steps_.push_back(Step{ "device-stop: close the device mid-take, Stop (bounded wait), verify commit + restart",
                               [this, inject, on, off](juce::String& failReason) -> bool {
                                   if (!hooks_.isRecordingInProgress())
                                   {
                                       failReason = "take did not start";
                                       return false;
                                   }
                                   inject(on(1, 76, 100));
                                   juce::Thread::sleep(200);
                                   inject(off(1, 76));
                                   juce::Thread::sleep(100);
                                   const int clipsBefore = (int)hooks_.liveMidiSummarizeAllClips(liveMidiInstTid_).size();
                                   if (!hooks_.closeAudioDeviceForTest())
                                   {
                                       failReason = "audio device could not be closed";
                                       return false;
                                   }
                                   juce::Thread::sleep(300);
                                   const double t0 = juce::Time::getMillisecondCounterHiRes();
                                   hooks_.recordToggleLikeKey(); // Stop with no callback running
                                   const double stopMs = juce::Time::getMillisecondCounterHiRes() - t0;
                                   const juce::String bounds = hooks_.lastRecordRunBoundaries();
                                   const int clipsAfter = (int)hooks_.liveMidiSummarizeAllClips(liveMidiInstTid_).size();
                                   appendStabilityRunLine("  device-stop: Stop returned after " + juce::String(stopMs, 0) + " ms, clips "
                                                          + juce::String(clipsBefore) + "->" + juce::String(clipsAfter) + " | " + bounds);
                                   juce::String restartFail;
                                   const bool restarted = hooks_.restartAudioDeviceForTest(restartFail);
                                   appendStabilityRunLine(juce::String("  device restarted: ") + (restarted ? "yes" : ("no - " + restartFail)));
                                   hooks_.liveMidiSetArm(liveMidiInstTid_, false);
                                   if (hooks_.isRecordingInProgress())
                                   {
                                       failReason = "take still active after Stop without a device";
                                       return false;
                                   }
                                   if (stopMs > 2000.0)
                                   {
                                       failReason = "Stop without a device callback took too long";
                                       return false;
                                   }
                                   if (!bounds.contains("acked=no"))
                                   {
                                       failReason = "expected the message-thread fallback boundary (no callback)";
                                       return false;
                                   }
                                   if (clipsAfter != clipsBefore + 1)
                                   {
                                       failReason = "the take was not committed after the device stopped";
                                       return false;
                                   }
                                   if (!restarted)
                                   {
                                       failReason = "audio device did not come back";
                                       return false;
                                   }
                                   hooks_.invokeUndo();
                                   juce::Thread::sleep(150);
                                   return true;
                               },
                               800 });
    }
    steps_.push_back(Step{ "midi-cycle: cleanup (Cycle off, disarm, Monitor off)",
                           [this](juce::String&) -> bool {
                               hooks_.setPlaybackActive(false);
                               hooks_.setCycleEnabled(false);
                               hooks_.liveMidiSetArm(liveMidiInstTid_, false);
                               hooks_.liveMidiSetArm(liveMidiLowerTid_, false);
                               hooks_.liveMidiSetMonitor(liveMidiInstTid_, false);
                               return true;
                           },
                           200 });
}

// -----------------------------------------------------------------------------
// Proxy-backed destinations during recording + schema-1 generations
// (`--stability-proxy-recording <project>`; designed for the TSE copy: Groove Agent SE row with a
// schema-1 generation whose content is layer-insensitive and whose pairing is intact, VB3-II row
// with a schema-1 generation whose save pairing is broken and a HALion Sonic Secondary)
// -----------------------------------------------------------------------------
void StabilityScenarioRunner::appendProxyRecordingSteps(const juce::File& project)
{
    if (hooks_.loadProjectFromFile == nullptr || hooks_.listAllTracks == nullptr || hooks_.proxyForcePrimaryUnavailable == nullptr
        || hooks_.proxyRuntimeStateName == nullptr || hooks_.proxyDestinationStateName == nullptr || hooks_.proxyGenerationInfo == nullptr
        || hooks_.activateTrackLikeHeaderClick == nullptr || hooks_.inspectorChooseMidiInput == nullptr || hooks_.liveMidiSetArm == nullptr
        || hooks_.liveMidiSetMonitor == nullptr || hooks_.recordToggleLikeKey == nullptr || hooks_.isCountInActive == nullptr
        || hooks_.isRecordingInProgress == nullptr || hooks_.liveMidiInject == nullptr || hooks_.liveMidiSummarizeAllClips == nullptr
        || hooks_.midiInputStatusTextForTrack == nullptr || hooks_.invokeUndo == nullptr || hooks_.seekTransportTo == nullptr
        || hooks_.setPlaybackActive == nullptr || hooks_.proxyIsLiveRecordingOverrideActive == nullptr
        || hooks_.proxyCopySecondaryConfigFromTrack == nullptr || hooks_.liveMidiAttachCaptureSinkToSecondary == nullptr
        || hooks_.liveMidiCapturedNoteCount == nullptr || hooks_.liveMidiCaptureReset == nullptr || hooks_.setCycleEnabled == nullptr)
    {
        steps_.push_back(Step{ "proxy-rec: hooks missing",
                               [](juce::String& failReason) -> bool {
                                   failReason = "proxy-recording hooks not installed";
                                   return false;
                               },
                               0 });
        return;
    }

    steps_.push_back(Step{ "proxy-rec: copy project to sibling test file",
                           [this, project](juce::String& failReason) -> bool {
                               const juce::File copy = project.getSiblingFile(project.getFileNameWithoutExtension() + "-proxyrectest.dalproj");
                               (void)copy.deleteFile();
                               if (!project.copyFileTo(copy))
                               {
                                   failReason = "could not copy project to " + copy.getFullPathName();
                                   return false;
                               }
                               openSaveCloseCopy_ = copy;
                               appendStabilityRunLine("  test copy: " + copy.getFullPathName());
                               return true;
                           },
                           kSettleDefaultMs });
    steps_.push_back(Step{ "proxy-rec: load test copy",
                           [this](juce::String&) -> bool {
                               hooks_.loadProjectFromFile(openSaveCloseCopy_);
                               return true;
                           },
                           kSettleAfterLoadMs + 2500 }); // plug-ins load
    auto gaTid = std::make_shared<TrackId>(kInvalidTrackId);
    auto vb3Tid = std::make_shared<TrackId>(kInvalidTrackId);
    auto gaGenerationBefore = std::make_shared<juce::String>();
    const auto inject = [this](const juce::MidiMessage& m) { hooks_.liveMidiInject(m); };
    const auto on = [](const int ch, const int note, const int vel) { return juce::MidiMessage::noteOn(ch, note, (juce::uint8)vel); };
    const auto off = [](const int ch, const int note) { return juce::MidiMessage::noteOff(ch, note, (juce::uint8)0); };
    /// Note-ons the capture sink saw on any channel for any pitch except `exceptNote` (-1 = all).
    const auto noteOnsExcept = [this](const int exceptNote) {
        int n = 0;
        for (int ch = 1; ch <= 16; ++ch)
        {
            for (int note = 0; note < 128; ++note)
            {
                if (note != exceptNote)
                {
                    n += hooks_.liveMidiCapturedNoteCount(ch, note, true);
                }
            }
        }
        return n;
    };

    // Plug-ins restore their saved state asynchronously after load; until the live state revision
    // equals the publication's, the live identity honestly differs (F2). Poll between steps.
    auto restoreLogged = std::make_shared<bool>(false);
    for (int poll = 1; poll <= 10; ++poll)
    {
        steps_.push_back(Step{ "proxy-rec: wait for the Primary instruments to finish restoring their state (" + juce::String(poll) + "/10)",
                               [this, poll, restoreLogged](juce::String&) -> bool {
                                   if (*restoreLogged)
                                   {
                                       settleOverrideMsForCurrentStep_ = 10;
                                       return true;
                                   }
                                   bool allRestored = true;
                                   for (const StabilityTrackInfo& t : hooks_.listAllTracks())
                                   {
                                       if (!t.isInstrument || hooks_.proxyGenerationInfo(t.id).isEmpty())
                                       {
                                           continue;
                                       }
                                       const juce::String info = hooks_.proxyGenerationInfo(t.id);
                                       const int pub = info.fromFirstOccurrenceOf("pub=", false, false).getIntValue();
                                       const int save = info.fromFirstOccurrenceOf("save=", false, false).getIntValue();
                                       const juce::String job = hooks_.proxyJobStatusText ? hooks_.proxyJobStatusText(t.id) : juce::String();
                                       const int rev = job.fromFirstOccurrenceOf(" rev=", false, false).getIntValue();
                                       // Only rows whose saved state IS the published one can be expected to come back to `pub`.
                                       if (pub != 0 && pub == save && rev != pub)
                                       {
                                           allRestored = false;
                                       }
                                   }
                                   if (allRestored || poll == 10)
                                   {
                                       appendStabilityRunLine(juce::String("  Primary state revisions ") + (allRestored ? "restored" : "NOT all restored (continuing)")
                                                              + " after poll " + juce::String(poll));
                                       *restoreLogged = true;
                                       settleOverrideMsForCurrentStep_ = 10;
                                   }
                                   return true;
                               },
                               1000 });
    }
    steps_.push_back(Step{ "proxy-rec: schema-1 generations in this project (recorded schema, pairing)",
                           [this, gaTid, vb3Tid, gaGenerationBefore](juce::String& failReason) -> bool {
                               for (const StabilityTrackInfo& t : hooks_.listAllTracks())
                               {
                                   if (!t.isInstrument)
                                   {
                                       continue;
                                   }
                                   const juce::String info = hooks_.proxyGenerationInfo(t.id);
                                   appendStabilityRunLine("  instrument row " + juce::String((juce::int64)t.id) + " \"" + t.name + "\": "
                                                          + (info.isEmpty() ? juce::String("no proxy generation") : info)
                                                          + " | runtime=" + hooks_.proxyRuntimeStateName(t.id) + " destination="
                                                          + hooks_.proxyDestinationStateName(t.id)
                                                          + (hooks_.proxyJobStatusText && info.isNotEmpty() ? " | " + hooks_.proxyJobStatusText(t.id) : juce::String()));
                                   // The "compatible generation" row: schema 1 with intact save pairing (pub == save)
                                   // and no Secondary of its own; the Secondary donor row is the one named VB3.
                                   if (*gaTid == kInvalidTrackId && info.contains("schema=1") && !t.name.containsIgnoreCase("VB3"))
                                   {
                                       const int pub = info.fromFirstOccurrenceOf("pub=", false, false).getIntValue();
                                       const int save = info.fromFirstOccurrenceOf("save=", false, false).getIntValue();
                                       if (pub != 0 && pub == save)
                                       {
                                           *gaTid = t.id;
                                           *gaGenerationBefore = info;
                                       }
                                   }
                                   if (t.name.containsIgnoreCase("VB3") && *vb3Tid == kInvalidTrackId)
                                   {
                                       *vb3Tid = t.id;
                                   }
                               }
                               if (*gaTid == kInvalidTrackId || gaGenerationBefore->isEmpty())
                               {
                                   failReason = "this project has no instrument row with a paired schema-1 proxy generation (the test fixture for this scenario)";
                                   return false;
                               }
                               appendStabilityRunLine("  compatible-generation row=" + juce::String((juce::int64)*gaTid) + " secondary donor row="
                                                      + juce::String((juce::int64)*vb3Tid));
                               return true;
                           },
                           300 });
    steps_.push_back(Step{ "proxy-rec: no-Primary machine for the Groove Agent row -> its schema-1 generation is provably compatible: ProxyCurrent, no re-render",
                           [this, gaTid, gaGenerationBefore](juce::String& failReason) -> bool {
                               hooks_.proxyForcePrimaryUnavailable(*gaTid, true);
                               juce::Thread::sleep(400);
                               const juce::String runtime = hooks_.proxyRuntimeStateName(*gaTid);
                               const juce::String dest = hooks_.proxyDestinationStateName(*gaTid);
                               const juce::String info = hooks_.proxyGenerationInfo(*gaTid);
                               appendStabilityRunLine("  GA forced no-Primary: runtime=" + runtime + " destination=" + dest + " generation=" + info);
                               if (runtime != "ProxyCurrent")
                               {
                                   failReason = "the layer-insensitive schema-1 generation should play as ProxyCurrent without the Primary, got " + runtime;
                                   return false;
                               }
                               if (info != *gaGenerationBefore)
                               {
                                   failReason = "the generation changed (a re-render happened) although the old one is compatible";
                                   return false;
                               }
                               return true;
                           },
                           1500 });
    steps_.push_back(Step{ "proxy-rec: ...and 1.5 s later still the same generation (no background re-render was triggered)",
                           [this, gaTid, gaGenerationBefore](juce::String& failReason) -> bool {
                               const juce::String info = hooks_.proxyGenerationInfo(*gaTid);
                               const juce::String dest = hooks_.proxyDestinationStateName(*gaTid);
                               appendStabilityRunLine("  GA after idle: destination=" + dest + " generation=" + info);
                               if (info != *gaGenerationBefore || dest == "Rendering")
                               {
                                   failReason = "a re-render ran for a compatible schema-1 generation";
                                   return false;
                               }
                               return true;
                           },
                           200 });
    steps_.push_back(Step{ "proxy-rec: VB3-II row (schema-1 generation, broken save pairing) forced no-Primary -> never Current; Secondary or honest stale",
                           [this, vb3Tid](juce::String& failReason) -> bool {
                               if (*vb3Tid == kInvalidTrackId)
                               {
                                   appendStabilityRunLine("  no VB3-II row - skipped");
                                   return true;
                               }
                               const juce::String before = hooks_.proxyGenerationInfo(*vb3Tid);
                               hooks_.proxyForcePrimaryUnavailable(*vb3Tid, true);
                               juce::Thread::sleep(600);
                               const juce::String runtime = hooks_.proxyRuntimeStateName(*vb3Tid);
                               const juce::String dest = hooks_.proxyDestinationStateName(*vb3Tid);
                               appendStabilityRunLine("  VB3-II forced no-Primary: runtime=" + runtime + " destination=" + dest + " generation=" + hooks_.proxyGenerationInfo(*vb3Tid));
                               hooks_.proxyForcePrimaryUnavailable(*vb3Tid, false);
                               if (runtime == "ProxyCurrent" || dest == "Current")
                               {
                                   failReason = "a schema-1 generation with broken pairing must not be Current";
                                   return false;
                               }
                               if (hooks_.proxyGenerationInfo(*vb3Tid) != before)
                               {
                                   failReason = "the VB3-II generation / metadata changed";
                                   return false;
                               }
                               return true;
                           },
                           400 });
    // ---- Recording on a proxy-backed destination WITHOUT a Secondary: the whole proxy stays, capture works, status says so.
    steps_.push_back(Step{ "proxy-rec: GA row (no Secondary) - MIDI Input = All via the Inspector, arm, Monitor off, seek 0, Record",
                           [this, gaTid](juce::String& failReason) -> bool {
                               hooks_.setPlaybackActive(false);
                               hooks_.setCycleEnabled(false);
                               hooks_.activateTrackLikeHeaderClick(*gaTid);
                               if (!hooks_.inspectorChooseMidiInput("All MIDI inputs"))
                               {
                                   failReason = "Inspector pick failed on the GA row";
                                   return false;
                               }
                               hooks_.seekTransportTo(0);
                               hooks_.liveMidiSetArm(*gaTid, true);
                               hooks_.liveMidiSetMonitor(*gaTid, false);
                               hooks_.recordToggleLikeKey();
                               juce::Thread::sleep(150);
                               if (!hooks_.isCountInActive())
                               {
                                   failReason = "Record did not start" + (hooks_.lastRecordStartRefusal ? " (" + hooks_.lastRecordStartRefusal().replace("\n", " / ") + ")" : juce::String());
                                   return false;
                               }
                               return true;
                           },
                           3700 });
    steps_.push_back(Step{ "proxy-rec: GA row (no Secondary) - during the take the proxy keeps playing and the status explains it; capture works",
                           [this, gaTid, inject, on, off](juce::String& failReason) -> bool {
                               if (!hooks_.isRecordingInProgress())
                               {
                                   failReason = "take did not start";
                                   return false;
                               }
                               const juce::String runtime = hooks_.proxyRuntimeStateName(*gaTid);
                               const juce::String status = hooks_.midiInputStatusTextForTrack(*gaTid);
                               appendStabilityRunLine("  during take (no Secondary): runtime=" + runtime + " override=" + (hooks_.proxyIsLiveRecordingOverrideActive(*gaTid) ? "yes" : "no")
                                                      + " status=\"" + status.replace("\n", " | ") + "\"");
                               inject(on(1, 36, 100));
                               juce::Thread::sleep(200);
                               inject(off(1, 36));
                               juce::Thread::sleep(300);
                               const int clipsBefore = (int)hooks_.liveMidiSummarizeAllClips(*gaTid).size();
                               hooks_.recordToggleLikeKey();
                               juce::Thread::sleep(300);
                               const int clipsAfter = (int)hooks_.liveMidiSummarizeAllClips(*gaTid).size();
                               const juce::String statusAfter = hooks_.midiInputStatusTextForTrack(*gaTid);
                               appendStabilityRunLine("  after Stop: clips " + juce::String(clipsBefore) + "->" + juce::String(clipsAfter) + " runtime="
                                                      + hooks_.proxyRuntimeStateName(*gaTid) + " status=\"" + statusAfter.replace("\n", " | ") + "\"");
                               if (runtime != "ProxyCurrent")
                               {
                                   failReason = "without a Secondary the whole proxy must keep playing during the take (got " + runtime + ")";
                                   return false;
                               }
                               if (!status.contains("still heard from the proxy"))
                               {
                                   failReason = "the status must say that earlier material is still heard from the proxy";
                                   return false;
                               }
                               if (clipsAfter != clipsBefore + 1)
                               {
                                   failReason = "MIDI capture must still work on a proxy-backed destination";
                                   return false;
                               }
                               if (statusAfter.contains("still heard from the proxy"))
                               {
                                   failReason = "the recording status must disappear with the take";
                                   return false;
                               }
                               hooks_.invokeUndo();
                               juce::Thread::sleep(300);
                               const juce::String afterUndo = hooks_.proxyRuntimeStateName(*gaTid);
                               appendStabilityRunLine("  after Undo: runtime=" + afterUndo);
                               if (afterUndo != "ProxyCurrent")
                               {
                                   failReason = "after undoing the take the compatible generation must be Current again";
                                   return false;
                               }
                               return true;
                           },
                           300 });
    // ---- With a Secondary: the take switches the destination to the Secondary (temporarily), the
    //      recording row's own clip is silent there; Monitor off = nothing delivered live.
    steps_.push_back(Step{ "proxy-rec: give the GA row the VB3-II row's Secondary (HALion Sonic); Monitor off; arm; seek 0; Record",
                           [this, gaTid, vb3Tid](juce::String& failReason) -> bool {
                               if (*vb3Tid == kInvalidTrackId || !hooks_.proxyCopySecondaryConfigFromTrack(*gaTid, *vb3Tid, failReason))
                               {
                                   failReason = "could not configure a Secondary on the GA row: " + failReason;
                                   return false;
                               }
                               juce::Thread::sleep(300);
                               appendStabilityRunLine("  GA with Secondary configured (idle): runtime=" + hooks_.proxyRuntimeStateName(*gaTid)
                                                      + " recordingOverride=" + (hooks_.proxyIsLiveRecordingOverrideActive(*gaTid) ? "yes" : "no"));
                               if (hooks_.proxyRuntimeStateName(*gaTid) != "ProxyCurrent" || hooks_.proxyIsLiveRecordingOverrideActive(*gaTid))
                               {
                                   failReason = "arming / configuring alone must not switch the source or load the Secondary";
                                   return false;
                               }
                               hooks_.seekTransportTo(0);
                               hooks_.liveMidiSetArm(*gaTid, true);
                               hooks_.liveMidiSetMonitor(*gaTid, false);
                               hooks_.recordToggleLikeKey();
                               juce::Thread::sleep(150);
                               if (!hooks_.isCountInActive())
                               {
                                   failReason = "Record did not start";
                                   return false;
                               }
                               return true;
                           },
                           3700 });
    steps_.push_back(Step{ "proxy-rec: Monitor off take - Secondary used temporarily, own clip silent, nothing delivered live; Stop -> override ends, real currency decides",
                           [this, gaTid, inject, on, off, noteOnsExcept](juce::String& failReason) -> bool {
                               if (!hooks_.isRecordingInProgress())
                               {
                                   failReason = "take did not start";
                                   return false;
                               }
                               // The Secondary may still be loading: give it a few seconds (prepared off the audio thread).
                               bool overrideActive = false;
                               for (int i = 0; i < 60 && !overrideActive; ++i)
                               {
                                   overrideActive = hooks_.proxyIsLiveRecordingOverrideActive(*gaTid);
                                   if (!overrideActive)
                                   {
                                       juce::Thread::sleep(100);
                                   }
                               }
                               const juce::String runtime = hooks_.proxyRuntimeStateName(*gaTid);
                               const juce::String status = hooks_.midiInputStatusTextForTrack(*gaTid);
                               appendStabilityRunLine("  during take (Secondary, Monitor off): runtime=" + runtime + " recordingOverride="
                                                      + (overrideActive ? "yes" : "no") + " status=\"" + status.replace("\n", " | ") + "\"");
                               if (!overrideActive || runtime != "SecondaryLive")
                               {
                                   failReason = "the take must switch a proxy-backed destination with a Secondary to SecondaryLive (got " + runtime + ")";
                                   return false;
                               }
                               juce::String sinkFail;
                               if (!hooks_.liveMidiAttachCaptureSinkToSecondary(*gaTid, sinkFail))
                               {
                                   failReason = sinkFail;
                                   return false;
                               }
                               hooks_.liveMidiCaptureReset();
                               juce::Thread::sleep(1200); // the GA row's own clip (166 notes from the start) would be delivered here if not suppressed
                               inject(on(1, 100, 100));
                               juce::Thread::sleep(200);
                               inject(off(1, 100));
                               juce::Thread::sleep(200);
                               const int clipNoteOns = noteOnsExcept(100);
                               const int liveNoteOns = hooks_.liveMidiCapturedNoteCount(1, 100, true);
                               appendStabilityRunLine("  delivered to the Secondary during the take: own-clip note-ons=" + juce::String(clipNoteOns)
                                                      + " live note 100=" + juce::String(liveNoteOns) + " (Monitor off -> 0 expected)");
                               const int clipsBefore = (int)hooks_.liveMidiSummarizeAllClips(*gaTid).size();
                               hooks_.recordToggleLikeKey();
                               juce::Thread::sleep(400);
                               const int clipsAfter = (int)hooks_.liveMidiSummarizeAllClips(*gaTid).size();
                               const juce::String runtimeAfter = hooks_.proxyRuntimeStateName(*gaTid);
                               const bool overrideAfter = hooks_.proxyIsLiveRecordingOverrideActive(*gaTid);
                               appendStabilityRunLine("  after Stop: clips " + juce::String(clipsBefore) + "->" + juce::String(clipsAfter) + " runtime=" + runtimeAfter
                                                      + " recordingOverride=" + (overrideAfter ? "yes" : "no") + " destination=" + hooks_.proxyDestinationStateName(*gaTid));
                               if (clipNoteOns != 0)
                               {
                                   failReason = "the recording row's own earlier clip was delivered to the live source during the take";
                                   return false;
                               }
                               if (liveNoteOns != 0)
                               {
                                   failReason = "with Monitor off live MIDI must not be delivered to the instrument";
                                   return false;
                               }
                               if (!status.contains("Secondary instrument used temporarily while recording"))
                               {
                                   failReason = "status must name the temporary Secondary use while recording";
                                   return false;
                               }
                               if (clipsAfter != clipsBefore + 1)
                               {
                                   failReason = "the take was not captured";
                                   return false;
                               }
                               if (overrideAfter || runtimeAfter == "ProxyCurrent")
                               {
                                   failReason = "after the take the override must end and the (now changed) content must not be Current";
                                   return false;
                               }
                               hooks_.invokeUndo();
                               juce::Thread::sleep(400);
                               const juce::String afterUndo = hooks_.proxyRuntimeStateName(*gaTid);
                               appendStabilityRunLine("  after Undo: runtime=" + afterUndo + " recordingOverride=" + (hooks_.proxyIsLiveRecordingOverrideActive(*gaTid) ? "yes" : "no"));
                               if (afterUndo != "ProxyCurrent")
                               {
                                   failReason = "with the content back to the generation's, the proxy must be Current again (real currency, no blind restore)";
                                   return false;
                               }
                               return true;
                           },
                           300 });
    steps_.push_back(Step{ "proxy-rec: Monitor ON take - live note heard through the Secondary, own clip still silent; Stop; Undo",
                           [this, gaTid, inject, on, off, noteOnsExcept](juce::String& failReason) -> bool {
                               hooks_.seekTransportTo(0);
                               hooks_.liveMidiSetMonitor(*gaTid, true);
                               hooks_.recordToggleLikeKey();
                               juce::Thread::sleep(150);
                               if (!hooks_.isCountInActive())
                               {
                                   failReason = "Record did not start";
                                   return false;
                               }
                               settleOverrideMsForCurrentStep_ = 3700 + 300;
                               return true;
                           },
                           100 });
    steps_.push_back(Step{ "proxy-rec: Monitor ON take - verify",
                           [this, gaTid, inject, on, off, noteOnsExcept](juce::String& failReason) -> bool {
                               if (!hooks_.isRecordingInProgress())
                               {
                                   failReason = "take did not start";
                                   return false;
                               }
                               bool overrideActive = false;
                               for (int i = 0; i < 60 && !overrideActive; ++i)
                               {
                                   overrideActive = hooks_.proxyIsLiveRecordingOverrideActive(*gaTid);
                                   if (!overrideActive)
                                   {
                                       juce::Thread::sleep(100);
                                   }
                               }
                               juce::String sinkFail;
                               if (!hooks_.liveMidiAttachCaptureSinkToSecondary(*gaTid, sinkFail))
                               {
                                   failReason = sinkFail;
                                   return false;
                               }
                               hooks_.liveMidiCaptureReset();
                               juce::Thread::sleep(800);
                               inject(on(1, 100, 100));
                               juce::Thread::sleep(250);
                               inject(off(1, 100));
                               juce::Thread::sleep(250);
                               const int clipNoteOns = noteOnsExcept(100);
                               const int liveNoteOns = hooks_.liveMidiCapturedNoteCount(1, 100, true);
                               const juce::String status = hooks_.midiInputStatusTextForTrack(*gaTid);
                               appendStabilityRunLine("  during take (Secondary, Monitor on): runtime=" + hooks_.proxyRuntimeStateName(*gaTid) + " own-clip note-ons="
                                                      + juce::String(clipNoteOns) + " live note 100=" + juce::String(liveNoteOns) + " status=\""
                                                      + status.replace("\n", " | ") + "\"");
                               hooks_.recordToggleLikeKey();
                               juce::Thread::sleep(400);
                               hooks_.invokeUndo();
                               juce::Thread::sleep(300);
                               hooks_.liveMidiSetMonitor(*gaTid, false);
                               hooks_.liveMidiSetArm(*gaTid, false);
                               if (!overrideActive || clipNoteOns != 0 || liveNoteOns < 1)
                               {
                                   failReason = "Monitor on: the live note must reach the Secondary while the row's own clip stays silent";
                                   return false;
                               }
                               if (!status.contains("Monitor on, recording"))
                               {
                                   failReason = "status must name both needs (Monitor on, recording)";
                                   return false;
                               }
                               const juce::String finalState = hooks_.proxyRuntimeStateName(*gaTid);
                               appendStabilityRunLine("  idle again: runtime=" + finalState + " monitorOverride="
                                                      + (hooks_.proxyIsLiveMonitorOverrideActive && hooks_.proxyIsLiveMonitorOverrideActive(*gaTid) ? "yes" : "no")
                                                      + " recordingOverride=" + (hooks_.proxyIsLiveRecordingOverrideActive(*gaTid) ? "yes" : "no"));
                               if (finalState != "ProxyCurrent")
                               {
                                   failReason = "no temporary override may linger after Monitor off + take end";
                                   return false;
                               }
                               return true;
                           },
                           300 });
    // ---- Two source rows into ONE proxy-backed destination: recording one of them must silence
    //      only that row inside the (temporary) live source; the other row keeps sounding there.
    if (hooks_.addMidiSourceRowRoutedTo != nullptr && hooks_.proxyRenderNow != nullptr)
    {
        auto midiRowTid = std::make_shared<TrackId>(kInvalidTrackId);
        constexpr int kRoutedPitch = 120; // outside any drum-kit mapping of the GA row's own clip
        steps_.push_back(Step{ "proxy-rec: two sources - add a MIDI row routed to the GA destination (note 120 x 48)",
                               [this, gaTid, midiRowTid](juce::String& failReason) -> bool {
                                   *midiRowTid = hooks_.addMidiSourceRowRoutedTo(*gaTid, kRoutedPitch, 48, failReason);
                                   if (*midiRowTid == kInvalidTrackId)
                                   {
                                       return false;
                                   }
                                   return true;
                               },
                               800 });
        steps_.push_back(Step{ "proxy-rec: two sources - the content changed (Stale); the machine with the Primary renders the new generation (Render now)",
                               [this, gaTid, midiRowTid](juce::String& failReason) -> bool {
                                   appendStabilityRunLine("  routed MIDI row id=" + juce::String((juce::int64)*midiRowTid) + " -> GA row "
                                                          + juce::String((juce::int64)*gaTid) + "; after the content change: runtime="
                                                          + hooks_.proxyRuntimeStateName(*gaTid) + " destination=" + hooks_.proxyDestinationStateName(*gaTid));
                                   if (hooks_.proxyDestinationStateName(*gaTid) != "Stale")
                                   {
                                       failReason = "adding routed content must make the old generation Stale";
                                       return false;
                                   }
                                   hooks_.proxyForcePrimaryUnavailable(*gaTid, false);
                                   return true;
                               },
                               800 });
        steps_.push_back(Step{ "proxy-rec: two sources - Render now",
                               [this, gaTid](juce::String& failReason) -> bool {
                                   if (hooks_.proxyJobStatusText)
                                   {
                                       appendStabilityRunLine("  before Render now: runtime=" + hooks_.proxyRuntimeStateName(*gaTid) + " " + hooks_.proxyJobStatusText(*gaTid));
                                   }
                                   // The sibling test copy shares the project folder's InstrumentProxies with the
                                   // source copy: a generation file left by an earlier run (same fingerprint, a
                                   // different tail length) would make publication refuse the name collision.
                                   if (hooks_.proxyDeleteUnpublishedGenerationFiles)
                                   {
                                       const int removed = hooks_.proxyDeleteUnpublishedGenerationFiles(*gaTid);
                                       if (removed > 0)
                                       {
                                           appendStabilityRunLine("  removed " + juce::String(removed) + " leftover generation file(s) of earlier runs for the GA row");
                                       }
                                   }
                                   if (!hooks_.proxyRenderNow(*gaTid))
                                   {
                                       failReason = "Render now was refused";
                                       return false;
                                   }
                                   if (hooks_.proxyJobStatusText)
                                   {
                                       appendStabilityRunLine("  right after Render now: " + hooks_.proxyJobStatusText(*gaTid));
                                   }
                                   return true;
                               },
                               1000 });
        // The render job completes through the message loop, so poll between steps (settle time),
        // never by blocking inside one step.
        constexpr int kRenderPolls = 40;
        auto lastJobLine = std::make_shared<juce::String>();
        for (int poll = 1; poll <= kRenderPolls; ++poll)
        {
            const bool last = poll == kRenderPolls;
            steps_.push_back(Step{ "proxy-rec: two sources - wait for the re-render (" + juce::String(poll) + "/" + juce::String(kRenderPolls) + ")",
                                   [this, gaTid, last, lastJobLine](juce::String& failReason) -> bool {
                                       const juce::String dest = hooks_.proxyDestinationStateName(*gaTid);
                                       if (hooks_.proxyJobStatusText)
                                       {
                                           const juce::String job = "destination=" + dest + " job: " + hooks_.proxyJobStatusText(*gaTid);
                                           if (job != *lastJobLine)
                                           {
                                               appendStabilityRunLine("  " + job);
                                               *lastJobLine = job;
                                           }
                                       }
                                       if (dest == "Current")
                                       {
                                           settleOverrideMsForCurrentStep_ = 10;
                                           return true;
                                       }
                                       if (last)
                                       {
                                           failReason = "the re-render did not produce a Current generation (destination=" + dest + ")";
                                           return false;
                                       }
                                       if (dest != "Rendering" && dest != "Stale")
                                       {
                                           failReason = "unexpected destination state while re-rendering: " + dest;
                                           return false;
                                       }
                                       return true;
                                   },
                                   2000 });
        }
        steps_.push_back(Step{ "proxy-rec: two sources - new generation Current; the no-Primary machine plays it (ProxyCurrent)",
                               [this, gaTid](juce::String& failReason) -> bool {
                                   appendStabilityRunLine("  after Render now: destination=" + hooks_.proxyDestinationStateName(*gaTid)
                                                          + " generation=" + hooks_.proxyGenerationInfo(*gaTid));
                                   hooks_.proxyForcePrimaryUnavailable(*gaTid, true);
                                   juce::Thread::sleep(300);
                                   const juce::String runtime = hooks_.proxyRuntimeStateName(*gaTid);
                                   appendStabilityRunLine("  no-Primary again: runtime=" + runtime);
                                   if (runtime != "ProxyCurrent")
                                   {
                                       failReason = "with the new generation the no-Primary machine must play the proxy (got " + runtime + ")";
                                       return false;
                                   }
                                   return true;
                               },
                               300 });
        /// Note-ons of `pitch` on any channel.
        const auto noteOnsOf = [this](const int pitch) {
            int n = 0;
            for (int ch = 1; ch <= 16; ++ch)
            {
                n += hooks_.liveMidiCapturedNoteCount(ch, pitch, true);
            }
            return n;
        };
        /// Own-clip note-ons of the GA row: anything except the routed pitch and the live note.
        const auto gaOwnNoteOns = [this, kRoutedPitch]() {
            int n = 0;
            for (int ch = 1; ch <= 16; ++ch)
            {
                for (int note = 0; note < 128; ++note)
                {
                    if (note != kRoutedPitch && note != 100)
                    {
                        n += hooks_.liveMidiCapturedNoteCount(ch, note, true);
                    }
                }
            }
            return n;
        };
        steps_.push_back(Step{ "proxy-rec: two sources - arm the GA row only (Monitor off); Record",
                               [this, gaTid, midiRowTid](juce::String& failReason) -> bool {
                                   hooks_.seekTransportTo(0);
                                   hooks_.liveMidiSetMonitor(*gaTid, false);
                                   hooks_.liveMidiSetArm(*gaTid, true);
                                   hooks_.liveMidiSetArm(*midiRowTid, false);
                                   hooks_.recordToggleLikeKey();
                                   juce::Thread::sleep(150);
                                   if (!hooks_.isCountInActive())
                                   {
                                       failReason = "Record did not start";
                                       return false;
                                   }
                                   return true;
                               },
                               3700 });
        steps_.push_back(Step{ "proxy-rec: two sources - GA row recording: the routed MIDI row is heard through the Secondary, the GA row's own clip is silent; Stop; Undo",
                               [this, gaTid, midiRowTid, noteOnsOf, gaOwnNoteOns, inject, on, off](juce::String& failReason) -> bool {
                                   if (!hooks_.isRecordingInProgress())
                                   {
                                       failReason = "take did not start";
                                       return false;
                                   }
                                   bool overrideActive = false;
                                   for (int i = 0; i < 60 && !overrideActive; ++i)
                                   {
                                       overrideActive = hooks_.proxyIsLiveRecordingOverrideActive(*gaTid);
                                       if (!overrideActive)
                                       {
                                           juce::Thread::sleep(100);
                                       }
                                   }
                                   juce::String sinkFail;
                                   if (!hooks_.liveMidiAttachCaptureSinkToSecondary(*gaTid, sinkFail))
                                   {
                                       failReason = sinkFail;
                                       return false;
                                   }
                                   hooks_.liveMidiCaptureReset();
                                   juce::Thread::sleep(1100);
                                   // A live note so the take commits a clip (Monitor off: recorded, not heard).
                                   inject(on(1, 100, 100));
                                   juce::Thread::sleep(200);
                                   inject(off(1, 100));
                                   juce::Thread::sleep(200);
                                   const int routed = noteOnsOf(kRoutedPitch);
                                   const int own = gaOwnNoteOns();
                                   const int liveHeard = hooks_.liveMidiCapturedNoteCount(1, 100, true);
                                   const juce::String gaStatus = hooks_.midiInputStatusTextForTrack(*gaTid);
                                   const juce::String midiStatus = hooks_.midiInputStatusTextForTrack(*midiRowTid);
                                   appendStabilityRunLine("  GA recording, two sources: runtime=" + hooks_.proxyRuntimeStateName(*gaTid)
                                                          + " routed-row note 120 heard=" + juce::String(routed) + " GA own-clip note-ons=" + juce::String(own)
                                                          + " live note 100 heard=" + juce::String(liveHeard) + " (Monitor off -> 0)"
                                                          + " GA status=\"" + gaStatus.replace("\n", " | ") + "\" MIDI-row status=\"" + midiStatus.replace("\n", " | ") + "\"");
                                   const int gaClipsBefore = (int)hooks_.liveMidiSummarizeAllClips(*gaTid).size();
                                   hooks_.recordToggleLikeKey();
                                   juce::Thread::sleep(400);
                                   const int gaClipsAfter = (int)hooks_.liveMidiSummarizeAllClips(*gaTid).size();
                                   appendStabilityRunLine("  after Stop: GA clips " + juce::String(gaClipsBefore) + "->" + juce::String(gaClipsAfter) + " runtime=" + hooks_.proxyRuntimeStateName(*gaTid)
                                                          + (hooks_.proxySnapshotSourcesText ? " | snapshot: " + hooks_.proxySnapshotSourcesText(*gaTid) : juce::String()));
                                   if (gaClipsAfter != gaClipsBefore + 1)
                                   {
                                       failReason = "the GA take was not captured";
                                       return false;
                                   }
                                   hooks_.invokeUndo();
                                   juce::Thread::sleep(300);
                                   appendStabilityRunLine("  after Undo: GA clips=" + juce::String((int)hooks_.liveMidiSummarizeAllClips(*gaTid).size()) + " runtime=" + hooks_.proxyRuntimeStateName(*gaTid)
                                                          + (hooks_.proxySnapshotSourcesText ? " | snapshot: " + hooks_.proxySnapshotSourcesText(*gaTid) : juce::String()));
                                   if (liveHeard != 0)
                                   {
                                       failReason = "with Monitor off the live note must not be delivered";
                                       return false;
                                   }
                                   if (!overrideActive)
                                   {
                                       failReason = "the take must switch the destination to the Secondary";
                                       return false;
                                   }
                                   if (routed < 1)
                                   {
                                       failReason = "the other source row routed to the same destination must keep sounding through the live source";
                                       return false;
                                   }
                                   if (own != 0)
                                   {
                                       failReason = "the recording row's own earlier clip must be silent in the live source";
                                       return false;
                                   }
                                   return true;
                               },
                               300 });
        steps_.push_back(Step{ "proxy-rec: two sources - now arm the MIDI row instead (GA row not armed); Record",
                               [this, gaTid, midiRowTid](juce::String& failReason) -> bool {
                                   hooks_.activateTrackLikeHeaderClick(*midiRowTid);
                                   if (!hooks_.inspectorChooseMidiInput("All MIDI inputs"))
                                   {
                                       failReason = "Inspector pick failed on the MIDI row";
                                       return false;
                                   }
                                   // The GA row's own clip starts sounding at 4 s (tick 11520 @ 180 BPM): start this
                                   // take at 3.5 s so its notes fall inside the capture window.
                                   hooks_.seekTransportTo((std::int64_t)std::llround(3.5 * (hooks_.getDeviceSampleRate() > 0.0 ? hooks_.getDeviceSampleRate() : 48000.0)));
                                   hooks_.liveMidiSetArm(*gaTid, false);
                                   hooks_.liveMidiSetMonitor(*midiRowTid, false);
                                   hooks_.liveMidiSetArm(*midiRowTid, true);
                                   juce::Thread::sleep(300);
                                   appendStabilityRunLine("  MIDI row armed (idle): GA runtime=" + hooks_.proxyRuntimeStateName(*gaTid)
                                                          + " recordingOverride=" + (hooks_.proxyIsLiveRecordingOverrideActive(*gaTid) ? "yes" : "no")
                                                          + " tracks=" + juce::String(hooks_.getTrackCount()) + " GA clips=" + juce::String((int)hooks_.liveMidiSummarizeAllClips(*gaTid).size())
                                                          + " MIDI-row clips=" + juce::String((int)hooks_.liveMidiSummarizeAllClips(*midiRowTid).size())
                                                          + " MIDI-row status=\"" + hooks_.midiInputStatusTextForTrack(*midiRowTid).replace("\n", " | ") + "\""
                                                          + (hooks_.proxyJobStatusText ? " | " + hooks_.proxyJobStatusText(*gaTid) : juce::String())
                                                          + (hooks_.proxySnapshotSourcesText ? " | snapshot: " + hooks_.proxySnapshotSourcesText(*gaTid) : juce::String()));
                                   {
                                       const auto gaClips = hooks_.liveMidiSummarizeAllClips(*gaTid);
                                       if (!gaClips.empty())
                                       {
                                           const StabilityMidiClipSummary& s = gaClips.front();
                                           juce::String firstNotes;
                                           for (size_t i = 0; i < s.notes.size() && i < 4; ++i)
                                           {
                                               firstNotes << s.notes[i].note << "@" << juce::String((juce::int64)s.notes[i].startTick) << " ";
                                           }
                                           appendStabilityRunLine("  GA own clip: start=" + juce::String((juce::int64)s.firstClipStartSamples) + " len="
                                                                  + juce::String((juce::int64)s.firstClipLengthSamples) + " bpm=" + juce::String(s.bpm) + " tpq="
                                                                  + juce::String(s.ticksPerQuarter) + " first notes(tick): " + firstNotes.trim());
                                       }
                                   }
                                   if (hooks_.proxyIsLiveRecordingOverrideActive(*gaTid))
                                   {
                                       failReason = "arming alone must not switch the destination's source";
                                       return false;
                                   }
                                   hooks_.recordToggleLikeKey();
                                   juce::Thread::sleep(150);
                                   if (!hooks_.isCountInActive())
                                   {
                                       failReason = "Record did not start" + (hooks_.lastRecordStartRefusal ? " (" + hooks_.lastRecordStartRefusal().replace("\n", " / ") + ")" : juce::String());
                                       return false;
                                   }
                                   return true;
                               },
                               3700 });
        steps_.push_back(Step{ "proxy-rec: two sources - MIDI row recording: the GA row's own clip is heard through the Secondary, the routed row is silent; Stop; Undo",
                               [this, gaTid, midiRowTid, noteOnsOf, gaOwnNoteOns, inject, on, off](juce::String& failReason) -> bool {
                                   if (!hooks_.isRecordingInProgress())
                                   {
                                       failReason = "take did not start";
                                       return false;
                                   }
                                   bool overrideActive = false;
                                   for (int i = 0; i < 60 && !overrideActive; ++i)
                                   {
                                       overrideActive = hooks_.proxyIsLiveRecordingOverrideActive(*gaTid);
                                       if (!overrideActive)
                                       {
                                           juce::Thread::sleep(100);
                                       }
                                   }
                                   juce::String sinkFail;
                                   if (!hooks_.liveMidiAttachCaptureSinkToSecondary(*gaTid, sinkFail))
                                   {
                                       failReason = sinkFail;
                                       return false;
                                   }
                                   hooks_.liveMidiCaptureReset();
                                   juce::Thread::sleep(1100);
                                   inject(on(1, 100, 100));
                                   juce::Thread::sleep(200);
                                   inject(off(1, 100));
                                   juce::Thread::sleep(200);
                                   const int routed = noteOnsOf(kRoutedPitch);
                                   const int own = gaOwnNoteOns();
                                   const juce::String midiStatus = hooks_.midiInputStatusTextForTrack(*midiRowTid);
                                   appendStabilityRunLine("  MIDI row recording, two sources: GA runtime=" + hooks_.proxyRuntimeStateName(*gaTid)
                                                          + " routed-row note 120 heard=" + juce::String(routed) + " GA own-clip note-ons=" + juce::String(own)
                                                          + " MIDI-row status=\"" + midiStatus.replace("\n", " | ") + "\"");
                                   const int clipsBefore = (int)hooks_.liveMidiSummarizeAllClips(*midiRowTid).size();
                                   hooks_.recordToggleLikeKey();
                                   juce::Thread::sleep(400);
                                   const int clipsAfter = (int)hooks_.liveMidiSummarizeAllClips(*midiRowTid).size();
                                   const juce::String runtimeAfter = hooks_.proxyRuntimeStateName(*gaTid);
                                   appendStabilityRunLine("  after Stop: MIDI-row clips " + juce::String(clipsBefore) + "->" + juce::String(clipsAfter)
                                                          + " GA runtime=" + runtimeAfter + " recordingOverride="
                                                          + (hooks_.proxyIsLiveRecordingOverrideActive(*gaTid) ? "yes" : "no"));
                                   hooks_.invokeUndo();
                                   juce::Thread::sleep(300);
                                   hooks_.liveMidiSetArm(*midiRowTid, false);
                                   if (!overrideActive)
                                   {
                                       failReason = "recording a MIDI row into a proxy-backed destination must switch that destination to the Secondary";
                                       return false;
                                   }
                                   if (own < 1)
                                   {
                                       failReason = "the destination's own earlier clip must keep sounding through the live source";
                                       return false;
                                   }
                                   if (routed != 0)
                                   {
                                       failReason = "the recording MIDI row's own earlier clip must be silent in the live source";
                                       return false;
                                   }
                                   if (!midiStatus.contains("Secondary instrument used temporarily while recording"))
                                   {
                                       failReason = "the MIDI row's status must name the temporary Secondary use while recording";
                                       return false;
                                   }
                                   if (clipsAfter != clipsBefore + 1)
                                   {
                                       failReason = "the MIDI row's take was not captured";
                                       return false;
                                   }
                                   if (hooks_.proxyIsLiveRecordingOverrideActive(*gaTid))
                                   {
                                       failReason = "no override may linger after the take";
                                       return false;
                                   }
                                   return true;
                               },
                               300 });
    }
    steps_.push_back(Step{ "proxy-rec: cleanup (Primary available again)",
                           [this, gaTid](juce::String&) -> bool {
                               hooks_.setPlaybackActive(false);
                               hooks_.proxyForcePrimaryUnavailable(*gaTid, false);
                               return true;
                           },
                           300 });
}

// -----------------------------------------------------------------------------
// Proxy render probe (`--stability-proxy-render-probe <project> <trackId> <outDir> …`)
// -----------------------------------------------------------------------------
void StabilityScenarioRunner::appendProxyRenderProbeSteps(const StabilityScenarioRequest& request)
{
    if (hooks_.proxyRenderProbeCapture == nullptr || hooks_.proxyRenderProbePrepare == nullptr
        || hooks_.proxyRenderProbeStartWorker == nullptr || hooks_.proxyRenderProbeIsDone == nullptr
        || hooks_.proxyRenderProbeFinish == nullptr || hooks_.proxyRenderProbePublish == nullptr
        || hooks_.loadProjectFromFile == nullptr || hooks_.saveProject == nullptr)
    {
        steps_.push_back(Step{ "proxy-probe: hooks missing",
                               [](juce::String& failReason) -> bool {
                                   failReason = "proxy-render-probe hooks not installed";
                                   return false;
                               },
                               0 });
        return;
    }
    const juce::File project = request.projectA;
    const TrackId tid = request.probeTrackId;
    const juce::File outDir = request.probeOutDir;
    const int repeat = request.probeRepeat;
    const int waitMs = request.probeWaitAfterPrepareMs;
    const bool publish = request.probePublish;
    StabilityRunnerHooks::ProxyRenderProbeOptions probeOptions;
    probeOptions.stateBlobOverride = request.probeStateBlobOverride;
    probeOptions.readinessEnabled = !request.probeNoReadiness;
    probeOptions.nonRealtimeIndication = !request.probeRealtimeIndication;
    probeOptions.tailPolicyV1 = request.probeTailPolicyV1;
    probeOptions.retainFailedArtifact = request.probeRetainFailed;
#if JUCE_DEBUG
    const juce::String build = "Debug";
#else
    const juce::String build = "Release";
#endif
    // The probe copy is KEPT between runs on purpose: a Debug run may publish a generation that a
    // later Release run then meets on disk (the collision case). Never registered for cleanup.
    const juce::File copy = project.getSiblingFile(project.getFileNameWithoutExtension() + "-proxyprobe.dalproj");

    steps_.push_back(Step{ "proxy-probe: prepare the sibling probe copy (reused when it already exists)",
                           [this, project, copy, outDir, build](juce::String& failReason) -> bool {
                               if (!copy.existsAsFile())
                               {
                                   if (!project.copyFileTo(copy))
                                   {
                                       failReason = "could not copy the project to " + copy.getFullPathName();
                                       return false;
                                   }
                                   appendStabilityRunLine("  probe copy created: " + copy.getFullPathName());
                               }
                               else
                               {
                                   appendStabilityRunLine("  probe copy reused (previous run's publication kept): " + copy.getFullPathName());
                               }
                               (void)outDir.createDirectory();
                               if (!outDir.isDirectory())
                               {
                                   failReason = "could not create the output folder " + outDir.getFullPathName();
                                   return false;
                               }
                               appendStabilityRunLine("  build=" + build + " outDir=" + outDir.getFullPathName());
                               return true;
                           },
                           100 });
    steps_.push_back(Step{ "proxy-probe: load the probe copy",
                           [this, copy](juce::String&) -> bool {
                               hooks_.loadProjectFromFile(copy);
                               return true;
                           },
                           kSettleAfterLoadMs + 3000 }); // + the plug-ins' asynchronous state restore
    steps_.push_back(Step{ "proxy-probe: destination row, Manual update mode, frozen identity",
                           [this, tid, outDir, probeOptions](juce::String& failReason) -> bool {
                               bool found = false;
                               for (const StabilityTrackInfo& t : hooks_.listAllTracks())
                               {
                                   if (t.id == tid)
                                   {
                                       found = true;
                                       appendStabilityRunLine("  destination row " + juce::String((juce::int64)t.id) + " \"" + t.name + "\" kind=" + t.kindName
                                                              + (hooks_.proxyGenerationInfo ? " | published: " + hooks_.proxyGenerationInfo(t.id) : juce::String())
                                                              + (hooks_.proxyDestinationStateName ? " | destination=" + hooks_.proxyDestinationStateName(t.id) : juce::String()));
                                   }
                               }
                               if (!found)
                               {
                                   failReason = "no track with id " + juce::String((juce::int64)tid) + " in this project";
                                   return false;
                               }
                               if (hooks_.proxySetUpdateModeManual && !hooks_.proxySetUpdateModeManual(tid))
                               {
                                   failReason = "could not set the update mode to Manual";
                                   return false;
                               }
                               const juce::String identity = hooks_.proxyRenderProbeCapture(tid, outDir, probeOptions);
                               for (const auto& line : juce::StringArray::fromLines(identity))
                               {
                                   if (line.isNotEmpty())
                                   {
                                       appendStabilityRunLine("  " + line);
                                   }
                               }
                               if (identity.startsWith("ERROR"))
                               {
                                   failReason = identity;
                                   return false;
                               }
                               return true;
                           },
                           200 });

    for (int i = 1; i <= repeat; ++i)
    {
        const juce::String label = build.toLowerCase() + "-render" + juce::String(i);
        steps_.push_back(Step{ "proxy-probe: " + label + " - create + restore + prepare the isolated instance",
                               [this, waitMs](juce::String& failReason) -> bool {
                                   if (!hooks_.proxyRenderProbePrepare(failReason))
                                   {
                                       return false;
                                   }
                                   if (hooks_.proxyRenderProbeInstanceStateHash)
                                   {
                                       appendStabilityRunLine("  instance state right after prepare: " + hooks_.proxyRenderProbeInstanceStateHash());
                                   }
                                   if (waitMs > 0)
                                   {
                                       appendStabilityRunLine("  diagnostic wait after prepare: " + juce::String(waitMs) + " ms (message loop running)");
                                   }
                                   return true;
                               },
                               10 });
        // The diagnostic wait is split into 500 ms slices so the instance's state bytes can be
        // sampled while the plug-in's asynchronous loading proceeds (message loop running).
        for (int waited = 0; waited < waitMs; waited += 500)
        {
            steps_.push_back(Step{ "proxy-probe: " + label + " - diagnostic wait slice (" + juce::String(waited + 500) + " ms)",
                                   [this](juce::String&) -> bool {
                                       if (hooks_.proxyRenderProbeInstanceStateHash)
                                       {
                                           appendStabilityRunLine("  instance state during the wait: " + hooks_.proxyRenderProbeInstanceStateHash());
                                       }
                                       return true;
                                   },
                                   juce::jmin(500, waitMs - waited) });
        }
        steps_.push_back(Step{ "proxy-probe: " + label + " - start the render worker",
                               [this](juce::String& failReason) -> bool {
                                   if (hooks_.proxyRenderProbeInstanceStateHash)
                                   {
                                       appendStabilityRunLine("  instance state right before the worker: " + hooks_.proxyRenderProbeInstanceStateHash());
                                   }
                                   return hooks_.proxyRenderProbeStartWorker(failReason);
                               },
                               300 });
        constexpr int kPolls = 1200; // 1200 × 500 ms = 10 min upper bound per render
        auto doneLogged = std::make_shared<bool>(false);
        auto lastProgress = std::make_shared<std::int64_t>(-1);
        for (int poll = 1; poll <= kPolls; ++poll)
        {
            const bool last = poll == kPolls;
            steps_.push_back(Step{ "proxy-probe: " + label + " - wait (" + juce::String(poll) + ")",
                                   [this, last, doneLogged, lastProgress](juce::String& failReason) -> bool {
                                       if (*doneLogged || hooks_.proxyRenderProbeIsDone())
                                       {
                                           *doneLogged = true;
                                           settleOverrideMsForCurrentStep_ = 10;
                                           return true;
                                       }
                                       if (hooks_.proxyRenderProbeProgressMs)
                                       {
                                           const std::int64_t p = hooks_.proxyRenderProbeProgressMs();
                                           if (p / 10000 != *lastProgress / 10000)
                                           {
                                               appendStabilityRunLine("  rendering… " + juce::String(p) + " ms of material");
                                               *lastProgress = p;
                                           }
                                       }
                                       if (last)
                                       {
                                           failReason = "the render did not finish within the probe's time bound";
                                           return false;
                                       }
                                       return true;
                                   },
                                   500 });
        }
        steps_.push_back(Step{ "proxy-probe: " + label + " - finish, analyze and keep the artifact",
                               [this, label](juce::String& failReason) -> bool {
                                   const juce::String summary = hooks_.proxyRenderProbeFinish(label);
                                   for (const auto& line : juce::StringArray::fromLines(summary))
                                   {
                                       if (line.isNotEmpty())
                                       {
                                           appendStabilityRunLine("  " + line);
                                       }
                                   }
                                   if (summary.startsWith("ERROR"))
                                   {
                                       failReason = summary;
                                       return false;
                                   }
                                   return true;
                               },
                               200 });
        if (publish)
        {
            steps_.push_back(Step{ "proxy-probe: " + label + " - publish through the production publication, then save the probe copy",
                                   [this, tid](juce::String&) -> bool {
                                       const juce::String outcome = hooks_.proxyRenderProbePublish();
                                       for (const auto& line : juce::StringArray::fromLines(outcome))
                                       {
                                           if (line.isNotEmpty())
                                           {
                                               appendStabilityRunLine("  " + line);
                                           }
                                       }
                                       if (hooks_.proxyGenerationInfo && hooks_.proxyDestinationStateName)
                                       {
                                           appendStabilityRunLine("  after publish: " + hooks_.proxyGenerationInfo(tid) + " | destination=" + hooks_.proxyDestinationStateName(tid)
                                                                  + (hooks_.proxyRuntimeStateName ? " runtime=" + hooks_.proxyRuntimeStateName(tid) : juce::String()));
                                       }
                                       hooks_.saveProject();
                                       return true; // the outcome itself is evidence; the comparison is made in the report
                                   },
                                   500 });
        }
    }
}

// -----------------------------------------------------------------------------
// Proxy playback edges (`--stability-proxy-playback-edges <project> <trackId> <outDir>`)
// -----------------------------------------------------------------------------
void StabilityScenarioRunner::appendProxyPlaybackEdgesSteps(const StabilityScenarioRequest& request)
{
    if (hooks_.loadProjectFromFile == nullptr || hooks_.listAllTracks == nullptr || hooks_.setMeteredTrack == nullptr
        || hooks_.drainTrackMeter == nullptr || hooks_.drainMasterMeter == nullptr || hooks_.setInsertLevelTapTrack == nullptr
        || hooks_.readAndResetInsertLevelTap == nullptr || hooks_.readAndResetInsertLevelTapFull == nullptr
        || hooks_.seekTransportTo == nullptr || hooks_.setPlaybackActive == nullptr
        || hooks_.setCycleEnabled == nullptr || hooks_.setLocatorsSamples == nullptr || hooks_.proxyForcePrimaryUnavailable == nullptr
        || hooks_.proxyRuntimeStateName == nullptr || hooks_.getDeviceSampleRate == nullptr || hooks_.runMixdownBlocking == nullptr
        || hooks_.proxyPublishedAssetShape == nullptr)
    {
        steps_.push_back(Step{ "proxy-edges: hooks missing",
                               [](juce::String& failReason) -> bool {
                                   failReason = "proxy-playback-edges hooks not installed";
                                   return false;
                               },
                               0 });
        return;
    }
    const juce::File project = request.projectA;
    const TrackId tid = request.probeTrackId;
    const juce::File outDir = request.probeOutDir;
    const juce::File copy = project.getSiblingFile(project.getFileNameWithoutExtension() + "-proxyedges.dalproj");
    auto report = std::make_shared<juce::StringArray>();
    auto eofSamples = std::make_shared<std::int64_t>(0);
    auto sr = std::make_shared<double>(48000.0);
    const auto say = [this, report](const juce::String& line) {
        appendStabilityRunLine("  " + line);
        report->add(line);
    };

    steps_.push_back(Step{ "proxy-edges: sibling copy + load",
                           [this, project, copy, outDir](juce::String& failReason) -> bool {
                               (void)copy.deleteFile();
                               if (!project.copyFileTo(copy))
                               {
                                   failReason = "could not copy the project";
                                   return false;
                               }
                               (void)outDir.createDirectory();
                               hooks_.loadProjectFromFile(copy);
                               return true;
                           },
                           kSettleAfterLoadMs + 3000 });
    steps_.push_back(Step{ "proxy-edges: destination, published asset, meters and insert tap on the row",
                           [this, tid, eofSamples, sr, say](juce::String& failReason) -> bool {
                               bool found = false;
                               for (const StabilityTrackInfo& t : hooks_.listAllTracks())
                               {
                                   if (t.id == tid)
                                   {
                                       found = true;
                                       say("row " + juce::String((juce::int64)t.id) + " \"" + t.name + "\" " + (hooks_.describeTrackForDiagnostics ? hooks_.describeTrackForDiagnostics(t.id) : juce::String()));
                                   }
                               }
                               if (!found)
                               {
                                   failReason = "no track with id " + juce::String((juce::int64)tid);
                                   return false;
                               }
                               std::int64_t len = 0;
                               double rate = 0.0;
                               if (!hooks_.proxyPublishedAssetShape(tid, len, rate) || len <= 0)
                               {
                                   failReason = "the destination has no published proxy generation to play";
                                   return false;
                               }
                               *sr = hooks_.getDeviceSampleRate() > 0.0 ? hooks_.getDeviceSampleRate() : 48000.0;
                               *eofSamples = (std::int64_t)std::llround((double)len * (*sr) / rate);
                               say("published asset: " + juce::String(len) + " samples @ " + juce::String(rate) + " Hz -> EOF at timeline "
                                   + juce::String(*eofSamples) + " (" + juce::String((double)*eofSamples / *sr, 3) + " s); device rate " + juce::String(*sr)
                                   + (hooks_.proxyGenerationInfo ? " | " + hooks_.proxyGenerationInfo(tid) : juce::String()));
                               double firstOn = 0.0, lastOff = 0.0;
                               if (hooks_.proxyTrackNoteSpanSeconds && hooks_.proxyTrackNoteSpanSeconds(tid, firstOn, lastOff))
                               {
                                   say("destination notes: first note-on at " + juce::String(firstOn, 3) + " s, last note-off at " + juce::String(lastOff, 3)
                                       + " s (the 1.0-3.5 s windows below are " + (firstOn > 3.5 ? "BEFORE the first note: lead-in only" : "inside the music") + ")");
                               }
                               hooks_.setPlaybackActive(false);
                               hooks_.setCycleEnabled(false);
                               hooks_.setMeteredTrack(tid);
                               hooks_.setInsertLevelTapTrack(tid);
                               (void)hooks_.drainTrackMeter();
                               (void)hooks_.drainMasterMeter();
                               float b, a;
                               double rb, ra;
                               std::uint32_t pb, pa;
                               hooks_.readAndResetInsertLevelTap(b, a, rb, ra, pb, pa);
                               return true;
                           },
                           300 });

    // One measurement window: arm (drain), settle, read; the reading names pre-insert peak (the
    // instrument boundary = what the proxy / Primary produced), post-strip peak/DC/first/last (after
    // the user's insert chain, fader, pan) and the Stereo Out peak.
    const auto window = [this, tid, say](const juce::String& label, const int settleMs) {
        steps_.push_back(Step{ "proxy-edges: " + label + " - arm window",
                               [this](juce::String&) -> bool {
                                   (void)hooks_.drainTrackMeter();
                                   (void)hooks_.drainMasterMeter();
                                   float b, a;
                                   double rb, ra;
                                   std::uint32_t pb, pa;
                                   hooks_.readAndResetInsertLevelTap(b, a, rb, ra, pb, pa);
                                   return true;
                               },
                               settleMs });
        steps_.push_back(Step{ "proxy-edges: " + label + " - read",
                               [this, label, say](juce::String&) -> bool {
                                   const StabilityLevelStats t = hooks_.drainTrackMeter();
                                   const StabilityLevelStats m = hooks_.drainMasterMeter();
                                   const StabilityRunnerHooks::InsertTapReading tap = hooks_.readAndResetInsertLevelTapFull();
                                   const float before = tap.peakBefore;
                                   const double rmsBefore = tap.rmsBefore;
                                   const auto db = [](const double v) { return v > 0.0 ? juce::String(20.0 * std::log10(v), 1) : juce::String("-inf"); };
                                   say(label + ": pre-insert peak=" + juce::String(before, 4) + " (" + db(before) + " dBFS) rms=" + juce::String(rmsBefore, 4)
                                       + " dc L/R=" + juce::String(tap.dcBefore[0], 4) + "/" + juce::String(tap.dcBefore[1], 4)
                                       + " | post-strip peak L/R=" + juce::String(t.peak[0], 4) + "/" + juce::String(t.peak[1], 4) + " (" + db(juce::jmax(t.peak[0], t.peak[1]))
                                       + " dBFS) dc L/R=" + juce::String(t.dcOffset[0], 4) + "/" + juce::String(t.dcOffset[1], 4) + " first L=" + juce::String(t.firstSample[0], 4)
                                       + " last L=" + juce::String(t.lastSample[0], 4) + " frames=" + juce::String((juce::int64)t.frames)
                                       + " | stereo out peak=" + juce::String(juce::jmax(m.peak[0], m.peak[1]), 4) + " dc=" + juce::String(m.dcOffset[0], 4));
                                   return true;
                               },
                               50 });
    };
    const auto seekSeconds = [this, sr](const double seconds) { hooks_.seekTransportTo((std::int64_t)std::llround(seconds * *sr)); };
    const auto eofSec = [eofSamples, sr]() { return (double)*eofSamples / *sr; };

    for (const bool proxyPath : { true, false })
    {
        const juce::String tag = proxyPath ? "PROXY" : "PRIMARY";
        steps_.push_back(Step{ "proxy-edges: " + tag + " - select the source (" + (proxyPath ? "Primary forced unavailable -> proxy" : "Primary available") + ")",
                               [this, tid, proxyPath, tag, say](juce::String& failReason) -> bool {
                                   hooks_.setPlaybackActive(false);
                                   hooks_.setCycleEnabled(false);
                                   hooks_.proxyForcePrimaryUnavailable(tid, proxyPath);
                                   juce::Thread::sleep(300);
                                   const juce::String runtime = hooks_.proxyRuntimeStateName(tid);
                                   say(tag + ": runtime=" + runtime);
                                   if (proxyPath && runtime != "ProxyCurrent")
                                   {
                                       failReason = "the proxy is not the source (runtime=" + runtime + ")";
                                       return false;
                                   }
                                   if (!proxyPath && runtime != "Primary")
                                   {
                                       failReason = "the Primary is not the source (runtime=" + runtime + ")";
                                       return false;
                                   }
                                   hooks_.seekTransportTo(0);
                                   return true;
                               },
                               500 });
        window(tag + " stopped at 0 (1 s)", 1000);
        steps_.push_back(Step{ "proxy-edges: " + tag + " - seek 1.0 s, Play",
                               [this, seekSeconds](juce::String&) -> bool {
                                   seekSeconds(1.0);
                                   juce::Thread::sleep(100);
                                   hooks_.setPlaybackActive(true);
                                   return true;
                               },
                               10 });
        window(tag + " play start at 1.0 s (first 0.5 s)", 500);
        window(tag + " playing 1.5-3.5 s", 2000);
        steps_.push_back(Step{ "proxy-edges: " + tag + " - seek to EOF - 3 s while playing",
                               [this, seekSeconds, eofSec](juce::String&) -> bool {
                                   seekSeconds(eofSec() - 3.0);
                                   return true;
                               },
                               400 });
        window(tag + " EOF-2.6 .. EOF-0.6 s (reverb decaying)", 2000);
        window(tag + " EOF-0.6 .. EOF+1.4 s (crossing the asset end)", 2000);
        window(tag + " EOF+1.4 .. EOF+3.4 s (continued transport past the end)", 2000);
        steps_.push_back(Step{ "proxy-edges: " + tag + " - Stop past the end",
                               [this](juce::String&) -> bool {
                                   hooks_.setPlaybackActive(false);
                                   return true;
                               },
                               10 });
        window(tag + " first 0.5 s after Stop", 500);
        window(tag + " stopped, 0.5-2.5 s after Stop", 2000);
        steps_.push_back(Step{ "proxy-edges: " + tag + " - Play again past the end",
                               [this](juce::String&) -> bool {
                                   hooks_.setPlaybackActive(true);
                                   return true;
                               },
                               10 });
        window(tag + " restart past the end (first 0.5 s)", 500);
        steps_.push_back(Step{ "proxy-edges: " + tag + " - Stop, loop [EOF-3, EOF+2], Cycle on, Play from EOF-3",
                               [this, eofSamples, sr, seekSeconds, eofSec](juce::String&) -> bool {
                                   hooks_.setPlaybackActive(false);
                                   const std::int64_t l = *eofSamples - (std::int64_t)std::llround(3.0 * *sr);
                                   const std::int64_t r = *eofSamples + (std::int64_t)std::llround(2.0 * *sr);
                                   hooks_.setLocatorsSamples(l, r);
                                   hooks_.setCycleEnabled(true);
                                   seekSeconds(eofSec() - 3.0);
                                   juce::Thread::sleep(100);
                                   hooks_.setPlaybackActive(true);
                                   return true;
                               },
                               5200 });
        window(tag + " loop: window containing the wrap from EOF+2 back to EOF-3", 2000);
        steps_.push_back(Step{ "proxy-edges: " + tag + " - Stop, Cycle off",
                               [this](juce::String&) -> bool {
                                   hooks_.setPlaybackActive(false);
                                   hooks_.setCycleEnabled(false);
                                   return true;
                               },
                               300 });
        steps_.push_back(Step{ "proxy-edges: " + tag + " - offline mixdown of [EOF-5, EOF+3] through the insert chain",
                               [this, tag, outDir, eofSamples, sr, say](juce::String& failReason) -> bool {
                                   const std::int64_t l = *eofSamples - (std::int64_t)std::llround(5.0 * *sr);
                                   const std::int64_t r = *eofSamples + (std::int64_t)std::llround(3.0 * *sr);
                                   hooks_.setLocatorsSamples(l, r);
                                   hooks_.setCycleEnabled(true); // the mixdown renders the ACTIVE loop range
                                   const juce::File wav = outDir.getChildFile("mixdown-" + tag.toLowerCase() + "-eof.wav");
                                   (void)wav.deleteFile();
                                   const juce::Result res = hooks_.runMixdownBlocking(wav, false);
                                   hooks_.setCycleEnabled(false);
                                   if (res.failed())
                                   {
                                       failReason = "mixdown failed: " + res.getErrorMessage();
                                       return false;
                                   }
                                   // Analyse the Stereo Out around the EOF crossing: the largest sample-to-sample
                                   // jump and the level per channel in 0.5 s slices.
                                   juce::WavAudioFormat fmt;
                                   std::unique_ptr<juce::AudioFormatReader> reader(fmt.createReaderFor(wav.createInputStream().release(), true));
                                   if (reader == nullptr)
                                   {
                                       failReason = "mixdown WAV unreadable";
                                       return false;
                                   }
                                   const int nch = juce::jmin(2, (int)reader->numChannels);
                                   const std::int64_t n = (std::int64_t)reader->lengthInSamples;
                                   juce::AudioBuffer<float> buf(nch, (int)juce::jmin<std::int64_t>(n, 1 << 22));
                                   reader->read(&buf, 0, buf.getNumSamples(), 0, true, nch > 1);
                                   const std::int64_t eofInFile = *eofSamples - l; // timeline EOF relative to the mixdown start
                                   juce::String slices;
                                   const int slice = (int)std::llround(0.5 * *sr);
                                   double maxJumpNearEof = 0.0, maxJumpElsewhere = 0.0;
                                   std::int64_t jumpAt = -1;
                                   for (int c = 0; c < nch; ++c)
                                   {
                                       const float* d = buf.getReadPointer(c);
                                       for (int i = 1; i < buf.getNumSamples(); ++i)
                                       {
                                           const double j = std::abs((double)d[i] - (double)d[i - 1]);
                                           const bool nearEof = std::abs((std::int64_t)i - eofInFile) < (std::int64_t)(0.5 * *sr);
                                           if (nearEof && j > maxJumpNearEof) { maxJumpNearEof = j; jumpAt = i; }
                                           if (!nearEof && j > maxJumpElsewhere) { maxJumpElsewhere = j; }
                                       }
                                   }
                                   for (int s = 0; s * slice < buf.getNumSamples(); ++s)
                                   {
                                       const int start = s * slice;
                                       const int count = juce::jmin(slice, buf.getNumSamples() - start);
                                       double peak = 0.0, sum = 0.0;
                                       for (int c = 0; c < nch; ++c)
                                       {
                                           const float* d = buf.getReadPointer(c);
                                           for (int i = 0; i < count; ++i) { peak = juce::jmax(peak, std::abs((double)d[start + i])); if (c == 0) { sum += d[start + i]; } }
                                       }
                                       slices << " [" << juce::String((double)(start) / *sr - 5.0, 1) << "s peak=" << juce::String(peak > 0 ? 20.0 * std::log10(peak) : -200.0, 1)
                                              << " dcL=" << juce::String(sum / juce::jmax(1, count), 4) << "]";
                                   }
                                   say(tag + " mixdown [EOF-5, EOF+3] (Stereo Out, after inserts): " + juce::String(n) + " samples, largest sample-to-sample jump within +-0.5 s of EOF = "
                                       + juce::String(maxJumpNearEof, 5) + (jumpAt >= 0 ? " at " + juce::String((double)(jumpAt - eofInFile) / *sr, 3) + " s from EOF" : juce::String())
                                       + ", elsewhere = " + juce::String(maxJumpElsewhere, 5) + " | 0.5 s slices (t rel. EOF):" + slices);
                                   return true;
                               },
                               500 });
    }
    steps_.push_back(Step{ "proxy-edges: write the report",
                           [this, outDir, report, tid](juce::String&) -> bool {
                               hooks_.proxyForcePrimaryUnavailable(tid, false);
                               (void)outDir.getChildFile("proxy-playback-edges.txt").replaceWithText(report->joinIntoString("\n") + "\n");
                               return true;
                           },
                           100 });
}

// -----------------------------------------------------------------------------
// Mixer window (`--stability-mixer <project>`)
// -----------------------------------------------------------------------------
void StabilityScenarioRunner::appendMixerSteps(const juce::File& project)
{
    const StabilityRunnerHooks::MixerHooks& M = hooks_.mixer;
    if (M.toggleLikeF3 == nullptr || M.isVisible == nullptr || M.windowInstanceCount == nullptr || M.capturePng == nullptr
        || M.verifyLayout == nullptr || M.stripOrder == nullptr || M.masterStripScreenBounds == nullptr
        || M.scrollStripsToRight == nullptr || M.clickSectionToggle == nullptr || M.sectionShown == nullptr
        || M.stripFaderType == nullptr || M.stripPanSet == nullptr || M.stripChooseRouting == nullptr
        || M.stripRoutingText == nullptr || M.stripChooseSendDestination == nullptr || M.stripSendDestinationText == nullptr
        || M.stripCommitSendAmount == nullptr || M.stripSendAmountText == nullptr || M.stripCommitPreGain == nullptr
        || M.stripPreGainText == nullptr || M.stripInsertRowsText == nullptr || M.stripInsertMenuAction == nullptr
        || M.stripClickButton == nullptr || M.stripButtonActive == nullptr || M.stripButtonVisible == nullptr
        || M.stripMeterHeldPeak == nullptr || M.inspectorMeterHeldPeak == nullptr || M.meterHubInterest == nullptr
        || M.windowBounds == nullptr || M.setWindowBounds == nullptr || M.audioRuntimeFingerprint == nullptr
        || M.stripKindTexts == nullptr || M.stripFaderValueText == nullptr || M.stripRoutingCaption == nullptr
        || M.stripMeterOverloadLatched == nullptr || M.stripClickMeter == nullptr || M.inspectorMeterOverloadLatched == nullptr
        || hooks_.listAllTracks == nullptr || hooks_.getTrackChannelFaderGain == nullptr || hooks_.activateTrackLikeHeaderClick == nullptr
        || hooks_.describeTrackForDiagnostics == nullptr || hooks_.inspectorFaderValueText == nullptr || hooks_.requestDeleteTrack == nullptr
        || hooks_.invokeUndo == nullptr || hooks_.seekTransportTo == nullptr || hooks_.setPlaybackActive == nullptr
        || hooks_.getActiveLoopSpan == nullptr || hooks_.loadProjectFromFile == nullptr || hooks_.saveProject == nullptr)
    {
        steps_.push_back(Step{ "mixer: hooks missing",
                               [](juce::String& failReason) -> bool {
                                   failReason = "mixer hooks not installed";
                                   return false;
                               },
                               0 });
        return;
    }

    auto evidenceDir = std::make_shared<juce::File>(
        juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("dal-stability-mixer"));
    (void)evidenceDir->deleteRecursively();
    (void)evidenceDir->createDirectory();
    // Sibling copy: every edit below lands in the copy, the user's project is never written.
    const juce::File copy = project.getSiblingFile(project.getFileNameWithoutExtension() + "-mixer.dalproj");
    steps_.push_back(Step{ "mixer: sibling copy",
                           [project, copy](juce::String& failReason) -> bool {
                               (void)copy.deleteFile();
                               if (!project.copyFileTo(copy))
                               {
                                   failReason = "could not copy the project";
                                   return false;
                               }
                               return true;
                           },
                           100 });
    appendLoadAndVerifySteps(copy, "mixer");

    const auto say = [this](const juce::String& line) { appendStabilityRunLine("  " + line); };
    const auto verifyAndCapture = [this, evidenceDir, M](const juce::String& label, juce::String& failReason) -> bool {
        juce::String report;
        const bool ok = M.verifyLayout(report, failReason);
        for (const auto& line : juce::StringArray::fromLines(report))
        {
            if (line.isNotEmpty())
            {
                appendStabilityRunLine("    " + line);
            }
        }
        const juce::File png = evidenceDir->getChildFile("mixer-" + label + ".png");
        if (M.capturePng(png))
        {
            appendStabilityRunLine("  evidence: " + png.getFullPathName());
        }
        return ok;
    };
    auto audioTid = std::make_shared<TrackId>(kInvalidTrackId);
    auto otherTid = std::make_shared<TrackId>(kInvalidTrackId);
    auto groupName = std::make_shared<juce::String>();
    auto instrumentTid = std::make_shared<TrackId>(kInvalidTrackId);
    auto faderBefore = std::make_shared<float>(1.0f);
    auto otherFaderBefore = std::make_shared<float>(1.0f);
    auto insertTid = std::make_shared<TrackId>(kInvalidTrackId);
    auto insertRowsBefore = std::make_shared<juce::String>();
    auto fingerprintBefore = std::make_shared<juce::String>();
    auto boundsBefore = std::make_shared<juce::Rectangle<int>>();
    auto sectionFlagsBefore = std::make_shared<std::vector<bool>>();
    const juce::StringArray sectionKeys{ "routing", "preGain", "preInserts", "postInserts", "sends", "faders", "meters" };

    // 1. F3 / close / reopen — one instance.
    steps_.push_back(Step{ "mixer: F3 shows the window (one instance), F3 hides, F3 shows again",
                           [this, M, say](juce::String& failReason) -> bool {
                               if (M.isVisible())
                               {
                                   M.toggleLikeF3();
                               }
                               M.toggleLikeF3();
                               if (!M.isVisible())
                               {
                                   failReason = "F3 did not show the mixer";
                                   return false;
                               }
                               const int n1 = M.windowInstanceCount();
                               M.toggleLikeF3();
                               const bool hidden = !M.isVisible();
                               M.toggleLikeF3();
                               const int n2 = M.windowInstanceCount();
                               say("mixer visible after F3; instances=" + juce::String(n1) + " hidden after second F3=" + juce::String(hidden ? "yes" : "no")
                                   + " instances after third F3=" + juce::String(n2));
                               if (!hidden || n1 != 1 || n2 != 1 || !M.isVisible())
                               {
                                   failReason = "F3 toggle / single instance failed";
                                   return false;
                               }
                               return true;
                           },
                           600 });

    // 1b. A Group row (send / output target) when the project has none — added like the menu does.
    steps_.push_back(Step{ "mixer: add a Group row when the project has none (routing / send target)",
                           [this, M, say](juce::String& failReason) -> bool {
                               for (const StabilityTrackInfo& t : hooks_.listAllTracks())
                               {
                                   if (t.kindName == "group")
                                   {
                                       say("group row present: " + t.name);
                                       return true;
                                   }
                               }
                               if (M.addGroupTrackLikeUi == nullptr)
                               {
                                   say("no group and no add hook: routing / send checks will be skipped");
                                   return true;
                               }
                               const TrackId gid = M.addGroupTrackLikeUi();
                               if (gid == kInvalidTrackId)
                               {
                                   failReason = "could not add a Group row";
                                   return false;
                               }
                               say("added group row " + juce::String((juce::int64)gid));
                               return true;
                           },
                           500 });

    // 2. Strips per kind, arrangement order, fixed Stereo Out.
    steps_.push_back(Step{ "mixer: strips follow the arrangement order; every kind present; Stereo Out fixed",
                           [this, M, say, verifyAndCapture, audioTid, otherTid, groupName, instrumentTid, insertTid](juce::String& failReason) -> bool {
                               std::vector<TrackId> expected;
                               for (const StabilityTrackInfo& t : hooks_.listAllTracks())
                               {
                                   if (t.kindName == "master")
                                   {
                                       continue;
                                   }
                                   expected.push_back(t.id);
                                   if (t.kindName == "audio" && *audioTid == kInvalidTrackId)
                                   {
                                       *audioTid = t.id;
                                   }
                                   else if (t.kindName == "group" && groupName->isEmpty())
                                   {
                                       *groupName = t.name;
                                   }
                                   if (t.kindName == "instrument" && *instrumentTid == kInvalidTrackId)
                                   {
                                       *instrumentTid = t.id;
                                   }
                                   const juce::String desc = hooks_.describeTrackForDiagnostics(t.id);
                                   if (*insertTid == kInvalidTrackId && desc.contains("inserts=[") && !desc.contains("inserts=[]"))
                                   {
                                       *insertTid = t.id;
                                   }
                               }
                               for (const StabilityTrackInfo& t : hooks_.listAllTracks())
                               {
                                   if (t.kindName != "master" && t.id != *audioTid && *otherTid == kInvalidTrackId)
                                   {
                                       *otherTid = t.id;
                                   }
                               }
                               const std::vector<TrackId> order = M.stripOrder();
                               say("strips: " + M.stripKindTexts());
                               if (order != expected)
                               {
                                   failReason = "strip order differs from the arrangement order";
                                   return false;
                               }
                               const juce::Rectangle<int> masterBefore = M.masterStripScreenBounds();
                               const int scrolledX = M.scrollStripsToRight();
                               const juce::Rectangle<int> masterAfter = M.masterStripScreenBounds();
                               say("horizontal scroll to x=" + juce::String(scrolledX) + "; Stereo Out strip at " + masterBefore.toString() + " -> " + masterAfter.toString());
                               if (masterBefore.isEmpty() || masterBefore != masterAfter)
                               {
                                   failReason = "the Stereo Out strip moved with the horizontal scroll";
                                   return false;
                               }
                               return verifyAndCapture("all-sections-scrolled", failReason);
                           },
                           400 });

    // 3. Section toggles: hide routing / inserts / sends → aligned faders + meters; then restore.
    steps_.push_back(Step{ "mixer: hide Routing, Pre inserts, Post inserts, Sends through the toolbar",
                           [this, M, say, sectionKeys, sectionFlagsBefore](juce::String& failReason) -> bool {
                               sectionFlagsBefore->clear();
                               for (const auto& k : sectionKeys)
                               {
                                   sectionFlagsBefore->push_back(M.sectionShown(k));
                               }
                               for (const char* k : { "routing", "preInserts", "postInserts", "sends" })
                               {
                                   if (M.sectionShown(k))
                                   {
                                       M.clickSectionToggle(k);
                                   }
                               }
                               juce::String flags;
                               for (const auto& k : sectionKeys)
                               {
                                   flags << k << "=" << (M.sectionShown(k) ? "1" : "0") << " ";
                               }
                               say("sections: " + flags);
                               if (M.sectionShown("routing") || M.sectionShown("sends") || !M.sectionShown("faders") || !M.sectionShown("meters"))
                               {
                                   failReason = "section toggles did not apply independently";
                                   return false;
                               }
                               return true;
                           },
                           400 });
    steps_.push_back(Step{ "mixer: layout with faders + meters only",
                           [verifyAndCapture](juce::String& failReason) -> bool { return verifyAndCapture("faders-meters", failReason); },
                           200 });
    steps_.push_back(Step{ "mixer: hide Meters too (faders alone), then Faders alone hidden (meters alone)",
                           [this, M, verifyAndCapture](juce::String& failReason) -> bool {
                               M.clickSectionToggle("meters");
                               if (!verifyAndCapture("faders-only", failReason))
                               {
                                   return false;
                               }
                               M.clickSectionToggle("meters");
                               M.clickSectionToggle("faders");
                               if (!verifyAndCapture("meters-only", failReason))
                               {
                                   return false;
                               }
                               M.clickSectionToggle("faders");
                               return true;
                           },
                           300 });
    steps_.push_back(Step{ "mixer: show every section again",
                           [this, M, sectionKeys, verifyAndCapture](juce::String& failReason) -> bool {
                               for (const auto& k : sectionKeys)
                               {
                                   if (!M.sectionShown(k))
                                   {
                                       M.clickSectionToggle(k);
                                   }
                               }
                               return verifyAndCapture("all-sections", failReason);
                           },
                           400 });

    // 4. TrackId binding: edit a NON-active row through its strip; the session and the Inspector follow.
    steps_.push_back(Step{ "mixer: activate another row, then edit the audio row's fader / pan / pre-gain / routing / send through its strip",
                           [this, M, say, audioTid, otherTid, groupName, faderBefore, otherFaderBefore](juce::String& failReason) -> bool {
                               if (*audioTid == kInvalidTrackId)
                               {
                                   failReason = "no audio row in this project";
                                   return false;
                               }
                               if (*otherTid != kInvalidTrackId)
                               {
                                   hooks_.activateTrackLikeHeaderClick(*otherTid);
                                   *otherFaderBefore = hooks_.getTrackChannelFaderGain(*otherTid);
                               }
                               *faderBefore = hooks_.getTrackChannelFaderGain(*audioTid);
                               if (!M.stripFaderType(*audioTid, "-6"))
                               {
                                   failReason = "no strip for the audio row";
                                   return false;
                               }
                               const float gainAfter = hooks_.getTrackChannelFaderGain(*audioTid);
                               const float otherAfter = *otherTid != kInvalidTrackId ? hooks_.getTrackChannelFaderGain(*otherTid) : 0.0f;
                               say("fader typed -6 on the audio strip: session gain " + juce::String(*faderBefore, 3) + " -> " + juce::String(gainAfter, 3)
                                   + " (expected 0.501); other (active) row " + juce::String(*otherFaderBefore, 3) + " -> " + juce::String(otherAfter, 3));
                               if (std::abs(gainAfter - 0.501f) > 0.01f)
                               {
                                   failReason = "the typed fader value did not reach the audio row's session gain";
                                   return false;
                               }
                               if (*otherTid != kInvalidTrackId && std::abs(otherAfter - *otherFaderBefore) > 1.0e-6f)
                               {
                                   failReason = "the active (other) row's gain changed";
                                   return false;
                               }
                               (void)M.stripPanSet(*audioTid, 0.5f);
                               (void)M.stripCommitPreGain(*audioTid, "+3");
                               const juce::String desc = hooks_.describeTrackForDiagnostics(*audioTid);
                               say("after pan 0.5 / pre-gain +3: " + desc);
                               if (!desc.contains("pan=0.50") || !desc.contains("preGainDb=3.00"))
                               {
                                   failReason = "pan / pre-gain did not reach the audio row";
                                   return false;
                               }
                               if (groupName->isNotEmpty())
                               {
                                   const juce::String routingBefore = M.stripRoutingText(*audioTid, 1);
                                   if (!M.stripChooseRouting(*audioTid, 1, *groupName))
                                   {
                                       failReason = "the group is not offered as the audio row's output";
                                       return false;
                                   }
                                   say("audio output " + routingBefore + " -> " + M.stripRoutingText(*audioTid, 1) + " | " + hooks_.describeTrackForDiagnostics(*audioTid));
                                   if (!M.stripChooseSendDestination(*audioTid, 0, *groupName))
                                   {
                                       failReason = "the group is not offered as a send destination";
                                       return false;
                                   }
                                   (void)M.stripCommitSendAmount(*audioTid, 0, "-6");
                                   say("send 1 -> " + M.stripSendDestinationText(*audioTid, 0) + " amount " + M.stripSendAmountText(*audioTid, 0));
                               }
                               else
                               {
                                   say("no Group row: routing / send edits skipped");
                               }
                               return true;
                           },
                           400 });
    steps_.push_back(Step{ "mixer: the strip's selectors show the committed routing / send after the poll",
                           [this, M, say, audioTid, groupName](juce::String& failReason) -> bool {
                               const juce::String desc = hooks_.describeTrackForDiagnostics(*audioTid);
                               say("audio row now: " + desc + " | strip output \"" + M.stripRoutingText(*audioTid, 1) + "\" send 1 \""
                                   + M.stripSendDestinationText(*audioTid, 0) + "\" " + M.stripSendAmountText(*audioTid, 0));
                               if (groupName->isNotEmpty())
                               {
                                   if (M.stripRoutingText(*audioTid, 1) != *groupName)
                                   {
                                       failReason = "the output selector does not show the routing edit";
                                       return false;
                                   }
                                   if (M.stripSendDestinationText(*audioTid, 0) != *groupName || !M.stripSendAmountText(*audioTid, 0).startsWith("-6.0"))
                                   {
                                       failReason = "the send row does not show the send edit";
                                       return false;
                                   }
                               }
                               return true;
                           },
                           200 });
    steps_.push_back(Step{ "mixer: activate the audio row like a header click",
                           [this, audioTid](juce::String&) -> bool {
                               hooks_.activateTrackLikeHeaderClick(*audioTid);
                               return true;
                           },
                           300 });
    steps_.push_back(Step{ "mixer: the Inspector shows the same values as the strip for the now-active audio row",
                           [this, M, say, audioTid](juce::String& failReason) -> bool {
                               const juce::String inspectorFader = hooks_.inspectorFaderValueText();
                               const juce::String stripFader = M.stripFaderValueText(*audioTid);
                               const juce::String inspectorPreGain = hooks_.inspectorPreGainFieldText ? hooks_.inspectorPreGainFieldText() : juce::String("?");
                               say("Inspector fader \"" + inspectorFader + "\" / strip \"" + stripFader + "\"; Inspector pre-gain \"" + inspectorPreGain
                                   + "\" / strip \"" + M.stripPreGainText(*audioTid) + "\"");
                               if (inspectorFader != stripFader || !inspectorFader.contains("-6"))
                               {
                                   failReason = "Inspector and mixer fader texts differ";
                                   return false;
                               }
                               if (hooks_.inspectorPreGainFieldText && inspectorPreGain != M.stripPreGainText(*audioTid))
                               {
                                   failReason = "Inspector and mixer pre-gain texts differ";
                                   return false;
                               }
                               return true;
                           },
                           400 });

    // 5. Insert row actions through the strip (stage move, then undo).
    steps_.push_back(Step{ "mixer: insert row 'move to the other stage' through the strip",
                           [this, M, say, insertTid, insertRowsBefore](juce::String& failReason) -> bool {
                               if (*insertTid == kInvalidTrackId)
                               {
                                   say("no row with inserts in this project: insert actions skipped");
                                   return true;
                               }
                               *insertRowsBefore = M.stripInsertRowsText(*insertTid);
                               say("row " + juce::String((juce::int64)*insertTid) + " strip inserts: " + *insertRowsBefore + " | host: " + hooks_.describeTrackForDiagnostics(*insertTid));
                               const bool pre = insertRowsBefore->startsWith("Pre:");
                               if (!M.stripInsertMenuAction(*insertTid, pre, 0, 4))
                               {
                                   failReason = "insert menu action refused";
                                   return false;
                               }
                               return true;
                           },
                           400 });
    steps_.push_back(Step{ "mixer: the strip and the host show the moved insert; undo",
                           [this, M, say, insertTid, insertRowsBefore](juce::String& failReason) -> bool {
                               if (*insertTid == kInvalidTrackId)
                               {
                                   return true;
                               }
                               const juce::String after = M.stripInsertRowsText(*insertTid);
                               say("after 'move to other stage': " + after + " | host: " + hooks_.describeTrackForDiagnostics(*insertTid));
                               if (after == *insertRowsBefore)
                               {
                                   failReason = "the stage move did not change the row's inserts";
                                   return false;
                               }
                               hooks_.invokeUndo();
                               return true;
                           },
                           500 });
    steps_.push_back(Step{ "mixer: undo restored the insert stages on the strip",
                           [this, M, say, insertTid, insertRowsBefore](juce::String& failReason) -> bool {
                               if (*insertTid == kInvalidTrackId)
                               {
                                   return true;
                               }
                               const juce::String restored = M.stripInsertRowsText(*insertTid);
                               say("after undo: " + restored);
                               if (restored != *insertRowsBefore)
                               {
                                   failReason = "undo did not restore the insert stages";
                                   return false;
                               }
                               return true;
                           },
                           200 });

    // 6. Base buttons through the strip.
    auto muteBefore = std::make_shared<bool>(false);
    steps_.push_back(Step{ "mixer: Mute through the strip",
                           [this, M, say, audioTid, muteBefore](juce::String&) -> bool {
                               *muteBefore = M.stripButtonActive(*audioTid, "mute");
                               (void)M.stripClickButton(*audioTid, "mute");
                               say("mute was " + juce::String(*muteBefore ? "on" : "off") + " | " + hooks_.describeTrackForDiagnostics(*audioTid));
                               return true;
                           },
                           300 });
    steps_.push_back(Step{ "mixer: the strip shows the toggled mute; toggle back",
                           [this, M, say, audioTid, muteBefore](juce::String& failReason) -> bool {
                               const bool after = M.stripButtonActive(*audioTid, "mute");
                               const juce::String desc = hooks_.describeTrackForDiagnostics(*audioTid);
                               say("mute now " + juce::String(after ? "on" : "off") + " | " + desc);
                               if (after == *muteBefore || !desc.contains(after ? "muted=yes" : "muted=no"))
                               {
                                   failReason = "mute did not toggle through the strip";
                                   return false;
                               }
                               (void)M.stripClickButton(*audioTid, "mute");
                               return true;
                           },
                           300 });

    // 7. Concurrent meters while playing.
    steps_.push_back(Step{ "mixer: play from 24 s with the instrument row active",
                           [this, M, say, instrumentTid, audioTid, fingerprintBefore](juce::String&) -> bool {
                               const TrackId active = *instrumentTid != kInvalidTrackId ? *instrumentTid : *audioTid;
                               hooks_.activateTrackLikeHeaderClick(active);
                               // 24 s into the arrangement: inside the material of the test projects used here
                               // (the loop span is only "active" with Cycle on, so seek by the device rate).
                               const double sr = hooks_.getDeviceSampleRate != nullptr && hooks_.getDeviceSampleRate() > 0.0 ? hooks_.getDeviceSampleRate() : 48000.0;
                               hooks_.seekTransportTo((std::int64_t)(24.0 * sr));
                               hooks_.setPlaybackActive(true);
                               *fingerprintBefore = M.audioRuntimeFingerprint();
                               say("playing from 24 s at " + juce::String(sr) + " Hz");
                               return true;
                           },
                           3500 });
    steps_.push_back(Step{ "mixer: two rows + Stereo Out meter concurrently; Inspector and mixer agree on the active row",
                           [this, M, say, verifyAndCapture, instrumentTid, audioTid](juce::String& failReason) -> bool {
                               std::vector<TrackId> signalRows;
                               TrackId master = kInvalidTrackId;
                               juce::String levels;
                               for (const StabilityTrackInfo& t : hooks_.listAllTracks())
                               {
                                   if (t.kindName == "master")
                                   {
                                       master = t.id;
                                   }
                                   const float held = M.stripMeterHeldPeak(t.id);
                                   levels << t.name << "=" << juce::String(held, 3) << " ";
                                   if (t.kindName != "master" && held > 0.001f)
                                   {
                                       signalRows.push_back(t.id);
                                   }
                               }
                               say("held peaks: " + levels);
                               const float masterHeld = master != kInvalidTrackId ? M.stripMeterHeldPeak(master) : 0.0f;
                               const TrackId active = *instrumentTid != kInvalidTrackId ? *instrumentTid : *audioTid;
                               const float inspectorHeld = M.inspectorMeterHeldPeak();
                               const float stripHeld = M.stripMeterHeldPeak(active);
                               const std::vector<TrackId> interest = M.meterHubInterest();
                               juce::String interestText;
                               for (const TrackId id : interest)
                               {
                                   interestText << juce::String((juce::int64)id) << " ";
                               }
                               say("Stereo Out held=" + juce::String(masterHeld, 3) + " | active row " + juce::String((juce::int64)active) + ": Inspector held="
                                   + juce::String(inspectorHeld, 3) + " mixer held=" + juce::String(stripHeld, 3) + " | hub interest: " + interestText);
                               if (!verifyAndCapture("playing", failReason))
                               {
                                   return false;
                               }
                               if (signalRows.size() < 2)
                               {
                                   failReason = "fewer than two rows show signal while playing";
                                   return false;
                               }
                               if (masterHeld <= 0.001f)
                               {
                                   failReason = "the Stereo Out strip shows no signal";
                                   return false;
                               }
                               if (std::abs(inspectorHeld - stripHeld) > 0.05f * juce::jmax(0.02f, stripHeld))
                               {
                                   failReason = "Inspector and mixer meters of the active row disagree";
                                   return false;
                               }
                               if (std::find(interest.begin(), interest.end(), active) == interest.end())
                               {
                                   failReason = "the active row is not in the hub's metered set";
                                   return false;
                               }
                               return true;
                           },
                           100 });

    // 8. Open / close while playing leaves the audio runtime untouched.
    for (int i = 0; i < 5; ++i)
    {
        steps_.push_back(Step{ "mixer: toggle the window while playing (" + juce::String(i + 1) + "/5)",
                               [M](juce::String&) -> bool {
                                   M.toggleLikeF3();
                                   return true;
                               },
                               150 });
    }
    steps_.push_back(Step{ "mixer: hidden -> no metered mixer rows; shown again -> runtime fingerprint unchanged",
                           [this, M, say, fingerprintBefore](juce::String& failReason) -> bool {
                               if (M.isVisible())
                               {
                                   M.toggleLikeF3();
                               }
                               const int hiddenInterest = (int)M.meterHubInterest().size();
                               M.toggleLikeF3();
                               const juce::String after = M.audioRuntimeFingerprint();
                               say("runtime before: " + *fingerprintBefore);
                               say("runtime after:  " + after + " | metered rows while hidden: " + juce::String(hiddenInterest) + " (the Inspector's row only)");
                               hooks_.setPlaybackActive(false);
                               if (after != *fingerprintBefore)
                               {
                                   failReason = "the audio runtime fingerprint changed across mixer open / close";
                                   return false;
                               }
                               if (hiddenInterest > 1)
                               {
                                   failReason = "hidden mixer still meters rows";
                                   return false;
                               }
                               return true;
                           },
                           400 });

    // 9. Track deletion and project reload.
    auto stripsBeforeDelete = std::make_shared<int>(0);
    steps_.push_back(Step{ "mixer: delete a row through the header's Delete Track path",
                           [this, M, say, otherTid, stripsBeforeDelete](juce::String&) -> bool {
                               *stripsBeforeDelete = (int)M.stripOrder().size();
                               if (*otherTid == kInvalidTrackId)
                               {
                                   say("only one non-master row: deletion check skipped");
                                   return true;
                               }
                               hooks_.requestDeleteTrack(*otherTid);
                               return true;
                           },
                           600 });
    steps_.push_back(Step{ "mixer: the deleted row's strip is gone; undo",
                           [this, M, say, otherTid, stripsBeforeDelete](juce::String& failReason) -> bool {
                               if (*otherTid == kInvalidTrackId)
                               {
                                   return true;
                               }
                               const std::vector<TrackId> order = M.stripOrder();
                               const bool gone = std::find(order.begin(), order.end(), *otherTid) == order.end();
                               say("strips " + juce::String(*stripsBeforeDelete) + " -> " + juce::String((int)order.size()) + "; deleted row strip gone=" + juce::String(gone ? "yes" : "no"));
                               if (!gone || (int)order.size() != *stripsBeforeDelete - 1)
                               {
                                   failReason = "the deleted row's strip did not disappear";
                                   return false;
                               }
                               hooks_.invokeUndo();
                               return true;
                           },
                           800 });
    steps_.push_back(Step{ "mixer: undo brought the strip back",
                           [this, M, say, otherTid, stripsBeforeDelete](juce::String& failReason) -> bool {
                               if (*otherTid == kInvalidTrackId)
                               {
                                   return true;
                               }
                               const std::vector<TrackId> restored = M.stripOrder();
                               say("after undo: " + juce::String((int)restored.size()) + " strips: " + M.stripKindTexts());
                               if ((int)restored.size() != *stripsBeforeDelete)
                               {
                                   failReason = "undo did not restore the strip";
                                   return false;
                               }
                               return true;
                           },
                           200 });
    steps_.push_back(Step{ "mixer: save the copy",
                           [this](juce::String&) -> bool {
                               hooks_.saveProject();
                               return true;
                           },
                           500 });
    appendLoadAndVerifySteps(copy, "mixer-reload");
    steps_.push_back(Step{ "mixer: strips and the metered set were rebuilt for the reloaded project",
                           [this, M, say](juce::String& failReason) -> bool {
                               std::vector<TrackId> expected;
                               for (const StabilityTrackInfo& t : hooks_.listAllTracks())
                               {
                                   if (t.kindName != "master")
                                   {
                                       expected.push_back(t.id);
                                   }
                               }
                               const std::vector<TrackId> order = M.stripOrder();
                               const std::vector<TrackId> interest = M.meterHubInterest();
                               bool interestOk = true;
                               for (const TrackId id : interest)
                               {
                                   if (std::find(expected.begin(), expected.end(), id) == expected.end())
                                   {
                                       interestOk = false;
                                   }
                               }
                               say("strips after reload: " + M.stripKindTexts() + " | metered rows all in snapshot: " + juce::String(interestOk ? "yes" : "no"));
                               if (order != expected || !interestOk)
                               {
                                   failReason = "strips / metered rows do not match the reloaded project";
                                   return false;
                               }
                               return true;
                           },
                           400 });

    // 10. Persistence of bounds + section flags; header width untouched.
    steps_.push_back(Step{ "mixer: move / resize the window and hide Sends",
                           [this, M, say, boundsBefore](juce::String&) -> bool {
                               if (!M.isVisible())
                               {
                                   M.toggleLikeF3();
                               }
                               *boundsBefore = M.windowBounds();
                               const juce::Rectangle<int> target = boundsBefore->withSize(1000, 640).translated(20, 10);
                               M.setWindowBounds(target);
                               M.clickSectionToggle("sends");
                               say("window " + boundsBefore->toString() + " -> " + M.windowBounds().toString() + ", sends hidden");
                               return true;
                           },
                           900 }); // > the 300 ms bounds settle
    steps_.push_back(Step{ "mixer: ui-layout.xml stores the bounds and the Sends flag and keeps the header width",
                           [this, M, say, boundsBefore](juce::String& failReason) -> bool {
                               const juce::File file = UiLayoutSettingsStore::defaultFile();
                               UiLayoutSettingsStore fresh(file);
                               fresh.loadFromFile();
                               const auto stored = fresh.getMixerWindowBounds();
                               const auto sends = fresh.getMixerSectionShown("sends");
                               const auto headerW = fresh.getTrackHeaderColumnWidthPx();
                               say("stored bounds " + (stored ? stored->toString() : juce::String("(none)")) + " (window " + M.windowBounds().toString()
                                   + "); sends=" + (sends ? juce::String(*sends ? "1" : "0") : juce::String("(absent)")) + "; header width "
                                   + (headerW ? juce::String(*headerW) : juce::String("(absent)")));
                               M.clickSectionToggle("sends");
                               M.setWindowBounds(*boundsBefore);
                               if (!stored || stored->getWidth() != 1000 || stored->getHeight() != 640 || !sends || *sends)
                               {
                                   failReason = "mixer bounds / section flag were not persisted as expected";
                                   return false;
                               }
                               return true;
                           },
                           600 });
    // 11. (1.1.17) The Inspector's pan control inside a NON-active strip: handler-level drag.
    steps_.push_back(Step{ "mixer: pan drag on a non-active strip (press stick, drag, release) reaches that row; Inspector shows the same",
                           [this, M, say, audioTid, otherTid, evidenceDir](juce::String& failReason) -> bool {
                               if (M.stripPanDragLikeMouse == nullptr || M.stripPanValue == nullptr || M.inspectorPanValue == nullptr)
                               {
                                   say("pan hooks missing: skipped");
                                   return true;
                               }
                               if (*otherTid != kInvalidTrackId)
                               {
                                   hooks_.activateTrackLikeHeaderClick(*otherTid);
                               }
                               (void)M.stripPanSet(*audioTid, 0.0f);
                               if (!M.stripPanDragLikeMouse(*audioTid, -0.6f))
                               {
                                   failReason = "pan drag could not run on the audio strip";
                                   return false;
                               }
                               const juce::String afterLeft = hooks_.describeTrackForDiagnostics(*audioTid);
                               const float stripLeft = M.stripPanValue(*audioTid);
                               (void)M.capturePng(evidenceDir->getChildFile("mixer-pan-left.png"));
                               (void)M.stripPanDragLikeMouse(*audioTid, 0.75f);
                               const juce::String afterRight = hooks_.describeTrackForDiagnostics(*audioTid);
                               const float stripRight = M.stripPanValue(*audioTid);
                               (void)M.capturePng(evidenceDir->getChildFile("mixer-pan-right.png"));
                               say("pan drag left: strip=" + juce::String(stripLeft, 2) + " | " + afterLeft);
                               say("pan drag right: strip=" + juce::String(stripRight, 2) + " | " + afterRight);
                               if (stripLeft > -0.4f || stripRight < 0.5f || !afterLeft.contains("pan=-0.") || !afterRight.contains("pan=0."))
                               {
                                   failReason = "the pan drag did not reach the audio row";
                                   return false;
                               }
                               hooks_.activateTrackLikeHeaderClick(*audioTid);
                               return true;
                           },
                           400 });
    steps_.push_back(Step{ "mixer: Inspector pan equals the strip's pan for the now-active row; back to centre via Ctrl-click path",
                           [this, M, say, audioTid, evidenceDir](juce::String& failReason) -> bool {
                               if (M.stripPanValue == nullptr || M.inspectorPanValue == nullptr)
                               {
                                   return true;
                               }
                               const float strip = M.stripPanValue(*audioTid);
                               const float inspector = M.inspectorPanValue();
                               say("pan: strip=" + juce::String(strip, 3) + " inspector=" + juce::String(inspector, 3));
                               if (std::abs(strip - inspector) > 1.0e-3f)
                               {
                                   failReason = "Inspector and mixer pan differ";
                                   return false;
                               }
                               (void)M.stripPanSet(*audioTid, 0.0f);
                               (void)M.capturePng(evidenceDir->getChildFile("mixer-pan-centre.png"));
                               return true;
                           },
                           300 });

    // 12. (1.1.17) Dividers: with a 900 px window (spare height above the fader minimum) drag the
    // Pre|Post inserts divider and the Sends|faders divider; alignment holds.
    auto boundsBeforeDividers = std::make_shared<juce::Rectangle<int>>();
    steps_.push_back(Step{ "mixer: window 900 px high, then drag Pre|Post inserts down 54 px and Sends|faders down 60 px",
                           [this, M, say, verifyAndCapture, boundsBeforeDividers](juce::String& failReason) -> bool {
                               if (M.dragDivider == nullptr || M.dividerCount == nullptr || M.sectionHeightsText == nullptr)
                               {
                                   say("divider hooks missing: skipped");
                                   return true;
                               }
                               *boundsBeforeDividers = M.windowBounds();
                               M.setWindowBounds(boundsBeforeDividers->withHeight(900));
                               if (M.resetSectionHeights != nullptr)
                               {
                                   M.resetSectionHeights(); // known start (a previous run may have persisted heights)
                               }
                               say("heights before: " + M.sectionHeightsText() + " dividers=" + juce::String(M.dividerCount()));
                               // Divider order with every section shown: 0 routing|preGain, 1 preGain|preInserts,
                               // 2 preInserts|postInserts, 3 postInserts|sends, 4 sends|lower.
                               if (M.dividerCount() != 5)
                               {
                                   failReason = "expected five dividers with every section shown, got " + juce::String(M.dividerCount());
                                   return false;
                               }
                               if (!M.dragDivider(2, 54))
                               {
                                   failReason = "divider 2 (Pre | Post inserts) could not be dragged";
                                   return false;
                               }
                               say("after Pre|Post drag (+54; Post stops at its minimum): " + M.sectionHeightsText());
                               if (!M.dragDivider(4, 60))
                               {
                                   failReason = "divider 4 (Sends | faders) could not be dragged";
                                   return false;
                               }
                               say("after Sends|faders drag (+60): " + M.sectionHeightsText());
                               if (!M.sectionHeightsText().contains("preInserts=87") || !M.sectionHeightsText().contains("postInserts=51")
                                   || !M.sectionHeightsText().contains("sends=155"))
                               {
                                   failReason = "divider drags did not redistribute as expected";
                                   return false;
                               }
                               return verifyAndCapture("dividers-dragged", failReason);
                           },
                           400 });
    steps_.push_back(Step{ "mixer: hiding Post inserts removes its divider; the Pre inserts height is kept when it returns",
                           [this, M, say, verifyAndCapture](juce::String& failReason) -> bool {
                               if (M.dragDivider == nullptr)
                               {
                                   return true;
                               }
                               const juce::String before = M.sectionHeightsText();
                               const int dividersBefore = M.dividerCount();
                               M.clickSectionToggle("postInserts");
                               const int dividersHidden = M.dividerCount();
                               const bool okHidden = verifyAndCapture("post-inserts-hidden", failReason);
                               M.clickSectionToggle("postInserts");
                               const juce::String after = M.sectionHeightsText();
                               say("dividers " + juce::String(dividersBefore) + " -> " + juce::String(dividersHidden) + " -> " + juce::String(M.dividerCount())
                                   + "; heights before \"" + before + "\" after \"" + after + "\"");
                               if (!okHidden)
                               {
                                   return false;
                               }
                               if (dividersHidden != dividersBefore - 1 || M.dividerCount() != dividersBefore || before != after)
                               {
                                   failReason = "divider count / kept heights wrong across hide + show";
                                   return false;
                               }
                               return true;
                           },
                           300 });

    // 13. (1.1.17) Six inserts in one stage: all reachable through the scrolling list.
    const juce::File delayBundle("C:\\Program Files\\Common Files\\VST3\\DALMonoDelay.vst3");
    steps_.push_back(Step{ "mixer: add six Post inserts (DAL Mono Delay) to the audio row through the picker's call",
                           [this, M, say, audioTid, delayBundle](juce::String& failReason) -> bool {
                               if (hooks_.addInsertLikePicker == nullptr || M.stripInsertRowCount == nullptr)
                               {
                                   say("insert hooks missing: skipped");
                                   return true;
                               }
                               if (!delayBundle.exists())
                               {
                                   say("DAL Mono Delay bundle not installed: six-insert check skipped");
                                   return true;
                               }
                               for (int i = 0; i < 6; ++i)
                               {
                                   const juce::Result r = hooks_.addInsertLikePicker(*audioTid, false, delayBundle);
                                   if (r.failed())
                                   {
                                       failReason = "adding insert " + juce::String(i + 1) + " failed: " + r.getErrorMessage();
                                       return false;
                                   }
                               }
                               return true;
                           },
                           700 });
    steps_.push_back(Step{ "mixer: the strip lists all six, the band shows fewer, the list scrolls to the last and the last row acts on its own slot",
                           [this, M, say, audioTid, delayBundle, verifyAndCapture](juce::String& failReason) -> bool {
                               if (hooks_.addInsertLikePicker == nullptr || M.stripInsertRowCount == nullptr || !delayBundle.exists())
                               {
                                   return true;
                               }
                               const int rows = M.stripInsertRowCount(*audioTid, false);
                               const int visibleBefore = M.stripVisibleInsertRowCount(*audioTid, false);
                               const bool scrollable = M.stripInsertListScrollable(*audioTid, false);
                               say("post inserts: rows=" + juce::String(rows) + " visible=" + juce::String(visibleBefore) + " scrollable=" + juce::String(scrollable ? "yes" : "no")
                                   + " " + M.stripInsertListAndAddBounds(*audioTid, false));
                               if (rows < 6)
                               {
                                   failReason = "the strip does not list every insert (" + juce::String(rows) + ")";
                                   return false;
                               }
                               if (!scrollable || visibleBefore >= rows)
                               {
                                   failReason = "six inserts in a default band must scroll";
                                   return false;
                               }
                               if (!M.stripScrollInsertListToRow(*audioTid, false, rows - 1))
                               {
                                   failReason = "could not scroll the list to the last row";
                                   return false;
                               }
                               if (!verifyAndCapture("six-inserts-scrolled", failReason))
                               {
                                   return false;
                               }
                               // Remove the LAST row through the strip's menu action: the host must lose exactly
                               // that slot (the row carried its own slot id).
                               const juce::String hostBefore = hooks_.describeTrackForDiagnostics(*audioTid);
                               if (!M.stripInsertMenuAction(*audioTid, false, rows - 1, 5))
                               {
                                   failReason = "remove action on the last row refused";
                                   return false;
                               }
                               say("host before remove: " + hostBefore);
                               return true;
                           },
                           400 });
    steps_.push_back(Step{ "mixer: after the remove the strip has five rows; dragging the band taller shows them all",
                           [this, M, say, audioTid, delayBundle, verifyAndCapture](juce::String& failReason) -> bool {
                               if (hooks_.addInsertLikePicker == nullptr || M.stripInsertRowCount == nullptr || !delayBundle.exists())
                               {
                                   return true;
                               }
                               const int rows = M.stripInsertRowCount(*audioTid, false);
                               say("post inserts after remove: rows=" + juce::String(rows) + " | " + hooks_.describeTrackForDiagnostics(*audioTid));
                               if (rows != 5)
                               {
                                   failReason = "expected five rows after removing one";
                                   return false;
                               }
                               // Grow the Post inserts band: divider 3 is postInserts|sends — dragging it down
                               // takes the height Sends gained above (Sends keeps at least its four slots).
                               (void)M.dragDivider(3, 90);
                               const int visible = M.stripVisibleInsertRowCount(*audioTid, false);
                               say("after dragging the band taller: " + M.sectionHeightsText() + " visible rows=" + juce::String(visible));
                               if (visible < 4)
                               {
                                   failReason = "a taller band did not reveal more rows";
                                   return false;
                               }
                               return verifyAndCapture("six-inserts-tall-band", failReason);
                           },
                           400 });

    // 14. (1.1.17) Section heights persist; a low window scrolls instead of overwriting them.
    steps_.push_back(Step{ "mixer: section heights are stored in ui-layout.xml; a 500 px window keeps them and scrolls",
                           [this, M, say, boundsBeforeDividers, verifyAndCapture](juce::String& failReason) -> bool {
                               if (M.sectionHeightsText == nullptr)
                               {
                                   return true;
                               }
                               UiLayoutSettingsStore fresh(UiLayoutSettingsStore::defaultFile());
                               fresh.loadFromFile();
                               const auto pre = fresh.getMixerSectionHeightPx("preInserts");
                               const auto post = fresh.getMixerSectionHeightPx("postInserts");
                               say("stored heights: preInserts=" + (pre ? juce::String(*pre) : juce::String("(absent)")) + " postInserts="
                                   + (post ? juce::String(*post) : juce::String("(absent)")) + " | live: " + M.sectionHeightsText());
                               if (!pre || !post)
                               {
                                   failReason = "section heights were not persisted after the drags";
                                   return false;
                               }
                               M.setWindowBounds(boundsBeforeDividers->withHeight(500));
                               const juce::String low = M.sectionHeightsText();
                               say("500 px window: " + low);
                               const bool lowOk = verifyAndCapture("low-window-500", failReason);
                               M.setWindowBounds(*boundsBeforeDividers);
                               if (!lowOk)
                               {
                                   return false;
                               }
                               if (!low.contains("postInserts=" + juce::String(*post)))
                               {
                                   failReason = "a low window changed the stored section heights";
                                   return false;
                               }
                               return true;
                           },
                           600 });

    steps_.push_back(Step{ "mixer: default section heights again, hide the window at the end",
                           [M](juce::String&) -> bool {
                               if (M.resetSectionHeights != nullptr)
                               {
                                   M.resetSectionHeights();
                               }
                               if (M.isVisible())
                               {
                                   M.toggleLikeF3();
                               }
                               return true;
                           },
                           200 });
}

void StabilityScenarioRunner::appendLoadAndVerifySteps(const juce::File& project,
                                                       const juce::String& label)
{
    steps_.push_back(Step{
        label + ": load " + project.getFileName(),
        [this, project](juce::String&) -> bool {
            hooks_.loadProjectFromFile(project);
            return true;
        },
        kSettleAfterLoadMs });
    steps_.push_back(Step{
        label + ": verify loaded",
        [this](juce::String& failReason) -> bool {
            const int n = hooks_.getTrackCount();
            appendStabilityRunLine("  track count after load: " + juce::String(n));
            if (n <= 0)
            {
                failReason = "no tracks after load (load failed?)";
                return false;
            }
            return true;
        },
        kSettleDefaultMs });
}

void StabilityScenarioRunner::appendLoadLoopSteps(const juce::File& project, const int iterations)
{
    for (int i = 1; i <= iterations; ++i)
    {
        appendLoadAndVerifySteps(project,
                                 "load-loop " + juce::String(i) + "/" + juce::String(iterations));
    }
}

void StabilityScenarioRunner::appendLoadAlternateSteps(const juce::File& a,
                                                       const juce::File& b,
                                                       const int iterations)
{
    for (int i = 1; i <= iterations; ++i)
    {
        const juce::File& f = (i % 2 == 1) ? a : b;
        appendLoadAndVerifySteps(
            f, "load-alternate " + juce::String(i) + "/" + juce::String(iterations));
    }
}

size_t StabilityScenarioRunner::insertDeleteCycleSteps(const size_t insertAt,
                                                       const StabilityTrackInfo& track,
                                                       const bool withPlayback,
                                                       const bool withMidiEditor,
                                                       const juce::String& label)
{
    const juce::String trackDesc = track.kindName + " \"" + track.name + "\" id="
                                   + juce::String(static_cast<juce::int64>(track.id));
    // Baseline track count captured by the delete step, checked by undo/redo steps.
    auto baseline = std::make_shared<int>(-1);
    std::vector<Step> cycle;

    if (withPlayback)
    {
        cycle.push_back(Step{ label + ": start playback",
                              [this](juce::String&) -> bool {
                                  hooks_.setPlaybackActive(true);
                                  return true;
                              },
                              kSettleAfterDeleteOpMs });
    }
    if (withMidiEditor && track.isInstrument)
    {
        cycle.push_back(Step{
            label + ": open MIDI editor on " + trackDesc,
            [this, track](juce::String&) -> bool {
                const bool opened = hooks_.openMidiEditorOnFirstClip(track.id);
                appendStabilityRunLine(opened ? "  MIDI editor opened"
                                              : "  MIDI editor not opened (no clips) - continuing");
                return true; // No clips is not a failure.
            },
            kSettleDefaultMs });
    }

    cycle.push_back(Step{ label + ": delete " + trackDesc,
                          [this, track, baseline](juce::String& failReason) -> bool {
                              *baseline = hooks_.getTrackCount();
                              hooks_.requestDeleteTrack(track.id);
                              const int after = hooks_.getTrackCount();
                              if (after != *baseline - 1)
                              {
                                  failReason = "track count after delete: expected "
                                               + juce::String(*baseline - 1) + " got "
                                               + juce::String(after);
                                  return false;
                              }
                              return true;
                          },
                          kSettleAfterDeleteOpMs });
    cycle.push_back(Step{ label + ": undo delete of " + trackDesc,
                          [this, baseline](juce::String& failReason) -> bool {
                              hooks_.invokeUndo();
                              const int after = hooks_.getTrackCount();
                              if (after != *baseline)
                              {
                                  failReason = "track count after undo: expected "
                                               + juce::String(*baseline) + " got "
                                               + juce::String(after);
                                  return false;
                              }
                              return true;
                          },
                          kSettleAfterDeleteOpMs });
    cycle.push_back(Step{ label + ": redo delete of " + trackDesc,
                          [this, baseline](juce::String& failReason) -> bool {
                              hooks_.invokeRedo();
                              const int after = hooks_.getTrackCount();
                              if (after != *baseline - 1)
                              {
                                  failReason = "track count after redo: expected "
                                               + juce::String(*baseline - 1) + " got "
                                               + juce::String(after);
                                  return false;
                              }
                              return true;
                          },
                          kSettleAfterDeleteOpMs });
    cycle.push_back(Step{ label + ": undo (restore) " + trackDesc,
                          [this, baseline](juce::String& failReason) -> bool {
                              hooks_.invokeUndo();
                              const int after = hooks_.getTrackCount();
                              if (after != *baseline)
                              {
                                  failReason = "track count after restore undo: expected "
                                               + juce::String(*baseline) + " got "
                                               + juce::String(after);
                                  return false;
                              }
                              return true;
                          },
                          kSettleAfterDeleteOpMs });

    if (withMidiEditor && track.isInstrument)
    {
        cycle.push_back(Step{ label + ": close MIDI editor",
                              [this](juce::String&) -> bool {
                                  hooks_.closeMidiEditor();
                                  return true;
                              },
                              kSettleDefaultMs });
    }
    if (withPlayback)
    {
        cycle.push_back(Step{ label + ": stop playback",
                              [this](juce::String&) -> bool {
                                  hooks_.setPlaybackActive(false);
                                  return true;
                              },
                              kSettleDefaultMs });
    }

    steps_.insert(steps_.begin() + static_cast<std::ptrdiff_t>(insertAt),
                  cycle.begin(),
                  cycle.end());
    return cycle.size();
}

void StabilityScenarioRunner::appendDeleteLoopSteps(const juce::File& project, const int iterations)
{
    appendLoadAndVerifySteps(project, "delete-loop setup");

    for (int i = 0; i < iterations; ++i)
    {
        // Iteration variants: 0 = plain, odd = during playback, even >= 2 = with the MIDI editor
        // open on instrument tracks. Tracks are enumerated fresh at plan time because TrackIds
        // are only guaranteed stable across the immediately surrounding delete/undo cycle.
        const bool withPlayback = (i % 2) == 1;
        const bool withMidiEditor = i >= 2 && (i % 2) == 0;
        const juce::String label = "delete-loop iter " + juce::String(i + 1) + "/"
                                   + juce::String(iterations);
        steps_.push_back(Step{
            label + ": plan (enumerate tracks)",
            [this, label, withPlayback, withMidiEditor](juce::String& failReason) -> bool {
                const std::vector<StabilityTrackInfo> tracks = hooks_.listDeletableTracks();
                if (tracks.empty())
                {
                    failReason = "no deletable tracks in session";
                    return false;
                }
                appendStabilityRunLine("  " + label + ": " + juce::String((int)tracks.size())
                                       + " deletable tracks; playback="
                                       + (withPlayback ? "yes" : "no") + " midiEditor="
                                       + (withMidiEditor ? "yes" : "no"));
                size_t insertAt = nextStepIndex_;
                for (const StabilityTrackInfo& t : tracks)
                {
                    insertAt += insertDeleteCycleSteps(
                        insertAt, t, withPlayback, withMidiEditor, label);
                }
                return true;
            },
            kSettleDefaultMs });
    }
}

void StabilityScenarioRunner::appendOpenSaveCloseSteps(const juce::File& project)
{
    // Work on a sibling copy so the user's project file is never modified. A sibling (not a temp
    // dir) keeps project-relative audio paths ("Audio/...") resolving identically.
    steps_.push_back(Step{
        "open-save-close: copy project to sibling test file",
        [this, project](juce::String& failReason) -> bool {
            const juce::File copy = project.getSiblingFile(
                project.getFileNameWithoutExtension() + "-stabilitytest.dalproj");
            (void)copy.deleteFile();
            if (!project.copyFileTo(copy))
            {
                failReason = "could not copy project to " + copy.getFullPathName();
                return false;
            }
            openSaveCloseCopy_ = copy;
            appendStabilityRunLine("  test copy: " + copy.getFullPathName());
            return true;
        },
        kSettleDefaultMs });

    steps_.push_back(Step{ "open-save-close: load test copy",
                           [this](juce::String&) -> bool {
                               hooks_.loadProjectFromFile(openSaveCloseCopy_);
                               return true;
                           },
                           kSettleAfterLoadMs });
    steps_.push_back(Step{ "open-save-close: verify loaded",
                           [this](juce::String& failReason) -> bool {
                               const int n = hooks_.getTrackCount();
                               appendStabilityRunLine("  track count after load: "
                                                      + juce::String(n));
                               if (n <= 0)
                               {
                                   failReason = "no tracks after load";
                                   return false;
                               }
                               return true;
                           },
                           kSettleDefaultMs });
    steps_.push_back(Step{
        "open-save-close: small undoable edit (rename first track)",
        [this](juce::String& failReason) -> bool {
            const std::vector<StabilityTrackInfo> tracks = hooks_.listDeletableTracks();
            if (tracks.empty())
            {
                failReason = "no renameable tracks";
                return false;
            }
            const juce::String newName
                = "StabTest " + juce::Time::getCurrentTime().formatted("%H%M%S");
            if (!hooks_.renameTrackUndoable(tracks.front().id, newName))
            {
                failReason = "rename refused";
                return false;
            }
            appendStabilityRunLine("  renamed track id="
                                   + juce::String((juce::int64)tracks.front().id) + " to \""
                                   + newName + "\"");
            return true;
        },
        kSettleDefaultMs });
    steps_.push_back(Step{ "open-save-close: save (direct save to test copy)",
                           [this](juce::String&) -> bool {
                               hooks_.saveProject();
                               return true;
                           },
                           500 });
    steps_.push_back(Step{ "open-save-close: verify saved file",
                           [this](juce::String& failReason) -> bool {
                               if (!openSaveCloseCopy_.existsAsFile()
                                   || openSaveCloseCopy_.getSize() <= 0)
                               {
                                   failReason = "saved test copy missing or empty";
                                   return false;
                               }
                               return true;
                           },
                           kSettleDefaultMs });
    steps_.push_back(Step{ "open-save-close: reload test copy",
                           [this](juce::String&) -> bool {
                               hooks_.loadProjectFromFile(openSaveCloseCopy_);
                               return true;
                           },
                           kSettleAfterLoadMs });
    steps_.push_back(Step{ "open-save-close: verify reloaded",
                           [this](juce::String& failReason) -> bool {
                               const int n = hooks_.getTrackCount();
                               appendStabilityRunLine("  track count after reload: "
                                                      + juce::String(n));
                               if (n <= 0)
                               {
                                   failReason = "no tracks after reload";
                                   return false;
                               }
                               return true;
                           },
                           kSettleDefaultMs });
}

void StabilityScenarioRunner::appendAutosaveSteps(const juce::File& project,
                                                  const bool withRecovery)
{
    const juce::String label = withRecovery ? juce::String("recover-autosave")
                                            : juce::String("autosave");
    appendLoadAndVerifySteps(project, label + " setup");

    // Shared across steps: original-file fingerprint, the rename applied as the dirty edit, and
    // the autosave target captured at force time.
    auto originalHash = std::make_shared<juce::String>();
    auto renamedTo = std::make_shared<juce::String>();
    auto autosaveFile = std::make_shared<juce::File>();

    steps_.push_back(Step{
        label + ": record original project file fingerprint",
        [project, originalHash](juce::String& failReason) -> bool {
            juce::MemoryBlock data;
            if (!project.loadFileAsData(data))
            {
                failReason = "could not read project file";
                return false;
            }
            *originalHash = juce::MD5(data).toHexString();
            appendStabilityRunLine("  original md5=" + *originalHash + " size="
                                   + juce::String(project.getSize()));
            return true;
        },
        kSettleDefaultMs });

    steps_.push_back(Step{
        label + ": dirty edit (rename first track)",
        [this, renamedTo](juce::String& failReason) -> bool {
            const std::vector<StabilityTrackInfo> tracks = hooks_.listDeletableTracks();
            if (tracks.empty())
            {
                failReason = "no renameable tracks";
                return false;
            }
            *renamedTo = "AutosaveTest " + juce::Time::getCurrentTime().formatted("%H%M%S");
            if (!hooks_.renameTrackUndoable(tracks.front().id, *renamedTo))
            {
                failReason = "rename refused";
                return false;
            }
            if (hooks_.isProjectDirty && !hooks_.isProjectDirty())
            {
                failReason = "project not dirty after undoable rename";
                return false;
            }
            appendStabilityRunLine("  renamed first track to \"" + *renamedTo
                                   + "\"; project dirty=yes");
            return true;
        },
        kSettleDefaultMs });

    steps_.push_back(Step{
        label + ": force autosave",
        [this, autosaveFile](juce::String& failReason) -> bool {
            if (!hooks_.forceAutosaveNow)
            {
                failReason = "forceAutosaveNow hook missing";
                return false;
            }
            *autosaveFile = hooks_.getAutosaveFilePath ? hooks_.getAutosaveFilePath()
                                                       : juce::File{};
            return hooks_.forceAutosaveNow(failReason);
        },
        kSettleDefaultMs });

    steps_.push_back(Step{
        label + ": verify autosave file and pointer",
        [this, project, autosaveFile](juce::String& failReason) -> bool {
            if (!autosaveFile->existsAsFile() || autosaveFile->getSize() <= 0)
            {
                failReason = "autosave file missing or empty: "
                             + autosaveFile->getFullPathName();
                return false;
            }
            // Autosave polish: a saved project's autosave must use the project-specific name
            // "<stem>_autosave.dalproj" next to the project file.
            const juce::String expectedName
                = project.getFileNameWithoutExtension() + "_autosave.dalproj";
            if (!autosaveFile->getFileName().equalsIgnoreCase(expectedName))
            {
                failReason = "autosave file name is \"" + autosaveFile->getFileName()
                             + "\", expected project-specific \"" + expectedName + "\"";
                return false;
            }
            const juce::File pointer = hooks_.getAutosavePointerFilePath
                                           ? hooks_.getAutosavePointerFilePath()
                                           : juce::File{};
            if (!pointer.existsAsFile())
            {
                failReason = "autosave pointer file missing: " + pointer.getFullPathName();
                return false;
            }
            juce::StringArray lines;
            pointer.readLines(lines);
            const juce::String recorded = lines.size() > 0 ? lines[0].trim() : juce::String{};
            if (recorded != autosaveFile->getFullPathName())
            {
                failReason = "pointer file records \"" + recorded + "\" but autosave is \""
                             + autosaveFile->getFullPathName() + "\"";
                return false;
            }
            // Line 2 (owner) must attribute the autosave to the original project.
            const juce::String owner = lines.size() > 1 ? lines[1].trim() : juce::String{};
            if (owner != project.getFullPathName())
            {
                failReason = "pointer owner line is \"" + owner + "\", expected \""
                             + project.getFullPathName() + "\"";
                return false;
            }
            appendStabilityRunLine("  autosave ok: " + autosaveFile->getFullPathName() + " ("
                                   + juce::String(autosaveFile->getSize())
                                   + " bytes), pointer + owner match");
            return true;
        },
        kSettleDefaultMs });

    if (withRecovery)
    {
        steps_.push_back(Step{
            label + ": recover autosave in-process",
            [this](juce::String& failReason) -> bool {
                if (!hooks_.recoverAutosaveNow)
                {
                    failReason = "recoverAutosaveNow hook missing";
                    return false;
                }
                return hooks_.recoverAutosaveNow(failReason);
            },
            kSettleAfterLoadMs });

        steps_.push_back(Step{
            label + ": verify recovered state",
            [this, renamedTo](juce::String& failReason) -> bool {
                const int n = hooks_.getTrackCount();
                if (n <= 0)
                {
                    failReason = "no tracks after recovery";
                    return false;
                }
                const juce::String currentPath
                    = hooks_.getCurrentProjectPath ? hooks_.getCurrentProjectPath()
                                                   : juce::String{};
                if (currentPath.isNotEmpty())
                {
                    failReason = "recovered project still has a save path (\"" + currentPath
                                 + "\"); Save would not go through Save As";
                    return false;
                }
                if (hooks_.isProjectDirty && !hooks_.isProjectDirty())
                {
                    failReason = "recovered project is not marked dirty";
                    return false;
                }
                const std::vector<StabilityTrackInfo> tracks = hooks_.listDeletableTracks();
                if (tracks.empty() || tracks.front().name != *renamedTo)
                {
                    failReason = "dirty edit not present after recovery (first track is \""
                                 + (tracks.empty() ? juce::String("<none>") : tracks.front().name)
                                 + "\", expected \"" + *renamedTo + "\")";
                    return false;
                }
                appendStabilityRunLine("  recovered: tracks=" + juce::String(n)
                                       + " savePath=<cleared> dirty=yes edit preserved");
                return true;
            },
            kSettleDefaultMs });
    }

    steps_.push_back(Step{
        label + ": verify original project file unchanged",
        [project, originalHash](juce::String& failReason) -> bool {
            juce::MemoryBlock data;
            if (!project.loadFileAsData(data))
            {
                failReason = "could not re-read project file";
                return false;
            }
            const juce::String nowHash = juce::MD5(data).toHexString();
            if (nowHash != *originalHash)
            {
                failReason = "original project file changed during the scenario (md5 "
                             + *originalHash + " -> " + nowHash + ")";
                return false;
            }
            appendStabilityRunLine("  original project file unchanged (md5 verified)");
            return true;
        },
        kSettleDefaultMs });

    steps_.push_back(Step{
        label + ": cleanup autosave artifacts",
        [this, autosaveFile](juce::String&) -> bool {
            const bool fileDeleted = autosaveFile->existsAsFile() && autosaveFile->deleteFile();
            const juce::File pointer = hooks_.getAutosavePointerFilePath
                                           ? hooks_.getAutosavePointerFilePath()
                                           : juce::File{};
            const bool pointerDeleted = pointer.existsAsFile() && pointer.deleteFile();
            appendStabilityRunLine(juce::String("  cleanup: autosave ")
                                   + (fileDeleted ? "deleted" : "absent/kept") + ", pointer "
                                   + (pointerDeleted ? "deleted" : "absent/kept"));
            return true;
        },
        kSettleDefaultMs });

    if (withRecovery)
    {
        // Backward compatibility: autosaves written before the project-specific naming were all
        // "<projectFolder>\autosave.dalproj", referenced by the pointer file. Simulate one and
        // verify recovery still accepts it via pointer metadata (never filename guessing).
        auto legacyFile = std::make_shared<juce::File>();
        steps_.push_back(Step{
            label + ": create legacy autosave + pointer (compat)",
            [this, project, legacyFile](juce::String& failReason) -> bool {
                *legacyFile = project.getSiblingFile("autosave.dalproj");
                if (!project.copyFileTo(*legacyFile))
                {
                    failReason = "could not create legacy autosave copy: "
                                 + legacyFile->getFullPathName();
                    return false;
                }
                const juce::File pointer = hooks_.getAutosavePointerFilePath
                                               ? hooks_.getAutosavePointerFilePath()
                                               : juce::File{};
                if (!pointer.replaceWithText(legacyFile->getFullPathName() + "\n"
                                             + project.getFullPathName() + "\n"))
                {
                    failReason = "could not write legacy pointer file";
                    return false;
                }
                appendStabilityRunLine("  legacy autosave staged: "
                                       + legacyFile->getFullPathName());
                return true;
            },
            kSettleDefaultMs });

        steps_.push_back(Step{
            label + ": recover legacy autosave (compat)",
            [this](juce::String& failReason) -> bool {
                if (!hooks_.recoverAutosaveNow)
                {
                    failReason = "recoverAutosaveNow hook missing";
                    return false;
                }
                return hooks_.recoverAutosaveNow(failReason);
            },
            kSettleAfterLoadMs });

        steps_.push_back(Step{
            label + ": verify legacy recovery + cleanup (compat)",
            [this, legacyFile](juce::String& failReason) -> bool {
                const juce::String currentPath
                    = hooks_.getCurrentProjectPath ? hooks_.getCurrentProjectPath()
                                                   : juce::String{};
                if (currentPath.isNotEmpty())
                {
                    failReason = "legacy recovery left a save path (\"" + currentPath + "\")";
                    return false;
                }
                if (hooks_.getTrackCount() <= 0)
                {
                    failReason = "no tracks after legacy recovery";
                    return false;
                }
                const bool fileDeleted = legacyFile->existsAsFile() && legacyFile->deleteFile();
                const juce::File pointer = hooks_.getAutosavePointerFilePath
                                               ? hooks_.getAutosavePointerFilePath()
                                               : juce::File{};
                const bool pointerDeleted = pointer.existsAsFile() && pointer.deleteFile();
                appendStabilityRunLine(juce::String("  legacy recovery ok; cleanup: autosave ")
                                       + (fileDeleted ? "deleted" : "absent/kept") + ", pointer "
                                       + (pointerDeleted ? "deleted" : "absent/kept"));
                return true;
            },
            kSettleDefaultMs });
    }
}

namespace
{
    /// Names of every file directly inside `dir`, sorted, for exact result-set assertions.
    [[nodiscard]] juce::StringArray listFileNames(const juce::File& dir)
    {
        juce::StringArray names;
        for (const auto& entry : juce::RangedDirectoryIterator(dir, false, "*", juce::File::findFiles))
        {
            names.add(entry.getFile().getFileName());
        }
        names.sort(true);
        return names;
    }

    /// DAL's own working files carry these tags; anything else in a folder is never ours to judge.
    [[nodiscard]] juce::StringArray listDalWorkingFileLeftovers(const juce::File& dir)
    {
        juce::StringArray leftovers;
        for (const auto& name : listFileNames(dir))
        {
            if (name.contains(".__dal_") || name.startsWith("DAL-mixdown-"))
            {
                leftovers.add(name);
            }
        }
        return leftovers;
    }

    /// RMS over all channels of a WAV via the production reader stack (juce_audio_formats).
    [[nodiscard]] bool measureWavRms(const juce::File& wav, double& rmsOut, juce::String& err)
    {
        juce::WavAudioFormat format;
        std::unique_ptr<juce::AudioFormatReader> reader(format.createReaderFor(wav.createInputStream().release(), true));
        if (reader == nullptr || reader->lengthInSamples <= 0)
        {
            err = "could not read WAV: " + wav.getFullPathName();
            return false;
        }
        const int numChannels = static_cast<int>(reader->numChannels);
        const juce::int64 total = reader->lengthInSamples;
        juce::AudioBuffer<float> block(numChannels, 8192);
        double sumSquares = 0.0;
        juce::int64 pos = 0;
        while (pos < total)
        {
            const int n = static_cast<int>(juce::jmin<juce::int64>(block.getNumSamples(), total - pos));
            if (!reader->read(&block, 0, n, pos, true, true))
            {
                err = "WAV read failed at " + juce::String(pos);
                return false;
            }
            for (int ch = 0; ch < numChannels; ++ch)
            {
                const float* const d = block.getReadPointer(ch);
                for (int i = 0; i < n; ++i)
                {
                    sumSquares += static_cast<double>(d[i]) * static_cast<double>(d[i]);
                }
            }
            pos += n;
        }
        rmsOut = std::sqrt(sumSquares / static_cast<double>(total * juce::jmax(1, numChannels)));
        return true;
    }
} // namespace

void StabilityScenarioRunner::appendMixdownSteps(const juce::File& project, const bool mp3)
{
    appendLoadAndVerifySteps(project, "mixdown setup");

    // A dedicated, initially empty folder: the export's visible result must be exactly one file,
    // so the whole folder content is asserted, not just the expected path.
    scenarioOutputDir_ = juce::File::getSpecialLocation(juce::File::tempDirectory)
                             .getChildFile("dal-stability-mixdown-out");
    const juce::File out
        = scenarioOutputDir_.getChildFile(mp3 ? "dal-stability-mixdown.mp3" : "dal-stability-mixdown.wav");
    const juce::File systemTemp = juce::File::getSpecialLocation(juce::File::tempDirectory);

    steps_.push_back(Step{
        juce::String("mixdown: export ") + (mp3 ? "mp3" : "wav") + " to an empty folder",
        [this, out, mp3, systemTemp](juce::String& failReason) -> bool {
            (void)scenarioOutputDir_.deleteRecursively();
            if (!scenarioOutputDir_.createDirectory())
            {
                failReason = "could not create " + scenarioOutputDir_.getFullPathName();
                return false;
            }
            const juce::StringArray tempLeftoversBefore = listDalWorkingFileLeftovers(systemTemp);
            const juce::Result r = hooks_.runMixdownBlocking(out, mp3);
            if (!r.wasOk())
            {
                failReason = "exporter failed: " + r.getErrorMessage();
                return false;
            }
            if (!out.existsAsFile() || out.getSize() <= 0)
            {
                failReason = "output missing or empty: " + out.getFullPathName();
                return false;
            }
            // Exactly the chosen file, nothing else: no intermediate WAV beside an MP3, no MP3
            // beside a WAV, no render temp.
            const juce::StringArray produced = listFileNames(scenarioOutputDir_);
            if (produced.size() != 1 || produced[0] != out.getFileName())
            {
                failReason = "export folder does not contain exactly the result file: "
                             + produced.joinIntoString(", ");
                return false;
            }
            // The MP3 path's working WAV lives in the system temp folder and must be gone again.
            const juce::StringArray tempLeftoversAfter = listDalWorkingFileLeftovers(systemTemp);
            if (tempLeftoversAfter.size() > tempLeftoversBefore.size())
            {
                failReason = "working files left in the system temp folder: "
                             + tempLeftoversAfter.joinIntoString(", ");
                return false;
            }
            appendStabilityRunLine("  mixdown output ok: " + out.getFullPathName() + " ("
                                   + juce::String(out.getSize()) + " bytes); folder contains only "
                                   + produced[0] + "; no DAL working files left in temp");
            return true;
        },
        kSettleDefaultMs });

    // Overwrite regression test (tester report: "export did not replace the existing file").
    // The destination is replaced with a small known sentinel, then exported over; if the second
    // export silently skips/cancels/fails to replace, the sentinel is still there and this fails.
    static const juce::String kSentinelText = "DAL_MIXDOWN_OVERWRITE_SENTINEL";
    steps_.push_back(Step{
        juce::String("mixdown: overwrite ") + (mp3 ? "mp3" : "wav") + " (sentinel replace)",
        [this, out, mp3](juce::String& failReason) -> bool {
            if (!out.replaceWithText(kSentinelText))
            {
                failReason = "could not write sentinel to " + out.getFullPathName();
                return false;
            }
            const juce::int64 sentinelSize = out.getSize();
            const juce::Result r = hooks_.runMixdownBlocking(out, mp3);
            if (!r.wasOk())
            {
                failReason = "overwrite export failed: " + r.getErrorMessage();
                return false;
            }
            if (!out.existsAsFile() || out.getSize() <= sentinelSize)
            {
                failReason = "output missing or not larger than sentinel after overwrite: "
                             + out.getFullPathName();
                return false;
            }
            juce::FileInputStream probe(out);
            juce::MemoryBlock head;
            probe.readIntoMemoryBlock(head, 64);
            const juce::String headText = juce::String::fromUTF8(
                static_cast<const char*>(head.getData()),
                static_cast<int>(head.getSize()));
            if (headText.contains(kSentinelText))
            {
                failReason = "destination still contains the sentinel; file was not replaced: "
                             + out.getFullPathName();
                return false;
            }
            appendStabilityRunLine("  overwrite ok: sentinel (" + juce::String(sentinelSize)
                                   + " bytes) replaced by " + juce::String(out.getSize())
                                   + " bytes of audio");
            (void)out.deleteFile();
            return true;
        },
        kSettleDefaultMs });

    // Controlled failure: a destination name Windows cannot create makes the render temp's
    // FileOutputStream fail *after* the offline gate was entered. The exporter must report the
    // error, leave the folder untouched, and release the gate so the app keeps working.
    steps_.push_back(Step{
        juce::String("mixdown: controlled write failure (") + (mp3 ? "mp3" : "wav") + ") reports an error and leaves no file",
        [this, mp3](juce::String& failReason) -> bool {
            const juce::StringArray before = listFileNames(scenarioOutputDir_);
            const juce::File bad = scenarioOutputDir_.getChildFile(mp3 ? "bad<>name.mp3" : "bad<>name.wav");
            const juce::Result r = hooks_.runMixdownBlocking(bad, mp3);
            if (r.wasOk())
            {
                failReason = "export to an invalid file name unexpectedly succeeded";
                return false;
            }
            appendStabilityRunLine("  controlled failure reported: "
                                   + r.getErrorMessage().replaceCharacter('\n', ' '));
            const juce::StringArray after = listFileNames(scenarioOutputDir_);
            if (after != before)
            {
                failReason = "failed export left files behind: " + after.joinIntoString(", ");
                return false;
            }
            return true;
        },
        kSettleDefaultMs });

    // The app must be fully usable afterwards: playback runs, the device output is audible and
    // the callback keeps advancing (gate released, no wedged state).
    if (hooks_.audioHealthProbeBegin != nullptr && hooks_.audioHealthProbeVerify != nullptr
        && hooks_.setPlaybackActive != nullptr)
    {
        steps_.push_back(Step{ "mixdown: playback after exports - start (probe armed)",
                               [this](juce::String&) -> bool {
                                   hooks_.setPlaybackActive(true);
                                   hooks_.audioHealthProbeBegin();
                                   return true;
                               },
                               1200 });
        steps_.push_back(Step{ "mixdown: playback after exports - verify audio health",
                               [this](juce::String& failReason) -> bool {
                                   const bool ok = hooks_.audioHealthProbeVerify("after-exports", failReason);
                                   hooks_.setPlaybackActive(false);
                                   return ok;
                               },
                               kSettleDefaultMs });
    }
}

// -----------------------------------------------------------------------------
// Pre-gain through the real engine (device callback + offline mixdown)
// -----------------------------------------------------------------------------
// The user's report was "−24 dB pre-gain changes nothing". This scenario measures the two
// production signal paths a listener can hear or export, on a copy of an audio-only fixture:
//   realtime  - device-output peak hold while the transport plays a steady tone, 0 dB vs −24 dB,
//               with the −24 dB value set WHILE PLAYING (live update + ramp must both work);
//   offline   - RMS of the mixdown WAV at 0 dB vs −24 dB.
// Both ratios must equal 10^(−24/20) ≈ 0.06310 (±3 % realtime, ±1 % offline): "applied once".
// A ratio near 1.0 would reproduce the report; a ratio near 0.004 would mean "applied twice".
void StabilityScenarioRunner::appendPreGainSteps(const juce::File& project)
{
    if (hooks_.setTrackPreGainDb == nullptr || hooks_.getTrackPreGainDb == nullptr
        || hooks_.readOutputPeakHoldAndReset == nullptr || hooks_.runMixdownBlocking == nullptr
        || hooks_.setPlaybackActive == nullptr || hooks_.saveProject == nullptr)
    {
        steps_.push_back(Step{ "pregain: hooks missing",
                               [](juce::String& failReason) -> bool {
                                   failReason = "pregain hooks not installed";
                                   return false;
                               },
                               0 });
        return;
    }

    constexpr float kTestDb = -24.0f;
    const double expectedRatio = std::pow(10.0, kTestDb / 20.0); // 0.0630957...

    // Work on a sibling copy: the save/reload step writes the project.
    steps_.push_back(Step{
        "pregain: copy project to sibling test file",
        [this, project](juce::String& failReason) -> bool {
            const juce::File copy = project.getSiblingFile(
                project.getFileNameWithoutExtension() + "-pregaintest.dalproj");
            (void)copy.deleteFile();
            if (!project.copyFileTo(copy))
            {
                failReason = "could not copy project to " + copy.getFullPathName();
                return false;
            }
            openSaveCloseCopy_ = copy;
            appendStabilityRunLine("  test copy: " + copy.getFullPathName());
            return true;
        },
        kSettleDefaultMs });
    steps_.push_back(Step{ "pregain: load test copy",
                           [this](juce::String&) -> bool {
                               hooks_.loadProjectFromFile(openSaveCloseCopy_);
                               return true;
                           },
                           kSettleAfterLoadMs });

    const auto audioTrackIds = [this]() {
        std::vector<TrackId> ids;
        for (const StabilityTrackInfo& t : hooks_.listDeletableTracks())
        {
            if (t.kindName == "audio")
            {
                ids.push_back(t.id);
            }
        }
        return ids;
    };
    const auto setAllAudioPreGain = [this, audioTrackIds](const float dB, juce::String& failReason) {
        const std::vector<TrackId> ids = audioTrackIds();
        if (ids.empty())
        {
            failReason = "fixture has no audio tracks";
            return false;
        }
        for (const TrackId id : ids)
        {
            (void)hooks_.setTrackPreGainDb(id, dB); // false = already at this value (fine)
            const float stored = hooks_.getTrackPreGainDb(id);
            if (std::fabs(stored - dB) > 1.0e-4f)
            {
                failReason = "track " + juce::String((juce::int64)id) + " stored pre-gain "
                             + juce::String(stored, 3) + " dB, expected " + juce::String(dB, 3);
                return false;
            }
        }
        return true;
    };

    // ---- Realtime: play, then measure the peak hold over a 1 s window at 0 dB. ----
    steps_.push_back(Step{ "pregain: realtime baseline 0 dB - start playback",
                           [this, setAllAudioPreGain](juce::String& failReason) -> bool {
                               if (!setAllAudioPreGain(0.0f, failReason))
                               {
                                   return false;
                               }
                               hooks_.setPlaybackActive(true);
                               return true;
                           },
                           600 }); // let playback settle before the measurement window opens
    steps_.push_back(Step{ "pregain: realtime baseline 0 dB - arm peak window",
                           [this](juce::String&) -> bool {
                               (void)hooks_.readOutputPeakHoldAndReset();
                               return true;
                           },
                           1000 });
    steps_.push_back(Step{ "pregain: realtime baseline 0 dB - read peak",
                           [this](juce::String& failReason) -> bool {
                               preGainPeakAt0dB_ = hooks_.readOutputPeakHoldAndReset();
                               appendStabilityRunLine("  realtime peak at 0 dB: "
                                                      + juce::String(preGainPeakAt0dB_, 5));
                               if (!(preGainPeakAt0dB_ > 1.0e-3f))
                               {
                                   failReason = "no audible output at 0 dB (fixture silent?)";
                                   return false;
                               }
                               return true;
                           },
                           kSettleDefaultMs });
    // The −24 dB value is set WHILE PLAYING: this is the live-update path (Inspector edit during
    // playback). The ramp completes within one block; the 400 ms settle keeps its first samples
    // out of the measurement window.
    steps_.push_back(Step{ "pregain: realtime -24 dB - set while playing",
                           [setAllAudioPreGain](juce::String& failReason) -> bool {
                               return setAllAudioPreGain(-24.0f, failReason);
                           },
                           400 });
    steps_.push_back(Step{ "pregain: realtime -24 dB - arm peak window",
                           [this](juce::String&) -> bool {
                               (void)hooks_.readOutputPeakHoldAndReset();
                               return true;
                           },
                           1000 });
    steps_.push_back(Step{
        "pregain: realtime -24 dB - read peak and compare",
        [this, expectedRatio](juce::String& failReason) -> bool {
            preGainPeakAtMinus24dB_ = hooks_.readOutputPeakHoldAndReset();
            hooks_.setPlaybackActive(false);
            const double ratio = preGainPeakAt0dB_ > 0.0f
                                     ? static_cast<double>(preGainPeakAtMinus24dB_) / static_cast<double>(preGainPeakAt0dB_)
                                     : 0.0;
            appendStabilityRunLine("  realtime peak at -24 dB: " + juce::String(preGainPeakAtMinus24dB_, 5)
                                   + " ratio=" + juce::String(ratio, 5) + " expected="
                                   + juce::String(expectedRatio, 5));
            if (std::fabs(ratio - expectedRatio) > expectedRatio * 0.03)
            {
                failReason = "realtime pre-gain ratio " + juce::String(ratio, 5) + " deviates from "
                             + juce::String(expectedRatio, 5) + " by more than 3%";
                return false;
            }
            return true;
        },
        kSettleDefaultMs });

    // ---- Offline: mixdown WAV RMS at 0 dB vs −24 dB (same fixture, transport stopped). ----
    scenarioOutputDir_ = juce::File::getSpecialLocation(juce::File::tempDirectory)
                             .getChildFile("dal-stability-pregain-out");
    const juce::File out0 = scenarioOutputDir_.getChildFile("pregain-0dB.wav");
    const juce::File out24 = scenarioOutputDir_.getChildFile("pregain-minus24dB.wav");
    steps_.push_back(Step{ "pregain: offline export at 0 dB",
                           [this, setAllAudioPreGain, out0](juce::String& failReason) -> bool {
                               (void)scenarioOutputDir_.deleteRecursively();
                               if (!scenarioOutputDir_.createDirectory())
                               {
                                   failReason = "could not create " + scenarioOutputDir_.getFullPathName();
                                   return false;
                               }
                               if (!setAllAudioPreGain(0.0f, failReason))
                               {
                                   return false;
                               }
                               const juce::Result r = hooks_.runMixdownBlocking(out0, false);
                               if (!r.wasOk())
                               {
                                   failReason = "export at 0 dB failed: " + r.getErrorMessage();
                                   return false;
                               }
                               return measureWavRms(out0, preGainRmsAt0dB_, failReason);
                           },
                           kSettleDefaultMs });
    steps_.push_back(Step{
        "pregain: offline export at -24 dB and compare RMS",
        [this, setAllAudioPreGain, out24, expectedRatio](juce::String& failReason) -> bool {
            if (!setAllAudioPreGain(-24.0f, failReason))
            {
                return false;
            }
            const juce::Result r = hooks_.runMixdownBlocking(out24, false);
            if (!r.wasOk())
            {
                failReason = "export at -24 dB failed: " + r.getErrorMessage();
                return false;
            }
            if (!measureWavRms(out24, preGainRmsAtMinus24dB_, failReason))
            {
                return false;
            }
            const double ratio = preGainRmsAt0dB_ > 0.0 ? preGainRmsAtMinus24dB_ / preGainRmsAt0dB_ : 0.0;
            appendStabilityRunLine("  offline RMS 0 dB=" + juce::String(preGainRmsAt0dB_, 6)
                                   + " -24 dB=" + juce::String(preGainRmsAtMinus24dB_, 6)
                                   + " ratio=" + juce::String(ratio, 5) + " expected="
                                   + juce::String(expectedRatio, 5));
            if (!(preGainRmsAt0dB_ > 1.0e-3))
            {
                failReason = "offline export at 0 dB is silent (fixture/loop range?)";
                return false;
            }
            if (std::fabs(ratio - expectedRatio) > expectedRatio * 0.01)
            {
                failReason = "offline pre-gain ratio " + juce::String(ratio, 5) + " deviates from "
                             + juce::String(expectedRatio, 5) + " by more than 1%";
                return false;
            }
            return true;
        },
        kSettleDefaultMs });

    // ---- Persistence: save the copy at −24 dB, reload, read the value back. ----
    steps_.push_back(Step{ "pregain: save test copy at -24 dB",
                           [this](juce::String&) -> bool {
                               hooks_.saveProject();
                               return true;
                           },
                           kSettleDefaultMs });
    steps_.push_back(Step{ "pregain: reload test copy",
                           [this](juce::String&) -> bool {
                               hooks_.loadProjectFromFile(openSaveCloseCopy_);
                               return true;
                           },
                           kSettleAfterLoadMs });
    steps_.push_back(Step{ "pregain: verify -24 dB survived save/reload",
                           [this, audioTrackIds](juce::String& failReason) -> bool {
                               const std::vector<TrackId> ids = audioTrackIds();
                               if (ids.empty())
                               {
                                   failReason = "no audio tracks after reload";
                                   return false;
                               }
                               for (const TrackId id : ids)
                               {
                                   const float stored = hooks_.getTrackPreGainDb(id);
                                   if (std::fabs(stored + 24.0f) > 1.0e-4f)
                                   {
                                       failReason = "track " + juce::String((juce::int64)id)
                                                    + " reloaded with pre-gain " + juce::String(stored, 3);
                                       return false;
                                   }
                               }
                               appendStabilityRunLine("  pre-gain -24 dB survived save/reload on "
                                                      + juce::String((int)ids.size()) + " track(s)");
                               return true;
                           },
                           kSettleDefaultMs });
}

// -----------------------------------------------------------------------------
// Pre-gain through the REAL Inspector on a real project (user-flow reproduction)
// -----------------------------------------------------------------------------
// Report under test: "pre-gain 0 -> -24 dB on the AmpliTube audio track changed nothing while the
// recorded guitar played". Every step below goes through the production UI / session objects of
// the running app (no direct session setter for the edit):
//   activate the track like a header click -> type "-24" + Return into the Inspector field ->
//   compare displayed text, committed snapshot value and the track id it landed on ->
//   measure, while the transport plays, the level entering the first insert and leaving the last
//   one (PluginInsertHost level tap) - first in the project's own state, then with the track
//   unmuted - so "which audio path is actually heard" is answered by numbers, not assumptions.
void StabilityScenarioRunner::appendPreGainInspectorSteps(const juce::File& project)
{
    if (hooks_.activateTrackLikeHeaderClick == nullptr || hooks_.inspectorTypePreGainAndReturn == nullptr
        || hooks_.inspectorPreGainFieldText == nullptr || hooks_.describeTrackForDiagnostics == nullptr
        || hooks_.findAudioTrackWithInsertNamed == nullptr || hooks_.setInsertLevelTapTrack == nullptr
        || hooks_.readAndResetInsertLevelTap == nullptr || hooks_.getTrackPreGainDb == nullptr
        || hooks_.setTrackMutedLikeHeader == nullptr || hooks_.seekTransportTo == nullptr
        || hooks_.readOutputPeakHoldAndReset == nullptr || hooks_.setPlaybackActive == nullptr)
    {
        steps_.push_back(Step{ "pregain-inspector: hooks missing",
                               [](juce::String& failReason) -> bool {
                                   failReason = "pregain-inspector hooks not installed";
                                   return false;
                               },
                               0 });
        return;
    }

    appendLoadAndVerifySteps(project, "pregain-inspector");

    steps_.push_back(Step{ "pregain-inspector: describe every track as the engine sees it",
                           [this](juce::String& failReason) -> bool {
                               for (const StabilityTrackInfo& t : hooks_.listDeletableTracks())
                               {
                                   appendStabilityRunLine("  " + hooks_.describeTrackForDiagnostics(t.id));
                               }
                               inspectorTargetTrackId_ = hooks_.findAudioTrackWithInsertNamed("AmpliTube");
                               if (inspectorTargetTrackId_ == kInvalidTrackId)
                               {
                                   failReason = "no audio track with an AmpliTube insert in this project";
                                   return false;
                               }
                               appendStabilityRunLine("  target (AmpliTube) track id="
                                                      + juce::String((juce::int64)inspectorTargetTrackId_));
                               hooks_.setInsertLevelTapTrack(inspectorTargetTrackId_);
                               return true;
                           },
                           kSettleDefaultMs });

    steps_.push_back(Step{
        "pregain-inspector: activate the AmpliTube track like a header click",
        [this](juce::String& failReason) -> bool {
            hooks_.activateTrackLikeHeaderClick(inspectorTargetTrackId_);
            const bool visible = hooks_.inspectorPreGainFieldVisible != nullptr && hooks_.inspectorPreGainFieldVisible();
            appendStabilityRunLine("  Inspector pre-gain field visible=" + juce::String(visible ? "yes" : "no")
                                   + " shows=\"" + hooks_.inspectorPreGainFieldText() + "\" stored="
                                   + juce::String(hooks_.getTrackPreGainDb(inspectorTargetTrackId_), 2) + " dB");
            if (!visible)
            {
                failReason = "Inspector does not show the pre-gain field for the active audio track";
                return false;
            }
            return true;
        },
        kSettleDefaultMs });

    // ---- Phase A: the project's own state (mute as saved), transport playing from the take. ----
    steps_.push_back(Step{ "pregain-inspector: phase A (project state) - seek 0 and play",
                           [this](juce::String&) -> bool {
                               hooks_.seekTransportTo(0);
                               hooks_.setPlaybackActive(true);
                               return true;
                           },
                           600 });
    const auto armTap = [this]() {
        (void)hooks_.readOutputPeakHoldAndReset();
        float b = 0.0f, a = 0.0f;
        double rb = 0.0, ra = 0.0;
        std::uint32_t pre = 0, post = 0;
        hooks_.readAndResetInsertLevelTap(b, a, rb, ra, pre, post);
    };
    steps_.push_back(Step{ "pregain-inspector: phase A - arm measurement window",
                           [armTap](juce::String&) -> bool {
                               armTap();
                               return true;
                           },
                           1200 });
    steps_.push_back(Step{
        "pregain-inspector: phase A - what is audible at the project's own settings",
        [this](juce::String&) -> bool {
            float before = 0.0f, after = 0.0f;
            double rmsBefore = 0.0, rmsAfter = 0.0;
            std::uint32_t preBlocks = 0, postBlocks = 0;
            hooks_.readAndResetInsertLevelTap(before, after, rmsBefore, rmsAfter, preBlocks, postBlocks);
            const float devicePeak = hooks_.readOutputPeakHoldAndReset();
            appendStabilityRunLine("  phase A: device output peak=" + juce::String(devicePeak, 5)
                                   + " | AmpliTube track chain processed blocks pre=" + juce::String((int)preBlocks)
                                   + " post=" + juce::String((int)postBlocks) + " level before first insert="
                                   + juce::String(before, 5) + " after last insert=" + juce::String(after, 5));
            appendStabilityRunLine("  phase A: " + hooks_.describeTrackForDiagnostics(inspectorTargetTrackId_));
            if (preBlocks == 0)
            {
                appendStabilityRunLine("  phase A: the AmpliTube track's insert chain is NOT processed - the "
                                       "track contributes nothing to what is heard (device output above comes "
                                       "from other rows)");
            }
            return true;
        },
        kSettleDefaultMs });

    // The user's edit, through the real Inspector, WHILE playing.
    steps_.push_back(Step{ "pregain-inspector: type \"-24\" + Return into the Inspector field (playing)",
                           [this](juce::String&) -> bool {
                               hooks_.inspectorTypePreGainAndReturn("-24");
                               return true;
                           },
                           400 });
    steps_.push_back(Step{
        "pregain-inspector: displayed vs committed value and track id",
        [this](juce::String& failReason) -> bool {
            const juce::String shown = hooks_.inspectorPreGainFieldText();
            const float stored = hooks_.getTrackPreGainDb(inspectorTargetTrackId_);
            juce::String others;
            for (const StabilityTrackInfo& t : hooks_.listDeletableTracks())
            {
                if (t.kindName == "audio" && t.id != inspectorTargetTrackId_)
                {
                    others << " track" << juce::String((juce::int64)t.id) << "=" << juce::String(hooks_.getTrackPreGainDb(t.id), 2);
                }
            }
            appendStabilityRunLine("  Inspector shows \"" + shown + "\" | committed on track "
                                   + juce::String((juce::int64)inspectorTargetTrackId_) + " = " + juce::String(stored, 2)
                                   + " dB | other audio tracks:" + others);
            if (shown != "-24.0")
            {
                failReason = "Inspector field shows \"" + shown + "\" instead of -24.0";
                return false;
            }
            if (std::fabs(stored + 24.0f) > 1.0e-4f)
            {
                failReason = "committed value on the AmpliTube track is " + juce::String(stored, 2) + " dB, not -24";
                return false;
            }
            return true;
        },
        kSettleDefaultMs });
    steps_.push_back(Step{ "pregain-inspector: phase A at -24 dB - arm measurement window",
                           [armTap](juce::String&) -> bool {
                               armTap();
                               return true;
                           },
                           1200 });
    steps_.push_back(Step{
        "pregain-inspector: phase A at -24 dB - what changed audibly",
        [this](juce::String&) -> bool {
            float before = 0.0f, after = 0.0f;
            double rmsBefore = 0.0, rmsAfter = 0.0;
            std::uint32_t preBlocks = 0, postBlocks = 0;
            hooks_.readAndResetInsertLevelTap(before, after, rmsBefore, rmsAfter, preBlocks, postBlocks);
            const float devicePeak = hooks_.readOutputPeakHoldAndReset();
            appendStabilityRunLine("  phase A (-24 dB): device output peak=" + juce::String(devicePeak, 5)
                                   + " | chain blocks pre=" + juce::String((int)preBlocks) + " post=" + juce::String((int)postBlocks)
                                   + " level before first insert=" + juce::String(before, 5) + " after last insert="
                                   + juce::String(after, 5));
            hooks_.setPlaybackActive(false);
            return true;
        },
        kSettleDefaultMs });

    // ---- Phase B: the same flow with the AmpliTube track audible (unmuted like the header). ----
    // Both measurement windows play the SAME passage of the take (seek 0, fixed lead-in, fixed
    // window) and compare RMS, so the ratio reflects the gain change and not the music.
    steps_.push_back(Step{ "pregain-inspector: phase B - unmute the AmpliTube track like the header",
                           [this](juce::String&) -> bool {
                               hooks_.setTrackMutedLikeHeader(inspectorTargetTrackId_, false);
                               hooks_.activateTrackLikeHeaderClick(inspectorTargetTrackId_);
                               return true;
                           },
                           kSettleDefaultMs });
    steps_.push_back(Step{ "pregain-inspector: phase B - type \"0\" + Return into the Inspector field",
                           [this](juce::String&) -> bool {
                               hooks_.inspectorTypePreGainAndReturn("0");
                               return true;
                           },
                           400 });
    steps_.push_back(Step{ "pregain-inspector: phase B at 0 dB - seek 0 and play (lead-in)",
                           [this](juce::String& failReason) -> bool {
                               if (std::fabs(hooks_.getTrackPreGainDb(inspectorTargetTrackId_)) > 1.0e-4f)
                               {
                                   failReason = "typing 0 did not commit 0 dB";
                                   return false;
                               }
                               hooks_.seekTransportTo(0);
                               hooks_.setPlaybackActive(true);
                               return true;
                           },
                           600 });
    steps_.push_back(Step{ "pregain-inspector: phase B at 0 dB - arm measurement window",
                           [armTap](juce::String&) -> bool {
                               armTap();
                               return true;
                           },
                           1500 });
    steps_.push_back(Step{
        "pregain-inspector: phase B at 0 dB - levels",
        [this](juce::String& failReason) -> bool {
            std::uint32_t preBlocks = 0, postBlocks = 0;
            hooks_.readAndResetInsertLevelTap(inspectorPeakBefore0dB_, inspectorPeakAfter0dB_, inspectorRmsBefore0dB_,
                                              inspectorRmsAfter0dB_, preBlocks, postBlocks);
            const float devicePeak = hooks_.readOutputPeakHoldAndReset();
            hooks_.setPlaybackActive(false);
            appendStabilityRunLine("  phase B (0 dB): device output peak=" + juce::String(devicePeak, 5)
                                   + " | chain blocks pre=" + juce::String((int)preBlocks) + " post=" + juce::String((int)postBlocks)
                                   + " | before first insert peak=" + juce::String(inspectorPeakBefore0dB_, 5)
                                   + " rms=" + juce::String(inspectorRmsBefore0dB_, 6) + " | after AmpliTube peak="
                                   + juce::String(inspectorPeakAfter0dB_, 5) + " rms=" + juce::String(inspectorRmsAfter0dB_, 6));
            appendStabilityRunLine("  phase B: " + hooks_.describeTrackForDiagnostics(inspectorTargetTrackId_));
            if (preBlocks == 0 || !(inspectorRmsBefore0dB_ > 1.0e-5))
            {
                failReason = "unmuted AmpliTube track still renders nothing into its insert chain";
                return false;
            }
            return true;
        },
        kSettleDefaultMs });
    // The user's edit while playing (live update), then the same passage is re-measured from 0.
    steps_.push_back(Step{ "pregain-inspector: phase B - play, then type \"-24\" + Return while playing",
                           [this](juce::String&) -> bool {
                               hooks_.setPlaybackActive(true);
                               hooks_.inspectorTypePreGainAndReturn("-24");
                               return true;
                           },
                           500 });
    steps_.push_back(Step{ "pregain-inspector: phase B at -24 dB - stop, seek 0 and play (lead-in)",
                           [this](juce::String& failReason) -> bool {
                               if (std::fabs(hooks_.getTrackPreGainDb(inspectorTargetTrackId_) + 24.0f) > 1.0e-4f)
                               {
                                   failReason = "typing -24 did not commit -24 dB";
                                   return false;
                               }
                               hooks_.setPlaybackActive(false);
                               hooks_.seekTransportTo(0);
                               hooks_.setPlaybackActive(true);
                               return true;
                           },
                           600 });
    steps_.push_back(Step{ "pregain-inspector: phase B at -24 dB - arm measurement window",
                           [armTap](juce::String&) -> bool {
                               armTap();
                               return true;
                           },
                           1500 });
    steps_.push_back(Step{
        "pregain-inspector: phase B at -24 dB - levels and ratios (same passage as 0 dB)",
        [this](juce::String& failReason) -> bool {
            float before = 0.0f, after = 0.0f;
            double rmsBefore = 0.0, rmsAfter = 0.0;
            std::uint32_t preBlocks = 0, postBlocks = 0;
            hooks_.readAndResetInsertLevelTap(before, after, rmsBefore, rmsAfter, preBlocks, postBlocks);
            const float devicePeak = hooks_.readOutputPeakHoldAndReset();
            hooks_.setPlaybackActive(false);
            const double expected = std::pow(10.0, -24.0 / 20.0);
            const double ratioBeforeRms = inspectorRmsBefore0dB_ > 0.0 ? rmsBefore / inspectorRmsBefore0dB_ : 0.0;
            const double afterDb = (rmsAfter > 0.0 && inspectorRmsAfter0dB_ > 0.0)
                                       ? 20.0 * std::log10(rmsAfter / inspectorRmsAfter0dB_)
                                       : -999.0;
            appendStabilityRunLine("  phase B (-24 dB): device output peak=" + juce::String(devicePeak, 5)
                                   + " | chain blocks pre=" + juce::String((int)preBlocks) + " post=" + juce::String((int)postBlocks)
                                   + " | before first insert peak=" + juce::String(before, 5) + " rms=" + juce::String(rmsBefore, 6)
                                   + " (rms ratio vs 0 dB=" + juce::String(ratioBeforeRms, 5) + ", expected " + juce::String(expected, 5)
                                   + ") | after AmpliTube peak=" + juce::String(after, 5) + " rms=" + juce::String(rmsAfter, 6)
                                   + " (" + juce::String(afterDb, 2) + " dB vs 0 dB)");
            if (std::fabs(ratioBeforeRms - expected) > expected * 0.05)
            {
                failReason = "RMS entering the first insert did not scale by -24 dB (ratio "
                             + juce::String(ratioBeforeRms, 5) + ")";
                return false;
            }
            if (!(rmsAfter < inspectorRmsAfter0dB_ * 0.9))
            {
                failReason = "AmpliTube output did not drop measurably at -24 dB input";
                return false;
            }
            return true;
        },
        kSettleDefaultMs });
}

// -----------------------------------------------------------------------------
// Inserts: Save / reload / autosave-recovery + unavailable-plugin placeholder, in the real app
// -----------------------------------------------------------------------------
namespace
{
    [[nodiscard]] juce::String describeInsertRows(const std::vector<StabilityInsertRowInfo>& rows)
    {
        juce::String s;
        for (const auto& r : rows)
        {
            s << "[" << (r.pre ? "pre" : "post") << " \"" << r.displayName << "\"" << (r.unavailable ? " UNAVAILABLE" : "")
              << "] ";
        }
        return s.isEmpty() ? juce::String("(none)") : s;
    }

    /// Expected rows for one track: `pre` + name fragment + unavailable flag, in chain order.
    struct ExpectedInsertRow
    {
        bool pre = false;
        juce::String nameFragment;
        bool unavailable = false;
    };

    [[nodiscard]] bool rowsMatch(const std::vector<StabilityInsertRowInfo>& rows,
                                 const std::vector<ExpectedInsertRow>& expected,
                                 juce::String& why)
    {
        if (rows.size() != expected.size())
        {
            why = "expected " + juce::String((int)expected.size()) + " insert row(s), found "
                  + juce::String((int)rows.size()) + ": " + describeInsertRows(rows);
            return false;
        }
        for (size_t i = 0; i < rows.size(); ++i)
        {
            const auto& r = rows[i];
            const auto& e = expected[i];
            if (r.pre != e.pre || !r.displayName.containsIgnoreCase(e.nameFragment) || r.unavailable != e.unavailable)
            {
                why = "row " + juce::String((int)i) + " is [" + (r.pre ? "pre" : "post") + " \"" + r.displayName + "\""
                      + (r.unavailable ? " UNAVAILABLE" : "") + "], expected [" + (e.pre ? "pre" : "post") + " ~\""
                      + e.nameFragment + "\"" + (e.unavailable ? " UNAVAILABLE" : "") + "]";
                return false;
            }
        }
        return true;
    }

    /// Re-points `pluginVst3Path` of every insert on `trackId` in `src` to `newPath`, written to `dest`.
    [[nodiscard]] bool rewriteInsertPathsForTrack(const juce::File& src,
                                                  const juce::File& dest,
                                                  const TrackId trackId,
                                                  const juce::String& newPath)
    {
        juce::var root = juce::JSON::parse(src);
        const juce::var tracks = root.getProperty("tracks", {});
        if (!tracks.isArray())
        {
            return false;
        }
        bool done = false;
        for (const juce::var& t : *tracks.getArray())
        {
            if (static_cast<juce::int64>(t.getProperty("id", 0)) != static_cast<juce::int64>(trackId))
            {
                continue;
            }
            const juce::var ins = t.getProperty("inserts", {});
            if (!ins.isArray())
            {
                continue;
            }
            for (const juce::var& iv : *ins.getArray())
            {
                if (auto* obj = iv.getDynamicObject())
                {
                    obj->setProperty("pluginVst3Path", newPath);
                    done = true;
                }
            }
        }
        return done && dest.replaceWithText(juce::JSON::toString(root));
    }
} // namespace

void StabilityScenarioRunner::appendInsertsSteps(const juce::File& project)
{
    if (hooks_.midiRoutingFixtureSetup == nullptr || hooks_.fixtureInstrumentTrackId == nullptr
        || hooks_.listAllTracks == nullptr || hooks_.addInsertLikePicker == nullptr || hooks_.listInsertRows == nullptr
        || hooks_.saveProject == nullptr || hooks_.renameTrackUndoable == nullptr || hooks_.forceAutosaveNow == nullptr
        || hooks_.recoverAutosaveNow == nullptr || hooks_.describeTrackForDiagnostics == nullptr)
    {
        steps_.push_back(Step{ "inserts: hooks missing",
                               [](juce::String& failReason) -> bool {
                                   failReason = "inserts hooks not installed";
                                   return false;
                               },
                               0 });
        return;
    }

    const juce::File delayBundle("C:\\Program Files\\Common Files\\VST3\\DALMonoDelay.vst3");

    steps_.push_back(Step{
        "inserts: copy project to sibling test file",
        [this, project](juce::String& failReason) -> bool {
            const juce::File copy = project.getSiblingFile(project.getFileNameWithoutExtension() + "-insertstest.dalproj");
            (void)copy.deleteFile();
            if (!project.copyFileTo(copy))
            {
                failReason = "could not copy project to " + copy.getFullPathName();
                return false;
            }
            openSaveCloseCopy_ = copy;
            appendStabilityRunLine("  test copy: " + copy.getFullPathName());
            return true;
        },
        kSettleDefaultMs });

    steps_.push_back(Step{ "inserts: load test copy",
                           [this](juce::String&) -> bool {
                               hooks_.loadProjectFromFile(openSaveCloseCopy_);
                               return true;
                           },
                           kSettleAfterLoadMs });

    steps_.push_back(Step{ "inserts: build an instrument shell row (same fixture as midi-routing)",
                           [this](juce::String& failReason) -> bool {
                               if (!hooks_.midiRoutingFixtureSetup(failReason))
                               {
                                   return false;
                               }
                               insertsInstrumentTrackId_ = hooks_.fixtureInstrumentTrackId();
                               for (const StabilityTrackInfo& t : hooks_.listAllTracks())
                               {
                                   if (t.kindName == "audio" && insertsAudioTrackId_ == kInvalidTrackId)
                                   {
                                       insertsAudioTrackId_ = t.id;
                                   }
                                   if (t.kindName == "master")
                                   {
                                       insertsMasterTrackId_ = t.id;
                                   }
                               }
                               appendStabilityRunLine("  instrument row id=" + juce::String((juce::int64)insertsInstrumentTrackId_)
                                                      + " audio row id=" + juce::String((juce::int64)insertsAudioTrackId_)
                                                      + " master id=" + juce::String((juce::int64)insertsMasterTrackId_));
                               if (insertsInstrumentTrackId_ == kInvalidTrackId || insertsAudioTrackId_ == kInvalidTrackId
                                   || insertsMasterTrackId_ == kInvalidTrackId)
                               {
                                   failReason = "fixture rows missing (need instrument + audio + master)";
                                   return false;
                               }
                               return true;
                           },
                           600 });

    steps_.push_back(Step{
        "inserts: add DAL Mono Delay - Post on instrument, Pre on audio, Post on master (picker path)",
        [this, delayBundle](juce::String& failReason) -> bool {
            if (!delayBundle.exists())
            {
                failReason = "DAL Mono Delay bundle not installed: " + delayBundle.getFullPathName();
                return false;
            }
            for (const auto& [tid, pre] : std::vector<std::pair<TrackId, bool>>{
                     { insertsInstrumentTrackId_, false }, { insertsAudioTrackId_, true }, { insertsMasterTrackId_, false } })
            {
                const juce::Result r = hooks_.addInsertLikePicker(tid, pre, delayBundle);
                if (r.failed())
                {
                    failReason = "addInsert on track " + juce::String((juce::int64)tid) + " failed: " + r.getErrorMessage();
                    return false;
                }
            }
            if (hooks_.isProjectDirty && !hooks_.isProjectDirty())
            {
                failReason = "project not marked dirty after adding inserts";
                return false;
            }
            for (const StabilityTrackInfo& t : hooks_.listAllTracks())
            {
                appendStabilityRunLine("  " + hooks_.describeTrackForDiagnostics(t.id));
            }
            return true;
        },
        600 });

    const auto verifyRows = [this](const juce::String& phase, juce::String& failReason) -> bool {
        struct Row { TrackId id; const char* kind; std::vector<ExpectedInsertRow> expected; };
        const Row rows[] = {
            { insertsInstrumentTrackId_, "instrument", { { false, "DAL Mono Delay", false } } },
            { insertsAudioTrackId_, "audio", { { true, "DAL Mono Delay", false } } },
            { insertsMasterTrackId_, "master", { { false, "DAL Mono Delay", false } } },
        };
        for (const Row& r : rows)
        {
            const auto got = hooks_.listInsertRows(r.id);
            appendStabilityRunLine("  " + phase + ": " + r.kind + " track " + juce::String((juce::int64)r.id) + " rows "
                                   + describeInsertRows(got));
            juce::String why;
            if (!rowsMatch(got, r.expected, why))
            {
                failReason = phase + ": " + r.kind + " track " + juce::String((juce::int64)r.id) + ": " + why;
                return false;
            }
        }
        return true;
    };

    steps_.push_back(Step{ "inserts: save (production Save to the test copy)",
                           [this](juce::String&) -> bool {
                               hooks_.saveProject();
                               return true;
                           },
                           600 });

    steps_.push_back(Step{ "inserts: reload test copy",
                           [this](juce::String&) -> bool {
                               hooks_.loadProjectFromFile(openSaveCloseCopy_);
                               return true;
                           },
                           kSettleAfterLoadMs + 800 });

    steps_.push_back(Step{ "inserts: verify every row after Save -> reload",
                           [verifyRows](juce::String& failReason) -> bool { return verifyRows("after reload", failReason); },
                           kSettleDefaultMs });

    steps_.push_back(Step{ "inserts: dirty edit (rename the audio row) + forced autosave",
                           [this](juce::String& failReason) -> bool {
                               if (!hooks_.renameTrackUndoable(insertsAudioTrackId_, "InsertsAutosave "
                                                                                          + juce::Time::getCurrentTime().formatted("%H%M%S")))
                               {
                                   failReason = "rename refused";
                                   return false;
                               }
                               return hooks_.forceAutosaveNow(failReason);
                           },
                           600 });

    steps_.push_back(Step{ "inserts: recover the autosave in-process",
                           [this](juce::String& failReason) -> bool { return hooks_.recoverAutosaveNow(failReason); },
                           kSettleAfterLoadMs + 800 });

    steps_.push_back(Step{ "inserts: verify every row after autosave recovery",
                           [verifyRows](juce::String& failReason) -> bool { return verifyRows("after recovery", failReason); },
                           kSettleDefaultMs });

    steps_.push_back(Step{
        "inserts: simulate an uninstalled plugin (re-point the instrument row's saved path; user plugins untouched)",
        [this](juce::String& failReason) -> bool {
            insertsMissingPluginCopy_ = openSaveCloseCopy_.getSiblingFile(
                openSaveCloseCopy_.getFileNameWithoutExtension() + "-missingplugin.dalproj");
            (void)insertsMissingPluginCopy_.deleteFile();
            const juce::String fake = "C:\\Program Files\\Common Files\\VST3\\__DAL_not_installed__\\DALMonoDelay.vst3";
            if (!rewriteInsertPathsForTrack(openSaveCloseCopy_, insertsMissingPluginCopy_, insertsInstrumentTrackId_, fake))
            {
                failReason = "could not rewrite the saved insert path";
                return false;
            }
            appendStabilityRunLine("  missing-plugin copy: " + insertsMissingPluginCopy_.getFullPathName());
            return true;
        },
        kSettleDefaultMs });

    steps_.push_back(Step{ "inserts: load the missing-plugin copy",
                           [this](juce::String&) -> bool {
                               hooks_.loadProjectFromFile(insertsMissingPluginCopy_);
                               return true;
                           },
                           kSettleAfterLoadMs + 800 });

    const auto verifyUnavailable = [this](const juce::String& phase, juce::String& failReason) -> bool {
        const auto inst = hooks_.listInsertRows(insertsInstrumentTrackId_);
        const auto audio = hooks_.listInsertRows(insertsAudioTrackId_);
        appendStabilityRunLine("  " + phase + ": instrument rows " + describeInsertRows(inst) + " | audio rows "
                               + describeInsertRows(audio));
        juce::String why;
        if (!rowsMatch(inst, { { false, "DAL Mono Delay", true } }, why))
        {
            failReason = phase + ": instrument row: " + why;
            return false;
        }
        if (!rowsMatch(audio, { { true, "DAL Mono Delay", false } }, why))
        {
            failReason = phase + ": audio row: " + why;
            return false;
        }
        return true;
    };

    steps_.push_back(Step{ "inserts: verify the insert is kept as \"(unavailable)\" (others live)",
                           [verifyUnavailable](juce::String& failReason) -> bool {
                               return verifyUnavailable("missing plugin", failReason);
                           },
                           kSettleDefaultMs });

    steps_.push_back(Step{ "inserts: save while the plugin is missing, then reload",
                           [this](juce::String&) -> bool {
                               hooks_.saveProject();
                               return true;
                           },
                           600 });

    steps_.push_back(Step{ "inserts: reload the missing-plugin copy",
                           [this](juce::String&) -> bool {
                               hooks_.loadProjectFromFile(insertsMissingPluginCopy_);
                               return true;
                           },
                           kSettleAfterLoadMs + 800 });

    steps_.push_back(Step{ "inserts: verify the unavailable insert survived save -> reload (no data loss)",
                           [verifyUnavailable](juce::String& failReason) -> bool {
                               return verifyUnavailable("after re-save", failReason);
                           },
                           kSettleDefaultMs });
}

// -----------------------------------------------------------------------------
// Header column: shared boundary geometry, resize path, persistence, PNG evidence
// -----------------------------------------------------------------------------
void StabilityScenarioRunner::appendHeaderColumnSteps(const juce::File& project)
{
    if (hooks_.dragHeaderColumnLikeHandle == nullptr || hooks_.getHeaderColumnWidthPreference == nullptr
        || hooks_.getHeaderColumnEffectiveWidth == nullptr || hooks_.readPersistedHeaderColumnWidth == nullptr
        || hooks_.verifyHeaderColumnLayout == nullptr || hooks_.captureArrangementPng == nullptr
        || hooks_.addMidiTrackLikeUi == nullptr || hooks_.requestDeleteTrack == nullptr)
    {
        steps_.push_back(Step{ "header-column: hooks missing",
                               [](juce::String& failReason) -> bool {
                                   failReason = "header-column hooks not installed";
                                   return false;
                               },
                               0 });
        return;
    }

    // Evidence folder is deliberately NOT `scenarioOutputDir_` (which `finish` deletes): the PNGs
    // are the deliverable of this scenario and stay for the report.
    auto evidenceDir = std::make_shared<juce::File>(
        juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("dal-stability-header-column"));
    (void)evidenceDir->deleteRecursively();
    (void)evidenceDir->createDirectory();

    appendLoadAndVerifySteps(project, "header-column");

    const auto verifyAndCapture = [this, evidenceDir](const juce::String& label, const int expectedEffective,
                                                      juce::String& failReason) -> bool {
        const int pref = hooks_.getHeaderColumnWidthPreference();
        const int eff = hooks_.getHeaderColumnEffectiveWidth();
        appendStabilityRunLine("  " + label + ": preference=" + juce::String(pref) + " effective=" + juce::String(eff));
        if (expectedEffective > 0 && eff != expectedEffective)
        {
            failReason = label + ": effective width " + juce::String(eff) + " != expected " + juce::String(expectedEffective);
            return false;
        }
        juce::String report;
        const bool ok = hooks_.verifyHeaderColumnLayout(report, failReason);
        for (const auto& line : juce::StringArray::fromLines(report))
        {
            if (line.isNotEmpty())
            {
                appendStabilityRunLine("    " + line);
            }
        }
        const juce::File png = evidenceDir->getChildFile("header-column-" + label + ".png");
        if (hooks_.captureArrangementPng(png))
        {
            appendStabilityRunLine("  evidence: " + png.getFullPathName());
        }
        return ok;
    };

    steps_.push_back(Step{ "header-column: geometry at the startup width (persisted preference)",
                           [this, verifyAndCapture](juce::String& failReason) -> bool {
                               headerColumnWidthAtStart_ = hooks_.getHeaderColumnWidthPreference();
                               appendStabilityRunLine("  persisted on disk: "
                                                      + (hooks_.readPersistedHeaderColumnWidth().has_value()
                                                             ? juce::String(*hooks_.readPersistedHeaderColumnWidth())
                                                             : juce::String("(absent -> default)")));
                               return verifyAndCapture("startup", 0, failReason);
                           },
                           kSettleDefaultMs });

    steps_.push_back(Step{ "header-column: drag to the default width (144)",
                           [this, verifyAndCapture](juce::String& failReason) -> bool {
                               hooks_.dragHeaderColumnLikeHandle(144 - hooks_.getHeaderColumnEffectiveWidth());
                               return verifyAndCapture("default-144", 144, failReason);
                           },
                           kSettleDefaultMs });

    steps_.push_back(Step{ "header-column: drag far left -> clamps at the minimum (132), every button inside",
                           [this, verifyAndCapture](juce::String& failReason) -> bool {
                               hooks_.dragHeaderColumnLikeHandle(-600);
                               return verifyAndCapture("minimum-132", 132, failReason);
                           },
                           kSettleDefaultMs });

    steps_.push_back(Step{ "header-column: drag to a wide setting (240) for long names",
                           [this, verifyAndCapture](juce::String& failReason) -> bool {
                               hooks_.dragHeaderColumnLikeHandle(240 - hooks_.getHeaderColumnEffectiveWidth());
                               return verifyAndCapture("wide-240", 240, failReason);
                           },
                           kSettleDefaultMs });

    steps_.push_back(Step{ "header-column: persisted app-wide after the drag ended",
                           [this](juce::String& failReason) -> bool {
                               const std::optional<int> onDisk = hooks_.readPersistedHeaderColumnWidth();
                               appendStabilityRunLine("  persisted on disk: "
                                                      + (onDisk.has_value() ? juce::String(*onDisk) : juce::String("(absent)")));
                               if (!onDisk.has_value() || *onDisk != 240)
                               {
                                   failReason = "ui-layout.xml does not carry 240 after the drag";
                                   return false;
                               }
                               return true;
                           },
                           kSettleDefaultMs });

    steps_.push_back(Step{ "header-column: add a MIDI track -> width unchanged, new row on the same boundary",
                           [this, verifyAndCapture](juce::String& failReason) -> bool {
                               const std::optional<TrackId> tid = hooks_.addMidiTrackLikeUi();
                               if (!tid.has_value())
                               {
                                   failReason = "add MIDI track refused";
                                   return false;
                               }
                               scenarioMidiTrackId_ = *tid;
                               return verifyAndCapture("after-add-track", 240, failReason);
                           },
                           600 });

    steps_.push_back(Step{ "header-column: delete that track -> width unchanged",
                           [this, verifyAndCapture](juce::String& failReason) -> bool {
                               hooks_.requestDeleteTrack(scenarioMidiTrackId_);
                               return verifyAndCapture("after-delete-track", 240, failReason);
                           },
                           kSettleAfterDeleteOpMs });

    steps_.push_back(Step{ "header-column: restore the user's preference and persist it",
                           [this](juce::String& failReason) -> bool {
                               hooks_.dragHeaderColumnLikeHandle(headerColumnWidthAtStart_ - hooks_.getHeaderColumnEffectiveWidth());
                               const std::optional<int> onDisk = hooks_.readPersistedHeaderColumnWidth();
                               appendStabilityRunLine("  restored preference=" + juce::String(hooks_.getHeaderColumnWidthPreference())
                                                      + " on disk=" + (onDisk.has_value() ? juce::String(*onDisk) : juce::String("(absent)")));
                               if (hooks_.getHeaderColumnWidthPreference() != headerColumnWidthAtStart_)
                               {
                                   failReason = "could not restore the startup preference";
                                   return false;
                               }
                               return true;
                           },
                           kSettleDefaultMs });
}

// -----------------------------------------------------------------------------
// Export levels: realtime Stereo Out vs offline export (float / 24-bit / MP3) on a project copy
// -----------------------------------------------------------------------------
namespace
{
    [[nodiscard]] juce::String dbfs(const float linear)
    {
        if (!std::isfinite(linear))
        {
            return "NaN";
        }
        if (linear <= 1.0e-6f)
        {
            return "-inf";
        }
        const double db = 20.0 * std::log10((double)linear);
        return (db > 0.0 ? "+" : "") + juce::String(db, 2);
    }

    [[nodiscard]] juce::String describeLevelStats(const StabilityLevelStats& s)
    {
        return "frames=" + juce::String((juce::int64)s.frames)
               + " peakL=" + juce::String(s.peak[0], 4) + " (" + dbfs(s.peak[0]) + " dBFS)"
               + " peakR=" + juce::String(s.peak[1], 4) + " (" + dbfs(s.peak[1]) + " dBFS)"
               + " rmsL=" + juce::String(s.rms[0], 4) + " (" + dbfs((float)s.rms[0]) + ")"
               + " rmsR=" + juce::String(s.rms[1], 4) + " (" + dbfs((float)s.rms[1]) + ")"
               + " dcL=" + juce::String(s.dcOffset[0], 5) + " dcR=" + juce::String(s.dcOffset[1], 5)
               + " oversL=" + juce::String((juce::int64)s.overs[0]) + " oversR=" + juce::String((juce::int64)s.overs[1])
               + " nonFinite=" + juce::String((juce::int64)s.nonFinite)
               + " first=[" + juce::String(s.firstSample[0], 5) + "," + juce::String(s.firstSample[1], 5) + "]"
               + " last=[" + juce::String(s.lastSample[0], 5) + "," + juce::String(s.lastSample[1], 5) + "]";
    }

    [[nodiscard]] double ratioDb(const double a, const double b)
    {
        if (a <= 0.0 || b <= 0.0)
        {
            return 0.0;
        }
        return 20.0 * std::log10(a / b);
    }
} // namespace

void StabilityScenarioRunner::appendExportLevelsSteps(const juce::File& project)
{
    if (hooks_.drainMasterMeter == nullptr || hooks_.runMixdownWithLevelReport == nullptr
        || hooks_.getTrackChannelFaderGain == nullptr || hooks_.setTrackChannelFaderGain == nullptr
        || hooks_.getActiveLoopSpan == nullptr || hooks_.listAllTracks == nullptr || hooks_.seekTransportTo == nullptr
        || hooks_.setPlaybackActive == nullptr || hooks_.describeTrackForDiagnostics == nullptr)
    {
        steps_.push_back(Step{ "export-levels: hooks missing",
                               [](juce::String& failReason) -> bool {
                                   failReason = "export-levels hooks not installed";
                                   return false;
                               },
                               0 });
        return;
    }

    // Kept for the offline analyzer (ExportLevelFocusedTests --analyze): deliberately NOT
    // `scenarioOutputDir_` (which `finish` deletes).
    auto outDir = std::make_shared<juce::File>(
        juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("dal-export-levels"));
    (void)outDir->deleteRecursively();
    (void)outDir->createDirectory();

    appendLoadAndVerifySteps(project, "export-levels");

    steps_.push_back(Step{ "export-levels: describe tracks, resolve the active loop and the master row",
                           [this, outDir](juce::String& failReason) -> bool {
                               for (const StabilityTrackInfo& t : hooks_.listAllTracks())
                               {
                                   appendStabilityRunLine("  " + hooks_.describeTrackForDiagnostics(t.id)
                                                          + " fader=" + juce::String(hooks_.getTrackChannelFaderGain(t.id), 4));
                                   if (t.kindName == "master")
                                   {
                                       exportMasterTrackId_ = t.id;
                                   }
                               }
                               std::int64_t start = 0;
                               if (!hooks_.getActiveLoopSpan(start, exportLoopLengthSamples_, exportSampleRate_))
                               {
                                   failReason = "no active loop range (cycle must be on with R > L)";
                                   return false;
                               }
                               exportMasterFaderAtStart_ = exportMasterTrackId_ != kInvalidTrackId
                                                               ? hooks_.getTrackChannelFaderGain(exportMasterTrackId_)
                                                               : 1.0f;
                               appendStabilityRunLine("  loop start=" + juce::String((juce::int64)start) + " length="
                                                      + juce::String((juce::int64)exportLoopLengthSamples_) + " samples ("
                                                      + juce::String((double)exportLoopLengthSamples_ / exportSampleRate_, 2)
                                                      + " s) sampleRate=" + juce::String(exportSampleRate_)
                                                      + " master id=" + juce::String((juce::int64)exportMasterTrackId_)
                                                      + " master fader=" + juce::String(exportMasterFaderAtStart_, 4) + " ("
                                                      + dbfs(exportMasterFaderAtStart_) + " dB)" + " out=" + outDir->getFullPathName());
                               return exportMasterTrackId_ != kInvalidTrackId;
                           },
                           kSettleDefaultMs });

    // Realtime pass: play the whole loop once from its start and fold the device output.
    const auto realtimePassSteps = [this](const juce::String& label, StabilityLevelStats* into) {
        steps_.push_back(Step{ "export-levels: " + label + " realtime - seek loop start and play",
                               [this](juce::String&) -> bool {
                                   std::int64_t start = 0, len = 0;
                                   double sr = 0.0;
                                   (void)hooks_.getActiveLoopSpan(start, len, sr);
                                   hooks_.seekTransportTo(start);
                                   hooks_.setPlaybackActive(true);
                                   (void)hooks_.drainMasterMeter(); // discard the pre-roll block(s)
                                   return true;
                               },
                               250 });
        steps_.push_back(Step{ "export-levels: " + label + " realtime - arm measurement window (whole loop)",
                               [this](juce::String&) -> bool {
                                   (void)hooks_.drainMasterMeter();
                                   // Whole loop, capped at 40 s so long projects stay practical.
                                   settleOverrideMsForCurrentStep_ = static_cast<int>(juce::jlimit(
                                       2000.0, 40000.0,
                                       1000.0 * (double)exportLoopLengthSamples_ / juce::jmax(1.0, exportSampleRate_)));
                                   appendStabilityRunLine("  measuring for " + juce::String(settleOverrideMsForCurrentStep_) + " ms");
                                   return true;
                               },
                               2000 });
        steps_.push_back(Step{ "export-levels: " + label + " realtime - read Stereo Out meter and stop",
                               [this, into, label](juce::String& failReason) -> bool {
                                   *into = hooks_.drainMasterMeter();
                                   hooks_.setPlaybackActive(false);
                                   appendStabilityRunLine("  " + label + " REALTIME Stereo Out: " + describeLevelStats(*into));
                                   if (into->frames == 0)
                                   {
                                       failReason = "no audio blocks were measured (device callback not running?)";
                                       return false;
                                   }
                                   return true;
                               },
                               600 });
    };

    realtimePassSteps("A (project state)", &exportRealtimeA_);

    // Per-track sweep: which row carries level / DC. Each row is metered for 3 s of playback from
    // the loop start (post-strip stage: after fader, mute and pan, before its output bus).
    if (hooks_.setMeteredTrack != nullptr && hooks_.drainTrackMeter != nullptr)
    {
        steps_.push_back(Step{ "export-levels: per-track sweep - plan",
                               [this](juce::String&) -> bool {
                                   const std::vector<StabilityTrackInfo> tracks = hooks_.listAllTracks();
                                   size_t insertAt = nextStepIndex_;
                                   for (const StabilityTrackInfo& t : tracks)
                                   {
                                       if (t.kindName == "midi")
                                       {
                                           continue; // no audio path
                                       }
                                       const TrackId tid = t.id;
                                       const juce::String who = t.kindName + " track " + juce::String((juce::int64)tid) + " \"" + t.name + "\"";
                                       steps_.insert(steps_.begin() + (long)insertAt++,
                                                     Step{ "export-levels: sweep " + who + " - meter + play 3 s",
                                                           [this, tid](juce::String&) -> bool {
                                                               std::int64_t start = 0, len = 0;
                                                               double sr = 0.0;
                                                               (void)hooks_.getActiveLoopSpan(start, len, sr);
                                                               // Select the row like a header click so the Inspector
                                                               // panel follows (it owns the live meter tap), then
                                                               // point the tap explicitly for the diagnostics window.
                                                               if (hooks_.activateTrackLikeHeaderClick != nullptr)
                                                               {
                                                                   hooks_.activateTrackLikeHeaderClick(tid);
                                                               }
                                                               hooks_.setMeteredTrack(tid);
                                                               hooks_.seekTransportTo(start);
                                                               hooks_.setPlaybackActive(true);
                                                               (void)hooks_.drainTrackMeter();
                                                               (void)hooks_.drainMasterMeter();
                                                               return true;
                                                           },
                                                           3000 });
                                       steps_.insert(steps_.begin() + (long)insertAt++,
                                                     Step{ "export-levels: sweep " + who + " - read",
                                                           [this, who](juce::String&) -> bool {
                                                               const StabilityLevelStats s = hooks_.drainTrackMeter();
                                                               const StabilityLevelStats m = hooks_.drainMasterMeter();
                                                               hooks_.setPlaybackActive(false);
                                                               appendStabilityRunLine("  " + who + " post-strip: " + describeLevelStats(s));
                                                               appendStabilityRunLine("  (Stereo Out during the same 3 s: " + describeLevelStats(m) + ")");
                                                               return true;
                                                           },
                                                           500 });
                                   }
                                   steps_.insert(steps_.begin() + (long)insertAt++,
                                                 Step{ "export-levels: sweep done - meter off",
                                                       [this](juce::String&) -> bool {
                                                           hooks_.setMeteredTrack(kInvalidTrackId);
                                                           return true;
                                                       },
                                                       kSettleDefaultMs });
                                   return true;
                               },
                               0 });
    }

    const auto exportStep = [this, outDir](const juce::String& label, const juce::String& fileName, const bool mp3,
                                           const int bits, StabilityLevelStats* into) {
        steps_.push_back(Step{ "export-levels: " + label,
                               [this, outDir, fileName, mp3, bits, into, label](juce::String& failReason) -> bool {
                                   StabilityLevelStats report;
                                   const juce::File out = outDir->getChildFile(fileName);
                                   const juce::Result r = hooks_.runMixdownWithLevelReport(out, mp3, bits, report);
                                   if (r.failed())
                                   {
                                       failReason = "export failed: " + r.getErrorMessage();
                                       return false;
                                   }
                                   if (!out.existsAsFile() || out.getSize() <= 0)
                                   {
                                       failReason = "export produced no file: " + out.getFullPathName();
                                       return false;
                                   }
                                   appendStabilityRunLine("  " + label + " -> " + out.getFullPathName() + " (" + juce::String(out.getSize())
                                                          + " bytes) OFFLINE render: " + describeLevelStats(report));
                                   if (into != nullptr)
                                   {
                                       *into = report;
                                   }
                                   return true;
                               },
                               600 });
    };

    exportStep("A export WAV float32", "A-master-float32.wav", false, 32, &exportFloatA_);
    exportStep("A export WAV 24-bit", "A-master-pcm24.wav", false, 24, nullptr);
    exportStep("A export MP3 192 kbps", "A-master-192.mp3", true, 0, nullptr);

    steps_.push_back(Step{ "export-levels: compare realtime A vs offline float A",
                           [this](juce::String&) -> bool {
                               const double dPeakL = ratioDb(exportFloatA_.peak[0], exportRealtimeA_.peak[0]);
                               const double dPeakR = ratioDb(exportFloatA_.peak[1], exportRealtimeA_.peak[1]);
                               const double dRmsL = ratioDb(exportFloatA_.rms[0], exportRealtimeA_.rms[0]);
                               const double dRmsR = ratioDb(exportFloatA_.rms[1], exportRealtimeA_.rms[1]);
                               appendStabilityRunLine("  offline/realtime delta: peakL=" + juce::String(dPeakL, 2) + " dB peakR="
                                                      + juce::String(dPeakR, 2) + " dB rmsL=" + juce::String(dRmsL, 2) + " dB rmsR="
                                                      + juce::String(dRmsR, 2) + " dB (positive = export louder)");
                               return true; // informational: plug-ins with modulation make exact equality meaningless here
                           },
                           kSettleDefaultMs });

    // Diagnostic: master fader 12 dB lower, both paths again.
    steps_.push_back(Step{ "export-levels: B - set Stereo Out fader 12 dB lower (diagnostic, restored later)",
                           [this](juce::String&) -> bool {
                               const float target = exportMasterFaderAtStart_ * 0.251189f;
                               hooks_.setTrackChannelFaderGain(exportMasterTrackId_, target);
                               appendStabilityRunLine("  master fader " + juce::String(exportMasterFaderAtStart_, 4) + " -> "
                                                      + juce::String(hooks_.getTrackChannelFaderGain(exportMasterTrackId_), 4));
                               return true;
                           },
                           kSettleDefaultMs });
    exportStep("B export WAV float32 (-12 dB master)", "B-master-minus12-float32.wav", false, 32, &exportFloatB_);
    exportStep("B export MP3 192 kbps (-12 dB master)", "B-master-minus12-192.mp3", true, 0, nullptr);
    realtimePassSteps("B (-12 dB master)", &exportRealtimeB_);

    steps_.push_back(Step{ "export-levels: master fader reaches both paths (B vs A)",
                           [this](juce::String& failReason) -> bool {
                               const double offL = ratioDb(exportFloatB_.rms[0], exportFloatA_.rms[0]);
                               const double offR = ratioDb(exportFloatB_.rms[1], exportFloatA_.rms[1]);
                               const double rtL = ratioDb(exportRealtimeB_.rms[0], exportRealtimeA_.rms[0]);
                               const double rtR = ratioDb(exportRealtimeB_.rms[1], exportRealtimeA_.rms[1]);
                               const double offPk = ratioDb(juce::jmax(exportFloatB_.peak[0], exportFloatB_.peak[1]),
                                                            juce::jmax(exportFloatA_.peak[0], exportFloatA_.peak[1]));
                               const double rtPk = ratioDb(juce::jmax(exportRealtimeB_.peak[0], exportRealtimeB_.peak[1]),
                                                           juce::jmax(exportRealtimeA_.peak[0], exportRealtimeA_.peak[1]));
                               appendStabilityRunLine("  RMS change B-A: offline L=" + juce::String(offL, 2) + " R=" + juce::String(offR, 2)
                                                      + " dB | realtime L=" + juce::String(rtL, 2) + " R=" + juce::String(rtR, 2)
                                                      + " dB | peak change offline=" + juce::String(offPk, 2) + " dB realtime="
                                                      + juce::String(rtPk, 2) + " dB (expected -12.0)");
                               // Offline renders the same loop twice: only plug-in modulation can differ (1 dB).
                               if (std::fabs(offL + 12.0) > 1.0 || std::fabs(offR + 12.0) > 1.0)
                               {
                                   failReason = "offline export did not follow the master fader by -12 dB";
                                   return false;
                               }
                               // Realtime passes are two separate live performances of the loop (plug-in
                               // modulation, DC-offset presence of a plug-in varies with its history), so
                               // the RMS is only informational; the peak must follow within 3 dB.
                               if (std::fabs(rtPk + 12.0) > 3.0)
                               {
                                   failReason = "realtime output peak did not follow the master fader by -12 dB (within 3 dB)";
                                   return false;
                               }
                               return true;
                           },
                           kSettleDefaultMs });

    steps_.push_back(Step{ "export-levels: restore the Stereo Out fader",
                           [this](juce::String&) -> bool {
                               hooks_.setTrackChannelFaderGain(exportMasterTrackId_, exportMasterFaderAtStart_);
                               appendStabilityRunLine("  master fader restored to "
                                                      + juce::String(hooks_.getTrackChannelFaderGain(exportMasterTrackId_), 4));
                               return true;
                           },
                           kSettleDefaultMs });

    if (hooks_.audioHealthProbeBegin != nullptr && hooks_.audioHealthProbeVerify != nullptr)
    {
        steps_.push_back(Step{ "export-levels: playback after exports - start (probe armed)",
                               [this](juce::String&) -> bool {
                                   std::int64_t start = 0, len = 0;
                                   double sr = 0.0;
                                   (void)hooks_.getActiveLoopSpan(start, len, sr);
                                   hooks_.seekTransportTo(start);
                                   hooks_.audioHealthProbeBegin();
                                   hooks_.setPlaybackActive(true);
                                   return true;
                               },
                               2500 });
        steps_.push_back(Step{ "export-levels: playback after exports - verify audio health",
                               [this](juce::String& failReason) -> bool {
                                   const bool ok = hooks_.audioHealthProbeVerify("after exports", failReason);
                                   hooks_.setPlaybackActive(false);
                                   return ok;
                               },
                               600 });
    }
}

// -----------------------------------------------------------------------------
// Inspector channel panel in the real app
// -----------------------------------------------------------------------------
void StabilityScenarioRunner::appendInspectorPanelSteps(const juce::File& project)
{
    if (hooks_.verifyInspectorPanelLayout == nullptr || hooks_.captureInspectorPng == nullptr
        || hooks_.describeInspectorMeters == nullptr || hooks_.isInspectorOutputMeterOverloadLatched == nullptr
        || hooks_.inspectorOutputMeterShowsSignal == nullptr
        || hooks_.resetInspectorOverloadLatches == nullptr || hooks_.inspectorFaderTypeValue == nullptr
        || hooks_.inspectorFaderResetGesture == nullptr || hooks_.inspectorFaderValueText == nullptr
        || hooks_.inspectorScrollToBottomAndVerify == nullptr || hooks_.getMainWindowBounds == nullptr
        || hooks_.setMainWindowSize == nullptr || hooks_.activateTrackLikeHeaderClick == nullptr
        || hooks_.listAllTracks == nullptr || hooks_.getTrackChannelFaderGain == nullptr
        || hooks_.setTrackChannelFaderGain == nullptr || hooks_.seekTransportTo == nullptr
        || hooks_.setPlaybackActive == nullptr)
    {
        steps_.push_back(Step{ "inspector-panel: hooks missing",
                               [](juce::String& failReason) -> bool {
                                   failReason = "inspector-panel hooks not installed";
                                   return false;
                               },
                               0 });
        return;
    }

    auto evidenceDir = std::make_shared<juce::File>(
        juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("dal-stability-inspector-panel"));
    (void)evidenceDir->deleteRecursively();
    (void)evidenceDir->createDirectory();

    appendLoadAndVerifySteps(project, "inspector-panel");

    const auto verifyAndCapture = [this, evidenceDir](const juce::String& label, juce::String& failReason) -> bool {
        juce::String report;
        const bool ok = hooks_.verifyInspectorPanelLayout(report, failReason);
        for (const auto& line : juce::StringArray::fromLines(report))
        {
            if (line.isNotEmpty())
            {
                appendStabilityRunLine("    " + line);
            }
        }
        appendStabilityRunLine("    meters: " + hooks_.describeInspectorMeters());
        const juce::File png = evidenceDir->getChildFile("inspector-" + label + ".png");
        if (hooks_.captureInspectorPng(png))
        {
            appendStabilityRunLine("  evidence: " + png.getFullPathName());
        }
        return ok;
    };

    steps_.push_back(Step{ "inspector-panel: remember window bounds; layout at startup",
                           [this, verifyAndCapture](juce::String& failReason) -> bool {
                               inspectorWindowBoundsAtStart_ = hooks_.getMainWindowBounds();
                               appendStabilityRunLine("  main window " + inspectorWindowBoundsAtStart_.toString());
                               return verifyAndCapture("startup", failReason);
                           },
                           kSettleDefaultMs });

    // One activation + check per row kind present in the project.
    steps_.push_back(Step{ "inspector-panel: plan per-kind activations",
                           [this, verifyAndCapture](juce::String&) -> bool {
                               size_t insertAt = nextStepIndex_;
                               juce::StringArray seenKinds;
                               for (const StabilityTrackInfo& t : hooks_.listAllTracks())
                               {
                                   if (seenKinds.contains(t.kindName))
                                   {
                                       continue;
                                   }
                                   seenKinds.add(t.kindName);
                                   const TrackId tid = t.id;
                                   const juce::String kind = t.kindName;
                                   if (kind == "audio" && inspectorAudioTrackId_ == kInvalidTrackId)
                                   {
                                       inspectorAudioTrackId_ = tid;
                                   }
                                   steps_.insert(steps_.begin() + (long)insertAt++,
                                                 Step{ "inspector-panel: activate " + kind + " track " + juce::String((juce::int64)tid)
                                                           + " like a header click",
                                                       [this, tid](juce::String&) -> bool {
                                                           hooks_.activateTrackLikeHeaderClick(tid);
                                                           return true;
                                                       },
                                                       400 });
                                   steps_.insert(steps_.begin() + (long)insertAt++,
                                                 Step{ "inspector-panel: verify channel panel for " + kind,
                                                       [verifyAndCapture, kind](juce::String& failReason) -> bool {
                                                           return verifyAndCapture(kind, failReason);
                                                       },
                                                       kSettleDefaultMs });
                               }
                               return true;
                           },
                           0 });

    // Meters while playing (instrument row selected so the track meter has a source).
    steps_.push_back(Step{ "inspector-panel: select the instrument row with content and play from 24 s",
                           [this](juce::String& failReason) -> bool {
                               TrackId inst = kInvalidTrackId;
                               for (const StabilityTrackInfo& t : hooks_.listAllTracks())
                               {
                                   if (t.kindName == "instrument" && t.name.containsIgnoreCase("VB3"))
                                   {
                                       inst = t.id;
                                   }
                               }
                               if (inst == kInvalidTrackId)
                               {
                                   for (const StabilityTrackInfo& t : hooks_.listAllTracks())
                                   {
                                       if (t.kindName == "instrument")
                                       {
                                           inst = t.id;
                                           break;
                                       }
                                   }
                               }
                               if (inst == kInvalidTrackId)
                               {
                                   failReason = "no instrument row in this project";
                                   return false;
                               }
                               hooks_.activateTrackLikeHeaderClick(inst);
                               hooks_.resetInspectorOverloadLatches();
                               std::int64_t start = 0, len = 0;
                               double sr = 48000.0;
                               if (hooks_.getActiveLoopSpan != nullptr && hooks_.getActiveLoopSpan(start, len, sr))
                               {
                                   hooks_.seekTransportTo(start + (std::int64_t)(24.0 * sr));
                               }
                               hooks_.setPlaybackActive(true);
                               return true;
                           },
                           3000 });
    steps_.push_back(Step{ "inspector-panel: the selected instrument row's output meter shows signal while playing",
                           [this, verifyAndCapture](juce::String& failReason) -> bool {
                               const bool signal = hooks_.inspectorOutputMeterShowsSignal();
                               const bool ok = verifyAndCapture("playing", failReason);
                               hooks_.setPlaybackActive(false);
                               if (!ok)
                               {
                                   return false;
                               }
                               if (!signal)
                               {
                                   failReason = "output meter shows no signal for the playing instrument row";
                                   return false;
                               }
                               return true;
                           },
                           600 });

    // Overload latch: select the Stereo Out row (its meter is the master output), +6 dB makes this
    // project exceed 0 dBFS.
    steps_.push_back(Step{ "inspector-panel: select Stereo Out, fader +6 dB, play 3 s (forces an overload)",
                           [this](juce::String& failReason) -> bool {
                               TrackId master = kInvalidTrackId;
                               for (const StabilityTrackInfo& t : hooks_.listAllTracks())
                               {
                                   if (t.kindName == "master")
                                   {
                                       master = t.id;
                                   }
                               }
                               if (master == kInvalidTrackId)
                               {
                                   failReason = "no master row";
                                   return false;
                               }
                               exportMasterTrackId_ = master;
                               hooks_.activateTrackLikeHeaderClick(master);
                               exportMasterFaderAtStart_ = hooks_.getTrackChannelFaderGain(master);
                               hooks_.setTrackChannelFaderGain(master, 1.99526f);
                               hooks_.resetInspectorOverloadLatches();
                               std::int64_t start = 0, len = 0;
                               double sr = 48000.0;
                               if (hooks_.getActiveLoopSpan != nullptr && hooks_.getActiveLoopSpan(start, len, sr))
                               {
                                   hooks_.seekTransportTo(start + (std::int64_t)(24.0 * sr));
                               }
                               hooks_.setPlaybackActive(true);
                               return true;
                           },
                           3000 });
    steps_.push_back(Step{ "inspector-panel: overload latch set on the Stereo Out meter, then reset by the user gesture",
                           [this, verifyAndCapture](juce::String& failReason) -> bool {
                               hooks_.setPlaybackActive(false);
                               const bool latched = hooks_.isInspectorOutputMeterOverloadLatched();
                               appendStabilityRunLine("  after +6 dB playback: " + hooks_.describeInspectorMeters());
                               (void)verifyAndCapture("overload", failReason);
                               hooks_.setTrackChannelFaderGain(exportMasterTrackId_, exportMasterFaderAtStart_);
                               if (!latched)
                               {
                                   failReason = "Stereo Out overload lamp did not latch although the mix exceeded 0 dBFS";
                                   return false;
                               }
                               hooks_.resetInspectorOverloadLatches();
                               if (hooks_.isInspectorOutputMeterOverloadLatched())
                               {
                                   failReason = "overload lamp stayed latched after reset";
                                   return false;
                               }
                               appendStabilityRunLine("  latch reset ok; master fader restored to "
                                                      + juce::String(hooks_.getTrackChannelFaderGain(exportMasterTrackId_), 4));
                               return true;
                           },
                           kSettleDefaultMs });

    // Fader paths on the audio row: typed values and the reset gesture reach the session.
    steps_.push_back(Step{ "inspector-panel: fader typed values + reset gesture reach the session",
                           [this](juce::String& failReason) -> bool {
                               if (inspectorAudioTrackId_ == kInvalidTrackId)
                               {
                                   failReason = "no audio row";
                                   return false;
                               }
                               hooks_.activateTrackLikeHeaderClick(inspectorAudioTrackId_);
                               inspectorAudioFaderAtStart_ = hooks_.getTrackChannelFaderGain(inspectorAudioTrackId_);
                               struct Case { const char* text; float expected; };
                               const Case cases[] = { { "-6", 0.501187f }, { "+3", 1.412538f }, { "-inf", 0.0f }, { "12", 1.995262f } };
                               for (const Case& c : cases)
                               {
                                   hooks_.inspectorFaderTypeValue(c.text);
                                   const float got = hooks_.getTrackChannelFaderGain(inspectorAudioTrackId_);
                                   appendStabilityRunLine("  typed \"" + juce::String(c.text) + "\" -> session gain " + juce::String(got, 5)
                                                          + " (field shows \"" + hooks_.inspectorFaderValueText() + "\")");
                                   if (std::fabs(got - c.expected) > 1.0e-3f)
                                   {
                                       failReason = "typed \"" + juce::String(c.text) + "\" did not reach the session (expected "
                                                    + juce::String(c.expected, 4) + ")";
                                       return false;
                                   }
                               }
                               hooks_.inspectorFaderResetGesture();
                               const float reset = hooks_.getTrackChannelFaderGain(inspectorAudioTrackId_);
                               appendStabilityRunLine("  reset gesture -> session gain " + juce::String(reset, 5) + " (field \""
                                                      + hooks_.inspectorFaderValueText() + "\")");
                               hooks_.setTrackChannelFaderGain(inspectorAudioTrackId_, inspectorAudioFaderAtStart_);
                               if (std::fabs(reset - 1.0f) > 1.0e-6f)
                               {
                                   failReason = "reset gesture did not return the fader to 0 dB";
                                   return false;
                               }
                               return true;
                           },
                           kSettleDefaultMs });

    steps_.push_back(Step{ "inspector-panel: low window (height 640) keeps the panel usable and the scroll area alive",
                           [this](juce::String&) -> bool {
                               hooks_.setMainWindowSize(juce::jmax(900, inspectorWindowBoundsAtStart_.getWidth()), 640);
                               return true;
                           },
                           600 });
    steps_.push_back(Step{ "inspector-panel: verify layout at the low window",
                           [this, verifyAndCapture](juce::String& failReason) -> bool {
                               return verifyAndCapture("low-window", failReason);
                           },
                           kSettleDefaultMs });
    steps_.push_back(Step{ "inspector-panel: every Inspector control reachable by scrolling (low window)",
                           [this, verifyAndCapture](juce::String& failReason) -> bool {
                               juce::String detail;
                               const bool ok = hooks_.inspectorScrollToBottomAndVerify(detail);
                               appendStabilityRunLine("  " + detail);
                               const bool layoutOk = verifyAndCapture("low-window-scrolled", failReason);
                               hooks_.setMainWindowSize(inspectorWindowBoundsAtStart_.getWidth(), inspectorWindowBoundsAtStart_.getHeight());
                               if (!layoutOk)
                               {
                                   return false;
                               }
                               if (!ok)
                               {
                                   failReason = "Inspector content is not fully reachable by scrolling: " + detail;
                                   return false;
                               }
                               return true;
                           },
                           600 });
    steps_.push_back(Step{ "inspector-panel: window restored",
                           [this, verifyAndCapture](juce::String& failReason) -> bool {
                               appendStabilityRunLine("  main window now " + hooks_.getMainWindowBounds().toString());
                               return verifyAndCapture("restored", failReason);
                           },
                           kSettleDefaultMs });
}

// -----------------------------------------------------------------------------
// Live MIDI input: monitoring, routing, a recorded take, persistence, export, undo, guards
// -----------------------------------------------------------------------------
void StabilityScenarioRunner::appendLiveMidiSteps(const juce::File& project)
{
    if (hooks_.liveMidiFixtureSetup == nullptr || hooks_.liveMidiInject == nullptr || hooks_.liveMidiSetMonitor == nullptr
        || hooks_.liveMidiSetArm == nullptr || hooks_.liveMidiCapturedNoteCount == nullptr || hooks_.liveMidiCaptureReset == nullptr
        || hooks_.recordToggleLikeKey == nullptr || hooks_.isCountInActive == nullptr || hooks_.isRecordingInProgress == nullptr
        || hooks_.getTransportPlayheadSamples == nullptr || hooks_.liveMidiSummarizeClips == nullptr || hooks_.undoStackSize == nullptr
        || hooks_.invokeUndo == nullptr || hooks_.invokeRedo == nullptr || hooks_.isProjectDirty == nullptr
        || hooks_.loadProjectFromFile == nullptr || hooks_.saveProject == nullptr)
    {
        steps_.push_back(Step{ "live-midi: hooks missing",
                               [](juce::String& failReason) -> bool {
                                   failReason = "live-midi hooks not installed";
                                   return false;
                               },
                               0 });
        return;
    }

    // Sibling copy: the user's project is never modified; relative media paths keep resolving.
    steps_.push_back(Step{ "live-midi: copy project to sibling test file",
                           [this, project](juce::String& failReason) -> bool {
                               const juce::File copy = project.getSiblingFile(project.getFileNameWithoutExtension() + "-livemiditest.dalproj");
                               (void)copy.deleteFile();
                               if (!project.copyFileTo(copy))
                               {
                                   failReason = "could not copy project to " + copy.getFullPathName();
                                   return false;
                               }
                               openSaveCloseCopy_ = copy;
                               appendStabilityRunLine("  test copy: " + copy.getFullPathName());
                               return true;
                           },
                           kSettleDefaultMs });
    steps_.push_back(Step{ "live-midi: load test copy",
                           [this](juce::String&) -> bool {
                               hooks_.loadProjectFromFile(openSaveCloseCopy_);
                               return true;
                           },
                           kSettleAfterLoadMs });
    steps_.push_back(Step{ "live-midi: MIDI devices present on this machine",
                           [this](juce::String&) -> bool {
                               if (hooks_.liveMidiDescribeDevices)
                               {
                                   appendStabilityRunLine("  " + hooks_.liveMidiDescribeDevices());
                               }
                               appendStabilityRunLine(juce::String("  injection path: ")
                                                      + (hooks_.liveMidiInjectUsesRealPort && hooks_.liveMidiInjectUsesRealPort()
                                                             ? "REAL loopback MIDI port (device callback + timestamps exercised)"
                                                             : "device-thread entry of the bus (no loopback port on this machine)"));
                               return true;
                           },
                           kSettleDefaultMs });
    steps_.push_back(Step{ "live-midi: build fixture (instrument shell + Lower / Pedal routed rows, MIDI Input = All, filters 1/5/6)",
                           [this](juce::String& failReason) -> bool {
                               if (!hooks_.liveMidiFixtureSetup(liveMidiInstTid_, liveMidiLowerTid_, liveMidiPedalTid_, failReason))
                               {
                                   return false;
                               }
                               appendStabilityRunLine("  fixture: inst=" + juce::String((juce::int64)liveMidiInstTid_) + " lower="
                                                      + juce::String((juce::int64)liveMidiLowerTid_) + " pedal="
                                                      + juce::String((juce::int64)liveMidiPedalTid_));
                               return true;
                           },
                           600 });
    const auto inject = [this](const juce::MidiMessage& m) { hooks_.liveMidiInject(m); };
    const auto on = [](const int ch, const int note, const int vel) {
        return juce::MidiMessage::noteOn(ch, note, (juce::uint8)vel);
    };
    const auto off = [](const int ch, const int note) { return juce::MidiMessage::noteOff(ch, note, (juce::uint8)0); };

    // ---- 0. The user's real control path (the 1.1.10 failure): another row is active, the user
    //         clicks R + Monitor on the instrument row, then picks "All MIDI inputs" in the
    //         Inspector and presses Record. The R / Monitor click must make THAT row the one the
    //         Inspector edits; the refusal (while no input is chosen) must name the row and the
    //         real reason; the pick must land on the clicked row and be published as a route.
    if (hooks_.clickHeaderCellLikeMouse && hooks_.inspectorChooseMidiInput && hooks_.getActiveTrackId
        && hooks_.describeTrackMidiInputFromSession && hooks_.describePublishedRouteForTrack && hooks_.lastRecordStartRefusal
        && hooks_.activateTrackLikeHeaderClick)
    {
        steps_.push_back(Step{ "user-flow: another row (Pedal) is active; click R and Monitor on the INSTRUMENT row's header cells",
                               [this](juce::String& failReason) -> bool {
                                   hooks_.activateTrackLikeHeaderClick(liveMidiPedalTid_);
                                   if (hooks_.getActiveTrackId() != liveMidiPedalTid_)
                                   {
                                       failReason = "could not make the Pedal row active";
                                       return false;
                                   }
                                   if (!hooks_.clickHeaderCellLikeMouse(liveMidiInstTid_, "arm")
                                       || !hooks_.clickHeaderCellLikeMouse(liveMidiInstTid_, "monitor"))
                                   {
                                       failReason = "R / Monitor cell on the instrument row could not be clicked (absent or disabled)";
                                       return false;
                                   }
                                   const TrackId active = hooks_.getActiveTrackId();
                                   appendStabilityRunLine("  after clicking R + Monitor on inst=" + juce::String((juce::int64)liveMidiInstTid_)
                                                          + ": active row=" + juce::String((juce::int64)active));
                                   if (active != liveMidiInstTid_)
                                   {
                                       failReason = "clicking R / Monitor did not make the instrument row the active (Inspector) row - a MIDI "
                                                    "Input chosen now would land on the Pedal row";
                                       return false;
                                   }
                                   return true;
                               },
                               400 });
        steps_.push_back(Step{ "user-flow: R + Monitor on, no MIDI Input yet -> Inspector says so; Record names the row and the reason",
                               [this](juce::String& failReason) -> bool {
                                   if (hooks_.inspectorMidiInputTexts)
                                   {
                                       const juce::String texts = hooks_.inspectorMidiInputTexts();
                                       appendStabilityRunLine("  inspector: " + texts);
                                       if (!texts.contains("input=\"None\"") || !texts.containsIgnoreCase("no MIDI Input"))
                                       {
                                           failReason = "Inspector does not explain that R/Monitor are on without a MIDI Input: " + texts;
                                           return false;
                                       }
                                   }
                                   hooks_.recordToggleLikeKey();
                                   juce::Thread::sleep(150);
                                   const juce::String refusal = hooks_.lastRecordStartRefusal();
                                   appendStabilityRunLine("  record refusal: " + refusal.replace("\n", " / "));
                                   if (hooks_.isCountInActive())
                                   {
                                       failReason = "Record started although the armed row has no MIDI Input";
                                       return false;
                                   }
                                   if (refusal.isEmpty() || !refusal.contains("LiveMidiDest") || !refusal.containsIgnoreCase("no MIDI Input")
                                       || refusal.startsWith("Arm a track"))
                                   {
                                       failReason = "the refusal must name the armed row and say that it has no MIDI Input (not 'arm a track')";
                                       return false;
                                   }
                                   return true;
                               },
                               300 });
        steps_.push_back(Step{ "user-flow: pick \"All MIDI inputs\" + Input Channel 1 in the Inspector -> lands on the instrument row, route published",
                               [this](juce::String& failReason) -> bool {
                                   if (!hooks_.inspectorChooseMidiInput("All MIDI inputs"))
                                   {
                                       failReason = "Inspector has no \"All MIDI inputs\" item";
                                       return false;
                                   }
                                   if (hooks_.inspectorChooseMidiInputChannel && !hooks_.inspectorChooseMidiInputChannel(1))
                                   {
                                       failReason = "Input Channel combo not enabled after choosing an input";
                                       return false;
                                   }
                                   juce::Thread::sleep(250); // the coordinator follows the session on its 30 Hz tick
                                   const juce::String instInput = hooks_.describeTrackMidiInputFromSession(liveMidiInstTid_);
                                   const juce::String pedalInput = hooks_.describeTrackMidiInputFromSession(liveMidiPedalTid_);
                                   const juce::String route = hooks_.describePublishedRouteForTrack(liveMidiInstTid_);
                                   appendStabilityRunLine("  session: inst=" + instInput + " pedal=" + pedalInput + "; published route (inst): " + route);
                                   if (!instInput.startsWith("all ch=1"))
                                   {
                                       failReason = "the Inspector pick did not reach the instrument row's session state: " + instInput;
                                       return false;
                                   }
                                   if (!pedalInput.startsWith("all ch=6"))
                                   {
                                       failReason = "the Pedal row's input changed although it was not the edited row: " + pedalInput;
                                       return false;
                                   }
                                   if (!route.contains("monitor=yes") || !route.contains("capture=yes") || !route.contains("slot=-1"))
                                   {
                                       failReason = "the runtime routing does not match the UI (monitor/capture/All): " + route;
                                       return false;
                                   }
                                   if (hooks_.inspectorMidiInputTexts)
                                   {
                                       const juce::String texts = hooks_.inspectorMidiInputTexts();
                                       appendStabilityRunLine("  inspector now: " + texts);
                                       if (!texts.contains("All MIDI inputs"))
                                       {
                                           failReason = "Inspector does not show the picked input";
                                           return false;
                                       }
                                       // No technical counter / readiness text in the UI (1.1.13): a working
                                       // input shows nothing but real obstacles.
                                       if (texts.contains("MIDI received") || texts.contains("Ready -") || texts.contains("events"))
                                       {
                                           failReason = "Inspector still shows the MIDI event counter / readiness text";
                                           return false;
                                       }
                                   }
                                   return true;
                               },
                               300 });
        steps_.push_back(Step{ "user-flow: Record now starts (count-in) - then cancel it; switch R + Monitor off again via the header cells",
                               [this](juce::String& failReason) -> bool {
                                   // The user-flow check is about the input, not about passes: run it as a
                                   // linear take (Cycle off) so the later steps' expectations stay simple.
                                   if (hooks_.setCycleEnabled)
                                   {
                                       hooks_.setCycleEnabled(false);
                                   }
                                   hooks_.recordToggleLikeKey();
                                   juce::Thread::sleep(150);
                                   const bool countIn = hooks_.isCountInActive();
                                   appendStabilityRunLine(juce::String("  count-in started: ") + (countIn ? "yes" : "no")
                                                          + (hooks_.lastRecordStartRefusal().isEmpty() ? "" : " refusal=" + hooks_.lastRecordStartRefusal().replace("\n", " / ")));
                                   if (!countIn)
                                   {
                                       failReason = "Record still refused after choosing All MIDI inputs on the armed row";
                                       return false;
                                   }
                                   hooks_.recordToggleLikeKey(); // Record during count-in = cancel
                                   juce::Thread::sleep(100);
                                   (void)hooks_.clickHeaderCellLikeMouse(liveMidiInstTid_, "arm");
                                   (void)hooks_.clickHeaderCellLikeMouse(liveMidiInstTid_, "monitor");
                                   return !hooks_.isCountInActive();
                               },
                               400 });
    }
    else
    {
        steps_.push_back(Step{ "live-midi: configure the instrument row's input (hooks for the UI path missing - direct session edit)",
                               [this](juce::String& failReason) -> bool {
                                   if (!hooks_.liveMidiSetTrackInputDevice || !hooks_.liveMidiSetTrackInputDevice(liveMidiInstTid_, {}, {}))
                                   {
                                       failReason = "could not configure the instrument row";
                                       return false;
                                   }
                                   return true;
                               },
                               300 });
    }

    // ---- 0b. Audible check when this project has a LOADED instrument: live MIDI through the
    //          Inspector + header path must produce AUDIO on that row (not just a delivered event).
    if (hooks_.firstLoadedInstrumentRow && hooks_.setMeteredTrack && hooks_.drainTrackMeter && hooks_.clickHeaderCellLikeMouse
        && hooks_.inspectorChooseMidiInput && hooks_.activateTrackLikeHeaderClick)
    {
        auto loaded = std::make_shared<TrackId>(kInvalidTrackId);
        steps_.push_back(Step{ "audible: find a row with a loaded instrument, give it All MIDI inputs via the Inspector, Monitor on via the header",
                               [this, loaded](juce::String& failReason) -> bool {
                                   *loaded = hooks_.firstLoadedInstrumentRow();
                                   if (*loaded == kInvalidTrackId)
                                   {
                                       appendStabilityRunLine("  no loaded instrument in this project - audible check skipped (MIDI delivery only)");
                                       return true;
                                   }
                                   hooks_.activateTrackLikeHeaderClick(*loaded);
                                   if (!hooks_.inspectorChooseMidiInput("All MIDI inputs"))
                                   {
                                       failReason = "Inspector pick failed on the loaded instrument row";
                                       return false;
                                   }
                                   if (!hooks_.clickHeaderCellLikeMouse(*loaded, "monitor"))
                                   {
                                       failReason = "Monitor cell could not be clicked on the loaded instrument row";
                                       return false;
                                   }
                                   hooks_.setMeteredTrack(*loaded);
                                   (void)hooks_.drainTrackMeter();
                                   appendStabilityRunLine("  loaded instrument row " + juce::String((juce::int64)*loaded) + ": "
                                                          + (hooks_.describeTrackForDiagnostics ? hooks_.describeTrackForDiagnostics(*loaded) : juce::String()));
                                   return true;
                               },
                               400 });
        steps_.push_back(Step{ "audible: silence before the key press (post-strip meter of the row)",
                               [this, loaded](juce::String&) -> bool {
                                   if (*loaded == kInvalidTrackId)
                                   {
                                       return true;
                                   }
                                   const StabilityLevelStats s = hooks_.drainTrackMeter();
                                   appendStabilityRunLine("  before: frames=" + juce::String((juce::int64)s.frames) + " peak=[" + juce::String(s.peak[0], 4) + ","
                                                          + juce::String(s.peak[1], 4) + "]");
                                   return true;
                               },
                               600 });
        steps_.push_back(Step{ "audible: play ch1 note 60 live (transport stopped) - the row's audio output must rise",
                               [this, loaded, inject, on, off](juce::String& failReason) -> bool {
                                   if (*loaded == kInvalidTrackId)
                                   {
                                       return true;
                                   }
                                   (void)hooks_.drainTrackMeter();
                                   inject(on(1, 60, 110));
                                   inject(on(1, 38, 110)); // a GM snare too, in case the row hosts a drum kit
                                   juce::Thread::sleep(700);
                                   const StabilityLevelStats s = hooks_.drainTrackMeter();
                                   inject(off(1, 60));
                                   inject(off(1, 38));
                                   const float peak = juce::jmax(s.peak[0], s.peak[1]);
                                   appendStabilityRunLine("  during live note: frames=" + juce::String((juce::int64)s.frames) + " peak=[" + juce::String(s.peak[0], 4)
                                                          + "," + juce::String(s.peak[1], 4) + "] (" + dbfs(peak) + " dBFS)");
                                   if (s.frames == 0)
                                   {
                                       failReason = "the loaded instrument row produced no blocks while stopped";
                                       return false;
                                   }
                                   if (peak < 1.0e-3f)
                                   {
                                       failReason = "live MIDI reached the row but no audio came out of the instrument (peak " + dbfs(peak) + " dBFS)";
                                       return false;
                                   }
                                   return true;
                               },
                               300 });
        steps_.push_back(Step{ "audible: Monitor off via the header, MIDI Input back to None via the Inspector, release the meter",
                               [this, loaded](juce::String&) -> bool {
                                   if (*loaded == kInvalidTrackId)
                                   {
                                       return true;
                                   }
                                   (void)hooks_.clickHeaderCellLikeMouse(*loaded, "monitor");
                                   hooks_.activateTrackLikeHeaderClick(*loaded);
                                   (void)hooks_.inspectorChooseMidiInput("None");
                                   hooks_.setMeteredTrack(kInvalidTrackId);
                                   return true;
                               },
                               300 });
    }

    steps_.push_back(Step{ "live-midi: save fixture (direct save to the test copy) and clear the dirty flag",
                           [this](juce::String&) -> bool {
                               hooks_.saveProject();
                               return true;
                           },
                           600 });

    // ---- 1. Monitor on, transport stopped: live MIDI reaches the destination; project not dirty.
    steps_.push_back(Step{ "live-midi: Inspector shows MIDI Input / Input Channel for the instrument row",
                           [this](juce::String& failReason) -> bool {
                               if (hooks_.selectTrackLikeHeaderClick)
                               {
                                   hooks_.selectTrackLikeHeaderClick(liveMidiInstTid_);
                               }
                               if (hooks_.inspectorMidiInputTexts)
                               {
                                   const juce::String texts = hooks_.inspectorMidiInputTexts();
                                   appendStabilityRunLine("  inspector: " + texts);
                                   if (!texts.contains("All MIDI inputs") || !texts.contains("channel=1"))
                                   {
                                       failReason = "Inspector does not show the fixture's MIDI Input (All) / channel 1: " + texts;
                                       return false;
                                   }
                               }
                               if (hooks_.verifyLiveMidiHeaderCells)
                               {
                                   juce::String report;
                                   for (const TrackId tid : { liveMidiInstTid_, liveMidiLowerTid_ })
                                   {
                                       if (!hooks_.verifyLiveMidiHeaderCells(tid, report, failReason))
                                       {
                                           return false;
                                       }
                                       appendStabilityRunLine("  header: " + report);
                                   }
                               }
                               return true;
                           },
                           400 });
    steps_.push_back(Step{ "live-midi: Monitor ON (instrument row), transport stopped, play ch1 note 60",
                           [this, inject, on](juce::String& failReason) -> bool {
                               if (hooks_.isProjectDirty())
                               {
                                   failReason = "project dirty before monitoring started";
                                   return false;
                               }
                               hooks_.liveMidiCaptureReset();
                               hooks_.liveMidiSetMonitor(liveMidiInstTid_, true);
                               inject(on(1, 60, 100));
                               return true;
                           },
                           400 });
    steps_.push_back(Step{ "live-midi: note reached the destination host on channel 1; project still clean",
                           [this, inject, off](juce::String& failReason) -> bool {
                               const int n = hooks_.liveMidiCapturedNoteCount(1, 60, true);
                               appendStabilityRunLine("  captured ch1 note60 on=" + juce::String(n));
                               if (n != 1)
                               {
                                   failReason = "expected exactly one ch1 note-on at the destination while stopped, got " + juce::String(n);
                                   return false;
                               }
                               if (hooks_.isProjectDirty())
                               {
                                   failReason = "Monitor on + live playing marked the project dirty";
                                   return false;
                               }
                               inject(off(1, 60));
                               return true;
                           },
                           400 });
    steps_.push_back(Step{ "live-midi: filter: ch5 / ch6 are NOT forwarded by the instrument row (filter 1)",
                           [this, inject, on, off](juce::String& failReason) -> bool {
                               if (hooks_.liveMidiCapturedNoteCount(1, 60, false) != 1)
                               {
                                   failReason = "note-off did not reach the destination";
                                   return false;
                               }
                               hooks_.liveMidiCaptureReset();
                               inject(on(5, 62, 90));
                               inject(on(6, 64, 80));
                               juce::Thread::sleep(250);
                               const int c5 = hooks_.liveMidiCapturedNoteCount(5, 62, true) + hooks_.liveMidiCapturedNoteCount(2, 62, true);
                               const int c6 = hooks_.liveMidiCapturedNoteCount(6, 64, true) + hooks_.liveMidiCapturedNoteCount(3, 64, true);
                               appendStabilityRunLine("  with only the instrument row monitoring (filter 1): ch5->" + juce::String(c5)
                                                      + " ch6->" + juce::String(c6) + " (both expected 0)");
                               inject(off(5, 62));
                               inject(off(6, 64));
                               if (c5 != 0 || c6 != 0)
                               {
                                   failReason = "input channel filter leaked events";
                                   return false;
                               }
                               return true;
                           },
                           300 });
    // ---- 2. Routed rows: Lower (filter 5 -> Force 2), Pedal (filter 6 -> Force 3) to the SAME host.
    steps_.push_back(Step{ "live-midi: Monitor ON Lower + Pedal, play ch5 and ch6 -> arrive as ch2 / ch3 on the same host",
                           [this, inject, on](juce::String&) -> bool {
                               hooks_.liveMidiCaptureReset();
                               hooks_.liveMidiSetMonitor(liveMidiLowerTid_, true);
                               hooks_.liveMidiSetMonitor(liveMidiPedalTid_, true);
                               inject(on(5, 62, 90));
                               inject(on(6, 64, 80));
                               inject(on(1, 65, 70));
                               return true;
                           },
                           400 });
    steps_.push_back(Step{ "live-midi: per-channel counts 1/1/1 (Upper ch1, Lower ch2, Pedal ch3), native channels never leak",
                           [this](juce::String& failReason) -> bool {
                               const int ch1 = hooks_.liveMidiCapturedNoteCount(1, 65, true);
                               const int ch2 = hooks_.liveMidiCapturedNoteCount(2, 62, true);
                               const int ch3 = hooks_.liveMidiCapturedNoteCount(3, 64, true);
                               const int leak = hooks_.liveMidiCapturedNoteCount(5, 62, true) + hooks_.liveMidiCapturedNoteCount(6, 64, true);
                               appendStabilityRunLine("  captured: ch1=" + juce::String(ch1) + " ch2=" + juce::String(ch2) + " ch3="
                                                      + juce::String(ch3) + " leaked native ch5/6=" + juce::String(leak));
                               if (ch1 != 1 || ch2 != 1 || ch3 != 1 || leak != 0)
                               {
                                   failReason = "Force mapping / routing of live MIDI is wrong (expected 1/1/1, no leak)";
                                   return false;
                               }
                               return true;
                           },
                           kSettleDefaultMs });
    steps_.push_back(Step{ "live-midi: Monitor OFF on Lower while its note is held -> targeted note-off on ch2 only",
                           [this](juce::String&) -> bool {
                               hooks_.liveMidiCaptureReset();
                               hooks_.liveMidiSetMonitor(liveMidiLowerTid_, false);
                               return true;
                           },
                           400 });
    steps_.push_back(Step{ "live-midi: release check (ch2 off = 1, ch1/ch3 still sounding)",
                           [this, inject, off](juce::String& failReason) -> bool {
                               const int off2 = hooks_.liveMidiCapturedNoteCount(2, 62, false);
                               const int off1 = hooks_.liveMidiCapturedNoteCount(1, 65, false);
                               const int off3 = hooks_.liveMidiCapturedNoteCount(3, 64, false);
                               appendStabilityRunLine("  after Monitor off (Lower): off ch2=" + juce::String(off2) + " ch1=" + juce::String(off1)
                                                      + " ch3=" + juce::String(off3));
                               inject(off(1, 65));
                               inject(off(6, 64));
                               if (off2 != 1 || off1 != 0 || off3 != 0)
                               {
                                   failReason = "Monitor off must release exactly the row's own live notes";
                                   return false;
                               }
                               return true;
                           },
                           300 });
    steps_.push_back(Step{ "live-midi: all Monitors off; nothing dirty so far",
                           [this](juce::String& failReason) -> bool {
                               hooks_.liveMidiSetMonitor(liveMidiInstTid_, false);
                               hooks_.liveMidiSetMonitor(liveMidiPedalTid_, false);
                               if (hooks_.isProjectDirty())
                               {
                                   failReason = "monitoring alone made the project dirty";
                                   return false;
                               }
                               return true;
                           },
                           300 });

    // ---- 2b. Real device path: when a physical MIDI input exists, assign it (Device mode) and
    //          prove the device is opened on the device manager with the slot callback registered,
    //          that a message entering that slot still reaches the host, and that "None" closes it.
    if (hooks_.liveMidiFirstRealInputDevice && hooks_.liveMidiSetTrackInputDevice && hooks_.liveMidiIsDeviceOpen)
    {
        auto devId = std::make_shared<juce::String>();
        auto devName = std::make_shared<juce::String>();
        steps_.push_back(Step{ "live-midi: real device - pick the first physical MIDI input for the instrument row in the Inspector (Device mode)",
                               [this, devId, devName](juce::String& failReason) -> bool {
                                   if (!hooks_.liveMidiFirstRealInputDevice(*devId, *devName))
                                   {
                                       appendStabilityRunLine("  no physical MIDI input on this machine - device-open check skipped");
                                       return true;
                                   }
                                   // The user's path: activate the row, pick the device by its name in the combo.
                                   if (hooks_.activateTrackLikeHeaderClick && hooks_.inspectorChooseMidiInput)
                                   {
                                       hooks_.activateTrackLikeHeaderClick(liveMidiInstTid_);
                                       if (!hooks_.inspectorChooseMidiInput(*devName))
                                       {
                                           failReason = "the Inspector lists no item for device \"" + *devName + "\"";
                                           return false;
                                       }
                                   }
                                   else if (!hooks_.liveMidiSetTrackInputDevice(liveMidiInstTid_, *devId, *devName))
                                   {
                                       failReason = "could not assign device \"" + *devName + "\"";
                                       return false;
                                   }
                                   if (hooks_.describeTrackMidiInputFromSession)
                                   {
                                       const juce::String s = hooks_.describeTrackMidiInputFromSession(liveMidiInstTid_);
                                       appendStabilityRunLine("  session after the pick: " + s);
                                       if (!s.startsWith("device:" + *devName))
                                       {
                                           failReason = "the device pick did not reach the session: " + s;
                                           return false;
                                       }
                                   }
                                   return true;
                               },
                               500 });
        steps_.push_back(Step{ "live-midi: missing device - a saved device that is not connected is explained and kept; Record refuses with the reason",
                               [this, devId](juce::String& failReason) -> bool {
                                   if (devId->isEmpty() || !hooks_.liveMidiSetTrackInputDevice)
                                   {
                                       return true;
                                   }
                                   // Simulate a project saved on another machine: an identifier no device here has.
                                   if (!hooks_.liveMidiSetTrackInputDevice(liveMidiInstTid_, "dal-test-missing-device-id", "Ghost Keyboard"))
                                   {
                                       failReason = "could not assign the ghost device";
                                       return false;
                                   }
                                   juce::String texts;
                                   if (hooks_.selectTrackLikeHeaderClick && hooks_.inspectorMidiInputTexts)
                                   {
                                       hooks_.selectTrackLikeHeaderClick(liveMidiInstTid_);
                                       texts = hooks_.inspectorMidiInputTexts();
                                       appendStabilityRunLine("  inspector with a missing device: " + texts);
                                       if (!texts.contains("Ghost Keyboard (missing)") || !texts.contains("not connected"))
                                       {
                                           failReason = "Inspector does not show the missing device as missing + kept";
                                           return false;
                                       }
                                   }
                                   hooks_.liveMidiSetArm(liveMidiInstTid_, true);
                                   if (hooks_.setCycleEnabled)
                                   {
                                       hooks_.setCycleEnabled(false);
                                   }
                                   hooks_.recordToggleLikeKey();
                                   juce::Thread::sleep(150);
                                   const juce::String refusal = hooks_.lastRecordStartRefusal ? hooks_.lastRecordStartRefusal() : juce::String();
                                   appendStabilityRunLine("  record refusal: " + refusal.replace("\n", " / "));
                                   const bool countIn = hooks_.isCountInActive();
                                   if (countIn)
                                   {
                                       hooks_.recordToggleLikeKey();
                                   }
                                   hooks_.liveMidiSetArm(liveMidiInstTid_, false);
                                   if (countIn || !refusal.contains("Ghost Keyboard") || !refusal.contains("not connected"))
                                   {
                                       failReason = "Record must refuse and name the missing device";
                                       return false;
                                   }
                                   const juce::String s = hooks_.describeTrackMidiInputFromSession ? hooks_.describeTrackMidiInputFromSession(liveMidiInstTid_) : juce::String();
                                   if (!s.startsWith("device:Ghost Keyboard"))
                                   {
                                       failReason = "the missing device assignment was not kept: " + s;
                                       return false;
                                   }
                                   return true;
                               },
                               300 });
        steps_.push_back(Step{ "live-midi: real device - re-pick the physical device in the Inspector (back from the ghost)",
                               [this, devId, devName](juce::String& failReason) -> bool {
                                   if (devId->isEmpty())
                                   {
                                       return true;
                                   }
                                   if (hooks_.activateTrackLikeHeaderClick && hooks_.inspectorChooseMidiInput)
                                   {
                                       hooks_.activateTrackLikeHeaderClick(liveMidiInstTid_);
                                       if (!hooks_.inspectorChooseMidiInput(*devName))
                                       {
                                           failReason = "could not re-pick \"" + *devName + "\"";
                                           return false;
                                       }
                                   }
                                   else if (!hooks_.liveMidiSetTrackInputDevice(liveMidiInstTid_, *devId, *devName))
                                   {
                                       failReason = "could not re-assign the device";
                                       return false;
                                   }
                                   return true;
                               },
                               400 });
        steps_.push_back(Step{ "live-midi: real device - opened on the device manager, slot callback registered, Inspector shows it",
                               [this, devId, devName, inject, on, off](juce::String& failReason) -> bool {
                                   if (devId->isEmpty())
                                   {
                                       return true;
                                   }
                                   juce::String detail;
                                   const bool open = hooks_.liveMidiIsDeviceOpen(*devId, detail);
                                   appendStabilityRunLine("  device \"" + *devName + "\": " + detail);
                                   if (!open)
                                   {
                                       // The port is held by another application (verified by running
                                       // `LiveMidiRecordingFocusedTests --hold-midi-input <name> <s>` alongside):
                                       // the UI must say so and Record must refuse with that reason.
                                       juce::String texts;
                                       if (hooks_.selectTrackLikeHeaderClick && hooks_.inspectorMidiInputTexts)
                                       {
                                           hooks_.selectTrackLikeHeaderClick(liveMidiInstTid_);
                                           texts = hooks_.inspectorMidiInputTexts();
                                           appendStabilityRunLine("  inspector (busy port): " + texts);
                                       }
                                       hooks_.liveMidiSetArm(liveMidiInstTid_, true);
                                       if (hooks_.setCycleEnabled)
                                       {
                                           hooks_.setCycleEnabled(false);
                                       }
                                       hooks_.recordToggleLikeKey();
                                       juce::Thread::sleep(150);
                                       const juce::String refusal = hooks_.lastRecordStartRefusal ? hooks_.lastRecordStartRefusal() : juce::String();
                                       appendStabilityRunLine("  record refusal (busy port): " + refusal.replace("\n", " / "));
                                       if (hooks_.isCountInActive())
                                       {
                                           hooks_.recordToggleLikeKey();
                                       }
                                       hooks_.liveMidiSetArm(liveMidiInstTid_, false);
                                       if (!texts.contains("could not be opened") || !refusal.contains("could not be opened"))
                                       {
                                           failReason = "physical MIDI input could not be opened and the UI / Record did not explain it: " + detail;
                                           return false;
                                       }
                                       appendStabilityRunLine("  BUSY-PORT PATH VERIFIED: device held elsewhere, explanation shown, Record refused with the reason; "
                                                              "delivery checks skipped for this run");
                                       return true;
                                   }
                                   if (hooks_.selectTrackLikeHeaderClick && hooks_.inspectorMidiInputTexts)
                                   {
                                       hooks_.selectTrackLikeHeaderClick(liveMidiInstTid_);
                                       const juce::String texts = hooks_.inspectorMidiInputTexts();
                                       appendStabilityRunLine("  inspector: " + texts);
                                       if (!texts.contains(*devName))
                                       {
                                           failReason = "Inspector does not show the assigned device name";
                                           return false;
                                       }
                                       if (hooks_.captureInspectorPng)
                                       {
                                           const juce::File dir = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("dal-stability-live-midi");
                                           (void)dir.createDirectory();
                                           (void)hooks_.captureInspectorPng(dir.getChildFile("live-midi-inspector-device.png"));
                                       }
                                   }
                                   // A message entering the device's slot (slot 0 = first enumerated device) is
                                   // routed by the Device-mode row.
                                   hooks_.liveMidiCaptureReset();
                                   hooks_.liveMidiSetMonitor(liveMidiInstTid_, true);
                                   inject(on(1, 61, 100));
                                   juce::Thread::sleep(250);
                                   const int n = hooks_.liveMidiCapturedNoteCount(1, 61, true);
                                   inject(off(1, 61));
                                   juce::Thread::sleep(150);
                                   hooks_.liveMidiSetMonitor(liveMidiInstTid_, false);
                                   appendStabilityRunLine("  Device-mode route delivered ch1 note61 on=" + juce::String(n));
                                   if (n != 1)
                                   {
                                       failReason = "Device-mode route did not deliver the message entering its slot";
                                       return false;
                                   }
                                   return true;
                               },
                               300 });
        steps_.push_back(Step{ "live-midi: real device - back to All MIDI inputs (device stays open: still referenced by All)",
                               [this, devId, devName](juce::String& failReason) -> bool {
                                   if (devId->isEmpty())
                                   {
                                       return true;
                                   }
                                   if (!hooks_.liveMidiSetTrackInputDevice(liveMidiInstTid_, {}, {}))
                                   {
                                       failReason = "could not restore All MIDI inputs";
                                       return false;
                                   }
                                   juce::String detail;
                                   const bool open = hooks_.liveMidiIsDeviceOpen(*devId, detail);
                                   appendStabilityRunLine("  after restoring All: " + detail);
                                   if (!open)
                                   {
                                       failReason = "device closed although rows still use All MIDI inputs";
                                       return false;
                                   }
                                   return true;
                               },
                               400 });
    }

    // ---- 3. Cycle: Record with armed MIDI rows and Cycle on STARTS (cycle takes are passes — the
    //         full behaviour is covered by `--stability-midi-cycle-takes`); Cycle is left untouched.
    if (hooks_.setCycleEnabled && hooks_.isCycleEnabled)
    {
        steps_.push_back(Step{ "live-midi: Cycle ON + armed MIDI row + Record -> count-in starts (cycle recording), Cycle unchanged; cancel",
                               [this](juce::String& failReason) -> bool {
                                   hooks_.liveMidiSetArm(liveMidiInstTid_, true);
                                   hooks_.setCycleEnabled(true);
                                   hooks_.recordToggleLikeKey();
                                   juce::Thread::sleep(150);
                                   const bool countIn = hooks_.isCountInActive();
                                   const bool cycleStill = hooks_.isCycleEnabled();
                                   appendStabilityRunLine(juce::String("  countIn=") + (countIn ? "yes" : "no") + " cycleStillOn="
                                                          + (cycleStill ? "yes" : "no")
                                                          + (hooks_.lastRecordStartRefusal && hooks_.lastRecordStartRefusal().isNotEmpty()
                                                                 ? " refusal=" + hooks_.lastRecordStartRefusal().replace("\n", " / ")
                                                                 : juce::String()));
                                   if (countIn)
                                   {
                                       hooks_.recordToggleLikeKey(); // cancel the count-in
                                       juce::Thread::sleep(100);
                                   }
                                   hooks_.setCycleEnabled(false);
                                   if (!countIn)
                                   {
                                       failReason = "Record refused with Cycle on although MIDI cycle recording is supported";
                                       return false;
                                   }
                                   if (!cycleStill)
                                   {
                                       failReason = "Cycle was switched off automatically";
                                       return false;
                                   }
                                   return hooks_.isCountInActive() == false;
                               },
                               400 });
    }

    // ---- 4. The take: instrument (filter 1) + Lower (filter 5) armed, Monitor on instrument only.
    steps_.push_back(Step{ "live-midi: arm instrument + Lower, Monitor ON instrument only, press Record (count-in starts)",
                           [this](juce::String& failReason) -> bool {
                               liveMidiUndoSizeBeforeTake_ = hooks_.undoStackSize();
                               hooks_.liveMidiSetArm(liveMidiInstTid_, true);
                               hooks_.liveMidiSetArm(liveMidiLowerTid_, true);
                               hooks_.liveMidiSetMonitor(liveMidiInstTid_, true);
                               hooks_.liveMidiCaptureReset();
                               hooks_.recordToggleLikeKey();
                               juce::Thread::sleep(100);
                               if (!hooks_.isCountInActive())
                               {
                                   failReason = "Record did not start the count-in for a MIDI-only take";
                                   return false;
                               }
                               return true;
                           },
                           200 });
    steps_.push_back(Step{ "live-midi: during count-in: hold ch1 note 48 (must enter the take at tick 0); Lower monitored off still records",
                           [this, inject, on](juce::String&) -> bool {
                               inject(on(1, 48, 77));
                               return true;
                           },
                           3700 }); // count-in = 8 × 375 ms + 375 ms
    steps_.push_back(Step{ "live-midi: recording started (transport playing)",
                           [this](juce::String& failReason) -> bool {
                               if (!hooks_.isRecordingInProgress() || hooks_.isCountInActive())
                               {
                                   failReason = "take did not start after the count-in";
                                   return false;
                               }
                               liveMidiTakeStart_ = hooks_.liveMidiTakeStartSample ? hooks_.liveMidiTakeStartSample() : 0;
                               appendStabilityRunLine("  take start sample " + juce::String((juce::int64)liveMidiTakeStart_)
                                                      + " playhead now " + juce::String((juce::int64)hooks_.getTransportPlayheadSamples()));
                               return true;
                           },
                           500 });
    steps_.push_back(Step{ "live-midi: +0.5 s ch1 note 60 on (vel 100), ch5 note 62 on (vel 90), sustain down, expression 90, pitch bend 12000",
                           [this, inject, on](juce::String&) -> bool {
                               if (hooks_.captureArrangementPng)
                               {
                                   const juce::File dir = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("dal-stability-live-midi");
                                   (void)dir.createDirectory();
                                   (void)hooks_.captureArrangementPng(dir.getChildFile("live-midi-recording.png"));
                               }
                               liveMidiNoteOnPlayhead_ = hooks_.getTransportPlayheadSamples();
                               inject(on(1, 60, 100));
                               liveMidiLowerOnPlayhead_ = liveMidiNoteOnPlayhead_;
                               inject(on(5, 62, 90));
                               inject(juce::MidiMessage::controllerEvent(1, 64, 127));
                               inject(juce::MidiMessage::controllerEvent(1, 11, 90));
                               inject(juce::MidiMessage::pitchWheel(1, 12000));
                               return true;
                           },
                           700 });
    steps_.push_back(Step{ "live-midi: +1.2 s release ch1 60 (vel-0 shorthand) and ch5 62, release note 48; ch1 note 72 stays held past stop",
                           [this, inject, on, off](juce::String&) -> bool {
                               liveMidiNoteOffPlayhead_ = hooks_.getTransportPlayheadSamples();
                               inject(on(1, 60, 0));
                               liveMidiLowerOffPlayhead_ = liveMidiNoteOffPlayhead_;
                               inject(off(5, 62));
                               inject(off(1, 48));
                               inject(on(1, 72, 64));
                               return true;
                           },
                           700 });
    steps_.push_back(Step{ "live-midi: monitored events were heard live (ch1 60 on at the destination during the take)",
                           [this](juce::String& failReason) -> bool {
                               if (hooks_.captureArrangementPng)
                               {
                                   const juce::File dir = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("dal-stability-live-midi");
                                   (void)dir.createDirectory();
                                   (void)hooks_.captureArrangementPng(dir.getChildFile("live-midi-recording-growing.png"));
                               }
                               const int heard = hooks_.liveMidiCapturedNoteCount(1, 60, true);
                               const int lowerHeard = hooks_.liveMidiCapturedNoteCount(2, 62, true);
                               appendStabilityRunLine("  live during take: ch1 60 on=" + juce::String(heard) + " Lower(ch2) 62 on="
                                                      + juce::String(lowerHeard) + " (Lower Monitor OFF -> 0 expected, still recorded)");
                               if (heard != 1)
                               {
                                   failReason = "monitored row did not hear its live note during recording";
                                   return false;
                               }
                               if (lowerHeard != 0)
                               {
                                   failReason = "a row with Monitor OFF must not be heard live";
                                   return false;
                               }
                               return true;
                           },
                           100 });
    steps_.push_back(Step{ "live-midi: Stop (same path as the Stop button) -> take committed",
                           [this](juce::String& failReason) -> bool {
                               liveMidiStopPlayhead_ = hooks_.getTransportPlayheadSamples();
                               hooks_.recordToggleLikeKey(); // Record key while recording = stop + commit
                               juce::Thread::sleep(150);
                               if (hooks_.isRecordingInProgress())
                               {
                                   failReason = "take still active after stop";
                                   return false;
                               }
                               return true;
                           },
                           600 });
    steps_.push_back(Step{ "live-midi: release the key still held after stop (must not re-enter any clip)",
                           [this, inject, off](juce::String&) -> bool {
                               inject(off(1, 72));
                               return true;
                           },
                           300 });
    steps_.push_back(Step{ "live-midi: verify recorded clips (instrument row: notes 48 / 60 / 72, CC64 / CC11, pitch bend; Lower row: note 62 ch5)",
                           [this](juce::String& failReason) -> bool {
                               const StabilityMidiClipSummary inst = hooks_.liveMidiSummarizeClips(liveMidiInstTid_);
                               const StabilityMidiClipSummary lower = hooks_.liveMidiSummarizeClips(liveMidiLowerTid_);
                               const StabilityMidiClipSummary pedal = hooks_.liveMidiSummarizeClips(liveMidiPedalTid_);
                               const auto describe = [](const char* who, const StabilityMidiClipSummary& s) {
                                   juce::String line = juce::String(who) + ": clips=" + juce::String(s.clipCount);
                                   if (s.clipCount > 0)
                                   {
                                       line << " start=" << juce::String((juce::int64)s.firstClipStartSamples) << " len="
                                            << juce::String((juce::int64)s.firstClipLengthSamples) << " bpm=" << juce::String(s.bpm, 2)
                                            << " tpq=" << juce::String(s.ticksPerQuarter) << " notes=[";
                                       for (const auto& n : s.notes)
                                       {
                                           line << "(n" << n.note << " v" << n.velocity << " ch" << n.channel << " @" << juce::String((juce::int64)n.startTick)
                                                << " d" << juce::String((juce::int64)n.durationTicks) << ")";
                                       }
                                       line << "] cc=[";
                                       for (const auto& c : s.cc)
                                       {
                                           line << "(cc" << c.controller << "=" << c.value << " ch" << c.channel << " @" << juce::String((juce::int64)c.tick) << ")";
                                       }
                                       line << "] pb=[";
                                       for (const auto& b : s.pitchBend)
                                       {
                                           line << "(" << b.value << " ch" << b.channel << " @" << juce::String((juce::int64)b.tick) << ")";
                                       }
                                       line << "]";
                                   }
                                   return line;
                               };
                               appendStabilityRunLine("  " + describe("instrument", inst));
                               appendStabilityRunLine("  " + describe("lower", lower));
                               appendStabilityRunLine("  " + describe("pedal", pedal));
                               if (inst.clipCount != 1 || lower.clipCount != 1 || pedal.clipCount != 0)
                               {
                                   failReason = "expected one clip on the instrument row and on Lower, none on Pedal (not armed)";
                                   return false;
                               }
                               if (inst.firstClipStartSamples != liveMidiTakeStart_)
                               {
                                   failReason = "instrument clip does not start at the record boundary";
                                   return false;
                               }
                               // Expected ticks from the playhead stamps taken at injection (±tolerance: the UI
                               // playhead is a per-block staircase and the device/callback clocks jitter by < 2 blocks).
                               const double sr = 48000.0;
                               const std::int64_t latency = hooks_.reportedOutputLatencySamples ? hooks_.reportedOutputLatencySamples() : 0;
                               const auto tickOf = [&inst, sr, latency, this](const std::int64_t playhead) {
                                   const std::int64_t rel = juce::jmax<std::int64_t>(0, playhead - liveMidiTakeStart_ - latency);
                                   return relativeSamplesToTicks(rel, inst.bpm, inst.ticksPerQuarter, sr);
                               };
                               const std::int64_t tolTicks = relativeSamplesToTicks(3 * 1024 + 2400, inst.bpm, inst.ticksPerQuarter, sr);
                               const std::int64_t stopTick = relativeSamplesToTicks(inst.firstClipLengthSamples, inst.bpm, inst.ticksPerQuarter, sr);
                               bool n48 = false, n60 = false, n72 = false;
                               for (const auto& n : inst.notes)
                               {
                                   if (n.note == 48 && n.channel == 1 && n.velocity == 77 && n.startTick == 0
                                       && std::llabs(n.durationTicks - tickOf(liveMidiNoteOffPlayhead_)) <= tolTicks)
                                   {
                                       n48 = true;
                                   }
                                   if (n.note == 60 && n.channel == 1 && n.velocity == 100
                                       && std::llabs(n.startTick - tickOf(liveMidiNoteOnPlayhead_)) <= tolTicks
                                       && std::llabs((n.startTick + n.durationTicks) - tickOf(liveMidiNoteOffPlayhead_)) <= tolTicks)
                                   {
                                       n60 = true;
                                   }
                                   if (n.note == 72 && n.channel == 1 && n.velocity == 64 && n.startTick + n.durationTicks == stopTick)
                                   {
                                       n72 = true;
                                   }
                               }
                               appendStabilityRunLine("  expected ticks: on=" + juce::String((juce::int64)tickOf(liveMidiNoteOnPlayhead_)) + " off="
                                                      + juce::String((juce::int64)tickOf(liveMidiNoteOffPlayhead_)) + " stop=" + juce::String((juce::int64)stopTick)
                                                      + " tolerance=" + juce::String((juce::int64)tolTicks) + " outputLatency=" + juce::String((juce::int64)latency));
                               if (!n48 || !n60 || !n72 || inst.notes.size() != 3)
                               {
                                   failReason = juce::String("instrument take notes wrong: held-at-start=") + (n48 ? "ok" : "BAD")
                                                + " timed-note=" + (n60 ? "ok" : "BAD") + " held-at-stop=" + (n72 ? "ok" : "BAD")
                                                + " count=" + juce::String((int)inst.notes.size());
                                   return false;
                               }
                               bool cc64on = false, cc64off = false, cc11 = false, pb = false;
                               for (const auto& c : inst.cc)
                               {
                                   cc64on = cc64on || (c.controller == 64 && c.value == 127 && c.channel == 1);
                                   cc64off = cc64off || (c.controller == 64 && c.value == 0 && c.channel == 1 && c.tick == stopTick);
                                   cc11 = cc11 || (c.controller == 11 && c.value == 90 && c.channel == 1);
                               }
                               for (const auto& b : inst.pitchBend)
                               {
                                   pb = pb || (b.value == 12000 && b.channel == 1);
                               }
                               if (!cc64on || !cc64off || !cc11 || !pb)
                               {
                                   failReason = juce::String("controller data wrong: sustainOn=") + (cc64on ? "ok" : "BAD")
                                                + " sustainReleasedAtStop=" + (cc64off ? "ok" : "BAD") + " cc11=" + (cc11 ? "ok" : "BAD")
                                                + " pitchBend=" + (pb ? "ok" : "BAD");
                                   return false;
                               }
                               if (lower.notes.size() != 1 || lower.notes[0].note != 62 || lower.notes[0].channel != 5
                                   || lower.notes[0].velocity != 90)
                               {
                                   failReason = "Lower take must hold exactly note 62 on its RECEIVED channel 5 (not the Force 2 output)";
                                   return false;
                               }
                               if (hooks_.undoStackSize() != liveMidiUndoSizeBeforeTake_ + 1)
                               {
                                   failReason = "the take did not produce exactly one undo step (got "
                                                + juce::String(hooks_.undoStackSize() - liveMidiUndoSizeBeforeTake_) + ")";
                                   return false;
                               }
                               return true;
                           },
                           kSettleDefaultMs });
    steps_.push_back(Step{ "live-midi: Undo removes both rows' take clips at once; Redo restores both",
                           [this](juce::String& failReason) -> bool {
                               hooks_.invokeUndo();
                               juce::Thread::sleep(200);
                               const int afterUndoInst = hooks_.liveMidiSummarizeClips(liveMidiInstTid_).clipCount;
                               const int afterUndoLower = hooks_.liveMidiSummarizeClips(liveMidiLowerTid_).clipCount;
                               hooks_.invokeRedo();
                               juce::Thread::sleep(200);
                               const StabilityMidiClipSummary redoInst = hooks_.liveMidiSummarizeClips(liveMidiInstTid_);
                               const int afterRedoLower = hooks_.liveMidiSummarizeClips(liveMidiLowerTid_).clipCount;
                               appendStabilityRunLine("  undo -> inst/lower clips " + juce::String(afterUndoInst) + "/" + juce::String(afterUndoLower)
                                                      + "; redo -> " + juce::String(redoInst.clipCount) + "/" + juce::String(afterRedoLower));
                               if (afterUndoInst != 0 || afterUndoLower != 0 || redoInst.clipCount != 1 || afterRedoLower != 1
                                   || redoInst.notes.size() != 3 || redoInst.pitchBend.empty())
                               {
                                   failReason = "undo/redo of the take is not atomic across rows (or lost controller data)";
                                   return false;
                               }
                               return true;
                           },
                           kSettleDefaultMs });
    steps_.push_back(Step{ "live-midi: save, reload, verify the take survived (notes, CC, pitch bend, MIDI Input assignment)",
                           [this](juce::String&) -> bool {
                               hooks_.saveProject();
                               return true;
                           },
                           600 });
    steps_.push_back(Step{ "live-midi: reload test copy",
                           [this](juce::String&) -> bool {
                               hooks_.loadProjectFromFile(openSaveCloseCopy_);
                               return true;
                           },
                           kSettleAfterLoadMs });
    steps_.push_back(Step{ "live-midi: after reload: clips, controller data and the Inspector MIDI Input persisted; Monitor/Arm are OFF",
                           [this](juce::String& failReason) -> bool {
                               // The reload rebuilt the destination runtime: attach the capture sink to the new host.
                               if (hooks_.liveMidiAttachCaptureSink && !hooks_.liveMidiAttachCaptureSink(liveMidiInstTid_, failReason))
                               {
                                   return false;
                               }
                               const StabilityMidiClipSummary inst = hooks_.liveMidiSummarizeClips(liveMidiInstTid_);
                               const StabilityMidiClipSummary lower = hooks_.liveMidiSummarizeClips(liveMidiLowerTid_);
                               bool pb = false, cc = false;
                               for (const auto& b : inst.pitchBend)
                               {
                                   pb = pb || b.value == 12000;
                               }
                               for (const auto& c : inst.cc)
                               {
                                   cc = cc || (c.controller == 64 && c.value == 127);
                               }
                               appendStabilityRunLine("  after reload: inst clips=" + juce::String(inst.clipCount) + " notes=" + juce::String((int)inst.notes.size())
                                                      + " cc=" + juce::String((int)inst.cc.size()) + " pb=" + juce::String((int)inst.pitchBend.size())
                                                      + "; lower clips=" + juce::String(lower.clipCount));
                               if (inst.clipCount != 1 || inst.notes.size() != 3 || !pb || !cc || lower.clipCount != 1)
                               {
                                   failReason = "take did not survive save/reload";
                                   return false;
                               }
                               if (hooks_.selectTrackLikeHeaderClick && hooks_.inspectorMidiInputTexts)
                               {
                                   hooks_.selectTrackLikeHeaderClick(liveMidiLowerTid_);
                                   juce::Thread::sleep(150);
                                   const juce::String texts = hooks_.inspectorMidiInputTexts();
                                   appendStabilityRunLine("  inspector (Lower) after reload: " + texts);
                                   if (!texts.contains("All MIDI inputs") || !texts.contains("channel=5"))
                                   {
                                       failReason = "Lower's MIDI Input assignment (All, channel 5) did not persist";
                                       return false;
                                   }
                               }
                               return true;
                           },
                           400 });
    steps_.push_back(Step{ "live-midi: MIDI export of the instrument take carries notes, CC and pitch wheel",
                           [this](juce::String& failReason) -> bool {
                               if (!hooks_.liveMidiExportFirstClip)
                               {
                                   return true;
                               }
                               liveMidiExportFile_ = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("dal-live-midi-take.mid");
                               int notes = 0, cc = 0, pb = 0;
                               if (!hooks_.liveMidiExportFirstClip(liveMidiInstTid_, liveMidiExportFile_, notes, cc, pb, failReason))
                               {
                                   return false;
                               }
                               appendStabilityRunLine("  export: notes=" + juce::String(notes) + " cc=" + juce::String(cc) + " pitchWheel="
                                                      + juce::String(pb) + " -> " + liveMidiExportFile_.getFullPathName());
                               if (notes != 3 || cc < 3 || pb < 1)
                               {
                                   failReason = "exported SMF lacks the recorded notes / CC / pitch wheel";
                                   return false;
                               }
                               return true;
                           },
                           kSettleDefaultMs });
    steps_.push_back(Step{ "live-midi: the recorded clip opens in the MIDI editor (ordinary clip), then close",
                           [this](juce::String& failReason) -> bool {
                               if (!hooks_.openMidiEditorOnFirstClip || !hooks_.closeMidiEditor)
                               {
                                   return true;
                               }
                               if (!hooks_.openMidiEditorOnFirstClip(liveMidiInstTid_))
                               {
                                   failReason = "could not open the MIDI editor on the recorded clip";
                                   return false;
                               }
                               return true;
                           },
                           600 });
    steps_.push_back(Step{ "live-midi: close the MIDI editor",
                           [this](juce::String&) -> bool {
                               if (hooks_.closeMidiEditor)
                               {
                                   hooks_.closeMidiEditor();
                               }
                               return true;
                           },
                           400 });
    if (hooks_.runMixdownBlocking)
    {
        steps_.push_back(Step{ "live-midi: hold a live note (Monitor on) and run an offline WAV export: the clip renders, live MIDI never leaks",
                               [this, inject, on, off](juce::String& failReason) -> bool {
                                   hooks_.liveMidiSetMonitor(liveMidiInstTid_, true);
                                   inject(on(1, 90, 100));
                                   juce::Thread::sleep(250);
                                   const int heardLive = hooks_.liveMidiCapturedNoteCount(1, 90, true);
                                   hooks_.liveMidiCaptureReset();
                                   const juce::File out = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("dal-live-midi-export.wav");
                                   (void)out.deleteFile();
                                   // The exporter renders the active loop range: enable Cycle for the export only
                                   // (the project's locators are untouched), restore afterwards.
                                   const bool cycleWas = hooks_.isCycleEnabled ? hooks_.isCycleEnabled() : false;
                                   if (hooks_.setCycleEnabled)
                                   {
                                       hooks_.setCycleEnabled(true);
                                   }
                                   const juce::Result r = hooks_.runMixdownBlocking(out, false);
                                   if (hooks_.setCycleEnabled)
                                   {
                                       hooks_.setCycleEnabled(cycleWas);
                                   }
                                   const int leaked = hooks_.liveMidiCapturedNoteCount(1, 90, true);
                                   const int clipNotes = hooks_.liveMidiCapturedNoteCount(1, 60, true);
                                   appendStabilityRunLine("  offline export: result=" + juce::String(r.wasOk() ? "ok" : r.getErrorMessage())
                                                          + " heardLiveBefore=" + juce::String(heardLive) + " liveLeakedIntoExport="
                                                          + juce::String(leaked) + " clipNotesRendered=" + juce::String(clipNotes) + " size="
                                                          + juce::String(out.getSize()));
                                   inject(off(1, 90));
                                   (void)out.deleteFile();
                                   hooks_.liveMidiSetMonitor(liveMidiInstTid_, false);
                                   if (!r.wasOk())
                                   {
                                       failReason = "offline export failed after the take: " + r.getErrorMessage();
                                       return false;
                                   }
                                   if (heardLive != 1 || leaked != 0 || clipNotes < 1)
                                   {
                                       failReason = "live MIDI leaked into the offline render, or the recorded clip did not render";
                                       return false;
                                   }
                                   return true;
                               },
                               600 });
    }
    steps_.push_back(Step{ "live-midi: playback after the take (transport + clips play normally)",
                           [this](juce::String&) -> bool {
                               hooks_.liveMidiCaptureReset();
                               if (hooks_.seekTransportTo)
                               {
                                   hooks_.seekTransportTo(juce::jmax<std::int64_t>(0, liveMidiTakeStart_ - 24000));
                               }
                               hooks_.setPlaybackActive(true);
                               return true;
                           },
                           2500 });
    steps_.push_back(Step{ "live-midi: the recorded notes came back through playback (ch1 60 on >= 1); stop",
                           [this](juce::String& failReason) -> bool {
                               const int n = hooks_.liveMidiCapturedNoteCount(1, 60, true);
                               const int lowerN = hooks_.liveMidiCapturedNoteCount(2, 62, true);
                               appendStabilityRunLine("  playback: ch1 60 on=" + juce::String(n) + " Lower->ch2 62 on=" + juce::String(lowerN));
                               hooks_.setPlaybackActive(false);
                               if (n < 1 || lowerN < 1)
                               {
                                   failReason = "recorded clips did not play back to the destination";
                                   return false;
                               }
                               return true;
                           },
                           500 });
    // ---- 5. Empty take: arm, record, play nothing, stop -> no clip, no undo step.
    steps_.push_back(Step{ "live-midi: empty take - arm instrument, Record, no input, Stop -> no clip, no undo step",
                           [this](juce::String& failReason) -> bool {
                               hooks_.liveMidiSetArm(liveMidiInstTid_, true);
                               const int before = hooks_.liveMidiSummarizeClips(liveMidiInstTid_).clipCount;
                               liveMidiUndoSizeBeforeTake_ = hooks_.undoStackSize();
                               hooks_.recordToggleLikeKey();
                               juce::Thread::sleep(100);
                               if (!hooks_.isCountInActive())
                               {
                                   failReason = "empty-take Record did not start a count-in";
                                   return false;
                               }
                               liveMidiNoteOnPlayhead_ = before; // reuse as scratch: clip count before
                               return true;
                           },
                           4300 });
    steps_.push_back(Step{ "live-midi: stop the empty take",
                           [this](juce::String& failReason) -> bool {
                               if (!hooks_.isRecordingInProgress())
                               {
                                   failReason = "empty take never started recording";
                                   return false;
                               }
                               hooks_.recordToggleLikeKey();
                               juce::Thread::sleep(200);
                               const int after = hooks_.liveMidiSummarizeClips(liveMidiInstTid_).clipCount;
                               appendStabilityRunLine("  empty take: clips before/after=" + juce::String((int)liveMidiNoteOnPlayhead_) + "/" + juce::String(after)
                                                      + " undo size delta=" + juce::String(hooks_.undoStackSize() - liveMidiUndoSizeBeforeTake_));
                               if (after != (int)liveMidiNoteOnPlayhead_ || hooks_.undoStackSize() != liveMidiUndoSizeBeforeTake_)
                               {
                                   failReason = "an empty take created a clip or an undo step";
                                   return false;
                               }
                               hooks_.liveMidiSetArm(liveMidiInstTid_, false);
                               hooks_.liveMidiSetArm(liveMidiLowerTid_, false);
                               hooks_.liveMidiSetMonitor(liveMidiInstTid_, false);
                               return true;
                           },
                           kSettleDefaultMs });
    // ---- 6. Audio + MIDI in ONE take (needs an audio track with an available input; skipped
    //         with a logged reason when the device has no active input).
    if (hooks_.armAudioTrackForRecording && hooks_.audioClipCountForTrack && hooks_.listAllTracks)
    {
        auto audioTid = std::make_shared<TrackId>(kInvalidTrackId);
        auto audioClipsBefore = std::make_shared<int>(0);
        auto combinedStarted = std::make_shared<bool>(false);
        steps_.push_back(Step{ "live-midi: combined take - arm an audio track (R) + the instrument row, Record",
                               [this, audioTid, audioClipsBefore, combinedStarted](juce::String&) -> bool {
                                   for (const StabilityTrackInfo& t : hooks_.listAllTracks())
                                   {
                                       if (t.kindName == "audio" && *audioTid == kInvalidTrackId)
                                       {
                                           *audioTid = t.id;
                                       }
                                   }
                                   if (*audioTid == kInvalidTrackId)
                                   {
                                       appendStabilityRunLine("  no audio track in this project - combined take skipped");
                                       return true;
                                   }
                                   *audioClipsBefore = hooks_.audioClipCountForTrack(*audioTid);
                                   liveMidiUndoSizeBeforeTake_ = hooks_.undoStackSize();
                                   hooks_.armAudioTrackForRecording(*audioTid);
                                   hooks_.liveMidiSetArm(liveMidiInstTid_, true);
                                   hooks_.recordToggleLikeKey();
                                   juce::Thread::sleep(200);
                                   *combinedStarted = hooks_.isCountInActive();
                                   appendStabilityRunLine(juce::String("  combined take count-in started: ") + (*combinedStarted ? "yes" : "no (audio input unavailable on this device - skipped)"));
                                   return true;
                               },
                               3900 });
        steps_.push_back(Step{ "live-midi: combined take - play ch1 note 67 while recording",
                               [this, combinedStarted, inject, on](juce::String& failReason) -> bool {
                                   if (!*combinedStarted)
                                   {
                                       return true;
                                   }
                                   if (!hooks_.isRecordingInProgress())
                                   {
                                       failReason = "combined take did not start recording";
                                       return false;
                                   }
                                   inject(on(1, 67, 100));
                                   return true;
                               },
                               800 });
        steps_.push_back(Step{ "live-midi: combined take - release, Stop, verify ONE undo step holds both the audio clip and the MIDI clip",
                               [this, audioTid, audioClipsBefore, combinedStarted, inject, off](juce::String& failReason) -> bool {
                                   if (!*combinedStarted)
                                   {
                                       hooks_.armAudioTrackForRecording(kInvalidTrackId);
                                       hooks_.liveMidiSetArm(liveMidiInstTid_, false);
                                       return true;
                                   }
                                   inject(off(1, 67));
                                   juce::Thread::sleep(150);
                                   const int midiBefore = hooks_.liveMidiSummarizeClips(liveMidiInstTid_).clipCount;
                                   hooks_.recordToggleLikeKey();
                                   juce::Thread::sleep(400);
                                   const int audioAfter = hooks_.audioClipCountForTrack(*audioTid);
                                   const int midiAfter = hooks_.liveMidiSummarizeClips(liveMidiInstTid_).clipCount;
                                   const int undoDelta = hooks_.undoStackSize() - liveMidiUndoSizeBeforeTake_;
                                   appendStabilityRunLine("  combined take: audio clips " + juce::String(*audioClipsBefore) + "->" + juce::String(audioAfter)
                                                          + ", midi clips " + juce::String(midiBefore) + "->" + juce::String(midiAfter) + ", undo steps +"
                                                          + juce::String(undoDelta));
                                   hooks_.armAudioTrackForRecording(kInvalidTrackId);
                                   hooks_.liveMidiSetArm(liveMidiInstTid_, false);
                                   if (audioAfter != *audioClipsBefore + 1 || midiAfter != midiBefore + 1 || undoDelta != 1)
                                   {
                                       failReason = "audio + MIDI take must add one audio clip and one MIDI clip in exactly one undo step";
                                       return false;
                                   }
                                   // Shared stop boundary (1.1.13): the audio clip and the MIDI clip of one run have
                                   // the SAME length (engine-acknowledged start/stop), while their placements keep
                                   // their own deliberate compensations (audio: latency-store offset; MIDI: per
                                   // gesture −output latency inside the clip, window start raw).
                                   if (hooks_.audioClipWindowsForTrack && hooks_.liveMidiSummarizeAllClips)
                                   {
                                       const auto audio = hooks_.audioClipWindowsForTrack(*audioTid);
                                       const auto midi = hooks_.liveMidiSummarizeAllClips(liveMidiInstTid_);
                                       if (!audio.empty() && !midi.empty())
                                       {
                                           const std::int64_t audioLen = audio.front().second;
                                           const std::int64_t midiLen = midi.back().firstClipLengthSamples;
                                           appendStabilityRunLine("  shared stop boundary: audio len=" + juce::String((juce::int64)audioLen) + " start="
                                                                  + juce::String((juce::int64)audio.front().first) + " | midi len="
                                                                  + juce::String((juce::int64)midiLen) + " start="
                                                                  + juce::String((juce::int64)midi.back().firstClipStartSamples)
                                                                  + " | audio placement offset="
                                                                  + juce::String((juce::int64)(hooks_.recordingPlacementOffsetSamples ? hooks_.recordingPlacementOffsetSamples() : 0))
                                                                  + " midi gesture offset=-" + juce::String(hooks_.reportedOutputLatencySamples ? hooks_.reportedOutputLatencySamples() : 0)
                                                                  + " | " + (hooks_.lastRecordRunBoundaries ? hooks_.lastRecordRunBoundaries() : juce::String()));
                                           if (audioLen != midiLen)
                                           {
                                               failReason = "audio and MIDI clips of one run must have the same length (shared stop boundary)";
                                               return false;
                                           }
                                       }
                                   }
                                   hooks_.invokeUndo();
                                   juce::Thread::sleep(300);
                                   const int audioUndo = hooks_.audioClipCountForTrack(*audioTid);
                                   const int midiUndo = hooks_.liveMidiSummarizeClips(liveMidiInstTid_).clipCount;
                                   hooks_.invokeRedo();
                                   juce::Thread::sleep(300);
                                   const int audioRedo = hooks_.audioClipCountForTrack(*audioTid);
                                   const int midiRedo = hooks_.liveMidiSummarizeClips(liveMidiInstTid_).clipCount;
                                   appendStabilityRunLine("  combined take undo -> audio/midi " + juce::String(audioUndo) + "/" + juce::String(midiUndo)
                                                          + "; redo -> " + juce::String(audioRedo) + "/" + juce::String(midiRedo));
                                   if (audioUndo != *audioClipsBefore || midiUndo != midiBefore || audioRedo != *audioClipsBefore + 1
                                       || midiRedo != midiBefore + 1)
                                   {
                                       failReason = "undo/redo of the combined take is not atomic";
                                       return false;
                                   }
                                   return true;
                               },
                               kSettleDefaultMs });
    }
    steps_.push_back(Step{ "live-midi: delete the export file",
                           [this](juce::String&) -> bool {
                               (void)liveMidiExportFile_.deleteFile();
                               return true;
                           },
                           0 });
}

// -----------------------------------------------------------------------------
// Organ residual DC / AC with the transport stopped, around a header mute
// -----------------------------------------------------------------------------
void StabilityScenarioRunner::appendOrganDcSteps(const juce::File& project)
{
    if (hooks_.listAllTracks == nullptr || hooks_.activateTrackLikeHeaderClick == nullptr || hooks_.setMeteredTrack == nullptr
        || hooks_.drainTrackMeter == nullptr || hooks_.drainMasterMeter == nullptr || hooks_.setTrackMutedLikeHeader == nullptr
        || hooks_.describeTrackForDiagnostics == nullptr || hooks_.seekTransportTo == nullptr || hooks_.setPlaybackActive == nullptr)
    {
        steps_.push_back(Step{ "organ-dc: hooks missing",
                               [](juce::String& failReason) -> bool {
                                   failReason = "organ-dc hooks not installed";
                                   return false;
                               },
                               0 });
        return;
    }

    appendLoadAndVerifySteps(project, "organ-dc");

    auto organ = std::make_shared<TrackId>(kInvalidTrackId);
    const auto acRms = [](const StabilityLevelStats& s, const int ch) {
        const double rms2 = s.rms[ch] * s.rms[ch];
        const double dc2 = s.dcOffset[ch] * s.dcOffset[ch];
        return std::sqrt(juce::jmax(0.0, rms2 - dc2));
    };
    const auto describe = [acRms](const juce::String& who, const StabilityLevelStats& s) {
        return who + ": frames=" + juce::String((juce::int64)s.frames) + " dc=[" + juce::String(s.dcOffset[0], 4) + "," + juce::String(s.dcOffset[1], 4)
               + "] (" + dbfs((float)std::fabs(s.dcOffset[0])) + " dBFS) acRms=[" + juce::String(acRms(s, 0), 5) + "," + juce::String(acRms(s, 1), 5)
               + "] peak=[" + juce::String(s.peak[0], 4) + "," + juce::String(s.peak[1], 4) + "] rms=[" + juce::String(s.rms[0], 4) + ","
               + juce::String(s.rms[1], 4) + "]";
    };

    steps_.push_back(Step{ "organ-dc: select the VB3-II row like a header click (transport stopped)",
                           [this, organ](juce::String& failReason) -> bool {
                               for (const StabilityTrackInfo& t : hooks_.listAllTracks())
                               {
                                   if (t.kindName == "instrument" && t.name.containsIgnoreCase("VB3"))
                                   {
                                       *organ = t.id;
                                   }
                               }
                               if (*organ == kInvalidTrackId)
                               {
                                   failReason = "no VB3-II instrument row in this project";
                                   return false;
                               }
                               hooks_.setPlaybackActive(false);
                               hooks_.activateTrackLikeHeaderClick(*organ);
                               hooks_.setMeteredTrack(*organ);
                               appendStabilityRunLine("  " + hooks_.describeTrackForDiagnostics(*organ));
                               (void)hooks_.drainTrackMeter();
                               (void)hooks_.drainMasterMeter();
                               return true;
                           },
                           500 });

    auto hostBlocksAtArm = std::make_shared<std::uint64_t>(0);
    const auto measureStep = [this, organ, describe, hostBlocksAtArm](const juce::String& label, const int settleMs,
                                                                      const bool expectTrackSignal, const bool expectTrackSilence) {
        steps_.push_back(Step{ "organ-dc: " + label + " - arm 2 s window",
                               [this, organ, hostBlocksAtArm](juce::String&) -> bool {
                                   (void)hooks_.drainTrackMeter();
                                   (void)hooks_.drainMasterMeter();
                                   *hostBlocksAtArm = hooks_.instrumentProcessedBlocks != nullptr ? hooks_.instrumentProcessedBlocks(*organ) : 0;
                                   return true;
                               },
                               settleMs });
        steps_.push_back(Step{ "organ-dc: " + label + " - read",
                               [this, organ, hostBlocksAtArm, label, describe, expectTrackSignal, expectTrackSilence](juce::String& failReason) -> bool {
                                   const StabilityLevelStats t = hooks_.drainTrackMeter();
                                   const StabilityLevelStats m = hooks_.drainMasterMeter();
                                   const std::uint64_t hostBlocks = hooks_.instrumentProcessedBlocks != nullptr
                                                                        ? hooks_.instrumentProcessedBlocks(*organ) - *hostBlocksAtArm
                                                                        : 0;
                                   appendStabilityRunLine("  " + describe(label + " ORGAN post-strip", t) + " hostBlocksProcessed="
                                                          + juce::String((juce::int64)hostBlocks));
                                   appendStabilityRunLine("  " + describe(label + " STEREO OUT", m));
                                   if (hooks_.instrumentProcessedBlocks != nullptr && hostBlocks == 0)
                                   {
                                       failReason = "the organ's instrument host processed no blocks in this window (a muted row must keep "
                                                    "processing with gain 0 - otherwise its MIDI piles up and bursts on unmute)";
                                       return false;
                                   }
                                   if (hooks_.describeInspectorMeters != nullptr)
                                   {
                                       appendStabilityRunLine("  " + label + " UI meters: " + hooks_.describeInspectorMeters());
                                   }
                                   if (m.frames == 0)
                                   {
                                       failReason = "no audio blocks measured on the Stereo Out (callback not running?)";
                                       return false;
                                   }
                                   if (t.frames == 0)
                                   {
                                       failReason = "no audio blocks measured on the organ row's stage (the strip must run - muted = gain 0)";
                                       return false;
                                   }
                                   const double worstDc = juce::jmax(std::fabs(t.dcOffset[0]), std::fabs(t.dcOffset[1]));
                                   if (expectTrackSignal && worstDc < 0.01 && t.peak[0] < 0.01)
                                   {
                                       appendStabilityRunLine("  NOTE: organ row is silent in this window (no residual signal present right now)");
                                   }
                                   if (expectTrackSilence && (t.peak[0] > 1.0e-4f || t.peak[1] > 1.0e-4f))
                                   {
                                       failReason = "muted organ row still carries signal on its post-strip stage";
                                       return false;
                                   }
                                   return true;
                               },
                               kSettleDefaultMs });
    };

    measureStep("stopped, unmuted, before any playback", 2000, true, false);

    steps_.push_back(Step{ "organ-dc: mute the organ row like the header button (transport still stopped)",
                           [this, organ](juce::String&) -> bool {
                               hooks_.setTrackMutedLikeHeader(*organ, true);
                               return true;
                           },
                           200 });
    measureStep("stopped, MUTED", 2000, false, true);
    steps_.push_back(Step{ "organ-dc: unmute like the header button",
                           [this, organ](juce::String&) -> bool {
                               hooks_.setTrackMutedLikeHeader(*organ, false);
                               return true;
                           },
                           200 });
    measureStep("stopped, unmuted again", 2000, true, false);

    steps_.push_back(Step{ "organ-dc: play 4 s from 24 s (the organ plays there), then stop",
                           [this](juce::String&) -> bool {
                               std::int64_t start = 0, len = 0;
                               double sr = 48000.0;
                               if (hooks_.getActiveLoopSpan != nullptr && hooks_.getActiveLoopSpan(start, len, sr))
                               {
                                   hooks_.seekTransportTo(start + (std::int64_t)(24.0 * sr));
                               }
                               hooks_.setPlaybackActive(true);
                               return true;
                           },
                           4000 });
    steps_.push_back(Step{ "organ-dc: stop",
                           [this](juce::String&) -> bool {
                               hooks_.setPlaybackActive(false);
                               return true;
                           },
                           500 });
    measureStep("stopped after playback, unmuted", 2000, true, false);
    steps_.push_back(Step{ "organ-dc: mute after playback",
                           [this, organ](juce::String&) -> bool {
                               hooks_.setTrackMutedLikeHeader(*organ, true);
                               return true;
                           },
                           200 });
    measureStep("stopped after playback, MUTED", 2000, false, true);

    // ---- Mute WHILE PLAYING: the muted row must keep consuming its transport MIDI block by block.
    // Pre-1.1.9 the engine skipped muted instrument rows before the host ran, so the MIDI scheduled
    // every block piled up in the host's per-block buffer and was delivered as one stale burst on
    // unmute. The per-block maximum over "4 s muted playback + unmute while playing" must therefore
    // stay in the same range as the reference maximum from normal (unmuted) playback.
    auto refMaxEvents = std::make_shared<std::uint32_t>(0);
    auto blocksAtMutedPlayStart = std::make_shared<std::uint64_t>(0);
    // Same timeline span as the muted run below (24 s -> 29 s) so the per-block maxima are comparable.
    steps_.push_back(Step{ "organ-dc: reference - play 5 s from 24 s unmuted, record max MIDI events per block",
                           [this, organ](juce::String&) -> bool {
                               hooks_.setTrackMutedLikeHeader(*organ, false);
                               if (hooks_.instrumentMaxMidiEventsInOneBlock != nullptr)
                               {
                                   (void)hooks_.instrumentMaxMidiEventsInOneBlock(*organ, true);
                               }
                               std::int64_t start = 0, len = 0;
                               double sr = 48000.0;
                               if (hooks_.getActiveLoopSpan != nullptr && hooks_.getActiveLoopSpan(start, len, sr))
                               {
                                   hooks_.seekTransportTo(start + (std::int64_t)(24.0 * sr));
                               }
                               hooks_.setPlaybackActive(true);
                               return true;
                           },
                           5000 });
    steps_.push_back(Step{ "organ-dc: stop reference playback",
                           [this, organ, refMaxEvents](juce::String& failReason) -> bool {
                               hooks_.setPlaybackActive(false);
                               if (hooks_.instrumentMaxMidiEventsInOneBlock == nullptr)
                               {
                                   return true;
                               }
                               *refMaxEvents = hooks_.instrumentMaxMidiEventsInOneBlock(*organ, true);
                               appendStabilityRunLine("  reference (unmuted playback): max MIDI events in one block = "
                                                      + juce::String((int)*refMaxEvents));
                               if (*refMaxEvents == 0)
                               {
                                   failReason = "the organ received no MIDI during the reference playback (does the organ play at 24 s?)";
                                   return false;
                               }
                               return true;
                           },
                           500 });
    // One muted run = mute -> play from 24 s for `mutedMs` -> read the muted-window maximum -> unmute
    // while playing -> 1 s -> stop -> read the unmute-window maximum. Two runs with very different
    // mute durations: accumulation would make the unmute maximum grow with the mute duration;
    // a bounded, duration-independent value is the normal per-block content plus the CC chase.
    struct MutedRunResult
    {
        std::uint64_t blocksWhileMuted = 0;
        std::uint32_t maxEventsWhileMuted = 0;
        std::uint32_t maxEventsAfterUnmute = 0;
    };
    auto longRun = std::make_shared<MutedRunResult>();
    auto shortRun = std::make_shared<MutedRunResult>();
    const auto appendMutedRun = [this, organ, blocksAtMutedPlayStart, refMaxEvents](const int mutedMs,
                                                                                    std::shared_ptr<MutedRunResult> result) {
        const juce::String tag = juce::String(mutedMs) + " ms";
        steps_.push_back(Step{ "organ-dc: mute, then play from 24 s with the row MUTED for " + tag,
                               [this, organ, blocksAtMutedPlayStart](juce::String&) -> bool {
                                   hooks_.setTrackMutedLikeHeader(*organ, true);
                                   *blocksAtMutedPlayStart = hooks_.instrumentProcessedBlocks != nullptr
                                                                 ? hooks_.instrumentProcessedBlocks(*organ)
                                                                 : 0;
                                   std::int64_t start = 0, len = 0;
                                   double sr = 48000.0;
                                   if (hooks_.getActiveLoopSpan != nullptr && hooks_.getActiveLoopSpan(start, len, sr))
                                   {
                                       hooks_.seekTransportTo(start + (std::int64_t)(24.0 * sr));
                                   }
                                   hooks_.setPlaybackActive(true);
                                   return true;
                               },
                               mutedMs });
        steps_.push_back(Step{ "organ-dc: unmute WHILE PLAYING after " + tag + " (play 1 s more)",
                               [this, organ, blocksAtMutedPlayStart, result](juce::String& failReason) -> bool {
                                   result->blocksWhileMuted = hooks_.instrumentProcessedBlocks != nullptr
                                                                  ? hooks_.instrumentProcessedBlocks(*organ) - *blocksAtMutedPlayStart
                                                                  : 0;
                                   result->maxEventsWhileMuted = hooks_.instrumentMaxMidiEventsInOneBlock != nullptr
                                                                     ? hooks_.instrumentMaxMidiEventsInOneBlock(*organ, true)
                                                                     : 0;
                                   hooks_.setTrackMutedLikeHeader(*organ, false);
                                   if (hooks_.instrumentProcessedBlocks != nullptr && result->blocksWhileMuted == 0)
                                   {
                                       failReason = "the organ's host processed no blocks while muted during playback";
                                       return false;
                                   }
                                   return true;
                               },
                               1000 });
        steps_.push_back(Step{ "organ-dc: stop after the " + tag + " muted run",
                               [this, organ, refMaxEvents, result, tag](juce::String& failReason) -> bool {
                                   hooks_.setPlaybackActive(false);
                                   result->maxEventsAfterUnmute = hooks_.instrumentMaxMidiEventsInOneBlock != nullptr
                                                                      ? hooks_.instrumentMaxMidiEventsInOneBlock(*organ, true)
                                                                      : 0;
                                   appendStabilityRunLine("  muted " + tag + ": host blocks processed while muted = "
                                                          + juce::String((juce::int64)result->blocksWhileMuted)
                                                          + ", max MIDI events in one block while muted = "
                                                          + juce::String((int)result->maxEventsWhileMuted)
                                                          + ", in the 1 s after unmute = " + juce::String((int)result->maxEventsAfterUnmute)
                                                          + " (unmuted reference " + juce::String((int)*refMaxEvents) + ")");
                                   if (hooks_.instrumentMaxMidiEventsInOneBlock != nullptr
                                       && result->maxEventsWhileMuted > *refMaxEvents * 2 + 8)
                                   {
                                       failReason = "the muted row received " + juce::String((int)result->maxEventsWhileMuted)
                                                    + " MIDI events in one block vs reference " + juce::String((int)*refMaxEvents)
                                                    + " (per-block buffer not consumed while muted)";
                                       return false;
                                   }
                                   return true;
                               },
                               500 });
    };
    appendMutedRun(4000, longRun);
    appendMutedRun(300, shortRun);
    steps_.push_back(Step{ "organ-dc: unmute burst must not depend on the mute duration",
                           [this, longRun, shortRun](juce::String& failReason) -> bool {
                               if (hooks_.instrumentMaxMidiEventsInOneBlock == nullptr)
                               {
                                   return true;
                               }
                               appendStabilityRunLine("  unmute block maximum: after 4000 ms muted = "
                                                      + juce::String((int)longRun->maxEventsAfterUnmute) + ", after 300 ms muted = "
                                                      + juce::String((int)shortRun->maxEventsAfterUnmute));
                               if (longRun->maxEventsAfterUnmute > shortRun->maxEventsAfterUnmute * 2 + 8)
                               {
                                   failReason = "a longer mute produced a larger MIDI burst on unmute (stale events accumulated while muted)";
                                   return false;
                               }
                               return true;
                           },
                           0 });

    steps_.push_back(Step{ "organ-dc: release the meter tap",
                           [this](juce::String&) -> bool {
                               hooks_.setMeteredTrack(kInvalidTrackId);
                               return true;
                           },
                           kSettleDefaultMs });
}

void StabilityScenarioRunner::appendMidiRoutingSteps(const juce::File& project)
{
    if (hooks_.midiRoutingFixtureSetup == nullptr || hooks_.midiRoutingVerifyDelivery == nullptr
        || hooks_.midiRoutingVerifyAfterReload == nullptr)
    {
        steps_.push_back(Step{ "midi-routing: hooks missing",
                               [](juce::String& failReason) -> bool {
                                   failReason = "midi-routing hooks not installed";
                                   return false;
                               },
                               0 });
        return;
    }

    // Sibling copy, same reasoning as open-save-close: the user's project is never modified and
    // project-relative paths keep resolving.
    steps_.push_back(Step{
        "midi-routing: copy project to sibling test file",
        [this, project](juce::String& failReason) -> bool {
            const juce::File copy = project.getSiblingFile(
                project.getFileNameWithoutExtension() + "-midiroutingtest.dalproj");
            (void)copy.deleteFile();
            if (!project.copyFileTo(copy))
            {
                failReason = "could not copy project to " + copy.getFullPathName();
                return false;
            }
            openSaveCloseCopy_ = copy;
            appendStabilityRunLine("  test copy: " + copy.getFullPathName());
            return true;
        },
        kSettleDefaultMs });

    steps_.push_back(Step{ "midi-routing: load test copy",
                           [this](juce::String&) -> bool {
                               hooks_.loadProjectFromFile(openSaveCloseCopy_);
                               return true;
                           },
                           kSettleAfterLoadMs });

    steps_.push_back(Step{ "midi-routing: build fixture (instrument shell + routed MIDI track)",
                           [this](juce::String& failReason) -> bool {
                               return hooks_.midiRoutingFixtureSetup(failReason);
                           },
                           600 });

    steps_.push_back(Step{ "midi-routing: start playback",
                           [this](juce::String&) -> bool {
                               hooks_.setPlaybackActive(true);
                               return true;
                           },
                           3000 });

    steps_.push_back(Step{ "midi-routing: stop playback",
                           [this](juce::String&) -> bool {
                               hooks_.setPlaybackActive(false);
                               return true;
                           },
                           600 });

    steps_.push_back(Step{ "midi-routing: verify capture-seam delivery",
                           [this](juce::String& failReason) -> bool {
                               return hooks_.midiRoutingVerifyDelivery(failReason);
                           },
                           kSettleDefaultMs });

    // Phase B.1: same fixture, offline mixdown path - the capture sink must see equivalent
    // routed MIDI (per-channel counts identical to the realtime pass).
    steps_.push_back(Step{ "midi-routing: offline mixdown parity (same routed MIDI)",
                           [this](juce::String& failReason) -> bool {
                               if (hooks_.midiRoutingRunOfflineParity == nullptr)
                               {
                                   failReason = "offline parity hook not installed";
                                   return false;
                               }
                               return hooks_.midiRoutingRunOfflineParity(failReason);
                           },
                           kSettleDefaultMs });

    steps_.push_back(Step{ "midi-routing: save (direct save to test copy)",
                           [this](juce::String&) -> bool {
                               hooks_.saveProject();
                               return true;
                           },
                           600 });

    steps_.push_back(Step{ "midi-routing: reload test copy",
                           [this](juce::String&) -> bool {
                               hooks_.loadProjectFromFile(openSaveCloseCopy_);
                               return true;
                           },
                           kSettleAfterLoadMs });

    steps_.push_back(Step{ "midi-routing: verify v18 roundtrip (Midi row + destination + clip)",
                           [this](juce::String& failReason) -> bool {
                               return hooks_.midiRoutingVerifyAfterReload(failReason);
                           },
                           kSettleDefaultMs });
}

void StabilityScenarioRunner::appendMidiTrackParitySteps(const juce::File& project)
{
    if (hooks_.midiRoutingFixtureSetup == nullptr || hooks_.midiTrackParityVerify == nullptr
        || hooks_.midiTrackParityVerifyAfterReload == nullptr)
    {
        steps_.push_back(Step{ "midi-track-parity: hooks missing",
                               [](juce::String& failReason) -> bool {
                                   failReason = "midi-track-parity hooks not installed";
                                   return false;
                               },
                               0 });
        return;
    }

    // Sibling copy: the user's project is never modified and project-relative paths keep resolving.
    steps_.push_back(Step{
        "midi-track-parity: copy project to sibling test file",
        [this, project](juce::String& failReason) -> bool {
            const juce::File copy = project.getSiblingFile(
                project.getFileNameWithoutExtension() + "-midiparitytest.dalproj");
            (void)copy.deleteFile();
            if (!project.copyFileTo(copy))
            {
                failReason = "could not copy project to " + copy.getFullPathName();
                return false;
            }
            openSaveCloseCopy_ = copy;
            appendStabilityRunLine("  test copy: " + copy.getFullPathName());
            return true;
        },
        kSettleDefaultMs });

    steps_.push_back(Step{ "midi-track-parity: load test copy",
                           [this](juce::String&) -> bool {
                               hooks_.loadProjectFromFile(openSaveCloseCopy_);
                               return true;
                           },
                           kSettleAfterLoadMs });

    // Reuses the routing fixture: one instrument row plus routed TrackKind::Midi rows with clips.
    steps_.push_back(Step{ "midi-track-parity: build fixture (instrument row + routed MIDI rows)",
                           [this](juce::String& failReason) -> bool {
                               return hooks_.midiRoutingFixtureSetup(failReason);
                           },
                           600 });

    steps_.push_back(Step{ "midi-track-parity: import onto MIDI row + move MIDI<->instrument",
                           [this](juce::String& failReason) -> bool {
                               return hooks_.midiTrackParityVerify(failReason);
                           },
                           kSettleDefaultMs });

    steps_.push_back(Step{ "midi-track-parity: save (direct save to test copy)",
                           [this](juce::String&) -> bool {
                               hooks_.saveProject();
                               return true;
                           },
                           600 });

    steps_.push_back(Step{ "midi-track-parity: reload test copy",
                           [this](juce::String&) -> bool {
                               hooks_.loadProjectFromFile(openSaveCloseCopy_);
                               return true;
                           },
                           kSettleAfterLoadMs });

    steps_.push_back(Step{ "midi-track-parity: verify imported clip survived on the MIDI row",
                           [this](juce::String& failReason) -> bool {
                               return hooks_.midiTrackParityVerifyAfterReload(failReason);
                           },
                           kSettleDefaultMs });
}

namespace
{
    /// Built-in import fixture when `--midi` is absent: a format-0 SMF (480 PPQ) with eight quarter
    /// notes on channel 1 - the same shape as a typical single-track export.
    [[nodiscard]] bool writeFixtureMidiFile(const juce::File& target)
    {
        constexpr int kPpq = 480;
        juce::MidiMessageSequence seq;
        for (int i = 0; i < 8; ++i)
        {
            const double t = static_cast<double>(i) * kPpq;
            seq.addEvent(juce::MidiMessage::noteOn(1, 60 + (i % 5) * 2, (juce::uint8)100), t);
            seq.addEvent(juce::MidiMessage::noteOff(1, 60 + (i % 5) * 2), t + kPpq / 2);
        }
        seq.updateMatchedPairs();
        juce::MidiFile mf;
        mf.setTicksPerQuarterNote(kPpq);
        mf.addTrack(seq);
        (void)target.deleteFile();
        juce::FileOutputStream os(target);
        return os.openedOk() && mf.writeTo(os, 0);
    }
} // namespace

void StabilityScenarioRunner::appendMidiImportAudioSteps(const juce::File& project,
                                                         const juce::File& midiFileArg)
{
    if (hooks_.audioHealthProbeBegin == nullptr || hooks_.audioHealthProbeVerify == nullptr
        || hooks_.addMidiTrackLikeUi == nullptr || hooks_.importMidiFileOntoTrack == nullptr)
    {
        steps_.push_back(Step{ "midi-import-audio: hooks missing",
                               [](juce::String& failReason) -> bool {
                                   failReason = "midi-import-audio hooks not installed";
                                   return false;
                               },
                               0 });
        return;
    }

    steps_.push_back(Step{
        "midi-import-audio: copy project to sibling test file",
        [this, project](juce::String& failReason) -> bool {
            const juce::File copy = project.getSiblingFile(
                project.getFileNameWithoutExtension() + "-midiimportaudiotest.dalproj");
            (void)copy.deleteFile();
            if (!project.copyFileTo(copy))
            {
                failReason = "could not copy project to " + copy.getFullPathName();
                return false;
            }
            openSaveCloseCopy_ = copy;
            appendStabilityRunLine("  test copy: " + copy.getFullPathName());
            return true;
        },
        kSettleDefaultMs });

    steps_.push_back(Step{ "midi-import-audio: load test copy",
                           [this](juce::String&) -> bool {
                               hooks_.loadProjectFromFile(openSaveCloseCopy_);
                               return true;
                           },
                           kSettleAfterLoadMs });

    // Baseline: the project must be audibly playing BEFORE anything is imported, otherwise a
    // silent "after" proves nothing.
    steps_.push_back(Step{ "midi-import-audio: baseline start playback (probe armed)",
                           [this](juce::String&) -> bool {
                               hooks_.setPlaybackActive(true);
                               hooks_.audioHealthProbeBegin();
                               return true;
                           },
                           2500 });
    steps_.push_back(Step{ "midi-import-audio: baseline verify audio health",
                           [this](juce::String& failReason) -> bool {
                               return hooks_.audioHealthProbeVerify("baseline-playing", failReason);
                           },
                           kSettleDefaultMs });
    steps_.push_back(Step{ "midi-import-audio: baseline stop playback",
                           [this](juce::String&) -> bool {
                               hooks_.setPlaybackActive(false);
                               return true;
                           },
                           600 });

    steps_.push_back(Step{ "midi-import-audio: add MIDI track (transport menu path)",
                           [this](juce::String& failReason) -> bool {
                               const std::optional<TrackId> tid = hooks_.addMidiTrackLikeUi();
                               if (!tid.has_value() || *tid == kInvalidTrackId)
                               {
                                   failReason = "add MIDI track returned no row id";
                                   return false;
                               }
                               scenarioMidiTrackId_ = *tid;
                               appendStabilityRunLine("  midi row id=" + juce::String((juce::int64)*tid));
                               return true;
                           },
                           600 });

    steps_.push_back(Step{
        "midi-import-audio: import MIDI file onto the MIDI track",
        [this, midiFileArg](juce::String& failReason) -> bool {
            juce::File midi = midiFileArg;
            if (midi == juce::File{} || !midi.existsAsFile())
            {
                midi = juce::File::getSpecialLocation(juce::File::tempDirectory)
                           .getChildFile("MiniDAWLab-midi-import-audio-fixture.mid");
                if (!writeFixtureMidiFile(midi))
                {
                    failReason = "could not write the built-in MIDI fixture";
                    return false;
                }
                appendStabilityRunLine("  using built-in MIDI fixture: " + midi.getFullPathName());
            }
            else
            {
                appendStabilityRunLine("  importing: " + midi.getFullPathName() + " ("
                                       + juce::String((juce::int64)midi.getSize()) + " bytes)");
            }
            return hooks_.importMidiFileOntoTrack(scenarioMidiTrackId_, midi, failReason);
        },
        800 });

    steps_.push_back(Step{ "midi-import-audio: after import start playback (probe armed)",
                           [this](juce::String&) -> bool {
                               hooks_.setPlaybackActive(true);
                               hooks_.audioHealthProbeBegin();
                               return true;
                           },
                           2500 });
    steps_.push_back(Step{ "midi-import-audio: after import verify audio health",
                           [this](juce::String& failReason) -> bool {
                               return hooks_.audioHealthProbeVerify("after-import-playing", failReason);
                           },
                           kSettleDefaultMs });
    steps_.push_back(Step{ "midi-import-audio: stop playback",
                           [this](juce::String&) -> bool {
                               hooks_.setPlaybackActive(false);
                               return true;
                           },
                           600 });
}

void StabilityScenarioRunner::appendMidiEditorMoveCrashSteps(const juce::File& project)
{
    if (hooks_.midiRoutingFixtureSetup == nullptr || hooks_.isMidiEditorOpen == nullptr
        || hooks_.clipCountOnTrack == nullptr || hooks_.selectFirstClipOnTrack == nullptr
        || hooks_.openMidiEditorOnFirstClipOfTrack == nullptr
        || hooks_.copyThenPasteSelectedMidiClipLikeUi == nullptr
        || hooks_.moveMidiClipCrossTrackLikeUi == nullptr || hooks_.audioHealthProbeBegin == nullptr
        || hooks_.audioHealthProbeVerify == nullptr)
    {
        steps_.push_back(Step{ "midi-editor-move: hooks missing",
                               [](juce::String& failReason) -> bool {
                                   failReason = "midi-editor-move hooks not installed";
                                   return false;
                               },
                               0 });
        return;
    }

    steps_.push_back(Step{
        "midi-editor-move: copy project to sibling test file",
        [this, project](juce::String& failReason) -> bool {
            const juce::File copy = project.getSiblingFile(
                project.getFileNameWithoutExtension() + "-midieditormovetest.dalproj");
            (void)copy.deleteFile();
            if (!project.copyFileTo(copy))
            {
                failReason = "could not copy project to " + copy.getFullPathName();
                return false;
            }
            openSaveCloseCopy_ = copy;
            appendStabilityRunLine("  test copy: " + copy.getFullPathName());
            return true;
        },
        kSettleDefaultMs });

    steps_.push_back(Step{ "midi-editor-move: load test copy",
                           [this](juce::String&) -> bool {
                               hooks_.loadProjectFromFile(openSaveCloseCopy_);
                               return true;
                           },
                           kSettleAfterLoadMs });

    // Fixture: one instrument row (destination) plus two routed MIDI rows ("Lower"/"Pedal") that
    // each own a clip. `midiRoutingInstTid_` / `midiRoutingMidiLowerTid_` are remembered by the hook.
    steps_.push_back(Step{ "midi-editor-move: build fixture (instrument row + routed MIDI rows)",
                           [this](juce::String& failReason) -> bool {
                               return hooks_.midiRoutingFixtureSetup(failReason);
                           },
                           600 });

    // --- Symptom B: copy/paste must not open the MIDI editor ---
    steps_.push_back(Step{
        "midi-editor-move: select then copy+paste the MIDI clip (must NOT open editor)",
        [this](juce::String& failReason) -> bool {
            if (hooks_.isMidiEditorOpen())
            {
                failReason = "editor already open before paste (unexpected initial state)";
                return false;
            }
            if (hooks_.selectFirstClipOnTrack(hooks_.fixtureMidiTrackId()) == 0)
            {
                failReason = "no clip to select on the MIDI row";
                return false;
            }
            hooks_.copyThenPasteSelectedMidiClipLikeUi();
            return true;
        },
        kSettleDefaultMs });
    steps_.push_back(Step{ "midi-editor-move: assert paste did not open the editor",
                           [this](juce::String& failReason) -> bool {
                               if (hooks_.isMidiEditorOpen())
                               {
                                   failReason = "paste opened the MIDI editor (symptom B)";
                                   return false;
                               }
                               return true;
                           },
                           kSettleDefaultMs });

    // --- Symptom C: open the editor on the clip, then move it cross-track ---
    steps_.push_back(Step{ "midi-editor-move: open editor on the MIDI row's clip",
                           [this](juce::String& failReason) -> bool {
                               if (!hooks_.openMidiEditorOnFirstClipOfTrack(hooks_.fixtureMidiTrackId()))
                               {
                                   failReason = "could not open the MIDI editor on the clip";
                                   return false;
                               }
                               return true;
                           },
                           800 });
    steps_.push_back(Step{ "midi-editor-move: verify editor open",
                           [this](juce::String& failReason) -> bool {
                               if (!hooks_.isMidiEditorOpen())
                               {
                                   failReason = "editor did not open";
                                   return false;
                               }
                               return true;
                           },
                           kSettleDefaultMs });
    steps_.push_back(Step{
        "midi-editor-move: move the open clip cross-track to the instrument row",
        [this](juce::String& failReason) -> bool {
            const InstrumentMidiClipId clipId = hooks_.selectFirstClipOnTrack(hooks_.fixtureMidiTrackId());
            if (clipId == 0)
            {
                failReason = "clip vanished before the move";
                return false;
            }
            if (!hooks_.moveMidiClipCrossTrackLikeUi(
                    hooks_.fixtureMidiTrackId(), hooks_.fixtureInstrumentTrackId(), clipId))
            {
                failReason = "cross-track move refused";
                return false;
            }
            return true;
        },
        kSettleDefaultMs });
    // The settle above and this extra window let the controller's ASYNC change message dispatch -
    // this is exactly where the pre-fix use-after-free crashed. Reaching the next step proves the
    // process survived it.
    steps_.push_back(Step{ "midi-editor-move: settle after move (async change dispatch)",
                           [](juce::String&) -> bool { return true; },
                           800 });
    // Force the roll rebuild that reads the editor's bound clip (syncInstrumentStateFromHost ->
    // pushRowsModeToRoll / setSessionTimelineContext). Pre-fix, the clip was freed by the move and
    // this dereferenced it (the crash); post-fix the editor detached, so it is safe.
    steps_.push_back(Step{ "midi-editor-move: refresh open editor after move (crash trigger)",
                           [this](juce::String&) -> bool {
                               if (hooks_.refreshInstrumentEditorUi != nullptr)
                               {
                                   hooks_.refreshInstrumentEditorUi();
                               }
                               return true;
                           },
                           600 });
    steps_.push_back(Step{ "midi-editor-move: verify survived + editor detached from moved clip",
                           [this](juce::String& failReason) -> bool {
                               // The edited clip left this track, so the editor must have detached
                               // (its bound clip id no longer resolves here).
                               if (hooks_.clipCountOnTrack(hooks_.fixtureInstrumentTrackId()) < 1)
                               {
                                   failReason = "moved clip did not arrive on the instrument row";
                                   return false;
                               }
                               return true;
                           },
                           kSettleDefaultMs });

    // Audio must still be healthy after the whole sequence.
    steps_.push_back(Step{ "midi-editor-move: start playback (probe armed)",
                           [this](juce::String&) -> bool {
                               hooks_.setPlaybackActive(true);
                               hooks_.audioHealthProbeBegin();
                               return true;
                           },
                           2500 });
    steps_.push_back(Step{ "midi-editor-move: verify audio health after sequence",
                           [this](juce::String& failReason) -> bool {
                               return hooks_.audioHealthProbeVerify("after-move-playing", failReason);
                           },
                           kSettleDefaultMs });
    steps_.push_back(Step{ "midi-editor-move: stop playback",
                           [this](juce::String&) -> bool {
                               hooks_.setPlaybackActive(false);
                               return true;
                           },
                           600 });
}

// -----------------------------------------------------------------------------
// Performance profile (`--stability-perf-profile <project>`)
// -----------------------------------------------------------------------------
namespace
{
    /// Process + system CPU / memory as seen from the message thread (Win32; diagnostics only).
    struct PerfProcessSample
    {
        bool valid = false;
        double processCpuMs = 0.0;   ///< kernel + user time of this process
        double systemBusyMs = 0.0;   ///< all cores: kernel (incl. idle) + user − idle
        double systemTotalMs = 0.0;  ///< all cores: kernel + user
        double wallMs = 0.0;
        std::uint64_t workingSetBytes = 0;
        std::uint64_t privateBytes = 0;
        std::uint64_t pageFaults = 0; ///< soft + hard (Win32 does not separate them here)
    };

    [[nodiscard]] PerfProcessSample samplePerfProcess() noexcept
    {
        PerfProcessSample s;
#if JUCE_WINDOWS
        FILETIME create{}, exitT{}, kernel{}, user{};
        FILETIME sIdle{}, sKernel{}, sUser{};
        const auto toMs = [](const FILETIME& ft) noexcept -> double {
            ULARGE_INTEGER u;
            u.LowPart = ft.dwLowDateTime;
            u.HighPart = ft.dwHighDateTime;
            return (double)u.QuadPart / 10000.0; // 100 ns units
        };
        if (::GetProcessTimes(::GetCurrentProcess(), &create, &exitT, &kernel, &user) != 0)
        {
            s.processCpuMs = toMs(kernel) + toMs(user);
            s.valid = true;
        }
        if (::GetSystemTimes(&sIdle, &sKernel, &sUser) != 0)
        {
            s.systemTotalMs = toMs(sKernel) + toMs(sUser);
            s.systemBusyMs = s.systemTotalMs - toMs(sIdle);
        }
        PROCESS_MEMORY_COUNTERS_EX pmc{};
        pmc.cb = sizeof(pmc);
        if (::K32GetProcessMemoryInfo(::GetCurrentProcess(),
                                      reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc)) != 0)
        {
            s.workingSetBytes = (std::uint64_t)pmc.WorkingSetSize;
            s.privateBytes = (std::uint64_t)pmc.PrivateUsage;
            s.pageFaults = (std::uint64_t)pmc.PageFaultCount;
        }
#endif
        s.wallMs = juce::Time::getMillisecondCounterHiRes();
        return s;
    }

    [[nodiscard]] juce::String perfMb(const std::uint64_t bytes)
    {
        return juce::String((double)bytes / (1024.0 * 1024.0), 0) + " MB";
    }
} // namespace

void StabilityScenarioRunner::appendPerfProfileSteps(const StabilityScenarioRequest& request)
{
    const juce::File project = request.projectA;
    const int measureSeconds = request.perfSeconds;
    const int warmupSeconds = request.perfWarmupSeconds;
    const double startSeconds = request.perfStartSeconds;
    const int requestedBuffer = request.perfRequestedBufferSize;
    const bool mixerOpen = request.perfMixerOpen;
    const bool profilerOn = !request.perfProfilerOff;

    auto say = [this](const juce::String& s) { appendStabilityRunLine("  " + s); };
    auto baseline = std::make_shared<PerfProcessSample>();
    auto underrunBaseline = std::make_shared<std::int64_t>(0);
    auto playheadBaseline = std::make_shared<std::int64_t>(0);
    auto mixerOpenedHere = std::make_shared<bool>(false);

    appendLoadAndVerifySteps(project, "perf-profile");

    steps_.push_back(Step{ "perf-profile: settle after load (plug-ins / proxies ready)",
                           [](juce::String&) -> bool { return true; },
                           8000 });

    steps_.push_back(Step{ "perf-profile: runtime conditions" + juce::String(requestedBuffer > 0 ? " + buffer request" : ""),
                           [this, say, requestedBuffer](juce::String& failReason) -> bool {
                               if (!hooks_.perf.describeRuntime)
                               {
                                   failReason = "perf hooks not installed";
                                   return false;
                               }
                               say(hooks_.perf.describeRuntime());
                               if (requestedBuffer > 0 && hooks_.perf.requestDeviceBufferSize)
                               {
                                   juce::String detail;
                                   const int actual = hooks_.perf.requestDeviceBufferSize(requestedBuffer, detail);
                                   say("buffer request " + juce::String(requestedBuffer) + " -> actual "
                                       + juce::String(actual) + " | " + detail);
                               }
                               return true;
                           },
                           requestedBuffer > 0 ? 4000 : 300 });

    steps_.push_back(Step{ "perf-profile: project load (loaded vs processed)",
                           [this, say](juce::String&) -> bool {
                               if (hooks_.perf.describeRuntime)
                               {
                                   say(hooks_.perf.describeRuntime()); // after a possible device restart
                               }
                               if (hooks_.perf.describeProjectLoad)
                               {
                                   say(hooks_.perf.describeProjectLoad());
                               }
                               return true;
                           },
                           300 });

    steps_.push_back(Step{ juce::String("perf-profile: mixer window ") + (mixerOpen ? "OPEN" : "closed"),
                           [this, say, mixerOpen, mixerOpenedHere](juce::String&) -> bool {
                               const auto& M = hooks_.mixer;
                               if (!M.toggleLikeF3 || !M.isVisible)
                               {
                                   say("mixer hooks absent; window state unchanged");
                                   return true;
                               }
                               if (M.isVisible() != mixerOpen)
                               {
                                   M.toggleLikeF3();
                                   *mixerOpenedHere = mixerOpen;
                               }
                               say(juce::String("mixer visible=") + (M.isVisible() ? "yes" : "no"));
                               return true;
                           },
                           mixerOpen ? 1500 : 300 });

    steps_.push_back(Step{ "perf-profile: start playback (warm-up " + juce::String(warmupSeconds) + " s)",
                           [this, say, startSeconds, profilerOn](juce::String&) -> bool {
                               const double sr = hooks_.getDeviceSampleRate ? hooks_.getDeviceSampleRate() : 48000.0;
                               const auto startSample = (std::int64_t)(startSeconds * (sr > 0.0 ? sr : 48000.0));
                               if (hooks_.seekTransportTo)
                               {
                                   hooks_.seekTransportTo(startSample);
                               }
                               say("start at " + juce::String(startSeconds, 2) + " s (sample " + juce::String(startSample)
                                   + ") cycle=" + (hooks_.isCycleEnabled && hooks_.isCycleEnabled() ? "on" : "off")
                                   + " profiler=" + (profilerOn ? "ON" : "off"));
                               if (hooks_.perf.setProfilerEnabled)
                               {
                                   hooks_.perf.setProfilerEnabled(profilerOn);
                               }
                               hooks_.setPlaybackActive(true);
                               return true;
                           },
                           juce::jmax(200, warmupSeconds * 1000) });

    steps_.push_back(Step{ "perf-profile: measurement window begins (" + juce::String(measureSeconds) + " s)",
                           [this, say, baseline, underrunBaseline, playheadBaseline](juce::String&) -> bool {
                               if (hooks_.perf.resetMeasurementWindows)
                               {
                                   hooks_.perf.resetMeasurementWindows();
                               }
                               int proxySelected = 0;
                               *underrunBaseline = hooks_.perf.proxyUnderrunTotal ? hooks_.perf.proxyUnderrunTotal(proxySelected) : 0;
                               *playheadBaseline = hooks_.getTransportPlayheadSamples ? hooks_.getTransportPlayheadSamples() : 0;
                               *baseline = samplePerfProcess();
                               say("window start: playhead=" + juce::String(*playheadBaseline)
                                   + " proxyUnderrunsSoFar=" + juce::String(*underrunBaseline)
                                   + " proxySelected=" + juce::String(proxySelected)
                                   + " workingSet=" + perfMb(baseline->workingSetBytes)
                                   + " private=" + perfMb(baseline->privateBytes));
                               return true;
                           },
                           juce::jmax(1000, measureSeconds * 1000) });

    steps_.push_back(Step{ "perf-profile: collect",
                           [this, say, baseline, underrunBaseline, playheadBaseline, measureSeconds, profilerOn](juce::String&) -> bool {
                               const PerfProcessSample now = samplePerfProcess();
                               const double wall = now.wallMs - baseline->wallMs;
                               const int cores = juce::SystemStats::getNumCpus();
                               const double procCpuPctOfOneCore = wall > 0.0 ? 100.0 * (now.processCpuMs - baseline->processCpuMs) / wall : 0.0;
                               const double sysBusyPct = (now.systemTotalMs - baseline->systemTotalMs) > 0.0
                                                             ? 100.0 * (now.systemBusyMs - baseline->systemBusyMs)
                                                                   / (now.systemTotalMs - baseline->systemTotalMs)
                                                             : 0.0;
                               const std::int64_t playheadNow = hooks_.getTransportPlayheadSamples ? hooks_.getTransportPlayheadSamples() : 0;
                               say("window end: wall=" + juce::String(wall / 1000.0, 2) + " s playhead=" + juce::String(playheadNow)
                                   + " (started " + juce::String(*playheadBaseline) + ")");
                               if (hooks_.perf.audioLoadText)
                               {
                                   say("engine: " + hooks_.perf.audioLoadText());
                               }
                               const float peak = hooks_.readOutputPeakHoldAndReset ? hooks_.readOutputPeakHoldAndReset() : -1.0f;
                               juce::String master;
                               if (hooks_.drainMasterMeter)
                               {
                                   const StabilityLevelStats m = hooks_.drainMasterMeter();
                                   master = " masterPeakL/R=" + juce::String(m.peak[0], 3) + "/" + juce::String(m.peak[1], 3)
                                            + " oversL/R=" + juce::String((int)m.overs[0]) + "/" + juce::String((int)m.overs[1])
                                            + " rmsL/R=" + juce::String(m.rms[0], 4) + "/" + juce::String(m.rms[1], 4)
                                            + " nonFinite=" + juce::String((int)m.nonFinite);
                               }
                               say("output: peakHold=" + juce::String(peak, 3) + master);
                               int proxySelected = 0;
                               const std::int64_t underruns = hooks_.perf.proxyUnderrunTotal ? hooks_.perf.proxyUnderrunTotal(proxySelected) : 0;
                               say("proxy: selectedDestinations=" + juce::String(proxySelected)
                                   + " underrunsInWindow=" + juce::String(underruns - *underrunBaseline)
                                   + " (total " + juce::String(underruns) + ")");
                               if (hooks_.perf.instrumentActivityText)
                               {
                                   say("instruments: " + hooks_.perf.instrumentActivityText());
                               }
                               say("process: cpu=" + juce::String(procCpuPctOfOneCore, 1) + "% of one core ("
                                   + juce::String(cores > 0 ? procCpuPctOfOneCore / (double)cores : 0.0, 1) + "% of " + juce::String(cores)
                                   + " cores) systemBusy=" + juce::String(sysBusyPct, 1) + "%"
                                   + " workingSet=" + perfMb(now.workingSetBytes) + " private=" + perfMb(now.privateBytes)
                                   + " pageFaultsInWindow=" + juce::String((juce::int64)(now.pageFaults - baseline->pageFaults))
                                   + " (soft+hard; " + juce::String(wall > 0.0 ? (double)(now.pageFaults - baseline->pageFaults) * 1000.0 / wall : 0.0, 0)
                                   + "/s)");
                               if (profilerOn && hooks_.perf.profilerReportText)
                               {
                                   const juce::String report = hooks_.perf.profilerReportText(12);
                                   juce::StringArray lines;
                                   lines.addLines(report);
                                   for (const auto& l : lines)
                                   {
                                       if (l.isNotEmpty())
                                       {
                                           say("profile: " + l);
                                       }
                                   }
                               }
                               juce::ignoreUnused(measureSeconds);
                               return true;
                           },
                           200 });

    steps_.push_back(Step{ "perf-profile: stop playback, profiler off",
                           [this, mixerOpenedHere](juce::String&) -> bool {
                               hooks_.setPlaybackActive(false);
                               if (hooks_.perf.setProfilerEnabled)
                               {
                                   hooks_.perf.setProfilerEnabled(false);
                               }
                               if (*mixerOpenedHere && hooks_.mixer.isVisible && hooks_.mixer.isVisible() && hooks_.mixer.toggleLikeF3)
                               {
                                   hooks_.mixer.toggleLikeF3();
                               }
                               return true;
                           },
                           800 });
}
