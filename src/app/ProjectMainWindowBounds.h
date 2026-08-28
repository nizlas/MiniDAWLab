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
/// Returns nullopt for degenerate/absurd sizes.
[[nodiscard]] std::optional<ProjectFileMainWindowBoundsV1> captureProjectWindowBoundsForProjectSave(
    juce::DocumentWindow& w, int minW, int minH) noexcept;

/// Apply saved bounds to a DAL-owned window: clamp size to [minW/minH, 10000], keep the window on a
/// visible display (centred fallback if fully offscreen), then re-maximize if saved as maximized.
ProjectWindowBoundsRestoreOutcome applyProjectWindowBoundsClamped(juce::DocumentWindow& w,
                                                                  const ProjectFileMainWindowBoundsV1& b,
                                                                  int minW, int minH) noexcept;

/// Screen-space bounds from the main `DocumentWindow` for project save.
[[nodiscard]] std::optional<ProjectFileMainWindowBoundsV1> captureProjectMainWindowBoundsForProjectSave(
    juce::DocumentWindow& w) noexcept;

/// Apply optional `mainWindow` from a loaded project; no-op if `hasMainWindowBounds` is false.
void applyLoadedProjectMainWindowBounds(juce::DocumentWindow& w, const ProjectFileV1& projectFile) noexcept;

/// Min useful size for MIDI editor window restore (matches its `setResizeLimits`).
inline constexpr int kMidiEditorWindowMinW = 640;
inline constexpr int kMidiEditorWindowMinH = 420;
