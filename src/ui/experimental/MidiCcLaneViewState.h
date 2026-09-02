// =============================================================================
// MidiCcLaneViewState.h
//
// Pure, deterministic view-state decisions for the MIDI editor's collapsible CC
// automation lane. This is runtime UI state only: it never touches CC points,
// undo history, dirty state, playback, chase or MIDI export. Kept header-only
// and JUCE-free so MiniDAWSelftests can verify collapse/expand behavior without
// instantiating the piano-roll component.
// =============================================================================

#pragma once

namespace midi_cc_lane
{

/// Runtime-only lane view state. heightPref == 0 means collapsed (the initial
/// state); expandedMemo remembers the last expanded height so reopening
/// restores the user's lane size within the same editor window.
struct ViewState
{
    int heightPref = 0;
    int expandedMemo = 0;
};

/// Effective on-screen lane height for a preferred height, clamped exactly like
/// the piano roll clamps it: never below 0, never eating the minimum grid area,
/// never more than half the view.
[[nodiscard]] constexpr int effectiveLaneHeight(const int heightPref,
                                                const int viewHeight,
                                                const int rulerHeight,
                                                const int minGridPx) noexcept
{
    const int available = viewHeight - rulerHeight - minGridPx;
    const int halfView = viewHeight / 2;
    int hi = available < halfView ? available : halfView;
    if (hi < 0)
    {
        hi = 0;
    }
    if (heightPref < 0)
    {
        return 0;
    }
    return heightPref > hi ? hi : heightPref;
}

/// Collapse the lane (view-only). Remembers the current expanded height for
/// reopen; a no-op when already collapsed.
[[nodiscard]] constexpr ViewState collapsed(const ViewState s) noexcept
{
    if (s.heightPref <= 0)
    {
        return s;
    }
    return { 0, s.heightPref };
}

/// Reopen the lane at the remembered runtime height, falling back to the given
/// default; a no-op when already expanded.
[[nodiscard]] constexpr ViewState reopened(const ViewState s, const int defaultHeight) noexcept
{
    if (s.heightPref > 0)
    {
        return s;
    }
    return { s.expandedMemo > 0 ? s.expandedMemo : defaultHeight, s.expandedMemo };
}

} // namespace midi_cc_lane
