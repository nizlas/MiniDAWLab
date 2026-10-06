// =============================================================================
// InstrumentParallelFocusedTests — parallel live-instrument generation (engine/InstrumentRenderPool)
// =============================================================================
// Drives the PRODUCTION PlaybackEngine callback (stub device geometry) with REAL
// ExperimentalInstrumentHost objects that carry a deterministic test instrument installed through
// the host's test seam (no VST3). Checks, with the serial path and the parallel pool in the same
// build:
//   * serial vs parallel output is bit-identical (sum order unchanged) for a session with eight
//     instrument rows incl. a muted row, an Off row, an instrument-less host, a Group bus with a
//     routed instrument and a send;
//   * every live instrument's processBlock runs exactly once per block, never re-entered, never
//     concurrently with itself; the Off row's instance is never processed; the muted row's is;
//   * the small-workload fallback stays serial; `--instrument-workers 0` style override = serial;
//   * publish-before-destroy under a continuously running device thread: hosts are destroyed only
//     after the drain, no instance is ever destroyed or re-prepared while processing;
//   * device stop / start between blocks, and the offline mixdown path (serial by contract)
//     producing the same instrument output as the realtime path.
// Device-free (stub geometry), deterministic, no file I/O except nothing.
// =============================================================================

#include <JuceHeader.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <memory>
#include <thread>
#include <vector>

#include "domain/Session.h"
#include "domain/SessionSnapshot.h"
#include "engine/InstrumentRenderPool.h"
#include "engine/PlaybackEngine.h"
#include "instruments/InstrumentTrackController.h"
#include "plugins/ExperimentalInstrumentHost.h"
#include "plugins/PluginInsertHost.h"
#include "transport/Transport.h"

namespace
{
constexpr double kRate = 48000.0;
constexpr int kBlock = 512;

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

// ---------------------------------------------------------------------------------------------
// Deterministic test instrument: sine voices from MIDI, optional busy work, concurrency guards.
// ---------------------------------------------------------------------------------------------
std::atomic<std::uint64_t> gCurrentBlockId{ 0 };     ///< set by the "device" before every callback
std::atomic<int> gOverlaps{ 0 };                     ///< processBlock re-entered / concurrent on one instance
std::atomic<int> gDoubleProcessInBlock{ 0 };         ///< one instance processed twice in one block
std::atomic<int> gDestroyedWhileProcessing{ 0 };     ///< instance destroyed while inside processBlock
std::atomic<int> gPreparedWhileProcessing{ 0 };      ///< prepareToPlay / releaseResources overlapped processBlock

class TestToneInstrument final : public juce::AudioPluginInstance
{
public:
    explicit TestToneInstrument(const int busyIterations)
        : juce::AudioPluginInstance(BusesProperties().withOutput("Out", juce::AudioChannelSet::stereo(), true))
        , busyIterations_(busyIterations)
    {
    }
    ~TestToneInstrument() override
    {
        if (inProcess_.load(std::memory_order_acquire) != 0)
        {
            gDestroyedWhileProcessing.fetch_add(1, std::memory_order_relaxed);
        }
    }

