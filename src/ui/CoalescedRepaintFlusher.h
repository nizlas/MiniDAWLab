#pragma once

// =============================================================================
// CoalescedRepaintFlusher — one repaint flush per message batch (message thread)
// =============================================================================
// Repaint-storm fix (see MAIN_FOLLOW_ZOOM_UI_FREEZE_FORENSIC_AUDIT.md): marking components dirty
// on *every* wheel/drag viewport event lets the OS/JUCE interleave full window recompositions with
// the queued input events — a fast zoom gesture then costs one full repaint per event and the
// message thread saturates for many seconds. Owners route per-event repaint requests through
// `triggerAsyncUpdate()` instead: the viewport *state* is applied immediately (event handling and
// geometry stay exact), but the dirty-marking runs once per queued batch, so N buffered events
// collapse into ~one full recomposition.
//
// `juce::AsyncUpdater` already coalesces multiple triggers into one callback per message-loop
// turn; this wrapper just binds it to a flush closure and cancels on destruction.
// =============================================================================

#include <juce_events/juce_events.h>

#include <functional>
#include <utility>

class CoalescedRepaintFlusher final : private juce::AsyncUpdater
{
public:
    explicit CoalescedRepaintFlusher(std::function<void()> flush) noexcept
        : flush_(std::move(flush))
    {
    }

    ~CoalescedRepaintFlusher() override { cancelPendingUpdate(); }

    /// Request a flush; any number of requests within one message batch produce one flush.
    void requestFlush() { triggerAsyncUpdate(); }

    /// Run a pending flush immediately (used when the caller knows the current turn will paint
    /// anyway, e.g. a follow page whose overlay already invalidated this frame).
    void flushNowIfPending() { handleUpdateNowIfNeeded(); }

private:
    void handleAsyncUpdate() override
    {
        if (flush_ != nullptr)
        {
            flush_();
        }
    }

    std::function<void()> flush_;
};
