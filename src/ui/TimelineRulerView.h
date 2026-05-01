#pragma once

// =============================================================================
// TimelineRulerView  —  minimal time bar for seek + playhead marker (message thread only)
// =============================================================================
//
// ROLE IN THE ARCHITECTURE
//   User points on the **session** timeline (same sample space as
//   `Session::getArrangementExtentSamples` / `Transport`) without clicking the event lane.
//   **Seek** uses `Transport::requestSeek` on press and drag on the **lower** half; **Ctrl** sets
//   the **left** locator, **Alt** sets the **right**. The **upper** half (no modifier) toggles
//   **cycle** visualization (transient on `Transport`) when not blocked during record/count-in.
//   The event lane (`ClipWaveformView`) stays focused on
//
// PRESENTATION
//   Tick marks + lightweight time labels (seconds / fractional seconds / `m:ss` / `m:ss.mmm`) from
//   timeline samples × an effective **display** sample rate (device SR or 48000 Hz fallback only —
//   never persisted). Density adapts to zoom; labels never follow playback audible-offset; no
//   bar/beat/tempo grid. A short **playhead** stroke at the top aligns with the lane vertical line
//   when widths match (see `Main.cpp`).
//
// THREADING
//   juce::Component + Timer: [Message thread] only. Reads `Session` / `Transport`; never runs on the
//   audio callback. `AudioDeviceManager` is used on the message thread to read an **effective** sample
//   rate for ruler tick/label placement only — not for transport state and not persisted.
//
// NOT RESPONSIBLE FOR
//   Clip ordering, snap content, file I/O, or writing the authoritative playhead (only `requestSeek`).
//   No shared “timeline model” object; mapping matches `ClipWaveformView` by contract and layout.
//
// See: `TimelineRulerView.cpp` (tick/label step ladder, timecode labels, x→sample mapping).
// =============================================================================

#include "domain/Session.h"
#include "ui/TimelineViewportModel.h"

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <cstdint>
#include <functional>

class Transport;

// ---------------------------------------------------------------------------
// TimelineRulerView — thin strip above the event lane
// ---------------------------------------------------------------------------
// Responsibility: paint ticks, time labels, playhead marker; map pointer x to session samples and
// request seek. Does not own transport truth; does not store a second playhead.
//
// Threading: [Message thread] for construction, paint, mouse, timer.
// ---------------------------------------------------------------------------
class TimelineRulerView : public juce::Component, private juce::Timer
{
public:
    // [Message thread] Optional: when set, upper-half cycle toggle is suppressed (e.g. recording
    // or count-in). Must be safe to call synchronously from ruler mouse handlers.
    TimelineRulerView(Session& session,
                      Transport& transport,
                      juce::AudioDeviceManager& deviceManager,
                      TimelineViewportModel& timelineViewport,
                      std::function<bool()> isUiInputBlockedByRecording = {});

    ~TimelineRulerView() override;

    void paint(juce::Graphics& g) override;
    void resized() override;

    // [Message thread] Map local x to session timeline sample and `requestSeek` (press + drag scrub).
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;
    void mouseUp(const juce::MouseEvent& e) override;
    void mouseWheelMove(
        const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override;

    // [Message thread] Shared with `TrackLanesView` (timeline column) and `ClipWaveformView`: one
    // **samples-per-pixel** contract for the visible window.
    [[nodiscard]] static std::int64_t xToSessionSampleClamped(
        float positionX,
        float widthPx,
        std::int64_t visibleStart,
        double samplesPerPixel) noexcept;
    /// `originX` + (s - visStart) / samplesPerPixel
    [[nodiscard]] static float sessionSampleToLocalX(
        std::int64_t s,
        float originX,
        std::int64_t visibleStart,
        double samplesPerPixel) noexcept;
    /// Map when a **span** of `spanSamples` (not the normal visible length) covers full width, e.g.
    /// right-edge trim preview extended past the visible end.
    [[nodiscard]] static float sessionSampleToLocalXForSpan(
        std::int64_t s,
        const juce::Rectangle<float>& b,
        std::int64_t visibleStart,
        std::int64_t spanSamples) noexcept;

private:
    // [Message thread] Low-rate `repaint` so the ruler playhead matches the lane’s animated line
    // without a cached playhead value on the view.
    void timerCallback() override;

    // [Message thread] Shared by mouse down/drag: map local x to sample and `requestSeek`.
    void applySeekForLocalX(float x) noexcept;
    void applyLeftLocatorForLocalX(float x) noexcept;
    void applyRightLocatorForLocalX(float x) noexcept;
    void tryToggleCycleEnabled() noexcept;

    Session& session_;
    Transport& transport_;
    juce::AudioDeviceManager& deviceManager_;
    TimelineViewportModel& timelineViewport_;
    std::function<bool()> isUiInputBlockedByRecording_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TimelineRulerView)
};
