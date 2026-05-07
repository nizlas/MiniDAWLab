#pragma once

#include "ui/experimental/ExperimentalMidiPattern.h"

#include <juce_gui_basics/juce_gui_basics.h>

class ExperimentalMidiPatternPlayer;

/// Drum hits mode: diamonds at step centers; piano-style rows 24..72.
class ExperimentalPianoRollView final : public juce::Component, private juce::Timer
{
public:
    static constexpr int kPitchLow = 24;
    static constexpr int kPitchHigh = 72;
    static constexpr int kRowHeight = 14;
    static constexpr int kKeyboardWidth = 40;

    ExperimentalPianoRollView(ExperimentalMidiPattern& pattern, ExperimentalMidiPatternPlayer* player);

    void paint(juce::Graphics& g) override;
    void mouseDown(const juce::MouseEvent& e) override;
    void resized() override;

private:
    void timerCallback() override;

    bool loggedFirstPaint_ = false;

    [[nodiscard]] int pitchAtY(int y) const;
    [[nodiscard]] int stepAtX(int x) const;
    [[nodiscard]] juce::Rectangle<int> keyboardBounds() const;
    [[nodiscard]] juce::Rectangle<int> gridBounds() const;
    [[nodiscard]] float cellWidth() const;

    ExperimentalMidiPattern& pattern_;
    ExperimentalMidiPatternPlayer* player_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ExperimentalPianoRollView)
};
