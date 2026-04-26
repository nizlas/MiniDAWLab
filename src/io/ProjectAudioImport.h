// =============================================================================
// ProjectAudioImport  —  copy external add-clip picks into <ProjectFolder>/Audio/
// =============================================================================
// Copy-only: never delete, move, or modify the user’s original file; only
// `juce::File::copyFileTo` into the project. Session / ProjectFile unchanged.

#pragma once

#include <juce_core/juce_core.h>

namespace mini_daw
{
    [[nodiscard]] juce::File getProjectAudioDir(const juce::File& projectFolder);

    // Ensures `audioDir` exists. If `source` is already in `audioDir`, `outPathToUse` is the
    // source path (no copy). Otherwise copies into `audioDir` with a unique `name[_n].ext`.
    [[nodiscard]] juce::Result importAudioIntoProjectAudioDir(
        const juce::File& source,
        const juce::File& audioDir,
        juce::File& outPathToUse);
} // namespace mini_daw
