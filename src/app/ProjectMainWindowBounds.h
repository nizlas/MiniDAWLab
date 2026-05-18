#pragma once

#include "io/ProjectFile.h"

#include <optional>

namespace juce
{
class DocumentWindow;
}

/// Screen-space bounds from the main `DocumentWindow` for project save.
[[nodiscard]] std::optional<ProjectFileMainWindowBoundsV1> captureProjectMainWindowBoundsForProjectSave(
    juce::DocumentWindow& w) noexcept;

/// Apply optional `mainWindow` from a loaded project; no-op if `hasMainWindowBounds` is false.
void applyLoadedProjectMainWindowBounds(juce::DocumentWindow& w, const ProjectFileV1& projectFile) noexcept;
