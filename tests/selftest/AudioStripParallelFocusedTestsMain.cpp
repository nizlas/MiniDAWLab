// =============================================================================
// AudioStripParallelFocusedTests — Stage A1: parallel audio-row channel strips
// =============================================================================
// Drives the PRODUCTION PlaybackEngine callback (stub device geometry) with the transport PLAYING
// over deterministic audio clips, REAL PluginInsertHost chains carrying deterministic test insert
// processors (host test seam, no VST3) and REAL ExperimentalInstrumentHost objects in the SAME
// combined job batch. Checks:
//   * serial-hint vs parallel device output is bit-identical for a mixed session (audio rows with
//     Pre+Post inserts, a muted row, an Off row, Group routing + a send, two instrument rows);
//   * cycle-wrap split blocks: both segments rendered exactly once in order, the insert chain sees
//     the per-segment transport position (seg1 start, then L), no duplication / loss, and the
//     fanned region matches the CLIP CONTENT (regression for the two pre-existing staged-path
//     defects: chain scratch read at destFrame, stale stage region fanned on segment 2);
//   * a monitored row contributes NO clip job (its inserts run via the monitoring pass instead);
//   * Mute keeps the row's job but skips its chain; Off renders nothing and never runs the chain;
//   * no insert or instrument instance is ever processed concurrently with itself;
//   * chain removal / swap and device stop / start under running jobs (publish-before-destroy);
//   * the capacity limit (> kMaxAudioStripJobs rows) falls back to the SERIAL strip path without
//     dropping any row; `--instrument-workers 0` renders through the same job code serially;
//   * the shared strip helpers still serve the OFFLINE mixdown path (bit-equal to realtime).
// Device-free (stub geometry), deterministic, no file I/O.
// =============================================================================

#include <JuceHeader.h>

#include <atomic>
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
// Shared concurrency guards (every deterministic test processor reports into these).
// ---------------------------------------------------------------------------------------------
std::atomic<std::uint64_t> gCurrentBlockId{ 0 };  ///< set by the "device" before every callback
std::atomic<int> gOverlaps{ 0 };                  ///< processBlock re-entered / concurrent on one instance
std::atomic<int> gDestroyedWhileProcessing{ 0 };  ///< instance destroyed while inside processBlock
std::atomic<int> gPreparedWhileProcessing{ 0 };   ///< prepare / release overlapped processBlock
std::atomic<int> gInstrumentDoubleProcess{ 0 };   ///< an INSTRUMENT instance processed twice in one block

// ---------------------------------------------------------------------------------------------
// Deterministic test INSERT effect. Records, per processBlock call: device block id, the playhead
// position the chain presented (per-segment transport context), and the sample count — enough to
// verify segment order, per-call positions and exact coverage. Optional one-pole state makes the
// output depend on processing ORDER and CONTINUITY (detects duplicated / dropped / reordered
// segments through the bit-identity checks). Guards against concurrent processing of one instance.
// ---------------------------------------------------------------------------------------------
class TestInsertEffect final : public juce::AudioPluginInstance
{
public:
    struct Call
    {
        std::uint64_t blockId = 0;
        std::int64_t timeInSamples = -1;
        int numSamples = 0;
    };

    TestInsertEffect(const float gain, const bool stateful)
        : juce::AudioPluginInstance(BusesProperties()
                                        .withInput("In", juce::AudioChannelSet::stereo(), true)
                                        .withOutput("Out", juce::AudioChannelSet::stereo(), true))
        , gain_(gain)
        , stateful_(stateful)
    {
        calls_.resize(1 << 14);
    }
    ~TestInsertEffect() override
    {
        if (inProcess_.load(std::memory_order_acquire) != 0)
        {
            gDestroyedWhileProcessing.fetch_add(1, std::memory_order_relaxed);
        }
    }

    void fillInPluginDescription(juce::PluginDescription& d) const override
    {
        d.name = "TestInsert";
        d.pluginFormatName = "Test";
        d.isInstrument = false;
    }
    const juce::String getName() const override { return "TestInsert"; }
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
        c.blockId = gCurrentBlockId.load(std::memory_order_acquire);
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
                const float y = stateful_ ? (gain_ * d[i] + 0.25f * z) : (gain_ * d[i]);
                z = y;
                d[i] = y;
            }
            if (stateful_)
            {
                state_[ch] = z;
            }
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
    const bool stateful_;
    std::atomic<int> inProcess_{ 0 };
    std::atomic<int> callCount_{ 0 };
    std::atomic<std::uint64_t> samplesProcessed_{ 0 };
    std::vector<Call> calls_;
    float state_[2] = { 0.0f, 0.0f };
};

