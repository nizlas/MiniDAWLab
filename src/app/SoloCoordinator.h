#pragma once

// =============================================================================
// SoloCoordinator — message-thread owner of the Solo feature's command + publish flow
// =============================================================================
//
// ROLE
//   One shared command path for every S button (arrangement header AND mixer strip) and for the
//   four-button Solo memory strip, plus the single place that re-derives and republishes the
//   engine's `SoloMuteView` after anything relevant changes. `Session` owns the five explicit
//   sets (storage + persistence); this class owns POLICY: memory-button semantics, the narrow
//   undo wrapping for memory content edits, the proxy partial-isolation feasibility refusal, and
//   keeping the published derived view in sync with the session.
//
// PUBLISH DISCIPLINE
//   `republishDerivedView()` derives on the message thread and hands an immutable view to
//   `PlaybackEngine::publishSoloMuteView`. It runs after every solo command, after undo/redo of a
//   memory step (`UndoRedoCoordinator::Callbacks::refreshSoloStateAfterUndoRestore`), after
//   project load, and from a light timer that pointer-compares the published session snapshot —
//   so routing/track/Off edits made through ANY session command re-derive within one timer tick
//   without hooking all ~40 snapshot publish sites. Until the re-derive lands, tracks missing
//   from the stale view are ForcedSilent while solo is active (never falsely audible).
//
// PROXY REFUSAL (spec §5)
//   A solo command whose derived picture needs PARTIAL isolation inside a destination currently
//   playing its proxy (own clips gated, or a gated subset of routed sources) is checked up
//   front: when a live source (Secondary) can take over, the command proceeds and
//   `soloIsolationRequested` makes `ProxyPlaybackCoordinator` switch that destination to
//   SecondaryLive; when no live source is usable the command is REFUSED atomically — previous
//   set, memory selection and sound state untouched, and the user is told why. Whole-destination
//   solo never needs isolation, so a valid proxy may keep playing for it.
// =============================================================================

#include "domain/Track.h"
#include "engine/SoloMuteView.h"

#include <juce_events/juce_events.h>

#include <functional>
#include <memory>
#include <vector>

class Session;
class PlaybackEngine;

class SoloCoordinator final : private juce::Timer
{
public:
    struct Callbacks
    {
        /// True when this Instrument destination currently plays its proxy (message thread).
        std::function<bool(TrackId)> instrumentPlayingProxy;
        /// True when a live source (Secondary) can be made usable for this destination.
        std::function<bool(TrackId)> instrumentLiveSourceUsable;
        /// Re-evaluate one proxy destination (its `soloIsolationRequested` input may have changed).
        std::function<void(TrackId)> refreshProxyDestination;
        /// Repaint everything that shows solo state (S cells, memory strip, locked M faces).
        std::function<void()> refreshSoloUi;
        /// User-visible refusal report (modal/status line). Optional; refusal happens regardless.
        std::function<void(const juce::String&)> reportSoloRefusal;
        /// Wrap a memory-content mutation in the NARROW undo step
        /// (`UndoRedoCoordinator::executeUndoableSoloMemoryEdit`). Optional: absent (focused
        /// tests) applies the mutator directly with no undo/dirty bookkeeping.
        std::function<void(const juce::String& label, int memoryIndex, std::function<bool()>)>
            executeUndoableSoloMemoryEdit;
    };

    SoloCoordinator(Session& session, PlaybackEngine& engine, Callbacks callbacks);
    ~SoloCoordinator() override;

    SoloCoordinator(const SoloCoordinator&) = delete;
    SoloCoordinator& operator=(const SoloCoordinator&) = delete;

    // ------------------------------------------------------------- commands
    /// S click (same path from arrangement and mixer; uses the row's explicit TrackId). Edits the
    /// CURRENT set: the temporary set directly (not undoable, not dirty), an active memory
    /// through the narrow undo step (undoable, dirty). Returns false when refused (unknown id,
    /// Master row, or the proxy partial-isolation refusal) — nothing changed then.
    bool toggleTrackSolo(TrackId trackId);

    /// Memory button click (spec §3): inactive button → activate that memory's EXISTING set
    /// (also switching directly from another memory); the active button → deactivate back to the
    /// temporary set (preserved unchanged the whole time). Never copies content between sets;
    /// selection changes are not undoable and not dirty. Refused atomically when the memory's
    /// picture needs impossible proxy isolation.
    void handleSoloMemoryButtonClick(int memoryIndex);

    // ------------------------------------------------------------- UI queries
    [[nodiscard]] bool isTrackExplicitlySoloed(TrackId trackId) const;
    [[nodiscard]] bool isSoloActive() const;
    [[nodiscard]] bool isMuteChangeLocked() const { return isSoloActive(); }
    [[nodiscard]] int activeSoloMemoryIndex() const;
    /// Derived strip decision for the M face (distinct solo-silenced tint): from the LAST
    /// PUBLISHED view. PassStoredMute while solo is inactive.
    [[nodiscard]] SoloTrackAudioDecision audioDecisionForTrack(TrackId trackId) const;

    // ------------------------------------------------------------- integration
    /// `ProxyPlaybackCoordinator::Dependencies::soloIsolationRequested`: true when the CURRENT
    /// published picture partially isolates inside `destination` (own clips gated or ≥1 routed
    /// source gated while the destination strip is open).
    [[nodiscard]] bool soloIsolationRequested(TrackId destination) const;

    /// Re-derive from `Session` + current snapshot and publish to the engine; refreshes solo UI.
    /// Called by commands internally; call explicitly after project load and undo restore.
    void republishDerivedView();

private:
    void timerCallback() override;

    /// Instrument destinations that `view` partially isolates (needs a live source when proxied).
    [[nodiscard]] std::vector<TrackId> partiallyIsolatedDestinations(const SoloMuteView& view) const;
    /// Candidate-set feasibility: derive a view for `candidateSet` and refuse (report + false)
    /// when some partially isolated destination plays a proxy and no live source is usable.
    [[nodiscard]] bool proxyIsolationFeasible(const std::vector<TrackId>& candidateSet,
                                              juce::String& outRefusalReason) const;

    Session& session_;
    PlaybackEngine& engine_;
    Callbacks callbacks_;

    /// Last published view (message-thread copy of what the engine consumes) + the snapshot it
    /// was derived from (pointer identity only — drift detection in `timerCallback`).
    std::shared_ptr<const SoloMuteView> lastPublishedView_;
    const void* lastDerivedFromSnapshot_ = nullptr;
};
