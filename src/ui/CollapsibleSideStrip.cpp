// =============================================================================
// CollapsibleSideStrip.cpp — logic mirrored from Main.cpp inspector chrome
// =============================================================================

#include "ui/CollapsibleSideStrip.h"

namespace collapsible_side_strip
{

void ResizeSplitter::mouseDown(const juce::MouseEvent&)
{
    anchorW_ = host_.getSideStripWidth();
}

void ResizeSplitter::mouseDrag(const juce::MouseEvent& e)
{
    const int w = juce::jlimit(0, host_.getSideStripMaxWidth(), anchorW_ + e.getDistanceFromDragStartX());
    if (w != host_.getSideStripWidth())
    {
        host_.setSideStripWidth(w);
        host_.sideStripLayoutChanged();
    }
}

void ResizeSplitter::paint(juce::Graphics& g)
{
    juce::ignoreUnused(g);
}

void CollapsedKnob::mouseEnter(const juce::MouseEvent&)
{
    setMouseCursor(juce::MouseCursor::LeftRightResizeCursor);
}

void CollapsedKnob::mouseExit(const juce::MouseEvent&)
{
    setMouseCursor(juce::MouseCursor::NormalCursor);
}

void CollapsedKnob::mouseDown(const juce::MouseEvent&)
{
    dragAnchorW_ = host_.getSideStripWidth();
}

void CollapsedKnob::mouseDrag(const juce::MouseEvent& e)
{
    const int w = juce::jlimit(0, host_.getSideStripMaxWidth(), dragAnchorW_ + e.getDistanceFromDragStartX());
    if (w != host_.getSideStripWidth())
    {
        host_.setSideStripWidth(w);
        host_.sideStripLayoutChanged();
    }
}

void CollapsedKnob::mouseUp(const juce::MouseEvent& e)
{
    if (host_.getSideStripWidth() != 0)
    {
        return;
    }
    if (e.getDistanceFromDragStart() >= 4.0f)
    {
        return;
    }
    host_.setSideStripWidth(host_.getSideStripDefaultWidth());
    host_.sideStripLayoutChanged();
}

void CollapsedKnob::paint(juce::Graphics& g)
{
    const auto r = getLocalBounds().toFloat();
    if (r.getWidth() <= 0.f || r.getHeight() <= 0.f)
    {
        return;
    }
    const float rad = juce::jmin(3.f, r.getWidth() * 0.42f, r.getHeight() * 0.4f);
    g.setColour(juce::Colours::grey.withAlpha(0.70f));
    g.fillRoundedRectangle(r, rad);
    g.setColour(juce::Colours::darkgrey.withAlpha(0.88f));
    g.drawRoundedRectangle(r, rad, 1.f);
}

} // namespace collapsible_side_strip
