#pragma once

// =============================================================================
// ExperimentalMidiCcAutomation — sparse MIDI CC points + deterministic evaluation
// =============================================================================
//
// ROLE IN THE ARCHITECTURE (Stage D)
//   Pure model + evaluator for the MIDI editor's controller lane. A clip's pattern owns a sparse,
//   sorted list of `MidiCcPoint`s in the same tick domain as its `TimelineMidiNote`s, so CC
//   automation *moves, duplicates and deletes with its clip* exactly like notes do.
//
//   Everything here is header-only pure logic (no JUCE components, no session, no host) so the
//   selftest target exercises the exact evaluator the realtime, offline, audition and export
//   paths use — parity by construction, not by parallel implementations.
//
// CHANNEL SEMANTICS
//   A point stores its **native** channel (1…16), mirroring `TimelineMidiNote::channel`. The
//   effective channel sent/exported is `midi_channel_diag::effectiveChannel(native, trackOutput)`
//   of the point's own SOURCE track — never of a `MIDI To` destination. Changing the track output
//   never rewrites stored points.
//
// CURVE RULES (all deterministic, no invented defaults)
//   * at a point's exact tick the value is that point's exact value;
//   * `Hold` keeps the previous point's value until the next point;
//   * `Linear` interpolates to the next point, rounded to the nearest integer 0…127
//     (llround, half away from zero — stable everywhere);
//   * before the first point of a (controller, channel) stream there is NO value at all
//     (`std::nullopt`) — nothing is emitted and no default like 0/64/127 is assumed;
//   * after the final point its value holds forever.
//
// EVENT GENERATION (bounded, no floods)
//   `collectCcEventsInTickRange` emits one event per *value change* only: the segment endpoints
//   plus, inside a Linear segment, the first tick at which each crossed integer value is reached.
//   A monotonic ramp therefore emits at most |v1 - v0| + 1 events regardless of length, and a
//   Hold segment emits exactly its endpoints. Repeated evaluation of adjacent ranges never
//   re-emits an unchanged value (the caller tracks the last sent value per stream — see
//   `MidiCcChaseState`).
//
// THREADING
//   Pure functions; safe anywhere. The realtime path consumes *precomputed* events published in
//   render snapshots — no evaluation, no allocation on the audio thread.
// =============================================================================

#include <juce_core/juce_core.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <vector>

/// Segment shape from one CC point to the next.
enum class MidiCcInterpolation : std::uint8_t
{
    hold = 0,
    linear = 1
};

/// One sparse MIDI controller automation point, in the owning clip's tick domain (tick 0 = the
/// clip's MIDI time zero, same as `TimelineMidiNote::startTick`).
struct MidiCcPoint
{
    std::int64_t startTick = 0;
    /// Controller number 0…127 (11 = Expression, the primary acceptance case).
    std::uint8_t controller = 11;
    /// Controller value 0…127.
    std::uint8_t value = 0;
    /// Native MIDI channel 1…16 (same storage convention as notes).
    std::uint8_t channel = 1;
    /// Shape of the segment leading to the NEXT point of the same (controller, channel) stream.
    MidiCcInterpolation interpolationToNext = MidiCcInterpolation::linear;
};

namespace midi_cc
{
    [[nodiscard]] inline int sanitizeController(const int c) noexcept
    {
        return juce::jlimit(0, 127, c);
    }
    [[nodiscard]] inline int sanitizeValue(const int v) noexcept { return juce::jlimit(0, 127, v); }
    [[nodiscard]] inline int sanitizeChannel(const int ch) noexcept
    {
        return juce::jlimit(1, 16, ch);
    }
    [[nodiscard]] inline MidiCcInterpolation sanitizeInterpolation(const int raw) noexcept
    {
        return raw == 1 ? MidiCcInterpolation::linear : MidiCcInterpolation::hold;
    }

    /// Canonical ordering: tick, then controller, then channel. Value/shape are payload.
    [[nodiscard]] inline bool pointOrderLess(const MidiCcPoint& a, const MidiCcPoint& b) noexcept
    {
        if (a.startTick != b.startTick)
        {
            return a.startTick < b.startTick;
        }
        if (a.controller != b.controller)
        {
            return a.controller < b.controller;
        }
        return a.channel < b.channel;
    }

    /// Two points are duplicates when they occupy the same (tick, controller, channel) identity.
    [[nodiscard]] inline bool pointsShareIdentity(const MidiCcPoint& a, const MidiCcPoint& b) noexcept
    {
        return a.startTick == b.startTick && a.controller == b.controller && a.channel == b.channel;
    }

