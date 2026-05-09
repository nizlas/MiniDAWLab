// =============================================================================
// SessionHistory.cpp — undo/redo deque bookkeeping (message thread only)
// =============================================================================

#include "domain/SessionHistory.h"

#include "diagnostics/UndoDiagnosticConfig.h"
#include "diagnostics/UndoDiagnosticFileLog.h"

namespace
{
    [[nodiscard]] juce::String ptrTag(const SessionSnapshot* p)
    {
        if (p == nullptr)
        {
            return "null";
        }
        return "0x" + juce::String::toHexString(reinterpret_cast<juce::pointer_sized_int>(p));
    }
}

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
        if constexpr (undo_diagnostic::kUndoDiag)
        {
            writeUndoDiagnosticLogLine("[UndoDiag] SessionHistory::record skip: null before/after label=\""
                                       + label + "\" before=" + ptrTag(before.get()) + " after="
                                       + ptrTag(after.get()));
        }
        return;
    }
    const bool pluginDelta = pluginSides.has_value() && pluginSides->trackId != kInvalidTrackId
                             && !pluginSides->before.chainEquals(pluginSides->after);
    if (!pluginDelta)
    {
        if (before.get() == after.get())
        {
            if constexpr (undo_diagnostic::kUndoDiag)
            {
                writeUndoDiagnosticLogLine(
                    "[UndoDiag] SessionHistory::record skip: identical ptr label=\"" + label
                    + "\" ptr=" + ptrTag(before.get()));
            }
            return;
        }
    }
    redo_.clear();
    undo_.push_back(Step{ std::move(label), std::move(before), std::move(after), std::move(pluginSides) });
    while (static_cast<int>(undo_.size()) > maxSteps_)
    {
        undo_.pop_front();
    }
    if constexpr (undo_diagnostic::kUndoDiag)
    {
        writeUndoDiagnosticLogLine("[UndoDiag] SessionHistory::record pushed label=\""
                                   + undo_.back().label + "\" undoSize=" + juce::String(undo_.size())
                                   + " redoCleared");
    }
}

std::optional<SessionHistoryRestoreBundle> SessionHistory::popUndo() noexcept
{
    if (undo_.empty())
    {
        if constexpr (undo_diagnostic::kUndoDiag)
        {
            writeUndoDiagnosticLogLine("[UndoDiag] SessionHistory::popUndo empty");
        }
        return std::nullopt;
    }
    Step step = undo_.back();
    undo_.pop_back();
    redo_.push_back(step);
    SessionHistoryRestoreBundle bundle;
    bundle.timelineSnapshot = step.before;
    bundle.pluginSides = step.pluginSides;
    bundle.isRedo = false;
    if constexpr (undo_diagnostic::kUndoDiag)
    {
        writeUndoDiagnosticLogLine(
            "[UndoDiag] SessionHistory::popUndo ok label=\"" + step.label + "\" timeline="
            + ptrTag(step.before.get()) + " undoSize=" + juce::String(static_cast<int>(undo_.size()))
            + " redoSize=" + juce::String(static_cast<int>(redo_.size())));
    }
    return bundle;
}

std::optional<SessionHistoryRestoreBundle> SessionHistory::popRedo() noexcept
{
    if (redo_.empty())
    {
        if constexpr (undo_diagnostic::kUndoDiag)
        {
            writeUndoDiagnosticLogLine("[UndoDiag] SessionHistory::popRedo empty");
        }
        return std::nullopt;
    }
    Step step = redo_.back();
    redo_.pop_back();
    undo_.push_back(step);
    SessionHistoryRestoreBundle bundle;
    bundle.timelineSnapshot = step.after;
    bundle.pluginSides = step.pluginSides;
    bundle.isRedo = true;
    if constexpr (undo_diagnostic::kUndoDiag)
    {
        writeUndoDiagnosticLogLine(
            "[UndoDiag] SessionHistory::popRedo ok label=\"" + step.label + "\" timeline="
            + ptrTag(step.after.get()) + " undoSize=" + juce::String(static_cast<int>(undo_.size()))
            + " redoSize=" + juce::String(static_cast<int>(redo_.size())));
    }
    return bundle;
}
