// =============================================================================
// StabilityScenarioRunner — staged load progress + Duplicate Track / vertical scrollbar
// =============================================================================
// Two scenarios of the in-process runner (see StabilityScenarioRunner.h), kept in their own
// translation unit:
//   --stability-load-progress <project>   : the Interactive (message-loop-driven) staged load
//   --stability-duplicate-track <project> : Duplicate Track + the arrangement's vertical scrollbar
// Both operate on a sibling copy of the given project; nothing of the user's project changes.
// =============================================================================

#include "diagnostics/StabilityScenarioRunner.h"

#include "diagnostics/StabilityDiagnosticLog.h"

#include <cmath>
#include <limits>
#include <memory>

namespace
{
    constexpr int kSettleDefaultMs = 250;
    constexpr int kSettleAfterLoadMs = 1200;

    /// 25 ms message-loop probe: the longest gap between two ticks is the longest stretch the UI
    /// could not paint or react (Windows shows "Not responding" after ~5 s of that).
    class MessageLoopStallProbe final : private juce::Timer
    {
    public:
        void begin(std::function<juce::String()> phaseProvider)
        {
            phaseProvider_ = std::move(phaseProvider);
            last_ = juce::Time::getMillisecondCounterHiRes();
            maxGapMs_ = 0.0;
            gapsOver500_ = 0;
            gapsOver2000_ = 0;
            gapsOver5000_ = 0;
            ticks_ = 0;
            phaseAtMaxGap_.clear();
            lastPhase_.clear();
            startTimer(25);
        }
        void end() { stopTimer(); }
        [[nodiscard]] juce::String report() const
        {
            juce::String s;
            s << "ticks=" << ticks_ << " maxGapMs=" << juce::String(maxGapMs_, 0) << " gaps>500ms=" << gapsOver500_
              << " gaps>2000ms=" << gapsOver2000_ << " gaps>5000ms=" << gapsOver5000_ << " phaseBeforeMaxGap=\""
              << phaseAtMaxGap_ << "\"";
            return s;
        }
        [[nodiscard]] double maxGapMs() const noexcept { return maxGapMs_; }
        [[nodiscard]] int gapsOver5000() const noexcept { return gapsOver5000_; }

    private:
        void timerCallback() override
        {
            const double now = juce::Time::getMillisecondCounterHiRes();
            const double gap = now - last_;
            last_ = now;
            ++ticks_;
            if (gap > maxGapMs_)
            {
                maxGapMs_ = gap;
                phaseAtMaxGap_ = lastPhase_;
            }
            if (gap > 500.0) { ++gapsOver500_; }
            if (gap > 2000.0) { ++gapsOver2000_; }
            if (gap > 5000.0) { ++gapsOver5000_; }
            if (phaseProvider_)
            {
                lastPhase_ = phaseProvider_();
            }
        }
        std::function<juce::String()> phaseProvider_;
        double last_ = 0.0;
        double maxGapMs_ = 0.0;
        int gapsOver500_ = 0;
        int gapsOver2000_ = 0;
        int gapsOver5000_ = 0;
        int ticks_ = 0;
        juce::String phaseAtMaxGap_;
        juce::String lastPhase_;
    };

    [[nodiscard]] juce::File evidenceDirFor(const StabilityScenarioRequest& request, const char* scenario)
    {
        juce::File dir = request.evidenceDir;
        if (dir == juce::File{})
        {
            dir = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile(juce::String("dal-") + scenario + "-evidence");
        }
        (void) dir.createDirectory();
        return dir;
    }

    [[nodiscard]] bool writeTextFile(const juce::File& f, const juce::String& text)
    {
        f.getParentDirectory().createDirectory();
        return f.replaceWithText(text);
    }

    /// Clip ids of a `describeSessionRow` line ("id=<n>" tokens inside the clips list).
    [[nodiscard]] juce::StringArray clipIdsFromRowDescription(const juce::String& row)
    {
        juce::StringArray ids;
        const juce::String clips = row.fromFirstOccurrenceOf("clips=[", false, false);
        int pos = 0;
        while (true)
        {
            const int at = clips.indexOf(pos, "(id=");
            if (at < 0)
            {
                break;
            }
            const int end = clips.indexOf(at + 4, " ");
            if (end < 0)
            {
                break;
            }
            ids.add(clips.substring(at + 4, end));
            pos = end;
        }
        return ids;
    }

    [[nodiscard]] juce::String rowNameFromDescription(const juce::String& row)
    {
        return row.fromFirstOccurrenceOf("name=\"", false, false).upToFirstOccurrenceOf("\"", false, false);
    }

    [[nodiscard]] int rowIndexFromDescription(const juce::String& row)
    {
        return row.fromFirstOccurrenceOf("index=", false, false).getIntValue();
    }

    [[nodiscard]] juce::String insertRowsText(const std::vector<StabilityInsertRowInfo>& rows)
    {
        juce::String s;
        for (const StabilityInsertRowInfo& r : rows)
        {
            s << (r.pre ? "Pre:" : "Post:") << r.displayName << (r.unavailable ? "[unavailable]" : "") << " ";
        }
        return s.trim();
    }

    [[nodiscard]] int modelValue(const juce::String& model, const char* key)
    {
        return model.fromFirstOccurrenceOf(juce::String(key) + "=", false, false).upToFirstOccurrenceOf(" ", false, false).getIntValue();
    }

    /// `insertChainDigest` is "identity=<sha> state=<sha> sizes=<n,..> slots=N". A faithful copy of
    /// a LIVE chain has the same identity part and non-empty state wherever the source has state
    /// (a live plug-in may re-serialize its state after a restore, so bytes are not compared).
    [[nodiscard]] juce::String chainIdentityPart(const juce::String& digest)
    {
        return digest.upToFirstOccurrenceOf(" state=", false, false) + " slots=" + digest.fromFirstOccurrenceOf("slots=", false, false);
    }
    [[nodiscard]] bool chainStatePresenceMatches(const juce::String& srcDigest, const juce::String& copyDigest)
    {
        juce::StringArray a, b;
        a.addTokens(srcDigest.fromFirstOccurrenceOf("sizes=", false, false).upToFirstOccurrenceOf(" ", false, false), ",", "");
        b.addTokens(copyDigest.fromFirstOccurrenceOf("sizes=", false, false).upToFirstOccurrenceOf(" ", false, false), ",", "");
        if (a.size() != b.size())
        {
            return false;
        }
        for (int i = 0; i < a.size(); ++i)
        {
            if ((a[i].getIntValue() > 0) != (b[i].getIntValue() > 0))
            {
                return false;
            }
        }
        return true;
    }
} // namespace

