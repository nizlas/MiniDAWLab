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
// SYNC
//   A 10 Hz poll (same cadence as the Inspector) reads the published snapshot: strips are
//   created / removed / reordered to match the row list, each strip refreshes its controls from
//   its own row, the active row is highlighted. Nothing here is a second source of truth — all
//   state is the session's; the layout flags are machine-local preferences reported to the owner
//   through `onSectionVisibilityChanged` for persistence.
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

    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    void timerCallback() override;
    void syncStripSet(const SessionSnapshot& snap);
    void layoutStrips();
    void syncMasterScroll();

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

    const MixerStripBindings& bindings_;
    LevelMeterHub* meterHub_;
    mixer_layout::SectionVisibility visibility_;
    std::array<juce::TextButton, mixer_layout::kSectionCount> sectionToggles_;
    juce::Label titleLabel_;
    SyncedViewport stripsViewport_;
    StripsRow stripsRow_;
    SyncedViewport masterViewport_;
    juce::Component masterHolder_;
    std::vector<std::unique_ptr<MixerChannelStrip>> strips_;
    std::unique_ptr<MixerChannelStrip> masterStrip_;
    TrackId masterTrackId_ = kInvalidTrackId;
    bool syncingScroll_ = false;
    int lastStripHeight_ = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MixerContentComponent)
};
