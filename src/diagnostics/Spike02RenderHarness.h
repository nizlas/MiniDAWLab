#pragma once

// ============================================================================
// SPIKE-02 DIAGNOSTIC SCAFFOLDING — NOT PRODUCT CODE.
//
// Isolated render-instance lifecycle / offline-throughput / contention / latency
// / tail measurement harness (canonical steering docs/PORTABLE_INSTRUMENTS_AND_PROXIES.md
// §9.4.4, §14, §15, §21 PID-004/PID-005, §22 P1D; tests T-09/T-10/T-15/T-16/T-29/T-31).
// Reached ONLY through the flag-gated SPIKE panel auto plans
// (`--spike01-state-capture --spike01-auto=S2…`); nothing in the product path
// includes or instantiates anything in this header.
//
// Core safety contract implemented here (SPIKE-02 task):
//   1. Primary state is captured on the message thread (by the panel).
//   2. The render instance is created on the message thread.
//   3. State restore happens on the message thread.
//   4. setNonRealtime + bus layout + prepareToPlay happen on the message thread.
//   5. Exclusive ownership is transferred to ONE dedicated low-priority worker.
//   6. processBlock is called ONLY from that worker.
//   7. No concurrent access happens while the worker owns the instance (the
//      panel state machine only polls atomics until the worker reports done).
//   8. Ownership returns to the message thread after processing.
//   9. releaseResources + destruction happen on the message thread.
// The live Primary instance is never processed, reset, re-prepared or otherwise
// used as the render instance; it is only read via `getStateInformation` on the
// message thread (the Save-path precedent) and compared BY POINTER VALUE.
//
// PRIVACY: raw state bytes live only in in-memory MemoryBlocks and are never
// logged or written to disk; logs contain sizes/hashes/timings/counters only.
// Temporary WAV artifacts go to the system temp directory and are deleted after
// metrics are computed.
//
// Removal: delete src/diagnostics/Spike02*.h, the S2* plans + wiring in
// Spike01StateCapturePanel.cpp/.h, and the `snapshotAudioLoad` callback in
// MainAppWindow.cpp.
// ============================================================================

#include <JuceHeader.h>

#include "diagnostics/Spike01Sha256.h"
#include "diagnostics/Spike02TailAndStats.h"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <map>
#include <memory>
#include <vector>

#if JUCE_WINDOWS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h> // GetThreadTimes (worker CPU-time metric); juce_core already pulled it in
#endif

