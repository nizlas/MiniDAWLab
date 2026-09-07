#pragma once

// =============================================================================
// PortableProjectService — P1J "Prepare Portable Project" (steering
// docs/PORTABLE_INSTRUMENTS_AND_PROXIES.md §16.6, PID-011 Locked, R12,
// invariants PI-023/PI-024/PI-026/PI-028/PI-030; acceptance §24.13, T-23/T-27).
//
// One explicit, cancellable operation that:
//   1. captures a coherent preparation identity (project file + per-destination
//      expected fingerprints + referenced media/proxy assets);
//   2. classifies every Primary instrument destination (T-23 verdicts);
//   3. renders required stale/missing/corrupt proxies through the existing P1E
//      scheduler when Primary is available — a MODE-INDEPENDENT one-shot that
//      never changes the persisted proxyUpdateMode (§18.1 exception, Locked);
//   4. reports unrenderable destinations as honest blockers (no package);
//   5. collects the project file, referenced Audio/ media and referenced
//      CURRENT proxy generations into a same-filesystem staging folder;
//   6. validates the completed staging copy (paths, checksums, proxy assets,
//      silent-generation rules, plugin/licence boundary);
//   7. re-verifies the preparation identity, then publishes the folder with an
//      atomic same-volume rename (§16.3 discipline: cancel/failure leaves no
//      partially published package).
//
// PACKAGE FORMAT (decided in-slice per PID-011 "Recommended"): a normal DAL
// project folder — the same layout the project already uses (project file +
// `Audio/` + `InstrumentProxies/`), no new archive container. Media and proxy
// references are ALREADY project-relative (enforced by Session / proxy_store),
// so the collected copy resolves within the portable root by construction; the
// project JSON is re-written (not byte-copied) through the normal writer with
// machine-absolute UI paths sanitized out.
//
// PLUGIN / LICENCE BOUNDARY (PI-026): the portable project keeps the plugin
// descriptors and opaque serialized plugin state that already belong to the
// musical project. It never copies plugin binaries, installation directories,
// licence-manager data, activation files or machine credentials — and the
// package validator REJECTS any binary-like file that would somehow appear.
// Opaque state blobs are never inspected or rewritten. Downstream insert
// effects are reported in the manifest as external requirements — P1 does not
// freeze effects (that is P3) and never claims complete sonic self-containment.
//
// THREADING: control state machine on the MESSAGE thread (a production
// juce::Timer calls tick(); tests call tick() directly). File collection and
// package validation run on ONE worker std::thread (never the message thread;
// progress via atomics, strings under a small mutex). Render work stays on the
// P1E scheduler's worker. The final identity re-check and the atomic rename
// happen back on the message thread. No UI pointers are held anywhere here.
// =============================================================================

#include "domain/Track.h"
#include "instruments/ProxyAssetStore.h"
#include "instruments/ProxyRenderScheduler.h"
#include "io/ProjectFile.h"

#include <juce_core/juce_core.h>
#include <juce_cryptography/juce_cryptography.h>
#include <juce_events/juce_events.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <utility>
#include <vector>

