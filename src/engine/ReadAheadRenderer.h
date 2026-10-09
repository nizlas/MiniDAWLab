#pragma once

// =============================================================================
// ReadAheadRenderer.h / ReadAheadRenderer.cpp — experimental audio-row read-ahead (Stage B prototype)
// =============================================================================
//
// Model: docs/READAHEAD_PROTOTYPE.md. OFF by default; exists only when the process was started
// with `--experimental-readahead[=N]` (or a test created it in pump mode). One dedicated
// low-priority worker thread — separate from the InstrumentRenderPool that must meet the current
// audio deadline — renders FUTURE blocks of adopted audio rows through the production strip core
// into per-row SPSC rings. The audio callback consumes ready blocks; a not-ready block is a MISS
// (silence that block, counted, never a wait).
//
// INVARIANT (the whole point): exactly-once, contiguous sample stream per insert instance.
//   * adoption is gapless by construction (the worker's first block is exactly the block after
//     the row's last live-rendered block, started only after the published live progress proves
//     the callback is done with the chain);
//   * draining release is gapless (worker stops producing, callback consumes the queue dry);
//   * a discard reset (seek / stop / wrap / offset change) discards OUTPUT only — the instance
//     state is ahead by <= depth blocks and that residue rides the transport discontinuity
//     (documented in the model doc, never silently "rewound").
//
// THREADING
//   * All ownership transitions happen on the AUDIO CALLBACK thread in `audioThread_beginBlock`
//     (plus `audioThread_offerAdoption`), so the owned set is block-stable.
//   * The worker claims one row at a time via a Dekker-style busy/stop pair; it never holds a
//     claim across blocks and acquires session snapshot / solo view / insert map FRESH per block.
//   * Message-thread `pauseWorkerAndWait` / `resumeWorker` bracket windows that need exclusive
//     chain access off the callback (plugin retire destroy, offline export, device stop).
//   * No allocation and no locks on the audio thread; the worker allocates nothing after
//     `prepareForDevice` (shared_ptr refcounts aside, same discipline as the callback).
// =============================================================================

#include <juce_audio_basics/juce_audio_basics.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <limits>
#include <memory>
#include <thread>

#include "domain/Track.h"
#include "engine/PlaybackMixHelpers.h"
#include "engine/SoloMuteView.h"
#include "plugins/PluginInsertHost.h"

class Session;

namespace readahead
{

/// Process-wide CLI configuration (`--experimental-readahead[=N]`); 0 = disabled (default).
/// Message thread, before the engine is constructed (same pattern as the render-pool override).
void setConfiguredReadAheadDepth(int depthBlocksOrZero) noexcept;
[[nodiscard]] int configuredReadAheadDepth() noexcept;

class ReadAheadRenderer
{
public:
    static constexpr int kMaxRows = 16;  ///< adopted rows; further eligible rows stay live (A1)
    static constexpr int kMinDepth = 2;
    static constexpr int kMaxDepth = 8;

    /// Everything the worker needs, owned by others and outliving the renderer.
    struct Deps
    {
        Session* session = nullptr;
        PluginInsertHost* pluginHost = nullptr;
        playback_mix_helpers::PreGainRampState* preGainRamp = nullptr;
        /// The engine's solo-view publication point (worker acquire-loads per block).
        std::atomic<std::shared_ptr<const SoloMuteView>>* soloViewAtomic = nullptr;
    };

    /// [Message thread] `spawnWorkerThread == false` = deterministic PUMP mode for tests: no
    /// thread is created and the test drives the worker with `testPumpWorkerOnce()`.
    ReadAheadRenderer(const Deps& deps, int depthBlocks, bool spawnWorkerThread);
    ~ReadAheadRenderer();

    ReadAheadRenderer(const ReadAheadRenderer&) = delete;
    ReadAheadRenderer& operator=(const ReadAheadRenderer&) = delete;

    // ---------------------------------------------------------------------
    // Message-thread lifecycle
    // ---------------------------------------------------------------------
    /// [Message thread, no callback running] Allocate per-row slot buffers + worker scratch for
    /// the device block size and resume the worker.
    void prepareForDevice(double sampleRate, int blockSizeSamples);
    /// [Message thread, no callback running] Pause the worker and hard-reset all ownership.
    void releaseForDevice() noexcept;
    /// [Message thread] Bounded pause: returns once the worker acknowledged (it holds no chain,
    /// no map and no snapshot while paused). Used around plugin-retire destroys and offline
    /// export. In pump mode this only sets the flag (the pump and the caller share a thread).
    void pauseWorkerAndWait() noexcept;
    void resumeWorker() noexcept;
    /// [Any thread] The next `audioThread_beginBlock` performs a full discard reset (used after
    /// retire/offline windows; the callback thread itself performs the transitions).
    void requestFullReset() noexcept;