namespace spike02
{

//==============================================================================
// Logging (same conventions as the SPIKE-01 session log; message thread AND
// worker thread append here — the lock makes interleaving safe; the worker only
// logs outside its processBlock loop, never per block)
//==============================================================================

inline void log(const juce::String& line)
{
    static juce::CriticalSection lock;
    const juce::ScopedLock sl(lock);
    const juce::File f = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
                             .getChildFile("MiniDAWLab")
                             .getChildFile("spike02-render-log.txt");
    f.getParentDirectory().createDirectory();
    f.appendText(juce::Time::getCurrentTime().toISO8601(true) + "  " + line + "\n", false, false,
                 "\n");
}

[[nodiscard]] inline bool onMessageThreadNow()
{
    auto* mm = juce::MessageManager::getInstanceWithoutCreating();
    return mm != nullptr && mm->isThisTheMessageThread();
}

[[nodiscard]] inline juce::String currentThreadIdHex()
{
    return juce::String::toHexString((juce::pointer_sized_int)juce::Thread::getCurrentThreadId());
}

[[nodiscard]] inline juce::String ptrHex(const void* p)
{
    return juce::String::toHexString((juce::pointer_sized_int)p);
}

//==============================================================================
// Deterministic MIDI schedules
//==============================================================================

struct ScheduledMidi
{
    std::int64_t sample = 0;
    std::uint8_t size = 3;
    std::uint8_t bytes[3] = { 0, 0, 0 };
};

[[nodiscard]] inline ScheduledMidi noteOn(std::int64_t s, int ch, int note, int vel)
{
    return { s, 3, { (std::uint8_t)(0x90 | (ch - 1)), (std::uint8_t)note, (std::uint8_t)vel } };
}
[[nodiscard]] inline ScheduledMidi noteOff(std::int64_t s, int ch, int note)
{
    return { s, 3, { (std::uint8_t)(0x80 | (ch - 1)), (std::uint8_t)note, 0 } };
}
[[nodiscard]] inline ScheduledMidi cc(std::int64_t s, int ch, int num, int val)
{
    return { s, 3, { (std::uint8_t)(0xB0 | (ch - 1)), (std::uint8_t)num, (std::uint8_t)val } };
}

/// Diagnostic organ workload replicating the verified TSE_pt2 Organ TOPOLOGY (SPIKE-01B-M
/// §28.0: destination receives channel-1 clip notes + CC11, channel-2 "Organ Lower" notes,
/// channel-3 "Organ pedal" notes at 180 BPM). The exact musical phrase is NOT copied from
/// the project (musical content stays out of diagnostics); this is a fixed synthetic
/// pattern with the same channel/CC structure and density, tiled to `totalSamples`.
[[nodiscard]] inline std::vector<ScheduledMidi> makeOrganPatternSchedule(const double sampleRate,
                                                                         const std::int64_t totalSamples)
{
    const auto sec = [sampleRate](const double s) { return (std::int64_t)std::llround(s * sampleRate); };
    const std::int64_t patternLen = sec(4.0);

    std::vector<ScheduledMidi> pattern;
    // ch1: upper-manual chords + CC11 expression ramp (the project's arranged CC lane type)
    for (const int n : { 60, 64, 67 })
    {
        pattern.push_back(noteOn(sec(0.00), 1, n, 100));
        pattern.push_back(noteOff(sec(1.80), 1, n));
    }
    for (const int n : { 65, 69, 72 })
    {
        pattern.push_back(noteOn(sec(2.00), 1, n, 100));
        pattern.push_back(noteOff(sec(3.80), 1, n));
    }
    for (int i = 0; i < 16; ++i)
    {
        pattern.push_back(cc(sec(0.05 + 0.25 * i), 1, 11, juce::jlimit(0, 127, 40 + 6 * i)));
    }
    // ch2: lower-manual line
    const int lower[4] = { 48, 52, 53, 55 };
    for (int i = 0; i < 4; ++i)
    {
        pattern.push_back(noteOn(sec(0.00 + i), 2, lower[i], 90));
        pattern.push_back(noteOff(sec(0.90 + i), 2, lower[i]));
    }
    // ch3: pedal
    pattern.push_back(noteOn(sec(0.00), 3, 36, 110));
    pattern.push_back(noteOff(sec(1.90), 3, 36));
    pattern.push_back(noteOn(sec(2.00), 3, 41, 110));
    pattern.push_back(noteOff(sec(3.90), 3, 41));

    std::vector<ScheduledMidi> out;
    for (std::int64_t base = 0; base < totalSamples; base += patternLen)
    {
        for (auto ev : pattern)
        {
            ev.sample += base;
            if (ev.sample < totalSamples)
            {
                out.push_back(ev);
            }
        }
    }
    std::sort(out.begin(), out.end(),
              [](const ScheduledMidi& a, const ScheduledMidi& b) { return a.sample < b.sample; });
    return out;
}

/// Tail-test phrase: chord (ch1) + pedal (ch3) + CC11, all Note Offs and sustain (CC64=0)
/// released at `releaseSec`. The final scheduled event lands exactly at `releaseSec`.
[[nodiscard]] inline std::vector<ScheduledMidi> makeTailPhraseSchedule(const double sampleRate,
                                                                       const double releaseSec)
{
    const auto sec = [sampleRate](const double s) { return (std::int64_t)std::llround(s * sampleRate); };
    std::vector<ScheduledMidi> out;
    out.push_back(cc(sec(0.05), 1, 11, 110));
    for (const int n : { 60, 64, 67 })
    {
        out.push_back(noteOn(sec(0.10), 1, n, 110));
    }
    out.push_back(noteOn(sec(0.10), 3, 36, 110));
    for (const int n : { 60, 64, 67 })
    {
        out.push_back(noteOff(sec(releaseSec), 1, n));
    }
    out.push_back(noteOff(sec(releaseSec), 3, 36));
    for (int ch = 1; ch <= 3; ++ch)
    {
        out.push_back(cc(sec(releaseSec), ch, 64, 0)); // sustain release
    }
    std::sort(out.begin(), out.end(),
              [](const ScheduledMidi& a, const ScheduledMidi& b) { return a.sample < b.sample; });
    return out;
}

/// Percussive tail-test excitation (second real plugin, drum-style): two hits, explicit
/// Note Offs; final event at `releaseSec`. Uses the same generic note range the product's
/// Phase-C drum-name probe already plays into state-copied clones (notes 24..55).
[[nodiscard]] inline std::vector<ScheduledMidi> makeDrumHitSchedule(const double sampleRate,
                                                                    const double releaseSec)
{
    const auto sec = [sampleRate](const double s) { return (std::int64_t)std::llround(s * sampleRate); };
    std::vector<ScheduledMidi> out;
    out.push_back(noteOn(sec(0.10), 1, 36, 110));
    out.push_back(noteOn(sec(0.35), 1, 38, 110));
    out.push_back(noteOff(sec(0.60), 1, 36));
    out.push_back(noteOff(sec(releaseSec), 1, 38));
    for (int ch = 1; ch <= 2; ++ch)
    {
        out.push_back(cc(sec(releaseSec), ch, 64, 0));
    }
    std::sort(out.begin(), out.end(),
              [](const ScheduledMidi& a, const ScheduledMidi& b) { return a.sample < b.sample; });
    return out;
}

/// P1D deterministic reset/chase prefix (steering §9.4.4): per used channel, sustain off,
/// all sound off (CC120), reset all controllers (CC121), all notes off (CC123), then the
/// initial CC11 value the pattern starts from. Injected at sample 0 before the workload.
[[nodiscard]] inline std::vector<ScheduledMidi> makeChasePrefix()
{
    std::vector<ScheduledMidi> out;
    for (int ch = 1; ch <= 3; ++ch)
    {
        out.push_back(cc(0, ch, 64, 0));
        out.push_back(cc(0, ch, 120, 0));
        out.push_back(cc(0, ch, 121, 0));
        out.push_back(cc(0, ch, 123, 0));
    }
    out.push_back(cc(0, 1, 11, 40));
    return out;
}

//==============================================================================
// Deterministic synthetic instrument with known fixed latency (F: latency
// preservation; A: lifecycle without a real plugin)
//==============================================================================

class SyntheticLatencyInstrument final : public juce::AudioProcessor
{
public:
    static constexpr int kFixedLatencySamples = 333;