    /// Load/repair normalization: clamps every field into range, drops negative-tick points,
    /// sorts canonically and resolves duplicate identities deterministically (the LAST point in
    /// input order wins, matching "the last deterministic event wins" everywhere else). Notes are
    /// never touched — this operates on the CC vector alone. Returns the number of points that
    /// were dropped or clamped, for a load diagnostic.
    inline int normalizePoints(std::vector<MidiCcPoint>& points)
    {
        int repaired = 0;
        std::vector<MidiCcPoint> keep;
        keep.reserve(points.size());
        for (const auto& raw : points)
        {
            if (raw.startTick < 0)
            {
                ++repaired;
                continue;
            }
            MidiCcPoint p = raw;
            const int c = sanitizeController((int)p.controller);
            const int v = sanitizeValue((int)p.value);
            const int ch = sanitizeChannel((int)p.channel);
            if (c != (int)p.controller || v != (int)p.value || ch != (int)p.channel)
            {
                ++repaired;
            }
            p.controller = (std::uint8_t)c;
            p.value = (std::uint8_t)v;
            p.channel = (std::uint8_t)ch;
            // Later duplicate replaces earlier — deterministic last-wins.
            bool replaced = false;
            for (auto& existing : keep)
            {
                if (pointsShareIdentity(existing, p))
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

    /// Rounded linear interpolation between two points at `tick` (caller guarantees
    /// `a.startTick <= tick <= b.startTick` and `a.startTick < b.startTick`).
    [[nodiscard]] inline int linearValueAt(const MidiCcPoint& a,
                                           const MidiCcPoint& b,
                                           const std::int64_t tick) noexcept
    {
        const double span = (double)(b.startTick - a.startTick);
        const double t = (double)(tick - a.startTick) / span;
        const double v = (double)a.value + t * ((double)b.value - (double)a.value);
        return sanitizeValue((int)std::llround(v));
    }

    /// The curve value of one (controller, channel) stream at `tick`, or `std::nullopt` before
    /// its first point. `points` must be normalized (sorted; unique identities).
    [[nodiscard]] inline std::optional<int> valueAtTick(const std::vector<MidiCcPoint>& points,
                                                        const int controller,
                                                        const int channel,
                                                        const std::int64_t tick) noexcept
    {
        const MidiCcPoint* prev = nullptr;
        const MidiCcPoint* next = nullptr;
        for (const auto& p : points)
        {
            if ((int)p.controller != controller || (int)p.channel != channel)
            {
                continue;
            }
            if (p.startTick <= tick)
            {
                prev = &p; // sorted: keeps advancing to the latest point at or before `tick`
            }
            else
            {
                next = &p;
                break;
            }
        }
        if (prev == nullptr)
        {
            return std::nullopt; // before the first point: no value exists, nothing may be sent
        }
        if (prev->startTick == tick || next == nullptr
            || prev->interpolationToNext == MidiCcInterpolation::hold)
        {
            return (int)prev->value;
        }
        return linearValueAt(*prev, *next, tick);
    }

    /// One discrete controller change ready to schedule/export (tick domain of the clip).
    struct MidiCcEvent
    {
        std::int64_t tick = 0;
        std::uint8_t controller = 0;
        std::uint8_t value = 0;
        /// NATIVE channel; callers apply `midi_channel_diag::effectiveChannel` when sending.
        std::uint8_t channel = 1;
    };

    /// Emits the bounded discrete events of ONE (controller, channel) stream inside
    /// `[rangeStartTick, rangeEndTickExclusive)`: segment endpoints in range, plus the first tick
    /// of every crossed integer value inside Linear segments. Values equal to `lastSentValue`
    /// at the range start are skipped so adjacent-range scans never resend an unchanged value.
    /// Events are appended in ascending tick order.
    inline void collectCcEventsInTickRange(const std::vector<MidiCcPoint>& points,
                                           const int controller,
                                           const int channel,
                                           const std::int64_t rangeStartTick,
                                           const std::int64_t rangeEndTickExclusive,
                                           std::optional<int> lastSentValue,
                                           std::vector<MidiCcEvent>& out)
    {
        if (rangeEndTickExclusive <= rangeStartTick)
        {
            return;
        }
        // Gather this stream's points (already sorted globally → stays sorted).
        std::vector<const MidiCcPoint*> stream;
        for (const auto& p : points)
        {
            if ((int)p.controller == controller && (int)p.channel == channel)
            {
                stream.push_back(&p);
            }
        }
        if (stream.empty())
        {
            return;
        }

        const auto emit = [&](const std::int64_t tick, const int value) {
            if (lastSentValue.has_value() && *lastSentValue == value)
            {
                return;
            }
            MidiCcEvent e;
            e.tick = tick;
            e.controller = (std::uint8_t)controller;
            e.value = (std::uint8_t)sanitizeValue(value);
            e.channel = (std::uint8_t)channel;
            out.push_back(e);
            lastSentValue = value;
        };

        for (size_t i = 0; i < stream.size(); ++i)
        {
            const MidiCcPoint& a = *stream[i];
            const MidiCcPoint* b = (i + 1 < stream.size()) ? stream[i + 1] : nullptr;

            // The point itself, when inside the range.
            if (a.startTick >= rangeStartTick && a.startTick < rangeEndTickExclusive)
            {
                emit(a.startTick, (int)a.value);
            }
            if (b == nullptr || a.interpolationToNext == MidiCcInterpolation::hold)
            {
                continue;
            }
            // Linear segment (a → b): first tick of each crossed integer value. Bounded by
            // |b.value - a.value| events regardless of segment length.
            const std::int64_t segLo = juce::jmax(a.startTick + 1, rangeStartTick);
            const std::int64_t segHi = juce::jmin(b->startTick, rangeEndTickExclusive);
            if (segLo >= segHi || a.value == b->value)
            {
                continue;
            }
            const int dir = b->value > a.value ? 1 : -1;
            const double span = (double)(b->startTick - a.startTick);
            const double slope = ((double)b->value - (double)a.value) / span;
            for (int v = (int)a.value + dir; v != (int)b->value + dir; v += dir)
            {
                // First tick at which the ROUNDED value becomes v: solve round(a + slope*dt) == v
                // → a + slope*dt >= v - 0.5*dir … take the smallest integer dt with that property.
                const double target = (double)v - 0.5 * (double)dir;
                double dt = ((double)target - (double)a.value) / slope;
                std::int64_t tick = a.startTick + (std::int64_t)std::ceil(dt - 1.0e-9);
                // llround rounds half away from zero, matching linearValueAt; nudge until the
                // evaluator agrees (at most one tick — guards float edge cases deterministically).
                while (tick < b->startTick && linearValueAt(a, *b, tick) != v)
                {
                    ++tick;
                }
                if (tick >= segHi)
                {
                    break;
                }
                if (tick >= segLo && tick < b->startTick)
                {
                    emit(tick, v);
                }
            }
        }
    }

    /// Every distinct (controller, channel) stream present, in canonical order.
    struct MidiCcStreamKey
    {
        int controller = 0;
        int channel = 1;
    };
    [[nodiscard]] inline std::vector<MidiCcStreamKey>
    distinctStreams(const std::vector<MidiCcPoint>& points)
    {
        std::vector<MidiCcStreamKey> keys;
        for (const auto& p : points)
        {
            bool found = false;
            for (const auto& k : keys)
            {
                found = found || (k.controller == (int)p.controller && k.channel == (int)p.channel);
            }
            if (!found)
            {
                keys.push_back({ (int)p.controller, (int)p.channel });
            }
        }
        std::sort(keys.begin(), keys.end(), [](const MidiCcStreamKey& a, const MidiCcStreamKey& b) {
            return a.controller != b.controller ? a.controller < b.controller
                                                : a.channel < b.channel;
        });
        return keys;
    }

    /// Common controller names for the lane selector. Always paired with the number by callers —
    /// naming may never create ambiguity. Empty for controllers without a well-known meaning.
    [[nodiscard]] inline juce::String controllerShortName(const int controller)
    {
        switch (controller)
        {
            case 1: return "Modulation";
            case 2: return "Breath";
            case 4: return "Foot";
            case 5: return "Portamento Time";
            case 7: return "Volume";
            case 10: return "Pan";
            case 11: return "Expression";
            case 64: return "Sustain";
            case 65: return "Portamento";
            case 66: return "Sostenuto";
            case 67: return "Soft Pedal";
            case 91: return "Reverb";
            case 93: return "Chorus";
            default: return {};
        }
    }

    /// `"CC11 Expression"` / `"CC3"` — the display string used by the lane selector.
    [[nodiscard]] inline juce::String controllerDisplayName(const int controller)
    {
        const juce::String name = controllerShortName(controller);
        juce::String s = "CC" + juce::String(sanitizeController(controller));
        if (name.isNotEmpty())
        {
            s += " " + name;
        }
        return s;
    }
} // namespace midi_cc
