#include "app/Vst3PluginPickerCoordinator.h"

#include <thread>
#include <utility>

#include <juce_gui_basics/juce_gui_basics.h>

#include "domain/Session.h"
#include "instruments/InstrumentTrackController.h"
#include "plugins/ExperimentalInstrumentHost.h"
#include "plugins/InsertSlotId.h"
#include "plugins/PluginDiscovery.h"
#include "plugins/PluginInsertHost.h"
#include "plugins/Vst3ChildProcessScan.h"

namespace
{
    [[nodiscard]] juce::String instrumentVst3InsertBlockedMessage()
    {
        return juce::String{
            "This plugin appears to be an instrument. MiniDAWLab does not support instrument inserts on audio tracks yet."};
    }

    [[nodiscard]] juce::String experimentalInstrumentRescanFailureDetail(const mini_daw::Vst3OopScanOutcome outcome,
                                                                         const bool successButNoDescriptions)
    {
        if (successButNoDescriptions)
        {
            return "The scan finished but no plugin descriptions were returned.";
        }
        switch (outcome)
        {
        case mini_daw::Vst3OopScanOutcome::Success:
            return {};
        case mini_daw::Vst3OopScanOutcome::ChildCrashedOrFailed:
            return "The scan process exited with an error.";
        case mini_daw::Vst3OopScanOutcome::Timeout:
            return "The scan timed out.";
        case mini_daw::Vst3OopScanOutcome::LaunchFailed:
            return "The scan process could not be started.";
        case mini_daw::Vst3OopScanOutcome::ParseFailed:
            return "The scan result could not be read.";
        default:
            return "Unknown error.";
        }
    }

    [[nodiscard]] juce::String experimentalInstrumentRescanOutcomeLogTag(const mini_daw::Vst3OopScanOutcome outcome,
                                                                         const bool successButNoDescriptions)
    {
        if (successButNoDescriptions)
        {
            return "success_no_descriptions";
        }
        switch (outcome)
        {
        case mini_daw::Vst3OopScanOutcome::Success:
            return "success";
        case mini_daw::Vst3OopScanOutcome::ChildCrashedOrFailed:
            return "child_failed";
        case mini_daw::Vst3OopScanOutcome::Timeout:
            return "timeout";
        case mini_daw::Vst3OopScanOutcome::LaunchFailed:
            return "launch_failed";
        case mini_daw::Vst3OopScanOutcome::ParseFailed:
            return "parse_failed";
        default:
            return "unknown";
        }
    }
} // namespace

Vst3PluginPickerCoordinator::Vst3PluginPickerCoordinator(juce::Component& ownerUi,
                                                         Session& session,
                                                         PluginInsertHost& pluginHost,
                                                         Callbacks callbacks)
    : ownerUi_(ownerUi)
    , session_(session)
    , pluginHost_(pluginHost)
    , callbacks_(std::move(callbacks))
{
    juce::ignoreUnused(session_);
    setInterceptsMouseClicks(false, false);
    setOpaque(false);
}

Vst3PluginPickerCoordinator::~Vst3PluginPickerCoordinator()
{
    if (juce::Component* p = getParentComponent())
    {
        p->removeChildComponent(this);
    }
}

