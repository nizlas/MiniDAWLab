#pragma once

#include <cmath>
#include <cstdint>
#include <functional>

#include <juce_graphics/juce_graphics.h>

/// Shared drawing for session L/R locators + cycle band on timeline rulers
/// (`TimelineRulerView`, `ExperimentalPianoRollView` ruler strip).
namespace timeline_locator_paint
{
/// Matches triangle half-width in ruler label collision logic.
constexpr float kLocatorTriangleHalfWidth = 5.5f;

/// Strip behind time labels (same as main ruler).
constexpr float kRulerLabelStripHeightPx = 12.0f;

/// Short playhead stroke from ruler top (matches main `TimelineRulerView` / piano-roll ruler strip).
constexpr float kRulerPlayheadMarkerLengthPx = 7.0f;

void paintLocatorCycleBandAndStripe(
    juce::Graphics& g,
    const juce::Rectangle<float>& rulerBounds,
    const std::function<float(std::int64_t)>& sampleToX,
    std::int64_t visibleStartSamples,
    std::int64_t visibleLengthSamples,
    std::int64_t leftLocatorSamples,
    std::int64_t rightLocatorSamples,
    bool cycleEnabled);

void paintLocatorTriangleHandles(
    juce::Graphics& g,
    const juce::Rectangle<float>& rulerBounds,
    const std::function<float(std::int64_t)>& sampleToX,
    std::int64_t visibleStartSamples,
    std::int64_t visibleLengthSamples,
    std::int64_t leftLocatorSamples,
    std::int64_t rightLocatorSamples,
    bool cycleEnabled);

void paintRulerTickMarks(
    juce::Graphics& g,
    const juce::Rectangle<float>& rulerBounds,
    const std::function<float(std::int64_t)>& sampleToX,
    std::int64_t arrangementExtentSamples,
    std::int64_t visibleStartSamples,
    std::int64_t visibleLengthSamples,
    double displaySampleRate);

void paintRulerTimeLabels(
    juce::Graphics& g,
    const juce::Rectangle<float>& rulerBounds,
    const std::function<float(std::int64_t)>& sampleToX,
    std::int64_t arrangementExtentSamples,
    std::int64_t visibleStartSamples,
    std::int64_t visibleLengthSamples,
    double displaySampleRate,
    std::int64_t leftLocatorSamples,
    std::int64_t rightLocatorSamples);

} // namespace timeline_locator_paint
