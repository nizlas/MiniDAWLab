#pragma once

// =============================================================================
// ProxyRenderInstanceLifecycle — isolated plugin lifecycle + foreground job (P1D)
// (steering docs/PORTABLE_INSTRUMENTS_AND_PROXIES.md §9.4.4, §13; SPIKE-02 §4/§8)
// =============================================================================
// The message-thread halves of the measured SPIKE-02 lifecycle, promoted to
// production (the spike harness' Controller/RenderWorker are superseded by this
// file + ProxyRenderExecutor.h):
//
//   [Message thread] create the SECOND plugin instance
//   [Message thread] restore the captured exact opaque state bytes
//   [Message thread] setNonRealtime(true) + bus coercion + prepareToPlay
//                    (render rate, block 512) + reset() (flush transient state)
//   [Worker thread ] exclusive ownership: processBlock via renderProxyDestination
//   [Message thread] releaseResources + destroy
//
// No editor is ever created. The LIVE audio-thread instance is never processed,
// reset, mutated or shared with the renderer — the job only ever holds its own
// isolated instance; the live pointer enters this file solely as an identity
// value for Debug/test distinctness assertions.
//
// This header needs juce_audio_processors (plugin hosting) and is included by
// APP code only; the deterministic selftests cover the worker body through the
// template seam in ProxyRenderExecutor.h instead.

#include "instruments/ProxyRenderExecutor.h"
#include "instruments/ProxyRenderSnapshot.h"
#include "instruments/ProxyRenderTypes.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <memory>
#include <utility>

namespace proxy_render
{

//==============================================================================
// The complete render request — deep-copied, immutable, self-sufficient (§1/§2).
// Holds NO references to Session/Track objects, editors, routing containers, the
// live plugin instance, or UI components; the snapshot inside is the P1C deep
// copy and the state blob is the exact opaque capture.
//==============================================================================
struct ProxyRenderRequest
{
    proxy_snapshot::ProxyRenderSnapshot snapshot; ///< immutable P1C destination snapshot
    juce::PluginDescription pluginDescription;    ///< identity for isolated-instance creation
    juce::String expectedFingerprint;             ///< computed at capture (message thread)
    std::uint64_t primarySemanticRevision = 0;    ///< §9.4.2 revision at capture
    double renderSampleRate = 48000.0;            ///< engine rate at enqueue (§15.3 render rate)
    int renderBlockSize = kRenderBlockSize;
    juce::File temporaryWavFile;                  ///< temp artifact target (outside project media)
};

//==============================================================================
// Message-thread lifecycle primitives
//==============================================================================
class ProxyRenderInstanceLifecycle final
{
public:
    struct CreateOutcome
    {
        std::unique_ptr<juce::AudioPluginInstance> instance; ///< prepared isolated instance
        ProxyRenderFailureReason failureReason = ProxyRenderFailureReason::None;
        juce::String error;
    };

