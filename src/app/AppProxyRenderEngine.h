#pragma once

// =============================================================================
// AppProxyRenderEngine — the production ProxyRenderJobEngine (P1E/P1F glue)
// =============================================================================
// Composes the verified production seams behind the scheduler's engine
// interface — it never reimplements them:
//
//   * request capture  = P1C snapshot builder + canonical fingerprint + exact
//     opaque Primary state capture (moved here from MainAppWindow; the window
//     now only calls the narrow scheduler API);
//   * prepare/teardown = P1D ProxyRenderInstanceLifecycle (message thread);
//   * render           = P1D renderProxyDestination (worker; pause gate wired);
//   * publish          = P1F ProxyAssetStore atomic publication + the
//     destination controller's in-memory v20 metadata update (message thread;
//     no musical undo entry, no track-state rewrite, visible to the next save);
//   * currency         = fingerprint + Primary semantic revision recomputed
//     from live musical content (§9.4.2 — never a fresh state-blob hash).
//
// OWNERSHIP: constructed by TransportControlsContent (which owns the instrument
// runtime coordinator the delegates reach into) and attached to the
// application-owned ProxyRenderScheduler. The content view MUST call
// scheduler.detachEngineAndShutdownJobs() before this engine (and the
// coordinators behind it) is destroyed — see ~TransportControlsContent.
//
// THREADING: every method here runs on the MESSAGE thread except
// PreparedAppJob::render (worker). The prepared job owns the isolated instance
// exclusively; its destructor (message thread, scheduler contract) runs the
// verified teardown. The LIVE audio-thread instance is never processed, reset,
// mutated or shared — its pointer enters only as an identity value (PI-011).

#include "domain/Session.h"
#include "instruments/InstrumentTrackController.h"
#include "instruments/ProxyAssetStore.h"
#include "instruments/ProxyFingerprint.h"
#include "instruments/ProxyRenderInstanceLifecycle.h"
#include "instruments/ProxyRenderScheduler.h"
#include "instruments/ProxyRenderSnapshot.h"
#include "plugins/ExperimentalInstrumentHost.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

namespace proxy_render
{

class AppProxyRenderEngine final : public ProxyRenderJobEngine
{
public:
    struct Dependencies
    {
        Session* session = nullptr;
        juce::AudioDeviceManager* deviceManager = nullptr;
        /// [Message thread] Live host of a destination (nullptr when absent).
        std::function<ExperimentalInstrumentHost*(TrackId)> hostForTrack;
        /// [Message thread] Controller owning clips + v20 proxy metadata.
        std::function<InstrumentTrackController*(TrackId)> controllerForTrack;
        /// [Message thread] Stored clips of any track id (destination + sources).
        std::function<std::vector<const InstrumentMidiClip*>(TrackId)> clipsForTrack;
        /// [Message thread, optional] Fired after a successful publication + metadata
        /// update (P1G: lets the playback coordinator re-evaluate source selection).
        std::function<void(TrackId)> onProxyPublished;
    };

    explicit AppProxyRenderEngine(Dependencies deps) : deps_(std::move(deps))
    {
        jassert(deps_.session != nullptr && deps_.deviceManager != nullptr);
        formatManager_.addFormat(new juce::VST3PluginFormat());
    }

    //==========================================================================
    // Identity (currency comparisons; §12.3 derivation)
    //==========================================================================
    ProxyCurrentIdentity currentIdentity(const TrackId destination) override
    {
        ProxyCurrentIdentity id;
        proxy_snapshot::ProxyRenderSnapshot snap;
        std::uint64_t revision = 0;
        if (!buildIdentitySnapshot(destination, snap, revision))
        {
            return id; // destination missing / no Primary ⇒ not renderable
        }
        id.destinationExists = true;
        id.expectedFingerprint = proxy_fingerprint::computeFingerprint(snap);
        id.primarySemanticRevision = revision;
        return id;
    }

