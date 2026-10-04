#include "app/RecordingCoordinator.h"

#include <algorithm>
#include <vector>

#include <juce_gui_basics/juce_gui_basics.h>

#include "domain/AudioClip.h"
#include "domain/Session.h"
#include "domain/SessionSnapshot.h"
#include "diagnostics/StabilityDiagnosticLog.h"
#include "diagnostics/StabilityScenarioRunner.h"
#include "diagnostics/UndoDiagnosticConfig.h"
#include "diagnostics/UndoDiagnosticFileLog.h"
#include "engine/CountInClickOutput.h"
#include "engine/PlaybackEngine.h"
#include "audio/LatencySettingsStore.h"
#include "io/AudioFileLoader.h"
#include "io/MonoWavFileWriter.h"
#include "transport/Transport.h"

namespace
{
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
        static constexpr int kMaxAttempts = 20;
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
} // namespace

struct RecordingCoordinator::CountInTimer final : juce::Timer
{
    explicit CountInTimer(RecordingCoordinator& o) noexcept
        : owner(o)
    {
    }
    void timerCallback() override { owner.onCountInTimerTick(); }
    RecordingCoordinator& owner;
};

struct RecordingCoordinator::CycleRecordingWrapTimer final : juce::Timer
{
    explicit CycleRecordingWrapTimer(RecordingCoordinator& o) noexcept
        : owner(o)
    {
    }
    void timerCallback() override { owner.onCycleRecordingWrapTimerTick(); }
    RecordingCoordinator& owner;
};

struct RecordingCoordinator::RecordingExtentFollowTimer final : juce::Timer
{
    explicit RecordingExtentFollowTimer(RecordingCoordinator& o) noexcept
        : owner(o)
    {
    }
    void timerCallback() override { owner.onRecordingExtentFollowTick(); }
    RecordingCoordinator& owner;
};

RecordingCoordinator::RecordingCoordinator(Transport& transport,
                                           Session& session,
                                           PlaybackEngine& playbackEngine,
                                           juce::AudioDeviceManager& deviceManager,
                                           RecorderService& recorder,
                                           CountInClickOutput& countInClicks,
                                           LatencySettingsStore& latencyStore,
                                           juce::Label& countInStatusLabel,
                                           Callbacks callbacks)
    : transport_(transport)
    , session_(session)
    , playbackEngine_(playbackEngine)
    , deviceManager_(deviceManager)
    , recorder_(recorder)
    , countInClicks_(countInClicks)
    , latencyStore_(latencyStore)
    , countInStatusLabel_(countInStatusLabel)
    , callbacks_(std::move(callbacks))
{
}

RecordingCoordinator::~RecordingCoordinator()
{
    if (countInTimer_ != nullptr)
    {
        countInTimer_->stopTimer();
    }
    if (cycleRecordingWrapTimer_ != nullptr)
    {
        cycleRecordingWrapTimer_->stopTimer();
    }
    if (extentFollowTimer_ != nullptr)
    {
        extentFollowTimer_->stopTimer();
    }
}

void RecordingCoordinator::onRecordingExtentFollowTick()
{
    if (!extentFollowActive_ || !isRecordingInProgress())
    {
        return;
    }
    // Display follow only: the engine keeps the transport running through the take regardless
    // (record run), this merely keeps the ruler / scroll range a few seconds ahead of the
    // playhead through the existing grow-only session operation. The margin is removed at Stop.
    const double sr = runSampleRate_ > 0.0 ? runSampleRate_ : 48000.0;
    const std::int64_t margin = (std::int64_t)std::llround(5.0 * sr);
    const std::int64_t head = transport_.readPlayheadSamplesForUi();
    if (head + margin > session_.getArrangementExtentSamples())
    {
        session_.setArrangementExtentSamples(head + margin);
        callbacks_.repaintRulerAndLanes();
    }
}

