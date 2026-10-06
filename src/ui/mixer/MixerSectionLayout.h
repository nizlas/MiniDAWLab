#pragma once

// =============================================================================
// MixerSectionLayout — the one vertical layout every mixer channel strip follows
// =============================================================================
//
// ROLE
//   The mixer's sections (Routing, Pre-gain, Pre inserts, Post inserts, Sends, Faders, Meters)
//   can each be shown or minimised, GLOBALLY for all strips. This header turns the visibility
//   flags and the available height into one list of section bands (y ranges) that every strip —
//   the scrolling ones and the fixed Stereo Out — applies verbatim, so a row's own content
//   (fewer routing rows, no inserts, no sends) can never move its fader relative to the
//   neighbours. Pure geometry; no components, no session.
//
// SIZES (logical px)
//   Strip width 150 (inside the requested 140–160 window) and fixed band heights per section;
//   the lower band (fader + pan at the left, output meter at the right) takes whatever height
//   remains, never less than `kLowerBandMinHeightPx` — the common vertical scroll bar appears
//   when the window is lower than the stack.
// =============================================================================

#include <array>
#include <juce_graphics/juce_graphics.h>

namespace mixer_layout
{

enum class Section : int
{
    Routing = 0,
    PreGain,
    PreInserts,
    PostInserts,
    Sends,
    Faders,
    Meters,
    Count
};

inline constexpr int kSectionCount = static_cast<int>(Section::Count);

[[nodiscard]] inline const char* sectionName(const Section s) noexcept
{
    switch (s)
    {
    case Section::Routing: return "Routing";
    case Section::PreGain: return "Pre-gain";
    case Section::PreInserts: return "Pre inserts";
    case Section::PostInserts: return "Post inserts";
    case Section::Sends: return "Sends";
    case Section::Faders: return "Faders";
    case Section::Meters: return "Meters";
    case Section::Count: break;
    }
    return "";
}

/// Stable persistence keys (ui-layout.xml attributes); never renamed.
[[nodiscard]] inline const char* sectionKey(const Section s) noexcept
{
    switch (s)
    {
    case Section::Routing: return "routing";
    case Section::PreGain: return "preGain";
    case Section::PreInserts: return "preInserts";
    case Section::PostInserts: return "postInserts";
    case Section::Sends: return "sends";
    case Section::Faders: return "faders";
    case Section::Meters: return "meters";
    case Section::Count: break;
    }
    return "";
}

struct SectionVisibility
{
    std::array<bool, kSectionCount> shown{ { true, true, true, true, true, true, true } };

    [[nodiscard]] bool get(const Section s) const noexcept { return shown[static_cast<size_t>(s)]; }
    void set(const Section s, const bool v) noexcept { shown[static_cast<size_t>(s)] = v; }
    [[nodiscard]] bool operator==(const SectionVisibility& o) const noexcept { return shown == o.shown; }
    [[nodiscard]] bool operator!=(const SectionVisibility& o) const noexcept { return !(*this == o); }
};

// --- fixed geometry -----------------------------------------------------------------------------
inline constexpr int kStripWidthPx = 150;
inline constexpr int kStripGapPx = 3;
inline constexpr int kStripPadPx = 4;
inline constexpr int kSectionGapPx = 4;
inline constexpr int kSectionCaptionHeightPx = 13;

/// Header (always shown): name row, kind row, base-button row.
inline constexpr int kHeaderNameRowHeightPx = 20;
inline constexpr int kHeaderKindRowHeightPx = 12;
inline constexpr int kHeaderButtonsRowHeightPx = 22;
inline constexpr int kHeaderHeightPx = kHeaderNameRowHeightPx + kHeaderKindRowHeightPx + kHeaderButtonsRowHeightPx + 4;

/// Routing: four caption+selector rows (the kind with most rows — Instrument / Midi — needs 4).
inline constexpr int kRoutingRowsPerStrip = 4;
inline constexpr int kRoutingCaptionHeightPx = 11;
inline constexpr int kRoutingControlHeightPx = 19;
inline constexpr int kRoutingRowHeightPx = kRoutingCaptionHeightPx + kRoutingControlHeightPx + 2;
inline constexpr int kRoutingSectionHeightPx = kSectionCaptionHeightPx + kRoutingRowsPerStrip * kRoutingRowHeightPx;

/// Pre-gain: caption + one value row.
inline constexpr int kPreGainValueRowHeightPx = 20;
inline constexpr int kPreGainSectionHeightPx = kSectionCaptionHeightPx + kPreGainValueRowHeightPx + 1;

/// Inserts (per stage): caption, `kInsertRowsPerStage` rows, one add row.
inline constexpr int kInsertRowsPerStage = 3;
inline constexpr int kInsertRowHeightPx = 18;
inline constexpr int kInsertsSectionHeightPx = kSectionCaptionHeightPx + kInsertRowsPerStage * kInsertRowHeightPx + kInsertRowHeightPx + 2;

/// Sends: caption + four one-line rows (enable | destination | amount).
inline constexpr int kSendRows = 4;
inline constexpr int kSendRowHeightPx = 20;
inline constexpr int kSendsSectionHeightPx = kSectionCaptionHeightPx + kSendRows * kSendRowHeightPx + 2;

/// Lower band: pan row above the fader; the meter shares the band on the right. With every
/// section shown the stack is 678 px: it fits the default 720 px window above its scroll bar.
inline constexpr int kPanRowHeightPx = 24;
inline constexpr int kLowerBandMinHeightPx = 144;

[[nodiscard]] inline int fixedSectionHeight(const Section s) noexcept
{
    switch (s)
    {
    case Section::Routing: return kRoutingSectionHeightPx;
    case Section::PreGain: return kPreGainSectionHeightPx;
    case Section::PreInserts: return kInsertsSectionHeightPx;
    case Section::PostInserts: return kInsertsSectionHeightPx;
    case Section::Sends: return kSendsSectionHeightPx;
    case Section::Faders:
    case Section::Meters:
    case Section::Count: break;
    }
    return 0;
}

/// The bands every strip applies. A hidden section has an empty band (height 0).
struct ComputedLayout
{
    juce::Rectangle<int> header;
    std::array<juce::Rectangle<int>, kSectionCount> bands{};
    /// The lower band shared by Faders and Meters (empty when both are hidden).
    juce::Rectangle<int> lowerBand;
    int totalHeight = 0;