    SyntheticLatencyInstrument()
        : juce::AudioProcessor(BusesProperties().withOutput("Out", juce::AudioChannelSet::stereo(), true))
    {
    }

    const juce::String getName() const override { return "Spike02SyntheticLatencyInstrument"; }

    void prepareToPlay(double sampleRate, int) override
    {
        juce::ignoreUnused(sampleRate);
        setLatencySamples(kFixedLatencySamples);
        samplePos_ = 0;
        pendingImpulses_.clear();
    }

    void releaseResources() override {}

    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override
    {
        buffer.clear();
        // A received noteOn at absolute sample T emits a single unit impulse at T + latency:
        // a harness that preserves leading latency and never trims/shifts must observe the
        // first nonzero output sample at exactly noteOnSample + kFixedLatencySamples.
        for (const auto meta : midi)
        {
            const auto* d = meta.data;
            if (meta.numBytes >= 3 && (d[0] & 0xF0) == 0x90 && d[2] > 0)
            {
                pendingImpulses_.push_back(samplePos_ + meta.samplePosition + kFixedLatencySamples);
            }
        }
        const std::int64_t blockStart = samplePos_;
        const std::int64_t blockEnd = samplePos_ + buffer.getNumSamples();
        for (std::size_t i = 0; i < pendingImpulses_.size();)
        {
            const std::int64_t t = pendingImpulses_[i];
            if (t < blockEnd)
            {
                if (t >= blockStart)
                {
                    const int off = (int)(t - blockStart);
                    for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
                    {
                        buffer.setSample(ch, off, 1.0f);
                    }
                }
                pendingImpulses_.erase(pendingImpulses_.begin() + (std::ptrdiff_t)i);
            }
            else
            {
                ++i;
            }
        }
        samplePos_ = blockEnd;
    }

    double getTailLengthSeconds() const override { return 0.0; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    bool hasEditor() const override { return false; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return "default"; }
    void changeProgramName(int, const juce::String&) override {}
    void getStateInformation(juce::MemoryBlock&) override {}
    void setStateInformation(const void*, int) override {}

private:
    std::int64_t samplePos_ = 0;
    std::vector<std::int64_t> pendingImpulses_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SyntheticLatencyInstrument)
};

//==============================================================================
// Render job configuration / results
//==============================================================================

struct RenderConfig
{
    double sampleRate = 48000.0;
    int blockSize = 512;
    bool nonRealtime = true;   ///< value passed to setNonRealtime before prepareToPlay
    bool realtimePaced = false; ///< pace processing to the wall clock (fallback mode)
    int yieldEveryBlocks = 0;  ///< cooperative yielding: sleep yieldMs every N blocks (0 = off)
    int yieldMs = 0;
    std::int64_t totalSamples = 0; ///< audio duration to render
    std::int64_t levelsFromSample = -1; ///< collect per-block levels from here (-1 = off)
    bool collectPerSecond = true;
    juce::String wavPath; ///< non-empty = write 32-bit-float stereo WAV (temp artifact)
    juce::String label;   ///< run identifier for the log
};

struct RenderResult
{
    juce::String label;
    bool finished = false;
    bool cancelled = false;
    double wallMs = 0.0;
    double audioSec = 0.0;
    double realtimeFactor = 0.0; ///< audioSec / wallSec
    std::uint64_t blocks = 0;
    std::uint64_t blocksWithMidi = 0;
    TimingStats blockTiming;
    LevelSummary levels;                   ///< whole run
    std::vector<BlockLevel> blockLevels;   ///< from levelsFromSample (tail analysis)
    std::vector<BlockLevel> perSecond;     ///< per rendered second
    std::int64_t firstNonZeroSample = -1;
    std::int64_t lastNonZeroSample = -1;
    bool allFinite = true;
    int latencySamplesAtStart = -1;  ///< getLatencySamples read on the worker before block 0
    int latencySamplesAtEnd = -1;
    double wavWriteMs = 0.0;
    std::int64_t wavBytes = 0;
    double cancelLatencyMs = -1.0; ///< cancel request -> worker loop exit
    juce::String workerThreadId;
    double workerCpuMs = -1.0; ///< kernel+user CPU of the worker thread (Windows)
    double processedAudioSec = 0.0; ///< actual audio processed (== audioSec unless cancelled)
};

//==============================================================================
// The single dedicated low-priority render worker
//==============================================================================

class RenderWorker final : public juce::Thread
{
public:
    /// The worker BORROWS `proc` with exclusive ownership from start until its state
    /// becomes Done (contract: the message thread must not touch `proc` in between).
    RenderWorker(juce::AudioProcessor& proc,
                 RenderConfig cfg,
                 std::vector<ScheduledMidi> schedule)
        : juce::Thread("Spike02RenderWorker"), proc_(proc), cfg_(std::move(cfg)),
          schedule_(std::move(schedule))
    {
    }