RecordRunBoundaries RecordingCoordinator::stopRecordRunAndCollectBoundaries()
{
    // Intent first, run stop second: the callback that acknowledges the stop also sees the
    // Stopped intent, so the boundary block does not advance the playhead (see PlaybackEngine).
    transport_.requestPlaybackIntent(PlaybackIntent::Stopped);
    playbackEngine_.requestRecordRunStop();

    PlaybackEngine::RecordRunBoundary fallback;
    fallback.monoSample = playbackEngine_.readMonoSampleClockForUi();
    fallback.timelineSample = transport_.readPlayheadSamplesForUi();
    fallback.wrapSerial = transport_.readCycleWrapCountForUi();
    fallback.valid = true;
    const bool acked = playbackEngine_.waitForRecordRunStop(250, fallback);
    if (!acked)
    {
        juce::Logger::writeToLog("[Rec] WARNING: the audio callback did not acknowledge the stop within 250 ms "
                                 "(device stopped or lost) - the run was closed from the message thread at mono "
                                 + juce::String((juce::int64)fallback.monoSample) + " / timeline "
                                 + juce::String((juce::int64)fallback.timelineSample));
    }
    const PlaybackEngine::RecordRunBoundary startB = playbackEngine_.recordRunStartBoundary();
    const PlaybackEngine::RecordRunBoundary stopB = playbackEngine_.recordRunStopBoundary();
    RecordRunBoundaries b;
    b.startTimelineSample = startB.timelineSample;
    b.startMonoSample = startB.monoSample;
    b.startWrapSerial = startB.wrapSerial;
    b.stopTimelineSample = stopB.timelineSample;
    b.stopMonoSample = stopB.monoSample;
    b.stopWrapSerial = stopB.wrapSerial;
    b.acknowledgedByEngine = acked;
    lastRunBoundaries_ = b;
    const juce::String line = "[Rec] run boundaries: start mono=" + juce::String((juce::int64)b.startMonoSample) + " tl="
                              + juce::String((juce::int64)b.startTimelineSample) + " wrap=" + juce::String((int)b.startWrapSerial)
                              + " | stop mono=" + juce::String((juce::int64)b.stopMonoSample) + " tl="
                              + juce::String((juce::int64)b.stopTimelineSample) + " wrap=" + juce::String((int)b.stopWrapSerial)
                              + " | captured frames=" + juce::String((juce::int64)(b.stopMonoSample - b.startMonoSample))
                              + (acked ? "" : " (fallback, no callback)");
    juce::Logger::writeToLog(line);
    if (isStabilityTestModeActive())
    {
        appendStabilityRunLine("  " + line);
    }
    return b;
}

void RecordingCoordinator::onCycleRecordingWrapTimerTick()
{
    if (!cycleRecordingActive_ || !recorder_.isRecording())
    {
        return;
    }
    const std::uint32_t now = transport_.readCycleWrapCountForUi();
    if (now != lastSeenWrapCount_)
    {
        numCompletedPasses_ += static_cast<int>(now - lastSeenWrapCount_);
        lastSeenWrapCount_ = now;
    }
}

