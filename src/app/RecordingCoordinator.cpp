#include "app/RecordingCoordinator.h"

#include <algorithm>
#include <vector>

#include <juce_gui_basics/juce_gui_basics.h>

#include "domain/AudioClip.h"
#include "domain/Session.h"
#include "domain/SessionSnapshot.h"
#include "diagnostics/UndoDiagnosticConfig.h"
#include "diagnostics/UndoDiagnosticFileLog.h"
#include "engine/CountInClickOutput.h"
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

RecordingCoordinator::RecordingCoordinator(Transport& transport,
                                           Session& session,
                                           juce::AudioDeviceManager& deviceManager,
                                           RecorderService& recorder,
                                           CountInClickOutput& countInClicks,
                                           LatencySettingsStore& latencyStore,
                                           juce::Label& countInStatusLabel,
                                           Callbacks callbacks)
    : transport_(transport)
    , session_(session)
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
    if (!recorder_.isRecording())
    {
        return;
    }
    if (sourceContext != nullptr)
    {
        juce::Logger::writeToLog(juce::String{"[Rec] stop/commit source="} + sourceContext);
    }

    const bool commitCycleTakes = cycleRecordingActive_;
    const TrackId cycleTrackId = cycleSessionTrackId_;
    const std::int64_t cycleLocL = cycleSessionLocL_;
    const std::int64_t cycleLocR = cycleSessionLocR_;
    const std::int64_t cycleStart = cycleSessionRecordingStartSample_;
    const double cycleSr = cycleSessionSampleRate_;

    transport_.requestPlaybackIntent(PlaybackIntent::Stopped);
    callbacks_.updatePlayPauseButtonFromTransport();
    if (cycleRecordingWrapTimer_ != nullptr)
    {
        cycleRecordingWrapTimer_->stopTimer();
    }

    callbacks_.clearCycleRecordingPreviewContext();
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
        juce::Logger::writeToLog(juce::String{"[Rec] stop/finalize failed: "} + r.errorMessage);
        return;
    }

    std::uint32_t wrapFinal = transport_.readCycleWrapCountForUi();
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
            callbacks_.repaintRulerAndLanes();
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
            callbacks_.repaintRulerAndLanes();
            return;
        }

        std::vector<float> pcmStable(
            static_cast<size_t>(juce::jmax<std::int64_t>(std::int64_t{ 0 }, totalAvail)));
        for (std::int64_t i = 0; i < totalAvail; ++i)
        {
            pcmStable[(size_t)i] = pcmLive[i];
        }
        loadedClip.reset();

        const float* const pcm = pcmStable.data();

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
        const std::int64_t recordingPlacementOffsetSamples = latencyStore_.getCurrentRecordingOffsetSamples();
        const std::int64_t committedStartSamples = juce::jmax<std::int64_t>(
            std::int64_t{ 0 }, r.recordingStartSample + recordingPlacementOffsetSamples);

        const juce::Result ar = session_.addRecordedTakeAtSample(
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
            callbacks_.syncViewportFromSession();
        }
    }

    callbacks_.repaintRulerAndLanes();
}

void RecordingCoordinator::numpadRecordToggled()
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
    req.recordingStartSample = transport_.readPlayheadSamplesForUi();
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
        return;
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

    transport_.requestPlaybackIntent(PlaybackIntent::Playing);
    callbacks_.updatePlayPauseButtonFromTransport();
}
