#pragma once

// =============================================================================
// TrackValueFieldText — the dB value-field texts the Inspector and the mixer share
// =============================================================================
//
// ROLE
//   One formatting / parsing vocabulary for the pre-gain field ("+6.0" / "-3.5" / "0.0", unit
//   outside) and the send-amount field ("0.00" / "+3.00" / "-Inf", dB) so a value typed in the
//   mixer reads exactly like the same value in the Inspector and both accept the same input
//   ("3", "+3 dB", "-12.5", "-inf"). Pure functions; no UI, no Session.
//
// THREADING
//   [Message thread] (pure; thread-agnostic).
// =============================================================================

#include "domain/Track.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <cctype>
#include <cmath>
#include <string>

namespace track_value_text
{

[[nodiscard]] inline juce::String utf8InfinityChar()
{
    return juce::String(juce::CharPointer_UTF8("\xe2\x88\x9e"));
}

[[nodiscard]] inline juce::String stripDbUnitSuffix(juce::String s)
{
    s = s.trim();
    if (s.endsWithIgnoreCase("db"))
    {
        s = s.substring(0, s.length() - 2).trimEnd();
    }
    return s.trim();
}

[[nodiscard]] inline bool isNegativeInfinityText(const juce::String& stripped)
{
    const juce::String t = stripped.trim();
    juce::String compact = t.toLowerCase();
    compact = compact.removeCharacters(" \t");
    if (compact == "-inf" || compact == "-infinity")
    {
        return true;
    }
    return t == (juce::String("-") + utf8InfinityChar());
}

/// Strict decimal parse of the whole string (trailing whitespace allowed); false = not a number.
[[nodiscard]] inline bool parseWholeDecimal(const juce::String& text, double& out)
{
    const std::string buf = text.toStdString();
    char* endPtr = nullptr;
    const double v = std::strtod(buf.c_str(), &endPtr);
    if (endPtr == buf.c_str())
    {
        return false;
    }
    while (endPtr != buf.c_str() + buf.size() && std::isspace(static_cast<unsigned char>(*endPtr)))
    {
        ++endPtr;
    }
    if (endPtr != buf.c_str() + buf.size())
    {
        return false;
    }
    out = v;
    return true;
}

// ---- pre-gain (dB, [-24, +24]) ---------------------------------------------------------------

/// Pre-gain field text: signed 1-decimal dB ("+6.0" / "-3.5" / "0.0"), unit label outside.
[[nodiscard]] inline juce::String formatPreGainDbToValueFieldOnly(const float preGainDb)
{
    const double d = juce::jlimit(static_cast<double>(kTrackPreGainDbMin), static_cast<double>(kTrackPreGainDbMax),
                                  static_cast<double>(preGainDb));
    if (std::fabs(d) <= 0.00005)
    {
        return juce::String("0.0");
    }
    if (d < 0.0)
    {
        return juce::String(d, 1);
    }
    return "+" + juce::String(d, 1);
}

/// Accepts "3", "+3", "-12.5", optional "dB" suffix; clamps to [-24, +24]. False = keep old.
[[nodiscard]] inline bool tryParsePreGainDbText(const juce::String& raw, float& outPreGainDb)
{
    const juce::String strippedUnit = stripDbUnitSuffix(raw);
    if (strippedUnit.isEmpty())
    {
        return false;
    }
    double v = 0.0;
    if (!parseWholeDecimal(strippedUnit, v))
    {
        return false;
    }
    outPreGainDb = sanitizeTrackPreGainDb(static_cast<float>(v));
    return true;
}

// ---- send amount (linear [0, 2] shown as dB) -------------------------------------------------

inline constexpr double kSendAmountMinDb = -60.0;
inline constexpr double kSendAmountMaxDb = 6.02;
/// Typed values closer than this to the stored amount are no-ops (no undo step).
inline constexpr float kSendAmountDriftEps = 5.0e-5f;

[[nodiscard]] inline juce::String formatSendLinearToDbField(const float amountLinear)
{
    const float clamped = clampTrackSendAmountLinear(amountLinear);
    if (clamped <= 0.0f)
    {
        return juce::String("-Inf");
    }
    const float db = juce::Decibels::gainToDecibels(clamped, static_cast<float>(kSendAmountMinDb));
    const double d = juce::jlimit(kSendAmountMinDb, kSendAmountMaxDb, static_cast<double>(db));
    if (std::fabs(d) <= 0.00005)
    {
        return juce::String("0.00");
    }
    if (d > 0.00005)
    {
        return juce::String("+") + juce::String(d, 2);
    }
    return juce::String(d, 2);
}

[[nodiscard]] inline bool tryParseSendAmountText(const juce::String& raw, float& outLinear)
{
    const juce::String strippedUnit = stripDbUnitSuffix(raw);
    if (strippedUnit.isEmpty())
    {
        return false;
    }
    if (isNegativeInfinityText(strippedUnit))
    {
        outLinear = 0.0f;
        return true;
    }
    double db = 0.0;
    if (!parseWholeDecimal(strippedUnit, db))
    {
        return false;
    }
    db = juce::jlimit(kSendAmountMinDb, kSendAmountMaxDb, db);
    if (db <= kSendAmountMinDb + 1.0e-9)
    {
        outLinear = 0.0f;
        return true;
    }
    outLinear = clampTrackSendAmountLinear(
        juce::Decibels::decibelsToGain(static_cast<float>(db), static_cast<float>(kSendAmountMinDb)));
    return true;
}

} // namespace track_value_text