void RecordingCoordinator::stopRecordingAndCommitFromUi(const char* sourceContext)
{
    if (!isRecordingInProgress())
    {
        return;
    }
    if (sourceContext != nullptr)
    {
        juce::Logger::writeToLog(juce::String{"[Rec] stop/commit source="} + sourceContext);
    }

    const bool audioTake = recorder_.isRecording();
    const bool midiTake = midiTakeActive_;
    const bool commitCycleTakes = cycleRecordingActive_ && audioTake;
    const TrackId cycleTrackId = cycleSessionTrackId_;
    const std::int64_t cycleLocL = cycleSessionLocL_;
    const std::int64_t cycleLocR = cycleSessionLocR_;
    const double cycleSr = cycleSessionSampleRate_;

    // ONE stop boundary for audio and MIDI, stamped by the audio thread at a block boundary
    // (mono clock + transport position + wrap serial of the same sample). The audio recorder
    // received its last block before it; the MIDI take is cut at it; both finalizations below use
    // these values — never separate UI reads. Held notes end here, a held pedal is released here.
    const RecordRunBoundaries run = stopRecordRunAndCollectBoundaries();
    const std::int64_t stopSample = run.stopTimelineSample;
    const std::uint32_t stopWrapSerial = run.stopWrapSerial;
    // Audio placement and cycle slicing start from the acknowledged start boundary (equal to the
    // count-in position when the transport was stopped; exact also for a punch-in while playing).
    const std::int64_t cycleStart = run.startTimelineSample;
    callbacks_.updatePlayPauseButtonFromTransport();
    if (cycleRecordingWrapTimer_ != nullptr)
    {
        cycleRecordingWrapTimer_->stopTimer();
    }
    // Recording past the end: drop the display headroom again. The stored extent returns to its
    // pre-run value (never shrinking an older project's saved extent); the committed clips below
    // extend the content end by exactly what the result needs. Done BEFORE the undoable commit so
    // neither undo side carries the temporary margin.
    if (extentFollowTimer_ != nullptr)
    {
        extentFollowTimer_->stopTimer();
    }
    if (extentFollowActive_)
    {
        extentFollowActive_ = false;
        session_.restoreArrangementExtentAfterRecording(storedExtentBeforeRun_, run.stopTimelineSample);
        callbacks_.syncViewportFromSession();
    }

    callbacks_.clearCycleRecordingPreviewContext();
    cycleRecordingActive_ = false;
    midiTakeActive_ = false;

    // One atomic commit for everything this take produced (audio clip + MIDI clips), so a single
    // Undo removes the whole take and Redo restores it coherently.
    const auto commitUndoable = [this](std::function<void()> commit) {
        if (callbacks_.runUndoableTakeCommit)
        {
            callbacks_.runUndoableTakeCommit("Record take", std::move(commit));
        }
        else
        {
            commit();
        }
    };
    const auto commitMidiClipsNow = [this, midiTake, run]() -> int {
        if (!midiTake || !callbacks_.commitMidiTake)
        {
            return 0;
        }
        const int clips = callbacks_.commitMidiTake(run);
        juce::Logger::writeToLog("[Rec] MIDI take committed: " + juce::String(clips) + " clip(s), stop sample "
                                 + juce::String((juce::int64)run.stopTimelineSample) + " wrapSerial="
                                 + juce::String((int)run.stopWrapSerial));
        return clips;
    };
    // Every return path below closes the engine's run once the finalizations consumed the boundaries.
    struct FinishRunAtExit
    {
        PlaybackEngine& engine;
        ~FinishRunAtExit() { engine.finishRecordRun(); }
    } finishRunAtExit { playbackEngine_ };

    if (!audioTake)
    {
        // MIDI-only take.
        commitUndoable([&] { (void)commitMidiClipsNow(); });
        callbacks_.syncViewportFromSession();
        callbacks_.repaintRulerAndLanes();
        return;
    }

    // The audio thread acknowledged the stop: no further `pushInputBlock` can run, so the
    // recorder may join its writer and free the FIFO / buffers.
    const RecordedTakeResult r = recorder_.stopRecordingAndFinalize();

    if (!r.success)
    {
        numCompletedPasses_ = 0;
        lastSeenWrapCount_ = 0;
        juce::AlertWindow::showMessageBoxAsync(
            juce::AlertWindow::WarningIcon,
            "Recording",
            r.errorMessage.isNotEmpty() ? r.errorMessage : "Could not finalize recording.");
        juce::Logger::writeToLog(juce::String{"[Rec] stop/finalize failed: "} + r.errorMessage);
        // The MIDI part of the take is still valid material — keep it.
        if (midiTake)
        {
            commitUndoable([&] { (void)commitMidiClipsNow(); });
            callbacks_.syncViewportFromSession();
            callbacks_.repaintRulerAndLanes();
        }
        return;
    }

    // The audio slices and the MIDI passes count wraps between the SAME acknowledged boundaries,
    // so a combined audio + MIDI cycle take has identical pass boundaries on every row.
    if (commitCycleTakes)
    {
        numCompletedPasses_ = static_cast<int>(stopWrapSerial - run.startWrapSerial);
        lastSeenWrapCount_ = stopWrapSerial;
    }
    // Cycle-commit failure paths below: the MIDI passes are still valid material — keep them.
    const auto commitMidiOnlyAfterAudioFailure = [&]() {
        if (midiTake)
        {
            commitUndoable([&] { (void)commitMidiClipsNow(); });
            callbacks_.syncViewportFromSession();
        }
    };

    if (r.droppedSampleCount > 0)
    {
        const juce::String w = "Recording overrun: " + juce::String(r.droppedSampleCount)
                               + (r.droppedSampleCount == 1 ? " sample was" : " samples were")
                               + " replaced with silence.";
        juce::Logger::writeToLog(juce::String{"[Rec] "} + w);
        juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::InfoIcon, "Recording", w);
    }

    if (commitCycleTakes)
    {
        std::unique_ptr<AudioClip> loadedClip;
        const auto loadClipResult = AudioFileLoader::loadFromFile(r.takeFile, cycleSr, loadedClip);
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
            commitMidiOnlyAfterAudioFailure();
            callbacks_.repaintRulerAndLanes();
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
            commitMidiOnlyAfterAudioFailure();
            callbacks_.repaintRulerAndLanes();
            return;
        }

        juce::File audioDir = session_.getCurrentProjectFolder().getChildFile("Audio");
        if (audioDir.getFullPathName().isEmpty())
        {
            numCompletedPasses_ = 0;
            lastSeenWrapCount_ = 0;
            juce::AlertWindow::showMessageBoxAsync(
                juce::AlertWindow::WarningIcon, "Session", "Could not resolve project Audio folder.");
            commitMidiOnlyAfterAudioFailure();
            callbacks_.repaintRulerAndLanes();
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
            commitMidiOnlyAfterAudioFailure();
            callbacks_.repaintRulerAndLanes();
            return;
        }

        // Input-selection slice: the continuous cycle master may be mono OR stereo (selected
        // stereo pair); slices keep the take's channel layout.
        const int takeChans = juce::jlimit(1, 2, loadedClip->getNumChannels());
        const float* const pcmLiveL = loadedClip->getAudio().getReadPointer(0);
        const float* const pcmLiveR
            = takeChans >= 2 ? loadedClip->getAudio().getReadPointer(1) : nullptr;
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
            commitMidiOnlyAfterAudioFailure();
            callbacks_.repaintRulerAndLanes();
            return;
        }

        std::vector<float> pcmStableL(static_cast<size_t>(totalAvail));
        std::vector<float> pcmStableR(takeChans >= 2 ? static_cast<size_t>(totalAvail) : size_t{ 0 });
        for (std::int64_t i = 0; i < totalAvail; ++i)
        {
            pcmStableL[(size_t)i] = pcmLiveL[i];
            if (pcmLiveR != nullptr)
            {
                pcmStableR[(size_t)i] = pcmLiveR[i];
            }
        }
        loadedClip.reset();

        const juce::String batchStamp = juce::Time::getCurrentTime().formatted("%Y%m%d_%H%M%S");
        bool allOk = true;
        const juce::File continuousMaster = r.takeFile;
        int sliceFileIndex = 0;

        const std::int64_t recordingPlacementOffsetSamples = latencyStore_.getCurrentRecordingOffsetSamples();

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

            const float* sliceChannels[2] = { pcmStableL.data() + wavOff,
                                              takeChans >= 2 ? pcmStableR.data() + wavOff : nullptr };
            const juce::Result wrResult = MonoWavFileWriter::writeMulti24BitWavSegment(
                sliceWav, sliceChannels, takeChans, sampleCount, cycleSr);

            if (!wrResult.wasOk())
            {
                allOk = false;
                juce::Logger::writeToLog(
                    "[Rec] cycle split write failed (" + sliceWav.getFileName()
                    + "): " + wrResult.getErrorMessage());
                return;
            }

            const juce::Result ar = session_.addRecordedTakeAtSample(
                sliceWav, cycleSr, timelinePos, cycleTrackId, sliceUse);
            if (!ar.wasOk())
            {
                allOk = false;
                juce::Logger::writeToLog(
                    "[Rec] cycle addRecordedTake " + sliceWav.getFileName() + ": " + ar.getErrorMessage());
            }
        };

        const std::int64_t actualStart = juce::jmax<std::int64_t>(std::int64_t{ 0 }, cycleStart);
        const int wraps = juce::jmax(0, numCompletedPasses_);

        // ONE undo step for the whole recording run: every audio pass slice plus every MIDI pass
        // of every recording row (older takes are never overwritten — each pass is a new clip).
        commitUndoable([&] {
            if (actualStart >= cycleLocR || wraps <= 0)
            {
                writeSliceCommit(std::int64_t{ 0 }, totalAvail, actualStart);
            }
            else
            {
                const std::int64_t firstSegLen = juce::jmin(cycleLocR - actualStart, totalAvail);
                writeSliceCommit(std::int64_t{ 0 }, firstSegLen, actualStart);

                const std::int64_t remainingAfterFirst = totalAvail - firstSegLen;
                const std::int64_t maxAdditionalFullsBySamples
                    = passLen > 0 ? remainingAfterFirst / passLen : std::int64_t{ 0 };
                const int subsequentFull = static_cast<int>(
                    juce::jmin(static_cast<std::int64_t>(juce::jmax(0, wraps - 1)),
                               maxAdditionalFullsBySamples));
                for (int i = 0; i < subsequentFull; ++i)
                {
                    const std::int64_t off = firstSegLen + static_cast<std::int64_t>(i) * passLen;
                    writeSliceCommit(off, passLen, cycleLocL);
                }

                const std::int64_t partialOffset
                    = firstSegLen + static_cast<std::int64_t>(subsequentFull) * passLen;
                std::int64_t partialLen = totalAvail - partialOffset;
                partialLen = juce::jlimit<std::int64_t>(std::int64_t{ 0 }, passLen, partialLen);
                if (partialLen > 0)
                {
                    writeSliceCommit(partialOffset, partialLen, cycleLocL);
                }
            }
            (void)commitMidiClipsNow();
        });

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
            callbacks_.syncViewportFromSession();
            scheduleCycleContinuousMasterCleanup(continuousMaster);
        }
    }
    else
    {
        // Raw start boundary (engine-acknowledged) + the deliberate audio placement compensation
        // (latency store) — the MIDI take applies its own, separate output-latency offset per gesture.
        const std::int64_t recordingPlacementOffsetSamples = latencyStore_.getCurrentRecordingOffsetSamples();
        const std::int64_t committedStartSamples = juce::jmax<std::int64_t>(
            std::int64_t{ 0 }, run.startTimelineSample + recordingPlacementOffsetSamples);

        juce::Result ar = juce::Result::ok();
        commitUndoable([&] {
            ar = session_.addRecordedTakeAtSample(
                r.takeFile,
                r.sampleRate,
                committedStartSamples,
                r.targetTrackId,
                r.intendedSampleCount);
            (void)commitMidiClipsNow();
        });
        if (!ar.wasOk())
        {
            juce::AlertWindow::showMessageBoxAsync(
                juce::AlertWindow::WarningIcon, "Session", ar.getErrorMessage());
            juce::Logger::writeToLog(
                juce::String{"[Rec] addRecordedTakeAtSample failed: "} + ar.getErrorMessage());
        }
        else
        {
            callbacks_.syncViewportFromSession();
        }
    }

    callbacks_.repaintRulerAndLanes();
}

