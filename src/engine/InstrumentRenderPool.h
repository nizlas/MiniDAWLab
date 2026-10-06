#pragma once

// =============================================================================
// InstrumentRenderPool — fixed realtime worker pool for the live-instrument generation stage
// =============================================================================
//
// ROLE
//   The audio callback renders every live instrument's generation stage (MIDI merge + plug-in
//   `processBlock` into the host's OWN preallocated scratch) as independent jobs across a fixed
//   set of worker threads, with the callback thread participating. Everything downstream — the
//   row's inserts, fader / mute / pan, meters, routing and summing — stays on the callback thread
//   in the existing deterministic row order, so the output is bit-identical to the serial path
//   for deterministic instruments (`PlaybackEngine::audioDeviceIOCallbackWithContext`, phase
//   "instrument generation"). Jobs never outlive the callback that dispatched them: the callback
//   waits for the last job before it continues, so every existing drain / gate
//   (`isAudioCallbackInProcessingSection`, `waitForAudioCallbackExit`, the offline-render gate)
//   keeps covering the workers' access to hosts, snapshots and scratch buffers. A missed deadline
//   therefore means a late block (as before), never a half-rendered buffer being summed and never
//   a plug-in entering the next block while the previous one is still running.
//
// WAKE / COMPLETION MECHANISM
//   * Dispatch (callback thread): writes the job descriptors into the caller's preallocated array,
//     publishes each with a per-job claim flag (0 = pending), stores the job count, increments a
//     generation counter (release) and wakes all workers (`std::atomic::notify_all` — a
//     `WakeByAddressAll` on Windows, no mutex, no kernel object per worker).
//   * Claiming: every participant (workers and the callback thread) scans the job array and claims
//     a job with `exchange(1)` on its flag. Per-job flags — instead of a shared "next index"
//     counter — make a worker's possibly stale view of the job count harmless: indices beyond the
//     current count stay claimed from the previous block, and a job is run exactly once by exactly
//     one thread. Jobs are dispatched in longest-first order by the host's last render duration,
//     so a worker that wakes late only picks up what is left.
//   * Completion: whoever finishes a job decrements `remaining`; the thread that reaches zero
//     notifies. The callback thread, after claiming nothing more, spins for at most
//     `kJoinSpinMicros` (checking `remaining`) and then blocks in `remaining.wait()`
//     (`WaitOnAddress`) — a bounded spin, then a kernel wait. Risk accepted: the wake latency of
//     a kernel wait (typically 10–50 µs on Windows) is paid once per block when a worker finishes
//     last; a worker that is descheduled by the OS delays the whole block exactly as a slow plug-in
//     would (the callback cannot continue without the job).
//   * Idle workers block in `generation.wait()`; they never spin between blocks.
//
// REALTIME CONTRACT
//   No allocation, file I/O, logging or mutex in `audioThread_runJobs` or in the worker loop.
//   Thread creation / destruction happens only in `setWorkerCount` / the destructor (message
//   thread, device stopped). Worker threads join MMCSS "Pro Audio" when available (thread-level
//   priority only; the process priority class is never changed).
//
// WORKER COUNT
//   `defaultWorkerCount()` is conservative: half the physical cores minus one, at most
//   `kMaxWorkers`, 0 on 1–2 core machines (0 = serial). `--instrument-workers N` (0 = serial)
//   overrides it process-wide (`setConfiguredWorkerCountOverride`). Small workloads (fewer than two
//   jobs, or a summed last-block duration below `kMinParallelWorkMicros`) run serially on the
//   callback thread without waking anyone.
// =============================================================================

#include <juce_core/juce_core.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

class ExperimentalInstrumentHost;

