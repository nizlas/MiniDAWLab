#pragma once

// =============================================================================
// ProxyMetadataCheckpoint — P1 acceptance correction (steering
// docs/PORTABLE_INSTRUMENTS_AND_PROXIES.md §18.3/§18.4): automatic
// metadata-only persistence of a newly PUBLISHED proxy generation into the
// main `.dalproj`, without ever silently saving unrelated user edits.
//
// WHY THIS EXISTS: publication updates the controller's in-memory proxy
// metadata immediately (runtime use is immediate), but the `.dalproj`
// reference used to wait for the NEXT user Save/autosave/close. For On Save
// mode that meant a second Save, and a whole-folder copy taken between
// publication and that Save carried a stale reference. This checkpoint writes
// the reference automatically WHEN IT IS PROVABLY SAFE.
//
// SAFETY DESIGN (why this can never capture unrelated edits): the checkpoint
// NEVER serializes the live session. It re-reads the last successfully saved
// on-disk project representation, replaces ONLY the matching experimental
// track's proxy metadata object, and atomically republishes the file through
// the same Windows-safe writer the normal Save path uses
// (`writeProjectFile` → parse-back validation → temp + rename). Everything
// else in the file — musical data, plugin descriptors and opaque state blobs,
// other tracks' proxy entries, window/workspace fields — round-trips from the
// saved representation untouched. Unsaved user edits live only in the session
// and therefore CANNOT leak into the write.
//
// GUARD (decided by the caller via `checkpointRefusalReason`): the write is
// allowed only when the project has a real on-disk file, the project is NOT
// dirty (no user/musical/plugin edit since the last successful Save/load —
// publication itself no longer marks dirty when the checkpoint succeeds), and
// the file's current SHA-256 equals the identity recorded at the last
// successful Save/load/checkpoint (external modification / replacement
// detection). Refusal is never an error: the metadata stays in controller
// memory and the next explicit user Save persists it (existing behavior).
//
// THREADING: message thread only (same discipline as ProjectIoCoordinator's
// save path). Serialization of near-simultaneous publications is therefore
// structural: each checkpoint re-reads the file the previous one just wrote.
// =============================================================================

#include "domain/Track.h"
#include "io/ProjectFile.h"

#include <juce_core/juce_core.h>
#include <juce_cryptography/juce_cryptography.h>

