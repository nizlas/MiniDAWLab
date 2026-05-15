#pragma once

#include <JuceHeader.h>

#include <atomic>
#include <functional>

#include "domain/Track.h"

class Session;
class PluginInsertHost;
class ExperimentalInstrumentHost;
class InstrumentTrackController;

/// Message-thread VST3 discovery menu, folder/file choosers, and experimental OOP plugin-description rescan.
/// Does not own `Session`, inserts, or instrument runtimes — uses `PluginInsertHost` and callbacks only.
class Vst3PluginPickerCoordinator final : public juce::Component
{
public:
    enum class InsertPickerMode
    {
        AddPre,
        AddPost,
    };

    struct Callbacks
    {
        std::function<bool()> isUiInputBlockedByRecording;
        std::function<void()> refreshInspectorFromSession;
        std::function<void(TrackId)> refreshInstrumentUiForTrack;
        std::function<void(TrackId)> updatePlaybackBridgeAfterRegistryChange;
        std::function<std::pair<ExperimentalInstrumentHost*, InstrumentTrackController*>(TrackId)>
            getOrCreateInstrumentRuntimeForTrack;

        /// Used by the out-of-process description rescan path (host lookup; no allocation).
        std::function<ExperimentalInstrumentHost*(TrackId)> getInstrumentHostForTrack;

        /// Mirrors `TransportControlsContent::refreshInstrumentUi` after rescan.
        std::function<void()> refreshInstrumentUi;

        std::function<TrackId()> getCanonicalInstrumentLaneTrackIdFromSession;
    };

    Vst3PluginPickerCoordinator(juce::Component& ownerUi,
                                Session& session,
                                PluginInsertHost& pluginHost,
                                Callbacks callbacks);

    ~Vst3PluginPickerCoordinator() override;

    void showVst3PluginPickerForTrack(TrackId trackId,
                                      InsertPickerMode mode,
                                      juce::Component* anchor);

    void beginAddVst3FolderForTrack(TrackId trackId, juce::Component* anchor, InsertPickerMode mode);
    void beginLoadVst3ForTrack(TrackId trackId, InsertPickerMode mode);

    void runExperimentalInstrumentPluginDescriptionRescan();
    void runExperimentalInstrumentPluginDescriptionRescanForTrack(TrackId tid);

private:
    juce::Component& ownerUi_;
    Session& session_;
    PluginInsertHost& pluginHost_;
    Callbacks callbacks_;

    bool vst3ChooserInFlight_ = false;
    bool vst3FolderChooserInFlight_ = false;
    std::atomic<bool> experimentalOopScanBusy_{ false };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(Vst3PluginPickerCoordinator)
};
