// =============================================================================
// ReadAheadPrototypeFocusedTests — experimental audio-row read-ahead (Stage B prototype)
// =============================================================================
// Drives the PRODUCTION PlaybackEngine callback (stub device geometry) with the read-ahead
// renderer in deterministic PUMP mode (no worker thread; the test IS the worker), plus one real
// worker-thread smoke run. The test insert is STATE-DEPENDENT (one-pole feedback) and records the
// playhead position of every processBlock call, so misplaced internal time changes both the output
// and the recorded position sequence. Verifies ONLY the model's claims (docs/READAHEAD_PROTOTYPE.md):
//   * flag off = no renderer object = unchanged engine (covered by AudioStripParallelFocusedTests;
//     here the flag-off harness doubles as the bit-identity reference);
//   * stable linear playback: output BIT-IDENTICAL to the direct path, adoption included; the
//     insert instance sees an exactly-once, contiguous sample stream across the adoption;
//   * a deliberately delayed worker: owned rows are SILENT for missed blocks (counted), ownership
//     is kept, late results are discarded as stale, and the stream stays exactly-once;
//   * stop -> replay from the same position: the documented <= depth re-feed residue is VISIBLE
//     (duplicate position 0 calls) — asserted as documented, not hidden;
//   * seek while ahead: discard reset, re-adoption, per-segment contiguous streams;
//   * Monitor enabled mid-run: gapless draining release, monitoring onset delayed <= depth blocks;
//   * chain removal while owned: publish-before-destroy + worker quiesce (no touched freed memory);
//   * cycle active: adoption gated off entirely (bit-identical to flag-off cycle playback);
//   * no instance is ever processed concurrently or destroyed/prepared while processing.
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
    void getStateInformation(juce::MemoryBlock&) override {}
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
        // Production hook (Main.cpp): publish-before-destroy drains the callback AND quiesces the
        // read-ahead worker before retired instances are destroyed.
        pluginHost.setRealtimeDrainAfterPublish([this] {
            (void)engine.waitForAudioCallbackExit(250.0);
            engine.quiesceReadAheadForExclusiveChainAccess();
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
    /// each), Stop (+ one stop-edge callback). Returns interleaved L/R output of the play blocks.
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

[[nodiscard]] double peakOfRange(const std::vector<float>& v, const size_t from)
{
    double p = 0.0;
    for (size_t i = from; i < v.size(); ++i) { p = std::max(p, std::fabs((double)v[i])); }
    return p;
}

/// Positions form contiguous full-block runs: each call advances by exactly kBlock, except at
/// breaks the caller allows. Returns the number of breaks (position jumps).
[[nodiscard]] int countStreamBreaks(const StatefulProbeInsert* fx)
{
    int breaks = 0;
    for (int i = 1; i < fx->callCount(); ++i)
    {
        if (fx->call(i).timeInSamples != fx->call(i - 1).timeInSamples + kBlock)
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
    std::printf("[info] counters: adopted=%lld produced=%lld consumed=%lld missed=%lld stale=%lld "
                "drainRel=%lld discardResets=%lld\n",
                (long long)c.adopted, (long long)c.producedBlocks, (long long)c.consumedBlocks,
                (long long)c.missedBlocks, (long long)c.staleDiscarded, (long long)c.drainReleases,
                (long long)c.discardResets);

    const int rows = audioRowCount(on.h);
    expect(peakOf(outOff) > 0.01, "fixture produces audible output");
    const double diff = maxAbsDiff(outOff, outOn);
    std::printf("[info] flag-off vs read-ahead max |diff| = %.3g (peak %.3f)\n", diff, peakOf(outOff));
    expect(diff == 0.0, "read-ahead output BIT-IDENTICAL to the direct path (adoption included)");

    expect(c.adopted == rows, "every eligible audio row adopted exactly once");
    expect(c.missedBlocks == 0, "no misses during stable pumped playback");
    expect(c.consumedBlocks == (std::int64_t)rows * (kBlocks - 1),
           "every owned block after the adoption block consumed from the ring");
    expect(c.discardResets == rows, "stop performed exactly one discard reset per owned row");

    // Exactly-once contiguous stream through the state-dependent chain: the worker processed the
    // SAME contiguous positions the live path would have (plus <= depth discarded-at-stop blocks
    // whose OUTPUT never played but whose state advance is the documented residue).
    for (StatefulProbeInsert* fx : { on.pre, on.post })
    {
        expect(allCallsFullBlock(fx), "chain always processed full blocks");
        expect(countStreamBreaks(fx) == 0, "chain stream contiguous across adoption (no dup/loss/reorder)");
        expect(fx->call(0).timeInSamples == 0, "chain stream starts at sample 0");
        expect(fx->callCount() >= kBlocks && fx->callCount() <= kBlocks + kDepth,
               "chain processed every played block, at most depth blocks ahead at stop");
    }
    expect(gOverlaps.load() == 0 && gDestroyedWhileProcessing.load() == 0
               && gPreparedWhileProcessing.load() == 0,
           "no concurrent processing / destruction / prepare of an active instance");
}

// =============================================================================================
void testDelayedWorkerMisses()
{
    std::printf("\n-- deliberately delayed worker: silence at miss, counted, ownership retained --\n");
    constexpr int kBlocks = 16;

    Fixture on(kDepth);
    // NEVER pump: the worker is infinitely late. Block 0 renders live (Scheduled); every owned
    // block after that misses -> the owned rows are silent and the only sources ARE those rows.
    const std::vector<float> out = on.h.runPlayingBlocks(kBlocks, 0, [](int) {});
    const auto c = on.h.engine.experimentalReadAhead()->countersSnapshot();
    const int rows = audioRowCount(on.h);

    expect(peakOf(out) > 0.01, "adoption block (rendered live) is audible");
    expect(peakOfRange(out, (size_t)kBlock * 2) == 0.0,
           "every block after adoption is EXACT silence (miss is silence, not a wait)");
    expect(c.adopted == rows, "rows adopted once and ownership retained through the misses");
    expect(c.missedBlocks == (std::int64_t)rows * (kBlocks - 1),
           "every post-adoption block counted as a miss for every owned row");
    expect(c.producedBlocks == 0 && c.consumedBlocks == 0, "nothing produced or consumed (worker never ran)");
    expect(on.pre->callCount() == 1 && on.pre->call(0).timeInSamples == 0,
           "state-dependent chain processed ONLY the adoption block (stream never re-rendered live)");
}

// =============================================================================================
void testStaleDiscardAndRecovery()
{
    std::printf("\n-- worker falls behind, then catches up: misses, stale discards, clean recovery --\n");
    constexpr int kBlocks = 24;
    constexpr int kPumpStop = 10; // pump after blocks 0..9, skip 10..13, resume 14..
    constexpr int kPumpResume = 14;

    Fixture on(kDepth);
    const std::vector<float> out = on.h.runPlayingBlocks(kBlocks, 0, [&](const int b) {
        if (b < kPumpStop || b >= kPumpResume)
        {
            on.h.pumpUntilIdle();
        }
    });
    const auto c = on.h.engine.experimentalReadAhead()->countersSnapshot();
    const int rows = audioRowCount(on.h);
    std::printf("[info] counters: missed=%lld stale=%lld consumed=%lld produced=%lld\n",
                (long long)c.missedBlocks, (long long)c.staleDiscarded, (long long)c.consumedBlocks,
                (long long)c.producedBlocks);

    // Queue depth 3 covers blocks 10..12 after the pump stops; 13 and 14 miss (the resume pump
    // runs AFTER block 14's callback); the late blocks 13/14 are then discarded as stale.
    expect(c.missedBlocks == (std::int64_t)rows * 2, "exactly the uncovered blocks missed (per owned row)");
    expect(c.staleDiscarded >= (std::int64_t)rows * 2, "late results discarded unplayed (stale), never played");
    expect(peakOfRange(out, (size_t)(kPumpResume + 1) * kBlock * 2) > 0.0,
           "playback recovered to ring hits after the worker caught up");
    expect(countStreamBreaks(on.pre) == 0,
           "the chain's input stream stayed contiguous through miss + stale discard (state aligned)");
    expect(gOverlaps.load() == 0, "no concurrent processing");
}

// =============================================================================================
void testStopReplayResidue()
{
    std::printf("\n-- stop -> replay from the same position: documented <= depth re-feed residue --\n");
    constexpr int kBlocks = 12;

    Fixture on(kDepth);
    (void)on.h.runPlayingBlocksPumped(kBlocks);
    const int callsAfterFirstRun = on.pre->callCount();
    expect(callsAfterFirstRun >= kBlocks && callsAfterFirstRun <= kBlocks + kDepth,
           "first run: chain state advanced at most depth blocks past the stop");

    (void)on.h.runPlayingBlocksPumped(kBlocks); // replay from 0 on the SAME instances
    int zeroPositionCalls = 0;
    for (int i = 0; i < on.pre->callCount(); ++i)
    {
        if (on.pre->call(i).timeInSamples == 0)
        {
            ++zeroPositionCalls;
        }
    }
    expect(zeroPositionCalls == 2,
           "replay re-fed position 0 to a chain whose state was ahead — the DOCUMENTED residue, visible");
    expect(countStreamBreaks(on.pre) == 1, "exactly one stream break: the stop/replay discontinuity");
    expect(gOverlaps.load() == 0 && gDestroyedWhileProcessing.load() == 0, "lifetime guards clean");
}

// =============================================================================================
void testSeekWhileAhead()
{
    std::printf("\n-- seek while ahead: discard reset, re-adoption, contiguous per-segment streams --\n");
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
    const int rows = audioRowCount(on.h);

    expect(c.discardResets >= (std::int64_t)rows * 2,
           "seek + stop each performed a discard reset of every owned row");
    expect(c.adopted == (std::int64_t)rows * 2, "rows re-adopted after the seek");
    expect(countStreamBreaks(on.pre) == 1, "exactly one stream break (the seek)");
    bool segment2StartsAtTarget = false;
    for (int i = 1; i < on.pre->callCount(); ++i)
    {
        if (on.pre->call(i).timeInSamples != on.pre->call(i - 1).timeInSamples + kBlock)
        {
            segment2StartsAtTarget = on.pre->call(i).timeInSamples == kSeekTarget;
            break;
        }
    }
    expect(segment2StartsAtTarget, "post-seek stream starts exactly at the seek target");
    expect(gOverlaps.load() == 0, "no concurrent processing across the reset");
}

// =============================================================================================
void testMonitorEnableDrainsGapless()
{
    std::printf("\n-- Monitor enabled mid-run: gapless draining release, onset delayed <= depth --\n");
    constexpr int kBlocks = 24;
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

    expect(c.drainReleases == 1, "exactly one gapless draining release (the monitored row)");
    expect(c.missedBlocks == 0, "the drain played the whole queue — no misses, no silence gap");
    // Gapless by the model: queued clip blocks play out, then the monitor pass takes over at the
    // very next block position — the chain's stream has NO break and NO duplicate.
    expect(countStreamBreaks(on.pre) == 0,
           "chain stream contiguous across owned -> draining -> monitoring (gapless release)");
    expect(allCallsFullBlock(on.pre), "monitoring processes full blocks too");
    // +1: the stop-edge callback also runs the monitor pass (monitoring processes while stopped —
    // today's behavior, untouched by the prototype).
    expect(on.pre->callCount() == kBlocks + 1,
           "every block processed exactly once (consume path, then monitor pass) — onset delay visible as positions, not loss");
    expect(gOverlaps.load() == 0, "worker and monitor pass never touched the chain concurrently");
}

// =============================================================================================
void testChainRemovalWhileOwned()
{
    std::printf("\n-- insert chain removed while owned: publish-before-destroy + worker quiesce --\n");
    constexpr int kBlocks = 24;
    constexpr int kRemoveAfterBlock = 8;

    Fixture on(kDepth);
    const TrackId editedTid = on.h.audioTids[1];
    const std::vector<float> out = on.h.runPlayingBlocks(kBlocks, 0, [&](const int b) {
        on.h.pumpUntilIdle();
        if (b == kRemoveAfterBlock)
        {
            // Message-thread chain edit: publish -> drain hook (callback exit + read-ahead
            // quiesce) -> destroy. The probe pointers are DANGLING after this call.
            on.h.pluginHost.removePlugin(editedTid);
            on.pre = nullptr;
            on.post = nullptr;
        }
    });
    const auto c = on.h.engine.experimentalReadAhead()->countersSnapshot();
    const int rows = audioRowCount(on.h);

    expect(gDestroyedWhileProcessing.load() == 0,
           "no instance destroyed while processing (publish-before-destroy + quiesce held)");
    expect(gOverlaps.load() == 0 && gPreparedWhileProcessing.load() == 0, "concurrency guards clean");
    expect(c.discardResets >= (std::int64_t)rows, "the quiesce forced a full reset (rows back to Live)");
    expect(c.adopted >= (std::int64_t)rows * 2, "rows re-adopted after the chain edit");
    expect(peakOfRange(out, (size_t)(kRemoveAfterBlock + 3) * kBlock * 2) > 0.0,
           "playback continued after the edit (dry row data still fans)");
}

// =============================================================================================
void testCycleGatesAdoptionOff()
{
    std::printf("\n-- cycle active: adoption gated OFF, output identical to the direct path --\n");
    constexpr int kBlocks = 40;
    constexpr std::int64_t kLocL = 1000;
    constexpr std::int64_t kLocR = 3000;

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

    expect(c.adopted == 0, "no row was ever adopted while the cycle was active");
    expect(c.producedBlocks == 0 && c.missedBlocks == 0, "worker never ran, nothing missed");
    expect(maxAbsDiff(outOff, outOn) == 0.0,
           "cycle playback bit-identical with the flag on (pure A1/A2 path)");
}

// =============================================================================================
void testWorkerThreadSmoke()
{
    std::printf("\n-- real worker THREAD smoke run (timing non-deterministic; guards + accounting only) --\n");
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
    const int rows = audioRowCount(on.h);
    std::printf("[info] thread-mode counters: adopted=%lld produced=%lld consumed=%lld missed=%lld stale=%lld\n",
                (long long)c.adopted, (long long)c.producedBlocks, (long long)c.consumedBlocks,
                (long long)c.missedBlocks, (long long)c.staleDiscarded);

    expect(peakOf(out) > 0.01, "audible output with the real worker thread");
    expect(c.adopted >= rows, "rows adopted");
    expect(c.producedBlocks > 0 && c.consumedBlocks > 0, "the worker thread produced and the callback consumed");
    expect(c.consumedBlocks + c.missedBlocks == (std::int64_t)rows * (kBlocks - 1),
           "every owned block accounted for as exactly one hit or one miss (never both, never neither)");
    expect(countStreamBreaks(on.pre) == 0, "chain stream contiguous under the real thread");
    expect(gOverlaps.load() == 0 && gDestroyedWhileProcessing.load() == 0
               && gPreparedWhileProcessing.load() == 0,
           "no concurrent processing / lifetime violations under the real thread");
}

} // namespace

int main()
{
    std::setvbuf(stdout, nullptr, _IONBF, 0); // progress visible even when a hang has to be killed
    juce::ScopedJuceInitialiser_GUI juceInit; // MessageManager: prepareForDevice requires the message thread
    std::printf("ReadAheadPrototypeFocusedTests (model: docs/READAHEAD_PROTOTYPE.md)\n");

    testStablePlaybackBitIdentity();
    testDelayedWorkerMisses();
    testStaleDiscardAndRecovery();
    testStopReplayResidue();
    testSeekWhileAhead();
    testMonitorEnableDrainsGapless();
    testChainRemovalWhileOwned();
    testCycleGatesAdoptionOff();
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
