#pragma once

// =============================================================================
// Track  —  one timeline lane in the session (Phase 3 minimal multi-track)
// =============================================================================
//
// ROLE
//   A **track** is a stable identity (`TrackId`) and an **ordered** list of `PlacedClip` rows on
//   the **same** session timeline (device samples) as all other tracks. **Overlap** (which clip
//   wins at an instant) is **only** defined among clips *on this track* — the same "index 0 =
//   front / newest" rule as late Phase 2, but the list does not span the whole project.
//
// CROSS-TRACK AUDIO
//   The engine may **add** the audible output of several tracks. That is **not** a mixer UI and
//   not a routing graph; it is a minimal sum for multi-track hearing (see `PlaybackEngine` and
//   `PHASE_PLAN` Phase 3).
//
// CHANNEL FADER (mixer signal chain — this field only in this slice)
//   `channelFaderGain_` is linear gain at the **channel-fader point** in the eventual per-track
//   mixer chain — not clip gain, not input-trim/pre-gain, not pre-insert gain. Conceptual ordering
//   later: clips → optional input trim/pre-gain → optional pre-fader inserts / send taps →
//   **channel fader** → optional post-fader inserts / send taps → optional group/master routing.
//   Playback applies this gain when summing the track after each lane's clip/overlap logic; PCM
//   files and waveform data are unaffected. Recording path is unaffected; WAV is captured pre-fader.
//   **Stereo pan** (`Track::stereoPan_`) is applied after this gain (and mute/off) when mixing into
//   the device stereo bus — see `TrackStereoPan.h`.
//   Future **post-fader** inserts/sends/explicit taps may require per-track staging buffers — not
//   implemented here; currently gain is applied at the simplified track-output point before summing.
//
// LIFECYCLE
//   A `Track` is held **by value** inside an immutable `SessionSnapshot` — edits happen only by
//   building a **new** snapshot on the message thread, same pattern as pre-track session state.
// =============================================================================

#include "domain/PlacedClip.h"
#include "domain/TrackStereoPan.h"

#include <juce_core/juce_core.h>

#include <cstdint>
#include <vector>

using TrackId = std::uint64_t;

inline constexpr TrackId kInvalidTrackId = 0;

inline constexpr float kTrackChannelVolumeUnityGain = 1.0f;
inline constexpr float kTrackChannelFaderGainMax = 8.0f;
inline constexpr float kTrackStereoPanCenter = 0.0f;

enum class TrackKind : std::uint8_t
{
    Audio,
    Instrument,
    /// Internal summing bus; routes to another Group or Master. No timeline clips.
    Group,
    /// Final stereo output bus (exactly one row per session). No timeline clips.
    Master,
};

/// System-owned display name for `TrackKind::Master` (not user-renamable).
inline constexpr const char kMasterTrackDisplayName[] = "Stereo Out";

// ---------------------------------------------------------------------------
// Track — one lane’s clips (session timeline samples; front-most at index 0 within this track)
// ---------------------------------------------------------------------------
class Track
{
public:
    // Unity channel fader; use `four-arg` ctor for explicit linear gain (`channelFaderGain`).
    explicit Track(TrackId id, juce::String name, std::vector<PlacedClip> placedClips) noexcept;

    // [Message thread, snapshot build] `channelFaderGain` linear, clamped [0, kTrackChannelFaderGainMax].
    // `stereoPan` in [-1,+1] (full left … full right), default center — see `TrackStereoPan.h`.
    // `trackOff`: lane skipped by playback engine. `trackMuted`: engine applies zero effective gain
    // without changing stored `channelFaderGain`.
    explicit Track(TrackId id,
                   juce::String name,
                   std::vector<PlacedClip> placedClips,
                   float channelFaderGain,
                   bool trackOff = false,
                   bool trackMuted = false,
                   TrackKind kind = TrackKind::Audio,
                   float stereoPan = kTrackStereoPanCenter,
                   TrackId routedOutputTrackId = kInvalidTrackId) noexcept;

    [[nodiscard]] TrackKind getKind() const noexcept { return kind_; }

    /// Main output destination (`Group` or `Master` row id). Unused on `Master` rows.
    [[nodiscard]] TrackId getRoutedOutputTrackId() const noexcept { return routedOutputTrackId_; }

    [[nodiscard]] TrackId getId() const noexcept { return id_; }
    [[nodiscard]] const juce::String& getName() const noexcept { return name_; }
    [[nodiscard]] int getNumPlacedClips() const noexcept;
    [[nodiscard]] const PlacedClip& getPlacedClip(int index) const;
    [[nodiscard]] const std::vector<PlacedClip>& getPlacedClips() const noexcept
    {
        return placedClips_;
    }
    // Linear gain at the channel-fader point (see header block above). Not clip gain or pre-gain.
    [[nodiscard]] float getChannelFaderGain() const noexcept { return channelFaderGain_; }
    /// If true, `PlaybackEngine` skips this track entirely (not the same as mute).
    [[nodiscard]] bool isTrackOff() const noexcept { return trackOff_; }
    /// If true, effective output gain is zero; stored fader value is unchanged.
    [[nodiscard]] bool isMuted() const noexcept { return trackMuted_; }

    /// Stereo pan [-1,+1]: left … center … right (applied after fader in playback).
    [[nodiscard]] float getStereoPan() const noexcept { return stereoPan_; }

    /// [Message thread] Same clips/gain/mute/off/kind/pan; new display name (immutable snapshot pattern).
    [[nodiscard]] Track renamed(juce::String newName) const noexcept;

private:
    TrackId id_ = kInvalidTrackId;
    juce::String name_;
    std::vector<PlacedClip> placedClips_;
    float channelFaderGain_ = kTrackChannelVolumeUnityGain;
    bool trackOff_ = false;
    bool trackMuted_ = false;
    TrackKind kind_ = TrackKind::Audio;
    float stereoPan_ = kTrackStereoPanCenter;
    TrackId routedOutputTrackId_ = kInvalidTrackId;
};
