// Implementation notes live in ReadAheadRenderer.h and docs/READAHEAD_PROTOTYPE.md.

#include "engine/ReadAheadRenderer.h"

#include "engine/ReadAheadStartupConfig.h"

#include <chrono>

static_assert(readahead::kReadAheadDepthMin == readahead::ReadAheadRenderer::kMinDepth
                  && readahead::kReadAheadDepthMax == readahead::ReadAheadRenderer::kMaxDepth,
              "startup depth clamp must match the renderer");

#include "domain/Session.h"
#include "domain/SessionSnapshot.h"

namespace readahead
{

namespace
{
std::atomic<int> gConfiguredDepth{ 0 };

void sleepBrieflyMicros(const int micros) noexcept
{
    std::this_thread::sleep_for(std::chrono::microseconds(micros));
}
} // namespace

void setConfiguredReadAheadDepth(const int depthBlocksOrZero) noexcept
{
    gConfiguredDepth.store(depthBlocksOrZero, std::memory_order_relaxed);
}

int configuredReadAheadDepth() noexcept
{
    return gConfiguredDepth.load(std::memory_order_relaxed);
}

ReadAheadRenderer::ReadAheadRenderer(const Deps& deps, const int depthBlocks, const bool spawnWorkerThread)
    : deps_(deps)
    , depth_(juce::jlimit(kMinDepth, kMaxDepth, depthBlocks))
    , pumpMode_(!spawnWorkerThread)
{
    // The worker starts PARKED (pauseDepth_ initialized to 1 — the "between devices" park);
    // `prepareForDevice` (which allocates the buffers it renders into) releases the park.
    if (!pumpMode_)
    {
        workerThread_ = std::thread([this] { workerThreadMain(); });
    }
}

ReadAheadRenderer::~ReadAheadRenderer()
{
    workerShouldExit_.store(true, std::memory_order_release);
    if (workerThread_.joinable())
    {
        workerThread_.join();
    }
}

// ---------------------------------------------------------------------------
// Message-thread lifecycle
// ---------------------------------------------------------------------------

void ReadAheadRenderer::prepareForDevice(const double sampleRate, const int blockSizeSamples)
{
    juce::ignoreUnused(sampleRate);
    prepared_.store(false, std::memory_order_release);
    if (!acquireWorkerPause(pauseAckTimeoutMs_.load(std::memory_order_relaxed)))
    {
        // No real acknowledgment (a worker stuck inside a plugin render from an earlier device
        // session). The slot/scratch buffers it may still be writing into must NOT be resized or
        // freed, and the rows it may still claim must not be reset. The renderer stays DORMANT
        // (`prepared_ == false`: `audioThread_beginBlock` no-ops and nothing is ever adopted);
        // the park depth, when held, keeps the pause requested so the worker parks the moment
        // its render returns. The next device start retries.
        jassertfalse;
        juce::Logger::writeToLog("[read-ahead] prepareForDevice: worker pause not acknowledged — "
                                 "renderer stays dormant for this device session");
        return;
    }
    const int bs = juce::jmax(16, blockSizeSamples);
    if (bs > slotCapacitySamples_)
    {
        slotBuffer_.setSize(kMaxRows * depth_ * 2, bs, false, true, true);
        workerScratch_.setSize(2, bs, false, true, true);
        slotCapacitySamples_ = bs;
    }
    workerScratchPtrs_[0] = workerScratch_.getWritePointer(0);
    workerScratchPtrs_[1] = workerScratch_.getWritePointer(1);
    workerScratchCapacity_ = workerScratch_.getNumSamples();
    workerMidiScratch_.ensureSize(1024);
    blockSizeSamples_ = bs;
    for (int r = 0; r < kMaxRows; ++r)
    {
        Row& row = rows_[(size_t)r];
        for (int s = 0; s < depth_; ++s)
        {
            Slot& slot = row.slots[(size_t)s];
            slot.dataL = slotBuffer_.getWritePointer(((r * depth_) + s) * 2);
            slot.dataR = slotBuffer_.getWritePointer(((r * depth_) + s) * 2 + 1);
        }
        // No callback is running: hard-reset the row to Live / inactive.
        row.workerActive.store(false, std::memory_order_release);
        row.stopProduce.store(true, std::memory_order_seq_cst);
        row.workerAckedStop.store(false, std::memory_order_release);
        row.workerSelfStopped.store(false, std::memory_order_release);
        row.head.store(0, std::memory_order_release);
        row.tail.store(0, std::memory_order_release);
        row.state.store((int)RowState::Live, std::memory_order_release);
        row.trackId = kInvalidTrackId;
    }
    for (auto& cd : cooldowns_)
    {
        cd = Cooldown{};
    }
    anyNonLive_.store(false, std::memory_order_release);
    haveExpectedNextT0_ = false;
    blockSerial_ = 0;
    joinedSerial_.store(std::numeric_limits<std::int64_t>::min(), std::memory_order_release);
    prepared_.store(true, std::memory_order_release);
    if (deviceParkHeld_)
    {
        releaseWorkerPause(); // the between-devices park
        deviceParkHeld_ = false;
    }
    releaseWorkerPause(); // this call's own acquire — worker runs once no one else holds a pause
}

void ReadAheadRenderer::releaseForDevice() noexcept
{
    prepared_.store(false, std::memory_order_release);
    // The host releases plugin resources right after this returns (`releaseResources` on live
    // instances), and the row reset below rewrites state a claiming worker reads: only a REAL
    // acknowledgment may let this proceed. A plugin stuck inside the worker's render stalls
    // device stop with all resources retained — the same guarantee JUCE's device teardown gives
    // against the audio callback, provided here for the worker.
    acquireWorkerPauseBlocking("releaseForDevice");
    for (auto& row : rows_)
    {
        row.workerActive.store(false, std::memory_order_release);
        row.stopProduce.store(true, std::memory_order_seq_cst);
        row.head.store(0, std::memory_order_release);
        row.tail.store(0, std::memory_order_release);
        row.state.store((int)RowState::Live, std::memory_order_release);
        row.trackId = kInvalidTrackId;
    }
    anyNonLive_.store(false, std::memory_order_release);
    haveExpectedNextT0_ = false;
    // Stays parked until the next prepareForDevice: keep exactly one park depth.
    if (deviceParkHeld_)
    {
        releaseWorkerPause(); // fold this call's acquire into the already-held park
    }
    else
    {
        deviceParkHeld_ = true; // this call's acquire becomes the park
    }
}

bool ReadAheadRenderer::pauseWorkerAndWait() noexcept
{
    return acquireWorkerPause(pauseAckTimeoutMs_.load(std::memory_order_relaxed));
}

bool ReadAheadRenderer::acquireWorkerPause(const int timeoutMs) noexcept
{
    pauseDepth_.fetch_add(1, std::memory_order_seq_cst);
    if (pumpMode_ || !workerThread_.joinable())
    {
        return true; // pump mode: the pump and this caller share the message thread
    }
    const std::uint64_t epoch = pauseEpoch_.fetch_add(1, std::memory_order_seq_cst) + 1;
    // The worker publishes the latest request epoch from its PARKED branch only (it holds no
    // chain, no map and no snapshot there). Seeing `acked >= epoch` therefore proves the worker
    // parked after this request existed — and it stays parked while our depth is held.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(juce::jmax(1, timeoutMs));
    while (pauseAckedEpoch_.load(std::memory_order_acquire) < epoch)
    {
        if (std::chrono::steady_clock::now() > deadline)
        {
            // No acknowledgment: withdraw this request (balanced). The caller gained NO
            // exclusivity and must abort, defer or retry — never proceed. Other holders'
            // depths (if any) remain untouched.
            pauseDepth_.fetch_sub(1, std::memory_order_seq_cst);
            return false;
        }
        sleepBrieflyMicros(100);
    }
    return true;
}

void ReadAheadRenderer::acquireWorkerPauseBlocking(const char* why) noexcept
{
    bool reported = false;
    while (!acquireWorkerPause(pauseAckTimeoutMs_.load(std::memory_order_relaxed)))
    {
        if (!reported)
        {
            reported = true;
            jassertfalse; // debug visibility; the SAFE behavior below is to keep waiting
            juce::Logger::writeToLog(juce::String("[read-ahead] ") + why
                                     + ": worker pause not acknowledged — waiting (resources "
                                       "are retained until the worker lets go)");
        }
    }
}

void ReadAheadRenderer::resumeWorker() noexcept
{
    releaseWorkerPause();
}

void ReadAheadRenderer::releaseWorkerPause() noexcept
{
    const int previous = pauseDepth_.fetch_sub(1, std::memory_order_seq_cst);
    jassert(previous > 0);
    juce::ignoreUnused(previous);
}

void ReadAheadRenderer::requestFullReset() noexcept
{
    fullResetRequested_.store(true, std::memory_order_release);
}

void ReadAheadRenderer::beginStateCaptureHold() noexcept
{
    captureHold_.fetch_add(1, std::memory_order_acq_rel);
}

void ReadAheadRenderer::endStateCaptureHold() noexcept
{
    captureHold_.fetch_sub(1, std::memory_order_acq_rel);
}

// ---------------------------------------------------------------------------
// Deterministic segmentation (replicates PlaybackEngine's run arithmetic exactly)
// ---------------------------------------------------------------------------

ReadAheadRenderer::SegStep ReadAheadRenderer::stepSegment(const std::int64_t pos, const int fill,
                                                          const int blockFrames, const bool cycle,
                                                          const std::int64_t locL, const std::int64_t locR,
                                                          const std::int64_t end) noexcept
{
    SegStep s;
    s.nextPos = pos;
    s.nextFill = 0;
    const std::int64_t avail = end - pos;
    const std::int64_t rem = (std::int64_t)blockFrames - (std::int64_t)fill;
    if (avail <= 0 || rem <= 0)
    {
        return s; // freeze (arrangement end / degenerate) — the engine early-returns here too
    }
    if (fill > 0)
    {
        // Wrap continuation: the engine wraps AT MOST once per block; the second segment fills
        // the rest of the block bounded by the loop span, then the block ends
        // (`secondRun = min(remainingInBlock, loopSpan, end - locL)`; short second runs land
        // the playhead at locL + secondRun — possibly exactly locR, where playback goes linear).
        const std::int64_t toR = cycle ? (locR - pos) : avail;
        const std::int64_t run = juce::jmin(rem, avail, juce::jmax((std::int64_t)0, toR));
        if (run <= 0)
        {
            return s;
        }
        s.freeze = false;
        s.start = pos;
        s.run = (int)run;
        s.destFrame = fill;
        s.nextPos = pos + run;
        s.nextFill = 0;
        return s;
    }
    if (!cycle || pos >= locR)
    {
        // Linear (cycle off, or playhead at/past the right locator — the engine stays linear).
        const std::int64_t run = juce::jmin(rem, avail);
        s.freeze = false;
        s.start = pos;
        s.run = (int)run;
        s.destFrame = 0;
        s.nextPos = pos + run;
        s.nextFill = 0;
        return s;
    }
    const std::int64_t toR = locR - pos;
    const std::int64_t run = juce::jmin(rem, avail, toR);
    if (run <= 0)
    {
        return s;
    }
    s.freeze = false;
    s.start = pos;
    s.run = (int)run;
    s.destFrame = 0;
    if (run == toR)
    {
        // Reached the right locator: wrap. The rest of the block (if any) renders from locL.
        const int fill2 = (int)run;
        s.nextPos = locL;
        s.nextFill = fill2 >= blockFrames ? 0 : fill2;
        return s;
    }
    s.nextPos = pos + run;
    s.nextFill = 0;
    return s;
}

std::int64_t ReadAheadRenderer::predictNextBlockStart(const std::int64_t t0, const int blockFrames,
                                                      const bool cycle, const std::int64_t locL,
                                                      const std::int64_t locR,
                                                      const std::int64_t end) noexcept
{
    std::int64_t pos = t0;
    int fill = 0;
    for (int i = 0; i < 2; ++i) // a block is at most two segments
    {
        const SegStep s = stepSegment(pos, fill, blockFrames, cycle, locL, locR, end);
        if (s.freeze)
        {
            return pos; // playhead frozen (the engine advances by 0)
        }
        pos = s.nextPos;
        fill = s.nextFill;
        if (fill == 0)
        {
            return pos;
        }
    }
    return pos;
}

// ---------------------------------------------------------------------------
// Audio-callback API
// ---------------------------------------------------------------------------

void ReadAheadRenderer::audioThread_beginBlock(const BlockBeginInfo& info) noexcept
{
    ++blockSerial_;
    blockT0_ = info.t0;
    blockNumSamples_ = info.numSamples;
    blockArrangementEnd_ = info.arrangementEnd;
    blockPlaybackShift_ = info.playbackShift;
    blockArmedTrackId_ = info.armedTrackId;
    blockRecordingTrackId_ = info.recordingTrackId;
    adoptionAllowedThisBlock_ = false;
    if (!prepared_.load(std::memory_order_acquire))
    {
        return;
    }

    for (auto& cd : cooldowns_)
    {
        if (cd.blocksLeft > 0 && --cd.blocksLeft == 0)
        {
            cd.trackId = kInvalidTrackId;
        }
    }

    const bool anyOwned = anyNonLive_.load(std::memory_order_relaxed);
    const bool force = fullResetRequested_.exchange(false, std::memory_order_acq_rel);
    const bool holdActive = captureHold_.load(std::memory_order_acquire) > 0;

    // Geometry/offset edits while owned are deliberate discontinuities (model doc §3/§4): the
    // queue was predicted under the old basis. Pause is NOT one — position continuity is only
    // checked while playing, against the prediction from the last playing block (which a pause
    // leaves untouched, so resume-at-the-same-position continues the queue).
    const bool geometryChanged = anyOwned
                                 && (info.cycleActive != lastCycleActive_
                                     || (info.cycleActive
                                         && (info.locLeft != lastLocL_ || info.locRight != lastLocR_)));
    const bool shiftChanged = anyOwned && info.playbackShift != lastPlaybackShift_;
    bool discontinuity = force || geometryChanged || shiftChanged || (info.playing && !info.planUsable);
    if (info.playing)
    {
        if (haveExpectedNextT0_ && info.t0 != expectedNextT0_)
        {
            discontinuity = true; // seek, stop-button jump, frozen playhead, prediction divergence
        }
        expectedNextT0_ = predictNextBlockStart(info.t0, juce::jmax(1, info.numSamples),
                                                info.cycleActive, info.locLeft, info.locRight,
                                                info.arrangementEnd);
        haveExpectedNextT0_ = true;
    }
    // (not playing: keep the continuation basis — a pause must resume against it)
    lastPlaybackShift_ = info.playbackShift;
    lastCycleActive_ = info.cycleActive;
    lastLocL_ = info.locLeft;
    lastLocR_ = info.locRight;

    for (auto& row : rows_)
    {
        const auto state = (RowState)row.state.load(std::memory_order_relaxed);
        if (state == RowState::Live)
        {
            continue;
        }
        if (discontinuity)
        {
            audioThread_discardResetRow(row);
            continue;
        }
        // The mode does not hold for this row: leave it (model doc §7) and sit out the cooldown.
        if ((state == RowState::Ahead || state == RowState::Draining)
            && row.consecutiveMisses >= kConsecutiveMissAbandonThreshold)
        {
            audioThread_addCooldown(row.trackId);
            cMissAbandons_.fetch_add(1, std::memory_order_relaxed);
            audioThread_discardResetRow(row);
            continue;
        }
        // Consume absence: the row was owned through a playing, planned block but the engine
        // never offered it a consume — it left the routing plan / rendered path. Release it.
        if ((state == RowState::Ahead || state == RowState::Draining)
            && row.consumeCheckArmed && !row.consumeTouched)
        {
            audioThread_discardResetRow(row);
            continue;
        }
        if (state == RowState::Scheduled)
        {
            // Monitor/record must win even during a prime: the direct path changes semantics
            // this same block, so a queued or in-flight clip segment must not play.
            const bool handover = (info.monitorView != nullptr && info.monitorView->contains(row.trackId))
                                  || (blockRecordingTrackId_ != kInvalidTrackId
                                      && row.trackId == blockRecordingTrackId_);
            if (handover)
            {
                audioThread_discardResetRow(row);
                continue;
            }
            // Commit to the consume path only when this block's first segment is already
            // queued, or the worker has already fed the instance (ring holds something, so
            // live-rendering would use a chain that is no longer at this position). An empty
            // ring whose worker has not entered declines back to live: that is the startup
            // case where consumption used to begin before the single worker could deliver,
            // and it must not become silence. The empty read here is not the decision — the
            // decline re-reads the ring after production is stopped and the claim is clear.
            // If the worker has entered, stay exclusive — no live render, no wait.
            row.primingExclusive = false;
            const bool ringHasSegment = row.head.load(std::memory_order_acquire)
                                        != row.tail.load(std::memory_order_acquire);
            if (audioThread_primingHeadMatches(row, info) || ringHasSegment)
            {
                row.state.store((int)RowState::Ahead, std::memory_order_release);
            }
            else if (audioThread_tryDeclineUncommittedPrime(row))
            {
                continue;
            }
            else if ((RowState)row.state.load(std::memory_order_relaxed) == RowState::Scheduled)
            {
                // Still Scheduled: the worker holds the claim and the segment is not queued.
                // A decline that found a published segment has already moved the row to Ahead.
                row.primingExclusive = true;
            }
        }
        const auto current = (RowState)row.state.load(std::memory_order_relaxed);
        if (current == RowState::Ahead || current == RowState::Draining)
        {
            // Immediate handover (discard): the direct path switches these semantics the same
            // block (monitor suppresses clip playback; recording omits the row's clips), so a
            // gapless drain would over-play queued clip audio. Model doc §5.
            const bool handover = (info.monitorView != nullptr && info.monitorView->contains(row.trackId))
                                  || (blockRecordingTrackId_ != kInvalidTrackId
                                      && row.trackId == blockRecordingTrackId_);
            if (handover)
            {
                audioThread_discardResetRow(row);
                continue;
            }
        }
        if (current == RowState::Ahead)
        {
            const bool nearEnd = info.arrangementEnd - info.t0
                                 < (std::int64_t)(depth_ + 2) * (std::int64_t)juce::jmax(1, info.numSamples);
            const bool armed = blockArmedTrackId_ != kInvalidTrackId && row.trackId == blockArmedTrackId_;
            // Gapless drains: the queued audio is still correct; stop producing and play it out.
            // (While paused nothing consumes, so the drain simply completes after resume.)
            const bool wantDrain = (!info.cycleActive && nearEnd) || holdActive || armed
                                   || row.workerSelfStopped.load(std::memory_order_acquire);
            if (wantDrain)
            {
                row.stopProduce.store(true, std::memory_order_seq_cst);
                row.state.store((int)RowState::Draining, std::memory_order_release);
            }
        }
        const auto afterDrainCheck = (RowState)row.state.load(std::memory_order_relaxed);
        if (afterDrainCheck == RowState::Draining || afterDrainCheck == RowState::Abandoning)
        {
            // The worker aborts before producing once it observes `stopProduce` (Dekker pair),
            // so `busy == 0` here means the ring state is final even without an explicit ack.
            const bool workerDone = row.workerAckedStop.load(std::memory_order_acquire)
                                    || row.busy.load(std::memory_order_seq_cst) == 0;
            const bool ringEmpty = row.head.load(std::memory_order_acquire)
                                   == row.tail.load(std::memory_order_acquire);
            if (afterDrainCheck == RowState::Draining && workerDone && ringEmpty)
            {
                row.workerActive.store(false, std::memory_order_release);
                row.state.store((int)RowState::Live, std::memory_order_release);
                cDrainReleases_.fetch_add(1, std::memory_order_relaxed);
            }
            else if (afterDrainCheck == RowState::Abandoning && workerDone)
            {
                audioThread_purgeRing(row);
                row.workerActive.store(false, std::memory_order_release);
                row.state.store((int)RowState::Live, std::memory_order_release);
            }
        }
        // Arm the consume-absence check for the coming block; clear this block's touch flag.
        const auto endState = (RowState)row.state.load(std::memory_order_relaxed);
        row.consumeTouched = false;
        row.consumeCheckArmed = (endState == RowState::Ahead || endState == RowState::Draining)
                                && info.playing && info.planUsable;
    }

    adoptionAllowedThisBlock_ = info.playing && !discontinuity && !holdActive && info.planUsable
                                && info.numSamples > 0 && info.numSamples <= blockSizeSamples_;
    audioThread_refreshOwnedFlags();
}

void ReadAheadRenderer::audioThread_offerAdoption(const TrackId trackId, const int trackIndex) noexcept
{
    if (!adoptionAllowedThisBlock_ || trackId == kInvalidTrackId || trackIndex < 0)
    {
        return;
    }
    // Activation conditions (model doc §2): enough arrangement headroom, non-negative audible
    // positions (including the loop start under cycle), no armed/recording row, no cooldown.
    if (blockArrangementEnd_ - blockT0_ < (std::int64_t)(depth_ + 2) * (std::int64_t)blockNumSamples_)
    {
        return;
    }
    if (blockT0_ + blockPlaybackShift_ < 0)
    {
        return;
    }
    if (lastCycleActive_ && lastLocL_ + blockPlaybackShift_ < 0)
    {
        return;
    }
    if (trackId == blockArmedTrackId_ || trackId == blockRecordingTrackId_)
    {
        return;
    }
    if (audioThread_isCoolingDown(trackId))
    {
        return;
    }
    if (findRowByTrackId(trackId) != nullptr)
    {
        return; // already owned (any state)
    }
    for (auto& row : rows_)
    {
        if ((RowState)row.state.load(std::memory_order_relaxed) != RowState::Live
            || row.workerActive.load(std::memory_order_acquire)
            || row.busy.load(std::memory_order_seq_cst) != 0)
        {
            continue;
        }
        row.trackId = trackId;
        row.trackIndex = trackIndex;
        ++row.generation;
        row.playbackShift = blockPlaybackShift_;
        row.startAfterSerial = blockSerial_;
        // The worker's first segment is the start of the NEXT block (transport domain),
        // derived with the same segmentation the engine will use — gapless by construction.
        row.nextProducePos = predictNextBlockStart(blockT0_, blockNumSamples_, lastCycleActive_,
                                                   lastLocL_, lastLocR_, blockArrangementEnd_);
        row.produceFill = 0;
        row.workerStarted = false;
        row.produceSeq = 0;
        row.consumeSeq = 0;
        row.consecutiveMisses = 0;
        row.consumeTouched = false;
        row.consumeCheckArmed = false;
        row.head.store(0, std::memory_order_release);
        row.tail.store(0, std::memory_order_release);
        row.workerAckedStop.store(false, std::memory_order_release);
        row.workerSelfStopped.store(false, std::memory_order_release);
        row.primingExclusive = false;
        row.stopProduce.store(false, std::memory_order_seq_cst);
        row.state.store((int)RowState::Scheduled, std::memory_order_release);
        // The release publish the worker synchronizes with: every field above is visible once
        // `workerActive` reads true.
        row.workerActive.store(true, std::memory_order_release);
        cAdopted_.fetch_add(1, std::memory_order_relaxed);
        audioThread_refreshOwnedFlags();
        return;
    }
}

bool ReadAheadRenderer::audioThread_isOwnedForRender(const TrackId trackId) const noexcept
{
    if (!anyNonLive_.load(std::memory_order_relaxed))
    {
        return false;
    }
    const Row* const row = findRowByTrackId(trackId);
    if (row == nullptr)
    {
        return false;
    }
    const auto state = (RowState)row->state.load(std::memory_order_relaxed);
    if (state == RowState::Ahead || state == RowState::Draining || state == RowState::Abandoning)
    {
        return true;
    }
    // Priming row whose worker has already entered the chain: the live strip must skip it.
    // tryConsume plays the segment if it has landed, otherwise counts the miss.
    return state == RowState::Scheduled && row->primingExclusive;
}

int ReadAheadRenderer::audioThread_exportExcludedTrackIds(TrackId* const out) const noexcept
{
    int n = 0;
    if (!anyNonLive_.load(std::memory_order_relaxed))
    {
        return 0;
    }
    for (const auto& row : rows_)
    {
        if ((RowState)row.state.load(std::memory_order_relaxed) != RowState::Live)
        {
            out[n++] = row.trackId;
        }
    }
    return n;
}

bool ReadAheadRenderer::audioThread_tryConsume(const TrackId trackId, const std::int64_t timelineStartAudible,
                                               const int run, const int destFrame, ConsumeView& out) noexcept
{
    Row* const row = findRowByTrackId(trackId);
    if (row == nullptr)
    {
        cMissed_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    row->consumeTouched = true;
    const auto state = (RowState)row->state.load(std::memory_order_relaxed);
    const bool primingConsume = state == RowState::Scheduled && row->primingExclusive;
    if (!primingConsume && state != RowState::Ahead && state != RowState::Draining)
    {
        cMissed_.fetch_add(1, std::memory_order_relaxed);
        return false; // Abandoning, or a Scheduled row the live path still owns
    }
    const auto miss = [&]() noexcept {
        // The audible stream advanced past this segment: the expected sequence moves on, so a
        // late worker result becomes stale (discarded by seq, never played from a wrong time).
        ++row->consumeSeq;
        ++row->consecutiveMisses;
        cMissed_.fetch_add(1, std::memory_order_relaxed);
        return false;
    };
    for (;;)
    {
        const std::uint32_t h = row->head.load(std::memory_order_relaxed);
        if (h == row->tail.load(std::memory_order_acquire))
        {
            return miss(); // not ready — silence this segment, ownership retained (model doc §7)
        }
        Slot& s = row->slots[(size_t)(h % (std::uint32_t)depth_)];
        if (s.generation != row->generation || s.seq < row->consumeSeq)
        {
            // Stale (produced for a segment the audible stream already passed, or a previous
            // adoption): discard, never play. Sequence comparison is loop-pass-safe — position
            // ordering is meaningless across a cycle wrap.
            row->head.store(h + 1, std::memory_order_release);
            cStale_.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        if (s.seq == row->consumeSeq && s.start == timelineStartAudible && s.run == run
            && s.destFrame == destFrame)
        {
            if (primingConsume)
            {
                // The in-flight prime landed before the strip pass. From here the row is
                // ahead: the worker keeps the chain, the callback keeps consuming.
                row->primingExclusive = false;
                row->state.store((int)RowState::Ahead, std::memory_order_release);
            }
            out.stageL = s.dataL;
            out.stageR = s.dataR;
            return true;
        }
        // Key mismatch at the expected sequence (segmentation divergence — defensive) or a
        // future sequence: miss without popping; the slot becomes stale on the next attempt.
        return miss();
    }
}

void ReadAheadRenderer::audioThread_releaseConsumed(const TrackId trackId) noexcept
{
    Row* const row = findRowByTrackId(trackId);
    if (row == nullptr)
    {
        return;
    }
    const std::uint32_t h = row->head.load(std::memory_order_relaxed);
    if (h != row->tail.load(std::memory_order_acquire))
    {
        row->head.store(h + 1, std::memory_order_release);
        ++row->consumeSeq;
        row->consecutiveMisses = 0;
        cConsumed_.fetch_add(1, std::memory_order_relaxed);
    }
}

void ReadAheadRenderer::audioThread_noteMiss(const TrackId trackId) noexcept
{
    Row* const row = findRowByTrackId(trackId);
    if (row != nullptr)
    {
        row->consumeTouched = true;
        ++row->consumeSeq;
        ++row->consecutiveMisses;
    }
    cMissed_.fetch_add(1, std::memory_order_relaxed);
}

void ReadAheadRenderer::audioThread_publishJoinedBlock() noexcept
{
    joinedSerial_.store(blockSerial_, std::memory_order_release);
}

void ReadAheadRenderer::audioThread_publishContextTemplate(const PluginProcessTransportContext& context) noexcept
{
    const int next = 1 - contextTemplateIndex_.load(std::memory_order_relaxed);
    contextTemplates_[next] = context;
    contextTemplateIndex_.store(next, std::memory_order_release);
}

// ---------------------------------------------------------------------------
// Callback-thread helpers
// ---------------------------------------------------------------------------

void ReadAheadRenderer::audioThread_discardResetRow(Row& row) noexcept
{
    // `stopProduce` stays set until the next adoption re-arms the row, so a worker that raced
    // past the `workerActive` check aborts after claiming `busy` without touching the ring.
    row.primingExclusive = false;
    row.stopProduce.store(true, std::memory_order_seq_cst);
    cDiscardResets_.fetch_add(1, std::memory_order_relaxed);
    if (row.busy.load(std::memory_order_seq_cst) == 0)
    {
        audioThread_purgeRing(row);
        row.workerActive.store(false, std::memory_order_release);
        row.state.store((int)RowState::Live, std::memory_order_release);
        return;
    }
    // The worker is inside this row's render: the row stays Abandoning (silent, never rendered
    // live concurrently) until the IN-FLIGHT PLUGIN CALL RETURNS and the worker's stop is
    // observed at a block begin. That is typically well under one block, but it is bounded only
    // by the plugin's processBlock duration — no fixed block count is guaranteed (model doc §5).
    row.state.store((int)RowState::Abandoning, std::memory_order_release);
}

void ReadAheadRenderer::audioThread_purgeRing(Row& row) noexcept
{
    std::uint32_t h = row.head.load(std::memory_order_relaxed);
    const std::uint32_t t = row.tail.load(std::memory_order_acquire);
    if (t != h)
    {
        cStale_.fetch_add((std::int64_t)(t - h), std::memory_order_relaxed);
    }
    row.head.store(t, std::memory_order_release);
    juce::ignoreUnused(h);
}

void ReadAheadRenderer::audioThread_refreshOwnedFlags() noexcept
{
    bool any = false;
    for (const auto& row : rows_)
    {
        if ((RowState)row.state.load(std::memory_order_relaxed) != RowState::Live)
        {
            any = true;
            break;
        }
    }
    anyNonLive_.store(any, std::memory_order_release);
}

void ReadAheadRenderer::audioThread_addCooldown(const TrackId trackId) noexcept
{
    Cooldown* slot = nullptr;
    for (auto& cd : cooldowns_)
    {
        if (cd.trackId == trackId)
        {
            slot = &cd;
            break;
        }
        if (slot == nullptr && cd.blocksLeft == 0)
        {
            slot = &cd;
        }
    }
    if (slot != nullptr)
    {
        slot->trackId = trackId;
        slot->blocksLeft = kMissReAdoptionCooldownBlocks;
    }
}

bool ReadAheadRenderer::audioThread_primingHeadMatches(const Row& row, const BlockBeginInfo& info) const noexcept
{
    const std::uint32_t h = row.head.load(std::memory_order_acquire);
    if (h == row.tail.load(std::memory_order_acquire))
    {
        return false;
    }
    const Slot& s = row.slots[(size_t)(h % (std::uint32_t)depth_)];
    if (s.generation != row.generation || s.seq != row.consumeSeq)
    {
        return false;
    }
    // Same segmentation the engine will consume this block (first segment only; a wrap's
    // second segment is a later tryConsume once the row is Ahead).
    const SegStep seg = stepSegment(info.t0, 0, info.numSamples, info.cycleActive, info.locLeft,
                                    info.locRight, info.arrangementEnd);
    if (seg.freeze || seg.run <= 0)
    {
        return false;
    }
    const std::int64_t audible = seg.start + row.playbackShift;
    return s.start == audible && s.run == seg.run && s.destFrame == seg.destFrame;
}

bool ReadAheadRenderer::audioThread_enterDeclineRaceForTests() noexcept
{
    if (declineRace_.load(std::memory_order_acquire) != 1)
    {
        return false;
    }
    // The caller has already observed an empty ring. Park so a test can publish that segment
    // and drop `busy` before the decision below runs. Production leaves the gate at 0.
    declineRace_.store(2, std::memory_order_release);
    const auto cap = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (declineRace_.load(std::memory_order_acquire) == 2
           && std::chrono::steady_clock::now() < cap)
    {
        std::this_thread::yield();
    }
    return true;
}

void ReadAheadRenderer::workerHoldAfterPublishedPrimeForDeclineRace() noexcept
{
    const int gate = declineRace_.load(std::memory_order_acquire);
    if (gate != 2 && gate != 3)
    {
        return;
    }
    // The first segment is published and `busy` is already clear. Stay out of the next claim
    // until the callback finishes the decline decision (it stores 0). A timeout only keeps a
    // failed test from hanging the worker.
    const auto cap = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < cap)
    {
        const int now = declineRace_.load(std::memory_order_acquire);
        if (now != 2 && now != 3)
        {
            return;
        }
        std::this_thread::yield();
    }
}

bool ReadAheadRenderer::testRowPublishedAndUnclaimed(const TrackId trackId) const noexcept
{
    const Row* const row = findRowByTrackId(trackId);
    if (row == nullptr)
    {
        return false;
    }
    const bool published = row->head.load(std::memory_order_acquire) != row->tail.load(std::memory_order_acquire);
    const bool unclaimed = row->busy.load(std::memory_order_seq_cst) == 0
                           && row->inPlugin.load(std::memory_order_seq_cst) == 0;
    return published && unclaimed;
}

bool ReadAheadRenderer::audioThread_tryDeclineUncommittedPrime(Row& row) noexcept
{
    // Ring is empty (caller checked). The test rendezvous, when armed, lets the worker publish
    // that segment and clear `busy` before the loads below — the interleaving this decision
    // has to get right. Production does not arm it.
    const bool inRace = audioThread_enterDeclineRaceForTests();
    const auto leaveRace = [&]() noexcept {
        if (inRace)
        {
            declineRace_.store(0, std::memory_order_release);
        }
    };

    // Claim the stop BEFORE re-reading the worker's claim, matching discardResetRow's Dekker
    // pair. A worker that has not entered observes the stop and never touches the plugin.
    if (row.inPlugin.load(std::memory_order_seq_cst) != 0 || row.busy.load(std::memory_order_seq_cst) != 0)
    {
        leaveRace();
        return false;
    }
    row.stopProduce.store(true, std::memory_order_seq_cst);
    if (row.inPlugin.load(std::memory_order_seq_cst) != 0 || row.busy.load(std::memory_order_seq_cst) != 0)
    {
        // The worker committed between the two loads. Do not leave the stop set: the segment
        // it is about to publish is the one this block wants to play, and production must
        // continue afterwards. The caller holds the row exclusive instead.
        row.stopProduce.store(false, std::memory_order_seq_cst);
        leaveRace();
        return false;
    }
    // The worker is outside the chain and further production is stopped. The caller's earlier
    // empty-ring read is not evidence: the worker stores `tail` (release) before it clears
    // `busy` (release), and the seq_cst load above observed `busy == 0`, so it synchronizes
    // with that clear. This acquire therefore sees a segment published in between. A segment
    // that is still absent cannot appear afterwards — the worker has to claim `busy` (seq_cst)
    // and only then load `stopProduce` (seq_cst) before it may enter the plugin, and this stop
    // is already visible to that load. Reading the ring before the stop would miss the same race.
    if (row.head.load(std::memory_order_acquire) != row.tail.load(std::memory_order_acquire))
    {
        // Already fed. Stay on the consume path and withdraw the stop so the next scan is not
        // permanently barred. `workerActive` stays set. The caller consumes through the ring
        // when the key matches and does not live-render this instance.
        row.stopProduce.store(false, std::memory_order_seq_cst);
        row.workerAckedStop.store(false, std::memory_order_release);
        row.primingExclusive = false;
        row.state.store((int)RowState::Ahead, std::memory_order_release);
        leaveRace();
        return false;
    }
    row.primingExclusive = false;
    row.workerActive.store(false, std::memory_order_release);
    row.state.store((int)RowState::Live, std::memory_order_release);
    leaveRace();
    return true;
}

bool ReadAheadRenderer::audioThread_isCoolingDown(const TrackId trackId) const noexcept
{
    for (const auto& cd : cooldowns_)
    {
        if (cd.trackId == trackId && cd.blocksLeft > 0)
        {
            return true;
        }
    }
    return false;
}

ReadAheadRenderer::Row* ReadAheadRenderer::findRowByTrackId(const TrackId trackId) noexcept
{
    for (auto& row : rows_)
    {
        if (row.trackId == trackId
            && (RowState)row.state.load(std::memory_order_relaxed) != RowState::Live)
        {
            return &row;
        }
    }
    return nullptr;
}

const ReadAheadRenderer::Row* ReadAheadRenderer::findRowByTrackId(const TrackId trackId) const noexcept
{
    return const_cast<ReadAheadRenderer*>(this)->findRowByTrackId(trackId);
}

// ---------------------------------------------------------------------------
// Worker
// ---------------------------------------------------------------------------

int ReadAheadRenderer::workerScanOnce() noexcept
{
    int rendered = 0;
    for (auto& row : rows_)
    {
        if (!row.workerActive.load(std::memory_order_acquire))
        {
            continue;
        }
        // Test-only gate, checked BEFORE the busy claim so the callback can still decline
        // the prime and render live. Production leaves the flag clear.
        if (!row.workerStarted && deferPrimeForTests_.load(std::memory_order_acquire) != 0)
        {
            continue;
        }
        row.busy.store(1, std::memory_order_seq_cst);
        if (row.stopProduce.load(std::memory_order_seq_cst))
        {
            row.busy.store(0, std::memory_order_release);
            // Ack only if the stop is still the one we observed. A decline that found a
            // published segment withdraws the stop; acking that withdrawn stop would let a
            // later drain treat the worker as finished while a new render is in the plugin.
            if (row.stopProduce.load(std::memory_order_seq_cst))
            {
                if (!row.workerAckedStop.load(std::memory_order_relaxed))
                {
                    row.workerAckedStop.store(true, std::memory_order_release);
                }
            }
            else
            {
                row.workerAckedStop.store(false, std::memory_order_release);
            }
            continue;
        }
        row.workerAckedStop.store(false, std::memory_order_release);
        const bool did = workerRenderOneSegment(row);
        row.busy.store(0, std::memory_order_release);
        if (did)
        {
            ++rendered;
            // Test-only. After the release of `busy` above, so a parked decline observes the
            // published segment with the claim already clear. No-op unless a test armed the gate.
            workerHoldAfterPublishedPrimeForDeclineRace();
        }
    }
    return rendered;
}

bool ReadAheadRenderer::workerRenderOneSegment(Row& row) noexcept
{
    if (!row.workerStarted)
    {
        // Gapless adoption (model doc §1): the worker starts only after the adoption block's
        // join published its serial — the callback's last touch of that chain. The monotone
        // serial is wrap-safe (timeline positions are not).
        if (joinedSerial_.load(std::memory_order_acquire) < row.startAfterSerial)
        {
            return false;
        }
        row.workerStarted = true;
    }
    const std::uint32_t h = row.head.load(std::memory_order_acquire);
    const std::uint32_t t = row.tail.load(std::memory_order_relaxed);
    if (t - h >= (std::uint32_t)depth_)
    {
        return false; // ring full — bounded queue, bounded state lead
    }
    if (deps_.session == nullptr)
    {
        return false;
    }
    const std::shared_ptr<const SessionSnapshot> snap = deps_.session->loadSessionSnapshotForAudioThread();
    if (snap == nullptr)
    {
        return false;
    }
    // The adopted index must still be the adopted track: track-list edits release the row
    // instead of silently rendering (and sharing a pre-gain ramp entry with) a DIFFERENT track.
    if (row.trackIndex >= snap->getNumTracks()
        || snap->getTrack(row.trackIndex).getId() != row.trackId)
    {
        row.workerSelfStopped.store(true, std::memory_order_release);
        return false;
    }
    // Loop geometry rides the published context template (the callback's own values; a change
    // while owned forces a discard reset, so divergence here is bounded and key-checked anyway).
    const PluginProcessTransportContext contextTemplate
        = contextTemplates_[contextTemplateIndex_.load(std::memory_order_acquire)];
    const std::int64_t end = snap->getArrangementExtentSamples();
    const SegStep seg = stepSegment(row.nextProducePos, row.produceFill, blockSizeSamples_,
                                    contextTemplate.isLooping, contextTemplate.loopStartSample,
                                    contextTemplate.loopEndSample, end);
    if (seg.freeze || seg.run <= 0)
    {
        // Arrangement end / degenerate geometry ahead: stop producing; the near-end drain (or
        // the discontinuity reset at the actual freeze) releases the row.
        row.workerSelfStopped.store(true, std::memory_order_release);
        return false;
    }
    const std::int64_t startAudible = seg.start + row.playbackShift;
    if (startAudible < 0 || seg.destFrame + seg.run > blockSizeSamples_)
    {
        row.workerSelfStopped.store(true, std::memory_order_release);
        return false;
    }
    Slot& slot = row.slots[(size_t)(t % (std::uint32_t)depth_)];
    playback_mix_helpers::clearStereoScratch(slot.dataL + seg.destFrame, slot.dataR + seg.destFrame,
                                             seg.run);

    // Fresh per segment: session snapshot (above), insert map, solo view, context template —
    // control changes apply late by <= depth segments (model doc §6), lifetime is per segment.
    const std::shared_ptr<const PluginAudioThreadMap> map
        = deps_.pluginHost != nullptr ? deps_.pluginHost->audioThread_acquireMapForBlock() : nullptr;
    const PluginAudioThreadMap::Entry* const entry
        = map != nullptr ? PluginInsertHost::audioThread_findEntry(*map, row.trackId) : nullptr;

    playback_mix_helpers::AudioStripInsertAccess access;
    if (entry != nullptr && deps_.pluginHost != nullptr)
    {
        access.host = deps_.pluginHost;
        access.entry = entry;
        access.chainScratch = workerScratchPtrs_;
        access.chainScratchCapacity = workerScratchCapacity_;
        access.chainMidiScratch = &workerMidiScratch_;
        access.laneIndex = PluginInsertHost::kReadAheadProcessingLane;
        PluginProcessTransportContext context = contextTemplate;
        context.timelineSample = startAudible;
        // This chain's OWN playhead — the worker is its only writer while the row is owned
        // (the global setter excludes owned rows).
        PluginInsertHost::audioThread_setEntryTransportContext(*entry, context);
    }

    const SoloMuteView* soloView = nullptr;
    std::shared_ptr<const SoloMuteView> soloSnap;
    if (deps_.soloViewAtomic != nullptr)
    {
        soloSnap = deps_.soloViewAtomic->load(std::memory_order_acquire);
        soloView = (soloSnap != nullptr && soloSnap->soloActive) ? soloSnap.get() : nullptr;
    }

    // THE production strip core — identical DSP to the live A1 path. The recording row is
    // never owned (immediate handover), so there is no omitted clip-playback track.
    // `inPlugin` covers exactly this call: the callback's decline path treats it as
    // exclusive ownership and does not live-render the same instance.
    row.inPlugin.store(1, std::memory_order_seq_cst);
    playback_mix_helpers::renderAudioTrackPostStripToStereoScratchWithChainAccess(
        *snap, startAudible, seg.run, seg.destFrame, slot.dataL, slot.dataR, access,
        kInvalidTrackId, end, row.trackIndex, deps_.preGainRamp, soloView);
    row.inPlugin.store(0, std::memory_order_seq_cst);

    slot.start = startAudible;
    slot.run = seg.run;
    slot.destFrame = seg.destFrame;
    slot.seq = row.produceSeq++;
    slot.generation = row.generation;
    row.tail.store(t + 1, std::memory_order_release);
    row.nextProducePos = seg.nextPos;
    row.produceFill = seg.nextFill;
    cProduced_.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void ReadAheadRenderer::workerThreadMain() noexcept
{
    while (!workerShouldExit_.load(std::memory_order_acquire))
    {
        if (pauseDepth_.load(std::memory_order_seq_cst) > 0)
        {
            // Parked: outside every chain, map and snapshot. Publishing the latest request
            // epoch here (and only here) is what makes the acknowledgment REAL — see
            // `acquireWorkerPause`. Re-published every parked iteration so later requests are
            // acknowledged while parked.
            pauseAckedEpoch_.store(pauseEpoch_.load(std::memory_order_seq_cst),
                                   std::memory_order_release);
            sleepBrieflyMicros(200);
            continue;
        }
        if (workerScanOnce() == 0)
        {
            sleepBrieflyMicros(300); // self-paced: the callback never signals (no RT syscalls)
        }
    }
}

int ReadAheadRenderer::testPumpWorkerOnce() noexcept
{
    jassert(pumpMode_);
    if (!pumpMode_ || pauseDepth_.load(std::memory_order_seq_cst) > 0)
    {
        return 0;
    }
    return workerScanOnce();
}

ReadAheadRenderer::Counters ReadAheadRenderer::countersSnapshot() const noexcept
{
    Counters c;
    c.adopted = cAdopted_.load(std::memory_order_relaxed);
    c.producedSegments = cProduced_.load(std::memory_order_relaxed);
    c.consumedSegments = cConsumed_.load(std::memory_order_relaxed);
    c.missedSegments = cMissed_.load(std::memory_order_relaxed);
    c.staleDiscarded = cStale_.load(std::memory_order_relaxed);
    c.drainReleases = cDrainReleases_.load(std::memory_order_relaxed);
    c.discardResets = cDiscardResets_.load(std::memory_order_relaxed);
    c.missAbandons = cMissAbandons_.load(std::memory_order_relaxed);
    return c;
}

} // namespace readahead
