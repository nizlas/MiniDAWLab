#pragma once

#include "io/ProjectFile.h"

#include <cstdint>
#include <functional>
#include <optional>

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

/// Conny 1B: internal MIDI editor view state persisted per project (window bounds live separately).
/// Horizontal roll pan/zoom/Follow are per-clip (`midiRoll*` fields) and not duplicated here.
struct MidiEditorWorkspaceUiState
{
    /// Topmost visible MIDI pitch (vertical pitch scroll); -1 = unknown.
    int topVisibleMidiPitch = -1;
    /// Velocity lane height in px; 0 = minimized, -1 = unknown.
    int velocityLaneHeight = -1;
    /// Rows mode: 1 = piano, 2 = drum names, 0 = unknown (keep auto default from kit).
    int rowLabelMode = 0;
};

/// I3h: non-owning handles + callbacks into `TransportControlsContent` — one global transport; no
/// duplicated `Transport` / `Session` / recorder state in the MIDI editor.
struct ExperimentalMidiTransportCommands
{
    Transport* transport = nullptr;
    std::function<void()> onTogglePlayPause;
    std::function<void()> onStop;
    std::function<void()> onToggleRecord;
    std::function<void()> onToggleCycle;
    /// Same as main window `invokeJumpToLeftLocatorFromWindowShortcut` (valid locator range + seek).
    std::function<void()> onJumpToLeftLocator;
    /// Same semantics as `TimelineRulerView`'s `isUiInputBlockedByRecording` (count-in + recording).
    std::function<bool()> isUiInputBlockedByRecording;
    /// Ctrl+S: same flow as File -> Save Project (known path saves directly, else Save As chooser).
    std::function<void()> onSaveProject;
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

    /// Match `Session` bars/beats vs seconds ruler mode (toolbar label + roll repaint); call when the
    /// session preference changes from the main window or another editor instance.
    void syncTimelineRulerFormatFromSession();

    /// Re-read global arrangement snap toggle + resolution from `Session` (shared with main window).
    void refreshArrangementSnapMirrorFromSession();

    /// Notify main window to refresh its snap toolbar after user edits here (`Session` already updated).
    void setArrangementSnapToolbarSyncHandler(std::function<void()> fn);

    /// Bind editor to a clip's pattern (address stable for clip lifetime). Pass session/transport/
    /// deviceManager for absolute-timeline roll + shared playhead/locators; `timelineClip` may be null
    /// only when the editor is unbound. `mainTimelineViewport` is reserved for future sync; default roll zoom uses ~5 bars from session tempo/meter when the clip has no saved viewport.
    void bindExternalPattern(ExperimentalMidiPattern* pattern,
                             InstrumentMidiClip* timelineClip,
                             InstrumentTrackController* instrumentTrackForClip,
                             Session* session,
                             Transport* transport,
                             juce::AudioDeviceManager* deviceManager,
                             const TimelineViewportModel* mainTimelineViewport,
                             const juce::String& titleSuffix);

    void unbindExternalPattern();

    /// I3h: wire Space / numpad-* / jump-to-L / toolbar to the same main-window transport paths.
    void bindTransportCommands(ExperimentalMidiTransportCommands commands);

    /// I3i: global `SessionHistory` integration for clip-bound edits (empty handlers = scratch editor).
    void setInstrumentMusicalUndoUi(
        std::function<void(const juce::String&, std::function<bool()>)> onUndoableEdit,
        std::function<void()> onUndoShortcut,
        std::function<void()> onRedoShortcut);

    /// After instrument musical undo applies, `InstrumentMidiClip` storage may be reallocated; rebind
    /// if the editor was clip-bound (returns nullopt for scratch mode).
    [[nodiscard]] std::optional<std::uint64_t> getBoundInstrumentClipId() const noexcept;

    /// After a seek from outside the MIDI roll (main shortcuts, stop-to-L, etc.): re-anchor roll UI.
    void notifyExternalTransportSeek(std::int64_t targetSample) noexcept;

    /// Persists piano-roll pan/zoom/Follow on the bound clip (call before project save and when closing).
    void snapshotOpenClipViewportFromRoll() noexcept;

    /// Current internal view state for project save (vertical pitch scroll, velocity lane, rows mode).
    [[nodiscard]] MidiEditorWorkspaceUiState captureWorkspaceUiState() const noexcept;

    /// Restore internal view state after `bindExternalPattern` (unknown fields are left unchanged).
    /// A restored rows mode is pinned as a user pick so async drum-label probes do not flip it back.
    void applyWorkspaceUiState(const MidiEditorWorkspaceUiState& s) noexcept;

    /// Transient "Saving project" indicator in this window (mirror of the main-window toast); shown
    /// by the save path when this editor is the active window.
    void showSavingProjectToast();

    bool keyPressed(const juce::KeyPress& key, juce::Component* originating) override;

private:
    class Body;

    ExperimentalInstrumentHost& host_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ExperimentalMidiEditorWindow)
};
