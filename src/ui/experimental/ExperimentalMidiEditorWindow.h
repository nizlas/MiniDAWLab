#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

class ExperimentalInstrumentHost;
struct ExperimentalMidiPattern;
class InstrumentTrackController;
struct InstrumentMidiClip;
class Session;
class Transport;

namespace juce
{
class AudioDeviceManager;
}

class ExperimentalMidiEditorWindow final : public juce::DocumentWindow
{
public:
    explicit ExperimentalMidiEditorWindow(ExperimentalInstrumentHost& host);
    ~ExperimentalMidiEditorWindow() override;

    void closeButtonPressed() override;

    /// Call from Main **before** `ExperimentalInstrumentHost::unloadInstrument()` so note-offs /
    /// all-notes-off still reach the plugin.
    void prepareInstrumentUnloadFromHost();

    /// Re-read `ExperimentalInstrumentHost` and refresh Play/Stop/labels (call after load/unload and
    /// whenever the MIDI roll is shown).
    void syncInstrumentStateFromHost();

    /// Bind editor to a clip's pattern (address stable for clip lifetime). Pass session/transport/
    /// deviceManager for absolute-timeline roll + shared playhead/locators; `timelineClip` may be null
    /// only for legacy detached use.
    void bindExternalPattern(ExperimentalMidiPattern* pattern,
                             InstrumentMidiClip* timelineClip,
                             InstrumentTrackController* instrumentTrackForClip,
                             Session* session,
                             Transport* transport,
                             juce::AudioDeviceManager* deviceManager,
                             const juce::String& titleSuffix);
    void unbindExternalPattern();

private:
    class Body;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ExperimentalMidiEditorWindow)
};
