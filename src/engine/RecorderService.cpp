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

    ensureWriterStopped();

    std::uint32_t cap = request.sampleFifoCapacity;
    if (cap == 0u)
    {
        cap = defaultFifoSizeSamples(request.sampleRate);
    }
    else
    {
        cap = nextPow2(cap);
    }

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
                                                   1u,
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

    juce::AudioBuffer<float> zeroBlock(1, kWriterScratchCap);
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
        RecordedTakeResult r;
        r.success = false;
        r.errorMessage = std::move(err);
        r.takeFile = outFile;
        r.targetTrackId = target;
        r.recordingStartSample = start;
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

    RecordedTakeResult result;
    result.success = true;
    result.errorMessage = {};
    result.takeFile = outFile;
    result.targetTrackId = target;
    result.recordingStartSample = start;
    result.intendedSampleCount = intended;
    result.actuallyWrittenSampleCount = written; // from FIFO only; `toPad` silence matches rest
    result.sampleRate = sr;
    result.droppedSampleCount = dropped;
    return result;
}

void RecorderService::tryPushPreviewFromBlock(const float* inputMono, int numSamples) noexcept
{
    if (inputMono == nullptr || numSamples <= 0 || ! previewFifo_ || ! isRecording_.load(std::memory_order_relaxed))
    {
        return;
    }
    if (previewFifo_->getFreeSpace() < 1)
    {
        return;
    }
    float mn = inputMono[0], mx = inputMono[0];
    for (int i = 1; i < numSamples; ++i)
    {
        const float s = inputMono[i];
        mn = juce::jmin(mn, s);
        mx = juce::jmax(mx, s);
    }
    int a = 0, b = 0, c = 0, d = 0;
    previewFifo_->prepareToWrite(1, a, b, c, d);
    const int n = b + d;
    if (n < 1)
    {
        return;
    }
    const int index = b > 0 ? a : c;
    previewBuffer_.data()[static_cast<size_t>(index)] = RecordingPreviewPeakBlock{mn, mx, numSamples};
    previewFifo_->finishedWrite(n);
}

void RecorderService::pushInputBlock(const float* inputMono, int numSamples) noexcept
{
    if (numSamples <= 0)
    {
        return;
    }
    if (! isRecording_.load(std::memory_order_acquire))
    {
        return;
    }
    if (inputMono == nullptr)
    {
        intendedSampleTotal_.fetch_add(numSamples, std::memory_order_relaxed);
        droppedSampleTotal_.fetch_add(numSamples, std::memory_order_relaxed);
        return;
    }

    intendedSampleTotal_.fetch_add(numSamples, std::memory_order_relaxed);
    tryPushPreviewFromBlock(inputMono, numSamples);

    if (sampleFifo_ == nullptr)
    {
        droppedSampleTotal_.fetch_add(numSamples, std::memory_order_relaxed);
        return;
    }

    const int free = sampleFifo_->getFreeSpace();
    const int toWrite = juce::jmin(free, numSamples);
    if (toWrite < numSamples)
    {
        const auto drop = static_cast<std::int64_t>(numSamples - toWrite);
        droppedSampleTotal_.fetch_add(drop, std::memory_order_relaxed);
    }
    if (toWrite == 0)
    {
        return;
    }

    int s1, z1, s2, z2;
    // `w` can be < `toWrite` (JUCE `AbstractFifo` may grant fewer than requested).
    sampleFifo_->prepareToWrite(toWrite, s1, z1, s2, z2);
    const int w = z1 + z2;
    if (w <= 0)
    {
        droppedSampleTotal_.fetch_add(static_cast<std::int64_t>(toWrite), std::memory_order_relaxed);
        return;
    }
    if (w < toWrite)
    {
        droppedSampleTotal_.fetch_add(static_cast<std::int64_t>(toWrite - w), std::memory_order_relaxed);
    }

    float* const buf = sampleBuffer_.data();
    {
        // Two-segment JUCE `AbstractFifo` write: first block may be the tail of the ring, then wrap.
        int done = 0;
        if (z1 > 0)
        {
            const int n1 = juce::jmin(w, z1);
            juce::FloatVectorOperations::copy(buf + s1, inputMono, n1);
            done = n1;
        }
        if (w > done)
        {
            juce::FloatVectorOperations::copy(buf + s2, inputMono + done, w - done);
        }
    }
    sampleFifo_->finishedWrite(w);
}

void RecorderService::writerThreadMain() noexcept
{
    juce::AudioBuffer<float> scratch(1, kWriterScratchCap);
    for (;;)
    {
        const int ready = sampleFifo_ != nullptr ? sampleFifo_->getNumReady() : 0;
        const bool run = writerRun_.load(std::memory_order_acquire);
        if (! run && ready == 0)
        {
            break;
        }
        if (ready == 0)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }

        const int toRead = juce::jmin(ready, kWriterScratchCap);
        int s1, z1, s2, z2;
        if (sampleFifo_ == nullptr)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        sampleFifo_->prepareToRead(toRead, s1, z1, s2, z2);
        const int n = z1 + z2;
        if (n <= 0)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }

        const float* const data = sampleBuffer_.data();
        if (z1 > 0)
        {
            scratch.copyFrom(0, 0, data + s1, n <= z1 ? n : z1);
        }
        if (n > z1 && z2 > 0)
        {
            scratch.copyFrom(0, z1, data + s2, n - z1);
        }

        if (takeWriter_ != nullptr)
        {
            if (takeWriter_->writeFromAudioSampleBuffer(scratch, 0, n))
            {
                samplesWrittenToFile_.fetch_add(static_cast<std::int64_t>(n), std::memory_order_relaxed);
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
