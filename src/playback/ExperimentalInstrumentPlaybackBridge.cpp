#include "playback/ExperimentalInstrumentPlaybackBridge.h"

#include <unordered_map>
#include <vector>

#include <juce_core/juce_core.h>

#include "diagnostics/ExperimentalPlaybackRoutingLog.h"
#include "domain/Session.h"
#include "domain/SessionSnapshot.h"
#include "domain/Track.h"
#include "engine/PlaybackEngine.h"
#include "instruments/InstrumentTrackController.h"
#include "plugins/ExperimentalInstrumentHost.h"

namespace mini_daw::experimental_instrument_playback_bridge
{
void reconcileRegistryAgainstSessionRows(
    std::unordered_map<TrackId, std::unique_ptr<ExperimentalInstrumentHost>>& hostsByTrackId,
    std::unordered_map<TrackId, std::unique_ptr<InstrumentTrackController>>& controllersByTrackId) noexcept
{
    if (hostsByTrackId.size() != controllersByTrackId.size())
    {
        return;
    }

    for (;;)
    {
        TrackId staleKey = kInvalidTrackId;
        TrackId domTarget = kInvalidTrackId;
        InstrumentTrackController* ctlProbe = nullptr;
        ExperimentalInstrumentHost* hostProbe = nullptr;

        for (const auto& kv : controllersByTrackId)
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
            auto hi = hostsByTrackId.find(staleKey);
            hostProbe = (hi != hostsByTrackId.end()) ? hi->second.get() : nullptr;
            break;
        }

        juce::ignoreUnused(ctlProbe);
        if (staleKey == kInvalidTrackId || domTarget == kInvalidTrackId || hostProbe == nullptr)
        {
            break;
        }
        auto hostIt = hostsByTrackId.find(staleKey);
        auto ctlIt = controllersByTrackId.find(staleKey);
        if (hostIt == hostsByTrackId.end() || ctlIt == controllersByTrackId.end())
        {
            break;
        }
        if (hostsByTrackId.count(domTarget) != 0 || controllersByTrackId.count(domTarget) != 0)
        {
            juce::Logger::writeToLog(
                "[TransportControlsContent] Instrument re-key aborted: collision at domain TrackId="
                + juce::String((juce::int64)domTarget) + " staleKey="
                + juce::String((juce::int64)staleKey));
            break;
        }

        std::unique_ptr<ExperimentalInstrumentHost> uh = std::move(hostIt->second);
        std::unique_ptr<InstrumentTrackController> uc = std::move(ctlIt->second);
        hostsByTrackId.erase(staleKey);
        controllersByTrackId.erase(staleKey);
        hostsByTrackId[domTarget] = std::move(uh);
        controllersByTrackId[domTarget] = std::move(uc);

        juce::Logger::writeToLog("[TransportControlsContent] Instrument runtime maps re-keyed: map key "
                                 + juce::String((juce::int64)(std::int64_t)staleKey)
                                 + " → controller domain track id "
                                 + juce::String((juce::int64)(std::int64_t)domTarget));
    }
}

