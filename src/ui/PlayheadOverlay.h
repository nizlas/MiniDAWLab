#pragma once

// =============================================================================
// PlayheadOverlay — lane-column playhead line only (message thread)
// =============================================================================
// Draws the session playhead stroke over waveform lanes without forcing each
// `ClipWaveformView` to repaint at playhead cadence.

#include "domain/Session.h"
#include "ui/TimelineViewportModel.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <cstdint>

class Transport;

class PlayheadOverlay final : public juce::Component, private juce::Timer
{
public:
    PlayheadOverlay(Session& session, Transport& transport, TimelineViewportModel& timelineViewport);

    ~PlayheadOverlay() override;

    void paint(juce::Graphics& g) override;

private:
    void timerCallback() override;

    Session& session_;
    Transport& transport_;
    TimelineViewportModel& timelineViewport_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PlayheadOverlay)
};