    /// [Message thread] Create + restore + configure + prepare + reset the isolated render
    /// instance (the validated §4 initial-state sequence up to the worker handoff; the CC
    /// chase prefix is then emitted by the sequencer inside the first rendered block).
    [[nodiscard]] static CreateOutcome createPreparedIsolatedInstance(
        juce::AudioPluginFormatManager& formatManager,
        const juce::PluginDescription& desc,
        const juce::MemoryBlock& stateBlob,
        const double renderSampleRate,
        const int renderBlockSize)
    {
        jassert(juce::MessageManager::getInstance()->isThisTheMessageThread());
        CreateOutcome out;

        juce::String err;
        std::unique_ptr<juce::AudioPluginInstance> inst;
        try
        {
            inst = formatManager.createPluginInstance(desc, renderSampleRate, renderBlockSize, err);
        }
        catch (...)
        {
            inst = nullptr;
        }
        if (inst == nullptr)
        {
            out.failureReason = ProxyRenderFailureReason::PluginCreationFailed;
            out.error = err.isNotEmpty() ? err : juce::String("createPluginInstance failed");
            return out;
        }

        if (stateBlob.getSize() > 0)
        {
            try
            {
                inst->setStateInformation(stateBlob.getData(), (int)stateBlob.getSize());
            }
            catch (...)
            {
                out.failureReason = ProxyRenderFailureReason::StateRestoreFailed;
                out.error = "setStateInformation threw";
                return out; // inst destroyed here, on the message thread
            }
        }

        // Same bus-coercion shape as the product's tryPrepareInstrumentLayout
        // (ExperimentalInstrumentHost.cpp): accept 0-in/stereo-out, else coerce to it.
        const int mainIn = inst->getMainBusNumInputChannels();
        const int mainOut = inst->getMainBusNumOutputChannels();
        if (!(mainIn == 0 && mainOut >= 2))
        {
            inst->releaseResources();
            inst->setPlayConfigDetails(0, 2, renderSampleRate, renderBlockSize);
            juce::AudioProcessor::BusesLayout layout;
            layout.inputBuses.add(juce::AudioChannelSet::disabled());
            layout.outputBuses.add(juce::AudioChannelSet::stereo());
            if (!inst->setBusesLayout(layout))
            {
                inst->setPlayConfigDetails(0, 2, renderSampleRate, renderBlockSize);
            }
        }
        if (inst->getMainBusNumOutputChannels() < 2)
        {
            out.failureReason = ProxyRenderFailureReason::PrepareFailed;
            out.error = "no stereo main output after bus coercion";
            return out;
        }

        // Offline indication (correctness signal, §15.4; measured speed-neutral for VB3-II).
        inst->setNonRealtime(true);
        inst->prepareToPlay(renderSampleRate, renderBlockSize);
        // Measured §4 contract: flush transient state after prepare, before any scheduling.
        inst->reset();

        jassert(inst->getActiveEditor() == nullptr); // never create an editor for a render clone
        out.instance = std::move(inst);
        return out;
    }

    /// [Message thread] Verified-safe teardown after the worker returned ownership.
    static void teardownIsolatedInstance(std::unique_ptr<juce::AudioPluginInstance> inst) noexcept
    {
        jassert(juce::MessageManager::getInstance()->isThisTheMessageThread());
        if (inst == nullptr)
        {
            return;
        }
        jassert(inst->getActiveEditor() == nullptr);
        inst->releaseResources();
        inst.reset();
    }
};

//==============================================================================
// Foreground render job: one dedicated low-priority worker + message-thread poll
//==============================================================================
/// Owns the isolated instance and ONE render worker. Construction (message thread)
/// performs the lifecycle up to the worker handoff and starts the worker; the
/// message thread then polls isDone() (timer/idle) and calls finish() to join,
/// tear the instance down and take the result. The destructor is RAII-safe:
/// cancel + join + message-thread teardown, so failures/cancellation never leak
/// temporary files (executor guard) or plugin instances (this class).
class ProxyForegroundRenderJob final
{
public:
    /// [Message thread] `liveInstanceForIdentityCheck` is used ONLY as a pointer value to
    /// prove clone distinctness (never dereferenced here or on the worker).
    ProxyForegroundRenderJob(juce::AudioPluginFormatManager& formatManager,
                             ProxyRenderRequest request,
                             const void* liveInstanceForIdentityCheck)
        : request_(std::move(request)), livePtr_(liveInstanceForIdentityCheck)
    {
        jassert(juce::MessageManager::getInstance()->isThisTheMessageThread());
        auto created = ProxyRenderInstanceLifecycle::createPreparedIsolatedInstance(
            formatManager, request_.pluginDescription, request_.snapshot.pluginStateBlob,
            request_.renderSampleRate, request_.renderBlockSize);
        if (created.instance == nullptr)
        {
            failedResult_ = std::make_unique<ProxyRenderResult>();
            failedResult_->status = ProxyRenderStatus::Failed;
            failedResult_->failureReason = created.failureReason;
            failedResult_->message = created.error;
            failedResult_->expectedFingerprint = request_.expectedFingerprint;
            failedResult_->primarySemanticRevision = request_.primarySemanticRevision;
            failedResult_->renderSampleRate = request_.renderSampleRate;
            failedResult_->blockSize = request_.renderBlockSize;
            return;
        }
        // Debug/test identity contract: the render instance MUST differ from the live one.
        jassert((const void*)created.instance.get() != livePtr_);
        instance_ = std::move(created.instance);
        worker_ = std::make_unique<Worker>(*this);
        worker_->startThread(juce::Thread::Priority::low); // locked P1 resource default (§14.3)
    }