namespace portable_project
{

//==============================================================================
// Vocabulary
//==============================================================================

enum class PreparationPhase : int
{
    Idle = 0,
    ValidatingProject,
    WaitingForSnapshot,   ///< render(s) due; snapshot capture deferred (§9.4.4)
    WaitingForRecording,  ///< render(s) due; recording pauses eligibility (§14.3)
    RenderingProxies,
    CollectingMedia,
    WritingProject,
    ValidatingPackage,
    Publishing,
    Complete,
    Cancelled,
    Failed,
};

[[nodiscard]] inline const char* preparationPhaseName(const PreparationPhase p) noexcept
{
    switch (p)
    {
        case PreparationPhase::Idle: return "Idle";
        case PreparationPhase::ValidatingProject: return "Validating project";
        case PreparationPhase::WaitingForSnapshot: return "Waiting for snapshot";
        case PreparationPhase::WaitingForRecording: return "Waiting until recording stops";
        case PreparationPhase::RenderingProxies: return "Rendering required proxies";
        case PreparationPhase::CollectingMedia: return "Collecting media";
        case PreparationPhase::WritingProject: return "Writing portable project";
        case PreparationPhase::ValidatingPackage: return "Validating portable copy";
        case PreparationPhase::Publishing: return "Publishing";
        case PreparationPhase::Complete: return "Complete";
        case PreparationPhase::Cancelled: return "Cancelled";
        case PreparationPhase::Failed: return "Failed";
    }
    return "?";
}

/// T-23 per-destination readiness verdict at classification time.
enum class DestinationVerdict : int
{
    Ready = 0,     ///< Current generation, validated asset (or valid silent generation)
    NeedsRender,   ///< stale/missing/corrupt/failed proxy + Primary available
    RenderingNow,  ///< a render issued by THIS preparation is in flight
    Blocked,       ///< cannot become ready (see detail) — no package
};

struct DestinationReport
{
    TrackId trackId = kInvalidTrackId;
    juce::String name;
    DestinationVerdict verdict = DestinationVerdict::Blocked;
    /// Human-readable status/blocker detail (no fingerprints/generation ids).
    juce::String detail;
};

/// Immutable status copy for the UI (message thread).
struct PreparationStatus
{
    PreparationPhase phase = PreparationPhase::Idle;
    juce::String currentItem;       ///< current destination/file (human readable)
    int completedFiles = 0;
    int totalFiles = 0;
    std::vector<DestinationReport> destinations;
    juce::StringArray blockers;     ///< actionable blocker list (Failed)
    juce::String failureReason;     ///< non-blocker failure (I/O, conflict, changed project)
    juce::File finalFolder;         ///< valid in Complete
};

//==============================================================================
// Pure helpers (unit-tested directly; also used by the worker)
//==============================================================================

/// Safe portable-relative path: non-empty, forward slashes, no drive/root/UNC,
/// no `.`/`..` segments, and (when `requiredPrefix` non-empty) inside it.
[[nodiscard]] inline bool isSafePortableRelativePath(const juce::String& rel,
                                                     const juce::String& requiredPrefix)
{
    if (rel.isEmpty() || rel.containsChar('\\') || rel.containsChar(':')
        || rel.startsWithChar('/') || rel.endsWithChar('/'))
    {
        return false;
    }
    juce::StringArray segments;
    segments.addTokens(rel, "/", "");
    for (const auto& s : segments)
    {
        if (s.isEmpty() || s == "." || s == "..")
        {
            return false;
        }
    }
    return requiredPrefix.isEmpty() || rel.startsWith(requiredPrefix);
}

/// Streaming SHA-256 of a file ("" when unreadable). Never loads the whole file.
[[nodiscard]] inline juce::String sha256HexOfFile(const juce::File& f)
{
    juce::FileInputStream in(f);
    if (!in.openedOk())
    {
        return {};
    }
    juce::SHA256 sha(in);
    return sha.toHexString();
}

/// Chunked, cancellable copy with source-hash capture and destination re-read
/// verification (size + SHA-256). Never overwrites an existing destination.
[[nodiscard]] inline juce::Result copyFileVerified(const juce::File& src, const juce::File& dst,
                                                   const std::atomic<bool>& cancelled)
{
    if (!src.existsAsFile())
    {
        return juce::Result::fail("source missing: " + src.getFileName());
    }
    if (dst.exists())
    {
        return juce::Result::fail("destination already occupied: " + dst.getFileName());
    }
    if (!dst.getParentDirectory().createDirectory())
    {
        return juce::Result::fail("cannot create: " + dst.getParentDirectory().getFullPathName());
    }
    juce::String srcHashHex;
    {
        juce::FileInputStream in(src);
        if (!in.openedOk())
        {
            return juce::Result::fail("cannot read: " + src.getFileName());
        }
        juce::FileOutputStream out(dst);
        if (!out.openedOk())
        {
            return juce::Result::fail("cannot write: " + dst.getFileName());
        }
        // Manual chunk loop so cancellation reacts at safe boundaries (1 MiB chunks).
        juce::MemoryBlock chunk(1024 * 1024);
        for (;;)
        {
            if (cancelled.load(std::memory_order_relaxed))
            {
                out.flush();
                return juce::Result::fail("cancelled");
            }
            const int n = in.read(chunk.getData(), (int)chunk.getSize());
            if (n < 0)
            {
                return juce::Result::fail("read failed: " + src.getFileName());
            }
            if (n == 0)
            {
                break;
            }
            if (!out.write(chunk.getData(), (size_t)n))
            {
                return juce::Result::fail("write failed: " + dst.getFileName());
            }
        }
        out.flush();
    }
    srcHashHex = sha256HexOfFile(src);
    if (dst.getSize() != src.getSize())
    {
        return juce::Result::fail("size mismatch after copy: " + dst.getFileName());
    }
    if (srcHashHex.isEmpty() || sha256HexOfFile(dst) != srcHashHex)
    {
        return juce::Result::fail("checksum mismatch after copy: " + dst.getFileName());
    }
    return juce::Result::ok();
}

/// One referenced proxy asset to collect.
struct ProxyCollectionItem
{
    TrackId trackId = kInvalidTrackId;
    juce::String trackName;
    juce::String relativePath;   ///< empty for a valid silent generation
    juce::String generationId;   ///< captured expected fingerprint (currency proof)
    bool silentGeneration = false;
};

/// What the captured project generation actually references (media dedup'd,
/// deterministic order — never display names).
struct CollectionPlan
{
    juce::StringArray mediaRelativePaths;      ///< clip `sourcePath`s under `Audio/`
    std::vector<ProxyCollectionItem> proxies;  ///< referenced CURRENT generations only
    juce::StringArray problems;                ///< unsafe/invalid references (blockers)
};

/// Enumerate referenced media from the persisted project data. Proxy items are
/// added by the service from the CAPTURED destination metadata (not from disk
/// scans — "most recent file" is never proof of currency).
[[nodiscard]] inline CollectionPlan planMediaCollection(const ProjectFileV1& data)
{
    CollectionPlan plan;
    std::set<juce::String> seen;
    for (const auto& t : data.tracks)
    {
        for (const auto& c : t.clips)
        {
            if (c.sourcePath.isEmpty())
            {
                continue;
            }
            if (!isSafePortableRelativePath(c.sourcePath, "Audio/"))
            {
                plan.problems.add("unsafe media reference: " + c.sourcePath);
                continue;
            }
            if (seen.insert(c.sourcePath).second)
            {
                plan.mediaRelativePaths.add(c.sourcePath);
            }
        }
    }
    plan.mediaRelativePaths.sort(false); // deterministic collection order
    return plan;
}

/// Portable-copy sanitation: drop machine-absolute UI paths from the copy (the
/// SOURCE project is never modified). Currently: an absolute audio-mixdown
/// output directory resets to the project-relative default.
inline void sanitizePortableProjectData(ProjectFileV1& data)
{
    if (data.hasAudioMixdown
        && (data.audioMixdown.outputDirectory.containsChar(':')
            || data.audioMixdown.outputDirectory.startsWithChar('/')
            || data.audioMixdown.outputDirectory.startsWithChar('\\')))
    {
        data.audioMixdown.outputDirectory = "Mixdown";
    }
}

/// PI-026 boundary check: no plugin-binary/installer/licence-like file may
/// exist anywhere in the package. Collection never copies these; this makes the
/// boundary an enforced validation, not a convention.
[[nodiscard]] inline bool isForbiddenPackageFile(const juce::File& f)
{
    static const char* const kForbidden[]
        = { ".vst3", ".dll", ".exe", ".msi", ".dylib", ".so", ".vst", ".component", ".lic",
            ".license", ".key" };
    const juce::String ext = f.getFileExtension().toLowerCase();
    for (const char* const e : kForbidden)
    {
        if (ext == e)
        {
            return true;
        }
    }
    return false;
}

/// Full staged-package validation (also run by tests against arbitrary roots).
/// Returns an empty array when the package is valid. `expectedFingerprints` are
/// the CAPTURED preparation fingerprints per destination (currency proof).
[[nodiscard]] inline juce::StringArray
validatePortablePackage(const juce::File& root, const juce::String& projectFileName,
                        const std::vector<std::pair<TrackId, juce::String>>& expectedFingerprints)
{
    juce::StringArray problems;
    const juce::File projectFile = root.getChildFile(projectFileName);
    ProjectFileV1 data;
    if (!projectFile.existsAsFile())
    {
        problems.add("portable project file missing");
        return problems;
    }
    const juce::Result parsed = readProjectFile(projectFile, data);
    if (parsed.failed())
    {
        problems.add("portable project file does not load: " + parsed.getErrorMessage());
        return problems;
    }

    // Referenced media: safe path + present inside the root.
    for (const auto& t : data.tracks)
    {
        for (const auto& c : t.clips)
        {
            if (c.sourcePath.isEmpty())
            {
                continue;
            }
            if (!isSafePortableRelativePath(c.sourcePath, "Audio/"))
            {
                problems.add("unsafe media path in portable project: " + c.sourcePath);
                continue;
            }
            const juce::File media = root.getChildFile(c.sourcePath);
            if (!media.existsAsFile() || media.getSize() <= 0)
            {
                problems.add("referenced media missing in package: " + c.sourcePath);
            }
        }
    }

    // No absolute machine path may remain in known path-bearing UI fields.
    if (data.hasAudioMixdown
        && (data.audioMixdown.outputDirectory.containsChar(':')
            || data.audioMixdown.outputDirectory.startsWithChar('/')
            || data.audioMixdown.outputDirectory.startsWithChar('\\')))
    {
        problems.add("absolute mixdown output path remains in portable project");
    }

    // Required proxies: metadata present, fingerprint == captured, asset/silent
    // metadata validates via the authoritative proxy_store checks (§16).
    std::map<TrackId, const ProjectFileExperimentalInstrumentTrackV1*> byTrack;
    for (const auto& it : data.experimentalInstrumentTracks)
    {
        byTrack[it.trackId] = &it;
    }
    for (const auto& [tid, expectedFp] : expectedFingerprints)
    {
        const auto found = byTrack.find(tid);
        if (found == byTrack.end() || !found->second->hasProxy)
        {
            problems.add("required proxy metadata missing for track " + juce::String((int)tid));
            continue;
        }
        const ProjectFileProxyMetadataV20& meta = found->second->proxy;
        if (expectedFp.isNotEmpty() && meta.generationId != expectedFp)
        {
            problems.add("proxy generation is not current for track " + juce::String((int)tid));
            continue;
        }
        if (meta.silentGeneration)
        {
            if (meta.relativePath.isNotEmpty() || meta.lengthSamples != 0)
            {
                problems.add("invalid silent-generation metadata for track "
                             + juce::String((int)tid));
            }
            continue; // valid silent generation: no WAV by definition (§15.7)
        }
        if (!proxy_store::isSafeProxyRelativePath(meta.relativePath))
        {
            problems.add("unsafe proxy path in portable project: " + meta.relativePath);
            continue;
        }
        const auto check = proxy_store::validatePublishedAsset(root, meta);
        if (!check.ok)
        {
            problems.add("proxy asset invalid in package (track " + juce::String((int)tid)
                         + "): " + check.error);
        }
    }

    // PI-026: nothing binary/licence-like anywhere; no temp/derived-cache files.
    for (const auto& f : root.findChildFiles(juce::File::findFiles, true))
    {
        if (isForbiddenPackageFile(f))
        {
            problems.add("forbidden file in package: "
                         + f.getRelativePathFrom(root).replaceCharacter('\\', '/'));
        }
        if (f.getFileName().startsWith("tmp_") && f.getFileExtension() == ".wav")
        {
            problems.add("temporary render file in package: "
                         + f.getRelativePathFrom(root).replaceCharacter('\\', '/'));
        }
    }
    return problems;
}

//==============================================================================
// The preparation service
//==============================================================================

class PortablePreparationService final
{
public:
    /// All callables run on the MESSAGE thread (the worker never touches them).
    struct Dependencies
    {
        std::function<double()> nowMs;
        std::function<std::vector<TrackId>()> listDestinations;
        std::function<juce::String(TrackId)> destinationName;
        struct Identity
        {
            bool exists = false; ///< destination present AND renderable (usable Primary)
            juce::String fingerprint;
            std::uint64_t revision = 0;
        };
        std::function<Identity(TrackId)> identityForTrack;
        std::function<proxy_render::ProxyDestinationState(TrackId)> destinationState;
        std::function<proxy_render::ProxyJobStatus(TrackId)> jobStatus;
        std::function<proxy_render::ProxyJobStatus(TrackId)> requestRender;
        std::function<void(TrackId)> cancelDestination;
        /// §9.4.4 quiescence; null = always eligible.
        std::function<bool(TrackId)> snapshotEligible;
        /// Recording pauses render eligibility (§14.3); null = never recording.
        std::function<bool()> recordingActive;
        std::function<bool(TrackId, ProjectFileProxyMetadataV20&)> getProxyMetadata;
        std::function<juce::File()> getProjectFile;
        std::function<bool()> isProjectDirty;
        /// Persists post-render proxy metadata through the NORMAL save path
        /// before collection (§18.4 checkpoint; the user already confirmed the
        /// operation). Must not wait for rendering (none is active by then).
        std::function<bool()> saveProjectNow;
        /// Tests: run collection inline inside tick() (deterministic, no thread).
        bool runCollectionSynchronously = false;
    };

