#include "app/PluginHostUiBindings.h"

#include "app/Vst3PluginPickerCoordinator.h"
#include "plugins/InsertSlotId.h"
#include "plugins/PluginEditorWindows.h"
#include "plugins/PluginInsertHost.h"
#include "ui/InspectorView.h"
#include "ui/TrackLanesView.h"

namespace
{
PluginInsertHost::InsertInstanceToken tokenFor(const TrackId trackId, const InsertSlotId slotId, const std::uint64_t epoch)
{
    PluginInsertHost::InsertInstanceToken token;
    token.trackId = trackId;
    token.slotId = slotId;
    token.instanceEpoch = epoch;
    return token;
}

void showSettingsError(const juce::String& title, const juce::Result& result)
{
    if (!result.wasOk())
    {
        juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::WarningIcon, title, result.getErrorMessage());
    }
}
} // namespace

InspectorPluginHost PluginHostUiBindings::makeInsertSeam(PluginInsertHost& pluginHost,
                                                         Vst3PluginPickerCoordinator& picker,
                                                         juce::Component* const pickerAnchor)
{
    PluginInsertHost* const host = &pluginHost;
    Vst3PluginPickerCoordinator* const pick = &picker;
    return {
        [host](const TrackId tid) { return host->hasAnyInsertOnTrack(tid); },
        [host](const TrackId tid) {
            std::vector<InspectorInsertRow> rows;
            rows.reserve(8);
            for (const auto& rv : host->getInsertRowsForTrack(tid))
            {
                InspectorInsertRow ir;
                ir.slotId = rv.slotId;
                ir.stage = rv.stage;
                ir.displayName = rv.displayName;
                ir.unavailable = rv.unavailable;
                ir.hostActive = rv.hostActive;
                ir.hostBypassed = rv.hostBypassed;
                rows.push_back(std::move(ir));
            }
            return rows;
        },
        [pick, pickerAnchor](const TrackId tid, const InsertStage st) {
            pick->showVst3PluginPickerForTrack(
                tid,
                st == InsertStage::Pre ? Vst3PluginPickerCoordinator::InsertPickerMode::AddPre
                                       : Vst3PluginPickerCoordinator::InsertPickerMode::AddPost,
                pickerAnchor);
        },
        [host](const TrackId tid, const InsertSlotId sid) { host->openNativeEditor(tid, sid); },
        [host](const TrackId tid, const InsertSlotId sid) { host->removeInsert(tid, sid); },
        [host](const TrackId tid, const InsertSlotId sid, const InsertStage st, const int gap) {
            host->moveInsertToStageAtGap(tid, sid, st, gap);
        },
        [host](const TrackId tid, const InsertSlotId sid, const int gapIndex) {
            host->reorderInsertWithinStage(tid, sid, gapIndex);
        },
        [host](const TrackId tid, const InsertSlotId sid, const bool active) {
            host->setInsertHostActive(tid, sid, active);
        },
        [host](const TrackId tid, const InsertSlotId sid, const bool bypassed) {
            host->setInsertHostBypassed(tid, sid, bypassed);
        },
        [host](const TrackId tid, const InsertSlotId sid) {
            return host->insertInstanceToken(tid, sid).instanceEpoch;
        },
        [host](const TrackId tid, const InsertSlotId sid) {
            return host->insertInstanceToken(tid, sid).namesALiveInstance();
        },
        [host](const TrackId tid, const InsertSlotId sid) { return host->canPasteInsertSettings(tid, sid); },
        [host](const TrackId tid, const InsertSlotId sid, const std::uint64_t epoch) {
            showSettingsError("Copy settings", host->copyInsertSettings(tokenFor(tid, sid, epoch)));
        },
        [host](const TrackId tid, const InsertSlotId sid, const std::uint64_t epoch) {
            showSettingsError("Paste settings", host->pasteInsertSettings(tokenFor(tid, sid, epoch)));
        },
        [host](const TrackId tid, const InsertSlotId sid, const std::uint64_t epoch) {
            launchInsertPresetFileChooser(*host, tid, sid, epoch, false, {});
        },
        [host](const TrackId tid, const InsertSlotId sid, const std::uint64_t epoch) {
            launchInsertPresetFileChooser(*host, tid, sid, epoch, true, {});
        }
    };
}

void PluginHostUiBindings::install(Refs r)
{
    // Capture `r` by value so stored callbacks do not refer to the temporary `Refs` stack frame.
    r.trackLanesView.setTrackHeaderPluginHost(
        { [r](const TrackId tid) {
              r.vst3PluginPickerCoordinator.showVst3PluginPickerForTrack(
                  tid, Vst3PluginPickerCoordinator::InsertPickerMode::AddPost, &r.trackHeaderPluginPickerAnchor);
          },
          [r](const TrackId tid) { r.pluginHost.openNativeEditor(tid); },
          [r](const TrackId tid) { r.pluginHost.openGenericParamsEditor(tid); },
          [r](const TrackId tid) { r.pluginHost.removePlugin(tid); } });

    r.inspectorView.setInspectorPluginHost(makeInsertSeam(r.pluginHost, r.vst3PluginPickerCoordinator, &r.inspectorView));
}