    ~ProxyForegroundRenderJob()
    {
        cancel();
        if (worker_ != nullptr)
        {
            worker_->stopThread(15000);
            worker_.reset();
        }
        ProxyRenderInstanceLifecycle::teardownIsolatedInstance(std::move(instance_));
    }

    /// [Any thread] Cooperative cancel (§9): prompt stop, Cancelled (not Failed), temp cleanup.
    void cancel() const noexcept { cancelToken_.requestCancel(); }

    [[nodiscard]] bool isDone() const noexcept
    {
        return failedResult_ != nullptr || (worker_ != nullptr && worker_->done.load(std::memory_order_acquire));
    }

    /// [Message thread] The isolated instance pointer for identity checks (poll only while no
    /// worker is running or after isDone(); never dereference on another thread).
    [[nodiscard]] const void* isolatedInstanceForIdentityCheck() const noexcept
    {
        return instance_.get();
    }
    [[nodiscard]] const void* liveInstanceForIdentityCheck() const noexcept { return livePtr_; }

    /// [Message thread] Join the finished worker, tear the isolated instance down and return
    /// the structured result. Call once after isDone().
    [[nodiscard]] ProxyRenderResult finish()
    {
        jassert(juce::MessageManager::getInstance()->isThisTheMessageThread());
        if (failedResult_ != nullptr)
        {
            return *failedResult_;
        }
        jassert(isDone());
        worker_->stopThread(15000);
        ProxyRenderResult result = std::move(worker_->result);
        worker_.reset();
        ProxyRenderInstanceLifecycle::teardownIsolatedInstance(std::move(instance_));
        return result;
    }

private:
    struct Worker final : juce::Thread
    {
        explicit Worker(ProxyForegroundRenderJob& j) : juce::Thread("ProxyRenderWorker"), job(j) {}
        ~Worker() override { stopThread(15000); }

        void run() override
        {
            // [Render worker] Exclusive owner of the isolated instance for the duration of
            // renderProxyDestination — the ONLY processBlock caller for the clone.
            ProxyRenderExecutionConfig cfg;
            cfg.renderSampleRate = job.request_.renderSampleRate;
            cfg.blockSize = job.request_.renderBlockSize;
            cfg.temporaryWavFile = job.request_.temporaryWavFile;
            cfg.expectedFingerprint = job.request_.expectedFingerprint;
            cfg.primarySemanticRevision = job.request_.primarySemanticRevision;
            result = renderProxyDestination(*job.instance_, job.request_.snapshot, cfg,
                                            job.cancelToken_);
            done.store(true, std::memory_order_release);
        }

        ProxyForegroundRenderJob& job;
        ProxyRenderResult result;
        std::atomic<bool> done{ false };
    };

    ProxyRenderRequest request_;
    const void* livePtr_ = nullptr;
    std::unique_ptr<juce::AudioPluginInstance> instance_;
    std::unique_ptr<Worker> worker_;
    std::unique_ptr<ProxyRenderResult> failedResult_;
    ProxyRenderCancellationToken cancelToken_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ProxyForegroundRenderJob)
};

} // namespace proxy_render