    explicit PortablePreparationService(Dependencies deps) : deps_(std::move(deps)) {}

    ~PortablePreparationService() { shutdownAndJoin(); }

    //==========================================================================
    // Control (message thread)
    //==========================================================================

    /// Begin preparing into `finalFolder` (must not exist yet — never
    /// destructively overwritten). Returns false when already running.
    bool start(const juce::File& finalFolder)
    {
        if (running())
        {
            return false;
        }
        joinWorker(); // reap any previous run's thread object
        resetRunState();
        finalFolder_ = finalFolder;
        stagingFolder_ = finalFolder.getSiblingFile(finalFolder.getFileName() + ".preparing");
        setPhase(PreparationPhase::ValidatingProject);
        startTicker();
        tick(); // immediate first evaluation (tests drive tick() directly)
        return true;
    }

    /// Cooperative cancel: preparation-issued renders are cancelled, the worker
    /// stops at its next safe boundary, staging is removed. Source files and any
    /// previously completed portable folder are untouched.
    void cancel()
    {
        if (!running())
        {
            return;
        }
        cancelled_.store(true, std::memory_order_relaxed);
        cancelPreparationRenders();
        if (!workerActive_.load(std::memory_order_acquire))
        {
            cleanupStaging();
            setPhase(PreparationPhase::Cancelled);
            stopTicker();
        }
        // else: tick() observes the worker exit and finishes the cancellation.
    }

