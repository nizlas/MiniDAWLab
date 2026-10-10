#pragma once

// =============================================================================
// PlaybackEngine.h / PlaybackEngine.cpp — JUCE audio I/O callback: device ← Session + Transport
// =============================================================================
//
// File role: declare the one object registered with juce::AudioDeviceManager to receive block
// callbacks. Implementation narrates the fill loop, mono→stereo rule, and playhead advance in
// PlaybackEngine.cpp.
//
// CLASS RESPONSIBILITY
//   Implements juce::AudioIODeviceCallback. The audio device invokes this object on a high-
//   priority thread to fill output buffers. This class is the *only* bridge from our domain
//   (which sample to play) to the hardware (float arrays per channel). It advances Transport’s
//   playhead (timeline-absolute) to match *timeline* samples advanced while Playing, including
//   silence in gaps. Phase 3: per-track coverage (Phase 2 rule in each lane) plus **sum** across lanes.
//
// OWNERSHIP AND LIFETIME
//   Does not own Transport, Session, or `RecorderService`. The application (Main) owns all of
//   them and outlives the engine. Tear order: removeAudioCallback → destroy `PlaybackEngine` →
//   destroy `RecorderService` so the non-owning `RecorderService*` is never used after the engine
//   dies. The optional recorder pointer is valid for the whole callback lifetime when non-null.
//
// DELIBERATELY NOT RESPONSIBLE FOR
//   File I/O, decoding, waveform UI, or deciding user intent (Play/Pause) beyond *reading* it
//   from Transport. Does not set seek targets — user code queues seeks on Transport; we only
//   run audioThread_beginBlock to apply them at block boundaries.
//
// THREADING (which methods on which thread)
//   See comments on each public override: two are message-thread (device lifecycle) and one is
//   audio-thread (per block). The callback’s body is the central realtime path: it holds to the
//   body-readability tier (in-body plain-language at branches) so JUCE buffer layout and channel
//   indexing are not the only place “what the user hears” is defined.
//
// JUCE: AudioIODeviceCallback is the interface the audio device uses; see .cpp for the
//      implementation body and a plain-language walkthrough of the buffer fill.
//
// Optional `RecorderService` (Phase 4): non-owning pointer for **input** `pushInputBlock` from the
// audio thread only. Does **not** own the recorder, does not call `Transport` / `Session`. May be
// null if recording is not composed in.

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_devices/juce_audio_devices.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "domain/Track.h"
#include "engine/InstrumentRenderPool.h"
#include "engine/LevelMeterAccumulator.h"
#include "engine/TrackMeterBank.h"
#include "engine/LiveMidiInputBus.h"
#include "engine/PlaybackMixHelpers.h"
#include "engine/ReadAheadRenderer.h"
#include "engine/RoutingPlan.h"
#include "engine/SoloMuteView.h"
#include "transport/Transport.h"

class CountInClickOutput;
class ExperimentalInstrumentHost;
class InstrumentTrackController;
class PluginInsertHost;
class RecorderService;
class Session;
class SessionSnapshot;

// =============================================================================
// ExperimentalInstrumentPlaybackSnapshot  —  message-thread publishes, RT reads
// =============================================================================
//
// One immutable vector of `{TrackId, host*, controller*}` pairs (**one entry per hosted instrument
// lane** currently wired in Main). The audio callback walks `SessionSnapshot` `TrackKind::Instrument`
// rows in timeline order and resolves each row **by TrackId** against this vector—**not** “first
// instrument only”. Each match runs that lane’s MIDI + hosted-synth path (`ExperimentalInstrumentHost`).
// Main owns hosts/controllers; pointers are stable for the snapshot's lifetime. Publication uses
// `publishExperimentalInstrumentPlaybackSnapshot` (release-store); reads use acquire-load plus
// `shared_ptr` retain — no mutation and no allocator traffic on RT.
//
// ---------------------------------------------------------------------------
struct ExperimentalInstrumentPlaybackEntry
{
    TrackId trackId = kInvalidTrackId;
    ExperimentalInstrumentHost* host = nullptr;
    InstrumentTrackController* midiController = nullptr;
    /// P2 (steering §17, PID-008): optional Secondary AUDITION host. Non-null only when the
    /// track's Primary is missing, a loaded Secondary exists, and the Secondary is NOT already
    /// `host` (i.e. not the transport source). Processed through the SAME strip as `host`, but
    /// ONLY while the transport is not playing — live audition is never layered over proxy
    /// transport playback (the audio thread gates this per block; no message-thread hook races).
    /// Never receives transport MIDI (scheduling targets `host` only).
    ExperimentalInstrumentHost* auditionHost = nullptr;
};

/// One plugin-less `TrackKind::Midi` source lane (Phase B). Deliberately carries **no destination**:
/// the audio callback reads `Track::getMidiDestinationTrackId()` from the SessionSnapshot each block
/// and resolves it against `entries` by TrackId, so an Inspector "MIDI To" edit is picked up on the
/// next block without republishing this snapshot, and a deleted/illegal destination simply fails to
/// resolve (silent, never dangling).
struct ExperimentalMidiSourcePlaybackEntry
{
    TrackId trackId = kInvalidTrackId;
    InstrumentTrackController* midiController = nullptr;
};

struct ExperimentalInstrumentPlaybackSnapshot
{
    std::vector<ExperimentalInstrumentPlaybackEntry> entries;
    /// `TrackKind::Midi` sources in **session track order** — the deterministic many-to-one merge
    /// order (a destination's own events first, then MIDI sources in ascending track-list order).
    std::vector<ExperimentalMidiSourcePlaybackEntry> midiSources;

    ExperimentalInstrumentPlaybackSnapshot() = default;
    explicit ExperimentalInstrumentPlaybackSnapshot(std::vector<ExperimentalInstrumentPlaybackEntry>&& e)
        : entries(std::move(e))
    {
    }
    ExperimentalInstrumentPlaybackSnapshot(std::vector<ExperimentalInstrumentPlaybackEntry>&& e,
                                           std::vector<ExperimentalMidiSourcePlaybackEntry>&& m)
        : entries(std::move(e))
        , midiSources(std::move(m))
    {
    }
};

