#pragma once

// =============================================================================
// PlayheadOverlay — lane-column playhead line + main-window playhead frame driver
// =============================================================================
// Draws the session playhead stroke over waveform lanes without forcing each
// `ClipWaveformView` to repaint at playhead cadence.
//
// FRAME CONTRACT (message thread)
//   This component is the **only** sampler of the window's `UiPlayheadClock`: its timer computes
//   one display position per frame, stores it, and pushes the same value to the ruler through
//   `onPlayheadFrameAdvanced`. `paint` draws only the stored frame position and never reads the
//   clock — so a repaint triggered by *any* other component (lane content, window damage) redraws
//   the line in exactly the column the dirty-rect bookkeeping knows about. Re-reading time inside
//   `paint` is what previously left stale line columns behind and let the segment over one lane
//   lead the rest of the line.
//
// Invalidation: a tick that moves the line invalidates only the previous and new pixel columns;
// a tick that stays in the same column repaints nothing. Viewport/extent changes repaint fully.

#include "domain/Session.h"
#include "ui/TimelineViewportModel.h"

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <cstdint>
#include <functional>
#include <limits>

class Transport;
class UiPlayheadClock;

class PlayheadOverlay final : public juce::Component, private juce::Timer
{
public:
    /// Playhead animation rate (frame-callback cadence). Public so the follow governor can derive
    /// nominal playhead travel per UI frame (`sampleRate / kUpdateHz`).
    static constexpr int kUpdateHz = 60;

    PlayheadOverlay(Session& session,
                    Transport& transport,
                    TimelineViewportModel& timelineViewport,
                    juce::AudioDeviceManager& deviceManager,
                    UiPlayheadClock& uiPlayheadClock);

    ~PlayheadOverlay() override;

    void paint(juce::Graphics& g) override;

    /// [Message thread] Height of the timeline-ruler band at the top of this overlay's bounds
    /// (layout sets it). When > 0, `paint` draws the short ruler marker segment there and the long
    /// lane line below the band — the ruler itself paints no playhead, so playhead frames never
    /// invalidate the buffered ruler/lane content underneath.
    void setRulerBandHeightPx(int px) noexcept { rulerBandHeightPx_ = px; }

    /// [Message thread] Called once per frame with the display position (session samples) every
    /// main-window playhead renderer must use this frame. Wired in `MainAppWindow` (follow + watchdog).
    void setOnPlayheadFrameAdvanced(std::function<void(double displaySamples)> fn) noexcept;

    /// [Message thread] Immediately adopt `displaySamples` as this frame's position after an
    /// explicit UI seek (e.g. jump-to-left-locator), invalidating the old and new line columns —
    /// the visual jump must not wait for the next timer tick or the clock's smoothing.
    void snapFrameDisplaySamplesForSeek(double displaySamples) noexcept;

    /// Part E diagnostics: aggregated UI playhead timer + invalidation behaviour since the previous
    /// call. Cheap counters only; read once per second from the transport UI timer.
    struct UiRenderStats
    {
        int timerTicks = 0;
        int repaintRequests = 0;
        double repaintAreaPx = 0.0;
        double timerIntervalMinMs = 0.0;
        double timerIntervalMeanMs = 0.0;
        double timerIntervalMaxMs = 0.0;
        double lastFrameDisplaySamples = 0.0;
        double lastFrameCentreX = 0.0;
    };
    [[nodiscard]] UiRenderStats snapshotUiRenderStatsAndReset() noexcept;

private:
    void timerCallback() override;

    /// Map the stored frame display sample to a pixel-centre x in local coords (NaN when unusable).
    [[nodiscard]] float frameCentreXForCurrentViewport() const noexcept;

    /// Invalidate the previous and current line columns (or everything, when unknown) after
    /// `frameDisplaySamples_` changed. Shared by the timer tick and the explicit seek snap.
    void invalidateForFrameMove() noexcept;

    Session& session_;
    Transport& transport_;
    TimelineViewportModel& timelineViewport_;
    juce::AudioDeviceManager& deviceManager_;
    UiPlayheadClock& uiPlayheadClock_;
    std::function<void(double)> onPlayheadFrameAdvanced_;

    /// The single per-frame display position (session samples); `paint` derives everything from it.
    double frameDisplaySamples_ = 0.0;
    bool frameValid_ = false;

    /// Ruler band height at the top of the overlay (0 = overlay covers lanes only, one full line).
    int rulerBandHeightPx_ = 0;

    /// Playhead column the last tick's invalidation accounted for; NaN forces a full repaint.
    float lastInvalidatedCentreX_ = std::numeric_limits<float>::quiet_NaN();

    /// Structural snapshot: viewport/extent changes repaint the full overlay instead of stripes.
    std::int64_t lastVisibleStartUi_ = -1;
    double lastSamplesPerPixelUi_ = 0.0;
    std::int64_t lastArrangementExtentUi_ = -1;
    juce::Rectangle<int> lastBoundsUi_;

    int statsTimerTicks_ = 0;
    int statsRepaintRequests_ = 0;
    double statsRepaintAreaPx_ = 0.0;
    double statsIntervalSumMs_ = 0.0;
    double statsIntervalMinMs_ = 0.0;
    double statsIntervalMaxMs_ = 0.0;
    int statsIntervalSamples_ = 0;
    double lastTimerWallMs_ = -1.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PlayheadOverlay)
};
