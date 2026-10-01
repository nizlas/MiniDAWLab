#include "ui/InspectorPanel.h"

InspectorPanel::InspectorPanel(Session& session)
    : inspectorView_(session)
    , channelPanel_(session)
{
    viewport_.setViewedComponent(&inspectorView_, false);
    viewport_.setScrollBarsShown(true, false, true, false);
    viewport_.setScrollBarThickness(10);
    addAndMakeVisible(viewport_);
    addAndMakeVisible(channelPanel_);
    // The viewed component is sized by this panel (width = viewport width minus scrollbar, height =
    // the Inspector's own preferred content height), so the Viewport scrolls, never the Inspector.
    inspectorView_.setOnPreferredHeightChanged([this] { resized(); });
}

InspectorPanel::~InspectorPanel()
{
    inspectorView_.setOnPreferredHeightChanged(nullptr);
    viewport_.setViewedComponent(nullptr, false);
}

void InspectorPanel::paint(juce::Graphics& g)
{
    g.fillAll(getLookAndFeel().findColour(juce::ResizableWindow::backgroundColourId));
}

void InspectorPanel::resized()
{
    auto area = getLocalBounds();
    if (area.isEmpty())
    {
        return;
    }
    // Channel panel height: preferred when the Inspector is tall enough; otherwise give the scroll
    // area its minimum and shrink the panel, but never below the panel's own minimum.
    int panelH = ChannelStripPanel::preferredHeight();
    const int roomForPanel = area.getHeight() - kMinimumScrollAreaHeightPx;
    if (roomForPanel < panelH)
    {
        panelH = juce::jmax(ChannelStripPanel::minimumHeight(), roomForPanel);
    }
    panelH = juce::jmin(panelH, area.getHeight());
    channelPanel_.setBounds(area.removeFromBottom(panelH));
    viewport_.setBounds(area);

    const int contentW = juce::jmax(0, area.getWidth() - (viewport_.isVerticalScrollBarShown() ? viewport_.getScrollBarThickness() : 0));
    const int contentH = juce::jmax(area.getHeight(), inspectorView_.getPreferredContentHeight(contentW));
    inspectorView_.setSize(contentW, contentH);
}
