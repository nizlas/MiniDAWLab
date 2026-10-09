// Implementation notes live in ReadAheadRenderer.h and docs/READAHEAD_PROTOTYPE.md.

#include "engine/ReadAheadRenderer.h"

#include <chrono>

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
    // The worker starts PAUSED; `prepareForDevice` (which allocates the buffers it renders
    // into) resumes it.
    pauseRequested_.store(true, std::memory_order_release);
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
    pauseWorkerAndWait();
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
    resumeWorker();
}

void ReadAheadRenderer::releaseForDevice() noexcept
{
    pauseWorkerAndWait();
    prepared_.store(false, std::memory_order_release);
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
    // Stays paused until the next prepareForDevice.
}

void ReadAheadRenderer::pauseWorkerAndWait() noexcept
{
    pauseRequested_.store(true, std::memory_order_seq_cst);
    if (pumpMode_ || !workerThread_.joinable())
    {
        return; // pump mode: the pump and this caller share the message thread
    }
    // Bounded wait: the ack is set between segment renders (never while a row is claimed), so
    // an acked worker holds no chain, no map and no snapshot. One segment render bounds latency.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2000);
    while (!pauseAcked_.load(std::memory_order_acquire))
    {
        if (std::chrono::steady_clock::now() > deadline)
        {
            jassertfalse; // worker unresponsive — diagnostics only, callers proceed regardless
            break;
        }
        sleepBrieflyMicros(100);
    }
}

void ReadAheadRenderer::resumeWorker() noexcept
{
    pauseRequested_.store(false, std::memory_order_release);
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
            // The scheduled block is over (its join published the block serial); from here on
            // the worker owns the chain and the callback consumes.
            row.state.store((int)RowState::Ahead, std::memory_order_release);
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
    return state == RowState::Ahead || state == RowState::Draining || state == RowState::Abandoning;
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
    if (state != RowState::Ahead && state != RowState::Draining)
    {
        cMissed_.fetch_add(1, std::memory_order_relaxed);
        return false; // Abandoning: silent until the worker lets go (counted)
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
    row.stopProduce.store(true, std::memory_order_seq_cst);
    cDiscardResets_.fetch_add(1, std::memory_order_relaxed);
    if (row.busy.load(std::memory_order_seq_cst) == 0)
    {
        audioThread_purgeRing(row);
        row.workerActive.store(false, std::memory_order_release);
        row.state.store((int)RowState::Live, std::memory_order_release);
        return;
    }
    // The worker is inside this row's render: it finishes its segment, observes the stop and
    // acks; the next block begin purges and frees the row. The row is silent meanwhile
    // (consume misses) — never rendered live concurrently.
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
        row.busy.store(1, std::memory_order_seq_cst);
        if (row.stopProduce.load(std::memory_order_seq_cst))
        {
            row.busy.store(0, std::memory_order_release);
            if (!row.workerAckedStop.load(std::memory_order_relaxed))
            {
                row.workerAckedStop.store(true, std::memory_order_release);
            }
            continue;
        }
        const bool did = workerRenderOneSegment(row);
        row.busy.store(0, std::memory_order_release);
        if (did)
        {
            ++rendered;
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
    playback_mix_helpers::renderAudioTrackPostStripToStereoScratchWithChainAccess(
        *snap, startAudible, seg.run, seg.destFrame, slot.dataL, slot.dataR, access,
        kInvalidTrackId, end, row.trackIndex, deps_.preGainRamp, soloView);

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
        if (pauseRequested_.load(std::memory_order_acquire))
        {
            pauseAcked_.store(true, std::memory_order_release);
            sleepBrieflyMicros(200);
            continue;
        }
        pauseAcked_.store(false, std::memory_order_relaxed);
        if (workerScanOnce() == 0)
        {
            sleepBrieflyMicros(300); // self-paced: the callback never signals (no RT syscalls)
        }
    }
}

int ReadAheadRenderer::testPumpWorkerOnce() noexcept
{
    jassert(pumpMode_);
    if (!pumpMode_ || pauseRequested_.load(std::memory_order_acquire))
    {
        if (pauseRequested_.load(std::memory_order_acquire))
        {
            pauseAcked_.store(true, std::memory_order_release);
        }
        return 0;
    }
    pauseAcked_.store(false, std::memory_order_relaxed);
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
