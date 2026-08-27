#include "app/InstrumentRuntimeCoordinator.h"

#include <map>

#include "diagnostics/ProjectLoadDiagnosticLog.h"
#include "diagnostics/StabilityDiagnosticLog.h"
#include "domain/Session.h"
#include "domain/SessionSnapshot.h"
#include "domain/Track.h"
#include "engine/PlaybackEngine.h"
#include "instruments/InstrumentTrackController.h"
#include "plugins/ExperimentalInstrumentHost.h"
#include "plugins/Vst3ChildProcessScan.h"

#include "diagnostics/DiagnosticBuildFlags.h"
#include "diagnostics/ExperimentalPlaybackRoutingLog.h"

InstrumentRuntimeCoordinator::InstrumentRuntimeCoordinator(Session& session,
                                                         PlaybackEngine& playbackEngine,
                                                         Callbacks callbacks)
    : session_(session)
    , playbackEngine_(playbackEngine)
    , callbacks_(std::move(callbacks))
{
}

void InstrumentRuntimeCoordinator::wireExperimentalInstrumentHost(ExperimentalInstrumentHost& host,
                                                                    InstrumentTrackController& ctrl) noexcept
{
    host.setDrumNamePhaseCAudioProbeShouldSkip([this]() noexcept {
        return invokeDrumNamePhaseAudioProbeShouldSkipPredicate();
    });
    host.setOnPluginDrumNamesDiscovered([&host, &ctrl](const std::map<int, juce::String>& discovered) {
        juce::PluginDescription d{};
        const juce::String pluginId
            = host.getLastLoadedPluginDescription(d) ? d.createIdentifierString() : juce::String{};
        ctrl.mergeAutoPluginDrumLabels(discovered, pluginId);
        ExperimentalInstrumentHost::appendInstrumentHostLogLine(
            "drum-track: mergeAutoPluginDrumLabels source=afterEditorOpen keys="
            + juce::String(static_cast<int>(discovered.size())));
    });
}

bool InstrumentRuntimeCoordinator::invokeDrumNamePhaseAudioProbeShouldSkipPredicate() noexcept
{
    if (callbacks_.drumNamePhaseCAudioProbeShouldSkip == nullptr)
    {
        return false;
    }
    return callbacks_.drumNamePhaseCAudioProbeShouldSkip();
}

void InstrumentRuntimeCoordinator::runSyncInstrumentTimelineRowAttachmentCallback()
{
    if (callbacks_.syncInstrumentTimelineRowAttachmentToSession != nullptr)
    {
        callbacks_.syncInstrumentTimelineRowAttachmentToSession();
    }
}

TrackId InstrumentRuntimeCoordinator::canonicalInstrumentLaneTrackIdFromSession() const noexcept
{
    const std::shared_ptr<const SessionSnapshot> snap = session_.loadSessionSnapshotForAudioThread();
    if (snap == nullptr)
    {
        return kInvalidTrackId;
    }
    for (int ti = 0; ti < snap->getNumTracks(); ++ti)
    {
        const Track& tr = snap->getTrack(ti);
        if (tr.getKind() == TrackKind::Instrument)
        {
            return tr.getId();
        }
    }
    return kInvalidTrackId;
}

bool InstrumentRuntimeCoordinator::anyHeldHostShowsGrooveAgentLoaded() const noexcept
{
    return findGrooveAgentTemplateHostPreferKeyed(nullptr) != nullptr;
}

bool InstrumentRuntimeCoordinator::anyHeldHostShowsHalionSonicLoaded() const noexcept
{
    return findHalionSonicTemplateHostPreferKeyed(nullptr) != nullptr;
}

ExperimentalInstrumentHost*
    InstrumentRuntimeCoordinator::findGrooveAgentTemplateHostPreferKeyed(
        ExperimentalInstrumentHost* avoidSameAs) const noexcept
{
    for (const auto& kv : instrumentHostsByTrackId_)
    {
        if (kv.second != nullptr && kv.second.get() != avoidSameAs && kv.second->hasInstrument()
            && kv.second->getInstrumentNameForUi().containsIgnoreCase("Groove Agent"))
        {
            return kv.second.get();
        }
    }
    if (instrumentStagingHost_ != nullptr && instrumentStagingHost_.get() != avoidSameAs && instrumentStagingHost_->hasInstrument()
        && instrumentStagingHost_->getInstrumentNameForUi().containsIgnoreCase("Groove Agent"))
    {
        return instrumentStagingHost_.get();
    }
    return nullptr;
}