// =============================================================================
// --stability-load-progress
// =============================================================================
void StabilityScenarioRunner::appendLoadProgressSteps(const StabilityScenarioRequest& request)
{
    const juce::File project = request.projectA;
    if (!hooks_.load.beginInteractiveLoad || !hooks_.load.isLoadInProgress || !hooks_.load.progressText
        || !hooks_.load.captureProgressWindowPng || !hooks_.isProjectDirty || !hooks_.getTrackCount)
    {
        steps_.push_back(Step{ "load-progress: hooks missing",
                               [](juce::String& failReason) -> bool {
                                   failReason = "load-progress hooks not installed";
                                   return false;
                               },
                               0 });
        return;
    }

    const juce::File evidence = evidenceDirFor(request, "load-progress");
    struct State
    {
        juce::File copy;
        juce::File corrupt;
        std::shared_ptr<MessageLoopStallProbe> probe = std::make_shared<MessageLoopStallProbe>();
        juce::int64 startMs = 0;
        juce::String lastText;
        juce::StringArray phasesSeen;
        bool capturedReading = false, capturedAudio = false, capturedInstruments = false, capturedEffects = false,
             capturedFinalizing = false, capturedFirst = false, capturedInstrumentsMid = false, capturedEffectsMid = false;
        bool windowSeenShowing = false;
        bool gatesChecked = false;
        bool done = false;
        bool fullFractionWhileLoading = false;
        double maxFractionWhileLoading = -1.0;
        int tracksAfterFirstLoad = 0;
        int pollsUsed = 0;
        juce::int64 copyMtimeBeforeSaveAttempt = 0;
        juce::String report;
    };
    auto st = std::make_shared<State>();

    const auto phaseOf = [](const juce::String& text) { return text.upToFirstOccurrenceOf(" |", false, false).trim(); };
    const auto fractionOf = [](const juce::String& text) -> double {
        // "<phase> | <detail> | 37.5% | elapsedMs=.. | window=.."
        juce::StringArray parts;
        parts.addTokens(text, "|", "");
        if (parts.size() < 3)
        {
            return -1.0;
        }
        const juce::String frac = parts[2].trim();
        if (frac.startsWith("indeterminate"))
        {
            return -1.0;
        }
        return frac.upToFirstOccurrenceOf("%", false, false).getDoubleValue() / 100.0;
    };

    steps_.push_back(Step{ "load-progress: copy project to sibling test file",
                           [this, project, st, evidence](juce::String& failReason) -> bool {
                               const juce::File copy = project.getSiblingFile(project.getFileNameWithoutExtension() + "-loadprogress.dalproj");
                               (void) copy.deleteFile();
                               if (!project.copyFileTo(copy))
                               {
                                   failReason = "could not copy project to " + copy.getFullPathName();
                                   return false;
                               }
                               openSaveCloseCopy_ = copy;
                               st->copy = copy;
                               appendStabilityRunLine("  test copy: " + copy.getFullPathName());
                               appendStabilityRunLine("  evidence: " + evidence.getFullPathName());
                               return true;
                           },
                           kSettleDefaultMs });

    steps_.push_back(Step{ "load-progress: begin the Interactive staged load (returns at once) + start the message-loop probe",
                           [this, st, evidence, phaseOf](juce::String& failReason) -> bool {
                               st->probe->begin([this] { return hooks_.load.progressText(); });
                               st->startMs = (juce::int64) juce::Time::getMillisecondCounterHiRes();
                               hooks_.load.beginInteractiveLoad(st->copy);
                               const juce::String text = hooks_.load.progressText();
                               appendStabilityRunLine("  right after begin: inProgress=" + juce::String(hooks_.load.isLoadInProgress() ? 1 : 0)
                                                      + " progress=\"" + text + "\"");
                               if (!hooks_.load.isLoadInProgress())
                               {
                                   failReason = "load did not start (or finished synchronously - Interactive drive expected)";
                                   return false;
                               }
                               if (text.isEmpty())
                               {
                                   failReason = "no progress text right after begin";
                                   return false;
                               }
                               st->phasesSeen.addIfNotAlreadyThere(phaseOf(text));
                               st->lastText = text;
                               if (text.contains("window=showing"))
                               {
                                   st->windowSeenShowing = true;
                                   if (hooks_.load.captureProgressWindowPng(evidence.getChildFile("progress-00-reading.png")))
                                   {
                                       st->capturedReading = true;
                                   }
                               }
                               return true;
                           },
                           150 });

    constexpr int kMaxPolls = 600; // 600 x 300 ms = 3 min upper bound for a 100-track project
    for (int poll = 1; poll <= kMaxPolls; ++poll)
    {
        steps_.push_back(Step{ "load-progress: poll " + juce::String(poll),
                               [this, st, evidence, poll, phaseOf, fractionOf](juce::String& failReason) -> bool {
                                   if (st->done)
                                   {
                                       settleOverrideMsForCurrentStep_ = 1;
                                       return true;
                                   }
                                   st->pollsUsed = poll;
                                   const bool inProgress = hooks_.load.isLoadInProgress();
                                   const juce::String text = hooks_.load.progressText();
                                   if (inProgress)
                                   {
                                       if (text != st->lastText)
                                       {
                                           appendStabilityRunLine("  t+" + juce::String((juce::int64) juce::Time::getMillisecondCounterHiRes() - st->startMs)
                                                                  + "ms progress=\"" + text + "\"");
                                           st->lastText = text;
                                       }
                                       st->phasesSeen.addIfNotAlreadyThere(phaseOf(text));
                                       if (text.contains("window=showing"))
                                       {
                                           st->windowSeenShowing = true;
                                       }
                                       const double frac = fractionOf(text);
                                       if (frac >= 0.0)
                                       {
                                           st->maxFractionWhileLoading = juce::jmax(st->maxFractionWhileLoading, frac);
                                           if (frac >= 1.0)
                                           {
                                               st->fullFractionWhileLoading = true;
                                           }
                                       }
                                       const juce::String phase = phaseOf(text).toLowerCase();
                                       auto captureOnce = [&](bool& flag, const char* name) {
                                           if (!flag && hooks_.load.captureProgressWindowPng(evidence.getChildFile(name)))
                                           {
                                               flag = true;
                                               appendStabilityRunLine(juce::String("  PNG: ") + name + " (" + text + ")");
                                           }
                                       };
                                       if (!st->capturedFirst) { captureOnce(st->capturedFirst, "progress-01-first-poll.png"); }
                                       if (phase.contains("audio")) { captureOnce(st->capturedAudio, "progress-02-audio.png"); }
                                       if (phase.contains("instrument")) { captureOnce(st->capturedInstruments, "progress-03-instruments.png"); }
                                       if (phase.contains("instrument") && frac >= 0.4) { captureOnce(st->capturedInstrumentsMid, "progress-03b-instruments-mid.png"); }
                                       if (phase.contains("effect")) { captureOnce(st->capturedEffects, "progress-04-effects.png"); }
                                       if (phase.contains("effect") && frac >= 0.4) { captureOnce(st->capturedEffectsMid, "progress-04b-effects-mid.png"); }
                                       if (phase.contains("finaliz")) { captureOnce(st->capturedFinalizing, "progress-05-finalizing.png"); }

                                       if (!st->gatesChecked && poll >= 2)
                                       {
                                           st->gatesChecked = true;
                                           juce::String g;
                                           const bool dirty = hooks_.isProjectDirty();
                                           g << "dirty=" << (dirty ? 1 : 0);
                                           bool structuralBlocked = true;
                                           if (hooks_.load.isStructuralEditBlocked)
                                           {
                                               structuralBlocked = hooks_.load.isStructuralEditBlocked();
                                               g << " structuralEditBlocked=" << (structuralBlocked ? 1 : 0);
                                           }
                                           bool playing = false;
                                           if (hooks_.load.togglePlayLikeButton && hooks_.load.isTransportPlaying)
                                           {
                                               hooks_.load.togglePlayLikeButton();
                                               playing = hooks_.load.isTransportPlaying();
                                               g << " playAfterToggle=" << (playing ? 1 : 0);
                                           }
                                           bool addedTrack = false;
                                           if (hooks_.load.addAudioTrackLikeUi)
                                           {
                                               addedTrack = hooks_.load.addAudioTrackLikeUi().has_value();
                                               g << " addTrackAccepted=" << (addedTrack ? 1 : 0);
                                           }
                                           juce::String recordRefusal;
                                           if (hooks_.recordToggleLikeKey && hooks_.lastRecordStartRefusal)
                                           {
                                               hooks_.recordToggleLikeKey();
                                               recordRefusal = hooks_.lastRecordStartRefusal();
                                               g << " recordRefusal=\"" << recordRefusal << "\"";
                                           }
                                           if (hooks_.invokeUndo)
                                           {
                                               hooks_.invokeUndo();
                                               g << " undoInvoked=1";
                                           }
                                           if (hooks_.saveProject)
                                           {
                                               st->copyMtimeBeforeSaveAttempt = st->copy.getLastModificationTime().toMilliseconds();
                                               hooks_.saveProject();
                                               const bool changed = st->copy.getLastModificationTime().toMilliseconds() != st->copyMtimeBeforeSaveAttempt;
                                               g << " saveWroteFile=" << (changed ? 1 : 0);
                                               if (changed)
                                               {
                                                   failReason = "Save wrote the project file while the load was in progress";
                                                   return false;
                                               }
                                           }
                                           appendStabilityRunLine("  gates while loading: " + g);
                                           if (dirty || !structuralBlocked || playing || addedTrack
                                               || (hooks_.lastRecordStartRefusal && recordRefusal.isEmpty()))
                                           {
                                               failReason = "a gate did not hold while loading: " + g;
                                               return false;
                                           }
                                           if (!hooks_.load.isLoadInProgress())
                                           {
                                               appendStabilityRunLine("  (load finished during the gate checks)");
                                           }
                                       }
                                       return true;
                                   }

                                   // ---- finished
                                   st->done = true;
                                   st->probe->end();
                                   const juce::int64 elapsed = (juce::int64) juce::Time::getMillisecondCounterHiRes() - st->startMs;
                                   st->tracksAfterFirstLoad = hooks_.getTrackCount();
                                   appendStabilityRunLine("  load finished after " + juce::String(elapsed) + " ms, polls=" + juce::String(poll)
                                                          + " tracks=" + juce::String(st->tracksAfterFirstLoad));
                                   appendStabilityRunLine("  message-loop probe: " + st->probe->report());
                                   appendStabilityRunLine("  phases seen: " + st->phasesSeen.joinIntoString(" -> "));
                                   appendStabilityRunLine("  max determinate fraction while loading: "
                                                          + (st->maxFractionWhileLoading < 0.0 ? juce::String("none") : juce::String(st->maxFractionWhileLoading * 100.0, 1) + "%")
                                                          + " reached100%WhileLoading=" + juce::String(st->fullFractionWhileLoading ? 1 : 0));
                                   if (hooks_.load.tailProjectLoadDiagnosticLog)
                                   {
                                       for (const juce::String& line : hooks_.load.tailProjectLoadDiagnosticLog(140))
                                       {
                                           if (line.contains("load:") || line.contains("apply:") || line.contains("unit ")
                                               || line.contains("staged") || line.contains("deferred") || line.contains("elapsedMs"))
                                           {
                                               appendStabilityRunLine("    diag| " + line.trim());
                                           }
                                       }
                                   }
                                   st->report << "project=" << st->copy.getFullPathName() << "\nelapsedMs=" << elapsed
                                              << "\ntracks=" << st->tracksAfterFirstLoad << "\nprobe: " << st->probe->report()
                                              << "\nphases: " << st->phasesSeen.joinIntoString(" -> ") << "\n";
                                   (void) writeTextFile(evidence.getChildFile("load-progress-report.txt"), st->report);
                                   if (hooks_.captureArrangementPng)
                                   {
                                       (void) hooks_.captureArrangementPng(evidence.getChildFile("arrangement-after-load.png"));
                                   }
                                   if (st->tracksAfterFirstLoad <= 0)
                                   {
                                       failReason = "no tracks after the staged load";
                                       return false;
                                   }
                                   if (!st->windowSeenShowing)
                                   {
                                       failReason = "the progress window was never showing while the load ran";
                                       return false;
                                   }
                                   if (st->fullFractionWhileLoading)
                                   {
                                       failReason = "the progress bar reported 100% before the load completed";
                                       return false;
                                   }
                                   if (hooks_.isProjectDirty())
                                   {
                                       failReason = "project is dirty right after the load";
                                       return false;
                                   }
                                   if (hooks_.load.isStructuralEditBlocked && hooks_.load.isStructuralEditBlocked())
                                   {
                                       failReason = "structural edits are still blocked after the load finished";
                                       return false;
                                   }
                                   if (st->probe->gapsOver5000() > 0)
                                   {
                                       appendStabilityRunLine("  WARNING: at least one message-loop gap over 5 s (a single plug-in operation that long "
                                                              "cannot be split by DAL; see phaseBeforeMaxGap)");
                                   }
                                   settleOverrideMsForCurrentStep_ = 300;
                                   return true;
                               },
                               300 });
    }

    steps_.push_back(Step{ "load-progress: verify the load completed within the poll budget",
                           [st](juce::String& failReason) -> bool {
                               if (!st->done)
                               {
                                   failReason = "load still in progress after the poll budget";
                                   return false;
                               }
                               return true;
                           },
                           kSettleDefaultMs });

    // ---- failure path -------------------------------------------------------------------------
    steps_.push_back(Step{ "load-progress: failure path - begin loading a corrupt project file",
                           [this, st](juce::String& failReason) -> bool {
                               st->corrupt = st->copy.getSiblingFile("dal-corrupt-loadprogress.dalproj");
                               if (!writeTextFile(st->corrupt, "{ \"this is\": not a project file "))
                               {
                                   failReason = "could not write the corrupt file";
                                   return false;
                               }
                               hooks_.load.beginInteractiveLoad(st->corrupt);
                               appendStabilityRunLine("  corrupt load begun: inProgress=" + juce::String(hooks_.load.isLoadInProgress() ? 1 : 0));
                               return true;
                           },
                           600 });
    for (int poll = 1; poll <= 20; ++poll)
    {
        steps_.push_back(Step{ "load-progress: failure path - wait " + juce::String(poll) + "/20",
                               [this, st, poll](juce::String& failReason) -> bool {
                                   if (hooks_.load.isLoadInProgress())
                                   {
                                       if (poll == 20)
                                       {
                                           failReason = "corrupt-file load still in progress after 20 polls";
                                           return false;
                                       }
                                       return true;
                                   }
                                   settleOverrideMsForCurrentStep_ = 1;
                                   return true;
                               },
                               300 });
    }
    steps_.push_back(Step{ "load-progress: failure path - gates released, previous project intact",
                           [this, st](juce::String& failReason) -> bool {
                               const int tracks = hooks_.getTrackCount();
                               const bool dirty = hooks_.isProjectDirty();
                               const bool blocked = hooks_.load.isStructuralEditBlocked ? hooks_.load.isStructuralEditBlocked() : false;
                               appendStabilityRunLine("  after failed load: inProgress=" + juce::String(hooks_.load.isLoadInProgress() ? 1 : 0)
                                                      + " tracks=" + juce::String(tracks) + " dirty=" + juce::String(dirty ? 1 : 0)
                                                      + " structuralEditBlocked=" + juce::String(blocked ? 1 : 0)
                                                      + " progress=\"" + hooks_.load.progressText() + "\"");
                               (void) st->corrupt.deleteFile();
                               if (hooks_.load.isLoadInProgress() || dirty || blocked)
                               {
                                   failReason = "gates not released after the failed load";
                                   return false;
                               }
                               if (tracks != st->tracksAfterFirstLoad)
                               {
                                   failReason = "track count changed by a failed load (" + juce::String(tracks) + " vs "
                                                + juce::String(st->tracksAfterFirstLoad) + ")";
                                   return false;
                               }
                               return true;
                           },
                           kSettleDefaultMs });

    // ---- the next open works ------------------------------------------------------------------
    steps_.push_back(Step{ "load-progress: open the good project again (Interactive)",
                           [this, st](juce::String&) -> bool {
                               st->done = false;
                               st->startMs = (juce::int64) juce::Time::getMillisecondCounterHiRes();
                               hooks_.load.beginInteractiveLoad(st->copy);
                               return true;
                           },
                           300 });
    for (int poll = 1; poll <= kMaxPolls; ++poll)
    {
        steps_.push_back(Step{ "load-progress: second load poll " + juce::String(poll),
                               [this, st, poll](juce::String& failReason) -> bool {
                                   if (st->done)
                                   {
                                       settleOverrideMsForCurrentStep_ = 1;
                                       return true;
                                   }
                                   if (hooks_.load.isLoadInProgress())
                                   {
                                       if (poll == kMaxPolls)
                                       {
                                           failReason = "second load still in progress after the poll budget";
                                           return false;
                                       }
                                       return true;
                                   }
                                   st->done = true;
                                   const int tracks = hooks_.getTrackCount();
                                   appendStabilityRunLine("  second load finished after "
                                                          + juce::String((juce::int64) juce::Time::getMillisecondCounterHiRes() - st->startMs)
                                                          + " ms tracks=" + juce::String(tracks));
                                   if (tracks != st->tracksAfterFirstLoad)
                                   {
                                       failReason = "second load has " + juce::String(tracks) + " tracks, first had "
                                                    + juce::String(st->tracksAfterFirstLoad);
                                       return false;
                                   }
                                   settleOverrideMsForCurrentStep_ = 300;
                                   return true;
                               },
                               300 });
    }
}

