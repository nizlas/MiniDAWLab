#pragma once

// =============================================================================
// MixerSectionLayout — the one vertical layout every mixer channel strip follows
// =============================================================================
//
// ROLE
//   The mixer's sections (Routing, Pre-gain, Pre inserts, Post inserts, Sends, Faders, Meters)
//   can each be shown or minimised, GLOBALLY for all strips, and the upper sections have one
//   shared, user-draggable height each (`SectionHeights`). This header turns the visibility
//   flags, the heights and the available strip height into one list of section bands (y
//   ranges) that every strip — the scrolling ones and the fixed Stereo Out — applies verbatim,
//   so a row's own content (fewer routing rows, no inserts, no sends) can never move its fader
//   relative to the neighbours. It also describes the DIVIDERS between consecutive visible bands
//   and how a drag on one of them redistributes height. Pure geometry; no components, no session.
//
// SIZES (logical px)
//   Strip width 150 (inside the requested 140–160 window). Upper sections have a minimum that
//   keeps their controls reachable (Routing: all four selector rows; Pre-gain: its row; Sends:
//   the four slots; inserts: caption + one row + the add button — the list scrolls) and a stored
//   desired height (defaults below). The lower band (pan + fader at the left, output meter at the
//   right) takes whatever height remains, never less than `kLowerBandMinHeightPx` — the common
//   vertical scroll bar appears when the window is lower than the stack.
// =============================================================================

#include <array>
#include <vector>
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

