// =============================================================================
// Session.cpp  —  release/acquire of `const SessionSnapshot` (same contract as old single-clip)
// =============================================================================
//
// ROLE IN THE ARCHITECTURE
//   Publishes immutable snapshots that now contain an **ordered** list of **tracks** (Phase 3
//   minimal), each with its own front-to-back `PlacedClip` list. The audio path still
//   acquire-loads; no extra mutex.
//
// IN-BODY COMMENTS
//   Where we touch `sessionSnapshot_`, the comments name **user-visible meaning** (keep old
//   file on error, what clear implies) in plain language, not a narration of the C++ calls.
// =============================================================================

#include "domain/Session.h"

#include "domain/AudioClip.h"
#include "io/AudioFileLoader.h"
#include "io/ProjectFile.h"
#include "transport/Transport.h"

#include <juce_core/juce_core.h>

#include <cstdint>
#include <exception>
#include <memory>
#include <new>
#include <utility>
#include <limits>

namespace
{
    int countPlacedClipsIn(const SessionSnapshot& s) noexcept
    {
        int n = 0;
        for (int t = 0; t < s.getNumTracks(); ++t)
        {
            n += s.getTrack(t).getNumPlacedClips();
        }
        return n;
    }

    std::uint64_t sumPcmBytesInSnapshotForDiagnosis(const SessionSnapshot& s) noexcept
    {
        std::uint64_t total = 0;
        for (int t = 0; t < s.getNumTracks(); ++t)
        {
            const Track& tr = s.getTrack(t);
            for (int i = 0; i < tr.getNumPlacedClips(); ++i)
            {
                const AudioClip& c = tr.getPlacedClip(i).getAudioClip();
                total += static_cast<std::uint64_t>(c.getNumChannels())
                         * static_cast<std::uint64_t>(c.getNumSamples())
                         * static_cast<std::uint64_t>(sizeof(float));
            }
        }
        return total;
    }

    // ---------------------------------------------------------------------
    // Project persistence: strict `Audio/`-relative `sourcePath` strings only (portable `.dalproj`).
    // ---------------------------------------------------------------------

