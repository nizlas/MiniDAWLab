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
//   * a worker that never starts, or is held off before it enters the chain: the prime is
//     declined and every block stays on the direct path (audible, no miss, no overlap). A
//     worker parked inside the first plugin call stays exclusive; rows it has not entered
//     keep rendering live. Once a row is Ahead, a miss is still counted silence and two
//     consecutive misses still leave the mode;
//   * a transient single miss: late results discarded as stale (never played from a wrong
//     time), ownership retained, recovery without an abandon;
//   * Monitor enabled mid-run: IMMEDIATE discard handover (same-block monitor semantics, zero
//     added monitor latency) — the bounded duplicate-position residue is asserted VISIBLE;
//   * chain removal while owned: ownership and queued segments SURVIVE the edit (publish-
//     before-destroy + bounded worker pause; chain edits late-apply like other controls);
//   * Save capture window: adoption hold + gapless drain, state captured with zero lead at the
//     capture point, playback resumes gaplessly — the whole save is audibly a no-op;
//   * FAILURE contract (pause ack / save window): a holdable probe parks the worker INSIDE
//     processBlock; a pause timeout confers NO exclusivity (false return), the blocked worker is
//     never overlapped (Abandoning waits) or destroyed/prepared over, chain retire and device
//     stop WAIT for the real ack, a stalled drain FAILS the save window cleanly (error Result,
//     previous file byte-identical, dirty kept, deferred retry works), and Playing->Paused
//     during the drain wait resolves to the paused-capture semantics;
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

