#include "app/TransportLayoutHelper.h"

#include "app/ShortcutDiagnostics.h"
#include "ui/InspectorView.h"
#include "ui/PlayheadOverlay.h"
#include "ui/TimelineRulerView.h"
#include "ui/TrackLanesView.h"

void mini_daw_app_transport::applyTransportControlsLayout(const TransportLayoutRefs& r)
{
    auto area = r.owner.getLocalBounds().reduced(8);
    auto row = area.removeFromTop(32);
    if (shortcut_diagnostics::kShowKeyDiagnostic)
    {
        r.keyDiagLabel.setBounds(row.removeFromRight(300).reduced(2, 0));
    }
    constexpr int kCountInLabelWidth = 140;
    r.countInStatusLabel.setBounds(row.removeFromRight(kCountInLabelWidth).reduced(4, 0));
    const int buttonWidth = juce::jmax(48, row.getWidth() / 8);

    r.addClipButton.setBounds(row.removeFromLeft(buttonWidth).reduced(2));
    r.addTrackButton.setBounds(row.removeFromLeft(buttonWidth).reduced(2));
    r.saveProjectButton.setBounds(row.removeFromLeft(buttonWidth).reduced(2));
    r.loadProjectButton.setBounds(row.removeFromLeft(buttonWidth).reduced(2));
    r.playPauseButton.setBounds(row.removeFromLeft(buttonWidth).reduced(2));
    r.stopButton.setBounds(row.removeFromLeft(buttonWidth).reduced(2));
    r.audioSettingsButton.setBounds(row.removeFromLeft(buttonWidth).reduced(2));
    r.helpButton.setBounds(row.removeFromLeft(buttonWidth).reduced(2));
    auto toolRow = area.removeFromTop(28);
    constexpr int kToolButtonW = 80;
    r.pointerToolButton.setBounds(toolRow.removeFromLeft(kToolButtonW).reduced(2, 2));
    r.splitToolButton.setBounds(toolRow.removeFromLeft(kToolButtonW).reduced(2, 2));
    if constexpr (shortcut_diagnostics::kShowShortcutDiagnostics)
    {
        if (r.shortcutDiagLabel != nullptr)
        {
            r.shortcutDiagLabel->setBounds(area.removeFromTop(28));
        }
    }
    constexpr int kTimelineRulerHeight = 20;
    const int lanesBandTop = area.getY() + kTimelineRulerHeight;
    const int lanesBandHeight = juce::jmax(0, area.getHeight() - kTimelineRulerHeight);
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
    auto timelineRow = area.removeFromTop(kTimelineRulerHeight);
    timelineRow.removeFromLeft(TrackLanesView::kTrackHeaderWidth);
    r.rulerView.setBounds(timelineRow);
    r.trackLanesView.setBounds(area);
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

        const int topY = r.trackLanesView.getY();
        const int bottomY = r.trackLanesView.getBottom();
        if (laneW > 0 && bottomY > topY)
        {
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
