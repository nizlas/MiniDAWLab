#include "app/PluginHostUiBindings.h"

#include "app/Vst3PluginPickerCoordinator.h"
#include "plugins/InsertSlotId.h"
#include "plugins/PluginInsertHost.h"
#include "ui/InspectorView.h"
#include "ui/TrackLanesView.h"

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
