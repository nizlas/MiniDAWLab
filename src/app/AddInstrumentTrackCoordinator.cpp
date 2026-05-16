#include "app/AddInstrumentTrackCoordinator.h"

#include <JuceHeader.h>

#include <memory>
#include <optional>
#include <thread>

#include "app/InstrumentRuntimeCoordinator.h"
#include "domain/Session.h"
#include "instruments/InstrumentTrackController.h"
#include "plugins/ExperimentalInstrumentHost.h"

namespace
{
    constexpr const char* kGrooveAgentAddFailureMsg
        = "Could not find or load Groove Agent SE. Make sure it is installed, then rescan VST3 plugins or "
          "choose it manually from the VST3 picker.";

    /// Loads another Groove Agent instance using an existing host's description and bundle path only.
    /// Intentionally does **not** restore plugin state so normal Add Track gets a fresh/default kit.
    [[nodiscard]] bool tryCloneGrooveAgentFromAnyExistingInto(InstrumentRuntimeCoordinator& instrumentRuntimeCoordinator,
                                                               ExperimentalInstrumentHost& dest) noexcept
    {
        ExperimentalInstrumentHost* src = instrumentRuntimeCoordinator.findGrooveAgentTemplateHostPreferKeyed(&dest);
        if (src == nullptr || src == &dest)
        {
            return false;
        }
        juce::PluginDescription d{};
        if (!src->getLastLoadedPluginDescription(d))
        {
            return false;
        }
        const juce::File original(src->getLastLoadedVst3OriginalPath());
        if (!original.exists())
        {
            return false;
        }
        juce::String warnIgnored;
        return dest
            .loadInstrumentFromDescription(
                d,
                original,
                "groove-agent-sibling-fresh",
                nullptr,
                &warnIgnored)
            .wasOk();
    }

    [[nodiscard]] bool tryLoadGrooveAgentFromCacheCandidate(ExperimentalInstrumentHost& host,
                                                            const mini_daw::Vst3GrooveCacheLoadCandidate& cand,
                                                            const char* loadTag) noexcept
    {
        if (!cand.valid || cand.descriptions.empty())
        {
            return false;
        }
        juce::String warnIgnored;
        return host
            .loadInstrumentFromDescription(
                cand.descriptions.front(),
                cand.resolvedBundle,
                loadTag,
                nullptr,
                &warnIgnored)
            .wasOk();
    }
} // namespace

AddInstrumentTrackCoordinator::AddInstrumentTrackCoordinator(Refs refs, Callbacks callbacks)
    : refs_(refs)
    , callbacks_(std::move(callbacks))
{
}

void AddInstrumentTrackCoordinator::addGrooveAgentInstrumentTrackFromMenu()
{
    Session& session = refs_.session;
    InstrumentRuntimeCoordinator& instrumentRuntimeCoordinator = refs_.instrumentRuntimeCoordinator;

    if (instrumentRuntimeCoordinator.anyHeldHostShowsGrooveAgentLoaded())
    {
        finishAddGrooveAgentInstrumentTrackAfterInstrumentResolved();
        return;
    }

    const auto prStaging = instrumentRuntimeCoordinator.getExperimentRuntimePairForGrooveAdds();
    ExperimentalInstrumentHost* const mh = prStaging.first;
    InstrumentTrackController* const ctlStaging = prStaging.second;
    if (mh == nullptr || ctlStaging == nullptr)
    {
        juce::AlertWindow::showMessageBoxAsync(
            juce::AlertWindow::WarningIcon, "Instrument track", kGrooveAgentAddFailureMsg);
        return;
    }

    if (mh->hasInstrument() && mh->getInstrumentNameForUi().containsIgnoreCase("Groove Agent"))
    {
        finishAddGrooveAgentInstrumentTrackAfterInstrumentResolved();
        return;
    }

    mini_daw::Vst3GrooveCacheLoadCandidate v2Cand;
    mini_daw::Vst3GrooveCacheLoadCandidate v1Cand;
    juce::String cacheInfo;
    (void)mini_daw::tryLoadGrooveAgentCacheCandidates({}, v2Cand, v1Cand, cacheInfo);
    juce::ignoreUnused(cacheInfo);

    bool loadOk = false;
    if (v2Cand.valid)
    {
        loadOk = tryLoadGrooveAgentFromCacheCandidate(*mh, v2Cand, "add-track-cached-v2");
    }

    if (loadOk)
    {
        finishAddGrooveAgentInstrumentTrackAfterInstrumentResolved();
        return;
    }

    if (!v2Cand.valid)
    {
        beginAsyncGrooveAgentOopScanForAddTrack(std::move(v1Cand));
        return;
    }

    if (v1Cand.valid && tryLoadGrooveAgentFromCacheCandidate(*mh, v1Cand, "add-track-cached-v1"))
    {
        finishAddGrooveAgentInstrumentTrackAfterInstrumentResolved();
        return;
    }

    beginAsyncGrooveAgentOopScanForAddTrack(std::move(v1Cand));
}

