#pragma once

// =============================================================================
// Plugin insert chain — serializable snapshots for undo/redo and project I/O
// =============================================================================
//
// ROLE
//   Value types for undo/redo and project load/save. Not live processors; `PluginInsertHost`
//   turns `PluginInsertDescriptor` rows into `juce::AudioPluginInstance` on the message thread.
//
// THREADING
//   [Message thread] only — `MemoryBlock` may be large; never passed through the audio callback.
// =============================================================================

#include "plugins/InsertSlotId.h"

#include "domain/Track.h"

#include <juce_core/juce_core.h>

#include <vector>

struct PluginInsertDescriptor
{
    InsertSlotId slotId = kInvalidInsertSlotId;
    InsertStage stage = InsertStage::Post;
    /// False: ignore path / identifier / state (should not appear in persisted chains).
    bool occupied = false;
    juce::String vst3AbsolutePath;
    juce::String pluginIdentifier;
    juce::MemoryBlock opaqueState;

    [[nodiscard]] bool descriptorEquals(const PluginInsertDescriptor& o) const noexcept
    {
        return slotId == o.slotId && stage == o.stage && occupied == o.occupied
               && vst3AbsolutePath == o.vst3AbsolutePath && pluginIdentifier == o.pluginIdentifier
               && opaqueState == o.opaqueState;
    }
};

struct PluginTrackChain
{
    /// Pre slots first, then Post slots — canonical processing order.
    std::vector<PluginInsertDescriptor> slots;

    [[nodiscard]] bool chainEquals(const PluginTrackChain& o) const noexcept
    {
        if (slots.size() != o.slots.size())
        {
            return false;
        }
        for (size_t i = 0; i < slots.size(); ++i)
        {
            if (!slots[i].descriptorEquals(o.slots[i]))
            {
                return false;
            }
        }
        return true;
    }
};

struct PluginUndoStepSides
{
    TrackId trackId = kInvalidTrackId;
    PluginTrackChain before{};
    PluginTrackChain after{};
};
