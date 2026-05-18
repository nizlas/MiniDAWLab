#pragma once

// =============================================================================
// clearAndPopulateSnapResolutionComboBox — single source for main + MIDI editors
// =============================================================================

#include "ui/SnapSettings.h"

#include <juce_gui_basics/juce_gui_basics.h>

inline void clearAndPopulateSnapResolutionComboBox(juce::ComboBox& box)
{
    box.clear(juce::dontSendNotification);
    for (const SnapResolution r : kSnapResolutionComboOrder)
    {
        box.addItem(snapResolutionDisplayName(r), snapResolutionToComboItemId(r));
    }
}
