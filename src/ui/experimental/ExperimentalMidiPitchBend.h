#pragma once

// =============================================================================
// ExperimentalMidiPitchBend — sparse 14-bit pitch-bend points, clip-owned, hold semantics
// =============================================================================
//
// ROLE IN THE ARCHITECTURE (live MIDI recording slice)
//   A recorded performance's pitch wheel is stored as sparse points in the clip's own tick domain,
//   exactly like `MidiCcPoint`s: the data moves, duplicates, deletes and undoes with its clip, is
//   serialized per clip (project v24 `pitchBend[]`), baked into render streams for realtime and
//   offline playback, mirrored into the proxy render snapshot / fingerprint, and exported as
//   standard Pitch Wheel events.
//
// VALUE RULES (deterministic, no invented defaults)
//   * `value` is the raw 14-bit wheel position 0 … 16383 (8192 = centre), per MIDI channel;
//   * a point holds until the next point of the same channel (recorded performances are stored
//     verbatim — no linear interpolation is ever invented between two received values);
//   * before a channel's first point there is NO value: nothing is emitted and no centre reset is
//     assumed, mirroring the CC rules;
//   * duplicate identities (same tick, same channel) resolve deterministically: the LAST point in
//     input order wins.
//
// THREADING
//   Pure functions; safe anywhere. The realtime path consumes precomputed, sorted events from the
//   render snapshot — no evaluation or allocation on the audio thread.
// =============================================================================

#include <juce_core/juce_core.h>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <vector>

inline constexpr int kMidiPitchBendCentre = 8192;
inline constexpr int kMidiPitchBendMax = 16383;

/// One pitch-bend point in the owning clip's tick domain (tick 0 = the clip's MIDI time zero).
struct MidiPitchBendPoint
{
    std::int64_t startTick = 0;
    /// Raw 14-bit wheel position 0 … 16383 (8192 = centre).
    int value = kMidiPitchBendCentre;
    /// Native MIDI channel 1 … 16 (same storage convention as notes and CC points).
    std::uint8_t channel = 1;
};

namespace midi_pb
{
    [[nodiscard]] inline int sanitizeValue(const int v) noexcept
    {
        return juce::jlimit(0, kMidiPitchBendMax, v);
    }
    [[nodiscard]] inline int sanitizeChannel(const int ch) noexcept { return juce::jlimit(1, 16, ch); }

    /// Canonical ordering: tick, then channel. Value is payload.
    [[nodiscard]] inline bool pointOrderLess(const MidiPitchBendPoint& a,
                                             const MidiPitchBendPoint& b) noexcept
    {
        if (a.startTick != b.startTick)
        {
            return a.startTick < b.startTick;
        }
        return a.channel < b.channel;
    }

    /// Load/repair normalization (same contract as `midi_cc::normalizePoints`): clamps, drops
    /// negative ticks, sorts canonically, resolves duplicate (tick, channel) identities last-wins.
    /// Returns the number of points dropped or clamped (load diagnostic).
    inline int normalizePoints(std::vector<MidiPitchBendPoint>& points)
    {
        int repaired = 0;
        std::vector<MidiPitchBendPoint> keep;
        keep.reserve(points.size());
        for (const auto& raw : points)
        {
            if (raw.startTick < 0)
            {
                ++repaired;
                continue;
            }
            MidiPitchBendPoint p = raw;
            const int v = sanitizeValue(p.value);
            const int ch = sanitizeChannel((int)p.channel);
            if (v != p.value || ch != (int)p.channel)
            {
                ++repaired;
            }
            p.value = v;
            p.channel = (std::uint8_t)ch;
            bool replaced = false;
            for (auto& existing : keep)
            {
                if (existing.startTick == p.startTick && existing.channel == p.channel)
                {
                    existing = p;
                    replaced = true;
                    ++repaired;
                    break;
                }
            }
            if (!replaced)
            {
                keep.push_back(p);
            }
        }
        std::stable_sort(keep.begin(), keep.end(), pointOrderLess);
        points = std::move(keep);
        return repaired;
    }

    /// Held value of one channel at `tick`, or `std::nullopt` before that channel's first point.
    /// `points` must be normalized.
    [[nodiscard]] inline std::optional<int> valueAtTick(const std::vector<MidiPitchBendPoint>& points,
                                                        const int channel,
                                                        const std::int64_t tick) noexcept
    {
        std::optional<int> held;
        for (const auto& p : points)
        {
            if ((int)p.channel != channel)
            {
                continue;
            }
            if (p.startTick > tick)
            {
                break;
            }
            held = p.value;
        }
        return held;
    }

    /// Every distinct channel present, ascending.
    [[nodiscard]] inline std::vector<int> distinctChannels(const std::vector<MidiPitchBendPoint>& points)
    {
        std::vector<int> chans;
        for (const auto& p : points)
        {
            if (std::find(chans.begin(), chans.end(), (int)p.channel) == chans.end())
            {
                chans.push_back((int)p.channel);
            }
        }
        std::sort(chans.begin(), chans.end());
        return chans;
    }
} // namespace midi_pb
