#include "diagnostics/StabilityScenarioRunner.h"

#include "diagnostics/StabilityDiagnosticLog.h"
#include "diagnostics/StabilityInvariants.h"

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
    // the run started — a matrix must never silently pass with invariant failures.
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
    resumeAtMs_ = nowMs() + step.settleMsAfter;
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
//   realtime  — device-output peak hold while the transport plays a steady tone, 0 dB vs −24 dB,
//               with the −24 dB value set WHILE PLAYING (live update + ramp must both work);
//   offline   — RMS of the mixdown WAV at 0 dB vs −24 dB.
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
// Report under test: "pre-gain 0 → -24 dB on the AmpliTube audio track changed nothing while the
// recorded guitar played". Every step below goes through the production UI / session objects of
// the running app (no direct session setter for the edit):
//   activate the track like a header click → type "-24" + Return into the Inspector field →
//   compare displayed text, committed snapshot value and the track id it landed on →
//   measure, while the transport plays, the level entering the first insert and leaving the last
//   one (PluginInsertHost level tap) — first in the project's own state, then with the track
//   unmuted — so "which audio path is actually heard" is answered by numbers, not assumptions.
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

    // Phase B.1: same fixture, offline mixdown path — the capture sink must see equivalent
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
    /// notes on channel 1 — the same shape as a typical single-track export.
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
    // The settle above and this extra window let the controller's ASYNC change message dispatch —
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
