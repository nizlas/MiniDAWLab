#pragma once

// =============================================================================
// LevelMeterComponent — vertical dBFS peak meter (1–2 bars) with hold, overload latch and DC tag
// =============================================================================
//
// ROLE
//   Displays `level_meter::Reading` windows the owner drains from the engine (Stereo Out or the
//   selected track) at its own timer rate. Pure UI: no engine access, no audio-thread code.
//
// SCALE AND NUMBERS
//   0 dBFS at the top down to −60 dBFS at the bottom (sample peak, labelled as such — not true
//   peak). Silence reads "−∞". The numeric peak text shows the held maximum and is NEVER capped to
//   0 dBFS: a float value above full scale is shown as a positive dBFS number — that is an overload
//   warning at this measuring point, not proof that clipping has happened here.
//
// FOUR SEPARATE THINGS (deliberately kept apart)
//   * instantaneous block peak    — what arrived in the last drained window (jumps up instantly);
//   * displayed bar level         — falls at `kFallDbPerSecond`, wall-clock based (repaint-rate
//                                   independent) so a short peak survives a slow UI timer;
//   * peak hold line + text       — the maximum, held `kPeakHoldSeconds` after the last rise, then
//                                   released to follow the bar;
//   * overload latch              — set by any sample above full scale (or NaN/Inf) and kept until
//                                   the user clicks the meter (reset) or the owner clears it on a
//                                   track switch / project replace. Count is shown while latched.
//   A "DC" tag appears while the drained window's mean exceeds `kDcTagThreshold` (|dc| > −40 dBFS):
//   a constant offset wastes headroom and clicks at every start/stop and file edge.
//
// THREADING
//   [Message thread] only.
// =============================================================================

#include "engine/LevelMeterAccumulator.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

class LevelMeterComponent final : public juce::Component,
                                  public juce::SettableTooltipClient
{
public:
    static constexpr double kMinDb = -60.0;
    static constexpr double kMaxDb = 0.0;
    static constexpr double kFallDbPerSecond = 24.0;
    static constexpr double kPeakHoldSeconds = 2.0;
    static constexpr double kDcTagThreshold = 0.01;

    LevelMeterComponent();

    /// Owner feeds one drained window; `nowSeconds` is the owner's wall clock (monotonic).
    void pushReading(const level_meter::Reading& reading, double nowSeconds);
    /// Owner's animation tick (wall clock); applies the time-based fall and repaints when needed.
    void tick(double nowSeconds);
    /// Track switch / project replace: forgets everything, including the overload latch.
    void clear();
    /// Clears only the overload latch (also what a click on the meter does).
    void resetOverloadLatch();

    /// 1 or 2 bars. Mono shows ONE bar — never an invented second channel.
    void setChannelCount(int channels);
    [[nodiscard]] int getChannelCount() const noexcept { return channels_; }
    /// Draws the dB scale (ticks + labels) beside the bars when true; a compact meter omits labels.
    void setShowScale(bool show);
    void setShowScaleLabels(bool show);
    /// Shows the held peak number under the bars.
    void setShowPeakText(bool show);
    [[nodiscard]] juce::String getPeakText() const;
    [[nodiscard]] bool isOverloadLatched() const noexcept { return overloadLatched_; }
    [[nodiscard]] juce::uint32 getOverloadSampleCount() const noexcept { return overloadSamples_; }
    [[nodiscard]] bool hasDcTag() const noexcept { return dcTag_; }
    [[nodiscard]] float getHeldPeakLinear() const noexcept { return juce::jmax(heldPeak_[0], heldPeak_[1]); }
    /// Bar level currently displayed (dB, after the time-based fall) — for tests.
    [[nodiscard]] double getDisplayedDb(int channel) const noexcept;

    /// Horizontal space the meter wants for `channels` bars plus the scale (logical px).
    [[nodiscard]] static int preferredWidthFor(int channels, bool withScaleLabels) noexcept;
    /// Bars' bounds in local coordinates (for tests / owner alignment).
    [[nodiscard]] juce::Rectangle<int> getBarsBounds() const noexcept;

    std::function<void()> onOverloadLatchReset;

    void paint(juce::Graphics& g) override;
    void mouseDown(const juce::MouseEvent& e) override;

    /// dB → y inside `barArea` (top = 0 dBFS, bottom = −60 dBFS); clamps.
    [[nodiscard]] static float yForDb(double db, juce::Rectangle<int> barArea) noexcept;

private:
    [[nodiscard]] juce::Rectangle<int> scaleBounds() const noexcept;
    [[nodiscard]] juce::Rectangle<int> overloadLampBounds() const noexcept;
    [[nodiscard]] juce::Rectangle<int> peakTextBounds() const noexcept;

    int channels_ = 2;
    bool showScale_ = true;
    bool showScaleLabels_ = true;
    bool showPeakText_ = true;

    double displayedDb_[2] = { kMinDb, kMinDb };
    float heldPeak_[2] = { 0.0f, 0.0f };
    double heldPeakRiseTime_[2] = { 0.0, 0.0 };
    double lastTickSeconds_ = 0.0;
    bool overloadLatched_ = false;
    juce::uint32 overloadSamples_ = 0;
    bool nonFiniteSeen_ = false;
    bool dcTag_ = false;
    double lastDc_ = 0.0;
    double lastReadingSeconds_ = 0.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LevelMeterComponent)
};