    ~RenderWorker() override { stopThread(10000); }

    void requestCancel() noexcept
    {
        cancelRequestMs_.store(juce::Time::getMillisecondCounterHiRes(), std::memory_order_relaxed);
        cancelRequested_.store(true, std::memory_order_release);
    }

    [[nodiscard]] bool isDone() const noexcept { return done_.load(std::memory_order_acquire); }
    [[nodiscard]] RenderResult& result() noexcept { return result_; } ///< valid once isDone()

    void run() override
    {
        result_.label = cfg_.label;
        result_.workerThreadId = currentThreadIdHex();

#if JUCE_WINDOWS
        FILETIME ftCreate{}, ftExit{}, ftKernel0{}, ftUser0{};
        const bool haveCpu0 =
            GetThreadTimes(GetCurrentThread(), &ftCreate, &ftExit, &ftKernel0, &ftUser0) != 0;
#endif

        // Same scratch-channel rule as the product host (ExperimentalInstrumentHost scratch
        // alloc and the Phase-C drum probe): the plugin may have more output buses than the
        // main stereo pair, so the process buffer must span jmax(2, totalIn, totalOut)
        // channels. Levels and the WAV artifact read only the main stereo pair (ch 0/1).
        const int totalCh = juce::jmax(2, juce::jmax(proc_.getTotalNumInputChannels(),
                                                     proc_.getTotalNumOutputChannels()));
        juce::AudioBuffer<float> buffer(totalCh, cfg_.blockSize);
        std::vector<float*> viewChans((std::size_t)totalCh, nullptr);
        juce::MidiBuffer midi;

        std::unique_ptr<juce::AudioFormatWriter> wavWriter;
        juce::File wavFile;
        if (cfg_.wavPath.isNotEmpty())
        {
            wavFile = juce::File(cfg_.wavPath);
            wavFile.getParentDirectory().createDirectory();
            wavFile.deleteFile();
            juce::WavAudioFormat fmt;
            if (auto stream = wavFile.createOutputStream())
            {
                wavWriter.reset(fmt.createWriterFor(stream.release(), cfg_.sampleRate, 2u, 32,
                                                    juce::StringPairArray(), 0));
            }
            if (wavWriter == nullptr)
            {
                log("worker[" + cfg_.label + "]: WAV writer creation FAILED path=" + cfg_.wavPath);
            }
        }

        result_.latencySamplesAtStart = proc_.getLatencySamples();
        result_.blockTiming = {};
        std::vector<double> blockMs;
        blockMs.reserve((std::size_t)juce::jmax<std::int64_t>(
            16, cfg_.totalSamples / juce::jmax(1, cfg_.blockSize) + 2));

        // Per-second accumulation
        double secSumSquares = 0.0;
        std::int64_t secSamples = 0;
        double secPeak = 0.0;
        std::int64_t nextSecondBoundary = (std::int64_t)cfg_.sampleRate;

        double runSumSquares = 0.0;
        std::int64_t runSamples = 0;

        std::size_t scheduleIdx = 0;
        std::int64_t pos = 0;
        int blocksSinceYield = 0;
        const double wallStart = juce::Time::getMillisecondCounterHiRes();

        while (pos < cfg_.totalSamples)
        {
            if (cancelRequested_.load(std::memory_order_acquire) || threadShouldExit())
            {
                result_.cancelled = true;
                const double reqMs = cancelRequestMs_.load(std::memory_order_relaxed);
                result_.cancelLatencyMs =
                    reqMs > 0.0 ? juce::Time::getMillisecondCounterHiRes() - reqMs : -1.0;
                break;
            }

            const int n = (int)juce::jmin<std::int64_t>(cfg_.blockSize, cfg_.totalSamples - pos);
            midi.clear();
            bool hadMidi = false;
            while (scheduleIdx < schedule_.size() && schedule_[scheduleIdx].sample < pos + n)
            {
                const auto& ev = schedule_[scheduleIdx];
                const int off = (int)juce::jmax<std::int64_t>(0, ev.sample - pos);
                midi.addEvent(ev.bytes, ev.size, off);
                hadMidi = true;
                ++scheduleIdx;
            }
            if (hadMidi)
            {
                ++result_.blocksWithMidi;
            }

            // The final block may be partial: hand the plugin a view of exactly n samples
            // (allowed: n <= prepared block size). No allocation — the view aliases `buffer`.
            for (int ch = 0; ch < totalCh; ++ch)
            {
                viewChans[(std::size_t)ch] = buffer.getWritePointer(ch);
            }
            juce::AudioBuffer<float> view(viewChans.data(), totalCh, n);
            view.clear();

            const double t0 = juce::Time::getMillisecondCounterHiRes();
            proc_.processBlock(view, midi);
            const double t1 = juce::Time::getMillisecondCounterHiRes();
            blockMs.push_back(t1 - t0);
            ++result_.blocks;

            // Levels / validity scan (n samples of the block are real audio)
            double blockPeak = 0.0;
            double blockSumSquares = 0.0;
            for (int ch = 0; ch < 2; ++ch)
            {
                const float* d = view.getReadPointer(ch);
                for (int i = 0; i < n; ++i)
                {
                    const float v = d[i];
                    if (!std::isfinite(v))
                    {
                        result_.allFinite = false;
                        continue;
                    }
                    const double a = std::abs((double)v);
                    if (a > 1.0e-7)
                    {
                        if (result_.firstNonZeroSample < 0)
                        {
                            result_.firstNonZeroSample = pos + i;
                        }
                        result_.lastNonZeroSample = pos + i;
                    }
                    blockPeak = juce::jmax(blockPeak, a);
                    blockSumSquares += a * a;
                }
            }
            const double blockRms = std::sqrt(blockSumSquares / (double)(2 * n));
            runSumSquares += blockSumSquares;
            runSamples += 2 * n;
            result_.levels.maxPeak = juce::jmax(result_.levels.maxPeak, blockPeak);

            if (cfg_.levelsFromSample >= 0 && pos + n > cfg_.levelsFromSample)
            {
                result_.blockLevels.push_back({ blockPeak, blockRms });
            }
            if (cfg_.collectPerSecond)
            {
                secPeak = juce::jmax(secPeak, blockPeak);
                secSumSquares += blockSumSquares;
                secSamples += 2 * n;
                if (pos + n >= nextSecondBoundary)
                {
                    result_.perSecond.push_back(
                        { secPeak, secSamples > 0 ? std::sqrt(secSumSquares / (double)secSamples) : 0.0 });
                    secPeak = 0.0;
                    secSumSquares = 0.0;
                    secSamples = 0;
                    nextSecondBoundary += (std::int64_t)cfg_.sampleRate;
                }
            }

            if (wavWriter != nullptr)
            {
                // Main stereo pair only (the planned proxy artifact is stereo).
                float* stereoChans[2] = { viewChans[0], viewChans[1] };
                juce::AudioBuffer<float> stereoView(stereoChans, 2, n);
                const double w0 = juce::Time::getMillisecondCounterHiRes();
                wavWriter->writeFromAudioSampleBuffer(stereoView, 0, n);
                result_.wavWriteMs += juce::Time::getMillisecondCounterHiRes() - w0;
            }

            pos += n;

            if (cfg_.realtimePaced)
            {
                const double audioMsSoFar = (double)pos * 1000.0 / cfg_.sampleRate;
                const double wallMsSoFar = juce::Time::getMillisecondCounterHiRes() - wallStart;
                const int sleepMs = (int)std::floor(audioMsSoFar - wallMsSoFar);
                if (sleepMs > 0)
                {
                    juce::Thread::sleep(juce::jmin(sleepMs, 250));
                }
            }
            else if (cfg_.yieldEveryBlocks > 0 && ++blocksSinceYield >= cfg_.yieldEveryBlocks)
            {
                blocksSinceYield = 0;
                juce::Thread::sleep(juce::jmax(1, cfg_.yieldMs));
            }
        }

        const double wallEnd = juce::Time::getMillisecondCounterHiRes();
        if (wavWriter != nullptr)
        {
            const double w0 = juce::Time::getMillisecondCounterHiRes();
            wavWriter.reset(); // flush + close
            result_.wavWriteMs += juce::Time::getMillisecondCounterHiRes() - w0;
            result_.wavBytes = wavFile.getSize();
        }

        result_.finished = !result_.cancelled;
        result_.wallMs = wallEnd - wallStart;
        result_.processedAudioSec = (double)pos / cfg_.sampleRate;
        result_.audioSec = (double)cfg_.totalSamples / cfg_.sampleRate;
        result_.realtimeFactor =
            result_.wallMs > 0.0 ? (result_.processedAudioSec * 1000.0) / result_.wallMs : 0.0;
        result_.blockTiming = computeTimingStats(std::move(blockMs));
        result_.levels.maxPeakDb = dbfsFromLinear(result_.levels.maxPeak);
        result_.levels.meanRms = runSamples > 0 ? std::sqrt(runSumSquares / (double)runSamples) : 0.0;
        result_.levels.meanRmsDb = dbfsFromLinear(result_.levels.meanRms);
        result_.levels.blocks = (std::size_t)result_.blocks;
        result_.latencySamplesAtEnd = proc_.getLatencySamples();

#if JUCE_WINDOWS
        if (haveCpu0)
        {
            FILETIME ftKernel1{}, ftUser1{};
            if (GetThreadTimes(GetCurrentThread(), &ftCreate, &ftExit, &ftKernel1, &ftUser1) != 0)
            {
                const auto toMs = [](const FILETIME& a, const FILETIME& b) {
                    ULARGE_INTEGER ua{}, ub{};
                    ua.LowPart = a.dwLowDateTime;
                    ua.HighPart = a.dwHighDateTime;
                    ub.LowPart = b.dwLowDateTime;
                    ub.HighPart = b.dwHighDateTime;
                    return (double)(ub.QuadPart - ua.QuadPart) / 10000.0; // 100ns -> ms
                };
                result_.workerCpuMs = toMs(ftKernel0, ftKernel1) + toMs(ftUser0, ftUser1);
            }
        }
#endif

        done_.store(true, std::memory_order_release);
    }

private:
    juce::AudioProcessor& proc_;
    const RenderConfig cfg_;
    const std::vector<ScheduledMidi> schedule_;
    RenderResult result_;
    std::atomic<bool> cancelRequested_{ false };
    std::atomic<double> cancelRequestMs_{ 0.0 };
    std::atomic<bool> done_{ false };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(RenderWorker)
};

//==============================================================================
// Message-thread controller: owns the isolated instance + the single worker
//==============================================================================

class Controller final
{
public:
    Controller()
    {
        formatManager_.addFormat(new juce::VST3PluginFormat());
        log("=== SPIKE-02 controller created; messageThread=" + currentThreadIdHex() + " ===");
    }

