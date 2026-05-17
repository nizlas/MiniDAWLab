#pragma once

// =============================================================================
// SnapSettings — arrangement snap UI + project root persistence; Slice D applies snap on arrangement edits.
// =============================================================================

#include <juce_core/juce_core.h>

enum class SnapResolution
{
    Bar = 0,
    Half,
    Quarter,
    Eighth,
    Sixteenth,
};

struct SnapSettings
{
    bool enabled = false;
    SnapResolution resolution = SnapResolution::Quarter;
};

/// Serialized under project root keys `snapEnabled` / `snapResolution`.
struct SnapProjectRootFields
{
    bool enabled = false;
    juce::String resolutionKey { "1_4" };
};

[[nodiscard]] inline juce::String snapResolutionToProjectString(SnapResolution r) noexcept
{
    switch (r)
    {
    case SnapResolution::Bar:
        return "bar";
    case SnapResolution::Half:
        return "1_2";
    case SnapResolution::Quarter:
        return "1_4";
    case SnapResolution::Eighth:
        return "1_8";
    case SnapResolution::Sixteenth:
        return "1_16";
    }
    return "1_4";
}

[[nodiscard]] inline SnapResolution snapResolutionFromProjectString(const juce::String& s) noexcept
{
    if (s.equalsIgnoreCase("bar"))
    {
        return SnapResolution::Bar;
    }
    if (s.equalsIgnoreCase("1_2"))
    {
        return SnapResolution::Half;
    }
    if (s.equalsIgnoreCase("1_4"))
    {
        return SnapResolution::Quarter;
    }
    if (s.equalsIgnoreCase("1_8"))
    {
        return SnapResolution::Eighth;
    }
    if (s.equalsIgnoreCase("1_16"))
    {
        return SnapResolution::Sixteenth;
    }
    return SnapResolution::Quarter;
}

[[nodiscard]] inline juce::String snapResolutionDisplayName(SnapResolution r) noexcept
{
    switch (r)
    {
    case SnapResolution::Bar:
        return "Bar";
    case SnapResolution::Half:
        return "1/2";
    case SnapResolution::Quarter:
        return "1/4";
    case SnapResolution::Eighth:
        return "1/8";
    case SnapResolution::Sixteenth:
        return "1/16";
    }
    return "1/4";
}

[[nodiscard]] inline int snapResolutionToComboItemId(SnapResolution r) noexcept
{
    switch (r)
    {
    case SnapResolution::Bar:
        return 1;
    case SnapResolution::Half:
        return 2;
    case SnapResolution::Quarter:
        return 3;
    case SnapResolution::Eighth:
        return 4;
    case SnapResolution::Sixteenth:
        return 5;
    default:
        return 3;
    }
}

[[nodiscard]] inline SnapResolution snapResolutionFromComboItemId(int id) noexcept
{
    switch (id)
    {
    case 1:
        return SnapResolution::Bar;
    case 2:
        return SnapResolution::Half;
    case 3:
        return SnapResolution::Quarter;
    case 4:
        return SnapResolution::Eighth;
    case 5:
        return SnapResolution::Sixteenth;
    default:
        return SnapResolution::Quarter;
    }
}
