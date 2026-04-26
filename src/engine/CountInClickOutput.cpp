#include "engine/CountInClickOutput.h"

#include <cmath>
#include <algorithm>

namespace
{
    constexpr int kTockHz = 800;
    constexpr int kTickHz = 2000;
    // ~40 ms burst at device rate; keeps buffers modest at high SR
    constexpr double kClickDurationSec = 0.04;
} // namespace

void CountInClickOutput::prepare(double sampleRate)
{
    if (sampleRate <= 0.0)
    {
        return;
    }
    if (sampleRate == preparedRate_ && !tock_.empty())
    {
        return;
    }
    const int n = (int)std::llround(kClickDurationSec * sampleRate);
    if (n <= 0)
    {
        return;
    }
    tock_.assign((size_t)n, 0.0f);
    tick_.assign((size_t)n, 0.0f);
    const float invN = 1.0f / (float)std::max(1, n - 1);
    for (int i = 0; i < n; ++i)
    {
        const float t = (float)i / (float)sampleRate;
        const float env = (1.0f - (float)i * invN);
        tock_[(size_t)i] = 0.45f * env
                           * std::sin(2.0f * 3.14159265f * (float)kTockHz * t);
        tick_[(size_t)i] = 0.5f * env
                           * std::sin(2.0f * 3.14159265f * (float)kTickHz * t);
    }
    preparedRate_ = sampleRate;
    readIndex_.store(kIdle, std::memory_order_release);
}

void CountInClickOutput::triggerTock() noexcept
{
    if (tock_.empty())
    {
        return;
    }
    which_.store(0, std::memory_order_relaxed);
    length_.store((int)tock_.size(), std::memory_order_relaxed);
    readIndex_.store(0, std::memory_order_release);
}

void CountInClickOutput::triggerTick() noexcept
{
    if (tick_.empty())
    {
        return;
    }
    which_.store(1, std::memory_order_relaxed);
    length_.store((int)tick_.size(), std::memory_order_relaxed);
    readIndex_.store(0, std::memory_order_release);
}

void CountInClickOutput::cancel() noexcept
{
    readIndex_.store(kIdle, std::memory_order_release);
}

void CountInClickOutput::audioThread_mixInto(float* const* outputChannelData,
                                            int numOutputChannels,
                                            int numSamples) noexcept
{
    if (outputChannelData == nullptr || numSamples <= 0)
    {
        return;
    }
    int r = readIndex_.load(std::memory_order_acquire);
    if (r < 0)
    {
        return;
    }
    const int w = which_.load(std::memory_order_relaxed);
    const int len = length_.load(std::memory_order_relaxed);
    const float* const buf = (w == 0) ? tock_.data() : tick_.data();
    if (buf == nullptr || len <= 0)
    {
        readIndex_.store(kIdle, std::memory_order_release);
        return;
    }

    for (int i = 0; i < numSamples && r < len; ++i, ++r)
    {
        const float s = buf[(size_t)r];
        for (int c = 0; c < numOutputChannels; ++c)
        {
            float* d = outputChannelData[c];
            if (d != nullptr)
            {
                d[i] += s;
            }
        }
    }
    if (r >= len)
    {
        readIndex_.store(kIdle, std::memory_order_release);
    }
    else
    {
        readIndex_.store(r, std::memory_order_release);
    }
}