void Vst3PluginPickerCoordinator::showVst3PluginPickerForTrack(const TrackId trackId,
                                                               const InsertPickerMode mode,
                                                               juce::Component* anchor)
{
    if (trackId == kInvalidTrackId)
    {
        return;
    }
    juce::FileSearchPath combined = mini_daw::getStandardVst3SearchPaths();
    const juce::FileSearchPath userPaths = mini_daw::loadUserVst3SearchPaths();
    for (int i = 0; i < userPaths.getNumPaths(); ++i)
    {
        combined.add(userPaths[i], -1);
    }
    auto scan = mini_daw::scanForVst3Plugins(combined);

    juce::PopupMenu menu;
    juce::PopupMenu discovered;
    if (scan.entries.empty())
    {
        discovered.addItem(
            juce::PopupMenu::Item("(no VST3 plugins found)").setEnabled(false));
    }
    else
    {
        constexpr int kFoundBase = 1000;
        for (size_t i = 0; i < scan.entries.size(); ++i)
        {
            const auto& en = scan.entries[i];
            juce::PopupMenu::Item item(
                en.support == mini_daw::PluginPickerSupport::SupportedCandidate
                    ? en.displayName
                    : en.displayName + "  (" + en.unsupportedReason + ")");
            item.itemID = static_cast<int>(kFoundBase + static_cast<int>(i));
            item.setEnabled(en.support == mini_daw::PluginPickerSupport::SupportedCandidate);
            discovered.addItem(item);
        }
    }
    menu.addSubMenu("Discovered VST3 plugins", discovered);
    menu.addSeparator();
    menu.addItem(1, "Add VST3 Folder...");
    menu.addItem(2, "Load Specific VST3...");

    juce::Component* const target = (anchor != nullptr) ? anchor : &ownerUi_;
    juce::Component::SafePointer<Vst3PluginPickerCoordinator> safePicker(this);
    std::vector<mini_daw::PluginDiscoveryEntry> entries = std::move(scan.entries);
    auto refreshInspector = callbacks_.refreshInspectorFromSession;
    PluginInsertHost* const pluginHost = &pluginHost_;
    menu.showMenuAsync(
        juce::PopupMenu::Options().withTargetComponent(target),
        [safePicker,
         pluginHost,
         refreshInspector,
         trackId,
         mode,
         entries = std::move(entries),
         anchor](const int result) {
            if (safePicker == nullptr || result == 0)
            {
                return;
            }
            if (result == 1)
            {
                safePicker->beginAddVst3FolderForTrack(trackId, anchor, mode);
                return;
            }
            if (result == 2)
            {
                safePicker->beginLoadVst3ForTrack(trackId, mode);
                return;
            }
            constexpr int kFoundBase = 1000;
            const size_t idx = static_cast<size_t>(result - kFoundBase);
            if (idx >= entries.size())
            {
                return;
            }
            if (mini_daw::classifyVst3Candidate(entries[idx].file.getFileNameWithoutExtension())
                == mini_daw::PluginPickerSupport::UnsupportedInstrument)
            {
                juce::AlertWindow::showMessageBoxAsync(
                    juce::AlertWindow::WarningIcon, "VST3", instrumentVst3InsertBlockedMessage());
                if (refreshInspector != nullptr)
                {
                    refreshInspector();
                }
                return;
            }
            const InsertStage stage
                = (mode == InsertPickerMode::AddPre) ? InsertStage::Pre : InsertStage::Post;
            const juce::Result r = pluginHost->addInsertFromVst3File(trackId, stage, entries[idx].file);
            if (!r.wasOk())
            {
                juce::AlertWindow::showMessageBoxAsync(
                    juce::AlertWindow::WarningIcon, "VST3", r.getErrorMessage());
            }
            if (refreshInspector != nullptr)
            {
                refreshInspector();
            }
        });
}

