#pragma once

// =============================================================================
// TrackHeaderView  —  minimal label + active highlight; header-drag for track reorder (message)
// =============================================================================
// `mouseDown` on the **name** region calls `Session::setActiveTrack` (no snapshot republish). The
// control strip (left → right **[Power][M][R]**) uses `Session` for off/mute and `RecorderService`
// for arm. **Off** only applies when transport is not Playing and not recording (ignored silently
// otherwise). **Mute** may toggle anytime.
// Drag past a threshold (from the name strip) is coordinated by `TrackLanesView` (insert line,
// `Session::moveTrack`). The event lane to the right is not a header-drag target. Invalid drop uses
// `getForbiddenNoDropMouseCursor` (`ForbiddenCursor.h`).
// =============================================================================

#include "domain/Track.h"

#include <functional>
#include <juce_gui_basics/juce_gui_basics.h>

class RecorderService;
class Session;
class Transport;
class TrackHeaderView;

// [Message thread] `TrackLanesView` implements these; Began/Ended pair with move updates.
struct TrackHeaderDragHost
{
    std::function<void(TrackId, TrackHeaderView*)> onHeaderDragBegan;
    std::function<void(TrackId, juce::Point<int>)> onHeaderDragMoved;
    std::function<void(TrackId)> onHeaderDragEnded;
};

class TrackHeaderView : public juce::Component
{
public:
    TrackHeaderView(
        Session& session,
        RecorderService& recorder,
        Transport& transport,
        TrackId trackId,
        std::function<void()> onActiveChanged,
        std::function<void()> onArmStateChanged,
        TrackHeaderDragHost dragHost) noexcept;

    void paint(juce::Graphics& g) override;
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;
    void mouseUp(const juce::MouseEvent& e) override;

    // [Message thread] Only `TrackLanesView` calls these on the source header during a drag.
    void setSourceForbiddenForHeaderDrag() noexcept;
    void restoreSourceCursorAfterHeaderDrag() noexcept;

private:
    enum class DragBlocker : std::uint8_t
    {
        None,
        Arm,
        Mute,
        Power
    };

    /// [Power][M][R] trio (see `paint`); widths match hit targets.
    [[nodiscard]] juce::Rectangle<int> getRightControlsStripBounds() const noexcept;
    [[nodiscard]] juce::Rectangle<int> getPowerButtonBounds() const noexcept;
    /// Small circle centred in the power column (same sizing rule as **`M`** / **`R`**).
    [[nodiscard]] juce::Rectangle<int> getPowerVisualCircleBounds() const noexcept;
    [[nodiscard]] juce::Rectangle<int> getMuteButtonBounds() const noexcept;
    /// [Message / paint] Wider than the visible circle for click targets; `mouseDown` uses this.
    [[nodiscard]] juce::Rectangle<int> getArmButtonBounds() const noexcept;
    /// Small circle centred in the mute column (same sizing rule as **`R`**).
    [[nodiscard]] juce::Rectangle<int> getMuteVisualCircleBounds() const noexcept;
    /// Small circle centred in the record-arm column (**`R`** glyph).
    [[nodiscard]] juce::Rectangle<int> getArmVisualCircleBounds() const noexcept;

    Session& session_;
    RecorderService& recorder_;
    Transport& transport_;
    const TrackId trackId_;
    std::function<void()> onActiveChanged_;
    std::function<void()> onArmStateChanged_;
    TrackHeaderDragHost dragHost_;
    bool headerDragInProgress_ = false;
    /// When non-None: gesture began on that control — do not promote to header-drag.
    DragBlocker dragBlocker_ = DragBlocker::None;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TrackHeaderView)
};
