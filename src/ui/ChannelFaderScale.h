#pragma once

// =============================================================================
// ChannelFaderScale — the Inspector channel fader's dB ↔ travel mapping (pure functions)
// =============================================================================
//
// ROLE
//   One place that defines how the vertical Channel Volume fader maps its travel (0 = bottom,
//   1 = top) to the track's existing linear `channelFaderGain`. The fader shows GAIN in dB
//   (0 dB = unchanged, +6 dB = ×1.995, −∞ = ×0); it is not a signal level — meters show dBFS.
//
// SCALE
//   Graded like a mixing console: most of the travel covers the −20 … +6 dB range where mixes are
//   actually balanced, with the low end compressed. Piecewise-linear in dB between fixed marks,
//   linear between −60 and −80 dB in the last 4 % and exactly −∞ (gain 0) at the very bottom.
//   The same breakpoints paint the scale ticks, so the knob always sits on the printed value.
//
// PERSISTENCE
//   Only linear gain is ever stored (`Track::channelFaderGain`): −∞ is 0.0, never an
//   "Infinity"/"NaN" literal. Text in/out goes through `gainText` / `parseGainText`.
// =============================================================================

#include <juce_core/juce_core.h>

#include <array>
#include <cmath>
#include <cstdlib>
#include <string>

namespace channel_fader_scale
{

struct Scale
{
    static constexpr double kMaxDb = 6.0;
    /// Everything at/below this reads as −∞ (gain 0). Below the −60 dB mark the travel is linear
    /// down to here; position 0 snaps to exactly zero gain.
    static constexpr double kMinDb = -80.0;
    /// Printed marks (dB → travel 0..1), top to bottom. `kMinDb` is the bottom end (travel 0).
    struct Mark
    {
        double db;
        double position;
    };
    static constexpr std::array<Mark, 10> kMarks{ {
        { 6.0, 1.00 },
        { 0.0, 0.78 },
        { -6.0, 0.62 },
        { -12.0, 0.50 },
        { -20.0, 0.37 },
        { -30.0, 0.26 },
        { -40.0, 0.17 },
        { -50.0, 0.09 },
        { -60.0, 0.04 },
        { kMinDb, 0.00 },
    } };

    [[nodiscard]] static double positionForDb(double db) noexcept
    {
        if (!std::isfinite(db) || db <= kMinDb)
        {
            return 0.0;
        }
        if (db >= kMaxDb)
        {
            return 1.0;
        }
        for (size_t i = 0; i + 1 < kMarks.size(); ++i)
        {
            const Mark& hi = kMarks[i];
            const Mark& lo = kMarks[i + 1];
            if (db <= hi.db && db >= lo.db)
            {
                const double t = (db - lo.db) / (hi.db - lo.db);
                return lo.position + t * (hi.position - lo.position);
            }
        }
        return 0.0;
    }

    [[nodiscard]] static double dbForPosition(double position) noexcept
    {
        if (!(position > 0.0))
        {
            return kMinDb; // exactly the bottom = −∞
        }
        if (position >= 1.0)
        {
            return kMaxDb;
        }
        for (size_t i = 0; i + 1 < kMarks.size(); ++i)
        {
            const Mark& hi = kMarks[i];
            const Mark& lo = kMarks[i + 1];
            if (position <= hi.position && position >= lo.position)
            {
                const double t = (position - lo.position) / (hi.position - lo.position);
                return lo.db + t * (hi.db - lo.db);
            }
        }
        return kMinDb;
    }

    /// Travel → linear gain. Position 0 is exactly 0 (−∞); the top is +6 dB (1.995).
    [[nodiscard]] static float linearGainForPosition(const double position) noexcept
    {
        const double db = dbForPosition(position);
        if (db <= kMinDb)
        {
            return 0.0f;
        }
        return static_cast<float>(std::pow(10.0, db / 20.0));
    }

    [[nodiscard]] static double dbForLinearGain(const float linearGain) noexcept
    {
        if (!(linearGain > 0.0f) || !std::isfinite(linearGain))
        {
            return kMinDb;
        }
        return 20.0 * std::log10(static_cast<double>(linearGain));
    }

    [[nodiscard]] static double positionForLinearGain(const float linearGain) noexcept
    {
        return positionForDb(dbForLinearGain(linearGain));
    }

    /// "−∞" for zero gain, otherwise signed two-decimal dB ("0.00", "+6.00", "-6.02").
    [[nodiscard]] static juce::String gainText(const float linearGain)
    {
        const double db = dbForLinearGain(linearGain);
        if (db <= kMinDb)
        {
            return juce::String(juce::CharPointer_UTF8("-\xe2\x88\x9e"));
        }
        const double shown = juce::jlimit(kMinDb, kMaxDb, db);
        if (std::fabs(shown) < 0.005)
        {
            return "0.00";
        }
        return (shown > 0.0 ? "+" : "") + juce::String(shown, 2);
    }

    /// Accepts "-inf" / "-∞" / "−∞", plain numbers with optional "dB" suffix and either decimal
    /// separator; clamps to [−∞, +6 dB] (values at/below −80 dB become −∞). False = unparsable.
    [[nodiscard]] static bool parseGainText(const juce::String& raw, float& outLinearGain) noexcept
    {
        juce::String s = raw.trim();
        if (s.endsWithIgnoreCase("db"))
        {
            s = s.dropLastCharacters(2).trim();
        }
        if (s.isEmpty())
        {
            return false;
        }
        juce::String compact = s.toLowerCase().removeCharacters(" \t");
        compact = compact.replace(juce::String(juce::CharPointer_UTF8("\xe2\x88\x92")), "-"); // U+2212 minus
        if (compact == "-inf" || compact == "-infinity" || compact == "inf" || compact == "-oo"
            || compact == juce::String("-") + juce::String(juce::CharPointer_UTF8("\xe2\x88\x9e")))
        {
            outLinearGain = 0.0f;
            return true;
        }
        compact = compact.replaceCharacter(',', '.');
        const std::string buf = compact.toStdString();
        char* end = nullptr;
        const double v = std::strtod(buf.c_str(), &end);
        if (end == buf.c_str() || end != buf.c_str() + buf.size() || !std::isfinite(v))
        {
            return false;
        }
        const double db = juce::jmin(kMaxDb, v);
        outLinearGain = db <= kMinDb ? 0.0f : static_cast<float>(std::pow(10.0, db / 20.0));
        return true;
    }
};

} // namespace channel_fader_scale