    [[nodiscard]] juce::Rectangle<int> band(const Section s) const noexcept { return bands[static_cast<size_t>(s)]; }
};

/// Height the fixed sections need (header included) for the given visibility, without the
/// lower band.
[[nodiscard]] inline int fixedStackHeight(const SectionVisibility& v) noexcept
{
    int h = kStripPadPx + kHeaderHeightPx;
    for (int i = 0; i < kSectionCount; ++i)
    {
        const Section s = static_cast<Section>(i);
        if (s == Section::Faders || s == Section::Meters || !v.get(s))
        {
            continue;
        }
        h += kSectionGapPx + fixedSectionHeight(s);
    }
    return h;
}

/// Minimum strip height (lower band at its minimum when any of Faders / Meters is shown).
[[nodiscard]] inline int minimumStripHeight(const SectionVisibility& v) noexcept
{
    const bool lower = v.get(Section::Faders) || v.get(Section::Meters);
    return fixedStackHeight(v) + (lower ? kSectionGapPx + kLowerBandMinHeightPx : 0) + kStripPadPx;
}

/// Lay the sections out in `availableHeight` (the strip's height). When the window is lower
/// than `minimumStripHeight`, the result is taller than `availableHeight` and the owner scrolls.
[[nodiscard]] inline ComputedLayout computeLayout(const SectionVisibility& v, const int stripWidth, const int availableHeight) noexcept
{
    ComputedLayout out;
    const int x = kStripPadPx;
    const int w = juce::jmax(0, stripWidth - 2 * kStripPadPx);
    int y = kStripPadPx;
    out.header = juce::Rectangle<int>(x, y, w, kHeaderHeightPx);
    y += kHeaderHeightPx;
    for (int i = 0; i < kSectionCount; ++i)
    {
        const Section s = static_cast<Section>(i);
        if (s == Section::Faders || s == Section::Meters)
        {
            continue;
        }
        if (!v.get(s))
        {
            out.bands[static_cast<size_t>(i)] = {};
            continue;
        }
        y += kSectionGapPx;
        out.bands[static_cast<size_t>(i)] = juce::Rectangle<int>(x, y, w, fixedSectionHeight(s));
        y += fixedSectionHeight(s);
    }
    const bool showFaders = v.get(Section::Faders);
    const bool showMeters = v.get(Section::Meters);
    if (showFaders || showMeters)
    {
        y += kSectionGapPx;
        const int remaining = availableHeight - y - kStripPadPx;
        const int lowerH = juce::jmax(kLowerBandMinHeightPx, remaining);
        out.lowerBand = juce::Rectangle<int>(x, y, w, lowerH);
        // Fader column at the left, meter column at the right; each alone takes the whole band.
        const int gap = 8;
        if (showFaders && showMeters)
        {
            const int meterW = juce::jmin(52, juce::jmax(30, w / 3));
            out.bands[static_cast<size_t>(Section::Faders)] = out.lowerBand.withWidth(w - meterW - gap);
            out.bands[static_cast<size_t>(Section::Meters)] = out.lowerBand.withX(out.lowerBand.getRight() - meterW).withWidth(meterW);
        }
        else if (showFaders)
        {
            out.bands[static_cast<size_t>(Section::Faders)] = out.lowerBand;
        }
        else
        {
            out.bands[static_cast<size_t>(Section::Meters)] = out.lowerBand;
        }
        y += lowerH;
    }
    y += kStripPadPx;
    out.totalHeight = y;
    return out;
}

} // namespace mixer_layout
