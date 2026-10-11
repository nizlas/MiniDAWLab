#pragma once

// One Volume or Pan lane under a track. Not a track row. The curve it draws is
// `trackAutomationEvaluate`, the same function playback uses.

#include "domain/Track.h"
#include "domain/TrackAutomation.h"

#include <functional>

#include <juce_gui_basics/juce_gui_basics.h>

class TrackAutomationLaneView final : public juce::Component
{
public:
    static constexpr int kLaneHeightPx = 48;
    static constexpr int kLabelWidthPx = 88;

    TrackAutomationLaneView(int parameterId, TrackId trackId);

    void setCurve(const TrackAutomationCurve& curve, float manual, juce::Colour colour);
    void setTimeMap(double samplesPerPixel, std::int64_t visibleStartSamples, double engineRate) noexcept;
    void setPlayheadSeconds(double seconds) noexcept;

    std::function<void(TrackAutomationCurve curve)> onCurveCommitted;
    /// Swap this lane to the other parameter, or show it beside this one.
    std::function<void(int parameterId)> onParameterChosen;
    std::function<void()> onToggleOtherLane;

    [[nodiscard]] juce::Rectangle<int> getPlotBounds() const noexcept;
    [[nodiscard]] juce::Rectangle<int> getParameterMenuBounds() const noexcept;
    [[nodiscard]] bool isDraggingForTest() const noexcept { return drag_ != Drag::None; }
    [[nodiscard]] float valueToY(float value) const noexcept;
    [[nodiscard]] float secondsToX(double seconds) const noexcept;
    [[nodiscard]] double xToSeconds(float x) const noexcept;
    /// The same mouseDown / mouseDrag / mouseUp path a pointer uses.
    void dragCurveLikeMouseForTest(juce::Point<float> down, juce::Point<float> drag);

private:
    enum class Drag
    {
        None,
        Point,
        Curve
    };

    void paint(juce::Graphics& g) override;
    void mouseMove(const juce::MouseEvent& e) override;
    void mouseExit(const juce::MouseEvent& e) override;
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;
    void mouseUp(const juce::MouseEvent& e) override;
    void mouseDoubleClick(const juce::MouseEvent& e) override;

    [[nodiscard]] float yToValue(float y) const noexcept;
    [[nodiscard]] int pointAt(juce::Point<float> p) const noexcept;
    [[nodiscard]] int segmentAt(double seconds) const noexcept;
    void showHandle(int segment, double u) noexcept;
    void commit() noexcept;

    int parameterId_ = kTrackAutomationVolumeId;
    TrackId trackId_ = kInvalidTrackId;
    TrackAutomationCurve curve_{};
    float manual_ = 0.0f;
    juce::Colour colour_{ juce::Colours::grey };
    double samplesPerPixel_ = 1.0;
    std::int64_t visibleStartSamples_ = 0;
    double engineRate_ = 48000.0;
    double playheadSeconds_ = 0.0;
    bool playheadValid_ = false;

    Drag drag_ = Drag::None;
    int dragIndex_ = -1;
    double grabU_ = 0.5;
    bool handleVisible_ = false;
    int handleSegment_ = -1;
    float handleX_ = 0.0f;
    float handleY_ = 0.0f;
};
