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

#include "diagnostics/ProjectLoadDiagnosticLog.h"
#include "diagnostics/UndoDiagnosticConfig.h"
#include "diagnostics/UndoDiagnosticFileLog.h"
#include "domain/AudioClip.h"
#include "domain/SessionRouting.h"
#include "domain/MixdownWavProbe.h"
#include "domain/TrackStereoPan.h"
#include "instruments/InstrumentTrackController.h"
#include "io/AudioFileLoader.h"
#include "io/ProjectFile.h"
#include "plugins/PluginInsertHost.h"
#include "transport/Transport.h"

#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>

#include <algorithm>
#include <cstdint>
#include <exception>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <new>
#include <optional>
#include <utility>
#include <limits>

namespace
{
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
    // One default audio lane plus `Stereo Out` master row (`withSingleEmptyTrack`). `activeTrackId_`
    // / `nextTrackId_` are set in the header (first track = 1; master id = 2).
    nextTrackId_ = 3;
}

Session::~Session() = default;

AudioMixdownProjectSettings Session::getAudioMixdownSettings() const noexcept
{
    return audioMixdown_;
}

void Session::setAudioMixdownSettings(AudioMixdownProjectSettings settings) noexcept
{
    settings.mp3BitRateKbps = clampMp3BitRateKbps(settings.mp3BitRateKbps);
    audioMixdown_ = std::move(settings);
}

