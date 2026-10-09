#pragma once

// =============================================================================
// ScopedPluginStateCaptureWindow — RAII plugin-state capture window for project writes
// =============================================================================
// Brackets every project write that serializes plugin state (Save, Save As, autosave) with the
// engine's plugin-state capture window (docs/READAHEAD_PROTOTYPE.md §9). A no-op success when
// the experimental read-ahead is not enabled.
//
// CONTRACT: `succeeded()` must gate the write. When it is false the window could NOT be
// established (owned rows did not drain — e.g. no audio callbacks running while the transport
// claims Playing — or the worker never acknowledged its pause): plugin state must NOT be
// captured and the project file must NOT be written as if the save were safe. The destructor
// closes the window only when it was actually opened, so a failed window leaves no stuck
// adoption hold and no stuck worker pause behind.
// =============================================================================

#include "engine/PlaybackEngine.h"

class ScopedPluginStateCaptureWindow final
{
public:
    explicit ScopedPluginStateCaptureWindow(PlaybackEngine& playbackEngine) noexcept
        : playbackEngine_(playbackEngine)
        , ok_(playbackEngine.beginPluginStateCaptureWindow())
    {
    }
    ~ScopedPluginStateCaptureWindow() noexcept
    {
        if (ok_)
        {
            playbackEngine_.endPluginStateCaptureWindow();
        }
    }
    /// True only when the window is really established (drained + acknowledged worker pause,
    /// or the trivial no-read-ahead case). False = do not capture state, do not write the file.
    [[nodiscard]] bool succeeded() const noexcept { return ok_; }

    ScopedPluginStateCaptureWindow(const ScopedPluginStateCaptureWindow&) = delete;
    ScopedPluginStateCaptureWindow& operator=(const ScopedPluginStateCaptureWindow&) = delete;

private:
    PlaybackEngine& playbackEngine_;
    const bool ok_;
};