ExperimentalInstrumentHost*
    InstrumentRuntimeCoordinator::findHalionSonicTemplateHostPreferKeyed(
        ExperimentalInstrumentHost* avoidSameAs) const noexcept
{
    for (const auto& kv : instrumentHostsByTrackId_)
    {
        if (kv.second != nullptr && kv.second.get() != avoidSameAs && kv.second->hasInstrument()
            && mini_daw::instrumentDisplayNameLooksLikeHalionSonic(kv.second->getInstrumentNameForUi()))
        {
            return kv.second.get();
        }
    }
    if (instrumentStagingHost_ != nullptr && instrumentStagingHost_.get() != avoidSameAs && instrumentStagingHost_->hasInstrument()
        && mini_daw::instrumentDisplayNameLooksLikeHalionSonic(instrumentStagingHost_->getInstrumentNameForUi()))
    {
        return instrumentStagingHost_.get();
    }
    return nullptr;
}

ExperimentalInstrumentHost* InstrumentRuntimeCoordinator::getInstrumentHostForTrack(const TrackId tid) const noexcept
{
    auto it = instrumentHostsByTrackId_.find(tid);
    return (it == instrumentHostsByTrackId_.end()) ? nullptr : it->second.get();
}

InstrumentTrackController* InstrumentRuntimeCoordinator::getInstrumentControllerForTrack(const TrackId tid) const noexcept
{
    auto it = instrumentControllersByTrackId_.find(tid);
    return (it == instrumentControllersByTrackId_.end()) ? nullptr : it->second.get();
}

void InstrumentRuntimeCoordinator::forEachInstrumentController(
    const std::function<void(TrackId, InstrumentTrackController&)>& fn)
{
    if (fn == nullptr)
    {
        return;
    }
    for (auto& kv : instrumentControllersByTrackId_)
    {
        if (kv.second != nullptr)
        {
            fn(kv.first, *kv.second);
        }
    }
}

bool InstrumentRuntimeCoordinator::isKeyedRuntimeRegistryEmpty() const noexcept
{
    return instrumentHostsByTrackId_.empty();
}

std::vector<std::tuple<TrackId, const void*, const void*>>
    InstrumentRuntimeCoordinator::exportKeyedRuntimePointersForDiagnostics() const
{
    std::vector<std::tuple<TrackId, const void*, const void*>> out;
    out.reserve(instrumentHostsByTrackId_.size());
    for (const auto& [tid, host] : instrumentHostsByTrackId_)
    {
        const auto ctlIt = instrumentControllersByTrackId_.find(tid);
        out.emplace_back(tid,
                         static_cast<const void*>(host.get()),
                         ctlIt != instrumentControllersByTrackId_.end()
                             ? static_cast<const void*>(ctlIt->second.get())
                             : nullptr);
    }
    return out;
}

ExperimentalInstrumentHost* InstrumentRuntimeCoordinator::stagingInstrumentHostUnchecked() const noexcept
{
    return instrumentStagingHost_.get();
}

InstrumentTrackController* InstrumentRuntimeCoordinator::stagingInstrumentControllerUnchecked() const noexcept
{
    return instrumentStagingController_.get();
}

