#pragma once

// =============================================================================
// TrackPowerCoordinator — plug-in instances follow the track Power (Off) flag
// (docs/RESOURCE_SAVING_2026-11-05.md §3)
// =============================================================================
// Message thread only. The session row's Off flag is the truth; this coordinator makes the
// runtime follow it:
//
//   * Off: the row's insert instances become dormant placeholders
//     (`PluginInsertHost::makeTrackInsertsDormant`) and its instrument Primary is unloaded after
//     its state was captured into the controller's retained row. The Secondary instance, if any,
//     is retired too (its configuration lives on the controller). Nothing is captured or
//     destroyed without an acknowledged drain; a refused drain is retried on a later tick while
//     Off keeps the row silent.
//   * On: `requestPowerOn` does NOT switch the row On at once when it has unloaded plug-ins. It
//     keeps the row Off, loads the instrument and then each dormant insert on separate
//     message-loop turns, and switches the row On only when everything is in place, so the
//     callback never runs a half-built chain. A plug-in that fails to load stays preserved as
//     data and is reported; the row still comes On (as after a project load with a missing
//     plug-in).
//   * A snapshot restore (undo / redo) that turns a row with unloaded plug-ins On is put back
//     Off and woken the same way.
//
// Each wake job carries a token. Deleting the row, a project change or another request for the
// same row abandons a stale job; a stale step never touches another row.

#include "domain/SessionSnapshot.h"
#include "domain/Track.h"

#include <juce_events/juce_events.h>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <set>

class PluginInsertHost;
class ExperimentalInstrumentHost;
class InstrumentTrackController;

class TrackPowerCoordinator final : private juce::Timer
{
public:
    struct Callbacks
    {
        std::function<std::shared_ptr<const SessionSnapshot>()> loadSnapshot;
        /// Writes the row's Off flag (instrument / MIDI rows through their controller mirror).
        std::function<void(TrackId, bool off)> setTrackOff;
        /// True once the audio callback has been observed outside its processing section.
        std::function<bool()> waitForAudioCallbackExit;
        std::function<ExperimentalInstrumentHost*(TrackId)> instrumentHost;
        std::function<InstrumentTrackController*(TrackId)> instrumentController;
        std::function<bool(TrackId)> hasSecondaryInstance;
        std::function<void(TrackId)> retireSecondaryInstance;
        /// Playing, recording, count-in or a project load: Power must not flip now.
        std::function<bool()> transportBusy;
        /// Runtime changed (headers, Inspector, mixer).
        std::function<void()> onRuntimeChanged;
        /// Progress text while loading (empty = hide).
        std::function<void(const juce::String&)> showProgress;
        /// A plug-in could not be loaded when the row came On.
        std::function<void(const juce::String&)> reportLoadProblem;
    };

    TrackPowerCoordinator(PluginInsertHost& inserts, Callbacks callbacks);
    ~TrackPowerCoordinator() override;

    /// Power On from a header, the mixer or a scenario. Returns false when refused (busy).
    bool requestPowerOn(TrackId tid);
    /// True while `tid` is being loaded for On.
    [[nodiscard]] bool isWaking(TrackId tid) const noexcept { return jobs_.count(tid) != 0; }
    [[nodiscard]] bool anyWaking() const noexcept { return !jobs_.empty(); }

    /// Bring the runtime in line with the published snapshot now (also runs on the timer).
    void reconcileNow();
    /// Project replaced or closing: drop every wake job.
    void noteProjectChanged();

    /// Rows whose plug-ins are unloaded (diagnostics / tests).
    [[nodiscard]] bool rowHasUnloadedPlugins(TrackId tid) const;

private:
    struct WakeJob
    {
        std::uint64_t token = 0;
        bool instrumentDone = false;
        int insertsDone = 0;
        int insertsTotal = 0;
        juce::StringArray problems;
    };

    void timerCallback() override;
    void stepOneWakeJob();
    bool unloadRow(const Track& tr);
    [[nodiscard]] bool rowHasLivePlugins(TrackId tid) const;
    [[nodiscard]] juce::String rowName(TrackId tid) const;
    void finishJob(TrackId tid, WakeJob& job);
    void scheduleTimer();

    PluginInsertHost& inserts_;
    Callbacks cb_;
    std::map<TrackId, WakeJob> jobs_;
    std::uint64_t nextToken_ = 1;
    std::shared_ptr<const SessionSnapshot> lastReconciled_;
    bool retryPending_ = false;
    /// Rows whose instrument did not load (or could not be captured): not woken again until Off.
    std::set<TrackId> failedInstrumentLoad_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TrackPowerCoordinator)
};