class PlaybackEngine : public juce::AudioIODeviceCallback
{
public:
    // Contract: retain non-owning references; Main must outlive the engine and unregister the
    // callback before destroy. Thread: Main / message thread.
    // `recorder` may be null; if non-null, it must outlive this engine (destroy engine before recorder).
    // `countIn` is optional: short count-in metronome clicks to device outputs only (no session/recorder).
    // `pluginHost` optional Phase 8: per-track VST3 insert; must outlive this engine until after
    // `removeAudioCallback` (same tear order as `recorder`).
    PlaybackEngine(Transport& transport, Session& session, RecorderService* recorder = nullptr,
                   CountInClickOutput* countIn = nullptr, PluginInsertHost* pluginHost = nullptr);

    /// Message thread only: installs the next immutable instrument playback view for the RT.
    /// Passing nullptr clears the snapshot; passing an empty snapshot is equivalent (no lookups hit).
    void publishExperimentalInstrumentPlaybackSnapshot(
        std::shared_ptr<const ExperimentalInstrumentPlaybackSnapshot> snapshot) noexcept;

    void setExperimentalInstrumentDeviceLifecycleHooks(
        std::function<void(double sampleRate, int blockSizeSamples)> prepareAllHosts,
        std::function<void()> releaseAllHosts,
        std::function<void(int numSamples)> beginBlockAllHosts) noexcept;

    ~PlaybackEngine() override;

    PlaybackEngine(const PlaybackEngine&) = delete;
    PlaybackEngine& operator=(const PlaybackEngine&) = delete;
    PlaybackEngine(PlaybackEngine&&) = delete;
    PlaybackEngine& operator=(PlaybackEngine&&) = delete;

    // [Audio thread] Realtime: fill `outputChannelData` using **per-track** coverage (front-most
    // `PlacedClip` in each lane that covers each timeline position; gaps = silence **in that lane**).
    // **Across** tracks, samples are **added** into the same output (minimal sum, not a mixer).
    // Optional: forward mono **input[0]** to `RecorderService::pushInputBlock` when a recorder is
    // composed in (independent of `Session`; no-op if not recording or no input channels).
    // No decode, I/O, locks, or UI; no new heap use on the hot path beyond the two snapshot pointer
    // retains (`Session` + instrument playback, same pattern as `Session::loadSessionSnapshotForAudioThread`).
    // See .cpp for coverage runs, mono→stereo, and transport advance.
    void audioDeviceIOCallbackWithContext(const float* const* inputChannelData,
                                          int numInputChannels,
                                          float* const* outputChannelData,
                                          int numOutputChannels,
                                          int numSamples,
                                          const juce::AudioIODeviceCallbackContext& context) override;

    // [Message thread] JUCE: stream starting; reserved for a later phase (e.g. sample rate).
    void audioDeviceAboutToStart(juce::AudioIODevice* device) override;
    // [Message thread] JUCE: stream ended; nothing to release in Phase 1.
    void audioDeviceStopped() override;

    // [Message thread] Audible read position shifts by adding this to the transport timeline sample each block.
    // Positive reads later material; negative reads earlier. Wrap decisions still use unshifted playhead.
    void setPlaybackOffsetSamples(std::int64_t samples) noexcept;

    /// [Message thread] Offline mixdown gate (depth-counted so WAV-inside-MP3 nests safely). While the
    /// depth is > 0, `audioDeviceIOCallbackWithContext` outputs silence only and never touches plugin
    /// hosts / scratch buffers. Returns true when this call made the gate active (depth 0 -> 1); the
    /// caller must then drain the in-flight callback via `isAudioCallbackInProcessingSection()`.
    bool beginOfflineRenderGate() noexcept;
    /// [Message thread] Decrements the gate depth. Returns true when realtime processing resumed
    /// (depth 1 -> 0).
    bool endOfflineRenderGate() noexcept;
    [[nodiscard]] bool isOfflineRenderInProgress() const noexcept;

    /// [Any thread] True while the device callback is between its entry and exit for the current block
    /// (set before the offline gate is checked, so a successful gate + drain guarantees no callback is
    /// touching engine/plugin/scratch state). Seq-cst pairing with `beginOfflineRenderGate`.
    [[nodiscard]] bool isAudioCallbackInProcessingSection() const noexcept;

    /// [Message thread] Bounded sleep-wait until no device callback is in flight (drains the callback
    /// that may still hold a previously published snapshot/map). Publish the new realtime view *before*
    /// calling this, then destroy the retired objects afterwards. Returns true when drained; false on
    /// timeout. `waitedMsOut` (optional) receives the elapsed wait time.
    bool waitForAudioCallbackExit(double maxWaitMs, double* waitedMsOut = nullptr) noexcept;

    /// Stability C2B: coarse "where is the audio callback right now" marker, published with relaxed
    /// stores from the audio thread. Read from the message thread purely for gate-timeout
    /// diagnostics (never for synchronization).
    enum class AudioCallbackPhase : int
    {
        Idle = 0,
        Begin,
        RecorderPush,
        TransportBeginBlock,
        OfflineGateSilence,
        LoadSnapshot,
        InstrumentBeginBlock,
        MixPrep,
        CountIn,
        ClipRender,
        TransportMidiSchedule,
        InstrumentMix,
        FinalizeRouting,
        FinalizeStagedBusLoop,
        FinalizeLegacyBusLoop,
        FinalizeMasterFallback,
    };

    /// [Any thread] One diagnostic line describing the callback's current phase, last block size,
    /// playhead, and transport intent. Logged by gate sites when `waitForAudioCallbackExit` times out.
    [[nodiscard]] juce::String describeAudioCallbackStateForDiagnostics() const noexcept;

    /// Aggregated audio callback cost since the previous snapshot. Purely diagnostic: the audio
    /// thread accumulates relaxed atomics (two clock reads per block), the message thread drains
    /// them. `budgetPercent` = callback duration relative to the wall-clock time one block of audio
    /// represents, i.e. 100% means the callback used its entire real-time budget.
    struct AudioCallbackLoadSnapshot
    {
        std::uint64_t blocks = 0;
        double minMs = 0.0;
        double meanMs = 0.0;
        double maxMs = 0.0;
        double meanBudgetPercent = 0.0;
        double maxBudgetPercent = 0.0;
        std::uint32_t nearOverruns = 0; ///< blocks over 70% of budget
        std::uint32_t overruns = 0;     ///< blocks over 100% of budget
        int lastBlockSamples = 0;
        double sampleRate = 0.0;
    };

