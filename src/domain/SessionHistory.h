#pragma once

// =============================================================================
// SessionHistory — message-thread undo/redo stack of SessionSnapshot pairs (Undo-1)
// =============================================================================
//
// ROLE
//   Stores discrete edit steps as (`before`, `after`) snapshot pointers. Undo restores `before`;
//   redo restores `after`. Never touches disk, Transport, or the audio thread — callers invoke
//   `Session::restoreSessionSnapshotForUndo` on the message thread only.
//
// PHASE 8
//   A step may optionally include a **plugin slot** before/after for one `TrackId`. Plugin-only
//   edits use the **same** snapshot pointer for `before` and `after` with a non-empty plugin delta
//   (`pluginSides.has_value()`). Apply order on undo: restore timeline snapshot, then
//   `PluginInsertHost::importSlot(trackId, pluginSides->before)`.
//
// RECORD
//   `record` clears the redo deque. Steps are dropped from the front when over capacity.
//
// NO-OP / CHANGE DETECTION
//   Without a plugin delta: ignores if `before.get() == after.get()`. With a plugin delta: records
//   when `before` and `after` slot snapshots differ; timeline pointers may match.
// =============================================================================

#include "domain/SessionSnapshot.h"
#include "plugins/PluginTrackSlot.h"

#include <juce_core/juce_core.h>

#include <deque>
#include <memory>
#include <optional>

struct SessionHistoryRestoreBundle
{
    /// Always non-null when `popUndo` / `popRedo` returns has_value.
    std::shared_ptr<const SessionSnapshot> timelineSnapshot;
    /// When set, restore this plugin slot **after** applying `timelineSnapshot`.
    std::optional<PluginUndoStepSides> pluginSides {};
    /// True: popped from redo stack (apply `pluginSides->after`), false: undo (apply `before`).
    bool isRedo = false;
};

class SessionHistory
{
public:
    explicit SessionHistory(int maxUndoSteps = 100) noexcept;

    void clear() noexcept;

    /// [Message thread] Pushes one undo step and clears redo. Without plugin delta: no-op if either
    /// snapshot pointer is null or both refer to the same instance. With plugin delta: requires
    /// valid `pluginSides->trackId` and allows identical snapshot pointers when slots differ.
    void record(juce::String label,
                std::shared_ptr<const SessionSnapshot> before,
                std::shared_ptr<const SessionSnapshot> after,
                std::optional<PluginUndoStepSides> pluginSides = std::nullopt) noexcept;

    /// [Message thread] Pops one undo step onto redo; returns bundle with timeline + optional plugin
    /// restore (`pluginSides` present — caller applies `before` slot).
    [[nodiscard]] std::optional<SessionHistoryRestoreBundle> popUndo() noexcept;

    /// [Message thread] Pops one redo step back onto undo; returns bundle (`pluginSides->after` if
    /// present).
    [[nodiscard]] std::optional<SessionHistoryRestoreBundle> popRedo() noexcept;

private:
    struct Step
    {
        juce::String label;
        std::shared_ptr<const SessionSnapshot> before;
        std::shared_ptr<const SessionSnapshot> after;
        std::optional<PluginUndoStepSides> pluginSides;
    };

    int maxSteps_;
    std::deque<Step> undo_;
    std::deque<Step> redo_;
};
