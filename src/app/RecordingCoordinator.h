#pragma once

#include <JuceHeader.h>

#include <functional>
#include <memory>
#include <optional>

#include "domain/Track.h"
#include "engine/RecorderService.h"

class Transport;
class Session;
class RecorderService;
class CountInClickOutput;
class LatencySettingsStore;

/// Message-thread orchestration for count-in, linear recording commit, and cycle recording split/commit.
/// Does not own `Session`, `Transport`, UI views, or the recorder implementation.
class RecordingCoordinator
{
public:
    struct Callbacks
    {
        std::function<void()> updatePlayPauseButtonFromTransport;
        std::function<void()> syncViewportFromSession;
        std::function<void()> repaintRulerAndLanes;

        std::function<void(bool active,
                           std::int64_t cycleLocL,
                           std::int64_t cycleLocR,
                           std::int64_t recordingStartSample,
                           std::uint32_t lastSeenWrapCount)>
            setCycleRecordingPreviewContext;

        std::function<void()> clearCycleRecordingPreviewContext;

        // ---- Live MIDI take (LiveMidiInputCoordinator seam; all optional) -------------------
        /// Rows that are record-armed AND have a MIDI input configured right now.
        std::function<std::vector<TrackId>()> armedMidiTracksReadyToRecord;
        /// Start capturing at the record boundary (same moment the audio take begins).
        std::function<void(std::int64_t recordStartSample, double sampleRate)> beginMidiTake;
        /// Finalize at the stop boundary: builds + appends one clip per row with content.
        /// Returns the number of clips created. Called INSIDE `runUndoableTakeCommit`.
        std::function<int(std::int64_t recordStopSample)> commitMidiTake;
        /// Drop a take without clips (count-in cancel after arming, failure paths).
        std::function<void()> abortMidiTake;
        /// Wrap the whole take commit (audio clip add + MIDI clips) in ONE undo step
        /// (`UndoRedoCoordinator::executeUndoableRecordingCommit`). When absent, `commit` runs
        /// directly (no undo step — the pre-live-MIDI behaviour of audio takes).
        std::function<void(const juce::String& label, std::function<void()> commit)> runUndoableTakeCommit;
    };

    RecordingCoordinator(Transport& transport,
                         Session& session,
                         juce::AudioDeviceManager& deviceManager,
                         RecorderService& recorder,
                         CountInClickOutput& countInClicks,
                         LatencySettingsStore& latencyStore,
                         juce::Label& countInStatusLabel,
                         Callbacks callbacks);

    ~RecordingCoordinator();

    void numpadRecordToggled();
    void stopRecordingAndCommitFromUi(const char* sourceContext);
    void cancelCountIn();
    [[nodiscard]] bool isCountInActive() const noexcept;
    /// True while a take is running — an audio take (`RecorderService::isRecording()`) and/or a
    /// live MIDI take. Every "no edits while recording" guard must use this, not the recorder
    /// alone, because a MIDI-only take never starts the audio recorder.
    [[nodiscard]] bool isRecordingInProgress() const noexcept;
    [[nodiscard]] bool isMidiTakeActive() const noexcept { return midiTakeActive_; }
    /// Project replacement while a MIDI take runs: the take belongs to the OLD project and is
    /// dropped without clips (never half-committed into the new one). Count-in is cancelled too.
    void abortMidiTakeForProjectReplace();

    /// Undo/redo: clear cycle booking id if its track vanished from the restored snapshot (diagnostics preserved).
    void reconcileCycleBookingAfterUndoSnapshotRestore();

    /// Install the live-MIDI take seam (see `Callbacks`); called once from the composition root.
    void setLiveMidiTakeCallbacks(std::function<std::vector<TrackId>()> armedMidiTracksReadyToRecord,
                                  std::function<void(std::int64_t, double)> beginMidiTake,
                                  std::function<int(std::int64_t)> commitMidiTake,
                                  std::function<void()> abortMidiTake,
                                  std::function<void(const juce::String&, std::function<void()>)> runUndoableTakeCommit)
    {
        callbacks_.armedMidiTracksReadyToRecord = std::move(armedMidiTracksReadyToRecord);
        callbacks_.beginMidiTake = std::move(beginMidiTake);
        callbacks_.commitMidiTake = std::move(commitMidiTake);
        callbacks_.abortMidiTake = std::move(abortMidiTake);
        callbacks_.runUndoableTakeCommit = std::move(runUndoableTakeCommit);
    }

private:
    struct CountInTimer;
    struct CycleRecordingWrapTimer;

    void onCountInTimerTick();
    void onCycleRecordingWrapTimerTick();
    void startCountInAfterValidation(BeginRecordingRequest&& req);
    void completeCountInAndStartRecording();

    Transport& transport_;
    Session& session_;
    juce::AudioDeviceManager& deviceManager_;
    RecorderService& recorder_;
    CountInClickOutput& countInClicks_;
    LatencySettingsStore& latencyStore_;
    juce::Label& countInStatusLabel_;
    Callbacks callbacks_;

    std::optional<BeginRecordingRequest> pendingCountIn_;
    int countInBeat_ = 0;
    bool countInAwaitingPostClickDelay_ = false;
    std::unique_ptr<CountInTimer> countInTimer_;

    std::unique_ptr<CycleRecordingWrapTimer> cycleRecordingWrapTimer_;
    bool cycleRecordingActive_ = false;
    /// Live MIDI take state (message thread). A take may be MIDI-only (no audio recorder).
    bool midiTakeActive_ = false;
    bool pendingMidiTake_ = false; ///< set during count-in when MIDI rows will record
    TrackId cycleSessionTrackId_ = kInvalidTrackId;
    std::int64_t cycleSessionLocL_ = 0;
    std::int64_t cycleSessionLocR_ = 0;
    std::int64_t cycleSessionRecordingStartSample_ = 0;
    double cycleSessionSampleRate_ = 0.0;
    juce::File cycleSessionTakeFile_;
    std::uint32_t lastSeenWrapCount_ = 0;
    int numCompletedPasses_ = 0;
};
