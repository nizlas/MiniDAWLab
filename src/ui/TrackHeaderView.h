#pragma once

// =============================================================================
// TrackHeaderView — shared track header chrome (audio + experimental instrument)
// =============================================================================
// Visual: top row — name (+ optional subtitle); below — left-aligned **[Instrument][Power][M][R]** strip (audio: no Instrument).
// State comes from `TrackHeaderModelProvider`; actions from `TrackHeaderCallbacks`.
// Optional `TrackHeaderDragHost` + non-`kInvalidTrackId` `dragTrackId` enable header-drag reorder
// (`setHeaderReorderDrag` can attach drag after construction for experimental instrument shells).
// =============================================================================

#include "domain/Track.h"

#include <functional>
#include <optional>
#include <vector>
#include <juce_gui_basics/juce_gui_basics.h>

class TrackHeaderView;

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
    /// When non-empty, drawn under `name` (smaller, grey); audio tracks leave this empty.
    juce::String subtitle;
    bool active = false;
    bool armed = false;
    bool muted = false;
    /// When true, power glyph reads as “track off” / standby (same as audio `Track::isTrackOff()`).
    bool off = false;
    bool powerInteractable = true;
    bool muteInteractable = true;
    bool armInteractable = true;
    /// When false or `callbacks.onOpenInstrumentEditor` unset, strip omits instrument-editor cell (audio rows).
    bool instrumentEditorAvailable = false;
};

using TrackHeaderModelProvider = std::function<TrackHeaderModel()>;

struct TrackHeaderCallbacks
{
    /// Left-click on name row / drag surface (not on **[Instrument][Power][Mute][R]** strip). Null = no-op.
    std::function<void()> onActivateName;
    /// Return true if the click was handled (blocks promoting to header-drag); false = ignored.
    std::function<bool()> onTogglePower;
    std::function<void()> onToggleMute;
    std::function<void()> onToggleArm;
    /// Optional: opens native instrument / plugin UI (Groove Agent row). Omit for audio lanes.
    std::function<void()> onOpenInstrumentEditor;
    /// Right-click; null = ignore context menu entirely.
    std::function<void(TrackHeaderView&, const juce::MouseEvent&)> onShowContextMenu;
    /// Bottom-edge row height drag; `startHeightPx` is header height at mouse-down (session thread).
    std::function<void(int startHeightPx, int deltaScreenYPx)> onRowHeightDrag;
    std::function<void()> onRowHeightDragEnd;
};

class TrackHeaderView : public juce::Component
{
public:
    /// Horizontal strip: each M/R/power/instrument cell (`squareStripButtonBodyFromCell` insets inside).
    static constexpr int kStripControlCellWidthPx = 22;
    static constexpr int kStripSquareBodyInsetPx = 1;

    /// Bottom-edge resize band inside the header (matches layout hit-testing).
    static constexpr int kHeaderResizeBandPx = 5;

    [[nodiscard]] static constexpr int resizeBandPx() noexcept { return kHeaderResizeBandPx; }

    /// Minimum row height when the control strip is fully clipped (name + outer pad + resize band only).
    /// Must stay aligned with `computeHeaderContentLayout` vertical constants.
    [[nodiscard]] static int minimumRowHeightPxForNameOnlyLayout(bool hasSubtitle) noexcept;

    /// Alias for layout clamp/snap call sites that think in “name-only chrome + resize band”.
    [[nodiscard]] static int minimumNameOnlyHeightPx(bool hasSubtitle) noexcept
    {
        return minimumRowHeightPxForNameOnlyLayout(hasSubtitle);
    }

    /// After a row-height drag ends: snap to name-only or full name+buttons using the 50% visibility rule.
    /// `hasSubtitle` selects the taller name block (instrument subtitle). Must match header paint geometry.
    /// `globalMinRowPx` must be <= the name-only ideal (`minimumRowHeightPxForNameOnlyLayout`) so the collapsed snap can stick.
    [[nodiscard]] static int snapTrackHeaderRowHeightAfterResize(int heightPx,
                                                                 bool hasSubtitle,
                                                                 int globalMinRowPx,
                                                                 int globalMaxRowPx) noexcept;

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
    void mouseMove(const juce::MouseEvent& e) override;
    void mouseExit(const juce::MouseEvent& e) override;

    /// Empty when instrument editor strip cell is inactive (audio tracks or no instrument).
    [[nodiscard]] juce::Rectangle<int> getInstrumentEditorButtonBounds() const noexcept;

    void setSourceForbiddenForHeaderDrag() noexcept;
    void restoreSourceCursorAfterHeaderDrag() noexcept;

    /// [Message thread] Non-audio rows (e.g. experimental instrument shell) can attach header reorder
    /// drag after construction. `dragTrackIdForwarded` must not be `kInvalidTrackId` when `host` set.
    void setHeaderReorderDrag(std::optional<TrackHeaderDragHost> host,
                              TrackId dragTrackIdForwarded) noexcept;

private:
    enum class DragBlocker : std::uint8_t
    {
        None,
        Arm,
        Mute,
        Power,
        RowResize
    };

    enum class TrackHeaderButtonKind : std::uint8_t
    {
        InstrumentEditor,
        Power,
        Mute,
        Arm,
    };

    struct TrackHeaderStripButtonSpec
    {
        TrackHeaderButtonKind kind = TrackHeaderButtonKind::Power;
        bool enabled = false;
        /// State for palette only (`Power`=standby/off, `Mute`=muted, `Arm`=armed). Instrument ignores bits.
        bool powerStandby = false;
        bool muteActive = false;
        bool armActive = false;
        juce::Rectangle<int> cellBounds;
    };

    [[nodiscard]] int computeRightStripCellCount() const noexcept;

    struct HeaderContentLayout
    {
        juce::Rectangle<int> nameTextBounds;
        /// Contains Instrument→Power→Mute→Arm cells left-to-right (`22px` wide each).
        juce::Rectangle<int> controlStripBounds;
    };

    [[nodiscard]] HeaderContentLayout computeHeaderContentLayout() const noexcept;

    /// Union of control cells (left-aligned strip); strip metrics / painting only (resize hit-test does not use this).
    [[nodiscard]] juce::Rectangle<int> getRightControlsStripBounds() const noexcept;
    [[nodiscard]] juce::Rectangle<int> getPowerButtonBounds() const noexcept;
    [[nodiscard]] juce::Rectangle<int> getMuteButtonBounds() const noexcept;
    [[nodiscard]] juce::Rectangle<int> getArmButtonBounds() const noexcept;

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

    TrackHeaderModelProvider modelProvider_;
    TrackHeaderCallbacks callbacks_;
    TrackId dragTrackId_ = kInvalidTrackId;
    std::optional<TrackHeaderDragHost> dragHost_;
    bool headerDragInProgress_ = false;
    DragBlocker dragBlocker_ = DragBlocker::None;
    std::optional<TrackHeaderButtonKind> stripHoveredButton_;
    int rowResizeStartHeightPx_ = 0;
    int rowResizeStartScreenY_ = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TrackHeaderView)
};
