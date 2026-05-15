#pragma once

#include <JuceHeader.h>

#include <vector>

class PluginInsertHost;
class TrackLanesView;
class InspectorView;
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
};
