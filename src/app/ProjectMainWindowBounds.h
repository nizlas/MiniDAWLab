#pragma once

#include "io/ProjectFile.h"

#include <optional>

namespace juce
{
class DocumentWindow;
}

/// How `applyProjectWindowBoundsClamped` placed the window (for project-load diagnostics).
enum class ProjectWindowBoundsRestoreOutcome
{
    restoredAsSaved,
    clampedToDisplay,
    centredOffscreenFallback,
};

/// Short token for diagnostics logs: "as-saved" / "clamped" / "centred-offscreen-fallback".
[[nodiscard]] const char* describeWindowBoundsRestoreOutcome(ProjectWindowBoundsRestoreOutcome o) noexcept;

/// Screen-space bounds (and maximized state) from a DAL-owned `DocumentWindow` for project save.
/// Persistence has no opinion about window size: the only rejects are true junk — a minimised
/// window (iconic bounds are meaningless), non-positive size, or absurd (>10000 px) size.
[[nodiscard]] std::optional<ProjectFileMainWindowBoundsV1> captureProjectWindowBoundsForProjectSave(
    juce::DocumentWindow& w) noexcept;

/// Apply saved bounds to a DAL-owned window exactly as saved, clamping only the *position* so the
/// window stays on a visible display (centred fallback if fully offscreen), then re-maximize if
/// saved as maximized. The saved size is restored as-is (sanity-capped at 10000 px).
ProjectWindowBoundsRestoreOutcome applyProjectWindowBoundsClamped(
    juce::DocumentWindow& w, const ProjectFileMainWindowBoundsV1& b) noexcept;

/// Screen-space bounds from the main `DocumentWindow` for project save.
[[nodiscard]] std::optional<ProjectFileMainWindowBoundsV1> captureProjectMainWindowBoundsForProjectSave(
    juce::DocumentWindow& w) noexcept;

/// Apply optional `mainWindow` from a loaded project; no-op if `hasMainWindowBounds` is false.
void applyLoadedProjectMainWindowBounds(juce::DocumentWindow& w, const ProjectFileV1& projectFile) noexcept;