std::pair<ExperimentalInstrumentHost*, InstrumentTrackController*>
    InstrumentRuntimeCoordinator::getOrCreateInstrumentRuntimeForTrack(const TrackId tid)
{
    if (tid == kInvalidTrackId)
    {
        return { nullptr, nullptr };
    }
    ExperimentalInstrumentHost* hostExisting = getInstrumentHostForTrack(tid);
    InstrumentTrackController* ctlExisting = getInstrumentControllerForTrack(tid);
    if (hostExisting != nullptr && ctlExisting != nullptr)
    {
        return { hostExisting, ctlExisting };
    }
    if (instrumentHostsByTrackId_.empty() && instrumentStagingController_ != nullptr
        && instrumentStagingHost_ != nullptr && instrumentStagingController_->hasInstrumentTrack()
        && instrumentStagingController_->getExperimentalInstrumentDomainTrackId() == tid)
    {
        promoteInstrumentStagingIntoRegistryBoundTo(tid);
        return { getInstrumentHostForTrack(tid), getInstrumentControllerForTrack(tid) };
    }

    auto host = std::make_unique<ExperimentalInstrumentHost>();
    auto ctl = std::make_unique<InstrumentTrackController>(*host);
    ctl->setSession(&session_);
    wireExperimentalInstrumentHost(*host, *ctl);
    ExperimentalInstrumentHost* const hostPtr = host.get();
    InstrumentTrackController* const ctlPtr = ctl.get();
    if (lastPreparedDeviceSampleRate_ > 0.0 && lastPreparedDeviceBlockSize_ > 0)
    {
        hostPtr->prepareForDevice(lastPreparedDeviceSampleRate_, lastPreparedDeviceBlockSize_);
    }
    instrumentHostsByTrackId_.emplace(tid, std::move(host));
    instrumentControllersByTrackId_.emplace(tid, std::move(ctl));
    updateExperimentalPlaybackBridgeAfterRegistryChange();
    runSyncInstrumentTimelineRowAttachmentCallback();
    return { hostPtr, ctlPtr };
}

std::pair<ExperimentalInstrumentHost*, InstrumentTrackController*>
    InstrumentRuntimeCoordinator::getExperimentRuntimePairForGrooveAdds()
{
    if (instrumentStagingHost_ == nullptr || instrumentStagingController_ == nullptr)
    {
        instrumentStagingHost_ = std::make_unique<ExperimentalInstrumentHost>();
        instrumentStagingController_ = std::make_unique<InstrumentTrackController>(*instrumentStagingHost_);
        instrumentStagingController_->setSession(&session_);
        wireExperimentalInstrumentHost(*instrumentStagingHost_, *instrumentStagingController_);
        if (lastPreparedDeviceSampleRate_ > 0.0 && lastPreparedDeviceBlockSize_ > 0)
        {
            instrumentStagingHost_->prepareForDevice(lastPreparedDeviceSampleRate_, lastPreparedDeviceBlockSize_);
        }
        updateExperimentalPlaybackBridgeAfterRegistryChange();
        runSyncInstrumentTimelineRowAttachmentCallback();
    }
    return { instrumentStagingHost_.get(), instrumentStagingController_.get() };
}

void InstrumentRuntimeCoordinator::promoteInstrumentStagingIntoRegistryBoundTo(const TrackId tid)
{
    if (tid == kInvalidTrackId || instrumentStagingHost_ == nullptr || instrumentStagingController_ == nullptr)
    {
        return;
    }
    if (!instrumentStagingController_->hasInstrumentTrack())
    {
        return;
    }
    if (!instrumentHostsByTrackId_.empty())
    {
        juce::Logger::writeToLog(
            "[TransportControlsContent] promoteInstrumentStaging: registry unexpectedly non-empty (TrackId="
            + juce::String((juce::int64)tid) + ").");
        return;
    }
    instrumentHostsByTrackId_[tid] = std::move(instrumentStagingHost_);
    instrumentControllersByTrackId_[tid] = std::move(instrumentStagingController_);
    updateExperimentalPlaybackBridgeAfterRegistryChange();
    runSyncInstrumentTimelineRowAttachmentCallback();
}

