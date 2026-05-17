#pragma once

#include "ui/experimental/ExperimentalMidiPattern.h"
#include "ui/CollapsibleSideStrip.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <unordered_set>

#include <juce_gui_basics/juce_gui_basics.h>

namespace juce
{
class AudioDeviceManager;
class TextEditor;
}

class ExperimentalMidiPatternPlayer;
struct InstrumentMidiClip;
class InstrumentTrackController;
class Session;
class Transport;
class TimelineViewportModel;

/// Drum hits mode: diamonds at step centers; piano-style rows (range is **per-instance**, see `setEditablePitchRange`).
/// I3d1: when bound to an `InstrumentMidiClip` + session/transport, X is **session-absolute samples**
/// with an **independent** zoom/pan (not `TimelineViewportModel`).
class ExperimentalPianoRollView final : public juce::Component,
                                          public juce::SettableTooltipClient,
                                          private juce::Timer,
                                          private juce::TextEditor::Listener
{
public:
    /// Default drum-oriented editor span (Groove Agent–class lanes).
    static constexpr int kDrumPitchLow = 24;
    static constexpr int kDrumPitchHigh = 72;
    /// Melodic span (~8 octaves C0–C8) for HALion Sonic–class lanes.
    static constexpr int kMelodicPitchLow = 12;
    static constexpr int kMelodicPitchHigh = 108;
    static constexpr int kRowHeight = 14;

    /// Legacy maximum piano strip width (also the resizable upper bound in Piano row mode).
    static constexpr int kMidiEditorKeyboardLaneWidthPianoMax = 40;
    /// Default ~half of the legacy 40px strip.
    static constexpr int kMidiEditorKeyboardLaneWidthPianoDefault = 20;
    static constexpr int kMidiEditorKeyboardLaneWidthPianoMin = 18;

    /// Legacy maximum drum-name column width (former fixed width); user can drag up to this for long labels.
    static constexpr int kMidiEditorKeyboardLaneWidthDrumNamesMax = 120;
    /// Default ~half of the legacy 120px drum name lane.
    static constexpr int kMidiEditorKeyboardLaneWidthDrumNamesDefault = 60;
    static constexpr int kMidiEditorKeyboardLaneWidthDrumNamesMin = 44;

    /// Absolute-timeline mode only (0 in legacy step-local mode).
    static constexpr int kRulerHeight = 22;

    ExperimentalPianoRollView(ExperimentalMidiPattern& pattern, ExperimentalMidiPatternPlayer* player);

    /// Inclusive MIDI note bounds for visible piano rows (clamped 0–127). Default: drum range.
    void setEditablePitchRange(int lowInclusive, int highInclusive) noexcept;
    [[nodiscard]] int pitchLow() const noexcept { return pitchLow_; }
    [[nodiscard]] int pitchHigh() const noexcept { return pitchHigh_; }

    void paint(juce::Graphics& g) override;
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;
    void mouseUp(const juce::MouseEvent& e) override;
    void mouseDoubleClick(const juce::MouseEvent& e) override;
    void mouseMove(const juce::MouseEvent& e) override;
    void mouseExit(const juce::MouseEvent& e) override;
    void mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override;
    void resized() override;

    /// Absolute timeline mode (clip editor). Pass nullptrs to use legacy clip-local step grid (internal pattern only).
    /// When the clip has no saved roll viewport, horizontal zoom is seeded to ~5 bars using `Session` tempo/meter (not main-timeline zoom).
    void setSessionTimelineContext(InstrumentMidiClip* timelineClip,
                                   Session* session,
                                   Transport* transport,
                                   juce::AudioDeviceManager* deviceManager,
                                   InstrumentTrackController* instrumentTrackController = nullptr,
                                   const TimelineViewportModel* mainTimelineViewport = nullptr) noexcept;

    void setFollowPlayheadEnabled(bool on) noexcept;

    /// I3h: same predicate as `TimelineRulerView` — block seek/locator/cycle during count-in/recording.
    void setTransportGestureBlockPredicate(std::function<bool()> f) noexcept;

    /// After controller rebuild, `timelineClip_` may dangle; compares controller lookup by id to current pointer.
    [[nodiscard]] bool isTimelineClipBindingFresh() const noexcept;

    void seedOrResetViewport();

    /// Replace horizontal pan/zoom (absolute timeline mode). Does not persist to clip until `syncViewportToBoundClip()`.
    void setViewportState(std::int64_t visibleStartSamples, double samplesPerPixel) noexcept;

    [[nodiscard]] bool hasValidViewportState() const noexcept;

    /// Writes current roll pan/zoom/Follow into `timelineClip_` when bound (for session + project save).
    void syncViewportToBoundClip() noexcept;

    /// Re-anchor smoothed playhead to `targetSample` (e.g. after external `Transport::requestSeek`).
    void resetUiPlayheadAnchorToSample(std::int64_t targetSample) noexcept;

    /// I3f: musical snap for timeline editing (combo ids: 1=Off, 2=1/8, 3=1/16, 4=1/32 at current PPQ).
    void setMusicalSnapComboId(int id) noexcept;

    /// Timeline paint only: 1 = compact hits (drums), 2 = duration bars (melodic). Does not change data or playback.
    void setTimelineNotesDisplayComboId(int id) noexcept;

    /// 1 = piano-style row labels (default), 2 = drum names (per-row text).
    void setRowLabelMode(int comboId) noexcept;

    /// Effective label for `midiNote` (used in Drum Names mode). Piano mode ignores this for paint.
    void setRowLabelProvider(std::function<juce::String(int midiNote)> fn) noexcept;

    /// Optional: hover text for drum-name rows (e.g. MIDI note + piano name when the visible label is blank).
    void setRowLabelTooltipProvider(std::function<juce::String(int midiNote)> fn) noexcept;

    /// Commits renamed row; pass empty string to clear user override (reset).
    void setOnCommitRowLabelEdit(std::function<void(int midiNote, juce::String newName)> fn) noexcept;

    [[nodiscard]] int rowLabelMode() const noexcept { return rowLabelMode_; }

    /// I3i: when set, note/step mutations are wrapped for global instrument undo (clip-bound editor only).
    void setUndoablePatternEditHandler(
        std::function<void(const juce::String&, std::function<bool()>)> handler) noexcept;

    [[nodiscard]] std::int64_t getViewportVisibleStartSamples() const noexcept { return visibleStartSamples_; }
    [[nodiscard]] double getViewportSamplesPerPixel() const noexcept { return samplesPerPixel_; }

    /// Note-name / piano key column content width (excludes splitter chrome; zero when fully collapsed).
    [[nodiscard]] int keyboardColumnWidth() const noexcept;
    /// Runtime-only UI preference (expand/collapse + lane width). App-wide persistence is a future follow-up.
    [[nodiscard]] bool isKeyboardLaneCollapsed() const noexcept { return sideStripTotalNow() == 0; }

    /// MIDI editor window (`Body`) owns strip width; the roll only mirrors it for layout/paint.
    void setSideStripTotalWidthForUiOnly(int totalIncludingSplitter) noexcept;

private:
    void timerCallback() override;

    void textEditorReturnKeyPressed(juce::TextEditor&) override;
    void textEditorEscapeKeyPressed(juce::TextEditor&) override;
    void textEditorFocusLost(juce::TextEditor&) override;

    void beginRowLabelInlineEdit(int midiNote);
    void dismissRowLabelEditor(bool commit);

    [[nodiscard]] bool useAbsoluteTimeline() const noexcept;
    void seedViewportFromMainTimelineOrFallback();
    void applyViewportAfterContextBound();
    [[nodiscard]] std::int64_t sampleAtGridX(float localX) const noexcept;
    [[nodiscard]] float xForSessionSample(std::int64_t s) const noexcept;
    [[nodiscard]] int pitchAtY(int y) const;
    [[nodiscard]] int sideStripTotalNow() const noexcept;
    [[nodiscard]] int sideStripContentWidthNow() const noexcept;
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
    void tryAddTimelineNoteAtGridClick(juce::Point<int> pos);

    /// Timeline note under `pos` (topmost if overlapping), using the same geometry as paint: Bars mode =
    /// filled rounded rect; Hits (drum) mode = diamond. Returns nullopt if none.
    [[nodiscard]] std::optional<int> findTimelineNoteIndexAtPoint(juce::Point<int> pos) const;
    /// Visual bounds in component space for marquee intersection (AABB of bar rect or diamond).
    [[nodiscard]] std::optional<juce::Rectangle<float>> getTimelineNoteVisualBounds(int noteIndex) const;
    void normalizeTimelineNoteSelection() noexcept;
    void clearTimelineNoteSelection() noexcept;
    void replaceTimelineNoteSelectionWithSingle(int noteIndex) noexcept;
    void toggleTimelineNoteInSelection(int noteIndex) noexcept;
    [[nodiscard]] bool isTimelineNoteIndexSelected(int noteIndex) const noexcept;
    void adjustTimelineNoteSelectionAfterErase(int erasedIndex) noexcept;
    void applyTimelineMarqueeSelectionFromRect(const juce::Rectangle<int>& r) noexcept;

    enum class TimelineMarqueeInteraction : std::uint8_t
    {
        None,
        Pending,
        Dragging
    };

    /// Ruler strip only (not note grid): mirrors `TimelineRulerView` mouse split + modifiers.
    void handleTimelineRulerMouseDown(const juce::MouseEvent& e, const juce::Rectangle<int>& rulerTrack);
    void handleTimelineRulerMouseDrag(const juce::MouseEvent& e, const juce::Rectangle<int>& rulerTrack);
    void applyRulerSeekAtXInTrack(float xInTrack, float trackWidth) noexcept;
    void applyLeftLocatorRulerX(float xInTrack, float trackWidth) noexcept;
    void applyRightLocatorRulerX(float xInTrack, float trackWidth) noexcept;
    void tryToggleCycleFromRuler() noexcept;
    void syncUiPlayheadAfterRulerSeek(std::int64_t seekTargetSamples) noexcept;
    void maybeFollowViewportToAnchorSample(double anchorSamples) noexcept;

    [[nodiscard]] int countVisiblePitchRows() const noexcept;
    [[nodiscard]] int maxPitchScrollOffsetRows() const noexcept;
    void clampPitchScrollOffset() noexcept;
    [[nodiscard]] int topVisiblePitch() const noexcept;
    [[nodiscard]] std::optional<juce::Rectangle<int>> visibleRowStripRect(const juce::Rectangle<int>& strip,
                                                                          int midiNote) const noexcept;

    enum class RulerGestureMode
    {
        None,
        Seek,
        LeftLocator,
        RightLocator
    };
    RulerGestureMode rulerGestureMode_ = RulerGestureMode::None;
    std::function<bool()> transportGestureBlock_;

    /// Total left strip width (content + splitter), inspector-style; **0** = collapsed. Owned/mirrored from MIDI editor `Body`.
    int currentSideStripTotal_ = kMidiEditorKeyboardLaneWidthPianoDefault + collapsible_side_strip::kSplitterWidth;

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

    /// Last known clip id from `setSessionTimelineContext`; used to detect stale `timelineClip_` without
    /// dereferencing it after controller clip storage rebuild.
    std::uint64_t boundClipIdForSafety_ = 0;

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

    /// Indices into `pattern_.timelineNotes` (not undoable).
    std::unordered_set<int> selectedTimelineNoteIndices_;
    TimelineMarqueeInteraction timelineMarqueeInteraction_ = TimelineMarqueeInteraction::None;
    juce::Point<int> timelineMarqueeAnchor_;
    juce::Rectangle<int> timelineMarqueeRect_;

    std::function<void(const juce::String&, std::function<bool()>)> undoablePatternEditHandler_;

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

    /// Until `readPlayheadSamplesForUi` catches up after `requestSeek`, keep ruler/grid playhead on
    /// the requested sample (audio thread commits seek asynchronously).
    std::optional<std::int64_t> uiRulerSeekDisplayHold_;

    /// Last frame transport playhead was considered “in view” for off-screen repaint skipping.
    bool lastOffscreenGatePlayheadInView_ = true;

    int rowLabelMode_ = 1;
    std::function<juce::String(int)> rowLabelProvider_;
    std::function<juce::String(int)> rowLabelTooltipProvider_;
    std::function<void(int, juce::String)> onCommitRowLabelEdit_;
    std::unique_ptr<juce::TextEditor> rowLabelEditor_;
    int rowLabelEditorPitch_ = -1;

    int pitchLow_ = kDrumPitchLow;
    int pitchHigh_ = kDrumPitchHigh;

    /// Vertical pitch window: row index 0 at the top of the grid maps to `pitchHigh_ - pitchScrollOffsetRows_`.
    int pitchScrollOffsetRows_ = 0;
    /// Fractional pitch rows from high-res / sub-line wheel deltas (applied with `trunc` in `mouseWheelMove`).
    float pitchWheelScrollRemainder_ = 0.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ExperimentalPianoRollView)
};
