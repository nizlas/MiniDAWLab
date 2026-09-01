#pragma once

#include "ui/experimental/ExperimentalMidiChannelDiagnostics.h"
#include "ui/experimental/ExperimentalMidiNoteLengthFormat.h"
#include "ui/experimental/ExperimentalMidiPattern.h"
#include "ui/CoalescedRepaintFlusher.h"
#include "ui/CollapsibleSideStrip.h"
#include "ui/FollowAutoscrollGovernor.h"

#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <set>
#include <unordered_set>
#include <vector>

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

/// Shared note-strength color: velocity 1..127 mapped muted blue (soft) -> violet/muted red (mid)
/// -> bright red (hard). Input is clamped. Used for grid notes/hits and velocity-lane bars so the
/// same value always reads as the same hue.
[[nodiscard]] juce::Colour colourForMidiVelocity(int velocity) noexcept;

/// Drum hits mode: diamonds at step centers; piano-style rows (range is **per-instance**, see `setEditablePitchRange`).
/// I3d1: when bound to an `InstrumentMidiClip` + session/transport, X is **session-absolute samples**
/// with an **independent** zoom/pan (not `TimelineViewportModel`).
class ExperimentalPianoRollView final : public juce::Component,
                                          public juce::SettableTooltipClient,
                                          private juce::Timer,
                                          private juce::TextEditor::Listener,
                                          private juce::ScrollBar::Listener
{
public:
    /// Phase B.1: every row mode exposes the full MIDI range (Cubase octave labels C-2 … G8).
    /// Piano/Drum views are *display* modes, not different legal pitch ranges.
    static constexpr int kFullPitchLow = 0;
    static constexpr int kFullPitchHigh = 127;
    /// Empty editors start centred around middle C (MIDI 60 = C3) instead of an extreme register.
    static constexpr int kDefaultVerticalCenterPitch = 60;
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

    /// Present when the roll is bound to a session clip; 0 when unbound.
    static constexpr int kRulerHeight = 22;

    ExperimentalPianoRollView(ExperimentalMidiPattern& pattern, ExperimentalMidiPatternPlayer* player);

    /// Inclusive MIDI note bounds for visible piano rows (clamped 0–127). Default (and, since
    /// Phase B.1, the only production value): the full 0–127 range.
    void setEditablePitchRange(int lowInclusive, int highInclusive) noexcept;
    /// Initial vertical view: centres on the bound clip's note range when it has notes, else on
    /// `fallbackCenterPitch`. Called once per roll rebuild; a later workspace restore overrides.
    void seedDefaultVerticalScroll(int fallbackCenterPitch) noexcept;
    [[nodiscard]] int pitchLow() const noexcept { return pitchLow_; }
    [[nodiscard]] int pitchHigh() const noexcept { return pitchHigh_; }
    /// Topmost visible MIDI pitch at the grid (row 0); encodes vertical pitch scroll (`pitchScrollOffsetRows_`).
    [[nodiscard]] int topVisibleMidiPitch() const noexcept;
    /// After a rebind that recreates the roll (e.g. musical undo), restore prior vertical pitch scroll; clamps to range.
    void restoreVerticalPitchScrollToPriorTopPitch(int previousTopVisibleMidiPitch) noexcept;

    void paint(juce::Graphics& g) override;
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;
    void mouseUp(const juce::MouseEvent& e) override;
    void mouseDoubleClick(const juce::MouseEvent& e) override;
    void mouseMove(const juce::MouseEvent& e) override;
    void mouseExit(const juce::MouseEvent& e) override;
    void mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override;
    bool keyPressed(const juce::KeyPress& key) override;
    void resized() override;

    /// Bind the roll to a session clip (absolute timeline). Pass nullptrs when unbound.
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

    /// Legacy hook: global snap comes from `Session`; kept so older call sites still compile.
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

    /// Phase B.1 (spec E): key-strip / drum-row audition uses the editor's *current* Vel and Off
    /// toolbar values, supplied by the owner as (noteOnVelocity, noteOffVelocity). Defaults
    /// (100, 64) apply when unset or when the fields are blank.
    void setKeyStripAuditionVelocityProvider(std::function<std::pair<int, int>()> fn) noexcept
    {
        keyStripAuditionVelocityProvider_ = std::move(fn);
    }

    /// Mouse Up for the active audition gesture (also the owner's focus-loss safety hook, after
    /// it has released held previews on the player). Safe when no gesture is active.
    void endActiveAuditionGestureOnMouseUp() noexcept;

    [[nodiscard]] int rowLabelMode() const noexcept { return rowLabelMode_; }

    /// I3i: when set, note/step mutations are wrapped for global instrument undo (clip-bound editor only).
    void setUndoablePatternEditHandler(
        std::function<void(const juce::String&, std::function<bool()>)> handler) noexcept;

    [[nodiscard]] std::int64_t getViewportVisibleStartSamples() const noexcept { return visibleStartSamples_; }
    [[nodiscard]] double getViewportSamplesPerPixel() const noexcept { return samplesPerPixel_; }

    /// Component height that would show exactly `rows` pitch rows: ruler + rows*kRowHeight + the
    /// velocity lane height preference (unclamped best-effort; the lane re-clamps at layout time).
    [[nodiscard]] int preferredComponentHeightForPitchRows(int rows) const noexcept;

    /// Current velocity lane height preference in px (0 = minimized). For project UI persistence.
    [[nodiscard]] int velocityLaneHeightPreference() const noexcept { return velocityLaneHeightPref_; }
    /// Restore velocity lane height from project UI metadata (clamped; 0 = minimized).
    void setVelocityLaneHeightPreference(int heightPx) noexcept;

    /// Note-name / piano key column content width (excludes splitter chrome; zero when fully collapsed).
    [[nodiscard]] int keyboardColumnWidth() const noexcept;
    /// Runtime-only UI preference (expand/collapse + lane width). App-wide persistence is a future follow-up.
    [[nodiscard]] bool isKeyboardLaneCollapsed() const noexcept { return sideStripTotalNow() == 0; }

    /// MIDI editor window (`Body`) owns strip width; the roll only mirrors it for layout/paint.
    void setSideStripTotalWidthForUiOnly(int totalIncludingSplitter) noexcept;

    /// What the MIDI editor's compact Vel / Off toolbar fields should show for the current selection.
    struct SelectedNotesVelocitySummary
    {
        int selectedCount = 0;
        /// Shared note-on velocity; empty when the selection mixes values.
        std::optional<int> velocity;
        /// Shared note-off velocity; empty when the selection mixes values.
        std::optional<int> offVelocity;
    };
    [[nodiscard]] SelectedNotesVelocitySummary summarizeSelectedNotesVelocities() const noexcept;

    /// Set note-on velocity (clamped 1…127) on every selected note as **one** undoable edit.
    /// Timing, pitch, channel, selection and viewport are untouched. False = nothing to change.
    bool applyVelocityToSelectedNotes(int velocity);
    /// Same for note-off velocity (clamped 0…127).
    bool applyOffVelocityToSelectedNotes(int offVelocity);

    /// What the MIDI editor's compact **Len** toolbar field should show for the current selection.
    struct SelectedNotesDurationSummary
    {
        int selectedCount = 0;
        /// Shared exact `durationTicks`; empty when the selection mixes lengths.
        std::optional<std::int64_t> durationTicks;
    };
    [[nodiscard]] SelectedNotesDurationSummary summarizeSelectedNotesDurations() const noexcept;

    /// Grid the **Len** field's `n.p.q.r` text is parsed/formatted against: clip PPQ + project meter.
    [[nodiscard]] midi_note_length::BarGrid noteLengthBarGrid() const noexcept;

    /// Smallest length any editor path may produce right now (snap-aware; see
    /// `minTimelineNoteDurationTicks`). The **Len** field clamps typed values to this so numeric
    /// entry can never undercut what mouse create/resize enforce.
    [[nodiscard]] std::int64_t minimumNoteLengthTicks() const noexcept;

    /// MIDI channel stamped on notes created here: the track's fixed output channel when it has
    /// one, otherwise 10 (what the editor always used, kept for `Any` tracks from pre-v17 projects).
    [[nodiscard]] int channelForNewlyCreatedNotes() const noexcept;

    enum class NoteLengthApplyResult
    {
        NoSelection,   ///< Nothing selected (field should be blank/disabled anyway).
        NoChange,      ///< Every selected note already has the clamped length.
        RejectedOverlap, ///< Batch would overlap on some pitch/channel: nothing changed, no undo step.
        Applied        ///< All selected notes now have the clamped length (one undo step).
    };

    /// Set `durationTicks` on **every** selected note to `requestedTicks` as one undoable edit,
    /// clamped up to `minimumNoteLengthTicks()`. Start, pitch, channel, velocity and off-velocity
    /// are untouched. All-or-nothing: if the resulting batch would overlap an existing note (or
    /// another selected note that did not already overlap it), nothing is written.
    NoteLengthApplyResult applyLengthTicksToSelectedNotes(std::int64_t requestedTicks);

    /// The bound track's MIDI output channel: `kTrackMidiOutputChannelAny` (preserve each note's
    /// stored channel) or a fixed 1…16. `Any` when there is no track/session binding, since an
    /// unbound scratch roll cannot claim a track setting.
    [[nodiscard]] int trackMidiOutputChannel() const noexcept;

    /// Native (stored) channels of the current selection, for the editor's channel readout. Read
    /// from the note data, never from the track selector — telling those apart is the point.
    [[nodiscard]] midi_channel_diag::NativeChannelSummary
    summarizeSelectedNotesNativeChannels() const noexcept;

    /// How much a channel remap is allowed to touch.
    enum class ChannelRemapScope
    {
        SelectedNotes,   ///< Only the current selection, in the open clip.
        AllNotesOnTrack  ///< Every note in every clip of the bound instrument track.
    };

    enum class ChannelRemapResult
    {
        NoFixedTrackChannel, ///< Track is `Any`: no target to write, command should be disabled.
        NothingInScope,      ///< Empty selection / no notes at all: nothing written.
        NoChange,            ///< Every note in scope already stores the target channel.
        RejectedOverlap,     ///< Would collapse two notes onto one pitch/channel: nothing written.
        Applied              ///< Native channels rewritten as one undo step.
    };

    struct ChannelRemapOutcome
    {
        ChannelRemapResult result = ChannelRemapResult::NothingInScope;
        int notesInScope = 0;
        int notesChanged = 0;
        /// The channel that was (or would be) written; 0 when the track has no fixed channel.
        int targetChannel = 0;
    };

    /// **Destructive**: rewrites the *stored* channel of the notes in scope to the track's fixed
    /// output channel, as one undoable edit. Time, duration, pitch, velocity, off-velocity, clip
    /// ownership and the selection are untouched, and the track's output selector is left alone —
    /// this only bakes the data so it survives switching back to `Any (Preserve)`.
    ChannelRemapOutcome remapNativeChannelsToTrackChannel(ChannelRemapScope scope);

    /// Slice E: copy/paste selected `timelineNotes` within the bound clip (internal clipboard only).
    [[nodiscard]] bool handleTimelineNotesCopyShortcut() noexcept;
    [[nodiscard]] bool handleTimelineNotesPasteShortcut();
    /// Delete/Backspace: remove all selected timeline notes as one undoable edit ("Delete MIDI notes").
    /// Returns false (key not consumed) when nothing is selected or the clip binding is unavailable.
    [[nodiscard]] bool handleTimelineNotesDeleteSelectionShortcut();

    /// Delete/Backspace with CC points selected: remove them as one undoable edit ("Delete CC
    /// points"). Checked before the note shortcut so CC selections never delete notes. Returns
    /// false when no CC point is selected.
    [[nodiscard]] bool handleCcPointsDeleteSelectionShortcut();

private:
    void timerCallback() override;

    void scrollBarMoved(juce::ScrollBar* bar, double newRangeStart) override;

    void textEditorReturnKeyPressed(juce::TextEditor&) override;
    void textEditorEscapeKeyPressed(juce::TextEditor&) override;
    void textEditorFocusLost(juce::TextEditor&) override;

    void beginRowLabelInlineEdit(int midiNote);
    void dismissRowLabelEditor(bool commit);

    /// Right-click exact-velocity popup: small numeric editor next to the clicked note. Targets the
    /// whole selection when the clicked note is selected, else just that note (selection untouched).
    void beginVelocityValueEdit(int noteIndex, juce::Point<int> anchorPos);
    /// commit=true applies the typed value (clamped 1..127) as one undoable edit and auditions the
    /// targets (chord only when they share one startTick). commit=false cancels without changes.
    void dismissVelocityValueEditor(bool commit);

    /// Shared implementation of the two batch velocity setters (`offField` picks which byte).
    bool applyVelocityFieldToSelectedNotes(bool offField, int value);

    [[nodiscard]] bool useAbsoluteTimeline() const noexcept;
    void seedViewportFromMainTimelineOrFallback();
    void applyViewportAfterContextBound();
    [[nodiscard]] std::int64_t sampleAtGridX(float localX) const noexcept;
    [[nodiscard]] float xForSessionSample(std::int64_t s) const noexcept;
    [[nodiscard]] int pitchAtY(int y) const;
    [[nodiscard]] int sideStripTotalNow() const noexcept;
    [[nodiscard]] int sideStripContentWidthNow() const noexcept;
    [[nodiscard]] int timelineRulerHeight() const noexcept;
    [[nodiscard]] juce::Rectangle<int> rulerCornerBounds() const;
    [[nodiscard]] juce::Rectangle<int> rulerTrackBounds() const;
    [[nodiscard]] juce::Rectangle<int> keyboardBounds() const;
    [[nodiscard]] juce::Rectangle<int> gridBounds() const;
    [[nodiscard]] std::int64_t visibleEndSamples() const noexcept;
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

    /// Timeline marquee (DAW-style rectangle select): component-local coordinates, clipped to `gridBounds()`.
    void beginMarqueeSelection(juce::Point<int> localPos);
    void updateMarqueeSelection(const juce::MouseEvent& e);
    void finishMarqueeSelection();
    [[nodiscard]] juce::Rectangle<int> getNormalizedMarqueeRect() const noexcept;
    void selectTimelineNotesIntersecting(const juce::Rectangle<int>& r) noexcept;

    enum class TimelineNoteResizeEdge : std::uint8_t
    {
        Left,
        Right
    };

    /// Bars-display note resize is available in both row modes (Piano and Drum Names): drum notes share
    /// the same TimelineMidiNote/durationTicks model, and stretching them is a visual aid for drums.
    [[nodiscard]] bool timelineBarsResizeEnabled() const noexcept;
    [[nodiscard]] std::optional<std::pair<int, TimelineNoteResizeEdge>> findTimelineBarResizeEdgeAtPoint(
        juce::Point<int> pos) const;
    [[nodiscard]] std::int64_t snapTimelineTickForEdit(std::int64_t tick) const noexcept;
    /// Create-on-click variant: floor to the snap cell's start (the cell visually under the
    /// pointer) instead of nearest grid line. Move/resize/paste keep `snapTimelineTickForEdit`.
    [[nodiscard]] std::int64_t snapTimelineTickForCreate(std::int64_t tick) const noexcept;
    [[nodiscard]] std::int64_t minTimelineNoteDurationTicks() const noexcept;
    void beginTimelineNoteResizeGesture(int noteIndex, TimelineNoteResizeEdge edge);
    void updateTimelineNoteResizeGesture(juce::Point<int> localPos);
    void finishTimelineNoteResizeGesture();

    struct TimelineNoteMoveCapture
    {
        int index = -1;
        TimelineMidiNote original {};
    };
    using TimelineNoteResizeCapture = TimelineNoteMoveCapture;
    void beginTimelineNoteMoveGesture(const juce::MouseEvent& e);
    void updateTimelineNoteMoveGesture(juce::Point<int> localPos);
    void finishTimelineNoteMoveGesture();
    void clearTimelineNoteMovePending() noexcept;

    [[nodiscard]] bool currentEditCandidatesOverlap(
        const std::vector<int>& ignoreIndices,
        const std::vector<TimelineMidiNote>& candidates,
        const std::vector<TimelineMidiNote>* grandfatherOriginals) const noexcept;
    void flashForbiddenNoDropCursor();
    void updateTimelineNoteEditCursor();

    // --- Velocity controller lane (bottom strip; edits TimelineMidiNote velocity only) ---
    /// Default lane height; also the restore height when the minimized knob is clicked.
    static constexpr int kVelocityLaneHeight = 96;
    static constexpr int kVelocityBarWidthPx = 5;
    static constexpr int kVelocityBarHitToleranceX = 4;
    /// Top strip of the lane that acts as a vertical resize grab band.
    static constexpr int kVelocityLaneResizeBandPx = 6;
    /// Below this the lane is unusably small: resize snaps to fully minimized (0).
    static constexpr int kVelocityLaneMinUsableHeight = 24;
    static constexpr int kVelocityLaneKnobWidth = 48;
    static constexpr int kVelocityLaneKnobHeight = 7;

    /// Bar area right of the side strip (empty when minimized or the component is too short).
    [[nodiscard]] juce::Rectangle<int> velocityLaneBounds() const;
    /// Bottom-left corner under the keyboard strip ("Velocity" label chrome).
    [[nodiscard]] juce::Rectangle<int> velocityLaneHeaderBounds() const;
    /// Lane minus vertical padding: bar height 0..127 maps into this rect.
    [[nodiscard]] juce::Rectangle<int> velocityLaneInnerBounds() const;
    /// Effective lane height: user preference clamped so a few pitch rows always stay visible.
    [[nodiscard]] int velocityLaneTotalHeight() const noexcept;
    /// Upper clamp for the lane: min(space keeping 3 pitch rows, ~50% of component height).
    [[nodiscard]] int maxVelocityLaneHeightNow() const noexcept;
    /// Full-width resize grab band at the lane's top edge (empty when minimized).
    [[nodiscard]] juce::Rectangle<int> velocityLaneResizeBandBounds() const;
    /// Small centered handle at the bottom edge when minimized (click restores, drag reopens).
    [[nodiscard]] juce::Rectangle<int> velocityLaneCollapsedKnobBounds() const;
    /// Lane bars/editing need a bound session clip.
    [[nodiscard]] bool velocityLaneEditingAvailable() const noexcept;
    [[nodiscard]] int velocityFromLaneY(int y) const noexcept;
    /// Bar onset X for eligible note (in pitch range + inside clip span); nullopt when not drawable.
    [[nodiscard]] std::optional<float> velocityBarCentreXForNoteIndex(int noteIndex) const;
    /// Nearest bar within `kVelocityBarHitToleranceX`; ties at the same x prefer the higher velocity
    /// (its top is what the cursor grabs). `selectedOnly` restricts to the current selection.
    [[nodiscard]] std::optional<int> findVelocityBarIndexNearX(int x, bool selectedOnly) const;
    void handleVelocityLaneMouseDown(const juce::MouseEvent& e);
    void updateVelocityLaneDrag(juce::Point<int> localPos);
    void finishVelocityLaneDragGesture();
    void paintVelocityLane(juce::Graphics& g);

    // --- MIDI CC automation lane (Stage D; below the velocity lane, edits pattern ccPoints only) ---
    /// Default expanded height when the collapsed knob is clicked.
    static constexpr int kCcLaneHeight = 110;
    static constexpr int kCcLaneResizeBandPx = 6;
    static constexpr int kCcLaneMinUsableHeight = 24;
    static constexpr int kCcPointHitRadiusPx = 6;
    static constexpr int kCcLaneKnobWidth = 48;
    static constexpr int kCcLaneKnobHeight = 7;

    /// Effective CC lane height (0 = collapsed; clamped like the velocity lane).
    [[nodiscard]] int ccLaneTotalHeight() const noexcept;
    [[nodiscard]] int maxCcLaneHeightNow() const noexcept;
    /// Bottom strip right of the side strip (below the velocity lane).
    [[nodiscard]] juce::Rectangle<int> ccLaneBounds() const;
    /// Bottom-left corner under the keyboard strip (controller selector chrome).
    [[nodiscard]] juce::Rectangle<int> ccLaneHeaderBounds() const;
    /// Lane minus vertical padding: CC value 0..127 maps into this rect.
    [[nodiscard]] juce::Rectangle<int> ccLaneInnerBounds() const;
    [[nodiscard]] juce::Rectangle<int> ccLaneResizeBandBounds() const;
    /// Small labeled handle at the bottom edge while collapsed (discoverability).
    [[nodiscard]] juce::Rectangle<int> ccLaneCollapsedKnobBounds() const;
    [[nodiscard]] bool ccLaneEditingAvailable() const noexcept;
    [[nodiscard]] int ccValueFromLaneY(int y) const noexcept;
    [[nodiscard]] float ccLaneYForValue(int value) const noexcept;
    /// Lane X of a CC point (index into `pattern_.ccPoints`); nullopt when not of the shown
    /// controller or not mappable.
    [[nodiscard]] std::optional<float> ccPointXForIndex(int idx) const;
    [[nodiscard]] std::optional<int> findCcPointIndexNear(juce::Point<int> pos) const;
    void handleCcLaneMouseDown(const juce::MouseEvent& e);
    void updateCcLaneDrag(juce::Point<int> localPos);
    void finishCcLaneDragGesture();
    void paintCcLane(juce::Graphics& g);
    void showCcControllerMenu();
    void showCcPointContextMenu(int pointIndex);
    /// Left-click on empty lane: insert a point at the snapped tick/value (undoable). New points
    /// take the same native channel rule as new notes (`channelForNewNotes(track output)`).
    void insertCcPointAt(juce::Point<int> pos);
    void deleteSelectedCcPoints();
    void normalizeCcSelection() noexcept;

    struct CcDragCapture
    {
        int index = -1;
        MidiCcPoint original;
    };
    /// Runtime-only lane height (0 = collapsed default; existing projects are not forced to show it).
    int ccLaneHeightPref_ = 0;
    /// Controller shown/edited in the lane. CC11 Expression is the default first view.
    int ccLaneController_ = 11;
    std::set<int> selectedCcPointIndices_;
    bool ccLaneResizeActive_ = false;
    bool ccLaneResizeFromCollapsedKnob_ = false;
    int ccLaneResizeAnchorY_ = 0;
    int ccLaneResizeAnchorHeight_ = 0;
    bool ccPointDragActive_ = false;
    bool ccDragMoved_ = false;
    std::vector<CcDragCapture> ccDragCaptures_;
    int ccDragPrimaryIndex_ = -1;
    std::int64_t ccDragAnchorTick_ = 0;
    int ccDragAnchorValue_ = 0;

    struct VelocityDragCapture
    {
        int index = -1;
        int originalVelocity = 100;
    };
    bool velocityLaneDragActive_ = false;
    /// Single capture -> absolute edit (bar top tracks cursor); multiple -> same delta applied to all.
    std::vector<VelocityDragCapture> velocityDragCaptures_;
    int velocityDragPrimaryIndex_ = -1;
    /// `velocityFromLaneY` at mouseDown; delta edits are relative to this so grabbing a bar never jumps.
    int velocityDragAnchorVelocity_ = 0;

    /// Runtime-only lane height preference (0 = minimized). Not persisted; resets when the roll is rebuilt.
    int velocityLaneHeightPref_ = kVelocityLaneHeight;
    bool velocityLaneResizeActive_ = false;
    /// Gesture started on the minimized knob: a click (no real drag) restores the default height.
    bool velocityLaneResizeFromCollapsedKnob_ = false;
    int velocityLaneResizeAnchorY_ = 0;
    int velocityLaneResizeAnchorHeight_ = 0;

    // --- Audition (gesture previews through the pattern player; no transport/timeline side effects) ---
    /// Min spacing between velocity-drag re-auditions (Cubase-style "rattle" without event spam).
    static constexpr double kVelocityDragAuditionThrottleMs = 45.0;

    /// Effective audition channel for an arranged note: the note's native channel under
    /// `Any (Preserve)`, the track's fixed output channel otherwise (Phase B.1 spec C/D).
    [[nodiscard]] int effectiveAuditionChannelForNote(const TimelineMidiNote& tn) const noexcept;
    /// Arranged-note Mouse Down: Note On now; ≥ default audition duration, hold extends (spec D).
    void beginArrangedNoteAuditionGesture(const TimelineMidiNote& tn) noexcept;
    /// Piano-key / drum-row Mouse Down: exact Note On; Note Off exactly at Mouse Up (spec E).
    void beginKeyStripAuditionGesture(int midiNote) noexcept;
    /// Create-note feedback: one-shot preview with the default arranged duration.
    void oneShotAuditionForCreatedNote(const TimelineMidiNote& tn) noexcept;
    /// Velocity-drag audition: primary note alone, or the whole capture set as a chord when all
    /// captured notes share one startTick. Throttled + velocity-change gated unless `force`.
    void maybeAuditionVelocityDrag(bool force) noexcept;

    /// (channel, pitch) key of the preview begun by the current mouse gesture; released on Mouse Up.
    std::optional<std::pair<int, int>> activeAuditionGestureKey_;
    /// True when the active gesture is a key-strip (exact Mouse Down/Up) preview.
    bool activeAuditionGestureIsKeyStrip_ = false;

    /// All captured notes share one startTick (single note trivially qualifies). Selections spanning
    /// several start times get no drag audition (no meaningful timing reference).
    bool velocityDragAuditionSameStart_ = false;
    double velocityDragLastAuditionMs_ = 0.0;
    int velocityDragLastAuditionVelocity_ = -1;
    std::function<std::pair<int, int>()> keyStripAuditionVelocityProvider_;

    /// Middle-button drag = horizontal hand-pan (grab-style: content follows the mouse).
    bool middlePanActive_ = false;
    float middlePanLastX_ = 0.0f;

    struct InternalTimelineClipboardItem
    {
        std::int64_t deltaStartTicks = 0;
        int midiNote = 60;
        int velocity = 100;
        int offVelocity = kDefaultMidiNoteOffVelocity;
        std::uint8_t channel = 1;
        std::int64_t durationTicks = 240;
    };
    std::vector<InternalTimelineClipboardItem> timelineInternalClipboard_;
    /// Earliest `startTick` among notes last copied (for paste fallback anchor).
    std::int64_t timelineClipboardSourceMinStartTick_ = 0;

    [[nodiscard]] std::int64_t computeTimelinePasteAnchorTick() const;
    void sortTimelineNotesForEditing() noexcept;
    void replaceTimelineSelectionWithNotesMatching(const std::vector<TimelineMidiNote>& matches) noexcept;

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

    /// Right-side vertical scrollbar strip (between grid and component right edge).
    static constexpr int kPitchScrollbarWidthPx = 12;

    [[nodiscard]] int countVisiblePitchRows() const noexcept;
    [[nodiscard]] int maxPitchScrollOffsetRows() const noexcept;
    void clampPitchScrollOffset() noexcept;
    /// Push `pitchScrollOffsetRows_` + visible row count into the right-side scrollbar (no notification).
    void syncPitchScrollbarFromState();
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

    /// The **one** display position every current-time indicator in this window paints from (ruler
    /// stroke and long grid line), clamped to the arrangement extent. Smoothed, not block-quantized.
    [[nodiscard]] double currentPlayheadDisplaySampleForPaint() const noexcept;
    /// Playhead-only frame: invalidate just the previous and new playhead columns (ruler + grid
    /// rows) instead of the whole roll, so notes/grid/velocity lane are not re-rasterized at 60 Hz.
    void repaintPlayheadColumnsOnly(float newCentreX);
    /// Playhead column last committed by `paint` (NaN = unknown -> next move repaints in full).
    float lastPaintedPlayheadCentreX_ = std::numeric_limits<float>::quiet_NaN();


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
    bool followPlayhead_ = true;

    /// Follow hardening (same policy as the main arrangement, independent instance): follow is
    /// page/event-driven — a page fires only on a comfort-band boundary crossing, re-arms only
    /// once the playhead was seen inside the band again (sparse re-arm interval otherwise),
    /// is capacity-gated (late-frame + clean-frame-after-page), and yields after user wheel/drag
    /// viewport gestures. Cross-window budget/yield goes through `GlobalFollowWorkCoordinator`
    /// at the call sites. Each page full-repaints the roll, so page admission — not repaint
    /// mechanics — bounds message-thread load.
    FollowAutoscrollGovernor followGovernor_;
    /// True when follow pages are worth doing at all: component showing and window not minimised.
    [[nodiscard]] bool followUiWorthUpdating() const noexcept;
    /// Record a user viewport gesture (wheel zoom/pan, middle-drag, pitch scroll) locally in the
    /// governor *and* globally so main-window follow yields to interaction here, and vice versa.
    void noteUserRollViewportGesture() noexcept;
    /// Repaint-storm fix: user viewport gestures mark the roll dirty once per message batch
    /// instead of once per wheel/drag event (see `CoalescedRepaintFlusher`).
    CoalescedRepaintFlusher rollViewportRepaintFlush_ { [this] { repaint(); } };
    // Follow diagnostics (reset when logged; wraparound harmless with the diag flag off).
    unsigned int statsFollowPans_ = 0;
    unsigned int statsFollowSkipsGesture_ = 0;
    unsigned int statsFollowSkipsLateFrame_ = 0;
    unsigned int statsFollowSkipsAwaitClean_ = 0;
    unsigned int statsFollowSkipsBoundary_ = 0;
    unsigned int statsFollowSkipsPacing_ = 0;
    unsigned int statsFollowSkipsCrossWindow_ = 0;
    unsigned int statsFollowSkipsGlobalBudget_ = 0;
    unsigned int statsFollowSkipsHidden_ = 0;
    double lastFollowDiagLogMs_ = 0.0;
    void maybeLogFollowDiagnostics(double nowMs, bool transportPlaying) noexcept;

    /// Invalidate when rebinding so the next `timerCallback` repaints (locators/cycle/playhead).
    bool sessionTransportSnapshotValid_ = false;
    std::int64_t lastObservedPlayheadUi_ = 0;
    std::int64_t lastObservedLocLUi_ = 0;
    std::int64_t lastObservedLocRUi_ = 0;
    bool lastObservedCycleUi_ = false;

    bool clipGeometrySnapshotValid_ = false;
    std::int64_t lastObservedClipStartSamplesUi_ = 0;
    std::int64_t lastObservedClipLengthSamplesUi_ = 0;

    int timelineNotesDisplayComboId_ = 1;
    int lastObservedTimelineNoteCountUi_ = -1;

    /// Indices into `pattern_.timelineNotes` (not undoable).
    std::unordered_set<int> selectedTimelineNoteIndices_;
    TimelineMarqueeInteraction timelineMarqueeInteraction_ = TimelineMarqueeInteraction::None;
    juce::Point<int> timelineMarqueeAnchor_;
    juce::Rectangle<int> timelineMarqueeRect_;

    bool timelineNoteResizeActive_ = false;
    TimelineNoteResizeEdge timelineResizeEdge_{};
    int timelineResizeNoteIndex_ = -1;
    std::int64_t timelineResizeOriginalStartTick_ = 0;
    std::int64_t timelineResizeOriginalDurationTicks_ = 0;
    /// Inclusive end tick in the model: `startTick + durationTicks` at gesture start (left-edge resize).
    std::int64_t timelineResizeAnchorEndTick_ = 0;
    std::vector<TimelineNoteResizeCapture> timelineResizeCaptures_;
    bool timelineResizeInvalid_ = false;
    bool timelineMoveInvalid_ = false;
    double forbiddenCursorFlashUntilMs_ = 0.0;

    bool timelineMovePending_ = false;
    int timelineMovePrimaryIndex_ = -1;
    bool timelineNoteMoveActive_ = false;
    std::vector<TimelineNoteMoveCapture> timelineMoveCaptures_;
    std::int64_t timelineMoveAnchorAbsSample_ = 0;
    int timelineMoveAnchorPitch_ = 0;
    std::int64_t timelineMovePrimaryOrigStartTick_ = 0;

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

    /// Right-click exact-velocity popup (nullptr = closed). Targets are captured when it opens.
    std::unique_ptr<juce::TextEditor> velocityValueEditor_;
    std::vector<int> velocityEditorTargetIndices_;

    int pitchLow_ = kFullPitchLow;
    int pitchHigh_ = kFullPitchHigh;

    /// Vertical pitch window: row index 0 at the top of the grid maps to `pitchHigh_ - pitchScrollOffsetRows_`.
    int pitchScrollOffsetRows_ = 0;
    /// Fractional pitch rows from high-res / sub-line wheel deltas (applied with `trunc` in `mouseWheelMove`).
    float pitchWheelScrollRemainder_ = 0.0f;

    /// Right-side vertical scrollbar; range unit = pitch rows (0 = topmost). Kept in sync via
    /// `syncPitchScrollbarFromState`; user drags land in `scrollBarMoved`.
    juce::ScrollBar pitchScrollbar_ { true };
    /// Guards against feedback while pushing state into the scrollbar programmatically.
    bool pitchScrollbarSyncing_ = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ExperimentalPianoRollView)
};