bool RecordingCoordinator::isRecordingInProgress() const noexcept
{
    return recorder_.isRecording() || midiTakeActive_;
}

void RecordingCoordinator::abortMidiTakeForProjectReplace()
{
    if (isCountInActive())
    {
        cancelCountIn();
    }
    if (!midiTakeActive_)
    {
        return;
    }
    midiTakeActive_ = false;
    // Close the engine's record run too (bounded wait; the take is dropped, so the boundary is
    // only needed to stop capturing) and drop the display headroom.
    if (!recorder_.isRecording())
    {
        (void)stopRecordRunAndCollectBoundaries();
        playbackEngine_.finishRecordRun();
    }
    if (extentFollowTimer_ != nullptr)
    {
        extentFollowTimer_->stopTimer();
    }
    if (extentFollowActive_)
    {
        extentFollowActive_ = false;
        session_.restoreArrangementExtentAfterRecording(storedExtentBeforeRun_, transport_.readPlayheadSamplesForUi());
    }
    if (callbacks_.abortMidiTake)
    {
        callbacks_.abortMidiTake();
    }
    juce::Logger::writeToLog("[Rec] MIDI take aborted (project replaced)");
}

void RecordingCoordinator::numpadRecordToggled()
{
    if (isRecordingInProgress())
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

    // Live MIDI rows (Instrument / Midi kind) record through their own capture path; several may
    // be armed at once and they may combine with the (single) armed audio track in one take.
    lastRecordStartRefusal_.clear();
    const std::vector<TrackId> midiTracks = callbacks_.armedMidiTracksReadyToRecord
                                                ? callbacks_.armedMidiTracksReadyToRecord()
                                                : std::vector<TrackId>{};
    const juce::StringArray midiNotReady = callbacks_.describeArmedMidiRowsNotReady
                                               ? callbacks_.describeArmedMidiRowsNotReady()
                                               : juce::StringArray{};
    const TrackId armed = recorder_.getArmedTrackId();
    if (armed == kInvalidTrackId && midiTracks.empty())
    {
        // Distinguish "nothing is armed" from "armed, but the MIDI input cannot deliver": the
        // row IS armed in the second case, so telling the user to arm it would be wrong.
        juce::String msg;
        if (midiNotReady.isEmpty())
        {
            msg = "Arm a track for recording first (the R control on a track header).";
        }
        else
        {
            msg = "Recording cannot start - the armed track" + juce::String(midiNotReady.size() > 1 ? "s have" : " has")
                  + " no usable MIDI input:\n\n" + midiNotReady.joinIntoString("\n")
                  + "\n\nThe track stays armed; fix the MIDI Input in the Inspector and press Record again.";
        }
        lastRecordStartRefusal_ = msg;
        juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::InfoIcon, "Recording", msg);
        juce::Logger::writeToLog("[Rec] start blocked: " + (midiNotReady.isEmpty() ? juce::String("no armed track")
                                                                                     : "armed MIDI rows not ready: " + midiNotReady.joinIntoString(" | ")));
        return;
    }
    if (!midiNotReady.isEmpty())
    {
        // Some armed MIDI rows cannot deliver while others (or the audio track) can: the take
        // starts for the ready rows; the skipped ones are logged and explained in the Inspector.
        juce::Logger::writeToLog("[Rec] armed MIDI rows skipped (not ready): " + midiNotReady.joinIntoString(" | "));
    }
    // MIDI rows record with Cycle on as well: the take is split into one clip per pass at the
    // engine's wrap markers (see LiveMidiTakeBuilder.h), with the same start boundary and locators
    // as the audio cycle slices when an audio track records alongside.
    if (armed == kInvalidTrackId)
    {
        // MIDI-only take: no take file, but the transport still needs a running device.
        juce::AudioIODevice* const dev = deviceManager_.getCurrentAudioDevice();
        if (dev == nullptr || dev->getCurrentSampleRate() <= 0.0)
        {
            lastRecordStartRefusal_ = "No active audio device.";
            juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::WarningIcon, "Audio", lastRecordStartRefusal_);
            return;
        }
        cycleRecordingActive_ = false;
        BeginRecordingRequest req;
        req.targetTrackId = kInvalidTrackId; // no audio recorder take
        req.recordingStartSample = 0;
        req.sampleRate = dev->getCurrentSampleRate();
        req.numChannels = 1;
        pendingMidiTake_ = true;
        startCountInAfterValidation(std::move(req));
        return;
    }
    {
        const std::shared_ptr<const SessionSnapshot> snap = session_.loadSessionSnapshotForAudioThread();
        const int armedIx = (snap != nullptr) ? snap->findTrackIndexById(armed) : -1;
        if (armedIx < 0 || !trackKindAcceptsRecordArm(snap->getTrack(armedIx).getKind()))
        {
            recorder_.disarm();
            juce::AlertWindow::showMessageBoxAsync(
                juce::AlertWindow::InfoIcon,
                "Recording",
                "Arm an audio track for recording (Group and Stereo Out cannot be armed).");
            juce::Logger::writeToLog("[Rec] start blocked: armed track is not an audio lane");
            return;
        }
    }
    if (!session_.hasKnownProjectFile())
    {
        juce::AlertWindow::showMessageBoxAsync(
            juce::AlertWindow::InfoIcon,
            "Recording",
            "Save the project before recording.");
        juce::Logger::writeToLog("[Rec] start blocked: project not saved to disk");
        return;
    }
    juce::File projectFile = session_.getCurrentProjectFile();
    if (projectFile.getFullPathName().isEmpty())
    {
        juce::Logger::writeToLog("[Rec] start blocked: empty project file path");
        return;
    }
    juce::File audioDir = session_.getCurrentProjectFolder().getChildFile("Audio");
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
    juce::AudioIODevice* const dev = deviceManager_.getCurrentAudioDevice();
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

    // Input-selection slice: resolve the armed track's input assignment against the ACTIVE device
    // configuration to concrete PHYSICAL channels for the whole take. An unresolved assignment
    // blocks recording with a clear message — never a silent substitution of another input.
    int takeNumChannels = 1;
    int takePhysA = -1;
    int takePhysB = -1;
    {
        TrackInputAssignment ia;
        {
            const std::shared_ptr<const SessionSnapshot> snap
                = session_.loadSessionSnapshotForAudioThread();
            const int armedIx = (snap != nullptr) ? snap->findTrackIndexById(armed) : -1;
            if (armedIx >= 0)
            {
                ia = snap->getTrack(armedIx).getInputAssignment();
            }
        }
        const juce::BigInteger activeIn = dev->getActiveInputChannels();
        const juce::StringArray inNames = dev->getInputChannelNames();
        const auto channelLabel = [&inNames](const int phys) {
            juce::String s = "input " + juce::String(phys + 1);
            if (phys >= 0 && phys < inNames.size() && inNames[phys].isNotEmpty())
            {
                s << " (" << inNames[phys] << ")";
            }
            return s;
        };
        switch (ia.kind)
        {
        case TrackInputKind::None:
            juce::AlertWindow::showMessageBoxAsync(
                juce::AlertWindow::InfoIcon,
                "Recording",
                "The armed track's Audio Input is set to \"No input\". Choose an input in the "
                "Inspector, then try again.");
            juce::Logger::writeToLog("[Rec] start blocked: armed track input = None");
            return;
        case TrackInputKind::Mono:
            if (ia.physicalChannelA < 0 || !activeIn[ia.physicalChannelA])
            {
                juce::AlertWindow::showMessageBoxAsync(
                    juce::AlertWindow::WarningIcon,
                    "Recording",
                    "The armed track's selected input — " + channelLabel(ia.physicalChannelA)
                        + " — is not available on the current audio device. Enable it in the "
                          "audio settings or choose another input in the Inspector. The saved "
                          "selection is kept unchanged.");
                juce::Logger::writeToLog("[Rec] start blocked: mono input unresolved (physical "
                                         + juce::String(ia.physicalChannelA) + ")");
                return;
            }
            takePhysA = ia.physicalChannelA;
            break;
        case TrackInputKind::StereoPair:
            if (ia.physicalChannelA < 0 || ia.physicalChannelB < 0
                || !activeIn[ia.physicalChannelA] || !activeIn[ia.physicalChannelB])
            {
                juce::AlertWindow::showMessageBoxAsync(
                    juce::AlertWindow::WarningIcon,
                    "Recording",
                    "The armed track's selected stereo input pair — "
                        + channelLabel(ia.physicalChannelA) + " + " + channelLabel(ia.physicalChannelB)
                        + " — is not fully available on the current audio device. Enable both "
                          "channels or choose another input in the Inspector. The saved selection "
                          "is kept unchanged.");
                juce::Logger::writeToLog("[Rec] start blocked: stereo input unresolved (physical "
                                         + juce::String(ia.physicalChannelA) + "+"
                                         + juce::String(ia.physicalChannelB) + ")");
                return;
            }
            takeNumChannels = 2;
            takePhysA = ia.physicalChannelA;
            takePhysB = ia.physicalChannelB;
            break;
        case TrackInputKind::DefaultFirstInput:
        default:
            // Legacy-compatible default: the first ACTIVE device input as mono — exactly the
            // pre-input-selection capture source (packed position 0). Resolved here so the take
            // keeps this concrete channel even if the device changes mid-take.
            takePhysA = activeIn.findNextSetBit(0);
            if (takePhysA < 0)
            {
                juce::AlertWindow::showMessageBoxAsync(
                    juce::AlertWindow::WarningIcon,
                    "Audio",
                    "No input channel is active. Enable an input in your audio device, then try again.");
                juce::Logger::writeToLog("[Rec] start blocked: no active input for default assignment");
                return;
            }
            break;
        }
    }

    cycleRecordingActive_ = false;
    const bool cycleOn = transport_.readCycleEnabledForUi();
    const std::int64_t locL = session_.getLeftLocatorSamples();
    const std::int64_t locR = session_.getRightLocatorSamples();
    if (cycleOn && locR > locL && locR > 0)
    {
        cycleRecordingActive_ = true;
        cycleSessionLocL_ = locL;
        cycleSessionLocR_ = locR;
        numCompletedPasses_ = 0;
    }

    BeginRecordingRequest req;
    req.takeFile = takeWav;
    req.targetTrackId = armed;
    req.recordingStartSample = 0;
    req.sampleRate = sr;
    req.numChannels = takeNumChannels;
    req.inputPhysicalChannelA = takePhysA;
    req.inputPhysicalChannelB = takePhysB;
    pendingMidiTake_ = !midiTracks.empty();
    startCountInAfterValidation(std::move(req));
}