void InstrumentRuntimeCoordinator::removeInstrumentRuntimeForTrack(const TrackId tid) noexcept
{
    // Publish-before-destroy (F4): retire the runtime out of the registry first, republish the
    // playback bridge without it, drain the in-flight audio callback (which may still hold the
    // previous snapshot with raw host/controller pointers), and only then unload/destroy.
    std::unique_ptr<ExperimentalInstrumentHost> retiredHost;
    std::unique_ptr<InstrumentTrackController> retiredController;
    if (const auto it = instrumentHostsByTrackId_.find(tid); it != instrumentHostsByTrackId_.end())
    {
        retiredHost = std::move(it->second);
        instrumentHostsByTrackId_.erase(it);
    }
    if (const auto it = instrumentControllersByTrackId_.find(tid);
        it != instrumentControllersByTrackId_.end())
    {
        retiredController = std::move(it->second);
        instrumentControllersByTrackId_.erase(it);
    }

    updateExperimentalPlaybackBridgeAfterRegistryChange();

    if (retiredHost != nullptr || retiredController != nullptr)
    {
        double waitedMs = 0.0;
        const bool drained = playbackEngine_.waitForAudioCallbackExit(250.0, &waitedMs);
        if (!drained)
        {
            // Stability C2B: identify where the callback is stuck when the drain times out.
            appendTrackDeleteDiagnosticLine(
                "drain timeout state: "
                + playbackEngine_.describeAudioCallbackStateForDiagnostics());
        }
        appendTrackDeleteDiagnosticLine(
            "instrument runtime retire trackId=" + juce::String((juce::int64)tid)
            + ": bridge republished; callback drain waitedMs=" + juce::String(waitedMs, 2)
            + " timeout=" + (drained ? "no" : "YES (proceeding anyway)"));
    }

    if (retiredHost != nullptr)
    {
        retiredHost->clearControllerWireCallbacks();
        retiredHost->unloadInstrument();
    }
    // Controller references the host; destroy it first.
    retiredController.reset();
    retiredHost.reset();
    runSyncInstrumentTimelineRowAttachmentCallback();
}

bool InstrumentRuntimeCoordinator::moveInstrumentMidiClipsBetweenTracks(
    const TrackId sourceTrackId,
    const TrackId destTrackId,
    std::vector<InstrumentMidiClipId> clipIdsInOrder,
    const std::int64_t deltaSamples) noexcept
{
    if (sourceTrackId == kInvalidTrackId || destTrackId == kInvalidTrackId || sourceTrackId == destTrackId
        || clipIdsInOrder.empty())
    {
        return false;
    }

    const std::shared_ptr<const SessionSnapshot> snap = session_.loadSessionSnapshotForAudioThread();
    if (snap == nullptr)
    {
        return false;
    }
    const int si = snap->findTrackIndexById(sourceTrackId);
    const int di = snap->findTrackIndexById(destTrackId);
    if (si < 0 || di < 0)
    {
        return false;
    }
    if (snap->getTrack(si).getKind() != TrackKind::Instrument
        || snap->getTrack(di).getKind() != TrackKind::Instrument)
    {
        return false;
    }

    InstrumentTrackController* const sourceCtl = getInstrumentControllerForTrack(sourceTrackId);
    InstrumentTrackController* const destCtl = getInstrumentControllerForTrack(destTrackId);
    if (sourceCtl == nullptr || destCtl == nullptr || !sourceCtl->hasInstrumentTrack()
        || !destCtl->hasInstrumentTrack())
    {
        return false;
    }

    std::vector<InstrumentMidiClip> snapshots;
    std::vector<std::pair<std::int64_t, std::int64_t>> newStartsAnchors;
    snapshots.reserve(clipIdsInOrder.size());
    newStartsAnchors.reserve(clipIdsInOrder.size());

    for (const InstrumentMidiClipId id : clipIdsInOrder)
    {
        const InstrumentMidiClip* const c = sourceCtl->getClipById(id);
        if (c == nullptr)
        {
            return false;
        }
        snapshots.push_back(*c);
        const std::int64_t ns = c->startSamples + deltaSamples;
        const std::int64_t na = c->timelineAnchorSamples + deltaSamples;
        newStartsAnchors.emplace_back(juce::jmax(std::int64_t{ 0 }, ns), na);
    }

    if (!sourceCtl->removeInstrumentMidiClipsByIds(clipIdsInOrder))
    {
        return false;
    }

    std::vector<InstrumentMidiClipId> newIds
        = destCtl->appendDeepCopiedInstrumentMidiClips(snapshots, newStartsAnchors);
    if (newIds.size() != snapshots.size())
    {
        std::vector<std::pair<std::int64_t, std::int64_t>> restoreStartsAnchors;
        restoreStartsAnchors.reserve(snapshots.size());
        for (const auto& s : snapshots)
        {
            restoreStartsAnchors.emplace_back(s.startSamples, s.timelineAnchorSamples);
        }
        [[maybe_unused]] const auto restored
            = sourceCtl->appendDeepCopiedInstrumentMidiClips(snapshots, restoreStartsAnchors);
        juce::ignoreUnused(restored);
        return false;
    }

    destCtl->replaceInstrumentMidiClipSelectionOrdered(std::move(newIds));
    return true;
}

