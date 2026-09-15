// =============================================================================
// RecorderService.cpp  —  lifecycle + SPSC push, writer thread, duration-correct Wav
// =============================================================================
// The **audio** path (`pushInputBlock`) only touches: atomics, preallocated
// `std::vector<float>` + `juce::AbstractFifo`, and a small preview buffer — never `Session` /
// `SessionSnapshot` / UI, never locks, never heap, never blocking waits.
// The **writer** thread drains the FIFO to disk and **does not** close the `AudioFormatWriter`.
// **message thread** in `stopRecordingAndFinalize` appends **silence** so the on-disk sample count
// matches `intendedSampleTotal_` (see `status/DECISION_LOG.md` Phase 4).
// **File format:** mono **24-bit linear PCM** `.wav` at `BeginRecordingRequest::sampleRate`. There is
// no 16-bit fallback; if a 24-bit writer cannot be created, `beginRecording` fails.
// =============================================================================

#include "engine/RecorderService.h"

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>

namespace
{
constexpr int kPreviewFifoDepth = 256;
constexpr int kWriterScratchCap = 65536;
// Cubase-style project/record convention alignment: 24-bit PCM takes (rate from device, often 48 kHz).
constexpr int kRecordedTakeBitsPerSample = 24;
} // namespace

RecorderService::RecorderService() = default;

RecorderService::~RecorderService()
{
    ensureWriterStopped();
}

void RecorderService::armForRecording(TrackId trackId) noexcept
{
    if (trackId == kInvalidTrackId)
    {
        armedTrackId_.store(0, std::memory_order_relaxed);
        return;
    }
    armedTrackId_.store(static_cast<std::uint64_t>(trackId), std::memory_order_relaxed);
}

void RecorderService::disarm() noexcept
{
    armedTrackId_.store(0, std::memory_order_relaxed);
}

TrackId RecorderService::getArmedTrackId() const noexcept
{
    const auto v = armedTrackId_.load(std::memory_order_relaxed);
    return v == 0 ? kInvalidTrackId : static_cast<TrackId>(v);
}

juce::String RecorderService::getLastError() const
{
    const std::lock_guard<std::mutex> lock(serviceMutex_);
    return lastError_;
}

void RecorderService::resetInternalCountersForNewTake() noexcept
{
    intendedSampleTotal_.store(0, std::memory_order_relaxed);
    droppedSampleTotal_.store(0, std::memory_order_relaxed);
    samplesWrittenToFile_.store(0, std::memory_order_relaxed);
    writerWriteFailed_.store(false, std::memory_order_relaxed);
}

void RecorderService::ensureWriterStopped() noexcept
{
    isRecording_.store(false, std::memory_order_release);
    writerRun_.store(false, std::memory_order_release);
    if (writerThread_ && writerThread_->joinable())
    {
        writerThread_->join();
    }
    writerThread_.reset();
    closeTakeWriter();
}

void RecorderService::closeTakeWriter() noexcept
{
    if (takeWriter_ != nullptr)
    {
        (void) takeWriter_->flush();
    }
    takeWriter_.reset();
}

std::uint32_t RecorderService::nextPow2(std::uint32_t x) noexcept
{
    if (x <= 1u)
    {
        return 1u;
    }
    --x;
    x |= x >> 1u;
    x |= x >> 2u;
    x |= x >> 4u;
    x |= x >> 8u;
    x |= x >> 16u;
    return x + 1u;
}

std::uint32_t RecorderService::defaultFifoSizeSamples(const double sampleRate) noexcept
{
    if (sampleRate <= 0.0 || ! std::isfinite(sampleRate))
    {
        return 1u << 18; // 262144, safe fallback
    }
    const double need = juce::jmax(1.0, sampleRate * 5.0);
    const double capped = juce::jmin(need, static_cast<double>(std::numeric_limits<std::uint32_t>::max() - 1u));
    return nextPow2(static_cast<std::uint32_t>(capped));
}

