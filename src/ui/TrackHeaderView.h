#pragma once

// =============================================================================
// TrackHeaderView  —  minimal label + active highlight for one track (message thread)
// =============================================================================
// Click without drag calls `Session::setActiveTrack` (no snapshot republish). The lane is to the
// right; cross-track drag does not treat the header as a drop target.
// =============================================================================

#include "domain/Track.h"

#include <functional>
#include <juce_gui_basics/juce_gui_basics.h>

class Session;

class TrackHeaderView : public juce::Component
{
public:
    TrackHeaderView(
        Session& session,
        TrackId trackId,
        std::function<void()> onActiveChanged) noexcept;

    void paint(juce::Graphics& g) override;
    void mouseUp(const juce::MouseEvent& e) override;

private:
    Session& session_;
    const TrackId trackId_;
    std::function<void()> onActiveChanged_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TrackHeaderView)
};
