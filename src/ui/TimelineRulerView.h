#pragma once

// =============================================================================
// TimelineRulerView  —  minimal time bar for seek + playhead marker (message thread only)
// =============================================================================
//
// ROLE IN THE ARCHITECTURE
    //   User points on the **session** timeline (same sample space as
    //   `Session::getArrangementExtentSamples` / `Transport` playhead)
//   and `Transport`’s playhead) without clicking the event lane. **Seek** is implemented only here:
//   `Transport::requestSeek` on press and drag. The event lane (`ClipWaveformView`) stays focused on
//   selection and clip drag; it does not seek on empty background.
//
// PRESENTATION
//   Plain tick marks at **round seconds** (device sample rate → seconds for layout only). Density
//   adapts so ticks do not merge. **No** mm:ss labels, no bar/beat. A short **playhead** stroke at
//   the top aligns with the lane’s vertical playhead line when both use the same linear x↔sample map
//   and the parent gives them **identical width** (see `Main.cpp` layout).
//
// THREADING
//   juce::Component + Timer: [Message thread] only. Reads `Session` / `Transport`; never runs on the
//   audio callback. `AudioDeviceManager` is used on the message thread to read the current device
//   sample rate for tick spacing only — not for transport state.
//
// NOT RESPONSIBLE FOR
//   Clip ordering, snap content, file I/O, or writing the authoritative playhead (only `requestSeek`).
//   No shared “timeline model” object; mapping matches `ClipWaveformView` by contract and layout.
//
// See: TimelineRulerView.cpp (tick step choice, x→sample mapping).
// =============================================================================

#include "domain/Session.h"
#include "ui/TimelineViewportModel.h"

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <cstdint>

class Transport;

// ---------------------------------------------------------------------------
// TimelineRulerView — thin strip above the event lane
// ---------------------------------------------------------------------------
// Responsibility: paint second ticks + playhead marker; map pointer x to session samples and
// request seek. Does not own transport truth; does not store a second playhead.
//
// Threading: [Message thread] for construction, paint, mouse, timer.
// ---------------------------------------------------------------------------
class TimelineRulerView : public juce::Component, private juce::Timer
{
public:
    // [Message thread] `session` / `transport` outlive this view. `deviceManager` is only sampled
    // in `paint` for the running device’s sample rate (tick placement in seconds).
    TimelineRulerView(Session& session,
                      Transport& transport,
                      juce::AudioDeviceManager& deviceManager,
                      TimelineViewportModel& timelineViewport);

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

    Session& session_;
    Transport& transport_;
    juce::AudioDeviceManager& deviceManager_;
    TimelineViewportModel& timelineViewport_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TimelineRulerView)
};
