// =============================================================================
// Transport.cpp  —  atomic load/store wiring for Transport (no JUCE)
// =============================================================================
//
// WHAT THIS FILE ADDS ON TOP OF Transport.h
//   The same rules as the header, plus *why* we use these std::memory_order values in one
//   place so you do not have to infer them from the implementation:
//
//   • requestPlaybackIntent uses memory_order_release on the intent. That “publishes” the new
//     intent so that a later acquire load on the audio thread sees the update *after* any
//     prior non-atomic work the UI did (in practice: user pressed Play; we want Playing seen).
//
//   • readPlayheadSamplesForUi uses memory_order_acquire so the playhead read is ordered
//     *after* release stores that updated the playhead from the audio thread.
//
//   • audioThread_loadIntent uses acquire on intent for the same handoff from UI → audio.
//
//   • audioThread_beginBlock uses acq_rel on the seek-pending flag because we both read and
//     clear it in one atomic step; the playhead store that follows is release so the new
//     position is visible to the UI’s acquire read.
//
//   • audioThread_loadPlayhead / advance use relaxed where we are only racing with the same
//     callback chain in the same order (no cross-thread “happens-after” from another CPU
//     writer in those spots beyond what beginBlock already synchronized).
//
// [Audio thread] on each function matches Transport.h. Everything else is [Message thread] or
// any app thread that is not the audio callback.
// =============================================================================

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
    // Release: publish intent so a later audio-thread load sees this write after any UI setup.
    intent_.store(static_cast<std::uint32_t>(intent), std::memory_order_release);
}

void Transport::requestSeek(std::int64_t sampleIndex) noexcept
{
    // System meaning: a new position on the *session timeline* (not an offset inside one buffer by
    // itself). Non-realtime code should clamp to [0, derived timeline length] using Session; we
    // do not re-derive that here. The audio block applies the target so PCM and this field align.
    seekTargetSamples_.store(sampleIndex, std::memory_order_relaxed);
    // Release: pair with the pending flag so the engine never sees "pending" without a target.
    seekPending_.store(true, std::memory_order_release);
}

std::int64_t Transport::readPlayheadSamplesForUi() const noexcept
{
    // Acquire: observe playhead stores from the callback as a coherent position for painting.
    return playheadSamples_.load(std::memory_order_acquire);
}

// [Audio thread]
void Transport::audioThread_beginBlock() noexcept
{
    if (seekPending_.exchange(false, std::memory_order_acq_rel))
    {
        // The user (or UI) set a new timeline position; we commit it here on the *audio* side so
        // the next `loadPlayhead` in this same callback matches the audio we are about to render.
        // Release so readPlayheadSamplesForUi on the message thread can show the new position
        // coherently after this store.
        playheadSamples_.store(
            seekTargetSamples_.load(std::memory_order_relaxed),
            std::memory_order_release);
    }
}

// [Audio thread]
std::int64_t Transport::audioThread_loadPlayhead() const noexcept
{
    return playheadSamples_.load(std::memory_order_relaxed);
}

// [Audio thread]
PlaybackIntent Transport::audioThread_loadIntent() const noexcept
{
    return static_cast<PlaybackIntent>(intent_.load(std::memory_order_acquire));
}

// [Audio thread]
void Transport::audioThread_advancePlayheadIfPlaying(std::int64_t deltaSamples) noexcept
{
    if (deltaSamples <= 0)
    {
        // Product: the engine did not advance the read cursor this block (Paused/Stopped, no
        // clip, or already at end — caller passes 0 in those cases), so the stored playhead must
        // not change either.
        return;
    }

    if (audioThread_loadIntent() != PlaybackIntent::Playing)
    {
        // Product: when Paused or Stopped, the playhead is a *position to hold* for the user,
        // not something that should creep forward every audio block. Only Playing advances.
        return;
    }

    // Relaxed: at this point we are the sole writer; beginBlock/seek and UI read paths are ordered.
    playheadSamples_.fetch_add(deltaSamples, std::memory_order_relaxed);
}
