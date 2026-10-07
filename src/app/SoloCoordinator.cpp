#include "app/SoloCoordinator.h"

#include "domain/Session.h"
#include "domain/SessionSnapshot.h"
#include "engine/PlaybackEngine.h"

#include <algorithm>

namespace
{
    constexpr int kSoloDriftTimerHz = 10; // light pointer-compare; re-derive within ~100 ms
}

SoloCoordinator::SoloCoordinator(Session& session, PlaybackEngine& engine, Callbacks callbacks)
    : session_(session)
    , engine_(engine)
    , callbacks_(std::move(callbacks))
{
    startTimerHz(kSoloDriftTimerHz);
}

SoloCoordinator::~SoloCoordinator()
{
    stopTimer();
    // Leave the engine without a solo view — a destroyed coordinator must not pin a stale one.
    engine_.publishSoloMuteView(nullptr);
}

// ---------------------------------------------------------------------------- commands

bool SoloCoordinator::toggleTrackSolo(const TrackId trackId)
{
    const std::shared_ptr<const SessionSnapshot> snap = session_.loadSessionSnapshotForAudioThread();
    if (snap == nullptr || trackId == kInvalidTrackId)
    {
        return false;
    }
    const int idx = snap->findTrackIndexById(trackId);
    if (idx < 0 || snap->getTrack(idx).getKind() == TrackKind::Master)
    {
        return false; // unknown row / no S on Stereo Out — mirrors Session::toggleTrackInCurrentSoloSet
    }

    // Proxy feasibility BEFORE mutating anything (atomic refusal, spec §5): evaluate the
    // candidate set the click would produce.
    std::vector<TrackId> candidate = session_.getCurrentSoloSetTrackIds();
    const auto it = std::find(candidate.begin(), candidate.end(), trackId);
    if (it != candidate.end())
    {
        candidate.erase(it);
    }
    else
    {
        candidate.push_back(trackId);
    }
    juce::String why;
    if (!proxyIsolationFeasible(candidate, why))
    {
        if (callbacks_.reportSoloRefusal)
        {
            callbacks_.reportSoloRefusal(why);
        }
        return false;
    }

    const int activeMemory = session_.getActiveSoloMemoryIndex();
    if (activeMemory >= 0 && callbacks_.executeUndoableSoloMemoryEdit)
    {
        // Editing a persistent memory: narrow undoable step targeting THAT memory (dirty).
        callbacks_.executeUndoableSoloMemoryEdit(
            "Solo memory " + juce::String(activeMemory + 1),
            activeMemory,
            [this, trackId] { return session_.toggleTrackInCurrentSoloSet(trackId); });
    }
    else
    {
        // Temporary set (or no undo wiring): direct edit — never undoable, never dirty.
        if (!session_.toggleTrackInCurrentSoloSet(trackId))
        {
            return false;
        }
    }
    republishDerivedView();
    return true;
}

void SoloCoordinator::handleSoloMemoryButtonClick(const int memoryIndex)
{
    if (memoryIndex < 0 || memoryIndex >= Session::kSoloMemoryCount)
    {
        return;
    }
    const int current = session_.getActiveSoloMemoryIndex();
    const int target = (current == memoryIndex) ? -1 : memoryIndex;

    // Atomic refusal also for a selection switch: activating a memory whose stored picture needs
    // impossible proxy isolation must leave the previous selection and sound state intact.
    const std::vector<TrackId> candidate
        = (target >= 0) ? session_.getSoloMemoryTrackIds(target) : std::vector<TrackId>{};
    juce::String why;
    if (!proxyIsolationFeasible(candidate, why))
    {
        if (callbacks_.reportSoloRefusal)
        {
            callbacks_.reportSoloRefusal(why);
        }
        return;
    }

    // Pure selection change: no content copied, temporary set preserved, not undoable, not dirty.
    session_.setActiveSoloMemoryIndex(target);
    republishDerivedView();
}

// ---------------------------------------------------------------------------- UI queries

bool SoloCoordinator::isTrackExplicitlySoloed(const TrackId trackId) const
{
    return session_.isTrackInCurrentSoloSet(trackId);
}

bool SoloCoordinator::isSoloActive() const
{
    return session_.isSoloActive();
}

int SoloCoordinator::activeSoloMemoryIndex() const
{
    return session_.getActiveSoloMemoryIndex();
}

SoloTrackAudioDecision SoloCoordinator::audioDecisionForTrack(const TrackId trackId) const
{
    const SoloMuteView* const view = lastPublishedView_.get();
    if (view == nullptr || !view->soloActive)
    {
        return SoloTrackAudioDecision::PassStoredMute;
    }
    for (const auto& [id, decision] : view->audioDecisions)
    {
        if (id == trackId)
        {
            return decision;
        }
    }
    return SoloTrackAudioDecision::ForcedSilent;
}

// ---------------------------------------------------------------------------- integration

