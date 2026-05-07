#pragma once

// =============================================================================
// TrackHeaderView — shared track header chrome (audio + experimental instrument)
// =============================================================================
// Visual: name (and optional subtitle), active accent, **[Power][M][R]** strip.
// State comes from `TrackHeaderModelProvider`; actions from `TrackHeaderCallbacks`.
// Optional `TrackHeaderDragHost` + `dragTrackId` enable header-drag reorder (audio lanes).
// =============================================================================

#include "domain/Track.h"

#include <functional>
#include <juce_gui_basics/juce_gui_basics.h>
#include <optional>

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
};

using TrackHeaderModelProvider = std::function<TrackHeaderModel()>;

struct TrackHeaderCallbacks
{
    /// Left-click on name strip (not on **[Power][M][R]**). Null = no-op.
    std::function<void()> onActivateName;
    /// Return true if the click was handled (blocks promoting to header-drag); false = ignored.
    std::function<bool()> onTogglePower;
    std::function<void()> onToggleMute;
    std::function<void()> onToggleArm;
    /// Right-click; null = ignore context menu entirely.
    std::function<void(TrackHeaderView&, const juce::MouseEvent&)> onShowContextMenu;
};

class TrackHeaderView : public juce::Component
{
public:
    /// `dragTrackId` is forwarded to `TrackHeaderDragHost`. Use `kInvalidTrackId` when there is no
    /// drag host (e.g. experimental instrument stripe).
    TrackHeaderView(TrackHeaderModelProvider modelProvider,
                    TrackHeaderCallbacks callbacks,
                    TrackId dragTrackId,
                    std::optional<TrackHeaderDragHost> dragHost) noexcept;

    void paint(juce::Graphics& g) override;
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;
    void mouseUp(const juce::MouseEvent& e) override;

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

    [[nodiscard]] juce::Rectangle<int> getRightControlsStripBounds() const noexcept;
    [[nodiscard]] juce::Rectangle<int> getPowerButtonBounds() const noexcept;
    [[nodiscard]] juce::Rectangle<int> getPowerVisualCircleBounds() const noexcept;
    [[nodiscard]] juce::Rectangle<int> getMuteButtonBounds() const noexcept;
    [[nodiscard]] juce::Rectangle<int> getArmButtonBounds() const noexcept;
    [[nodiscard]] juce::Rectangle<int> getMuteVisualCircleBounds() const noexcept;
    [[nodiscard]] juce::Rectangle<int> getArmVisualCircleBounds() const noexcept;

    TrackHeaderModelProvider modelProvider_;
    TrackHeaderCallbacks callbacks_;
    TrackId dragTrackId_ = kInvalidTrackId;
    std::optional<TrackHeaderDragHost> dragHost_;
    bool headerDragInProgress_ = false;
    DragBlocker dragBlocker_ = DragBlocker::None;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TrackHeaderView)
};
