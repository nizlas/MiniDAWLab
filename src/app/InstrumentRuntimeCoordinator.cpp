#include "app/InstrumentRuntimeCoordinator.h"

#include <map>

#include "diagnostics/ProjectLoadDiagnosticLog.h"
#include "diagnostics/StabilityDiagnosticLog.h"
#include "domain/Session.h"
#include "domain/SessionSnapshot.h"
#include "domain/Track.h"
#include "engine/PlaybackEngine.h"
#include "instruments/InstrumentTrackController.h"
#include "playback/InstrumentPlaybackRegistryPolicy.h"
#include "plugins/ExperimentalInstrumentHost.h"
#include "plugins/InstrumentCatalog.h"
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
    // P2 audition split (steering §17, PID-008): when the PRIMARY host would discard UI/editor
    // MIDI (no loaded instrument), the track's Secondary may audition it. The gate (wired from
    // Main) refuses audition while a proxy supplies transport playback; the Secondary is loaded
    // lazily on the first auditioned message. Message-thread only.
    host.setUiMidiSecondaryForwardResolver([this, ctrlPtr = &ctrl]() -> ExperimentalInstrumentHost* {
        const TrackId tid = ctrlPtr->getExperimentalInstrumentDomainTrackId();
        if (tid == kInvalidTrackId || !ctrlPtr->hasSecondaryInstrument())
        {
            return nullptr;
        }
        if (secondaryAuditionGate_ && !secondaryAuditionGate_(tid))
        {
            return nullptr;
        }
        if (!ensureSecondaryInstrumentLoadedForTrack(tid))
        {
            return nullptr;
        }
        return getSecondaryInstrumentHostForTrack(tid);
    });
    // P2 save-time Secondary state capture: fresh `getStateInformation` from the live Secondary
    // instance when loaded; empty keeps the persisted blob (unloadable Secondary loses nothing).
    ctrl.setSecondaryLiveStateProvider([this, ctrlPtr = &ctrl]() -> juce::String {
        const TrackId tid = ctrlPtr->getExperimentalInstrumentDomainTrackId();
        ExperimentalInstrumentHost* const sh = getSecondaryInstrumentHostForTrack(tid);
        return (sh != nullptr && sh->hasInstrument()) ? sh->getCurrentInstrumentStateBase64()
                                                      : juce::String();
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

InstrumentTrackController*
    InstrumentRuntimeCoordinator::getMidiContentControllerForTrack(const TrackId tid) const noexcept
{
    auto it = midiContentControllersByTrackId_.find(tid);
    return (it == midiContentControllersByTrackId_.end()) ? nullptr : it->second.get();
}

InstrumentTrackController*
    InstrumentRuntimeCoordinator::getOrCreateMidiContentControllerForTrack(const TrackId tid)
{
    if (tid == kInvalidTrackId)
    {
        return nullptr;
    }
    if (InstrumentTrackController* const existing = getMidiContentControllerForTrack(tid))
    {
        return existing;
    }
    auto ctl = std::make_unique<InstrumentTrackController>(nullptr);
    ctl->setSession(&session_);
    if (lastPreparedDeviceSampleRate_ > 0.0)
    {
        ctl->setTimelineSampleRate(lastPreparedDeviceSampleRate_);
    }
    InstrumentTrackController* const ctlPtr = ctl.get();
    if (!ctlPtr->bootstrapMidiContentShellForSessionTrack(tid))
    {
        // Session row missing or not TrackKind::Midi — never key a controller to an unknown row.
        return nullptr;
    }
    midiContentControllersByTrackId_.emplace(tid, std::move(ctl));
    updateExperimentalPlaybackBridgeAfterRegistryChange();
    runSyncInstrumentTimelineRowAttachmentCallback();
    return ctlPtr;
}

void InstrumentRuntimeCoordinator::removeMidiContentControllerForTrack(const TrackId tid) noexcept
{
    const auto it = midiContentControllersByTrackId_.find(tid);
    if (it == midiContentControllersByTrackId_.end())
    {
        return;
    }
    // Same publish-before-destroy protocol as instrument runtimes: republish the bridge without
    // this source, drain the in-flight callback (its snapshot may hold the raw pointer), destroy.
    std::unique_ptr<InstrumentTrackController> retired = std::move(it->second);
    midiContentControllersByTrackId_.erase(it);
    updateExperimentalPlaybackBridgeAfterRegistryChange();
    double waitedMs = 0.0;
    const bool drained = playbackEngine_.waitForAudioCallbackExit(250.0, &waitedMs);
    appendTrackDeleteDiagnosticLine(
        "midi content controller retire trackId=" + juce::String((juce::int64)tid)
        + ": bridge republished; callback drain waitedMs=" + juce::String(waitedMs, 2)
        + " timeout=" + (drained ? "no" : "YES (proceeding anyway)"));
    retired.reset();
    runSyncInstrumentTimelineRowAttachmentCallback();
}

InstrumentTrackController*
    InstrumentRuntimeCoordinator::getMidiClipControllerForTrack(const TrackId tid) const noexcept
{
    if (InstrumentTrackController* const inst = getInstrumentControllerForTrack(tid))
    {
        return inst;
    }
    return getMidiContentControllerForTrack(tid);
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
    for (auto& kv : midiContentControllersByTrackId_)
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
    auto ctl = std::make_unique<InstrumentTrackController>(host.get());
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
        instrumentStagingController_
            = std::make_unique<InstrumentTrackController>(instrumentStagingHost_.get());
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
    // P2: the track's Secondary runtime shares the destination's lifetime.
    removeSecondaryRuntimeForTrack(tid);
    runSyncInstrumentTimelineRowAttachmentCallback();
}

// --------------------------------------------------------------------------- P2 Secondary runtime

namespace
{
    [[nodiscard]] juce::String
        secondaryDescriptorIdentityKey(const ProjectFileGenericVst3DescriptorV1& d)
    {
        return d.fileOrIdentifier + "|" + juce::String(d.uniqueId) + "|" + d.name;
    }
} // namespace

ExperimentalInstrumentHost*
    InstrumentRuntimeCoordinator::getSecondaryInstrumentHostForTrack(const TrackId tid) const noexcept
{
    const auto it = secondaryInstrumentHostsByTrackId_.find(tid);
    return it != secondaryInstrumentHostsByTrackId_.end() ? it->second.get() : nullptr;
}

bool InstrumentRuntimeCoordinator::ensureSecondaryInstrumentLoadedForTrack(const TrackId tid)
{
    if (tid == kInvalidTrackId)
    {
        return false;
    }
    InstrumentTrackController* const ctl = getInstrumentControllerForTrack(tid);
    if (ctl == nullptr || !ctl->hasSecondaryInstrument())
    {
        return false;
    }
    const juce::String identityKey = secondaryDescriptorIdentityKey(ctl->getSecondaryDescriptor());

    // Already loaded with the SAME identity: refresh the channel mapping and succeed.
    if (ExperimentalInstrumentHost* const existing = getSecondaryInstrumentHostForTrack(tid);
        existing != nullptr && existing->hasInstrument())
    {
        const auto itId = secondaryLoadedIdentityByTrackId_.find(tid);
        if (itId != secondaryLoadedIdentityByTrackId_.end() && itId->second == identityKey)
        {
            existing->setForcedMidiChannelForDelivery(ctl->getSecondaryForcedMidiChannel());
            return true;
        }
    }

    // No retry storms: the same failed identity is attempted again only after reconfiguration
    // (noteSecondaryConfigurationChanged clears the latch).
    if (const auto itFail = secondaryLoadFailureLatchByTrackId_.find(tid);
        itFail != secondaryLoadFailureLatchByTrackId_.end() && itFail->second == identityKey)
    {
        return false;
    }

    const mini_daw::GenericVst3ProjectLoadResolution res
        = mini_daw::tryResolveGenericVst3ForProjectLoad(true,
                                                        ctl->getSecondaryDescriptor(),
                                                        ctl->getSecondaryPluginBundlePath(),
                                                        ctl->getSecondaryDescriptor().name);
    if (!res.resolved)
    {
        secondaryLoadFailureLatchByTrackId_[tid] = identityKey;
        secondaryLoadFailureReasonByTrackId_[tid]
            = "The plug-in could not be found (path/descriptor did not resolve). Bundle: "
              + (ctl->getSecondaryPluginBundlePath().isNotEmpty() ? ctl->getSecondaryPluginBundlePath()
                                                                  : juce::String("(none)"));
        return false;
    }

    auto& slot = secondaryInstrumentHostsByTrackId_[tid];
    if (slot == nullptr)
    {
        slot = std::make_unique<ExperimentalInstrumentHost>();
        // Deliberately NOT wired via wireExperimentalInstrumentHost: the Secondary never feeds
        // the Primary drum-name/template machinery and never bumps Primary semantics.
        if (lastPreparedDeviceSampleRate_ > 0.0 && lastPreparedDeviceBlockSize_ > 0)
        {
            slot->prepareForDevice(lastPreparedDeviceSampleRate_, lastPreparedDeviceBlockSize_);
        }
    }

    juce::MemoryBlock stateBlock;
    bool haveState = false;
    if (const juce::String stateB64 = ctl->getSecondaryPluginStateBase64(); stateB64.isNotEmpty())
    {
        juce::MemoryOutputStream mos;
        if (juce::Base64::convertFromBase64(mos, stateB64) && mos.getDataSize() > 0)
        {
            stateBlock.replaceAll(mos.getData(), mos.getDataSize());
            haveState = true;
        }
    }

    juce::String stateWarning;
    // HALion-family loads need the same description/path repair as every other HALion load
    // site (the host applies `repairHalionPluginDescriptionForLoad` for "halion"-tagged loads).
    const char* const sourceTag
        = mini_daw::instrumentDisplayNameLooksLikeHalionSonic(res.description.name)
              ? "secondary-halion"
              : "secondary";
    juce::Result r = slot->loadInstrumentFromDescription(
        res.description, res.bundle, sourceTag, haveState ? &stateBlock : nullptr, &stateWarning);
    if (r.failed() && haveState)
    {
        // A poisoned/incompatible saved state must never leave the Secondary permanently
        // unloadable — degrade to a clean load (default patch) with a visible notice instead.
        r = slot->loadInstrumentFromDescription(res.description, res.bundle, sourceTag, nullptr, nullptr);
        if (!r.failed())
        {
            stateWarning = "The saved plug-in state could not be applied (loaded with the default "
                           "state); reselect the sound in the plug-in editor if needed.";
        }
    }
    if (r.failed())
    {
        secondaryLoadFailureLatchByTrackId_[tid] = identityKey;
        secondaryLoadFailureReasonByTrackId_[tid] = r.getErrorMessage().isNotEmpty()
                                                        ? r.getErrorMessage()
                                                        : juce::String("The plug-in failed to load.");
        secondaryLoadedIdentityByTrackId_.erase(tid);
        return false;
    }
    secondaryLoadFailureLatchByTrackId_.erase(tid);
    secondaryLoadFailureReasonByTrackId_.erase(tid);
    secondaryLoadedIdentityByTrackId_[tid] = identityKey;
    slot->setForcedMidiChannelForDelivery(ctl->getSecondaryForcedMidiChannel());
    ctl->noteSecondaryResolvedBundlePath(res.bundle.getFullPathName());
    // Republish so the freshly loaded instance becomes visible to the audio thread (as the
    // AUDITION host until/unless the source decision activates it for transport).
    updateExperimentalPlaybackBridgeAfterRegistryChange();
    return true;
}

juce::String InstrumentRuntimeCoordinator::getSecondaryLoadFailureReasonForTrack(const TrackId tid) const
{
    const auto it = secondaryLoadFailureReasonByTrackId_.find(tid);
    return it != secondaryLoadFailureReasonByTrackId_.end() ? it->second : juce::String{};
}

bool InstrumentRuntimeCoordinator::retrySecondaryInstrumentLoadForTrack(const TrackId tid)
{
    // Explicit user action (Editor click / Retry): never blocked by the automatic-retry latch.
    // The latch only guards AUTOMATIC paths (transport decisions, audition forwards) against
    // retry storms; a deliberate click is always allowed one fresh attempt.
    secondaryLoadFailureLatchByTrackId_.erase(tid);
    secondaryLoadFailureReasonByTrackId_.erase(tid);
    return ensureSecondaryInstrumentLoadedForTrack(tid);
}

void InstrumentRuntimeCoordinator::setSecondaryTransportActive(const TrackId tid, const bool active)
{
    const bool current = secondaryTransportActive_.count(tid) != 0;
    if (current == active)
    {
        return;
    }
    if (active)
    {
        // Never activate an unloaded Secondary — the entry swap below requires a live instance.
        if (!ensureSecondaryInstrumentLoadedForTrack(tid))
        {
            return;
        }
        secondaryTransportActive_.insert(tid);
        // Reset any held state from a previous activation BEFORE new content arrives (the
        // queued events are delivered in the same block, ahead of transport MIDI).
        if (ExperimentalInstrumentHost* const sh = getSecondaryInstrumentHostForTrack(tid))
        {
            sh->enqueueAllNotesOffFromMessageThread();
        }
    }
    else
    {
        secondaryTransportActive_.erase(tid);
        // The host leaves the snapshot at the next block boundary (silent immediately); queue a
        // reset so held notes never replay when it is processed again later.
        if (ExperimentalInstrumentHost* const sh = getSecondaryInstrumentHostForTrack(tid))
        {
            sh->enqueueAllNotesOffFromMessageThread();
        }
    }
    // P2 CC chase on host swap: the destination's own controller AND every Midi source routed to
    // it must treat the next transport segment as a discontinuity, so the newly active host gets
    // the chased CC state (delivery memory belongs to the previous host).
    if (InstrumentTrackController* const destCtl = getInstrumentControllerForTrack(tid))
    {
        destCtl->noteTransportHostSwappedForChase();
    }
    if (const auto snap = session_.loadSessionSnapshotForAudioThread())
    {
        for (int ti = 0; ti < snap->getNumTracks(); ++ti)
        {
            const Track& tr = snap->getTrack(ti);
            if (tr.getKind() != TrackKind::Midi || tr.getMidiDestinationTrackId() != tid)
            {
                continue;
            }
            const auto it = midiContentControllersByTrackId_.find(tr.getId());
            if (it != midiContentControllersByTrackId_.end() && it->second != nullptr)
            {
                it->second->noteTransportHostSwappedForChase();
            }
        }
    }
    // Atomic snapshot republish: the source switch takes effect at the audio-block boundary.
    updateExperimentalPlaybackBridgeAfterRegistryChange();
}

void InstrumentRuntimeCoordinator::removeSecondaryRuntimeForTrack(const TrackId tid) noexcept
{
    std::unique_ptr<ExperimentalInstrumentHost> retired;
    if (const auto it = secondaryInstrumentHostsByTrackId_.find(tid);
        it != secondaryInstrumentHostsByTrackId_.end())
    {
        retired = std::move(it->second);
        secondaryInstrumentHostsByTrackId_.erase(it);
    }
    secondaryLoadedIdentityByTrackId_.erase(tid);
    secondaryLoadFailureLatchByTrackId_.erase(tid);
    secondaryLoadFailureReasonByTrackId_.erase(tid);
    const bool wasActive = secondaryTransportActive_.erase(tid) != 0;
    if (retired == nullptr)
    {
        return;
    }
    if (wasActive)
    {
        // Publish-before-destroy (same F4 pattern as removeInstrumentRuntimeForTrack): retire
        // the host out of the snapshot, drain the in-flight callback, then unload/destroy.
        updateExperimentalPlaybackBridgeAfterRegistryChange();
    }
    double waitedMs = 0.0;
    (void)playbackEngine_.waitForAudioCallbackExit(250.0, &waitedMs);
    retired->closeNativeEditor();
    retired->unloadInstrument();
    retired.reset();
}

void InstrumentRuntimeCoordinator::noteSecondaryConfigurationChanged(const TrackId tid)
{
    InstrumentTrackController* const ctl = getInstrumentControllerForTrack(tid);
    secondaryLoadFailureLatchByTrackId_.erase(tid);
    secondaryLoadFailureReasonByTrackId_.erase(tid);
    if (ctl == nullptr || !ctl->hasSecondaryInstrument())
    {
        // Secondary removed: drop the runtime instance; persisted Primary/proxy state untouched.
        removeSecondaryRuntimeForTrack(tid);
        return;
    }
    if (ExperimentalInstrumentHost* const sh = getSecondaryInstrumentHostForTrack(tid);
        sh != nullptr && sh->hasInstrument())
    {
        const auto itId = secondaryLoadedIdentityByTrackId_.find(tid);
        const bool identityChanged
            = itId == secondaryLoadedIdentityByTrackId_.end()
              || itId->second != secondaryDescriptorIdentityKey(ctl->getSecondaryDescriptor());
        if (identityChanged)
        {
            // Replacement: retire the old instance safely; the next ensure call (playback need,
            // audition, or explicit editor open) loads the new identity.
            removeSecondaryRuntimeForTrack(tid);
        }
        else
        {
            sh->setForcedMidiChannelForDelivery(ctl->getSecondaryForcedMidiChannel());
        }
    }
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
    for (auto& kv : secondaryInstrumentHostsByTrackId_)
    {
        detachAndUnloadHost(kv.second.get());
    }
    detachAndUnloadHost(instrumentStagingHost_.get());

    instrumentStagingController_.reset();
    instrumentStagingHost_.reset();
    instrumentControllersByTrackId_.clear();
    instrumentHostsByTrackId_.clear();
    secondaryInstrumentHostsByTrackId_.clear();
    secondaryLoadedIdentityByTrackId_.clear();
    secondaryLoadFailureLatchByTrackId_.clear();
    secondaryLoadFailureReasonByTrackId_.clear();
    secondaryTransportActive_.clear();
    midiContentControllersByTrackId_.clear();
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
    for (auto& kv : secondaryInstrumentHostsByTrackId_)
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
        // TLD-1: a fresh session (no project loaded/saved yet) adopts the first prepared device
        // rate as its timeline reference; a later device-rate change never re-stamps it, and
        // loading a project overwrites it with the file's authoritative reference (§10.1).
        session_.initializeTimelineSampleRateIfUnset(sampleRate);
    }

    for (auto& kv : instrumentHostsByTrackId_)
    {
        if (kv.second != nullptr)
        {
            kv.second->prepareForDevice(sampleRate, blockSamples);
        }
    }
    for (auto& kv : secondaryInstrumentHostsByTrackId_)
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
    for (auto& kv : secondaryInstrumentHostsByTrackId_)
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
    for (auto& kv : midiContentControllersByTrackId_)
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
    for (auto& kv : midiContentControllersByTrackId_)
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
    // Plugin-less MIDI content rows carry the SAME UI-active header flag as instrument rows:
    // omitting their map here latched their slate-blue selection paint forever once clicked.
    for (auto& kv : instrumentControllersByTrackId_)
    {
        if (kv.second != nullptr)
        {
            kv.second->setActive(false);
        }
    }
    for (auto& kv : midiContentControllersByTrackId_)
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
    for (const auto& kv : midiContentControllersByTrackId_)
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
    for (auto& kv : midiContentControllersByTrackId_)
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
    for (auto& kv : midiContentControllersByTrackId_)
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
    for (auto& kv : midiContentControllersByTrackId_)
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
                                               InstrumentTrackController* ctl,
                                               ExperimentalInstrumentHost* auditionHost = nullptr) noexcept
    {
        if (ctl == nullptr || host == nullptr)
        {
            return;
        }
        const TrackId playbackKey = ctl->getExperimentalInstrumentDomainTrackId();
        // P1 missing-Primary: registration must NOT depend on a loaded plugin — the
        // published proxy playback view can only sound through a registered entry
        // (see InstrumentPlaybackRegistryPolicy.h).
        if (!instrument_playback::playbackEntryEligible(ctl->hasInstrumentTrack(),
                                                        ctl->isGenericCatalogInstrument(),
                                                        host->hasInstrument(),
                                                        playbackKey))
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
        entries.push_back(
            ExperimentalInstrumentPlaybackEntry{ playbackKey, host, ctl, auditionHost });
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
        // P2 (steering §17): ONE entry per track — the transport host is the Secondary exactly
        // when the published source decision selected it; otherwise the Primary host (whose
        // published proxy view supplies Proxy playback). A loaded, non-transport Secondary rides
        // along as the AUDITION host (processed only while the transport is stopped) so stopped
        // audition works even when the proxy is Current. Primary/Proxy/Secondary can therefore
        // never feed the transport simultaneously.
        ExperimentalInstrumentHost* transportHost = itHost->second.get();
        ExperimentalInstrumentHost* auditionHost = nullptr;
        if (ExperimentalInstrumentHost* const secondaryHost
            = getSecondaryInstrumentHostForTrack(kv.first);
            secondaryHost != nullptr && secondaryHost->hasInstrument())
        {
            if (secondaryTransportActive_.count(kv.first) != 0)
            {
                transportHost = secondaryHost;
            }
            else if (!transportHost->hasInstrument())
            {
                auditionHost = secondaryHost;
            }
        }
        appendPlaybackRuntimePair(transportHost, ctl, auditionHost);
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

    // TrackKind::Midi sources, in session track order: this ordering *is* the deterministic
    // many-to-one merge policy (destination's own events first, then sources top-to-bottom).
    std::vector<ExperimentalMidiSourcePlaybackEntry> midiSources;
    {
        const std::shared_ptr<const SessionSnapshot> ordSnap = session_.loadSessionSnapshotForAudioThread();
        if (ordSnap != nullptr)
        {
            for (int ti = 0; ti < ordSnap->getNumTracks(); ++ti)
            {
                const Track& tr = ordSnap->getTrack(ti);
                if (tr.getKind() != TrackKind::Midi)
                {
                    continue;
                }
                auto it = midiContentControllersByTrackId_.find(tr.getId());
                if (it != midiContentControllersByTrackId_.end() && it->second != nullptr)
                {
                    midiSources.push_back(
                        ExperimentalMidiSourcePlaybackEntry{ tr.getId(), it->second.get() });
                }
            }
        }
    }

    const TrackId canonLaneIdForLog = canonicalInstrumentLaneTrackIdFromSession();

    juce::String routingPlaybackPublishFp = "playback-publish: firstInstTid=";
    routingPlaybackPublishFp
        += juce::String(static_cast<juce::int64>(static_cast<std::int64_t>(canonLaneIdForLog)));
    routingPlaybackPublishFp += juce::String(" entries=");
    routingPlaybackPublishFp += juce::String(static_cast<int>(entries.size()));
    routingPlaybackPublishFp += juce::String(" midiSources=");
    routingPlaybackPublishFp += juce::String(static_cast<int>(midiSources.size()));
    for (const auto& ms : midiSources)
    {
        routingPlaybackPublishFp
            += " {midiTid=" + juce::String(static_cast<juce::int64>(static_cast<std::int64_t>(ms.trackId)))
               + "}";
    }

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
        // No instrument destinations exist, so MIDI sources have nowhere to deliver; publishing
        // null keeps the audio path on its cheap early-out.
        playbackEngine_.publishExperimentalInstrumentPlaybackSnapshot(nullptr);
        return;
    }

    playbackEngine_.publishExperimentalInstrumentPlaybackSnapshot(
        std::make_shared<const ExperimentalInstrumentPlaybackSnapshot>(
            ExperimentalInstrumentPlaybackSnapshot{ std::move(entries), std::move(midiSources) }));
}
