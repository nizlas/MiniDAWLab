#pragma once

// =============================================================================
// Transport.h / Transport.cpp  —  single source of truth for playback control (see ARCHITECTURE)
// =============================================================================
//
// ROLE IN THE APP
//   Answers three questions for the rest of the program: (1) Should we be playing, paused, or
//   stopped? (2) Where is the playhead? It is a *session-timeline-absolute* sample index (from 0
//   to the derived end of placed material); PlaybackEngine maps it into each clip’s buffer. Step 4
//   data shape; Step 5 reinterprets the same `playheadSamples_` value accordingly (one clip at
//   start 0 stays numerically the same 0..length case).
//   (3) Did the user request a seek that the audio thread has not applied yet?
//
// ARCHITECTURAL PLACE
//   UI and non-realtime code call the public request* and read* methods. Only PlaybackEngine,
//   on the audio device callback thread, may advance the playhead or consume seeks (via the
//   private audioThread_* API). That split keeps one writer for the authoritative playhead and
//   avoids mutexes on the realtime path — state is std::atomic-based.
//
// THREAD MODEL (plain language)
//   • Message / UI thread: user clicks Play, Pause, Stop, seek on waveform → requestPlaybackIntent,
//     requestSeek. UI reads the playhead for drawing with readPlayheadSamplesForUi (read-only).
//   • Audio thread: at the start of each block, apply pending seek; read intent and playhead;
//     after rendering, advance playhead by samples actually played (PlaybackEngine does that).
//   • Never: UI writing the playhead directly. Never: audio thread decoding files.
//
// KEY COLLABORATORS
//   • PlaybackEngine (friend): only caller of audioThread_*.
//   • Session: separate; Transport does not know about clips.
//
// FILE PAIRING
//   Declarations here; definitions in Transport.cpp, including why each atomic uses its
//   memory_order (see Transport.cpp file header).

#include <atomic>
#include <cstdint>

// User-facing transport mode. Stored as uint32_t inside Transport for std::atomic compatibility.
enum class PlaybackIntent : std::uint32_t
{
    Stopped = 0,
    Playing = 1,
    Paused = 2,
};

// ---------------------------------------------------------------------------
// Transport
// ---------------------------------------------------------------------------
// Responsibility: own the atomic fields that represent playback *intent* (Stopped / Playing /
// Paused), the authoritative *playhead* (session-timeline-absolute sample index; see file header),
// and a *pending seek* (target index + flag). The UI requests changes; the audio callback is the
// only writer of the playhead and the consumer of seek requests, via the private `audioThread_*`
// API (see PlaybackEngine).
//
// Lifetime / ownership: a single instance owned at app composition level; outlives the audio
// callback registration. No heap ownership of clips or files.
//
// Threading: public methods and `readPlayheadSamplesForUi` are for any non-callback thread
// (typically the message / UI thread). `audioThread_*` is only for the audio device callback
// (PlaybackEngine is a `friend` so no other type can call them).
//
// Not responsible for: clip content, file decoding, or waveform (Session and UI own those).
//
// In-body comments in Transport.cpp state product meaning at control points (seek apply,
// playhead advance rules) where the atomic mechanics alone would not carry that meaning.
// ---------------------------------------------------------------------------
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

    // Contract: set user-facing transport mode. Store is release-ordered so the next audio
    // block can observe the new intent. Thread: not the audio callback.
    void requestPlaybackIntent(PlaybackIntent intent) noexcept;

    // [Message / UI] Last intent the UI published (or initial Stopped), acquire-ordered with
    // `requestPlaybackIntent` / `readPlayheadSamplesForUi` — for labels and shortcuts, not the
    // audio callback; use `audioThread_loadIntent` only on the device thread.
    [[nodiscard]] PlaybackIntent readPlaybackIntentForUi() const noexcept;

    // Contract: record seek target and set seek-pending. The playhead is updated only when
    // the audio thread runs `audioThread_beginBlock` (next block). Thread: not the callback.
    void requestSeek(std::int64_t sampleIndex) noexcept;

    // Contract: read the playhead the callback last published (timeline-absolute; acquire-ordered
    // w.r.t. playhead stores from the audio thread). Thread: any non-callback; safe for UI.
    [[nodiscard]] std::int64_t readPlayheadSamplesForUi() const noexcept;

    // Cycle/loop armed state (Cubase-like). Transient — not persisted. UI sets via release;
    // PlaybackEngine reads on the audio thread with acquire — see audioThread_loadCycleEnabled.
    void requestCycleEnabled(bool enabled) noexcept;
    [[nodiscard]] bool readCycleEnabledForUi() const noexcept;

    // [Message / UI] Wrapped loop counter incremented by PlaybackEngine once per audible wrap when
    // cycle playback is armed. Acquire-ordered vs audio-thread release increments.
    [[nodiscard]] std::uint32_t readCycleWrapCountForUi() const noexcept;

private:
    friend class PlaybackEngine;

    // [Audio thread] Acquire load — pairs with UI release stores on requestCycleEnabled.
    [[nodiscard]] bool audioThread_loadCycleEnabled() const noexcept;

    // [Audio thread] Direct playhead overwrite for cycle-loop resolution (exclusive with
    // audioThread_advancePlayheadIfPlaying in any block where the engine sets a definitive end
    // position via this method). Release store so UI readPlayheadSamplesForUi is coherent.
    void audioThread_storePlayheadOnWrap(std::int64_t timelineSample) noexcept;

    // [Audio thread] One increment per audible loop wrap while cycle playback wraps (same places as
    // wrap playhead stores — message thread observes via readCycleWrapCountForUi).
    void audioThread_signalCycleWrap() noexcept;

    // [Audio thread] Relaxed counter read — for rare wrap diagnostics only (PlaybackEngine Debug).
    [[nodiscard]] std::uint32_t audioThread_relaxedLoadWrapPassCount() const noexcept;

    // [Audio thread] If the UI set seek-pending, clear it and set the playhead to the target.
    // Call once at the start of each output block, before reading playhead for rendering.
    void audioThread_beginBlock() noexcept;

    // [Audio thread] Timeline-absolute read cursor (see file header). Call after
    // `audioThread_beginBlock` so a pending seek is visible. Relaxed: synchronized by beginBlock.
    [[nodiscard]] std::int64_t audioThread_loadPlayhead() const noexcept;

    // [Audio thread] Current playback intent. Acquire load: pairs with release stores on the
    // UI path so we see a coherent intent for this block.
    [[nodiscard]] PlaybackIntent audioThread_loadIntent() const noexcept;

    // [Audio thread] If intent is Playing, add `deltaSamples` to the playhead: timeline samples
    // *consumed* as clip audio this block (0 when no material played). No-op for non-Playing or
    // non-positive delta.
    void audioThread_advancePlayheadIfPlaying(std::int64_t deltaSamples) noexcept;

    std::atomic<std::uint32_t> intent_;
    std::atomic<std::int64_t> playheadSamples_;
    std::atomic<bool> seekPending_;
    std::atomic<std::int64_t> seekTargetSamples_;
    std::atomic<bool> cycleEnabled_;
    std::atomic<std::uint32_t> wrapPassCount_;
};