    //==========================================================================
    // Immutable request capture (message thread; the former MainAppWindow P1D
    // builder — §2 snapshot-and-identity sequence, everything deep-copied)
    //==========================================================================
    struct CapturedAppRequest final : ProxyCapturedRequest
    {
        TrackId destination = kInvalidTrackId;
        ProxyRenderRequest request;
    };

    std::unique_ptr<ProxyCapturedRequest> captureRequest(const TrackId destination,
                                                         juce::String& errorOut) override
    {
        auto* host = deps_.hostForTrack ? deps_.hostForTrack(destination) : nullptr;
        if (host == nullptr || !host->hasInstrument())
        {
            errorOut = "destination has no loaded instrument";
            return nullptr;
        }
        juce::PluginDescription desc;
        if (!host->getLastLoadedPluginDescription(desc))
        {
            errorOut = "no plugin description for the destination instrument";
            return nullptr;
        }
        // Exact opaque Primary state bytes (§9.4 authoritative capture).
        juce::MemoryBlock stateBlob;
        if (!host->captureInstrumentStateForRender(stateBlob))
        {
            errorOut = "Primary state capture failed or returned no bytes";
            return nullptr;
        }
        const auto sessionSnap = deps_.session->loadSessionSnapshotForAudioThread();
        if (sessionSnap == nullptr)
        {
            errorOut = "no session snapshot";
            return nullptr;
        }

        proxy_snapshot::BuildInputs in;
        fillBuildInputs(in, desc, host->getPrimarySemanticRevision());
        in.pluginStateBlob = stateBlob;

        auto captured = std::make_unique<CapturedAppRequest>();
        captured->destination = destination;
        captured->request.snapshot = proxy_snapshot::buildProxyRenderSnapshot(
            *sessionSnap, destination, deps_.clipsForTrack, in);
        captured->request.pluginDescription = desc;
        captured->request.expectedFingerprint
            = proxy_fingerprint::computeFingerprint(captured->request.snapshot);
        captured->request.primarySemanticRevision = in.stateIdentity.primaryStateRevision;
        captured->request.renderSampleRate = in.renderConfig.renderSampleRate;
        captured->request.renderBlockSize = kRenderBlockSize;
        // Temp target: same directory tree as the final asset (same-volume rename,
        // §16.1) when the project has a saved location; otherwise a bounded
        // system-temp file (publication then reports WaitingForProjectLocation).
        const juce::File projectFolder = deps_.session->getCurrentProjectFolder();
        const std::uint64_t jobSalt = ++tempSalt_;
        captured->request.temporaryWavFile
            = (projectFolder != juce::File() && projectFolder.isDirectory())
                  ? proxy_store::tempRenderTarget(projectFolder, destination, jobSalt)
                  : juce::File::getSpecialLocation(juce::File::tempDirectory)
                        .getChildFile("MiniDAWLab")
                        .getChildFile("proxy-unsaved-" + juce::String((juce::int64)destination)
                                      + "-" + juce::String((juce::int64)jobSalt) + "-"
                                      + juce::String(juce::Time::currentTimeMillis()) + ".wav");
        captured->expectedFingerprint = captured->request.expectedFingerprint;
        captured->primarySemanticRevision = captured->request.primarySemanticRevision;
        return captured;
    }

    //==========================================================================
    // Isolated instance lifecycle (P1D, message thread) + worker render body
    //==========================================================================
    struct PreparedAppJob final : ProxyPreparedJob
    {
        PreparedAppJob(std::unique_ptr<juce::AudioPluginInstance> instance,
                       ProxyRenderRequest request,
                       const bool distinctFromLive)
            : instance_(std::move(instance)), request_(std::move(request)),
              distinctFromLive_(distinctFromLive)
        {
        }

        ~PreparedAppJob() override
        {
            // Scheduler contract: destruction happens on the message thread.
            ProxyRenderInstanceLifecycle::teardownIsolatedInstance(std::move(instance_));
        }

