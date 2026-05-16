#include "ui/EditToolIconStrip.h"

#include "ui/ToolStripIconImages.h"
#include "ui/TrackHeaderView.h"

namespace
{
    constexpr unsigned int kToolFillIdleArgb = 0xff5a5858u;
    constexpr unsigned int kToolFillSelectedArgb = 0xff2f4a58u;
    constexpr unsigned int kToolEdgeArgb = 0xd0161616u;
} // namespace

class EditToolIconStrip::GlyphToggleButton final : public juce::Button
{
public:
    GlyphToggleButton(EditTool kind, const char* tooltipStr)
        : juce::Button({})
        , kind_(kind)
    {
        setTooltip(tooltipStr);
        setClickingTogglesState(true);
        setTriggeredOnMouseDown(false);
    }

    [[nodiscard]] EditTool toolKind() const noexcept { return kind_; }

    void paintButton(juce::Graphics& g,
                     bool shouldDrawButtonAsHighlighted,
                     bool shouldDrawButtonAsDown) override
    {
        const juce::Rectangle<int> cell = getLocalBounds();
        if (cell.isEmpty())
        {
            return;
        }

        const int inset = TrackHeaderView::kStripSquareBodyInsetPx;
        const juce::Rectangle<int> bodyPx = cell.reduced(inset);
        if (bodyPx.isEmpty())
        {
            return;
        }

        const juce::Rectangle<float> rf = bodyPx.toFloat();
        const float rad = juce::jlimit(1.4f, 2.85f, juce::jmin(rf.getWidth(), rf.getHeight()) * 0.16f);

        const bool on = getToggleState();
        juce::Colour fill((juce::uint32)(on ? kToolFillSelectedArgb : kToolFillIdleArgb));
        juce::Colour edge((juce::uint32)kToolEdgeArgb);
        if (shouldDrawButtonAsHighlighted && isEnabled())
        {
            fill = fill.brighter(0.11f);
            edge = edge.brighter(0.26f);
        }
        if (on)
        {
            edge = edge.brighter(0.18f);
        }
        if (shouldDrawButtonAsDown)
        {
            fill = fill.darker(0.08f);
        }

        g.setColour(fill);
        g.fillRoundedRectangle(rf, rad);
        g.setColour(edge);
        g.drawRoundedRectangle(rf, rad, 1.0f);

        // Slightly tighter interior padding so PNG glyphs read larger without changing button bounds.
        auto iconArea = rf.reduced(juce::jmax(1.2f, rad * 0.62f), juce::jmax(1.2f, rad * 0.62f));
        mini_daw_ui::drawToolStripToolGlyph(g, kind_, iconArea);
    }

private:
    EditTool kind_;
};

EditToolIconStrip::EditToolIconStrip()
    : pointerButton_(std::make_unique<GlyphToggleButton>(EditTool::Pointer, "Pointer"))
    , splitButton_(std::make_unique<GlyphToggleButton>(EditTool::Split, "Split"))
{
    constexpr int kEditToolRadioGroup = 90421;
    addAndMakeVisible(*pointerButton_);
    addAndMakeVisible(*splitButton_);
    pointerButton_->setRadioGroupId(kEditToolRadioGroup);
    splitButton_->setRadioGroupId(kEditToolRadioGroup);
    pointerButton_->setToggleState(true, juce::dontSendNotification);

    pointerButton_->onClick = [this] {
        if (pointerButton_->getToggleState() && onToolSelected != nullptr)
        {
            onToolSelected(EditTool::Pointer);
        }
    };
    splitButton_->onClick = [this] {
        if (splitButton_->getToggleState() && onToolSelected != nullptr)
        {
            onToolSelected(EditTool::Split);
        }
    };
}

EditToolIconStrip::~EditToolIconStrip() = default;

int EditToolIconStrip::preferredWidth() noexcept
{
    constexpr int gap = 2;
    constexpr int outerPad = 2;
    return outerPad + TrackHeaderView::kStripControlCellWidthPx + gap
           + TrackHeaderView::kStripControlCellWidthPx + outerPad;
}

int EditToolIconStrip::preferredHeight() noexcept
{
    return TrackHeaderView::kStripControlCellWidthPx + 4;
}

void EditToolIconStrip::setSelectedTool(const EditTool tool, const juce::NotificationType notify)
{
    const bool ptr = (tool == EditTool::Pointer);
    pointerButton_->setToggleState(ptr, notify);
    splitButton_->setToggleState(!ptr, notify);
}

EditTool EditToolIconStrip::getSelectedTool() const noexcept
{
    return pointerButton_->getToggleState() ? EditTool::Pointer : EditTool::Split;
}

void EditToolIconStrip::paint(juce::Graphics& g)
{
    const auto rf = getLocalBounds().toFloat();
    if (rf.isEmpty())
    {
        return;
    }
    const float rad = 3.5f;
    g.setColour(juce::Colour(0xff232328));
    g.fillRoundedRectangle(rf.reduced(0.25f), rad);
    g.setColour(juce::Colour(0x59000000));
    g.drawRoundedRectangle(rf.reduced(0.25f), rad, 1.0f);
}

void EditToolIconStrip::resized()
{
    auto r = getLocalBounds();
    constexpr int gap = 2;
    constexpr int outerPad = 2;
    r.reduce(outerPad, juce::jmax(0, (r.getHeight() - TrackHeaderView::kStripControlCellWidthPx) / 2));
    const int cell = TrackHeaderView::kStripControlCellWidthPx;
    pointerButton_->setBounds(r.removeFromLeft(cell));
    r.removeFromLeft(gap);
    splitButton_->setBounds(r.removeFromLeft(cell));
}
