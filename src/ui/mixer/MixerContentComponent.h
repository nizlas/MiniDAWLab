#pragma once

// =============================================================================
// MixerContentComponent — toolbar + scrolling channel strips + the fixed Stereo Out strip
// =============================================================================
//
// ROLE
//   The mixer window's content. A fixed toolbar holds one toggle per section (Routing, Pre-gain,
//   Pre inserts, Post inserts, Sends, Faders, Meters) — global for every strip. Below it the
//   channel strips of every non-Master row in ARRANGEMENT ORDER scroll horizontally (and
//   vertically when the window is lower than the strip stack) inside one viewport; the Stereo Out
//   strip sits in its own viewport at the right edge, never scrolls horizontally, and mirrors the
//   vertical scroll position so its bands stay aligned with the others.
//
// SECTION HEIGHTS AND DIVIDERS
//   The upper sections share one height each (`mixer_layout::SectionHeights`, persisted by the
//   owner). A `DividerOverlay` spanning the whole strip area (strips, gaps and Stereo Out alike)
//   draws a grey line in every gap between consecutive visible bands and lets the user drag it
//   with a vertical resize cursor: the drag redistributes height between the two adjacent bands
//   (or between the last upper band and the fader / meter band) through
//   `mixer_layout::applyDividerDrag`, the same central layout is re-applied to every strip, and
//   the heights are reported once at drag end (`onSectionHeightsChanged`). The overlay hit-tests
//   only a few pixels around each line, so strips, faders and the viewport scrolling underneath
//   are untouched.
//
// SYNC
//   A 10 Hz poll (same cadence as the Inspector) reads the published snapshot: strips are
//   created / removed / reordered to match the row list, each strip refreshes its controls from
//   its own row, the active row is highlighted. Nothing here is a second source of truth — all
//   state is the session's; the layout flags / heights are machine-local preferences reported
//   to the owner for persistence.
//
// THREADING
//   [Message thread] only.
// =============================================================================

#include "domain/Track.h"
#include "ui/LevelMeterHub.h"
#include "ui/mixer/MixerChannelStrip.h"
#include "ui/mixer/MixerSectionLayout.h"
#include "ui/mixer/MixerStripBindings.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>
#include <vector>

