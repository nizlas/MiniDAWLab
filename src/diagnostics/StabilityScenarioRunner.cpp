#include "diagnostics/StabilityScenarioRunner.h"

#include "diagnostics/StabilityDiagnosticLog.h"
#include "diagnostics/StabilityInvariants.h"

#include <atomic>
#include <cstdlib>
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

void StabilityScenarioRunner::appendMixdownSteps(const juce::File& project, const bool mp3)
{
    appendLoadAndVerifySteps(project, "mixdown setup");

    const juce::File out
        = juce::File::getSpecialLocation(juce::File::tempDirectory)
              .getChildFile(mp3 ? "dal-stability-mixdown.mp3" : "dal-stability-mixdown.wav");

    steps_.push_back(Step{
        juce::String("mixdown: export ") + (mp3 ? "mp3" : "wav") + " to temp",
        [this, out, mp3](juce::String& failReason) -> bool {
            (void)out.deleteFile();
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
            appendStabilityRunLine("  mixdown output ok: " + out.getFullPathName() + " ("
                                   + juce::String(out.getSize()) + " bytes)");
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