    [[nodiscard]] bool isRelativeAudioPath(const juce::String& stored) noexcept
    {
        if (stored.isEmpty() || juce::File::isAbsolutePath(stored))
        {
            return false;
        }
        if (stored.containsChar('\\'))
        {
            return false;
        }
        if (!stored.startsWith("Audio/"))
        {
            return false;
        }
        if (stored.endsWithChar('/'))
        {
            return false;
        }
        if (stored.startsWith("../") || stored == ".." || stored.contains("/../"))
        {
            return false;
        }

        const juce::StringArray segments = juce::StringArray::fromTokens(stored, "/", "");
        const int ns = (int)segments.size();
        if (ns < 2 || segments[ns - 1].isEmpty())
        {
            return false;
        }
        if (segments[0] != "Audio")
        {
            return false;
        }
        for (int i = 1; i < ns; ++i)
        {
            const juce::String& seg = segments[i];
            if (seg.isEmpty() || seg == ".." || seg == ".")
            {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] bool isClipSourceFileUnderProjectAudio(const juce::File& srcFile,
                                                          const juce::File& projectFolder) noexcept
    {
        if (!srcFile.existsAsFile())
        {
            return false;
        }
        const juce::File projectAudioDir = projectFolder.getChildFile("Audio");
        const juce::String relFromAudio = srcFile.getRelativePathFrom(projectAudioDir);
        if (juce::File::isAbsolutePath(relFromAudio))
        {
            return false;
        }

        const juce::String relNorm = relFromAudio.replaceCharacter('\\', '/');
        if (relNorm == ".." || relNorm.startsWith("../"))
        {
            return false;
        }
        const juce::File viaRel = projectAudioDir.getChildFile(relFromAudio);
        return (viaRel == srcFile);
    }

    [[nodiscard]] juce::String toProjectAudioStoredPath(const juce::File& src,
                                                        const juce::File& projectFolder)
    {
        return src.getRelativePathFrom(projectFolder).replaceCharacter('\\', '/');
    }

    [[nodiscard]] juce::File resolveProjectAudioStoredPath(const juce::String& stored,
                                                          const juce::File& projectFolder) noexcept
    {
        if (!isRelativeAudioPath(stored))
        {
            return {};
        }
        return projectFolder.getChildFile(stored);
    }

} // namespace

Session::Session()
    : sessionSnapshot_(SessionSnapshot::withSingleEmptyTrack(TrackId{1}, juce::String("Track 1")))
{
    // One default **track** (empty lane) so the first “Add clip” and the Phase 1 bridge clip query
    // still make sense. `activeTrackId_` / `nextTrackId_` are set in the header (first track = 1).
}

Session::~Session() = default;

juce::Result Session::addClipFromFileAtPlayhead(const juce::File& file,
                                                const double deviceSampleRate,
                                                const std::int64_t startSampleOnTimeline)
{
    if (activeTrackId_ == kInvalidTrackId)
    {
        return juce::Result::fail("No active track");
    }
    juce::Logger::writeToLog(
        juce::String("[CLIMPORT] STAGE:session:entry addClipAtPlayhead file=") + file.getFullPathName()
        + " startSample=" + juce::String(startSampleOnTimeline) + " activeTrackId=" + juce::String(activeTrackId_));
    // Decode on the message thread. Until we have a *complete* new `AudioClip`, we do not
    // publish: a corrupt file must not become a half-finished new snapshot.
    std::unique_ptr<AudioClip> loaded;
    const juce::Result loadResult = AudioFileLoader::loadFromFile(file, deviceSampleRate, loaded);

    if (!loadResult.wasOk())
    {
        // Failure: return the error and leave `sessionSnapshot_` untouched (acquire in the
        // callback still sees the previous pointer).
        return loadResult;
    }

    jassert(loaded != nullptr);
    {
        const std::uint64_t thisClipBytes
            = static_cast<std::uint64_t>(loaded->getNumChannels())
              * static_cast<std::uint64_t>(loaded->getNumSamples()) * static_cast<std::uint64_t>(sizeof(float));
        juce::Logger::writeToLog(juce::String("[CLIMPORT] STAGE:session:decode_ok thisClipPcmBytes=")
                                  + juce::String(thisClipBytes) + " file=" + file.getFileName());
    }

    const std::shared_ptr<const AudioClip> material(std::move(loaded));
    // `startSampleOnTimeline` was read on this thread (typically from `Transport` once, at add
    // time) — the clip is spliced in as the new front of **active track**; ordering within that
    // lane matches Phase 2 (newest at index 0 of that track).
    const PlacedClipId newId = nextPlacedClipId_++;
    jassert(newId != kInvalidPlacedClipId);
    const std::shared_ptr<const SessionSnapshot> current = loadSessionSnapshotForAudioThread();
    if (current == nullptr)
    {
        juce::Logger::writeToLog("[CLIMPORT] STAGE:session:fail current snapshot is null (unexpected)");
        return juce::Result::fail("Internal error: no session snapshot.");
    }
    juce::Logger::writeToLog(
        juce::String("[CLIMPORT] STAGE:session:build:begin newPlacedId=") + juce::String(newId) + " clipsBefore="
        + juce::String(countPlacedClipsIn(*current)));
    try
    {
        const std::shared_ptr<const SessionSnapshot> next
            = SessionSnapshot::withClipAddedAsNewestOnTargetTrack(
                *current, newId, material, startSampleOnTimeline, activeTrackId_);
        jassert(next != nullptr);
        if (next == nullptr)
        {
            juce::Logger::writeToLog(
                juce::String("[CLIMPORT] STAGE:session:build:fail withClipAdded returned null for ")
                + file.getFullPathName());
            return juce::Result::fail("Internal error: could not add clip to session.");
        }
        const int clipsAfter = countPlacedClipsIn(*next);
        const std::uint64_t totalPcm = sumPcmBytesInSnapshotForDiagnosis(*next);
        juce::Logger::writeToLog(
            juce::String("[CLIMPORT] STAGE:session:publish:begin clipsAfter=") + juce::String(clipsAfter)
            + " totalPcmBytesApprox=" + juce::String(totalPcm));
        // Release: make this snapshot the one future acquires see; old snapshot is kept alive by any
        // in-flight callback/UI read until their shared_ptrs drop.
        std::atomic_store_explicit(&sessionSnapshot_, next, std::memory_order_release);
    }
    catch (const std::bad_alloc&)
    {
        juce::Logger::writeToLog(
            juce::String("[CLIMPORT] STAGE:session:build:fail OOM while building snapshot for ")
            + file.getFullPathName());
        // Do not touch sessionSnapshot_ — previous snapshot is still the published one.
        return juce::Result::fail(
            "Out of memory while building the session (copying clips). Remove clips or use smaller or shorter files.");
    }
    catch (const std::exception& e)
    {
        juce::Logger::writeToLog(
            juce::String("[CLIMPORT] STAGE:session:build:fail std::exception: ") + e.what() + " file="
            + file.getFullPathName());
        return juce::Result::fail(
            juce::String("Exception while adding clip: ") + e.what() + " (" + file.getFileName() + ")");
    }
    catch (...)
    {
        juce::Logger::writeToLog(
            juce::String("[CLIMPORT] STAGE:session:build:fail unknown for ") + file.getFullPathName());
        return juce::Result::fail("Unknown exception while adding clip: " + file.getFileName());
    }

    juce::Logger::writeToLog(juce::String("[CLIMPORT] STAGE:session:publish:ok file=") + file.getFileName());
    return juce::Result::ok();
}

bool Session::hasKnownProjectFile() const noexcept
{
    return currentProjectFile_.getFullPathName().isNotEmpty();
}

juce::File Session::getCurrentProjectFolder() const noexcept
{
    return currentProjectFile_.getParentDirectory();
}

juce::Result Session::addRecordedTakeAtSample(
    const juce::File& file,
    const double deviceSampleRate,
    const std::int64_t startSampleOnTimeline,
    const TrackId targetTrackId,
    const std::int64_t intendedVisibleLengthSamples)
{
    if (targetTrackId == kInvalidTrackId)
    {
        return juce::Result::fail("Invalid target track for recorded take");
    }
    if (intendedVisibleLengthSamples <= 0)
    {
        return juce::Result::fail("Recorded take has no length");
    }
    std::unique_ptr<AudioClip> loaded;
    const juce::Result loadResult = AudioFileLoader::loadFromFile(file, deviceSampleRate, loaded);
    if (!loadResult.wasOk())
    {
        return loadResult;
    }
    jassert(loaded != nullptr);
    const std::shared_ptr<const AudioClip> material(std::move(loaded));
    const std::shared_ptr<const SessionSnapshot> current = loadSessionSnapshotForAudioThread();
    if (current == nullptr)
    {
        return juce::Result::fail("Internal error: no session snapshot.");
    }
    if (current->findTrackIndexById(targetTrackId) < 0)
    {
        return juce::Result::fail("Target track does not exist.");
    }
    const PlacedClipId newId = nextPlacedClipId_++;
    jassert(newId != kInvalidPlacedClipId);
    try
    {
        const std::shared_ptr<const SessionSnapshot> next
            = SessionSnapshot::withClipAddedAsNewestOnTargetTrack(
                *current, newId, material, startSampleOnTimeline, targetTrackId, 0, intendedVisibleLengthSamples);
        if (next == nullptr)
        {
            return juce::Result::fail("Internal error: could not add recorded take to session.");
        }
        std::atomic_store_explicit(&sessionSnapshot_, next, std::memory_order_release);
    }
    catch (const std::bad_alloc&)
    {
        return juce::Result::fail(
            "Out of memory while adding the recorded take.");
    }
    catch (const std::exception& e)
    {
        return juce::Result::fail(
            juce::String("Exception while adding recorded take: ") + e.what());
    }
    catch (...)
    {
        return juce::Result::fail("Unknown exception while adding recorded take.");
    }
    return juce::Result::ok();
}

juce::Result Session::addPlacedClipFromExistingMaterial(
    std::shared_ptr<const AudioClip> material,
    const std::int64_t startSampleOnTimeline,
    const std::int64_t leftTrimSamples,
    const std::int64_t visibleLengthSamples,
    const TrackId targetTrackId)
{
    return addPlacedClipFromExistingMaterial(std::move(material),
                                             startSampleOnTimeline,
                                             leftTrimSamples,
                                             visibleLengthSamples,
                                             targetTrackId,
                                             std::int64_t{ 0 },
                                             std::numeric_limits<std::int64_t>::min());
}

juce::Result Session::addPlacedClipFromExistingMaterial(
    std::shared_ptr<const AudioClip> material,
    const std::int64_t startSampleOnTimeline,
    const std::int64_t leftTrimSamples,
    const std::int64_t visibleLengthSamples,
    const TrackId targetTrackId,
    const std::int64_t materialWindowStartSamples,
    const std::int64_t materialWindowEndExclusiveSamples)
{
    if (targetTrackId == kInvalidTrackId)
    {
        return juce::Result::fail("Invalid target track");
    }
    if (material == nullptr)
    {
        return juce::Result::fail("No audio material");
    }
    if (visibleLengthSamples <= 0)
    {
        return juce::Result::fail("Placed clip has no length");
    }
    if (leftTrimSamples < 0)
    {
        return juce::Result::fail("Invalid left trim");
    }
    const std::shared_ptr<const SessionSnapshot> current = loadSessionSnapshotForAudioThread();
    if (current == nullptr)
    {
        return juce::Result::fail("Internal error: no session snapshot.");
    }
    if (current->findTrackIndexById(targetTrackId) < 0)
    {
        return juce::Result::fail("Target track does not exist.");
    }
    const PlacedClipId newId = nextPlacedClipId_++;
    jassert(newId != kInvalidPlacedClipId);
    try
    {
        const std::shared_ptr<const SessionSnapshot> next = SessionSnapshot::withClipAddedAsNewestOnTargetTrack(
            *current,
            newId,
            std::move(material),
            startSampleOnTimeline,
            targetTrackId,
            leftTrimSamples,
            visibleLengthSamples,
            materialWindowStartSamples,
            materialWindowEndExclusiveSamples);
        if (next == nullptr)
        {
            return juce::Result::fail("Internal error: could not add clip to session.");
        }
        std::atomic_store_explicit(&sessionSnapshot_, next, std::memory_order_release);
    }
    catch (const std::bad_alloc&)
    {
        return juce::Result::fail("Out of memory while adding the clip.");
    }
    catch (const std::exception& e)
    {
        return juce::Result::fail(juce::String("Exception while adding clip: ") + e.what());
    }
    catch (...)
    {
        return juce::Result::fail("Unknown exception while adding clip.");
    }
    return juce::Result::ok();
}

void Session::addTrack() noexcept
{
    const std::shared_ptr<const SessionSnapshot> current = loadSessionSnapshotForAudioThread();
    if (current == nullptr)
    {
        return;
    }
    const TrackId newId = nextTrackId_++;
    jassert(newId != kInvalidTrackId);
    const juce::String newName = juce::String("Track ") + juce::String(newId);
    const std::shared_ptr<const SessionSnapshot> next
        = SessionSnapshot::withTrackAdded(*current, newId, newName);
    if (next == nullptr)
    {
        jassert(false);
        return;
    }
    std::atomic_store_explicit(&sessionSnapshot_, next, std::memory_order_release);
    activeTrackId_ = newId;
}

TrackId Session::getActiveTrackId() const noexcept
{
    return activeTrackId_;
}

void Session::setActiveTrack(const TrackId id) noexcept
{
    if (id == kInvalidTrackId)
    {
        return;
    }
    const std::shared_ptr<const SessionSnapshot> s = loadSessionSnapshotForAudioThread();
    if (s == nullptr || s->findTrackIndexById(id) < 0)
    {
        return;
    }
    activeTrackId_ = id;
}

int Session::getNumTracks() const noexcept
{
    const std::shared_ptr<const SessionSnapshot> s = loadSessionSnapshotForAudioThread();
    return (s == nullptr) ? 0 : s->getNumTracks();
}

TrackId Session::getTrackIdAtIndex(const int index) const noexcept
{
    const std::shared_ptr<const SessionSnapshot> s = loadSessionSnapshotForAudioThread();
    if (s == nullptr || index < 0 || index >= s->getNumTracks())
    {
        return kInvalidTrackId;
    }
    return s->getTrack(index).getId();
}

void Session::moveClip(const PlacedClipId id, const std::int64_t newStartSampleOnTimeline) noexcept
{
    if (id == kInvalidPlacedClipId)
    {
        return;
    }
    const std::shared_ptr<const SessionSnapshot> current = loadSessionSnapshotForAudioThread();
    if (current == nullptr || current->isEmpty())
    {
        return;
    }
    // The snapshot factory alone applies “isolated → promote to 0, else keep ordinal” **within
    // the moved clip’s track** — this class only publishes, same as `addClipFromFileAtPlayhead`.
    const std::shared_ptr<const SessionSnapshot> next
        = SessionSnapshot::withClipMoved(*current, id, newStartSampleOnTimeline);
    jassert(next != nullptr);
    std::atomic_store_explicit(&sessionSnapshot_, next, std::memory_order_release);
}

void Session::moveClipToTrack(
    const PlacedClipId id,
    const std::int64_t newStartSampleOnTimeline,
    const TrackId targetTrackId) noexcept
{
    if (id == kInvalidPlacedClipId || targetTrackId == kInvalidTrackId)
    {
        return;
    }
    const std::shared_ptr<const SessionSnapshot> current = loadSessionSnapshotForAudioThread();
    if (current == nullptr || current->isEmpty())
    {
        return;
    }
    TrackId ownerTrackId = kInvalidTrackId;
    for (int t = 0; t < current->getNumTracks(); ++t)
    {
        const Track& tr = current->getTrack(t);
        for (int i = 0; i < tr.getNumPlacedClips(); ++i)
        {
            if (tr.getPlacedClip(i).getId() == id)
            {
                ownerTrackId = tr.getId();
                break;
            }
        }
        if (ownerTrackId != kInvalidTrackId)
        {
            break;
        }
    }
    if (ownerTrackId == kInvalidTrackId)
    {
        return;
    }
    if (ownerTrackId == targetTrackId)
    {
        // Same track: `withClipMovedToTrack` is not used; UI should call `moveClip` instead.
        return;
    }
    const std::shared_ptr<const SessionSnapshot> next = SessionSnapshot::withClipMovedToTrack(
        *current, id, newStartSampleOnTimeline, targetTrackId);
    jassert(next != nullptr);
    std::atomic_store_explicit(&sessionSnapshot_, next, std::memory_order_release);
}

void Session::setClipRightEdgeVisibleLength(
    const PlacedClipId id, const std::int64_t newVisibleLengthSamples) noexcept
{
    if (id == kInvalidPlacedClipId)
    {
        return;
    }
    const std::shared_ptr<const SessionSnapshot> current = loadSessionSnapshotForAudioThread();
    if (current == nullptr || current->isEmpty())
    {
        return;
    }
    const std::shared_ptr<const SessionSnapshot> next
        = SessionSnapshot::withClipRightEdgeTrimmed(*current, id, newVisibleLengthSamples);
    jassert(next != nullptr);
    std::atomic_store_explicit(&sessionSnapshot_, next, std::memory_order_release);
}

void Session::setClipLeftEdgeTrim(const PlacedClipId id, const std::int64_t newLeftTrimSamples) noexcept
{
    if (id == kInvalidPlacedClipId)
    {
        return;
    }
    const std::shared_ptr<const SessionSnapshot> current = loadSessionSnapshotForAudioThread();
    if (current == nullptr || current->isEmpty())
    {
        return;
    }
    const std::shared_ptr<const SessionSnapshot> next
        = SessionSnapshot::withClipLeftEdgeTrimmed(*current, id, newLeftTrimSamples);
    jassert(next != nullptr);
    std::atomic_store_explicit(&sessionSnapshot_, next, std::memory_order_release);
}

void Session::moveTrack(const TrackId movedTrackId, const int destIndex) noexcept
{
    if (movedTrackId == kInvalidTrackId)
    {
        return;
    }
    const std::shared_ptr<const SessionSnapshot> current = loadSessionSnapshotForAudioThread();
    if (current == nullptr || current->getNumTracks() == 0)
    {
        return;
    }
    const int s = current->findTrackIndexById(movedTrackId);
    if (s < 0)
    {
        return;
    }
    if (destIndex < 0 || destIndex >= current->getNumTracks())
    {
        return;
    }
    if (s == destIndex)
    {
        return;
    }
    const std::shared_ptr<const SessionSnapshot> next
        = SessionSnapshot::withTrackReordered(*current, movedTrackId, destIndex);
    jassert(next != nullptr);
    std::atomic_store_explicit(&sessionSnapshot_, next, std::memory_order_release);
    // `activeTrackId_` is intentionally unchanged — UI highlights by id.
}

void Session::setTrackChannelFaderGain(const TrackId trackId, float linearGain) noexcept
{
    if (trackId == kInvalidTrackId)
    {
        return;
    }
    const std::shared_ptr<const SessionSnapshot> current = loadSessionSnapshotForAudioThread();
    if (current == nullptr || current->findTrackIndexById(trackId) < 0)
    {
        return;
    }
    linearGain = juce::jlimit(0.0f, kTrackChannelFaderGainMax, linearGain);
    const std::shared_ptr<const SessionSnapshot> next
        = SessionSnapshot::withTrackChannelFaderGain(*current, trackId, linearGain);
    jassert(next != nullptr);
    std::atomic_store_explicit(&sessionSnapshot_, next, std::memory_order_release);
}

void Session::clearClip() noexcept
{
    // “No file”: one **empty** default track (id 1), same as a fresh `Session` — not the shared
    // *zero-track* `createEmpty` singleton.
    const std::shared_ptr<const SessionSnapshot> empty
        = SessionSnapshot::withSingleEmptyTrack(TrackId{1}, juce::String("Track 1"));
    std::atomic_store_explicit(&sessionSnapshot_, empty, std::memory_order_release);
    nextTrackId_ = 2;
    activeTrackId_ = 1;
}

const AudioClip* Session::getCurrentClip() const noexcept
{
    // Bridge: the **first** track’s front row’s material, if any (Phase 1 callers).
    const std::shared_ptr<const SessionSnapshot> snap = loadSessionSnapshotForAudioThread();
    if (snap == nullptr || snap->getNumTracks() == 0)
    {
        return nullptr;
    }
    const Track& t0 = snap->getTrack(0);
    if (t0.getNumPlacedClips() == 0)
    {
        return nullptr;
    }
    return &t0.getPlacedClip(0).getAudioClip();
}

std::int64_t Session::getContentEndSamples() const noexcept
{
    const std::shared_ptr<const SessionSnapshot> snap = loadSessionSnapshotForAudioThread();
    if (snap == nullptr)
    {
        return 0;
    }
    return snap->getDerivedTimelineLengthSamples();
}

std::int64_t Session::getArrangementExtentSamples() const noexcept
{
    const std::shared_ptr<const SessionSnapshot> snap = loadSessionSnapshotForAudioThread();
    if (snap == nullptr)
    {
        return 0;
    }
    return snap->getArrangementExtentSamples();
}

std::int64_t Session::getStoredArrangementExtentSamples() const noexcept
{
    const std::shared_ptr<const SessionSnapshot> snap = loadSessionSnapshotForAudioThread();
    if (snap == nullptr)
    {
        return 0;
    }
    return snap->getStoredArrangementExtentSamples();
}

void Session::setArrangementExtentSamples(const std::int64_t v) noexcept
{
    const std::shared_ptr<const SessionSnapshot> cur = loadSessionSnapshotForAudioThread();
    if (cur == nullptr)
    {
        return;
    }
    if (v <= cur->getStoredArrangementExtentSamples())
    {
        return;
    }
    const std::shared_ptr<const SessionSnapshot> next
        = SessionSnapshot::withArrangementExtent(*cur, v);
    jassert(next != nullptr);
    std::atomic_store_explicit(&sessionSnapshot_, next, std::memory_order_release);
}

void Session::setLeftLocatorAtSample(const std::int64_t s) noexcept
{
    const std::shared_ptr<const SessionSnapshot> cur = loadSessionSnapshotForAudioThread();
    if (cur == nullptr)
    {
        return;
    }
    const std::int64_t hi = cur->getArrangementExtentSamples();
    const std::int64_t clamped = juce::jlimit(std::int64_t{0}, hi, s);
    if (clamped == cur->getLeftLocatorSamples())
    {
        return;
    }
    const std::shared_ptr<const SessionSnapshot> next = SessionSnapshot::withLocators(
        *cur, clamped, cur->getRightLocatorSamples());
    jassert(next != nullptr);
    std::atomic_store_explicit(&sessionSnapshot_, next, std::memory_order_release);
}

void Session::setRightLocatorAtSample(const std::int64_t s) noexcept
{
    const std::shared_ptr<const SessionSnapshot> cur = loadSessionSnapshotForAudioThread();
    if (cur == nullptr)
    {
        return;
    }
    const std::int64_t hi = cur->getArrangementExtentSamples();
    const std::int64_t clamped = juce::jlimit(std::int64_t{0}, hi, s);
    if (clamped == cur->getRightLocatorSamples())
    {
        return;
    }
    const std::shared_ptr<const SessionSnapshot> next = SessionSnapshot::withLocators(
        *cur, cur->getLeftLocatorSamples(), clamped);
    jassert(next != nullptr);
    std::atomic_store_explicit(&sessionSnapshot_, next, std::memory_order_release);
}

std::int64_t Session::getLeftLocatorSamples() const noexcept
{
    const std::shared_ptr<const SessionSnapshot> snap = loadSessionSnapshotForAudioThread();
    return (snap != nullptr) ? snap->getLeftLocatorSamples() : 0;
}

std::int64_t Session::getRightLocatorSamples() const noexcept
{
    const std::shared_ptr<const SessionSnapshot> snap = loadSessionSnapshotForAudioThread();
    return (snap != nullptr) ? snap->getRightLocatorSamples() : 0;
}

std::shared_ptr<const SessionSnapshot> Session::loadSessionSnapshotForAudioThread() const noexcept
{
    // Acquire: pair with the release stores in replace/clear so this read happens-after the last
    // full snapshot publish; the cost on the hot path is the atomic + shared_ptr retain.
    return std::atomic_load_explicit(&sessionSnapshot_, std::memory_order_acquire);
}

juce::Result Session::saveProjectToFile(Transport& transport,
                                       const juce::File& file,
                                       const double deviceSampleRate)
{
    const std::shared_ptr<const SessionSnapshot> s = loadSessionSnapshotForAudioThread();
    if (s == nullptr)
    {
        return juce::Result::fail("No session to save.");
    }

    ProjectFileV1 out;
    out.version = ProjectFileV1::kCurrentVersion;
    out.nextPlacedClipId = nextPlacedClipId_;
    out.nextTrackId = nextTrackId_;
    out.activeTrackId = activeTrackId_;
    out.playheadSamples = transport.readPlayheadSamplesForUi();
    out.deviceSampleRateAtSave = deviceSampleRate;
    out.arrangementExtentSamples = s->getArrangementExtentSamples();

    for (int i = 0; i < s->getNumTracks(); ++i)
    {
        const Track& t = s->getTrack(i);
        ProjectFileTrackV1 tr;
        tr.id = t.getId();
        tr.name = t.getName();
        tr.channelFaderGain = t.getChannelFaderGain();
        for (int j = 0; j < t.getNumPlacedClips(); ++j)
        {
            const PlacedClip& p = t.getPlacedClip(j);
            ProjectFileClipV1 c;
            c.id = p.getId();
            c.startSample = p.getStartSample();
            {
                const juce::File src(p.getAudioClip().getSourceFilePath());
                if (src.getFullPathName().isEmpty())
                {
                    return juce::Result::fail("A clip in the session has no source file path; cannot save.");
                }
                const juce::File projectFolder = file.getParentDirectory();
                if (!isClipSourceFileUnderProjectAudio(src, projectFolder))
                {
                    return juce::Result::fail(
                        "Cannot save project because an audio clip refers to a file outside "
                        "the project Audio folder: "
                        + src.getFullPathName());
                }

                const juce::String storedRel = toProjectAudioStoredPath(src, projectFolder);
                if (!storedRel.startsWith("Audio/"))
                {
                    return juce::Result::fail(
                        "Cannot save project: clip source must lie under Audio/ relative to "
                        "the project file (unexpected path derivation for "
                        + src.getFullPathName()
                        + ").");
                }
                c.sourcePath = storedRel;
            }
            const int matN = p.getMaterialLengthSamples();
            const std::int64_t eff = p.getEffectiveLengthSamples();
            const std::int64_t ltrim = p.getLeftTrimSamples();
            const std::int64_t fullTail
                = (matN > 0) ? (static_cast<std::int64_t>(matN) - ltrim) : std::int64_t{ 0 };
            c.leftTrimSamples = ltrim;
            const std::int64_t ws = p.getMaterialWindowStartSamples();
            const std::int64_t we = p.getMaterialWindowEndExclusiveSamples();
            const bool narrowedFullMaterial
                = (matN > 0 && !(ws == 0 && we == static_cast<std::int64_t>(matN)));
            if (narrowedFullMaterial)
            {
                c.hasMaterialWindowInFile = true;
                c.materialWindowStartSamples = ws;
                c.materialWindowEndExclusiveSamples = we;
            }
            c.visibleLengthSamples = (matN > 0 && eff < fullTail) ? eff : 0;
            tr.clips.push_back(std::move(c));
        }
        out.tracks.push_back(std::move(tr));
    }

    if (out.tracks.empty())
    {
        return juce::Result::fail("Session has no tracks to save.");
    }
    const juce::Result wr = writeProjectFile(file, out);
    if (wr.wasOk())
    {
        currentProjectFile_ = file;
    }
    return wr;
}

juce::Result Session::loadProjectFromFile(Transport& transport,
                                          const juce::File& file,
                                          const double deviceSampleRate,
                                          juce::StringArray& outSkippedClipDetails,
                                          juce::String& outInfoNote)
{
    outSkippedClipDetails.clear();
    outInfoNote.clear();

    ProjectFileV1 parsed;
    const juce::Result pr = readProjectFile(file, parsed);
    if (!pr.wasOk())
    {
        return pr;
    }

    if (!juce::approximatelyEqual(deviceSampleRate, parsed.deviceSampleRateAtSave))
    {
        outInfoNote = "This project was saved with the audio device at "
                      + juce::String(parsed.deviceSampleRateAtSave, 1)
                      + " Hz. The current device is "
                      + juce::String(deviceSampleRate, 1)
                      + " Hz. Files that do not match the current device rate are skipped and "
                        "listed below.";
    }

    PlacedClipId maxClipInFile = 0;
    TrackId maxTrackInFile = 0;
    for (const auto& tr : parsed.tracks)
    {
        maxTrackInFile = juce::jmax(maxTrackInFile, tr.id);
        for (const auto& c : tr.clips)
        {
            maxClipInFile = juce::jmax(maxClipInFile, c.id);
        }
    }

    std::vector<Track> built;
    built.reserve(parsed.tracks.size());

    for (const auto& trDto : parsed.tracks)
    {
        std::vector<PlacedClip> placed;
        for (const auto& cDto : trDto.clips)
        {
            const juce::String& stored = cDto.sourcePath;
            if (juce::File::isAbsolutePath(stored))
            {
                outSkippedClipDetails.add(stored
                                          + " - Absolute audio paths are not allowed in project "
                                            "files (expected Audio/... relative to project folder).");
                continue;
            }
            if (!isRelativeAudioPath(stored))
            {
                outSkippedClipDetails.add(stored
                                          + " - Invalid audio path (must be Audio/<name> with "
                                            "forward slashes only, no parent-directory segments).");
                continue;
            }

            const juce::File f = resolveProjectAudioStoredPath(stored, file.getParentDirectory());
            std::unique_ptr<AudioClip> loaded;
            const juce::Result lr = AudioFileLoader::loadFromFile(f, deviceSampleRate, loaded);
            if (!lr.wasOk())
            {
                outSkippedClipDetails.add(
                    cDto.sourcePath + " - " + lr.getErrorMessage());
                continue;
            }
            const std::shared_ptr<const AudioClip> material(std::move(loaded));
            const int matN = material->getNumSamples();
            const std::int64_t lRaw = cDto.leftTrimSamples;
            const std::int64_t l
                = (matN > 0) ? juce::jlimit(std::int64_t{0},
                                            static_cast<std::int64_t>(matN) - 1,
                                            lRaw)
                             : 0;
            if (parsed.version >= 7 && cDto.hasMaterialWindowInFile && matN > 0)
            {
                const std::int64_t reqV = cDto.visibleLengthSamples > 0
                                              ? cDto.visibleLengthSamples
                                              : static_cast<std::int64_t>(-1);
                placed.emplace_back(
                    cDto.id,
                    material,
                    cDto.startSample,
                    l,
                    reqV,
                    cDto.materialWindowStartSamples,
                    cDto.materialWindowEndExclusiveSamples);
            }
            else if (cDto.visibleLengthSamples > 0)
            {
                placed.emplace_back(
                    cDto.id, material, cDto.startSample, l, cDto.visibleLengthSamples);
            }
            else
            {
                placed.emplace_back(
                    cDto.id, material, cDto.startSample, l, static_cast<std::int64_t>(-1));
            }
        }
        built.emplace_back(
            trDto.id,
            trDto.name,
            std::move(placed),
            juce::jlimit(0.0f, kTrackChannelFaderGainMax, trDto.channelFaderGain));
    }

    if (built.empty())
    {
        return juce::Result::fail("Project contained no valid tracks (internal).");
    }

    nextPlacedClipId_ = juce::jmax(parsed.nextPlacedClipId, static_cast<PlacedClipId>(maxClipInFile + 1));
    nextTrackId_ = juce::jmax(parsed.nextTrackId, static_cast<TrackId>(maxTrackInFile + 1));

    const std::shared_ptr<const SessionSnapshot> next = SessionSnapshot::withTracks(
        std::move(built),
        parsed.arrangementExtentSamples,
        parsed.leftLocatorSamples,
        parsed.rightLocatorSamples);
    if (next == nullptr)
    {
        return juce::Result::fail("Could not build session from project file.");
    }

    if (next->findTrackIndexById(parsed.activeTrackId) >= 0)
    {
        activeTrackId_ = parsed.activeTrackId;
    }
    else
    {
        activeTrackId_ = next->getTrack(0).getId();
    }

    std::atomic_store_explicit(&sessionSnapshot_, next, std::memory_order_release);

    currentProjectFile_ = file;

    const std::int64_t tlen = next->getArrangementExtentSamples();
    const std::int64_t hi = juce::jmax(std::int64_t{0}, tlen);
    const std::int64_t seekTo
        = juce::jlimit<std::int64_t>(0, hi, static_cast<std::int64_t>(parsed.playheadSamples));
    transport.requestSeek(seekTo);

    return juce::Result::ok();
}