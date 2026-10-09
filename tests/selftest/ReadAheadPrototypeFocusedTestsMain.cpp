// =============================================================================
// ReadAheadPrototypeFocusedTests — experimental audio-row read-ahead (Stage B, model v2)
// =============================================================================
// Drives the PRODUCTION PlaybackEngine callback (stub device geometry) with the read-ahead
// renderer in deterministic PUMP mode (no worker thread; the test IS the worker), plus one real
// worker-thread smoke run. The test insert is STATE-DEPENDENT (one-pole feedback) and records the
// playhead position and size of every processBlock call, so misplaced internal time changes both
// the output and the recorded call sequence. Verifies the model's claims (docs/READAHEAD_PROTOTYPE.md):
//   * flag off = no renderer object = unchanged engine (covered by AudioStripParallelFocusedTests;
//     here the flag-off harness doubles as the bit-identity reference);
//   * stable linear playback: output BIT-IDENTICAL to the direct path, adoption included; the
//     insert instance sees an exactly-once, contiguous sample stream; STOP (intent only — the
//     production Space/stop semantics keep the playhead) keeps ownership like a pause;
//   * pause/resume at the same position CONTINUES the queue: no discard, no duplicate feed;
//   * stop-button jump / seek: discard reset with the documented visible residue (the chain's
//     state lead rides the jump; the direct path also re-feeds positions after a jump);
//   * cycle: the worker replicates the engine's wrap segmentation exactly — bit-identity across
//     loop passes, including the short-loop "land at R, go linear" production quirk;
//   * geometry edits (cycle toggle / locator move) while owned: deliberate discontinuity;
//   * playback offset: queue keys live in the AUDIBLE domain (transport + offset) — bit-identity
//     with a non-zero persistent offset (v1 keyed these inconsistently and would always miss);
//   * a deliberately delayed worker: misses are counted silence; after
//     kConsecutiveMissAbandonThreshold consecutive missed segments the row LEAVES the mode
//     (live path + cooldown) instead of hiding a continued run of silent blocks;
//   * a transient single miss: late results discarded as stale (never played from a wrong
//     time), ownership retained, recovery without an abandon;
//   * Monitor enabled mid-run: IMMEDIATE discard handover (same-block monitor semantics, zero
//     added monitor latency) — the bounded duplicate-position residue is asserted VISIBLE;
//   * chain removal while owned: ownership and queued segments SURVIVE the edit (publish-
//     before-destroy + bounded worker pause; chain edits late-apply like other controls);
//   * Save capture window: adoption hold + gapless drain, state captured with zero lead at the
//     capture point, playback resumes gaplessly — the whole save is audibly a no-op;
//   * no instance is ever processed concurrently or destroyed/prepared while processing.
// Accounting honesty (model doc §10): "consumed + missed == total" alone proves nothing about
// audio correctness — the proof here is bit-identity plus the probes' recorded call sequences;
// counters are asserted as a SEPARATE, three-way ledger (consumed / missed / stale-discarded).
// Linux results verify LOGIC, AUDIO DATA and LIFETIME only — no claims about Windows/ASIO
// performance, real third-party plugins, or audible quality. Device-free, deterministic.
// =============================================================================

#include <JuceHeader.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>
#include <thread>
#include <vector>

#include "domain/AudioClip.h"
#include "domain/Session.h"
#include "domain/SessionSnapshot.h"
#include "engine/InstrumentRenderPool.h"
#include "engine/PlaybackEngine.h"
#include "engine/ReadAheadRenderer.h"
#include "instruments/InstrumentTrackController.h"
#include "plugins/PluginInsertHost.h"
#include "transport/Transport.h"

namespace
{
constexpr double kRate = 48000.0;
constexpr int kBlock = 512;
constexpr int kDepth = 3;

int checks = 0;
int failures = 0;

void expect(const bool ok, const char* what)
{
    ++checks;
    if (ok)
    {
        std::printf("[ ok ] %s\n", what);
    }
    else
    {
        ++failures;
        std::printf("[FAIL] %s\n", what);
    }
}

std::atomic<int> gOverlaps{ 0 };
std::atomic<int> gDestroyedWhileProcessing{ 0 };
std::atomic<int> gPreparedWhileProcessing{ 0 };

// ---------------------------------------------------------------------------------------------
// Deterministic, STATE-DEPENDENT test insert: a one-pole feedback makes the output depend on
// processing order/continuity; every processBlock call records the chain playhead position and
// size. Guards against concurrent processing / destruction of one instance.
// ---------------------------------------------------------------------------------------------
class StatefulProbeInsert final : public juce::AudioPluginInstance
{
public:
    struct Call
    {
        std::int64_t timeInSamples = -1;
        int numSamples = 0;
    };

    explicit StatefulProbeInsert(const float gain)
        : juce::AudioPluginInstance(BusesProperties()
                                        .withInput("In", juce::AudioChannelSet::stereo(), true)
                                        .withOutput("Out", juce::AudioChannelSet::stereo(), true))
        , gain_(gain)
    {
        calls_.resize(1 << 14);
    }
    ~StatefulProbeInsert() override
    {
        if (inProcess_.load(std::memory_order_acquire) != 0)
        {
            gDestroyedWhileProcessing.fetch_add(1, std::memory_order_relaxed);
        }
    }

    void fillInPluginDescription(juce::PluginDescription& d) const override
    {
        d.name = "StatefulProbe";
        d.pluginFormatName = "Test";
        d.isInstrument = false;
    }
    const juce::String getName() const override { return "StatefulProbe"; }
    void prepareToPlay(double, int) override
    {
        if (inProcess_.load(std::memory_order_acquire) != 0)
        {
            gPreparedWhileProcessing.fetch_add(1, std::memory_order_relaxed);
        }
    }
    void releaseResources() override
    {
        if (inProcess_.load(std::memory_order_acquire) != 0)
        {
            gPreparedWhileProcessing.fetch_add(1, std::memory_order_relaxed);
        }
    }
    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        if (inProcess_.exchange(1, std::memory_order_acq_rel) != 0)
        {
            gOverlaps.fetch_add(1, std::memory_order_relaxed);
        }
        Call c;
        c.numSamples = buffer.getNumSamples();
        if (juce::AudioPlayHead* const ph = getPlayHead())
        {
            if (const auto pos = ph->getPosition())
            {
                if (const auto t = pos->getTimeInSamples())
                {
                    c.timeInSamples = *t;
                }
            }
        }
        const int slot = callCount_.fetch_add(1, std::memory_order_acq_rel);
        if (slot < (int)calls_.size())
        {
            calls_[(size_t)slot] = c;
        }
        samplesProcessed_.fetch_add((std::uint64_t)(std::uint32_t)buffer.getNumSamples(),
                                    std::memory_order_relaxed);
        const int n = buffer.getNumSamples();
        for (int ch = 0; ch < juce::jmin(2, buffer.getNumChannels()); ++ch)
        {
            float* d = buffer.getWritePointer(ch);
            float z = state_[ch];
            for (int i = 0; i < n; ++i)
            {
                const float y = gain_ * d[i] + 0.3f * z;
                z = y;
                d[i] = y;
            }
            state_[ch] = z;
        }
        inProcess_.store(0, std::memory_order_release);
    }
    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    bool hasEditor() const override { return false; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}
    /// State = the probe's processed-history fingerprint. A save that captured at a wrong point
    /// (state lead, torn mid-process read) yields a different blob than the audible position.
    void getStateInformation(juce::MemoryBlock& dest) override
    {
        const std::uint64_t sp = samplesProcessed();
        const std::int64_t cc = (std::int64_t)callCount();
        dest.append(&sp, sizeof(sp));
        dest.append(&cc, sizeof(cc));
        dest.append(state_, sizeof(state_));
    }
    void setStateInformation(const void*, int) override {}

