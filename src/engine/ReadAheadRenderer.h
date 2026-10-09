#pragma once

// =============================================================================
// ReadAheadRenderer.h / ReadAheadRenderer.cpp — experimental audio-row read-ahead (Stage B, model v2)
// =============================================================================
//
// Model: docs/READAHEAD_PROTOTYPE.md. OFF by default; exists only when the process was started
// with `--experimental-readahead[=N]` (or a test created it in pump mode). One dedicated worker
// thread (default priority — deliberately NOT the pool's pro-audio priority) — separate from the
// InstrumentRenderPool that must meet the current audio deadline — renders FUTURE segments of
// adopted audio rows through the production strip core into per-row SPSC rings. The audio
// callback consumes ready segments by exact key. A row is not switched onto that consume path
// until the worker has queued the next block's segment; if the worker has not entered the
// chain, the prime is declined and the callback keeps rendering live (no miss, no silence).
// Once the row is ahead, a not-ready segment is a MISS (silence that segment, counted, never
// a wait). Two consecutive missed segments abandon the row back to the live path (with a
// re-adoption cooldown) — the mode is left when it does not hold.
//
// INVARIANT (the whole point): exactly-once, contiguous AUDIBLE SEGMENT STREAM per insert
// instance while owned. The engine's segmentation (linear blocks, cycle-wrap splits, short
// blocks at loop/arrangement limits) is a deterministic function of block-stable inputs; the
// worker replicates it exactly (`stepSegment`), so producer and consumer walk the same sequence:
//   * adoption stays on the direct path until that next segment is actually queued. The
//     worker's first segment is still the block after the row's last live-rendered block,
//     gated on a MONOTONE callback block serial (wrap-safe). An uncommitted prime (worker
//     has not entered the chain, ring empty) is declined: this block renders live, and it
//     is not a miss. A prime whose worker is already inside the plugin stays exclusive —
//     the callback does not render that instance and does not wait; if the segment is still
//     absent at consume, that one segment is a counted miss;
//   * pause/resume at the same position CONTINUES the queue (no discard, no re-feed);
//   * draining release is gapless (worker stops producing, callback consumes the queue dry);
//   * monitor/record handover and deliberate transport jumps (seek, stop-button, cycle/locator
//     geometry edits, playback-offset changes) discard OUTPUT only — the bounded state lead
//     rides the discontinuity (documented in the model doc, never silently "rewound");
//   * loop iterations are distinguished by per-adoption slot sequence numbers, never by
//     position comparison (positions are non-monotone across wraps).
//
// DOMAIN RULE: run-length arithmetic replicates the engine's transport-domain formulas; queue
// keys and rendered content use the AUDIBLE timeline domain (transport + playback offset).
//
// THREADING
//   * All ownership transitions happen on the AUDIO CALLBACK thread in `audioThread_beginBlock`
//     (plus `audioThread_offerAdoption`), so the owned set is block-stable.
//   * The worker claims one row at a time via a Dekker-style busy/stop pair; it never holds a
//     claim across segments and acquires session snapshot / solo view / insert map FRESH per
//     segment.
//   * Message-thread `pauseWorkerAndWait` / `resumeWorker` bracket windows that need the worker
//     provably outside every chain (plugin retire destroy, offline export, device stop, the
//     plugin-state capture window). The pause is DEPTH-COUNTED with an epoch handshake: only a
//     REAL acknowledgment (the worker observed the request while outside every chain, after the
//     request was made) returns success; a timeout returns false and confers NO exclusivity —
//     callers must then abort, defer, or keep waiting, never proceed. An aborted caller's
//     release cannot resume the worker while another caller still holds a pause.
//     `beginStateCaptureHold`/`endStateCaptureHold` additionally gate adoption off and
//     gaplessly drain owned rows while playback consumption runs.
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
    /// Two consecutive missed segments on a row => the mode does not hold for it: the row is
    /// abandoned back to the live path (model doc §7) and sits out the cooldown below.
    static constexpr int kConsecutiveMissAbandonThreshold = 2;
    static constexpr int kMissReAdoptionCooldownBlocks = 64;

    /// Everything the worker needs, owned by others and outliving the renderer.
    struct Deps
    {
        Session* session = nullptr;
        PluginInsertHost* pluginHost = nullptr;
        playback_mix_helpers::PreGainRampState* preGainRamp = nullptr;
        /// The engine's solo-view publication point (worker acquire-loads per segment).
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
    /// the device block size and resume the worker. Requires a real worker acknowledgment; if
    /// none arrives (worker stuck inside a plugin render from an earlier device session) the
    /// buffers are left UNTOUCHED and the renderer stays dormant (`prepared_ == false`: the
    /// callback never engages it) — resources are never resized/freed under a live render.
    void prepareForDevice(double sampleRate, int blockSizeSamples);
    /// [Message thread, no callback running] Acknowledged pause + hard ownership reset, then the
    /// renderer stays parked until the next `prepareForDevice`. The host releases plugin
    /// resources right after this returns, so it WAITS (bounded attempts, unbounded total, with
    /// diagnostics) until the worker really acknowledged: a plugin stuck inside the worker's
    /// render stalls device stop with resources retained — it never proceeds into
    /// `releaseResources` while the worker may still be inside a processBlock.
    void releaseForDevice() noexcept;
    /// [Message thread] Bounded pause request. Returns TRUE only on a real acknowledgment: the
    /// worker observed the request while outside every chain/map/snapshot, and stays parked
    /// until the matching `resumeWorker`. Returns FALSE on timeout — the request is withdrawn
    /// (balanced) and the caller gained NO exclusivity: it must abort, defer or retry; it must
    /// not touch or free worker-reachable resources. Pauses nest (depth-counted): an aborted or
    /// finished caller can never resume the worker under another caller still holding a pause.
    /// In pump mode there is no worker thread and the pause succeeds trivially (shared thread).
    [[nodiscard]] bool pauseWorkerAndWait() noexcept;
    void resumeWorker() noexcept;
    /// [Test only] Shrink/restore the acknowledgment timeout so timeout paths are testable with
    /// a deliberately blocked worker. Same decision logic as production, smaller bound.
    void setPauseAckTimeoutMsForTests(int timeoutMs) noexcept
    {
        pauseAckTimeoutMs_.store(juce::jmax(1, timeoutMs), std::memory_order_relaxed);
    }
    /// [Test only] While set, the worker does not claim a row it has not started. The callback
    /// therefore sees an uncommitted prime and keeps the row on the direct path. Production
    /// never sets this.
    void setDeferPrimeForTests(bool defer) noexcept
    {
        deferPrimeForTests_.store(defer ? 1 : 0, std::memory_order_release);
    }
    /// [Test only] Arms a rendezvous at the start of the decline decision, which is after the
    /// caller has already observed an empty ring. Gate: 1 armed, 2 callback parked in the
    /// decision, 3 the published segment is idle and the decision may continue (the worker
    /// stays outside the chain until the decision stores 0). Production never arms this, so
    /// the callback never waits.
    void armDeclineRaceForTests() noexcept
    {
        declineRace_.store(1, std::memory_order_release);
    }
    [[nodiscard]] int declineRaceStateForTests() const noexcept
    {
        return declineRace_.load(std::memory_order_acquire);
    }
    /// [Test only] Let the parked decline continue. The worker keeps holding the next claim
    /// until the decision itself clears the gate.
    void letDeclineDecisionContinueForTests() noexcept
    {
        declineRace_.store(3, std::memory_order_release);
    }
    void releaseDeclineRaceForTests() noexcept
    {
        declineRace_.store(0, std::memory_order_release);
    }
    /// [Test only] True when `trackId` has a published segment and holds neither `busy` nor
    /// `inPlugin`.
    [[nodiscard]] bool testRowPublishedAndUnclaimed(TrackId trackId) const noexcept;
    /// [Any thread] The next `audioThread_beginBlock` performs a full discard reset (used after
    /// offline-export windows; the callback thread itself performs the transitions).
    void requestFullReset() noexcept;

    /// [Message thread] Plugin-state capture window (model doc §9): while held, adoption is
    /// gated off and every owned row takes a gapless draining release as playback consumption
    /// proceeds (nothing is discarded; nothing audible changes). The caller waits on
    /// `audioThread_anyOwned()` (atomic, safe to poll off-thread) with its own bound, then
    /// brackets the capture with `pauseWorkerAndWait`/`resumeWorker`.
    void beginStateCaptureHold() noexcept;
    void endStateCaptureHold() noexcept;

    // ---------------------------------------------------------------------
    // Audio-callback API (block-stable ownership)
    // ---------------------------------------------------------------------
    struct BlockBeginInfo
    {
        std::int64_t t0 = 0;  ///< transport playhead at block start (post seek-apply)
        int numSamples = 0;
        bool playing = false;
        bool cycleActive = false;      ///< cycle on AND valid locators (engine's `validCycle`)
        std::int64_t locLeft = 0;      ///< transport-domain locators (meaningful when cycleActive)
        std::int64_t locRight = 0;
        TrackId recordingTrackId = kInvalidTrackId;  ///< active capture row (immediate handover)
        TrackId armedTrackId = kInvalidTrackId;      ///< record-armed row (pre-emptive drain)
        std::int64_t playbackShift = 0;
        std::int64_t arrangementEnd = 0;
        /// False = no usable routing plan this block: the engine's plan-less fallback paths have
        /// no owned-row skip, so ownership must discard-reset BEFORE any rendering decision.
        bool planUsable = false;
        /// This block's monitored-track view (owned monitored rows hand over immediately).
        const playback_mix_helpers::LiveInputMonitorSnapshot* monitorView = nullptr;
    };
    /// [Audio thread, once per callback, BEFORE the A1 pre-count] Performs every ownership
    /// transition for this block: geometry/offset/position discontinuities (discard reset),
    /// miss abandonment, consume-absence release, monitor/record handover (discard), armed /
    /// near-end / capture-hold / self-stop drains, drain completions. A Scheduled row commits
    /// to Ahead only when its first segment is already queued; otherwise an uncommitted prime
    /// returns to Live (direct path, no miss) and a prime the worker has already entered stays
    /// exclusive for this block. Pause (`playing == false` at an unchanged position) is NOT a
    /// discontinuity.
    void audioThread_beginBlock(const BlockBeginInfo& info) noexcept;

    /// [Audio thread, after beginBlock] Offer adoption of an eligible row (the ENGINE applies
    /// the A1 eligibility gates; the renderer applies the model gates: headroom, non-negative
    /// audible positions, armed/recording exclusion, cooldown, capacity). The row renders live
    /// THIS block (Scheduled). The next block consumes from the ring only if that segment is
    /// queued; otherwise the prime is declined or, if the worker has entered, held exclusive.
    void audioThread_offerAdoption(TrackId trackId, int trackIndex) noexcept;

    /// [Audio thread] True = the engine must NOT render this row live this block (collect /
    /// serial strip / monitor pass all skip it; its audio comes from `audioThread_tryConsume`).
    [[nodiscard]] bool audioThread_isOwnedForRender(TrackId trackId) const noexcept;
    /// [Audio thread] Rows whose chain playheads the global transport-context setter must skip
    /// this block (every non-Live row, Scheduled included — the worker may start mid-callback).
    /// Returns the count written to `out` (capacity `kMaxRows`).
    int audioThread_exportExcludedTrackIds(TrackId* out) const noexcept;
    /// Atomic owned-flag; also safe to poll from the message thread (state-capture window).
    [[nodiscard]] bool audioThread_anyOwned() const noexcept { return anyNonLive_.load(std::memory_order_acquire); }

    struct ConsumeView
    {
        /// Non-const only because the fan helpers take mutable stage pointers; consumers must
        /// not write (the slot is reused by the worker after `audioThread_releaseConsumed`).
        /// Segment data lives at [destFrame, destFrame + run) — same layout as an A1 stage.
        float* stageL = nullptr;
        float* stageR = nullptr;
    };
    /// [Audio thread] Consume the segment `(timelineStartAudible, run, destFrame)` of an owned
    /// row. HIT: view valid until `audioThread_releaseConsumed`. MISS (not ready / key mismatch;
    /// stale heads are discarded by sequence number and counted): returns false — the row is
    /// silent for this segment, the expected sequence advances (a late result becomes stale),
    /// and consecutive misses are tracked for abandonment.
    [[nodiscard]] bool audioThread_tryConsume(TrackId trackId, std::int64_t timelineStartAudible,
                                              int run, int destFrame, ConsumeView& out) noexcept;
    void audioThread_releaseConsumed(TrackId trackId) noexcept;
    /// [Audio thread] Count a miss for an owned row's segment the engine could not even attempt
    /// to consume (defensive; advances the expected sequence like a failed consume).
    void audioThread_noteMiss(TrackId trackId) noexcept;

    /// [Audio thread, right after the block's job JOIN] Marks this callback block's live
    /// rendering as finished; a Scheduled row's worker may only start once the serial of its
    /// adoption block has been published (the callback's last touch of that chain is the join).
    /// Monotone — safe across cycle wraps.
    void audioThread_publishJoinedBlock() noexcept;
    /// [Audio thread, once per callback] Block context template; the worker stamps
    /// `timelineSample` per rendered segment and reads the loop geometry from it.
    void audioThread_publishContextTemplate(const PluginProcessTransportContext& context) noexcept;

    // ---------------------------------------------------------------------
    // Diagnostics / tests (internal only — never surfaced in UI)
    // ---------------------------------------------------------------------
    struct Counters
    {
        std::int64_t adopted = 0;           ///< Live -> Scheduled commits
        std::int64_t producedSegments = 0;  ///< worker-rendered segments
        std::int64_t consumedSegments = 0;  ///< ring hits played
        std::int64_t missedSegments = 0;    ///< owned row had no ready matching segment (silence)
        std::int64_t staleDiscarded = 0;    ///< produced segments discarded unplayed
        std::int64_t drainReleases = 0;     ///< gapless Draining -> Live completions
        std::int64_t discardResets = 0;     ///< discontinuity/handover resets (rows x events)
        std::int64_t missAbandons = 0;      ///< rows that left the mode on consecutive misses
    };
    [[nodiscard]] Counters countersSnapshot() const noexcept;
    [[nodiscard]] int depthBlocks() const noexcept { return depth_; }
    /// [Test, pump mode only] One worker scan pass (renders at most one segment per owned row).
    /// Returns the number of segments rendered.
    int testPumpWorkerOnce() noexcept;

private:
    enum class RowState : int
    {
        Live = 0,
        Scheduled,   ///< renders live this block; worker starts at the next block
        Ahead,       ///< worker produces, callback consumes
        Draining,    ///< worker stopping/stopped; callback consumes the queue dry
        Abandoning,  ///< discard reset hit a busy worker; silent until acked, then Live
    };

    struct Slot
    {
        std::int64_t start = 0;  ///< audible-domain timeline start
        int run = 0;
        int destFrame = 0;       ///< offset within the device block (cycle-wrap second segment)
        std::uint32_t seq = 0;   ///< per-adoption monotone segment number (loop-pass-safe)
        std::uint32_t generation = 0;
        float* dataL = nullptr;  ///< into slotBuffer_, block capacity; data at [destFrame, destFrame+run)
        float* dataR = nullptr;
    };

    struct Row
    {
        std::atomic<int> state{ 0 };  ///< RowState, callback-thread writer only
        TrackId trackId = kInvalidTrackId;
        int trackIndex = -1;          ///< captured at adoption; worker verifies id match per segment
        std::uint32_t generation = 0; ///< bumped at adoption and at every reset
        std::int64_t playbackShift = 0;  ///< captured at adoption (changes force a reset)
        std::int64_t startAfterSerial = 0;  ///< adoption block's serial; worker starts after its join
        // Worker-private production cursor (initialized under the adoption publish).
        std::int64_t nextProducePos = 0;  ///< transport-domain segment start
        int produceFill = 0;              ///< frames already produced of the current block
        bool workerStarted = false;
        std::uint32_t produceSeq = 0;
        // Callback-private consumption cursor.
        std::uint32_t consumeSeq = 0;
        int consecutiveMisses = 0;
        bool consumeTouched = false;     ///< a consume/miss was attempted this block
        bool consumeCheckArmed = false;  ///< last block was playing+planned => absence = left the plan
        // SPSC ring: worker writes slot fields then release-increments `tail`; the callback
        // consumes at `head` and release-increments it. Slot index = sequence % depth.
        std::array<Slot, kMaxDepth> slots{};
        std::atomic<std::uint32_t> head{ 0 };
        std::atomic<std::uint32_t> tail{ 0 };
        // Dekker pair (model doc §1): the worker claims `busy` BEFORE re-checking `stopProduce`;
        // the callback sets `stopProduce` BEFORE reading `busy`. `stopProduce` stays set until
        // the NEXT adoption re-arms the row (so a worker racing a reset always aborts).
        std::atomic<int> busy{ 0 };
        std::atomic<bool> stopProduce{ false };
        std::atomic<bool> workerAckedStop{ false };
        std::atomic<bool> workerActive{ false };  ///< worker-side view: row has work
        std::atomic<bool> workerSelfStopped{ false };  ///< row vanished / hit a limit on the worker side
        /// Set only around the production strip call, so the callback can tell "about to enter
        /// or inside the plugin" from a busy claim that has not touched the chain.
        std::atomic<int> inPlugin{ 0 };
        /// Callback-only: this Scheduled row must not be live-rendered because the worker has
        /// already committed to its first segment and that segment is not queued yet.
        bool primingExclusive = false;
    };

    /// One step of the engine's deterministic segmentation (transport domain) — replicates
    /// `PlaybackEngine`'s linear/cycle-wrap run arithmetic exactly. `fill > 0` means the step
    /// continues a block after a wrap (never wraps again; the block ends after it).
    struct SegStep
    {
        bool freeze = true;       ///< no renderable segment (arrangement end / degenerate)
        std::int64_t start = 0;   ///< transport-domain segment start
        int run = 0;
        int destFrame = 0;
        std::int64_t nextPos = 0;
        int nextFill = 0;
    };
    static SegStep stepSegment(std::int64_t pos, int fill, int blockFrames, bool cycle,
                               std::int64_t locL, std::int64_t locR, std::int64_t end) noexcept;
    /// The next block's expected transport start after a full block from `t0` (<= 2 segments).
    static std::int64_t predictNextBlockStart(std::int64_t t0, int blockFrames, bool cycle,
                                              std::int64_t locL, std::int64_t locR,
                                              std::int64_t end) noexcept;

    // Worker body (worker thread, or the test pump on the message thread).
    int workerScanOnce() noexcept;
    bool workerRenderOneSegment(Row& row) noexcept;
    void workerThreadMain() noexcept;

    // Callback-thread helpers.
    void audioThread_discardResetRow(Row& row) noexcept;
    void audioThread_purgeRing(Row& row) noexcept;
    /// True when the ring head is the exact first segment of this block (the key tryConsume
    /// will request). False when the ring is empty or the head is a different segment.
    [[nodiscard]] bool audioThread_primingHeadMatches(const Row& row, const BlockBeginInfo& info) const noexcept;
    /// Dekker decline of a prime the worker has not entered. True = row is Live again and this
    /// block renders it directly (no miss, no discard count). False = do not live-render: the
    /// worker still holds the claim (caller keeps the row exclusive) or a segment was already
    /// published (the row is Ahead and the existing ring consumes it).
    [[nodiscard]] bool audioThread_tryDeclineUncommittedPrime(Row& row) noexcept;
    /// [Test only] If the decline-race gate is armed, park until the test has published the
    /// prime. Returns true when this call parked and must clear the gate before returning.
    [[nodiscard]] bool audioThread_enterDeclineRaceForTests() noexcept;
    /// [Test only] After a successful publish, stay out of the next claim while the callback
    /// is inside the armed decline decision.
    void workerHoldAfterPublishedPrimeForDeclineRace() noexcept;
    void audioThread_refreshOwnedFlags() noexcept;
    void audioThread_addCooldown(TrackId trackId) noexcept;
    [[nodiscard]] bool audioThread_isCoolingDown(TrackId trackId) const noexcept;
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

    // Block-context template double buffer (callback publishes, worker reads; carries the
    // loop geometry the worker's segmentation uses).
    PluginProcessTransportContext contextTemplates_[2]{};
    std::atomic<int> contextTemplateIndex_{ 0 };

    /// Monotone callback block serial (callback increments in beginBlock; the join publishes it
    /// into `joinedSerial_`). Wrap-safe replacement for v1's timeline-position progress gate.
    std::int64_t blockSerial_ = 0;
    std::atomic<std::int64_t> joinedSerial_{ std::numeric_limits<std::int64_t>::min() };

    std::atomic<bool> fullResetRequested_{ false };
    std::atomic<bool> prepared_{ false };
    std::atomic<int> captureHold_{ 0 };

    // Depth-counted pause with an epoch handshake (see the threading notes above). The worker
    // parks while `pauseDepth_ > 0` and, while parked, publishes the latest request epoch as its
    // acknowledgment. A waiter that sees `pauseAckedEpoch_ >= its epoch` KNOWS the worker was in
    // the parked branch (outside every chain) after the request existed — and the worker stays
    // parked for as long as the waiter holds its depth. This replaces the v2 bool pair, whose
    // stale acknowledgment could be observed just as the worker re-entered a scan.
    std::atomic<int> pauseDepth_{ 1 };  ///< starts parked; `prepareForDevice` releases the park
    std::atomic<std::uint64_t> pauseEpoch_{ 0 };
    std::atomic<std::uint64_t> pauseAckedEpoch_{ 0 };
    std::atomic<int> pauseAckTimeoutMs_{ 2000 };
    std::atomic<int> deferPrimeForTests_{ 0 };
    std::atomic<int> declineRace_{ 0 };
    bool deviceParkHeld_ = true;  ///< message-thread bookkeeping of the between-devices park depth

    /// Depth + epoch acquire; true only on real acknowledgment (balanced release on failure).
    [[nodiscard]] bool acquireWorkerPause(int timeoutMs) noexcept;
    /// Acquire that never gives up (bounded attempts, diagnostics, unbounded total). Used where
    /// the very next step frees or reconfigures worker-reachable resources.
    void acquireWorkerPauseBlocking(const char* why) noexcept;
    void releaseWorkerPause() noexcept;

    // Callback-private continuity + block state (audio thread only, block-stable after beginBlock).
    std::int64_t expectedNextT0_ = std::numeric_limits<std::int64_t>::min();
    bool haveExpectedNextT0_ = false;
    std::int64_t lastPlaybackShift_ = 0;
    bool lastCycleActive_ = false;
    std::int64_t lastLocL_ = 0;
    std::int64_t lastLocR_ = 0;
    bool adoptionAllowedThisBlock_ = false;
    std::int64_t blockT0_ = 0;
    int blockNumSamples_ = 0;
    std::int64_t blockArrangementEnd_ = 0;
    std::int64_t blockPlaybackShift_ = 0;
    TrackId blockArmedTrackId_ = kInvalidTrackId;
    TrackId blockRecordingTrackId_ = kInvalidTrackId;

    /// Re-adoption cooldown after a miss abandonment (callback-private).
    struct Cooldown
    {
        TrackId trackId = kInvalidTrackId;
        int blocksLeft = 0;
    };
    std::array<Cooldown, kMaxRows> cooldowns_{};

    std::atomic<bool> anyNonLive_{ false };

    // Worker thread control.
    std::thread workerThread_;
    std::atomic<bool> workerShouldExit_{ false };

    // Counters (relaxed; diagnostics only — never surfaced in UI).
    std::atomic<std::int64_t> cAdopted_{ 0 };
    std::atomic<std::int64_t> cProduced_{ 0 };
    std::atomic<std::int64_t> cConsumed_{ 0 };
    std::atomic<std::int64_t> cMissed_{ 0 };
    std::atomic<std::int64_t> cStale_{ 0 };
    std::atomic<std::int64_t> cDrainReleases_{ 0 };
    std::atomic<std::int64_t> cDiscardResets_{ 0 };
    std::atomic<std::int64_t> cMissAbandons_{ 0 };
};

} // namespace readahead