    /// [Message thread] Read and clear the accumulated audio callback load window.
    [[nodiscard]] AudioCallbackLoadSnapshot snapshotAudioCallbackLoadAndReset() noexcept;

    // -----------------------------------------------------------------------
    // Parallel live-instrument generation (engine/InstrumentRenderPool.h)
    // -----------------------------------------------------------------------
    // Inside `mixKeyedInstrumentLanesIntoOutputsIfAny` — after every MIDI source of the block has
    // been scheduled into the hosts — the generation stage of each live instrument row (host
    // `processBlock` into the host's own scratch) runs as one job per host across the pool, the
    // callback thread participating and waiting for the last job. The rows' inserts, fader / mute
    // / pan, meters, routing and summing then run exactly as before, in row order, on the callback
    // thread. Proxy-backed hosts, the sessionless fallback and the offline mixdown stay serial.
    // The pool is sized once at construction: `--instrument-workers N` (0 = serial) or
    // `instrument_render::defaultWorkerCount()`.
    [[nodiscard]] int instrumentRenderWorkerCount() const noexcept
    {
        return instrumentRenderPool_ != nullptr ? instrumentRenderPool_->workerCount() : 0;
    }
    // -----------------------------------------------------------------------
    // Experimental read-ahead (docs/READAHEAD_PROTOTYPE.md). `readAhead_` exists only when
    // `configuredReadAheadDepth()` is greater than 0 at construction (the startup resolver,
    // or a test), or a test created it in pump mode. Null = the A1/A2 paths with no read-ahead.
    // -----------------------------------------------------------------------
    /// [Message thread, BEFORE the device starts] Create a deterministic PUMP-mode renderer for
    /// focused tests (no worker thread; the test drives it via `experimentalReadAhead()`).
    void enableExperimentalReadAheadForTests(int depthBlocks);
    [[nodiscard]] readahead::ReadAheadRenderer* experimentalReadAhead() noexcept { return readAhead_.get(); }
    /// [Message thread] Pause+resume of the read-ahead worker — the publish-before-destroy hook
    /// calls this AFTER the new insert map is published and the callback was waited out: an
    /// acknowledged pause proves the worker is outside every chain render that could still
    /// reference retired instances; after the resume it only ever acquires the new map.
    /// Ownership and queues survive (model doc §8) — chain edits late-apply like other controls.
    /// Returns only after a REAL acknowledgment (bounded attempts, unbounded total, with
    /// diagnostics): the retired instances are destroyed right after the hook, so a worker stuck
    /// inside a plugin render STALLS the chain edit with resources retained — publish-before-
    /// destroy holds on the failure path too, it never proceeds into the destroy on a timeout.
    void pauseReadAheadWorkerAfterChainPublish() noexcept;
    /// [Message thread] Plugin-state capture window for Save / Save As / autosave (model doc
    /// §9): gates adoption off, gaplessly drains owned rows while playback consumption runs
    /// (bounded wait; a Playing -> Paused/Stopped transition during the wait resolves to the
    /// paused-capture semantics instead of stalling), then requires an ACKNOWLEDGED worker
    /// pause. Returns true only when the window is really established; on false (drain stalled —
    /// e.g. no callbacks running — or no worker acknowledgment) every hold is released again and
    /// the caller MUST NOT capture plugin state or write the project file as if it succeeded.
    /// `endPluginStateCaptureWindow` must be called only after a successful begin.
    /// Never blocks the audio callback; the wait runs on the caller's (message) thread only.
    [[nodiscard]] bool beginPluginStateCaptureWindow() noexcept;
    void endPluginStateCaptureWindow() noexcept;
    /// [Test only] Shrink/restore the drain wait bound so the failure path is testable.
    void setStateCaptureDrainTimeoutMsForTests(const int timeoutMs) noexcept
    {
        stateCaptureDrainTimeoutMs_.store(juce::jmax(1, timeoutMs), std::memory_order_relaxed);
    }

    [[nodiscard]] instrument_render::InstrumentRenderPool::Stats instrumentRenderPoolStats() const noexcept
    {
        return instrumentRenderPool_ != nullptr ? instrumentRenderPool_->statsRelaxed()
                                                : instrument_render::InstrumentRenderPool::Stats{};
    }
    /// [Message thread] Diagnostic A/B inside one process: force the serial generation path for
    /// the following blocks (the same code runs on the callback thread). Relaxed atomic.
    void setInstrumentRenderSerialForDiagnostics(const bool serial) noexcept
    {
        instrumentRenderSerialHint_.store(serial, std::memory_order_relaxed);
    }
    [[nodiscard]] bool isInstrumentRenderSerialForDiagnostics() const noexcept
    {
        return instrumentRenderSerialHint_.load(std::memory_order_relaxed);
    }

    /// [Any thread] Monotonic count of device-callback entries (relaxed). Two reads that differ
    /// prove the callback is still cycling; equal reads across a wait mean it has stopped.
    [[nodiscard]] std::uint64_t readAudioCallbackEnterCountForDiagnostics() const noexcept
    {
        return audioCallbackEnterCount_.load(std::memory_order_relaxed);
    }

    /// [Message thread] Largest absolute sample written to ANY device output channel since the
    /// previous call (peak hold across blocks), then resets the hold to 0. Distinguishes "callback
    /// runs but the mix is silent" from "sources render and reach the output". Diagnostics only:
    /// the audio thread folds one SIMD min/max per channel into a lock-free max (relaxed).
    [[nodiscard]] float readAndResetOutputPeakHoldForDiagnostics() noexcept
    {
        return outputPeakHold_.exchange(0.0f, std::memory_order_relaxed);
    }

    // -----------------------------------------------------------------------
    // Level meters (Inspector channel panel) — realtime-safe accumulators, UI drains
    // -----------------------------------------------------------------------
    // MEASURING POINTS
    //   * Stereo Out ("master") meter: the final device output of every callback — channels 0/1
    //     after the master bus strip (fader / mute / inserts / pan), i.e. the last float signal
    //     before the device driver converts it. Folded on every return path, including the
    //     offline-gate silence path, so the meter falls to silence while an export runs. The
    //     offline export measures its own output separately (`MixdownExportLevelReport`) —
    //     the live meter never shows export blocks.
    //   * Track meter: the selected track's post-channel-strip stage — after pre-gain, Pre
    //     inserts, fader / mute, Post inserts and pan, BEFORE it is fanned to its output bus and
    //     sends. For audio rows this is the clip path or, when Monitor is on, the live-input
    //     monitoring pass (exactly the source the user hears); instrument rows measure the
    //     host's actual source (Primary / proxy / Secondary audition) through the same strip;
    //     group rows measure their bus strip output. A muted track measures 0; a track that is
    //     off (or not rendered this block) folds nothing and reads as silence.
    // Only ONE track is metered at a time (`setMeteredTrackForUi`); the audio thread compares
    // the row id against a relaxed atomic — no per-track arrays, no allocation.