    [[nodiscard]] int callCount() const noexcept
    {
        return juce::jmin(callCount_.load(std::memory_order_acquire), (int)calls_.size());
    }
    [[nodiscard]] const Call& call(const int i) const noexcept { return calls_[(size_t)i]; }
    [[nodiscard]] std::uint64_t samplesProcessed() const noexcept
    {
        return samplesProcessed_.load(std::memory_order_acquire);
    }

private:
    const float gain_;
    std::atomic<int> inProcess_{ 0 };
    std::atomic<int> callCount_{ 0 };
    std::atomic<std::uint64_t> samplesProcessed_{ 0 };
    std::vector<Call> calls_;
    float state_[2] = { 0.0f, 0.0f };
};

class StubDevice final : public juce::AudioIODevice
{
public:
    StubDevice() : juce::AudioIODevice("StubDevice", "Stub") {}
    juce::StringArray getOutputChannelNames() override { return { "L", "R" }; }
    juce::StringArray getInputChannelNames() override { return {}; }
    juce::Array<double> getAvailableSampleRates() override { return { kRate }; }
    juce::Array<int> getAvailableBufferSizes() override { return { kBlock }; }
    int getDefaultBufferSize() override { return kBlock; }
    juce::String open(const juce::BigInteger&, const juce::BigInteger&, double, int) override { return {}; }
    void close() override {}
    bool isOpen() override { return true; }
    void start(juce::AudioIODeviceCallback*) override {}
    void stop() override {}
    bool isPlaying() override { return false; }
    juce::String getLastError() override { return {}; }
    int getCurrentBufferSizeSamples() override { return kBlock; }
    double getCurrentSampleRate() override { return kRate; }
    int getCurrentBitDepth() override { return 32; }
    juce::BigInteger getActiveOutputChannels() const override { juce::BigInteger b; b.setRange(0, 2, true); return b; }
    juce::BigInteger getActiveInputChannels() const override { return {}; }
    int getOutputLatencyInSamples() override { return 0; }
    int getInputLatencyInSamples() override { return 0; }
};

[[nodiscard]] std::shared_ptr<const AudioClip> makeNoiseClip(const std::uint32_t seed, const int numSamples)
{
    juce::AudioBuffer<float> buf(2, numSamples);
    std::uint32_t s = seed * 2654435761u + 12345u;
    for (int i = 0; i < numSamples; ++i)
    {
        for (int ch = 0; ch < 2; ++ch)
        {
            s = s * 1664525u + 1013904223u;
            const float v = ((float)(s >> 8) / (float)(1u << 24)) - 0.5f;
            buf.setSample(ch, i, 0.5f * v);
        }
    }
    return std::make_shared<const AudioClip>(std::move(buf), kRate,
                                             juce::String("ra-noise-") + juce::String((int)seed));
}

// ---------------------------------------------------------------------------------------------
// Harness: Session + Transport + PluginInsertHost + PlaybackEngine, "device" = the test calling
// the production callback. `readAheadDepth > 0` creates the PUMP-mode renderer BEFORE the device
// is prepared (same lifecycle as the CLI flag: engine construction time).
// ---------------------------------------------------------------------------------------------
struct Harness
{
    Session session;
    Transport transport;
    PluginInsertHost pluginHost;
    PlaybackEngine engine;
    StubDevice device;
    std::vector<TrackId> audioTids;

    explicit Harness(const int readAheadDepth) : engine(transport, session, nullptr, nullptr, &pluginHost)
    {
        if (readAheadDepth > 0)
        {
            engine.enableExperimentalReadAheadForTests(readAheadDepth);
        }
        // Production hook (Main.cpp): publish-before-destroy drains the callback AND pauses the
        // read-ahead worker (bounded) before retired instances are destroyed. Ownership and
        // queued segments SURVIVE the edit (model doc §8) — no reset here.
        pluginHost.setRealtimeDrainAfterPublish([this] {
            (void)engine.waitForAudioCallbackExit(250.0);
            engine.pauseReadAheadWorkerAfterChainPublish();
        });
    }

    TrackId addAudioTrackWithClip(const std::shared_ptr<const AudioClip>& material,
                                  const std::int64_t startSample = 0)
    {
        session.addTrack();
        const TrackId tid = session.getActiveTrackId();
        const auto res = session.addPlacedClipFromExistingMaterial(
            material, startSample, 0, material->getNumSamples(), tid);
        jassert(res.wasOk());
        juce::ignoreUnused(res);
        audioTids.push_back(tid);
        return tid;
    }

    void finishSetup()
    {
        session.setArrangementExtentSamples((std::int64_t)kRate * 120);
        engine.rebuildRoutingPlanFromSession();
        engine.audioDeviceAboutToStart(&device);
    }

    StatefulProbeInsert* installInsert(const TrackId tid, const InsertStage stage, const float gain)
    {
        auto fx = std::make_unique<StatefulProbeInsert>(gain);
        StatefulProbeInsert* const raw = fx.get();
        const bool ok = pluginHost.installInsertInstanceForTests(tid, stage, std::move(fx));
        jassert(ok);
        juce::ignoreUnused(ok);
        return raw;
    }

