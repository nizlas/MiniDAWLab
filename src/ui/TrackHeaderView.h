#pragma once

// =============================================================================
// TrackHeaderView — shared track header chrome (audio, instrument, MIDI, group, Stereo Out)
// =============================================================================
// Layout (left → right, one common structure for every row kind, compact-header slice 2026-10-07):
//   [group margin][colour segment: type icon + order number][Power][Mute][Solo][name …]
// The title row (22 px, top-aligned at a 2 px pad) is identical at every height, so even Micro
// rows show icon, number, Power / Mute / Solo and the name. Monitor, Record, the instrument
// editor and the instrument-alternatives button sit on a SECOND row below the title row and
// appear only when the height allows it (hidden = empty bounds: no paint, no hit area, no tooltip;
// hiding never changes a state). State comes from `TrackHeaderModelProvider`; actions from
// `TrackHeaderCallbacks`. Optional `TrackHeaderDragHost` + non-`kInvalidTrackId` `dragTrackId`
// enable header-drag reorder. Geometry constants that define the row-height grid live in
// `ui/TrackRowHeightPresets.h`; this header `static_assert`s against them.
// =============================================================================

#include "domain/Track.h"
#include "domain/TrackColour.h"
#include "ui/TrackRowHeightPresets.h"
#include "ui/TrackStripButtonGlyphs.h"

#include <functional>
#include <optional>
#include <vector>
#include <juce_gui_basics/juce_gui_basics.h>

class TrackHeaderView;

/// Horizontal header geometry used in constant expressions BEFORE `TrackHeaderView` is complete
/// (the class forwards to these; see the class constants for the documentation).
namespace track_header_geometry
{
inline constexpr int kGroupMarginPx = 8;
inline constexpr int kSegmentPadPx = 3;
inline constexpr int kTypeIconPx = 14;
inline constexpr int kDigitAdvancePx = 7;
inline constexpr int kMinNumberDigits = 3;
inline constexpr int kTitleCellCount = 3;
inline constexpr int kSegmentToStripGapPx = 3;
inline constexpr int kStripToNameGapPx = 4;
inline constexpr int kOuterPadXPx = 8;
inline constexpr int kNameMinWidthPx = 48;

[[nodiscard]] constexpr int colourSegmentWidthPxForDigits(const int digits) noexcept
{
    const int d = digits < kMinNumberDigits ? kMinNumberDigits : digits;
    return kSegmentPadPx + kTypeIconPx + kSegmentPadPx + d * kDigitAdvancePx + kSegmentPadPx;
}

inline constexpr int kMinimumHeaderColumnWidthPx
    = kGroupMarginPx + colourSegmentWidthPxForDigits(kMinNumberDigits) + kSegmentToStripGapPx
      + kTitleCellCount * track_row_heights::kHeaderControlCellPx + kStripToNameGapPx + kNameMinWidthPx + kOuterPadXPx;
inline constexpr int kDefaultHeaderColumnWidthPx = kMinimumHeaderColumnWidthPx + 19;
} // namespace track_header_geometry

/// Optional per-track VST3 actions from the header context menu (`Main` / `TrackLanesView`).
struct TrackHeaderPluginHost
{
    std::function<void(TrackId)> loadVst3;
    std::function<void(TrackId)> openPluginEditor;
    std::function<void(TrackId)> openPluginParams;
    std::function<void(TrackId)> removePlugin;
};

/// [Message thread] `TrackLanesView` implements these; Began/Ended pair with move updates.
struct TrackHeaderDragHost
{
    std::function<void(TrackId, TrackHeaderView*)> onHeaderDragBegan;
    std::function<void(TrackId, juce::Point<int>)> onHeaderDragMoved;
    std::function<void(TrackId)> onHeaderDragEnded;
};

