#pragma once

// =============================================================================
// InsertSlotRuntime — host-owned audible mix for one insert slot
// =============================================================================
//
// ROLE
//   The host's Active and Bypass choices for a single insert. This is not the plug-in's own
//   VST3 bypass parameter and it is never written into that parameter.
//
//   Wet:        processBlock, the buffer becomes the plug-in output.
//   HostBypass: processBlock still runs (the effect stays current), the buffer is replaced by
//               the signal that entered this slot. When the plug-in reports latency, that dry
//               copy is delayed by min(latency, kMaxDelaySamples - 1) so the comparison lines
//               up inside this insert only.
//   Inactive:   after the ramp, processBlock is not called and the buffer is left untouched
//               (no plug-in latency, no tail). Active off wins over a stored Bypass choice.
//
//   A change crossfades linearly over 10 ms, counted in samples at the prepared rate, so the
//   ramp does not depend on the UI frame rate. Turning Active back on reset()s the instance
//   on the message thread while the callback and the read-ahead worker are outside it, so a
//   frozen tail is not replayed. This does not promise that an effect keeps ringing after it
//   is switched off.
//
//   DAL does not delay-compensate the rest of the project. Switching Active off removes this
//   insert's latency from the chain. The 10 ms ramp between a latent wet signal and the
//   undelayed dry signal is not sample-aligned when latency is above zero.
//
// THREADING
//   prepare / setTarget / setLatencySamples / resetDelay: [Message thread]. prepare allocates.
//   process: [The one processing thread that owns this chain for the block]. No allocation,
//   no lock. It only reads the atomics and writes the ramp, delay cursor and preallocated
//   buffers.
// =============================================================================

#include <juce_audio_processors/juce_audio_processors.h>

#include <atomic>
#include <vector>

class InsertSlotRuntime
{
public:
    static constexpr int kRampMs = 10;
    /// Delay line used only to line up host-bypass dry audio with this insert's own latency.
    static constexpr int kMaxDelaySamples = 32768;

    enum class Target : int
    {
        Wet = 0,
        HostBypass = 1,
        Inactive = 2
    };

    /// [Message thread] Grow the dry/wet copies and the delay line. Does not clear a line that
    /// is already large enough, so a later prepare does not wipe a warm delay.
    void prepare(double sampleRate, int maxBlock) noexcept;

    /// [Message thread] The mix the processing thread should reach. The ramp starts on the
    /// processing thread, from the gain it has already reached.
    void setTarget(Target target) noexcept;

    [[nodiscard]] Target target() const noexcept;

    /// [Message thread] Plug-in latency used for the host-bypass dry delay. Clamped to the line.
    void setLatencySamples(int latencySamples) noexcept;

    /// [Message thread] Drop the delay memory. Call only while no processing thread is inside
    /// `process` (the reactivation drain).
    void resetDelay() noexcept;

    /// [Processing thread] Apply the mix to `view` in place. `midi` is forwarded to processBlock
    /// and left for the caller to clear, matching the previous chain loop.
    void process(juce::AudioProcessor& processor, juce::AudioBuffer<float>& view, juce::MidiBuffer& midi) noexcept;

private:
    void clockDryBlock(const float* const* dry, int numSamples, int numChannels) noexcept;

    std::atomic<int> target_ { static_cast<int>(Target::Wet) };
    std::atomic<float> sampleRate_ { 48000.0f };
    std::atomic<int> latencySamples_ { 0 };

    int rampDest_ = static_cast<int>(Target::Wet);
    int rampLeft_ = 0;
    int rampTotal_ = 1;
    float rampFrom_ = 1.0f;
    float rampTo_ = 1.0f;
    float wetGain_ = 1.0f;
    int delayWrite_ = 0;

    juce::AudioBuffer<float> dry_;
    juce::AudioBuffer<float> wet_;
    std::vector<float> delay_[2];
};