    ~Controller()
    {
        // Shutdown/abort path: cancel + join the worker, then tear the instance down on the
        // message thread (the destructor runs on the message thread with the panel).
        if (worker_ != nullptr)
        {
            log("controller dtor: cancelling in-flight worker");
            worker_->requestCancel();
            worker_->stopThread(10000);
            worker_.reset();
        }
        teardownIsolated("controller-dtor");
        log("=== SPIKE-02 controller destroyed ===");
    }

    //==========================================================================
    // State slots (raw bytes stay in RAM only; log carries size + SHA-256)
    //==========================================================================

    bool captureStateToSlot(juce::AudioPluginInstance& liveInst, const juce::String& slot)
    {
        jassert(onMessageThreadNow());
        juce::MemoryBlock mb;
        const double t0 = juce::Time::getMillisecondCounterHiRes();
        liveInst.getStateInformation(mb);
        const double t1 = juce::Time::getMillisecondCounterHiRes();
        log("captureStateToSlot[" + slot + "]: liveInstance=" + ptrHex(&liveInst) + " bytes="
            + juce::String((juce::int64)mb.getSize()) + " sha256="
            + juce::String(spike01::Sha256::hashHex(mb.getData(), mb.getSize())) + " ms="
            + juce::String(t1 - t0, 3) + " thread=" + currentThreadIdHex()
            + (onMessageThreadNow() ? " (message)" : " (OTHER)"));
        if (mb.getSize() == 0)
        {
            return false;
        }
        stateSlots_[slot] = std::move(mb);
        return true;
    }

