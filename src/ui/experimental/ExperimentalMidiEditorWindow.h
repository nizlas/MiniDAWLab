#pragma once

#include <functional>

#include <juce_gui_basics/juce_gui_basics.h>

class ExperimentalInstrumentHost;
struct ExperimentalMidiPattern;
class InstrumentTrackController;
struct InstrumentMidiClip;
class Session;
class Transport;
class TimelineViewportModel;

namespace juce
{
class AudioDeviceManager;
}

/// I3h: non-owning handles + callbacks into `TransportControlsContent` — one global transport; no
/// duplicated `Transport` / `Session` / recorder state in the MIDI editor.
struct ExperimentalMidiTransportCommands
{
    Transport* transport = nullptr;
    std::function<void()> onTogglePlayPause;
    std::function<void()> onStop;
    std::function<void()> onToggleRecord;
    std::function<void()> onToggleCycle;
    /// Same semantics as `TimelineRulerView`'s `isUiInputBlockedByRecording` (count-in + recording).
    std::function<bool()> isUiInputBlockedByRecording;
};

class ExperimentalMidiEditorWindow final : public juce::DocumentWindow, public juce::KeyListener
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
    /// only for legacy detached use. `mainTimelineViewport` seeds roll zoom when the clip has no saved viewport.
    void bindExternalPattern(ExperimentalMidiPattern* pattern,
                             InstrumentMidiClip* timelineClip,
                             InstrumentTrackController* instrumentTrackForClip,
                             Session* session,
                             Transport* transport,
                             juce::AudioDeviceManager* deviceManager,
                             const TimelineViewportModel* mainTimelineViewport,
                             const juce::String& titleSuffix);

    void unbindExternalPattern();

    /// I3h: wire Space / numpad-* / toolbar to the same main-window transport + record paths.
    void bindTransportCommands(ExperimentalMidiTransportCommands commands);

    /// Persists piano-roll pan/zoom/Follow on the bound clip (call before project save and when closing).
    void snapshotOpenClipViewportFromRoll() noexcept;

    bool keyPressed(const juce::KeyPress& key, juce::Component* originating) override;

private:
    class Body;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ExperimentalMidiEditorWindow)
};
