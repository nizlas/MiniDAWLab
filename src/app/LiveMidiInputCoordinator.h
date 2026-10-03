#pragma once

// =============================================================================
// LiveMidiInputCoordinator — message-thread owner of live MIDI input (devices, routing, take capture)
// =============================================================================
//
// ROLE IN THE ARCHITECTURE
//   App-layer coordinator (see docs/CURRENT_ARCHITECTURE.md, "App-layer coordinator map") for the
//   live-MIDI slice. It is the only code that:
//     * talks to JUCE MIDI devices: enumerates `juce::MidiInput::getAvailableDevices()`, assigns
//       each referenced device a stable SLOT, enables it on the app's `AudioDeviceManager` and
//       registers one `MidiInputCallback` per slot that forwards into `LiveMidiInputBus` (the
//       callback itself does nothing else — no UI, no session, no plugin calls);
//     * owns the runtime-only Monitor / Record-arm flags of Instrument and Midi rows (both start
//       OFF when a project opens; the per-track input CONFIGURATION lives in the session/project);
//     * builds the immutable routing snapshot from the session (`Track::getMidiInputAssignment`)
//       plus those flags and publishes it to the bus;
//     * feeds the per-row take trackers from the bus's capture ring (30 Hz) and finalizes takes
//       into clips through `LiveMidiTakeBuilder` + `InstrumentTrackController::appendRecordedTimelineMidiClip`
//       when `RecordingCoordinator` commits a take;
//     * tells the proxy playback policy which destinations currently need a LIVE source
//       (`liveMonitorRequestedForDestination`) and asks it to re-evaluate on every change — the
//       temporary Secondary-for-monitoring source swap is decided there, never here.
//
// THREADS
//   Everything public is message-thread only except the per-slot `MidiInputCallback` objects,
//   whose `handleIncomingMidiMessage` runs on JUCE's MIDI threads and only pushes into the bus.
// =============================================================================

#include <JuceHeader.h>

#include <array>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <vector>

#include "app/LiveMidiTakeBuilder.h"
#include "domain/Track.h"
#include "engine/LiveMidiInputBus.h"
#include "instruments/InstrumentTrackController.h"

class Session;
class PlaybackEngine;
class InstrumentRuntimeCoordinator;
class LatencySettingsStore;

/// One selectable MIDI input device (Inspector list).
struct LiveMidiInputDeviceOption
{
    juce::String identifier;
    juce::String name;
    bool present = true;
};

/// Result of committing one row's take.
struct LiveMidiTakeCommitEntry
{
    TrackId trackId = kInvalidTrackId;
    InstrumentMidiClipId clipId = 0; ///< 0 = no clip (empty take or controller missing)
    live_midi_take::TakeBuildResult build;
};

struct LiveMidiTakeCommitResult
{
    std::vector<LiveMidiTakeCommitEntry> entries;
    int clipsCreated = 0;
    bool captureOverflowSeen = false;
    [[nodiscard]] bool anyClipCreated() const noexcept { return clipsCreated > 0; }
};

class LiveMidiInputCoordinator final : private juce::Timer
{
public:
    struct Callbacks
    {
        /// Re-evaluate one instrument destination's playback source (ProxyPlaybackCoordinator).
        std::function<void(TrackId destination)> refreshProxyDestination;
        /// True while the destination plays its Secondary ONLY because live monitoring needs a
        /// live source (status text "temporary"). Optional.
        std::function<bool(TrackId destination)> isLiveMonitorOverrideActive;
        /// Header / lane repaint when Monitor, Arm or MIDI activity changes visibly.
        std::function<void()> onUiStateChanged;
        /// Diagnostics line (stability log / juce::Logger) — optional.
        std::function<void(const juce::String&)> logLine;
    };