// ---------------------------------------------------------------------------------------------
// Deterministic test INSTRUMENT (sine from enqueued MIDI + busy work) — same combined batch.
// ---------------------------------------------------------------------------------------------
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
            gInstrumentDoubleProcess.fetch_add(1, std::memory_order_relaxed);
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

/// Deterministic clip material: a per-track LCG noise lane (every sample non-zero-ish, values in
/// ±0.25) long enough for all tests. Stereo; L and R differ so pan/channel bugs surface.
[[nodiscard]] std::shared_ptr<const AudioClip> makeNoiseClip(const std::uint32_t seed, const int numSamples)
{
    juce::AudioBuffer<float> buf(2, numSamples);
    std::uint32_t s = seed * 2654435761u + 12345u;
    for (int i = 0; i < numSamples; ++i)
    {
        for (int ch = 0; ch < 2; ++ch)
        {
            s = s * 1664525u + 1013904223u;
            const float v = ((float)(s >> 8) / (float)(1u << 24)) - 0.5f; // [-0.5, 0.5), deterministic LCG
            buf.setSample(ch, i, 0.5f * v);
        }
    }
    return std::make_shared<const AudioClip>(std::move(buf), kRate, juce::String("test-noise-") + juce::String((int)seed));
}

[[nodiscard]] std::shared_ptr<const AudioClip> makeConstantClip(const float value, const int numSamples)
{
    juce::AudioBuffer<float> buf(2, numSamples);
    for (int ch = 0; ch < 2; ++ch)
    {
        juce::FloatVectorOperations::fill(buf.getWritePointer(ch), value, numSamples);
    }
    return std::make_shared<const AudioClip>(std::move(buf), kRate, "test-const");
}

// ---------------------------------------------------------------------------------------------
// Harness: Session + Transport + PluginInsertHost + PlaybackEngine; the "device" is the test
// calling the production callback. Audio rows carry deterministic clips; insert instances are
// installed through the host's test seam AFTER the device is prepared.
// ---------------------------------------------------------------------------------------------
struct Harness
{
    struct InstRow
    {
        TrackId tid = kInvalidTrackId;
        std::unique_ptr<ExperimentalInstrumentHost> host;
        TestToneInstrument* instrument = nullptr;
        int note = 60;
    };

    Session session;
    Transport transport;
    PluginInsertHost pluginHost;
    PlaybackEngine engine;
    StubDevice device;
    std::vector<TrackId> audioTids;
    std::vector<InstRow> instRows;