        ProxyRenderResult render(const ProxyRenderCancellationToken& cancel,
                                 const std::function<void()>& waitWhilePaused) override
        {
            ProxyRenderExecutionConfig cfg;
            cfg.renderSampleRate = request_.renderSampleRate;
            cfg.blockSize = request_.renderBlockSize;
            cfg.temporaryWavFile = request_.temporaryWavFile;
            cfg.expectedFingerprint = request_.expectedFingerprint;
            cfg.primarySemanticRevision = request_.primarySemanticRevision;
            cfg.blockBoundaryPauseGate = waitWhilePaused; // recording pause (§14.3)
            ProxyRenderResult r
                = renderProxyDestination(*instance_, request_.snapshot, cfg, cancel);
            r.renderInstanceDistinctFromLive = distinctFromLive_;
            return r;
        }

        std::unique_ptr<juce::AudioPluginInstance> instance_;
        ProxyRenderRequest request_;
        bool distinctFromLive_ = false;
    };

    std::unique_ptr<ProxyPreparedJob> prepare(const ProxyCapturedRequest& request,
                                              ProxyRenderResult& failureOut) override
    {
        const auto& c = static_cast<const CapturedAppRequest&>(request);
        auto created = ProxyRenderInstanceLifecycle::createPreparedIsolatedInstance(
            formatManager_, c.request.pluginDescription, c.request.snapshot.pluginStateBlob,
            c.request.renderSampleRate, c.request.renderBlockSize);
        if (created.instance == nullptr)
        {
            failureOut.status = ProxyRenderStatus::Failed;
            failureOut.failureReason = created.failureReason;
            failureOut.message = created.error;
            failureOut.expectedFingerprint = c.request.expectedFingerprint;
            failureOut.primarySemanticRevision = c.request.primarySemanticRevision;
            return nullptr;
        }
        // PI-011 evidence: the render instance MUST differ from the live one. The
        // live pointer is used as a VALUE only — never dereferenced or exposed.
        const void* livePtr = nullptr;
        if (auto* host = deps_.hostForTrack ? deps_.hostForTrack(c.destination) : nullptr)
        {
            livePtr = host->spike01LiveInstanceForDiagnostics();
        }
        const bool distinct = (const void*)created.instance.get() != livePtr;
        jassert(distinct);
        return std::make_unique<PreparedAppJob>(std::move(created.instance), c.request, distinct);
    }

    //==========================================================================
    // Atomic publication + metadata update (P1F, message thread)
    //==========================================================================
    bool publish(const TrackId destination, const ProxyCapturedRequest& request,
                 const ProxyRenderResult& result, juce::String& errorOut) override
    {
        const auto& c = static_cast<const CapturedAppRequest&>(request);
        auto* controller = deps_.controllerForTrack ? deps_.controllerForTrack(destination)
                                                    : nullptr;
        if (controller == nullptr)
        {
            errorOut = "destination controller unavailable";
            return false;
        }

        proxy_store::ProxyPublishOutcome outcome;
        if (result.status == ProxyRenderStatus::SucceededSilent)
        {
            // §15.7 explicit silent generation: metadata only, no WAV, no fake path.
            outcome = proxy_store::publishSilentGeneration(result,
                                                           c.request.snapshot.policies,
                                                           &c.request.snapshot);
        }
        else
        {
            outcome = proxy_store::publishRenderedProxy(deps_.session->getCurrentProjectFolder(),
                                                        destination, result,
                                                        c.request.snapshot.policies,
                                                        &c.request.snapshot);
        }
        if (!outcome.ok)
        {
            // Previous generation metadata/asset untouched (structural retention).
            errorOut = outcome.error;
            return false;
        }
        // §16.3 step 7: in-memory metadata update AFTER the validated rename. Cache
        // metadata: no musical undo entry, no track-state rewrite; the next normal
        // project save persists it (the save DTO reads these controller fields).
        controller->setProxyMetadataFromPublication(outcome.metadata);
        // P1H Save As rehoming: remember the published asset's absolute location while
        // it is authoritatively known (runtime-only hint; empty for silent generations).
        controller->setProxyAssetSourceHint(outcome.finalFile);
        if (deps_.onProxyPublished)
        {
            deps_.onProxyPublished(destination);
        }
        return true;
    }