    void fillInPluginDescription(juce::PluginDescription& d) const override
    {
        d.name = "TestTone";
        d.pluginFormatName = "Test";
        d.isInstrument = true;
    }
    const juce::String getName() const override { return "TestTone"; }
    void prepareToPlay(const double sampleRate, int) override
    {
        if (inProcess_.load(std::memory_order_acquire) != 0)
        {
            gPreparedWhileProcessing.fetch_add(1, std::memory_order_relaxed);
        }
        sampleRate_ = sampleRate > 0.0 ? sampleRate : kRate;
    }
    void releaseResources() override
    {
        if (inProcess_.load(std::memory_order_acquire) != 0)
        {
            gPreparedWhileProcessing.fetch_add(1, std::memory_order_relaxed);
        }
    }
    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override
    {
        if (inProcess_.exchange(1, std::memory_order_acq_rel) != 0)
        {
            gOverlaps.fetch_add(1, std::memory_order_relaxed);
        }
        const std::uint64_t block = gCurrentBlockId.load(std::memory_order_acquire);
        if (block != 0 && block == lastBlockId_)
        {
            gDoubleProcessInBlock.fetch_add(1, std::memory_order_relaxed);
        }
        lastBlockId_ = block;
        blocksProcessed.fetch_add(1, std::memory_order_relaxed);

        for (const auto meta : midi)
        {
            const juce::MidiMessage m = meta.getMessage();
            if (m.isNoteOn())
            {
                freq_ = 440.0 * std::pow(2.0, (m.getNoteNumber() - 69) / 12.0);
                amp_ = 0.2 * (double)m.getVelocity() / 127.0;
                phase_ = 0.0;
                active_ = true;
            }
            else if (m.isNoteOff())
            {
                active_ = false;
            }
        }
        const int n = buffer.getNumSamples();
        float* L = buffer.getNumChannels() > 0 ? buffer.getWritePointer(0) : nullptr;
        float* R = buffer.getNumChannels() > 1 ? buffer.getWritePointer(1) : nullptr;
        const double inc = 2.0 * juce::MathConstants<double>::pi * freq_ / sampleRate_;
        for (int i = 0; i < n; ++i)
        {
            const float v = active_ ? (float)(amp_ * std::sin(phase_)) : 0.0f;
            phase_ += inc;
            if (phase_ > 2.0 * juce::MathConstants<double>::pi)
            {
                phase_ -= 2.0 * juce::MathConstants<double>::pi;
            }
            if (L != nullptr) { L[i] = v; }
            if (R != nullptr) { R[i] = v; }
        }
        // Busy work that does not touch the output (keeps the result deterministic while making the
        // block expensive enough for the pool's parallel threshold).
        volatile double sink = 0.0;
        double acc = 1.0;
        for (int i = 0; i < busyIterations_; ++i)
        {
            acc = acc * 1.0000001 + 0.5;
        }
        sink = acc;
        juce::ignoreUnused(sink);
        inProcess_.store(0, std::memory_order_release);
    }
    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    bool hasEditor() const override { return false; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}
    void getStateInformation(juce::MemoryBlock&) override {}
    void setStateInformation(const void*, int) override {}

    std::atomic<std::uint64_t> blocksProcessed{ 0 };
    [[nodiscard]] int inProcessForDiag() const noexcept { return inProcess_.load(std::memory_order_relaxed); }

private:
    const int busyIterations_;
    std::atomic<int> inProcess_{ 0 };
    std::uint64_t lastBlockId_ = 0;
    double sampleRate_ = kRate;
    double freq_ = 440.0;
    double amp_ = 0.0;
    double phase_ = 0.0;
    bool active_ = false;
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

// ---------------------------------------------------------------------------------------------
// Harness: session + hosts + engine; the "device" is the test calling the callback.
// ---------------------------------------------------------------------------------------------
struct Harness
{
    struct Row
    {
        TrackId tid = kInvalidTrackId;
        std::unique_ptr<ExperimentalInstrumentHost> host;
        TestToneInstrument* instrument = nullptr; ///< owned by the host (null = instrument-less host)
        int note = 60;
    };

    Session session;
    Transport transport;
    PluginInsertHost pluginHost;
    PlaybackEngine engine;
    std::vector<Row> rows;
    StubDevice device;
    TrackId groupTid = kInvalidTrackId;
    int mutedIndex = -1;
    int offIndex = -1;
    int instrumentlessIndex = -1;

    Harness() : engine(transport, session, nullptr, nullptr, &pluginHost)
    {
        engine.setExperimentalInstrumentDeviceLifecycleHooks(
            [this](const double sr, const int bs) {
                for (auto& r : rows) { if (r.host) { r.host->prepareForDevice(sr, bs); } }
            },
            [this] {
                for (auto& r : rows) { if (r.host) { r.host->releaseResources(); } }
            },
            [this](const int n) {
                for (auto& r : rows) { if (r.host) { r.host->audioThread_beginAudioBlock(n); } }
            });
    }