namespace instrument_render
{
/// One live-instrument generation job for the current block. Written by the callback thread
/// before publication, read by exactly one claimant.
struct RenderJob
{
    ExperimentalInstrumentHost* host = nullptr;
    int numSamples = 0;
    std::int64_t lastRenderTicks = 0; ///< ordering key (longest first)
    /// 0 = pending (claimable), 1 = claimed / finished. Starts claimed so a stale index is inert.
    std::atomic<int> claimed{ 1 };
};

/// Process-wide diagnostic override (`--instrument-workers N`); −1 = none. Message thread,
/// before the engine is constructed.
void setConfiguredWorkerCountOverride(int workersOrMinusOne) noexcept;
[[nodiscard]] int configuredWorkerCountOverride() noexcept;
/// Conservative default from the machine's physical core count (see file header).
[[nodiscard]] int defaultWorkerCount() noexcept;

class InstrumentRenderPool
{
public:
    static constexpr int kMaxWorkers = 15;
    static constexpr int kMaxJobs = 256;
    /// Below this summed last-block render time the block runs serially (dispatch not worth it).
    static constexpr double kMinParallelWorkMicros = 300.0;
    /// Bounded spin before the callback thread blocks waiting for the last job.
    static constexpr double kJoinSpinMicros = 150.0;

    InstrumentRenderPool();
    ~InstrumentRenderPool();

    InstrumentRenderPool(const InstrumentRenderPool&) = delete;
    InstrumentRenderPool& operator=(const InstrumentRenderPool&) = delete;

    /// [Message thread, no callback running] Create / join worker threads. 0 = serial only.
    void setWorkerCount(int workers);
    [[nodiscard]] int workerCount() const noexcept { return static_cast<int>(workers_.size()); }

    /// [Audio thread] Run `count` jobs from `jobs` (caller-owned, preallocated; `claimed` flags
    /// are set here). Returns only when every job has finished. With no workers, a single job or
    /// a small summed workload the jobs run serially on the calling thread.
    /// `serialHint` forces the serial path for this call (diagnostic A/B inside one process).
    void audioThread_runJobs(RenderJob* jobs, int count, bool serialHint) noexcept;

    /// Relaxed diagnostics (message thread reads).
    struct Stats
    {
        std::uint64_t parallelBlocks = 0;      ///< blocks dispatched to the pool
        std::uint64_t serialBlocks = 0;        ///< blocks run serially (no workers / small work / hint)
        std::uint64_t jobsRunByCallback = 0;   ///< jobs the callback thread claimed itself
        std::uint64_t jobsRunByWorkers = 0;
        std::uint64_t joinWaits = 0;           ///< joins that had to block in the kernel wait
        double lastDispatchWallMs = 0.0;       ///< most recent parallel block: dispatch → all done
        double lastJoinWaitMs = 0.0;           ///< most recent parallel block: callback idle in join
    };
    [[nodiscard]] Stats statsRelaxed() const noexcept;

    /// [Audio thread] Wall time of the most recent `audioThread_runJobs` and the callback's idle
    /// wait inside it (zero for serial blocks); read by the profiler integration right after the call.
    [[nodiscard]] double lastRunWallMs() const noexcept { return lastRunWallMs_; }
    [[nodiscard]] double lastRunJoinWaitMs() const noexcept { return lastRunJoinWaitMs_; }
    [[nodiscard]] bool lastRunWasParallel() const noexcept { return lastRunParallel_; }

private:
    void workerLoop(int workerIndex) noexcept;
    /// Claim-and-run loop shared by workers and the callback thread; returns jobs run.
    int claimAndRunJobs(RenderJob* jobs, int count) noexcept;
    static void runJob(RenderJob& job) noexcept;
    static double ticksToMs(std::int64_t ticks) noexcept;

    std::vector<std::thread> workers_;
    std::atomic<bool> stop_{ false };
    /// Published job list for the current generation (callback writes before the generation bump).
    std::atomic<RenderJob*> jobs_{ nullptr };
    std::atomic<int> jobCount_{ 0 };
    std::atomic<std::uint32_t> generation_{ 0 };
    std::atomic<int> remaining_{ 0 };

    // audio thread (callback) only
    double lastRunWallMs_ = 0.0;
    double lastRunJoinWaitMs_ = 0.0;
    bool lastRunParallel_ = false;

    // relaxed diagnostics
    std::atomic<std::uint64_t> statParallelBlocks_{ 0 };
    std::atomic<std::uint64_t> statSerialBlocks_{ 0 };
    std::atomic<std::uint64_t> statJobsCallback_{ 0 };
    std::atomic<std::uint64_t> statJobsWorkers_{ 0 };
    std::atomic<std::uint64_t> statJoinWaits_{ 0 };
    std::atomic<double> statLastDispatchWallMs_{ 0.0 };
    std::atomic<double> statLastJoinWaitMs_{ 0.0 };
};
} // namespace instrument_render