bool RecordingCoordinator::isCountInActive() const noexcept
{
    return pendingCountIn_.has_value();
}

void RecordingCoordinator::reconcileCycleBookingAfterUndoSnapshotRestore()
{
    if (cycleSessionTrackId_ == kInvalidTrackId)
    {
        return;
    }
    const std::shared_ptr<const SessionSnapshot> snap = session_.loadSessionSnapshotForAudioThread();
    if (snap == nullptr || snap->findTrackIndexById(cycleSessionTrackId_) < 0)
    {
        if constexpr (undo_diagnostic::kUndoDiag)
        {
            writeUndoDiagnosticLogLine("[UndoDiag] cycleSessionTrackId cleared (track missing from snapshot)");
        }
        cycleSessionTrackId_ = kInvalidTrackId;
    }
}

void RecordingCoordinator::cancelCountIn()
{
    if (countInTimer_ != nullptr)
    {
        countInTimer_->stopTimer();
    }
    countInAwaitingPostClickDelay_ = false;
    pendingCountIn_.reset();
    pendingMidiTake_ = false;
    if (callbacks_.setMidiTakePending)
    {
        callbacks_.setMidiTakePending(false);
    }
    countInClicks_.cancel();
    countInStatusLabel_.setText({}, juce::dontSendNotification);
    if (cycleRecordingActive_)
    {
        cycleRecordingActive_ = false;
        callbacks_.clearCycleRecordingPreviewContext();
        if (cycleRecordingWrapTimer_ != nullptr)
        {
            cycleRecordingWrapTimer_->stopTimer();
        }
        numCompletedPasses_ = 0;
        lastSeenWrapCount_ = 0;
    }
    juce::Logger::writeToLog("[Rec] count-in cancelled");
}