void republishAfterRegistryChange(
    std::unordered_map<TrackId, std::unique_ptr<ExperimentalInstrumentHost>>& hostsByTrackId,
    std::unordered_map<TrackId, std::unique_ptr<InstrumentTrackController>>& controllersByTrackId,
    const std::unique_ptr<ExperimentalInstrumentHost>& stagingHost,
    const std::unique_ptr<InstrumentTrackController>& stagingController,
    Session& session,
    PlaybackEngine& playbackEngine,
    juce::String& lastPublishFingerprintInOut)
{
    reconcileRegistryAgainstSessionRows(hostsByTrackId, controllersByTrackId);

    std::vector<ExperimentalInstrumentPlaybackEntry> entries;
    entries.reserve(controllersByTrackId.size() + size_t { 2 });

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

    for (const auto& kv : controllersByTrackId)
    {
        InstrumentTrackController* const ctl = kv.second.get();
        if (ctl == nullptr)
        {
            continue;
        }
        auto itHost = hostsByTrackId.find(kv.first);
        if (itHost == hostsByTrackId.end() || itHost->second == nullptr)
        {
            continue;
        }
        appendPlaybackRuntimePair(itHost->second.get(), ctl);
    }

    if (stagingController != nullptr && stagingController->hasInstrumentTrack() && stagingHost != nullptr)
    {
        appendPlaybackRuntimePair(stagingHost.get(), stagingController.get());
    }

    {
        std::vector<ExperimentalInstrumentPlaybackEntry> reordered;
        reordered.reserve(entries.size());
        std::unordered_map<TrackId, ExperimentalInstrumentPlaybackEntry> leftover;
        for (auto& e : entries)
        {
            leftover.emplace(e.trackId, std::move(e));
        }
        const std::shared_ptr<const SessionSnapshot> ordSnap = session.loadSessionSnapshotForAudioThread();
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

    TrackId canonLaneIdForLog = kInvalidTrackId;
    const std::shared_ptr<const SessionSnapshot> snapCanon = session.loadSessionSnapshotForAudioThread();
    if (snapCanon != nullptr)
    {
        for (int ti = 0; ti < snapCanon->getNumTracks(); ++ti)
        {
            const Track& tr = snapCanon->getTrack(ti);
            if (tr.getKind() == TrackKind::Instrument)
            {
                canonLaneIdForLog = tr.getId();
                break;
            }
        }
    }

    juce::String routingPlaybackPublishFp = "playback-publish: firstInstTid=";
    routingPlaybackPublishFp += juce::String(static_cast<juce::int64>(static_cast<std::int64_t>(canonLaneIdForLog)));
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
            routingPlaybackPublishFp += juce::String(static_cast<juce::int64>(static_cast<std::int64_t>(e.trackId)));
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
            routingPlaybackPublishFp += ((e.host != nullptr)
                                               ? e.host->getInstrumentNameForUi().replaceCharacter('\"', '\'')
                                               : juce::String("--"));
            routingPlaybackPublishFp += "\"}";
        }
        routingPlaybackPublishFp += "]";
    }

    if (routingPlaybackPublishFp != lastPublishFingerprintInOut)
    {
        lastPublishFingerprintInOut = routingPlaybackPublishFp;
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
        playbackEngine.publishExperimentalInstrumentPlaybackSnapshot(nullptr);
        return;
    }

    playbackEngine.publishExperimentalInstrumentPlaybackSnapshot(
        std::make_shared<const ExperimentalInstrumentPlaybackSnapshot>(
            ExperimentalInstrumentPlaybackSnapshot{ std::move(entries) }));
}

void prepareAllHostsForDevice(
    std::unordered_map<TrackId, std::unique_ptr<ExperimentalInstrumentHost>>& hostsByTrackId,
    const std::unique_ptr<ExperimentalInstrumentHost>& stagingHost,
    const double sampleRate,
    const int blockSamples) noexcept
{
    for (auto& kv : hostsByTrackId)
    {
        if (kv.second != nullptr)
        {
            kv.second->prepareForDevice(sampleRate, blockSamples);
        }
    }
    if (stagingHost)
    {
        stagingHost->prepareForDevice(sampleRate, blockSamples);
    }
}

void releaseAllHostsDeviceResources(
    std::unordered_map<TrackId, std::unique_ptr<ExperimentalInstrumentHost>>& hostsByTrackId,
    const std::unique_ptr<ExperimentalInstrumentHost>& stagingHost) noexcept
{
    for (auto& kv : hostsByTrackId)
    {
        if (kv.second != nullptr)
        {
            kv.second->releaseResources();
        }
    }
    if (stagingHost)
    {
        stagingHost->releaseResources();
    }
}

void beginAudioBlockAllHosts(
    std::unordered_map<TrackId, std::unique_ptr<ExperimentalInstrumentHost>>& hostsByTrackId,
    const std::unique_ptr<ExperimentalInstrumentHost>& stagingHost,
    const std::int64_t numSamples) noexcept
{
    for (auto& kv : hostsByTrackId)
    {
        if (kv.second != nullptr)
        {
            kv.second->audioThread_beginAudioBlock((int)numSamples);
        }
    }
    if (stagingHost)
    {
        stagingHost->audioThread_beginAudioBlock((int)numSamples);
    }
}

} // namespace mini_daw::experimental_instrument_playback_bridge
