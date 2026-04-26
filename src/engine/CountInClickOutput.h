#pragma once

#include <atomic>
#include <cstdint>
#include <vector>

// Count-in click generator: message thread calls prepare/trigger; audio thread calls mix only.
// No Session/Transport; fixed buffers after prepare; realtime path does not allocate.
class CountInClickOutput
{
public:
    CountInClickOutput() = default;
    void prepare(double sampleRate);
    // Message thread: arm next click (tock = beats 1–3, tick = beat 4)
    void triggerTock() noexcept;
    void triggerTick() noexcept;
    // Stop the current click immediately (e.g. count-in cancelled)
    void cancel() noexcept;

    // [Audio / realtime] Add click samples into (already cleared) output buffers, stereo or mono
    void audioThread_mixInto(float* const* outputChannelData,
                            int numOutputChannels,
                            int numSamples) noexcept;

private:
    std::vector<float> tock_;
    std::vector<float> tick_;
    double preparedRate_ = 0.0;

    // -1 = idle; else index into tock_ or tick_
    static constexpr int kIdle = -1;
    std::atomic<int> readIndex_{kIdle};
    std::atomic<int> which_{0};  // 0 = tock, 1 = tick
    std::atomic<int> length_{0};
};
