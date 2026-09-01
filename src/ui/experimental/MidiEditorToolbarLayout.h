#pragma once

// =============================================================================
// MidiEditorToolbarLayout — pure MIDI-editor toolbar layout/visibility decisions
// =============================================================================
//
// The shared MIDI-editor window (drum, melodic instrument and MIDI-only editing) lays its toolbar
// out as a PER-CONTROL flow: all controls keep one stable logical left-to-right order and are
// placed sequentially; a control whose complete preferred width no longer fits on the current row
// starts the next row. Controls therefore move between rows individually — never as batches — and
// widening the window returns them one at a time in reverse. The decisions live here, std-only
// and header-only, so `MiniDAWSelftests` can verify the wrap boundaries and the `Fit Drums`
// visibility contract without a UI.
//
//   * conceptual-group spacing is metadata (`gapBefore`) that travels with the control after it
//     and is dropped at a row start — a group is never an indivisible layout unit;
//   * hidden controls (e.g. a contextually unavailable `Fit Drums`) consume no width and no gap;
//   * the row plan is a pure function of the available width and the visible item widths, so the
//     layout cannot oscillate around a breakpoint (no feedback from rows back into widths);
//   * one or two rows is the intended normal range; `minimumWidthForRows(items, 2)` derives the
//     editor's minimum client width from the actual visible control widths and spacing (no
//     resolution-specific constant) so the second row never clips instead of wrapping further.
//
// THREADING: pure functions on caller state; used from the [Message thread] only.
// =============================================================================

#include <vector>

namespace midi_editor_toolbar
{
    /// One toolbar row's height in px (the pre-existing single-row toolbar height).
    inline constexpr int kRowHeightPx = 40;

    /// Intended normal maximum rows; two rows is the normal narrow-layout solution (enforced via
    /// the computed minimum width rather than by clipping).
    inline constexpr int kMaxRows = 2;

    /// Horizontal padding added on each side of a text button's rendered label so the complete
    /// word is always visible (e.g. the `Remap` channel menu button).
    inline constexpr int kTextButtonPadPx = 10;

    /// Preferred width for a toolbar text button whose rendered label is `textWidthPx` wide.
    [[nodiscard]] inline int textButtonPreferredWidth(const int textWidthPx,
                                                      const int minimumPx = 0) noexcept
    {
        const int w = textWidthPx + 2 * kTextButtonPadPx;
        return w < minimumPx ? minimumPx : w;
    }

    /// One toolbar control in the stable logical order.
    struct ToolbarItem
    {
        /// Complete preferred width in px (a control is never rendered narrower than this).
        int width = 0;
        /// Conceptual-group spacing before this control; dropped when the control starts a row.
        int gapBefore = 0;
        /// Hidden controls consume neither width nor spacing.
        bool visible = true;
    };

    /// Where the flow placed one item. Hidden items get `placed == false` (row/x meaningless).
    struct ItemPlacement
    {
        int row = 0;
        int x = 0;
        int width = 0;
        bool placed = false;
    };

    /// Sequential per-control flow (see file header): place each visible item on the current row
    /// when its complete width fits, else start the next row with it. Order is never changed. An
    /// item wider than `availableWidthPx` still gets a row of its own (callers prevent this state
    /// via `minimumWidthForRows`). Row count is NOT clamped here — the caller derives the toolbar
    /// height from the actual rows and enforces the minimum width separately.
    [[nodiscard]] inline std::vector<ItemPlacement> flowLayout(const int availableWidthPx,
                                                               const std::vector<ToolbarItem>& items)
    {
        std::vector<ItemPlacement> out(items.size());
        int row = 0;
        int x = 0;
        for (size_t i = 0; i < items.size(); ++i)
        {
            const auto& it = items[i];
            if (!it.visible || it.width <= 0)
            {
                continue;
            }
            int gap = x > 0 ? it.gapBefore : 0;
            if (x > 0 && x + gap + it.width > availableWidthPx)
            {
                ++row;
                x = 0;
                gap = 0;
            }
            out[i].row = row;
            out[i].x = x + gap;
            out[i].width = it.width;
            out[i].placed = true;
            x += gap + it.width;
        }
        return out;
    }

    /// Number of rows a flow occupies (>= 1 even when nothing is visible).
    [[nodiscard]] inline int rowCountOf(const std::vector<ItemPlacement>& placements) noexcept
    {
        int maxRow = 0;
        for (const auto& p : placements)
        {
            if (p.placed && p.row > maxRow)
            {
                maxRow = p.row;
            }
        }
        return maxRow + 1;
    }

    /// Total toolbar height for an actual row count (the grid below moves down by the same amount).
    [[nodiscard]] inline int toolbarHeightForRows(const int rows) noexcept
    {
        return (rows < 1 ? 1 : rows) * kRowHeightPx;
    }

    /// The authoritative editor content height below the toolbar (ruler + note grid + velocity
    /// lane + collapsed CC strip live inside this rectangle, bottom-anchored internally). It is a
    /// pure function of the editor height and the FINAL toolbar row count only — editor width can
    /// never leak into vertical geometry, so two widths with the same row count partition the
    /// height identically: toolbarHeightForRows(rows) + editorContentHeight(...) == editorHeight
    /// with no remainder.
    [[nodiscard]] inline int editorContentHeight(const int editorHeightPx,
                                                 const int toolbarRows) noexcept
    {
        const int h = editorHeightPx - toolbarHeightForRows(toolbarRows);
        return h < 1 ? 1 : h;
    }

    /// Smallest available width whose flow fits `maxRows` rows, derived from the visible item
    /// widths and spacing only. Greedy flow row count is monotonically non-increasing in width,
    /// so a binary search between the widest single item and the everything-on-one-row total
    /// finds the exact boundary.
    [[nodiscard]] inline int minimumWidthForRows(const std::vector<ToolbarItem>& items,
                                                 const int maxRows) noexcept
    {
        int widest = 1;
        int total = 0;
        bool first = true;
        for (const auto& it : items)
        {
            if (!it.visible || it.width <= 0)
            {
                continue;
            }
            widest = it.width > widest ? it.width : widest;
            total += (first ? 0 : it.gapBefore) + it.width;
            first = false;
        }
        int lo = widest;
        int hi = total > widest ? total : widest;
        while (lo < hi)
        {
            const int mid = lo + (hi - lo) / 2;
            if (rowCountOf(flowLayout(mid, items)) <= maxRows)
            {
                hi = mid;
            }
            else
            {
                lo = mid + 1;
            }
        }
        return lo;
    }

    /// `Fit Drums` context gate: visible only in an explicit drum-instrument editor context —
    /// an instrument-track editor whose bound track carries DAL's explicit drum classification
    /// (effective drum labels, manual or plugin-discovered). Hidden for melodic instrument
    /// tracks and for MIDI-only tracks regardless of labels; never inferred from track or
    /// plugin names.
    [[nodiscard]] constexpr bool fitDrumsVisible(const bool isMidiOnlyTrack,
                                                 const bool hasExplicitDrumClassification) noexcept
    {
        return !isMidiOnlyTrack && hasExplicitDrumClassification;
    }

    /// `Fit Drums` enablement: a visible button is enabled when its normal operation is
    /// available (a useful drum row range exists to fit the editor to).
    [[nodiscard]] constexpr bool fitDrumsEnabled(const bool visible,
                                                 const bool fitRangeAvailable) noexcept
    {
        return visible && fitRangeAvailable;
    }
} // namespace midi_editor_toolbar