    /// Bounded shutdown for project close / app exit: cancel, join, clean.
    void shutdownAndJoin()
    {
        cancelled_.store(true, std::memory_order_relaxed);
        if (running())
        {
            cancelPreparationRenders();
        }
        stopTicker();
        joinWorker();
        if (running())
        {
            cleanupStaging();
            setPhase(PreparationPhase::Cancelled);
        }
    }

    [[nodiscard]] bool running() const noexcept
    {
        switch (phase())
        {
            case PreparationPhase::Idle:
            case PreparationPhase::Complete:
            case PreparationPhase::Cancelled:
            case PreparationPhase::Failed:
                return false;
            default:
                return true;
        }
    }

    [[nodiscard]] PreparationStatus status() const
    {
        PreparationStatus out;
        out.phase = phase();
        out.completedFiles = completedFiles_.load(std::memory_order_relaxed);
        out.totalFiles = totalFiles_.load(std::memory_order_relaxed);
        {
            const std::lock_guard<std::mutex> lock(stringsMutex_);
            out.currentItem = currentItem_;
            out.blockers = blockers_;
            out.failureReason = failureReason_;
        }
        out.destinations = destinations_;
        out.finalFolder = finalFolder_;
        // Live render detail for the UI ("Rendering current proxy for Organ").
        if (out.phase == PreparationPhase::RenderingProxies && deps_.jobStatus)
        {
            for (auto& d : out.destinations)
            {
                if (d.verdict != DestinationVerdict::RenderingNow)
                {
                    continue;
                }
                const auto s = deps_.jobStatus(d.trackId);
                if (s.exists && s.phase == proxy_render::ProxyJobPhase::Rendering)
                {
                    d.detail = "Rendering ("
                               + juce::String((double)s.progressRenderedMs / 1000.0, 1)
                               + " s rendered)";
                }
            }
        }
        return out;
    }

    //==========================================================================
    // Tick (message thread; production timer or tests)
    //==========================================================================
    void tick()
    {
        switch (phase())
        {
            case PreparationPhase::ValidatingProject:
                tickValidate();
                break;
            case PreparationPhase::WaitingForSnapshot:
            case PreparationPhase::WaitingForRecording:
            case PreparationPhase::RenderingProxies:
                tickRendering();
                break;
            case PreparationPhase::CollectingMedia:
            case PreparationPhase::WritingProject:
            case PreparationPhase::ValidatingPackage:
                tickWorker();
                break;
            default:
                break;
        }
    }

    /// Production ticker (dedicated timer; never the UI paint tick).
    void startTicker(const int intervalMs = 250)
    {
        if (ticker_ == nullptr)
        {
            ticker_ = std::make_unique<Ticker>(*this);
        }
        ticker_->startTimer(intervalMs);
    }

    void stopTicker()
    {
        if (ticker_ != nullptr)
        {
            ticker_->stopTimer();
        }
    }

private:
    //==========================================================================
    // Phase 1: validate + classify + capture preparation identity
    //==========================================================================
    void tickValidate()
    {
        const juce::File projectFile = deps_.getProjectFile ? deps_.getProjectFile()
                                                            : juce::File();
        if (projectFile == juce::File() || !projectFile.existsAsFile())
        {
            fail("The project has never been saved. Save the project first.");
            return;
        }
        if (deps_.isProjectDirty && deps_.isProjectDirty())
        {
            fail("The project has unsaved changes. Save the project first.");
            return;
        }
        if (finalFolder_.exists())
        {
            fail("The destination \"" + finalFolder_.getFileName()
                 + "\" already exists. Choose a new folder name — nothing is overwritten.");
            return;
        }
        sourceProjectFile_ = projectFile;
        sourceProjectFolder_ = projectFile.getParentDirectory();

        // Classify every Primary instrument destination (T-23).
        destinations_.clear();
        expectedFingerprints_.clear();
        pendingRender_.clear();
        bool anyBlocker = false;
        const std::vector<TrackId> dests
            = deps_.listDestinations ? deps_.listDestinations() : std::vector<TrackId>{};
        for (const TrackId tid : dests)
        {
            DestinationReport rep;
            rep.trackId = tid;
            rep.name = deps_.destinationName ? deps_.destinationName(tid)
                                             : "Track " + juce::String((int)tid);
            const Dependencies::Identity id
                = deps_.identityForTrack ? deps_.identityForTrack(tid)
                                         : Dependencies::Identity{};
            const proxy_render::ProxyDestinationState st
                = deps_.destinationState ? deps_.destinationState(tid)
                                         : proxy_render::ProxyDestinationState::Absent;
            if (st == proxy_render::ProxyDestinationState::Current)
            {
                // Ready — Current for the expected identity (fingerprint-derived,
                // never "latest file"). Includes missing-Primary + current proxy
                // and current silent generations; the asset itself is validated
                // during collection/validation against the captured fingerprint.
                rep.verdict = DestinationVerdict::Ready;
                ProjectFileProxyMetadataV20 meta;
                const bool hasMeta
                    = deps_.getProxyMetadata && deps_.getProxyMetadata(tid, meta);
                rep.detail = hasMeta && meta.silentGeneration
                                 ? "Current proxy (intentional silence)"
                                 : "Current proxy";
                expectedFingerprints_.emplace_back(
                    tid, hasMeta ? meta.generationId : id.fingerprint);
            }
            else if (id.exists)
            {
                // Stale/missing/corrupt/failed + available Primary: explicit
                // one-shot render for this package — regardless of the persisted
                // update mode, which is NOT changed (§16.6/§18.1 Locked).
                rep.verdict = DestinationVerdict::NeedsRender;
                rep.detail = "Proxy will be rendered for this package";
                pendingRender_.push_back({ tid, false });
                expectedFingerprints_.emplace_back(tid, id.fingerprint);
            }
            else
            {
                rep.verdict = DestinationVerdict::Blocked;
                switch (st)
                {
                    case proxy_render::ProxyDestinationState::Stale:
                        rep.detail = "Proxy is out of date and cannot be re-rendered "
                                     "because the Primary instrument is unavailable";
                        break;
                    case proxy_render::ProxyDestinationState::Failed:
                        rep.detail = "The last proxy render failed and the Primary "
                                     "instrument is unavailable";
                        break;
                    default:
                        rep.detail = "Primary instrument unavailable and no current "
                                     "proxy exists";
                        break;
                }
                addBlocker(rep.name + ": " + rep.detail);
                anyBlocker = true;
            }
            destinations_.push_back(std::move(rep));
        }
        if (anyBlocker)
        {
            failWithBlockers();
            return;
        }
        setPhase(pendingRender_.empty() ? PreparationPhase::CollectingMedia
                                        : PreparationPhase::RenderingProxies);
        if (pendingRender_.empty())
        {
            beginCollection();
        }
        else
        {
            tickRendering();
        }
    }

