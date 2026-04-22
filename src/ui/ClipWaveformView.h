#pragma once

#include "domain/Session.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <vector>

class Transport;

// Renders a peak waveform from Session / AudioClip (message thread only) and a playhead
// overlay from Transport::readPlayheadSamplesForUi() — not from PlaybackEngine.
class ClipWaveformView : public juce::Component, private juce::Timer
{
public:
    ClipWaveformView(Session& session, Transport& transport);
    ~ClipWaveformView() override;

    void paint(juce::Graphics& g) override;
    void mouseDown(const juce::MouseEvent& e) override;

private:
    void timerCallback() override;
    void rebuildPeaksIfNeeded();

    Session& session_;
    Transport& transport_;
    const AudioClip* lastClip_ = nullptr;
    int lastWidth_ = 0;
    std::vector<float> peaks_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ClipWaveformView)
};
