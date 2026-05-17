#include "domain/AudioMixdownProjectSettings.h"

#include <cstdlib>

namespace
{
[[nodiscard]] bool specIndicatesOutsideProjectRelative(const juce::String& rel)
{
    if (rel.isEmpty())
    {
        return false;
    }
    const juce::String norm = rel.replaceCharacter('\\', '/').trim();
    return norm.startsWith("../") || norm.startsWith("..\\") || norm.contains("/../")
           || norm.contains("\\..\\");
}
} // namespace

AudioMixdownProjectSettings defaultAudioMixdownProjectSettings() noexcept
{
    return {};
}

int clampMp3BitRateKbps(const int kbps) noexcept
{
    const int k[] {128, 160, 192, 224, 256, 320};
    int best = k[0];
    int bestDist = std::abs(kbps - best);
    for (const int v : k)
    {
        const int d = std::abs(kbps - v);
        if (d < bestDist)
        {
            bestDist = d;
            best = v;
        }
    }
    return best;
}

juce::String encodeMixdownOutputDirectorySpec(const juce::File& projectFolder,
                                               const juce::File& absoluteMixdownFolder)
{
    if (!absoluteMixdownFolder.isDirectory() && !absoluteMixdownFolder.getFullPathName().isEmpty())
    {
        // Non-existent path: still treat as user intent; prefer absolute if outside project.
    }

    if (!projectFolder.isDirectory())
    {
        return absoluteMixdownFolder.getFullPathName();
    }

    const juce::String absNorm = absoluteMixdownFolder.getFullPathName();
    const juce::File defaultMix = projectFolder.getChildFile("Mixdown");
    if (absoluteMixdownFolder == defaultMix || absNorm == defaultMix.getFullPathName())
    {
        return "Mixdown";
    }

    const juce::String rel = absoluteMixdownFolder.getRelativePathFrom(projectFolder);
    if (juce::File::isAbsolutePath(rel) || specIndicatesOutsideProjectRelative(rel))
    {
        return absoluteMixdownFolder.getFullPathName();
    }

    return rel.replaceCharacter('\\', '/');
}

juce::File decodeMixdownOutputDirectorySpec(const juce::File& projectFolder, const juce::String& spec)
{
    const juce::String trimmed = spec.trim();
    if (trimmed.isEmpty())
    {
        return projectFolder.isDirectory() ? projectFolder.getChildFile("Mixdown")
                                           : juce::File();
    }

    if (juce::File::isAbsolutePath(trimmed))
    {
        return juce::File(trimmed);
    }

    if (!projectFolder.isDirectory())
    {
        return {};
    }

    return projectFolder.getChildFile(trimmed).getFullPathName();
}
