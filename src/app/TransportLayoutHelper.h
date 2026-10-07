#pragma once

#include <JuceHeader.h>

#include "ui/CollapsibleSideStrip.h"

class TimelineRulerView;
class TrackLanesView;
class InspectorView;
class PlayheadOverlay;
class EditToolIconStrip;
class SoloMemoryStrip;

namespace mini_daw_app_transport
{

/// References to `TransportControlsContent` widgets touched by `applyTransportControlsLayout`.
/// Widget ownership stays with `TransportControlsContent`; this is layout-only.
struct TransportLayoutRefs
{
    juce::Component& owner;

    TimelineRulerView& rulerView;
    TrackLanesView& trackLanesView;
    /// The whole Inspector column (`InspectorPanel`: scrollable content + fixed channel panel).
    juce::Component& inspectorView;

    juce::MenuBarComponent& menuBar;

    juce::Button& addTrackCornerPlusButton;

    EditToolIconStrip& editToolStrip;

    juce::Label& arrangementBpmLabel;
    juce::TextEditor& arrangementBpmEditor;
    juce::ComboBox& arrangementTimeSignatureCombo;

    juce::ComboBox& arrangementTimelineFormatCombo;

    juce::ToggleButton& arrangementSnapToggle;
    juce::ComboBox& arrangementSnapResolutionCombo;

    /// Main-arrangement Follow toggle: far right of the toolbar row.
    juce::Button& mainFollowPlayheadToggle;

    juce::Label& countInStatusLabel;
    juce::Label& keyDiagLabel;
    /// May be null when shortcut diagnostics UI is not constructed.
    juce::Label* shortcutDiagLabel;

    int& inspectorCurrentWidth;

    collapsible_side_strip::ResizeSplitter& inspectorResizeSplitter;
    collapsible_side_strip::CollapsedKnob& inspectorCollapsedKnob;

    /// May be null until constructed in the owner ctor.
    PlayheadOverlay* lanePlayheadOverlay;

    /// Vertical arrangement scrollbar (right of the track rows, below the timeline gutter). May be
    /// null; when its range says everything fits it is hidden and the column is not reserved.
    juce::ScrollBar* arrangementVerticalScrollBar;

    /// Solo memory buttons (spec §3): placed in the toolbar band directly above the track-header
    /// column, same height as the Pointer/Split tool row, following the header-column width. May
    /// be null (not constructed); the tool strip's left clamp then falls back to the row edge.
    SoloMemoryStrip* soloMemoryStrip = nullptr;
};

/// Width of the vertical scrollbar column reserved right of the arrangement when it is shown.
inline constexpr int kArrangementVerticalScrollBarWidthPx = 14;

void applyTransportControlsLayout(const TransportLayoutRefs& refs);

} // namespace mini_daw_app_transport
