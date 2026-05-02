// =============================================================================
// SessionHistory.cpp — undo/redo deque bookkeeping (message thread only)
// =============================================================================

#include "domain/SessionHistory.h"

SessionHistory::SessionHistory(const int maxUndoSteps) noexcept
    : maxSteps_(juce::jmax(1, maxUndoSteps))
{
}

void SessionHistory::clear() noexcept
{
    undo_.clear();
    redo_.clear();
}

void SessionHistory::record(juce::String label,
                            std::shared_ptr<const SessionSnapshot> before,
                            std::shared_ptr<const SessionSnapshot> after) noexcept
{
    if (before == nullptr || after == nullptr)
    {
        return;
    }
    if (before.get() == after.get())
    {
        return;
    }
    redo_.clear();
    undo_.push_back(Step{std::move(label), std::move(before), std::move(after)});
    while (static_cast<int>(undo_.size()) > maxSteps_)
    {
        undo_.pop_front();
    }
}

std::optional<std::shared_ptr<const SessionSnapshot>> SessionHistory::popUndo() noexcept
{
    if (undo_.empty())
    {
        return std::nullopt;
    }
    Step step = undo_.back();
    undo_.pop_back();
    std::shared_ptr<const SessionSnapshot> toRestore = step.before;
    redo_.push_back(std::move(step));
    return toRestore;
}

std::optional<std::shared_ptr<const SessionSnapshot>> SessionHistory::popRedo() noexcept
{
    if (redo_.empty())
    {
        return std::nullopt;
    }
    Step step = redo_.back();
    redo_.pop_back();
    std::shared_ptr<const SessionSnapshot> toRestore = step.after;
    undo_.push_back(std::move(step));
    return toRestore;
}