bool RecorderService::beginRecording(const BeginRecordingRequest& request)
{
    const std::lock_guard<std::mutex> lock(serviceMutex_);
    lastError_.clear();

    if (isRecording_.load(std::memory_order_relaxed))
    {
        lastError_ = "Already recording";
        return false;
    }
    lastTakeFile_ = juce::File();
    if (request.targetTrackId == kInvalidTrackId || request.takeFile == juce::File())
    {
        lastError_ = "Invalid take file or target track";
        return false;
    }
    if (request.sampleRate <= 0.0 || ! std::isfinite(request.sampleRate))
    {
        lastError_ = "Invalid sample rate";
        return false;
    }
    const auto armed = static_cast<TrackId>(armedTrackId_.load(std::memory_order_relaxed));
    if (armed != request.targetTrackId)
    {
        lastError_ = "Target track does not match armed track";
        return false;
    }

    if (request.numChannels < 1 || request.numChannels > 2)
    {
        lastError_ = "Unsupported capture channel count (1 or 2)";
        return false;
    }

    ensureWriterStopped();

    // FIFO capacity is in SAMPLES; stereo takes store interleaved frames, so scale the frame
    // budget by the channel count (power-of-two capacity stays even → frame alignment holds).
    std::uint32_t cap = request.sampleFifoCapacity;
    if (cap == 0u)
    {
        cap = defaultFifoSizeSamples(request.sampleRate);
    }
    cap = nextPow2(cap * static_cast<std::uint32_t>(request.numChannels));

    sampleBuffer_.assign(static_cast<size_t>(cap), 0.0f);
    sampleFifo_ = std::make_unique<juce::AbstractFifo>(static_cast<int>(cap));

    previewBuffer_.assign(static_cast<size_t>(kPreviewFifoDepth), RecordingPreviewPeakBlock{});
    previewFifo_ = std::make_unique<juce::AbstractFifo>(kPreviewFifoDepth);

    resetInternalCountersForNewTake();

    auto out = std::make_unique<juce::FileOutputStream>(request.takeFile);
    if (out->failedToOpen())
    {
        lastError_ = "Could not open take file for writing";
        sampleFifo_.reset();
        previewFifo_.reset();
        sampleBuffer_.clear();
        previewBuffer_.clear();
        return false;
    }

    // JUCE: `createWriterFor` takes ownership of the stream; in this JUCE build it returns
    // `AudioFormatWriter*` (raw). Wrap in `std::unique_ptr` — **24-bit PCM only**, no 16-bit fallback.
    takeWriter_.reset (wavFormat_.createWriterFor (out.release(),
                                                   request.sampleRate,
                                                   static_cast<unsigned int>(request.numChannels),
                                                   kRecordedTakeBitsPerSample,
                                                   juce::StringPairArray(),
                                                   0));

    if (takeWriter_ == nullptr)
    {
        lastError_ = "Could not create 24-bit PCM WAV writer";
        sampleFifo_.reset();
        previewFifo_.reset();
        sampleBuffer_.clear();
        previewBuffer_.clear();
        return false;
    }
    if (takeWriter_->getBitsPerSample() != kRecordedTakeBitsPerSample)
    {
        takeWriter_.reset();
        lastError_ = "WAV writer is not 24-bit PCM (refusing non-conforming writer)";
        sampleFifo_.reset();
        previewFifo_.reset();
        sampleBuffer_.clear();
        previewBuffer_.clear();
        return false;
    }

    lastTakeFile_ = request.takeFile;
    activeRecordingStartSample_.store(request.recordingStartSample, std::memory_order_relaxed);
    activeSampleRate_.store(request.sampleRate, std::memory_order_relaxed);
    recordingNumChannels_.store(request.numChannels, std::memory_order_relaxed);
    recordingInputPhysA_.store(request.inputPhysicalChannelA, std::memory_order_relaxed);
    recordingInputPhysB_.store(request.inputPhysicalChannelB, std::memory_order_relaxed);
    recordingTrackId_.store(static_cast<std::uint64_t>(request.targetTrackId), std::memory_order_relaxed);

    writerRun_.store(true, std::memory_order_release);
    writerThread_ = std::make_unique<std::thread>([this]() { writerThreadMain(); });

    isRecording_.store(true, std::memory_order_release);
    return true;
}

bool RecorderService::appendSilencePaddingToMeetIntendedCount(const std::int64_t numSilenceSamples)
{
    if (numSilenceSamples == 0)
    {
        return true;
    }
    if (numSilenceSamples < 0 || takeWriter_ == nullptr)
    {
        return false;
    }

    // `numSilenceSamples` is in FRAMES; the zero block matches the take's channel count.
    const int nch = juce::jlimit(1, 2, recordingNumChannels_.load(std::memory_order_relaxed));
    juce::AudioBuffer<float> zeroBlock(nch, kWriterScratchCap);
    zeroBlock.clear();

    std::int64_t remaining = numSilenceSamples;
    while (remaining > 0)
    {
        const int chunk = static_cast<int>(std::min(remaining, static_cast<std::int64_t>(kWriterScratchCap)));
        if (chunk <= 0)
        {
            return false;
        }
        if (! takeWriter_->writeFromAudioSampleBuffer(zeroBlock, 0, chunk))
        {
            return false;
        }
        remaining -= static_cast<std::int64_t>(chunk);
    }
    return true;
}

