#include "app/AddInstrumentTrackCoordinator.h"

#include <JuceHeader.h>

#include <memory>
#include <optional>

#include "app/InstrumentRuntimeCoordinator.h"
#include "domain/Session.h"
#include "domain/SessionSnapshot.h"
#include "domain/Track.h"
#include "instruments/InstrumentTrackController.h"
#include "plugins/ExperimentalInstrumentHost.h"

namespace
{
    [[nodiscard]] juce::String proposeNextGrooveAgentInstrumentTrackDisplayName(Session& session)
    {
        int shells = 0;
        const std::shared_ptr<const SessionSnapshot> snap = session.loadSessionSnapshotForAudioThread();
        if (snap != nullptr)
        {
            for (int ti = 0; ti < snap->getNumTracks(); ++ti)
            {
                if (snap->getTrack(ti).getKind() == TrackKind::Instrument)
                {
                    ++shells;
                }
            }
        }
        if (shells == 0)
        {
            return juce::String("Groove Agent");
        }
        return juce::String("Groove Agent ") + juce::String(shells + 1);
    }

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
        juce::MemoryBlock restored;
        {
            const juce::String b64 = src->getCurrentInstrumentStateBase64();
            if (b64.isNotEmpty())
            {
                juce::MemoryOutputStream mos;
                if (juce::Base64::convertFromBase64(mos, b64))
                {
                    restored.append(mos.getData(), mos.getDataSize());
                }
            }
        }
        juce::String warnIgnored;
        const juce::MemoryBlock* mb = restored.getSize() > 0 ? &restored : nullptr;
        return dest
            .loadInstrumentFromDescription(
                d,
                original,
                "groove-agent-sibling-clone",
                mb,
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

    if (!callbacks_.anyHeldGrooveAgentLoaded())
    {
        juce::AlertWindow::showMessageBoxAsync(
            juce::AlertWindow::InfoIcon,
            "Instrument track",
            "Load Groove Agent SE from cached OOP description first.");
        return;
    }
    const juce::String displayName = proposeNextGrooveAgentInstrumentTrackDisplayName(session);
    const std::optional<TrackId> newIdOpt = session.appendExperimentalInstrumentShellTrack(displayName);
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
                                                    "Could not clone Groove Agent into this instrument track.");
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