struct TrackHeaderModel
{
    juce::String name;
    /// Kept for model compatibility; the shared layout shows the name only (no subtitle line).
    juce::String subtitle;
    bool active = false;
    bool armed = false;
    bool muted = false;
    /// When true, power glyph reads as “track off” / standby (same as audio `Track::isTrackOff()`).
    bool off = false;
    bool powerInteractable = true;
    bool muteInteractable = true;
    bool armInteractable = true;
    /// Solo cell (S, right of Mute). All row kinds except Master/Stereo Out set this true; the
    /// cell is also omitted when `callbacks.onToggleSolo` is unset (incremental wiring safety).
    bool soloAvailable = false;
    /// Explicit membership in the CURRENT solo set (red S). Derived pass-through is never red.
    bool soloed = false;
    /// Row is silenced BY SOLO while not stored-muted: the M face uses the distinct dimmed tint.
    bool soloSilenced = false;
    /// Solo active anywhere ⇒ Mute edits are locked (command path refuses). The M cell shows the
    /// effective state with a small lock instead of the generic disabled face, and ignores clicks.
    bool muteLockedBySolo = false;
    /// When false, Power and Arm cells are omitted (no greyed placeholders). Used for
    /// `TrackKind::Master` / Stereo Out and `TrackKind::Group` — bus rows use mute-only chrome.
    bool showRecordAndPowerStripCells = true;
    /// When false or `callbacks.onOpenInstrumentEditor` unset, the second row omits the
    /// instrument-editor cell (audio rows).
    bool instrumentEditorAvailable = false;
    /// Input-monitoring cell (speaker glyph, second row). Audio rows: live audio input monitoring.
    /// Instrument and Midi rows: live MIDI monitoring. Group / master rows: unavailable — no cell,
    /// hit target, or tooltip.
    bool monitorAvailable = false;
    /// Speaker lights orange while live input monitoring is on (runtime state, not persisted).
    bool monitorEnabled = false;
    /// False = disabled placeholder look + inert clicks (no tooltip).
    bool monitorInteractable = false;
    /// P2: when true and `callbacks.onShowInstrumentAlternatives` set, the "Instrument
    /// alternatives" cell appears at the end of the second row (instrument destination rows only;
    /// opens the anchored popup). Hidden with the whole second row at compact heights.
    bool instrumentAlternativesAvailable = false;
    /// When false, double-click inline rename is disabled (`TrackKind::Master` / Stereo Out).
    bool trackNameRenameEnabled = true;
    /// Live MIDI arrived for this row within the last ~150 ms: a small green dot is painted at
    /// the right end of the name row (discreet activity indicator, never a dialog).
    bool midiActivity = false;
    /// Header MULTI-selection membership (visual-group creation): a subtle blue wash + hairline
    /// over the header plate. Independent of `active` — the active-track stripe keeps its own
    /// look and the Inspector keeps following the single active track.
    bool headerMultiSelected = false;
    /// Visual track group membership: a discreet vertical marker in the left group margin,
    /// drawn right of the 4 px active stripe so neither it nor any button is obscured.
    bool visualGroupMember = false;
    /// Type icon shown in the colour segment (first), then the order number.
    track_strip_glyphs::TrackTypeIcon typeIcon = track_strip_glyphs::TrackTypeIcon::Audio;
    /// 1-based arrangement order number (0 = none). Derived live from the session order by the
    /// provider: insert / duplicate / delete / reorder renumber; collapsed members still count.
    int trackNumber = 0;
    /// Shared digit column width for every header (>= 3; grows with the track count).
    int trackNumberDigits = 3;
    /// The track's palette colour (segment background; events derive their fill from it too).
    TrackColourKey colourKey = TrackColourKey::DefaultGrey;
};

using TrackHeaderModelProvider = std::function<TrackHeaderModel()>;

