#pragma once

// =============================================================================
// FollowAutoscrollGovernor — page/event-driven follow scheduling (per window)
// GlobalFollowWorkCoordinator — cross-window follow budget + gesture yield
// =============================================================================
// [Message thread only.]
//
// Both follow implementations (main arrangement in `MainAppWindow`, MIDI editor roll in
// `ExperimentalPianoRollView`) autoscroll by mutating their viewport, and every such mutation
// forces a full repaint of the scrolled surface. Two freeze rounds
// (`MAIN_FOLLOW_ZOOM_UI_FREEZE_FORENSIC_AUDIT.md`) proved that follow must be **page/event
// driven**, never frame-driven:
//
//  - A wall-clock rate limit degenerates under load (wall time elapses while the thread paints).
//  - A per-frame edge test ("playhead past 92 % this frame?") re-arms forever whenever a page
//    cannot bring the playhead back inside the view (extent clamp near the arrangement end, or
//    fine zoom where the playhead crosses the view between two frames) — a pan storm at *normal*
//    zoom levels.
//  - Two windows following independently double the load on the one message thread.
//
// `FollowAutoscrollGovernor` (one per window) turns follow into a small state machine. A page is
// admitted only when **all** of the following hold:
//
//  1. **Boundary trigger** — the playhead is outside the comfort band of the *current* view.
//  2. **Armed** — after a page, follow re-arms only when the playhead has been observed *inside*
//     the comfort band again (the page succeeded and playback is approaching the boundary from
//     within), or after `pageRearmIntervalMs` for the fine-zoom/clamped cases where the playhead
//     never lands inside (degrades to sparse page-flips instead of chasing every frame).
//  3. **Capacity** — the previous frame tick was on time, and at least one on-time tick has been
//     observed since the last page (the tick after a page includes its repaint cost, so expensive
//     repaints automatically postpone the next page until the thread demonstrably caught up).
//  4. **No user gesture** — the user has not panned/zoomed this window very recently.
//
// Callers additionally consult `GlobalFollowWorkCoordinator` (one per process) so that:
//  - at most one follow page happens across *all* windows per `kGlobalMinPageIntervalMs`, and
//  - a user gesture in one window makes follow in *other* windows yield for
//    `kCrossWindowGestureHoldoffMs` (Follow stays visually ON; it just never fights interaction).
//
// Explicit single-shot moves (seek snap, Follow toggled ON) bypass the gates by design but must
// report through `notePageApplied` on both objects so frame-driven paging pauses right after.
// =============================================================================

class FollowAutoscrollGovernor final
{
public:
    /// Outcome of one page decision. Non-`apply` values feed diagnostic counters.
    enum class Decision
    {
        apply,                  ///< Page now (caller pans once, then calls `notePageApplied`).
        skipNotNeeded,          ///< Playhead inside the comfort band — nothing to do.
        skipUserGestureHoldoff, ///< User panned/zoomed this window very recently.
        skipLateFrame,          ///< Last frame interval shows the message thread is behind.
        skipAwaitCleanFrame,    ///< Previous page's repaint not yet followed by an on-time frame.
        skipBoundaryWait,       ///< Not re-armed: last page never captured the playhead (clamp /
                                ///< fine zoom) and the sparse re-arm interval has not elapsed.
        skipMinInterval,        ///< Safety spacing between pages.
    };

    struct Policy
    {
        /// Comfort band inside the view; a page triggers only when the playhead exits it.
        double leftBoundaryFraction = 0.08;
        double rightBoundaryFraction = 0.92;
        /// Late threshold for one frame tick (nominal ~16.7 ms at 60 Hz; ~45 ms = clearly behind).
        double lateFrameIntervalMs = 45.0;
        /// Minimum spacing between pages of this window (safety; normal-zoom pages are naturally
        /// seconds apart because the playhead must cross the comfort band first).
        double minPageIntervalMs = 150.0;
        /// Sparse re-arm when a page could not capture the playhead inside the band (extent clamp,
        /// fine zoom): degrade to page-flips at most this often instead of chasing.
        double pageRearmIntervalMs = 750.0;
        /// Follow yield window after a user viewport gesture in this window.
        double userGestureHoldoffMs = 250.0;
    };

    explicit FollowAutoscrollGovernor(const Policy policy = {}) noexcept : policy_(policy) {}

    /// Call once per UI frame tick (frame callback / animation timer), before any page decision in
    /// that tick. Measures the frame interval and clears the clean-frame debt when on time.
    void noteFrameTick(const double nowMs) noexcept
    {
        if (lastFrameTickMs_ > 0.0)
        {
            lastFrameIntervalMs_ = nowMs - lastFrameTickMs_;
        }
        lastFrameTickMs_ = nowMs;
        if (awaitCleanFrame_ && lastFrameIntervalMs_ >= 0.0
            && lastFrameIntervalMs_ <= policy_.lateFrameIntervalMs)
        {
            awaitCleanFrame_ = false;
        }
    }

    /// Call whenever the *user* (not follow) mutated this window's viewport (wheel zoom/pan, drag).
    void noteUserViewportChange(const double nowMs) noexcept
    {
        lastUserViewportChangeMs_ = nowMs;
        // A user viewport change redefines the view; the old page's boundary bookkeeping is stale.
        waitingForBoundaryRearm_ = false;
    }

