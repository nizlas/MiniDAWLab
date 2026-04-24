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

#include <juce_core/juce_core.h>

#include <memory>
#include <utility>

Session::Session()
    : sessionSnapshot_(SessionSnapshot::withSingleEmptyTrack(TrackId{1}))
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

    const std::shared_ptr<const AudioClip> material(std::move(loaded));
    // `startSampleOnTimeline` was read on this thread (typically from `Transport` once, at add
    // time) — the clip is spliced in as the new front of **active track**; ordering within that
    // lane matches Phase 2 (newest at index 0 of that track).
    const PlacedClipId newId = nextPlacedClipId_++;
    jassert(newId != kInvalidPlacedClipId);
    const std::shared_ptr<const SessionSnapshot> current = loadSessionSnapshotForAudioThread();
    const std::shared_ptr<const SessionSnapshot> next = SessionSnapshot::withClipAddedAsNewestOnTargetTrack(
        *current, newId, material, startSampleOnTimeline, activeTrackId_);
    jassert(next != nullptr);
    // Release: make this snapshot the one future acquires see; old snapshot is kept alive by any
    // in-flight callback/UI read until their shared_ptrs drop.
    std::atomic_store_explicit(&sessionSnapshot_, next, std::memory_order_release);
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
    const std::shared_ptr<const SessionSnapshot> next = SessionSnapshot::withTrackAdded(*current, newId);
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

void Session::clearClip() noexcept
{
    // “No file”: one **empty** default track (id 1), same as a fresh `Session` — not the shared
    // *zero-track* `createEmpty` singleton.
    const std::shared_ptr<const SessionSnapshot> empty = SessionSnapshot::withSingleEmptyTrack(TrackId{1});
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