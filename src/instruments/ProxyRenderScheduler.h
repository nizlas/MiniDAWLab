#pragma once

// =============================================================================
// ProxyRenderScheduler — the P1E background proxy-render service core
// (steering docs/PORTABLE_INSTRUMENTS_AND_PROXIES.md §13, §14, roadmap P1E)
// =============================================================================
// One application/project-owned scheduler that serializes every proxy render
// through ONE low-priority worker (Locked P1 resource defaults, §14.3):
//
//   * explicit render requests per instrument destination (P1H later adds the
//     Auto/On-Save/Manual/Off policies on top of these primitives);
//   * per-destination coalescing + monotonic generations + supersession;
//   * cooperative cancellation/obsolescence at block boundaries (PI-013);
//   * recording pauses starting AND progressing background work (resource
//     policy); transport playback never pauses it (measured, revision 6);
//   * currency verification on the message thread immediately before
//     publication (PI-028; revision comparison, never a fresh blob hash);
//   * deterministic shutdown: cancel, join, message-thread instance teardown.
//
// OWNERSHIP (§6): the service is owned at APPLICATION scope (Main.cpp), as a
// sibling of Session — never by a UI component. MainAppWindow only attaches the
// production engine (which references the instrument hosts it owns) and calls
// the narrow API; it must detach (detachEngineAndShutdownJobs) BEFORE the hosts
// and coordinators die. Shutdown order: MainAppWindow detach (cancel + join +
// teardown) → application destroys the service → Session/engine.
//
// THREAD MAP (§14.1):
//   [Message thread] every public API call; identity capture; instance
//                    create/restore/prepare; currency check; publication;
//                    metadata mutation; instance teardown.
//   [Render worker]  exclusively: ProxyPreparedJob::render (the processBlock
//                    loop + temp WAV writing).
//   Worker → message-thread transitions go through the injected poster
//   (production: MessageManager::callAsync; tests: a manual pump). Posted
//   callbacks capture shared_ptr<Job> + weak scheduler state — they never
//   dereference destroyed project/UI objects.
//
// The engine seam (ProxyRenderJobEngine) keeps this header free of plugin
// hosting so the deterministic selftests drive every race with controllable
// fake engines; production (AppProxyRenderEngine) composes the P1D renderer —
// it is never reimplemented here.

#include "domain/Track.h" // TrackId
#include "instruments/ProxyRenderTypes.h"