namespace proxy_checkpoint
{

/// Streaming SHA-256 of a file ("" when unreadable). Never loads the whole file.
[[nodiscard]] inline juce::String sha256HexOfFileForCheckpoint(const juce::File& f)
{
    juce::FileInputStream in(f);
    if (!in.openedOk())
    {
        return {};
    }
    juce::SHA256 sha(in);
    return sha.toHexString();
}

//==============================================================================
// Guard decision (pure; deterministic-testable)
//==============================================================================

/// Inputs for the "may this metadata checkpoint write automatically?" decision.
/// All values are captured by the caller ON THE MESSAGE THREAD right before the
/// write attempt (no TOCTOU beyond the atomic transaction itself, which
/// re-verifies the disk identity again).
struct CheckpointGuardState
{
    /// Session has a real main `.dalproj` path (false for never-saved projects
    /// and after autosave recovery detached the save path).
    bool hasProjectFile = false;
    /// That file currently exists on disk.
    bool projectFileExists = false;
    /// ProjectIoCoordinator::isProjectDirty(): any user/musical/plugin edit
    /// since the last successful Save/load. Publication itself must NOT have
    /// set this (the caller checkpoints BEFORE any dirty fallback marking).
    bool projectDirty = false;
    /// SHA-256 recorded after the last successful Save/load/checkpoint.
    juce::String knownDiskIdentity;
    /// SHA-256 of the file as it is on disk right now.
    juce::String actualDiskIdentity;
};

/// Empty result = the automatic checkpoint may proceed. A non-empty result is a
/// short actionable reason; the caller keeps the metadata pending for the next
/// explicit user Save (never an error, never silent — the caller logs it).
[[nodiscard]] inline juce::String checkpointRefusalReason(const CheckpointGuardState& g)
{
    if (!g.hasProjectFile)
    {
        return "project has no saved file yet (Save As pending)";
    }
    if (!g.projectFileExists)
    {
        return "main project file is missing on disk";
    }
    if (g.projectDirty)
    {
        return "unsaved user edits exist (metadata persists on the next Save)";
    }
    if (g.knownDiskIdentity.isEmpty())
    {
        return "no recorded disk identity for the project file";
    }
    if (g.actualDiskIdentity.isEmpty())
    {
        return "project file on disk is unreadable";
    }
    if (g.actualDiskIdentity != g.knownDiskIdentity)
    {
        return "project file changed on disk outside this session";
    }
    return {};
}

//==============================================================================
// Atomic metadata-only transaction (message thread)
//==============================================================================

struct CheckpointOutcome
{
    bool ok = false;
    /// Failure detail (actionable; logged by the caller). Empty on success.
    juce::String error;
    /// SHA-256 of the project file after the successful replace (the caller
    /// records it as the new known disk identity).
    juce::String newDiskIdentity;
};

/// Metadata-only atomic update of `projectFile`:
///   1. re-verify the on-disk SHA-256 equals `expectedDiskIdentity`;
///   2. read the saved project (the LAST SUCCESSFULLY SAVED representation —
///      never the live session);
///   3. replace ONLY the matching experimental track's proxy metadata;
///   4. write + fully re-read a temporary sibling file as validation;
///   5. atomically replace the main file through `writeProjectFile` (the
///      normal Save writer: parse-back validation + temp + rename);
///   6. report the new disk identity.
/// Failure at ANY step leaves the previous `.dalproj` intact (the writer's
/// temp+rename discipline), never touches published WAVs, and never deletes a
/// previous generation. This function creates no undo entry, never marks the
/// project clean/dirty, and never fires save callbacks — recursion into On
/// Save rendering is structurally impossible.
[[nodiscard]] inline CheckpointOutcome checkpointProxyMetadataOnDisk(
    const juce::File& projectFile,
    const juce::String& expectedDiskIdentity,
    const TrackId trackId,
    const ProjectFileProxyMetadataV20& metadata)
{
    CheckpointOutcome out;
    if (metadata.generationId.isEmpty())
    {
        out.error = "published metadata has no generation id";
        return out;
    }
    if (expectedDiskIdentity.isEmpty()
        || sha256HexOfFileForCheckpoint(projectFile) != expectedDiskIdentity)
    {
        out.error = "disk identity mismatch (file changed since the last Save)";
        return out;
    }

    ProjectFileV1 data;
    {
        const juce::Result r = readProjectFile(projectFile, data);
        if (!r.wasOk())
        {
            out.error = "could not read saved project: " + r.getErrorMessage();
            return out;
        }
    }

    ProjectFileExperimentalInstrumentTrackV1* target = nullptr;
    for (auto& et : data.experimentalInstrumentTracks)
    {
        if (et.trackId == trackId)
        {
            target = &et;
            break;
        }
    }
    if (target == nullptr)
    {
        out.error = "destination track " + juce::String((juce::int64)trackId)
                    + " is not part of the saved project";
        return out;
    }
    target->hasProxy = true;
    target->proxy = metadata;

    // Validation write: full write + full re-read of a sibling temp file BEFORE
    // touching the main file. Never reuses the writer's internal temp name.
    const juce::File validationFile
        = projectFile.getSiblingFile(projectFile.getFileNameWithoutExtension()
                                     + ".proxymeta-validate.tmp");
    if (validationFile.existsAsFile() && !validationFile.deleteFile())
    {
        out.error = "could not clear stale validation file: "
                    + validationFile.getFullPathName();
        return out;
    }
    {
        const juce::Result w = writeProjectFile(validationFile, data);
        if (!w.wasOk())
        {
            (void)validationFile.deleteFile();
            out.error = "validation write failed: " + w.getErrorMessage();
            return out;
        }
        ProjectFileV1 check;
        const juce::Result r = readProjectFile(validationFile, check);
        bool roundTripOk = r.wasOk();
        if (roundTripOk)
        {
            roundTripOk = false;
            for (const auto& et : check.experimentalInstrumentTracks)
            {
                if (et.trackId == trackId)
                {
                    roundTripOk = et.hasProxy && et.proxy.generationId == metadata.generationId;
                    break;
                }
            }
        }
        (void)validationFile.deleteFile();
        if (!roundTripOk)
        {
            out.error = "validation re-read failed"
                        + (r.wasOk() ? juce::String() : (": " + r.getErrorMessage()));
            return out;
        }
    }

    // Atomic replace through the normal project writer (parse-back validation +
    // temp + rename; on failure the previous main file remains intact).
    {
        const juce::Result w = writeProjectFile(projectFile, data);
        if (!w.wasOk())
        {
            out.error = "atomic replace failed: " + w.getErrorMessage();
            return out;
        }
    }

    out.newDiskIdentity = sha256HexOfFileForCheckpoint(projectFile);
    if (out.newDiskIdentity.isEmpty())
    {
        out.error = "replaced file is unreadable";
        return out;
    }
    out.ok = true;
    return out;
}

} // namespace proxy_checkpoint