RecordedTakeResult RecorderService::stopRecordingAndFinalize()
{
    const std::lock_guard<std::mutex> lock(serviceMutex_);
    lastError_.clear();

    if (! isRecording_.load(std::memory_order_relaxed))
    {
        RecordedTakeResult r;
        r.success = false;
        r.errorMessage = "Not recording";
        return r;
    }

    isRecording_.store(false, std::memory_order_release);
    writerRun_.store(false, std::memory_order_release);

    if (writerThread_ && writerThread_->joinable())
    {
        writerThread_->join();
    }
    writerThread_.reset();

    // Snapshot **before** clearing lane state; writer has drained the FIFO to disk.
    const auto target = static_cast<TrackId>(recordingTrackId_.load(std::memory_order_relaxed));
    const auto start = activeRecordingStartSample_.load(std::memory_order_relaxed);
    const double sr = activeSampleRate_.load(std::memory_order_relaxed);
    const auto intended = intendedSampleTotal_.load(std::memory_order_relaxed);
    const auto dropped = droppedSampleTotal_.load(std::memory_order_relaxed);
    const auto written = samplesWrittenToFile_.load(std::memory_order_relaxed);
    const bool writeFailed = writerWriteFailed_.load(std::memory_order_relaxed);
    const int takeNumChannels = recordingNumChannels_.load(std::memory_order_relaxed);
    const juce::File outFile = lastTakeFile_;

    const auto fail = [&](juce::String err) {
        lastError_ = err;
        closeTakeWriter();
        sampleFifo_.reset();
        previewFifo_.reset();
        sampleBuffer_.clear();
        previewBuffer_.clear();
        lastTakeFile_ = juce::File();
        recordingTrackId_.store(0, std::memory_order_relaxed);
        activeRecordingStartSample_.store(0, std::memory_order_relaxed);
        activeSampleRate_.store(0.0, std::memory_order_relaxed);
        recordingNumChannels_.store(1, std::memory_order_relaxed);
        recordingInputPhysA_.store(-1, std::memory_order_relaxed);
        recordingInputPhysB_.store(-1, std::memory_order_relaxed);
        RecordedTakeResult r;
        r.success = false;
        r.errorMessage = std::move(err);
        r.takeFile = outFile;
        r.targetTrackId = target;
        r.recordingStartSample = start;
        r.numChannels = takeNumChannels;
        r.intendedSampleCount = intended;
        r.actuallyWrittenSampleCount = written;
        r.sampleRate = sr;
        r.droppedSampleCount = dropped;
        return r;
    };

    if (outFile.getFullPathName().isEmpty())
    {
        return fail("Internal error: no take file path");
    }

    if (writeFailed)
    {
        return fail("WAV write failed (disk I/O; take may be incomplete — see intended vs written count)");
    }

    // Duration invariant: on-disk **mono** sample count must equal `intended` (silence = dropped
    // samples that never reached the ring, plus any theoretical gap; normally written + pad).
    const std::int64_t toPad = intended - written;
    if (toPad < 0)
    {
        return fail("Internal error: more samples written than intended (invariant broken)");
    }

    if (! appendSilencePaddingToMeetIntendedCount(toPad))
    {
        return fail("WAV write failed while appending silence for overrun/drop samples");
    }

    if (! takeWriter_->flush())
    {
        return fail("WAV flush failed after finalize");
    }

    closeTakeWriter();

    sampleFifo_.reset();
    previewFifo_.reset();
    sampleBuffer_.clear();
    previewBuffer_.clear();

    lastTakeFile_ = juce::File();
    recordingTrackId_.store(0, std::memory_order_relaxed);
    activeRecordingStartSample_.store(0, std::memory_order_relaxed);
    activeSampleRate_.store(0.0, std::memory_order_relaxed);
    recordingNumChannels_.store(1, std::memory_order_relaxed);
    recordingInputPhysA_.store(-1, std::memory_order_relaxed);
    recordingInputPhysB_.store(-1, std::memory_order_relaxed);

    RecordedTakeResult result;
    result.success = true;
    result.errorMessage = {};
    result.takeFile = outFile;
    result.targetTrackId = target;
    result.recordingStartSample = start;
    result.numChannels = takeNumChannels;
    result.intendedSampleCount = intended;
    result.actuallyWrittenSampleCount = written; // from FIFO only; `toPad` silence matches rest
    result.sampleRate = sr;
    result.droppedSampleCount = dropped;
    return result;
}