    Harness() : engine(transport, session, nullptr, nullptr, &pluginHost)
    {
        engine.setExperimentalInstrumentDeviceLifecycleHooks(
            [this](const double sr, const int bs) {
                for (auto& r : instRows) { if (r.host) { r.host->prepareForDevice(sr, bs); } }
            },
            [this] {
                for (auto& r : instRows) { if (r.host) { r.host->releaseResources(); } }
            },
            [this](const int n) {
                for (auto& r : instRows) { if (r.host) { r.host->audioThread_beginAudioBlock(n); } }
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

    TrackId addInstrumentRow(const int busyIterations, const int note)
    {
        InstRow r;
        const auto tid = session.appendExperimentalInstrumentShellTrack(
            "Inst " + juce::String((int)instRows.size()));
        jassert(tid.has_value());
        r.tid = *tid;
        r.host = std::make_unique<ExperimentalInstrumentHost>();
        r.note = note;
        instRows.push_back(std::move(r));
        auto& row = instRows.back();
        if (busyIterations >= 0)
        {
            // Installed after prepare (see finishSetup); remembered here for later installation.
            pendingBusy_.push_back({ (int)instRows.size() - 1, busyIterations });
        }
        return row.tid;
    }

    /// Prepare the device (which prepares insert host + instrument hosts), then install pending
    /// instrument instances and publish the playback snapshot.
    void finishSetup()
    {
        session.setArrangementExtentSamples((std::int64_t)kRate * 120);
        engine.rebuildRoutingPlanFromSession();
        engine.audioDeviceAboutToStart(&device);
        for (const auto& pb : pendingBusy_)
        {
            auto inst = std::make_unique<TestToneInstrument>(pb.second);
            instRows[(size_t)pb.first].instrument = inst.get();
            const bool ok = instRows[(size_t)pb.first].host->installInstrumentInstanceForTests(std::move(inst));
            jassert(ok);
            juce::ignoreUnused(ok);
        }
        pendingBusy_.clear();
        publishInstruments();
    }

    void publishInstruments(const int excludeIndex = -1)
    {
        if (instRows.empty())
        {
            return;
        }
        // Placeholder controller (same convention as InstrumentParallelFocusedTests): the stubs at
        // the bottom of this file are no-ops, and the members the PLAY-edge diagnostics read
        // through it (domain TrackId, atomic render-snapshot pointer) see zero-initialized storage
        // (= invalid id / null snapshot). The storage must cover the WHOLE object: a too-small
        // array makes those reads hit adjacent statics (observed: an atomic shared_ptr load
        // spinning forever on a garbage lock bit).
        static std::uint64_t placeholderControllerStorage[(sizeof(InstrumentTrackController) + 7) / 8 + 8] = {};
        auto* const placeholderController
            = reinterpret_cast<InstrumentTrackController*>(placeholderControllerStorage);
        std::vector<ExperimentalInstrumentPlaybackEntry> entries;
        for (size_t i = 0; i < instRows.size(); ++i)
        {
            if ((int)i == excludeIndex || instRows[i].host == nullptr)
            {
                continue;
            }
            entries.push_back(ExperimentalInstrumentPlaybackEntry{ instRows[i].tid, instRows[i].host.get(),
                                                                   placeholderController, nullptr });
        }
        engine.publishExperimentalInstrumentPlaybackSnapshot(
            std::make_shared<const ExperimentalInstrumentPlaybackSnapshot>(
                ExperimentalInstrumentPlaybackSnapshot{ std::move(entries) }));
    }

    TestInsertEffect* installInsert(const TrackId tid, const InsertStage stage, const float gain,
                                    const bool stateful)
    {
        auto fx = std::make_unique<TestInsertEffect>(gain, stateful);
        TestInsertEffect* const raw = fx.get();
        const bool ok = pluginHost.installInsertInstanceForTests(tid, stage, std::move(fx));
        jassert(ok);
        juce::ignoreUnused(ok);
        return raw;
    }

    void noteOnAll()
    {
        for (auto& r : instRows)
        {
            if (r.host)
            {
                r.host->enqueueMidiMessageFromMessageThread(
                    juce::MidiMessage::noteOn(1, r.note, (juce::uint8)100));
            }
        }
    }

    /// Seek + Play, run `blocks` production callbacks, Stop. Returns interleaved L/R output.
    [[nodiscard]] std::vector<float> runPlayingBlocks(const int blocks, const bool serialHint,
                                                      const std::int64_t startSample = 0)
    {
        engine.setInstrumentRenderSerialForDiagnostics(serialHint);
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
            gCurrentBlockId.fetch_add(1, std::memory_order_acq_rel);
            engine.audioDeviceIOCallbackWithContext(nullptr, 0, ptrs, 2, kBlock, ctx);
            for (int i = 0; i < kBlock; ++i)
            {
                out.push_back(ptrs[0][i]);
                out.push_back(ptrs[1][i]);
            }
        }
        transport.requestPlaybackIntent(PlaybackIntent::Stopped);
        // One more callback so the stop edge (note flushes etc.) is consumed inside this harness.
        blk.clear();
        gCurrentBlockId.fetch_add(1, std::memory_order_acq_rel);
        engine.audioDeviceIOCallbackWithContext(nullptr, 0, ptrs, 2, kBlock, ctx);
        return out;
    }

private:
    std::vector<std::pair<int, int>> pendingBusy_;
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

/// Audio rows in the published snapshot (the DEFAULT session already contains one empty audio
/// track, which gets a trivial strip job like any other non-monitored audio row).
[[nodiscard]] int countAudioRows(Harness& h)
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

/// Mixed fixture shared by several tests: 8 audio rows + 2 instrument rows.
///   row0: plain; row1: MUTED with a Post insert; row2: OFF with a Pre insert; row3: Pre+Post
///   inserts (stateful Pre — order/continuity sensitive); row4: pan −0.6, fader 0.5;
///   row5: routed to a Group; row6: send → Group (0.4); row7: plain, second noise lane.
struct MixedFixture
{
    Harness h;
    TrackId groupTid = kInvalidTrackId;
    TestInsertEffect* mutedRowInsert = nullptr;
    TestInsertEffect* offRowInsert = nullptr;
    TestInsertEffect* preInsert = nullptr;
    TestInsertEffect* postInsert = nullptr;

    explicit MixedFixture(const int instrumentBusy = 60000)
    {
        constexpr int kClipLen = kBlock * 64;
        h.session.addGroupTrack();
        for (int i = 0; i < 8; ++i)
        {
            h.addAudioTrackWithClip(makeNoiseClip((std::uint32_t)(i + 1), kClipLen));
        }
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
        h.session.setTrackMuted(h.audioTids[1], true);
        h.session.setTrackOff(h.audioTids[2], true);
        h.session.setTrackChannelFaderGain(h.audioTids[4], 0.5f);
        h.session.setTrackStereoPan(h.audioTids[4], -0.6f);
        const bool routed = h.session.setTrackRoutedOutput(h.audioTids[5], groupTid);
        const bool sent = h.session.insertTrackSend(h.audioTids[6], 0, groupTid, 0.4f);
        jassert(routed && sent);
        juce::ignoreUnused(routed, sent);
        h.session.setTrackChannelFaderGain(groupTid, 0.7f);
        h.addInstrumentRow(instrumentBusy, 52);
        h.addInstrumentRow(instrumentBusy, 59);
        h.finishSetup();
        mutedRowInsert = h.installInsert(h.audioTids[1], InsertStage::Post, 0.5f, false);
        offRowInsert = h.installInsert(h.audioTids[2], InsertStage::Pre, 0.5f, false);
        preInsert = h.installInsert(h.audioTids[3], InsertStage::Pre, 0.8f, true);
        postInsert = h.installInsert(h.audioTids[3], InsertStage::Post, 0.9f, false);
        h.noteOnAll();
    }
};

// ---------------------------------------------------------------------------------------------
void testSerialVsParallelIdenticalMixedSession()
{
    std::printf("\n-- mixed session: serial-hint vs parallel bit-identity, combined batch, Mute/Off --\n");
    instrument_render::setConfiguredWorkerCountOverride(4);
    constexpr int kBlocks = 48;

    MixedFixture a;
    expect(a.h.engine.instrumentRenderWorkerCount() == 4, "override: engine created 4 render workers");
    const std::vector<float> outSerial = a.h.runPlayingBlocks(kBlocks, /*serialHint*/ true);
    const auto statsA = a.h.engine.instrumentRenderPoolStats();
    expect(statsA.parallelBlocks == 0 && statsA.serialBlocks > 0,
           "serial hint: every dispatched batch ran serially on the callback thread");

    MixedFixture b;
    const std::vector<float> outParallel = b.h.runPlayingBlocks(kBlocks, /*serialHint*/ false);
    const auto statsB = b.h.engine.instrumentRenderPoolStats();
    std::printf("[info] parallel stats: parallelBlocks=%llu serialBlocks=%llu byCallback=%llu byWorkers=%llu\n",
                (unsigned long long)statsB.parallelBlocks, (unsigned long long)statsB.serialBlocks,
                (unsigned long long)statsB.jobsRunByCallback, (unsigned long long)statsB.jobsRunByWorkers);
    expect(statsB.parallelBlocks > 0, "parallel: blocks were dispatched to the pool");
    expect(statsB.jobsRunByWorkers > 0, "parallel: worker threads ran jobs");
    // ONE combined batch per playing block: one strip job per audio row (muted + Off rows included
    // as trivial/gated jobs; the default session's empty row too) + 2 instrument generation jobs.
    // The stop-edge callback after the loop dispatches the 2 instrument jobs once more (legacy
    // dispatch site — no audio collection while stopped).
    const std::uint64_t jobsPerBlock = (std::uint64_t)countAudioRows(b.h) + 2;
    expect(statsB.jobsRunByCallback + statsB.jobsRunByWorkers
               == (std::uint64_t)kBlocks * jobsPerBlock + 2,
           "parallel: exactly one combined batch per playing block (all audio rows + 2 instruments)");

    expect(peakOf(outSerial) > 0.01, "fixture produces audible output");
    const double diff = maxAbsDiff(outSerial, outParallel);
    std::printf("[info] serial vs parallel max |diff| = %.3g (peak %.3f)\n", diff, peakOf(outSerial));
    expect(diff == 0.0, "serial-hint and parallel device output are bit-identical (summing order unchanged)");

    for (MixedFixture* f : { &a, &b })
    {
        expect(f->mutedRowInsert->callCount() == 0,
               "MUTED audio row: chain never processed (today's gain-0 skip preserved)");
        expect(f->offRowInsert->callCount() == 0, "OFF audio row: chain never processed");
        expect(f->preInsert->callCount() >= kBlocks && f->postInsert->callCount() >= kBlocks,
               "live row's Pre and Post inserts processed every playing block");
        expect(f->preInsert->samplesProcessed() == (std::uint64_t)kBlocks * kBlock,
               "live row's Pre insert covered exactly every playing sample (no dup/loss)");
        for (const auto& r : f->h.instRows)
        {
            expect(r.instrument != nullptr && r.instrument->blocksProcessed.load() > 0,
                   "instrument generated in the same combined batch");
        }
    }
    expect(gOverlaps.load() == 0, "no insert or instrument instance ever processed concurrently");
    expect(gInstrumentDoubleProcess.load() == 0, "no instrument processed twice in one block");
}

void testWorkersZeroSameJobCode()
{
    std::printf("\n-- --instrument-workers 0: serial mode through the same job code --\n");
    instrument_render::setConfiguredWorkerCountOverride(0);
    constexpr int kBlocks = 24;
    MixedFixture z;
    expect(z.h.engine.instrumentRenderWorkerCount() == 0, "override 0: no worker threads");
    const std::vector<float> outZero = z.h.runPlayingBlocks(kBlocks, /*serialHint*/ false);
    const auto st = z.h.engine.instrumentRenderPoolStats();
    expect(st.parallelBlocks == 0 && st.serialBlocks > 0, "override 0: every batch ran serially");
    const std::uint64_t jobsPerBlock = (std::uint64_t)countAudioRows(z.h) + 2;
    expect(st.jobsRunByCallback == (std::uint64_t)kBlocks * jobsPerBlock + 2 && st.jobsRunByWorkers == 0,
           "override 0: ALL jobs of every block ran on the callback lane through the job code");

    instrument_render::setConfiguredWorkerCountOverride(4);
    MixedFixture p;
    const std::vector<float> outPar = p.h.runPlayingBlocks(kBlocks, /*serialHint*/ false);
    expect(maxAbsDiff(outZero, outPar) == 0.0, "workers-0 output is bit-identical to parallel output");
}

void testCycleWrapSegments()
{
    std::printf("\n-- cycle wrap: split block, segment order, per-segment playhead, clip-content regression --\n");
    instrument_render::setConfiguredWorkerCountOverride(4);
    constexpr std::int64_t kLocL = 1000;
    constexpr std::int64_t kLocR = 3000; // span 2000; from 0 the wrap block is [2560,3072): 440 + 72
    constexpr int kBlocks = 40;          // several wraps

    const auto buildCycleHarness = [](Harness& h, TestInsertEffect** fxOut) {
        constexpr int kClipLen = kBlock * 64;
        h.addAudioTrackWithClip(makeNoiseClip(11, kClipLen)); // dry row (clip-content ground truth)
        h.addAudioTrackWithClip(makeNoiseClip(22, kClipLen)); // insert row (playhead + coverage)
        h.session.setLeftLocatorAtSample(kLocL);
        h.session.setRightLocatorAtSample(kLocR);
        h.transport.requestCycleEnabled(true);
        h.finishSetup();
        *fxOut = h.installInsert(h.audioTids[1], InsertStage::Pre, 0.7f, true);
    };

    Harness hs;
    TestInsertEffect* fxS = nullptr;
    buildCycleHarness(hs, &fxS);
    const std::vector<float> outSerial = hs.runPlayingBlocks(kBlocks, /*serialHint*/ true);

    Harness hp;
    TestInsertEffect* fxP = nullptr;
    buildCycleHarness(hp, &fxP);
    const std::vector<float> outParallel = hp.runPlayingBlocks(kBlocks, /*serialHint*/ false);

    expect(peakOf(outSerial) > 0.01, "cycle fixture produces audible output");
    expect(maxAbsDiff(outSerial, outParallel) == 0.0,
           "cycle: serial-hint and parallel output bit-identical across all wraps (stateful insert)");

    // Per-segment playhead context + exact coverage on the first wrap block ([2560,3072) → 440 at
    // t=2560, then 72 at t=1000). The insert chain must see BOTH segment positions in order.
    for (TestInsertEffect* fx : { fxS, fxP })
    {
        bool sawWrapPair = false;
        for (int i = 0; i + 1 < fx->callCount(); ++i)
        {
            const auto& c1 = fx->call(i);
            const auto& c2 = fx->call(i + 1);
            if (c1.blockId == c2.blockId && c1.timeInSamples == 2560 && c1.numSamples == 440
                && c2.timeInSamples == kLocL && c2.numSamples == 72)
            {
                sawWrapPair = true;
                break;
            }
        }
        expect(sawWrapPair,
               "wrap block: chain processed segment 1 (t=2560, 440) then segment 2 (t=L, 72), in order");
        // Coverage: every playing block contributes exactly kBlock samples to this audible row.
        expect(fx->samplesProcessed() == (std::uint64_t)kBlocks * kBlock,
               "wrap: insert chain covered every sample exactly once (no duplication, no loss)");
    }

    // CLIP-CONTENT regression for the stale-stage/wrong-offset defects: the dry row's contribution
    // in the wrap block's SECOND segment must equal clip content at [L, L+72) scaled by the same
    // per-channel gain as in a mid-clip reference block. (Block 0 frames [0,512) give the gain.)
    {
        const auto clip = makeNoiseClip(11, kBlock * 4); // same seed → identical material
        Harness hd;
        hd.addAudioTrackWithClip(makeNoiseClip(11, kBlock * 64));
        hd.session.setLeftLocatorAtSample(kLocL);
        hd.session.setRightLocatorAtSample(kLocR);
        hd.transport.requestCycleEnabled(true);
        hd.finishSetup();
        const std::vector<float> out = hd.runPlayingBlocks(8, /*serialHint*/ false); // wrap in block 5
        const auto& mat = clip->getAudio();
        // Per-channel gain from frames [16, 48) of block 0 (all samples non-trivial noise).
        double gL = 0.0, gR = 0.0;
        int gn = 0;
        for (int i = 16; i < 48; ++i)
        {
            if (std::fabs(mat.getSample(0, i)) > 1.0e-3f && std::fabs(mat.getSample(1, i)) > 1.0e-3f)
            {
                gL += (double)out[(size_t)(2 * i)] / (double)mat.getSample(0, i);
                gR += (double)out[(size_t)(2 * i + 1)] / (double)mat.getSample(1, i);
                ++gn;
            }
        }
        expect(gn > 0, "gain calibration window found usable samples");
        gL /= (double)gn;
        gR /= (double)gn;
        // Wrap block index 5 covers [2560,3072): frames [440,512) are timeline [L, L+72).
        double worst = 0.0;
        for (int j = 0; j < 72; ++j)
        {
            const size_t frame = (size_t)(5 * kBlock + 440 + j);
            const double expL = gL * (double)clip->getAudio().getSample(0, (int)(kLocL + j));
            const double expR = gR * (double)clip->getAudio().getSample(1, (int)(kLocL + j));
            worst = std::max(worst, std::fabs((double)out[2 * frame] - expL));
            worst = std::max(worst, std::fabs((double)out[2 * frame + 1] - expR));
        }
        std::printf("[info] wrap segment-2 clip-content max |err| = %.3g\n", worst);
        expect(worst < 1.0e-6,
               "wrap segment 2 fans CLIP content, not stale stage data (defect regression)");
    }
}

void testMonitoredRowGetsNoClipJob()
{
    std::printf("\n-- monitored row: no clip job; its chain runs via the monitoring pass --\n");
    instrument_render::setConfiguredWorkerCountOverride(4);
    constexpr int kBlocks = 16;
    Harness h;
    const TrackId monTid = h.addAudioTrackWithClip(makeConstantClip(0.25f, kBlock * 64));
    h.addAudioTrackWithClip(makeNoiseClip(7, kBlock * 64));
    h.finishSetup();
    TestInsertEffect* const monFx = h.installInsert(monTid, InsertStage::Pre, 0.5f, false);
    h.engine.setTrackInputMonitoringEnabled(monTid, true);

    const std::vector<float> out = h.runPlayingBlocks(kBlocks, /*serialHint*/ false);
    const auto st = h.engine.instrumentRenderPoolStats();
    // All audio rows EXCEPT the monitored one get a strip job (the default empty row included).
    const std::uint64_t jobsPerBlock = (std::uint64_t)countAudioRows(h) - 1;
    expect(st.jobsRunByCallback + st.jobsRunByWorkers == (std::uint64_t)kBlocks * jobsPerBlock,
           "the monitored row is excluded from the batch (one job per OTHER audio row only)");
    // The monitored row's chain still processes every callback — via the monitoring pass, on the
    // callback thread, with silent input here (no device inputs in this stub).
    expect(monFx->callCount() >= kBlocks,
           "monitored row's insert chain still ran every callback (monitoring pass)");
    // Its clip (constant 0.25) must NOT reach the output: with only noise row 2 audible, the mean
    // of the output is ~0 — a constant leak of 0.25 would shift it decisively.
    double mean = 0.0;
    for (const float v : out) { mean += (double)v; }
    mean /= (double)out.size();
    std::printf("[info] device output mean with monitored clip row = %.5f\n", mean);
    expect(std::fabs(mean) < 0.01, "monitored row's CLIP playback is suppressed (no clip job rendered it)");
}

void testCapacityFallbackNeverDropsRows()
{
    std::printf("\n-- capacity: > kMaxAudioStripJobs rows fall back to the serial path, nothing dropped --\n");
    instrument_render::setConfiguredWorkerCountOverride(4);
    constexpr int kRows = 200; // > 192 payload slots
    constexpr float kValue = 0.001f;
    constexpr int kBlocks = 4;

    Harness h;
    const auto material = makeConstantClip(kValue, kBlock * 8);
    for (int i = 0; i < kRows; ++i)
    {
        h.addAudioTrackWithClip(material);
    }
    h.finishSetup();
    const std::vector<float> outBig = h.runPlayingBlocks(kBlocks, /*serialHint*/ false);
    const auto st = h.engine.instrumentRenderPoolStats();
    expect(st.jobsRunByCallback + st.jobsRunByWorkers == 0,
           "over capacity: no jobs dispatched — the whole block rendered on the serial strip path");

    // Reference: ONE identical row. All 200 rows are identical, so the big session's output must
    // be exactly 200× the single-row output (same per-row gain chain; summation of equal values).
    Harness h1;
    h1.addAudioTrackWithClip(material);
    h1.finishSetup();
    const std::vector<float> outOne = h1.runPlayingBlocks(kBlocks, /*serialHint*/ false);
    expect(peakOf(outOne) > 0.0, "single-row reference is audible");
    double worstRel = 0.0;
    for (size_t i = 0; i < outOne.size(); ++i)
    {
        const double expected = (double)outOne[i] * (double)kRows;
        if (std::fabs(expected) > 1.0e-9)
        {
            worstRel = std::max(worstRel, std::fabs((double)outBig[i] - expected) / std::fabs(expected));
        }
    }
    std::printf("[info] 200-row output vs 200x single row: worst relative error = %.3g\n", worstRel);
    expect(worstRel < 1.0e-4, "over-capacity serial fallback rendered ALL 200 rows (none dropped)");
}

void testChainSwapAndDeviceCycleUnderJobs()
{
    std::printf("\n-- chain removal/swap + device stop/start under running jobs --\n");
    instrument_render::setConfiguredWorkerCountOverride(4);
    // Heavy instruments keep the summed estimate decisively above the pool's parallel threshold
    // (kMinParallelWorkMicros) on any CPU clock state — the stress must actually go parallel.
    MixedFixture f(/*instrumentBusy*/ 150000);
    Harness& h = f.h;
    h.engine.setInstrumentRenderSerialForDiagnostics(false);
    h.transport.requestSeek(0);
    h.transport.requestPlaybackIntent(PlaybackIntent::Playing);

    std::atomic<bool> stop{ false };
    std::atomic<std::uint64_t> callbacks{ 0 };
    const auto deviceLoop = [&] {
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
    };
    std::thread device(deviceLoop);

    int swaps = 0;
    int drainTimeouts = 0;
    for (int iter = 0; iter < 60; ++iter)
    {
        // INSERT chain swap on a row whose strip runs inside jobs: production removal publishes a
        // map without the instance, then the test seam installs a fresh one (publish-before-use).
        const TrackId tid = h.audioTids[(size_t)(3 + (iter % 4)) % h.audioTids.size()];
        h.pluginHost.removePlugin(tid);
        if (!h.engine.waitForAudioCallbackExit(1000.0)) { ++drainTimeouts; }
        (void)h.installInsert(tid, (iter % 2) == 0 ? InsertStage::Pre : InsertStage::Post, 0.6f,
                              (iter % 3) == 0);
        // INSTRUMENT host swap in the same combined batch (publish-before-destroy).
        const int k = iter % (int)h.instRows.size();
        h.publishInstruments(k);
        if (!h.engine.waitForAudioCallbackExit(1000.0)) { ++drainTimeouts; }
        h.instRows[(size_t)k].host.reset();
        h.instRows[(size_t)k].instrument = nullptr;
        auto host = std::make_unique<ExperimentalInstrumentHost>();
        host->prepareForDevice(kRate, kBlock);
        auto inst = std::make_unique<TestToneInstrument>(20000);
        h.instRows[(size_t)k].instrument = inst.get();
        (void)host->installInstrumentInstanceForTests(std::move(inst));
        host->enqueueMidiMessageFromMessageThread(
            juce::MidiMessage::noteOn(1, h.instRows[(size_t)k].note, (juce::uint8)100));
        h.instRows[(size_t)k].host = std::move(host);
        h.publishInstruments();
        ++swaps;
        h.engine.setInstrumentRenderSerialForDiagnostics((iter % 7) == 3);
        if (iter == 30)
        {
            stop.store(true, std::memory_order_release);
            device.join();
            h.engine.audioDeviceStopped();
            h.engine.audioDeviceAboutToStart(&h.device);
            stop.store(false, std::memory_order_release);
            device = std::thread(deviceLoop);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    stop.store(true, std::memory_order_release);
    device.join();
    h.transport.requestPlaybackIntent(PlaybackIntent::Stopped);
    h.engine.audioDeviceStopped();

    std::printf("[info] swaps=%d callbacks=%llu drainTimeouts=%d\n", swaps,
                (unsigned long long)callbacks.load(), drainTimeouts);
    expect(swaps == 60 && callbacks.load() > 100, "stress ran: 60 chain/host swaps under a cycling device");
    expect(drainTimeouts == 0, "every drain completed (jobs never outlive the callback)");
    expect(gDestroyedWhileProcessing.load() == 0, "no instance destroyed while inside processBlock");
    expect(gPreparedWhileProcessing.load() == 0, "no prepare/release overlapped processBlock");
    expect(gOverlaps.load() == 0, "no concurrent processBlock on one instance during the stress");
    const auto st = h.engine.instrumentRenderPoolStats();
    expect(st.parallelBlocks > 0, "stress actually exercised the parallel batch");
}

void testOfflineHelpersStillWork()
{
    std::printf("\n-- offline mixdown (serial by contract) still renders through the shared strip core --\n");
    instrument_render::setConfiguredWorkerCountOverride(4);
    constexpr int kBlocks = 12;

    const auto build = [](Harness& h) {
        constexpr int kClipLen = kBlock * 32;
        h.addAudioTrackWithClip(makeNoiseClip(31, kClipLen));
        h.addAudioTrackWithClip(makeNoiseClip(32, kClipLen));
        h.finishSetup();
        (void)h.installInsert(h.audioTids[0], InsertStage::Pre, 0.8f, true);
        (void)h.installInsert(h.audioTids[0], InsertStage::Post, 0.9f, false);
    };

    Harness rt;
    build(rt);
    const std::vector<float> realtime = rt.runPlayingBlocks(kBlocks, /*serialHint*/ false);
    const auto stRt = rt.engine.instrumentRenderPoolStats();
    const std::uint64_t jobsPerBlock = (std::uint64_t)countAudioRows(rt);
    expect(stRt.jobsRunByCallback + stRt.jobsRunByWorkers == (std::uint64_t)kBlocks * jobsPerBlock,
           "realtime reference dispatched one audio strip job per row per block");

    Harness off;
    build(off);
    const auto snap = off.session.loadSessionSnapshotForAudioThread();
    std::vector<float> offline;
    juce::AudioBuffer<float> blk(2, kBlock);
    float* ptrs[2] = { blk.getWritePointer(0), blk.getWritePointer(1) };
    expect(off.engine.beginOfflineRenderGate(), "offline gate engaged");
    for (int b = 0; b < kBlocks; ++b)
    {
        blk.clear();
        gCurrentBlockId.fetch_add(1, std::memory_order_acq_rel);
        off.engine.renderOfflineMixdownBlock(*snap, nullptr, (std::int64_t)b * kBlock, kBlock, ptrs, b == 0);
        for (int i = 0; i < kBlock; ++i) { offline.push_back(ptrs[0][i]); offline.push_back(ptrs[1][i]); }
    }
    expect(off.engine.endOfflineRenderGate(), "offline gate released");
    const auto stOff = off.engine.instrumentRenderPoolStats();
    expect(stOff.jobsRunByCallback + stOff.jobsRunByWorkers == 0, "offline render never used the pool");
    const double diff = maxAbsDiff(realtime, offline);
    std::printf("[info] realtime(parallel) vs offline max |diff| = %.3g\n", diff);
    expect(diff <= 1.0e-6, "offline output equals the realtime (parallel) output (shared strip core)");
}
} // namespace

int main()
{
    std::setvbuf(stdout, nullptr, _IONBF, 0); // progress visible even when a hang has to be killed
    juce::ScopedJuceInitialiser_GUI juceInit; // MessageManager: prepareForDevice requires the message thread
    std::printf("AudioStripParallelFocusedTests (block %d, %.0f Hz)\n", kBlock, kRate);
    testSerialVsParallelIdenticalMixedSession();
    testWorkersZeroSameJobCode();
    testCycleWrapSegments();
    testMonitoredRowGetsNoClipJob();
    testCapacityFallbackNeverDropsRows();
    testChainSwapAndDeviceCycleUnderJobs();
    testOfflineHelpersStillWork();
    std::printf("\n%d checks, %d failure(s)\n", checks, failures);
    return failures == 0 ? 0 : 1;
}

// Link seams — PlaybackEngine.cpp references InstrumentTrackController entry points this harness
// exercises only through no-op stubs (the placeholder controller is never dereferenced by them).
ProjectFileExperimentalInstrumentTrackV1 InstrumentTrackController::buildExperimentalInstrumentProjectBlock() const
{
    jassertfalse;
    return {};
}
void InstrumentTrackController::audioThread_scheduleTransportMidiForSegment(ExperimentalInstrumentHost&, std::int64_t, int,
                                                                           int, bool, int, int*, bool, bool) noexcept {}
void InstrumentTrackController::audioThread_flushTransportMidi(ExperimentalInstrumentHost&, int, int) noexcept {}
void InstrumentTrackController::audioThread_flushPendingTransportOffsInto(ExperimentalInstrumentHost&, int, int) noexcept {}