    //==========================================================================
    // Phase 2: mode-independent one-shot renders through the P1E scheduler
    //==========================================================================
    void tickRendering()
    {
        if (cancelled_.load(std::memory_order_relaxed))
        {
            finishCancelNoWorker();
            return;
        }
        bool anyWaitingRecording = false;
        bool anyWaitingSnapshot = false;
        bool anyActive = false;
        for (auto& [tid, requested] : pendingRender_)
        {
            DestinationReport* const rep = reportFor(tid);
            if (rep == nullptr || rep->verdict == DestinationVerdict::Ready
                || rep->verdict == DestinationVerdict::Blocked)
            {
                continue;
            }
            if (!requested)
            {
                if (deps_.recordingActive && deps_.recordingActive())
                {
                    anyWaitingRecording = true;
                    rep->detail = "Waiting until recording stops";
                    continue;
                }
                if (deps_.snapshotEligible && !deps_.snapshotEligible(tid))
                {
                    anyWaitingSnapshot = true;
                    rep->detail = "Waiting for a quiet moment to capture the instrument";
                    continue;
                }
                const auto s = deps_.requestRender ? deps_.requestRender(tid)
                                                   : proxy_render::ProxyJobStatus{};
                if (!s.exists)
                {
                    rep->verdict = DestinationVerdict::Blocked;
                    rep->detail = "The proxy render could not be started"
                                  + (s.message.isNotEmpty() ? (": " + s.message)
                                                            : juce::String());
                    addBlocker(rep->name + ": " + rep->detail);
                    failWithBlockers();
                    return;
                }
                requested = true;
                rep->verdict = DestinationVerdict::RenderingNow;
                rep->detail = "Rendering current proxy";
                setCurrentItem("Rendering current proxy for " + rep->name);
                anyActive = true;
                continue;
            }
            // Requested: poll the job.
            const auto s = deps_.jobStatus ? deps_.jobStatus(tid)
                                           : proxy_render::ProxyJobStatus{};
            if (!s.exists)
            {
                anyActive = true; // status races a coalesced restart; keep waiting
                continue;
            }
            switch (s.phase)
            {
                case proxy_render::ProxyJobPhase::Published:
                {
                    // Verify the publication actually made the destination Current
                    // for the CAPTURED preparation fingerprint (PI-028: an obsolete
                    // or superseded result can never satisfy preparation).
                    const auto st = deps_.destinationState
                                        ? deps_.destinationState(tid)
                                        : proxy_render::ProxyDestinationState::Absent;
                    const Dependencies::Identity id
                        = deps_.identityForTrack ? deps_.identityForTrack(tid)
                                                 : Dependencies::Identity{};
                    if (st != proxy_render::ProxyDestinationState::Current
                        || id.fingerprint != capturedFingerprintFor(tid))
                    {
                        failProjectChanged();
                        return;
                    }
                    rep->verdict = DestinationVerdict::Ready;
                    rep->detail = "Current proxy (rendered for this package)";
                    break;
                }
                case proxy_render::ProxyJobPhase::Failed:
                {
                    juce::String reason = "Proxy render failed";
                    if (s.result.failureReason
                        == proxy_render::ProxyRenderFailureReason::TailLimitReached)
                    {
                        reason = "Proxy render reached the tail limit and was not "
                                 "accepted";
                    }
                    else if (s.message.isNotEmpty())
                    {
                        reason << " (" << s.message << ")";
                    }
                    rep->verdict = DestinationVerdict::Blocked;
                    rep->detail = reason;
                    addBlocker(rep->name + ": " + reason);
                    cancelPreparationRenders();
                    failWithBlockers();
                    return;
                }
                case proxy_render::ProxyJobPhase::Obsolete:
                    // A render-relevant edit superseded the job: the project no
                    // longer matches the captured preparation identity.
                    failProjectChanged();
                    return;
                case proxy_render::ProxyJobPhase::Cancelled:
                    if (!cancelled_.load(std::memory_order_relaxed))
                    {
                        rep->verdict = DestinationVerdict::Blocked;
                        rep->detail = "The proxy render was cancelled";
                        addBlocker(rep->name + ": " + rep->detail);
                        failWithBlockers();
                        return;
                    }
                    break;
                default:
                    anyActive = true;
                    break;
            }
        }

        // All pending renders resolved?
        for (const auto& [tid, requested] : pendingRender_)
        {
            const DestinationReport* const rep = reportFor(tid);
            if (rep != nullptr && rep->verdict != DestinationVerdict::Ready)
            {
                setPhase(anyActive ? PreparationPhase::RenderingProxies
                         : anyWaitingRecording
                             ? PreparationPhase::WaitingForRecording
                             : anyWaitingSnapshot ? PreparationPhase::WaitingForSnapshot
                                                  : PreparationPhase::RenderingProxies);
                return;
            }
        }

        // Renders complete. Re-verify the whole preparation identity, then persist
        // the freshly published metadata through the normal save path (§18.4) so
        // the collected project file references the new current generations.
        if (!verifyPreparationIdentityStillMatches())
        {
            failProjectChanged();
            return;
        }
        if (deps_.saveProjectNow && !deps_.saveProjectNow())
        {
            fail("The project could not be saved before collection.");
            return;
        }
        beginCollection();
    }

