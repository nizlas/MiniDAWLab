#pragma once

// =============================================================================
// ProxyAssetStore — P1F project-relative immutable proxy storage + atomic publication
// (steering docs/PORTABLE_INSTRUMENTS_AND_PROXIES.md §16, PID-003, PI-007/024/025/028)
// =============================================================================
// Layout (Locked, §16.1): `<ProjectFolder>/InstrumentProxies/` — a sibling of
// Audio/ (which has user-clip semantics baked into save validation and must not
// hold proxies). Generation files are content-addressed and IMMUTABLE:
//
//   InstrumentProxies/track_<TrackId>_<sanitized-fingerprint>.wav
//
// Temporary render targets live in the SAME directory (same filesystem/volume ⇒
// same-volume rename) and never match a generation name:
//
//   InstrumentProxies/tmp_track_<TrackId>_gen<generation>_<millis>.wav
//
// Publication (Locked sequence, §16.3): render → close writer → reopen+validate
// → verify job currency (scheduler, message thread) → moveFileTo the immutable
// final name → re-validate → update in-memory metadata → expose Published only
// after the metadata update. `juce::File::moveFileTo` is MoveFile/rename on
// Windows — the strongest same-volume primitive available through JUCE. Final
// generation names are new/unique, so publication never replaces a file an
// open reader could hold (§16.2); the previously published generation is NEVER
// deleted or overwritten by this header.
//
// Crash windows (§16.4): before the rename a tmp_ orphan may remain; after the
// rename but before a project save an unreferenced immutable generation may
// remain. Both leave the previous referenced generation intact. Cleanup here is
// CONSERVATIVE: only tmp_*.wav files not owned by an active job are swept;
// generation files are only ENUMERATED/reported — never deleted (referenced-by-
// loaded-project / on-disk-project / active-job ownership cannot be fully
// established in P1F, so we do not guess).
//
// Thread affinity: everything in this header runs on the MESSAGE thread (§14.1
// publication/metadata row). File operations never run on the audio thread.
// Header-only (juce_core + juce_audio_formats + the P1D validator) so the
// deterministic selftests cover the complete transaction on real files.

#include "instruments/ProxyFingerprint.h"
#include "instruments/ProxyRenderExecutor.h" // validateTemporaryWav (§8 validator)
#include "instruments/ProxyRenderSnapshot.h" // SnapshotPolicies
#include "instruments/ProxyRenderTypes.h"
#include "io/ProjectFile.h" // ProjectFileProxyMetadataV20

#include <juce_core/juce_core.h>

#include <cstdint>
#include <vector>