#include <juce_core/juce_core.h>

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace proxy_render
{

//==============================================================================
// State vocabulary (§13.1 / §13.2 — two separate machines)
//==============================================================================
/// Job EXECUTION state (internal to the queue; §13.2). Terminal: Published,
/// Obsolete, Cancelled, Failed.
enum class ProxyJobPhase
{
    Queued,     ///< request captured (message thread); waiting for the worker
    Preparing,  ///< [message thread] isolated instance create/restore/prepare
    Rendering,  ///< [worker] block loop with cancellation/obsolescence checks
    Finalizing, ///< render returned; currency check + publication pending (message thread)
    Published,  ///< publication + metadata update succeeded
    Obsolete,   ///< superseded / no longer current — never published (PI-028)
    Cancelled,  ///< explicit/lifecycle cancellation — never published
    Failed      ///< error (see result/failure message); previous generation retained
};

/// Destination PROXY state (derived, user-facing; §13.1). "Most recently
/// published" NEVER implies Current — currency is always the fingerprint /
/// semantic-identity verdict against the CURRENT expected identity.
enum class ProxyDestinationState
{
    Absent,    ///< no published generation and no active job
    Current,   ///< published generationId == current expected fingerprint
    Stale,     ///< a retained generation exists but no longer matches (PI-007)
    Rendering, ///< an active job (queued/preparing/rendering/finalizing) exists
    Failed     ///< last job for the still-current fingerprint failed; retained
               ///< previous generation (if any) keeps its own pure verdict
};

[[nodiscard]] inline const char* toString(const ProxyJobPhase p) noexcept
{
    switch (p)
    {
        case ProxyJobPhase::Queued: return "Queued";
        case ProxyJobPhase::Preparing: return "Preparing";
        case ProxyJobPhase::Rendering: return "Rendering";
        case ProxyJobPhase::Finalizing: return "Finalizing";
        case ProxyJobPhase::Published: return "Published";
        case ProxyJobPhase::Obsolete: return "Obsolete";
        case ProxyJobPhase::Cancelled: return "Cancelled";
        case ProxyJobPhase::Failed: return "Failed";
    }
    return "?";
}

[[nodiscard]] inline const char* toString(const ProxyDestinationState s) noexcept
{
    switch (s)
    {
        case ProxyDestinationState::Absent: return "Absent";
        case ProxyDestinationState::Current: return "Current";
        case ProxyDestinationState::Stale: return "Stale";
        case ProxyDestinationState::Rendering: return "Rendering";
        case ProxyDestinationState::Failed: return "Failed";
    }
    return "?";
}

//==============================================================================
// Engine seam — production composes P1D; tests provide controllable fakes.
// Every method except ProxyPreparedJob::render runs on the MESSAGE thread.
//==============================================================================
/// The destination's CURRENT expected identity (fingerprint + Primary semantic
/// revision). This is the §9.4.2 currency comparison — never a fresh state-blob hash.
struct ProxyCurrentIdentity
{
    bool destinationExists = false;
    juce::String expectedFingerprint;
    std::uint64_t primarySemanticRevision = 0;
};

/// The immutable captured render request. Production subclasses carry the full
/// deep-copied ProxyRenderRequest (P1C snapshot + state bytes + plugin identity);
/// the scheduler itself only needs the identity pair for currency comparison.
struct ProxyCapturedRequest
{
    virtual ~ProxyCapturedRequest() = default;
    juce::String expectedFingerprint;
    std::uint64_t primarySemanticRevision = 0;
};

/// A prepared isolated render job. Constructed (engine prepare) and DESTROYED on
/// the message thread — the destructor performs the verified instance teardown.
/// render() is called exactly once, on the render worker, which exclusively owns
/// the prepared instance for the duration of the call.
struct ProxyPreparedJob
{
    virtual ~ProxyPreparedJob() = default;

    /// [Render worker] Blocking render. Must honor `cancel` at block boundaries
    /// and call `waitWhilePaused` (recording pause gate) once per block.
    /// `progressRenderedMs` is a scheduler-owned atomic the implementation SHOULD
    /// update (relaxed) with the milliseconds of material rendered so far (P1I
    /// live progress, PI-013); implementations may ignore it.
    [[nodiscard]] virtual ProxyRenderResult render(const ProxyRenderCancellationToken& cancel,
                                                   const std::function<void()>& waitWhilePaused,
                                                   std::atomic<std::int64_t>& progressRenderedMs)
        = 0;
};

/// Production: AppProxyRenderEngine (P1D lifecycle + executor + P1F asset store).
class ProxyRenderJobEngine
{
public:
    virtual ~ProxyRenderJobEngine() = default;

    /// [Message thread] Current expected identity of the destination (rebuilt
    /// from live musical content + host-managed revision; §12.3 derivation).
    [[nodiscard]] virtual ProxyCurrentIdentity currentIdentity(TrackId destination) = 0;

    /// [Message thread] Deep-copy the complete immutable render request. Returns
    /// nullptr + error when the destination cannot be rendered.
    [[nodiscard]] virtual std::unique_ptr<ProxyCapturedRequest>
        captureRequest(TrackId destination, juce::String& errorOut) = 0;

    /// [Message thread] Create/restore/prepare the isolated render instance for
    /// a captured request. Returns nullptr with `failureOut` filled on failure.
    [[nodiscard]] virtual std::unique_ptr<ProxyPreparedJob>
        prepare(const ProxyCapturedRequest& request, ProxyRenderResult& failureOut) = 0;

    /// [Message thread] Validate + atomically publish a successful, still-current
    /// render (P1F). Also owns the temp artifact on success. Returns false +
    /// error on any publication failure (previous generation must be retained).
    [[nodiscard]] virtual bool publish(TrackId destination,
                                       const ProxyCapturedRequest& request,
                                       const ProxyRenderResult& result,
                                       juce::String& errorOut) = 0;

    /// [Message thread] generationId of the currently published metadata for the
    /// destination (empty = none). Used for the derived destination state.
    [[nodiscard]] virtual juce::String publishedGenerationId(TrackId destination) = 0;
};

//==============================================================================
// Immutable status snapshots (query API)
//==============================================================================
struct ProxyJobStatus
{
    bool exists = false;
    TrackId destination = kInvalidTrackId;
    std::uint64_t generation = 0;
    ProxyJobPhase phase = ProxyJobPhase::Queued;
    juce::String expectedFingerprint;
    std::uint64_t primarySemanticRevision = 0;
    juce::String message;        ///< failure/publication detail
    ProxyRenderResult result;    ///< meaningful once terminal (copy)
    /// P1I live progress (PI-013): milliseconds of destination material rendered so
    /// far (worker-updated atomic; safe to copy in ANY phase — plain integer, no COW).
    /// A percentage is deliberately not offered: the total length is unknown until
    /// the tail completes (§15.2), so honest progress is rendered material + speed.
    std::int64_t progressRenderedMs = 0;
};

//==============================================================================
// The scheduler
//==============================================================================
class ProxyRenderScheduler final
{
public:
    /// `postToMessageThread` runs a callback on the message thread (production:
    /// juce::MessageManager::callAsync; deterministic tests: a manual pump the
    /// test drains). Callbacks are self-contained (shared_ptr captures).
    explicit ProxyRenderScheduler(std::function<void(std::function<void()>)> postToMessageThread)
        : post_(std::move(postToMessageThread)), worker_(*this)
    {
        jassert(post_ != nullptr);
        worker_.startThread(juce::Thread::Priority::low); // one low-priority worker (§14.3)
    }

    ~ProxyRenderScheduler() { shutdown(); }

    //==========================================================================
    // Engine attachment (application lifecycle seam)
    //==========================================================================
    /// [Message thread] Attach the production engine. The engine must stay valid
    /// until detachEngineAndShutdownJobs() returns.
    void attachEngine(ProxyRenderJobEngine* engine)
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        engine_ = engine;
        draining_.store(false, std::memory_order_release);
    }

    /// [Message thread] Cancel everything, wait for the worker to go idle, tear
    /// down every prepared instance and detach the engine. Called by the engine
    /// owner (MainAppWindow) BEFORE the objects the engine references die.
    void detachEngineAndShutdownJobs()
    {
        draining_.store(true, std::memory_order_release); // worker stops picking up new jobs
        cancelAllInternal(true);
        waitForWorkerIdle(15000);
        sweepAllJobsOnMessageThread();
        const std::lock_guard<std::mutex> lock(mutex_);
        engine_ = nullptr;
    }

    /// [Message thread] Full shutdown: detach + stop/join the worker thread.
    /// Idempotent; also run by the destructor (application scope).
    void shutdown()
    {
        detachEngineAndShutdownJobs();
        worker_.signalThreadShouldExit();
        wake_.signal();
        pauseWake_.signal();
        worker_.stopThread(15000);
    }

    //==========================================================================
    // Narrow API (all [message thread] unless noted)
    //==========================================================================
    /// Explicit render request. Coalesces onto an equivalent active job (same
    /// expected identity, not obsolete); otherwise supersedes any older job and
    /// enqueues a new generation. Returns the (new or coalesced) job status.
    ProxyJobStatus requestRender(const TrackId destination)
    {
        std::shared_ptr<Job> toObsolete;
        ProxyJobStatus out;
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            if (engine_ == nullptr)
            {
                out.message = "no engine attached";
                return out;
            }
            const ProxyCurrentIdentity now = engine_->currentIdentity(destination);
            if (!now.destinationExists)
            {
                out.message = "destination does not exist / is not renderable";
                return out;
            }
            if (auto active = activeJobLocked(destination))
            {
                // Coalescing: an equivalent request joins the active job instead
                // of producing duplicate work.
                if (!active->obsolete.load() && !active->token.isCancelled()
                    && active->expectedFingerprint == now.expectedFingerprint
                    && active->primarySemanticRevision == now.primarySemanticRevision)
                {
                    return statusOfLocked(*active);
                }
                // Supersession: the newer request obsoletes queued/running work.
                toObsolete = active;
            }
            juce::String err;
            auto request = engine_->captureRequest(destination, err);
            if (request == nullptr)
            {
                out.destination = destination;
                out.message = "request capture failed: " + err;
                return out;
            }
            auto job = std::make_shared<Job>();
            job->destination = destination;
            job->generation = ++generationCounter_;
            job->projectEpoch = projectEpoch_;
            job->expectedFingerprint = request->expectedFingerprint;
            job->primarySemanticRevision = request->primarySemanticRevision;
            job->request = std::move(request);
            activeByDestination_[destination] = job;
            fifo_.push_back(job);
            out = statusOfLocked(*job);
        }
        if (toObsolete != nullptr)
        {
            markObsolete(*toObsolete);
        }
        wake_.signal();
        return out;
    }

    /// Cancel the destination's active job (any phase). Queued jobs terminalize
    /// when the worker reaches them; running jobs stop at the next boundary.
    void cancelDestination(const TrackId destination)
    {
        std::shared_ptr<Job> job;
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            job = activeJobLocked(destination);
        }
        if (job != nullptr)
        {
            job->cancelledExplicitly.store(true, std::memory_order_release);
            job->token.requestCancel();
            pauseWake_.signal();
        }
    }

    /// A render-relevant change happened (fingerprint input edit, Primary
    /// replacement/removal, preset restore, routing edit…). If the active job's
    /// captured identity no longer matches, it becomes Obsolete and is cancelled
    /// early — even if it finishes, it never publishes (PI-028).
    void notifyDestinationIdentityChanged(const TrackId destination)
    {
        std::shared_ptr<Job> job;
        ProxyCurrentIdentity now;
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            job = activeJobLocked(destination);
            if (job == nullptr || engine_ == nullptr)
            {
                return;
            }
            now = engine_->currentIdentity(destination);
        }
        if (!now.destinationExists || now.expectedFingerprint != job->expectedFingerprint
            || now.primarySemanticRevision != job->primarySemanticRevision)
        {
            markObsolete(*job);
        }
    }

    /// The destination track was removed: obsolete + cancel, drop bookkeeping.
    void notifyTrackRemoved(const TrackId destination)
    {
        std::shared_ptr<Job> job;
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            job = activeJobLocked(destination);
            lastTerminalByDestination_.erase(destination);
        }
        if (job != nullptr)
        {
            markObsolete(*job);
        }
    }

    /// Recording pauses STARTING and PROGRESSING background rendering (§14.3
    /// resource policy — not a correctness requirement). Any thread.
    void notifyRecordingState(const bool recordingActive)
    {
        recordingActive_.store(recordingActive, std::memory_order_release);
        if (!recordingActive)
        {
            wake_.signal();
            pauseWake_.signal();
        }
    }

    /// Project close/replacement: every active job belongs to the old epoch —
    /// cancel + obsolete all; completions arriving later are discarded by the
    /// epoch check (worker completion after project close, §13.3).
    void notifyProjectChanged()
    {
        std::vector<std::shared_ptr<Job>> actives;
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            ++projectEpoch_;
            for (auto& [tid, job] : activeByDestination_)
            {
                actives.push_back(job);
            }
            lastTerminalByDestination_.clear();
        }
        for (auto& job : actives)
        {
            markObsolete(*job);
        }
    }

    /// Immutable status of the destination's active job, else its last terminal
    /// job, else a not-exists status. Safe from the message thread.
    [[nodiscard]] ProxyJobStatus jobStatus(const TrackId destination) const
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (auto it = activeByDestination_.find(destination); it != activeByDestination_.end())
        {
            return statusOfLocked(*it->second);
        }
        if (auto it = lastTerminalByDestination_.find(destination);
            it != lastTerminalByDestination_.end())
        {
            return it->second;
        }
        return {};
    }

    /// [Message thread] Derived destination proxy state (§13.1). Consults the
    /// engine for the published generation and the current expected identity.
    [[nodiscard]] ProxyDestinationState destinationState(const TrackId destination) const
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (auto it = activeByDestination_.find(destination); it != activeByDestination_.end())
        {
            const auto phase = it->second->phase.load();
            if (phase == ProxyJobPhase::Queued || phase == ProxyJobPhase::Preparing
                || phase == ProxyJobPhase::Rendering || phase == ProxyJobPhase::Finalizing)
            {
                return ProxyDestinationState::Rendering;
            }
        }
        if (engine_ == nullptr)
        {
            return ProxyDestinationState::Absent;
        }
        const ProxyCurrentIdentity now = engine_->currentIdentity(destination);
        const juce::String published = engine_->publishedGenerationId(destination);
        if (published.isNotEmpty())
        {
            // "Current" is a pure identity verdict — never "latest file on disk".
            if (now.destinationExists && published == now.expectedFingerprint)
            {
                return ProxyDestinationState::Current;
            }
            // Retained but non-matching (or destination gone): Stale, unless the
            // most recent job for the STILL-CURRENT identity failed (below).
        }
        if (auto it = lastTerminalByDestination_.find(destination);
            it != lastTerminalByDestination_.end())
        {
            const auto& last = it->second;
            if (last.phase == ProxyJobPhase::Failed && now.destinationExists
                && last.expectedFingerprint == now.expectedFingerprint)
            {
                return ProxyDestinationState::Failed;
            }
        }
        if (published.isNotEmpty())
        {
            return ProxyDestinationState::Stale;
        }
        return ProxyDestinationState::Absent;
    }

    /// True while the worker is between jobs and the queue is empty (test/shutdown aid).
    [[nodiscard]] bool isIdle() const
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        return fifo_.empty() && runningJob_ == nullptr;
    }