void RecorderService::tryPushPreviewFromBlock(const float* inputA,
                                              const float* inputB,
                                              int numFrames) noexcept
{
    if (inputA == nullptr || numFrames <= 0 || ! previewFifo_ || ! isRecording_.load(std::memory_order_relaxed))
    {
        return;
    }
    if (previewFifo_->getFreeSpace() < 1)
    {
        return;
    }
    float mn = inputA[0], mx = inputA[0];
    for (int i = 1; i < numFrames; ++i)
    {
        const float s = inputA[i];
        mn = juce::jmin(mn, s);
        mx = juce::jmax(mx, s);
    }
    if (inputB != nullptr)
    {
        for (int i = 0; i < numFrames; ++i)
        {
            const float s = inputB[i];
            mn = juce::jmin(mn, s);
            mx = juce::jmax(mx, s);
        }
    }
    int a = 0, b = 0, c = 0, d = 0;
    previewFifo_->prepareToWrite(1, a, b, c, d);
    const int n = b + d;
    if (n < 1)
    {
        return;
    }
    const int index = b > 0 ? a : c;
    previewBuffer_.data()[static_cast<size_t>(index)] = RecordingPreviewPeakBlock{mn, mx, numFrames};
    previewFifo_->finishedWrite(n);
}

void RecorderService::pushInputBlock(const float* inputA,
                                     const float* inputB,
                                     int numFrames) noexcept
{
    if (numFrames <= 0)
    {
        return;
    }
    if (! isRecording_.load(std::memory_order_acquire))
    {
        return;
    }
    const int nch = juce::jlimit(1, 2, recordingNumChannels_.load(std::memory_order_relaxed));
    if (inputA == nullptr)
    {
        intendedSampleTotal_.fetch_add(numFrames, std::memory_order_relaxed);
        droppedSampleTotal_.fetch_add(numFrames, std::memory_order_relaxed);
        return;
    }

    intendedSampleTotal_.fetch_add(numFrames, std::memory_order_relaxed);
    tryPushPreviewFromBlock(inputA, nch == 2 ? inputB : nullptr, numFrames);

    if (sampleFifo_ == nullptr)
    {
        droppedSampleTotal_.fetch_add(numFrames, std::memory_order_relaxed);
        return;
    }

    // Whole-frame accounting: the ring stores interleaved samples for stereo takes, and every
    // write/read is a multiple of `nch`, so ring positions stay frame-aligned (capacity is a
    // power of two, hence even).
    const int freeFrames = sampleFifo_->getFreeSpace() / nch;
    const int framesToWrite = juce::jmin(freeFrames, numFrames);
    if (framesToWrite < numFrames)
    {
        droppedSampleTotal_.fetch_add(static_cast<std::int64_t>(numFrames - framesToWrite),
                                      std::memory_order_relaxed);
    }
    if (framesToWrite == 0)
    {
        return;
    }

    int s1, z1, s2, z2;
    // Granted size can be < requested (JUCE `AbstractFifo`); floor it to whole frames.
    sampleFifo_->prepareToWrite(framesToWrite * nch, s1, z1, s2, z2);
    const int grantedSamples = z1 + z2;
    const int framesGranted = grantedSamples / nch;
    if (framesGranted <= 0)
    {
        droppedSampleTotal_.fetch_add(static_cast<std::int64_t>(framesToWrite),
                                      std::memory_order_relaxed);
        return;
    }
    if (framesGranted < framesToWrite)
    {
        droppedSampleTotal_.fetch_add(static_cast<std::int64_t>(framesToWrite - framesGranted),
                                      std::memory_order_relaxed);
    }

    float* const buf = sampleBuffer_.data();
    if (nch == 1)
    {
        // Two-segment JUCE `AbstractFifo` write: first block may be the tail of the ring, then wrap.
        int done = 0;
        if (z1 > 0)
        {
            const int n1 = juce::jmin(framesGranted, z1);
            juce::FloatVectorOperations::copy(buf + s1, inputA, n1);
            done = n1;
        }
        if (framesGranted > done)
        {
            juce::FloatVectorOperations::copy(buf + s2, inputA + done, framesGranted - done);
        }
    }
    else
    {
        // Interleave L/R frames across the (up to) two ring segments. Null right channel is
        // captured as silence (defensive; the callback passes both pointers for stereo takes).
        int samplesDone = 0;
        const int totalSamples = framesGranted * nch;
        while (samplesDone < totalSamples)
        {
            const int frame = samplesDone / nch;
            const bool right = (samplesDone % nch) != 0;
            const float v = right ? (inputB != nullptr ? inputB[frame] : 0.0f) : inputA[frame];
            const int idx = samplesDone < z1 ? (s1 + samplesDone) : (s2 + (samplesDone - z1));
            buf[idx] = v;
            ++samplesDone;
        }
    }
    sampleFifo_->finishedWrite(framesGranted * nch);
}

