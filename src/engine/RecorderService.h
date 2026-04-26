#pragma once

// =============================================================================
// RecorderService.h / RecorderService.cpp — Phase 4 minimal mono input capture
// =============================================================================
//
// File role: define a **message-thread-owned** service that owns arm/begin/finalize
// policy for a single in-flight take, a **lock-free (SPSC) mono sample** path
// for the **audio** thread (`pushInputBlock`), a **background writer** that drains
// the sample ring to disk, and a **separate SPSC** path for best-effort preview
// min/max blocks for the **recording** lane UI (no UI wiring in this file).
//
// Deliberate non-responsibilities
//   * Does **not** call `Transport` or `Session`, does not read `SessionSnapshot`
//   * Does **not** create `PlacedClip` or publish snapshots — the app root / coordinator
//     does that **after** `stopRecordingAndFinalize` returns a complete result.
//   * Does not validate “project saved” or construct take paths; the coordinator
//     passes a fully specified `juce::File` in `BeginRecordingRequest`.
//   * Does not open or reconfigure the audio device (e.g. `initialiseWithDefaultDevices(1,2)`).
//
// Threading
//   * Arm / disarm, begin, stop, and preview **drain** are **message thread** (or
//     any **non-**realtime app thread) unless stated otherwise.
//   * `pushInputBlock` is **audio / realtime** only: no locks, no heap, no
//     `Session` / `SessionSnapshot` / UI; only preallocated FIFO + simple atomics.
//   * The **writer thread** is owned and joined here; it touches **only** the
//     `AudioFormatWriter` and sample buffer storage — never `Session` / `Transport`.
//
// JUCE: `juce::AbstractFifo` manages indices into a pre-sized `std::vector<float>`;
//       take files are **mono, 24-bit linear PCM** `.wav` via `WavAudioFormat` / `AudioFormatWriter`
//       on the **writer** thread and on finalize (silence padding); input buffers are float (JUCE
//       converts to fixed-point for the file). **No** 16-bit fallback — if 24-bit creation fails,
//       `beginRecording` returns false.
//
// See: `docs/PHASE_PLAN.md` Phase 4, `status/DECISION_LOG.md` (Phase 4 entry).
// =============================================================================

#include "domain/Track.h"

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>

// ---------------------------------------------------------------------------
// beginRecording: caller supplies everything; RecorderService does not pick paths.
// ---------------------------------------------------------------------------
struct BeginRecordingRequest
{
    // Absolute path; parent directory must exist; coordinator is responsible
    // for <projectDir>/takes/ and uniqueness.
    juce::File takeFile;

    // Must match `getArmedTrackId()` in single-armed policy.
    TrackId targetTrackId = kInvalidTrackId;

    // Timeline-absolute first sample of this take (device samples, same as Transport playhead).
    std::int64_t recordingStartSample = 0;

    // Device / take sample rate; used for default FIFO size (≈5 s at rate) and WAV header.
    // The recorded file is mono **24-bit PCM** at this rate (product convention often 48 kHz,
    // matching a typical Cubase-style project/record profile; not forced here).
    double sampleRate = 0.0;

    // 0 = derive capacity as next power of two >= (sampleRate * 5) samples (steering: ~5 s).
    // Non-zero = override (tests / future tuning). Must be a power of two; if not, it is
    // rounded up to a power of two in beginRecording.
    std::uint32_t sampleFifoCapacity = 0;
};

// ---------------------------------------------------------------------------
// stopRecordingAndFinalize: complete outcome for the coordinator and Session
// (clip creation happens **after** this returns; not done here).
// ---------------------------------------------------------------------------
struct RecordedTakeResult
{
    bool success = false;
    juce::String errorMessage;

    // On `success`, a mono **24-bit PCM WAV** at `sampleRate` (see `BeginRecordingRequest`).
    juce::File takeFile;
    TrackId targetTrackId = kInvalidTrackId;
    std::int64_t recordingStartSample = 0;

    // Intended logical length: sum of all `numSamples` passed to `pushInputBlock` while
    // recording (includes overrun / drop accounting; the finalized WAV is padded with silence
    // so on-disk length matches this, unless `success` is false).
    std::int64_t intendedSampleCount = 0;

    // Mono samples read from the FIFO and successfully written to the WAV (before optional
    // tail silence padding). `droppedSampleCount` samples were not stored in the FIFO; they
    // appear as **silence** in the file after finalization, not in this count.
    std::int64_t actuallyWrittenSampleCount = 0;

    double sampleRate = 0.0;

    // Samples that could not be enqueued in the realtime path (or null input, etc.); the file
    // is extended with that many silence samples on finalize (when `success` is true).
    std::int64_t droppedSampleCount = 0;
};

// One preview block per `pushInputBlock` that successfully queued preview data (best-effort
// if the small preview ring is full, that block is dropped from preview only, not from audio take).
struct RecordingPreviewPeakBlock
{
    float minSample = 0.0f;
    float maxSample = 0.0f;
    int numSourceSamples = 0;
};

// ---------------------------------------------------------------------------
// RecorderService — monotonic lifecycle: construction → optional arm → begin → stop, repeat.
// ---------------------------------------------------------------------------
class RecorderService
{
public:
    RecorderService();
    ~RecorderService();

    RecorderService(const RecorderService&) = delete;
    RecorderService& operator=(const RecorderService&) = delete;
    RecorderService(RecorderService&&) = delete;
    RecorderService& operator=(RecorderService&&) = delete;

