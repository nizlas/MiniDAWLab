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
// RECORD
//   `record` clears the redo deque. Steps are dropped from the front when over capacity.
//
// NO-OP / CHANGE DETECTION
//   Some `Session` mutators always allocate a new snapshot shared_ptr even when the edit was a
//   semantic no-op, so pointer-identity alone is insufficient. Callers must only invoke `record`
//   after a mutator that returns success / is pre-validated (see `Main.cpp` `executeUndoableSessionEdit`).
//   Additionally, `record` ignores `before.get() == after.get()` as a cheap safety net.
// =============================================================================

#include "domain/SessionSnapshot.h"

#include <juce_core/juce_core.h>

#include <deque>
#include <memory>
#include <optional>

class SessionHistory
{
public:
    explicit SessionHistory(int maxUndoSteps = 100) noexcept;

    void clear() noexcept;

    /// [Message thread] Pushes one undo step and clears redo. No-op if either pointer is null or
    /// both refer to the same snapshot instance.
    void record(juce::String label,
                std::shared_ptr<const SessionSnapshot> before,
                std::shared_ptr<const SessionSnapshot> after) noexcept;

    /// [Message thread] Pops one undo step, pushes it onto redo, returns the snapshot to restore
    /// (`before` of that step). Empty if nothing to undo.
    [[nodiscard]] std::optional<std::shared_ptr<const SessionSnapshot>> popUndo() noexcept;

    /// [Message thread] Pops one redo step, pushes it back onto undo, returns the snapshot to
    /// restore (`after` of that step). Empty if nothing to redo.
    [[nodiscard]] std::optional<std::shared_ptr<const SessionSnapshot>> popRedo() noexcept;

private:
    struct Step
    {
        juce::String label;
        std::shared_ptr<const SessionSnapshot> before;
        std::shared_ptr<const SessionSnapshot> after;
    };

    int maxSteps_;
    std::deque<Step> undo_;
    std::deque<Step> redo_;
};
