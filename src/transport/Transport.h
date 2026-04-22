#pragma once

#include <atomic>
#include <cstdint>

enum class PlaybackIntent : std::uint32_t
{
    Stopped = 0,
    Playing = 1,
    Paused = 2,
};

// Phase 1 transport source of truth (see docs/ARCHITECTURE_PRINCIPLES.md).
class PlaybackEngine;
class Transport
{
public:
    Transport();
    ~Transport();

    Transport(const Transport&) = delete;
    Transport& operator=(const Transport&) = delete;
    Transport(Transport&&) = delete;
    Transport& operator=(Transport&&) = delete;

    // Message thread / non-realtime: playback intent (play / pause / stop).
    void requestPlaybackIntent(PlaybackIntent intent) noexcept;

    // Message thread / non-realtime: queue seek; audio callback applies it.
    void requestSeek(std::int64_t sampleIndex) noexcept;

    // Message thread / UI: lock-free read of authoritative playhead (sample index). Not a second
    // source of truth; PlaybackEngine remains the only writer of the playhead value.
    [[nodiscard]] std::int64_t readPlayheadSamplesForUi() const noexcept;

private:
    friend class PlaybackEngine;

    // Audio callback only — call in this order: beginBlock, read playhead/intent, render,
    // then advancePlayheadIfPlaying with the number of timeline samples actually consumed.
    void audioThread_beginBlock() noexcept;
    [[nodiscard]] std::int64_t audioThread_loadPlayhead() const noexcept;
    [[nodiscard]] PlaybackIntent audioThread_loadIntent() const noexcept;
    void audioThread_advancePlayheadIfPlaying(std::int64_t deltaSamples) noexcept;

    std::atomic<std::uint32_t> intent_;
    std::atomic<std::int64_t> playheadSamples_;
    std::atomic<bool> seekPending_;
    std::atomic<std::int64_t> seekTargetSamples_;
};
