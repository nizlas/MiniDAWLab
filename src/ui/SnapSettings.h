#pragma once

// =============================================================================
// SnapSettings — global arrangement + MIDI-editor snap (enabled flag + resolution)
// =============================================================================
//
// Legacy project key "bar" (measure grid) maps to Straight_1_1 (1/1) on load — see
// `snapResolutionFromProjectString`.

#include <array>
#include <cstddef>

#include <juce_core/juce_core.h>

enum class SnapResolution
{
    Straight_1_1,
    Straight_1_2,
    Straight_1_4,
    Straight_1_8,
    Straight_1_16,
    Straight_1_32,
    Straight_1_64,
    Straight_1_128,
    Triplet_1_2,
    Triplet_1_4,
    Triplet_1_8,
    Triplet_1_16,
    Triplet_1_32,
    Triplet_1_64,
    Dotted_1_2,
    Dotted_1_4,
    Dotted_1_8,
    Dotted_1_16,
    Dotted_1_32,
    Dotted_1_64,
};

/// Combo order (ids 1 … N). "Off" is **not** here (`SnapSettings::enabled`). No "Bar" — use 1/1.
inline constexpr std::array<SnapResolution, 20> kSnapResolutionComboOrder{ {
    SnapResolution::Straight_1_1,
    SnapResolution::Straight_1_2,
    SnapResolution::Straight_1_4,
    SnapResolution::Straight_1_8,
    SnapResolution::Straight_1_16,
    SnapResolution::Straight_1_32,
    SnapResolution::Straight_1_64,
    SnapResolution::Straight_1_128,
    SnapResolution::Triplet_1_2,
    SnapResolution::Triplet_1_4,
    SnapResolution::Triplet_1_8,
    SnapResolution::Triplet_1_16,
    SnapResolution::Triplet_1_32,
    SnapResolution::Triplet_1_64,
    SnapResolution::Dotted_1_2,
    SnapResolution::Dotted_1_4,
    SnapResolution::Dotted_1_8,
    SnapResolution::Dotted_1_16,
    SnapResolution::Dotted_1_32,
    SnapResolution::Dotted_1_64,
} };

struct SnapSettings
{
    bool enabled = false;
    SnapResolution resolution = SnapResolution::Straight_1_4;
};

/// Serialized under project root keys `snapEnabled` / `snapResolution`.
struct SnapProjectRootFields
{
    bool enabled = false;
    juce::String resolutionKey { "1_4" };
};

namespace snap_resolution_detail
{
[[nodiscard]] inline SnapResolution snapFromStraightDenominatorToken(const juce::String& token) noexcept
{
    if (token == "1_1")
        return SnapResolution::Straight_1_1;
    if (token == "1_2")
        return SnapResolution::Straight_1_2;
    if (token == "1_4")
        return SnapResolution::Straight_1_4;
    if (token == "1_8")
        return SnapResolution::Straight_1_8;
    if (token == "1_16")
        return SnapResolution::Straight_1_16;
    if (token == "1_32")
        return SnapResolution::Straight_1_32;
    if (token == "1_64")
        return SnapResolution::Straight_1_64;
    if (token == "1_128")
        return SnapResolution::Straight_1_128;
    return SnapResolution::Straight_1_4;
}

[[nodiscard]] inline SnapResolution snapFromTripletDenominatorToken(const juce::String& token) noexcept
{
    if (token == "1_2")
        return SnapResolution::Triplet_1_2;
    if (token == "1_4")
        return SnapResolution::Triplet_1_4;
    if (token == "1_8")
        return SnapResolution::Triplet_1_8;
    if (token == "1_16")
        return SnapResolution::Triplet_1_16;
    if (token == "1_32")
        return SnapResolution::Triplet_1_32;
    if (token == "1_64")
        return SnapResolution::Triplet_1_64;
    return SnapResolution::Straight_1_4;
}

[[nodiscard]] inline SnapResolution snapFromDottedDenominatorToken(const juce::String& token) noexcept
{
    if (token == "1_2")
        return SnapResolution::Dotted_1_2;
    if (token == "1_4")
        return SnapResolution::Dotted_1_4;
    if (token == "1_8")
        return SnapResolution::Dotted_1_8;
    if (token == "1_16")
        return SnapResolution::Dotted_1_16;
    if (token == "1_32")
        return SnapResolution::Dotted_1_32;
    if (token == "1_64")
        return SnapResolution::Dotted_1_64;
    return SnapResolution::Straight_1_4;
}
} // namespace snap_resolution_detail

