#pragma once

// =============================================================================
// InspectorPanel — scrollable Inspector content above a fixed channel panel
// =============================================================================
//
// ROLE
//   Owns the Inspector UI as two parts: a vertically scrollable `InspectorView` (name, pre-gain,
//   pan, routing, inserts, sends — unchanged controls) inside a `juce::Viewport`, and the
//   always-visible `ChannelStripPanel` (fader + meters) pinned to the bottom. The scroll area
//   keeps meaningful height on low windows: the channel panel shrinks from its preferred height
//   towards its minimum before the scroll area gives up space (see `resized`).
//
// OWNERSHIP
//   Owns both children; `TransportControlsContent` keeps using `inspector()` exactly as it used the
//   former `inspectorView_` member. The Viewport does NOT own the viewed component.
//
// THREADING
//   [Message thread] only.
// =============================================================================

#include "ui/ChannelStripPanel.h"
#include "ui/InspectorView.h"

#include <juce_gui_basics/juce_gui_basics.h>

class Session;

class InspectorPanel final : public juce::Component
{
public:
    explicit InspectorPanel(Session& session);
    ~InspectorPanel() override;

    [[nodiscard]] InspectorView& inspector() noexcept { return inspectorView_; }
    [[nodiscard]] ChannelStripPanel& channelPanel() noexcept { return channelPanel_; }
    [[nodiscard]] juce::Viewport& scrollViewport() noexcept { return viewport_; }

    /// Minimum scroll-area height the layout defends before shrinking the channel panel below its
    /// preferred height; below `ChannelStripPanel::minimumHeight()` the panel never shrinks.
    static constexpr int kMinimumScrollAreaHeightPx = 160;

    void resized() override;
    void paint(juce::Graphics& g) override;

private:
    InspectorView inspectorView_;
    ChannelStripPanel channelPanel_;
    juce::Viewport viewport_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(InspectorPanel)
};