void AddInstrumentTrackCoordinator::beginAsyncGrooveAgentOopScanForAddTrack(
    mini_daw::Vst3GrooveCacheLoadCandidate v1Cand)
{
    const juce::File scanTarget = mini_daw::getGrooveAgentSeVst3BundlePathForOopScanFallback();

    AddInstrumentTrackCoordinator* const self = this;

    if (!scanTarget.exists())
    {
        juce::MessageManager::callAsync([self, v1Cand]() mutable {
            if (self == nullptr)
            {
                return;
            }
            auto pr = self->refs_.instrumentRuntimeCoordinator.getExperimentRuntimePairForGrooveAdds();
            ExperimentalInstrumentHost* const mh = pr.first;
            if (mh != nullptr && v1Cand.valid
                && tryLoadGrooveAgentFromCacheCandidate(*mh, v1Cand, "add-track-cached-v1"))
            {
                self->finishAddGrooveAgentInstrumentTrackAfterInstrumentResolved();
                return;
            }
            juce::AlertWindow::showMessageBoxAsync(
                juce::AlertWindow::WarningIcon, "Instrument track", kGrooveAgentAddFailureMsg);
        });
        return;
    }

    std::thread([self, scanTarget, v1Cand]() mutable {
        const mini_daw::Vst3OopScanResult scanResult
            = mini_daw::runVst3OopScanBlocking(scanTarget, mini_daw::kVst3OopScanReplyTimeoutMs);
        juce::MessageManager::callAsync([self, scanResult, scanTarget, v1Cand]() mutable {
            if (self == nullptr)
            {
                return;
            }
            auto pr = self->refs_.instrumentRuntimeCoordinator.getExperimentRuntimePairForGrooveAdds();
            ExperimentalInstrumentHost* const mh = pr.first;
            if (mh == nullptr)
            {
                juce::AlertWindow::showMessageBoxAsync(
                    juce::AlertWindow::WarningIcon, "Instrument track", kGrooveAgentAddFailureMsg);
                return;
            }

            const bool scanOk = scanResult.outcome == mini_daw::Vst3OopScanOutcome::Success
                                && !scanResult.descriptions.empty();
            juce::String warnIgnored;
            if (scanOk
                && mh->loadInstrumentFromDescription(
                       scanResult.descriptions.front(),
                       scanTarget,
                       "add-track-oop-fresh-v2",
                       nullptr,
                       &warnIgnored)
                       .wasOk())
            {
                self->finishAddGrooveAgentInstrumentTrackAfterInstrumentResolved();
                return;
            }

            if (v1Cand.valid && tryLoadGrooveAgentFromCacheCandidate(*mh, v1Cand, "add-track-cached-v1"))
            {
                self->finishAddGrooveAgentInstrumentTrackAfterInstrumentResolved();
                return;
            }

            juce::AlertWindow::showMessageBoxAsync(
                juce::AlertWindow::WarningIcon, "Instrument track", kGrooveAgentAddFailureMsg);
        });
    }).detach();
}

