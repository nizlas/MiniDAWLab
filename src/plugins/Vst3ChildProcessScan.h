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

/// Optional file fingerprint for v2 cache bundle rows (`experimental-vst3-descriptions-v2.xml`).
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

/// Windows: locate `Groove Agent SE.vst3` under common VST3 install folders (for OOP scan target fallback).
/// Empty file on other platforms or when not found.
[[nodiscard]] juce::File getGrooveAgentSeVst3BundlePathForOopScanFallback() noexcept;

/// Cheap bundle metadata for cache validity (size, mtime, short hash). Hash may be empty on failure.
[[nodiscard]] Vst3BundleFileFingerprint computeVst3BundleFileFingerprint(const juce::File& vst3Bundle) noexcept;

/// `%APPDATA%\\MiniDAWLab\\experimental-vst3-descriptions.xml` — legacy v1 cache (read-only in app code;
/// never written or migrated by this phase).
[[nodiscard]] juce::File getExperimentalVst3DescriptionsV1CacheFile();

/// `%APPDATA%\\MiniDAWLab\\experimental-vst3-descriptions-v2.xml` — writable v2 cache (successful OOP scan only).
[[nodiscard]] juce::File getExperimentalVst3DescriptionsV2CacheFile();

/// Same path as v1. Prefer the explicit `...V1...` name in new code; this alias remains for existing callers.
[[nodiscard]] juce::File getExperimentalVst3DescriptionsCacheFile();

enum class Vst3ExperimentalCacheTier
{
    V1,
    V2,
};

/// One resolved Groove-agent cache entry (direct bundle match or full-cache name scan + optional path repair).
struct Vst3GrooveCacheLoadCandidate
{
    bool valid = false;
    std::vector<juce::PluginDescription> descriptions;
    juce::File resolvedBundle;
    bool pathRepairUsed = false;
    Vst3ExperimentalCacheTier tier = Vst3ExperimentalCacheTier::V1;
};

/// Reload v1 cache only (`experimental-vst3-descriptions.xml`). Legacy `<PLUGIN>` children under `<bundle>`.
[[nodiscard]] bool tryLoadExperimentalVst3DescriptionsFromV1Cache(
    const juce::File& vst3Bundle,
    std::vector<juce::PluginDescription>& descriptionsOut);

/// Reload v2 cache only (`experimental-vst3-descriptions-v2.xml`). Prefers lowercase `<plugin>` wrappers when present.
[[nodiscard]] bool tryLoadExperimentalVst3DescriptionsFromV2Cache(
    const juce::File& vst3Bundle,
    std::vector<juce::PluginDescription>& descriptionsOut);

/// Try v2, then v1 (for UI badge / generic lookup). Does not run OOP scan.
[[nodiscard]] bool tryLoadExperimentalVst3DescriptionsFromCache(
    const juce::File& vst3Bundle,
    std::vector<juce::PluginDescription>& descriptionsOut);

/// Build independent Groove candidates from the v2 file and the v1 file (path repair in-memory only; never writes caches).
[[nodiscard]] bool tryLoadGrooveAgentCacheCandidates(const juce::File& savedOrAdvisoryBundle,
                                                     Vst3GrooveCacheLoadCandidate& v2Out,
                                                     Vst3GrooveCacheLoadCandidate& v1Out,
                                                     juce::String& infoOrWarningOut);

/// Merge / replace one `bundle` in **v2 only** (`experimental-vst3-descriptions-v2.xml`), via temp file + replace.
/// Call only after a **successful** OOP scan. Never writes or modifies the v1 legacy file.
void mergeExperimentalVst3DescriptionsCacheBundle(const juce::File& vst3Bundle,
                                                  const std::vector<juce::PluginDescription>& descriptions,
                                                  Vst3ExperimentalCacheScanOutcome scanOutcome
                                                  = Vst3ExperimentalCacheScanOutcome::Success);

/// Merge/replace `<capabilities>` under a v2 `<plugin>` wrapper in **v2 cache file only**. No-op if v2 missing,
/// corrupt, or bundle has no wrappers.
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
