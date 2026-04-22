#include "Transport.h"

Transport::Transport()
    : intent_(static_cast<std::uint32_t>(PlaybackIntent::Stopped))
    , playheadSamples_(0)
    , seekPending_(false)
    , seekTargetSamples_(0)
{
}

Transport::~Transport() = default;

void Transport::requestPlaybackIntent(PlaybackIntent intent) noexcept
{
    intent_.store(static_cast<std::uint32_t>(intent), std::memory_order_release);
}

void Transport::requestSeek(std::int64_t sampleIndex) noexcept
{
    seekTargetSamples_.store(sampleIndex, std::memory_order_relaxed);
    seekPending_.store(true, std::memory_order_release);
}

std::int64_t Transport::readPlayheadSamplesForUi() const noexcept
{
    return playheadSamples_.load(std::memory_order_acquire);
}

void Transport::audioThread_beginBlock() noexcept
{
    if (seekPending_.exchange(false, std::memory_order_acq_rel))
    {
        playheadSamples_.store(
            seekTargetSamples_.load(std::memory_order_relaxed),
            std::memory_order_release);
    }
}

std::int64_t Transport::audioThread_loadPlayhead() const noexcept
{
    return playheadSamples_.load(std::memory_order_relaxed);
}

PlaybackIntent Transport::audioThread_loadIntent() const noexcept
{
    return static_cast<PlaybackIntent>(intent_.load(std::memory_order_acquire));
}

void Transport::audioThread_advancePlayheadIfPlaying(std::int64_t deltaSamples) noexcept
{
    if (deltaSamples <= 0)
        return;

    if (audioThread_loadIntent() != PlaybackIntent::Playing)
        return;

    playheadSamples_.fetch_add(deltaSamples, std::memory_order_relaxed);
}