#include "app/PluginStateCaptureWindow.h"
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
        // Controlled synchronization for the pause-contract tests: when the gate is armed, PARK
        // inside this call (the worker is now provably mid-plugin-call) until the test releases
        // the gate. The 10 s hard cap guarantees the test process terminates even when an
        // assertion failed before the releasing line ran.
        if (holdGate_.load(std::memory_order_acquire) != 0)
        {
            holding_.store(1, std::memory_order_release);
            const auto holdCap = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            while (holdGate_.load(std::memory_order_acquire) != 0
                   && std::chrono::steady_clock::now() < holdCap)
            {
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            }
            holding_.store(0, std::memory_order_release);
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

    /// Arm/release the in-processBlock park (see processBlock). Callable from any thread.
    void setHoldInProcessBlock(const bool hold) noexcept
    {
        holdGate_.store(hold ? 1 : 0, std::memory_order_release);
    }
    [[nodiscard]] bool isHeldInProcessBlock() const noexcept
    {
        return holding_.load(std::memory_order_acquire) != 0;
    }

private:
    const float gain_;
    std::atomic<int> holdGate_{ 0 };
    std::atomic<int> holding_{ 0 };
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

/// `sourceDescription` override: the save path requires clip sources to be EXISTING files under
/// the project's Audio/ folder — the guarded-save test passes real temp-file paths here.
[[nodiscard]] std::shared_ptr<const AudioClip> makeNoiseClip(const std::uint32_t seed, const int numSamples,
                                                             const juce::String& sourceDescription = {})
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
                                             sourceDescription.isNotEmpty()
                                                 ? sourceDescription
                                                 : juce::String("ra-noise-") + juce::String((int)seed));
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

    /// Run `blocks` raw production callbacks against the CURRENT transport state — no seek, no
    /// intent change, no trailing stop edge (unlike runPlayingBlocks). Returns the output peak.
    /// `sleepMicrosBetween` gives a real worker thread breathing room between callbacks.
    [[nodiscard]] double runCallbackBlocks(const int blocks, const int sleepMicrosBetween = 0)
    {
        juce::AudioBuffer<float> blk(2, kBlock);
        float* ptrs[2] = { blk.getWritePointer(0), blk.getWritePointer(1) };
        juce::AudioIODeviceCallbackContext ctx;
        double peak = 0.0;
        for (int b = 0; b < blocks; ++b)
        {
            blk.clear();
            engine.audioDeviceIOCallbackWithContext(nullptr, 0, ptrs, 2, kBlock, ctx);
            for (int i = 0; i < kBlock; ++i)
            {
                peak = std::max(peak, std::fabs((double)ptrs[0][i]));
                peak = std::max(peak, std::fabs((double)ptrs[1][i]));
            }
            if (sleepMicrosBetween > 0)
            {
                std::this_thread::sleep_for(std::chrono::microseconds(sleepMicrosBetween));
            }
        }
        return peak;
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

    /// `clipSourceAudioDir`: when set, clip source descriptions become absolute paths of (dummy)
    /// files inside that directory — required by the project save path (sources must be existing
    /// files under the project's Audio/ folder).
    explicit Fixture(const int readAheadDepth, const juce::File* clipSourceAudioDir = nullptr)
        : h(readAheadDepth)
    {
        constexpr int kClipLen = kBlock * 400;
        const auto sourceFor = [clipSourceAudioDir](const int seed) -> juce::String {
            return clipSourceAudioDir == nullptr
                       ? juce::String()
                       : clipSourceAudioDir
                             ->getChildFile("ra-noise-" + juce::String(seed) + ".wav")
                             .getFullPathName();
        };
        h.session.addGroupTrack();
        h.addAudioTrackWithClip(makeNoiseClip(7, kClipLen, sourceFor(7)));
        h.addAudioTrackWithClip(makeNoiseClip(13, kClipLen, sourceFor(13)));
        h.addAudioTrackWithClip(makeNoiseClip(29, kClipLen, sourceFor(29)));
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
    std::printf("\n-- worker never runs: uncommitted primes decline, every block stays on the direct path --\n");
    constexpr int kBlocks = 16;

    Fixture on(kDepth);
    // NEVER pump. The old contract promoted every Scheduled row on the next block and then
    // counted an empty ring as silence. A prime the worker has not entered must instead
    // decline: this block renders live, no miss, no abandon, no hole in the chain stream.
    const std::vector<float> out = on.h.runPlayingBlocks(kBlocks, 0, [](int) {});
    const auto c = on.h.engine.experimentalReadAhead()->countersSnapshot();
    printCounters(c, "declined-prime counters");
    const int rows = audioRowCount(on.h);

    expect(peakOfBlockSpan(out, 0, kBlocks) > 0.01,
           "every block audible — a prime the worker never starts does not silence the row");
    expect(c.missedSegments == 0, "declining an uncommitted prime is not a miss");
    expect(c.missAbandons == 0, "declining an uncommitted prime is not an abandonment");
    expect(c.discardResets == 0, "a declined prime is not a discontinuity reset");
    expect(c.producedSegments == 0 && c.consumedSegments == 0,
           "nothing produced or consumed (worker never ran)");
    expect(c.adopted == (std::int64_t)rows * kBlocks,
           "each block re-offered the declined rows; none stayed owned across a block");
    expect(on.pre->callCount() == kBlocks,
           "the chain processed every playing block on the direct path");
    expect(countStreamBreaks(on.pre) == 0, "direct-path stream stayed contiguous (no hole, no duplicate)");
    expect(!on.h.engine.experimentalReadAhead()->audioThread_anyOwned(),
           "stop left no row owned");
}

// =============================================================================================
void testDeferredPrimeStaysOnDirectPath()
{
    std::printf("\n-- worker start held off: a prime that cannot run yet stays on the direct path --\n");
    constexpr int kBlocks = 12;

    readahead::setConfiguredReadAheadDepth(kDepth);
    Fixture on(0); // thread-mode renderer; the worker is real but not allowed to claim yet
    readahead::setConfiguredReadAheadDepth(0);
    readahead::ReadAheadRenderer* const ra = on.h.engine.experimentalReadAhead();
    expect(ra != nullptr, "thread-mode renderer constructed");
    // Hold is on the FIRST audio row, which the worker scans first. It is armed only AFTER
    // the adoption block's live render, so the callback itself is not the thread that parks.
    StatefulProbeInsert* const first = on.h.installInsert(on.h.audioTids[0], InsertStage::Pre, 0.5f);
    ra->setDeferPrimeForTests(true);

    on.h.transport.requestSeek(0);
    on.h.transport.requestPlaybackIntent(PlaybackIntent::Playing);
    (void)on.h.runCallbackBlocks(1);
    const auto duringDefer = ra->countersSnapshot();
    expect(duringDefer.producedSegments == 0, "deferred worker did not render during the adoption block");
    expect(duringDefer.missedSegments == 0, "adoption block is live, not a miss");
    expect(first->callCount() == 1, "first row rendered live once before the worker was released");
    expect(on.pre->callCount() == 1, "sibling row rendered live on the adoption block");

    first->setHoldInProcessBlock(true);
    ra->setDeferPrimeForTests(false);
    const auto holdDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!first->isHeldInProcessBlock() && std::chrono::steady_clock::now() < holdDeadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    expect(first->isHeldInProcessBlock(), "worker parked inside the first row's prime, not the callback");

    // While that one prime is inside the plugin, every other row is still uncommitted.
    // Those rows must keep rendering live. The held instance must not be entered again.
    const int siblingBefore = on.pre->callCount();
    const auto missesBefore = ra->countersSnapshot().missedSegments;
    constexpr int kWhileHeld = 6;
    (void)on.h.runCallbackBlocks(kWhileHeld, 1000);
    const auto whileHeld = ra->countersSnapshot();
    expect(first->isHeldInProcessBlock(), "worker still inside the original prime");
    expect(gOverlaps.load() == 0, "callback did not process the instance the worker is inside");
    expect(first->callCount() == 1,
           "the held row was not live-rendered again (its in-flight call has not even been counted yet)");
    expect(on.pre->callCount() == siblingBefore + kWhileHeld,
           "sibling row stayed on the direct path for every block while the prime could not finish");
    expect(countStreamBreaks(on.pre) == 0, "sibling stream stayed contiguous");
    // A row the worker already finished one segment for can still miss and leave the mode
    // while the worker is stuck — that is the steady-state miss policy, not a priming gap.
    // The in-flight row's own absent blocks must be counted, not replaced with silence that
    // the counters hide.
    expect(whileHeld.missedSegments - missesBefore >= kWhileHeld,
           "absent segments of the in-flight row are counted");

    first->setHoldInProcessBlock(false);
    const auto releaseDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (first->isHeldInProcessBlock() && std::chrono::steady_clock::now() < releaseDeadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    expect(!first->isHeldInProcessBlock(), "worker left the prime when the hold was released");

    // The rest of the run is ordinary playback with the worker free. The adoption block and
    // the held window above are the contract; this tail only checks the release does not
    // overlap the chain or mute the mix.
    const std::vector<float> tail = on.h.runPlayingBlocks(kBlocks, 0, [](int) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    });
    expect(peakOfBlockSpan(tail, 0, kBlocks) > 0.01, "playback stayed audible after the worker was released");
    expect(gOverlaps.load() == 0 && gDestroyedWhileProcessing.load() == 0,
           "no concurrent processing after the deferred prime was released");
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
            // concurrency class (worker paused, callback-only processing). Pump mode acquires
            // the pause trivially (the pump and this call share the thread).
            const bool pausedForCapture = ra->pauseWorkerAndWait();
            stateLeadZeroAtCapture = pausedForCapture
                                     && on.pre->callCount() == kCaptureAtBlock + 1
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
    const bool windowOk = on.h.engine.beginPluginStateCaptureWindow();
    if (windowOk)
    {
        on.h.engine.endPluginStateCaptureWindow();
    }
    const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now() - tStart)
                               .count();
    expect(windowOk, "non-playing engine-level capture window succeeds");
    expect(elapsedMs < 400, "non-playing engine-level capture window returns promptly (no 500ms wait)");
}