void InstrumentRuntimeCoordinator::clearRuntimesPreserveBridgeOnly() noexcept
{
    playbackEngine_.publishExperimentalInstrumentPlaybackSnapshot(nullptr);

    const auto detachAndUnloadHost = [](ExperimentalInstrumentHost* host) noexcept {
        if (host == nullptr)
        {
            return;
        }
        host->clearControllerWireCallbacks();
        host->unloadInstrument();
    };

    for (auto& kv : instrumentHostsByTrackId_)
    {
        detachAndUnloadHost(kv.second.get());
    }
    detachAndUnloadHost(instrumentStagingHost_.get());

    instrumentStagingController_.reset();
    instrumentStagingHost_.reset();
    instrumentControllersByTrackId_.clear();
    instrumentHostsByTrackId_.clear();
}

void InstrumentRuntimeCoordinator::experimentalBeginAudioBlockAllHosts(const std::int64_t numSamples) noexcept
{
    for (auto& kv : instrumentHostsByTrackId_)
    {
        if (kv.second != nullptr)
        {
            kv.second->audioThread_beginAudioBlock((int)numSamples);
        }
    }
    if (instrumentStagingHost_ != nullptr)
    {
        instrumentStagingHost_->audioThread_beginAudioBlock((int)numSamples);
    }
}

void InstrumentRuntimeCoordinator::prepareExperimentalInstrumentHostsForDevice(const double sampleRate,
                                                                               const int blockSamples) noexcept
{
    if (sampleRate > 0.0 && blockSamples > 0)
    {
        lastPreparedDeviceSampleRate_ = sampleRate;
        lastPreparedDeviceBlockSize_ = blockSamples;
    }

    for (auto& kv : instrumentHostsByTrackId_)
    {
        if (kv.second != nullptr)
        {
            kv.second->prepareForDevice(sampleRate, blockSamples);
        }
    }
    if (instrumentStagingHost_)
    {
        instrumentStagingHost_->prepareForDevice(sampleRate, blockSamples);
    }
}

void InstrumentRuntimeCoordinator::releaseExperimentalInstrumentHostsDeviceResources() noexcept
{
    for (auto& kv : instrumentHostsByTrackId_)
    {
        if (kv.second != nullptr)
        {
            kv.second->releaseResources();
        }
    }
    if (instrumentStagingHost_)
    {
        instrumentStagingHost_->releaseResources();
    }
}

