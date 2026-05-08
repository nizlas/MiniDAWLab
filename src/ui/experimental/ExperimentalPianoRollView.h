#pragma once

#include "ui/experimental/ExperimentalMidiPattern.h"

#include <cstdint>

#include <juce_gui_basics/juce_gui_basics.h>

namespace juce
{
class AudioDeviceManager;
}

class ExperimentalMidiPatternPlayer;
struct InstrumentMidiClip;
class Session;
class Transport;

/// Drum hits mode: diamonds at step centers; piano-style rows 24..72.
/// I3d1: when bound to an `InstrumentMidiClip` + session/transport, X is **session-absolute samples**
/// with an **independent** zoom/pan (not `TimelineViewportModel`).
class ExperimentalPianoRollView final : public juce::Component, private juce::Timer
{
public:
    static constexpr int kPitchLow = 24;
    static constexpr int kPitchHigh = 72;
    static constexpr int kRowHeight = 14;
    static constexpr int kKeyboardWidth = 40;
    /// Absolute-timeline mode only (0 in legacy step-local mode).
    static constexpr int kRulerHeight = 22;

    ExperimentalPianoRollView(ExperimentalMidiPattern& pattern, ExperimentalMidiPatternPlayer* player);

    void paint(juce::Graphics& g) override;
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override;
    void resized() override;

    /// Absolute timeline mode (clip editor). Pass nullptrs to use legacy clip-local step grid (internal pattern only).
    void setSessionTimelineContext(InstrumentMidiClip* timelineClip,
                                   Session* session,
                                   Transport* transport,
                                   juce::AudioDeviceManager* deviceManager) noexcept;

    void setFollowPlayheadEnabled(bool on) noexcept;

    void seedOrResetViewport();

private:
    void timerCallback() override;

    bool loggedFirstPaint_ = false;

    [[nodiscard]] bool useAbsoluteTimeline() const noexcept;
    void ensureViewportSeeded();
    [[nodiscard]] std::int64_t sampleAtGridX(float localX) const noexcept;
    [[nodiscard]] float xForSessionSample(std::int64_t s) const noexcept;
    [[nodiscard]] int pitchAtY(int y) const;
    [[nodiscard]] int stepAtPatternX(int x) const;
    [[nodiscard]] int stepAtTimelineX(int x) const;
    [[nodiscard]] int timelineRulerHeight() const noexcept;
    [[nodiscard]] juce::Rectangle<int> rulerCornerBounds() const;
    [[nodiscard]] juce::Rectangle<int> rulerTrackBounds() const;
    [[nodiscard]] juce::Rectangle<int> keyboardBounds() const;
    [[nodiscard]] juce::Rectangle<int> gridBounds() const;
    [[nodiscard]] std::int64_t visibleEndSamples() const noexcept;
    [[nodiscard]] float cellWidth() const;

    ExperimentalMidiPattern& pattern_;
    ExperimentalMidiPatternPlayer* player_;

    InstrumentMidiClip* timelineClip_ = nullptr;
    Session* session_ = nullptr;
    Transport* transport_ = nullptr;
    juce::AudioDeviceManager* deviceManager_ = nullptr;

    std::int64_t visibleStartSamples_ = 0;
    double samplesPerPixel_ = 0.0;
    bool followPlayhead_ = false;
    bool viewportInitialized_ = false;

    /// Invalidate when rebinding so the next `timerCallback` repaints (locators/cycle/playhead).
    bool sessionTransportSnapshotValid_ = false;
    std::int64_t lastObservedPlayheadUi_ = 0;
    std::int64_t lastObservedLocLUi_ = 0;
    std::int64_t lastObservedLocRUi_ = 0;
    bool lastObservedCycleUi_ = false;

    bool clipGeometrySnapshotValid_ = false;
    std::int64_t lastObservedClipStartSamplesUi_ = 0;
    std::int64_t lastObservedClipLengthSamplesUi_ = 0;
    int lastObservedNoteCountUi_ = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ExperimentalPianoRollView)
};
