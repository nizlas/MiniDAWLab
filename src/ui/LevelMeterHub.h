#pragma once

// =============================================================================
// LevelMeterHub — one 30 Hz collector that feeds every meter view (Inspector, mixer) per TrackId
// =============================================================================
//
// ROLE
//   The engine's meter accumulators are drain-and-reset windows: whoever drains first takes the
//   peak. With two views (Inspector channel panel + mixer strips) showing the same row, each
//   window must reach BOTH exactly once. The hub is the only drainer: on every tick it
//   (1) asks its listeners which rows they display, publishes that interest to the engine's
//   concurrent meter bank (`PlaybackEngine::setConcurrentlyMeteredTracks`), (2) drains one
//   window per interested row plus the Stereo Out window, and (3) hands each window to every
//   listener (`meterWindowArrived`). The Master row's window is the Stereo Out accumulator,
//   delivered under the Master row's TrackId, so views treat all rows uniformly.
//
// WHAT STAYS WHERE
//   Peak hold, the fall animation, the overload latch and the DC tag stay inside each
//   `LevelMeterComponent` (it receives identical windows, so two meters of one row agree); the
//   overload ACKNOWLEDGEMENT is shared through `acknowledgeOverload(trackId)` → every listener
//   resets its latch for that row, so clicking the lamp in the mixer also clears the Inspector.
//   The engine's diagnostics windows (`drain*ForDiagnostics`) are never touched here.
//
// COST
//   Rows nobody displays are not metered (the bank folds only the published interest set), so a
//   hidden mixer costs nothing on the audio thread beyond the Inspector's one row.
//
// THREADING
//   [Message thread] only. Listeners are raw non-owning pointers that must `removeListener`
//   before they die (the hub outlives every view; see the composition root).
// =============================================================================

#include "domain/Track.h"
#include "engine/LevelMeterAccumulator.h"

#include <juce_events/juce_events.h>

#include <functional>
#include <vector>

class LevelMeterHub final : private juce::Timer
{
public:
    static constexpr int kTickHz = 30;

    /// Engine seams (installed by the composition root; the hub never includes the engine).
    struct EngineHooks
    {
        /// `PlaybackEngine::setConcurrentlyMeteredTracks` — returns the overflow count.
        std::function<int(const std::vector<TrackId>&)> setMeteredTracks;
        /// `PlaybackEngine::drainConcurrentTrackLevels`.
        std::function<level_meter::Reading(TrackId)> drainTrack;
        /// `PlaybackEngine::drainMasterOutputLevels` (the Stereo Out window).
        std::function<level_meter::Reading()> drainMaster;
        /// Current Master row id (`SessionSnapshot::findCanonicalMasterTrackId`) or invalid.
        std::function<TrackId()> masterTrackId;
    };

    class Listener
    {
    public:
        virtual ~Listener() = default;
        /// Append the NON-master rows this view displays right now (hidden views append nothing).
        virtual void collectMeterInterest(std::vector<TrackId>& out) = 0;
        /// One drained window for `trackId` (the Master row arrives under its own id).
        virtual void meterWindowArrived(TrackId trackId, const level_meter::Reading& reading, double nowSeconds) = 0;
        /// A view acknowledged the overload lamp of `trackId`: reset yours for that row too.
        virtual void meterOverloadAcknowledged(TrackId trackId) = 0;
        /// Animation tick (wall clock) after the windows of this tick were delivered.
        virtual void meterTick(double nowSeconds) = 0;
    };

    LevelMeterHub();
    ~LevelMeterHub() override;

    void setEngineHooks(EngineHooks hooks);

    void addListener(Listener* l);
    void removeListener(Listener* l);

    /// A meter lamp was clicked in some view: fan the acknowledgement out to every listener.
    void acknowledgeOverload(TrackId trackId);

    /// Re-publish the interest set now (a view opened / closed or switched rows); otherwise it
    /// is recomputed on the next tick anyway.
    void refreshInterestNow();

    /// Runs one tick synchronously (tests / scenarios).
    void tickNowForTest() { timerCallback(); }

    /// Rows currently published to the engine (message-thread view; tests / diagnostics).
    [[nodiscard]] const std::vector<TrackId>& publishedInterest() const noexcept { return publishedInterest_; }
    /// Rows that could not be metered because the engine bank is full (0 normally).
    [[nodiscard]] int lastOverflowCount() const noexcept { return lastOverflow_; }

private:
    void timerCallback() override;
    void publishInterestIfChanged();

    EngineHooks hooks_;
    std::vector<Listener*> listeners_;
    std::vector<TrackId> publishedInterest_;
    std::vector<TrackId> scratchInterest_;
    int lastOverflow_ = 0;
    bool inTick_ = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LevelMeterHub)
};