    [[nodiscard]] bool hasSlot(const juce::String& slot) const
    {
        return stateSlots_.count(slot) > 0;
    }

    //==========================================================================
    // Isolated-instance lifecycle (message thread)
    //==========================================================================

    /// Create + optionally restore + configure + prepare an isolated render instance of the
    /// given plugin. Message thread only. Returns false on failure (logged).
    bool createIsolatedPlugin(const juce::PluginDescription& desc,
                              const RenderConfig& cfg,
                              const juce::String& stateSlot, // empty = no restore
                              const bool resetAfterPrepare)
    {
        jassert(onMessageThreadNow());
        teardownIsolated("pre-create");

        juce::String err;
        std::unique_ptr<juce::AudioPluginInstance> inst;
        const double t0 = juce::Time::getMillisecondCounterHiRes();
        try
        {
            inst = formatManager_.createPluginInstance(desc, cfg.sampleRate, cfg.blockSize, err);
        }
        catch (...)
        {
            log("createIsolatedPlugin: EXCEPTION during createPluginInstance");
            return false;
        }
        const double t1 = juce::Time::getMillisecondCounterHiRes();
        if (inst == nullptr)
        {
            log("createIsolatedPlugin: FAILED err=\"" + err + "\"");
            return false;
        }
        log("createIsolatedPlugin: OK name=\"" + inst->getName() + "\" instance=" + ptrHex(inst.get())
            + " createMs=" + juce::String(t1 - t0, 1) + " thread=" + currentThreadIdHex()
            + (onMessageThreadNow() ? " (message)" : " (OTHER)"));

        double restoreMs = 0.0;
        if (stateSlot.isNotEmpty())
        {
            const auto it = stateSlots_.find(stateSlot);
            if (it == stateSlots_.end())
            {
                log("createIsolatedPlugin: missing state slot '" + stateSlot + "'");
                return false;
            }
            const double r0 = juce::Time::getMillisecondCounterHiRes();
            inst->setStateInformation(it->second.getData(), (int)it->second.getSize());
            restoreMs = juce::Time::getMillisecondCounterHiRes() - r0;
        }

        prepareProcessor(*inst, cfg, resetAfterPrepare, "restoreMs=" + juce::String(restoreMs, 1)
                                                            + " slot=" + stateSlot);
        isolated_ = std::move(inst);
        isolatedIsPlugin_ = true;
        return true;
    }