    /// [Message thread] Select the row the track meter follows (`kInvalidTrackId` = none). Resets
    /// the accumulator so the next drain never carries the previous row's data.
    void setMeteredTrackForUi(TrackId trackId) noexcept;
    [[nodiscard]] TrackId getMeteredTrackForUi() const noexcept
    {
        return static_cast<TrackId>(meteredTrackId_.load(std::memory_order_relaxed));
    }
    /// [Message thread] Everything folded since the previous drain (peak hold across all blocks).
    /// These are the UI meters' windows (drained ~30 Hz by the Inspector channel panel).
    [[nodiscard]] level_meter::Reading drainMeteredTrackLevels() noexcept { return trackMeter_.drainAndReset(); }
    [[nodiscard]] level_meter::Reading drainMasterOutputLevels() noexcept { return masterMeter_.drainAndReset(); }
    /// [Message thread] Independent diagnostics windows fed from the SAME block statistics, so a
    /// stability scenario can measure over many seconds while the UI keeps draining its own copy.
    [[nodiscard]] level_meter::Reading drainMeteredTrackLevelsForDiagnostics() noexcept { return trackMeterDiag_.drainAndReset(); }
    [[nodiscard]] level_meter::Reading drainMasterOutputLevelsForDiagnostics() noexcept { return masterMeterDiag_.drainAndReset(); }

    // -----------------------------------------------------------------------
    // Concurrent per-row meters (mixer + Inspector through the UI's LevelMeterHub)
    // -----------------------------------------------------------------------
    // The same post-strip stage as the single track meter above, for MANY rows at once
    // (`engine/TrackMeterBank.h`). The message thread names the metered rows; the audio thread
    // folds each rendered row that has a slot. The Master row is not in the bank — its level is
    // `drainMasterOutputLevels()`. These windows belong to the UI hub only: the diagnostics
    // windows above are never drained by the mixer.

    /// [Message thread] Exactly these rows are metered concurrently from now on (rows that stay
    /// keep their pending window). Returns how many rows could not be metered (bank full).
    int setConcurrentlyMeteredTracks(const std::vector<TrackId>& trackIds) { return trackMeterBank_.setMeteredTracks(trackIds); }
    /// [Message thread] One row's window since the previous drain (empty when not metered).
    [[nodiscard]] level_meter::Reading drainConcurrentTrackLevels(const TrackId trackId) noexcept { return trackMeterBank_.drainAndReset(trackId); }
    [[nodiscard]] bool isConcurrentlyMetered(const TrackId trackId) const noexcept { return trackMeterBank_.isMetered(trackId); }

    /// [Any thread] Same acquire-load discipline as instrument snapshot reads inside the device callback.
    [[nodiscard]] std::shared_ptr<const ExperimentalInstrumentPlaybackSnapshot>
        loadExperimentalInstrumentPlaybackSnapshotForAudioThread() const noexcept;

    /// [Message thread] Stability C3: current published routing plan for invariant checks only.
    [[nodiscard]] std::shared_ptr<const RoutingPlan> loadRoutingPlanForDiagnostics() const noexcept
    {
        return routingPlan_.load(std::memory_order_acquire);
    }

    /// [Message thread] When true, the audio callback skips all experimental instrument host access
    /// (snapshot entries and coordinator map iteration via `experimentalBeginBlockAllHosts_`).
    void setInstrumentProcessingSuspended(bool suspended) noexcept;

    /// [Any thread] True while the audio callback is inside the instrument-host section for the current block.
    [[nodiscard]] bool isAudioInsideInstrumentSection() const noexcept;

    /// [Message thread] Rebuild `RoutingPlan` and bus scratch pool from the current session snapshot.
    void rebuildRoutingPlanFromSession() noexcept;

    // -----------------------------------------------------------------------
    // Live input monitoring (Monitor button) — runtime-only state, never persisted
    // -----------------------------------------------------------------------
    /// [Message thread] Toggle per-track input monitoring. Publishes a fresh immutable
    /// `LiveInputMonitorSnapshot` (atomic shared_ptr, same discipline as the routing plan).
    /// No-op beyond `LiveInputMonitorSnapshot::kMaxMonitoredTracks` concurrently monitored tracks.
    void setTrackInputMonitoringEnabled(TrackId trackId, bool enabled) noexcept;
    /// [Message thread] All monitoring off — called on project open/replace (Monitor defaults OFF).
    void clearAllInputMonitoring() noexcept;
    /// [Message thread] Current monitor state for UI (header button repaint).
    [[nodiscard]] bool isTrackInputMonitoringEnabled(TrackId trackId) const noexcept;

    // -----------------------------------------------------------------------
    // Solo — derived listening view (see `SoloMuteView`), runtime-only, never persisted
    // -----------------------------------------------------------------------
    /// [Message thread] Publish the derived Solo decision computed OFF the audio thread
    /// (`solo_mute_view::deriveSoloMuteView`). Null or `soloActive == false` restores plain
    /// stored-Mute behavior everywhere. Same atomic shared_ptr discipline as the monitor
    /// snapshot; the realtime callback and the offline mixdown gate acquire ONE view per block,
    /// so every strip in a block sees the same consistent picture (block-boundary publication).
    void publishSoloMuteView(std::shared_ptr<const SoloMuteView> view) noexcept;
    /// [Message thread] Currently published view (UI/diagnostics; may be null).
    [[nodiscard]] std::shared_ptr<const SoloMuteView> currentSoloMuteView() const noexcept;

