#pragma once

#include <juce_core/juce_core.h>

#include <vector>

namespace mini_daw
{

enum class PluginPickerSupport : juce::uint8
{
    SupportedCandidate,
    UnsupportedInstrument
};

struct PluginDiscoveryEntry
{
    juce::String displayName;
    juce::File file;
    PluginPickerSupport support = PluginPickerSupport::SupportedCandidate;
    juce::String unsupportedReason;
};

/// [Message thread] Name-only classification for the picker and UI guards; no plugin instantiation.
[[nodiscard]] PluginPickerSupport classifyVst3Candidate(const juce::String& displayName) noexcept;

struct PluginDiscoveryResult
{
    std::vector<PluginDiscoveryEntry> entries;
    std::vector<juce::File> inaccessibleFolders;
    int millisElapsed = 0;
};

/// %APPDATA%\\MiniDAWLab\\vst3-search-paths.xml (user-added search roots only).
[[nodiscard]] juce::File getVst3SearchPathsFile();

/// Platform-standard VST3 folder(s) via `juce::VST3PluginFormat::getDefaultLocationsToSearch()`.
[[nodiscard]] juce::FileSearchPath getStandardVst3SearchPaths();

[[nodiscard]] juce::FileSearchPath loadUserVst3SearchPaths();
void saveUserVst3SearchPaths(const juce::FileSearchPath& userPaths);

/// Recursively lists .vst3 files and bundle directories under each root. No plugin instantiation.
[[nodiscard]] PluginDiscoveryResult scanForVst3Plugins(const juce::FileSearchPath& combinedRoots);

} // namespace mini_daw
