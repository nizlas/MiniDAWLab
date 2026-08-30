#pragma once

// =============================================================================
// ExperimentalMidiNoteLengthFormat — `bars.quarters.sixteenths.120ths` note length text
// =============================================================================
//
// ROLE IN THE ARCHITECTURE
//   Pure conversion between a MIDI note's `durationTicks` (see `ExperimentalMidiPattern.h`) and the
//   user-facing musical length string typed into the MIDI editor's **Len** field. No JUCE component
//   state, no session access, no clamping policy: the caller supplies the grid and owns minimum
//   length / overlap rules (`ExperimentalPianoRollView`). Header-only so it can be unit-reasoned and
//   used from both the roll and the editor window without a translation unit.
//
// FORMAT `n.p.q.r`
//   n = whole bars, p = quarter notes, q = sixteenth notes (quarters of a quarter),
//   r = 120ths of a sixteenth note. Each field is a plain count, so the format is a mixed-radix
//   number: 120 r = 1 q, 4 q = 1 p, `quartersPerBar` p = 1 n. Values above a field's base simply
//   carry when converted to ticks (0.0.0.120 and 0.0.1.0 are the same length), and formatting always
//   returns the normalized form.
//
//   The 120 subdivisions of a sixteenth exist so the field can address the musically useful
//   irrational-against-binary divisions exactly: 120 = 2^3·3·5, so triplets (40 r), quintuplets
//   (24 r) and sextuplets (20 r) of a sixteenth are all whole r values, which a power-of-two
//   subdivision could not express.
//
// GRANULARITY
//   One r unit is `ticksPerQuarter / 480` ticks (2 ticks at the default 960 PPQ), so the field is
//   coarser than the 1-tick storage granularity. Durations that are not a whole number of r units
//   (e.g. imported MIDI) are *displayed* rounded to the nearest r unit while the note keeps its
//   exact ticks; only an actual commit writes the field's value back.
//
// THREADING
//   Pure functions; safe anywhere. Called from the [Message thread] only in practice.
// =============================================================================

#include <juce_core/juce_core.h>

#include <cmath>
#include <cstdint>

namespace midi_note_length
{
    /// `q` units in one `p` (sixteenths per quarter note).
    inline constexpr std::int64_t kSixteenthsPerQuarter = 4;
    /// `r` units in one `q` (the "120th parts of a sixteenth" of the format).
    inline constexpr std::int64_t kSubdivisionsPerSixteenth = 120;
    /// `r` units in one `p`; the resolution the whole format works in below bar level.
    inline constexpr std::int64_t kSubdivisionsPerQuarter
        = kSixteenthsPerQuarter * kSubdivisionsPerSixteenth;

    /// Musical grid the text form is interpreted against: the clip's PPQ plus the project meter's
    /// bar length in quarter notes (`beatsPerBar`, e.g. 4.0 in 4/4, 3.5 in 7/8).
    struct BarGrid
    {
        int ticksPerQuarter = 960;
        double quartersPerBar = 4.0;
    };

    [[nodiscard]] inline int sanitizedTicksPerQuarter(const BarGrid& g) noexcept
    {
        return juce::jmax(1, g.ticksPerQuarter);
    }

    [[nodiscard]] inline double sanitizedQuartersPerBar(const BarGrid& g) noexcept
    {
        return (std::isfinite(g.quartersPerBar) && g.quartersPerBar > 0.0) ? g.quartersPerBar : 4.0;
    }

    /// Ticks in one bar (at least 1 so division stays safe on degenerate grids).
    [[nodiscard]] inline std::int64_t ticksPerBar(const BarGrid& g) noexcept
    {
        const double t = sanitizedQuartersPerBar(g) * (double)sanitizedTicksPerQuarter(g);
        return juce::jmax<std::int64_t>(1, (std::int64_t)std::llround(t));
    }

    /// `r` units in one bar (rounded like `ticksPerBar`, so both agree on odd meters).
    [[nodiscard]] inline std::int64_t subdivisionsPerBar(const BarGrid& g) noexcept
    {
        const double u = sanitizedQuartersPerBar(g) * (double)kSubdivisionsPerQuarter;
        return juce::jmax<std::int64_t>(1, (std::int64_t)std::llround(u));
    }

