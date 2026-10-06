#pragma once

#include <JuceHeader.h>

#include <vector>

#include "ui/InspectorView.h" // InspectorPluginHost

class PluginInsertHost;
class TrackLanesView;
class Vst3PluginPickerCoordinator;

/// Installs `PluginInsertHost` callbacks on `TrackLanesView` headers and `InspectorView` (no ownership).
class PluginHostUiBindings final
{
public:
    struct Refs
    {
        PluginInsertHost& pluginHost;
        TrackLanesView& trackLanesView;
        InspectorView& inspectorView;
        Vst3PluginPickerCoordinator& vst3PluginPickerCoordinator;
        /// Passed to `showVst3PluginPickerForTrack` for track-header **Add post** (same as prior `this`).
        juce::Component& trackHeaderPluginPickerAnchor;
    };

    static void install(Refs refs);

    /// The TrackId + InsertSlotId insert seam (rows, add through the VST3 picker anchored at
    /// `pickerAnchor`, open editor, remove, move / reorder) — the Inspector installs it on
    /// itself and the mixer strips receive the same functions.
    [[nodiscard]] static InspectorPluginHost makeInsertSeam(PluginInsertHost& pluginHost,
                                                           Vst3PluginPickerCoordinator& picker,
                                                           juce::Component* pickerAnchor);
};
