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
                            std::shared_ptr<const SessionSnapshot> after,
                            std::optional<PluginUndoStepSides> pluginSides) noexcept
{
    if (before == nullptr || after == nullptr)
    {
        return;
    }
    const bool pluginDelta = pluginSides.has_value() && pluginSides->trackId != kInvalidTrackId
                             && !pluginSides->before.slotEquals(pluginSides->after);
    if (!pluginDelta)
    {
        if (before.get() == after.get())
        {
            return;
        }
    }
    redo_.clear();
    undo_.push_back(Step{ std::move(label), std::move(before), std::move(after), std::move(pluginSides) });
    while (static_cast<int>(undo_.size()) > maxSteps_)
    {
        undo_.pop_front();
    }
}

std::optional<SessionHistoryRestoreBundle> SessionHistory::popUndo() noexcept
{
    if (undo_.empty())
    {
        return std::nullopt;
    }
    Step step = undo_.back();
    undo_.pop_back();
    redo_.push_back(step);
    SessionHistoryRestoreBundle bundle;
    bundle.timelineSnapshot = step.before;
    bundle.pluginSides = step.pluginSides;
    bundle.isRedo = false;
    return bundle;
}

std::optional<SessionHistoryRestoreBundle> SessionHistory::popRedo() noexcept
{
    if (redo_.empty())
    {
        return std::nullopt;
    }
    Step step = redo_.back();
    redo_.pop_back();
    undo_.push_back(step);
    SessionHistoryRestoreBundle bundle;
    bundle.timelineSnapshot = step.after;
    bundle.pluginSides = step.pluginSides;
    bundle.isRedo = true;
    return bundle;
}