[[nodiscard]] inline juce::String snapResolutionToProjectString(SnapResolution r) noexcept
{
    switch (r)
    {
    case SnapResolution::Straight_1_1:
        return "1_1";
    case SnapResolution::Straight_1_2:
        return "1_2";
    case SnapResolution::Straight_1_4:
        return "1_4";
    case SnapResolution::Straight_1_8:
        return "1_8";
    case SnapResolution::Straight_1_16:
        return "1_16";
    case SnapResolution::Straight_1_32:
        return "1_32";
    case SnapResolution::Straight_1_64:
        return "1_64";
    case SnapResolution::Straight_1_128:
        return "1_128";
    case SnapResolution::Triplet_1_2:
        return "1_2t";
    case SnapResolution::Triplet_1_4:
        return "1_4t";
    case SnapResolution::Triplet_1_8:
        return "1_8t";
    case SnapResolution::Triplet_1_16:
        return "1_16t";
    case SnapResolution::Triplet_1_32:
        return "1_32t";
    case SnapResolution::Triplet_1_64:
        return "1_64t";
    case SnapResolution::Dotted_1_2:
        return "1_2d";
    case SnapResolution::Dotted_1_4:
        return "1_4d";
    case SnapResolution::Dotted_1_8:
        return "1_8d";
    case SnapResolution::Dotted_1_16:
        return "1_16d";
    case SnapResolution::Dotted_1_32:
        return "1_32d";
    case SnapResolution::Dotted_1_64:
        return "1_64d";
    }
    return "1_4";
}

/// Legacy `bar` → Straight_1_1. Also accepts `1_2`…`1_128`, `1_Nt`, `1_Nd`.
[[nodiscard]] inline SnapResolution snapResolutionFromProjectString(const juce::String& sIn) noexcept
{
    juce::String s = sIn.trim().toLowerCase();
    if (s == "bar")
        return SnapResolution::Straight_1_1;

    const bool triplet = s.endsWithIgnoreCase("t") && s.length() > 1;
    const bool dotted = s.endsWithIgnoreCase("d") && s.length() > 1;
    if (triplet && dotted)
        return SnapResolution::Straight_1_4;

    if (triplet)
        return snap_resolution_detail::snapFromTripletDenominatorToken(s.substring(0, s.length() - 1));
    if (dotted)
        return snap_resolution_detail::snapFromDottedDenominatorToken(s.substring(0, s.length() - 1));
    return snap_resolution_detail::snapFromStraightDenominatorToken(s);
}

[[nodiscard]] inline juce::String snapResolutionDisplayName(SnapResolution r) noexcept
{
    switch (r)
    {
    case SnapResolution::Straight_1_1:
        return "1/1";
    case SnapResolution::Straight_1_2:
        return "1/2";
    case SnapResolution::Straight_1_4:
        return "1/4";
    case SnapResolution::Straight_1_8:
        return "1/8";
    case SnapResolution::Straight_1_16:
        return "1/16";
    case SnapResolution::Straight_1_32:
        return "1/32";
    case SnapResolution::Straight_1_64:
        return "1/64";
    case SnapResolution::Straight_1_128:
        return "1/128";
    case SnapResolution::Triplet_1_2:
        return "1/2 Triplet";
    case SnapResolution::Triplet_1_4:
        return "1/4 Triplet";
    case SnapResolution::Triplet_1_8:
        return "1/8 Triplet";
    case SnapResolution::Triplet_1_16:
        return "1/16 Triplet";
    case SnapResolution::Triplet_1_32:
        return "1/32 Triplet";
    case SnapResolution::Triplet_1_64:
        return "1/64 Triplet";
    case SnapResolution::Dotted_1_2:
        return "1/2 Dotted";
    case SnapResolution::Dotted_1_4:
        return "1/4 Dotted";
    case SnapResolution::Dotted_1_8:
        return "1/8 Dotted";
    case SnapResolution::Dotted_1_16:
        return "1/16 Dotted";
    case SnapResolution::Dotted_1_32:
        return "1/32 Dotted";
    case SnapResolution::Dotted_1_64:
        return "1/64 Dotted";
    }
    return "1/4";
}

[[nodiscard]] inline int snapResolutionToComboItemId(SnapResolution r) noexcept
{
    int id = 1;
    for (const SnapResolution o : kSnapResolutionComboOrder)
    {
        if (o == r)
            return id;
        ++id;
    }
    return 3; // 1/4
}

[[nodiscard]] inline SnapResolution snapResolutionFromComboItemId(int id) noexcept
{
    if (id >= 1 && id <= static_cast<int>(kSnapResolutionComboOrder.size()))
        return kSnapResolutionComboOrder[static_cast<std::size_t>(id - 1)];
    return SnapResolution::Straight_1_4;
}