    LiveMidiInputCoordinator(Session& session,
                             juce::AudioDeviceManager& deviceManager,
                             PlaybackEngine& playbackEngine,
                             InstrumentRuntimeCoordinator& instrumentRuntime,
                             LatencySettingsStore& latencyStore,
                             Callbacks callbacks);
    ~LiveMidiInputCoordinator() override;

    // ------------------------------------------------------------------ devices
    /// Re-read the device list, (re)enable referenced devices, republish routing. Called on
    /// construction, device-list changes, and whenever a track's MIDI input assignment changed.
    void refreshDevicesAndRouting();
    /// Devices for the Inspector: every present device plus any assigned-but-missing device of
    /// `forTrack` (so a missing assignment stays visible and selectable, never auto-replaced).
    [[nodiscard]] std::vector<LiveMidiInputDeviceOption> availableDevicesFor(TrackId forTrack) const;
    [[nodiscard]] bool isAssignedDevicePresent(TrackId trackId) const;

    // ------------------------------------------------------------------ monitor / arm (runtime)
    void setMonitorEnabled(TrackId trackId, bool enabled);
    [[nodiscard]] bool isMonitorEnabled(TrackId trackId) const noexcept { return monitor_.count(trackId) != 0; }
    void setRecordArmed(TrackId trackId, bool armed);
    [[nodiscard]] bool isRecordArmed(TrackId trackId) const noexcept { return armed_.count(trackId) != 0; }
    /// Project open / replace: Monitor and Arm start OFF; the input configuration comes from the file.
    void clearRuntimeStateForProjectReplace();

    /// True when any monitored row (Instrument row itself, or a Midi row routed via MIDI To) with
    /// a configured input targets `destination` — the proxy policy then needs a live source.
    [[nodiscard]] bool liveMonitorRequestedForDestination(TrackId destination) const;

    // ------------------------------------------------------------------ status for the UI
    /// Rows that are armed AND have a MIDI input that can deliver right now (the rows a Record
    /// will capture): input configured, and — for Device mode — the device present and opened;
    /// for All — at least one input device opened. Instrument availability is NOT a condition.
    [[nodiscard]] std::vector<TrackId> armedTracksReadyToRecord() const;

    /// One armed Instrument / Midi row as the Record validation sees it.
    struct ArmedRowStatus
    {
        TrackId trackId = kInvalidTrackId;
        juce::String trackName;
        bool ready = false;
        /// Human reason when not ready: no input selected / device missing / device could not be
        /// opened / All but no device connected.
        juce::String reason;
    };
    /// Every armed Instrument / Midi row with its readiness and reason (for the Record message).
    [[nodiscard]] std::vector<ArmedRowStatus> armedRowsStatus() const;
    /// MIDI arrived for this row within the last ~150 ms (header activity dot).
    [[nodiscard]] bool isTrackMidiActive(TrackId trackId) const noexcept;
    /// One-line human status for the Inspector: device missing / no playable instrument /
    /// Secondary used for monitoring / overflow. Empty when everything is nominal.
    [[nodiscard]] juce::String describeInputStatus(TrackId trackId) const;
    /// Destination row for live MIDI of `trackId` (self for Instrument rows, MIDI To for Midi rows).
    [[nodiscard]] TrackId destinationForTrack(TrackId trackId) const;

    // ------------------------------------------------------------------ take capture (RecordingCoordinator)
    /// Start capturing for every armed, configured row. `recordStartSample` is the transport
    /// boundary the take begins at (events before it only shape the start state).
    void beginTake(std::int64_t recordStartSample, double sampleRate);
    [[nodiscard]] bool isTakeActive() const noexcept { return takeActive_; }
    [[nodiscard]] std::int64_t takeStartSample() const noexcept { return takeStartSample_; }
    [[nodiscard]] const std::vector<TrackId>& takeTracks() const noexcept { return takeTracks_; }
    /// Finalize at `recordStopSample` and append one clip per row with content (message thread;
    /// the caller wraps this in the recording undo step). Clears the take.
    [[nodiscard]] LiveMidiTakeCommitResult commitTake(std::int64_t recordStopSample);
    /// Drop the take without creating clips (cancel / failure paths).
    void abortTake();