    //==========================================================================
    // Phase 3: collection + validation (worker) and publication (message thread)
    //==========================================================================
    void beginCollection()
    {
        // Refresh captured metadata AFTER rendering/saving: the collected proxy
        // items must be the exact published current generations.
        ProjectFileV1 data;
        const juce::Result parsed = readProjectFile(sourceProjectFile_, data);
        if (parsed.failed())
        {
            fail("The saved project could not be read: " + parsed.getErrorMessage());
            return;
        }
        CollectionPlan plan = planMediaCollection(data);
        for (const auto& [tid, expectedFp] : expectedFingerprints_)
        {
            ProjectFileProxyMetadataV20 meta;
            if (!deps_.getProxyMetadata || !deps_.getProxyMetadata(tid, meta)
                || meta.generationId != expectedFp)
            {
                failProjectChanged();
                return;
            }
            ProxyCollectionItem item;
            item.trackId = tid;
            item.trackName = deps_.destinationName ? deps_.destinationName(tid)
                                                   : juce::String((int)tid);
            item.relativePath = meta.relativePath;
            item.generationId = meta.generationId;
            item.silentGeneration = meta.silentGeneration;
            if (!item.silentGeneration
                && !proxy_store::isSafeProxyRelativePath(item.relativePath))
            {
                plan.problems.add("unsafe proxy reference: " + item.relativePath);
            }
            plan.proxies.push_back(std::move(item));
        }
        if (!plan.problems.isEmpty())
        {
            for (const auto& p : plan.problems)
            {
                addBlocker(p);
            }
            failWithBlockers();
            return;
        }

        // Missing referenced media is an honest blocker before any copy starts.
        bool mediaMissing = false;
        for (const auto& rel : plan.mediaRelativePaths)
        {
            if (!sourceProjectFolder_.getChildFile(rel).existsAsFile())
            {
                addBlocker("Referenced audio file is missing: " + rel);
                mediaMissing = true;
            }
        }
        if (mediaMissing)
        {
            failWithBlockers();
            return;
        }

        sanitizePortableProjectData(data);
        const int proxyFiles = (int)std::count_if(
            plan.proxies.begin(), plan.proxies.end(),
            [](const ProxyCollectionItem& p) { return !p.silentGeneration; });
        totalFiles_.store(1 + plan.mediaRelativePaths.size() + proxyFiles,
                          std::memory_order_relaxed);
        completedFiles_.store(0, std::memory_order_relaxed);
        setPhase(PreparationPhase::CollectingMedia);

        // Snapshot everything the worker needs BY VALUE (no message-thread state).
        WorkerInputs in;
        in.sourceFolder = sourceProjectFolder_;
        in.staging = stagingFolder_;
        in.projectFileName = sourceProjectFile_.getFileName();
        in.projectData = std::move(data);
        in.plan = std::move(plan);
        in.expectedFingerprints = expectedFingerprints_;
        in.manifest = buildManifestText(in.projectData, in.plan);

        workerDone_.store(false, std::memory_order_release);
        workerFailed_.store(false, std::memory_order_release);
        if (deps_.runCollectionSynchronously)
        {
            workerActive_.store(true, std::memory_order_release);
            runCollection(std::move(in));
            tickWorker();
            return;
        }
        joinWorker();
        workerActive_.store(true, std::memory_order_release);
        worker_ = std::thread([this, inputs = std::move(in)]() mutable {
            runCollection(std::move(inputs));
        });
    }

    void tickWorker()
    {
        if (!workerDone_.load(std::memory_order_acquire))
        {
            // Mirror the worker's coarse progress into the visible phase.
            const int wp = workerPhase_.load(std::memory_order_relaxed);
            if (!cancelled_.load(std::memory_order_relaxed))
            {
                setPhase(wp >= 2 ? PreparationPhase::ValidatingPackage
                         : wp >= 1 ? PreparationPhase::WritingProject
                                   : PreparationPhase::CollectingMedia);
            }
            return;
        }
        joinWorker();
        if (cancelled_.load(std::memory_order_relaxed))
        {
            cleanupStaging();
            setPhase(PreparationPhase::Cancelled);
            stopTicker();
            return;
        }
        if (workerFailed_.load(std::memory_order_acquire))
        {
            cleanupStaging();
            {
                const std::lock_guard<std::mutex> lock(stringsMutex_);
                if (failureReason_.isEmpty())
                {
                    failureReason_ = "Collection failed.";
                }
            }
            setPhase(PreparationPhase::Failed);
            stopTicker();
            return;
        }

        // Staging is complete and validated. Final identity re-check on the
        // message thread, then atomic same-volume publication (§16.3).
        setPhase(PreparationPhase::Publishing);
        if (!verifyPreparationIdentityStillMatches()
            || (deps_.isProjectDirty && deps_.isProjectDirty()))
        {
            cleanupStaging();
            failProjectChanged();
            return;
        }
        if (finalFolder_.exists())
        {
            cleanupStaging();
            fail("The destination \"" + finalFolder_.getFileName()
                 + "\" appeared during preparation. Nothing was overwritten.");
            return;
        }
        if (!stagingFolder_.moveFileTo(finalFolder_))
        {
            cleanupStaging();
            fail("The prepared folder could not be moved to its final name.");
            return;
        }
        setCurrentItem("Portable project ready");
        setPhase(PreparationPhase::Complete);
        stopTicker();
    }

