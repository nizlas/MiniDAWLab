#pragma once

// =============================================================================
// UiPaintLoadCounters — per-component paint-rate counters (message thread only)
// =============================================================================
// Zoom-freeze forensic audit instrumentation (see MAIN_WINDOW_ZOOM_UI_FREEZE_FORENSIC_AUDIT.md):
// proves *which* main-arrangement paint path saturates during playback + zoom by counting paint
// invocations (and waveform raster rebuild cost) per second. Aggregated into the existing 1 Hz
// `playback-ui-load.log` line in `MainAppWindow`. Compiled out entirely unless
// `MINIDAW_DIAG_PLAYBACK_UI_LOAD` is 1 (default 0); the increment macros then expand to nothing.

#include "diagnostics/DiagnosticBuildFlags.h"

#if MINIDAW_DIAG_PLAYBACK_UI_LOAD

#include <cstdint>

namespace ui_paint_load
{
    /// Reset each time the 1 Hz diagnostic line is written. Plain integers: paints and the log
    /// tick both run on the message thread.
    struct Counters
    {
        std::uint32_t clipLanePaints = 0;        ///< ClipWaveformView::paint invocations.
        std::uint32_t clipLaneRasterRebuilds = 0; ///< Full wave-raster rebuilds (sync + deferred).
        std::uint64_t clipLaneRasterRebuildUs = 0; ///< Accumulated rebuild time (µs).
        std::uint32_t clipLaneUncachedPaints = 0; ///< paintUncachedFull fallbacks.
        std::uint32_t clipLaneStaleBlits = 0;    ///< Geometry-stale scaled blits (rebuild deferred).
        std::uint32_t clipLaneDeferredBuilds = 0; ///< Idle-timer raster rebuild executions.
        std::uint32_t midiLanePaints = 0;        ///< MidiEventLane::paint invocations.
        std::uint64_t midiLaneNoteIterations = 0; ///< Note-loop iterations (both preview passes).
        std::uint32_t lanesViewPaints = 0;       ///< TrackLanesView::paint (grid + separators).
        std::uint32_t rulerPaints = 0;           ///< TimelineRulerView::paint (ticks + labels).
        std::uint32_t overlayPaints = 0;         ///< PlayheadOverlay::paint.
    };

    [[nodiscard]] inline Counters& mutableCounters() noexcept
    {
        static Counters c;
        return c;
    }

    [[nodiscard]] inline Counters snapshotAndReset() noexcept
    {
        Counters out = mutableCounters();
        mutableCounters() = Counters{};
        return out;
    }
} // namespace ui_paint_load

#define MINIDAW_UI_PAINT_COUNT(field) (++ui_paint_load::mutableCounters().field)
#define MINIDAW_UI_PAINT_ADD(field, n) \
    (ui_paint_load::mutableCounters().field += static_cast<std::uint64_t>(n))

#else

#define MINIDAW_UI_PAINT_COUNT(field) ((void)0)
#define MINIDAW_UI_PAINT_ADD(field, n) ((void)0)

#endif