juce::Result Session::addClipFromFileAtPlayhead(const juce::File& file,
                                                const double deviceSampleRate,
                                                const std::int64_t startSampleOnTimeline)
{
    if (activeTrackId_ == kInvalidTrackId)
    {
        return juce::Result::fail("No active track");
    }
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

    const std::shared_ptr<const AudioClip> material(std::move(loaded));
    // `startSampleOnTimeline` was read on this thread (typically from `Transport` once, at add
    // time) — the clip is spliced in as the new front of **active track**; ordering within that
    // lane matches Phase 2 (newest at index 0 of that track).
    const PlacedClipId newId = nextPlacedClipId_++;
    jassert(newId != kInvalidPlacedClipId);
    const std::shared_ptr<const SessionSnapshot> current = loadSessionSnapshotForAudioThread();
    if (current == nullptr)
    {
        return juce::Result::fail("Internal error: no session snapshot.");
    }
    {
        const int aIdx = current->findTrackIndexById(activeTrackId_);
        if (aIdx >= 0)
        {
            const TrackKind k = current->getTrack(aIdx).getKind();
            if (!trackKindAcceptsTimelineAudioClips(k))
            {
                return juce::Result::fail(
                    k == TrackKind::Master ? "Stereo Out cannot host audio clips."
                    : k == TrackKind::Group ? "Group tracks cannot host audio clips."
                                            : "Add clip targets an audio lane; activate an audio track header first.");
            }
        }
    }
    try
    {
        const std::shared_ptr<const SessionSnapshot> next
            = SessionSnapshot::withClipAddedAsNewestOnTargetTrack(
                *current, newId, material, startSampleOnTimeline, activeTrackId_);
        jassert(next != nullptr);
        if (next == nullptr)
        {
            return juce::Result::fail("Internal error: could not add clip to session.");
        }
        // Release: make this snapshot the one future acquires see; old snapshot is kept alive by any
        // in-flight callback/UI read until their shared_ptrs drop.
        std::atomic_store_explicit(&sessionSnapshot_, next, std::memory_order_release);
    }
    catch (const std::bad_alloc&)
    {
        // Do not touch sessionSnapshot_ — previous snapshot is still the published one.
        return juce::Result::fail(
            "Out of memory while building the session (copying clips). Remove clips or use smaller or shorter files.");
    }
    catch (const std::exception& e)
    {
        return juce::Result::fail(
            juce::String("Exception while adding clip: ") + e.what() + " (" + file.getFileName() + ")");
    }
    catch (...)
    {
        return juce::Result::fail("Unknown exception while adding clip: " + file.getFileName());
    }

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
    {
        const int tIx = current->findTrackIndexById(targetTrackId);
        jassert(tIx >= 0);
        if (tIx >= 0 && !trackKindAcceptsTimelineAudioClips(current->getTrack(tIx).getKind()))
        {
            return juce::Result::fail("Recorded takes can only target audio tracks.");
        }
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
    {
        const int tIx = current->findTrackIndexById(targetTrackId);
        jassert(tIx >= 0);
        if (tIx >= 0 && !trackKindAcceptsTimelineAudioClips(current->getTrack(tIx).getKind()))
        {
            return juce::Result::fail("Timeline audio clips attach to audio lanes only.");
        }
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

juce::String Session::uniqueDuplicateTrackName(const juce::String& sourceName) const
{
    const std::shared_ptr<const SessionSnapshot> current = loadSessionSnapshotForAudioThread();
    const auto nameTaken = [&current](const juce::String& candidate) {
        for (int i = 0; current != nullptr && i < current->getNumTracks(); ++i)
        {
            if (current->getTrack(i).getName() == candidate)
            {
                return true;
            }
        }
        return false;
    };
    const juce::String base = sourceName.trim().isEmpty() ? juce::String("Track") : sourceName.trim();
    const juce::String first = base + juce::String(juce::CharPointer_UTF8(" \xe2\x80\x94 kopia"));
    if (!nameTaken(first))
    {
        return first;
    }
    for (int n = 2; n < 10000; ++n)
    {
        const juce::String candidate = first + " " + juce::String(n);
        if (!nameTaken(candidate))
        {
            return candidate;
        }
    }
    return first;
}

std::optional<TrackId> Session::duplicateTrack(const TrackId sourceTrackId, juce::String newTrackName) noexcept
{
    const std::shared_ptr<const SessionSnapshot> current = loadSessionSnapshotForAudioThread();
    if (current == nullptr || sourceTrackId == kInvalidTrackId)
    {
        return std::nullopt;
    }
    const int srcIx = current->findTrackIndexById(sourceTrackId);
    if (srcIx < 0 || current->getTrack(srcIx).getKind() == TrackKind::Master)
    {
        return std::nullopt;
    }
    if (newTrackName.isEmpty())
    {
        newTrackName = uniqueDuplicateTrackName(current->getTrack(srcIx).getName());
    }
    const TrackId newId = nextTrackId_;
    jassert(newId != kInvalidTrackId);
    int clipIdsUsed = 0;
    const std::shared_ptr<const SessionSnapshot> next = SessionSnapshot::withTrackDuplicated(
        *current, sourceTrackId, newId, std::move(newTrackName), nextPlacedClipId_, clipIdsUsed);
    if (next == nullptr)
    {
        return std::nullopt;
    }
    // Monotonic ids are consumed only on success (same discipline as addTrack / the clip paths).
    nextTrackId_ = newId + 1;
    nextPlacedClipId_ += static_cast<PlacedClipId>(juce::jmax(0, clipIdsUsed));
    std::atomic_store_explicit(&sessionSnapshot_, next, std::memory_order_release);
    activeTrackId_ = newId;
    // Visual track groups: the copy sits directly below its source, so when the source belongs to
    // a group the copy becomes a member too (inserted right after the source id; spec §6). Layout
    // metadata only — nothing about the published snapshot changes here.
    for (VisualTrackGroup& g : visualTrackGroups_)
    {
        const auto it = std::find(g.memberTrackIds.begin(), g.memberTrackIds.end(), sourceTrackId);
        if (it != g.memberTrackIds.end())
        {
            g.memberTrackIds.insert(it + 1, newId);
            break; // Effective memberships never overlap.
        }
    }
    return newId;
}

void Session::addGroupTrack() noexcept
{
    const std::shared_ptr<const SessionSnapshot> current = loadSessionSnapshotForAudioThread();
    if (current == nullptr)
    {
        return;
    }
    int groupCount = 0;
    for (int i = 0; i < current->getNumTracks(); ++i)
    {
        if (current->getTrack(i).getKind() == TrackKind::Group)
        {
            ++groupCount;
        }
    }
    const TrackId newId = nextTrackId_++;
    jassert(newId != kInvalidTrackId);
    const juce::String newName = juce::String("Group ") + juce::String(groupCount + 1);
    const std::shared_ptr<const SessionSnapshot> next
        = SessionSnapshot::withTrackAdded(*current, newId, newName, TrackKind::Group);
    if (next == nullptr)
    {
        jassert(false);
        return;
    }
    std::atomic_store_explicit(&sessionSnapshot_, next, std::memory_order_release);
    activeTrackId_ = newId;
}

std::optional<TrackId> Session::addMidiTrack() noexcept
{
    const std::shared_ptr<const SessionSnapshot> current = loadSessionSnapshotForAudioThread();
    if (current == nullptr)
    {
        return std::nullopt;
    }
    int midiCount = 0;
    for (int i = 0; i < current->getNumTracks(); ++i)
    {
        if (current->getTrack(i).getKind() == TrackKind::Midi)
        {
            ++midiCount;
        }
    }
    const TrackId newId = nextTrackId_++;
    jassert(newId != kInvalidTrackId);
    const juce::String newName = juce::String("MIDI ") + juce::String(midiCount + 1);
    const std::shared_ptr<const SessionSnapshot> next
        = SessionSnapshot::withTrackAdded(*current, newId, newName, TrackKind::Midi);
    if (next == nullptr)
    {
        jassert(false);
        return std::nullopt;
    }
    std::atomic_store_explicit(&sessionSnapshot_, next, std::memory_order_release);
    activeTrackId_ = newId;
    return newId;
}

bool Session::setTrackMidiDestination(const TrackId trackId,
                                      const TrackId destinationTrackId) noexcept
{
    if (trackId == kInvalidTrackId)
    {
        return false;
    }
    const std::shared_ptr<const SessionSnapshot> current = loadSessionSnapshotForAudioThread();
    if (current == nullptr)
    {
        return false;
    }
    const int tIdx = current->findTrackIndexById(trackId);
    if (tIdx < 0 || current->getTrack(tIdx).getKind() != TrackKind::Midi)
    {
        return false;
    }
    if (current->getTrack(tIdx).getMidiDestinationTrackId() == destinationTrackId)
    {
        return false;
    }
    if (destinationTrackId != kInvalidTrackId)
    {
        const int dIdx = current->findTrackIndexById(destinationTrackId);
        if (dIdx < 0 || current->getTrack(dIdx).getKind() != TrackKind::Instrument)
        {
            return false;
        }
    }
    const std::shared_ptr<const SessionSnapshot> next
        = SessionSnapshot::withTrackMidiDestination(*current, trackId, destinationTrackId);
    if (next == nullptr)
    {
        jassert(false);
        return false;
    }
    std::atomic_store_explicit(&sessionSnapshot_, next, std::memory_order_release);
    return true;
}

bool Session::setTrackRoutedOutput(const TrackId trackId, const TrackId destTrackId) noexcept
{
    if (trackId == kInvalidTrackId || destTrackId == kInvalidTrackId)
    {
        return false;
    }
    const std::shared_ptr<const SessionSnapshot> current = loadSessionSnapshotForAudioThread();
    if (current == nullptr)
    {
        return false;
    }
    if (!session_routing::isLegalRoutedOutputTarget(*current, trackId, destTrackId))
    {
        return false;
    }
    const int tIdx = current->findTrackIndexById(trackId);
    if (tIdx < 0 || current->getTrack(tIdx).getRoutedOutputTrackId() == destTrackId)
    {
        return false;
    }
    const std::shared_ptr<const SessionSnapshot> next
        = SessionSnapshot::withTrackRoutedOutputTo(*current, trackId, destTrackId);
    if (next == nullptr)
    {
        return false;
    }
    const int afterIdx = next->findTrackIndexById(trackId);
    if (afterIdx < 0 || next->getTrack(afterIdx).getRoutedOutputTrackId() != destTrackId)
    {
        return false;
    }
    std::atomic_store_explicit(&sessionSnapshot_, next, std::memory_order_release);
    return true;
}

bool Session::insertTrackSend(const TrackId trackId,
                              const int uiSlotIndex,
                              const TrackId destTrackId,
                              const float amountLinear) noexcept
{
    if (trackId == kInvalidTrackId || destTrackId == kInvalidTrackId
        || uiSlotIndex < 0 || uiSlotIndex >= kTrackSendInspectorUiSlotCount)
    {
        return false;
    }
    const std::shared_ptr<const SessionSnapshot> current = loadSessionSnapshotForAudioThread();
    if (current == nullptr)
    {
        return false;
    }
    if (!session_routing::isLegalSendDestination(*current, trackId, destTrackId))
    {
        return false;
    }
    const int beforeIdx = current->findTrackIndexById(trackId);
    if (beforeIdx < 0)
    {
        return false;
    }
    const Track& beforeTrack = current->getTrack(beforeIdx);
    if (findTrackSendVectorIndexForUiSlot(beforeTrack.getSends(), uiSlotIndex) >= 0)
    {
        return false;
    }
    const int numBefore = beforeTrack.getNumSends();
    const float clamped = clampTrackSendAmountLinear(amountLinear);
    const std::shared_ptr<const SessionSnapshot> next = SessionSnapshot::withTrackSendInserted(
        *current, trackId, uiSlotIndex, destTrackId, clamped);
    if (next == nullptr)
    {
        return false;
    }
    const int afterIdx = next->findTrackIndexById(trackId);
    if (afterIdx < 0)
    {
        return false;
    }
    const Track& after = next->getTrack(afterIdx);
    if (after.getNumSends() != numBefore + 1)
    {
        return false;
    }
    const int insertedIndex = findTrackSendVectorIndexForUiSlot(after.getSends(), uiSlotIndex);
    if (insertedIndex < 0)
    {
        return false;
    }
    const TrackSend& inserted = after.getSend(insertedIndex);
    if (inserted.destTrackId != destTrackId || !inserted.enabled
        || inserted.uiSlotIndex != uiSlotIndex
        || std::fabs((double)(inserted.amountLinear - clamped)) > 1.0e-6)
    {
        return false;
    }
    std::atomic_store_explicit(&sessionSnapshot_, next, std::memory_order_release);
    return true;
}

bool Session::removeTrackSend(const TrackId trackId, const int uiSlotIndex) noexcept
{
    if (uiSlotIndex < 0 || uiSlotIndex >= kTrackSendInspectorUiSlotCount)
    {
        return false;
    }
    const std::shared_ptr<const SessionSnapshot> current = loadSessionSnapshotForAudioThread();
    if (current == nullptr)
    {
        return false;
    }
    const int beforeIdx = current->findTrackIndexById(trackId);
    if (beforeIdx < 0)
    {
        return false;
    }
    const Track& beforeTrack = current->getTrack(beforeIdx);
    if (findTrackSendVectorIndexForUiSlot(beforeTrack.getSends(), uiSlotIndex) < 0)
    {
        return false;
    }
    const int numBefore = beforeTrack.getNumSends();
    const std::shared_ptr<const SessionSnapshot> next
        = SessionSnapshot::withTrackSendRemoved(*current, trackId, uiSlotIndex);
    if (next == nullptr)
    {
        return false;
    }
    const int afterIdx = next->findTrackIndexById(trackId);
    if (afterIdx < 0 || next->getTrack(afterIdx).getNumSends() != numBefore - 1)
    {
        return false;
    }
    if (findTrackSendVectorIndexForUiSlot(next->getTrack(afterIdx).getSends(), uiSlotIndex) >= 0)
    {
        return false;
    }
    std::atomic_store_explicit(&sessionSnapshot_, next, std::memory_order_release);
    return true;
}

bool Session::setTrackSendDestination(const TrackId trackId,
                                      const int uiSlotIndex,
                                      const TrackId destTrackId) noexcept
{
    if (destTrackId == kInvalidTrackId || uiSlotIndex < 0
        || uiSlotIndex >= kTrackSendInspectorUiSlotCount)
    {
        return false;
    }
    const std::shared_ptr<const SessionSnapshot> current = loadSessionSnapshotForAudioThread();
    if (current == nullptr)
    {
        return false;
    }
    const int beforeIdx = current->findTrackIndexById(trackId);
    if (beforeIdx < 0)
    {
        return false;
    }
    const Track& beforeTrack = current->getTrack(beforeIdx);
    const int sendIndex = findTrackSendVectorIndexForUiSlot(beforeTrack.getSends(), uiSlotIndex);
    if (sendIndex < 0)
    {
        return false;
    }
    if (beforeTrack.getSend(sendIndex).destTrackId == destTrackId)
    {
        return false;
    }
    const std::shared_ptr<const SessionSnapshot> next
        = SessionSnapshot::withTrackSendDestination(*current, trackId, uiSlotIndex, destTrackId);
    if (next == nullptr)
    {
        return false;
    }
    const int afterIdx = next->findTrackIndexById(trackId);
    if (afterIdx < 0)
    {
        return false;
    }
    const int afterSendIndex
        = findTrackSendVectorIndexForUiSlot(next->getTrack(afterIdx).getSends(), uiSlotIndex);
    if (afterSendIndex < 0 || next->getTrack(afterIdx).getSend(afterSendIndex).destTrackId != destTrackId)
    {
        return false;
    }
    std::atomic_store_explicit(&sessionSnapshot_, next, std::memory_order_release);
    return true;
}

bool Session::setTrackSendAmount(const TrackId trackId,
                                 const int uiSlotIndex,
                                 const float amountLinear) noexcept
{
    if (uiSlotIndex < 0 || uiSlotIndex >= kTrackSendInspectorUiSlotCount)
    {
        return false;
    }
    const std::shared_ptr<const SessionSnapshot> current = loadSessionSnapshotForAudioThread();
    if (current == nullptr)
    {
        return false;
    }
    const int beforeIdx = current->findTrackIndexById(trackId);
    if (beforeIdx < 0)
    {
        return false;
    }
    const Track& beforeTrack = current->getTrack(beforeIdx);
    const int sendIndex = findTrackSendVectorIndexForUiSlot(beforeTrack.getSends(), uiSlotIndex);
    if (sendIndex < 0)
    {
        return false;
    }
    const float clamped = clampTrackSendAmountLinear(amountLinear);
    if (std::fabs((double)(beforeTrack.getSend(sendIndex).amountLinear - clamped)) < 1.0e-6)
    {
        return false;
    }
    const std::shared_ptr<const SessionSnapshot> next
        = SessionSnapshot::withTrackSendAmount(*current, trackId, uiSlotIndex, clamped);
    if (next == nullptr)
    {
        return false;
    }
    const int afterIdx = next->findTrackIndexById(trackId);
    if (afterIdx < 0)
    {
        return false;
    }
    const int afterSendIndex
        = findTrackSendVectorIndexForUiSlot(next->getTrack(afterIdx).getSends(), uiSlotIndex);
    if (afterSendIndex < 0
        || std::fabs((double)(next->getTrack(afterIdx).getSend(afterSendIndex).amountLinear - clamped))
               > 1.0e-6)
    {
        return false;
    }
    std::atomic_store_explicit(&sessionSnapshot_, next, std::memory_order_release);
    return true;
}

bool Session::setTrackSendEnabled(const TrackId trackId, const int uiSlotIndex, const bool enabled) noexcept
{
    if (uiSlotIndex < 0 || uiSlotIndex >= kTrackSendInspectorUiSlotCount)
    {
        return false;
    }
    const std::shared_ptr<const SessionSnapshot> current = loadSessionSnapshotForAudioThread();
    if (current == nullptr)
    {
        return false;
    }
    const int beforeIdx = current->findTrackIndexById(trackId);
    if (beforeIdx < 0)
    {
        return false;
    }
    const Track& beforeTrack = current->getTrack(beforeIdx);
    const int sendIndex = findTrackSendVectorIndexForUiSlot(beforeTrack.getSends(), uiSlotIndex);
    if (sendIndex < 0)
    {
        return false;
    }
    if (beforeTrack.getSend(sendIndex).enabled == enabled)
    {
        return false;
    }
    const std::shared_ptr<const SessionSnapshot> next
        = SessionSnapshot::withTrackSendEnabled(*current, trackId, uiSlotIndex, enabled);
    if (next == nullptr)
    {
        return false;
    }
    const int afterIdx = next->findTrackIndexById(trackId);
    if (afterIdx < 0)
    {
        return false;
    }
    const int afterSendIndex
        = findTrackSendVectorIndexForUiSlot(next->getTrack(afterIdx).getSends(), uiSlotIndex);
    if (afterSendIndex < 0 || next->getTrack(afterIdx).getSend(afterSendIndex).enabled != enabled)
    {
        return false;
    }
    std::atomic_store_explicit(&sessionSnapshot_, next, std::memory_order_release);
    return true;
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

TrackKind Session::getTrackKindAtIndex(const int index) const noexcept
{
    const std::shared_ptr<const SessionSnapshot> s = loadSessionSnapshotForAudioThread();
    if (s == nullptr || index < 0 || index >= s->getNumTracks())
    {
        return TrackKind::Audio;
    }
    return s->getTrack(index).getKind();
}

TrackId Session::findCanonicalMasterTrackId() const noexcept
{
    const std::shared_ptr<const SessionSnapshot> s = loadSessionSnapshotForAudioThread();
    if (s == nullptr)
    {
        return kInvalidTrackId;
    }
    return s->findCanonicalMasterTrackId();
}

std::optional<TrackId> Session::appendExperimentalInstrumentShellTrack(juce::String trackDisplayName) noexcept
{
    const std::shared_ptr<const SessionSnapshot> current = loadSessionSnapshotForAudioThread();
    if (current == nullptr)
    {
        return std::nullopt;
    }

    const TrackId newId = nextTrackId_;
    jassert(newId != kInvalidTrackId);
    if (trackDisplayName.isEmpty())
    {
        trackDisplayName = juce::String("Track ") + juce::String(newId);
    }
    try
    {
        const std::shared_ptr<const SessionSnapshot> next
            = SessionSnapshot::withTrackAdded(*current,
                                                newId,
                                                std::move(trackDisplayName),
                                                TrackKind::Instrument);
        if (next == nullptr)
        {
            jassert(false);
            return std::nullopt;
        }
        std::atomic_store_explicit(&sessionSnapshot_, next, std::memory_order_release);
    }
    catch (...)
    {
        return std::nullopt;
    }
    nextTrackId_ = newId + 1;
    return newId;
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
    {
        const int targIdx = current->findTrackIndexById(targetTrackId);
        if (targIdx < 0)
        {
            return;
        }
        if (!trackKindAcceptsTimelineAudioClips(current->getTrack(targIdx).getKind()))
        {
            return;
        }
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

std::optional<std::pair<PlacedClipId, PlacedClipId>> Session::splitClip(
    const PlacedClipId id,
    const std::int64_t splitSampleOnTimeline) noexcept
{
    if (id == kInvalidPlacedClipId)
    {
        return std::nullopt;
    }
    const std::shared_ptr<const SessionSnapshot> current = loadSessionSnapshotForAudioThread();
    if (current == nullptr || current->isEmpty())
    {
        return std::nullopt;
    }
    int foundTi = -1;
    int foundCi = -1;
    for (int ti = 0; ti < current->getNumTracks(); ++ti)
    {
        const Track& tr = current->getTrack(ti);
        for (int ci = 0; ci < tr.getNumPlacedClips(); ++ci)
        {
            if (tr.getPlacedClip(ci).getId() == id)
            {
                foundTi = ti;
                foundCi = ci;
                break;
            }
        }
        if (foundTi >= 0)
        {
            break;
        }
    }
    if (foundTi < 0)
    {
        return std::nullopt;
    }
    const PlacedClip& orig = current->getTrack(foundTi).getPlacedClip(foundCi);
    const std::int64_t S = orig.getStartSample();
    const std::int64_t V = orig.getEffectiveLengthSamples();
    if (V <= 1)
    {
        return std::nullopt;
    }
    if (!(splitSampleOnTimeline > S && splitSampleOnTimeline < S + V))
    {
        return std::nullopt;
    }
    const PlacedClipId leftId = nextPlacedClipId_++;
    const PlacedClipId rightId = nextPlacedClipId_++;
    const std::shared_ptr<const SessionSnapshot> next = SessionSnapshot::withClipSplit(
        *current, id, splitSampleOnTimeline, leftId, rightId);
    jassert(next != nullptr);
    std::atomic_store_explicit(&sessionSnapshot_, next, std::memory_order_release);
    return std::make_pair(leftId, rightId);
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
    if (checkTrackMoveAgainstVisualGroups(movedTrackId, destIndex).has_value())
    {
        // Defensive: callers (header drag) refuse group-splitting moves with a visible message
        // BEFORE reaching here; this guarantees the audio-model order can never silently diverge
        // from a displayable group's contiguity even via other call sites.
        return;
    }
    const std::shared_ptr<const SessionSnapshot> next
        = SessionSnapshot::withTrackReordered(*current, movedTrackId, destIndex);
    jassert(next != nullptr);
    std::atomic_store_explicit(&sessionSnapshot_, next, std::memory_order_release);
    // `activeTrackId_` is intentionally unchanged — UI highlights by id.
}

void Session::removeTrack(const TrackId removedTrackId) noexcept
{
    if (removedTrackId == kInvalidTrackId)
    {
        return;
    }
    const std::shared_ptr<const SessionSnapshot> current = loadSessionSnapshotForAudioThread();
    if (current == nullptr)
    {
        return;
    }
    const int masterIdx = current->findTrackIndexById(removedTrackId);
    if (masterIdx >= 0 && current->getTrack(masterIdx).getKind() == TrackKind::Master)
    {
        return;
    }
    const int removedIndex = current->findTrackIndexById(removedTrackId);
    if (removedIndex < 0)
    {
        return;
    }
    const std::shared_ptr<const SessionSnapshot> next
        = SessionSnapshot::withTrackRemoved(*current, removedTrackId);
    jassert(next != nullptr);
    std::atomic_store_explicit(&sessionSnapshot_, next, std::memory_order_release);

    if (activeTrackId_ != removedTrackId)
    {
        return;
    }
    const int n = next->getNumTracks();
    if (n <= 0)
    {
        activeTrackId_ = kInvalidTrackId;
        return;
    }
    const int focusIdx = juce::jmin(removedIndex, n - 1);
    activeTrackId_ = next->getTrack(focusIdx).getId();
}

void Session::removePlacedClip(const TrackId trackId, const PlacedClipId placedClipId) noexcept
{
    if (trackId == kInvalidTrackId || placedClipId == kInvalidPlacedClipId)
    {
        return;
    }
    const std::shared_ptr<const SessionSnapshot> current = loadSessionSnapshotForAudioThread();
    if (current == nullptr)
    {
        return;
    }
    const std::shared_ptr<const SessionSnapshot> next
        = SessionSnapshot::withPlacedClipRemoved(*current, trackId, placedClipId);
    jassert(next != nullptr);
    std::atomic_store_explicit(&sessionSnapshot_, next, std::memory_order_release);
}

void Session::restoreSessionSnapshotForUndo(std::shared_ptr<const SessionSnapshot> restored) noexcept
{
    if (restored == nullptr)
    {
        return;
    }
    const std::shared_ptr<const SessionSnapshot> repaired
        = SessionSnapshot::ensuringMasterInvariant(*restored);
    if (repaired == nullptr)
    {
        return;
    }
    if constexpr (undo_diagnostic::kUndoDiag)
    {
        writeUndoDiagnosticLogLine(
            "[UndoDiag] Session::restoreSessionSnapshotForUndo restoredTracks="
            + juce::String(restored->getNumTracks())
            + " repairedTracks=" + juce::String(repaired->getNumTracks()));
    }
    std::atomic_store_explicit(&sessionSnapshot_, repaired, std::memory_order_release);

    const int n = repaired->getNumTracks();
    if (n <= 0)
    {
        activeTrackId_ = kInvalidTrackId;
        return;
    }
    if (repaired->findTrackIndexById(activeTrackId_) < 0)
    {
        activeTrackId_ = repaired->getTrack(0).getId();
    }
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

bool Session::setTrackPreGainDb(const TrackId trackId, const float preGainDb) noexcept
{
    if (trackId == kInvalidTrackId)
    {
        return false;
    }
    const std::shared_ptr<const SessionSnapshot> current = loadSessionSnapshotForAudioThread();
    if (current == nullptr)
    {
        return false;
    }
    const int idx = current->findTrackIndexById(trackId);
    if (idx < 0)
    {
        return false;
    }
    const float db = sanitizeTrackPreGainDb(preGainDb);
    if (std::fabs((double)(db - current->getTrack(idx).getPreGainDb())) < 1.0e-6)
    {
        return false;
    }
    const std::shared_ptr<const SessionSnapshot> next
        = SessionSnapshot::withTrackPreGainDb(*current, trackId, db);
    jassert(next != nullptr);
    std::atomic_store_explicit(&sessionSnapshot_, next, std::memory_order_release);
    return true;
}

bool Session::setTrackInputAssignment(const TrackId trackId,
                                      const TrackInputAssignment assignment) noexcept
{
    if (trackId == kInvalidTrackId)
    {
        return false;
    }
    const std::shared_ptr<const SessionSnapshot> current = loadSessionSnapshotForAudioThread();
    if (current == nullptr)
    {
        return false;
    }
    const int idx = current->findTrackIndexById(trackId);
    if (idx < 0 || current->getTrack(idx).getKind() != TrackKind::Audio)
    {
        return false;
    }
    const TrackInputAssignment sanitized = sanitizeTrackInputAssignment(assignment);
    if (sanitized == current->getTrack(idx).getInputAssignment())
    {
        return false;
    }
    const std::shared_ptr<const SessionSnapshot> next
        = SessionSnapshot::withTrackInputAssignment(*current, trackId, sanitized);
    jassert(next != nullptr);
    std::atomic_store_explicit(&sessionSnapshot_, next, std::memory_order_release);
    return true;
}

bool Session::setTrackMidiInputAssignment(const TrackId trackId,
                                          TrackMidiInputAssignment assignment) noexcept
{
    if (trackId == kInvalidTrackId)
    {
        return false;
    }
    const std::shared_ptr<const SessionSnapshot> current = loadSessionSnapshotForAudioThread();
    if (current == nullptr)
    {
        return false;
    }
    const int idx = current->findTrackIndexById(trackId);
    if (idx < 0 || !trackKindAcceptsLiveMidiInput(current->getTrack(idx).getKind()))
    {
        return false;
    }
    const TrackMidiInputAssignment sanitized = sanitizeTrackMidiInputAssignment(std::move(assignment));
    if (sanitized == current->getTrack(idx).getMidiInputAssignment())
    {
        return false;
    }
    const std::shared_ptr<const SessionSnapshot> next
        = SessionSnapshot::withTrackMidiInputAssignment(*current, trackId, sanitized);
    jassert(next != nullptr);
    std::atomic_store_explicit(&sessionSnapshot_, next, std::memory_order_release);
    return true;
}

void Session::setTrackStereoPan(const TrackId trackId, const float stereoPan) noexcept
{
    if (trackId == kInvalidTrackId)
    {
        return;
    }
    const std::shared_ptr<const SessionSnapshot> current = loadSessionSnapshotForAudioThread();
    if (current == nullptr)
    {
        return;
    }
    const int idx = current->findTrackIndexById(trackId);
    if (idx < 0)
    {
        return;
    }
    const float p = sanitizeTrackStereoPan(stereoPan);
    if (std::fabs((double)(p - current->getTrack(idx).getStereoPan())) < 1.0e-6)
    {
        return;
    }
    const std::shared_ptr<const SessionSnapshot> next
        = SessionSnapshot::withTrackStereoPan(*current, trackId, p);
    jassert(next != nullptr);
    std::atomic_store_explicit(&sessionSnapshot_, next, std::memory_order_release);
}

void Session::setTrackOff(const TrackId trackId, const bool trackOff) noexcept
{
    if (trackId == kInvalidTrackId)
    {
        return;
    }
    const std::shared_ptr<const SessionSnapshot> current = loadSessionSnapshotForAudioThread();
    if (current == nullptr)
    {
        return;
    }
    const int idx = current->findTrackIndexById(trackId);
    if (idx < 0)
    {
        return;
    }
    if (current->getTrack(idx).getKind() == TrackKind::Master && trackOff)
    {
        return;
    }
    const std::shared_ptr<const SessionSnapshot> next = SessionSnapshot::withTrackOff(*current, trackId, trackOff);
    jassert(next != nullptr);
    std::atomic_store_explicit(&sessionSnapshot_, next, std::memory_order_release);
}

void Session::setTrackMuted(const TrackId trackId, const bool muted) noexcept
{
    // Solo lock (spec §2): while the current solo set is non-empty, normal Mute changes are
    // refused in THE COMMON COMMAND PATH — every UI entry (arrangement header, mixer strip) goes
    // through here. Load/undo restore mute via snapshot replacement and are unaffected.
    if (isSoloActive())
    {
        return;
    }
    if (trackId == kInvalidTrackId)
    {
        return;
    }
    const std::shared_ptr<const SessionSnapshot> current = loadSessionSnapshotForAudioThread();
    if (current == nullptr || current->findTrackIndexById(trackId) < 0)
    {
        return;
    }
    const std::shared_ptr<const SessionSnapshot> next
        = SessionSnapshot::withTrackMuted(*current, trackId, muted);
    jassert(next != nullptr);
    std::atomic_store_explicit(&sessionSnapshot_, next, std::memory_order_release);
}

// --------------------------------------------------------------------------- Solo
// Five separate explicit sets (one temporary + four memories); see Session.h Solo section.
// None of these methods touch `sessionSnapshot_` — stored Mute/Off flags are never rewritten and
// the audio thread is unaffected until the app publishes a new derived SoloMuteView.

namespace
{
    [[nodiscard]] std::vector<TrackId> dedupedSoloIds(std::vector<TrackId> ids) noexcept
    {
        std::vector<TrackId> out;
        out.reserve(ids.size());
        for (const TrackId id : ids)
        {
            if (id == kInvalidTrackId)
            {
                continue;
            }
            if (std::find(out.begin(), out.end(), id) == out.end())
            {
                out.push_back(id);
            }
        }
        return out;
    }
} // namespace

void Session::setActiveSoloMemoryIndex(const int indexOrMinusOne) noexcept
{
    if (indexOrMinusOne < -1 || indexOrMinusOne >= kSoloMemoryCount)
    {
        return;
    }
    activeSoloMemoryIndex_ = indexOrMinusOne;
}

std::vector<TrackId> Session::getCurrentSoloSetTrackIds() const
{
    if (activeSoloMemoryIndex_ >= 0 && activeSoloMemoryIndex_ < kSoloMemoryCount)
    {
        return soloMemories_[static_cast<std::size_t>(activeSoloMemoryIndex_)];
    }
    return temporarySoloSet_;
}

std::vector<TrackId> Session::getSoloMemoryTrackIds(const int memoryIndex) const
{
    if (memoryIndex < 0 || memoryIndex >= kSoloMemoryCount)
    {
        return {};
    }
    return soloMemories_[static_cast<std::size_t>(memoryIndex)];
}

void Session::setSoloMemoryTrackIds(const int memoryIndex, std::vector<TrackId> ids) noexcept
{
    if (memoryIndex < 0 || memoryIndex >= kSoloMemoryCount)
    {
        return;
    }
    soloMemories_[static_cast<std::size_t>(memoryIndex)] = dedupedSoloIds(std::move(ids));
}

bool Session::isTrackInCurrentSoloSet(const TrackId trackId) const noexcept
{
    if (trackId == kInvalidTrackId)
    {
        return false;
    }
    const std::vector<TrackId>& cur = (activeSoloMemoryIndex_ >= 0)
        ? soloMemories_[static_cast<std::size_t>(activeSoloMemoryIndex_)]
        : temporarySoloSet_;
    return std::find(cur.begin(), cur.end(), trackId) != cur.end();
}

bool Session::toggleTrackInCurrentSoloSet(const TrackId trackId) noexcept
{
    if (trackId == kInvalidTrackId)
    {
        return false;
    }
    const std::shared_ptr<const SessionSnapshot> current = loadSessionSnapshotForAudioThread();
    if (current == nullptr)
    {
        return false;
    }
    const int idx = current->findTrackIndexById(trackId);
    if (idx < 0)
    {
        return false;
    }
    if (current->getTrack(idx).getKind() == TrackKind::Master)
    {
        // No S on Stereo Out (spec §4).
        return false;
    }
    std::vector<TrackId>& cur = (activeSoloMemoryIndex_ >= 0)
        ? soloMemories_[static_cast<std::size_t>(activeSoloMemoryIndex_)]
        : temporarySoloSet_;
    const auto it = std::find(cur.begin(), cur.end(), trackId);
    if (it != cur.end())
    {
        cur.erase(it);
    }
    else
    {
        cur.push_back(trackId);
    }
    return true;
}

std::vector<TrackId> Session::getEffectiveSoloedTrackIds() const
{
    const std::shared_ptr<const SessionSnapshot> current = loadSessionSnapshotForAudioThread();
    std::vector<TrackId> out;
    if (current == nullptr)
    {
        return out;
    }
    const std::vector<TrackId> cur = getCurrentSoloSetTrackIds();
    out.reserve(cur.size());
    for (const TrackId id : cur)
    {
        if (id == kInvalidTrackId || current->findTrackIndexById(id) < 0)
        {
            continue; // Stale id of a deleted track: never a ghost solo.
        }
        if (std::find(out.begin(), out.end(), id) == out.end())
        {
            out.push_back(id);
        }
    }
    return out;
}

bool Session::isSoloActive() const noexcept
{
    return !getEffectiveSoloedTrackIds().empty();
}

void Session::resetTransientSoloStateForProjectLoad() noexcept
{
    temporarySoloSet_.clear();
    activeSoloMemoryIndex_ = -1;
}

// ------------------------------------------------------- Visual track groups
// Layout metadata only (see Session.h / VisualTrackGroup.h). None of these methods touch
// `sessionSnapshot_` — the audio model and every stored track property are unaffected.

void Session::setAllVisualTrackGroups(std::vector<VisualTrackGroup> groups) noexcept
{
    visualTrackGroups_.clear();
    visualTrackGroups_.reserve(groups.size());
    int maxSeenId = 0;
    for (VisualTrackGroup& g : groups)
    {
        g.memberTrackIds = dedupedSoloIds(std::move(g.memberTrackIds));
        if (g.memberTrackIds.empty())
        {
            continue; // Nothing left to reference; keep state minimal.
        }
        if (g.id <= 0)
        {
            g.id = nextVisualTrackGroupId_++;
        }
        maxSeenId = std::max(maxSeenId, g.id);
        visualTrackGroups_.push_back(std::move(g));
    }
    nextVisualTrackGroupId_ = std::max(nextVisualTrackGroupId_, maxSeenId + 1);
}

std::optional<int> Session::createVisualTrackGroup(juce::String name,
                                                   std::vector<TrackId> memberTrackIds) noexcept
{
    const std::shared_ptr<const SessionSnapshot> current = loadSessionSnapshotForAudioThread();
    if (current == nullptr)
    {
        return std::nullopt;
    }
    // Deduplicate, then order + validate against the current snapshot: every member must exist,
    // none may be the Master row, and the member indices must form one contiguous run.
    const std::vector<TrackId> wanted = dedupedSoloIds(std::move(memberTrackIds));
    std::vector<std::pair<int, TrackId>> indexAndId;
    indexAndId.reserve(wanted.size());
    for (const TrackId id : wanted)
    {
        const int idx = current->findTrackIndexById(id);
        if (idx < 0 || current->getTrack(idx).getKind() == TrackKind::Master)
        {
            return std::nullopt;
        }
        if (findVisualTrackGroupIdContainingTrack(id).has_value())
        {
            return std::nullopt; // No overlap / nesting with an existing effective membership.
        }
        indexAndId.emplace_back(idx, id);
    }
    if (indexAndId.size() < 2)
    {
        return std::nullopt;
    }
    std::sort(indexAndId.begin(), indexAndId.end());
    for (std::size_t i = 1; i < indexAndId.size(); ++i)
    {
        if (indexAndId[i].first != indexAndId[i - 1].first + 1)
        {
            return std::nullopt; // Only adjacent tracks may form a group.
        }
    }
    VisualTrackGroup group;
    group.id = nextVisualTrackGroupId_++;
    group.name = name.trim().isEmpty() ? juce::String("Group ") + juce::String(group.id)
                                       : name.trim();
    group.memberTrackIds.reserve(indexAndId.size());
    for (const auto& [idx, id] : indexAndId)
    {
        juce::ignoreUnused(idx);
        group.memberTrackIds.push_back(id);
    }
    group.collapsed = false;
    visualTrackGroups_.push_back(std::move(group));
    return visualTrackGroups_.back().id;
}

void Session::renameVisualTrackGroup(const int groupId, juce::String newName) noexcept
{
    const juce::String trimmed = newName.trim();
    if (trimmed.isEmpty())
    {
        return;
    }
    for (VisualTrackGroup& g : visualTrackGroups_)
    {
        if (g.id == groupId)
        {
            g.name = trimmed;
            return;
        }
    }
}

bool Session::setVisualTrackGroupCollapsed(const int groupId, const bool collapsed) noexcept
{
    for (VisualTrackGroup& g : visualTrackGroups_)
    {
        if (g.id == groupId)
        {
            if (g.collapsed == collapsed)
            {
                return false;
            }
            g.collapsed = collapsed;
            return true;
        }
    }
    return false;
}

void Session::removeVisualTrackGroup(const int groupId) noexcept
{
    visualTrackGroups_.erase(std::remove_if(visualTrackGroups_.begin(),
                                            visualTrackGroups_.end(),
                                            [groupId](const VisualTrackGroup& g)
                                            { return g.id == groupId; }),
                             visualTrackGroups_.end());
}

const VisualTrackGroup* Session::findVisualTrackGroupById(const int groupId) const noexcept
{
    for (const VisualTrackGroup& g : visualTrackGroups_)
    {
        if (g.id == groupId)
        {
            return &g;
        }
    }
    return nullptr;
}

std::vector<TrackId> Session::getEffectiveVisualGroupMemberTrackIds(const int groupId) const
{
    std::vector<TrackId> out;
    const VisualTrackGroup* group = findVisualTrackGroupById(groupId);
    const std::shared_ptr<const SessionSnapshot> current = loadSessionSnapshotForAudioThread();
    if (group == nullptr || current == nullptr)
    {
        return out;
    }
    std::vector<int> indices;
    indices.reserve(group->memberTrackIds.size());
    for (const TrackId id : group->memberTrackIds)
    {
        const int idx = current->findTrackIndexById(id);
        if (idx >= 0)
        {
            indices.push_back(idx); // Stale ids of deleted tracks are invisible here.
        }
    }
    std::sort(indices.begin(), indices.end());
    out.reserve(indices.size());
    for (const int idx : indices)
    {
        out.push_back(current->getTrack(idx).getId());
    }
    return out;
}

bool Session::isVisualTrackGroupDisplayable(const int groupId) const
{
    const std::vector<TrackId> members = getEffectiveVisualGroupMemberTrackIds(groupId);
    if (members.size() < 2)
    {
        return false;
    }
    const std::shared_ptr<const SessionSnapshot> current = loadSessionSnapshotForAudioThread();
    if (current == nullptr)
    {
        return false;
    }
    int prevIdx = current->findTrackIndexById(members.front());
    for (std::size_t i = 1; i < members.size(); ++i)
    {
        const int idx = current->findTrackIndexById(members[i]);
        if (idx != prevIdx + 1)
        {
            return false; // Members drifted apart → render as normal tracks (safe fallback).
        }
        prevIdx = idx;
    }
    return true;
}

std::optional<int> Session::findVisualTrackGroupIdContainingTrack(const TrackId trackId) const
{
    if (trackId == kInvalidTrackId)
    {
        return std::nullopt;
    }
    const std::shared_ptr<const SessionSnapshot> current = loadSessionSnapshotForAudioThread();
    if (current == nullptr || current->findTrackIndexById(trackId) < 0)
    {
        return std::nullopt;
    }
    for (const VisualTrackGroup& g : visualTrackGroups_)
    {
        if (std::find(g.memberTrackIds.begin(), g.memberTrackIds.end(), trackId)
            != g.memberTrackIds.end())
        {
            return g.id;
        }
    }
    return std::nullopt;
}

std::optional<juce::String> Session::checkTrackMoveAgainstVisualGroups(const TrackId movedTrackId,
                                                                       const int destIndex) const
{
    const std::shared_ptr<const SessionSnapshot> current = loadSessionSnapshotForAudioThread();
    if (current == nullptr)
    {
        return std::nullopt;
    }
    const int numTracks = current->getNumTracks();
    const int fromIndex = current->findTrackIndexById(movedTrackId);
    if (fromIndex < 0 || destIndex < 0 || destIndex >= numTracks || destIndex == fromIndex)
    {
        return std::nullopt; // Not a real move; Session::moveTrack refuses these itself.
    }
    // Simulate the resulting order (same splice `SessionSnapshot::withTrackReordered` performs).
    std::vector<TrackId> order;
    order.reserve(static_cast<std::size_t>(numTracks));
    for (int i = 0; i < numTracks; ++i)
    {
        order.push_back(current->getTrack(i).getId());
    }
    order.erase(order.begin() + fromIndex);
    order.insert(order.begin() + destIndex, movedTrackId);
    // Every group displayable BEFORE the move must stay contiguous AFTER it — a safe reorder may
    // not split a group or drop an outside track into its middle. Reorders WITHIN one group (the
    // member run is preserved, order inside it may change) pass this check by construction.
    for (const VisualTrackGroup& g : visualTrackGroups_)
    {
        if (!isVisualTrackGroupDisplayable(g.id))
        {
            continue;
        }
        const std::vector<TrackId> members = getEffectiveVisualGroupMemberTrackIds(g.id);
        std::vector<int> newIndices;
        newIndices.reserve(members.size());
        for (const TrackId id : members)
        {
            const auto it = std::find(order.begin(), order.end(), id);
            jassert(it != order.end());
            newIndices.push_back(static_cast<int>(std::distance(order.begin(), it)));
        }
        std::sort(newIndices.begin(), newIndices.end());
        // Contiguity catches every violation, including an outside track landing inside the
        // member run (that insertion splits the run's indices).
        for (std::size_t i = 1; i < newIndices.size(); ++i)
        {
            if (newIndices[i] != newIndices[i - 1] + 1)
            {
                return g.name;
            }
        }
    }
    return std::nullopt;
}

bool Session::setTrackMidiOutputChannel(const TrackId trackId, const int midiOutputChannel) noexcept
{
    if (trackId == kInvalidTrackId)
    {
        return false;
    }
    const int wanted = sanitizeTrackMidiOutputChannel(midiOutputChannel);
    const std::shared_ptr<const SessionSnapshot> current = loadSessionSnapshotForAudioThread();
    if (current == nullptr)
    {
        return false;
    }
    const int tIdx = current->findTrackIndexById(trackId);
    if (tIdx < 0 || current->getTrack(tIdx).getMidiOutputChannel() == wanted)
    {
        return false;
    }
    const std::shared_ptr<const SessionSnapshot> next
        = SessionSnapshot::withTrackMidiOutputChannel(*current, trackId, wanted);
    if (next == nullptr)
    {
        return false;
    }
    const int afterIdx = next->findTrackIndexById(trackId);
    if (afterIdx < 0 || next->getTrack(afterIdx).getMidiOutputChannel() != wanted)
    {
        return false;
    }
    std::atomic_store_explicit(&sessionSnapshot_, next, std::memory_order_release);
    return true;
}

void Session::setTrackName(const TrackId trackId, juce::String newName) noexcept
{
    if (trackId == kInvalidTrackId)
    {
        return;
    }
    const std::shared_ptr<const SessionSnapshot> current = loadSessionSnapshotForAudioThread();
    if (current == nullptr)
    {
        return;
    }
    const int idx = current->findTrackIndexById(trackId);
    if (idx < 0)
    {
        return;
    }
    if (current->getTrack(idx).getKind() == TrackKind::Master)
    {
        return;
    }
    const juce::String trimmed = newName.trim();
    if (trimmed.isEmpty() || trimmed == current->getTrack(idx).getName())
    {
        return;
    }
    const std::shared_ptr<const SessionSnapshot> next
        = SessionSnapshot::withTrackRenamed(*current, trackId, std::move(newName));
    jassert(next != nullptr);
    std::atomic_store_explicit(&sessionSnapshot_, next, std::memory_order_release);
}

void Session::setPlacedClipName(const PlacedClipId clipId, juce::String newDisplayName) noexcept
{
    if (clipId == kInvalidPlacedClipId)
    {
        return;
    }
    const std::shared_ptr<const SessionSnapshot> current = loadSessionSnapshotForAudioThread();
    if (current == nullptr)
    {
        return;
    }
    const juce::String trimmed = newDisplayName.trim();
    if (trimmed.isEmpty())
    {
        return;
    }
    bool foundWithDifferentName = false;
    for (int ti = 0; ti < current->getNumTracks() && !foundWithDifferentName; ++ti)
    {
        const Track& tr = current->getTrack(ti);
        for (int ci = 0; ci < tr.getNumPlacedClips(); ++ci)
        {
            const PlacedClip& p = tr.getPlacedClip(ci);
            if (p.getId() == clipId)
            {
                foundWithDifferentName = (p.getDisplayName() != trimmed);
                break;
            }
        }
    }
    if (!foundWithDifferentName)
    {
        return;
    }
    const std::shared_ptr<const SessionSnapshot> next
        = SessionSnapshot::withClipRenamed(*current, clipId, trimmed);
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
    nextTrackId_ = 3;
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

void Session::restoreArrangementExtentAfterRecording(const std::int64_t storedExtentBeforeRun,
                                                     const std::int64_t recordedResultEndSamples) noexcept
{
    const std::shared_ptr<const SessionSnapshot> cur = loadSessionSnapshotForAudioThread();
    if (cur == nullptr)
    {
        return;
    }
    // Never below the pre-run stored value (an older project's saved extent may be deliberate),
    // never below the audio content end, and never below the end of what the run recorded (MIDI
    // clips live outside the snapshot, so the caller passes the run's stop position); only the
    // temporary display headroom goes away.
    const std::int64_t target = juce::jmax(juce::jmax(std::int64_t{ 0 }, storedExtentBeforeRun),
                                           juce::jmax(cur->getDerivedTimelineLengthSamples(),
                                                      juce::jmax(std::int64_t{ 0 }, recordedResultEndSamples)));
    if (target == cur->getStoredArrangementExtentSamples())
    {
        return;
    }
    const std::shared_ptr<const SessionSnapshot> next = SessionSnapshot::withArrangementExtent(*cur, target);
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

double Session::timelineSampleRateOr(const double fallback) const noexcept
{
    return (timelineSampleRate_ > 0.0 && std::isfinite(timelineSampleRate_)) ? timelineSampleRate_
                                                                             : fallback;
}

void Session::initializeTimelineSampleRateIfUnset(const double rate) noexcept
{
    if (timelineSampleRate_ > 0.0 && std::isfinite(timelineSampleRate_))
    {
        return; // Initialized once; never silently re-stamped (TLD-1).
    }
    if (rate > 0.0 && std::isfinite(rate))
    {
        timelineSampleRate_ = rate;
    }
}

ProjectMusicalTime Session::getProjectMusicalTime() const noexcept
{
    const std::shared_ptr<const SessionSnapshot> snap = loadSessionSnapshotForAudioThread();
    return (snap != nullptr) ? snap->getProjectMusicalTime() : ProjectMusicalTime{};
}

void Session::setProjectBpm(double bpm) noexcept
{
    const std::shared_ptr<const SessionSnapshot> cur = loadSessionSnapshotForAudioThread();
    if (cur == nullptr)
    {
        return;
    }
    ProjectMusicalTime mt = cur->getProjectMusicalTime();
    mt.bpm = bpm;
    const std::shared_ptr<const SessionSnapshot> next = SessionSnapshot::withMusicalTime(*cur, mt);
    jassert(next != nullptr);
    std::atomic_store_explicit(&sessionSnapshot_, next, std::memory_order_release);
}

void Session::setProjectMusicalTime(ProjectMusicalTime musicalTime) noexcept
{
    const std::shared_ptr<const SessionSnapshot> cur = loadSessionSnapshotForAudioThread();
    if (cur == nullptr)
    {
        return;
    }
    const std::shared_ptr<const SessionSnapshot> next = SessionSnapshot::withMusicalTime(*cur, musicalTime);
    jassert(next != nullptr);
    std::atomic_store_explicit(&sessionSnapshot_, next, std::memory_order_release);
}

std::shared_ptr<const SessionSnapshot> Session::loadSessionSnapshotForAudioThread() const noexcept
{
    // Acquire: pair with the release stores in replace/clear so this read happens-after the last
    // full snapshot publish; the cost on the hot path is the atomic + shared_ptr retain.
    return std::atomic_load_explicit(&sessionSnapshot_, std::memory_order_acquire);
}

juce::Result Session::saveProjectToFile(Transport& transport,
                                       const juce::File& file,
                                       const double deviceSampleRate,
                                       PluginInsertHost* pluginHost,
                                       ExperimentalInstrumentCtlLookupFn instrumentCtlByTrackId,
                                       const bool arrangementSnapEnabled,
                                       const juce::String arrangementSnapResolutionKey,
                                       std::optional<ProjectFileMainWindowBoundsV1> mainWindowBoundsForSave,
                                       std::optional<ProjectFileMainWindowBoundsV1> midiEditorWindowBoundsForSave,
                                       std::optional<ProjectFileMidiEditorWorkspaceV1> midiEditorWorkspaceForSave,
                                       std::optional<ProjectFileTrackRowHeightsV1> trackRowHeightsForSave)
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
    // TLD-1 (steering §10.1): `deviceSampleRateAtSave` above stays a re-stamped informational
    // device fact; the timeline reference rate below is the authoritative interpretation domain
    // for the sample integers and is written exactly as held — a save under a different device
    // rate must NOT re-stamp it. A fresh never-initialized session adopts the device rate once.
    initializeTimelineSampleRateIfUnset(deviceSampleRate);
    out.timelineSampleRate = timelineSampleRate_;
    out.arrangementExtentSamples = s->getArrangementExtentSamples();
    out.leftLocatorSamples = s->getLeftLocatorSamples();
    out.rightLocatorSamples = s->getRightLocatorSamples();
    out.cycleEnabled = transport.readCycleEnabledForUi();
    // Solo (v25): persist ONLY the four memories' explicit TrackId sets, filtered to rows that
    // exist in the saved snapshot (stale ids of deleted tracks never reach disk). The temporary
    // set, the active memory selection, and all derived solo/mute states are deliberately omitted.
    for (int m = 0; m < kSoloMemoryCount; ++m)
    {
        std::vector<TrackId>& memOut = out.soloMemories[static_cast<std::size_t>(m)];
        memOut.clear();
        for (const TrackId id : soloMemories_[static_cast<std::size_t>(m)])
        {
            if (id != kInvalidTrackId && s->findTrackIndexById(id) >= 0
                && std::find(memOut.begin(), memOut.end(), id) == memOut.end())
            {
                memOut.push_back(id);
            }
        }
    }
    {
        const ProjectMusicalTime mt = s->getProjectMusicalTime();
        out.bpm = mt.bpm;
        out.timeSignatureNumerator = mt.numerator;
        out.timeSignatureDenominator = mt.denominator;
        out.ticksPerQuarter = mt.ticksPerQuarter;
    }
    out.snapEnabled = arrangementSnapEnabled;
    out.snapResolution = arrangementSnapResolutionKey;
    if (mainWindowBoundsForSave.has_value())
    {
        out.hasMainWindowBounds = true;
        out.mainWindowBounds = *mainWindowBoundsForSave;
    }
    if (midiEditorWindowBoundsForSave.has_value())
    {
        out.hasMidiEditorWindowBounds = true;
        out.midiEditorWindowBounds = *midiEditorWindowBoundsForSave;
    }
    if (midiEditorWorkspaceForSave.has_value())
    {
        out.hasMidiEditorWorkspace = true;
        out.midiEditorWorkspace = *midiEditorWorkspaceForSave;
    }
    // Visual track groups (v27): persist only groups that are displayable right now (≥2 existing
    // members, contiguous), with members filtered to the saved snapshot in session order — stale
    // ids of deleted tracks never reach disk. Collapsed state is saved; member row heights are the
    // NORMAL heights on `tracks[].rowHeight` (a collapsed group's 4 px display is never written).
    out.visualTrackGroups.clear();
    for (const VisualTrackGroup& g : visualTrackGroups_)
    {
        if (!isVisualTrackGroupDisplayable(g.id))
        {
            continue;
        }
        ProjectFileVisualTrackGroupV1 gOut;
        gOut.name = g.name;
        gOut.collapsed = g.collapsed;
        gOut.memberTrackIds = getEffectiveVisualGroupMemberTrackIds(g.id);
        out.visualTrackGroups.push_back(std::move(gOut));
    }
    // Row heights (v26): UI-owned — the preset key at the root plus each row's actual height,
    // looked up per track id below. Tracks the UI did not report keep `rowHeightPx = 0` (omitted).
    std::unordered_map<TrackId, int> rowHeightPxByTrackId;
    if (trackRowHeightsForSave.has_value())
    {
        out.trackRowHeightPreset = trackRowHeightsForSave->presetKey;
        for (const auto& [tid, px] : trackRowHeightsForSave->perTrackRowHeightPx)
        {
            if (tid != kInvalidTrackId && px > 0)
            {
                rowHeightPxByTrackId[tid] = px;
            }
        }
    }

    for (int i = 0; i < s->getNumTracks(); ++i)
    {
        const Track& t = s->getTrack(i);
        ProjectFileTrackV1 tr;
        tr.id = t.getId();
        tr.name = (t.getKind() == TrackKind::Master) ? juce::String(kMasterTrackDisplayName) : t.getName();
        tr.channelFaderGain = t.getChannelFaderGain();
        tr.preGainDb = t.getPreGainDb();
        tr.inputAssignment = t.getInputAssignment();
        tr.midiInputAssignment = t.getMidiInputAssignment();
        tr.stereoPan = t.getStereoPan();
        tr.off = (t.getKind() == TrackKind::Master) ? false : t.isTrackOff();
        tr.muted = t.isMuted();
        tr.midiOutputChannel = t.getMidiOutputChannel();
        tr.midiDestinationTrackId
            = (t.getKind() == TrackKind::Midi) ? t.getMidiDestinationTrackId() : kInvalidTrackId;
        if (const auto rhIt = rowHeightPxByTrackId.find(t.getId()); rhIt != rowHeightPxByTrackId.end())
        {
            tr.rowHeightPx = rhIt->second;
        }
        switch (t.getKind())
        {
        case TrackKind::Instrument:
            tr.kind = "instrument";
            break;
        case TrackKind::Master:
            tr.kind = "master";
            break;
        case TrackKind::Group:
            tr.kind = "group";
            break;
        case TrackKind::Midi:
            tr.kind = "midi";
            break;
        case TrackKind::Audio:
        default:
            tr.kind = "audio";
            break;
        }

        if (t.getKind() != TrackKind::Master)
        {
            tr.routedOutputTrackId = t.getRoutedOutputTrackId();
            for (int si = 0; si < t.getNumSends(); ++si)
            {
                const TrackSend& send = t.getSend(si);
                if (send.destTrackId == kInvalidTrackId)
                {
                    continue;
                }
                ProjectFileSendV1 row;
                row.destTrackId = send.destTrackId;
                row.amount = send.amountLinear;
                row.enabled = send.enabled;
                row.uiSlotIndex = send.uiSlotIndex;
                tr.sends.push_back(std::move(row));
            }
        }

        const bool timelineAudioLane = (t.getKind() == TrackKind::Audio);

        if (timelineAudioLane)
        {
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
                c.name = p.getDisplayName();
                tr.clips.push_back(std::move(c));
            }
        }
        else if (t.getNumPlacedClips() > 0)
        {
            return juce::Result::fail(
                "Internal error: instrument lane carries timeline audio clips; save aborted.");
        }

        // Insert chains live in `PluginInsertHost` keyed by TrackId for every row kind the
        // Inspector offers Pre/Post inserts on and the engine processes (Audio, Instrument, Group,
        // Master). Persistence must cover exactly that set: until 1.1.7 only Audio rows were
        // exported, so inserts on instrument / group / master rows vanished on save+reload.
        if (pluginHost != nullptr)
        {
            const PluginTrackChain ch = pluginHost->exportChain(tr.id);
            for (const auto& d : ch.slots)
            {
                if (!d.occupied)
                {
                    continue;
                }
                ProjectFileInsertV1 ins;
                ins.slotId = d.slotId;
                ins.stage = d.stage;
                ins.pluginVst3Path = d.vst3AbsolutePath;
                ins.pluginIdentifier = d.pluginIdentifier;
                if (d.opaqueState.getSize() > 0)
                {
                    ins.pluginStateBase64 = juce::Base64::toBase64(
                        d.opaqueState.getData(), (int)d.opaqueState.getSize());
                }
                tr.inserts.push_back(std::move(ins));
            }
        }
        out.tracks.push_back(std::move(tr));
    }

    if (instrumentCtlByTrackId)
    {
        for (int ti = 0; ti < s->getNumTracks(); ++ti)
        {
            const Track& tr = s->getTrack(ti);
            // TrackKind::Midi rows persist their clips through the same block format; their
            // controller is the plugin-less MIDI content controller (kind "MidiContent").
            if (tr.getKind() != TrackKind::Instrument && tr.getKind() != TrackKind::Midi)
            {
                continue;
            }
            InstrumentTrackController* const ctl = instrumentCtlByTrackId(tr.getId());
            if (ctl != nullptr && ctl->hasInstrumentTrack()
                && ctl->getExperimentalInstrumentDomainTrackId() == tr.getId())
            {
                out.experimentalInstrumentTracks.push_back(ctl->buildExperimentalInstrumentProjectBlock());
            }
        }
    }

    if (out.tracks.empty())
    {
        return juce::Result::fail("Session has no tracks to save.");
    }

    {
        out.hasAudioMixdown = true;
        ProjectFileAudioMixdownV1& am = out.audioMixdown;
        am.fileNameWithoutExtension = audioMixdown_.fileNameWithoutExtension;
        juce::String dirSpec = audioMixdown_.outputDirectorySpec.trim();
        if (dirSpec.isEmpty())
        {
            dirSpec = "Mixdown";
        }
        am.outputDirectory = dirSpec;
        am.fileType = (audioMixdown_.fileType == AudioMixdownProjectSettings::FileType::MpegLayer3)
                          ? juce::String("mpeg1Layer3")
                          : juce::String("wave");
        int wb = audioMixdown_.wavBitDepth;
        if (wb != 16 && wb != 24 && wb != 32)
        {
            wb = probeStereoFloatWavSupportedMixdown(deviceSampleRate) ? 32 : 24;
        }
        am.wavBitDepth = wb;
        am.mp3BitRateKbps = clampMp3BitRateKbps(audioMixdown_.mp3BitRateKbps);
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
                                          juce::String& outInfoNote,
                                          PluginInsertHost* pluginHost)
{
    outSkippedClipDetails.clear();
    outInfoNote.clear();

    ProjectFileV1 parsed;
    const juce::Result pr = readProjectFile(file, parsed);
    if (!pr.wasOk())
    {
        return pr;
    }
    return applyLoadedProjectModel(
        transport,
        file,
        parsed,
        deviceSampleRate,
        outSkippedClipDetails,
        outInfoNote,
        pluginHost,
        beginProjectLoadGeneration());
}

std::uint64_t Session::beginProjectLoadGeneration() noexcept
{
    return loadGeneration_.fetch_add(1, std::memory_order_relaxed) + 1;
}

std::uint64_t Session::getProjectLoadGeneration() const noexcept
{
    return loadGeneration_.load(std::memory_order_acquire);
}

juce::File Session::resolveProjectAudioFile(const juce::String& storedSourcePath,
                                            const juce::File& projectFolder) noexcept
{
    return resolveProjectAudioStoredPath(storedSourcePath, projectFolder);
}

juce::Result Session::applyLoadedProjectModel(Transport& transport,
                                              const juce::File& file,
                                              const ProjectFileV1& parsed,
                                              const double deviceSampleRate,
                                              juce::StringArray& outSkippedClipDetails,
                                              juce::String& outInfoNote,
                                              PluginInsertHost* pluginHost,
                                              const std::uint64_t loadGenerationForDeferredRestore,
                                              const PreDecodedMaterialByPath* preDecodedMaterial,
                                              std::vector<PendingPluginInsertRestore>* outDeferredInsertRestores)
{
    outSkippedClipDetails.clear();
    outInfoNote.clear();

    appendProjectLoadDiagnosticLine("apply: enter tracks=" + juce::String((int)parsed.tracks.size())
                                    + " experimentalInstrumentTracks="
                                    + juce::String((int)parsed.experimentalInstrumentTracks.size()));

    if (pluginHost != nullptr)
    {
        appendProjectLoadDiagnosticLine("apply: before removeAllPlugins");
        juce::Thread::sleep(120);
        pluginHost->removeAllPlugins();
        appendProjectLoadDiagnosticLine("apply: after removeAllPlugins");
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

    std::unordered_set<TrackId> instrumentLaneIds;
    instrumentLaneIds.reserve(parsed.experimentalInstrumentTracks.size());
    for (const auto& et : parsed.experimentalInstrumentTracks)
    {
        if (et.trackId != kInvalidTrackId)
        {
            instrumentLaneIds.insert(et.trackId);
        }
    }

    std::vector<Track> built;
    built.reserve(parsed.tracks.size());

    TrackId masterIdFromFile = kInvalidTrackId;
    for (const auto& trDto : parsed.tracks)
    {
        if (trDto.kind.equalsIgnoreCase("master"))
        {
            masterIdFromFile = trDto.id;
        }
    }

    appendProjectLoadDiagnosticLine("apply: before build tracks from parsed.tracks");

    for (const auto& trDto : parsed.tracks)
    {
        TrackKind tk = TrackKind::Audio;
        if (trDto.kind.equalsIgnoreCase("instrument"))
        {
            tk = TrackKind::Instrument;
        }
        else if (trDto.kind.equalsIgnoreCase("group"))
        {
            tk = TrackKind::Group;
        }
        else if (trDto.kind.equalsIgnoreCase("master"))
        {
            tk = instrumentLaneIds.count(trDto.id) > 0 ? TrackKind::Instrument : TrackKind::Master;
        }
        else if (trDto.kind.equalsIgnoreCase("midi"))
        {
            tk = TrackKind::Midi;
        }

        std::vector<PlacedClip> placed;

        if (tk == TrackKind::Audio)
        {
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
                std::shared_ptr<const AudioClip> material;
                if (preDecodedMaterial != nullptr)
                {
                    // Staged loader: decoded once per file off the message thread (shared by every
                    // clip that references the file); a miss falls back to the synchronous decode.
                    const auto hit = preDecodedMaterial->find(f.getFullPathName());
                    if (hit != preDecodedMaterial->end() && hit->second != nullptr)
                    {
                        material = hit->second;
                    }
                }
                if (material == nullptr)
                {
                    std::unique_ptr<AudioClip> loaded;
                    const juce::Result lr = AudioFileLoader::loadFromFile(f, deviceSampleRate, loaded);
                    if (!lr.wasOk())
                    {
                        outSkippedClipDetails.add(
                            cDto.sourcePath + " - " + lr.getErrorMessage());
                        continue;
                    }
                    material = std::shared_ptr<const AudioClip>(std::move(loaded));
                }
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
                if (cDto.name.isNotEmpty())
                {
                    placed.back() = placed.back().withDisplayName(cDto.name);
                }
            }
        }
        else if (!trDto.clips.empty())
        {
            outSkippedClipDetails.add(
                juce::String("[track ") + juce::String((juce::int64)trDto.id)
                + (tk == TrackKind::Master ? "] Skipping WAV clips stored on Stereo Out."
                                           : "] Skipping WAV clips stored on instrument lane."));
        }

        const bool trackOff = (tk == TrackKind::Master) ? false : trDto.off;
        const juce::String trackName = (tk == TrackKind::Master) ? juce::String(kMasterTrackDisplayName)
                                                                 : trDto.name;
        TrackId routeOut = kInvalidTrackId;
        if (tk != TrackKind::Master && tk != TrackKind::Midi)
        {
            routeOut = (trDto.routedOutputTrackId != kInvalidTrackId) ? trDto.routedOutputTrackId
                                                                      : masterIdFromFile;
        }
        std::vector<TrackSend> sends;
        if (tk != TrackKind::Master)
        {
            sends.reserve(trDto.sends.size());
            for (const ProjectFileSendV1& sDto : trDto.sends)
            {
                if (sDto.destTrackId == kInvalidTrackId)
                {
                    continue;
                }
                TrackSend s;
                s.destTrackId = sDto.destTrackId;
                s.amountLinear = clampTrackSendAmountLinear(sDto.amount);
                s.enabled = sDto.enabled;
                s.uiSlotIndex = sDto.uiSlotIndex;
                sends.push_back(s);
            }
        }
        built.emplace_back(
            trDto.id,
            trackName,
            std::move(placed),
            juce::jlimit(0.0f, kTrackChannelFaderGainMax, trDto.channelFaderGain),
            trackOff,
            trDto.muted,
            tk,
            sanitizeTrackStereoPan(trDto.stereoPan),
            routeOut,
            std::move(sends),
            trDto.midiOutputChannel,
            (tk == TrackKind::Midi) ? trDto.midiDestinationTrackId : kInvalidTrackId);
        // Pre-gain (v22) travels via the sanitizing COW helper instead of a positional ctor arg —
        // the ctor arg list has already silently dropped a new field once (see Track.h).
        if (trDto.preGainDb != kTrackPreGainDbDefault)
        {
            built.back() = built.back().withPreGainDb(trDto.preGainDb);
        }
        // Input assignment (v23) travels the same sanitizing COW route; Audio lanes only (other
        // kinds keep the default and never serialize the fields).
        if (tk == TrackKind::Audio
            && trDto.inputAssignment.kind != TrackInputKind::DefaultFirstInput)
        {
            built.back() = built.back().withInputAssignment(trDto.inputAssignment);
        }
        // Live MIDI input (v24): Instrument / Midi rows only; pre-v24 files carry no keys and keep
        // the default `None` — no automatic MIDI coupling is created when an older project opens.
        if (trackKindAcceptsLiveMidiInput(tk)
            && trDto.midiInputAssignment.mode != TrackMidiInputMode::None)
        {
            built.back() = built.back().withMidiInputAssignment(trDto.midiInputAssignment);
        }
        appendProjectLoadDiagnosticLine(
            "apply: built track id=" + juce::String((juce::int64)trDto.id) + " kind="
            + trDto.kind + " name=\"" + trackName + "\" clips=" + juce::String((int)trDto.clips.size())
            + " inserts=" + juce::String((int)trDto.inserts.size()));
    }

    appendProjectLoadDiagnosticLine("apply: after build tracks count=" + juce::String((int)built.size()));

    if (built.empty())
    {
        return juce::Result::fail("Project contained no valid tracks (internal).");
    }

    nextPlacedClipId_ = juce::jmax(parsed.nextPlacedClipId, static_cast<PlacedClipId>(maxClipInFile + 1));

    // TLD-1: adopt the loaded project's timeline reference rate (the reader already normalized it:
    // a v19 file arrives migrated from its stored `deviceSampleRateAtSave`). This deliberately
    // overwrites any fresh-session initialization — the file is authoritative for its own timeline
    // domain, and the current device rate never reinterprets the loaded sample integers.
    timelineSampleRate_ = parsed.timelineSampleRate;

    ProjectMusicalTime loadedMusical;
    loadedMusical.bpm = parsed.bpm;
    loadedMusical.numerator = parsed.timeSignatureNumerator;
    loadedMusical.denominator = parsed.timeSignatureDenominator;
    loadedMusical.ticksPerQuarter = parsed.ticksPerQuarter;

    appendProjectLoadDiagnosticLine("apply: before SessionSnapshot::withTracks");
    const std::shared_ptr<const SessionSnapshot> next = SessionSnapshot::withTracks(
        std::move(built),
        parsed.arrangementExtentSamples,
        parsed.leftLocatorSamples,
        parsed.rightLocatorSamples,
        loadedMusical,
        &instrumentLaneIds);
    appendProjectLoadDiagnosticLine("apply: after SessionSnapshot::withTracks");
    if (next == nullptr)
    {
        return juce::Result::fail("Could not build session from project file.");
    }

    TrackId maxTrackAfterRepair = 0;
    for (int i = 0; i < next->getNumTracks(); ++i)
    {
        maxTrackAfterRepair = juce::jmax(maxTrackAfterRepair, next->getTrack(i).getId());
    }
    nextTrackId_ = juce::jmax(parsed.nextTrackId, static_cast<TrackId>(maxTrackAfterRepair + 1));

    if (next->findTrackIndexById(parsed.activeTrackId) >= 0)
    {
        activeTrackId_ = parsed.activeTrackId;
    }
    else
    {
        activeTrackId_ = next->getTrack(0).getId();
    }

    appendProjectLoadDiagnosticLine("apply: before session snapshot publish activeTrackId="
                                    + juce::String((juce::int64)activeTrackId_));
    std::atomic_store_explicit(&sessionSnapshot_, next, std::memory_order_release);
    appendProjectLoadDiagnosticLine("apply: after session snapshot publish");

    // Solo (v25): a project always opens WITHOUT active solo — temporary set empty, no memory
    // selected — while the four saved memories are adopted intact (deduplicated, unknown ids
    // dropped against the freshly built track list; pre-v25 files simply yield four empty sets).
    resetTransientSoloStateForProjectLoad();
    for (int m = 0; m < kSoloMemoryCount; ++m)
    {
        std::vector<TrackId> mem;
        for (const TrackId id : parsed.soloMemories[static_cast<std::size_t>(m)])
        {
            if (id != kInvalidTrackId && next->findTrackIndexById(id) >= 0
                && std::find(mem.begin(), mem.end(), id) == mem.end())
            {
                mem.push_back(id);
            }
        }
        soloMemories_[static_cast<std::size_t>(m)] = std::move(mem);
    }

    // Visual track groups (v27): adopt saved groups after validating against the freshly built
    // track list — unknown member ids are dropped, and a group that ends up with <2 members,
    // non-contiguous members, or members shared with an earlier group is ignored entirely (its
    // tracks render as normal rows). Musical data is NEVER changed to repair layout metadata.
    visualTrackGroups_.clear();
    nextVisualTrackGroupId_ = 1;
    {
        std::vector<TrackId> claimedMemberIds;
        for (const ProjectFileVisualTrackGroupV1& g : parsed.visualTrackGroups)
        {
            std::vector<int> indices;
            indices.reserve(g.memberTrackIds.size());
            bool anyClaimed = false;
            for (const TrackId id : g.memberTrackIds)
            {
                const int idx = (id == kInvalidTrackId) ? -1 : next->findTrackIndexById(id);
                if (idx < 0 || next->getTrack(idx).getKind() == TrackKind::Master)
                {
                    continue;
                }
                anyClaimed = anyClaimed
                             || std::find(claimedMemberIds.begin(), claimedMemberIds.end(), id)
                                    != claimedMemberIds.end();
                indices.push_back(idx);
            }
            std::sort(indices.begin(), indices.end());
            bool contiguous = indices.size() >= 2 && !anyClaimed;
            for (std::size_t i = 1; contiguous && i < indices.size(); ++i)
            {
                contiguous = indices[i] == indices[i - 1] + 1;
            }
            if (!contiguous)
            {
                continue; // Safe fallback: these tracks stay normal visible rows.
            }
            VisualTrackGroup adopted;
            adopted.id = nextVisualTrackGroupId_++;
            adopted.name = g.name.isEmpty() ? juce::String("Group ") + juce::String(adopted.id)
                                            : g.name;
            adopted.collapsed = g.collapsed;
            adopted.memberTrackIds.reserve(indices.size());
            for (const int idx : indices)
            {
                const TrackId id = next->getTrack(idx).getId();
                adopted.memberTrackIds.push_back(id);
                claimedMemberIds.push_back(id);
            }
            visualTrackGroups_.push_back(std::move(adopted));
        }
    }

    currentProjectFile_ = file;

    if (parsed.hasAudioMixdown)
    {
        const ProjectFileAudioMixdownV1& m = parsed.audioMixdown;
        audioMixdown_.fileNameWithoutExtension = m.fileNameWithoutExtension;
        audioMixdown_.outputDirectorySpec = m.outputDirectory.trim();
        const juce::String ft = m.fileType.trim().toLowerCase();
        if (ft == "mpeg1layer3" || ft == "mp3" || ft == "mpeg")
        {
            audioMixdown_.fileType = AudioMixdownProjectSettings::FileType::MpegLayer3;
        }
        else
        {
            audioMixdown_.fileType = AudioMixdownProjectSettings::FileType::Wave;
        }
        if (m.wavBitDepth == 16 || m.wavBitDepth == 24 || m.wavBitDepth == 32)
        {
            audioMixdown_.wavBitDepth = m.wavBitDepth;
        }
        else
        {
            audioMixdown_.wavBitDepth = 0;
        }
        audioMixdown_.mp3BitRateKbps = clampMp3BitRateKbps(m.mp3BitRateKbps);
    }
    else
    {
        audioMixdown_ = defaultAudioMixdownProjectSettings();
    }

    const std::int64_t tlen = next->getArrangementExtentSamples();
    const std::int64_t hi = juce::jmax(std::int64_t{0}, tlen);
    const std::int64_t seekTo
        = juce::jlimit<std::int64_t>(0, hi, static_cast<std::int64_t>(parsed.playheadSamples));
    appendProjectLoadDiagnosticLine("apply: before transport seek playhead=" + juce::String(seekTo));
    transport.requestSeek(seekTo);
    transport.requestCycleEnabled(parsed.cycleEnabled);
    appendProjectLoadDiagnosticLine("apply: after transport seek");

    if (pluginHost != nullptr && parsed.version >= 8)
    {
        struct PendingPluginInsertRestore
        {
            TrackId trackId = kInvalidTrackId;
            PluginTrackChain chain;
        };
        auto pendingRestores = std::make_shared<std::vector<PendingPluginInsertRestore>>();
        pendingRestores->reserve(parsed.tracks.size());

        appendProjectLoadDiagnosticLine("apply: collect plugin insert restore rows");
        for (const auto& trDto : parsed.tracks)
        {
            // Every row kind may carry `inserts[]` (same set the writer exports — see
            // `saveProjectToFile`); the instrument plugin itself is restored separately by the
            // instrument controller and never appears in this array.
            PluginTrackChain chain;
            for (const auto& ins : trDto.inserts)
            {
                if (ins.pluginVst3Path.isEmpty())
                {
                    continue;
                }
                PluginInsertDescriptor d;
                d.slotId = ins.slotId;
                d.stage = ins.stage;
                d.occupied = true;
                d.vst3AbsolutePath = ins.pluginVst3Path;
                d.pluginIdentifier = ins.pluginIdentifier;
                if (ins.pluginStateBase64.isNotEmpty())
                {
                    juce::MemoryOutputStream mos;
                    if (!juce::Base64::convertFromBase64(mos, ins.pluginStateBase64))
                    {
                        outSkippedClipDetails.add("[plugin] track " + juce::String((juce::int64)trDto.id)
                                                  + " - invalid pluginStateBase64.");
                    }
                    else
                    {
                        d.opaqueState.replaceAll(mos.getData(), mos.getDataSize());
                    }
                }
                chain.slots.push_back(std::move(d));
            }
            if (!chain.slots.empty())
            {
                pendingRestores->push_back(PendingPluginInsertRestore{ trDto.id, std::move(chain) });
            }
        }

        if (pendingRestores->empty())
        {
            appendProjectLoadDiagnosticLine("apply: after plugin insert restore (none)");
        }
        else if (outDeferredInsertRestores != nullptr)
        {
            // Staged loader: the caller imports these one chain per message-loop turn (progress +
            // a paintable window between plug-in instantiations) and finalizes afterwards.
            outDeferredInsertRestores->clear();
            outDeferredInsertRestores->reserve(pendingRestores->size());
            for (PendingPluginInsertRestore& row : *pendingRestores)
            {
                outDeferredInsertRestores->push_back(
                    Session::PendingPluginInsertRestore{ row.trackId, std::move(row.chain) });
            }
            appendProjectLoadDiagnosticLine("apply: plugin insert restore handed to the staged loader count="
                                            + juce::String((int)outDeferredInsertRestores->size()));
        }
        else
        {
            appendProjectLoadDiagnosticLine("apply: defer plugin insert restore count="
                                            + juce::String((int)pendingRestores->size())
                                            + " gen="
                                            + juce::String((juce::int64)loadGenerationForDeferredRestore));
            juce::MessageManager::callAsync([pluginHost, pendingRestores, loadGenerationForDeferredRestore,
                                               sessionPtr = this]() {
                if (loadGenerationForDeferredRestore != sessionPtr->getProjectLoadGeneration())
                {
                    appendProjectLoadDiagnosticLine(
                        "load: deferred plugin insert restore stale gen="
                        + juce::String((juce::int64)loadGenerationForDeferredRestore));
                    return;
                }
                appendProjectLoadDiagnosticLine("load: deferred plugin insert restore begin count="
                                                + juce::String((int)pendingRestores->size()));
                for (const PendingPluginInsertRestore& row : *pendingRestores)
                {
                    appendProjectLoadDiagnosticLine("load: deferred before importChain trackId="
                                                    + juce::String((juce::int64)row.trackId) + " slots="
                                                    + juce::String((int)row.chain.slots.size()));
                    pluginHost->importChain(row.trackId, row.chain);
                    appendProjectLoadDiagnosticLine("load: deferred after importChain trackId="
                                                    + juce::String((juce::int64)row.trackId));
                }
                appendProjectLoadDiagnosticLine("load: deferred plugin insert restore complete");
            });
        }
    }

    appendProjectLoadDiagnosticLine("apply: exit ok");
    return juce::Result::ok();
}

void Session::setTimelineRulerTimeDisplay(const TimelineRulerTimeDisplay d) noexcept
{
    if (timelineRulerTimeDisplay_ == d)
    {
        return;
    }
    timelineRulerTimeDisplay_ = d;
    if (onTimelineRulerTimeDisplayChanged_)
    {
        onTimelineRulerTimeDisplayChanged_();
    }
}

void Session::setOnTimelineRulerTimeDisplayChanged(std::function<void()> callback) noexcept
{
    onTimelineRulerTimeDisplayChanged_ = std::move(callback);
}