    // -----------------------------------------------------------------------
    // Live MIDI input (keyboard → instrument hosts → take capture) — runtime-only
    // -----------------------------------------------------------------------
    /// [Message thread, before the device starts or with the callback drained] Install the live
    /// MIDI bus. Every realtime callback dispatches its pending device events into the hosts of
    /// the current instrument snapshot (after `beginAudioBlock`, before processing), with sample
    /// offsets derived from the device timestamps; the offline render never sees live input.
    /// `nullptr` detaches. The bus must outlive the engine's callback registration.
    void setLiveMidiInputBus(live_midi::LiveMidiInputBus* bus) noexcept;
    /// [Message thread] Timeline placement offset added to every captured live-MIDI event
    /// (normally −reported output latency; see `LiveMidiInputBus` time model). Relaxed atomic.
    void setLiveMidiRecordPlacementOffsetSamples(std::int64_t samples) noexcept
    {
        liveMidiRecordPlacementOffsetSamples_.store(samples, std::memory_order_relaxed);
    }

    // -----------------------------------------------------------------------------------------
    // Recording run — ONE capture boundary for audio and MIDI, acknowledged by the audio thread
    // -----------------------------------------------------------------------------------------
    // A recording run (audio take, MIDI take, or both) starts and stops on a BLOCK BOUNDARY that
    // the audio callback itself stamps: the monotone device sample, the transport position and
    // the cycle wrap serial of that block's first sample, all read in the same callback. The
    // audio recorder receives input only while the run is `Running`, the MIDI take is cut at the
    // same boundary (`LiveMidiTakeBuilder` discards everything on the mono clock at or after the
    // stop), and both finalizations read these acknowledged values instead of separate UI reads
    // — so audio and MIDI end the same run at the same instant. Latency compensation stays
    // separate: this is the RAW capture boundary; placement offsets are applied by the
    // coordinators afterwards. The message thread requests, the audio thread acknowledges;
    // `waitForRecordRunStop` bounds the wait so a stopped / lost device never blocks Stop.
    enum class RecordRunState : int
    {
        Idle = 0,
        StartRequested,
        Running,
        StopRequested,
        Stopped,
    };
    struct RecordRunBoundary
    {
        std::int64_t monoSample = 0;     ///< engine device clock at the boundary block's first sample
        std::int64_t timelineSample = 0; ///< transport position at that sample (raw, uncompensated)
        std::uint32_t wrapSerial = 0;    ///< transport cycle wrap count at that sample
        bool valid = false;
    };
    /// [Message thread] After the recorder / MIDI take are prepared and the Playing intent was
    /// requested: the next callback stamps the start boundary and begins capturing.
    void requestRecordRunStart() noexcept;
    /// [Message thread] After the Stopped intent was requested: the next callback stamps the stop
    /// boundary (that block captures nothing) and acknowledges.
    void requestRecordRunStop() noexcept;
    /// [Message thread] Poll until the audio thread acknowledged the stop, at most `timeoutMs`.
    /// On timeout (no callback arrives: device stopped / lost) the run is closed from the message
    /// thread with the given fallback boundary and `false` is returned.
    bool waitForRecordRunStop(int timeoutMs, const RecordRunBoundary& fallbackBoundary) noexcept;
    /// [Message thread] Back to Idle after the finalizations consumed the boundaries.
    void finishRecordRun() noexcept;
    [[nodiscard]] RecordRunState recordRunState() const noexcept
    {
        return static_cast<RecordRunState>(recordRunState_.load(std::memory_order_acquire));
    }
    /// [Any thread] Boundaries (valid once the matching acknowledgement happened).
    [[nodiscard]] RecordRunBoundary recordRunStartBoundary() const noexcept;
    [[nodiscard]] RecordRunBoundary recordRunStopBoundary() const noexcept;
    /// [Any thread] The engine's monotone device clock as of the last callback (diagnostics and
    /// the no-callback fallback boundary).
    [[nodiscard]] std::int64_t readMonoSampleClockForUi() const noexcept
    {
        return monoSampleClockPublished_.load(std::memory_order_relaxed);
    }

    /// [Message thread] Physical device input channels active at the last device start, as a bit
    /// mask (bit N = physical input N enabled). Matches the callback's packed input array:
    /// active-array position of physical channel N = popcount of lower set bits.
    [[nodiscard]] std::uint64_t getActiveInputPhysicalMaskForUi() const noexcept
    {
        return activeInputPhysicalMask_.load(std::memory_order_acquire);
    }

    /// [Message thread] One stereo offline block (`stereoOutputLR[0]` = L, `[1]` = R), matching realtime summing
    /// order for clips, inserts, instruments, mute/off/fader/pan. Does not advance transport.
    void renderOfflineMixdownBlock(const SessionSnapshot& sessionSnap,
                                   const ExperimentalInstrumentPlaybackSnapshot* instrumentSnap,
                                   std::int64_t timelineSegStartSample,
                                   int numSamples,
                                   float* const* stereoOutputLR,
                                   bool instrumentForceDiscontinuity);

private:
    void invokeExperimentalInstrumentBeginBlocks(const ExperimentalInstrumentPlaybackSnapshot* instrumentSnap,
                                                 int numSamples) noexcept;

    // -----------------------------------------------------------------------
    // Stage A1 — parallel audio-row strips (docs/PARALLEL_AUDIO_AND_READAHEAD_PLAN.md §3)
    // -----------------------------------------------------------------------
    // One render-pool job per eligible `RoutingPlan::SourceStep` Audio row renders the row's
    // complete strip — clip segments (both cycle-wrap segments), pre-gain, Pre inserts,
    // fader/mute, Post inserts, pan — into the row's OWN preallocated stage buffer. Jobs never
    // touch shared bus scratch; after the join the callback fans the stages to dry bus + sends
    // in `sourceSteps` order and the segments' disjoint frame ranges, reproducing today's
    // verified accumulation order bit-for-bit (plan §3.1). Audio strip jobs and instrument
    // generation jobs are dispatched as ONE batch (one barrier per block). Buses, monitoring,
    // instrument strips, proxies, offline export stay serial. The pool's serial path runs the
    // SAME job code on the callback lane (`--instrument-workers 0` = identical-DSP serial mode).

    /// One timeline segment of an audio strip job (≤ 2 per device block: cycle wrap).
    struct AudioStripSegmentDesc
    {
        std::int64_t timelineStartAudible = 0;
        int audibleRun = 0;
        /// Offset of this segment in the job's stage buffer AND in the destination bus buffers
        /// (`outFrame0 + silencePrefix`) — the segments' frame ranges are disjoint.
        int destFrame = 0;
        TrackId omitClipPlaybackForTrack = kInvalidTrackId;
        /// Value copy of this segment's transport context: the job writes it into its OWN
        /// chain playhead only (plan §3.2 #10 — no shared mutable transport context).
        PluginProcessTransportContext insertContext;
    };

