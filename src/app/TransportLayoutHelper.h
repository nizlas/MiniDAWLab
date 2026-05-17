#pragma once

#include <JuceHeader.h>

#include "ui/CollapsibleSideStrip.h"

class TimelineRulerView;
class TrackLanesView;
class InspectorView;
class PlayheadOverlay;
class EditToolIconStrip;

namespace mini_daw_app_transport
{

/// References to `TransportControlsContent` widgets touched by `applyTransportControlsLayout`.
/// Widget ownership stays with `TransportControlsContent`; this is layout-only.
struct TransportLayoutRefs
{
    juce::Component& owner;

    TimelineRulerView& rulerView;
    TrackLanesView& trackLanesView;
    InspectorView& inspectorView;

    juce::MenuBarComponent& menuBar;

    juce::Button& addTrackCornerPlusButton;

    EditToolIconStrip& editToolStrip;

    juce::Label& arrangementBpmLabel;
    juce::TextEditor& arrangementBpmEditor;
    juce::ComboBox& arrangementTimeSignatureCombo;

    juce::ComboBox& arrangementTimelineFormatCombo;

    juce::ToggleButton& arrangementSnapToggle;
    juce::ComboBox& arrangementSnapResolutionCombo;

    juce::Label& countInStatusLabel;
    juce::Label& keyDiagLabel;
    /// May be null when shortcut diagnostics UI is not constructed.
    juce::Label* shortcutDiagLabel;

    int& inspectorCurrentWidth;

    collapsible_side_strip::ResizeSplitter& inspectorResizeSplitter;
    collapsible_side_strip::CollapsedKnob& inspectorCollapsedKnob;

    /// May be null until constructed in the owner ctor.
    PlayheadOverlay* lanePlayheadOverlay;
};

void applyTransportControlsLayout(const TransportLayoutRefs& refs);

} // namespace mini_daw_app_transport
