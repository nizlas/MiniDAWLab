#pragma once

#include <JuceHeader.h>

#include <functional>

#include "domain/Track.h"
#include "instruments/InstrumentTrackController.h"
#include "util/AsyncLifetimeToken.h"

class Session;
class Transport;
class InstrumentRuntimeCoordinator;
class TrackLanesView;
class TimelineRulerView;
class InspectorView;

/// Arrangement-level MIDI import at the transport playhead (track header → FileChooser). Accepts
/// every row that owns timeline MIDI clips (`trackKindOwnsTimelineMidiClips`): instrument rows and
/// plain `TrackKind::Midi` rows, which store clips in the same plugin-less controller and only
/// differ in where their notes are rendered (own plugin vs the "MIDI To" destination).
class InstrumentMidiImportCoordinator final
{
public:
    struct Callbacks
    {
        std::function<void(const juce::String& label, std::function<bool()> mutator)> executeUndoableInstrumentEdit;
        std::function<void()> syncViewportFromSession;
        std::function<void()> refreshInstrumentUi;
    };

    InstrumentMidiImportCoordinator(Session& session,
                                    Transport& transport,
                                    InstrumentRuntimeCoordinator& instrumentRuntime,
                                    TrackLanesView& trackLanesView,
                                    TimelineRulerView& rulerView,
                                    InspectorView& inspectorView,
                                    Callbacks callbacks);

    /// Header menu entry: validates the row, then opens the file chooser and imports the picked file
    /// via `importMidiFileOntoTrackNow`.
    void importMidiFileForTrack(TrackId tid);

    struct ImportOutcome
    {
        bool ok = false;
        InstrumentMidiClipId createdClipId = 0;
        int notesParsed = 0;
        juce::String userMessage; ///< Parse/import failure text shown to the user (empty on success).
    };

    /// [Message thread] The import itself, without the chooser: parse `file` with the production
    /// parser, append one clip at the transport playhead as an undoable instrument edit, sync the
    /// arrangement/inspector. Used by the chooser callback and by the stability scenarios, so tests
    /// exercise exactly the path a user's import takes.
    ImportOutcome importMidiFileOntoTrackNow(TrackId tid, const juce::File& file);

private:
    Session& session_;
    Transport& transport_;
    InstrumentRuntimeCoordinator& instrumentRuntime_;
    TrackLanesView& trackLanesView_;
    TimelineRulerView& rulerView_;
    InspectorView& inspectorView_;
    Callbacks callbacks_;
    bool importInFlight_ = false;
    /// Stability Slice 4: FileChooser completions check this before touching the coordinator.
    mini_daw::AsyncLifetimeOwnerToken asyncLifetime_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(InstrumentMidiImportCoordinator)
};