    /// Pump the renderer until it has no more work (bounded).
    void pumpUntilIdle()
    {
        readahead::ReadAheadRenderer* const ra = engine.experimentalReadAhead();
        for (int i = 0; ra != nullptr && i < 1024; ++i)
        {
            if (ra->testPumpWorkerOnce() == 0)
            {
                break;
            }
        }
    }

    /// Seek + Play, run `blocks` production callbacks (calling `afterBlock(blockIndex)` after
    /// each; transport commands issued there apply from the NEXT block, exactly like UI input
    /// between device callbacks), then request Stopped intent (production stop/Space semantics:
    /// the playhead KEEPS its position — stop-button additionally seeks, which tests issue
    /// explicitly) and run one stop-edge callback. Returns interleaved L/R of ALL `blocks`.
    template <typename AfterBlockFn>
    [[nodiscard]] std::vector<float> runPlayingBlocks(const int blocks, const std::int64_t startSample,
                                                      AfterBlockFn&& afterBlock)
    {
        transport.requestSeek(startSample);
        transport.requestPlaybackIntent(PlaybackIntent::Playing);
        std::vector<float> out;
        out.reserve((size_t)blocks * kBlock * 2);
        juce::AudioBuffer<float> blk(2, kBlock);
        float* ptrs[2] = { blk.getWritePointer(0), blk.getWritePointer(1) };
        juce::AudioIODeviceCallbackContext ctx;
        for (int b = 0; b < blocks; ++b)
        {
            blk.clear();
            engine.audioDeviceIOCallbackWithContext(nullptr, 0, ptrs, 2, kBlock, ctx);
            for (int i = 0; i < kBlock; ++i)
            {
                out.push_back(ptrs[0][i]);
                out.push_back(ptrs[1][i]);
            }
            afterBlock(b);
        }
        transport.requestPlaybackIntent(PlaybackIntent::Stopped);
        blk.clear();
        engine.audioDeviceIOCallbackWithContext(nullptr, 0, ptrs, 2, kBlock, ctx);
        return out;
    }

    [[nodiscard]] std::vector<float> runPlayingBlocksPumped(const int blocks,
                                                            const std::int64_t startSample = 0)
    {
        return runPlayingBlocks(blocks, startSample, [this](int) { pumpUntilIdle(); });
    }
};

/// Standard fixture: default empty audio row + 3 clip rows; row B carries the stateful probe
/// chain (Pre 0.8 + Post 0.9); row C fans a send to a Group (consume-time routing).
struct Fixture
{
    Harness h;
    TrackId groupTid = kInvalidTrackId;
    StatefulProbeInsert* pre = nullptr;
    StatefulProbeInsert* post = nullptr;

    explicit Fixture(const int readAheadDepth) : h(readAheadDepth)
    {
        constexpr int kClipLen = kBlock * 400;
        h.session.addGroupTrack();
        h.addAudioTrackWithClip(makeNoiseClip(7, kClipLen));
        h.addAudioTrackWithClip(makeNoiseClip(13, kClipLen));
        h.addAudioTrackWithClip(makeNoiseClip(29, kClipLen));
        {
            const auto snap = h.session.loadSessionSnapshotForAudioThread();
            for (int i = 0; snap != nullptr && i < snap->getNumTracks(); ++i)
            {
                if (snap->getTrack(i).getKind() == TrackKind::Group)
                {
                    groupTid = snap->getTrack(i).getId();
                }
            }
        }
        h.session.setTrackChannelFaderGain(h.audioTids[2], 0.6f);
        h.session.setTrackStereoPan(h.audioTids[2], 0.4f);
        const bool sent = h.session.insertTrackSend(h.audioTids[2], 0, groupTid, 0.4f);
        jassert(sent);
        juce::ignoreUnused(sent);
        h.session.setTrackChannelFaderGain(groupTid, 0.7f);
        h.finishSetup();
        pre = h.installInsert(h.audioTids[1], InsertStage::Pre, 0.8f);
        post = h.installInsert(h.audioTids[1], InsertStage::Post, 0.9f);
    }
};

[[nodiscard]] double maxAbsDiff(const std::vector<float>& a, const std::vector<float>& b)
{
    if (a.size() != b.size())
    {
        return 1.0e9;
    }
    double d = 0.0;
    for (size_t i = 0; i < a.size(); ++i)
    {
        d = std::max(d, std::fabs((double)a[i] - (double)b[i]));
    }
    return d;
}

[[nodiscard]] double peakOf(const std::vector<float>& v)
{
    double p = 0.0;
    for (const float x : v) { p = std::max(p, std::fabs((double)x)); }
    return p;
}

/// Peak over interleaved-stereo block range [fromBlock, toBlock).
[[nodiscard]] double peakOfBlockSpan(const std::vector<float>& v, const int fromBlock, const int toBlock)
{
    double p = 0.0;
    const size_t from = (size_t)fromBlock * kBlock * 2;
    const size_t to = std::min(v.size(), (size_t)toBlock * kBlock * 2);
    for (size_t i = from; i < to; ++i) { p = std::max(p, std::fabs((double)v[i])); }
    return p;
}

/// Positions form contiguous runs: each call starts where the previous ended, except at breaks
/// the caller allows. Returns the number of breaks (position jumps). Run-length aware, so split
/// cycle segments (two sub-block calls per device block) do NOT count as breaks.
[[nodiscard]] int countStreamBreaks(const StatefulProbeInsert* fx)
{
    int breaks = 0;
    for (int i = 1; i < fx->callCount(); ++i)
    {
        if (fx->call(i).timeInSamples
            != fx->call(i - 1).timeInSamples + fx->call(i - 1).numSamples)
        {
            ++breaks;
        }
    }
    return breaks;
}

[[nodiscard]] bool allCallsFullBlock(const StatefulProbeInsert* fx)
{
    for (int i = 0; i < fx->callCount(); ++i)
    {
        if (fx->call(i).numSamples != kBlock)
        {
            return false;
        }
    }
    return fx->callCount() > 0;
}

[[nodiscard]] int countCallsAtPosition(const StatefulProbeInsert* fx, const std::int64_t pos)
{
    int n = 0;
    for (int i = 0; i < fx->callCount(); ++i)
    {
        if (fx->call(i).timeInSamples == pos)
        {
            ++n;
        }
    }
    return n;
}