    /// Eight instrument rows: [1] muted, [2] Off, [3] instrument-less host, [5] routed to a Group,
    /// [6] with a send to that Group. Busy iterations make the work large enough for the pool.
    void buildStandardSession(const int busyIterations)
    {
        session.addGroupTrack();
        {
            const auto snap = session.loadSessionSnapshotForAudioThread();
            for (int i = 0; snap != nullptr && i < snap->getNumTracks(); ++i)
            {
                if (snap->getTrack(i).getKind() == TrackKind::Group) { groupTid = snap->getTrack(i).getId(); }
            }
        }
        mutedIndex = 1;
        offIndex = 2;
        instrumentlessIndex = 3;
        for (int i = 0; i < 8; ++i)
        {
            Row r;
            const auto tid = session.appendExperimentalInstrumentShellTrack("Inst " + juce::String(i));
            jassert(tid.has_value());
            r.tid = *tid;
            r.host = std::make_unique<ExperimentalInstrumentHost>();
            r.note = 48 + 3 * i;
            rows.push_back(std::move(r));
        }
        session.setTrackMuted(rows[(size_t)mutedIndex].tid, true);
        session.setTrackOff(rows[(size_t)offIndex].tid, true);
        session.setTrackChannelFaderGain(rows[0].tid, 0.5f);
        session.setTrackStereoPan(rows[4].tid, -0.6f);
        (void)session.setTrackRoutedOutput(rows[5].tid, groupTid);
        (void)session.insertTrackSend(rows[6].tid, 0, groupTid, 0.4f);
        session.setTrackChannelFaderGain(groupTid, 0.7f);
        // A navigable arrangement (no audio clips here): the offline mixdown renders inside it.
        session.setArrangementExtentSamples((std::int64_t)kRate * 60);
        engine.rebuildRoutingPlanFromSession();
        engine.audioDeviceAboutToStart(&device); // prepares hosts (sr / block)
        for (size_t i = 0; i < rows.size(); ++i)
        {
            if ((int)i == instrumentlessIndex)
            {
                continue;
            }
            auto inst = std::make_unique<TestToneInstrument>(busyIterations);
            rows[i].instrument = inst.get();
            const bool ok = rows[i].host->installInstrumentInstanceForTests(std::move(inst));
            jassert(ok);
            juce::ignoreUnused(ok);
        }
        publishAll();
    }

    void publishAll(const int excludeIndex = -1)
    {
        // The registry lookup (`findExperimentalInstrumentPlaybackEntry`) requires a non-null MIDI
        // controller per entry. With the transport STOPPED throughout these tests the engine never
        // dereferences it (scheduling / stop-edge flushes / play-edge diagnostics are playing-only
        // paths), so an opaque placeholder address stands in for the controller this harness does
        // not build (InstrumentTrackController is not part of this target).
        static std::uint64_t placeholderControllerStorage[64] = {};
        auto* const placeholderController = reinterpret_cast<InstrumentTrackController*>(placeholderControllerStorage);
        std::vector<ExperimentalInstrumentPlaybackEntry> entries;
        for (size_t i = 0; i < rows.size(); ++i)
        {
            if ((int)i == excludeIndex || rows[i].host == nullptr)
            {
                continue;
            }
            entries.push_back(ExperimentalInstrumentPlaybackEntry{ rows[i].tid, rows[i].host.get(), placeholderController, nullptr });
        }
        engine.publishExperimentalInstrumentPlaybackSnapshot(
            std::make_shared<const ExperimentalInstrumentPlaybackSnapshot>(
                ExperimentalInstrumentPlaybackSnapshot{ std::move(entries) }));
    }

    void noteOnAll()
    {
        for (auto& r : rows)
        {
            if (r.host)
            {
                r.host->enqueueMidiMessageFromMessageThread(juce::MidiMessage::noteOn(1, r.note, (juce::uint8)100));
            }
        }
    }