    //==========================================================================
    // Worker body (no deps_ access; everything passed by value)
    //==========================================================================
    struct WorkerInputs
    {
        juce::File sourceFolder;
        juce::File staging;
        juce::String projectFileName;
        ProjectFileV1 projectData;
        CollectionPlan plan;
        std::vector<std::pair<TrackId, juce::String>> expectedFingerprints;
        juce::String manifest;
    };

    void runCollection(WorkerInputs in)
    {
        const auto finish = [this](const bool failed, const juce::String& reason) {
            if (failed)
            {
                const std::lock_guard<std::mutex> lock(stringsMutex_);
                failureReason_ = reason;
            }
            workerFailed_.store(failed, std::memory_order_release);
            workerDone_.store(true, std::memory_order_release);
            workerActive_.store(false, std::memory_order_release);
        };

        // Fresh staging directory (leftovers from a crashed run are swept).
        if (in.staging.exists() && !in.staging.deleteRecursively())
        {
            finish(true, "Could not clear the staging folder.");
            return;
        }
        if (!in.staging.createDirectory())
        {
            finish(true, "Could not create the staging folder.");
            return;
        }

        // --- media + proxy assets (CollectingMedia) ---
        workerPhase_.store(0, std::memory_order_relaxed);
        juce::StringArray allRel = in.plan.mediaRelativePaths;
        for (const auto& p : in.plan.proxies)
        {
            if (!p.silentGeneration)
            {
                allRel.add(p.relativePath);
            }
        }
        for (const auto& rel : allRel)
        {
            if (cancelled_.load(std::memory_order_relaxed))
            {
                finish(false, {});
                return;
            }
            setCurrentItem("Copying " + rel);
            const juce::Result r = copyFileVerified(in.sourceFolder.getChildFile(rel),
                                                    in.staging.getChildFile(rel), cancelled_);
            if (r.failed())
            {
                finish(!cancelled_.load(std::memory_order_relaxed),
                       "Copy failed — " + r.getErrorMessage());
                return;
            }
            completedFiles_.fetch_add(1, std::memory_order_relaxed);
        }

        // --- portable project file + manifest (WritingProject) ---
        workerPhase_.store(1, std::memory_order_relaxed);
        setCurrentItem("Writing " + in.projectFileName);
        const juce::Result wrote
            = writeProjectFile(in.staging.getChildFile(in.projectFileName), in.projectData);
        if (wrote.failed())
        {
            finish(true, "Could not write the portable project file: "
                             + wrote.getErrorMessage());
            return;
        }
        completedFiles_.fetch_add(1, std::memory_order_relaxed);
        (void)in.staging.getChildFile("PortablePreparationReport.txt")
            .replaceWithText(in.manifest);

        // --- full package validation (ValidatingPackage) ---
        workerPhase_.store(2, std::memory_order_relaxed);
        setCurrentItem("Validating portable copy");
        if (cancelled_.load(std::memory_order_relaxed))
        {
            finish(false, {});
            return;
        }
        const juce::StringArray problems = validatePortablePackage(
            in.staging, in.projectFileName, in.expectedFingerprints);
        if (!problems.isEmpty())
        {
            finish(true, "Portable copy failed validation — " + problems.joinIntoString("; "));
            return;
        }
        finish(false, {});
    }

    /// Human-readable manifest + explicit dependency summary (PI-026 §7 report).
    [[nodiscard]] juce::String buildManifestText(const ProjectFileV1& data,
                                                 const CollectionPlan& plan) const
    {
        juce::String m;
        m << "MiniDAWLab portable project\n";
        m << "Prepared: " << juce::Time::getCurrentTime().toISO8601(true) << "\n";
        m << "Project file: " << sourceProjectFile_.getFileName() << "\n\n";
        m << "== Included ==\n";
        m << "Audio media files: " << plan.mediaRelativePaths.size() << "\n";
        for (const auto& p : plan.proxies)
        {
            m << "Instrument proxy (" << p.trackName << "): "
              << (p.silentGeneration ? juce::String("intentional silent generation")
                                     : p.relativePath)
              << "\n";
        }
        m << "\n== External requirements (NOT included; PI-026) ==\n";
        m << "Plugin binaries, licences and activation data are never packaged.\n";
        for (const auto& it : data.experimentalInstrumentTracks)
        {
            const juce::String plugin = it.hasGenericVst3Descriptor
                                            ? it.genericVst3Descriptor.name
                                            : it.instrumentKind;
            m << "Primary instrument \"" << it.name << "\": " << plugin
              << (it.pluginVersion.isNotEmpty() ? (" " + it.pluginVersion) : juce::String())
              << " — represented by its included proxy for playback; the plugin itself"
                 " is required only for future editing/re-rendering.\n";
        }
        juce::StringArray effectNames;
        for (const auto& t : data.tracks)
        {
            for (const auto& ins : t.inserts)
            {
                const juce::String label
                    = ins.pluginIdentifier.isNotEmpty()
                          ? ins.pluginIdentifier
                          : juce::File::createFileWithoutCheckingPath(ins.pluginVst3Path)
                                .getFileNameWithoutExtension();
                if (label.isNotEmpty())
                {
                    effectNames.addIfNotAlreadyThere(label);
                }
            }
        }
        if (!effectNames.isEmpty())
        {
            m << "Downstream effect plugins (NOT captured by instrument proxies — "
                 "playback on another machine needs them installed for identical "
                 "sound): "
              << effectNames.joinIntoString(", ") << "\n";
        }
        return m;
    }

