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
class PlaybackEngine;

/// The ONE recording-run boundary pair every finalization uses (audio take placement / cycle
/// slicing and the MIDI take), as acknowledged by the audio thread (`PlaybackEngine` record run):
/// raw capture positions — placement compensation is applied afterwards by each consumer.
struct RecordRunBoundaries
{
    std::int64_t startTimelineSample = 0;
    std::int64_t startMonoSample = 0;
    std::uint32_t startWrapSerial = 0;
    std::int64_t stopTimelineSample = 0;
    std::int64_t stopMonoSample = 0;
    std::uint32_t stopWrapSerial = 0;
    /// False when no callback acknowledged the stop (device stopped / lost): the values are the
    /// message thread's fallback reads.
    bool acknowledgedByEngine = false;
};

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
        /// Rows that are record-armed AND whose MIDI input can deliver right now.
        std::function<std::vector<TrackId>()> armedMidiTracksReadyToRecord;
        /// Human lines ("<track>: <reason>") for armed MIDI rows that are NOT ready: no input
        /// selected, device missing, device could not be opened, All without a connected device.
        /// Used to tell the user the real reason instead of a generic "arm a track".
        std::function<juce::StringArray()> describeArmedMidiRowsNotReady;
        /// Start capturing at the record boundary (same moment the audio take begins). With
        /// `cycleActive` the MIDI take is split into passes at the engine's wrap markers using the
        /// same locators and start boundary as the audio cycle slices; `wrapSerialAtStart` is the
        /// transport wrap count at that moment (lane preview).
        std::function<void(std::int64_t recordStartSample, double sampleRate, bool cycleActive,
                           std::int64_t leftLocatorSample, std::int64_t rightLocatorSample,
                           std::uint32_t wrapSerialAtStart)>
            beginMidiTake;
        /// Finalize at the engine-acknowledged run boundaries (the same pair the audio take uses):
        /// builds + appends the takes (one per pass and row). Returns the number of clips
        /// created. Called INSIDE `runUndoableTakeCommit`.
        std::function<int(const RecordRunBoundaries&)> commitMidiTake;
        /// Drop a take without clips (count-in cancel after arming, failure paths).
        std::function<void()> abortMidiTake;
        /// Count-in started (`true`, the rows that will record are the ready armed rows) or
        /// cancelled (`false`): destinations that play a proxy prepare their live source ahead of
        /// the first recorded block (`LiveMidiInputCoordinator::setTakePending`). Optional.
        std::function<void(bool pending)> setMidiTakePending;
        /// Wrap the whole take commit (audio clip add + MIDI clips) in ONE undo step
        /// (`UndoRedoCoordinator::executeUndoableRecordingCommit`). When absent, `commit` runs
        /// directly (no undo step — the pre-live-MIDI behaviour of audio takes).
        std::function<void(const juce::String& label, std::function<void()> commit)> runUndoableTakeCommit;
    };

    RecordingCoordinator(Transport& transport,
                         Session& session,
                         PlaybackEngine& playbackEngine,
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
    /// [Diagnostics / stability] The text of the last refusal shown by `numpadRecordToggled`
    /// (empty when the last press started a count-in or stopped a take).
    [[nodiscard]] juce::String getLastRecordStartRefusalForDiagnostics() const { return lastRecordStartRefusal_; }
    /// Project replacement while a MIDI take runs: the take belongs to the OLD project and is
    /// dropped without clips (never half-committed into the new one). Count-in is cancelled too.
    void abortMidiTakeForProjectReplace();

    /// Undo/redo: clear cycle booking id if its track vanished from the restored snapshot (diagnostics preserved).
    void reconcileCycleBookingAfterUndoSnapshotRestore();

    /// Install the live-MIDI take seam (see `Callbacks`); called once from the composition root.
    void setLiveMidiTakeCallbacks(std::function<std::vector<TrackId>()> armedMidiTracksReadyToRecord,
                                  std::function<juce::StringArray()> describeArmedMidiRowsNotReady,
                                  std::function<void(std::int64_t, double, bool, std::int64_t, std::int64_t, std::uint32_t)> beginMidiTake,
                                  std::function<int(const RecordRunBoundaries&)> commitMidiTake,
                                  std::function<void()> abortMidiTake,
                                  std::function<void(const juce::String&, std::function<void()>)> runUndoableTakeCommit,
                                  std::function<void(bool)> setMidiTakePending = {})
    {
        callbacks_.armedMidiTracksReadyToRecord = std::move(armedMidiTracksReadyToRecord);
        callbacks_.describeArmedMidiRowsNotReady = std::move(describeArmedMidiRowsNotReady);
        callbacks_.beginMidiTake = std::move(beginMidiTake);
        callbacks_.commitMidiTake = std::move(commitMidiTake);
        callbacks_.abortMidiTake = std::move(abortMidiTake);
        callbacks_.runUndoableTakeCommit = std::move(runUndoableTakeCommit);
        callbacks_.setMidiTakePending = std::move(setMidiTakePending);
    }

    /// [Diagnostics / stability] Boundaries of the last finished run (as used by the commits).
    [[nodiscard]] RecordRunBoundaries getLastRunBoundariesForDiagnostics() const noexcept { return lastRunBoundaries_; }

    /// Optional: when it returns a non-empty text, a Record START is refused with that text (shown
    /// like the other refusals, kept in `getLastRecordStartRefusalForDiagnostics`). Stopping a
    /// running take / cancelling a count-in is never blocked. Used while a staged project load
    /// owns the session.
    void setRecordStartBlockedPredicate(std::function<juce::String()> fn) { recordStartBlocked_ = std::move(fn); }

private:
    struct CountInTimer;
    struct CycleRecordingWrapTimer;
    struct RecordingExtentFollowTimer;

    void onCountInTimerTick();
    void onCycleRecordingWrapTimerTick();
    /// While a run captures: grow the navigable arrangement extent ahead of the playhead (small
    /// display margin) off the audio thread — the engine already runs past the end on its own.
    void onRecordingExtentFollowTick();
    void startCountInAfterValidation(BeginRecordingRequest&& req);
    void completeCountInAndStartRecording();
    /// Stop the engine's record run and collect the acknowledged boundaries (bounded wait;
    /// message-thread fallback when no callback arrives).
    [[nodiscard]] RecordRunBoundaries stopRecordRunAndCollectBoundaries();

    Transport& transport_;
    Session& session_;
    PlaybackEngine& playbackEngine_;
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
    juce::String lastRecordStartRefusal_;
    std::function<juce::String()> recordStartBlocked_;
    TrackId cycleSessionTrackId_ = kInvalidTrackId;
    std::int64_t cycleSessionLocL_ = 0;
    std::int64_t cycleSessionLocR_ = 0;
    std::int64_t cycleSessionRecordingStartSample_ = 0;
    double cycleSessionSampleRate_ = 0.0;
    juce::File cycleSessionTakeFile_;
    std::uint32_t lastSeenWrapCount_ = 0;
    int numCompletedPasses_ = 0;
    /// Recording past the arrangement end: the stored extent before the run (restored at Stop so
    /// no display headroom is persisted; the result's own clips keep whatever room they need).
    std::unique_ptr<RecordingExtentFollowTimer> extentFollowTimer_;
    bool extentFollowActive_ = false;
    std::int64_t storedExtentBeforeRun_ = 0;
    double runSampleRate_ = 0.0;
    RecordRunBoundaries lastRunBoundaries_;
};
