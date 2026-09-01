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
    /// MIDI-only source lane: owns MIDI clips and feeds them to one Instrument row
    /// (`getMidiDestinationTrackId()`). Produces **no** audio: it is not an audio-routing node, has
    /// no inserts/sends/audio output, allocates no audio scratch, and never appears in the audio
    /// `RoutingPlan`. Safe and silent when its destination is `None`/invalid.
    Midi,
};

/// System-owned display name for `TrackKind::Master` (not user-renamable).
inline constexpr const char kMasterTrackDisplayName[] = "Stereo Out";

// ---------------------------------------------------------------------------
// MIDI output channel — which channel a track's own timeline MIDI is sent on
// ---------------------------------------------------------------------------
//
// This is the **MIDI** destination channel, entirely unrelated to `routedOutputTrackId_` (the
// **audio** bus this track feeds) and to `channelFaderGain_` (a mixer gain, not a MIDI channel).
//
// Two modes, because instruments disagree about who owns the channel:
//   * **Fixed 1 … 16** — every outgoing event from this track is remapped to that channel. This is
//     what multi-timbral instruments need: GSi VB3-II listens for its Upper manual on channel 1,
//     Lower on 2, Pedal on 3, and a track that sends everything on channel 10 is simply inaudible
//     to it. New tracks default to channel 1; channel 10 is the drum convention and is chosen only
//     by an explicit drum-instrument creation path, never inferred from a track or plugin name.
//   * **Any / preserve** — events keep the channel stored per event (`TimelineMidiNote::channel`).
//     Required for multi-channel MIDI imports, where the file's own channel assignment is the
//     musical content, and used to migrate pre-v17 projects so their playback is bit-identical.
inline constexpr int kTrackMidiOutputChannelAny = 0;
inline constexpr int kTrackMidiOutputChannelMin = 1;
inline constexpr int kTrackMidiOutputChannelMax = 16;
/// Melodic default for newly created tracks (see the block above for why not 10).
inline constexpr int kTrackMidiOutputChannelDefault = 1;
/// General MIDI drum channel; only an explicit drum-instrument creation path may select it.
inline constexpr int kTrackMidiOutputChannelDrums = 10;

/// Repairs any stored/serialized value to `Any` or a legal 1 … 16 channel.
[[nodiscard]] inline constexpr int sanitizeTrackMidiOutputChannel(const int channel) noexcept
{
    if (channel == kTrackMidiOutputChannelAny)
    {
        return kTrackMidiOutputChannelAny;
    }
    return (channel < kTrackMidiOutputChannelMin || channel > kTrackMidiOutputChannelMax)
               ? kTrackMidiOutputChannelDefault
               : channel;
}

/// One additive send tap (post-channel-strip in V1; engine/UI in later slices).
struct TrackSend
{
    TrackId destTrackId = kInvalidTrackId;
    float amountLinear = 0.0f;
    bool enabled = true;
    /// Inspector mixer slot (0..`kTrackSendInspectorUiSlotCount - 1`); not used by engine routing.
    int uiSlotIndex = 0;
};

/// V1 Inspector exposes this many independent send slots per track.
inline constexpr int kTrackSendInspectorUiSlotCount = 4;

[[nodiscard]] inline int findTrackSendVectorIndexForUiSlot(const std::vector<TrackSend>& sends,
                                                           const int uiSlotIndex) noexcept
{
    if (uiSlotIndex < 0 || uiSlotIndex >= kTrackSendInspectorUiSlotCount)
    {
        return -1;
    }
    for (int i = 0; i < static_cast<int>(sends.size()); ++i)
    {
        if (sends[(size_t)i].uiSlotIndex == uiSlotIndex)
        {
            return i;
        }
    }
    return -1;
}

[[nodiscard]] inline int countTrackSendsOutsideInspectorUiSlots(
    const std::vector<TrackSend>& sends) noexcept
{
    int extra = 0;
    for (const TrackSend& s : sends)
    {
        if (s.uiSlotIndex < 0 || s.uiSlotIndex >= kTrackSendInspectorUiSlotCount)
        {
            ++extra;
        }
    }
    return extra;
}

inline constexpr float kSendAmountUnityLinear = 1.0f;
inline constexpr float kSendAmountMaxLinear = 2.0f;

[[nodiscard]] inline float clampTrackSendAmountLinear(const float amountLinear) noexcept
{
    return juce::jlimit(0.0f, kSendAmountMaxLinear, amountLinear);
}

[[nodiscard]] inline constexpr bool trackKindAcceptsTimelineAudioClips(const TrackKind kind) noexcept
{
    return kind == TrackKind::Audio;
}

