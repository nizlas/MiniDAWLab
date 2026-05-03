#pragma once

// =============================================================================
// PluginTrackSlot — serializable snapshot of one track’s VST3 insert slot (Phase 8)
// =============================================================================
//
// ROLE
//   Value type for undo/redo and project load/save. Not a live processor; `PluginInsertHost`
//   turns this into `juce::AudioPluginInstance` on the message thread.
//
// THREADING
//   [Message thread] only — `MemoryBlock` may be large; never passed through the audio callback.
// =============================================================================

#include "domain/Track.h"

#include <juce_core/juce_core.h>

struct PluginTrackSlot
{
    /// False: lane has no insert; ignore path / identifier / state.
    bool occupied = false;
    /// Absolute path to the `.vst3` bundle on disk (Windows / macOS).
    juce::String vst3AbsolutePath;
    /// JUCE `PluginDescription::createIdentifierString()` for mismatch detection on load.
    juce::String pluginIdentifier;
    /// `AudioProcessor::getStateInformation` blob (opaque to the host).
    juce::MemoryBlock opaqueState;

    [[nodiscard]] bool slotEquals(const PluginTrackSlot& o) const noexcept
    {
        return occupied == o.occupied && vst3AbsolutePath == o.vst3AbsolutePath
               && pluginIdentifier == o.pluginIdentifier && opaqueState == o.opaqueState;
    }
};

struct PluginUndoStepSides
{
    TrackId trackId = kInvalidTrackId;
    PluginTrackSlot before{};
    PluginTrackSlot after{};
};
