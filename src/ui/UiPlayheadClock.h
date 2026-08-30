#pragma once

// =============================================================================
// UiPlayheadClock — one smoothed current-time position per window (message thread)
// =============================================================================
//
// ROLE
//   The audio callback publishes `Transport::readPlayheadSamplesForUi()` once per device block, so
//   that value is a **staircase**: it stands still for a whole block and then jumps. Painting it
//   directly makes indicators step, and correcting an extrapolation *onto* every new step makes
//   them jitter.
//
//   This clock integrates a smooth message-thread position: it advances with wall-clock time and
//   applies a small proportional correction toward the published transport value each tick, so
//   block quantization is filtered out instead of chased. Large errors (seek, cycle wrap, device
//   dropout) snap immediately.
//
// DRIVER CONTRACT (single sampler per frame)
//   Exactly **one** component per window calls `readDisplaySamples` per UI tick (the arrangement
//   window uses `PlayheadOverlay`); every other renderer receives the resulting value pushed to it
//   and never reads time on its own. Two independent samplers would disagree by their timer phase,
//   which is exactly the split-line artifact this exists to prevent.
//
// WHAT IT IS NOT
//   Not a second transport and not used for anything audible: musical timing, scheduling, and the
//   authoritative playhead stay in `Transport` / `PlaybackEngine`. This is presentation only.
//
// THREADING
//   [Message thread] only (JUCE timers and paint). Never called from the audio callback.
// =============================================================================

#include <cstdint>

class UiPlayheadClock
{
public:
    /// [Message thread] Advance and return the smoothed display position in session samples.
    /// `rawTransportSample` is the value the audio thread last published. Call once per UI frame
    /// from the window's single driving component.
    [[nodiscard]] double readDisplaySamples(std::int64_t rawTransportSample,
                                            bool playing,
                                            double sampleRate) noexcept;

    /// [Message thread] Snap the display position after an explicit UI seek, so indicators land on
    /// the requested sample immediately instead of waiting for the audio thread to commit it. The
    /// published transport value is ignored until it catches up with the requested sample.
    void reanchorTo(std::int64_t sample) noexcept;

private:
    double lastValue_ = 0.0;
    double lastAdvanceWallSec_ = -1.0;
    std::int64_t lastRawSample_ = 0;
    bool wasPlaying_ = false;
    bool initialised_ = false;

    /// Set by `reanchorTo`: display follows this requested position until the audio thread commits it.
    bool seekHoldActive_ = false;
    std::int64_t seekHoldSample_ = 0;
};
