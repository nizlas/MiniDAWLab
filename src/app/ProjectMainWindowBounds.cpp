#include "app/ProjectMainWindowBounds.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace
{
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

const char* describeWindowBoundsRestoreOutcome(const ProjectWindowBoundsRestoreOutcome o) noexcept
{
    switch (o)
    {
        case ProjectWindowBoundsRestoreOutcome::restoredAsSaved: return "as-saved";
        case ProjectWindowBoundsRestoreOutcome::clampedToDisplay: return "clamped";
        case ProjectWindowBoundsRestoreOutcome::centredOffscreenFallback: break;
    }
    return "centred-offscreen-fallback";
}

std::optional<ProjectFileMainWindowBoundsV1> captureProjectWindowBoundsForProjectSave(
    juce::DocumentWindow& w) noexcept
{
    if (w.isMinimised())
    {
        // Iconic windows report junk screen bounds; keep whatever was remembered before.
        return std::nullopt;
    }
    const juce::Rectangle<int> r = w.getScreenBounds();
    if (r.getWidth() < 1 || r.getHeight() < 1)
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
    b.maximized = w.isFullScreen();
    return b;
}

ProjectWindowBoundsRestoreOutcome applyProjectWindowBoundsClamped(
    juce::DocumentWindow& w, const ProjectFileMainWindowBoundsV1& b) noexcept
{
    const auto relayoutAndRepaint = [&w]() noexcept {
        if (auto* c = w.getContentComponent())
        {
            c->repaint();
        }
        w.repaint();
    };
    const auto reapplyMaximizedIfSaved = [&w, &b]() noexcept {
        if (b.maximized && !w.isFullScreen())
        {
            w.setFullScreen(true);
        }
    };

    // A previously maximized window must not stay maximized while we reposition it, or setBounds
    // is ignored by the native window.
    if (w.isFullScreen())
    {
        w.setFullScreen(false);
    }

    // Size is restored exactly as saved; only guard against nonsense so the window stays grabbable.
    const int width = juce::jlimit(1, 10000, b.width);
    const int height = juce::jlimit(1, 10000, b.height);
    juce::Rectangle<int> proposed(b.x, b.y, width, height);

    const int visProbeW = juce::jmin(64, width);
    const int visProbeH = juce::jmin(64, height);
    if (!rectHasVisibleOverlapOnDisplays(proposed, visProbeW, visProbeH))
    {
        w.centreWithSize(width, height);
        reapplyMaximizedIfSaved();
        relayoutAndRepaint();
        return ProjectWindowBoundsRestoreOutcome::centredOffscreenFallback;
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
        const bool moved = (nx != proposed.getX() || ny != proposed.getY() || width != b.width
                            || height != b.height);
        proposed.setPosition(nx, ny);
        w.setBounds(proposed);
        if (rectHasVisibleOverlapOnDisplays(proposed, visProbeW, visProbeH))
        {
            reapplyMaximizedIfSaved();
            relayoutAndRepaint();
            return moved ? ProjectWindowBoundsRestoreOutcome::clampedToDisplay
                         : ProjectWindowBoundsRestoreOutcome::restoredAsSaved;
        }
        break;
    }

    w.centreWithSize(width, height);
    reapplyMaximizedIfSaved();
    relayoutAndRepaint();
    return ProjectWindowBoundsRestoreOutcome::centredOffscreenFallback;
}

std::optional<ProjectFileMainWindowBoundsV1> captureProjectMainWindowBoundsForProjectSave(
    juce::DocumentWindow& w) noexcept
{
    return captureProjectWindowBoundsForProjectSave(w);
}

void applyLoadedProjectMainWindowBounds(juce::DocumentWindow& w,
                                        const ProjectFileV1& projectFile) noexcept
{
    if (!projectFile.hasMainWindowBounds)
    {
        return;
    }
    (void)applyProjectWindowBoundsClamped(w, projectFile.mainWindowBounds);
}