    /// Runs `blocks` callbacks (transport stopped: instruments render from the enqueued MIDI) and
    /// returns the interleaved L/R device output.
    [[nodiscard]] std::vector<float> runBlocks(const int blocks, const bool serial)
    {
        engine.setInstrumentRenderSerialForDiagnostics(serial);
        std::vector<float> out;
        out.reserve((size_t)blocks * kBlock * 2);
        juce::AudioBuffer<float> blk(2, kBlock);
        float* ptrs[2] = { blk.getWritePointer(0), blk.getWritePointer(1) };
        juce::AudioIODeviceCallbackContext ctx;
        for (int b = 0; b < blocks; ++b)
        {
            blk.clear();
            gCurrentBlockId.fetch_add(1, std::memory_order_acq_rel);
            engine.audioDeviceIOCallbackWithContext(nullptr, 0, ptrs, 2, kBlock, ctx);
            for (int i = 0; i < kBlock; ++i)
            {
                out.push_back(ptrs[0][i]);
                out.push_back(ptrs[1][i]);
            }
        }
        return out;
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

// ---------------------------------------------------------------------------------------------
void testSerialVsParallelIdentical()
{
    std::printf("\n-- serial vs parallel: identical output, one processBlock per host per block --\n");
    instrument_render::setConfiguredWorkerCountOverride(4);
    constexpr int kBlocks = 48;
    constexpr int kBusy = 20000; // ≈ tens of µs per instance → summed work above the pool threshold

    Harness a;
    a.buildStandardSession(kBusy);
    expect(a.engine.instrumentRenderWorkerCount() == 4, "override: engine created 4 render workers");
    a.noteOnAll();
    const std::vector<float> outSerial = a.runBlocks(kBlocks, /*serial*/ true);
    const auto statsA = a.engine.instrumentRenderPoolStats();
    expect(statsA.parallelBlocks == 0 && statsA.serialBlocks == (std::uint64_t)kBlocks,
           "serial hint: every block ran the generation stage serially on the callback thread");

    Harness b;
    b.buildStandardSession(kBusy);
    b.noteOnAll();
    const std::vector<float> outParallel = b.runBlocks(kBlocks, /*serial*/ false);
    const auto statsB = b.engine.instrumentRenderPoolStats();
    std::printf("[info] parallel stats: parallelBlocks=%llu serialBlocks=%llu jobsByCallback=%llu jobsByWorkers=%llu\n",
                (unsigned long long)statsB.parallelBlocks, (unsigned long long)statsB.serialBlocks,
                (unsigned long long)statsB.jobsRunByCallback, (unsigned long long)statsB.jobsRunByWorkers);
    // The very first block has no per-host duration estimate yet (sum 0 → serial by design).
    expect(statsB.parallelBlocks >= (std::uint64_t)kBlocks - 1 && statsB.parallelBlocks + statsB.serialBlocks == (std::uint64_t)kBlocks,
           "parallel: every block after the first (no estimate yet) was dispatched to the pool");
    expect(statsB.jobsRunByWorkers > 0, "parallel: worker threads ran generation jobs");
    expect(statsB.jobsRunByCallback + statsB.jobsRunByWorkers == (std::uint64_t)kBlocks * 7,
           "parallel: exactly 7 jobs per block (8 rows − Off; the instrument-less host is a trivial job; muted included)");

    expect(peakOf(outSerial) > 0.05, "fixture produces audible output");
    const double diff = maxAbsDiff(outSerial, outParallel);
    std::printf("[info] serial vs parallel max |diff| = %.3g (peak %.3f)\n", diff, peakOf(outSerial));
    expect(diff == 0.0, "serial and parallel device output are bit-identical (sum order unchanged)");

    for (Harness* h : { &a, &b })
    {
        for (size_t i = 0; i < h->rows.size(); ++i)
        {
            const auto& r = h->rows[i];
            if (r.instrument == nullptr) { continue; }
            const std::uint64_t n = r.instrument->blocksProcessed.load();
            if ((int)i == h->offIndex)
            {
                expect(n == 0, "Off row: instance never processed");
            }
            else
            {
                expect(n == (std::uint64_t)kBlocks, (int)i == h->mutedIndex
                                                         ? "muted row: instance processed every block (gain 0, state kept)"
                                                         : "live row: instance processed exactly once per block");
            }
        }
    }
    expect(gOverlaps.load() == 0, "no instance was ever processed re-entrantly / concurrently");
    expect(gDoubleProcessInBlock.load() == 0, "no instance was processed twice in one block");
}

void testSmallWorkloadStaysSerialAndZeroWorkersOverride()
{
    std::printf("\n-- small workload fallback + 0-worker override --\n");
    instrument_render::setConfiguredWorkerCountOverride(4);
    {
        Harness h;
        h.buildStandardSession(/*busy*/ 0);
        h.noteOnAll();
        (void)h.runBlocks(16, false);
        const auto st = h.engine.instrumentRenderPoolStats();
        expect(st.parallelBlocks == 0 && st.serialBlocks == 16,
               "summed work below the threshold: generation stays serial without waking workers");
    }
    instrument_render::setConfiguredWorkerCountOverride(0);
    {
        Harness h;
        h.buildStandardSession(20000);
        expect(h.engine.instrumentRenderWorkerCount() == 0, "override 0: no worker threads");
        h.noteOnAll();
        (void)h.runBlocks(8, false);
        const auto st = h.engine.instrumentRenderPoolStats();
        expect(st.parallelBlocks == 0 && st.serialBlocks == 8, "override 0: every block serial");
    }
    expect(gOverlaps.load() == 0 && gDoubleProcessInBlock.load() == 0, "no overlaps in the serial variants");
}

void testOfflinePathMatchesRealtime()
{
    std::printf("\n-- offline mixdown (serial by contract) vs realtime --\n");
    instrument_render::setConfiguredWorkerCountOverride(4);
    constexpr int kBlocks = 12;
    Harness rt;
    rt.buildStandardSession(20000);
    rt.noteOnAll();
    const std::vector<float> realtime = rt.runBlocks(kBlocks, false);

    Harness off;
    off.buildStandardSession(20000);
    off.noteOnAll();
    const auto snap = off.session.loadSessionSnapshotForAudioThread();
    const auto instSnap = off.engine.loadExperimentalInstrumentPlaybackSnapshotForAudioThread();
    std::vector<float> offline;
    juce::AudioBuffer<float> blk(2, kBlock);
    float* ptrs[2] = { blk.getWritePointer(0), blk.getWritePointer(1) };
    expect(off.engine.beginOfflineRenderGate(), "offline gate engaged");
    for (int b = 0; b < kBlocks; ++b)
    {
        blk.clear();
        gCurrentBlockId.fetch_add(1, std::memory_order_acq_rel); // one "block" per offline render call
        off.engine.renderOfflineMixdownBlock(*snap, instSnap.get(), (std::int64_t)b * kBlock, kBlock, ptrs, b == 0);
        for (int i = 0; i < kBlock; ++i) { offline.push_back(ptrs[0][i]); offline.push_back(ptrs[1][i]); }
    }
    expect(off.engine.endOfflineRenderGate(), "offline gate released");
    const auto st = off.engine.instrumentRenderPoolStats();
    expect(st.parallelBlocks == 0 && st.serialBlocks == 0, "offline render never used the pool");
    const double diff = maxAbsDiff(realtime, offline);
    std::printf("[info] realtime(parallel) vs offline max |diff| = %.3g\n", diff);
    expect(diff <= 1.0e-6, "offline instrument output equals the realtime (parallel) output");
    expect(gDoubleProcessInBlock.load() == 0, "offline: each instance rendered once per offline block");
}

void testLifetimeUnderRunningDevice()
{
    std::printf("\n-- publish-before-destroy / device stop-start under a running device thread --\n");
    instrument_render::setConfiguredWorkerCountOverride(4);
    Harness h;
    h.buildStandardSession(20000);
    h.noteOnAll();

    std::atomic<bool> stop{ false };
    std::atomic<std::uint64_t> callbacks{ 0 };
    std::thread device([&] {
        juce::AudioBuffer<float> blk(2, kBlock);
        float* ptrs[2] = { blk.getWritePointer(0), blk.getWritePointer(1) };
        juce::AudioIODeviceCallbackContext ctx;
        while (!stop.load(std::memory_order_acquire))
        {
            blk.clear();
            gCurrentBlockId.fetch_add(1, std::memory_order_acq_rel);
            h.engine.audioDeviceIOCallbackWithContext(nullptr, 0, ptrs, 2, kBlock, ctx);
            callbacks.fetch_add(1, std::memory_order_relaxed);
            std::this_thread::sleep_for(std::chrono::microseconds(300));
        }
    });

    int swaps = 0;
    int drainTimeouts = 0;
    for (int iter = 0; iter < 60; ++iter)
    {
        const int k = iter % (int)h.rows.size();
        if (k == h.instrumentlessIndex) { continue; }
        // 1. publish without row k, 2. drain the in-flight callback (and with it every job),
        // 3. destroy the host, 4. new host + instrument, 5. publish the full set.
        h.publishAll(k);
        if (!h.engine.waitForAudioCallbackExit(1000.0)) { ++drainTimeouts; }
        h.rows[(size_t)k].host.reset();
        h.rows[(size_t)k].instrument = nullptr;
        auto host = std::make_unique<ExperimentalInstrumentHost>();
        host->prepareForDevice(kRate, kBlock);
        auto inst = std::make_unique<TestToneInstrument>(20000);
        h.rows[(size_t)k].instrument = inst.get();
        (void)host->installInstrumentInstanceForTests(std::move(inst));
        host->enqueueMidiMessageFromMessageThread(juce::MidiMessage::noteOn(1, h.rows[(size_t)k].note, (juce::uint8)100));
        h.rows[(size_t)k].host = std::move(host);
        h.publishAll();
        ++swaps;
        h.engine.setInstrumentRenderSerialForDiagnostics((iter % 7) == 3);
        if (iter == 30)
        {
            // Device stop / start between callbacks (prepare / release must never overlap processing).
            stop.store(true, std::memory_order_release);
            device.join();
            h.engine.audioDeviceStopped();
            h.engine.audioDeviceAboutToStart(&h.device);
            stop.store(false, std::memory_order_release);
            device = std::thread([&] {
                juce::AudioBuffer<float> blk(2, kBlock);
                float* ptrs[2] = { blk.getWritePointer(0), blk.getWritePointer(1) };
                juce::AudioIODeviceCallbackContext ctx;
                while (!stop.load(std::memory_order_acquire))
                {
                    blk.clear();
                    gCurrentBlockId.fetch_add(1, std::memory_order_acq_rel);
                    h.engine.audioDeviceIOCallbackWithContext(nullptr, 0, ptrs, 2, kBlock, ctx);
                    callbacks.fetch_add(1, std::memory_order_relaxed);
                    std::this_thread::sleep_for(std::chrono::microseconds(300));
                }
            });
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    stop.store(true, std::memory_order_release);
    device.join();
    h.engine.audioDeviceStopped();

    std::printf("[info] swaps=%d callbacks=%llu drainTimeouts=%d\n", swaps,
                (unsigned long long)callbacks.load(), drainTimeouts);
    expect(swaps >= 50 && callbacks.load() > 100, "stress ran: dozens of host swaps under a cycling device");
    expect(drainTimeouts == 0, "every publish-before-destroy drain completed (jobs never outlive the callback)");
    expect(gDestroyedWhileProcessing.load() == 0, "no instance destroyed while inside processBlock");
    expect(gPreparedWhileProcessing.load() == 0, "no prepare / release overlapped processBlock");
    expect(gOverlaps.load() == 0, "no concurrent processBlock on one instance during the stress");
    expect(gDoubleProcessInBlock.load() == 0, "no instance processed twice in one block during the stress");
    const auto st = h.engine.instrumentRenderPoolStats();
    expect(st.parallelBlocks > 0 && st.serialBlocks > 0, "stress covered both the parallel and the forced-serial path");
}
} // namespace

int main()
{
    std::setvbuf(stdout, nullptr, _IONBF, 0); // progress visible even when a hang has to be killed
    juce::ScopedJuceInitialiser_GUI juceInit; // MessageManager: hosts' prepareForDevice requires the message thread
    std::printf("InstrumentParallelFocusedTests (block %d, %.0f Hz)\n", kBlock, kRate);
    testSerialVsParallelIdentical();
    testSmallWorkloadStaysSerialAndZeroWorkersOverride();
    testOfflinePathMatchesRealtime();
    testLifetimeUnderRunningDevice();
    std::printf("\n%d checks, %d failure(s)\n", checks, failures);
    return failures == 0 ? 0 : 1;
}

// Link seams — PlaybackEngine.cpp references InstrumentTrackController entry points this harness
// never exercises (transport stopped: the placeholder controller is never dereferenced; the stubs
// below touch no member).
ProjectFileExperimentalInstrumentTrackV1 InstrumentTrackController::buildExperimentalInstrumentProjectBlock() const
{
    jassertfalse;
    return {};
}
void InstrumentTrackController::audioThread_scheduleTransportMidiForSegment(ExperimentalInstrumentHost&, std::int64_t, int,
                                                                           int, bool, int, int*) noexcept {}
void InstrumentTrackController::audioThread_flushTransportMidi(ExperimentalInstrumentHost&, int, int) noexcept {}
void InstrumentTrackController::audioThread_flushPendingTransportOffsInto(ExperimentalInstrumentHost&, int, int) noexcept {}