// =============================================================================
// --stability-duplicate-track
// =============================================================================
void StabilityScenarioRunner::appendDuplicateTrackSteps(const StabilityScenarioRequest& request)
{
    const juce::File project = request.projectA;
    auto& D = hooks_.dup;
    if (!D.duplicateTrackLikeMenu || !D.describeSessionRow || !D.describeSessionRowWithoutIdentity || !D.insertChainDigest
        || !D.insertInstancePointers || !D.instrumentRuntimeIdentity || !D.sessionTrackOrder || !hooks_.listAllTracks
        || !hooks_.listInsertRows || !hooks_.invokeUndo || !hooks_.invokeRedo || !hooks_.undoStackSize
        || !hooks_.getActiveTrackId || !hooks_.activateTrackLikeHeaderClick || !hooks_.isProjectDirty
        || !hooks_.saveProject || !hooks_.loadProjectFromFile || !hooks_.getTrackCount)
    {
        steps_.push_back(Step{ "duplicate-track: hooks missing",
                               [](juce::String& failReason) -> bool {
                                   failReason = "duplicate-track hooks not installed";
                                   return false;
                               },
                               0 });
        return;
    }

    const juce::File evidence = evidenceDirFor(request, "duplicate-track");
    const juce::File delayBundle("C:\\Program Files\\Common Files\\VST3\\DALMonoDelay.vst3");

    struct State
    {
        juce::File copy;
        TrackId audioSrc = kInvalidTrackId, audioCopy = kInvalidTrackId;
        TrackId instSrc = kInvalidTrackId, instCopy = kInvalidTrackId;
        TrackId midiSrc = kInvalidTrackId, midiCopy = kInvalidTrackId;
        TrackId groupSrc = kInvalidTrackId, groupCopy = kInvalidTrackId;
        TrackId placeholderSrc = kInvalidTrackId, placeholderCopy = kInvalidTrackId;
        TrackId otherTid = kInvalidTrackId;
        juce::String audioSrcRowBefore, audioSrcDigestBefore, audioSrcPointersBefore, audioCopyRow, audioCopyDigest;
        juce::String instSrcIdentityBefore;
        int tracksBeforeAudioDup = 0;
        int undoSizeBeforeAudioDup = 0;
        juce::String orderBeforeAudioDup;
        float param0OriginalBefore = std::numeric_limits<float>::quiet_NaN();
        bool paramCheckPossible = false;
        juce::Rectangle<int> windowBoundsAtStart;
        bool windowShrunk = false;
        juce::String scrollModelBeforeDup, scrollModelAfterDup;
        int rowCountAtScrollCheck = 0;
    };
    auto st = std::make_shared<State>();

    const auto trackName = [this](const TrackId tid) -> juce::String {
        for (const StabilityTrackInfo& t : hooks_.listAllTracks())
        {
            if (t.id == tid)
            {
                return t.name;
            }
        }
        return {};
    };
    const auto trackKind = [this](const TrackId tid) -> juce::String {
        for (const StabilityTrackInfo& t : hooks_.listAllTracks())
        {
            if (t.id == tid)
            {
                return t.kindName;
            }
        }
        return {};
    };
    const auto orderIds = [this]() -> juce::StringArray {
        juce::StringArray ids;
        juce::StringArray entries;
        entries.addTokens(hooks_.dup.sessionTrackOrder(), "|", "");
        for (const juce::String& e : entries)
        {
            ids.add(e.upToFirstOccurrenceOf(":", false, false));
        }
        return ids;
    };
    /// Common checks for every duplication: position, name, settings, clips, Monitor / Arm, active.
    const auto verifyCopyCommon = [this, orderIds](const TrackId src, const TrackId copy, const juce::String& label,
                                                  juce::String& failReason, const bool requireActive = true) -> bool {
        const juce::StringArray ids = orderIds();
        const int srcIx = ids.indexOf(juce::String((juce::int64) src));
        const int copyIx = ids.indexOf(juce::String((juce::int64) copy));
        const juce::String srcRow = hooks_.dup.describeSessionRow(src);
        const juce::String copyRow = hooks_.dup.describeSessionRow(copy);
        appendStabilityRunLine("  " + label + " source: " + srcRow);
        appendStabilityRunLine("  " + label + " copy:   " + copyRow);
        if (copyIx != srcIx + 1)
        {
            failReason = label + ": copy is at index " + juce::String(copyIx) + ", source at " + juce::String(srcIx)
                         + " (expected directly below)";
            return false;
        }
        const juce::String srcName = rowNameFromDescription(srcRow);
        const juce::String copyName = rowNameFromDescription(copyRow);
        if (!copyName.startsWith(srcName) || copyName == srcName || !copyName.contains("kopia"))
        {
            failReason = label + ": copy name \"" + copyName + "\" is not derived from \"" + srcName + "\"";
            return false;
        }
        const juce::String a = hooks_.dup.describeSessionRowWithoutIdentity(src);
        const juce::String b = hooks_.dup.describeSessionRowWithoutIdentity(copy);
        if (a != b)
        {
            failReason = label + ": settings / placements differ\n   src=" + a + "\n   copy=" + b;
            return false;
        }
        const juce::StringArray srcClipIds = clipIdsFromRowDescription(srcRow);
        const juce::StringArray copyClipIds = clipIdsFromRowDescription(copyRow);
        if (srcClipIds.size() != copyClipIds.size())
        {
            failReason = label + ": clip count differs";
            return false;
        }
        for (const juce::String& id : copyClipIds)
        {
            if (srcClipIds.contains(id))
            {
                failReason = label + ": copy reuses clip id " + id;
                return false;
            }
        }
        if (requireActive && hooks_.getActiveTrackId() != copy)
        {
            failReason = label + ": the copy is not the active track";
            return false;
        }
        if (hooks_.dup.isTrackArmed && hooks_.dup.isTrackArmed(copy))
        {
            failReason = label + ": copy is record-armed";
            return false;
        }
        if (hooks_.dup.isTrackMonitored && hooks_.dup.isTrackMonitored(copy))
        {
            failReason = label + ": copy has Monitor on";
            return false;
        }
        return true;
    };

    steps_.push_back(Step{ "duplicate-track: copy project to sibling test file",
                           [this, project, st, evidence](juce::String& failReason) -> bool {
                               const juce::File copy = project.getSiblingFile(project.getFileNameWithoutExtension() + "-duptest.dalproj");
                               (void) copy.deleteFile();
                               if (!project.copyFileTo(copy))
                               {
                                   failReason = "could not copy project to " + copy.getFullPathName();
                                   return false;
                               }
                               openSaveCloseCopy_ = copy;
                               st->copy = copy;
                               appendStabilityRunLine("  test copy: " + copy.getFullPathName());
                               appendStabilityRunLine("  evidence: " + evidence.getFullPathName());
                               if (hooks_.getMainWindowBounds)
                               {
                                   st->windowBoundsAtStart = hooks_.getMainWindowBounds();
                               }
                               return true;
                           },
                           kSettleDefaultMs });
    steps_.push_back(Step{ "duplicate-track: load test copy",
                           [this, st](juce::String&) -> bool {
                               hooks_.loadProjectFromFile(st->copy);
                               return true;
                           },
                           kSettleAfterLoadMs });

    // ---- pick rows + prepare the audio source (Group, routing, send, Post insert, pre-gain) -----
    steps_.push_back(Step{ "duplicate-track: pick rows; prepare the audio source (Group + routing + send + Post insert + pre-gain)",
                           [this, st, delayBundle, trackName](juce::String& failReason) -> bool {
                               if (hooks_.findAudioTrackWithInsertNamed)
                               {
                                   st->audioSrc = hooks_.findAudioTrackWithInsertNamed("AmpliTube");
                               }
                               for (const StabilityTrackInfo& t : hooks_.listAllTracks())
                               {
                                   if (st->audioSrc == kInvalidTrackId && t.kindName == "audio")
                                   {
                                       st->audioSrc = t.id;
                                   }
                               }
                               if (hooks_.firstLoadedInstrumentRow)
                               {
                                   st->instSrc = hooks_.firstLoadedInstrumentRow();
                               }
                               for (const StabilityTrackInfo& t : hooks_.listAllTracks())
                               {
                                   if (st->instSrc == kInvalidTrackId && t.kindName == "instrument")
                                   {
                                       st->instSrc = t.id;
                                   }
                                   if (t.id != st->audioSrc && t.kindName != "master" && st->otherTid == kInvalidTrackId)
                                   {
                                       st->otherTid = t.id;
                                   }
                               }
                               if (st->audioSrc == kInvalidTrackId)
                               {
                                   failReason = "project has no audio track";
                                   return false;
                               }
                               appendStabilityRunLine("  audio source: " + juce::String((juce::int64) st->audioSrc) + " \"" + trackName(st->audioSrc)
                                                      + "\" | instrument source: " + juce::String((juce::int64) st->instSrc) + " \"" + trackName(st->instSrc)
                                                      + "\" | other row: " + juce::String((juce::int64) st->otherTid));
                               if (hooks_.mixer.addGroupTrackLikeUi)
                               {
                                   st->groupSrc = hooks_.mixer.addGroupTrackLikeUi();
                               }
                               if (st->groupSrc == kInvalidTrackId)
                               {
                                   failReason = "could not add a Group track";
                                   return false;
                               }
                               if (hooks_.dup.setRoutedOutput && !hooks_.dup.setRoutedOutput(st->audioSrc, st->groupSrc))
                               {
                                   failReason = "could not route the audio source to the Group";
                                   return false;
                               }
                               if (hooks_.dup.insertSend && !hooks_.dup.insertSend(st->audioSrc, 1, st->groupSrc, 0.4f))
                               {
                                   failReason = "could not add a send on the audio source";
                                   return false;
                               }
                               if (hooks_.setTrackPreGainDb)
                               {
                                   (void) hooks_.setTrackPreGainDb(st->audioSrc, -6.0f);
                               }
                               if (hooks_.addInsertLikePicker && delayBundle.exists())
                               {
                                   const juce::Result r = hooks_.addInsertLikePicker(st->audioSrc, false, delayBundle);
                                   appendStabilityRunLine("  Post insert (DAL Mono Delay) on the source: " + juce::String(r.wasOk() ? "ok" : r.getErrorMessage()));
                               }
                               else
                               {
                                   appendStabilityRunLine("  DAL Mono Delay bundle not present - Post insert step skipped");
                               }
                               appendStabilityRunLine("  source inserts: " + insertRowsText(hooks_.listInsertRows(st->audioSrc)));
                               appendStabilityRunLine("  source row: " + hooks_.dup.describeSessionRow(st->audioSrc));
                               return true;
                           },
                           600 });

    // ---- audio duplication with the source NOT active ----------------------------------------
    steps_.push_back(Step{ "duplicate-track: audio row with inserts + send, source NOT active (another row activated first)",
                           [this, st, verifyCopyCommon](juce::String& failReason) -> bool {
                               if (st->otherTid != kInvalidTrackId)
                               {
                                   hooks_.activateTrackLikeHeaderClick(st->otherTid);
                                   if (hooks_.getActiveTrackId() != st->otherTid)
                                   {
                                       failReason = "could not activate the other row before duplicating";
                                       return false;
                                   }
                               }
                               st->tracksBeforeAudioDup = hooks_.getTrackCount();
                               st->undoSizeBeforeAudioDup = hooks_.undoStackSize();
                               st->orderBeforeAudioDup = hooks_.dup.sessionTrackOrder();
                               st->audioSrcRowBefore = hooks_.dup.describeSessionRow(st->audioSrc);
                               st->audioSrcDigestBefore = hooks_.dup.insertChainDigest(st->audioSrc);
                               st->audioSrcPointersBefore = hooks_.dup.insertInstancePointers(st->audioSrc);
                               if (hooks_.dup.lanesScrollModel)
                               {
                                   st->scrollModelBeforeDup = hooks_.dup.lanesScrollModel();
                               }
                               const bool dirtyBefore = hooks_.isProjectDirty();
                               st->audioCopy = hooks_.dup.duplicateTrackLikeMenu(st->audioSrc);
                               if (st->audioCopy == kInvalidTrackId)
                               {
                                   failReason = "duplicate refused";
                                   return false;
                               }
                               appendStabilityRunLine("  new track id " + juce::String((juce::int64) st->audioCopy) + " (dirty before=" + juce::String(dirtyBefore ? 1 : 0)
                                                      + " after=" + juce::String(hooks_.isProjectDirty() ? 1 : 0) + ")");
                               if (!verifyCopyCommon(st->audioSrc, st->audioCopy, "audio", failReason))
                               {
                                   return false;
                               }
                               if (hooks_.getTrackCount() != st->tracksBeforeAudioDup + 1)
                               {
                                   failReason = "track count did not grow by one";
                                   return false;
                               }
                               if (hooks_.undoStackSize() != st->undoSizeBeforeAudioDup + 1)
                               {
                                   failReason = "duplicate did not record exactly one undo step (before=" + juce::String(st->undoSizeBeforeAudioDup)
                                                + " after=" + juce::String(hooks_.undoStackSize()) + ")";
                                   return false;
                               }
                               if (!hooks_.isProjectDirty())
                               {
                                   failReason = "project not dirty after duplicate";
                                   return false;
                               }
                               st->audioCopyDigest = hooks_.dup.insertChainDigest(st->audioCopy);
                               const juce::String srcRows = insertRowsText(hooks_.listInsertRows(st->audioSrc));
                               const juce::String copyRows = insertRowsText(hooks_.listInsertRows(st->audioCopy));
                               appendStabilityRunLine("  inserts src=[" + srcRows + "] copy=[" + copyRows + "]");
                               appendStabilityRunLine("  chain digest src=" + st->audioSrcDigestBefore + " copy=" + st->audioCopyDigest);
                               const juce::String copyPtrs = hooks_.dup.insertInstancePointers(st->audioCopy);
                               appendStabilityRunLine("  instances src=[" + st->audioSrcPointersBefore + "] copy=[" + copyPtrs + "]");
                               if (srcRows != copyRows)
                               {
                                   failReason = "insert rows differ";
                                   return false;
                               }
                               if (chainIdentityPart(st->audioCopyDigest) != chainIdentityPart(st->audioSrcDigestBefore)
                                   || !chainStatePresenceMatches(st->audioSrcDigestBefore, st->audioCopyDigest))
                               {
                                   failReason = "insert chain identity / order / state presence not copied faithfully";
                                   return false;
                               }
                               if (chainIdentityPart(hooks_.dup.insertChainDigest(st->audioSrc)) != chainIdentityPart(st->audioSrcDigestBefore)
                                   || hooks_.dup.insertInstancePointers(st->audioSrc) != st->audioSrcPointersBefore)
                               {
                                   failReason = "the source's chain / instances changed by duplicating";
                                   return false;
                               }
                               juce::StringArray a, b;
                               a.addTokens(st->audioSrcPointersBefore, " ", "");
                               b.addTokens(copyPtrs, " ", "");
                               if (a.size() != b.size())
                               {
                                   failReason = "copy has a different number of live instances than the source";
                                   return false;
                               }
                               for (const juce::String& p : b)
                               {
                                   if (a.contains(p))
                                   {
                                       failReason = "copy shares a plug-in instance with the source: " + p;
                                       return false;
                                   }
                               }
                               if (hooks_.dup.lanesScrollModel)
                               {
                                   st->scrollModelAfterDup = hooks_.dup.lanesScrollModel();
                                   appendStabilityRunLine("  scroll model before=" + st->scrollModelBeforeDup + " after=" + st->scrollModelAfterDup);
                                   if (modelValue(st->scrollModelAfterDup, "content") <= modelValue(st->scrollModelBeforeDup, "content"))
                                   {
                                       failReason = "scroll content height did not grow with the new row";
                                       return false;
                                   }
                               }
                               return true;
                           },
                           600 });

    steps_.push_back(Step{ "duplicate-track: parameter independence (edit the copy's first insert, the source stays)",
                           [this, st](juce::String& failReason) -> bool {
                               if (!hooks_.dup.setInsertParam0 || !hooks_.dup.getInsertParam0)
                               {
                                   return true;
                               }
                               const float orig = hooks_.dup.getInsertParam0(st->audioSrc, 0);
                               const float copyBefore = hooks_.dup.getInsertParam0(st->audioCopy, 0);
                               if (std::isnan(orig) || std::isnan(copyBefore))
                               {
                                   appendStabilityRunLine("  no live parameter available on the first insert - independence check by instance pointers only");
                                   return true;
                               }
                               st->paramCheckPossible = true;
                               st->param0OriginalBefore = orig;
                               const float target = orig > 0.5f ? 0.1f : 0.9f;
                               hooks_.dup.setInsertParam0(st->audioCopy, 0, target);
                               const float origAfter = hooks_.dup.getInsertParam0(st->audioSrc, 0);
                               const float copyAfter = hooks_.dup.getInsertParam0(st->audioCopy, 0);
                               appendStabilityRunLine("  param0: source " + juce::String(orig, 4) + " -> " + juce::String(origAfter, 4)
                                                      + " | copy " + juce::String(copyBefore, 4) + " -> " + juce::String(copyAfter, 4)
                                                      + " (set " + juce::String(target, 2) + ")");
                               if (std::abs(origAfter - orig) > 1e-6f)
                               {
                                   failReason = "editing the copy changed the source's parameter";
                                   return false;
                               }
                               if (std::abs(copyAfter - copyBefore) < 1e-3f)
                               {
                                   appendStabilityRunLine("  note: the copy's parameter did not move (stepped / read-only parameter?) - not failed");
                               }
                               return true;
                           },
                           kSettleDefaultMs });

    steps_.push_back(Step{ "duplicate-track: undo removes the copy (source untouched, same instances)",
                           [this, st](juce::String& failReason) -> bool {
                               hooks_.invokeUndo();
                               juce::Thread::sleep(150);
                               const juce::String order = hooks_.dup.sessionTrackOrder();
                               appendStabilityRunLine("  order after undo: " + order);
                               if (order != st->orderBeforeAudioDup)
                               {
                                   failReason = "track order after undo differs from before the duplicate";
                                   return false;
                               }
                               if (hooks_.undoStackSize() != st->undoSizeBeforeAudioDup)
                               {
                                   failReason = "undo stack size not back to the pre-duplicate value";
                                   return false;
                               }
                               if (hooks_.dup.describeSessionRow(st->audioSrc) != st->audioSrcRowBefore)
                               {
                                   failReason = "source row changed across duplicate + undo";
                                   return false;
                               }
                               if (chainIdentityPart(hooks_.dup.insertChainDigest(st->audioSrc)) != chainIdentityPart(st->audioSrcDigestBefore))
                               {
                                   failReason = "source chain identity changed across duplicate + undo";
                                   return false;
                               }
                               if (hooks_.dup.insertInstancePointers(st->audioSrc) != st->audioSrcPointersBefore)
                               {
                                   failReason = "undo reloaded the source's plug-in instances";
                                   return false;
                               }
                               if (hooks_.listInsertRows(st->audioCopy).size() != 0 || hooks_.dup.insertInstancePointers(st->audioCopy).isNotEmpty())
                               {
                                   failReason = "the copy's insert chain survived the undo";
                                   return false;
                               }
                               if (hooks_.dup.lanesScrollModel)
                               {
                                   const juce::String m = hooks_.dup.lanesScrollModel();
                                   appendStabilityRunLine("  scroll model after undo=" + m);
                                   if (modelValue(m, "content") != modelValue(st->scrollModelBeforeDup, "content"))
                                   {
                                       failReason = "scroll content height after undo differs from before the duplicate";
                                       return false;
                                   }
                               }
                               return true;
                           },
                           600 });

    steps_.push_back(Step{ "duplicate-track: redo restores the copy with its content and own instances",
                           [this, st, verifyCopyCommon](juce::String& failReason) -> bool {
                               hooks_.invokeRedo();
                               juce::Thread::sleep(150);
                               // Redo restores rows + runtime; the active row is UI state outside the snapshot.
                               if (!verifyCopyCommon(st->audioSrc, st->audioCopy, "audio(redo)", failReason, false))
                               {
                                   return false;
                               }
                               const juce::String digest = hooks_.dup.insertChainDigest(st->audioCopy);
                               const juce::String ptrs = hooks_.dup.insertInstancePointers(st->audioCopy);
                               appendStabilityRunLine("  redo: copy digest=" + digest + " instances=[" + ptrs + "]");
                               if (chainIdentityPart(digest) != chainIdentityPart(st->audioSrcDigestBefore)
                                   || !chainStatePresenceMatches(st->audioSrcDigestBefore, digest))
                               {
                                   failReason = "copy chain identity / state presence after redo differs from the source";
                                   return false;
                               }
                               juce::StringArray a, b;
                               a.addTokens(st->audioSrcPointersBefore, " ", "");
                               b.addTokens(ptrs, " ", "");
                               for (const juce::String& p : b)
                               {
                                   if (a.contains(p))
                                   {
                                       failReason = "redo gave the copy an instance of the source";
                                       return false;
                                   }
                               }
                               if (a.size() != b.size())
                               {
                                   failReason = "redo: copy instance count differs from the source";
                                   return false;
                               }
                               return true;
                           },
                           600 });

    // ---- instrument row -------------------------------------------------------------------
    steps_.push_back(Step{ "duplicate-track: instrument row (own MIDI clips + saved state) -> own runtime, same content",
                           [this, st, verifyCopyCommon, trackName](juce::String& failReason) -> bool {
                               if (st->instSrc == kInvalidTrackId)
                               {
                                   appendStabilityRunLine("  no instrument row in the project - skipped");
                                   return true;
                               }
                               hooks_.activateTrackLikeHeaderClick(st->audioSrc);
                               st->instSrcIdentityBefore = hooks_.dup.instrumentRuntimeIdentity(st->instSrc);
                               appendStabilityRunLine("  source runtime: " + st->instSrcIdentityBefore);
                               st->instCopy = hooks_.dup.duplicateTrackLikeMenu(st->instSrc);
                               if (st->instCopy == kInvalidTrackId)
                               {
                                   failReason = "instrument duplicate refused";
                                   return false;
                               }
                               if (!verifyCopyCommon(st->instSrc, st->instCopy, "instrument", failReason))
                               {
                                   return false;
                               }
                               const juce::String copyId = hooks_.dup.instrumentRuntimeIdentity(st->instCopy);
                               const juce::String srcIdAfter = hooks_.dup.instrumentRuntimeIdentity(st->instSrc);
                               appendStabilityRunLine("  copy runtime:   " + copyId);
                               appendStabilityRunLine("  source after:   " + srcIdAfter);
                               if (srcIdAfter != st->instSrcIdentityBefore)
                               {
                                   // The source's own state blob may legitimately re-serialize; only the host and content must stay.
                                   const auto hostOf = [](const juce::String& s) { return s.upToFirstOccurrenceOf(" kind=", false, false); };
                                   if (hostOf(srcIdAfter) != hostOf(st->instSrcIdentityBefore))
                                   {
                                       failReason = "the source's instrument host changed by duplicating";
                                       return false;
                                   }
                               }
                               if (st->instSrcIdentityBefore == "none")
                               {
                                   if (copyId != "none")
                                   {
                                       failReason = "source had no runtime but the copy got one";
                                       return false;
                                   }
                                   return true;
                               }
                               if (copyId == "none")
                               {
                                   failReason = "copy has no instrument runtime";
                                   return false;
                               }
                               const auto field = [](const juce::String& s, const char* key) {
                                   return s.fromFirstOccurrenceOf(juce::String(key) + "=", false, false).upToFirstOccurrenceOf(" ", false, false);
                               };
                               if (field(copyId, "host") == field(st->instSrcIdentityBefore, "host"))
                               {
                                   failReason = "copy shares the instrument host with the source";
                                   return false;
                               }
                               for (const char* key : { "kind", "clips", "notes", "descriptor", "secondary", "updateMode" })
                               {
                                   if (field(copyId, key) != field(st->instSrcIdentityBefore, key))
                                   {
                                       failReason = juce::String("instrument copy differs in ") + key + ": " + field(copyId, key) + " vs "
                                                    + field(st->instSrcIdentityBefore, key);
                                       return false;
                                   }
                               }
                               if (field(st->instSrcIdentityBefore, "stateLen").getIntValue() > 0 && field(copyId, "stateLen").getIntValue() <= 0)
                               {
                                   failReason = "copy carries no instrument state although the source had one";
                                   return false;
                               }
                               if (field(copyId, "loaded") != field(st->instSrcIdentityBefore, "loaded"))
                               {
                                   appendStabilityRunLine("  note: loaded flag differs (source " + field(st->instSrcIdentityBefore, "loaded") + ", copy "
                                                          + field(copyId, "loaded") + ") - the copy keeps identity + state as a placeholder");
                               }
                               if (hooks_.proxyDestinationStateName && hooks_.proxyGenerationInfo)
                               {
                                   appendStabilityRunLine("  proxy: source state=" + hooks_.proxyDestinationStateName(st->instSrc) + " gen=\""
                                                          + hooks_.proxyGenerationInfo(st->instSrc) + "\" | copy state=" + hooks_.proxyDestinationStateName(st->instCopy)
                                                          + " gen=\"" + hooks_.proxyGenerationInfo(st->instCopy) + "\"");
                               }
                               return true;
                           },
                           1500 });

    // ---- pure MIDI row routed to the instrument ------------------------------------------------
    steps_.push_back(Step{ "duplicate-track: pure MIDI row (MIDI To = instrument) -> same destination, same notes, new ids",
                           [this, st, verifyCopyCommon](juce::String& failReason) -> bool {
                               if (st->instSrc == kInvalidTrackId || !hooks_.addMidiSourceRowRoutedTo || !hooks_.liveMidiSummarizeAllClips)
                               {
                                   appendStabilityRunLine("  prerequisites missing (instrument row / hooks) - skipped");
                                   return true;
                               }
                               st->midiSrc = hooks_.addMidiSourceRowRoutedTo(st->instSrc, 64, 3, failReason);
                               if (st->midiSrc == kInvalidTrackId)
                               {
                                   return false;
                               }
                               hooks_.activateTrackLikeHeaderClick(st->audioSrc);
                               const auto before = hooks_.liveMidiSummarizeAllClips(st->midiSrc);
                               st->midiCopy = hooks_.dup.duplicateTrackLikeMenu(st->midiSrc);
                               if (st->midiCopy == kInvalidTrackId)
                               {
                                   failReason = "MIDI row duplicate refused";
                                   return false;
                               }
                               if (!verifyCopyCommon(st->midiSrc, st->midiCopy, "midi", failReason))
                               {
                                   return false;
                               }
                               const auto after = hooks_.liveMidiSummarizeAllClips(st->midiCopy);
                               appendStabilityRunLine("  MIDI clips: source=" + juce::String((int) before.size()) + " copy=" + juce::String((int) after.size()));
                               if (before.size() != after.size())
                               {
                                   failReason = "MIDI clip count differs";
                                   return false;
                               }
                               for (size_t i = 0; i < before.size(); ++i)
                               {
                                   const auto& x = before[i];
                                   const auto& y = after[i];
                                   if (x.notes.size() != y.notes.size() || x.firstClipStartSamples != y.firstClipStartSamples
                                       || x.firstClipLengthSamples != y.firstClipLengthSamples)
                                   {
                                       failReason = "MIDI clip " + juce::String((int) i) + " differs (notes / start / length)";
                                       return false;
                                   }
                                   for (size_t n = 0; n < x.notes.size(); ++n)
                                   {
                                       if (x.notes[n].note != y.notes[n].note || x.notes[n].velocity != y.notes[n].velocity
                                           || x.notes[n].startTick != y.notes[n].startTick || x.notes[n].durationTicks != y.notes[n].durationTicks)
                                       {
                                           failReason = "MIDI note " + juce::String((int) n) + " of clip " + juce::String((int) i) + " differs";
                                           return false;
                                       }
                                   }
                               }
                               const juce::String srcRow = hooks_.dup.describeSessionRow(st->midiSrc);
                               if (!srcRow.contains("midiTo=" + juce::String((juce::int64) st->instSrc)))
                               {
                                   failReason = "MIDI source lost its destination";
                                   return false;
                               }
                               return true;
                           },
                           800 });

    // ---- group ------------------------------------------------------------------------------
    steps_.push_back(Step{ "duplicate-track: Group row -> own routing / sends, feeding tracks NOT copied or rerouted",
                           [this, st, verifyCopyCommon](juce::String& failReason) -> bool {
                               hooks_.activateTrackLikeHeaderClick(st->audioSrc);
                               const juce::String audioRowBefore = hooks_.dup.describeSessionRow(st->audioSrc);
                               st->groupCopy = hooks_.dup.duplicateTrackLikeMenu(st->groupSrc);
                               if (st->groupCopy == kInvalidTrackId)
                               {
                                   failReason = "Group duplicate refused";
                                   return false;
                               }
                               if (!verifyCopyCommon(st->groupSrc, st->groupCopy, "group", failReason))
                               {
                                   return false;
                               }
                               if (hooks_.dup.describeSessionRow(st->audioSrc) != audioRowBefore)
                               {
                                   failReason = "duplicating the Group changed the feeding audio row";
                                   return false;
                               }
                               const juce::String outputToken = "output=" + juce::String((juce::int64) st->groupCopy);
                               for (const StabilityTrackInfo& t : hooks_.listAllTracks())
                               {
                                   const juce::String row = hooks_.dup.describeSessionRow(t.id);
                                   if (row.contains(outputToken + " ") || row.contains("dest=" + juce::String((juce::int64) st->groupCopy) + " "))
                                   {
                                       failReason = "a track feeds the Group copy: " + row;
                                       return false;
                                   }
                               }
                               appendStabilityRunLine("  no track routes or sends to the Group copy (as required)");
                               return true;
                           },
                           600 });

    // ---- unavailable plug-in placeholder -----------------------------------------------------
    steps_.push_back(Step{ "duplicate-track: row with an unavailable plug-in placeholder -> placeholder + state preserved on the copy",
                           [this, st](juce::String& failReason) -> bool {
                               if (!hooks_.dup.addUnavailablePlaceholderInsert)
                               {
                                   return true;
                               }
                               st->placeholderSrc = st->groupSrc;
                               if (!hooks_.dup.addUnavailablePlaceholderInsert(st->placeholderSrc, failReason))
                               {
                                   return false;
                               }
                               const juce::String srcRows = insertRowsText(hooks_.listInsertRows(st->placeholderSrc));
                               const juce::String srcDigest = hooks_.dup.insertChainDigest(st->placeholderSrc);
                               hooks_.activateTrackLikeHeaderClick(st->audioSrc);
                               st->placeholderCopy = hooks_.dup.duplicateTrackLikeMenu(st->placeholderSrc);
                               if (st->placeholderCopy == kInvalidTrackId)
                               {
                                   failReason = "duplicate of the placeholder row refused";
                                   return false;
                               }
                               const juce::String copyRows = insertRowsText(hooks_.listInsertRows(st->placeholderCopy));
                               const juce::String copyDigest = hooks_.dup.insertChainDigest(st->placeholderCopy);
                               appendStabilityRunLine("  placeholder rows src=[" + srcRows + "] copy=[" + copyRows + "]");
                               appendStabilityRunLine("  placeholder digest src=" + srcDigest + " copy=" + copyDigest);
                               if (!copyRows.contains("[unavailable]"))
                               {
                                   failReason = "copy does not show the unavailable placeholder";
                                   return false;
                               }
                               if (srcDigest != copyDigest)
                               {
                                   failReason = "placeholder identity / state differs on the copy";
                                   return false;
                               }
                               return true;
                           },
                           600 });

    // ---- vertical scrollbar --------------------------------------------------------------
    steps_.push_back(Step{ "scrollbar: varying row heights; range from real content; header/lane alignment",
                           [this, st](juce::String& failReason) -> bool {
                               if (!hooks_.dup.verifyVerticalLayout || !hooks_.dup.lanesScrollModel || !hooks_.dup.scrollBarDiagnostics)
                               {
                                   appendStabilityRunLine("  scrollbar hooks missing - skipped");
                                   return true;
                               }
                               if (hooks_.dup.setRowHeightPx)
                               {
                                   hooks_.dup.setRowHeightPx(st->audioSrc, 170);
                                   hooks_.dup.setRowHeightPx(st->audioCopy, 60);
                                   if (st->instSrc != kInvalidTrackId)
                                   {
                                       hooks_.dup.setRowHeightPx(st->instSrc, 130);
                                   }
                               }
                               juce::String model = hooks_.dup.lanesScrollModel();
                               if (modelValue(model, "content") <= modelValue(model, "viewport") && hooks_.setMainWindowSize && hooks_.getMainWindowBounds)
                               {
                                   // Make the arrangement overflow: lower the main window.
                                   const juce::Rectangle<int> b = hooks_.getMainWindowBounds();
                                   hooks_.setMainWindowSize(b.getWidth(), juce::jmax(480, juce::jmin(b.getHeight(), 640)));
                                   st->windowShrunk = true;
                                   juce::Thread::sleep(100);
                                   model = hooks_.dup.lanesScrollModel();
                               }
                               appendStabilityRunLine("  model: " + model + " | " + hooks_.dup.scrollBarDiagnostics());
                               juce::String report;
                               if (!hooks_.dup.verifyVerticalLayout(report, failReason))
                               {
                                   appendStabilityRunLine(report);
                                   return false;
                               }
                               st->rowCountAtScrollCheck = hooks_.getTrackCount();
                               if (modelValue(model, "content") <= modelValue(model, "viewport"))
                               {
                                   appendStabilityRunLine("  everything fits even at the lowered window - drive checks skipped (bar hidden verified)");
                                   return true;
                               }
                               return true;
                           },
                           400 });

    steps_.push_back(Step{ "scrollbar: thumb drag to the bottom, page click up, wheel down, drag to top (one model, aligned rows)",
                           [this, st, evidence](juce::String& failReason) -> bool {
                               if (!hooks_.dup.verifyVerticalLayout || !hooks_.dup.scrollBarDragTo || !hooks_.dup.lanesScrollModel)
                               {
                                   return true;
                               }
                               juce::String model = hooks_.dup.lanesScrollModel();
                               const int max = modelValue(model, "max");
                               if (max <= 0)
                               {
                                   appendStabilityRunLine("  no scroll range - skipped");
                                   return true;
                               }
                               juce::String report;
                               hooks_.dup.scrollBarDragTo(max);
                               model = hooks_.dup.lanesScrollModel();
                               appendStabilityRunLine("  after thumb drag to bottom: " + model + " | " + hooks_.dup.scrollBarDiagnostics());
                               if (modelValue(model, "offset") != max)
                               {
                                   failReason = "thumb drag to the bottom did not reach the max offset";
                                   return false;
                               }
                               if (!hooks_.dup.verifyVerticalLayout(report, failReason))
                               {
                                   appendStabilityRunLine(report);
                                   return false;
                               }
                               // The LAST row (Stereo Out) must now end exactly at the viewport bottom.
                               if (hooks_.captureArrangementPng)
                               {
                                   (void) hooks_.captureArrangementPng(evidence.getChildFile("scrollbar-bottom.png"));
                               }
                               if (hooks_.dup.scrollBarPageClick)
                               {
                                   hooks_.dup.scrollBarPageClick(-1);
                                   model = hooks_.dup.lanesScrollModel();
                                   appendStabilityRunLine("  after page click up: " + model);
                                   if (modelValue(model, "offset") >= max)
                                   {
                                       failReason = "page click in the trough did not scroll up";
                                       return false;
                                   }
                               }
                               const int offsetBeforeWheel = modelValue(hooks_.dup.lanesScrollModel(), "offset");
                               if (hooks_.dup.wheelLanes)
                               {
                                   hooks_.dup.wheelLanes(2);
                                   model = hooks_.dup.lanesScrollModel();
                                   appendStabilityRunLine("  after wheel down x2: " + model + " | " + hooks_.dup.scrollBarDiagnostics());
                                   if (modelValue(model, "offset") <= offsetBeforeWheel && offsetBeforeWheel < max)
                                   {
                                       failReason = "wheel did not scroll down";
                                       return false;
                                   }
                                   if (!hooks_.dup.verifyVerticalLayout(report, failReason))
                                   {
                                       appendStabilityRunLine(report);
                                       return false;
                                   }
                               }
                               hooks_.dup.scrollBarDragTo(0);
                               model = hooks_.dup.lanesScrollModel();
                               appendStabilityRunLine("  after thumb drag to top: " + model + " | " + hooks_.dup.scrollBarDiagnostics());
                               if (modelValue(model, "offset") != 0)
                               {
                                   failReason = "thumb drag to the top did not reach offset 0";
                                   return false;
                               }
                               if (!hooks_.dup.verifyVerticalLayout(report, failReason))
                               {
                                   appendStabilityRunLine(report);
                                   return false;
                               }
                               if (hooks_.dup.rowTopOffsetPxForTrack && hooks_.dup.visibleRowIndexForTrack)
                               {
                                   // Scroll so the copy's row top is at the viewport top; its header must sit at the gutter.
                                   const int top = hooks_.dup.rowTopOffsetPxForTrack(st->audioCopy);
                                   hooks_.dup.scrollBarDragTo(juce::jmin(top, max));
                                   if (!hooks_.dup.verifyVerticalLayout(report, failReason))
                                   {
                                       appendStabilityRunLine(report);
                                       return false;
                                   }
                                   appendStabilityRunLine("  scrolled to the copy's row (top=" + juce::String(top) + "): " + hooks_.dup.lanesScrollModel());
                                   hooks_.dup.scrollBarDragTo(0);
                               }
                               if (hooks_.captureArrangementPng)
                               {
                                   (void) hooks_.captureArrangementPng(evidence.getChildFile("scrollbar-top.png"));
                               }
                               (void) writeTextFile(evidence.getChildFile("scrollbar-layout-report.txt"), report);
                               return true;
                           },
                           400 });

    steps_.push_back(Step{ "scrollbar: range follows delete + undo (content height back and forth)",
                           [this, st](juce::String& failReason) -> bool {
                               if (!hooks_.dup.lanesScrollModel || !hooks_.requestDeleteTrack || st->placeholderCopy == kInvalidTrackId)
                               {
                                   return true;
                               }
                               const juce::String before = hooks_.dup.lanesScrollModel();
                               hooks_.requestDeleteTrack(st->placeholderCopy);
                               juce::Thread::sleep(200);
                               const juce::String afterDelete = hooks_.dup.lanesScrollModel();
                               hooks_.invokeUndo();
                               juce::Thread::sleep(200);
                               const juce::String afterUndo = hooks_.dup.lanesScrollModel();
                               appendStabilityRunLine("  before=" + before + " | afterDelete=" + afterDelete + " | afterUndo=" + afterUndo);
                               if (modelValue(afterDelete, "content") >= modelValue(before, "content"))
                               {
                                   failReason = "content height did not shrink after deleting a row";
                                   return false;
                               }
                               if (modelValue(afterUndo, "content") != modelValue(before, "content"))
                               {
                                   failReason = "content height after undo differs from before the delete";
                                   return false;
                               }
                               juce::String report;
                               if (hooks_.dup.verifyVerticalLayout && !hooks_.dup.verifyVerticalLayout(report, failReason))
                               {
                                   appendStabilityRunLine(report);
                                   return false;
                               }
                               return true;
                           },
                           600 });

    // ---- header menu PNG ---------------------------------------------------------------------
    steps_.push_back(Step{ "duplicate-track: open the header menu of the audio copy like a right click (PNG) and dismiss",
                           [this, st, evidence](juce::String&) -> bool {
                               if (!hooks_.dup.openHeaderMenuCaptureAndDismiss)
                               {
                                   return true;
                               }
                               if (hooks_.dup.scrollBarDragTo)
                               {
                                   hooks_.dup.scrollBarDragTo(0);
                               }
                               juce::String detail;
                               const bool ok = hooks_.dup.openHeaderMenuCaptureAndDismiss(st->audioSrc, evidence.getChildFile("duplicate-track-menu.png"), detail);
                               appendStabilityRunLine(juce::String("  header menu PNG: ") + (ok ? "written" : "NOT written") + " " + detail);
                               if (hooks_.captureArrangementPng)
                               {
                                   (void) hooks_.captureArrangementPng(evidence.getChildFile("arrangement-after-duplicates.png"));
                               }
                               return true;
                           },
                           400 });

    // ---- save / reload ----------------------------------------------------------------------
    steps_.push_back(Step{ "duplicate-track: save the test copy",
                           [this](juce::String&) -> bool {
                               hooks_.saveProject();
                               return true;
                           },
                           800 });
    steps_.push_back(Step{ "duplicate-track: reload the test copy",
                           [this, st](juce::String&) -> bool {
                               hooks_.loadProjectFromFile(st->copy);
                               return true;
                           },
                           kSettleAfterLoadMs });
    steps_.push_back(Step{ "duplicate-track: after reload - copies present with inserts, clips, runtime and destination",
                           [this, st, trackKind](juce::String& failReason) -> bool {
                               juce::String kopior;
                               int found = 0;
                               for (const StabilityTrackInfo& t : hooks_.listAllTracks())
                               {
                                   if (t.name.contains("kopia"))
                                   {
                                       ++found;
                                       kopior << "  reloaded copy " << juce::String((juce::int64) t.id) << " " << t.kindName << " \"" << t.name
                                              << "\" inserts=[" << insertRowsText(hooks_.listInsertRows(t.id)) << "] row=" << hooks_.dup.describeSessionRow(t.id)
                                              << " runtime=" << hooks_.dup.instrumentRuntimeIdentity(t.id) << "\n";
                                   }
                               }
                               appendStabilityRunLine(kopior.trimEnd());
                               int expected = 1 /*audio*/ + 1 /*group*/ + (st->instCopy != kInvalidTrackId ? 1 : 0)
                                              + (st->midiCopy != kInvalidTrackId ? 1 : 0) + (st->placeholderCopy != kInvalidTrackId ? 1 : 0);
                               appendStabilityRunLine("  copies found after reload: " + juce::String(found) + " expected " + juce::String(expected));
                               if (found != expected)
                               {
                                   failReason = "copies after reload: " + juce::String(found) + " expected " + juce::String(expected);
                                   return false;
                               }
                               // The audio copy must still carry the same chain as the source.
                               const juce::String srcDigest = hooks_.dup.insertChainDigest(st->audioSrc);
                               const juce::String copyDigest = hooks_.dup.insertChainDigest(st->audioCopy);
                               appendStabilityRunLine("  reloaded chain digest src=" + srcDigest + " copy=" + copyDigest);
                               if (chainIdentityPart(srcDigest) != chainIdentityPart(copyDigest) || !chainStatePresenceMatches(srcDigest, copyDigest))
                               {
                                   failReason = "after reload the audio copy's chain differs from the source";
                                   return false;
                               }
                               if (hooks_.dup.describeSessionRowWithoutIdentity(st->audioSrc) != hooks_.dup.describeSessionRowWithoutIdentity(st->audioCopy))
                               {
                                   failReason = "after reload the audio copy's settings differ from the source";
                                   return false;
                               }
                               if (st->instCopy != kInvalidTrackId && st->instSrcIdentityBefore != "none"
                                   && hooks_.dup.instrumentRuntimeIdentity(st->instCopy) == "none")
                               {
                                   failReason = "after reload the instrument copy has no runtime";
                                   return false;
                               }
                               juce::ignoreUnused(trackKind);
                               return true;
                           },
                           kSettleDefaultMs });

    steps_.push_back(Step{ "duplicate-track: after reload (clean): Stereo Out refused + still clean; a real duplicate dirties; undo",
                           [this, st](juce::String& failReason) -> bool {
                               if (hooks_.isProjectDirty())
                               {
                                   failReason = "project dirty right after reload";
                                   return false;
                               }
                               TrackId master = kInvalidTrackId;
                               for (const StabilityTrackInfo& t : hooks_.listAllTracks())
                               {
                                   if (t.kindName == "master")
                                   {
                                       master = t.id;
                                   }
                               }
                               const TrackId refused = hooks_.dup.duplicateTrackLikeMenu(master);
                               appendStabilityRunLine("  duplicate(Stereo Out) -> " + juce::String((juce::int64) refused) + " dirty="
                                                      + juce::String(hooks_.isProjectDirty() ? 1 : 0) + " tracks=" + juce::String(hooks_.getTrackCount()));
                               if (refused != kInvalidTrackId || hooks_.isProjectDirty())
                               {
                                   failReason = "Stereo Out duplication was not refused cleanly";
                                   return false;
                               }
                               const int undoBefore = hooks_.undoStackSize();
                               const TrackId again = hooks_.dup.duplicateTrackLikeMenu(st->audioSrc);
                               if (again == kInvalidTrackId || !hooks_.isProjectDirty() || hooks_.undoStackSize() != undoBefore + 1)
                               {
                                   failReason = "duplicate after reload did not succeed / dirty / record one undo step";
                                   return false;
                               }
                               hooks_.invokeUndo();
                               juce::Thread::sleep(150);
                               appendStabilityRunLine("  after undo: tracks=" + juce::String(hooks_.getTrackCount()) + " dirty=" + juce::String(hooks_.isProjectDirty() ? 1 : 0));
                               return true;
                           },
                           600 });

    steps_.push_back(Step{ "duplicate-track: restore the main window size",
                           [this, st](juce::String&) -> bool {
                               if (st->windowShrunk && hooks_.setMainWindowSize && !st->windowBoundsAtStart.isEmpty())
                               {
                                   hooks_.setMainWindowSize(st->windowBoundsAtStart.getWidth(), st->windowBoundsAtStart.getHeight());
                               }
                               return true;
                           },
                           300 });
}
