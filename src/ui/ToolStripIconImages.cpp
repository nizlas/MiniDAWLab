#include "ui/ToolStripIconImages.h"

#include "ui/ClipWaveformView.h"

#include <MiniDAWLabToolStripBinaryData.h>

#include <juce_core/juce_core.h>

namespace
{
    [[nodiscard]] bool isMagentaKey(const juce::Colour c) noexcept
    {
        return c.getRed() > 220 && c.getBlue() > 220 && c.getGreen() < 80;
    }

    [[nodiscard]] juce::Image applyMagentaKey(const juce::Image& src)
    {
        if (!src.isValid())
        {
            return {};
        }

        const int w = src.getWidth();
        const int h = src.getHeight();
        juce::Image out(juce::Image::ARGB, w, h, true);
        for (int y = 0; y < h; ++y)
        {
            for (int x = 0; x < w; ++x)
            {
                const juce::Colour c = src.getPixelAt(x, y);
                if (isMagentaKey(c))
                {
                    out.setPixelAt(x, y, juce::Colours::transparentBlack);
                }
                else
                {
                    out.setPixelAt(x, y, c);
                }
            }
        }
        return out;
    }

    [[nodiscard]] juce::Image loadKeyedPng(const char* data, const int dataSize)
    {
        if (data == nullptr || dataSize <= 0)
        {
            return {};
        }
        juce::MemoryInputStream mis(static_cast<const void*>(data), static_cast<size_t>(dataSize), false);
        auto decoded = juce::ImageFileFormat::loadFrom(mis);
        return applyMagentaKey(decoded);
    }

    [[nodiscard]] const juce::Image& pointerIconImage()
    {
        static const juce::Image img = loadKeyedPng(mini_daw_tool_strip_icons::pointer_tool_png,
                                                    mini_daw_tool_strip_icons::pointer_tool_pngSize);
        return img;
    }

    [[nodiscard]] const juce::Image& splitIconImage()
    {
        static const juce::Image img = loadKeyedPng(mini_daw_tool_strip_icons::split_tool_png,
                                                    mini_daw_tool_strip_icons::split_tool_pngSize);
        return img;
    }
} // namespace

namespace mini_daw_ui
{

void drawToolStripToolGlyph(juce::Graphics& g, const EditTool tool, juce::Rectangle<float> iconArea)
{
    const juce::Image* img = nullptr;
    if (tool == EditTool::Pointer)
    {
        img = &pointerIconImage();
    }
    else if (tool == EditTool::Split)
    {
        img = &splitIconImage();
    }

    if (img == nullptr || !img->isValid())
    {
        return;
    }

    g.setImageResamplingQuality(juce::Graphics::highResamplingQuality);
    // Pointer uses a bit less inner inset (~15–20% larger glyph); split slightly less (~10–15%).
    const float innerPad = (tool == EditTool::Pointer) ? 0.45f : 1.0f;
    const auto dest = iconArea.reduced(innerPad);
    if (dest.isEmpty())
    {
        return;
    }

    g.drawImage(*img,
                dest,
                juce::RectanglePlacement::centred);
}

} // namespace mini_daw_ui