    /// One page decision for the current frame. `playheadSamples` / `visStartSamples` / `spanSamples`
    /// describe the *current* view (span = width × samplesPerPixel).
    [[nodiscard]] Decision decidePage(const double nowMs,
                                      const double playheadSamples,
                                      const double visStartSamples,
                                      const double spanSamples) noexcept
    {
        if (spanSamples <= 0.0)
        {
            return Decision::skipNotNeeded;
        }
        const bool inside = isInsideComfortBand(playheadSamples, visStartSamples, spanSamples);
        if (inside)
        {
            // The page (or natural playback) brought the playhead into view: re-arm so the next
            // *boundary crossing from within* triggers exactly one page.
            waitingForBoundaryRearm_ = false;
            return Decision::skipNotNeeded;
        }
        if (nowMs - lastUserViewportChangeMs_ < policy_.userGestureHoldoffMs)
        {
            return Decision::skipUserGestureHoldoff;
        }
        if (lastFrameIntervalMs_ > policy_.lateFrameIntervalMs)
        {
            return Decision::skipLateFrame;
        }
        if (awaitCleanFrame_)
        {
            return Decision::skipAwaitCleanFrame;
        }
        if (waitingForBoundaryRearm_ && nowMs - lastPageMs_ < policy_.pageRearmIntervalMs)
        {
            return Decision::skipBoundaryWait;
        }
        if (nowMs - lastPageMs_ < policy_.minPageIntervalMs)
        {
            return Decision::skipMinInterval;
        }
        return Decision::apply;
    }

    /// Call after every follow-induced viewport mutation of this window (paged *and* explicit
    /// single-shot moves). `visStartSamples`/`spanSamples` describe the view *after* the move;
    /// if the playhead still is not inside the comfort band the page could not capture it (extent
    /// clamp / fine zoom) and paging switches to the sparse re-arm interval.
    void notePageApplied(const double nowMs,
                         const double playheadSamples,
                         const double visStartSamples,
                         const double spanSamples) noexcept
    {
        lastPageMs_ = nowMs;
        awaitCleanFrame_ = true;
        waitingForBoundaryRearm_
            = !isInsideComfortBand(playheadSamples, visStartSamples, spanSamples);
    }

    /// Last measured frame interval (diagnostics only; 0 until two ticks were observed).
    [[nodiscard]] double lastFrameIntervalMs() const noexcept { return lastFrameIntervalMs_; }

private:
    [[nodiscard]] bool isInsideComfortBand(const double playheadSamples,
                                           const double visStartSamples,
                                           const double spanSamples) const noexcept
    {
        const double rel = (playheadSamples - visStartSamples) / spanSamples;
        return rel > policy_.leftBoundaryFraction && rel < policy_.rightBoundaryFraction;
    }

    Policy policy_;
    double lastFrameTickMs_ = -1.0;
    double lastFrameIntervalMs_ = 0.0;
    double lastUserViewportChangeMs_ = -1.0e9;
    double lastPageMs_ = -1.0e9;
    bool awaitCleanFrame_ = false;
    /// Set when the last page left the playhead outside the comfort band; cleared when the playhead
    /// is observed inside the band or the user changes the viewport.
    bool waitingForBoundaryRearm_ = false;
};

// =============================================================================

/// Cross-window follow coordination (message thread only). One instance per process: the main
/// arrangement and the MIDI editor follow independently, but their pages land on the same message
/// thread — this object makes sure at most one window pages per `kGlobalMinPageIntervalMs`, and
/// that follow in *other* windows yields while the user is actively panning/zooming somewhere.
class GlobalFollowWorkCoordinator final
{
public:
    /// One follow page across all windows per this interval.
    static constexpr double kGlobalMinPageIntervalMs = 250.0;
    /// While the user pans/zooms one window, follow paging in *other* windows yields this long.
    static constexpr double kCrossWindowGestureHoldoffMs = 1000.0;

    [[nodiscard]] static GlobalFollowWorkCoordinator& instance() noexcept
    {
        static GlobalFollowWorkCoordinator g;
        return g;
    }

    /// Report a user viewport gesture in the window identified by `windowTag` (any stable pointer).
    void noteUserViewportGesture(const void* const windowTag, const double nowMs) noexcept
    {
        lastGestureWindowTag_ = windowTag;
        lastGestureMs_ = nowMs;
    }

    /// True while a *different* window saw a user viewport gesture within the holdoff window.
    [[nodiscard]] bool otherWindowGestureActive(const void* const windowTag,
                                                const double nowMs) const noexcept
    {
        return lastGestureWindowTag_ != nullptr && lastGestureWindowTag_ != windowTag
               && nowMs - lastGestureMs_ < kCrossWindowGestureHoldoffMs;
    }

    /// True when the global one-page-per-interval budget admits another page now.
    [[nodiscard]] bool pageSlotAvailable(const double nowMs) const noexcept
    {
        return nowMs - lastPageMs_ >= kGlobalMinPageIntervalMs;
    }

    /// Record any follow-induced viewport mutation (paged or explicit) in any window.
    void notePageApplied(const double nowMs) noexcept
    {
        lastPageMs_ = nowMs;
        ++pagesApplied_;
    }

    /// Total pages across all windows since start (diagnostics; caller computes deltas).
    [[nodiscard]] unsigned int totalPagesApplied() const noexcept { return pagesApplied_; }

private:
    GlobalFollowWorkCoordinator() = default;

    const void* lastGestureWindowTag_ = nullptr;
    double lastGestureMs_ = -1.0e9;
    double lastPageMs_ = -1.0e9;
    unsigned int pagesApplied_ = 0;
};
