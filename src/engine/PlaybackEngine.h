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
};

struct ExperimentalInstrumentPlaybackSnapshot
{
    std::vector<ExperimentalInstrumentPlaybackEntry> entries;

    ExperimentalInstrumentPlaybackSnapshot() = default;
    explicit ExperimentalInstrumentPlaybackSnapshot(std::vector<ExperimentalInstrumentPlaybackEntry>&& e)
        : entries(std::move(e))
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
};