    /// [Tests / stability] The bus, for deterministic injection and counters.
    [[nodiscard]] live_midi::LiveMidiInputBus& busForDiagnostics() noexcept { return bus_; }
    /// [Tests / stability] Slot index of a present device identifier, or -1.
    [[nodiscard]] int slotForDeviceIdentifierForDiagnostics(const juce::String& identifier) const;

private:
    struct SlotCallback;
    struct DeviceSlot
    {
        juce::String identifier;
        juce::String name;
        bool present = false;
        bool enabledOnManager = false;
        /// The last open attempt failed although the device is present (typically: the port is
        /// held exclusively by another application). Retried periodically while still wanted.
        bool openFailed = false;
        bool wanted = false;
        std::unique_ptr<SlotCallback> callback;
    };
    /// Device-mode / All readiness of one row's input right now (shared by status + Record).
    struct InputAvailability
    {
        bool configured = false; ///< mode != None
        bool deliverable = false; ///< at least one opened device feeds this row
        juce::String problem;     ///< empty when deliverable
    };
    [[nodiscard]] InputAvailability inputAvailabilityForTrack(TrackId trackId) const;
    [[nodiscard]] bool anyDeviceOpen() const noexcept;
    void retryFailedDeviceOpens();

    void timerCallback() override;
    void rebuildRoutingIfChanged();
    void drainCaptureRing();
    void updateActivityFromBus();
    [[nodiscard]] int ensureSlotForDevice(const juce::String& identifier, const juce::String& name);
    void enableSlotOnManager(DeviceSlot& slot, int slotIndex, bool enable);
    [[nodiscard]] bool anyTrackUsesAllInputs() const;
    [[nodiscard]] std::set<juce::String> deviceIdentifiersReferencedByTracks() const;
    [[nodiscard]] std::shared_ptr<const live_midi::RoutingSnapshot> buildRoutingSnapshot();
    void refreshProxyDestinationsForTrackChange(TrackId trackId);

    Session& session_;
    juce::AudioDeviceManager& deviceManager_;
    PlaybackEngine& playbackEngine_;
    InstrumentRuntimeCoordinator& instrumentRuntime_;
    LatencySettingsStore& latencyStore_;
    Callbacks callbacks_;

    live_midi::LiveMidiInputBus bus_;
    std::array<DeviceSlot, (size_t)live_midi::kMaxDeviceSlots> slots_;
    std::unique_ptr<juce::MidiDeviceListConnection> deviceListConnection_;

    std::set<TrackId> monitor_;
    std::set<TrackId> armed_;

    std::shared_ptr<const live_midi::RoutingSnapshot> publishedRouting_;
    std::vector<TrackId> publishedRouteTracks_; ///< parallel to the published routes
    std::uint32_t routingRevision_ = 0;
    const void* lastSeenSessionSnapshot_ = nullptr;
    std::set<juce::String> lastReferencedDevices_;
    bool lastAllMode_ = false;

    /// Per-row always-on tracker + the take's event list.
    struct RowCapture
    {
        live_midi_take::TakeStateTracker tracker;
        live_midi_take::TakeStateTracker trackerAtTakeStart;
        std::vector<live_midi_take::TakeEvent> takeEvents;
        bool inTake = false;
    };
    std::map<TrackId, RowCapture> rows_;
    bool takeActive_ = false;
    std::int64_t takeStartSample_ = 0;
    double takeSampleRate_ = 48000.0;
    std::vector<TrackId> takeTracks_;
    std::uint32_t captureOverflowAtTakeStart_ = 0;

    std::vector<std::uint32_t> lastActivityCounts_;
    std::map<TrackId, double> lastActivityMsByTrack_;
    std::set<TrackId> activeNow_;
    double lastOpenRetryMs_ = 0.0;
};