// =============================================================================================
void testSaveWindowDrainFailureAndPausedTransition()
{
    std::printf("\n-- save window FAILURE contract: stalled drain fails cleanly; Playing->Paused resolves --\n");

    Fixture on(kDepth);
    readahead::ReadAheadRenderer* const ra = on.h.engine.experimentalReadAhead();

    // Drive playback manually: the intent STAYS Playing across the save attempts (runPlayingBlocks
    // would end the run with a stop edge).
    on.h.transport.requestSeek(0);
    on.h.transport.requestPlaybackIntent(PlaybackIntent::Playing);
    for (int b = 0; b < 8; ++b)
    {
        (void)on.h.runCallbackBlocks(1);
        on.h.pumpUntilIdle();
    }
    expect(ra->audioThread_anyOwned(), "rows owned before the save attempts");

    // (a) The device stalls: intent claims Playing but NO callbacks run during the wait (the
    //     single-threaded pump test IS that scenario), so the gapless drain can never complete.
    //     The window must FAIL in bounded time and unwind its hold — never capture as if it
    //     had succeeded.
    on.h.engine.setStateCaptureDrainTimeoutMsForTests(60);
    const auto t0 = std::chrono::steady_clock::now();
    const bool stalledWindow = on.h.engine.beginPluginStateCaptureWindow();
    const auto msStalled = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now() - t0)
                               .count();
    if (stalledWindow)
    {
        on.h.engine.endPluginStateCaptureWindow(); // never leak the window on a FAILED expectation
    }
    expect(!stalledWindow, "drain that cannot complete FAILS the window (no false success)");
    expect(msStalled < 3000, "the failure is bounded (message-thread wait, injectable timeout)");

    // The failed window left no gate behind: playback continues hit-for-hit, ownership intact.
    const auto cBefore = ra->countersSnapshot();
    for (int b = 0; b < 6; ++b)
    {
        (void)on.h.runCallbackBlocks(1);
        on.h.pumpUntilIdle();
    }
    const auto cMid = ra->countersSnapshot();
    expect(ra->audioThread_anyOwned(), "ownership survived the failed window");
    expect(cMid.missedSegments == cBefore.missedSegments && cMid.discardResets == cBefore.discardResets
               && cMid.drainReleases == cBefore.drainReleases,
           "no miss, no reset, no drain release after the failed window (all gates unwound)");

    // (b) Playing -> Paused DURING the drain wait: the window must resolve to the paused-capture
    //     semantics (rows stay owned, documented <= depth state lead) instead of riding into the
    //     timeout — the intent is re-read every wait iteration.
    on.h.engine.setStateCaptureDrainTimeoutMsForTests(5000);
    std::thread flipper([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(120));
        on.h.transport.requestPlaybackIntent(PlaybackIntent::Paused);
    });
    const auto t1 = std::chrono::steady_clock::now();
    const bool pausedWindow = on.h.engine.beginPluginStateCaptureWindow();
    const auto msPaused = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now() - t1)
                              .count();
    flipper.join();
    expect(pausedWindow, "window succeeds once the transport leaves Playing mid-wait");
    expect(msPaused < 4000, "the Playing->Paused transition resolved the wait (not the full timeout)");
    expect(ra->audioThread_anyOwned(), "paused-capture semantics: rows stay owned (<= depth lead)");
    if (pausedWindow)
    {
        on.h.engine.endPluginStateCaptureWindow();
    }
    on.h.engine.setStateCaptureDrainTimeoutMsForTests(500);

    // Resume: the queue continues (pause/resume continuation), the stream stayed clean end to end.
    on.h.transport.requestPlaybackIntent(PlaybackIntent::Playing);
    for (int b = 0; b < 8; ++b)
    {
        (void)on.h.runCallbackBlocks(1);
        on.h.pumpUntilIdle();
    }
    on.h.transport.requestPlaybackIntent(PlaybackIntent::Stopped);
    (void)on.h.runCallbackBlocks(1);
    const auto c = ra->countersSnapshot();
    printCounters(c, "drain-failure counters");
    expect(c.missedSegments == 0 && c.discardResets == 0 && c.staleDiscarded == 0,
           "whole scenario: no miss, no reset, nothing stale");
    expect(countStreamBreaks(on.pre) == 0,
           "chain stream contiguous through the failed window AND the paused window");
    expect(gOverlaps.load() == 0, "no concurrent processing");
}

