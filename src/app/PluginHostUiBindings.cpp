#include "app/PluginHostUiBindings.h"

#include "app/Vst3PluginPickerCoordinator.h"
#include "plugins/InsertSlotId.h"
#include "plugins/PluginInsertHost.h"
#include "ui/InspectorView.h"
#include "ui/TrackLanesView.h"

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

    r.inspectorView.setInspectorPluginHost({
        [r](const TrackId tid) { return r.pluginHost.hasAnyInsertOnTrack(tid); },
        [r](const TrackId tid) {
            std::vector<InspectorInsertRow> rows;
            rows.reserve(8);
            for (const auto& rv : r.pluginHost.getInsertRowsForTrack(tid))
            {
                InspectorInsertRow ir;
                ir.slotId = rv.slotId;
                ir.stage = rv.stage;
                ir.displayName = rv.displayName;
                rows.push_back(std::move(ir));
            }
            return rows;
        },
        [r](const TrackId tid, const InsertStage st) {
            r.vst3PluginPickerCoordinator.showVst3PluginPickerForTrack(
                tid,
                st == InsertStage::Pre ? Vst3PluginPickerCoordinator::InsertPickerMode::AddPre
                                       : Vst3PluginPickerCoordinator::InsertPickerMode::AddPost,
                &r.inspectorView);
        },
        [r](const TrackId tid, const InsertSlotId sid) { r.pluginHost.openNativeEditor(tid, sid); },
        [r](const TrackId tid, const InsertSlotId sid) { r.pluginHost.removeInsert(tid, sid); },
        [r](const TrackId tid, const InsertSlotId sid, const InsertStage st, const int gap) {
            r.pluginHost.moveInsertToStageAtGap(tid, sid, st, gap);
        },
        [r](const TrackId tid, const InsertSlotId sid, const int gapIndex) {
            r.pluginHost.reorderInsertWithinStage(tid, sid, gapIndex);
        } });
}