    // [Message thread] Single track armed for record; arming a different id replaces.
    // `kInvalidTrackId` clears the arm.
    void armForRecording(TrackId trackId) noexcept;
    // [Message thread]
    void disarm() noexcept;
    // [Any thread, relaxed] 0 = none armed
    [[nodiscard]] TrackId getArmedTrackId() const noexcept;

    // [Message thread] Opens take file, allocates FIFO, starts background writer. Returns false
    // on validation / I/O error (`getLastError()`).
    [[nodiscard]] bool beginRecording(const BeginRecordingRequest& request);
    // [Message thread] Stops capture, joins writer, closes WAV, returns a complete `RecordedTakeResult`.
    // If not recording, returns failure with a clear `errorMessage`.
    [[nodiscard]] RecordedTakeResult stopRecordingAndFinalize();
    // [Message thread] Last error from begin/stop; diagnostic only.
    [[nodiscard]] juce::String getLastError() const;
    // [Message thread, relaxed] True between successful `beginRecording` and completion of `stop`.
    [[nodiscard]] bool isRecording() const noexcept;

    [[nodiscard]] TrackId getRecordingTrackId() const noexcept;
    // Only valid while `isRecording()`; use result after stop for committed values
    [[nodiscard]] std::int64_t getRecordingStartSample() const noexcept;
    [[nodiscard]] double getRecordingSampleRate() const noexcept;
    // Running intended total (sum of all audio callback `numSamples` for this take).
    [[nodiscard]] std::int64_t getRecordedSampleCount() const noexcept;
    // Sum of successful writer writes to disk (FIFO → file) for the current/last take.
    [[nodiscard]] std::int64_t getActuallyWrittenSampleCount() const noexcept;
    [[nodiscard]] std::int64_t getDroppedSampleCount() const noexcept;

    // [Audio / realtime] Push one mono block. **SPSC / realtime contract:** do not call from a
    // thread that holds locks on Session/UI. Does not allocate, does not block, does not wait,
    // does not touch `Session` / `SessionSnapshot` / components. If the FIFO is full, accepts a
    // prefix, counts the remainder as dropped, and **does not** shorten the intended take length.
    // No-op (cheap early out) if not recording.
    void pushInputBlock(const float* inputMono, int numSamples) noexcept;

    // [Message thread] Drain at most one preview block for the **recording** lane. Returns false
    // if none available.
    bool drainNextPreviewBlock(RecordingPreviewPeakBlock& out) noexcept;

private:
    // [Message thread] For next begin: tear down any leftover writer state; safe to call
    // from destructor path.
    void ensureWriterStopped() noexcept;
    // [Message thread] Reset counters and file state for a new take.
    void resetInternalCountersForNewTake() noexcept;
    // [Message thread] After writer join, pad with float silence so the WAV sample length matches
    // `intendedSampleCount`; the open writer is **24-bit PCM** (same as real audio chunks). Returns
    // false on write failure; caller must `closeTakeWriter` on all paths.
    bool appendSilencePaddingToMeetIntendedCount(std::int64_t numSilenceSamples);
    // [Message thread] Close writer; flush to disk.
    void closeTakeWriter() noexcept;
    // [Writer thread] Drains `sampleFifo_` to `AudioFormatWriter`; does **not** close the writer.
    void writerThreadMain() noexcept;
    // [Any] Next power of two >= x (for x >= 1).
    static std::uint32_t nextPow2(std::uint32_t x) noexcept;
    static std::uint32_t defaultFifoSizeSamples(double sampleRate) noexcept;
    // [Realtime] Enqueue one preview min/max; drops preview only if full.
    void tryPushPreviewFromBlock(const float* inputMono, int numSamples) noexcept;

    juce::String lastError_;
    std::unique_ptr<std::thread> writerThread_;
    // Serialized begin/stop/finalize with arm (coordinator is expected single-threaded; guard anyway).
    mutable std::mutex serviceMutex_;

    std::unique_ptr<juce::AbstractFifo> sampleFifo_;
    std::vector<float> sampleBuffer_;
    std::unique_ptr<juce::AbstractFifo> previewFifo_;
    std::vector<RecordingPreviewPeakBlock> previewBuffer_;

    std::unique_ptr<juce::AudioFormatWriter> takeWriter_;
    juce::WavAudioFormat wavFormat_;
    // Path of the take currently being written; copied into `RecordedTakeResult` on stop.
    juce::File lastTakeFile_;

    // Arm / recording id visibility without touching Session
    std::atomic<std::uint64_t> armedTrackId_{0};
    std::atomic<std::uint64_t> recordingTrackId_{0};
    std::atomic<bool> isRecording_{false};

    // Active take parameters (read by getters; set in begin, cleared in stop)
    std::atomic<std::int64_t> activeRecordingStartSample_{0};
    std::atomic<double> activeSampleRate_{0.0};

    // Sample accounting (audio thread updates `intended` / `dropped` while recording; writer
    // thread updates `samplesWritten` on successful disk writes; read on message thread after join.
    std::atomic<std::int64_t> intendedSampleTotal_{0};
    std::atomic<std::int64_t> droppedSampleTotal_{0};
    std::atomic<std::int64_t> samplesWrittenToFile_{0};
    // Set on first failed `writeFromAudioSampleBuffer` in the writer; finalize fails the take.
    std::atomic<bool> writerWriteFailed_{false};

    // Writer should keep draining after record flag clears until empty
    std::atomic<bool> writerRun_{false};
};
