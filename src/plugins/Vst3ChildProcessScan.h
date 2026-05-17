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
// Phase 2 — optional `<capabilities>` under v2 `<plugin>` (fingerprints, future per-track maps).
// Global v2 drum capabilities (rawPitchMap / drumNoteDisplay) are NOT canonical for production:
// they are ignored for automatic drum-row display. OOP / Rescan continue to refresh PluginDescription only.
// -----------------------------------------------------------------------------

struct RawPitchMapNoteEntry
{
    int midi = 0;
    juce::String name;
};

struct RawPitchMapCapability
{
    /// e.g. `iUnitInfoProgramPitchName`
    juce::String source;
    /// Selected program index used when harvesting names (-1 if unknown).
    int programIndex = -1;
    /// Typically pitch names for MIDI notes 24–51 when present; may include other notes.
    std::vector<RawPitchMapNoteEntry> notes;
};

struct DrumNoteDisplayActiveNote
{
    int midi = 0;
    /// Name from the plugin raw pitch map (`getProgramPitchName`) for this MIDI note.
    juce::String rawName;
};

struct DrumNoteDisplayCapability
{
    /// e.g. structural cluster reason from derivation, or `fallback`.
    juce::String derivation;
    /// e.g. `high` when `pluginDrumNameMapAuthoritative_` is true at persist time.
    juce::String confidence;
    /// Active display pad notes (e.g. 36–51 for Groove Agent SE).
    std::vector<DrumNoteDisplayActiveNote> activeNotes;
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

/// Windows: locate a **HALion Sonic-family** `.vst3` bundle under common VST3 folders (filename contains
/// "halion" and "sonic", case-insensitive), preferring `Steinberg\HALion Sonic.vst3` when present.
/// Empty file on other platforms or when not found.
[[nodiscard]] juce::File getHalionSonicVst3BundlePathForOopScanFallback() noexcept;

/// From any path inside a VST3 bundle layout, walk up to the directory whose name ends in `.vst3`
/// (the bundle root Steinberg/JUCE expect for hosting). Empty when none is found.
[[nodiscard]] juce::File normalizePathToVst3BundleRootDirectory(const juce::File& path) noexcept;

/// Normalize HALion `PluginDescription` paths for hosting: `originalPath` is the bundle root from the host;
/// sets `fileOrIdentifier` to the inner module when present (same pattern as Groove Agent SE repair).
void repairHalionPluginDescriptionForLoad(juce::PluginDescription& d, const juce::File& originalPath);

/// True when a host/plugin display name looks like the HALion Sonic product line (e.g. "HALion Sonic",
/// "HALion Sonic 7", "HALion Sonic SE"). Case-insensitive; requires both "halion" and "sonic" substrings.
[[nodiscard]] bool instrumentDisplayNameLooksLikeHalionSonic(const juce::String& name) noexcept;

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

/// Bootstrap / tests only: parsed `<capabilities>` XML from v2. **Not used for production drum UI**
/// (global cache is not authoritative; future model is per-track / project-local drum maps).
[[nodiscard]] bool tryLoadExperimentalVst3PluginCapabilitiesFromV2Cache(
    const juce::File& bundlePathKey,
    const juce::PluginDescription& forPlugin,
    PluginCapabilities& capsOut);

/// Try v2, then v1 (for UI badge / generic lookup). Does not run OOP scan.
[[nodiscard]] bool tryLoadExperimentalVst3DescriptionsFromCache(
    const juce::File& vst3Bundle,
    std::vector<juce::PluginDescription>& descriptionsOut);

/// Build independent Groove candidates from the v2 file and the v1 file (path repair in-memory only; never writes caches).
[[nodiscard]] bool tryLoadGrooveAgentCacheCandidates(const juce::File& savedOrAdvisoryBundle,
                                                     Vst3GrooveCacheLoadCandidate& v2Out,
                                                     Vst3GrooveCacheLoadCandidate& v1Out,
                                                     juce::String& infoOrWarningOut);

/// HALion Sonic: same cache/OOP strategy as Groove Agent SE, independent name scan + bundle fallback.
[[nodiscard]] bool tryLoadHalionSonicCacheCandidates(const juce::File& savedOrAdvisoryBundle,
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
/// corrupt, or bundle has no wrappers. Returns true only when the v2 cache file was replaced successfully.
/// Note: production drum-row UI **does not** read global v2 drum capabilities; callers that persist them are gated
/// (e.g. `kPersistGlobalDrumCapabilityHints` in `ExperimentalInstrumentHost`).
[[nodiscard]] bool mergeCapabilitiesIntoBundle(const juce::File& vst3Bundle,
                                              const juce::PluginDescription& forPlugin,
                                              const PluginCapabilities& caps);

/// Dev / CI: in-memory + temp-file checks for v1/v2 cache XML (does not touch the user cache file).
[[nodiscard]] bool verifyExperimentalVst3DescriptionsCachePhase2() noexcept;

/// Project-load helper for **GrooveAgentSE** / **HALionSonic**: load from cache (optional full-cache scan), and if the
/// cached bundle / `fileOrIdentifier` no longer exists, search standard Windows VST3 folders for
/// the known bundle name, patch `PluginDescription` paths in memory, and return the resolved bundle.
/// Does not run in-process `findAllTypesForFile` or raw OOP scan. Logs to `experimental-vst3-oop-scan.log`.
[[nodiscard]] bool tryLoadExperimentalVst3DescriptionsFromCacheWithPathRepair(
    const juce::File& savedOrAdvisoryBundle,
    const juce::String& instrumentKind,
    std::vector<juce::PluginDescription>& descriptionsOut,
    juce::File& resolvedBundleOut,
    juce::String& infoOrWarningOut,
    bool* pathRepairWasUsedOut = nullptr);

} // namespace mini_daw