/// The reference probe's full call sequence (positions AND sizes) is an exact prefix of the
/// read-ahead probe's sequence. This is the strongest exactly-once statement available for
/// contiguous playback: the state-dependent chain saw the SAME work in the SAME order, the
/// read-ahead instance merely ran ahead by the bounded queue.
[[nodiscard]] bool probeCallsArePrefixOf(const StatefulProbeInsert* ref, const StatefulProbeInsert* ahead)
{
    if (ahead->callCount() < ref->callCount())
    {
        return false;
    }
    for (int i = 0; i < ref->callCount(); ++i)
    {
        if (ref->call(i).timeInSamples != ahead->call(i).timeInSamples
            || ref->call(i).numSamples != ahead->call(i).numSamples)
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] int audioRowCount(Harness& h)
{
    const auto snap = h.session.loadSessionSnapshotForAudioThread();
    int n = 0;
    for (int i = 0; snap != nullptr && i < snap->getNumTracks(); ++i)
    {
        if (snap->getTrack(i).getKind() == TrackKind::Audio)
        {
            ++n;
        }
    }
    return n;
}

void printCounters(const readahead::ReadAheadRenderer::Counters& c, const char* tag)
{
    std::printf("[info] %s: adopted=%lld produced=%lld consumed=%lld missed=%lld stale=%lld "
                "drainRel=%lld discardResets=%lld missAbandons=%lld\n",
                tag, (long long)c.adopted, (long long)c.producedSegments,
                (long long)c.consumedSegments, (long long)c.missedSegments,
                (long long)c.staleDiscarded, (long long)c.drainReleases,
                (long long)c.discardResets, (long long)c.missAbandons);
}

// =============================================================================================
void testStablePlaybackBitIdentity()
{
    std::printf("\n-- stable linear playback: flag-off vs read-ahead bit-identity, exactly-once stream --\n");
    constexpr int kBlocks = 48;

    Fixture off(0);
    expect(off.h.engine.experimentalReadAhead() == nullptr, "flag off: engine holds NO read-ahead object");
    const std::vector<float> outOff = off.h.runPlayingBlocksPumped(kBlocks);

    Fixture on(kDepth);
    readahead::ReadAheadRenderer* const ra = on.h.engine.experimentalReadAhead();
    expect(ra != nullptr && ra->depthBlocks() == kDepth, "pump-mode renderer created with depth 3");
    const std::vector<float> outOn = on.h.runPlayingBlocksPumped(kBlocks);
    const auto c = ra->countersSnapshot();
    printCounters(c, "counters");

    const int rows = audioRowCount(on.h);
    expect(peakOf(outOff) > 0.01, "fixture produces audible output");
    const double diff = maxAbsDiff(outOff, outOn);
    std::printf("[info] flag-off vs read-ahead max |diff| = %.3g (peak %.3f)\n", diff, peakOf(outOff));
    expect(diff == 0.0, "read-ahead output BIT-IDENTICAL to the direct path (adoption included)");

    expect(c.adopted == rows, "every eligible audio row adopted exactly once");
    expect(c.missedSegments == 0 && c.staleDiscarded == 0, "no misses, nothing discarded stale");
    expect(c.consumedSegments == (std::int64_t)rows * (kBlocks - 1),
           "every owned block after the adoption block consumed from the ring");
    expect(c.discardResets == 0 && c.drainReleases == 0,
           "STOP (intent only, playhead kept) is a continuation point: no reset, no drain");
    expect(ra->audioThread_anyOwned(), "rows stay owned across the stop (resume would continue the queue)");

    // Exactly-once CONTIGUOUS stream through the state-dependent chain — valid here because the
    // whole run is one contiguous audible stream (the claim is NOT made for deliberate jumps).
    for (StatefulProbeInsert* fx : { on.pre, on.post })
    {
        expect(allCallsFullBlock(fx), "chain always processed full blocks");
        expect(countStreamBreaks(fx) == 0, "chain stream contiguous across adoption (no dup/loss/reorder)");
        expect(fx->call(0).timeInSamples == 0, "chain stream starts at sample 0");
        expect(fx->callCount() >= kBlocks && fx->callCount() <= kBlocks + kDepth,
           "chain processed every played block, at most depth segments ahead at stop");
    }
    expect(gOverlaps.load() == 0 && gDestroyedWhileProcessing.load() == 0
               && gPreparedWhileProcessing.load() == 0,
           "no concurrent processing / destruction / prepare of an active instance");
}

// =============================================================================================
void testPauseResumeContinuation()
{
    std::printf("\n-- pause -> resume at the same position: queue CONTINUES (no discard, no re-feed) --\n");
    constexpr int kBlocks = 24;
    constexpr int kPauseAfter = 9;   // Paused intent issued after block 9 -> blocks 10..13 paused
    constexpr int kResumeAfter = 13; // Playing intent issued after block 13 -> playing from 14
    const auto driver = [](Harness& h) {
        return [&h](const int b) {
            h.pumpUntilIdle();
            if (b == kPauseAfter)
            {
                h.transport.requestPlaybackIntent(PlaybackIntent::Paused);
            }
            if (b == kResumeAfter)
            {
                h.transport.requestPlaybackIntent(PlaybackIntent::Playing);
            }
        };
    };

    Fixture off(0);
    const std::vector<float> outOff = off.h.runPlayingBlocks(kBlocks, 0, driver(off.h));

    Fixture on(kDepth);
    const std::vector<float> outOn = on.h.runPlayingBlocks(kBlocks, 0, driver(on.h));
    const auto c = on.h.engine.experimentalReadAhead()->countersSnapshot();
    printCounters(c, "pause/resume counters");
    const int rows = audioRowCount(on.h);

    expect(maxAbsDiff(outOff, outOn) == 0.0,
           "pause + resume output BIT-IDENTICAL to the direct path (paused blocks silent in both)");
    expect(c.adopted == rows, "adopted ONCE — the pause did not re-adopt");
    expect(c.discardResets == 0 && c.staleDiscarded == 0,
           "nothing discarded: the queue rode through the pause untouched");
    expect(c.missedSegments == 0, "no misses at the resume edge (v1 re-fed and re-missed here)");
    constexpr int kPlayingBlocks = (kPauseAfter + 1) + (kBlocks - (kResumeAfter + 1)); // 10 + 10
    expect(c.consumedSegments == (std::int64_t)rows * (kPlayingBlocks - 1),
           "every playing block after adoption consumed; paused blocks consumed nothing");
    expect(countStreamBreaks(on.pre) == 0,
           "chain stream contiguous across pause/resume — ZERO duplicate positions (the v1 defect)");
    expect(on.pre->callCount() <= kPlayingBlocks + kDepth, "state lead still bounded by depth");
    expect(gOverlaps.load() == 0, "no concurrent processing");
}

// =============================================================================================
void testPlaybackOffsetDomain()
{
    std::printf("\n-- persistent playback offset: audible-domain queue keys (v1 missed 100%% here) --\n");
    constexpr int kBlocks = 24;
    constexpr std::int64_t kShift = (std::int64_t)kBlock * 3;

    Fixture off(0);
    off.h.engine.setPlaybackOffsetSamples(kShift);
    const std::vector<float> outOff = off.h.runPlayingBlocksPumped(kBlocks);

    Fixture on(kDepth);
    on.h.engine.setPlaybackOffsetSamples(kShift);
    const std::vector<float> outOn = on.h.runPlayingBlocksPumped(kBlocks);
    const auto c = on.h.engine.experimentalReadAhead()->countersSnapshot();
    printCounters(c, "offset counters");
    const int rows = audioRowCount(on.h);

    expect(peakOf(outOff) > 0.01, "offset fixture produces audible output");
    expect(maxAbsDiff(outOff, outOn) == 0.0,
           "output BIT-IDENTICAL with a non-zero persistent playback offset");
    expect(c.adopted == rows && c.missedSegments == 0,
           "adoption worked and every segment HIT under the offset (keys share one domain)");
    expect(c.consumedSegments == (std::int64_t)rows * (kBlocks - 1), "full consumption under the offset");
    expect(countStreamBreaks(on.pre) == 0, "chain stream contiguous under the offset");
}

// =============================================================================================
void testStopButtonReplayResidue()
{
    std::printf("\n-- stop-button jump (stop + seek 0) -> replay: discard + documented visible residue --\n");
    constexpr int kBlocks = 12;

    Fixture on(kDepth);
    (void)on.h.runPlayingBlocksPumped(kBlocks);
    const int callsAfterFirstRun = on.pre->callCount();
    expect(callsAfterFirstRun >= kBlocks && callsAfterFirstRun <= kBlocks + kDepth,
           "first run: chain state advanced at most depth segments past the stop");

    // The replay helper seeks to 0 — together with the Stopped intent above this is exactly the
    // production stop-button (requestPlaybackIntent(Stopped) + requestSeek(0)).
    (void)on.h.runPlayingBlocksPumped(kBlocks);
    const auto c = on.h.engine.experimentalReadAhead()->countersSnapshot();
    printCounters(c, "stop-button counters");
    const int rows = audioRowCount(on.h);

    expect(countCallsAtPosition(on.pre, 0) == 2,
           "replay re-fed position 0 to a chain whose state was ahead — the DOCUMENTED residue, "
           "visible (the direct path also re-feeds position 0 after the jump)");
    expect(countStreamBreaks(on.pre) == 1, "exactly one stream break: the deliberate jump");
    expect(c.discardResets == rows, "the jump performed exactly one discard reset per owned row");
    expect(c.staleDiscarded == (std::int64_t)rows * kDepth,
           "the full ring of every row was discarded UNPLAYED at the jump (counted residue)");
    expect(c.adopted == (std::int64_t)rows * 2, "rows re-adopted once the stream was contiguous again");
    expect(c.missedSegments == 0, "a deliberate jump is not a miss — nothing counted as underrun");
    expect(gOverlaps.load() == 0 && gDestroyedWhileProcessing.load() == 0, "lifetime guards clean");
}

// =============================================================================================
void testSeekWhileAhead()
{
    std::printf("\n-- seek while ahead: discard reset, re-adoption, stream restarts AT the target --\n");
    constexpr int kBlocks = 24;
    constexpr int kSeekAfterBlock = 9;
    constexpr std::int64_t kSeekTarget = (std::int64_t)kBlock * 100;

    Fixture on(kDepth);
    const std::vector<float> out = on.h.runPlayingBlocks(kBlocks, 0, [&](const int b) {
        on.h.pumpUntilIdle();
        if (b == kSeekAfterBlock)
        {
            on.h.transport.requestSeek(kSeekTarget);
        }
    });
    juce::ignoreUnused(out);
    const auto c = on.h.engine.experimentalReadAhead()->countersSnapshot();
    printCounters(c, "seek counters");
    const int rows = audioRowCount(on.h);

    expect(c.discardResets == rows, "the seek performed exactly one discard reset per owned row");
    expect(c.staleDiscarded == (std::int64_t)rows * kDepth, "every queued segment discarded unplayed");
    expect(c.adopted == (std::int64_t)rows * 2, "rows re-adopted after the seek");
    expect(c.missedSegments == 0, "the deliberate jump never counted as a miss");
    expect(countStreamBreaks(on.pre) == 1, "exactly one stream break (the seek)");
    bool segment2StartsAtTarget = false;
    for (int i = 1; i < on.pre->callCount(); ++i)
    {
        if (on.pre->call(i).timeInSamples
            != on.pre->call(i - 1).timeInSamples + on.pre->call(i - 1).numSamples)
        {
            segment2StartsAtTarget = on.pre->call(i).timeInSamples == kSeekTarget;
            break;
        }
    }
    expect(segment2StartsAtTarget, "post-seek stream starts exactly at the seek target");
    expect(gOverlaps.load() == 0, "no concurrent processing across the reset");
}

// =============================================================================================
void testCycleBitIdentity()
{
    std::printf("\n-- cycle: worker replicates the engine's wrap segmentation — bit-identity across passes --\n");
    constexpr int kBlocks = 80;
    // Deliberately NOT block-aligned: wraps split device blocks into two sub-block segments.
    constexpr std::int64_t kLocL = (std::int64_t)kBlock * 5 + 37;
    constexpr std::int64_t kLocR = (std::int64_t)kBlock * 13 + 211;

    const auto build = [](Fixture& f) {
        f.h.session.setLeftLocatorAtSample(kLocL);
        f.h.session.setRightLocatorAtSample(kLocR);
        f.h.transport.requestCycleEnabled(true);
    };

    Fixture off(0);
    build(off);
    const std::vector<float> outOff = off.h.runPlayingBlocksPumped(kBlocks);

    Fixture on(kDepth);
    build(on);
    const std::vector<float> outOn = on.h.runPlayingBlocksPumped(kBlocks);
    const auto c = on.h.engine.experimentalReadAhead()->countersSnapshot();
    printCounters(c, "cycle counters");
    const int rows = audioRowCount(on.h);

    expect(peakOf(outOff) > 0.01, "cycle fixture produces audible output");
    expect(maxAbsDiff(outOff, outOn) == 0.0,
           "cycle playback BIT-IDENTICAL across ~9 loop passes (wrap-splitting segments included)");
    expect(c.adopted == rows, "adopted once — a loop WRAP is contiguous playback, never a reset");
    expect(c.discardResets == 0, "no discard across any wrap (prediction matched the engine exactly)");
    expect(c.missedSegments == 0 && c.staleDiscarded == 0,
           "every segment of every pass HIT by key — loop passes distinguished by sequence, not position");
    expect(c.consumedSegments >= (std::int64_t)rows * (kBlocks - 1),
           "wrap blocks consumed as two segments each (count >= one per block)");
    expect(probeCallsArePrefixOf(off.pre, on.pre)
               && on.pre->callCount() - off.pre->callCount() <= kDepth,
           "direct path's call sequence is an exact prefix of the read-ahead sequence (positions + sizes)");
    expect(gOverlaps.load() == 0, "no concurrent processing across wraps");
}

// =============================================================================================
void testShortLoopLinearEscape()
{
    std::printf("\n-- short loop (span < block): the 'land at R, go linear' production quirk replicated --\n");
    constexpr int kBlocks = 20;
    constexpr std::int64_t kLocL = (std::int64_t)kBlock * 2 + 100; // 1124
    constexpr std::int64_t kLocR = kLocL + 350;                    // span 350 < kBlock

    const auto build = [](Fixture& f) {
        f.h.session.setLeftLocatorAtSample(kLocL);
        f.h.session.setRightLocatorAtSample(kLocR);
        f.h.transport.requestCycleEnabled(true);
    };

    Fixture off(0);
    build(off);
    const std::vector<float> outOff = off.h.runPlayingBlocksPumped(kBlocks);

    Fixture on(kDepth);
    build(on);
    const std::vector<float> outOn = on.h.runPlayingBlocksPumped(kBlocks);
    const auto c = on.h.engine.experimentalReadAhead()->countersSnapshot();
    printCounters(c, "short-loop counters");

    // The production engine wraps at most once per block; when the loop span is shorter than the
    // rest of the block the playhead lands exactly at R and playback continues LINEAR (a known
    // engine quirk the worker must REPLICATE, not fix — divergence here would show up as a
    // discard reset storm and a bit-diff).
    expect(maxAbsDiff(outOff, outOn) == 0.0, "short-loop output BIT-IDENTICAL (quirk replicated)");
    expect(c.discardResets == 0, "no prediction divergence at the linear escape");
    expect(c.missedSegments == 0, "no misses through the truncated wrap block");
    expect(probeCallsArePrefixOf(off.pre, on.pre)
               && on.pre->callCount() - off.pre->callCount() <= kDepth,
           "call sequences identical through the quirk (sizes included)");
}

// =============================================================================================
void testCycleGeometryChangeWhileOwned()
{
    std::printf("\n-- cycle toggle / locator move while owned: deliberate discontinuity, bounded + counted --\n");
    constexpr int kBlocks = 30;
    constexpr std::int64_t kLocL = (std::int64_t)kBlock * 10;
    constexpr std::int64_t kLocR = (std::int64_t)kBlock * 20;

    Fixture on(kDepth);
    on.h.session.setLeftLocatorAtSample(kLocL);
    on.h.session.setRightLocatorAtSample(kLocR); // locators set; cycle still OFF
    const std::vector<float> out = on.h.runPlayingBlocks(kBlocks, 0, [&](const int b) {
        on.h.pumpUntilIdle();
        if (b == 7)
        {
            on.h.transport.requestCycleEnabled(true); // geometry event 1 (visible at block 8)
        }
        if (b == 17)
        {
            on.h.session.setRightLocatorAtSample(kLocR + kBlock); // geometry event 2 (block 18)
        }
    });
    const auto c = on.h.engine.experimentalReadAhead()->countersSnapshot();
    printCounters(c, "geometry counters");
    const int rows = audioRowCount(on.h);

    expect(c.discardResets == (std::int64_t)rows * 2,
           "each geometry edit performed exactly one discard reset per owned row — and nothing else did");
    expect(c.adopted == (std::int64_t)rows * 3, "rows re-adopted after each geometry edit");
    expect(c.missAbandons == 0 && c.missedSegments == 0,
           "geometry edits are deliberate discontinuities, never counted as misses/overload");
    expect(peakOfBlockSpan(out, 8, kBlocks) > 0.01,
           "playback stayed audible through both edits (live path covered the reset blocks)");
    expect(gOverlaps.load() == 0 && gDestroyedWhileProcessing.load() == 0, "guards clean");
}

// =============================================================================================
void testDelayedWorkerMissAbandon()
{
    std::printf("\n-- worker never runs: 2 consecutive missed segments ABANDON the row to the live path --\n");
    constexpr int kBlocks = 16;

    Fixture on(kDepth);
    // NEVER pump: the worker is infinitely late. Block 0 renders live (Scheduled); blocks 1 and
    // 2 miss (counted silence); at block 3's begin the rows LEAVE the mode (abandon + cooldown)
    // and render live again — the system does NOT hide a continued run of silent blocks.
    const std::vector<float> out = on.h.runPlayingBlocks(kBlocks, 0, [](int) {});
    const auto c = on.h.engine.experimentalReadAhead()->countersSnapshot();
    printCounters(c, "miss-abandon counters");
    const int rows = audioRowCount(on.h);

    expect(peakOfBlockSpan(out, 0, 1) > 0.01, "adoption block (rendered live) is audible");
    expect(peakOfBlockSpan(out, 1, 3) == 0.0,
           "exactly the 2 tolerated miss blocks are EXACT silence (miss = silence, never a wait)");
    expect(peakOfBlockSpan(out, 3, kBlocks) > 0.01,
           "audible again from block 3: the rows abandoned the mode instead of staying silent");
    expect(c.missAbandons == rows, "every owned row counted exactly one miss abandonment");
    expect(c.missedSegments == (std::int64_t)rows * 2,
           "exactly the threshold's worth of misses counted — real underruns, not transition gaps");
    expect(c.adopted == rows, "cooldown blocked re-adoption for the rest of the run");
    expect(c.producedSegments == 0 && c.consumedSegments == 0, "nothing produced or consumed (worker never ran)");
    expect(on.pre->callCount() == kBlocks - 2,
           "chain processed the adoption block + every live block after the abandon (2 miss blocks lost)");
    expect(countStreamBreaks(on.pre) == 1, "one break: the 2-block hole the abandon closed");
    expect(!on.h.engine.experimentalReadAhead()->audioThread_anyOwned(),
           "no row still owned — the mode was LEFT when it did not hold");
}

// =============================================================================================
void testTransientSingleMissRecovery()
{
    std::printf("\n-- transient single miss: stale discard of the late result, ownership retained --\n");
    constexpr int kBlocks = 24;
    // Pump normally through block 9 (ring then covers 10..12), skip pumps after 10..12, resume
    // from block 13's pump: exactly ONE miss (block 13), then the late segment 13 is discarded
    // STALE (its audible time has passed — it is never played from the wrong position).
    Fixture on(kDepth);
    const std::vector<float> out = on.h.runPlayingBlocks(kBlocks, 0, [&](const int b) {
        if (b < 10 || b >= 13)
        {
            on.h.pumpUntilIdle();
        }
    });
    const auto c = on.h.engine.experimentalReadAhead()->countersSnapshot();
    printCounters(c, "transient-miss counters");
    const int rows = audioRowCount(on.h);

    expect(c.missedSegments == rows, "exactly one miss per owned row (block 13)");
    expect(c.staleDiscarded == rows,
           "the late block-13 segment discarded UNPLAYED per row — never audio from a wrong time");
    expect(c.missAbandons == 0 && c.discardResets == 0,
           "one miss is tolerated: no abandon, no reset, ownership retained");
    expect(peakOfBlockSpan(out, 13, 14) == 0.0, "the missed block is exact silence");
    expect(peakOfBlockSpan(out, 14, kBlocks) > 0.01, "ring hits resume immediately after");
    expect(countStreamBreaks(on.pre) == 0,
           "the chain's input stream stayed contiguous through miss + stale discard (state aligned)");
    expect(on.h.engine.experimentalReadAhead()->audioThread_anyOwned(), "rows still owned at the end");
    expect(gOverlaps.load() == 0, "no concurrent processing");
}

// =============================================================================================
void testMonitorImmediateHandover()
{
    std::printf("\n-- Monitor enabled mid-run: IMMEDIATE discard handover, zero monitor-onset delay --\n");
    constexpr int kBlocks = 16;
    constexpr int kEnableAfterBlock = 8;

    Fixture on(kDepth);
    const TrackId monitoredTid = on.h.audioTids[1]; // the probe-chain row
    const std::vector<float> out = on.h.runPlayingBlocks(kBlocks, 0, [&](const int b) {
        on.h.pumpUntilIdle();
        if (b == kEnableAfterBlock)
        {
            on.h.engine.setTrackInputMonitoringEnabled(monitoredTid, true);
        }
    });
    juce::ignoreUnused(out);
    const auto c = on.h.engine.experimentalReadAhead()->countersSnapshot();
    printCounters(c, "monitor counters");

    // The direct path switches semantics the SAME block monitoring appears (clip playback
    // suppressed, live input passes): a gapless drain would both over-play queued clip audio
    // and delay the input onset by <= depth. The model therefore DISCARDS: handover at block 9,
    // monitor pass processes the chain from block 9's position immediately.
    expect(c.discardResets == 1, "exactly one discard reset: the monitored row's immediate handover");
    expect(c.drainReleases == 0, "no drain — monitored audio must not be delayed by the queue");
    expect(c.staleDiscarded == kDepth, "the monitored row's full ring discarded unplayed (counted)");
    expect(c.missedSegments == 0, "the handover is not a miss — no underrun counted");
    expect(countCallsAtPosition(on.pre, (std::int64_t)kBlock * (kEnableAfterBlock + 1)) == 2,
           "zero monitor-onset delay: the monitor pass processed block 9's position the same block "
           "(the duplicate is the documented, bounded state residue — asserted VISIBLE, not hidden)");
    expect(countCallsAtPosition(on.pre, (std::int64_t)kBlock * (kEnableAfterBlock + kDepth)) == 2,
           "the residue is exactly the queued depth (last queued position also appears twice)");
    // Consumed stream 0..8 (9 calls) + depth ahead (3) + monitor passes 9..15 (7) + the stop-edge
    // callback (monitoring processes while stopped — today's behavior, untouched): 20 calls.
    expect(on.pre->callCount() == kBlocks + kDepth + 1,
           "call ledger matches: consumed + bounded residue + monitor passes, nothing lost");
    expect(allCallsFullBlock(on.pre), "monitoring processes full blocks too");
    expect(gOverlaps.load() == 0, "worker and monitor pass never touched the chain concurrently");
}

// =============================================================================================
void testChainRemovalWhileOwned()
{
    std::printf("\n-- insert chain removed while owned: ownership and queue SURVIVE the edit --\n");
    constexpr int kBlocks = 24;
    constexpr int kRemoveAfterBlock = 8;

    Fixture on(kDepth);
    const TrackId editedTid = on.h.audioTids[1];
    const std::vector<float> out = on.h.runPlayingBlocks(kBlocks, 0, [&](const int b) {
        on.h.pumpUntilIdle();
        if (b == kRemoveAfterBlock)
        {
            // Message-thread chain edit: publish -> drain hook (callback exit + bounded worker
            // pause) -> destroy. The probe pointers are DANGLING after this call. The queued
            // segments were rendered with the OLD chain and still play (chain edits late-apply
            // by <= depth, like every other control); ownership is NOT reset.
            on.h.pluginHost.removePlugin(editedTid);
            on.pre = nullptr;
            on.post = nullptr;
        }
    });
    const auto c = on.h.engine.experimentalReadAhead()->countersSnapshot();
    printCounters(c, "chain-edit counters");
    const int rows = audioRowCount(on.h);

    expect(gDestroyedWhileProcessing.load() == 0,
           "no instance destroyed while processing (publish-before-destroy + bounded pause held)");
    expect(gOverlaps.load() == 0 && gPreparedWhileProcessing.load() == 0, "concurrency guards clean");
    expect(c.discardResets == 0, "chain edit forced NO reset — ownership survived (v2 improvement)");
    expect(c.adopted == rows, "no re-adoption needed: the rows never left the mode");
    expect(c.missedSegments == 0, "no miss at the edit: the queue bridged the publish window");
    expect(peakOfBlockSpan(out, kRemoveAfterBlock + 2, kBlocks) > 0.01,
           "playback continued after the edit (dry row data still fans)");
}

// =============================================================================================
void testSaveCaptureWindow()
{
    std::printf("\n-- save capture window: hold + gapless drain, zero state lead at capture, gapless resume --\n");
    constexpr int kBlocks = 20;
    constexpr int kHoldAfterBlock = 8;
    // Ring holds segments 9..11 at the hold; blocks 9..11 consume them; block 12's begin releases
    // the drained rows (live render from 12) -> capture there; release the hold after block 12.
    constexpr int kCaptureAtBlock = 12;

    Fixture off(0);
    const std::vector<float> outOff = off.h.runPlayingBlocksPumped(kBlocks);

    Fixture on(kDepth);
    readahead::ReadAheadRenderer* const ra = on.h.engine.experimentalReadAhead();
    bool drainedAtCapture = false;
    bool stateLeadZeroAtCapture = false;
    juce::MemoryBlock capturedState;
    const std::vector<float> outOn = on.h.runPlayingBlocks(kBlocks, 0, [&](const int b) {
        on.h.pumpUntilIdle();
        if (b == kHoldAfterBlock)
        {
            // Renderer-level window (the engine-level wrapper's bounded drain WAIT needs real
            // callbacks to proceed, which a single-threaded pump test cannot run while waiting —
            // the wrapper's non-playing fast path is smoke-tested below).
            ra->beginStateCaptureHold();
        }
        if (b == kCaptureAtBlock)
        {
            drainedAtCapture = !ra->audioThread_anyOwned();
            // Zero state lead: the chain processed EXACTLY the audibly consumed stream — blocks
            // 0..12 inclusive, nothing ahead. getStateInformation here is the production-path
            // concurrency class (worker paused, callback-only processing).
            ra->pauseWorkerAndWait();
            stateLeadZeroAtCapture = on.pre->callCount() == kCaptureAtBlock + 1
                                     && on.pre->samplesProcessed()
                                            == (std::uint64_t)(kCaptureAtBlock + 1) * kBlock;
            on.pre->getStateInformation(capturedState);
            ra->resumeWorker();
            ra->endStateCaptureHold();
        }
    });
    const auto c = ra->countersSnapshot();
    printCounters(c, "save-window counters");
    const int rows = audioRowCount(on.h);

    expect(drainedAtCapture, "every owned row gaplessly drained within depth+1 blocks of the hold");
    expect(stateLeadZeroAtCapture,
           "ZERO state lead at the capture point: captured state == the audible position");
    expect(capturedState.getSize() > 0, "state captured inside the window");
    expect(c.drainReleases == rows, "all rows released by gapless drain (nothing discarded)");
    expect(c.discardResets == 0 && c.staleDiscarded == 0 && c.missedSegments == 0,
           "the save window discarded nothing and missed nothing");
    expect(c.adopted == (std::int64_t)rows * 2, "rows re-adopted after the hold was released");
    expect(countStreamBreaks(on.pre) == 0,
           "chain stream contiguous through hold -> drain -> capture -> resume -> re-adoption");
    expect(maxAbsDiff(outOff, outOn) == 0.0,
           "the whole save window is audibly a NO-OP: output bit-identical to an undisturbed run");

    // Engine-level wrapper smoke: with a non-playing transport the capture window must return
    // promptly (no drain wait — nothing consumes while stopped; model doc §9).
    const auto tStart = std::chrono::steady_clock::now();
    on.h.engine.beginPluginStateCaptureWindow();
    on.h.engine.endPluginStateCaptureWindow();
    const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now() - tStart)
                               .count();
    expect(elapsedMs < 400, "non-playing engine-level capture window returns promptly (no 500ms wait)");
}