    juce::String publishedGenerationId(const TrackId destination) override
    {
        auto* controller = deps_.controllerForTrack ? deps_.controllerForTrack(destination)
                                                    : nullptr;
        if (controller == nullptr)
        {
            return {};
        }
        const auto* metadata = controller->getProxyMetadata();
        return metadata != nullptr ? metadata->generationId : juce::String();
    }

private:
    /// Shared BuildInputs population (capture + identity paths use identical
    /// configuration so their fingerprints are comparable).
    void fillBuildInputs(proxy_snapshot::BuildInputs& in, const juce::PluginDescription& desc,
                         const std::uint64_t revision) const
    {
        in.pluginIdentity.fileOrIdentifier = desc.fileOrIdentifier;
        in.pluginIdentity.uniqueId = desc.uniqueId;
        in.pluginIdentity.deprecatedUid = desc.deprecatedUid;
        in.pluginIdentity.format = desc.pluginFormatName;
        in.pluginIdentity.isInstrument = desc.isInstrument;
        in.pluginIdentity.version = desc.version;
        in.stateIdentity.primaryStateRevision = revision;
        in.stateIdentity.pairedWithSavedState = false;
        // §15.3 Locked: v1 renders at the current engine rate at enqueue time;
        // persisted sample-domain placement stays under the timeline REFERENCE rate.
        double renderRate = 0.0;
        if (juce::AudioIODevice* dev = deps_.deviceManager->getCurrentAudioDevice())
        {
            renderRate = dev->getCurrentSampleRate();
        }
        const double referenceRate
            = deps_.session->timelineSampleRateOr(renderRate > 0.0 ? renderRate : 48000.0);
        in.renderConfig.renderSampleRate = renderRate > 0.0 ? renderRate : referenceRate;
        in.renderConfig.renderBlockSize = kRenderBlockSize;
        in.renderConfig.timelineReferenceRate = referenceRate;
        in.renderConfig.noteOffGateMs = 100;
        // §15.7 conservative default: nothing is classified host-event-driven yet.
        in.instrumentClassifiedHostEventDriven = false;
    }

    /// Identity-only snapshot (EMPTY state blob — blob bytes are render content,
    /// never fingerprint identity, §9.4.2). Returns false when not renderable.
    bool buildIdentitySnapshot(const TrackId destination,
                               proxy_snapshot::ProxyRenderSnapshot& out,
                               std::uint64_t& revisionOut) const
    {
        auto* host = deps_.hostForTrack ? deps_.hostForTrack(destination) : nullptr;
        if (host == nullptr || !host->hasInstrument())
        {
            return false;
        }
        juce::PluginDescription desc;
        if (!host->getLastLoadedPluginDescription(desc))
        {
            return false;
        }
        const auto sessionSnap = deps_.session->loadSessionSnapshotForAudioThread();
        if (sessionSnap == nullptr)
        {
            return false;
        }
        proxy_snapshot::BuildInputs in;
        fillBuildInputs(in, desc, host->getPrimarySemanticRevision());
        out = proxy_snapshot::buildProxyRenderSnapshot(*sessionSnap, destination,
                                                       deps_.clipsForTrack, in);
        revisionOut = in.stateIdentity.primaryStateRevision;
        return true;
    }

    Dependencies deps_;
    juce::AudioPluginFormatManager formatManager_;
    std::atomic<std::uint64_t> tempSalt_{ 0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AppProxyRenderEngine)
};

} // namespace proxy_render
