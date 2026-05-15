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

    /// Undo/redo: clear cycle booking id if its track vanished from the restored snapshot (diagnostics preserved).
    void reconcileCycleBookingAfterUndoSnapshotRestore();

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
    TrackId cycleSessionTrackId_ = kInvalidTrackId;
    std::int64_t cycleSessionLocL_ = 0;
    std::int64_t cycleSessionLocR_ = 0;
    std::int64_t cycleSessionRecordingStartSample_ = 0;
    double cycleSessionSampleRate_ = 0.0;
    juce::File cycleSessionTakeFile_;
    std::uint32_t lastSeenWrapCount_ = 0;
    int numCompletedPasses_ = 0;
};