void InstrumentRuntimeCoordinator::reconcileInstrumentRegistryAgainstSessionRows() noexcept
{
    if (instrumentHostsByTrackId_.size() != instrumentControllersByTrackId_.size())
    {
        return;
    }

    for (;;)
    {
        TrackId staleKey = kInvalidTrackId;
        TrackId domTarget = kInvalidTrackId;
        InstrumentTrackController* ctlProbe = nullptr;
        ExperimentalInstrumentHost* hostProbe = nullptr;

        for (const auto& kv : instrumentControllersByTrackId_)
        {
            InstrumentTrackController* const ctl = kv.second.get();
            if (ctl == nullptr || !ctl->hasInstrumentTrack())
            {
                continue;
            }
            const TrackId dom = ctl->getExperimentalInstrumentDomainTrackId();
            if (dom == kInvalidTrackId || kv.first == dom)
            {
                continue;
            }
            staleKey = kv.first;
            domTarget = dom;
            ctlProbe = ctl;
            auto hi = instrumentHostsByTrackId_.find(staleKey);
            hostProbe = (hi != instrumentHostsByTrackId_.end()) ? hi->second.get() : nullptr;
            break;
        }

        juce::ignoreUnused(ctlProbe);
        if (staleKey == kInvalidTrackId || domTarget == kInvalidTrackId || hostProbe == nullptr)
        {
            break;
        }
        auto hostIt = instrumentHostsByTrackId_.find(staleKey);
        auto ctlIt = instrumentControllersByTrackId_.find(staleKey);
        if (hostIt == instrumentHostsByTrackId_.end() || ctlIt == instrumentControllersByTrackId_.end())
        {
            break;
        }
        if (instrumentHostsByTrackId_.count(domTarget) != 0 || instrumentControllersByTrackId_.count(domTarget) != 0)
        {
            juce::Logger::writeToLog(
                "[TransportControlsContent] Instrument re-key aborted: collision at domain TrackId="
                + juce::String((juce::int64)domTarget) + " staleKey="
                + juce::String((juce::int64)staleKey));
            break;
        }

        std::unique_ptr<ExperimentalInstrumentHost> uh = std::move(hostIt->second);
        std::unique_ptr<InstrumentTrackController> uc = std::move(ctlIt->second);
        instrumentHostsByTrackId_.erase(staleKey);
        instrumentControllersByTrackId_.erase(staleKey);
        instrumentHostsByTrackId_[domTarget] = std::move(uh);
        instrumentControllersByTrackId_[domTarget] = std::move(uc);

        juce::Logger::writeToLog("[TransportControlsContent] Instrument runtime maps re-keyed: map key "
                                 + juce::String((juce::int64)(std::int64_t)staleKey)
                                 + " -> controller domain track id "
                                 + juce::String((juce::int64)(std::int64_t)domTarget));
    }
}

void InstrumentRuntimeCoordinator::applyTimelineSampleRateToKeyedAndStaging(const double sr) noexcept
{
    if (instrumentStagingController_)
    {
        instrumentStagingController_->setTimelineSampleRate(sr);
    }
    for (auto& kv : instrumentControllersByTrackId_)
    {
        if (kv.second != nullptr)
        {
            kv.second->setTimelineSampleRate(sr);
        }
    }
}

void InstrumentRuntimeCoordinator::alignAllInstrumentClipTemposToProjectTempo() noexcept
{
    if (instrumentStagingController_ != nullptr)
    {
        instrumentStagingController_->alignClipTemposToProjectTempo();
    }
    for (auto& kv : instrumentControllersByTrackId_)
    {
        if (kv.second != nullptr)
        {
            kv.second->alignClipTemposToProjectTempo();
        }
    }
}

void InstrumentRuntimeCoordinator::syncAllKeyedAndStagingShellWithHostState() noexcept
{
    if (instrumentStagingController_ != nullptr)
    {
        instrumentStagingController_->syncShellWithHostState();
    }
    for (auto& kv : instrumentControllersByTrackId_)
    {
        if (kv.second != nullptr)
        {
            kv.second->syncShellWithHostState();
        }
    }
}

void InstrumentRuntimeCoordinator::deactivateAllKeyedAndStagingControllers() noexcept
{
    for (auto& kv : instrumentControllersByTrackId_)
    {
        if (kv.second != nullptr)
        {
            kv.second->setActive(false);
        }
    }
    if (instrumentStagingController_ != nullptr)
    {
        instrumentStagingController_->setActive(false);
    }
}

bool InstrumentRuntimeCoordinator::hasAnyKeyedInstrumentControllerActive() const noexcept
{
    for (const auto& kv : instrumentControllersByTrackId_)
    {
        if (kv.second != nullptr && kv.second->isActive())
        {
            return true;
        }
    }
    return false;
}

void InstrumentRuntimeCoordinator::deactivateKeyedInstrumentControllersOnly() noexcept
{
    for (auto& kv : instrumentControllersByTrackId_)
    {
        if (kv.second != nullptr)
        {
            kv.second->setActive(false);
        }
    }
}