// =============================================================================================
void testGuardedSaveFailurePreservesFileAndRetry()
{
    std::printf("\n-- guarded save: failed window => error + previous file + dirty kept; retry succeeds --\n");

    // Real project folder layout: the save path requires clip sources to be existing files under
    // <projectDir>/Audio/, so dummy source files are created and the fixture's clips point at them.
    const juce::File dir = juce::File::getSpecialLocation(juce::File::tempDirectory)
                               .getChildFile("ra-save-window-tests");
    (void)dir.deleteRecursively();
    const juce::File audioDir = dir.getChildFile("Audio");
    expect(audioDir.createDirectory().wasOk(), "temp project Audio dir created");
    for (const int seed : { 7, 13, 29 })
    {
        const juce::Result made
            = audioDir.getChildFile("ra-noise-" + juce::String(seed) + ".wav").create();
        expect(made.wasOk(), "dummy clip source file created");
    }
    Fixture on(kDepth, &audioDir);
    const juce::File projectFile = dir.getChildFile("guarded-save.mdlproj");

    // The production decision shape (ProjectIoCoordinator around every saveProjectToFile call):
    // open the capture window; only a SUCCEEDED window may write; only a wasOk() result may mark
    // the project clean. `dirty` models the coordinator's dirty flag with that exact rule.
    bool dirty = true;
    const auto guardedSave = [&]() -> juce::Result {
        ScopedPluginStateCaptureWindow window(on.h.engine);
        const juce::Result r = window.succeeded()
                                   ? on.h.session.saveProjectToFile(on.h.transport, projectFile,
                                                                    kRate, &on.h.pluginHost)
                                   : juce::Result::fail("read-ahead capture window failed");
        if (r.wasOk())
        {
            dirty = false; // markProjectCleanNow analogue: ONLY on a completed save
        }
        return r;
    };

    const juce::Result r1 = guardedSave();
    expect(r1.wasOk(), "baseline save succeeds (stopped transport, window trivial)");
    expect(!dirty, "baseline save marked the project clean");
    juce::MemoryBlock baseline;
    expect(projectFile.loadFileAsData(baseline) && baseline.getSize() > 0, "baseline file written");

    // Dirty edit, then playback with a stalled drain (no callbacks run during the wait).
    on.h.session.setTrackChannelFaderGain(on.h.audioTids[2], 0.33f);
    dirty = true;
    on.h.transport.requestSeek(0);
    on.h.transport.requestPlaybackIntent(PlaybackIntent::Playing);
    for (int b = 0; b < 6; ++b)
    {
        (void)on.h.runCallbackBlocks(1);
        on.h.pumpUntilIdle();
    }
    on.h.engine.setStateCaptureDrainTimeoutMsForTests(50);

    const juce::Result r2 = guardedSave();
    expect(!r2.wasOk(), "save with a stalled drain FAILS (an error Result, never silent success)");
    expect(dirty, "the failed save did NOT mark the project clean");
    juce::MemoryBlock afterFail;
    expect(projectFile.loadFileAsData(afterFail) && afterFail == baseline,
           "existing project file byte-identical after the failed save (not corrupted or replaced)");

    // Deferred retry (user retry / the autosave timer tick): once the window can complete, the
    // SAME guarded flow succeeds and the pending edit lands.
    on.h.engine.setStateCaptureDrainTimeoutMsForTests(500);
    on.h.transport.requestPlaybackIntent(PlaybackIntent::Stopped);
    (void)on.h.runCallbackBlocks(1);
    const juce::Result r3 = guardedSave();
    expect(r3.wasOk(), "deferred retry succeeds once the window can complete");
    expect(!dirty, "the successful retry marked the project clean");
    juce::MemoryBlock afterRetry;
    expect(projectFile.loadFileAsData(afterRetry) && !(afterRetry == baseline),
           "the retry wrote the NEW content (the pending fader edit persisted)");

    (void)dir.deleteRecursively();
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

/// Thread-mode fixture whose worker is parked INSIDE the probe row's Pre processBlock. Returns
/// once the park is confirmed (bounded); the probe's 10 s hard cap guarantees termination even
/// when a test assertion fails before the release line runs.
[[nodiscard]] bool parkWorkerInsideProbe(Fixture& on)
{
    on.pre->setHoldInProcessBlock(true);
    on.h.transport.requestSeek(0);
    on.h.transport.requestPlaybackIntent(PlaybackIntent::Playing);
    (void)on.h.runCallbackBlocks(2, 2000); // adopt the rows; the worker produces on its own
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!on.pre->isHeldInProcessBlock() && std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return on.pre->isHeldInProcessBlock();
}

// =============================================================================================
void testWorkerPauseAckTimeoutContract()
{
    std::printf("\n-- pause-ack timeout contract: no false exclusivity, no overlap, late-ack recovery --\n");

    readahead::setConfiguredReadAheadDepth(kDepth);
    Fixture on(0); // thread-mode renderer via the CLI-flag path
    readahead::setConfiguredReadAheadDepth(0);
    readahead::ReadAheadRenderer* const ra = on.h.engine.experimentalReadAhead();
    expect(ra != nullptr, "thread-mode renderer constructed");
    expect(parkWorkerInsideProbe(on), "worker parked inside the probe's processBlock");

    // (1) The production decision logic with an injectable bound: a pause that is NOT really
    //     acknowledged returns false — it confers no exclusivity and withdraws its request.
    ra->setPauseAckTimeoutMsForTests(50);
    expect(!ra->pauseWorkerAndWait(), "pause times out mid-plugin-call: returns false (no exclusivity)");

    // (2) The save window on top of that failed pause: FAILS, no state capture happens. (Paused
    //     intent: the window skips the drain wait and goes straight to the pause attempt.)
    on.h.transport.requestPlaybackIntent(PlaybackIntent::Paused);
    const bool blockedWindow = on.h.engine.beginPluginStateCaptureWindow();
    if (blockedWindow)
    {
        on.h.engine.endPluginStateCaptureWindow(); // never leak the window on a FAILED expectation
    }
    expect(!blockedWindow, "capture window FAILS while the pause cannot be acknowledged");
    on.h.transport.requestPlaybackIntent(PlaybackIntent::Playing);

    // (3) The audio callback was never blocked: it keeps running, the starved rows go
    //     miss -> abandon, and the Abandoning row WAITS for the worker — the in-flight
    //     processBlock is never overlapped by the live path and never loses its resources.
    (void)on.h.runCallbackBlocks(8, 2000);
    expect(on.pre->isHeldInProcessBlock(), "worker STILL inside the original processBlock call");
    expect(gOverlaps.load() == 0, "abandon under a blocked worker never overlapped the chain");

    // (4) Late acknowledgment: release the plugin call from a helper thread; a pause with a
    //     generous bound must succeed, and ONLY after the real release (no permanent lock, no
    //     premature resume of anything).
    std::atomic<bool> releasedFirst{ false };
    std::thread releaser([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        releasedFirst.store(true, std::memory_order_release);
        on.pre->setHoldInProcessBlock(false);
    });
    ra->setPauseAckTimeoutMsForTests(5000);
    const bool ackAfterRelease = ra->pauseWorkerAndWait();
    const bool releaseCameFirst = releasedFirst.load(std::memory_order_acquire);
    releaser.join();
    expect(ackAfterRelease, "pause acknowledged after the plugin call returned (late-ack recovery)");
    expect(releaseCameFirst, "the ack arrived ONLY after the real release — never before");
    ra->resumeWorker();

    // (5) Full recovery: a normal capture window works again.
    on.h.transport.requestPlaybackIntent(PlaybackIntent::Stopped);
    (void)on.h.runCallbackBlocks(1);
    const bool recoveredWindow = on.h.engine.beginPluginStateCaptureWindow();
    if (recoveredWindow)
    {
        on.h.engine.endPluginStateCaptureWindow();
    }
    expect(recoveredWindow, "capture window succeeds after the worker recovered");

    ra->setPauseAckTimeoutMsForTests(2000);
    expect(gOverlaps.load() == 0 && gDestroyedWhileProcessing.load() == 0
               && gPreparedWhileProcessing.load() == 0,
           "lifetime guards clean through timeout, late ack and recovery");
}

// =============================================================================================
void testChainRemovalWaitsForBlockedWorker()
{
    std::printf("\n-- chain removal under a BLOCKED worker: retire waits for the REAL ack before destroy --\n");

    readahead::setConfiguredReadAheadDepth(kDepth);
    Fixture on(0);
    readahead::setConfiguredReadAheadDepth(0);
    readahead::ReadAheadRenderer* const ra = on.h.engine.experimentalReadAhead();
    expect(parkWorkerInsideProbe(on), "worker parked inside the probe's processBlock before the edit");

    // Short per-attempt bound: the retire path must RETRY until the real ack instead of falling
    // through — the instances may only be destroyed after the in-flight plugin call returned.
    ra->setPauseAckTimeoutMsForTests(50);
    std::atomic<bool> releasedFirst{ false };
    StatefulProbeInsert* const heldPre = on.pre;
    std::thread releaser([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        releasedFirst.store(true, std::memory_order_release);
        heldPre->setHoldInProcessBlock(false); // the store is the releaser's LAST touch
    });
    on.h.pluginHost.removePlugin(on.h.audioTids[1]); // publish -> drain hook -> destroy
    const bool releaseCameFirst = releasedFirst.load(std::memory_order_acquire);
    on.pre = nullptr;
    on.post = nullptr;
    releaser.join();

    expect(releaseCameFirst,
           "removePlugin returned only AFTER the worker released (retire stalled, never fell through)");
    expect(gDestroyedWhileProcessing.load() == 0,
           "retired instances never destroyed while the worker was inside them");
    expect(gOverlaps.load() == 0 && gPreparedWhileProcessing.load() == 0, "concurrency guards clean");

    ra->setPauseAckTimeoutMsForTests(2000);
    on.h.transport.requestPlaybackIntent(PlaybackIntent::Stopped);
    (void)on.h.runCallbackBlocks(1);
}

// =============================================================================================
void testDeviceStopWaitsForBlockedWorker()
{
    std::printf("\n-- device stop under a BLOCKED worker: row teardown waits before releaseResources --\n");

    readahead::setConfiguredReadAheadDepth(kDepth);
    Fixture on(0);
    readahead::setConfiguredReadAheadDepth(0);
    readahead::ReadAheadRenderer* const ra = on.h.engine.experimentalReadAhead();
    expect(parkWorkerInsideProbe(on), "worker parked inside the probe's processBlock before the stop");

    // Short per-attempt bound again: releaseForDevice must BLOCK (retrying) until the real ack —
    // the engine calls the host's releaseResources right after it, mirroring JUCE's guarantee
    // that no callback runs during teardown.
    ra->setPauseAckTimeoutMsForTests(50);
    std::atomic<bool> releasedFirst{ false };
    std::thread releaser([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        releasedFirst.store(true, std::memory_order_release);
        on.pre->setHoldInProcessBlock(false);
    });
    on.h.engine.audioDeviceStopped(); // production device-stop path
    const bool releaseCameFirst = releasedFirst.load(std::memory_order_acquire);
    releaser.join();

    expect(releaseCameFirst, "audioDeviceStopped returned only AFTER the worker really released");
    expect(gPreparedWhileProcessing.load() == 0,
           "releaseResources never overlapped the worker's in-flight processBlock");
    expect(gOverlaps.load() == 0 && gDestroyedWhileProcessing.load() == 0, "guards clean");

    // Device restart: the renderer leaves its between-devices park and plays normally again.
    ra->setPauseAckTimeoutMsForTests(2000);
    on.h.engine.audioDeviceAboutToStart(&on.h.device);
    const double peak = on.h.runCallbackBlocks(16, 2000);
    expect(peak > 0.01, "audible playback after the device restart");
    on.h.transport.requestPlaybackIntent(PlaybackIntent::Stopped);
    (void)on.h.runCallbackBlocks(1);
    expect(gOverlaps.load() == 0 && gDestroyedWhileProcessing.load() == 0
               && gPreparedWhileProcessing.load() == 0,
           "lifetime guards clean through stop, blocked teardown and restart");
}

// =============================================================================================
// DAL Mono Delay round-trip. Not part of the default suite (real VST3, fresh process per
// reopen). Run: ReadAheadPrototypeFocusedTests --mono-delay-save
// Reopen child: --mono-delay-reopen <project> <expected-sidecar>
// =============================================================================================

struct SavedDelayParam
{
    juce::String name;
    float value = 0.0f;
    juce::String text;
};

[[nodiscard]] bool applyKnownDelayParams(juce::AudioPluginInstance& inst, juce::String& why)
{
    struct Spec
    {
        const char* name;
        bool asText;
        float norm;
        const char* text;
    };
    const Spec specs[] = {
        { "Sync", false, 0.0f, nullptr },
        { "Delay time", true, 0.0f, "250.0 ms" },
        { "Feedback", true, 0.0f, "20.0 %" },
        { "Mix", true, 0.0f, "35.0 %" },
        { "Low Cut enabled", false, 0.0f, nullptr },
        { "High Cut enabled", false, 0.0f, nullptr },
    };
    for (const Spec& s : specs)
    {
        juce::AudioProcessorParameter* found = nullptr;
        for (auto* p : inst.getParameters())
        {
            if (p->getName(64) == s.name)
            {
                found = p;
                break;
            }
        }
        if (found == nullptr)
        {
            why = juce::String("parameter not found: ") + s.name;
            return false;
        }
        if (s.asText)
        {
            found->setValueNotifyingHost(found->getValueForText(s.text));
        }
        else
        {
            found->setValueNotifyingHost(s.norm);
        }
    }
    return true;
}

[[nodiscard]] bool readDelayParams(juce::AudioPluginInstance& inst, std::vector<SavedDelayParam>& out, juce::String& why)
{
    const char* names[] = { "Sync", "Delay time", "Feedback", "Mix", "Low Cut enabled", "High Cut enabled" };
    out.clear();
    for (const char* name : names)
    {
        juce::AudioProcessorParameter* found = nullptr;
        for (auto* p : inst.getParameters())
        {
            if (p->getName(64) == name)
            {
                found = p;
                break;
            }
        }
        if (found == nullptr)
        {
            why = juce::String("parameter not found: ") + name;
            return false;
        }
        SavedDelayParam row;
        row.name = name;
        row.value = found->getValue();
        row.text = found->getText(found->getValue(), 32);
        out.push_back(row);
    }
    return true;
}

[[nodiscard]] bool writeParamSidecar(const juce::File& file, const std::vector<SavedDelayParam>& rows)
{
    juce::String text;
    for (const auto& r : rows)
    {
        text << r.name << "\t" << juce::String(r.value, 8) << "\t" << r.text << "\n";
    }
    return file.replaceWithText(text);
}

[[nodiscard]] bool loadParamSidecar(const juce::File& file, std::vector<SavedDelayParam>& rows)
{
    rows.clear();
    juce::StringArray lines;
    lines.addLines(file.loadFileAsString());
    for (const auto& line : lines)
    {
        if (line.isEmpty())
        {
            continue;
        }
        juce::String name, value, text;
        if (!line.containsChar('\t'))
        {
            return false;
        }
        name = line.upToFirstOccurrenceOf("\t", false, false);
        const juce::String rest = line.fromFirstOccurrenceOf("\t", false, false);
        value = rest.upToFirstOccurrenceOf("\t", false, false);
        text = rest.fromFirstOccurrenceOf("\t", false, false);
        SavedDelayParam row;
        row.name = name;
        row.value = value.getFloatValue();
        row.text = text;
        rows.push_back(row);
    }
    return rows.size() == 6;
}

void pumpMessageLoopMs(const int ms)
{
    const auto start = juce::Time::getMillisecondCounter();
    while ((int)(juce::Time::getMillisecondCounter() - start) < ms)
    {
        juce::MessageManager::getInstance()->runDispatchLoopUntil(20);
    }
}

[[nodiscard]] int measureDelayEchoSamples(PluginInsertHost& host, const TrackId tid)
{
    const auto process = [&](const bool impulse) {
        host.audioThread_clearScratch(2, kBlock);
        float* const* ptrs = host.audioThread_getScratchWritePointers();
        if (impulse && ptrs != nullptr && ptrs[0] != nullptr && ptrs[1] != nullptr)
        {
            ptrs[0][0] = 1.0f;
            ptrs[1][0] = 1.0f;
        }
        host.audioThread_processChainForTrack(tid, InsertStage::Pre, kBlock);
        return ptrs;
    };
    const int warm = (int)(1.0 * kRate / kBlock);
    for (int i = 0; i < warm; ++i)
    {
        (void)process(false);
    }
    std::vector<float> rendered;
    const int cap = (int)(0.40 * kRate / kBlock);
    for (int i = 0; i < cap; ++i)
    {
        float* const* ptrs = process(i == 0);
        if (ptrs != nullptr && ptrs[0] != nullptr)
        {
            rendered.insert(rendered.end(), ptrs[0], ptrs[0] + kBlock);
        }
    }
    int peakAt = -1;
    float peak = 0.0f;
    for (int i = 64; i < (int)rendered.size(); ++i)
    {
        const float a = std::fabs(rendered[(size_t)i]);
        if (a > peak)
        {
            peak = a;
            peakAt = i;
        }
    }
    std::printf("[info] echo sample=%d peak=%f\n", peakAt, peak);
    return (peak > 0.02f) ? peakAt : -1;
}

[[nodiscard]] bool writeToneWav(const juce::File& wav)
{
    juce::WavAudioFormat format;
    std::unique_ptr<juce::FileOutputStream> stream(wav.createOutputStream());
    if (stream == nullptr)
    {
        return false;
    }
    std::unique_ptr<juce::AudioFormatWriter> writer(
        format.createWriterFor(stream.get(), kRate, 2, 16, {}, 0));
    if (writer == nullptr)
    {
        return false;
    }
    stream.release();
    juce::AudioBuffer<float> buf(2, kBlock * 8);
    buf.clear();
    buf.setSample(0, 0, 0.8f);
    buf.setSample(1, 0, 0.8f);
    return writer->writeFromAudioSampleBuffer(buf, 0, buf.getNumSamples());
}

/// The production bracket shared by Save and autosave: ScopedPluginStateCaptureWindow gates
/// Session::saveProjectToFile. Autosave (ProjectIoCoordinator::writeAutosaveNow) is that
/// bracket plus restoring the user's project path afterwards so the autosave does not become
/// the Save target.
[[nodiscard]] bool saveThroughCaptureWindow(Harness& h, const juce::File& file, juce::String& why)
{
    const ScopedPluginStateCaptureWindow window(h.engine);
    if (!window.succeeded())
    {
        why = "capture window failed";
        return false;
    }
    const juce::Result r = h.session.saveProjectToFile(h.transport, file, kRate, &h.pluginHost);
    if (!r.wasOk())
    {
        why = r.getErrorMessage();
        return false;
    }
    return true;
}

[[nodiscard]] int reopenDelayProject(const juce::File& project, const juce::File& sidecar)
{
    std::vector<SavedDelayParam> expected;
    if (!loadParamSidecar(sidecar, expected))
    {
        std::printf("[FAIL] sidecar unreadable: %s\n", sidecar.getFullPathName().toRawUTF8());
        return 1;
    }
    Session session;
    Transport transport;
    PluginInsertHost host;
    host.prepareForDevice(kRate, kBlock, 2);
    juce::StringArray skipped;
    juce::String note;
    const juce::Result loaded = session.loadProjectFromFile(transport, project, kRate, skipped, note, &host);
    if (!loaded.wasOk())
    {
        std::printf("[FAIL] load %s: %s\n", project.getFileName().toRawUTF8(),
                    loaded.getErrorMessage().toRawUTF8());
        return 1;
    }
    pumpMessageLoopMs(500);
    for (const auto& s : skipped)
    {
        std::printf("[info] skipped: %s\n", s.toRawUTF8());
    }
    const TrackId tid{ 1 };
    juce::AudioPluginInstance* const inst = host.liveInstanceAtChainIndexForDiagnostics(tid, 0);
    if (inst == nullptr)
    {
        std::printf("[FAIL] no live DAL Mono Delay after reload of %s\n", project.getFileName().toRawUTF8());
        return 1;
    }
    std::vector<SavedDelayParam> got;
    juce::String why;
    if (!readDelayParams(*inst, got, why) || got.size() != expected.size())
    {
        std::printf("[FAIL] %s\n", why.toRawUTF8());
        return 1;
    }
    bool paramsOk = true;
    for (size_t i = 0; i < got.size(); ++i)
    {
        const float dv = std::fabs(got[i].value - expected[i].value);
        const bool textOk = got[i].text == expected[i].text;
        const bool valueOk = dv < 1.0e-4f;
        std::printf("[info] %s value=%f (expected %f) text=\"%s\" (expected \"%s\")\n",
                    got[i].name.toRawUTF8(), got[i].value, expected[i].value,
                    got[i].text.toRawUTF8(), expected[i].text.toRawUTF8());
        if (!textOk || !valueOk || got[i].name != expected[i].name)
        {
            paramsOk = false;
        }
    }
    const int echo = measureDelayEchoSamples(host, tid);
    const int expectedEcho = (int)std::lround(0.250 * kRate);
    const bool echoOk = std::abs(echo - expectedEcho) <= (int)(0.003 * kRate);
    std::printf("%s parameters %s\n", paramsOk ? "[ ok ]" : "[FAIL]", project.getFileName().toRawUTF8());
    std::printf("%s echo at %d samples, expected %d (+/- 3 ms) %s\n",
                echoOk ? "[ ok ]" : "[FAIL]", echo, expectedEcho, project.getFileName().toRawUTF8());
    return (paramsOk && echoOk) ? 0 : 1;
}

[[nodiscard]] int runMonoDelaySave(const int argc, char** argv)
{
    const juce::String mode(argv[1]);
    if (mode == "--mono-delay-reopen")
    {
        if (argc < 4)
        {
            std::printf("usage: --mono-delay-reopen <project> <sidecar>\n");
            return 2;
        }
        juce::ScopedJuceInitialiser_GUI juceInit;
        return reopenDelayProject(juce::File(argv[2]), juce::File(argv[3]));
    }
    if (mode != "--mono-delay-save")
    {
        return 2;
    }

    juce::ScopedJuceInitialiser_GUI juceInit;
    const juce::File bundle("C:\\Program Files\\Common Files\\VST3\\DALMonoDelay.vst3");
    if (!bundle.exists())
    {
        std::printf("[FAIL] DAL Mono Delay bundle missing: %s\n", bundle.getFullPathName().toRawUTF8());
        return 1;
    }

    const juce::File work = juce::File::getSpecialLocation(juce::File::tempDirectory)
                                .getChildFile("dal-pr7-monodelay-save");
    (void)work.deleteRecursively();
    const juce::File audioDir = work.getChildFile("Audio");
    if (!audioDir.createDirectory())
    {
        std::printf("[FAIL] could not create %s\n", audioDir.getFullPathName().toRawUTF8());
        return 1;
    }
    const juce::File wav = audioDir.getChildFile("tone.wav");
    if (!writeToneWav(wav))
    {
        std::printf("[FAIL] could not write %s\n", wav.getFullPathName().toRawUTF8());
        return 1;
    }
    const juce::File sidecar = work.getChildFile("expected-params.txt");

    const auto runMode = [&](const bool readAhead, const juce::String& tag) -> bool {
        std::printf("\n-- DAL Mono Delay save (%s) --\n", tag.toRawUTF8());
        if (readAhead)
        {
            readahead::setConfiguredReadAheadDepth(kDepth);
        }
        Harness h(0);
        readahead::setConfiguredReadAheadDepth(0);
        if (readAhead && h.engine.experimentalReadAhead() == nullptr)
        {
            std::printf("[FAIL] read-ahead renderer was not constructed\n");
            return false;
        }
        if (!readAhead && h.engine.experimentalReadAhead() != nullptr)
        {
            std::printf("[FAIL] direct path constructed a read-ahead renderer\n");
            return false;
        }
        const auto clip = makeNoiseClip(11, kBlock * 400, wav.getFullPathName());
        const auto placed = h.session.addPlacedClipFromExistingMaterial(clip, 0, 0, clip->getNumSamples(), TrackId{ 1 });
        if (!placed.wasOk())
        {
            std::printf("[FAIL] clip: %s\n", placed.getErrorMessage().toRawUTF8());
            return false;
        }
        h.finishSetup();
        const juce::Result added = h.pluginHost.addInsertFromVst3File(TrackId{ 1 }, InsertStage::Pre, bundle);
        if (!added.wasOk())
        {
            std::printf("[FAIL] add insert: %s\n", added.getErrorMessage().toRawUTF8());
            return false;
        }
        juce::AudioPluginInstance* const inst = h.pluginHost.liveInstanceAtChainIndexForDiagnostics(TrackId{ 1 }, 0);
        juce::String why;
        if (inst == nullptr || !applyKnownDelayParams(*inst, why))
        {
            std::printf("[FAIL] set params: %s\n", why.isEmpty() ? "no instance" : why.toRawUTF8());
            return false;
        }
        std::vector<SavedDelayParam> rows;
        if (!readDelayParams(*inst, rows, why) || !writeParamSidecar(sidecar, rows))
        {
            std::printf("[FAIL] record params: %s\n", why.toRawUTF8());
            return false;
        }
        for (const auto& r : rows)
        {
            std::printf("[info] set %s = %f \"%s\"\n", r.name.toRawUTF8(), r.value, r.text.toRawUTF8());
        }

        std::atomic<bool> pumping{ true };
        h.transport.requestSeek(0);
        h.transport.requestPlaybackIntent(PlaybackIntent::Playing);
        std::thread pump([&] {
            while (pumping.load(std::memory_order_acquire))
            {
                (void)h.runCallbackBlocks(1, 1500);
            }
        });
        const auto stopPump = [&] {
            pumping.store(false, std::memory_order_release);
            if (pump.joinable())
            {
                pump.join();
            }
        };

        bool adopted = !readAhead;
        if (readAhead)
        {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
            while (std::chrono::steady_clock::now() < deadline)
            {
                const auto c = h.engine.experimentalReadAhead()->countersSnapshot();
                if (h.engine.experimentalReadAhead()->audioThread_anyOwned() && c.producedSegments > 0)
                {
                    adopted = c.missedSegments == 0 && c.missAbandons == 0;
                    std::printf("[info] adopted produced=%lld missed=%lld abandons=%lld\n",
                                (long long)c.producedSegments, (long long)c.missedSegments,
                                (long long)c.missAbandons);
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        }
        if (!adopted)
        {
            std::printf("[FAIL] delay row was not adopted cleanly (or misses appeared at prime)\n");
            stopPump();
            h.engine.audioDeviceStopped();
            return false;
        }

        const juce::File playing = work.getChildFile(tag + "-playing.dalproj");
        const juce::File paused = work.getChildFile(tag + "-paused.dalproj");
        const juce::File autosave = work.getChildFile(tag + "-autosave.dalproj");
        if (!saveThroughCaptureWindow(h, playing, why))
        {
            std::printf("[FAIL] save while playing: %s\n", why.toRawUTF8());
            stopPump();
            h.engine.audioDeviceStopped();
            return false;
        }
        std::printf("[ ok ] save while playing -> %s (%lld bytes)\n",
                    playing.getFileName().toRawUTF8(), (long long)playing.getSize());

        stopPump();
        h.transport.requestPlaybackIntent(PlaybackIntent::Paused);
        (void)h.runCallbackBlocks(1);
        if (!saveThroughCaptureWindow(h, paused, why))
        {
            std::printf("[FAIL] save while paused: %s\n", why.toRawUTF8());
            h.engine.audioDeviceStopped();
            return false;
        }
        std::printf("[ ok ] save while paused -> %s (%lld bytes)\n",
                    paused.getFileName().toRawUTF8(), (long long)paused.getSize());

        // Autosave's production writer: same capture window, then put the current project
        // path back so the autosave file does not replace Save's target.
        pumping.store(true, std::memory_order_release);
        h.transport.requestPlaybackIntent(PlaybackIntent::Playing);
        std::thread pump2([&] {
            while (pumping.load(std::memory_order_acquire))
            {
                (void)h.runCallbackBlocks(1, 1500);
            }
        });
        if (readAhead)
        {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
            bool again = false;
            while (std::chrono::steady_clock::now() < deadline)
            {
                if (h.engine.experimentalReadAhead()->audioThread_anyOwned())
                {
                    again = true;
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            if (!again)
            {
                std::printf("[FAIL] row was not adopted again before autosave\n");
                pumping.store(false, std::memory_order_release);
                pump2.join();
                h.engine.audioDeviceStopped();
                return false;
            }
        }
        const juce::File keep = h.session.getCurrentProjectFile();
        if (!saveThroughCaptureWindow(h, autosave, why))
        {
            std::printf("[FAIL] autosave bracket: %s\n", why.toRawUTF8());
            pumping.store(false, std::memory_order_release);
            pump2.join();
            h.engine.audioDeviceStopped();
            return false;
        }
        h.session.setCurrentProjectFile(keep);
        const bool restoredTarget = h.session.getCurrentProjectFile() == keep;
        pumping.store(false, std::memory_order_release);
        pump2.join();
        if (!restoredTarget)
        {
            std::printf("[FAIL] autosave hijacked the Save target\n");
            h.engine.audioDeviceStopped();
            return false;
        }
        std::printf("[ ok ] autosave bracket -> %s (%lld bytes), Save target unchanged\n",
                    autosave.getFileName().toRawUTF8(), (long long)autosave.getSize());
        const auto end = readAhead ? h.engine.experimentalReadAhead()->countersSnapshot()
                                   : readahead::ReadAheadRenderer::Counters{};
        if (readAhead)
        {
            std::printf("[info] end counters missed=%lld abandons=%lld\n",
                        (long long)end.missedSegments, (long long)end.missAbandons);
            if (end.missedSegments != 0 || end.missAbandons != 0)
            {
                std::printf("[FAIL] read-ahead misses during the delay save run\n");
                h.engine.audioDeviceStopped();
                return false;
            }
        }
        h.engine.audioDeviceStopped();
        return true;
    };

    if (!runMode(true, "readahead") || !runMode(false, "direct"))
    {
        return 1;
    }

    const juce::File exe = juce::File::getSpecialLocation(juce::File::currentExecutableFile);
    const juce::StringArray projects = {
        "readahead-playing.dalproj", "readahead-paused.dalproj", "readahead-autosave.dalproj",
        "direct-playing.dalproj",    "direct-paused.dalproj",    "direct-autosave.dalproj",
    };
    int rc = 0;
    for (const auto& name : projects)
    {
        juce::ChildProcess child;
        juce::StringArray args;
        args.add(exe.getFullPathName());
        args.add("--mono-delay-reopen");
        args.add(work.getChildFile(name).getFullPathName());
        args.add(sidecar.getFullPathName());
        if (!child.start(args))
        {
            std::printf("[FAIL] could not start reopen process for %s\n", name.toRawUTF8());
            rc = 1;
            continue;
        }
        const juce::String output = child.readAllProcessOutput();
        child.waitForProcessToFinish(60000);
        std::printf("%s", output.toRawUTF8());
        const int code = child.getExitCode();
        std::printf("%s fresh process %s exit=%d\n", code == 0 ? "[ ok ]" : "[FAIL]",
                    name.toRawUTF8(), code);
        if (code != 0)
        {
            rc = 1;
        }
    }
    return rc;
}

} // namespace

int main(int argc, char** argv)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0); // progress visible even when a hang has to be killed
    if (argc >= 2 && juce::String(argv[1]).startsWith("--mono-delay"))
    {
        return runMonoDelaySave(argc, argv);
    }
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
    testDeferredPrimeStaysOnDirectPath();
    testTransientSingleMissRecovery();
    testMonitorImmediateHandover();
    testChainRemovalWhileOwned();
    testSaveCaptureWindow();
    testSaveWindowDrainFailureAndPausedTransition();
    testGuardedSaveFailurePreservesFileAndRetry();
    testWorkerThreadSmoke();
    testWorkerPauseAckTimeoutContract();
    testChainRemovalWaitsForBlockedWorker();
    testDeviceStopWaitsForBlockedWorker();

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