    //==========================================================================
    // Shared plumbing
    //==========================================================================
    [[nodiscard]] PreparationPhase phase() const noexcept
    {
        return (PreparationPhase)phase_.load(std::memory_order_acquire);
    }

    void setPhase(const PreparationPhase p) noexcept
    {
        phase_.store((int)p, std::memory_order_release);
    }

    void setCurrentItem(const juce::String& s)
    {
        const std::lock_guard<std::mutex> lock(stringsMutex_);
        currentItem_ = s;
    }

    void addBlocker(const juce::String& s)
    {
        const std::lock_guard<std::mutex> lock(stringsMutex_);
        blockers_.add(s);
    }

    void fail(const juce::String& reason)
    {
        {
            const std::lock_guard<std::mutex> lock(stringsMutex_);
            failureReason_ = reason;
        }
        cleanupStaging();
        setPhase(PreparationPhase::Failed);
        stopTicker();
    }

    void failWithBlockers()
    {
        {
            const std::lock_guard<std::mutex> lock(stringsMutex_);
            if (failureReason_.isEmpty())
            {
                failureReason_ = "The portable project cannot be completed. "
                                 "See the blocking tracks below.";
            }
        }
        cleanupStaging();
        setPhase(PreparationPhase::Failed);
        stopTicker();
    }

    void failProjectChanged()
    {
        cancelPreparationRenders();
        fail("The project changed during preparation. Nothing was published — "
             "start Prepare Portable Project again.");
    }

    void finishCancelNoWorker()
    {
        cleanupStaging();
        setPhase(PreparationPhase::Cancelled);
        stopTicker();
    }

    [[nodiscard]] DestinationReport* reportFor(const TrackId tid) noexcept
    {
        for (auto& d : destinations_)
        {
            if (d.trackId == tid)
            {
                return &d;
            }
        }
        return nullptr;
    }

    [[nodiscard]] juce::String capturedFingerprintFor(const TrackId tid) const
    {
        for (const auto& [t, fp] : expectedFingerprints_)
        {
            if (t == tid)
            {
                return fp;
            }
        }
        return {};
    }

    /// PI-028 currency proof: every captured destination fingerprint must still
    /// equal the live expected fingerprint (a render-relevant edit anywhere in
    /// between makes the package a forbidden mixed generation).
    [[nodiscard]] bool verifyPreparationIdentityStillMatches() const
    {
        for (const auto& [tid, fp] : expectedFingerprints_)
        {
            const Dependencies::Identity id
                = deps_.identityForTrack ? deps_.identityForTrack(tid)
                                         : Dependencies::Identity{};
            // A destination whose Primary is unavailable keeps its captured
            // fingerprint (no identity is derivable live; its Current verdict
            // was already fingerprint-based and its asset validates by hash).
            if (id.exists && id.fingerprint != fp)
            {
                return false;
            }
            if (deps_.destinationState
                && deps_.destinationState(tid) != proxy_render::ProxyDestinationState::Current)
            {
                return false;
            }
        }
        return true;
    }

    void cancelPreparationRenders()
    {
        if (!deps_.cancelDestination)
        {
            return;
        }
        for (const auto& [tid, requested] : pendingRender_)
        {
            if (requested)
            {
                const DestinationReport* const rep = reportFor(tid);
                if (rep != nullptr && rep->verdict == DestinationVerdict::RenderingNow)
                {
                    deps_.cancelDestination(tid);
                }
            }
        }
    }

    void cleanupStaging()
    {
        if (stagingFolder_ != juce::File() && stagingFolder_.exists())
        {
            (void)stagingFolder_.deleteRecursively();
        }
    }

    void joinWorker()
    {
        if (worker_.joinable())
        {
            worker_.join();
        }
    }

    void resetRunState()
    {
        cancelled_.store(false, std::memory_order_relaxed);
        workerDone_.store(false, std::memory_order_relaxed);
        workerFailed_.store(false, std::memory_order_relaxed);
        workerActive_.store(false, std::memory_order_relaxed);
        workerPhase_.store(0, std::memory_order_relaxed);
        completedFiles_.store(0, std::memory_order_relaxed);
        totalFiles_.store(0, std::memory_order_relaxed);
        destinations_.clear();
        expectedFingerprints_.clear();
        pendingRender_.clear();
        {
            const std::lock_guard<std::mutex> lock(stringsMutex_);
            currentItem_.clear();
            blockers_.clear();
            failureReason_.clear();
        }
        finalFolder_ = juce::File();
        stagingFolder_ = juce::File();
        sourceProjectFile_ = juce::File();
        sourceProjectFolder_ = juce::File();
    }

    struct Ticker final : juce::Timer
    {
        explicit Ticker(PortablePreparationService& s) : service(s) {}
        void timerCallback() override { service.tick(); }
        PortablePreparationService& service;
    };

    Dependencies deps_;
    std::atomic<int> phase_{ (int)PreparationPhase::Idle };
    std::atomic<bool> cancelled_{ false };
    std::atomic<bool> workerDone_{ false };
    std::atomic<bool> workerFailed_{ false };
    std::atomic<bool> workerActive_{ false };
    std::atomic<int> workerPhase_{ 0 }; ///< 0 copy, 1 write, 2 validate
    std::atomic<int> completedFiles_{ 0 };
    std::atomic<int> totalFiles_{ 0 };
    mutable std::mutex stringsMutex_;
    juce::String currentItem_;
    juce::StringArray blockers_;
    juce::String failureReason_;
    std::vector<DestinationReport> destinations_;           ///< message thread
    std::vector<std::pair<TrackId, juce::String>> expectedFingerprints_;
    std::vector<std::pair<TrackId, bool>> pendingRender_;   ///< {tid, requested}
    juce::File finalFolder_, stagingFolder_;
    juce::File sourceProjectFile_, sourceProjectFolder_;
    std::thread worker_;
    std::unique_ptr<Ticker> ticker_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PortablePreparationService)
};

} // namespace portable_project