// =============================================================================================
void testWorkerThreadSmoke()
{
    std::printf("\n-- real worker THREAD smoke run (timing non-deterministic; guards + sanity only) --\n");
    constexpr int kBlocks = 80;

    // Thread-mode renderer via the CLI-flag path: configure before the engine is constructed.
    readahead::setConfiguredReadAheadDepth(kDepth);
    Fixture on(0); // Harness(0) skips the pump-mode seam; the engine ctor read the global config
    readahead::setConfiguredReadAheadDepth(0);
    readahead::ReadAheadRenderer* const ra = on.h.engine.experimentalReadAhead();
    expect(ra != nullptr, "engine constructed the thread-mode renderer from the configured depth");

    const std::vector<float> out = on.h.runPlayingBlocks(kBlocks, 0, [](int) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    });
    const auto c = ra->countersSnapshot();
    printCounters(c, "thread-mode counters");
    const int rows = audioRowCount(on.h);

    // Timing-dependent: misses, abandons and re-adoptions may legitimately occur; the exact
    // "every block is one hit or one miss" ledger holds only per owned stretch, which this test
    // cannot pin down — it asserts production, consumption, bounded accounting and lifetime.
    expect(peakOf(out) > 0.01, "audible output with the real worker thread");
    expect(c.adopted >= rows, "rows adopted at least once");
    expect(c.producedSegments > 0 && c.consumedSegments > 0,
           "the worker thread produced and the callback consumed");
    expect(c.consumedSegments <= c.producedSegments, "nothing consumed that was never produced");
    expect(gOverlaps.load() == 0 && gDestroyedWhileProcessing.load() == 0
               && gPreparedWhileProcessing.load() == 0,
           "no concurrent processing / lifetime violations under the real thread");
}

} // namespace