/// The sections stacked above the lower band, in order (Faders / Meters share the lower band).
[[nodiscard]] inline constexpr bool isUpperSection(const Section s) noexcept
{
    return s == Section::Routing || s == Section::PreGain || s == Section::PreInserts || s == Section::PostInserts || s == Section::Sends;
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
/// Widened 150 → 174 by the Solo slice: the instrument strip's base-button row grew to seven
/// cells ([Instrument][Power][M][S][Monitor][R][Alternatives] = 7×20 + 6×3 px) which, plus the
/// 4 px pads and the 6 px stripe inset, needs ≥172 px. Every band derives from this constant.
inline constexpr int kStripWidthPx = 174;
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

/// Inserts (per stage): caption, a SCROLLING list of rows (every insert of the chain), one add
/// row outside the list. The band height decides how many rows are visible.
inline constexpr int kInsertRowHeightPx = 18;
inline constexpr int kInsertAddRowHeightPx = 18;
inline constexpr int kInsertsFixedChromeHeightPx = kSectionCaptionHeightPx + kInsertAddRowHeightPx + 2;
/// Minimum: caption + one row + add. Default: two rows visible.
inline constexpr int kInsertsMinSectionHeightPx = kInsertsFixedChromeHeightPx + kInsertRowHeightPx;
inline constexpr int kInsertsDefaultSectionHeightPx = kInsertsFixedChromeHeightPx + 2 * kInsertRowHeightPx;

/// Sends: caption + four one-line rows (enable | destination | amount).
inline constexpr int kSendRows = 4;
inline constexpr int kSendRowHeightPx = 20;
inline constexpr int kSendsSectionHeightPx = kSectionCaptionHeightPx + kSendRows * kSendRowHeightPx + 2;

/// Lower band: "Pan" caption + the Inspector's pan field (36 px, identical component and size)
/// above the fader; the meter shares the band on the right. The minimum keeps ≥ 118 px of fader.
inline constexpr int kPanCaptionHeightPx = kSectionCaptionHeightPx;
inline constexpr int kPanFieldHeightPx = 36;
inline constexpr int kPanRowHeightPx = kPanCaptionHeightPx + kPanFieldHeightPx;
inline constexpr int kFaderMinHeightPx = 118;
inline constexpr int kLowerBandMinHeightPx = kPanRowHeightPx + 2 + kFaderMinHeightPx;

/// Upper bound for a stored / dragged section height (sanity clamp, not a design limit).
inline constexpr int kSectionHeightMaxPx = 2000;

/// Minimum height that keeps a section's controls reachable.
[[nodiscard]] inline constexpr int minimumSectionHeight(const Section s) noexcept
{
    switch (s)
    {
    case Section::Routing: return kRoutingSectionHeightPx;
    case Section::PreGain: return kPreGainSectionHeightPx;
    case Section::PreInserts:
    case Section::PostInserts: return kInsertsMinSectionHeightPx;
    case Section::Sends: return kSendsSectionHeightPx;
    case Section::Faders:
    case Section::Meters:
    case Section::Count: break;
    }
    return 0;
}

/// Default (first-run) height of an upper section.
[[nodiscard]] inline constexpr int defaultSectionHeight(const Section s) noexcept
{
    switch (s)
    {
    case Section::PreInserts:
    case Section::PostInserts: return kInsertsDefaultSectionHeightPx;
    default: return minimumSectionHeight(s);
    }
}

/// Clamp a stored / dragged height to the usable range of a section.
[[nodiscard]] inline constexpr int clampSectionHeight(const Section s, const int px) noexcept
{
    const int lo = minimumSectionHeight(s);
    return px < lo ? lo : (px > kSectionHeightMaxPx ? kSectionHeightMaxPx : px);
}

/// The user's desired heights of the upper sections (shared by every strip; persisted).
struct SectionHeights
{
    std::array<int, kSectionCount> px{};

    SectionHeights() noexcept
    {
        for (int i = 0; i < kSectionCount; ++i)
        {
            px[static_cast<size_t>(i)] = defaultSectionHeight(static_cast<Section>(i));
        }
    }
    [[nodiscard]] int get(const Section s) const noexcept { return px[static_cast<size_t>(s)]; }
    void set(const Section s, const int v) noexcept { px[static_cast<size_t>(s)] = isUpperSection(s) ? clampSectionHeight(s, v) : 0; }
    [[nodiscard]] bool operator==(const SectionHeights& o) const noexcept { return px == o.px; }
    [[nodiscard]] bool operator!=(const SectionHeights& o) const noexcept { return !(*this == o); }
};

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

/// Height the fixed part of the stack needs (header + visible upper sections) for the given
/// visibility and heights, without the lower band.
[[nodiscard]] inline int fixedStackHeight(const SectionVisibility& v, const SectionHeights& h) noexcept
{
    int total = kStripPadPx + kHeaderHeightPx;
    for (int i = 0; i < kSectionCount; ++i)
    {
        const Section s = static_cast<Section>(i);
        if (!isUpperSection(s) || !v.get(s))
        {
            continue;
        }
        total += kSectionGapPx + clampSectionHeight(s, h.get(s));
    }
    return total;
}

/// Minimum strip height (lower band at its minimum when any of Faders / Meters is shown).
[[nodiscard]] inline int minimumStripHeight(const SectionVisibility& v, const SectionHeights& h) noexcept
{
    const bool lower = v.get(Section::Faders) || v.get(Section::Meters);
    return fixedStackHeight(v, h) + (lower ? kSectionGapPx + kLowerBandMinHeightPx : 0) + kStripPadPx;
}

/// Lay the sections out in `availableHeight` (the strip's height). When the window is lower
/// than `minimumStripHeight`, the result is taller than `availableHeight` and the owner scrolls.
[[nodiscard]] inline ComputedLayout computeLayout(const SectionVisibility& v, const SectionHeights& h, const int stripWidth, const int availableHeight) noexcept
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
        if (!isUpperSection(s))
        {
            continue;
        }
        if (!v.get(s))
        {
            out.bands[static_cast<size_t>(i)] = {};
            continue;
        }
        y += kSectionGapPx;
        const int bandH = clampSectionHeight(s, h.get(s));
        out.bands[static_cast<size_t>(i)] = juce::Rectangle<int>(x, y, w, bandH);
        y += bandH;
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

/// Backwards-compatible overload: default heights.
[[nodiscard]] inline ComputedLayout computeLayout(const SectionVisibility& v, const int stripWidth, const int availableHeight) noexcept
{
    return computeLayout(v, SectionHeights{}, stripWidth, availableHeight);
}
[[nodiscard]] inline int minimumStripHeight(const SectionVisibility& v) noexcept
{
    return minimumStripHeight(v, SectionHeights{});
}

// --- dividers -----------------------------------------------------------------------------------

/// One draggable divider: the gap between two consecutive VISIBLE bands. `below == Section::Count`
/// means the lower (fader / meter) band.
struct Divider
{
    Section above = Section::Routing;
    Section below = Section::Count;
    /// y of the thin line in strip coordinates (centre of the gap).
    int lineY = 0;
};

/// The dividers of a layout, top to bottom. Hidden sections never produce a divider; the last
/// one separates the last visible upper band from the lower band when that band is shown.
[[nodiscard]] inline std::vector<Divider> dividersFor(const ComputedLayout& layout, const SectionVisibility& v)
{
    std::vector<Divider> out;
    Section previous = Section::Count;
    for (int i = 0; i < kSectionCount; ++i)
    {
        const Section s = static_cast<Section>(i);
        if (!isUpperSection(s) || !v.get(s))
        {
            continue;
        }
        if (previous != Section::Count)
        {
            out.push_back({ previous, s, layout.band(s).getY() - kSectionGapPx / 2 });
        }
        previous = s;
    }
    if (previous != Section::Count && !layout.lowerBand.isEmpty())
    {
        out.push_back({ previous, Section::Count, layout.lowerBand.getY() - kSectionGapPx / 2 });
    }
    return out;
}

/// Apply a drag of `deltaY` px on `divider` (positive = down): height moves between the two
/// adjacent bands so the total stays put. Against the lower band the upper section can grow only
/// into the lower band's spare height (never into a scroll) and shrink down to its minimum.
[[nodiscard]] inline SectionHeights applyDividerDrag(const SectionHeights& heights,
                                                     const SectionVisibility& v,
                                                     const Divider& divider,
                                                     const int deltaY,
                                                     const int availableHeight) noexcept
{
    SectionHeights next = heights;
    if (!isUpperSection(divider.above) || !v.get(divider.above))
    {
        return next;
    }
    const int above = clampSectionHeight(divider.above, heights.get(divider.above));
    if (divider.below != Section::Count)
    {
        if (!isUpperSection(divider.below) || !v.get(divider.below))
        {
            return next;
        }
        const int below = clampSectionHeight(divider.below, heights.get(divider.below));
        const int total = above + below;
        const int minAbove = minimumSectionHeight(divider.above);
        const int maxAbove = total - minimumSectionHeight(divider.below);
        if (maxAbove < minAbove)
        {
            return next; // both already at their minimum: nothing to redistribute
        }
        const int newAbove = juce::jlimit(minAbove, maxAbove, above + deltaY);
        next.set(divider.above, newAbove);
        next.set(divider.below, total - newAbove);
        return next;
    }
    // Against the lower band.
    const ComputedLayout current = computeLayout(v, heights, kStripWidthPx, availableHeight);
    const int spare = juce::jmax(0, current.lowerBand.getHeight() - kLowerBandMinHeightPx);
    const int minAbove = minimumSectionHeight(divider.above);
    const int maxAbove = above + spare;
    next.set(divider.above, juce::jlimit(minAbove, juce::jmax(minAbove, maxAbove), above + deltaY));
    return next;
}

} // namespace mixer_layout
