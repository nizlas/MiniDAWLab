#include "app/AddInstrumentTrackCoordinator.h"

#include <JuceHeader.h>

#include <memory>
#include <optional>
#include <thread>

#include "app/InstrumentRuntimeCoordinator.h"
#include "diagnostics/StabilityDiagnosticLog.h"
#include "domain/Session.h"
#include "instruments/InstrumentTrackController.h"
#include "plugins/ExperimentalInstrumentHost.h"
#include "plugins/InstrumentCatalog.h"
#include "plugins/InstrumentCatalog.h"
#include "plugins/Vst3ChildProcessScan.h"

namespace
{
    [[nodiscard]] const char* oopScanOutcomeLabel(const mini_daw::Vst3OopScanOutcome o) noexcept
    {
        using O = mini_daw::Vst3OopScanOutcome;
        switch (o)
        {
        case O::Success:
            return "Success";
        case O::ChildCrashedOrFailed:
            return "ChildCrashedOrFailed";
        case O::Timeout:
            return "Timeout";
        case O::LaunchFailed:
            return "LaunchFailed";
        case O::ParseFailed:
            return "ParseFailed";
        default:
            return "Unknown";
        }
    }

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

    constexpr const char* kHalionSonicAddFailureMsg
        = "Could not find or load a HALion Sonic-family VST3. Install the plug-in, then try again. "
          "Diagnostics: %APPDATA%\\MiniDAWLab\\experimental-vst3-oop-scan.log (search for \"halion\"). "
          "Bundle folder names are matched when they contain both \"HALion\" and \"Sonic\" (any edition).";

