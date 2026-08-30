#pragma once

#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>

#include <juce_graphics/juce_graphics.h>

#include "domain/ProjectMusicalTime.h"

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

/// Default cull window = "no culling" (full visible range). Callers that know the dirty clip
/// region (e.g. a narrow playhead-stripe repaint) pass a tighter sample window so tick/label/grid
/// loops iterate only what can intersect the dirty area (zoom-freeze fix: stripe repaints used to
/// pay the full visible-span loop cost at 60 Hz during playback).
constexpr std::int64_t kNoCullStartSamples = std::numeric_limits<std::int64_t>::min();
constexpr std::int64_t kNoCullEndSamples = std::numeric_limits<std::int64_t>::max();

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
    double displaySampleRate,
    std::int64_t cullStartSamples = kNoCullStartSamples,
    std::int64_t cullEndSamples = kNoCullEndSamples);

void paintRulerTimeLabels(
    juce::Graphics& g,
    const juce::Rectangle<float>& rulerBounds,
    const std::function<float(std::int64_t)>& sampleToX,
    std::int64_t arrangementExtentSamples,
    std::int64_t visibleStartSamples,
    std::int64_t visibleLengthSamples,
    double displaySampleRate,
    std::int64_t leftLocatorSamples,
    std::int64_t rightLocatorSamples,
    std::int64_t cullStartSamples = kNoCullStartSamples,
    std::int64_t cullEndSamples = kNoCullEndSamples);

/// Arrangement ruler only (main timeline): bars/beats ticks from project tempo + meter.
/// Ticks follow the **visible** sample window (`visibleStart` + `visibleLength`); `arrangementExtentSamples`
/// is kept for call-site symmetry and is currently unused.
void paintRulerMusicalTickMarks(
    juce::Graphics& g,
    const juce::Rectangle<float>& rulerBounds,
    const std::function<float(std::int64_t)>& sampleToX,
    std::int64_t arrangementExtentSamples,
    std::int64_t visibleStartSamples,
    std::int64_t visibleLengthSamples,
    double displaySampleRate,
    double samplesPerPixel,
    ProjectMusicalTime projectMusicalTime,
    std::int64_t cullStartSamples = kNoCullStartSamples,
    std::int64_t cullEndSamples = kNoCullEndSamples);

/// Arrangement ruler only: bar numbers at bar boundaries (sparse when zoomed out); when zoomed in,
/// bar.N.1 at each bar line plus optional beat labels if beat grid lines are shown — bar-start labels
/// stay primary (no beat-only streak across bars). Visible window only; `arrangementExtentSamples` unused.
/// Musical labels keep their spacing/suppression decisions **independent of the cull window**
/// (the whole visible loop still runs so label placement matches a full repaint exactly); the cull
/// window only skips the expensive `drawText` for labels that cannot intersect the dirty region.
void paintRulerMusicalLabels(
    juce::Graphics& g,
    const juce::Rectangle<float>& rulerBounds,
    const std::function<float(std::int64_t)>& sampleToX,
    std::int64_t arrangementExtentSamples,
    std::int64_t visibleStartSamples,
    std::int64_t visibleLengthSamples,
    double displaySampleRate,
    double samplesPerPixel,
    ProjectMusicalTime projectMusicalTime,
    std::int64_t leftLocatorSamples,
    std::int64_t rightLocatorSamples,
    std::int64_t cullStartSamples = kNoCullStartSamples,
    std::int64_t cullEndSamples = kNoCullEndSamples);

/// Vertical musical grid for arrangement timeline column (lanes area); faint lines behind clips.
/// Visible window only; `arrangementExtentSamples` unused (call-site symmetry).
void paintArrangementMusicalVerticalGrid(
    juce::Graphics& g,
    const juce::Rectangle<float>& gridBounds,
    const std::function<float(std::int64_t)>& sampleToX,
    std::int64_t arrangementExtentSamples,
    std::int64_t visibleStartSamples,
    std::int64_t visibleLengthSamples,
    double displaySampleRate,
    double samplesPerPixel,
    ProjectMusicalTime projectMusicalTime,
    std::int64_t cullStartSamples = kNoCullStartSamples,
    std::int64_t cullEndSamples = kNoCullEndSamples);

} // namespace timeline_locator_paint
