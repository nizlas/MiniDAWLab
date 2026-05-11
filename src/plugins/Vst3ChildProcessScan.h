#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_core/juce_core.h>

#include <optional>
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

enum class Vst3ExperimentalCacheScanOutcome
{
    Success,
    Failed,
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

/// Optional file fingerprint for `experimental-vst3-descriptions.xml` bundle rows (Phase 2 cache).
struct Vst3BundleFileFingerprint
{
    juce::int64 fileSizeBytes = 0;
    juce::String fileMtimeIso;
    /// First 8 bytes of SHA256 as 16 hex chars; empty if hashing failed or file skipped (too large / missing).
    juce::String fileSha256Prefix16Hex;
};

// -----------------------------------------------------------------------------
// Phase 2 — general plugin capabilities (optional fields; mostly empty until later phases).
// -----------------------------------------------------------------------------

struct DrumNoteDisplayCapability
{
};

struct RawPitchMapCapability
{
};

struct PlayableRangeCapability
{
};

struct ProgramListCapability
{
};

struct PluginCapabilities
{
    std::optional<DrumNoteDisplayCapability> drumNoteDisplay;
    std::optional<RawPitchMapCapability> rawPitchMap;
    std::optional<PlayableRangeCapability> playableRange;
    std::optional<ProgramListCapability> programList;
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

/// Cheap bundle metadata for cache validity (size, mtime, short hash). Hash may be empty on failure.
[[nodiscard]] Vst3BundleFileFingerprint computeVst3BundleFileFingerprint(const juce::File& vst3Bundle) noexcept;

/// `%APPDATA%\\MiniDAWLab\\experimental-vst3-descriptions.xml` — experimental instrument-path cache
/// (filled when raw OOP scan succeeds; entries keyed by VST3 bundle path).
[[nodiscard]] juce::File getExperimentalVst3DescriptionsCacheFile();

/// Reload cached `PluginDescription`s for `vst3Bundle` if that path exists in the cache file.
/// Experimental only; does not perform an OOP scan.
/// v2: reads `<plugin>` entries when present (no duplicate bare + wrapped descriptions).
[[nodiscard]] bool tryLoadExperimentalVst3DescriptionsFromCache(
    const juce::File& vst3Bundle,
    std::vector<juce::PluginDescription>& descriptionsOut);

/// Merge / replace one `bundle` entry in `experimental-vst3-descriptions.xml` (used after OOP scan or path repair).
/// Writes v2 structure with legacy bare `<PluginDescription/>` children preserved for older readers.
void mergeExperimentalVst3DescriptionsCacheBundle(const juce::File& vst3Bundle,
                                                  const std::vector<juce::PluginDescription>& descriptions,
                                                  Vst3ExperimentalCacheScanOutcome scanOutcome
                                                  = Vst3ExperimentalCacheScanOutcome::Success);

/// Phase 2: merge/replace `<capabilities>` under the matching v2 `<plugin>` (identifier match).
/// No-op if cache or bundle or plugin row is missing, or if the bundle has no `<plugin>` wrappers (v1-only).
void mergeCapabilitiesIntoBundle(const juce::File& vst3Bundle,
                                 const juce::PluginDescription& forPlugin,
                                 const PluginCapabilities& caps);

/// Dev / CI: in-memory + temp-file checks for v1/v2 cache XML (does not touch the user cache file).
[[nodiscard]] bool verifyExperimentalVst3DescriptionsCachePhase2() noexcept;

/// Project-load helper for **GrooveAgentSE** only: load from cache (optional full-cache scan), and if the
/// cached bundle / `fileOrIdentifier` no longer exists, search standard Windows VST3 folders for
/// `Groove Agent SE.vst3`, patch `PluginDescription` paths in memory, and return the resolved bundle.
/// Does not run in-process `findAllTypesForFile` or raw OOP scan. Logs to `experimental-vst3-oop-scan.log`.
[[nodiscard]] bool tryLoadExperimentalVst3DescriptionsFromCacheWithPathRepair(
    const juce::File& savedOrAdvisoryBundle,
    const juce::String& instrumentKind,
    std::vector<juce::PluginDescription>& descriptionsOut,
    juce::File& resolvedBundleOut,
    juce::String& infoOrWarningOut,
    bool* pathRepairWasUsedOut = nullptr);

} // namespace mini_daw
