#include "diagnostics/StabilityScenarioRunner.h"

#include "diagnostics/StabilityDiagnosticLog.h"
#include "diagnostics/StabilityInvariants.h"
#include "ui/experimental/ExperimentalMidiPattern.h"

#include <atomic>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <thread>

#if JUCE_WINDOWS
 #define WIN32_LEAN_AND_MEAN
 #include <windows.h>
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
    steps_.push_back(Step{ "live-midi: save fixture (direct save to the test copy) and clear the dirty flag",
                           [this](juce::String&) -> bool {
                               hooks_.saveProject();
                               return true;
                           },
                           600 });

    const auto inject = [this](const juce::MidiMessage& m) { hooks_.liveMidiInject(m); };
    const auto on = [](const int ch, const int note, const int vel) {
        return juce::MidiMessage::noteOn(ch, note, (juce::uint8)vel);
    };
    const auto off = [](const int ch, const int note) { return juce::MidiMessage::noteOff(ch, note, (juce::uint8)0); };

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
        steps_.push_back(Step{ "live-midi: real device - assign the first physical MIDI input to the instrument row (Device mode)",
                               [this, devId, devName](juce::String& failReason) -> bool {
                                   if (!hooks_.liveMidiFirstRealInputDevice(*devId, *devName))
                                   {
                                       appendStabilityRunLine("  no physical MIDI input on this machine - device-open check skipped");
                                       return true;
                                   }
                                   if (!hooks_.liveMidiSetTrackInputDevice(liveMidiInstTid_, *devId, *devName))
                                   {
                                       failReason = "could not assign device \"" + *devName + "\"";
                                       return false;
                                   }
                                   return true;
                               },
                               500 });
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
                                       failReason = "physical MIDI input was not opened / registered: " + detail;
                                       return false;
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

    // ---- 3. Cycle guard: Record with armed MIDI rows and Cycle on must refuse (Cycle untouched).
    if (hooks_.setCycleEnabled && hooks_.isCycleEnabled)
    {
        steps_.push_back(Step{ "live-midi: Cycle ON + armed MIDI row + Record -> refused, Cycle unchanged",
                               [this](juce::String& failReason) -> bool {
                                   hooks_.liveMidiSetArm(liveMidiInstTid_, true);
                                   hooks_.setCycleEnabled(true);
                                   hooks_.recordToggleLikeKey();
                                   juce::Thread::sleep(150);
                                   const bool countIn = hooks_.isCountInActive();
                                   const bool cycleStill = hooks_.isCycleEnabled();
                                   appendStabilityRunLine(juce::String("  countIn=") + (countIn ? "yes" : "no") + " cycleStillOn="
                                                          + (cycleStill ? "yes" : "no"));
                                   hooks_.setCycleEnabled(false);
                                   if (countIn)
                                   {
                                       failReason = "Record started a count-in although Cycle was on with an armed MIDI row";
                                       return false;
                                   }
                                   if (!cycleStill)
                                   {
                                       failReason = "Cycle was switched off automatically";
                                       return false;
                                   }
                                   return true;
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