void RecordingCoordinator::onCountInTimerTick()
{
    if (!pendingCountIn_.has_value())
    {
        if (countInTimer_ != nullptr)
        {
            countInTimer_->stopTimer();
        }
        return;
    }
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

void RecordingCoordinator::startCountInAfterValidation(BeginRecordingRequest&& req)
{
    countInClicks_.prepare(req.sampleRate);
    pendingCountIn_ = std::move(req);
    countInBeat_ = 0;
    countInAwaitingPostClickDelay_ = false;
    if (countInTimer_ == nullptr)
    {
        countInTimer_ = std::make_unique<CountInTimer>(*this);
    }
    countInStatusLabel_.setText("Count-in…", juce::dontSendNotification);
    static constexpr int kCountInIntervalMs = 375;
    countInTimer_->startTimer(kCountInIntervalMs);
    juce::Logger::writeToLog(
        "[Rec] count-in started (8 clicks, 375 ms, +375 ms pre-roll before record)");
    // The rows that will record are known now: destinations that play a proxy prepare their live
    // source during the count-in, off the audio thread (cancel withdraws it again).
    if (pendingMidiTake_ && callbacks_.setMidiTakePending)
    {
        callbacks_.setMidiTakePending(true);
    }
}

void RecordingCoordinator::completeCountInAndStartRecording()
{
    if (!pendingCountIn_.has_value())
    {
        return;
    }
    BeginRecordingRequest req = *pendingCountIn_;
    pendingCountIn_.reset();
    countInStatusLabel_.setText({}, juce::dontSendNotification);

    const bool armedCycleSession = cycleRecordingActive_;
    const bool audioTake = req.targetTrackId != kInvalidTrackId;
    const bool midiTake = pendingMidiTake_;
    pendingMidiTake_ = false;
    // ONE boundary for every armed row, audio and MIDI alike: the transport position when the
    // count-in finishes. The MIDI capture maps each gesture onto this timeline (see
    // LiveMidiInputBus time model); the audio take places its file here (+ its own offset).
    req.recordingStartSample = transport_.readPlayheadSamplesForUi();
    // Recording at / past the end of the arrangement: the engine's record run keeps the transport
    // running through the take; this coordinator only follows with the navigable extent (small
    // display margin, see `onRecordingExtentFollowTick`) and removes that margin again at Stop.
    storedExtentBeforeRun_ = session_.getStoredArrangementExtentSamples();
    runSampleRate_ = req.sampleRate;
    if (armedCycleSession)
    {
        cycleSessionTrackId_ = req.targetTrackId;
        cycleSessionSampleRate_ = req.sampleRate;
        cycleSessionTakeFile_ = req.takeFile;
        cycleSessionRecordingStartSample_ = req.recordingStartSample;
    }

    if (audioTake && !recorder_.beginRecording(req))
    {
        if (armedCycleSession)
        {
            cycleRecordingActive_ = false;
            callbacks_.clearCycleRecordingPreviewContext();
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
        juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::WarningIcon, "Recording", err);
        juce::Logger::writeToLog(juce::String{"[Rec] beginRecording failed: "} + err);
        if (callbacks_.setMidiTakePending)
        {
            callbacks_.setMidiTakePending(false); // no temporary live-source request may linger
        }
        return; // the MIDI take is not started either: one take, one outcome
    }

    if (midiTake && callbacks_.beginMidiTake)
    {
        // Cycle context from the transport as it is NOW (the same decision the audio cycle session
        // took above when an audio track records; a MIDI-only take reads it here).
        const std::int64_t locL = session_.getLeftLocatorSamples();
        const std::int64_t locR = session_.getRightLocatorSamples();
        const bool cycleActive = transport_.readCycleEnabledForUi() && locR > locL && locR > 0;
        callbacks_.beginMidiTake(req.recordingStartSample, req.sampleRate, cycleActive, locL, locR,
                                 transport_.readCycleWrapCountForUi());
        midiTakeActive_ = true;
        juce::Logger::writeToLog("[Rec] MIDI take started at sample "
                                 + juce::String((juce::int64)req.recordingStartSample)
                                 + (audioTake ? " (with audio take)" : " (MIDI only)")
                                 + (cycleActive ? " cycle" : " linear"));
    }

    if (armedCycleSession)
    {
        lastSeenWrapCount_ = transport_.readCycleWrapCountForUi();
        numCompletedPasses_ = 0;
        callbacks_.setCycleRecordingPreviewContext(
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

    // Intent first, run start second (both release-stores): the callback that acknowledges the
    // start is the first playing block, so the acknowledged start boundary IS the take's first
    // sample for audio and MIDI alike.
    transport_.requestPlaybackIntent(PlaybackIntent::Playing);
    playbackEngine_.requestRecordRunStart();
    if (extentFollowTimer_ == nullptr)
    {
        extentFollowTimer_ = std::make_unique<RecordingExtentFollowTimer>(*this);
    }
    extentFollowActive_ = true;
    extentFollowTimer_->startTimerHz(10);
    callbacks_.updatePlayPauseButtonFromTransport();
}
