#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

class ExperimentalInstrumentHost;

class ExperimentalMidiEditorWindow final : public juce::DocumentWindow
{
public:
    explicit ExperimentalMidiEditorWindow(ExperimentalInstrumentHost& host);
    ~ExperimentalMidiEditorWindow() override;

    void closeButtonPressed() override;
    void notifyInstrumentUnloaded();

private:
    class Body;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ExperimentalMidiEditorWindow)
};
