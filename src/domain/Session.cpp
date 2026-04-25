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

std::int64_t Session::getTimelineLengthSamples() const noexcept
{
    const std::shared_ptr<const SessionSnapshot> snap = loadSessionSnapshotForAudioThread();
    if (snap == nullptr || snap->isEmpty())
    {
        return 0;
    }
    return snap->getDerivedTimelineLengthSamples();
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

    for (int i = 0; i < s->getNumTracks(); ++i)
    {
        const Track& t = s->getTrack(i);
        ProjectFileTrackV1 tr;
        tr.id = t.getId();
        tr.name = t.getName();
        for (int j = 0; j < t.getNumPlacedClips(); ++j)
        {
            const PlacedClip& p = t.getPlacedClip(j);
            ProjectFileClipV1 c;
            c.id = p.getId();
            c.startSample = p.getStartSample();
            c.sourcePath = p.getAudioClip().getSourceFilePath();
            if (c.sourcePath.isEmpty())
            {
                return juce::Result::fail("A clip in the session has no source file path; cannot save.");
            }
            tr.clips.push_back(std::move(c));
        }
        out.tracks.push_back(std::move(tr));
    }

    if (out.tracks.empty())
    {
        return juce::Result::fail("Session has no tracks to save.");
    }
    return writeProjectFile(file, out);
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
            const juce::File f(cDto.sourcePath);
            std::unique_ptr<AudioClip> loaded;
            const juce::Result lr = AudioFileLoader::loadFromFile(f, deviceSampleRate, loaded);
            if (!lr.wasOk())
            {
                outSkippedClipDetails.add(
                    cDto.sourcePath + " - " + lr.getErrorMessage());
                continue;
            }
            const std::shared_ptr<const AudioClip> material(std::move(loaded));
            placed.emplace_back(cDto.id, material, cDto.startSample);
        }
        built.emplace_back(trDto.id, trDto.name, std::move(placed));
    }

    if (built.empty())
    {
        return juce::Result::fail("Project contained no valid tracks (internal).");
    }

    nextPlacedClipId_ = juce::jmax(parsed.nextPlacedClipId, static_cast<PlacedClipId>(maxClipInFile + 1));
    nextTrackId_ = juce::jmax(parsed.nextTrackId, static_cast<TrackId>(maxTrackInFile + 1));

    const std::shared_ptr<const SessionSnapshot> next
        = SessionSnapshot::withTracks(std::move(built));
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

    const std::int64_t tlen = next->getDerivedTimelineLengthSamples();
    const std::int64_t hi = juce::jmax(std::int64_t{0}, tlen);
    const std::int64_t seekTo = juce::jlimit<std::int64_t>(
        0, hi, static_cast<std::int64_t>(parsed.playheadSamples));
    transport.requestSeek(seekTo);

    return juce::Result::ok();
}