class MixerContentComponent final : public juce::Component,
                                    private juce::Timer
{
public:
    MixerContentComponent(const MixerStripBindings& bindings, LevelMeterHub* hub);
    ~MixerContentComponent() override;

    /// Rebuild / refresh now (also runs on the 10 Hz poll).
    void refreshFromSession();

    void setSectionVisibility(const mixer_layout::SectionVisibility& v);
    [[nodiscard]] const mixer_layout::SectionVisibility& sectionVisibility() const noexcept { return visibility_; }
    /// Fired after a toolbar toggle changed the global section flags (owner persists them).
    std::function<void(const mixer_layout::SectionVisibility&)> onSectionVisibilityChanged;

    /// The shared upper-section heights (clamped on the way in; a hidden section keeps its value).
    void setSectionHeights(const mixer_layout::SectionHeights& h);
    [[nodiscard]] const mixer_layout::SectionHeights& sectionHeights() const noexcept { return heights_; }
    /// Fired once when a divider drag ends (owner persists the heights).
    std::function<void(const mixer_layout::SectionHeights&)> onSectionHeightsChanged;

    // --- test / scenario surfaces -----------------------------------------------------------------
    [[nodiscard]] MixerChannelStrip* stripForTrack(TrackId trackId) const noexcept;
    [[nodiscard]] MixerChannelStrip* masterStrip() const noexcept { return masterStrip_.get(); }
    [[nodiscard]] int stripCount() const noexcept { return static_cast<int>(strips_.size()); }
    [[nodiscard]] std::vector<TrackId> stripOrder() const;
    [[nodiscard]] juce::Viewport& stripsViewport() noexcept { return stripsViewport_; }
    [[nodiscard]] juce::Rectangle<int> masterStripScreenBounds() const;
    [[nodiscard]] int toolbarHeight() const noexcept;
    /// Geometry check across toolbar, viewport, strips, bands, fixed Stereo Out; appends a report.
    [[nodiscard]] bool verifyLayout(juce::String& report, juce::String& failReason) const;
    /// Click a section toggle like the user (the button's own click path).
    void clickSectionToggleForTest(mixer_layout::Section s);
    /// The dividers of the current layout (top to bottom) and a drag on one of them through the
    /// overlay's own drag handler (the mouse path minus the OS: same model call, same callbacks).
    [[nodiscard]] std::vector<mixer_layout::Divider> currentDividers() const;
    bool dragDividerForTest(int dividerIndex, int deltaY);
    [[nodiscard]] int currentStripHeight() const noexcept { return lastStripHeight_; }

    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    void timerCallback() override;
    void syncStripSet(const SessionSnapshot& snap);
    void layoutStrips();
    void syncMasterScroll();
    [[nodiscard]] int availableStripHeight() const noexcept;
    [[nodiscard]] mixer_layout::ComputedLayout currentLayout() const noexcept;
    void beginDividerDrag(int dividerIndex);
    void continueDividerDrag(int dividerIndex, int deltaY);
    void endDividerDrag();

    class StripsRow final : public juce::Component
    {
    public:
        void paint(juce::Graphics& g) override;
    };

    /// Viewport whose vertical position drives the master viewport (and vice versa).
    class SyncedViewport final : public juce::Viewport
    {
    public:
        std::function<void()> onVisibleAreaChanged;
        void visibleAreaChanged(const juce::Rectangle<int>&) override
        {
            if (onVisibleAreaChanged != nullptr)
            {
                onVisibleAreaChanged();
            }
        }
    };

    /// Transparent layer over the strip area: paints the shared divider lines across the whole
    /// mixer and owns the drag. Hit-tests only near a line; everything else passes through.
    class DividerOverlay final : public juce::Component
    {
    public:
        explicit DividerOverlay(MixerContentComponent& owner) : owner_(owner)
        {
            setInterceptsMouseClicks(true, false);
            setRepaintsOnMouseActivity(true);
        }
        bool hitTest(int x, int y) override;
        void paint(juce::Graphics& g) override;
        void mouseDown(const juce::MouseEvent& e) override;
        void mouseDrag(const juce::MouseEvent& e) override;
        void mouseUp(const juce::MouseEvent& e) override;
        /// A wheel over the thin line scrolls the stack like anywhere else (the overlay owns no scroll).
        void mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override
        {
            owner_.stripsViewport_.mouseWheelMove(e.getEventRelativeTo(&owner_.stripsViewport_), wheel);
        }
        juce::MouseCursor getMouseCursor() override { return juce::MouseCursor::UpDownResizeCursor; }
        /// Divider index under `y` (overlay coordinates) or -1.
        [[nodiscard]] int dividerAt(int y) const;
        /// Overlay y of a divider's line (strip y shifted by the vertical scroll).
        [[nodiscard]] int lineYOf(const mixer_layout::Divider& d) const;

    private:
        MixerContentComponent& owner_;
        int draggingIndex_ = -1;
        int dragStartY_ = 0;
    };

    const MixerStripBindings& bindings_;
    LevelMeterHub* meterHub_;
    mixer_layout::SectionVisibility visibility_;
    mixer_layout::SectionHeights heights_;
    mixer_layout::SectionHeights heightsAtDragStart_;
    std::vector<mixer_layout::Divider> dividersAtDragStart_;
    std::array<juce::TextButton, mixer_layout::kSectionCount> sectionToggles_;
    juce::Label titleLabel_;
    SyncedViewport stripsViewport_;
    StripsRow stripsRow_;
    SyncedViewport masterViewport_;
    juce::Component masterHolder_;
    DividerOverlay dividerOverlay_{ *this };
    std::vector<std::unique_ptr<MixerChannelStrip>> strips_;
    std::unique_ptr<MixerChannelStrip> masterStrip_;
    TrackId masterTrackId_ = kInvalidTrackId;
    bool syncingScroll_ = false;
    int lastStripHeight_ = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MixerContentComponent)
};