int main()
{
    std::setvbuf(stdout, nullptr, _IONBF, 0); // progress visible even when a hang has to be killed
    juce::ScopedJuceInitialiser_GUI juceInit; // MessageManager: prepareForDevice requires the message thread
    std::printf("ReadAheadPrototypeFocusedTests (model: docs/READAHEAD_PROTOTYPE.md, v2)\n");

    testStablePlaybackBitIdentity();
    testPauseResumeContinuation();
    testPlaybackOffsetDomain();
    testStopButtonReplayResidue();
    testSeekWhileAhead();
    testCycleBitIdentity();
    testShortLoopLinearEscape();
    testCycleGeometryChangeWhileOwned();
    testDelayedWorkerMissAbandon();
    testTransientSingleMissRecovery();
    testMonitorImmediateHandover();
    testChainRemovalWhileOwned();
    testSaveCaptureWindow();
    testWorkerThreadSmoke();

    std::printf("\n%d checks, %d failure(s)\n", checks, failures);
    return failures == 0 ? 0 : 1;
}

// Link seams — PlaybackEngine.cpp references InstrumentTrackController entry points this harness
// never exercises (no instrument rows are published).
ProjectFileExperimentalInstrumentTrackV1 InstrumentTrackController::buildExperimentalInstrumentProjectBlock() const
{
    jassertfalse;
    return {};
}
void InstrumentTrackController::audioThread_scheduleTransportMidiForSegment(ExperimentalInstrumentHost&, std::int64_t, int,
                                                                            int, bool, int, int*, bool, bool) noexcept {}
void InstrumentTrackController::audioThread_flushTransportMidi(ExperimentalInstrumentHost&, int, int) noexcept {}
void InstrumentTrackController::audioThread_flushPendingTransportOffsInto(ExperimentalInstrumentHost&, int, int) noexcept {}