void Vst3PluginPickerCoordinator::beginAddVst3FolderForTrack(const TrackId trackId,
                                                           juce::Component* anchor,
                                                           const InsertPickerMode mode)
{
    if (trackId == kInvalidTrackId)
    {
        return;
    }
    if (vst3FolderChooserInFlight_)
    {
        return;
    }
    vst3FolderChooserInFlight_ = true;
    const auto chooserFlags = juce::FileBrowserComponent::openMode
                              | juce::FileBrowserComponent::canSelectDirectories;
    auto chooser = std::make_shared<juce::FileChooser>("Add VST3 search folder", juce::File{}, "*");
    juce::Component::SafePointer<Vst3PluginPickerCoordinator> safePicker(this);
    bool* const folderInFlightFlag = &vst3FolderChooserInFlight_;
    chooser->launchAsync(
        chooserFlags,
        [safePicker, chooser, trackId, anchor, mode, folderInFlightFlag](const juce::FileChooser& fc) {
            juce::ignoreUnused(chooser);
            if (safePicker == nullptr)
            {
                return;
            }
            struct ClearFolderChooser
            {
                bool* p;
                explicit ClearFolderChooser(bool* f) noexcept
                    : p(f)
                {
                }
                ~ClearFolderChooser()
                {
                    if (p != nullptr)
                    {
                        *p = false;
                    }
                }
            } clearFlag{ folderInFlightFlag };
            const juce::File folder = fc.getResult();
            if (!folder.isDirectory())
            {
                return;
            }
            juce::FileSearchPath paths = mini_daw::loadUserVst3SearchPaths();
            paths.add(folder, -1);
            mini_daw::saveUserVst3SearchPaths(paths);
            safePicker->showVst3PluginPickerForTrack(trackId, mode, anchor);
        });
}

void Vst3PluginPickerCoordinator::beginLoadVst3ForTrack(const TrackId trackId,
                                                        const InsertPickerMode mode)
{
    if (trackId == kInvalidTrackId)
    {
        return;
    }
    if (vst3ChooserInFlight_)
    {
        return;
    }
    vst3ChooserInFlight_ = true;
    const auto fileChooserFlags = juce::FileBrowserComponent::openMode
                                  | juce::FileBrowserComponent::canSelectFiles;
    auto chooser = std::make_shared<juce::FileChooser>("Load VST3", juce::File{}, "*.vst3");
    juce::Component::SafePointer<Vst3PluginPickerCoordinator> safePicker(this);
    bool* const fileInFlightFlag = &vst3ChooserInFlight_;
    auto refreshInspector = callbacks_.refreshInspectorFromSession;
    PluginInsertHost* const pluginHost = &pluginHost_;
    chooser->launchAsync(
        fileChooserFlags,
        [safePicker, chooser, trackId, mode, refreshInspector, pluginHost, fileInFlightFlag](
            const juce::FileChooser& fc) {
            juce::ignoreUnused(chooser);
            if (safePicker == nullptr)
            {
                return;
            }
            struct ClearVst3Chooser
            {
                bool* p;
                explicit ClearVst3Chooser(bool* f) noexcept
                    : p(f)
                {
                }
                ~ClearVst3Chooser()
                {
                    if (p != nullptr)
                    {
                        *p = false;
                    }
                }
            } clearFlag{ fileInFlightFlag };
            const juce::File file = fc.getResult();
            if (!file.exists())
            {
                if (refreshInspector != nullptr)
                {
                    refreshInspector();
                }
                return;
            }
            if (mini_daw::classifyVst3Candidate(file.getFileNameWithoutExtension())
                == mini_daw::PluginPickerSupport::UnsupportedInstrument)
            {
                juce::AlertWindow::showMessageBoxAsync(
                    juce::AlertWindow::WarningIcon, "VST3", instrumentVst3InsertBlockedMessage());
                if (refreshInspector != nullptr)
                {
                    refreshInspector();
                }
                return;
            }
            const InsertStage stage
                = (mode == InsertPickerMode::AddPre) ? InsertStage::Pre : InsertStage::Post;
            const juce::Result r = pluginHost->addInsertFromVst3File(trackId, stage, file);
            if (!r.wasOk())
            {
                juce::AlertWindow::showMessageBoxAsync(
                    juce::AlertWindow::WarningIcon, "VST3", r.getErrorMessage());
            }
            if (refreshInspector != nullptr)
            {
                refreshInspector();
            }
        });
}

void Vst3PluginPickerCoordinator::runExperimentalInstrumentPluginDescriptionRescan()
{
    if (callbacks_.getCanonicalInstrumentLaneTrackIdFromSession == nullptr)
    {
        return;
    }
    const TrackId tid = callbacks_.getCanonicalInstrumentLaneTrackIdFromSession();
    if (tid != kInvalidTrackId)
    {
        runExperimentalInstrumentPluginDescriptionRescanForTrack(tid);
    }
}