    // ---------------------------------------------------------------------
    // Audio-callback API (block-stable ownership)
    // ---------------------------------------------------------------------
    struct BlockBeginInfo
    {
        std::int64_t t0 = 0;
        int numSamples = 0;
        bool playing = false;
        bool cycleActive = false;  ///< cycle on AND valid locators
        bool recording = false;    ///< recorder exists and is recording / capturing
        std::int64_t playbackShift = 0;
        std::int64_t arrangementEnd = 0;
        /// This block's monitored-track view (adopted rows must drain when monitored).
        const playback_mix_helpers::LiveInputMonitorSnapshot* monitorView = nullptr;
    };
    /// [Audio thread, once per callback, BEFORE the A1 pre-count] Performs every ownership
    /// transition for this block: continuity check (discard reset on t0 / shift discontinuity,
    /// stop, wrap), drain triggers (cycle on, recording, monitor, near-end, structure change),
    /// drain completions (ring empty + worker ack -> Live), Scheduled -> Ahead commits.
    void audioThread_beginBlock(const BlockBeginInfo& info) noexcept;

    /// [Audio thread, after beginBlock] Offer adoption of an eligible row (the ENGINE applies
    /// the A1 eligibility gates). The row renders live THIS block (Scheduled) and is consumed
    /// from the ring from the next block on. Ignored when full / already owned / gated off.
    void audioThread_offerAdoption(TrackId trackId, int trackIndex) noexcept;

    /// [Audio thread] True = the engine must NOT render this row live this block (collect /
    /// serial strip / monitor pass all skip it; its audio comes from `audioThread_tryConsume`).
    [[nodiscard]] bool audioThread_isOwnedForRender(TrackId trackId) const noexcept;
    /// [Audio thread] Rows whose chain playheads the global transport-context setter must skip
    /// this block (every non-Live row, Scheduled included — the worker may start mid-callback).
    /// Returns the count written to `out` (capacity `kMaxRows`).
    int audioThread_exportExcludedTrackIds(TrackId* out) const noexcept;
    [[nodiscard]] bool audioThread_anyOwned() const noexcept { return anyNonLive_.load(std::memory_order_relaxed); }

    struct ConsumeView
    {
        /// Non-const only because the fan helpers take mutable stage pointers; consumers must
        /// not write (the slot is reused by the worker after `audioThread_releaseConsumed`).
        float* stageL = nullptr;
        float* stageR = nullptr;
    };
    /// [Audio thread] Consume the block `(timelineStartAudible, run)` of an owned row. HIT: view
    /// valid until `audioThread_releaseConsumed`. MISS (not ready / key mismatch; stale heads
    /// are discarded and counted): returns false — the row is silent this block.
    [[nodiscard]] bool audioThread_tryConsume(TrackId trackId, std::int64_t timelineStartAudible,
                                              int run, ConsumeView& out) noexcept;
    void audioThread_releaseConsumed(TrackId trackId) noexcept;
    /// [Audio thread] Count a miss for an owned row the engine could not even attempt to consume
    /// (pathological segmenting — e.g. a wrap block reached while still owned).
    void audioThread_noteMiss(TrackId trackId) noexcept;

    /// [Audio thread, right after the block's job JOIN] Publishes how far the live stream has
    /// rendered; a Scheduled row's worker may only start once this reaches its boundary (the
    /// callback's last touch of that chain is the join).
    void audioThread_publishLiveProgress(std::int64_t liveStreamEnd) noexcept;
    /// [Audio thread, once per callback] Block context template; the worker stamps
    /// `timelineSample` per rendered block (all other fields are block-constant).
    void audioThread_publishContextTemplate(const PluginProcessTransportContext& context) noexcept;

    // ---------------------------------------------------------------------
    // Diagnostics / tests
    // ---------------------------------------------------------------------
    struct Counters
    {
        std::int64_t adopted = 0;         ///< Live -> Scheduled commits
        std::int64_t producedBlocks = 0;  ///< worker-rendered blocks
        std::int64_t consumedBlocks = 0;  ///< ring hits played
        std::int64_t missedBlocks = 0;    ///< owned row had no ready matching block (silence)
        std::int64_t staleDiscarded = 0;  ///< produced blocks discarded unplayed
        std::int64_t drainReleases = 0;   ///< gapless Draining -> Live completions
        std::int64_t discardResets = 0;   ///< discontinuity resets (rows x events)
    };
    [[nodiscard]] Counters countersSnapshot() const noexcept;
    [[nodiscard]] int depthBlocks() const noexcept { return depth_; }
    /// [Test, pump mode only] One worker scan pass (renders at most one block per owned row).
    /// Returns the number of blocks rendered.
    int testPumpWorkerOnce() noexcept;

private:
    enum class RowState : int
    {
        Live = 0,
        Scheduled,   ///< renders live this block; worker starts at `boundary`
        Ahead,       ///< worker produces, callback consumes
        Draining,    ///< worker stopping/stopped; callback consumes the queue dry
        Abandoning,  ///< discard reset hit a busy worker; silent until acked, then Live
    };