[[nodiscard]] inline constexpr bool trackKindAcceptsRecordArm(const TrackKind kind) noexcept
{
    return kind == TrackKind::Audio;
}

/// Rows that own timeline MIDI clips and are edited in the MIDI editor.
[[nodiscard]] inline constexpr bool trackKindOwnsTimelineMidiClips(const TrackKind kind) noexcept
{
    return kind == TrackKind::Instrument || kind == TrackKind::Midi;
}

/// Rows that participate in the **audio** routing graph as a source (feed a Group/Master bus).
/// `Midi` rows are deliberately excluded: they produce no audio at all.
[[nodiscard]] inline constexpr bool trackKindIsAudioRoutingSource(const TrackKind kind) noexcept
{
    return kind == TrackKind::Audio || kind == TrackKind::Instrument || kind == TrackKind::Group;
}

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
                   TrackId routedOutputTrackId = kInvalidTrackId,
                   std::vector<TrackSend> sends = {},
                   int midiOutputChannel = kTrackMidiOutputChannelDefault,
                   TrackId midiDestinationTrackId = kInvalidTrackId) noexcept;

    [[nodiscard]] TrackKind getKind() const noexcept { return kind_; }

    /// Main **audio** output destination (`Group` or `Master` row id). Unused on `Master` rows.
    /// Not a MIDI destination — see `getMidiOutputChannel()`.
    [[nodiscard]] TrackId getRoutedOutputTrackId() const noexcept { return routedOutputTrackId_; }

    /// `kTrackMidiOutputChannelAny` (preserve per-event channel) or a fixed 1 … 16 channel that all
    /// of this track's outgoing timeline MIDI is remapped to. Meaningless on rows that emit no MIDI.
    [[nodiscard]] int getMidiOutputChannel() const noexcept { return midiOutputChannel_; }

    /// **MIDI To** — which `Instrument` row this `Midi` track feeds (`kInvalidTrackId` = None:
    /// silent, but fully editable). Only Instrument rows are legal destinations; validity is
    /// enforced by `Session`/`SessionSnapshot` repair, never by name or list position. Meaningless
    /// on every other kind. Not related to `getRoutedOutputTrackId()`, which is **audio** routing.
    [[nodiscard]] TrackId getMidiDestinationTrackId() const noexcept
    {
        return midiDestinationTrackId_;
    }

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

    [[nodiscard]] int getNumSends() const noexcept;
    [[nodiscard]] const TrackSend& getSend(int index) const;
    [[nodiscard]] const std::vector<TrackSend>& getSends() const noexcept { return sends_; }

    /// [Message thread] Same clips/gain/mute/off/kind/pan; new display name (immutable snapshot pattern).
    [[nodiscard]] Track renamed(juce::String newName) const noexcept;

    // -----------------------------------------------------------------------
    // Field-preserving copy-on-write helpers
    // -----------------------------------------------------------------------
    // Every mutation starts from a complete copy of `*this` and changes exactly one field. This is
    // the required mutation shape for snapshot rebuilds: positional re-construction (calling the
    // full constructor and listing every field by hand) has already silently dropped a new field
    // once (`midiOutputChannel` in three SessionRouting repair paths), and each new persistent
    // field multiplies that risk. The full constructor remains only for building *fresh* rows
    // (track add, project load, Master factory), where every field is deliberately explicit.
    // Values are sanitized exactly like the constructor sanitizes them.
    [[nodiscard]] Track withName(juce::String name) const noexcept;
    [[nodiscard]] Track withPlacedClips(std::vector<PlacedClip> clips) const noexcept;
    [[nodiscard]] Track withChannelFaderGain(float gain) const noexcept;
    [[nodiscard]] Track withTrackOff(bool off) const noexcept;
    [[nodiscard]] Track withMuted(bool muted) const noexcept;
    [[nodiscard]] Track withKind(TrackKind kind) const noexcept;
    [[nodiscard]] Track withStereoPan(float pan) const noexcept;
    [[nodiscard]] Track withRoutedOutputTrackId(TrackId dest) const noexcept;
    [[nodiscard]] Track withSends(std::vector<TrackSend> sends) const noexcept;
    [[nodiscard]] Track withMidiOutputChannel(int channel) const noexcept;
    [[nodiscard]] Track withMidiDestinationTrackId(TrackId dest) const noexcept;

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
    std::vector<TrackSend> sends_;
    int midiOutputChannel_ = kTrackMidiOutputChannelDefault;
    TrackId midiDestinationTrackId_ = kInvalidTrackId;
};