    /// Create + prepare the deterministic synthetic instrument (no plugin binary involved).
    bool createIsolatedSynthetic(const RenderConfig& cfg)
    {
        jassert(onMessageThreadNow());
        teardownIsolated("pre-create-synth");
        auto inst = std::make_unique<SyntheticLatencyInstrument>();
        log("createIsolatedSynthetic: instance=" + ptrHex(inst.get()) + " thread="
            + currentThreadIdHex() + (onMessageThreadNow() ? " (message)" : " (OTHER)"));
        prepareProcessor(*inst, cfg, false, "synthetic");
        isolated_ = std::move(inst);
        isolatedIsPlugin_ = false;
        return true;
    }

    /// Re-configure + re-prepare the CURRENT isolated instance (e.g. new block size) without
    /// recreating it. Message thread only; no worker may be running.
    bool reprepareIsolated(const RenderConfig& cfg, const bool resetAfterPrepare)
    {
        jassert(onMessageThreadNow());
        if (isolated_ == nullptr || worker_ != nullptr)
        {
            return false;
        }
        isolated_->releaseResources();
        prepareProcessor(*isolated_, cfg, resetAfterPrepare, "reprepare");
        return true;
    }

    [[nodiscard]] juce::AudioProcessor* isolatedForMessageThreadChecks() noexcept
    {
        jassert(onMessageThreadNow());
        return worker_ == nullptr ? isolated_.get() : nullptr;
    }

    void teardownIsolated(const juce::String& why)
    {
        jassert(onMessageThreadNow());
        jassert(worker_ == nullptr); // never destroy while the worker owns the instance
        if (isolated_ == nullptr)
        {
            return;
        }
        const bool editorEverCreated = isolated_->getActiveEditor() != nullptr;
        isolated_->releaseResources();
        log("teardownIsolated(" + why + "): instance=" + ptrHex(isolated_.get())
            + " editorPresentAtTeardown=" + (editorEverCreated ? "TRUE(!)" : "false") + " thread="
            + currentThreadIdHex() + (onMessageThreadNow() ? " (message)" : " (OTHER)"));
        isolated_.reset();
    }

    //==========================================================================
    // Worker control (start on message thread; poll isDone from the panel timer)
    //==========================================================================

    bool startJob(const RenderConfig& cfg, std::vector<ScheduledMidi> schedule)
    {
        jassert(onMessageThreadNow());
        if (isolated_ == nullptr || worker_ != nullptr)
        {
            log("startJob[" + cfg.label + "]: REFUSED (no instance or worker busy)");
            return false;
        }
        lastConfig_ = cfg;
        worker_ = std::make_unique<RenderWorker>(*isolated_, cfg, std::move(schedule));
        log("startJob[" + cfg.label + "]: ownership -> worker; instance=" + ptrHex(isolated_.get())
            + " sr=" + juce::String(cfg.sampleRate, 0) + " bs=" + juce::String(cfg.blockSize)
            + " nonRealtime=" + (cfg.nonRealtime ? "true" : "false")
            + " paced=" + (cfg.realtimePaced ? "true" : "false")
            + " yieldEvery=" + juce::String(cfg.yieldEveryBlocks)
            + " audioSec=" + juce::String((double)cfg.totalSamples / cfg.sampleRate, 1)
            + " startedFromThread=" + currentThreadIdHex());
        worker_->startThread(juce::Thread::Priority::low);
        return true;
    }

    [[nodiscard]] bool jobRunning() const noexcept
    {
        return worker_ != nullptr && !worker_->isDone();
    }

    void requestCancel()
    {
        if (worker_ != nullptr)
        {
            log("requestCancel: from thread=" + currentThreadIdHex()
                + (onMessageThreadNow() ? " (message)" : " (OTHER)"));
            worker_->requestCancel();
        }
    }

