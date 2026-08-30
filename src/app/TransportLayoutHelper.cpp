#include "app/TransportLayoutHelper.h"

#include "app/ShortcutDiagnostics.h"
#include "ui/EditToolIconStrip.h"
#include "ui/InspectorView.h"
#include "ui/PlayheadOverlay.h"
#include "ui/TimelineRulerView.h"
#include "ui/TrackHeaderView.h"
#include "ui/TrackLanesView.h"

void mini_daw_app_transport::applyTransportControlsLayout(const TransportLayoutRefs& r)
{
    // Horizontal margin matches prior chrome; slightly tighter vertical inset for a more compact top strip.
    auto area = r.owner.getLocalBounds().reduced(8, 6);
    const int menuBarH = juce::jmax(22, r.owner.getLookAndFeel().getDefaultMenuBarHeight());
    r.menuBar.setBounds(area.removeFromTop(menuBarH));

    // Single toolbar band below the menu: count-in (and optional diagnostics) on the right,
    // Pointer/Split strip centred on the window (clamped so it never sits under the right labels).
    constexpr int kToolbarRowH = 28;
    constexpr int kCountInLabelWidth = 140;
    constexpr int kSnapGapAfterTools = 8;
    constexpr int kBpmLabelWidthPx = 28;
    constexpr int kBpmEditorWidthPx = 44;
    constexpr int kMusicalInnerGapPx = 4;
    constexpr int kTimeSigComboWidthPx = 54;
    constexpr int kGapTimeSigToFormatPx = 4;
    constexpr int kTimelineFormatComboWidthPx = 128;
    constexpr int kGapMusicalToSnapPx = 8;
    constexpr int kMusicalClusterWidthPx = kSnapGapAfterTools + kBpmLabelWidthPx + kMusicalInnerGapPx
                                         + kBpmEditorWidthPx + kMusicalInnerGapPx + kTimeSigComboWidthPx
                                         + kGapTimeSigToFormatPx + kTimelineFormatComboWidthPx;
    constexpr int kSnapToggleWidthPx = 52;
    constexpr int kSnapComboWidthPx = 118;
    constexpr int kSnapClusterWidthPx = kSnapToggleWidthPx + 4 + kSnapComboWidthPx;
    constexpr int kToolbarRightReservedPx
        = kMusicalClusterWidthPx + kGapMusicalToSnapPx + kSnapClusterWidthPx;
    const auto fullToolbarRow = area.removeFromTop(kToolbarRowH);
    auto row = fullToolbarRow;
    // Far right of the row: the main-arrangement Follow toggle, then diagnostics/count-in.
    constexpr int kFollowToggleWidth = 64;
    r.mainFollowPlayheadToggle.setBounds(row.removeFromRight(kFollowToggleWidth).reduced(2, 2));
    if (shortcut_diagnostics::kShowKeyDiagnostic)
    {
        r.keyDiagLabel.setBounds(row.removeFromRight(300).reduced(2, 0));
    }
    r.countInStatusLabel.setBounds(row.removeFromRight(kCountInLabelWidth).reduced(4, 0));

    {
        const int pw = EditToolIconStrip::preferredWidth();
        const int ph = EditToolIconStrip::preferredHeight();
        const int useH = juce::jmin(ph, fullToolbarRow.getHeight());
        const int reservedRight = (shortcut_diagnostics::kShowKeyDiagnostic ? 300 : 0) + kCountInLabelWidth
                                  + kToolbarRightReservedPx;

        juce::Rectangle<int> strip(fullToolbarRow.getCentreX() - pw / 2,
                                   fullToolbarRow.getCentreY() - useH / 2,
                                   pw,
                                   useH);
        const int minX = fullToolbarRow.getX();
        const int maxX = fullToolbarRow.getRight() - reservedRight - pw;
        if (maxX >= minX)
        {
            strip.setX(juce::jlimit(minX, maxX, strip.getX()));
        }
        else
        {
            strip.setX(minX);
        }
        r.editToolStrip.setBounds(strip);

        const int musicalTop = fullToolbarRow.getCentreY() - useH / 2;
        int x = strip.getRight() + kSnapGapAfterTools;
        r.arrangementBpmLabel.setBounds(x, musicalTop, kBpmLabelWidthPx, useH);
        x += kBpmLabelWidthPx + kMusicalInnerGapPx;
        r.arrangementBpmEditor.setBounds(x, musicalTop, kBpmEditorWidthPx, useH);
        x += kBpmEditorWidthPx + kMusicalInnerGapPx;
        r.arrangementTimeSignatureCombo.setBounds(x, musicalTop, kTimeSigComboWidthPx, useH);

        x += kTimeSigComboWidthPx + kGapTimeSigToFormatPx;
        r.arrangementTimelineFormatCombo.setBounds(x, musicalTop, kTimelineFormatComboWidthPx, useH);

        const int snapLeft = x + kTimelineFormatComboWidthPx + kGapMusicalToSnapPx;
        r.arrangementSnapToggle.setBounds(snapLeft, musicalTop, kSnapToggleWidthPx, useH);
        r.arrangementSnapResolutionCombo.setBounds(
            snapLeft + kSnapToggleWidthPx + 4, musicalTop, kSnapComboWidthPx, useH);
    }
    if constexpr (shortcut_diagnostics::kShowShortcutDiagnostics)
    {
        if (r.shortcutDiagLabel != nullptr)
        {
            r.shortcutDiagLabel->setBounds(area.removeFromTop(28));
        }
    }
    constexpr int kAddTrackPlusPad = 4;
    const int gutter = TrackLanesView::kArrangementTimelineHeaderGutterPx;
    const int lanesBandTop = area.getY() + gutter;
    const int lanesBandHeight = juce::jmax(0, area.getHeight() - gutter);
    if (r.inspectorCurrentWidth > 0)
    {
        auto inspectorStrip = area.removeFromLeft(r.inspectorCurrentWidth);
        const int splitW = juce::jmin(collapsible_side_strip::kSplitterWidth, inspectorStrip.getWidth());
        const int contentW = juce::jmax(0, inspectorStrip.getWidth() - splitW);
        r.inspectorView.setBounds(
            inspectorStrip.getX(),
            inspectorStrip.getY(),
            contentW,
            inspectorStrip.getHeight());
        r.inspectorView.setVisible(true);
        r.inspectorResizeSplitter.setBounds(
            inspectorStrip.getX() + contentW,
            inspectorStrip.getY(),
            splitW,
            inspectorStrip.getHeight());
        r.inspectorResizeSplitter.setVisible(true);
        r.inspectorCollapsedKnob.setVisible(false);
    }
    else
    {
        if (area.getX() > 0)
        {
            area.setLeft(0);
        }
        r.inspectorView.setBounds(area.getX(), lanesBandTop, 0, lanesBandHeight);
        r.inspectorView.setVisible(true);
        r.inspectorResizeSplitter.setBounds(0, 0, 0, 0);
        r.inspectorResizeSplitter.setVisible(false);
    }
    const int timelineBandTop = area.getY();
    auto timelineRow = area.removeFromTop(gutter);
    auto rulerLaneCorner = timelineRow.removeFromLeft(TrackLanesView::kTrackHeaderWidth);
    {
        const int cellSide = TrackHeaderView::kStripControlCellWidthPx;
        const int maxSide = rulerLaneCorner.getWidth() - 2 * kAddTrackPlusPad;
        const int side = juce::jmax(1, juce::jmin(cellSide, maxSide));
        const int xPlus = rulerLaneCorner.getX() + kAddTrackPlusPad;
        const int yPlus = rulerLaneCorner.getCentreY() - side / 2;
        r.addTrackCornerPlusButton.setBounds(xPlus, yPlus, side, side);
    }
    r.rulerView.setBounds(timelineRow);
    r.trackLanesView.setBounds(
        area.getX(),
        timelineBandTop,
        area.getWidth(),
        juce::jmax(0, area.getBottom() - timelineBandTop));
    if (r.lanePlayheadOverlay != nullptr)
    {
        const int tw = r.trackLanesView.getWidth();
        const int leftStrip = juce::jmin(TrackLanesView::kTrackHeaderWidth, tw);
        const int laneContentLeft = r.trackLanesView.getX() + leftStrip;
        const int laneW = juce::jmax(0, tw - leftStrip);
        static constexpr bool kLogTransportLaneLayout = false;
        if constexpr (kLogTransportLaneLayout)
        {
            juce::Logger::writeToLog(
                "Transport layout: trackLanes=" + r.trackLanesView.getBounds().toString()
                + " kTrackHeaderWidth=" + juce::String(TrackLanesView::kTrackHeaderWidth)
                + " laneContentLeft=" + juce::String(laneContentLeft)
                + " instrumentRowVisible="
                + juce::String(r.trackLanesView.isInstrumentTimelineRowVisible() ? 1 : 0)
                + " ruler=" + r.rulerView.getBounds().toString()
                + " laneW=" + juce::String(laneW));
        }

        // The overlay covers the ruler band too and draws the short ruler marker segment itself,
        // so playhead frames never invalidate the buffered ruler (see PlayheadOverlay).
        const int topY = r.trackLanesView.getY();
        const int bottomY = r.trackLanesView.getBottom();
        if (laneW > 0 && bottomY > topY)
        {
            r.lanePlayheadOverlay->setRulerBandHeightPx(gutter);
            r.lanePlayheadOverlay->setBounds(laneContentLeft, topY, laneW, bottomY - topY);
            r.lanePlayheadOverlay->setVisible(true);
            r.lanePlayheadOverlay->toFront(false);
        }
        else
        {
            r.lanePlayheadOverlay->setBounds(0, 0, 0, 0);
            r.lanePlayheadOverlay->setVisible(false);
        }
    }
    r.rulerView.toFront(false);
    r.addTrackCornerPlusButton.toFront(false);
    if (r.lanePlayheadOverlay != nullptr && r.lanePlayheadOverlay->isVisible())
    {
        r.lanePlayheadOverlay->toFront(false);
    }
    if (r.inspectorCurrentWidth == 0)
    {
        const int knobX = r.trackLanesView.getBounds().getX();
        const int knobY = r.trackLanesView.getBounds().getCentreY()
                          - collapsible_side_strip::kCollapsedKnobHeight / 2;
        r.inspectorCollapsedKnob.setBounds(
            knobX,
            knobY,
            collapsible_side_strip::kCollapsedKnobWidth,
            collapsible_side_strip::kCollapsedKnobHeight);
        r.inspectorCollapsedKnob.setVisible(true);
        r.inspectorCollapsedKnob.toFront(false);
    }
}
