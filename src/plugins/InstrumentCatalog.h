#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_core/juce_core.h>

#include "io/ProjectFile.h"

#include <vector>
namespace mini_daw
{

struct InstrumentCatalogEntry
{
    juce::String bundlePath;
    juce::PluginDescription description;
    juce::String classificationReason;
};

struct InstrumentCatalogRescanSummary
{
    juce::StringArray scannedFolderPaths;
    int candidateBundleCount = 0;
    int acceptedInstrumentCount = 0;
    int rejectedEffectCount = 0;
    int rejectedValidationFailedCount = 0;
    int rejectedDuplicateCount = 0;
    bool completed = false;
    /// Human-readable finding when a bundle path matches BBC Symphony Orchestra (empty if not seen).
    juce::String bbcSymphonyOrchestraFinding;
};

/// `%APPDATA%\\MiniDAWLab\\instrument-catalog-v1.xml`
[[nodiscard]] juce::File getInstrumentCatalogV1CacheFile();

/// `%APPDATA%\\MiniDAWLab\\instrument-catalog-rescan.log`
[[nodiscard]] juce::File getInstrumentCatalogRescanLogFile();

void writeInstrumentCatalogRescanLogLine(const juce::String& message);

/// [Any thread] True when `d` is an instrument: `isInstrument` first, then safe category fallback
/// (Instrument / Synth substrings). `reasonOut` describes acceptance or rejection.
[[nodiscard]] bool classifyPluginDescriptionAsInstrument(const juce::PluginDescription& d,
                                                           juce::String& reasonOut) noexcept;

/// [Background thread only] Full VST3-folder discovery, OOP scan, classification, catalog write, and logging.
[[nodiscard]] InstrumentCatalogRescanSummary rescanInstrumentCatalogBlocking();

/// [Message thread] Load accepted instruments from `instrument-catalog-v1.xml` (sorted by display name).
/// Returns false if the cache file is missing or unreadable.
[[nodiscard]] bool loadInstrumentCatalogFromCache(std::vector<InstrumentCatalogEntry>& out);

/// [Background / message thread] Resolve saved GenericVst3 identity to a bundle + instrument description.
struct GenericVst3ProjectLoadResolution
{
    bool resolved = false;
    juce::File bundle;
    juce::PluginDescription description;
};

void fillProjectGenericVst3DescriptorFromPluginDescription(
    ProjectFileGenericVst3DescriptorV1& out,
    const juce::PluginDescription& d) noexcept;

[[nodiscard]] juce::PluginDescription pluginDescriptionFromProjectGenericVst3Descriptor(
    const ProjectFileGenericVst3DescriptorV1& saved) noexcept;

[[nodiscard]] GenericVst3ProjectLoadResolution tryResolveGenericVst3ForProjectLoad(
    bool hasSavedDescriptor,
    const ProjectFileGenericVst3DescriptorV1& savedDescriptor,
    const juce::String& savedBundlePath,
    const juce::String& fallbackDisplayName) noexcept;

} // namespace mini_daw