private:
    //==========================================================================
    struct Job
    {
        TrackId destination = kInvalidTrackId;
        std::uint64_t generation = 0;
        std::uint64_t projectEpoch = 0;
        juce::String expectedFingerprint;
        std::uint64_t primarySemanticRevision = 0;

        ProxyRenderCancellationToken token;
        std::atomic<ProxyJobPhase> phase{ ProxyJobPhase::Queued };
        std::atomic<bool> obsolete{ false };
        std::atomic<bool> cancelledExplicitly{ false };
        std::atomic<bool> renderReturned{ false }; ///< worker set, release-ordered by phase store
        bool finalized = false; ///< message-thread-only latch (single finalize)
        /// P1I live progress (PI-013): worker-updated rendered-material milliseconds.
        std::atomic<std::int64_t> progressRenderedMs{ 0 };

        std::unique_ptr<ProxyCapturedRequest> request;  ///< written msg thread pre-enqueue
        std::unique_ptr<ProxyPreparedJob> prepared;     ///< create+destroy msg thread; render worker
        ProxyRenderResult result;                       ///< written by worker, read after
        juce::String message;

        juce::WaitableEvent prepareDone;
        juce::WaitableEvent finalizeDone;
    };

    //==========================================================================
    // Worker (ONE low-priority render worker; §14.3)
    //==========================================================================
    struct WorkerThread final : juce::Thread
    {
        explicit WorkerThread(ProxyRenderScheduler& s)
            : juce::Thread("ProxyRenderSchedulerWorker"), sched(s)
        {
        }
        ~WorkerThread() override { stopThread(15000); }
        void run() override { sched.workerLoop(); }
        ProxyRenderScheduler& sched;
    };

    void workerLoop()
    {
        while (!worker_.threadShouldExit())
        {
            std::shared_ptr<Job> job;
            {
                const std::lock_guard<std::mutex> lock(mutex_);
                // Recording pause: do not START new work while recording (§14.3);
                // drain gate: no new work during detach/shutdown sweeps.
                if (!fifo_.empty() && !recordingActive_.load(std::memory_order_acquire)
                    && !draining_.load(std::memory_order_acquire))
                {
                    job = fifo_.front();
                    fifo_.pop_front();
                    runningJob_ = job;
                }
            }
            if (job == nullptr)
            {
                wake_.wait(100);
                continue;
            }
            runOneJob(*job);
            {
                const std::lock_guard<std::mutex> lock(mutex_);
                runningJob_ = nullptr;
            }
        }
    }

    /// [Worker] Drive one job through Preparing → Rendering → Finalizing. All
    /// engine work is bounced to the message thread; the worker itself only
    /// calls ProxyPreparedJob::render.
    void runOneJob(Job& job)
    {
        auto self = sharedJobFor(&job);
        if (self == nullptr)
        {
            return;
        }

        // Early exit for work cancelled/obsoleted while still queued.
        if (job.token.isCancelled() || job.obsolete.load())
        {
            job.phase.store(ProxyJobPhase::Finalizing, std::memory_order_release);
            postFinalize(self);
            waitJobEvent(job.finalizeDone);
            return;
        }

        // Preparing (message thread does the actual work).
        job.phase.store(ProxyJobPhase::Preparing, std::memory_order_release);
        post_([this, self] {
            // Message thread: skip when already finalized (shutdown sweep) or cancelled.
            if (!self->finalized && !self->token.isCancelled() && !self->obsolete.load())
            {
                ProxyRenderJobEngine* engine = nullptr;
                {
                    const std::lock_guard<std::mutex> lock(mutex_);
                    engine = engine_;
                }
                if (engine != nullptr)
                {
                    ProxyRenderResult failure;
                    self->prepared = engine->prepare(*self->request, failure);
                    if (self->prepared == nullptr)
                    {
                        self->result = failure;
                        self->message = failure.message;
                    }
                }
                else
                {
                    self->message = "engine detached during prepare";
                }
            }
            self->prepareDone.signal();
        });
        if (!waitJobEvent(job.prepareDone))
        {
            return; // shutdown while waiting — sweep tears everything down
        }

        if (job.prepared == nullptr || job.token.isCancelled() || job.obsolete.load())
        {
            job.phase.store(ProxyJobPhase::Finalizing, std::memory_order_release);
            postFinalize(self);
            waitJobEvent(job.finalizeDone);
            return;
        }

        // Rendering — the worker exclusively owns the prepared instance now.
        job.phase.store(ProxyJobPhase::Rendering, std::memory_order_release);
        job.result = job.prepared->render(
            job.token,
            [this, &job] {
                // Recording pause gate: hold PROGRESS at the block boundary while
                // recording, but never a cancelled/exiting job (prompt cancellation).
                while (recordingActive_.load(std::memory_order_acquire)
                       && !job.token.isCancelled() && !worker_.threadShouldExit())
                {
                    pauseWake_.wait(50);
                }
            },
            job.progressRenderedMs);

        // Finalizing (message thread: currency check + publication + teardown).
        job.renderReturned.store(true, std::memory_order_release);
        job.phase.store(ProxyJobPhase::Finalizing, std::memory_order_release);
        postFinalize(self);
        waitJobEvent(job.finalizeDone); // strict serialization: one job in flight, ever
    }

    /// [Worker] Wait for a message-thread signal without deadlocking shutdown
    /// (the message thread may be joining us — bail out on threadShouldExit).
    bool waitJobEvent(juce::WaitableEvent& ev)
    {
        while (!ev.wait(50))
        {
            if (worker_.threadShouldExit())
            {
                return false;
            }
        }
        return true;
    }

    void postFinalize(const std::shared_ptr<Job>& self)
    {
        post_([this, self] { finalizeOnMessageThread(self); });
    }

    /// [Message thread] Single point of job termination: currency check,
    /// publication, temp discard, instance teardown, bookkeeping. Idempotent
    /// via the `finalized` latch (the shutdown sweep can win the race).
    void finalizeOnMessageThread(const std::shared_ptr<Job>& job)
    {
        if (job->finalized)
        {
            job->finalizeDone.signal();
            return;
        }
        job->finalized = true;

        ProxyRenderJobEngine* engine = nullptr;
        bool epochCurrent = false;
        bool generationCurrent = false;
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            engine = engine_;
            epochCurrent = job->projectEpoch == projectEpoch_;
            const auto it = activeByDestination_.find(job->destination);
            generationCurrent
                = it != activeByDestination_.end() && it->second.get() == job.get();
        }

        ProxyJobPhase terminal;
        if (job->cancelledExplicitly.load())
        {
            terminal = ProxyJobPhase::Cancelled;
        }
        else if (job->obsolete.load() || !epochCurrent || !generationCurrent)
        {
            terminal = ProxyJobPhase::Obsolete;
        }
        else if (job->token.isCancelled())
        {
            terminal = ProxyJobPhase::Cancelled;
        }
        else if (!job->renderReturned.load(std::memory_order_acquire) || job->prepared == nullptr)
        {
            terminal = ProxyJobPhase::Failed;
            if (job->message.isEmpty())
            {
                job->message = job->result.message.isNotEmpty() ? job->result.message
                                                                : "prepare failed";
            }
        }
        else if (job->result.status == ProxyRenderStatus::Cancelled)
        {
            terminal = ProxyJobPhase::Cancelled;
        }
        else if (job->result.status == ProxyRenderStatus::Failed)
        {
            terminal = ProxyJobPhase::Failed;
            job->message = job->result.message;
        }
        else if (engine == nullptr)
        {
            terminal = ProxyJobPhase::Obsolete;
            job->message = "engine detached before publication";
        }
        else
        {
            // Currency verification IMMEDIATELY before publication (PI-028):
            // destination still exists, same project epoch (checked above), the
            // job generation is still the destination's newest (checked above),
            // fingerprint still matches and the Primary semantic revision still
            // matches — by identity comparison, NEVER a fresh blob hash (§9.4.2).
            const ProxyCurrentIdentity now = engine->currentIdentity(job->destination);
            const bool current = now.destinationExists
                                 && now.expectedFingerprint == job->expectedFingerprint
                                 && now.primarySemanticRevision == job->primarySemanticRevision;
            if (!current)
            {
                terminal = ProxyJobPhase::Obsolete;
                job->message = "identity changed during finalizing — result discarded";
            }
            else
            {
                juce::String err;
                if (engine->publish(job->destination, *job->request, job->result, err))
                {
                    terminal = ProxyJobPhase::Published;
                }
                else
                {
                    // Publication failure: previous generation/metadata retained
                    // by the engine contract; this job just records the error.
                    terminal = ProxyJobPhase::Failed;
                    job->message = "publication failed: " + err;
                }
            }
        }

        // Non-published temp output is discarded here (obsolete/cancelled work
        // may have completed a WAV — it must never survive as a publishable file).
        if (terminal != ProxyJobPhase::Published
            && job->result.temporaryWavFile != juce::File())
        {
            (void)job->result.temporaryWavFile.deleteFile();
            job->result.temporaryWavFile = juce::File();
        }

        // Verified-safe instance teardown on the message thread.
        job->prepared.reset();

        job->phase.store(terminal, std::memory_order_release);
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            const auto it = activeByDestination_.find(job->destination);
            if (it != activeByDestination_.end() && it->second.get() == job.get())
            {
                activeByDestination_.erase(it);
            }
            lastTerminalByDestination_[job->destination] = statusOfLocked(*job);
        }
        job->finalizeDone.signal();
        wake_.signal();
    }

    //==========================================================================
    // Internals
    //==========================================================================
    void markObsolete(Job& job)
    {
        job.obsolete.store(true, std::memory_order_release);
        job.token.requestCancel(); // obsolete work may be cancelled early
        pauseWake_.signal();
    }

    /// Shutdown/detach cancellation: jobs terminalize as CANCELLED (lifecycle
    /// stop), not Obsolete (supersession) and never Failed.
    void cancelAllInternal(const bool explicitCancel)
    {
        std::vector<std::shared_ptr<Job>> actives;
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            for (auto& [tid, job] : activeByDestination_)
            {
                actives.push_back(job);
            }
        }
        for (auto& job : actives)
        {
            if (explicitCancel)
            {
                job->cancelledExplicitly.store(true, std::memory_order_release);
            }
            job->token.requestCancel();
        }
        pauseWake_.signal();
        wake_.signal();
    }

    void waitForWorkerIdle(const int timeoutMs)
    {
        const double deadline = juce::Time::getMillisecondCounterHiRes() + timeoutMs;
        for (;;)
        {
            std::shared_ptr<Job> running;
            {
                const std::lock_guard<std::mutex> lock(mutex_);
                running = runningJob_;
            }
            if (running == nullptr)
            {
                return;
            }
            // The worker may be blocked waiting for a message-thread prepare or
            // finalize that will never run while WE occupy the message thread —
            // execute the pending step for it deterministically (idempotent).
            if (!running->finalized)
            {
                const auto phase = running->phase.load();
                if (phase == ProxyJobPhase::Preparing)
                {
                    // Cancelled prepare: skip the engine work, unblock the worker
                    // (the posted prepare lambda later no-ops via the latch).
                    running->prepareDone.signal();
                }
                else if (phase == ProxyJobPhase::Finalizing)
                {
                    // Safe direct finalize (idempotent latch); a job still
                    // Rendering stops at its next block via the cancel above.
                    finalizeOnMessageThread(running);
                }
            }
            if (juce::Time::getMillisecondCounterHiRes() > deadline)
            {
                jassertfalse; // worker failed to go idle — bounded join still follows
                return;
            }
            juce::Thread::sleep(5);
        }
    }

    /// [Message thread] Deterministic terminalization of every remaining job
    /// (shutdown/detach): tear down prepared instances, delete temp artifacts.
    void sweepAllJobsOnMessageThread()
    {
        std::vector<std::shared_ptr<Job>> all;
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            for (auto& [tid, job] : activeByDestination_)
            {
                all.push_back(job);
            }
            fifo_.clear();
        }
        for (auto& job : all)
        {
            if (!job->finalized)
            {
                job->finalized = true;
                job->prepared.reset(); // verified message-thread teardown
                if (job->result.temporaryWavFile != juce::File())
                {
                    (void)job->result.temporaryWavFile.deleteFile();
                    job->result.temporaryWavFile = juce::File();
                }
                job->phase.store(ProxyJobPhase::Cancelled, std::memory_order_release);
                job->finalizeDone.signal();
                job->prepareDone.signal();
            }
        }
        const std::lock_guard<std::mutex> lock(mutex_);
        for (auto& job : all)
        {
            lastTerminalByDestination_[job->destination] = statusOfLocked(*job);
            activeByDestination_.erase(job->destination);
        }
    }

    [[nodiscard]] std::shared_ptr<Job> activeJobLocked(const TrackId destination) const
    {
        const auto it = activeByDestination_.find(destination);
        return it != activeByDestination_.end() ? it->second : nullptr;
    }

    [[nodiscard]] std::shared_ptr<Job> sharedJobFor(Job* raw)
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (runningJob_ != nullptr && runningJob_.get() == raw)
        {
            return runningJob_;
        }
        return nullptr;
    }

    [[nodiscard]] ProxyJobStatus statusOfLocked(const Job& job) const
    {
        ProxyJobStatus s;
        s.exists = true;
        s.destination = job.destination;
        s.generation = job.generation;
        s.phase = job.phase.load(std::memory_order_acquire);
        s.expectedFingerprint = job.expectedFingerprint;
        s.primarySemanticRevision = job.primarySemanticRevision;
        // Plain atomic — safe to copy in any phase (P1I live progress).
        s.progressRenderedMs = job.progressRenderedMs.load(std::memory_order_relaxed);
        // RACE-SAFETY CONTRACT (P1H hang fix): `message`/`result` contain reference-counted
        // juce::String/juce::File members that the worker mutates during Preparing/Rendering
        // AND the message thread mutates during Finalizing (finalizeOnMessageThread writes
        // job->message and clears result.temporaryWavFile) and during the shutdown sweep
        // (phase still Queued). None of those writers hold `mutex_`, so copying here while
        // they run is a data race on the COW refcounts — in a Debug CRT heap that corrupts
        // allocator metadata and manifested as the intermittent P1E selftest hang. Copy the
        // mutable payload ONLY after acquiring a TERMINAL phase: the terminal store is
        // release-ordered after the last mutation, and terminal jobs are never written again.
        switch (s.phase)
        {
            case ProxyJobPhase::Published:
            case ProxyJobPhase::Obsolete:
            case ProxyJobPhase::Cancelled:
            case ProxyJobPhase::Failed:
                s.message = job.message;
                s.result = job.result;
                break;
            case ProxyJobPhase::Queued:
            case ProxyJobPhase::Preparing:
            case ProxyJobPhase::Rendering:
            case ProxyJobPhase::Finalizing:
                break; // payload still owned by the worker or the finalizing message thread
        }
        return s;
    }

    //==========================================================================
    std::function<void(std::function<void()>)> post_;
    WorkerThread worker_;
    juce::WaitableEvent wake_;
    juce::WaitableEvent pauseWake_;

    mutable std::mutex mutex_;
    ProxyRenderJobEngine* engine_ = nullptr;
    std::deque<std::shared_ptr<Job>> fifo_;
    std::map<TrackId, std::shared_ptr<Job>> activeByDestination_;
    std::map<TrackId, ProxyJobStatus> lastTerminalByDestination_;
    std::shared_ptr<Job> runningJob_;
    std::uint64_t generationCounter_ = 0;
    std::uint64_t projectEpoch_ = 1;
    std::atomic<bool> recordingActive_{ false };
    std::atomic<bool> draining_{ false };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ProxyRenderScheduler)
};

} // namespace proxy_render