    struct Slot
    {
        std::int64_t start = 0;
        int run = 0;
        std::uint32_t generation = 0;
        float* dataL = nullptr;  ///< into slotBuffer_, block capacity
        float* dataR = nullptr;
    };

    struct Row
    {
        std::atomic<int> state{ 0 };  ///< RowState, callback-thread writer only
        TrackId trackId = kInvalidTrackId;
        int trackIndex = -1;          ///< captured at adoption; worker verifies id match per block
        std::int64_t boundary = 0;    ///< first worker block (== first consumed block)
        std::uint32_t generation = 0; ///< bumped at adoption and at every reset
        std::int64_t nextProduceT0 = 0;  ///< worker-private once started
        bool workerStarted = false;      ///< worker-private (initialized under the adoption publish)
        // SPSC ring: worker writes slot fields then release-increments `tail`; the callback
        // consumes at `head` and release-increments it. Slot index = sequence % depth.
        std::array<Slot, kMaxDepth> slots{};
        std::atomic<std::uint32_t> head{ 0 };
        std::atomic<std::uint32_t> tail{ 0 };
        // Dekker pair (model doc §3): the worker claims `busy` BEFORE re-checking `stopProduce`;
        // the callback sets `stopProduce` BEFORE reading `busy`. `stopProduce` stays set until
        // the NEXT adoption re-arms the row (so a worker racing a reset always aborts).
        std::atomic<int> busy{ 0 };
        std::atomic<bool> stopProduce{ false };
        std::atomic<bool> workerAckedStop{ false };
        std::atomic<bool> workerActive{ false };  ///< worker-side view: row has work
        std::atomic<bool> workerSelfStopped{ false };  ///< row vanished from worker's snapshot
    };

    // Worker body (worker thread, or the test pump on the message thread).
    int workerScanOnce() noexcept;
    bool workerRenderOneBlock(Row& row) noexcept;
    void workerThreadMain() noexcept;

    // Callback-thread helpers.
    void audioThread_discardResetRow(Row& row) noexcept;
    void audioThread_purgeRing(Row& row) noexcept;
    void audioThread_refreshOwnedFlags() noexcept;
    [[nodiscard]] Row* findRowByTrackId(TrackId trackId) noexcept;
    [[nodiscard]] const Row* findRowByTrackId(TrackId trackId) const noexcept;

    Deps deps_{};
    const int depth_;
    const bool pumpMode_;

    std::array<Row, kMaxRows> rows_{};
    /// Slot sample storage: row r, slot s => channels at ((r * depth) + s) * 2 and +1.
    juce::AudioBuffer<float> slotBuffer_;
    int slotCapacitySamples_ = 0;
    int blockSizeSamples_ = 0;

    // Worker-exclusive strip scratch (the chain processes in here) + MIDI scratch, lane 16.
    juce::AudioBuffer<float> workerScratch_;
    float* workerScratchPtrs_[2] = { nullptr, nullptr };
    int workerScratchCapacity_ = 0;
    juce::MidiBuffer workerMidiScratch_;

    // Block-context template double buffer (callback publishes, worker reads).
    PluginProcessTransportContext contextTemplates_[2]{};
    std::atomic<int> contextTemplateIndex_{ 0 };

    std::atomic<std::int64_t> liveProgress_{ std::numeric_limits<std::int64_t>::min() };
    std::atomic<bool> fullResetRequested_{ false };
    std::atomic<bool> prepared_{ false };

    // Callback-private continuity + block state (audio thread only, block-stable after beginBlock).
    std::int64_t expectedT0_ = std::numeric_limits<std::int64_t>::min();
    std::int64_t lastPlaybackShift_ = 0;
    bool haveExpectedT0_ = false;
    bool adoptionAllowedThisBlock_ = false;
    std::int64_t blockT0_ = 0;
    int blockNumSamples_ = 0;
    std::int64_t blockArrangementEnd_ = 0;
    std::int64_t blockPlaybackShift_ = 0;

    std::atomic<bool> anyNonLive_{ false };

    // Worker thread control.
    std::thread workerThread_;
    std::atomic<bool> workerShouldExit_{ false };
    std::atomic<bool> pauseRequested_{ false };
    std::atomic<bool> pauseAcked_{ false };

    // Counters (relaxed; diagnostics only).
    std::atomic<std::int64_t> cAdopted_{ 0 };
    std::atomic<std::int64_t> cProduced_{ 0 };
    std::atomic<std::int64_t> cConsumed_{ 0 };
    std::atomic<std::int64_t> cMissed_{ 0 };
    std::atomic<std::int64_t> cStale_{ 0 };
    std::atomic<std::int64_t> cDrainReleases_{ 0 };
    std::atomic<std::int64_t> cDiscardResets_{ 0 };
};

} // namespace readahead
