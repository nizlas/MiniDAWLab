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
class InstrumentTrackController;
class Session;
class Transport;
class TimelineViewportModel;

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
    /// `mainTimelineViewport` seeds horizontal zoom from the main DAW timeline when the clip has no roll viewport yet.
    void setSessionTimelineContext(InstrumentMidiClip* timelineClip,
                                   Session* session,
                                   Transport* transport,
                                   juce::AudioDeviceManager* deviceManager,
                                   InstrumentTrackController* instrumentTrackController = nullptr,
                                   const TimelineViewportModel* mainTimelineViewport = nullptr) noexcept;

    void setFollowPlayheadEnabled(bool on) noexcept;

    void seedOrResetViewport();

    /// Replace horizontal pan/zoom (absolute timeline mode). Does not persist to clip until `syncViewportToBoundClip()`.
    void setViewportState(std::int64_t visibleStartSamples, double samplesPerPixel) noexcept;

    [[nodiscard]] bool hasValidViewportState() const noexcept;

    /// Writes current roll pan/zoom/Follow into `timelineClip_` when bound (for session + project save).
    void syncViewportToBoundClip() noexcept;

    /// I3f: musical snap for timeline editing (combo ids: 1=Off, 2=1/8, 3=1/16, 4=1/32 at current PPQ).
    void setMusicalSnapComboId(int id) noexcept;

    /// Timeline paint only: 1 = compact hits (drums), 2 = duration bars (melodic). Does not change data or playback.
    void setTimelineNotesDisplayComboId(int id) noexcept;

    [[nodiscard]] std::int64_t getViewportVisibleStartSamples() const noexcept { return visibleStartSamples_; }
    [[nodiscard]] double getViewportSamplesPerPixel() const noexcept { return samplesPerPixel_; }

private:
    void timerCallback() override;

    [[nodiscard]] bool useAbsoluteTimeline() const noexcept;
    void seedViewportFromMainTimelineOrFallback();
    void applyViewportAfterContextBound();
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
    [[nodiscard]] std::int64_t musicalSnapGridTicks() const noexcept;
    [[nodiscard]] std::int64_t referenceTimelineGridTicks() const noexcept;
    void handleTimelineNotesMouseDown(const juce::MouseEvent& e);

    /// Sub-sample horizontal mapping for UI-smoothed playhead (same linear map as integer path).
    [[nodiscard]] float xForSessionSampleD(double s) const noexcept;


    ExperimentalMidiPattern& pattern_;
    ExperimentalMidiPatternPlayer* player_;

    InstrumentMidiClip* timelineClip_ = nullptr;
    Session* session_ = nullptr;
    Transport* transport_ = nullptr;
    juce::AudioDeviceManager* deviceManager_ = nullptr;
    InstrumentTrackController* instrumentTrackController_ = nullptr;
    const TimelineViewportModel* mainTimelineViewport_ = nullptr;

    std::int64_t visibleStartSamples_ = 0;
    double samplesPerPixel_ = 0.0;
    bool followPlayhead_ = false;

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

    int musicalSnapComboId_ = 1;
    /// 1 = Hits (default), 2 = Bars.
    int timelineNotesDisplayComboId_ = 1;
    int lastObservedTimelineNoteCountUi_ = -1;

    /// Last `startTimerHz` value; updated when switching between idle and playback animation rates.
    int uiTimerHzConfigured_ = -1;

    /// Message-thread-only extrapolation for transport playhead (block-quantized UI reads + smooth motion).
    double uiPlayheadDisplaySamples_ = 0.0;
    double uiPlayheadExtrapBaseSample_ = 0.0;
    double uiPlayheadExtrapWallSec_ = 0.0;
    std::int64_t uiPlayheadLastRawPh_ = 0;

    /// Last tick believed playing; used for stop-edge repaint.
    bool wasTransportPlayingUi_ = false;

    /// Latest preview absolute sample for paint when clip-bound (Debug Preview).
    double uiPreviewDisplayAbsSample_ = 0.0;

    /// Last frame transport playhead was considered “in view” for off-screen repaint skipping.
    bool lastOffscreenGatePlayheadInView_ = true;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ExperimentalPianoRollView)
};