    /// One audio row's strip job. Written by the callback during segment collection, read by
    /// exactly one claimant (worker lane or the callback lane), summed after the join. All
    /// referenced objects (snapshot, solo view, plan step, insert map entry) are retained by
    /// the callback for the whole block, which the jobs never outlive.
    struct AudioStripJobPayload
    {
        static constexpr int kMaxSegments = 4; ///< 2 occur today (cycle wrap); margin is defensive

        PlaybackEngine* engine = nullptr;
        const SessionSnapshot* sessionSnap = nullptr;
        const SoloMuteView* soloView = nullptr;
        /// Resolved insert chain (null = row has none). Points into the block's acquired
        /// `PluginAudioThreadMap`, which the callback retains until every job has joined.
        const PluginAudioThreadMap::Entry* chainEntry = nullptr;
        const RoutingPlan::SourceStep* step = nullptr; ///< kept alive by the callback's plan retain
        float* stageL = nullptr; ///< this job's OWN stage buffer (block capacity)
        float* stageR = nullptr;
        int trackIndex = -1;
        std::int64_t timelineEnd = 0;
        int numSegments = 0;
        std::array<AudioStripSegmentDesc, kMaxSegments> segments{};
    };

    /// Preallocated payload slots (grow-never, audio thread writes between dispatch and join
    /// only). More eligible rows than this = the whole block renders on the serial inline path
    /// (same job code on the callback lane) — rows are NEVER dropped or split across modes.
    static constexpr int kMaxAudioStripJobs = 192;

    // -----------------------------------------------------------------------
    // Stage A2 — instrument generation + instrument-row strip combined into ONE job
    // -----------------------------------------------------------------------
    // On combine-active blocks (transport playing, pool present, buffers prepared) each eligible
    // live-instrument row's job runs generation (`audioThread_renderGenerationStageForBlock`) and
    // then the row's strip — Pre inserts, fader/mute, Post inserts, pan — into the row's OWN
    // stage buffer, in the SAME batch as the Stage A1 audio strip jobs (still one dispatch/join
    // per device block). The callback consumes the finished stage in the unchanged MIX ORDER row
    // loop (meter fold + dry-bus/send fan) and must NOT run that row's strip again. Rows that
    // stay serial by design: proxy-selected hosts (never start Primary from a job), additional
    // rows sharing an already-claimed host (one generation per instance, strips fan serially),
    // audition hosts (exist only while stopped — combine never active), payload/job overflow.

    /// One instrument row's combined generation + strip job. Same lifetime rules as
    /// `AudioStripJobPayload`: everything referenced is retained by the callback past the join.
    struct InstrumentStripJobPayload
    {
        PlaybackEngine* engine = nullptr;
        const SessionSnapshot* sessionSnap = nullptr;
        const SoloMuteView* soloView = nullptr;
        /// Resolved insert chain (null = row has none). Points into the block's acquired map.
        const PluginAudioThreadMap::Entry* chainEntry = nullptr;
        ExperimentalInstrumentHost* host = nullptr;
        float* stageL = nullptr; ///< this job's OWN stage buffer (block capacity)
        float* stageR = nullptr;
        int trackIndex = -1;
        int numSamples = 0;
        /// Whole-block context at the callback's t0 — the same PositionInfo the serial path
        /// presents instrument chains via `setInsertProcessContext(t0)` (never segmented).
        PluginProcessTransportContext insertContext;
    };

    /// Preallocated combined-job slots. More eligible instrument rows than this = the overflow
    /// rows keep a generation-only job and their strip runs serially in the MIX ORDER loop
    /// (exactly today's path) — rows are NEVER dropped or double-processed.
    static constexpr int kMaxInstrumentStripJobs = 64;

    /// [Render-pool job entry] Runs `audioThread_runAudioStripPayload` on the claiming lane.
    static void runAudioStripRenderJob(instrument_render::RenderJob& job, int laneIndex) noexcept;
    /// [One job thread per payload, or the callback lane serially] Render the payload's clip
    /// segments + strip into its own stage buffer using the lane's exclusive chain scratch /
    /// MIDI scratch. No allocation, no locks, no shared-bus writes.
    void audioThread_runAudioStripPayload(AudioStripJobPayload& payload, int laneIndex) noexcept;
    /// [Render-pool job entry] Runs `audioThread_runInstrumentStripPayload` on the claiming lane.
    static void runInstrumentStripRenderJob(instrument_render::RenderJob& job, int laneIndex) noexcept;
    /// [One job thread per payload, or the callback lane serially] Generation stage for the
    /// payload's host, then the instrument strip core into the payload's own stage buffer using
    /// the lane's exclusive chain scratch / MIDI scratch. No allocation, no locks, no shared-bus
    /// writes, never a proxy-selected host (collection keeps those on the serial path).
    void audioThread_runInstrumentStripPayload(InstrumentStripJobPayload& payload, int laneIndex) noexcept;
    /// [Message thread, device stopped] Pre-size the per-job stage buffers, per-lane insert
    /// scratch and per-lane MIDI scratch for the device block size.
    void ensureAudioStripJobBuffersCapacity(int numSamples) noexcept;

    std::array<AudioStripJobPayload, kMaxAudioStripJobs> audioStripPayloads_{};
    int audioStripPayloadCount_ = 0;       ///< [audio thread] valid for the current block only
    bool audioStripCollectActive_ = false; ///< [audio thread] this block collects strip jobs
    /// Per-job stage buffers: channels 2k / 2k+1 belong to payload k.
    juce::AudioBuffer<float> audioStripStageBuffer_;
    int audioStripStageCapacity_ = 0;
    /// Stage A2 combined instrument jobs for the current block (same per-block lifetime rules).
    std::array<InstrumentStripJobPayload, kMaxInstrumentStripJobs> instrumentStripPayloads_{};
    int instrumentStripPayloadCount_ = 0; ///< [audio thread] valid for the current block only
    /// Per-job stage buffers: channels 2k / 2k+1 belong to combined instrument payload k.
    juce::AudioBuffer<float> instrumentStripStageBuffer_;
    int instrumentStripStageCapacity_ = 0;
    /// Per-LANE insert chain scratch + MIDI scratch (lane = render-pool worker index, last lane =
    /// callback). A job runs entirely on one lane, so these are exclusive while it runs.
    juce::AudioBuffer<float> insertLaneScratch_;
    float* insertLaneScratchPtrs_[instrument_render::InstrumentRenderPool::kNumLanes][2] = {};
    int insertLaneScratchCapacity_ = 0;
    std::array<juce::MidiBuffer, instrument_render::InstrumentRenderPool::kNumLanes> insertLaneMidi_;
    /// Last strip-job duration per track index (longest-first ordering key, same role as the
    /// instrument hosts' `audioThread_lastRenderTicksRelaxed`). Written by the job that owns the
    /// row, read by the callback before the next dispatch (ordered by the pool's join).
    std::array<std::int64_t, 1024> audioStripLastRenderTicks_{};