void RecorderService::writerThreadMain() noexcept
{
    const int nch = juce::jlimit(1, 2, recordingNumChannels_.load(std::memory_order_relaxed));
    juce::AudioBuffer<float> scratch(nch, kWriterScratchCap);
    for (;;)
    {
        // The ring holds interleaved samples for stereo takes; only whole frames are drained
        // (pushes are frame-aligned, so `ready` is always a frame multiple once settled).
        const int readyFrames = (sampleFifo_ != nullptr ? sampleFifo_->getNumReady() : 0) / nch;
        const bool run = writerRun_.load(std::memory_order_acquire);
        if (! run && readyFrames == 0)
        {
            break;
        }
        if (readyFrames == 0)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }

        const int framesToRead = juce::jmin(readyFrames, kWriterScratchCap);
        int s1, z1, s2, z2;
        if (sampleFifo_ == nullptr)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        sampleFifo_->prepareToRead(framesToRead * nch, s1, z1, s2, z2);
        const int grantedSamples = z1 + z2;
        const int frames = grantedSamples / nch;
        if (frames <= 0)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        const int n = frames * nch;

        const float* const data = sampleBuffer_.data();
        if (nch == 1)
        {
            if (z1 > 0)
            {
                scratch.copyFrom(0, 0, data + s1, n <= z1 ? n : z1);
            }
            if (n > z1 && z2 > 0)
            {
                scratch.copyFrom(0, z1, data + s2, n - z1);
            }
        }
        else
        {
            // Deinterleave the (up to) two ring segments into channel-planar scratch.
            float* const outL = scratch.getWritePointer(0);
            float* const outR = scratch.getWritePointer(1);
            for (int i = 0; i < n; ++i)
            {
                const float v = i < z1 ? data[s1 + i] : data[s2 + (i - z1)];
                if ((i & 1) == 0)
                {
                    outL[i / 2] = v;
                }
                else
                {
                    outR[i / 2] = v;
                }
            }
        }

        if (takeWriter_ != nullptr)
        {
            if (takeWriter_->writeFromAudioSampleBuffer(scratch, 0, frames))
            {
                samplesWrittenToFile_.fetch_add(static_cast<std::int64_t>(frames), std::memory_order_relaxed);
            }
            else
            {
                writerWriteFailed_.store(true, std::memory_order_release);
            }
        }
        if (sampleFifo_ != nullptr)
        {
            sampleFifo_->finishedRead(n);
        }
    }
    // `takeWriter_` is closed in `stopRecordingAndFinalize` after padding (or in `fail` paths).
}

bool RecorderService::isRecording() const noexcept
{
    return isRecording_.load(std::memory_order_relaxed);
}

TrackId RecorderService::getRecordingTrackId() const noexcept
{
    if (! isRecording())
    {
        return kInvalidTrackId;
    }
    const auto t = recordingTrackId_.load(std::memory_order_relaxed);
    return t == 0u ? kInvalidTrackId : static_cast<TrackId>(t);
}

std::int64_t RecorderService::getRecordingStartSample() const noexcept
{
    if (! isRecording())
    {
        return 0;
    }
    return activeRecordingStartSample_.load(std::memory_order_relaxed);
}

double RecorderService::getRecordingSampleRate() const noexcept
{
    if (! isRecording())
    {
        return 0.0;
    }
    return activeSampleRate_.load(std::memory_order_relaxed);
}

std::int64_t RecorderService::getRecordedSampleCount() const noexcept
{
    return intendedSampleTotal_.load(std::memory_order_relaxed);
}

std::int64_t RecorderService::getActuallyWrittenSampleCount() const noexcept
{
    return samplesWrittenToFile_.load(std::memory_order_relaxed);
}

std::int64_t RecorderService::getDroppedSampleCount() const noexcept
{
    return droppedSampleTotal_.load(std::memory_order_relaxed);
}

bool RecorderService::drainNextPreviewBlock(RecordingPreviewPeakBlock& out) noexcept
{
    if (previewFifo_ == nullptr || previewFifo_->getNumReady() < 1)
    {
        return false;
    }
    int a, b, c, d;
    previewFifo_->prepareToRead(1, a, b, c, d);
    const int n = b + d;
    if (n < 1)
    {
        return false;
    }
    const int index = b > 0 ? a : c;
    out = previewBuffer_.data()[static_cast<size_t>(index)];
    previewFifo_->finishedRead(n);
    return true;
}