bool SoloCoordinator::soloIsolationRequested(const TrackId destination) const
{
    if (lastPublishedView_ == nullptr || !lastPublishedView_->soloActive)
    {
        return false;
    }
    const std::vector<TrackId> isolated = partiallyIsolatedDestinations(*lastPublishedView_);
    return std::find(isolated.begin(), isolated.end(), destination) != isolated.end();
}

void SoloCoordinator::republishDerivedView()
{
    const std::shared_ptr<const SessionSnapshot> snap = session_.loadSessionSnapshotForAudioThread();
    std::shared_ptr<const SoloMuteView> next;
    if (snap != nullptr)
    {
        next = solo_mute_view::deriveSoloMuteView(*snap, session_.getCurrentSoloSetTrackIds());
    }

    // Proxy destinations whose partial-isolation need may have appeared OR disappeared with this
    // republish: refresh the union of old and new isolated destinations.
    std::vector<TrackId> refresh;
    if (lastPublishedView_ != nullptr && lastPublishedView_->soloActive)
    {
        refresh = partiallyIsolatedDestinations(*lastPublishedView_);
    }
    if (next != nullptr && next->soloActive)
    {
        for (const TrackId id : partiallyIsolatedDestinations(*next))
        {
            if (std::find(refresh.begin(), refresh.end(), id) == refresh.end())
            {
                refresh.push_back(id);
            }
        }
    }

    lastPublishedView_ = next;
    lastDerivedFromSnapshot_ = snap.get();
    engine_.publishSoloMuteView(next);

    if (callbacks_.refreshProxyDestination)
    {
        for (const TrackId id : refresh)
        {
            callbacks_.refreshProxyDestination(id);
        }
    }
    if (callbacks_.refreshSoloUi)
    {
        callbacks_.refreshSoloUi();
    }
}

// ---------------------------------------------------------------------------- internals

void SoloCoordinator::timerCallback()
{
    // Drift detection: any session command (track delete, routing/send/Off edits, undo restores)
    // publishes a NEW snapshot; the derived view must follow it. Pointer compare only — no work
    // when nothing changed. While solo is fully inactive (no view published, empty current set)
    // there is nothing to re-derive either.
    const std::shared_ptr<const SessionSnapshot> snap = session_.loadSessionSnapshotForAudioThread();
    if (snap.get() == lastDerivedFromSnapshot_)
    {
        return;
    }
    const bool viewActive = lastPublishedView_ != nullptr && lastPublishedView_->soloActive;
    if (!viewActive && session_.getCurrentSoloSetTrackIds().empty())
    {
        lastDerivedFromSnapshot_ = snap.get();
        return;
    }
    republishDerivedView();
}

std::vector<TrackId> SoloCoordinator::partiallyIsolatedDestinations(const SoloMuteView& view) const
{
    std::vector<TrackId> out = view.suppressOwnTransportClips;
    const std::shared_ptr<const SessionSnapshot> snap = session_.loadSessionSnapshotForAudioThread();
    if (snap != nullptr)
    {
        for (const TrackId sourceId : view.suppressRoutedMidiSources)
        {
            const int srcIdx = snap->findTrackIndexById(sourceId);
            if (srcIdx < 0)
            {
                continue;
            }
            const TrackId dest = snap->getTrack(srcIdx).getMidiDestinationTrackId();
            if (dest != kInvalidTrackId && std::find(out.begin(), out.end(), dest) == out.end())
            {
                out.push_back(dest);
            }
        }
    }
    return out;
}

bool SoloCoordinator::proxyIsolationFeasible(const std::vector<TrackId>& candidateSet,
                                             juce::String& outRefusalReason) const
{
    if (!callbacks_.instrumentPlayingProxy)
    {
        return true; // no proxy system wired (focused tests) — nothing to refuse
    }
    const std::shared_ptr<const SessionSnapshot> snap = session_.loadSessionSnapshotForAudioThread();
    if (snap == nullptr)
    {
        return true;
    }
    const std::shared_ptr<const SoloMuteView> candidateView
        = solo_mute_view::deriveSoloMuteView(*snap, candidateSet);
    if (candidateView == nullptr || !candidateView->soloActive)
    {
        return true;
    }
    for (const TrackId dest : partiallyIsolatedDestinations(*candidateView))
    {
        if (!callbacks_.instrumentPlayingProxy(dest))
        {
            continue; // live instrument: event-level gating handles isolation
        }
        const bool liveUsable = callbacks_.instrumentLiveSourceUsable
                                && callbacks_.instrumentLiveSourceUsable(dest);
        if (!liveUsable)
        {
            const int idx = snap->findTrackIndexById(dest);
            const juce::String name = idx >= 0 ? snap->getTrack(idx).getName() : juce::String(dest);
            outRefusalReason
                = "Solo refused: isolating a single voice inside \"" + name
                  + "\" is not possible right now — the instrument plays its rendered proxy "
                    "(one baked mix) and no live source (Secondary) is available. Load the "
                    "plugin or assign a Secondary, or solo the whole instrument track instead.";
            return false;
        }
    }
    return true;
}