void AddInstrumentTrackCoordinator::finishAddGrooveAgentInstrumentTrackAfterInstrumentResolved()
{
    Session& session = refs_.session;
    InstrumentRuntimeCoordinator& instrumentRuntimeCoordinator = refs_.instrumentRuntimeCoordinator;

    const std::optional<TrackId> newIdOpt = session.appendExperimentalInstrumentShellTrack({});
    if (!newIdOpt.has_value())
    {
        juce::AlertWindow::showMessageBoxAsync(
            juce::AlertWindow::InfoIcon,
            "Instrument track",
            "Could not add instrument track shell.");
        return;
    }

    const TrackId tid = *newIdOpt;
    const bool registryWasEmpty = instrumentRuntimeCoordinator.isKeyedRuntimeRegistryEmpty();
    ExperimentalInstrumentHost* mh = nullptr;
    InstrumentTrackController* ctl = nullptr;

    if (registryWasEmpty && instrumentRuntimeCoordinator.stagingInstrumentHostUnchecked() != nullptr
        && instrumentRuntimeCoordinator.stagingInstrumentControllerUnchecked() != nullptr
        && instrumentRuntimeCoordinator.stagingInstrumentHostUnchecked()->hasInstrument()
        && instrumentRuntimeCoordinator.stagingInstrumentHostUnchecked()->getInstrumentNameForUi().containsIgnoreCase(
               "Groove Agent"))
    {
        mh = instrumentRuntimeCoordinator.stagingInstrumentHostUnchecked();
        ctl = instrumentRuntimeCoordinator.stagingInstrumentControllerUnchecked();
    }
    else
    {
        const auto pr = instrumentRuntimeCoordinator.getOrCreateInstrumentRuntimeForTrack(tid);
        mh = pr.first;
        ctl = pr.second;
    }

    if (mh == nullptr || ctl == nullptr)
    {
        juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::InfoIcon,
                                                "Instrument track",
                                                "Could not allocate Groove Agent instrument runtime.");
        session.removeTrack(tid);
        if (!registryWasEmpty)
        {
            instrumentRuntimeCoordinator.removeInstrumentRuntimeForTrack(tid);
        }
        callbacks_.refreshInstrumentUi();
        return;
    }

    if (!mh->hasInstrument() || !mh->getInstrumentNameForUi().containsIgnoreCase("Groove Agent"))
    {
        if (!tryCloneGrooveAgentFromAnyExistingInto(instrumentRuntimeCoordinator, *mh))
        {
            juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::WarningIcon,
                                                    "Instrument track",
                                                    "Could not load Groove Agent into this instrument track.");
            session.removeTrack(tid);
            if (!registryWasEmpty)
            {
                instrumentRuntimeCoordinator.removeInstrumentRuntimeForTrack(tid);
            }
            callbacks_.refreshInstrumentUi();
            return;
        }
    }

    if (!ctl->bootstrapGrooveAgentShellForSessionTrack(tid))
    {
        juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::InfoIcon,
                                                "Instrument track",
                                                "Could not bind Groove Agent to the new timeline row.");
        session.removeTrack(tid);
        if (!registryWasEmpty)
        {
            instrumentRuntimeCoordinator.removeInstrumentRuntimeForTrack(tid);
        }
        callbacks_.refreshInstrumentUi();
        return;
    }

    if (registryWasEmpty && mh == instrumentRuntimeCoordinator.stagingInstrumentHostUnchecked()
        && ctl == instrumentRuntimeCoordinator.stagingInstrumentControllerUnchecked())
    {
        instrumentRuntimeCoordinator.promoteInstrumentStagingIntoRegistryBoundTo(tid);
    }

    ctl->syncShellWithHostState();
    instrumentRuntimeCoordinator.updateExperimentalPlaybackBridgeAfterRegistryChange();
    callbacks_.syncMidiEditorInstrumentClipTimelineFromDeviceIfOpen();
    callbacks_.refreshInstrumentUi();
    callbacks_.requestLayoutResized();
}
