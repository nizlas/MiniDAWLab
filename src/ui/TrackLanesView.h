#pragma once

// =============================================================================
// TrackLanesView  —  one `ClipWaveformView` per `Track` (message thread)
// =============================================================================
//
// ROLE
//   Sits in the main layout **below** `TimelineRulerView`. When `Session` publishes a snapshot
//   with N tracks, this component ensures N child lanes, each created with a stable `TrackId` and
//   the same session-wide x -> sample map as the ruler. It wires a small callback so selecting a
//   clip in one lane clears selection in the others — **no** cross-track drag (clips stay in their
//   track for moves).
//
// See: `Session::getNumTracks` / `getTrackIdAtIndex`, `ClipWaveformView`.
// =============================================================================

#include "domain/Track.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>
#include <vector>

class ClipWaveformView;
class Session;
class Transport;

// ---------------------------------------------------------------------------
// TrackLanesView — vertical stack of per-track event lanes
// ---------------------------------------------------------------------------
class TrackLanesView : public juce::Component
{
public:
    ~TrackLanesView() override;

    // [Message thread] `session` / `transport` outlive this view. Rebuilds child lanes in `resized`
    // to match the current `SessionSnapshot` track list.
    TrackLanesView(Session& session, Transport& transport);

    void resized() override;

    // [Message thread] `Main` can call this after `Session::addTrack` so a new `ClipWaveformView`
    // is created before layout without waiting for a user resize.
    void syncTracksFromSession();

private:
    // [Message thread] Match `std::vector` size and `TrackId` order to the session snapshot; id-
    // order changes (not in this project) would rebuild every lane.
    void rebuildChildLanesIfNeeded();

    Session& session_;
    Transport& transport_;
    std::vector<std::unique_ptr<ClipWaveformView>> lanes_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TrackLanesView)
};
