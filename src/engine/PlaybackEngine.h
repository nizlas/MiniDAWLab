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

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "domain/Track.h"
#include "engine/LevelMeterAccumulator.h"
#include "engine/LiveMidiInputBus.h"
#include "engine/PlaybackMixHelpers.h"
#include "engine/RoutingPlan.h"
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
    /// [Audio thread] Fold a track's post-strip stage when it is the metered row (no-op otherwise).
    void audioThread_foldTrackMeterIfMetered(TrackId trackId,
                                             const float* stageL,
                                             const float* stageR,
                                             int numSamples) noexcept
    {
        if (static_cast<std::int64_t>(trackId) == meteredTrackId_.load(std::memory_order_relaxed))
        {
            const level_meter::BlockStats stats = level_meter::analyzeBlock(stageL, stageR, numSamples);
            trackMeter_.audioThread_foldStats(stats);
            trackMeterDiag_.audioThread_foldStats(stats);
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

    /// Live MIDI bus (non-owning; installed from Main). Relaxed pointer: installed before the
    /// device starts / with the callback drained, so the callback never races the store.
    std::atomic<live_midi::LiveMidiInputBus*> liveMidiBus_{ nullptr };
    std::atomic<std::int64_t> liveMidiRecordPlacementOffsetSamples_{ 0 };
    /// [Audio thread] Monotone device sample clock: advanced by every callback's block size on
    /// every path (gated, stopped, playing). The time base of the live-MIDI anchors and cycle
    /// wrap markers (`LiveMidiInputBus` time model); never reset while the device runs.
    std::int64_t monoSampleClock_ = 0;
    /// [Audio thread] Adapter from the bus's delivery seam onto the host's per-block MIDI buffer.
    static void audioThread_deliverLiveMidiToHost(void* context, ExperimentalInstrumentHost* host,
                                                  int sampleOffset, const juce::MidiMessage& message) noexcept;
};