    [[nodiscard]] bool tryCloneHalionSonicFromAnyExistingInto(InstrumentRuntimeCoordinator& instrumentRuntimeCoordinator,
                                                               ExperimentalInstrumentHost& dest) noexcept
    {
        ExperimentalInstrumentHost* src = instrumentRuntimeCoordinator.findHalionSonicTemplateHostPreferKeyed(&dest);
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
                "halion-sibling-fresh",
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
    const mini_daw::AsyncCallbackGuard aliveGuard = asyncLifetime_.guard();

    if (!scanTarget.exists())
    {
        juce::MessageManager::callAsync([self, aliveGuard, v1Cand]() mutable {
            if (!aliveGuard.isAlive())
            {
                juce::Logger::writeToLog("[stale-async] skipped: GrooveAgent add-track cached fallback");
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

    std::thread([self, aliveGuard, scanTarget, v1Cand]() mutable {
        const mini_daw::Vst3OopScanResult scanResult
            = mini_daw::runVst3OopScanBlocking(scanTarget, mini_daw::kVst3OopScanReplyTimeoutMs);
        juce::MessageManager::callAsync([self, aliveGuard, scanResult, scanTarget, v1Cand]() mutable {
            if (!aliveGuard.isAlive())
            {
                juce::Logger::writeToLog("[stale-async] skipped: GrooveAgent add-track OOP scan completion");
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
    // Groove Agent is a drum instrument and maps its kit to the General MIDI drum channel, so this
    // one creation path opts into channel 10 while every other new track keeps the melodic default
    // of 1. This is a property of the menu entry the user picked, never inferred from a track or
    // plugin name. Set before the shell bootstraps so the first render snapshot already carries it.
    (void)session.setTrackMidiOutputChannel(tid, kTrackMidiOutputChannelDrums);
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

void AddInstrumentTrackCoordinator::addHalionSonicInstrumentTrackFromMenu()
{
    InstrumentRuntimeCoordinator& instrumentRuntimeCoordinator = refs_.instrumentRuntimeCoordinator;

    if (instrumentRuntimeCoordinator.anyHeldHostShowsHalionSonicLoaded())
    {
        finishAddHalionSonicInstrumentTrackAfterInstrumentResolved();
        return;
    }

    const auto prStaging = instrumentRuntimeCoordinator.getExperimentRuntimePairForGrooveAdds();
    ExperimentalInstrumentHost* const mh = prStaging.first;
    InstrumentTrackController* const ctlStaging = prStaging.second;
    if (mh == nullptr || ctlStaging == nullptr)
    {
        juce::AlertWindow::showMessageBoxAsync(
            juce::AlertWindow::WarningIcon, "Instrument track", kHalionSonicAddFailureMsg);
        return;
    }

    if (mh->hasInstrument() && mini_daw::instrumentDisplayNameLooksLikeHalionSonic(mh->getInstrumentNameForUi()))
    {
        finishAddHalionSonicInstrumentTrackAfterInstrumentResolved();
        return;
    }

    mini_daw::Vst3GrooveCacheLoadCandidate v2Cand;
    mini_daw::Vst3GrooveCacheLoadCandidate v1Cand;
    juce::String cacheInfo;
    (void)mini_daw::tryLoadHalionSonicCacheCandidates({}, v2Cand, v1Cand, cacheInfo);
    juce::ignoreUnused(cacheInfo);

    bool loadOk = false;
    if (v2Cand.valid)
    {
        loadOk = tryLoadGrooveAgentFromCacheCandidate(*mh, v2Cand, "add-track-halion-cached-v2");
    }

    if (loadOk)
    {
        finishAddHalionSonicInstrumentTrackAfterInstrumentResolved();
        return;
    }

    if (!v2Cand.valid)
    {
        beginAsyncHalionSonicOopScanForAddTrack(std::move(v1Cand));
        return;
    }

    if (v1Cand.valid && tryLoadGrooveAgentFromCacheCandidate(*mh, v1Cand, "add-track-halion-cached-v1"))
    {
        finishAddHalionSonicInstrumentTrackAfterInstrumentResolved();
        return;
    }

    beginAsyncHalionSonicOopScanForAddTrack(std::move(v1Cand));
}

void AddInstrumentTrackCoordinator::beginAsyncHalionSonicOopScanForAddTrack(
    mini_daw::Vst3GrooveCacheLoadCandidate v1Cand)
{
    const juce::File scanTarget = mini_daw::getHalionSonicVst3BundlePathForOopScanFallback();

    AddInstrumentTrackCoordinator* const self = this;
    const mini_daw::AsyncCallbackGuard aliveGuard = asyncLifetime_.guard();

    if (!scanTarget.exists())
    {
        juce::MessageManager::callAsync([self, aliveGuard, v1Cand]() mutable {
            if (!aliveGuard.isAlive())
            {
                juce::Logger::writeToLog("[stale-async] skipped: HALion add-track cached fallback");
                return;
            }
            auto pr = self->refs_.instrumentRuntimeCoordinator.getExperimentRuntimePairForGrooveAdds();
            ExperimentalInstrumentHost* const mh = pr.first;
            if (mh != nullptr && v1Cand.valid
                && tryLoadGrooveAgentFromCacheCandidate(*mh, v1Cand, "add-track-halion-cached-v1"))
            {
                self->finishAddHalionSonicInstrumentTrackAfterInstrumentResolved();
                return;
            }
            juce::AlertWindow::showMessageBoxAsync(
                juce::AlertWindow::WarningIcon, "Instrument track", kHalionSonicAddFailureMsg);
        });
        return;
    }

    std::thread([self, aliveGuard, scanTarget, v1Cand]() mutable {
        const mini_daw::Vst3OopScanResult scanResult
            = mini_daw::runVst3OopScanBlocking(scanTarget, mini_daw::kVst3OopScanReplyTimeoutMs);
        juce::MessageManager::callAsync([self, aliveGuard, scanResult, scanTarget, v1Cand]() mutable {
            if (!aliveGuard.isAlive())
            {
                juce::Logger::writeToLog("[stale-async] skipped: HALion add-track OOP scan completion");
                return;
            }
            auto pr = self->refs_.instrumentRuntimeCoordinator.getExperimentRuntimePairForGrooveAdds();
            ExperimentalInstrumentHost* const mh = pr.first;
            if (mh == nullptr)
            {
                juce::AlertWindow::showMessageBoxAsync(
                    juce::AlertWindow::WarningIcon, "Instrument track", kHalionSonicAddFailureMsg);
                return;
            }

            const bool scanOk = scanResult.outcome == mini_daw::Vst3OopScanOutcome::Success
                                && !scanResult.descriptions.empty();
            if (!scanOk)
            {
                mini_daw::writeVst3OopScanDiagnosticLogLine(
                    "halion add-track OOP: scan not usable outcome=" + juce::String(oopScanOutcomeLabel(scanResult.outcome))
                    + " descriptionCount=" + juce::String(scanResult.descriptionCount) + " scanTarget=\""
                    + scanTarget.getFullPathName() + "\"");
            }
            juce::String warnIgnored;
            if (scanOk)
            {
                const juce::Result loadRes = mh->loadInstrumentFromDescription(
                    scanResult.descriptions.front(),
                    scanTarget,
                    "add-track-halion-oop-fresh-v2",
                    nullptr,
                    &warnIgnored);
                if (loadRes.wasOk())
                {
                    mini_daw::writeVst3OopScanDiagnosticLogLine("halion add-track OOP: load OK scanTarget=\""
                                                                + scanTarget.getFullPathName() + "\"");
                    self->finishAddHalionSonicInstrumentTrackAfterInstrumentResolved();
                    return;
                }
                mini_daw::writeVst3OopScanDiagnosticLogLine(
                    "halion add-track OOP: scan OK but load failed message=\"" + loadRes.getErrorMessage()
                    + "\" scanTarget=\"" + scanTarget.getFullPathName() + "\"");
            }
            if (v1Cand.valid && tryLoadGrooveAgentFromCacheCandidate(*mh, v1Cand, "add-track-halion-cached-v1"))
            {
                self->finishAddHalionSonicInstrumentTrackAfterInstrumentResolved();
                return;
            }

            juce::AlertWindow::showMessageBoxAsync(
                juce::AlertWindow::WarningIcon, "Instrument track", kHalionSonicAddFailureMsg);
        });
    }).detach();
}

void AddInstrumentTrackCoordinator::finishAddHalionSonicInstrumentTrackAfterInstrumentResolved()
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
        && mini_daw::instrumentDisplayNameLooksLikeHalionSonic(
               instrumentRuntimeCoordinator.stagingInstrumentHostUnchecked()->getInstrumentNameForUi()))
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
                                                "Could not allocate HALion Sonic instrument runtime.");
        session.removeTrack(tid);
        if (!registryWasEmpty)
        {
            instrumentRuntimeCoordinator.removeInstrumentRuntimeForTrack(tid);
        }
        callbacks_.refreshInstrumentUi();
        return;
    }

    if (!mh->hasInstrument() || !mini_daw::instrumentDisplayNameLooksLikeHalionSonic(mh->getInstrumentNameForUi()))
    {
        if (!tryCloneHalionSonicFromAnyExistingInto(instrumentRuntimeCoordinator, *mh))
        {
            juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::WarningIcon,
                                                    "Instrument track",
                                                    "Could not load HALion Sonic into this instrument track.");
            session.removeTrack(tid);
            if (!registryWasEmpty)
            {
                instrumentRuntimeCoordinator.removeInstrumentRuntimeForTrack(tid);
            }
            callbacks_.refreshInstrumentUi();
            return;
        }
    }

    if (!ctl->bootstrapHalionSonicShellForSessionTrack(tid))
    {
        juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::InfoIcon,
                                                "Instrument track",
                                                "Could not bind HALion Sonic to the new timeline row.");
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

void AddInstrumentTrackCoordinator::rescanInstrumentPluginsFromMenu()
{
    bool expectedBusy = false;
    if (!instrumentCatalogRescanBusy_.compare_exchange_strong(expectedBusy, true))
    {
        mini_daw::writeInstrumentCatalogRescanLogLine("rescan ignored (already in progress)");
        juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::InfoIcon,
                                               "Instrument plugins",
                                               "An instrument plugin rescan is already running.");
        return;
    }

    mini_daw::writeInstrumentCatalogRescanLogLine("rescan requested from Add Instrument Track menu");
    writeLastOperationBreadcrumb("plugin rescan start");

    AddInstrumentTrackCoordinator* const self = this;
    const mini_daw::AsyncCallbackGuard aliveGuard = asyncLifetime_.guard();
    std::thread([self, aliveGuard] {
        const mini_daw::InstrumentCatalogRescanSummary summary = mini_daw::rescanInstrumentCatalogBlocking();
        juce::MessageManager::callAsync([self, aliveGuard, summary] {
            if (!aliveGuard.isAlive())
            {
                juce::Logger::writeToLog("[stale-async] skipped: instrument plugin rescan completion");
                return;
            }
            self->instrumentCatalogRescanBusy_.store(false);
            writeLastOperationBreadcrumb("plugin rescan end");

            juce::String msg = "Instrument plugin rescan complete.\n\n";
            msg << "Accepted instruments: " << juce::String(summary.acceptedInstrumentCount) << "\n";
            msg << "Rejected (effects): " << juce::String(summary.rejectedEffectCount) << "\n";
            msg << "Validation failures: " << juce::String(summary.rejectedValidationFailedCount) << "\n";
            msg << "Duplicates skipped: " << juce::String(summary.rejectedDuplicateCount) << "\n\n";
            msg << "Log: " << mini_daw::getInstrumentCatalogRescanLogFile().getFullPathName() << "\n";
            msg << "Catalog: " << mini_daw::getInstrumentCatalogV1CacheFile().getFullPathName();
            if (summary.bbcSymphonyOrchestraFinding.isNotEmpty())
            {
                msg << "\n\nBBC Symphony Orchestra: " << summary.bbcSymphonyOrchestraFinding;
            }
            juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::InfoIcon, "Instrument plugins", msg);
        });
    }).detach();
}

namespace
{
    void showPluginCacheImportSummaryDialog(const mini_daw::Vst3CacheImportSummary& summary)
    {
        const juce::String logPath = mini_daw::getPluginCacheImportLogFile().getFullPathName();

        if (summary.errorMessage.isNotEmpty())
        {
            juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::WarningIcon,
                                                   "Import plugin cache",
                                                   summary.errorMessage + "\n\nLog: " + logPath);
            return;
        }

        juce::StringArray imported;
        juce::StringArray notFound;
        for (const auto& b : summary.bundles)
        {
            if (b.written)
            {
                imported.add(b.displayName + (b.matchMethod == "bundle-name" ? " (path repaired)" : ""));
            }
            else
            {
                notFound.add(b.displayName);
            }
        }

        juce::String msg;
        if (!summary.anyWritten)
        {
            msg << "Plugin cache import found no matching plugins on this computer.\n\n";
        }
        else if (notFound.isEmpty())
        {
            msg << "Plugin cache import complete.\n\n";
        }
        else
        {
            msg << "Plugin cache import partially completed.\n\n";
        }

        if (!imported.isEmpty())
        {
            msg << "Imported " << juce::String(imported.size()) << " plugin description bundle(s):\n";
            for (const auto& s : imported)
            {
                msg << "- " << s << "\n";
            }
            msg << "\n";
        }
        if (!notFound.isEmpty())
        {
            msg << "Not found on this computer:\n";
            for (const auto& s : notFound)
            {
                msg << "- " << s << "\n";
            }
            msg << "\nInstall the plugin(s) or check your VST3 folders, then import again.\n\n";
        }
        if (summary.anyWritten)
        {
            msg << "Local cache was written to:\n" << summary.localCacheFile.getFullPathName() << "\n\n";
        }
        msg << "Log: " << logPath;

        juce::AlertWindow::showMessageBoxAsync(
            summary.anyWritten ? juce::AlertWindow::InfoIcon : juce::AlertWindow::WarningIcon,
            "Import plugin cache",
            msg);
    }
} // namespace

void AddInstrumentTrackCoordinator::importPluginCacheFromMenu()
{
    bool expectedBusy = false;
    if (!pluginCacheImportBusy_.compare_exchange_strong(expectedBusy, true))
    {
        juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::InfoIcon,
                                               "Import plugin cache",
                                               "A plugin cache import is already running.");
        return;
    }

    pluginCacheImportChooser_ = std::make_unique<juce::FileChooser>(
        "Import plugin cache",
        juce::File::getSpecialLocation(juce::File::userDocumentsDirectory),
        "*.xml");

    AddInstrumentTrackCoordinator* const self = this;
    const mini_daw::AsyncCallbackGuard aliveGuard = asyncLifetime_.guard();
    pluginCacheImportChooser_->launchAsync(
        juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
        [self, aliveGuard](const juce::FileChooser& chooser) {
            if (!aliveGuard.isAlive())
            {
                juce::Logger::writeToLog("[stale-async] skipped: plugin cache import file chooser");
                return;
            }
            const juce::File chosen = chooser.getResult();
            if (chosen == juce::File{})
            {
                self->pluginCacheImportBusy_.store(false);
                return;
            }
            // Import scans local VST3 folders on disk; keep it off the message thread.
            // Same lifetime pattern as rescanInstrumentPluginsFromMenu (coordinator outlives the UI).
            writeLastOperationBreadcrumb("plugin cache import start: " + chosen.getFullPathName());
            std::thread([self, aliveGuard, chosen] {
                const mini_daw::Vst3CacheImportSummary summary
                    = mini_daw::importExperimentalVst3DescriptionsCacheFileWithPathRepair(chosen);
                juce::MessageManager::callAsync([self, aliveGuard, summary] {
                    if (!aliveGuard.isAlive())
                    {
                        juce::Logger::writeToLog("[stale-async] skipped: plugin cache import completion");
                        return;
                    }
                    self->pluginCacheImportBusy_.store(false);
                    writeLastOperationBreadcrumb("plugin cache import end");
                    showPluginCacheImportSummaryDialog(summary);
                });
            }).detach();
        });
}

void AddInstrumentTrackCoordinator::addGenericInstrumentTrackFromCatalog(
    const mini_daw::InstrumentCatalogEntry& entry)
{
    Session& session = refs_.session;
    InstrumentRuntimeCoordinator& instrumentRuntimeCoordinator = refs_.instrumentRuntimeCoordinator;

    const juce::File bundle(entry.bundlePath);
    if (!bundle.exists())
    {
        juce::AlertWindow::showMessageBoxAsync(
            juce::AlertWindow::WarningIcon,
            "Instrument track",
            "VST3 bundle no longer exists at:\n" + entry.bundlePath);
        return;
    }

    const juce::String trackName = entry.description.name.isNotEmpty()
                                       ? entry.description.name
                                       : bundle.getFileNameWithoutExtension();

    const std::optional<TrackId> newIdOpt = session.appendExperimentalInstrumentShellTrack(trackName);
    if (!newIdOpt.has_value())
    {
        juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::InfoIcon,
                                               "Instrument track",
                                               "Could not add instrument track shell.");
        return;
    }

    const TrackId tid = *newIdOpt;
    const auto pr = instrumentRuntimeCoordinator.getOrCreateInstrumentRuntimeForTrack(tid);
    ExperimentalInstrumentHost* mh = pr.first;
    InstrumentTrackController* ctl = pr.second;

    if (mh == nullptr || ctl == nullptr)
    {
        juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::InfoIcon,
                                                "Instrument track",
                                                "Could not allocate instrument runtime.");
        session.removeTrack(tid);
        instrumentRuntimeCoordinator.removeInstrumentRuntimeForTrack(tid);
        callbacks_.refreshInstrumentUi();
        return;
    }

    juce::String loadWarn;
    const juce::Result loadRes = mh->loadInstrumentFromDescription(
        entry.description, bundle, "catalog-v1", nullptr, &loadWarn);

    if (!loadRes.wasOk())
    {
        juce::AlertWindow::showMessageBoxAsync(
            juce::AlertWindow::WarningIcon,
            "Instrument track",
            "Could not load \"" + trackName + "\" into a new instrument track.\n\n"
                + loadRes.getErrorMessage());
        session.removeTrack(tid);
        instrumentRuntimeCoordinator.removeInstrumentRuntimeForTrack(tid);
        callbacks_.refreshInstrumentUi();
        return;
    }

    if (!ctl->bootstrapGenericCatalogInstrumentShellForSessionTrack(tid))
    {
        juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::InfoIcon,
                                                "Instrument track",
                                                "Could not bind the loaded instrument to the new timeline row.");
        mh->unloadInstrument();
        session.removeTrack(tid);
        instrumentRuntimeCoordinator.removeInstrumentRuntimeForTrack(tid);
        callbacks_.refreshInstrumentUi();
        return;
    }

    ctl->syncShellWithHostState();
    instrumentRuntimeCoordinator.updateExperimentalPlaybackBridgeAfterRegistryChange();
    callbacks_.syncMidiEditorInstrumentClipTimelineFromDeviceIfOpen();
    callbacks_.refreshInstrumentUi();
    callbacks_.requestLayoutResized();

    if (loadWarn.isNotEmpty())
    {
        juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::InfoIcon, "Instrument track", loadWarn);
    }
}
