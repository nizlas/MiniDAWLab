#include "app/ProjectMainWindowBounds.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace
{
    constexpr int kMainWindowMinW = 320;
    constexpr int kMainWindowMinH = 240;

    [[nodiscard]] bool rectHasVisibleOverlapOnDisplays(const juce::Rectangle<int>& r, const int minW,
                                                      const int minH) noexcept
    {
        for (const auto& d : juce::Desktop::getInstance().getDisplays().displays)
        {
            const juce::Rectangle<int> inter = r.getIntersection(d.userArea);
            if (inter.getWidth() >= minW && inter.getHeight() >= minH)
            {
                return true;
            }
        }
        return false;
    }
} // namespace

std::optional<ProjectFileMainWindowBoundsV1> captureProjectMainWindowBoundsForProjectSave(
    juce::DocumentWindow& w) noexcept
{
    const juce::Rectangle<int> r = w.getScreenBounds();
    if (r.getWidth() < kMainWindowMinW || r.getHeight() < kMainWindowMinH)
    {
        return std::nullopt;
    }
    if (r.getWidth() > 10000 || r.getHeight() > 10000)
    {
        return std::nullopt;
    }
    ProjectFileMainWindowBoundsV1 b;
    b.x = r.getX();
    b.y = r.getY();
    b.width = r.getWidth();
    b.height = r.getHeight();
    return b;
}

void applyLoadedProjectMainWindowBounds(juce::DocumentWindow& w,
                                        const ProjectFileV1& projectFile) noexcept
{
    if (!projectFile.hasMainWindowBounds)
    {
        return;
    }
    const auto relayoutAndRepaint = [&w]() noexcept {
        if (auto* c = w.getContentComponent())
        {
            c->repaint();
        }
        w.repaint();
    };

    const int width = juce::jlimit(kMainWindowMinW, 10000, projectFile.mainWindowBounds.width);
    const int height = juce::jlimit(kMainWindowMinH, 10000, projectFile.mainWindowBounds.height);
    juce::Rectangle<int> proposed(projectFile.mainWindowBounds.x, projectFile.mainWindowBounds.y, width, height);

    const int visProbeW = juce::jmin(64, width);
    const int visProbeH = juce::jmin(64, height);
    if (!rectHasVisibleOverlapOnDisplays(proposed, visProbeW, visProbeH))
    {
        w.centreWithSize(width, height);
        relayoutAndRepaint();
        return;
    }

    const juce::Point<int> centre = proposed.getCentre();
    for (const auto& d : juce::Desktop::getInstance().getDisplays().displays)
    {
        if (!d.userArea.contains(centre))
        {
            continue;
        }
        const auto ua = d.userArea;
        const int maxX = juce::jmax(ua.getX(), ua.getRight() - proposed.getWidth());
        const int maxY = juce::jmax(ua.getY(), ua.getBottom() - proposed.getHeight());
        const int nx = juce::jlimit(ua.getX(), maxX, proposed.getX());
        const int ny = juce::jlimit(ua.getY(), maxY, proposed.getY());
        proposed.setPosition(nx, ny);
        w.setBounds(proposed);
        if (rectHasVisibleOverlapOnDisplays(proposed, visProbeW, visProbeH))
        {
            relayoutAndRepaint();
            return;
        }
        break;
    }

    w.centreWithSize(width, height);
    relayoutAndRepaint();
}