namespace proxy_store
{

inline constexpr const char* kProxyFolderName = "InstrumentProxies";

//==============================================================================
// Naming (stable, ASCII/filesystem-safe, TrackId-owned, collision-resistant,
// display-name independent, immutable after publication)
//==============================================================================
/// Filename-safe fingerprint: "sha256:<hex>" → "sha256_<hex>"; any character
/// outside [A-Za-z0-9._-] maps to '_' (ASCII-only, Windows-safe).
[[nodiscard]] inline juce::String sanitizeFingerprintForFileName(const juce::String& fingerprint)
{
    juce::String out;
    out.preallocateBytes((size_t)fingerprint.length());
    for (const auto ch : fingerprint)
    {
        const bool safe = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z')
                          || (ch >= '0' && ch <= '9') || ch == '.' || ch == '-' || ch == '_';
        out << (safe ? juce::String::charToString(ch) : juce::String("_"));
    }
    return out;
}

[[nodiscard]] inline juce::String generationFileName(const TrackId trackId,
                                                     const juce::String& fingerprint)
{
    return "track_" + juce::String((juce::int64)trackId) + "_"
           + sanitizeFingerprintForFileName(fingerprint) + ".wav";
}

/// Project-relative pointer stored in v20 metadata (forward slashes, §12.2).
[[nodiscard]] inline juce::String generationRelativePath(const TrackId trackId,
                                                         const juce::String& fingerprint)
{
    return juce::String(kProxyFolderName) + "/" + generationFileName(trackId, fingerprint);
}

[[nodiscard]] inline juce::File proxyDirectory(const juce::File& projectFolder)
{
    return projectFolder.getChildFile(kProxyFolderName);
}

/// Unique temporary render target in the SAME directory tree as the final asset
/// (same-volume rename guarantee). Generation + time salt prevent collisions
/// between jobs; the tmp_ prefix never matches a generation name.
[[nodiscard]] inline juce::File tempRenderTarget(const juce::File& projectFolder,
                                                 const TrackId trackId,
                                                 const std::uint64_t generation)
{
    return proxyDirectory(projectFolder)
        .getChildFile("tmp_track_" + juce::String((juce::int64)trackId) + "_gen"
                      + juce::String((juce::int64)generation) + "_"
                      + juce::String(juce::Time::currentTimeMillis()) + ".wav");
}

//==============================================================================
// Relative-path safety (§12.3 — loaded metadata must never escape the project)
//==============================================================================
/// Accepts exactly `InstrumentProxies/<safe-file>.wav`. Rejects traversal
/// (`..`), alternate roots, drive prefixes/colons (ADS), backslashes, absolute
/// paths, empty segments and non-ASCII/unsafe characters.
[[nodiscard]] inline bool isSafeProxyRelativePath(const juce::String& relativePath)
{
    if (relativePath.isEmpty() || relativePath.length() > 512)
    {
        return false;
    }
    // ASCII whitelist scan (also excludes ':', '\\', control chars, unicode tricks).
    for (const auto ch : relativePath)
    {
        const bool safe = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z')
                          || (ch >= '0' && ch <= '9') || ch == '.' || ch == '-' || ch == '_'
                          || ch == '/';
        if (!safe)
        {
            return false;
        }
    }
    juce::StringArray segments;
    segments.addTokens(relativePath, "/", {});
    if (segments.size() != 2) // exactly InstrumentProxies/<file>
    {
        return false;
    }
    if (segments[0] != juce::String(kProxyFolderName))
    {
        return false;
    }
    const juce::String& file = segments[1];
    if (file.isEmpty() || file == "." || file == ".." || file.contains("..")
        || !file.endsWith(".wav") || file.startsWith("."))
    {
        return false;
    }
    return true;
}

/// Resolve a VALIDATED relative path below the project folder. Returns an
/// invalid File for unsafe input (defense in depth on top of the validator).
[[nodiscard]] inline juce::File resolveProxyRelativePath(const juce::File& projectFolder,
                                                         const juce::String& relativePath)
{
    if (projectFolder == juce::File() || !isSafeProxyRelativePath(relativePath))
    {
        return {};
    }
    juce::File f = projectFolder;
    juce::StringArray segments;
    segments.addTokens(relativePath, "/", {});
    for (const auto& s : segments)
    {
        f = f.getChildFile(s);
    }
    // Normalization tricks defense: the resolved path must stay below the root.
    if (!f.getFullPathName().startsWith(projectFolder.getFullPathName()))
    {
        return {};
    }
    return f;
}

//==============================================================================
// Publication transaction (§16.3)
//==============================================================================
struct ProxyPublishOutcome
{
    bool ok = false;
    bool reusedExistingIdentical = false; ///< identical immutable generation already on disk
    bool waitingForProjectLocation = false; ///< unsaved project: nothing publishable yet
    juce::String error;
    juce::File finalFile;
    ProjectFileProxyMetadataV20 metadata; ///< filled on success (ok == true)
};

[[nodiscard]] inline ProjectFileProxyMetadataV20
    buildGenerationMetadata(const proxy_render::ProxyRenderResult& result,
                            const proxy_snapshot::SnapshotPolicies& policies,
                            const juce::String& relativePath,
                            const bool silentGeneration,
                            const proxy_snapshot::ProxyRenderSnapshot* identitySource = nullptr)
{
    ProjectFileProxyMetadataV20 m;
    m.generationId = result.expectedFingerprint;
    m.fingerprintSchemaVersion = (int)proxy_fingerprint::kFingerprintSchemaVersion;
    m.fingerprintAlgorithmId = (int)proxy_fingerprint::kFingerprintAlgorithmId;
    m.relativePath = relativePath; // empty for a silent generation (no fake paths)
    m.silentGeneration = silentGeneration;
    m.sampleRate = result.renderSampleRate;
    m.lengthSamples = silentGeneration ? 0 : result.renderedLengthSamples;
    m.channels = result.channels;
    m.pluginLatencySamples = juce::jmax(0, result.pluginLatencySamplesAtStart);
    m.latencyPolicyVersion = policies.latencyPolicyVersion;
    m.tailPolicyVersion = policies.tailPolicyVersion;
    m.renderPolicyVersion = policies.renderPolicyVersion;
    m.proxyFormatVersion = policies.proxyFormatVersion;
    m.renderedUtc = juce::Time::getCurrentTime().toISO8601(true);
    if (identitySource != nullptr)
    {
        // P1G (§12.3): record the exact non-musical fingerprint inputs so currency can be
        // recomputed under this generation's recorded configuration when Primary is missing.
        m.pluginFileOrIdentifier = identitySource->pluginIdentity.fileOrIdentifier;
        m.pluginUniqueId = identitySource->pluginIdentity.uniqueId;
        m.pluginDeprecatedUid = identitySource->pluginIdentity.deprecatedUid;
        m.pluginFormatName = identitySource->pluginIdentity.format;
        m.pluginIsInstrument = identitySource->pluginIdentity.isInstrument;
        m.pluginVersionAtRender = identitySource->pluginIdentity.version;
        m.primaryStateRevisionAtPublish
            = (std::int64_t)identitySource->stateIdentity.primaryStateRevision;
        m.pairedWithSavedStateAtRender = identitySource->stateIdentity.pairedWithSavedState;
        m.timelineReferenceRate = identitySource->renderConfig.timelineReferenceRate;
        m.renderBlockSize = identitySource->renderConfig.renderBlockSize;
        m.noteOffGateMs = identitySource->renderConfig.noteOffGateMs;
    }
    return m;
}

/// [Message thread] Publish one successful, validated, still-current render.
/// The caller (scheduler finalize) has already verified job currency (PI-028).
/// On success the temp file has been consumed (renamed or deleted-after-reuse).
/// On ANY failure the temp file is left for the caller's cleanup, no existing
/// file has been overwritten, and the previously published generation (metadata
/// AND asset) is untouched — retention is structural, not a recovery step.
[[nodiscard]] inline ProxyPublishOutcome
    publishRenderedProxy(const juce::File& projectFolder,
                         const TrackId trackId,
                         const proxy_render::ProxyRenderResult& result,
                         const proxy_snapshot::SnapshotPolicies& policies,
                         const proxy_snapshot::ProxyRenderSnapshot* identitySource = nullptr)
{
    ProxyPublishOutcome out;

    // Unsaved project (§ Unsaved projects): never invent a permanent path.
    if (projectFolder == juce::File() || !projectFolder.isDirectory())
    {
        out.waitingForProjectLocation = true;
        out.error = "WaitingForProjectLocation: the project has no saved directory yet";
        return out;
    }
    if (result.status != proxy_render::ProxyRenderStatus::Succeeded)
    {
        out.error = "only a Succeeded render result is publishable";
        return out;
    }
    const juce::File temp = result.temporaryWavFile;
    if (temp == juce::File() || !temp.existsAsFile())
    {
        out.error = "temporary render artifact is missing";
        return out;
    }

    const juce::File dir = proxyDirectory(projectFolder);
    (void)dir.createDirectory();
    const juce::String relativePath = generationRelativePath(trackId, result.expectedFingerprint);
    jassert(isSafeProxyRelativePath(relativePath));
    const juce::File finalFile = dir.getChildFile(generationFileName(trackId,
                                                                    result.expectedFingerprint));
    // Same-volume guarantee: temp targets are created inside the proxy directory
    // (tempRenderTarget) so the §16.3 rename below is a same-volume MoveFile.
    jassert(temp.getParentDirectory() == dir);

    if (finalFile.isDirectory())
    {
        // Never remove/overwrite a foreign object occupying the generation name
        // (juce::File::moveFileTo would delete an empty directory target).
        out.error = "generation name is blocked by a foreign directory";
        return out;
    }
    if (finalFile.existsAsFile())
    {
        // Identical immutable generation already published (same content address):
        // validate before safe reuse; NEVER overwrite an unrelated/corrupt file.
        const auto v = proxy_render::validateTemporaryWav(finalFile, result.renderSampleRate,
                                                          result.channels,
                                                          result.renderedLengthSamples);
        if (!v.ok)
        {
            out.error = "generation-name collision with a non-identical existing file: "
                        + v.error;
            return out;
        }
        (void)temp.deleteFile(); // reuse the existing asset; drop the duplicate temp
        out.reusedExistingIdentical = true;
    }
    else
    {
        // §16.3 step 3: same-volume rename to the immutable final name (Windows
        // MoveFile — new unique name ⇒ no open-handle replacement, §16.2).
        if (!temp.moveFileTo(finalFile))
        {
            out.error = "rename to the immutable generation name failed";
            return out;
        }
        // Verify the final asset (§16.3 step 6 / task §8 order).
        const auto v = proxy_render::validateTemporaryWav(finalFile, result.renderSampleRate,
                                                          result.channels,
                                                          result.renderedLengthSamples);
        if (!v.ok)
        {
            // The moved file is invalid — it carries no metadata reference yet, so
            // removing it is safe and prevents a corrupt immutable generation.
            (void)finalFile.deleteFile();
            out.error = "final asset failed validation after rename: " + v.error;
            return out;
        }
    }

    out.ok = true;
    out.finalFile = finalFile;
    out.metadata = buildGenerationMetadata(result, policies, relativePath,
                                           /*silentGeneration*/ false, identitySource);
    return out;
}

/// [Message thread] §15.7 explicit silent generation: metadata WITHOUT a WAV —
/// no zero-filled file, no fake path (relativePath stays empty, the explicit
/// silentGeneration flag makes the metadata unambiguous).
[[nodiscard]] inline ProxyPublishOutcome
    publishSilentGeneration(const proxy_render::ProxyRenderResult& result,
                            const proxy_snapshot::SnapshotPolicies& policies,
                            const proxy_snapshot::ProxyRenderSnapshot* identitySource = nullptr)
{
    ProxyPublishOutcome out;
    if (result.status != proxy_render::ProxyRenderStatus::SucceededSilent)
    {
        out.error = "not an explicit silent generation result";
        return out;
    }
    out.ok = true;
    out.metadata = buildGenerationMetadata(result, policies, {}, /*silentGeneration*/ true,
                                           identitySource);
    return out;
}

//==============================================================================
// Load-time / diagnostic validation (PI-025 degradation input)
//==============================================================================
struct ProxyAssetCheck
{
    bool ok = false;
    bool missing = false;
    juce::String error;
    juce::File file;
};

/// [Message thread] Validate published metadata against the on-disk asset.
/// Missing/corrupt ⇒ degrade (report), never fail a load (PI-025).
[[nodiscard]] inline ProxyAssetCheck
    validatePublishedAsset(const juce::File& projectFolder,
                           const ProjectFileProxyMetadataV20& metadata)
{
    ProxyAssetCheck c;
    if (metadata.silentGeneration)
    {
        c.ok = metadata.relativePath.isEmpty(); // silent generations carry no path
        if (!c.ok)
        {
            c.error = "silent generation must not carry an asset path";
        }
        return c;
    }
    if (!isSafeProxyRelativePath(metadata.relativePath))
    {
        c.error = "unsafe or malformed proxy relative path";
        return c;
    }
    c.file = resolveProxyRelativePath(projectFolder, metadata.relativePath);
    if (c.file == juce::File() || !c.file.existsAsFile())
    {
        c.missing = true;
        c.error = "proxy asset missing on disk";
        return c;
    }
    const auto v = proxy_render::validateTemporaryWav(c.file, metadata.sampleRate,
                                                      metadata.channels,
                                                      metadata.lengthSamples);
    if (!v.ok)
    {
        c.error = "proxy asset failed validation: " + v.error;
        return c;
    }
    c.ok = true;
    return c;
}

//==============================================================================
// P1H Save As rehoming (§16.6): copy referenced generations into a NEW project
// folder. The persisted relative path is content-addressed and project-relative,
// so it is identical in the new layout — nothing in the metadata changes; the
// gate is copy + validation. The ORIGINAL project and its assets are read-only
// inputs here (copy, never move). Failures are honest nonfatal states: the new
// project simply shows ProxyMissing for that destination (PI-025 degradation).
//==============================================================================
struct ProxyRehomeItem
{
    TrackId trackId = kInvalidTrackId;
    ProjectFileProxyMetadataV20 metadata; ///< referenced generation (by value)
    juce::File sourceFile;                ///< last known absolute asset location
};

struct ProxyRehomeOutcome
{
    int copied = 0;          ///< assets copied + validated into the new layout
    int alreadyPresent = 0;  ///< identical asset already present and valid
    int silent = 0;          ///< silent generations (nothing to copy by design)
    juce::StringArray errors; ///< per-item honest failures (nonfatal)
    [[nodiscard]] bool allOk() const noexcept { return errors.isEmpty(); }
};

/// [Message thread] Rehome every referenced generation into `newProjectFolder`
/// using the §16.3 discipline: copy to a unique temp sibling, validate against
/// the metadata contract, then same-volume rename to the immutable final name.
/// A partially copied temp never survives; an existing valid identical asset is
/// reused; an existing NON-matching file under the generation name is an error
/// (never overwritten).
[[nodiscard]] inline ProxyRehomeOutcome
    rehomeProxyAssets(const juce::File& newProjectFolder,
                      const std::vector<ProxyRehomeItem>& items)
{
    ProxyRehomeOutcome out;
    if (newProjectFolder == juce::File() || !newProjectFolder.isDirectory())
    {
        out.errors.add("rehome target project folder does not exist");
        return out;
    }
    std::uint64_t salt = 0;
    for (const auto& item : items)
    {
        const auto& meta = item.metadata;
        if (meta.silentGeneration)
        {
            ++out.silent; // metadata-only generation: nothing to copy (§15.7)
            continue;
        }
        const juce::String label = "track " + juce::String((juce::int64)item.trackId) + ": ";
        if (!isSafeProxyRelativePath(meta.relativePath))
        {
            out.errors.add(label + "unsafe relative path — not rehomed");
            continue;
        }
        const juce::File target = resolveProxyRelativePath(newProjectFolder, meta.relativePath);
        if (target == juce::File())
        {
            out.errors.add(label + "relative path did not resolve under the new project");
            continue;
        }
        if (target.existsAsFile())
        {
            const auto v = proxy_render::validateTemporaryWav(target, meta.sampleRate,
                                                              meta.channels,
                                                              meta.lengthSamples);
            if (v.ok)
            {
                ++out.alreadyPresent; // identical immutable generation already in place
            }
            else
            {
                out.errors.add(label + "target name occupied by a non-matching file: "
                               + v.error);
            }
            continue;
        }
        if (item.sourceFile == juce::File() || !item.sourceFile.existsAsFile())
        {
            out.errors.add(label + "source asset unknown or missing — stays honest-missing");
            continue;
        }
        (void)target.getParentDirectory().createDirectory();
        const juce::File temp = target.getParentDirectory().getChildFile(
            "tmp_rehome_" + juce::String((juce::int64)item.trackId) + "_"
            + juce::String((juce::int64)++salt) + "_"
            + juce::String(juce::Time::currentTimeMillis()) + ".wav");
        if (!item.sourceFile.copyFileTo(temp))
        {
            (void)temp.deleteFile();
            out.errors.add(label + "copy into the new project failed");
            continue;
        }
        const auto v = proxy_render::validateTemporaryWav(temp, meta.sampleRate, meta.channels,
                                                          meta.lengthSamples);
        if (!v.ok)
        {
            (void)temp.deleteFile();
            out.errors.add(label + "copied asset failed validation: " + v.error);
            continue;
        }
        if (target.isDirectory() || !temp.moveFileTo(target))
        {
            (void)temp.deleteFile();
            out.errors.add(label + "rename to the immutable generation name failed");
            continue;
        }
        ++out.copied;
    }
    return out;
}

//==============================================================================
// Conservative cleanup (§16.4): temporaries only; generations are reported.
//==============================================================================
/// [Message thread] Delete orphan `tmp_*.wav` files not owned by an active job.
/// Returns the number of files removed. Never touches generation files.
inline int sweepOrphanTemporaries(const juce::File& projectFolder,
                                  const juce::StringArray& activeTempFileNames)
{
    const juce::File dir = proxyDirectory(projectFolder);
    if (!dir.isDirectory())
    {
        return 0;
    }
    int removed = 0;
    for (const auto& f : dir.findChildFiles(juce::File::findFiles, false, "tmp_*.wav"))
    {
        if (!activeTempFileNames.contains(f.getFileName()))
        {
            if (f.deleteFile())
            {
                ++removed;
            }
        }
    }
    return removed;
}

/// [Message thread] REPORT-ONLY enumeration of generation files not referenced
/// by the provided relative paths (loaded project + retained previous). Safe
/// general deletion needs ownership knowledge P1F does not have — we never
/// remove an asset whose ownership cannot be established (task contract).
[[nodiscard]] inline juce::StringArray
    listUnreferencedGenerationFiles(const juce::File& projectFolder,
                                    const juce::StringArray& referencedRelativePaths)
{
    juce::StringArray orphans;
    const juce::File dir = proxyDirectory(projectFolder);
    if (!dir.isDirectory())
    {
        return orphans;
    }
    for (const auto& f : dir.findChildFiles(juce::File::findFiles, false, "track_*.wav"))
    {
        const juce::String rel = juce::String(kProxyFolderName) + "/" + f.getFileName();
        if (!referencedRelativePaths.contains(rel))
        {
            orphans.add(rel);
        }
    }
    return orphans;
}

} // namespace proxy_store