    /// Experimental read-ahead (docs/READAHEAD_PROTOTYPE.md); null unless explicitly enabled.
    std::unique_ptr<readahead::ReadAheadRenderer> readAhead_;
    /// State-capture drain wait bound (ms); test-shrinkable, same decision logic as production.
    std::atomic<int> stateCaptureDrainTimeoutMs_{ 500 };
    /// [Audio thread, block-stable after the read-ahead block begin] Owned rows excluded from the
    /// global transport-context refresh, and this block's consume keys — one per `renderRun`
    /// segment (a cycle-wrap block has two: up to the right locator, then from the left locator
    /// at its destination frame). Row-independent; recorded once per segment in the collect walk.
    std::array<TrackId, readahead::ReadAheadRenderer::kMaxRows> readAheadExcludedIds_{};
    int readAheadExcludedCount_ = 0;
    static constexpr int kReadAheadMaxSegmentsPerBlock = 2;
    std::int64_t readAheadSegStart_[kReadAheadMaxSegmentsPerBlock] = { 0, 0 };
    int readAheadSegRun_[kReadAheadMaxSegmentsPerBlock] = { 0, 0 };
    int readAheadSegDestFrame_[kReadAheadMaxSegmentsPerBlock] = { 0, 0 };
    int readAheadSegCount_ = 0;

    /// [Audio thread] Fold one finished callback into the diagnostic load window (relaxed atomics).
    void audioThread_accumulateCallbackLoad(int numSamples, std::int64_t startTicks) noexcept;

    /// [Message thread] Pre-size stereo master summing scratch (device block and offline cap).
    void ensureMasterScratchCapacity(int numSamples) noexcept;

    void ensureRoutingBusScratchPool(std::size_t numBuses, int numSamples) noexcept;

    void ensurePostStripStageScratchCapacity(int numSamples) noexcept;

    [[nodiscard]] int destBusIndexForTrackInPlan(const RoutingPlan& plan,
                                                 const SessionSnapshot& snap,
                                                 int trackIndex) const noexcept;

    Transport& transport_;
    Session& session_;
    RecorderService* const recorder_;
    CountInClickOutput* const countIn_;
    PluginInsertHost* const pluginHost_;
    /// Fixed worker pool + preallocated job descriptors for the generation stage (see the public
    /// section). The job array is written by the callback thread before each dispatch and never
    /// touched by workers outside a dispatch → join window.
    std::unique_ptr<instrument_render::InstrumentRenderPool> instrumentRenderPool_;
    std::array<instrument_render::RenderJob, instrument_render::InstrumentRenderPool::kMaxJobs> instrumentRenderJobs_{};
    std::atomic<bool> instrumentRenderSerialHint_{ false };
    /// [Audio thread] acquire-load retains const snapshot — same handoff discipline as Session.
    std::atomic<std::shared_ptr<const ExperimentalInstrumentPlaybackSnapshot>>
        experimentalInstrumentPlaybackSnapshot_;

    std::function<void(double, int)> experimentalPrepareAllHosts_;
    std::function<void()> experimentalReleaseAllHosts_;
    std::function<void(int)> experimentalBeginBlockAllHosts_;
    std::atomic<std::int64_t> playbackOffsetSamples_{ 0 };
    std::atomic<int> offlineRenderGateDepth_{ 0 };
    std::atomic<bool> instrumentProcessingSuspended_{ false };
    std::atomic<bool> audioInsideInstrumentSection_{ false };
    std::atomic<bool> audioCallbackInProcessingSection_{ false };
    /// Stability C2B diagnostics only (see AudioCallbackPhase). Relaxed stores on the audio thread.
    std::atomic<int> audioCallbackPhase_{ 0 };
    std::atomic<int> audioCallbackLastBlockSamples_{ 0 };
    /// Incremented at every callback entry; a frozen value across timeout logs = stuck callback,
    /// an advancing value = callbacks still cycling (flag observed true by unlucky sampling).
    std::atomic<std::uint64_t> audioCallbackEnterCount_{ 0 };
    /// Peak-hold of the device output (see `readAndResetOutputPeakHoldForDiagnostics`). Relaxed.
    std::atomic<float> outputPeakHold_{ 0.0f };
    /// [Audio thread] Fold this block's output peak into `outputPeakHold_` (lock-free max) and
    /// the Stereo Out meter accumulator (channels 0/1 of the device output).
    void audioThread_foldOutputPeak(const float* const* outputChannelData,
                                    int numOutputChannels,
                                    int numSamples) noexcept;

    /// Level meters (see the public section): the Stereo Out accumulator and the single metered
    /// track's accumulator. `meteredTrackId_` is compared on the audio thread per rendered stage.
    level_meter::Accumulator masterMeter_;
    level_meter::Accumulator trackMeter_;
    level_meter::Accumulator masterMeterDiag_;
    level_meter::Accumulator trackMeterDiag_;
    std::atomic<std::int64_t> meteredTrackId_{ static_cast<std::int64_t>(kInvalidTrackId) };
    /// Concurrent per-row meters for the mixer / Inspector hub (see the public section). The
    /// audio thread caches its slot map once per callback (`audioThread_beginBlock`).
    level_meter::TrackMeterBank trackMeterBank_;
    /// [Audio thread] Fold a track's post-strip stage into the single metered-row accumulators
    /// and / or its concurrent slot. The block statistics are computed at most once per stage.
    void audioThread_foldTrackMeterIfMetered(TrackId trackId,
                                             const float* stageL,
                                             const float* stageR,
                                             int numSamples) noexcept
    {
        const bool singleMetered = static_cast<std::int64_t>(trackId) == meteredTrackId_.load(std::memory_order_relaxed);
        const bool bankMetered = trackMeterBank_.audioThread_isMetered(trackId);
        if (!singleMetered && !bankMetered)
        {
            return;
        }
        const level_meter::BlockStats stats = level_meter::analyzeBlock(stageL, stageR, numSamples);
        if (singleMetered)
        {
            trackMeter_.audioThread_foldStats(stats);
            trackMeterDiag_.audioThread_foldStats(stats);
        }
        if (bankMetered)
        {
            trackMeterBank_.audioThread_foldStats(trackId, stats);
        }
    }

