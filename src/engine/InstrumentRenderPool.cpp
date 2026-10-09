#include "engine/InstrumentRenderPool.h"

#include "diagnostics/AudioThreadProfiler.h"
#include "plugins/ExperimentalInstrumentHost.h"

#include <algorithm>
#include <utility>

#if JUCE_WINDOWS
 #define WIN32_LEAN_AND_MEAN
 #include <windows.h>
#endif
#if JUCE_INTEL
 #include <immintrin.h>
#endif

namespace instrument_render
{
namespace
{
    std::atomic<int> gConfiguredWorkerOverride{ -1 };

    /// Conservative default cap (the override may go up to kMaxWorkers).
    constexpr int kDefaultMaxWorkers = 7;

    void cpuRelax() noexcept
    {
#if JUCE_INTEL
        _mm_pause();
#else
        std::this_thread::yield();
#endif
    }

    /// Worker thread entry: join MMCSS "Pro Audio" (thread-level; never the process class). Falls
    /// back to a time-critical thread priority when MMCSS is unavailable. Not a realtime path.
    void elevateWorkerThreadPriority() noexcept
    {
#if JUCE_WINDOWS
        bool mmcssOk = false;
        if (HMODULE avrt = ::LoadLibraryW(L"avrt.dll"))
        {
            using SetCharacteristicsFn = HANDLE(WINAPI*)(LPCWSTR, LPDWORD);
            if (auto fn = reinterpret_cast<SetCharacteristicsFn>(
                    reinterpret_cast<void*>(::GetProcAddress(avrt, "AvSetMmThreadCharacteristicsW"))))
            {
                DWORD taskIndex = 0;
                mmcssOk = fn(L"Pro Audio", &taskIndex) != nullptr;
            }
            // The module stays loaded for the process lifetime (the MMCSS task handle refers to it).
        }
        if (!mmcssOk)
        {
            ::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
        }
#endif
    }
} // namespace

void setConfiguredWorkerCountOverride(const int workersOrMinusOne) noexcept
{
    gConfiguredWorkerOverride.store(workersOrMinusOne, std::memory_order_relaxed);
}

int configuredWorkerCountOverride() noexcept
{
    return gConfiguredWorkerOverride.load(std::memory_order_relaxed);
}

int defaultWorkerCount() noexcept
{
    const int physical = juce::SystemStats::getNumPhysicalCpus();
    if (physical <= 3)
    {
        return 0; // serial: too few cores to share with the UI, the driver and the OS
    }
    return juce::jlimit(1, kDefaultMaxWorkers, physical / 2 - 1);
}

InstrumentRenderPool::InstrumentRenderPool() = default;

InstrumentRenderPool::~InstrumentRenderPool()
{
    setWorkerCount(0);
}

void InstrumentRenderPool::setWorkerCount(const int workers)
{
    const int target = juce::jlimit(0, kMaxWorkers, workers);
    if (target == workerCount())
    {
        return;
    }
    // Stop and join the current set (idle: no callback is running by contract).
    if (!workers_.empty())
    {
        stop_.store(true, std::memory_order_release);
        generation_.fetch_add(1, std::memory_order_acq_rel);
        generation_.notify_all();
        for (auto& t : workers_)
        {
            if (t.joinable())
            {
                t.join();
            }
        }
        workers_.clear();
        stop_.store(false, std::memory_order_release);
    }
    workers_.reserve((size_t)target);
    for (int i = 0; i < target; ++i)
    {
        workers_.emplace_back([this, i] { workerLoop(i); });
    }
}

double InstrumentRenderPool::ticksToMs(const std::int64_t ticks) noexcept
{
    const double perSec = (double)juce::Time::getHighResolutionTicksPerSecond();
    return perSec > 0.0 ? (double)ticks * 1000.0 / perSec : 0.0;
}

void InstrumentRenderPool::runJob(RenderJob& job, const int laneIndex) noexcept
{
    // The TLS marker makes the profiler attribute the job's folds to the parallel section's
    // wall time (summed CPU, not callback wall) — for custom jobs exactly as for generation.
    if (job.p.run != nullptr)
    {
        audio_profiler::AudioThreadProfiler::setInsideGenerationJob(true);
        job.p.run(job, laneIndex);
        audio_profiler::AudioThreadProfiler::setInsideGenerationJob(false);
        return;
    }
    if (job.p.host != nullptr && job.p.numSamples > 0)
    {
        audio_profiler::AudioThreadProfiler::setInsideGenerationJob(true);
        job.p.host->audioThread_renderGenerationStageForBlock(job.p.numSamples);
        audio_profiler::AudioThreadProfiler::setInsideGenerationJob(false);
    }
}

int InstrumentRenderPool::claimAndRunJobs(RenderJob* jobs, const int count, const int laneIndex) noexcept
{
    int ran = 0;
    for (int i = 0; i < count; ++i)
    {
        RenderJob& j = jobs[i];
        if (j.claimed.load(std::memory_order_relaxed) != 0)
        {
            continue;
        }
        if (j.claimed.exchange(1, std::memory_order_acq_rel) != 0)
        {
            continue;
        }
        runJob(j, laneIndex);
        ++ran;
        if (remaining_.fetch_sub(1, std::memory_order_acq_rel) == 1)
        {
            remaining_.notify_all();
        }
    }
    return ran;
}

void InstrumentRenderPool::workerLoop(const int workerIndex) noexcept
{
    elevateWorkerThreadPriority();
    std::uint32_t seen = generation_.load(std::memory_order_acquire);
    while (!stop_.load(std::memory_order_acquire))
    {
        generation_.wait(seen, std::memory_order_acquire); // blocks while generation_ == seen
        seen = generation_.load(std::memory_order_acquire);
        if (stop_.load(std::memory_order_acquire))
        {
            break;
        }
        RenderJob* const jobs = jobs_.load(std::memory_order_acquire);
        const int count = jobCount_.load(std::memory_order_acquire);
        if (jobs == nullptr || count <= 0)
        {
            continue;
        }
        const int ran = claimAndRunJobs(jobs, count, workerIndex);
        if (ran > 0)
        {
            statJobsWorkers_.fetch_add((std::uint64_t)ran, std::memory_order_relaxed);
        }
    }
}

void InstrumentRenderPool::audioThread_runJobs(RenderJob* jobs, const int count, const bool serialHint) noexcept
{
    const std::int64_t t0 = juce::Time::getHighResolutionTicks();
    lastRunParallel_ = false;
    lastRunJoinWaitMs_ = 0.0;
    if (jobs == nullptr || count <= 0)
    {
        lastRunWallMs_ = 0.0;
        return;
    }

    std::int64_t sumTicks = 0;
    for (int i = 0; i < count; ++i)
    {
        sumTicks += jobs[i].p.lastRenderTicks;
    }
    const double sumMicros = ticksToMs(sumTicks) * 1000.0;
    const bool serial = serialHint || workers_.empty() || count < 2 || sumMicros < kMinParallelWorkMicros;

    if (serial)
    {
        // Identical job code on the callback lane — `--instrument-workers 0` is this path.
        for (int i = 0; i < count; ++i)
        {
            jobs[i].claimed.store(1, std::memory_order_relaxed);
            runJob(jobs[i], kCallbackLane);
        }
        statSerialBlocks_.fetch_add(1, std::memory_order_relaxed);
        statJobsCallback_.fetch_add((std::uint64_t)count, std::memory_order_relaxed);
        lastRunWallMs_ = ticksToMs(juce::Time::getHighResolutionTicks() - t0);
        return;
    }

    // Longest-first order (by the job's previous block): a late-waking worker only finds what is
    // left, and the heaviest plug-ins start first. Insertion sort on the job DATA (no allocation;
    // the claim flags are published afterwards).
    for (int i = 1; i < count; ++i)
    {
        int k = i;
        while (k > 0 && jobs[k - 1].p.lastRenderTicks < jobs[k].p.lastRenderTicks)
        {
            std::swap(jobs[k - 1].p, jobs[k].p);
            --k;
        }
    }

    remaining_.store(count, std::memory_order_release);
    for (int i = 0; i < count; ++i)
    {
        jobs[i].claimed.store(0, std::memory_order_release);
    }
    jobs_.store(jobs, std::memory_order_release);
    jobCount_.store(count, std::memory_order_release);
    generation_.fetch_add(1, std::memory_order_acq_rel);
    generation_.notify_all();

    const int own = claimAndRunJobs(jobs, count, kCallbackLane);

    const std::int64_t tJoin = juce::Time::getHighResolutionTicks();
    bool blocked = false;
    int rem = remaining_.load(std::memory_order_acquire);
    while (rem > 0)
    {
        const double spunMicros = ticksToMs(juce::Time::getHighResolutionTicks() - tJoin) * 1000.0;
        if (spunMicros < kJoinSpinMicros)
        {
            cpuRelax();
            rem = remaining_.load(std::memory_order_acquire);
            continue;
        }
        blocked = true;
        remaining_.wait(rem, std::memory_order_acquire); // returns when remaining_ != rem
        rem = remaining_.load(std::memory_order_acquire);
    }
    const std::int64_t tEnd = juce::Time::getHighResolutionTicks();
    lastRunJoinWaitMs_ = ticksToMs(tEnd - tJoin);
    lastRunWallMs_ = ticksToMs(tEnd - t0);
    lastRunParallel_ = true;

    statParallelBlocks_.fetch_add(1, std::memory_order_relaxed);
    statJobsCallback_.fetch_add((std::uint64_t)own, std::memory_order_relaxed);
    if (blocked)
    {
        statJoinWaits_.fetch_add(1, std::memory_order_relaxed);
    }
    statLastDispatchWallMs_.store(lastRunWallMs_, std::memory_order_relaxed);
    statLastJoinWaitMs_.store(lastRunJoinWaitMs_, std::memory_order_relaxed);
}

InstrumentRenderPool::Stats InstrumentRenderPool::statsRelaxed() const noexcept
{
    Stats s;
    s.parallelBlocks = statParallelBlocks_.load(std::memory_order_relaxed);
    s.serialBlocks = statSerialBlocks_.load(std::memory_order_relaxed);
    s.jobsRunByCallback = statJobsCallback_.load(std::memory_order_relaxed);
    s.jobsRunByWorkers = statJobsWorkers_.load(std::memory_order_relaxed);
    s.joinWaits = statJoinWaits_.load(std::memory_order_relaxed);
    s.lastDispatchWallMs = statLastDispatchWallMs_.load(std::memory_order_relaxed);
    s.lastJoinWaitMs = statLastJoinWaitMs_.load(std::memory_order_relaxed);
    return s;
}
} // namespace instrument_render