void Vst3PluginPickerCoordinator::runExperimentalInstrumentPluginDescriptionRescanForTrack(
    const TrackId tid)
{
    if (callbacks_.getInstrumentHostForTrack == nullptr)
    {
        return;
    }
    if (tid == kInvalidTrackId)
    {
        juce::AlertWindow::showMessageBoxAsync(
            juce::AlertWindow::InfoIcon, "Experimental instrument", "No instrument track is attached.");
        return;
    }
    ExperimentalInstrumentHost* const mh = callbacks_.getInstrumentHostForTrack(tid);
    if (mh == nullptr || !mh->hasInstrument())
    {
        juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::InfoIcon,
                                               "Experimental instrument",
                                               "No instrument plugin is loaded for this track.");
        return;
    }
    const juce::File bundle(mh->getLastLoadedVst3OriginalPath());
    if (!bundle.exists())
    {
        juce::AlertWindow::showMessageBoxAsync(
            juce::AlertWindow::WarningIcon,
            "Experimental instrument",
            "No VST3 bundle path is known for the loaded instrument.");
        return;
    }

    bool expectedBusy = false;
    if (!experimentalOopScanBusy_.compare_exchange_strong(expectedBusy, true))
    {
        mini_daw::writeVst3OopScanDiagnosticLogLine(
            "parent: instrument description rescan ignored (OOP operation already in progress)");
        return;
    }

    mini_daw::writeVst3OopScanDiagnosticLogLine("rescan requested trackId="
                                                + juce::String((juce::int64)tid) + " path=\""
                                                + bundle.getFullPathName() + "\"");

    juce::Component::SafePointer<Vst3PluginPickerCoordinator> safePicker(this);
    std::atomic<bool>* const oopBusyFlag = &experimentalOopScanBusy_;
    auto refreshInstrumentUiCb = callbacks_.refreshInstrumentUi;
    const juce::File bundleCopy = bundle;
    std::thread([safePicker, bundleCopy, oopBusyFlag, refreshInstrumentUiCb] {
        const mini_daw::Vst3OopScanResult scanResult
            = mini_daw::runVst3OopScanBlocking(bundleCopy, mini_daw::kVst3OopScanReplyTimeoutMs);
        juce::MessageManager::callAsync(
            [safePicker, scanResult, bundleCopy, oopBusyFlag, refreshInstrumentUiCb] {
            juce::ignoreUnused(bundleCopy);
            if (safePicker == nullptr)
            {
                return;
            }
            oopBusyFlag->store(false);
            if (refreshInstrumentUiCb != nullptr)
            {
                refreshInstrumentUiCb();
            }

            const bool successNoDesc = scanResult.outcome == mini_daw::Vst3OopScanOutcome::Success
                                       && scanResult.descriptions.empty();
            const bool ok = scanResult.outcome == mini_daw::Vst3OopScanOutcome::Success
                            && !scanResult.descriptions.empty();

            if (!ok)
            {
                const juce::String tag = experimentalInstrumentRescanOutcomeLogTag(
                    scanResult.outcome,
                    successNoDesc);
                mini_daw::writeVst3OopScanDiagnosticLogLine("rescan failed outcome=" + tag);
                juce::String msg = "Plugin description scan failed. Existing cache and loaded instrument "
                                  "were left unchanged.\n\n";
                msg << experimentalInstrumentRescanFailureDetail(scanResult.outcome, successNoDesc);
                juce::AlertWindow::showMessageBoxAsync(
                    juce::AlertWindow::WarningIcon, "Experimental instrument", msg);
                return;
            }

            mini_daw::writeVst3OopScanDiagnosticLogLine(
                "rescan success descriptionCount=" + juce::String((int)scanResult.descriptions.size())
                + " v2Updated=yes");
        });
    }).detach();
}
