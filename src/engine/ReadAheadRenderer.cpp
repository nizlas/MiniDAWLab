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
    anyNonLive_.store(false, std::memory_order_release);
    haveExpectedT0_ = false;
    liveProgress_.store(std::numeric_limits<std::int64_t>::min(), std::memory_order_release);
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
    haveExpectedT0_ = false;
    // Stays paused until the next prepareForDevice.
}

void ReadAheadRenderer::pauseWorkerAndWait() noexcept
{
    pauseRequested_.store(true, std::memory_order_seq_cst);
    if (pumpMode_ || !workerThread_.joinable())
    {
        return; // pump mode: the pump and this caller share the message thread
    }
    // Bounded wait: the ack is set between row renders (never while a row is claimed), so an
    // acked worker holds no chain, no map and no snapshot. One block render bounds the latency.
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

// ---------------------------------------------------------------------------
// Audio-callback API
// ---------------------------------------------------------------------------

void ReadAheadRenderer::audioThread_beginBlock(const BlockBeginInfo& info) noexcept
{
    blockT0_ = info.t0;
    blockNumSamples_ = info.numSamples;
    blockArrangementEnd_ = info.arrangementEnd;
    blockPlaybackShift_ = info.playbackShift;
    adoptionAllowedThisBlock_ = false;
    if (!prepared_.load(std::memory_order_acquire))
    {
        return;
    }

    const bool force = fullResetRequested_.exchange(false, std::memory_order_acq_rel);
    bool discontinuity = force || !info.playing;
    if (info.playing)
    {
        if (haveExpectedT0_ && info.t0 != expectedT0_)
        {
            discontinuity = true; // seek, wrap, frozen playhead (arrangement end), stop+replay
        }
        if (anyNonLive_.load(std::memory_order_relaxed) && info.playbackShift != lastPlaybackShift_)
        {
            discontinuity = true; // audible position remapped under the queued blocks
        }
        expectedT0_ = info.t0 + info.numSamples;
        haveExpectedT0_ = true;
    }
    else
    {
        haveExpectedT0_ = false;
    }
    lastPlaybackShift_ = info.playbackShift;

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
        if (state == RowState::Scheduled)
        {
            // The scheduled block is over (its join published the live progress); from here on
            // the worker owns the chain and the callback consumes.
            row.state.store((int)RowState::Ahead, std::memory_order_release);
        }
        const auto current = (RowState)row.state.load(std::memory_order_relaxed);
        if (current == RowState::Ahead)
        {
            const bool nearEnd = info.arrangementEnd - info.t0
                                 < (std::int64_t)(depth_ + 2) * (std::int64_t)juce::jmax(1, info.numSamples);
            const bool wantDrain = info.cycleActive || info.recording || nearEnd
                                   || row.workerSelfStopped.load(std::memory_order_acquire)
                                   || (info.monitorView != nullptr && info.monitorView->contains(row.trackId));
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
    }

    adoptionAllowedThisBlock_ = info.playing && !info.cycleActive && !info.recording && !discontinuity
                                && info.numSamples > 0 && info.numSamples <= blockSizeSamples_;
    audioThread_refreshOwnedFlags();
}

void ReadAheadRenderer::audioThread_offerAdoption(const TrackId trackId, const int trackIndex) noexcept
{
    if (!adoptionAllowedThisBlock_ || trackId == kInvalidTrackId || trackIndex < 0)
    {
        return;
    }
    // Activation conditions (model doc §2): enough arrangement headroom that the worker never
    // renders the partial end block, and a non-negative audible position (no silence prefix).
    if (blockArrangementEnd_ - blockT0_ < (std::int64_t)(depth_ + 2) * (std::int64_t)blockNumSamples_)
    {
        return;
    }
    if (blockT0_ + blockPlaybackShift_ < 0)
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
        row.boundary = blockT0_ + blockNumSamples_;
        row.nextProduceT0 = row.boundary;
        row.workerStarted = false;
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
                                               const int run, ConsumeView& out) noexcept
{
    Row* const row = findRowByTrackId(trackId);
    if (row == nullptr)
    {
        return false;
    }
    const auto state = (RowState)row->state.load(std::memory_order_relaxed);
    if (state != RowState::Ahead && state != RowState::Draining)
    {
        cMissed_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    for (;;)
    {
        const std::uint32_t h = row->head.load(std::memory_order_relaxed);
        if (h == row->tail.load(std::memory_order_acquire))
        {
            cMissed_.fetch_add(1, std::memory_order_relaxed);
            return false; // not ready — silence this block, ownership retained (model doc §7)
        }
        Slot& s = row->slots[(size_t)(h % (std::uint32_t)depth_)];
        if (s.generation != row->generation || s.start < timelineStartAudible)
        {
            // Stale (produced for a block that was already missed or invalidated): discard,
            // never play. The worker's input stream stayed contiguous, so dropping OUTPUT
            // keeps the plugin state aligned with the timeline.
            row->head.store(h + 1, std::memory_order_release);
            cStale_.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        if (s.start == timelineStartAudible && s.run == run)
        {
            out.stageL = s.dataL;
            out.stageR = s.dataR;
            cConsumed_.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
        // Head is a FUTURE block (or a run-length mismatch, e.g. diverging arrangement ends):
        // do not pop — the discontinuity reset at the next block begin cleans up.
        cMissed_.fetch_add(1, std::memory_order_relaxed);
        return false;
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
    }
}

void ReadAheadRenderer::audioThread_noteMiss(const TrackId trackId) noexcept
{
    juce::ignoreUnused(trackId);
    cMissed_.fetch_add(1, std::memory_order_relaxed);
}

void ReadAheadRenderer::audioThread_publishLiveProgress(const std::int64_t liveStreamEnd) noexcept
{
    liveProgress_.store(liveStreamEnd, std::memory_order_release);
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
    // The worker is inside this row's render: it finishes its block, observes the stop and
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
    anyNonLive_.store(any, std::memory_order_relaxed);
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
        const bool did = workerRenderOneBlock(row);
        row.busy.store(0, std::memory_order_release);
        if (did)
        {
            ++rendered;
        }
    }
    return rendered;
}

bool ReadAheadRenderer::workerRenderOneBlock(Row& row) noexcept
{
    if (!row.workerStarted)
    {
        // Gapless adoption (model doc §4): the first worker block is `boundary`, started only
        // after the callback's join published that the live stream rendered up to it.
        if (liveProgress_.load(std::memory_order_acquire) < row.boundary)
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
    const std::int64_t start = row.nextProduceT0;
    const std::int64_t end = snap->getArrangementExtentSamples();
    if (end - start < (std::int64_t)blockSizeSamples_)
    {
        // Never render the partial end block ahead (the near-end drain releases the row first).
        row.workerSelfStopped.store(true, std::memory_order_release);
        return false;
    }
    const int run = blockSizeSamples_;
    Slot& slot = row.slots[(size_t)(t % (std::uint32_t)depth_)];
    playback_mix_helpers::clearStereoScratch(slot.dataL, slot.dataR, run);

    // Fresh per block: session snapshot (above), insert map, solo view, context template —
    // control changes apply late by <= depth blocks (model doc §6), lifetime is per block.
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
        PluginProcessTransportContext context
            = contextTemplates_[contextTemplateIndex_.load(std::memory_order_acquire)];
        context.timelineSample = start;
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

    // THE production strip core — identical DSP to the live A1 path. Recording never overlaps
    // ownership (drain trigger), so there is no omitted clip-playback track.
    playback_mix_helpers::renderAudioTrackPostStripToStereoScratchWithChainAccess(
        *snap, start, run, 0, slot.dataL, slot.dataR, access, kInvalidTrackId, end,
        row.trackIndex, deps_.preGainRamp, soloView);

    slot.start = start;
    slot.run = run;
    slot.generation = row.generation;
    row.tail.store(t + 1, std::memory_order_release);
    row.nextProduceT0 = start + run;
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
    c.producedBlocks = cProduced_.load(std::memory_order_relaxed);
    c.consumedBlocks = cConsumed_.load(std::memory_order_relaxed);
    c.missedBlocks = cMissed_.load(std::memory_order_relaxed);
    c.staleDiscarded = cStale_.load(std::memory_order_relaxed);
    c.drainReleases = cDrainReleases_.load(std::memory_order_relaxed);
    c.discardResets = cDiscardResets_.load(std::memory_order_relaxed);
    return c;
}

} // namespace readahead