struct TrackHeaderCallbacks
{
    /// Left-click on the name / segment / drag surface (not on a control cell). Null = no-op.
    std::function<void()> onActivateName;
    /// Header multi-selection click, dispatched with `onActivateName` from the same press:
    /// `shiftRange` = shift held (select the contiguous range from the selection anchor);
    /// otherwise the click selects exactly this header and moves the anchor. Null = no multi-
    /// selection (clip selection, height drag, and reorder behave exactly as before).
    std::function<void(bool shiftRange)> onHeaderSelectionClick;
    /// Return true if the click was handled (blocks promoting to header-drag); false = ignored.
    std::function<bool()> onTogglePower;
    std::function<void()> onToggleMute;
    /// Solo toggle (S cell). Omit (or leave `soloAvailable` false) to hide the cell (Master row).
    std::function<void()> onToggleSolo;
    std::function<void()> onToggleArm;
    /// Input-monitoring toggle (cell omitted when unset or `monitorAvailable` false).
    std::function<void()> onToggleMonitor;
    /// Optional: opens native instrument / plugin UI (Groove Agent row). Omit for audio lanes.
    std::function<void()> onOpenInstrumentEditor;
    /// P2: opens the "Instrument alternatives" popup anchored at the given SCREEN bounds (the
    /// clicked cell). Omit for non-instrument rows.
    std::function<void(juce::Rectangle<int> screenAnchorBounds)> onShowInstrumentAlternatives;
    /// Right-click outside the colour segment; null = ignore context menu entirely.
    std::function<void(TrackHeaderView&, const juce::MouseEvent&)> onShowContextMenu;
    /// Right-click ON the type-icon / number segment: the track colour palette, anchored at the
    /// segment's SCREEN bounds. Null = the generic context menu is shown there instead.
    std::function<void(TrackHeaderView&, juce::Rectangle<int> segmentScreenBounds)> onShowColourMenu;
    /// Bottom-edge row height drag; `startHeightPx` is header height at mouse-down (session thread).
    std::function<void(int startHeightPx, int deltaScreenYPx)> onRowHeightDrag;
    std::function<void()> onRowHeightDragEnd;
    /// Double-click inline rename; null `onCommitRenameTrack` disables rename affordance.
    std::function<bool()> canBeginRenameTrack;
    std::function<bool(juce::String trimmedNewName)> onCommitRenameTrack;
};

