#pragma once

#include <JuceHeader.h>

#include "ui/CollapsibleSideStrip.h"

class TimelineRulerView;
class TrackLanesView;
class InspectorView;
class PlayheadOverlay;

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

    juce::TextButton& addClipButton;
    juce::TextButton& addTrackButton;
    juce::TextButton& saveProjectButton;
    juce::TextButton& loadProjectButton;
    juce::TextButton& playPauseButton;
    juce::TextButton& stopButton;
    juce::TextButton& audioSettingsButton;
    juce::TextButton& helpButton;

    juce::TextButton& pointerToolButton;
    juce::TextButton& splitToolButton;

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
