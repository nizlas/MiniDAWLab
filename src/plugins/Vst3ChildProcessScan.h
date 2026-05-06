#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_core/juce_core.h>

#include <vector>

namespace mini_daw
{

/// First CLI token in raw OOP scan worker mode (parameters only; executable is not in this array).
inline constexpr const char* kVst3OopRawScanWorkerArg = "--minidaw-vst3-raw-scan-worker";

/// Max wall-clock wait for the raw child process (diagnostic cap).
inline constexpr int kVst3OopScanReplyTimeoutMs = 30000;

enum class Vst3OopScanOutcome
{
    Success,
    ChildCrashedOrFailed,
    Timeout,
    LaunchFailed,
    ParseFailed,
};

struct Vst3OopScanResult
{
    Vst3OopScanOutcome outcome = Vst3OopScanOutcome::LaunchFailed;
    int descriptionCount = 0;
    juce::StringArray descriptionLines;
    /// Populated on successful parse (same order as XML children); used for OOP scan + load without rescan.
    std::vector<juce::PluginDescription> descriptions;
    juce::String rawXmlHint;
};

/// Parse `commandLine` into argv suitable for raw scan worker checks (strips leading exe path if present).
[[nodiscard]] juce::StringArray rawScanWorkerArgsFromCommandLine(const juce::String& commandLine);

/// True when argv is `[--minidaw-vst3-raw-scan-worker, vst3Path, resultXmlPath, ...]`.
[[nodiscard]] bool isVst3RawScanWorkerArgv(const juce::StringArray& argv) noexcept;

/// Raw worker entry (no GUI, no audio). Exits via return code; logs child: lines to experimental-vst3-oop-scan.log.
[[nodiscard]] int runVst3RawScanWorkerMain(const juce::StringArray& argv);

/// Raw `juce::ChildProcess` OOP scan. Safe from a background thread (not the message thread).
/// Writes to experimental-vst3-oop-scan.log, including one final `parent: outcome=... elapsedMs=...` per call.
[[nodiscard]] Vst3OopScanResult runVst3OopScanBlocking(const juce::File& vst3File,
                                                       int replyTimeoutMs = kVst3OopScanReplyTimeoutMs);

void writeVst3OopScanDiagnosticLogLine(const juce::String& message);

/// `%APPDATA%\\MiniDAWLab\\experimental-vst3-descriptions.xml` — experimental instrument-path cache
/// (filled when raw OOP scan succeeds; entries keyed by VST3 bundle path).
[[nodiscard]] juce::File getExperimentalVst3DescriptionsCacheFile();

/// Reload cached `PluginDescription`s for `vst3Bundle` if that path exists in the cache file.
/// Experimental only; does not perform an OOP scan.
[[nodiscard]] bool tryLoadExperimentalVst3DescriptionsFromCache(
    const juce::File& vst3Bundle,
    std::vector<juce::PluginDescription>& descriptionsOut);

} // namespace mini_daw
