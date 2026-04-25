#pragma once

// =============================================================================
// TrackLanesView  —  one `ClipWaveformView` per `Track` (message thread)
// =============================================================================
//
// ROLE
//   Sits in the main layout **below** `TimelineRulerView`. When `Session` publishes a snapshot
//   with N tracks, this component ensures N child lanes, each created with a stable `TrackId` and
//   the same session-wide x -> sample map as the ruler. It wires a small callback so selecting a
//   clip in one lane clears selection in the others. **Cross-track drag:** `ClipWaveformLaneHost`
//   callbacks resolve which lane is under the pointer, set a **single** drop ghost on that lane, and
//   clear ghosts — **no** track-type predicate; “valid lane” is geometric only (the header strip
//   is not a lane — pointer over a header is not a valid drop).
//
// See: `Session::getNumTracks` / `getTrackIdAtIndex`, `ClipWaveformView`, `TrackHeaderView`.
// =============================================================================

#include "domain/Track.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>
#include <vector>

class ClipWaveformView;
class TrackHeaderView;
class Session;
class Transport;

// ---------------------------------------------------------------------------
// TrackLanesView — vertical stack of per-track event lanes
// ---------------------------------------------------------------------------
class TrackLanesView : public juce::Component
{
public:
    // Width of the left name/active strip. `Main` insets the timeline ruler by the same value so
    // the ruler’s x <-> session-sample map matches the lane area.
    static constexpr int kTrackHeaderWidth = 120;

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

    // [Message thread] Screen point → which child `ClipWaveformView` (lane) that point falls in, or
    // `nullptr` if outside this view’s bounds (e.g. over the ruler or chrome).
    [[nodiscard]] ClipWaveformView* findLaneAtScreenPosition(juce::Point<int> screenPos);
    void setGhostOnLaneImpl(ClipWaveformView* target, std::int64_t startSample, std::int64_t lengthSamples);
    void clearAllGhostsImpl();

    Session& session_;
    Transport& transport_;
    std::vector<std::unique_ptr<TrackHeaderView>> headers_;
    std::vector<std::unique_ptr<ClipWaveformView>> lanes_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TrackLanesView)
};