    /// Join the finished worker and take its result (ownership returns to the message
    /// thread). Returns false when no finished worker exists.
    bool finishJob(RenderResult& out)
    {
        jassert(onMessageThreadNow());
        if (worker_ == nullptr || !worker_->isDone())
        {
            return false;
        }
        worker_->stopThread(10000);
        out = std::move(worker_->result());
        worker_.reset();
        logResult(out);
        results_.push_back(out);
        return true;
    }

    [[nodiscard]] const std::vector<RenderResult>& allResults() const noexcept { return results_; }
    [[nodiscard]] const RenderConfig& lastConfig() const noexcept { return lastConfig_; }

private:
    void prepareProcessor(juce::AudioProcessor& p,
                          const RenderConfig& cfg,
                          const bool resetAfterPrepare,
                          const juce::String& note)
    {
        // Same bus-coercion shape as the product's tryPrepareInstrumentLayout
        // (ExperimentalInstrumentHost.cpp): accept 0-in/stereo-out, else coerce.
        const int mainIn = p.getMainBusNumInputChannels();
        const int mainOut = p.getMainBusNumOutputChannels();
        if (!(mainIn == 0 && mainOut >= 2))
        {
            p.releaseResources();
            p.setPlayConfigDetails(0, 2, cfg.sampleRate, cfg.blockSize);
            juce::AudioProcessor::BusesLayout layout;
            layout.inputBuses.add(juce::AudioChannelSet::disabled());
            layout.outputBuses.add(juce::AudioChannelSet::stereo());
            if (!p.setBusesLayout(layout))
            {
                p.setPlayConfigDetails(0, 2, cfg.sampleRate, cfg.blockSize);
            }
        }
        p.setNonRealtime(cfg.nonRealtime);
        const double t0 = juce::Time::getMillisecondCounterHiRes();
        p.prepareToPlay(cfg.sampleRate, cfg.blockSize);
        const double t1 = juce::Time::getMillisecondCounterHiRes();
        if (resetAfterPrepare)
        {
            p.reset();
        }
        log("prepare: instance=" + ptrHex(&p) + " sr=" + juce::String(cfg.sampleRate, 0) + " bs="
            + juce::String(cfg.blockSize) + " nonRealtime=" + (cfg.nonRealtime ? "true" : "false")
            + " prepareMs=" + juce::String(t1 - t0, 1)
            + " reset=" + (resetAfterPrepare ? "true" : "false") + " latency="
            + juce::String(p.getLatencySamples()) + " mainOut="
            + juce::String(p.getMainBusNumOutputChannels()) + " totalOut="
            + juce::String(p.getTotalNumOutputChannels()) + " " + note + " thread="
            + currentThreadIdHex() + (onMessageThreadNow() ? " (message)" : " (OTHER)"));
    }

    void logResult(const RenderResult& r)
    {
        log("result[" + r.label + "]: finished=" + (r.finished ? "true" : "false") + " cancelled="
            + (r.cancelled ? "true" : "false") + " blocks=" + juce::String((juce::int64)r.blocks)
            + " blocksWithMidi=" + juce::String((juce::int64)r.blocksWithMidi) + " audioSec="
            + juce::String(r.processedAudioSec, 2) + "/" + juce::String(r.audioSec, 2) + " wallMs="
            + juce::String(r.wallMs, 1) + " rtFactor=" + juce::String(r.realtimeFactor, 2)
            + " blockMs{med=" + juce::String(r.blockTiming.medianMs, 3) + " p95="
            + juce::String(r.blockTiming.p95Ms, 3) + " max=" + juce::String(r.blockTiming.maxMs, 3)
            + "} peakDb=" + juce::String(r.levels.maxPeakDb, 1) + " meanRmsDb="
            + juce::String(r.levels.meanRmsDb, 1) + " firstNonZero="
            + juce::String(r.firstNonZeroSample) + " lastNonZero="
            + juce::String(r.lastNonZeroSample) + " allFinite=" + (r.allFinite ? "true" : "FALSE")
            + " latencyStart=" + juce::String(r.latencySamplesAtStart) + " latencyEnd="
            + juce::String(r.latencySamplesAtEnd) + " wavMs=" + juce::String(r.wavWriteMs, 1)
            + " wavBytes=" + juce::String(r.wavBytes) + " cancelLatencyMs="
            + juce::String(r.cancelLatencyMs, 2) + " workerThread=" + r.workerThreadId
            + " workerCpuMs=" + juce::String(r.workerCpuMs, 1));
    }

    juce::AudioPluginFormatManager formatManager_;
    std::unique_ptr<juce::AudioProcessor> isolated_;
    bool isolatedIsPlugin_ = false;
    std::unique_ptr<RenderWorker> worker_;
    std::map<juce::String, juce::MemoryBlock> stateSlots_;
    std::vector<RenderResult> results_;
    RenderConfig lastConfig_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(Controller)
};

} // namespace spike02