    /// Load window (see `AudioCallbackLoadSnapshot`). Relaxed only; never used for synchronization.
    std::atomic<std::uint64_t> loadWindowBlocks_{ 0 };
    std::atomic<double> loadWindowSumMs_{ 0.0 };
    std::atomic<double> loadWindowMinMs_{ 0.0 };
    std::atomic<double> loadWindowMaxMs_{ 0.0 };
    std::atomic<double> loadWindowSumBudgetPercent_{ 0.0 };
    std::atomic<double> loadWindowMaxBudgetPercent_{ 0.0 };
    std::atomic<std::uint32_t> loadWindowNearOverruns_{ 0 };
    std::atomic<std::uint32_t> loadWindowOverruns_{ 0 };
    std::atomic<double> deviceSampleRateForDiagnostics_{ 0.0 };

    PlaybackIntent lastTransportIntentInCallback_ = PlaybackIntent::Stopped;

    static constexpr int kOfflineMixdownBlockCapSamples = 4096;
    juce::AudioBuffer<float> masterScratch_;
    float* masterScratchPtrs_[2] = { nullptr, nullptr };
    int masterScratchCapacity_ = 0;

    struct RoutingBusScratchSlot
    {
        juce::AudioBuffer<float> buf;
        float* ptrs[2] = { nullptr, nullptr };
    };
    /// Stability C4B: slots are shared and immutable once created. The pool only grows, and a slot
    /// that needs a bigger buffer is *replaced* with a fresh slot (never `setSize` in place); every
    /// published `RoutingPlan` co-owns its slots via `busScratchOwners`, so the audio thread can
    /// keep addressing an older plan's buffers while the message thread rebuilds. Unused capacity
    /// is retained deliberately — freeing it while audio may run is exactly the ASan C4 bug.
    std::vector<std::shared_ptr<RoutingBusScratchSlot>> routingBusScratch_;
    juce::AudioBuffer<float> postStripStageScratch_;
    float* postStripStagePtrs_[2] = { nullptr, nullptr };
    int postStripStageCapacity_ = 0;
    std::atomic<std::shared_ptr<const RoutingPlan>> routingPlan_;
    /// [Audio thread only] Last-applied per-track pre-gain for click-free live adjustment;
    /// reset (unprimed) in `audioDeviceAboutToStart` so a saved pre-gain never fades in at
    /// playback start. Offline mixdown deliberately renders without it (constant target).
    playback_mix_helpers::PreGainRampState preGainRampState_;

    /// Physical→packed input mapping source of truth for the audio callback and recording push:
    /// bit N set = physical device input N is enabled (packed active-array position = popcount of
    /// lower set bits). Captured in `audioDeviceAboutToStart`; channels >= 64 are not addressable
    /// by input assignment (beyond every currently supported interface here).
    std::atomic<std::uint64_t> activeInputPhysicalMask_{ 0 };

    /// Monitor state (see `LiveInputMonitorSnapshot`): published by the message thread, acquire-
    /// loaded once per audio callback. Null = nothing monitored.
    std::atomic<std::shared_ptr<const playback_mix_helpers::LiveInputMonitorSnapshot>>
        liveInputMonitorSnapshot_;

    /// Derived Solo listening view (see `SoloMuteView`): published by the message thread,
    /// acquire-loaded once per audio callback / offline block. Null = solo inactive.
    std::atomic<std::shared_ptr<const SoloMuteView>> soloMuteView_;

    /// Live MIDI bus (non-owning; installed from Main). Relaxed pointer: installed before the
    /// device starts / with the callback drained, so the callback never races the store.
    std::atomic<live_midi::LiveMidiInputBus*> liveMidiBus_{ nullptr };
    std::atomic<std::int64_t> liveMidiRecordPlacementOffsetSamples_{ 0 };
    /// [Audio thread] Monotone device sample clock: advanced by every callback's block size on
    /// every path (gated, stopped, playing). The time base of the live-MIDI anchors, cycle
    /// wrap markers and the record-run boundaries; never reset while the device runs.
    std::int64_t monoSampleClock_ = 0;
    std::atomic<std::int64_t> monoSampleClockPublished_{ 0 };

    /// Record run (see the public section). State transitions: message thread Idle→StartRequested
    /// and Running→StopRequested; audio thread StartRequested→Running and StopRequested→Stopped
    /// (boundary fields written BEFORE the release-store of the state); message thread
    /// Stopped→Idle (and StopRequested→Stopped only on the no-callback timeout).
    std::atomic<int> recordRunState_{ 0 };
    std::atomic<std::int64_t> recordRunStartMono_{ 0 };
    std::atomic<std::int64_t> recordRunStartTimeline_{ 0 };
    std::atomic<std::uint32_t> recordRunStartWrap_{ 0 };
    std::atomic<std::int64_t> recordRunStopMono_{ 0 };
    std::atomic<std::int64_t> recordRunStopTimeline_{ 0 };
    std::atomic<std::uint32_t> recordRunStopWrap_{ 0 };
    /// True from the audio thread's start acknowledgement until the run is finished: tells the
    /// message thread (no-callback fallback) whether the start boundary holds THIS run's values.
    std::atomic<bool> recordRunStartAcked_{ false };
    /// [Audio thread] Acknowledge a pending start / stop at this block's first sample; returns
    /// true while the run captures (input is pushed to the recorder, transport runs past the end).
    bool audioThread_updateRecordRun(std::int64_t monoSampleAtBlockStart, std::int64_t playheadAtBlockStart) noexcept;
    /// [Audio thread] Adapter from the bus's delivery seam onto the host's per-block MIDI buffer.
    static void audioThread_deliverLiveMidiToHost(void* context, ExperimentalInstrumentHost* host,
                                                  int sampleOffset, const juce::MidiMessage& message) noexcept;
};
