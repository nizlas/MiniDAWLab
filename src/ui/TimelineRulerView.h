#pragma once

// =============================================================================
// TimelineRulerView  —  minimal time bar for seek + playhead marker (message thread only)
// =============================================================================
//
// ROLE IN THE ARCHITECTURE
//   User points on the **session** timeline (same sample space as `Session::getTimelineLengthSamples`
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
    TimelineRulerView(Session& session, Transport& transport, juce::AudioDeviceManager& deviceManager);

    ~TimelineRulerView() override;

    void paint(juce::Graphics& g) override;

    // [Message thread] Map local x to session timeline sample and `requestSeek` (press + drag scrub).
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;
    void mouseUp(const juce::MouseEvent& e) override;

private:
    // [Message thread] Low-rate `repaint` so the ruler playhead matches the lane’s animated line
    // without a cached playhead value on the view.
    void timerCallback() override;

    // [Message thread] Same x→session-sample mapping as `ClipWaveformView` for the playhead: linear
    // over the component width, clamped to [0, `timelineLength`]. `positionX` is in **local** coords
    // (0 .. width) so it matches JUCE’s `e.position` when the event is for this component.
    [[nodiscard]] static std::int64_t xToSessionSampleClamped(
        const float positionX, const float widthPx, const std::int64_t timelineLength) noexcept;

    // [Message thread] Shared by mouse down/drag: map local x to sample and `requestSeek`.
    void applySeekForLocalX(float x) noexcept;

    Session& session_;
    Transport& transport_;
    juce::AudioDeviceManager& deviceManager_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TimelineRulerView)
};
