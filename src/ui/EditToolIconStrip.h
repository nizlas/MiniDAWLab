#pragma once

#include "ui/ClipWaveformView.h"

#include <functional>
#include <memory>

#include <juce_gui_basics/juce_gui_basics.h>

// -----------------------------------------------------------------------------
// EditToolIconStrip — compact Pointer / Split toggle strip (arrangement toolbar)
// -----------------------------------------------------------------------------
class EditToolIconStrip final : public juce::Component
{
public:
    EditToolIconStrip();
    ~EditToolIconStrip() override;

    void setSelectedTool(EditTool tool,
                         juce::NotificationType notify = juce::dontSendNotification);
    [[nodiscard]] EditTool getSelectedTool() const noexcept;

    /// Fires when the user picks a tool (not when `setSelectedTool` uses dontSend).
    std::function<void(EditTool)> onToolSelected;

    [[nodiscard]] static int preferredWidth() noexcept;
    [[nodiscard]] static int preferredHeight() noexcept;

    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    class GlyphToggleButton;

    std::unique_ptr<GlyphToggleButton> pointerButton_;
    std::unique_ptr<GlyphToggleButton> splitButton_;
};