class TrackHeaderView : public juce::Component,
                        public juce::TooltipClient,
                        private juce::TextEditor::Listener
{
public:
    // ---------------------------------------------------------------- horizontal structure (px)
    /// Square control cell (= the title-row height, `track_row_heights::kHeaderControlCellPx`).
    static constexpr int kStripControlCellWidthPx = track_row_heights::kHeaderControlCellPx;
    static constexpr int kStripSquareBodyInsetPx = 1;
    /// Left group margin: the 4 px active-row stripe + the 2 px visual-group marker + 2 px gap.
    /// All content starts right of it (identical for active and inactive rows).
    static constexpr int kHeaderActiveStripeWidthPx = 4;
    static constexpr int kHeaderGroupMarkerXPx = kHeaderActiveStripeWidthPx + 1;
    static constexpr int kHeaderGroupMarkerWidthPx = 2;
    static constexpr int kHeaderGroupMarginPx = track_header_geometry::kGroupMarginPx;
    /// Colour segment: [pad][type icon][pad][digits][pad]; the digit column is shared by all
    /// headers (`trackNumberDigits`, at least 3) so numbers never clip and columns stay aligned.
    static constexpr int kHeaderSegmentPadPx = track_header_geometry::kSegmentPadPx;
    static constexpr int kHeaderTypeIconPx = track_header_geometry::kTypeIconPx;
    static constexpr int kHeaderDigitAdvancePx = track_header_geometry::kDigitAdvancePx;
    static constexpr int kHeaderMinNumberDigits = track_header_geometry::kMinNumberDigits;
    /// Title-row control slots reserved after the segment ([Power][Mute][Solo]; kinds without
    /// Power / Solo collapse left, the name keeps the same x on every row kind).
    static constexpr int kHeaderTitleCellCount = track_header_geometry::kTitleCellCount;
    static constexpr int kHeaderSegmentToStripGapPx = track_header_geometry::kSegmentToStripGapPx;
    static constexpr int kHeaderStripToNameGapPx = track_header_geometry::kStripToNameGapPx;
    /// Right padding of the name and the minimum name room the column width must leave.
    static constexpr int kHeaderOuterPadXPx = track_header_geometry::kOuterPadXPx;
    static constexpr int kHeaderNameMinWidthPx = track_header_geometry::kNameMinWidthPx;

    [[nodiscard]] static constexpr int colourSegmentWidthPxForDigits(const int digits) noexcept
    {
        return track_header_geometry::colourSegmentWidthPxForDigits(digits);
    }
    /// Minimum header-column width (3-digit column): margin 8 + segment 44 + gap 3 + three title
    /// cells 66 + gap 4 + name 48 + pad 8 = 181 logical px. Saved narrower preferences clamp UP
    /// on load / display (`TrackLanesView::clampHeaderColumnWidthForTotalWidth`); wider saved
    /// widths are preserved.
    static constexpr int kMinimumHeaderColumnWidthPx = track_header_geometry::kMinimumHeaderColumnWidthPx;
    /// Default column width: the minimum plus 19 px more name room (200 px).
    static constexpr int kDefaultHeaderColumnWidthPx = track_header_geometry::kDefaultHeaderColumnWidthPx;
    static_assert(kMinimumHeaderColumnWidthPx == 181 && kDefaultHeaderColumnWidthPx == 200,
                  "header column limits: documented values");

    // ------------------------------------------------------------------ vertical structure (px)
    static constexpr int kHeaderRowTopPadPx = track_row_heights::kHeaderRowTopPadPx;
    static constexpr int kHeaderRowGapPx = track_row_heights::kHeaderRowGapPx;
    /// Bottom-edge resize band inside the header (matches layout hit-testing).
    static constexpr int kHeaderResizeBandPx = track_row_heights::kHeaderResizeBandPx;
    [[nodiscard]] static constexpr int resizeBandPx() noexcept { return kHeaderResizeBandPx; }
    /// The second control row fits when the header is at least this tall (Small and up).
    static constexpr int kMinimumHeightForSecondRowPx
        = kHeaderRowTopPadPx + kStripControlCellWidthPx + kHeaderRowGapPx + kStripControlCellWidthPx
          + kHeaderResizeBandPx;
    static_assert(kMinimumHeightForSecondRowPx <= track_row_heights::kSmallPresetPx
                      && kMinimumHeightForSecondRowPx > track_row_heights::kMiniPresetPx,
                  "second control row: present from Small, absent at Mini / Micro");

    /// `dragTrackId` is forwarded to `TrackHeaderDragHost`. Use `kInvalidTrackId` when there is no
    /// reorder drag initially (e.g. until the owning view calls `setHeaderReorderDrag`).
    TrackHeaderView(TrackHeaderModelProvider modelProvider,
                    TrackHeaderCallbacks callbacks,
                    TrackId dragTrackId,
                    std::optional<TrackHeaderDragHost> dragHost) noexcept;

    void paint(juce::Graphics& g) override;
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;
    void mouseUp(const juce::MouseEvent& e) override;
    void mouseDoubleClick(const juce::MouseEvent& e) override;
    void mouseMove(const juce::MouseEvent& e) override;
    void mouseExit(const juce::MouseEvent& e) override;
    void resized() override;

    [[nodiscard]] TrackId getBoundTrackId() const noexcept { return dragTrackId_; }
    [[nodiscard]] TrackId getTrackId() const noexcept { return dragTrackId_; }

    void setSourceForbiddenForHeaderDrag() noexcept;
    void restoreSourceCursorAfterHeaderDrag() noexcept;

    /// [Message thread] Non-audio rows (e.g. experimental instrument shell) can attach header reorder
    /// drag after construction. `dragTrackIdForwarded` must not be `kInvalidTrackId` when `host` set.
    void setHeaderReorderDrag(std::optional<TrackHeaderDragHost> host,
                              TrackId dragTrackIdForwarded) noexcept;

    /// [Message thread] Patch rename callbacks after construction (instrument rows created before edit coordinator `install()`).
    void patchRenameCallbacks(std::function<bool()> canBeginRenameTrack,
                              std::function<bool(juce::String trimmedNewName)> onCommitRenameTrack) noexcept;

    /// Tooltip for the hovered cell (currently only the "Instrument alternatives" cell).
    juce::String getTooltip() override;

    // ----------------------------------------------------------- geometry (public for the tests)
    /// Cell geometry (empty when the cell is not present for the current model / height). Public
    /// for layout verification (non-overlap / visibility checks in the focused UI render tests).
    [[nodiscard]] juce::Rectangle<int> getPowerButtonBounds() const noexcept;
    [[nodiscard]] juce::Rectangle<int> getMuteButtonBounds() const noexcept;
    [[nodiscard]] juce::Rectangle<int> getSoloButtonBounds() const noexcept;
    [[nodiscard]] juce::Rectangle<int> getMonitorButtonBounds() const noexcept;
    [[nodiscard]] juce::Rectangle<int> getArmButtonBounds() const noexcept;
    /// Empty when instrument editor cell is inactive (audio tracks or no instrument) or hidden.
    [[nodiscard]] juce::Rectangle<int> getInstrumentEditorButtonBounds() const noexcept;
    /// The P2 "Instrument alternatives" cell (second row; empty when absent or hidden). Public:
    /// used as the popup's anchor rectangle.
    [[nodiscard]] juce::Rectangle<int> getAlternativesButtonBounds() const noexcept;
    /// Colour segment (type icon + number), the name text area and the second-row strip area.
    [[nodiscard]] juce::Rectangle<int> getColourSegmentBounds() const noexcept;
    [[nodiscard]] juce::Rectangle<int> getTypeIconBounds() const noexcept;
    [[nodiscard]] juce::Rectangle<int> getTrackNumberBounds() const noexcept;
    [[nodiscard]] juce::Rectangle<int> getNameTextBounds() const noexcept;
    /// Empty below `kMinimumHeightForSecondRowPx`.
    [[nodiscard]] juce::Rectangle<int> getSecondRowStripBounds() const noexcept;
    /// True when `pos` is inside the colour segment (where a right-click opens the palette).
    [[nodiscard]] bool isPositionInColourSegment(juce::Point<int> pos) const noexcept;

    /// [Stability] Click the Monitor / Arm / Mute / Solo / Power cell exactly like a left mouse
    /// press at its centre: the same hit test, enabled check and callback dispatch as `mouseDown`.
    /// False when the cell is absent or disabled (nothing dispatched).
    bool clickMonitorCellLikeMouseForStabilityTest();
    bool clickArmCellLikeMouseForStabilityTest();
    bool clickMuteCellLikeMouseForStabilityTest();
    bool clickSoloCellLikeMouseForStabilityTest();
    bool clickPowerCellLikeMouseForStabilityTest();
    /// [Stability] Open the header context menu exactly like a right-button press at the name
    /// area (`mouseDown` with popup modifiers → the owner's `onShowContextMenu`). False when no
    /// menu callback is wired.
    bool showContextMenuLikeRightClickForStabilityTest();
    /// [Stability] Right-click the colour segment like the mouse (→ `onShowColourMenu`). False
    /// when no colour-menu callback is wired.
    bool showColourMenuLikeRightClickForStabilityTest();

private:
    enum class DragBlocker : std::uint8_t
    {
        None,
        Arm,
        Mute,
        Solo,
        Power,
        Monitor,
        RowResize,
        InlineRename,
    };

    enum class TrackHeaderButtonKind : std::uint8_t
    {
        InstrumentEditor,
        Power,
        Mute,
        /// Explicit solo (red S, right of Mute) — every row kind except Master/Stereo Out.
        Solo,
        /// Input monitoring (speaker glyph, orange when on) — second row.
        Monitor,
        Arm,
        /// P2: "Instrument alternatives" popup trigger — last cell of the second row.
        Alternatives,
    };

    bool clickStripCellLikeMouse(TrackHeaderButtonKind kind);

    struct TrackHeaderStripButtonSpec
    {
        TrackHeaderButtonKind kind = TrackHeaderButtonKind::Power;
        bool enabled = false;
        /// State for palette only (`Power`=standby/off, `Mute`=muted, `Arm`=armed). Instrument ignores bits.
        bool powerStandby = false;
        bool muteActive = false;
        /// Mute cell while solo is active: effective-state display + lock marking (`TrackStripButtonGlyphs`).
        bool muteSoloSilenced = false;
        bool muteLocked = false;
        bool soloActive = false;
        bool armActive = false;
        bool monitorActive = false;
        juce::Rectangle<int> cellBounds;
    };

    struct HeaderContentLayout
    {
        /// Full chrome height (above the resize band), right of the group margin.
        juce::Rectangle<int> colourSegmentBounds;
        juce::Rectangle<int> typeIconBounds;
        juce::Rectangle<int> numberBounds;
        /// Reserved [Power][Mute][Solo] area on the title row (present cells collapse left).
        juce::Rectangle<int> titleStripBounds;
        juce::Rectangle<int> nameTextBounds;
        /// Second row ([Monitor][Arm][InstrumentEditor][Alternatives], collapsed left); empty when
        /// the height does not fit it.
        juce::Rectangle<int> secondStripBounds;
    };

    [[nodiscard]] HeaderContentLayout computeHeaderContentLayout() const noexcept;

    [[nodiscard]] bool hasPowerCell() const noexcept;
    [[nodiscard]] bool hasInstrumentEditorCell() const noexcept;
    [[nodiscard]] bool hasSoloCell() const noexcept;
    [[nodiscard]] bool hasMonitorCell() const noexcept;
    [[nodiscard]] bool hasArmCell() const noexcept;
    [[nodiscard]] bool hasAlternativesCell() const noexcept;
    /// Cell `index` (left to right) of the title strip / the second strip (empty when out of range).
    [[nodiscard]] juce::Rectangle<int> titleStripCellAtIndex(int index) const noexcept;
    [[nodiscard]] juce::Rectangle<int> secondStripCellAtIndex(int index) const noexcept;

    [[nodiscard]] juce::Rectangle<int>
    squareStripButtonBodyFromCell(juce::Rectangle<int> cell) const noexcept;
    [[nodiscard]] std::vector<TrackHeaderStripButtonSpec> buildStripControlSpecs() const noexcept;
    [[nodiscard]] const TrackHeaderStripButtonSpec*
    findStripControlSpec(std::vector<TrackHeaderStripButtonSpec> const& specs,
                         TrackHeaderButtonKind kind) const noexcept;
    [[nodiscard]] juce::Rectangle<int>
    stripButtonCellBounds(TrackHeaderButtonKind kind,
                          std::vector<TrackHeaderStripButtonSpec> const& specs) const noexcept;

    void drawStripControlButton(juce::Graphics& g,
                                TrackHeaderStripButtonSpec const& spec,
                                bool hoverThis,
                                juce::Colour const& ctlEdgeNeutral) noexcept;
    [[nodiscard]] bool dispatchStripClick(juce::Point<int> position,
                                          std::vector<TrackHeaderStripButtonSpec>&& specs) noexcept;

    void repaintStripHoverCell(std::optional<TrackHeaderButtonKind> kind) noexcept;
    void updateStripHoverFromPosition(juce::Point<int> position) noexcept;
    void clearStripHover() noexcept;

    [[nodiscard]] bool isPositionInRowResizeBand(juce::Point<int> position) const noexcept;

    [[nodiscard]] juce::Rectangle<int> visibleChromeBoundsExcludingResizeBand() const noexcept;
    [[nodiscard]] bool stripCellHitIntersectsVisibleChrome(juce::Rectangle<int> cell,
                                                           juce::Point<int> pos) const noexcept;

    void ensureTrackNameEditor();
    void layoutInlineTrackNameEditor();
    void beginInlineTrackRenameIfPossible(juce::Point<int> clickLocal);
    void cancelInlineTrackRename() noexcept;
    void submitInlineTrackRenameFromEditor();

    void textEditorReturnKeyPressed(juce::TextEditor& e) override;
    void textEditorEscapeKeyPressed(juce::TextEditor& e) override;
    void textEditorFocusLost(juce::TextEditor& e) override;

    TrackHeaderModelProvider modelProvider_;
    TrackHeaderCallbacks callbacks_;
    TrackId dragTrackId_ = kInvalidTrackId;
    std::optional<TrackHeaderDragHost> dragHost_;
    bool headerDragInProgress_ = false;
    DragBlocker dragBlocker_ = DragBlocker::None;
    std::optional<TrackHeaderButtonKind> stripHoveredButton_;
    int rowResizeStartHeightPx_ = 0;
    int rowResizeStartScreenY_ = 0;

    std::unique_ptr<juce::TextEditor> trackNameEditor_;
    bool trackNameRenameSubmitting_ = false;
    bool ignoreTrackNameEditorFocusLoss_ = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TrackHeaderView)
};