    /// Sub-bar `r` units → ticks (nearest tick; 1 r = tpq/480 ticks, i.e. 2 ticks at 960 PPQ).
    [[nodiscard]] inline std::int64_t subdivisionsToTicks(const std::int64_t units,
                                                          const BarGrid& g) noexcept
    {
        const double ticks
            = (double)units * (double)sanitizedTicksPerQuarter(g) / (double)kSubdivisionsPerQuarter;
        return (std::int64_t)std::llround(ticks);
    }

    /// Ticks → `r` units, rounded to nearest (display path; the note keeps its exact ticks).
    [[nodiscard]] inline std::int64_t ticksToSubdivisionsRounded(const std::int64_t ticks,
                                                                 const BarGrid& g) noexcept
    {
        const double units
            = (double)ticks * (double)kSubdivisionsPerQuarter / (double)sanitizedTicksPerQuarter(g);
        return (std::int64_t)std::llround(units);
    }

    struct ParseResult
    {
        bool ok = false;
        /// Total length in ticks; only meaningful when `ok`. Never negative; may be 0 (caller
        /// decides whether zero length is clamped to its minimum or rejected).
        std::int64_t ticks = 0;
    };

    /// Parses `n.p.q.r`. Surrounding whitespace is ignored. Trailing fields may be omitted
    /// (`"0.1"` = `0.1.0.0`), each present field must be non-empty decimal digits, so any sign,
    /// separator or stray character is rejected rather than silently reinterpreted. Fields above
    /// their base carry naturally because everything is summed into ticks.
    [[nodiscard]] inline ParseResult parseNoteLength(const juce::String& text,
                                                     const BarGrid& g) noexcept
    {
        const juce::String trimmed = text.trim();
        if (trimmed.isEmpty())
        {
            return {};
        }
        juce::StringArray fields;
        fields.addTokens(trimmed, ".", "");
        if (fields.size() < 1 || fields.size() > 4)
        {
            return {};
        }
        std::int64_t value[4] = { 0, 0, 0, 0 };
        for (int i = 0; i < fields.size(); ++i)
        {
            const juce::String f = fields[i].trim();
            if (f.isEmpty() || !f.containsOnly("0123456789"))
            {
                return {};
            }
            value[i] = (std::int64_t)f.getLargeIntValue();
        }

        ParseResult r;
        r.ok = true;
        r.ticks = value[0] * ticksPerBar(g)
                  + subdivisionsToTicks(value[1] * kSubdivisionsPerQuarter
                                            + value[2] * kSubdivisionsPerSixteenth
                                            + value[3],
                                        g);
        r.ticks = juce::jmax<std::int64_t>(0, r.ticks);
        return r;
    }

    /// Formats `ticks` as the normalized `n.p.q.r` form (rounded to the nearest r unit; a rounding
    /// carry past a full bar increments `n` so the text never shows an out-of-range field).
    [[nodiscard]] inline juce::String formatNoteLength(const std::int64_t ticks,
                                                       const BarGrid& g) noexcept
    {
        const std::int64_t safeTicks = juce::jmax<std::int64_t>(0, ticks);
        const std::int64_t perBarTicks = ticksPerBar(g);
        const std::int64_t perBarUnits = subdivisionsPerBar(g);

        std::int64_t bars = safeTicks / perBarTicks;
        std::int64_t units = ticksToSubdivisionsRounded(safeTicks % perBarTicks, g);
        if (units >= perBarUnits)
        {
            ++bars;
            units -= perBarUnits;
        }
        const std::int64_t quarters = units / kSubdivisionsPerQuarter;
        const std::int64_t sixteenths
            = (units % kSubdivisionsPerQuarter) / kSubdivisionsPerSixteenth;
        const std::int64_t subdivisions = units % kSubdivisionsPerSixteenth;

        return juce::String(bars) + "." + juce::String(quarters) + "." + juce::String(sixteenths)
               + "." + juce::String(subdivisions);
    }
} // namespace midi_note_length