void InstrumentRuntimeCoordinator::setKeyedInstrumentControllersActiveExclusive(const TrackId tid) noexcept
{
    for (auto& kv : instrumentControllersByTrackId_)
    {
        if (kv.second != nullptr)
        {
            kv.second->setActive(kv.first == tid);
        }
    }
    if (instrumentStagingController_ != nullptr)
    {
        instrumentStagingController_->setActive(false);
    }
}

void InstrumentRuntimeCoordinator::applyInstrumentMusicalUndoVectorToAllKeyedAndStaging(
    const std::vector<ProjectFileExperimentalInstrumentTrackV1>& tracks) noexcept
{
    for (auto& kv : instrumentControllersByTrackId_)
    {
        if (kv.second != nullptr)
        {
            kv.second->applyExperimentalInstrumentMusicalUndoBlock(tracks);
        }
    }
    if (instrumentStagingController_ != nullptr)
    {
        instrumentStagingController_->applyExperimentalInstrumentMusicalUndoBlock(tracks);
    }
}

void InstrumentRuntimeCoordinator::updateExperimentalPlaybackBridgeAfterRegistryChange()
{
    reconcileInstrumentRegistryAgainstSessionRows();

    std::vector<ExperimentalInstrumentPlaybackEntry> entries;
    entries.reserve(instrumentControllersByTrackId_.size() + size_t { 2 });

    const auto appendPlaybackRuntimePair = [&](ExperimentalInstrumentHost* host,
                                               InstrumentTrackController* ctl) noexcept
    {
        if (ctl == nullptr || host == nullptr || !ctl->hasInstrumentTrack())
        {
            return;
        }
        if (ctl->isGenericCatalogInstrument() && !host->hasInstrument())
        {
            return;
        }
        const TrackId playbackKey = ctl->getExperimentalInstrumentDomainTrackId();
        if (playbackKey == kInvalidTrackId)
        {
            return;
        }
        for (const auto& e : entries)
        {
            if (e.trackId == playbackKey)
            {
                return;
            }
        }
        entries.push_back(ExperimentalInstrumentPlaybackEntry{ playbackKey, host, ctl });
    };

    for (const auto& kv : instrumentControllersByTrackId_)
    {
        InstrumentTrackController* const ctl = kv.second.get();
        if (ctl == nullptr)
        {
            continue;
        }
        auto itHost = instrumentHostsByTrackId_.find(kv.first);
        if (itHost == instrumentHostsByTrackId_.end() || itHost->second == nullptr)
        {
            continue;
        }
        appendPlaybackRuntimePair(itHost->second.get(), ctl);
    }

    if (instrumentStagingController_ != nullptr && instrumentStagingController_->hasInstrumentTrack()
        && instrumentStagingHost_ != nullptr)
    {
        appendPlaybackRuntimePair(instrumentStagingHost_.get(), instrumentStagingController_.get());
    }

    {
        std::vector<ExperimentalInstrumentPlaybackEntry> reordered;
        reordered.reserve(entries.size());
        std::unordered_map<TrackId, ExperimentalInstrumentPlaybackEntry> leftover;
        for (auto& e : entries)
        {
            leftover.emplace(e.trackId, std::move(e));
        }
        const std::shared_ptr<const SessionSnapshot> ordSnap = session_.loadSessionSnapshotForAudioThread();
        if (ordSnap != nullptr)
        {
            for (int ti = 0; ti < ordSnap->getNumTracks(); ++ti)
            {
                const Track& tr = ordSnap->getTrack(ti);
                if (tr.getKind() != TrackKind::Instrument)
                {
                    continue;
                }
                auto li = leftover.find(tr.getId());
                if (li != leftover.end())
                {
                    reordered.push_back(std::move(li->second));
                    leftover.erase(li);
                }
            }
        }
        for (auto& lr : leftover)
        {
            reordered.push_back(std::move(lr.second));
        }
        entries = std::move(reordered);
    }

    const TrackId canonLaneIdForLog = canonicalInstrumentLaneTrackIdFromSession();

    juce::String routingPlaybackPublishFp = "playback-publish: firstInstTid=";
    routingPlaybackPublishFp
        += juce::String(static_cast<juce::int64>(static_cast<std::int64_t>(canonLaneIdForLog)));
    routingPlaybackPublishFp += juce::String(" entries=");
    routingPlaybackPublishFp += juce::String(static_cast<int>(entries.size()));

    if (!entries.empty())
    {
        routingPlaybackPublishFp += " [";
        for (size_t i = 0; i < entries.size(); ++i)
        {
            const auto& e = entries[i];
            if (i != 0)
            {
                routingPlaybackPublishFp += ", ";
            }
            const InstrumentTrackController* ctlInfo = e.midiController;
            routingPlaybackPublishFp += "{tid=";
            routingPlaybackPublishFp
                += juce::String(static_cast<juce::int64>(static_cast<std::int64_t>(e.trackId)));
            routingPlaybackPublishFp += " host=";
            routingPlaybackPublishFp +=
                ((e.host != nullptr)
                     ? ("0x"
                        + juce::String::toHexString(static_cast<juce::int64>(
                              reinterpret_cast<std::intptr_t>(static_cast<void*>(e.host)))))
                     : juce::String("null"));
            routingPlaybackPublishFp += " ctl=";
            routingPlaybackPublishFp +=
                ((e.midiController != nullptr)
                     ? ("0x"
                        + juce::String::toHexString(static_cast<juce::int64>(reinterpret_cast<std::intptr_t>(
                              static_cast<void*>(e.midiController)))))
                     : juce::String("null"));
            const TrackId dom = (ctlInfo != nullptr) ? ctlInfo->getExperimentalInstrumentDomainTrackId()
                                                     : kInvalidTrackId;
            routingPlaybackPublishFp += " ctlDomain="
                                         + juce::String(static_cast<juce::int64>(static_cast<std::int64_t>(dom)));
            routingPlaybackPublishFp += " ctlHasTrack=";
            routingPlaybackPublishFp += ((ctlInfo != nullptr && ctlInfo->hasInstrumentTrack()) ? "yes" : "no");
            routingPlaybackPublishFp += " ctlPower=";
            routingPlaybackPublishFp += ((ctlInfo != nullptr && ctlInfo->isPowerOn()) ? "on" : "off");
            routingPlaybackPublishFp += " ctlMuted=";
            routingPlaybackPublishFp += ((ctlInfo != nullptr && ctlInfo->isMuted()) ? "yes" : "no");
            routingPlaybackPublishFp += " ctlActive=";
            routingPlaybackPublishFp += ((ctlInfo != nullptr && ctlInfo->isActive()) ? "yes" : "no");
            routingPlaybackPublishFp += " ctlLoaded=";
            routingPlaybackPublishFp += ((ctlInfo != nullptr && ctlInfo->isInstrumentLoaded()) ? "yes" : "no");
            routingPlaybackPublishFp += " hostHasInstrument=";
            routingPlaybackPublishFp += ((e.host != nullptr && e.host->hasInstrument()) ? "yes" : "no");
            routingPlaybackPublishFp += " uiName=\"";
            routingPlaybackPublishFp
                += ((e.host != nullptr) ? e.host->getInstrumentNameForUi().replaceCharacter('\"', '\'')
                                       : juce::String("--"));
            routingPlaybackPublishFp += "\"}";
        }
        routingPlaybackPublishFp += "]";
    }

    if (routingPlaybackPublishFp != lastExperimentalPlaybackRoutingPublishFingerprint_)
    {
        lastExperimentalPlaybackRoutingPublishFingerprint_ = routingPlaybackPublishFp;
#if MINIDAW_DIAG_PLAYBACK_ROUTING
        appendExperimentalPlaybackRoutingLogLine(routingPlaybackPublishFp);
#endif
#if !defined(NDEBUG)
        juce::Logger::writeToLog("[TransportControlsContent] Experimental playback snapshot entries changed "
                                 + routingPlaybackPublishFp);
#endif
    }

    if (entries.empty())
    {
        playbackEngine_.publishExperimentalInstrumentPlaybackSnapshot(nullptr);
        return;
    }

    playbackEngine_.publishExperimentalInstrumentPlaybackSnapshot(
        std::make_shared<const ExperimentalInstrumentPlaybackSnapshot>(
            ExperimentalInstrumentPlaybackSnapshot{ std::move(entries) }));
}
