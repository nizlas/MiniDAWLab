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
//   Future **post-fader** inserts/sends/explicit taps may require per-track staging buffers — not
//   implemented here; currently gain is applied at the simplified track-output point before summing.
//
// LIFECYCLE
//   A `Track` is held **by value** inside an immutable `SessionSnapshot` — edits happen only by
//   building a **new** snapshot on the message thread, same pattern as pre-track session state.
// =============================================================================

#include "domain/PlacedClip.h"

#include <juce_core/juce_core.h>

#include <cstdint>
#include <vector>

using TrackId = std::uint64_t;

inline constexpr TrackId kInvalidTrackId = 0;

inline constexpr float kTrackChannelVolumeUnityGain = 1.0f;
inline constexpr float kTrackChannelFaderGainMax = 8.0f;

// ---------------------------------------------------------------------------
// Track — one lane’s clips (session timeline samples; front-most at index 0 within this track)
// ---------------------------------------------------------------------------
class Track
{
public:
    // Unity channel fader; use `four-arg` ctor for explicit linear gain (`channelFaderGain`).
    explicit Track(TrackId id, juce::String name, std::vector<PlacedClip> placedClips) noexcept;

    // [Message thread, snapshot build] Same as three-arg constructor; `channelFaderGain` is linear
    // (0 = fader at −∞, 1 = 0 dB). Clamped to [0, kTrackChannelFaderGainMax].
    explicit Track(TrackId id,
                   juce::String name,
                   std::vector<PlacedClip> placedClips,
                   float channelFaderGain) noexcept;

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

private:
    TrackId id_ = kInvalidTrackId;
    juce::String name_;
    std::vector<PlacedClip> placedClips_;
    float channelFaderGain_ = kTrackChannelVolumeUnityGain;
};
