#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

class ExperimentalInstrumentHost;

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

private:
    class Body;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ExperimentalMidiEditorWindow)
};
