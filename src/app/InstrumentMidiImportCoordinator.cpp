#include "app/InstrumentMidiImportCoordinator.h"

#include <memory>

#include "app/InstrumentRuntimeCoordinator.h"
#include "domain/Session.h"
#include "domain/SessionSnapshot.h"
#include "domain/Track.h"
#include "instruments/InstrumentTrackController.h"
#include "transport/Transport.h"
#include "ui/experimental/ExperimentalMidiImport.h"
#include "ui/InspectorView.h"
#include "ui/TimelineRulerView.h"
#include "ui/TrackLanesView.h"

InstrumentMidiImportCoordinator::InstrumentMidiImportCoordinator(Session& session,
                                                                 Transport& transport,
                                                                 InstrumentRuntimeCoordinator& instrumentRuntime,
                                                                 TrackLanesView& trackLanesView,
                                                                 TimelineRulerView& rulerView,
                                                                 InspectorView& inspectorView,
                                                                 Callbacks callbacks)
    : session_(session)
    , transport_(transport)
    , instrumentRuntime_(instrumentRuntime)
    , trackLanesView_(trackLanesView)
    , rulerView_(rulerView)
    , inspectorView_(inspectorView)
    , callbacks_(std::move(callbacks))
{
}

void InstrumentMidiImportCoordinator::importMidiFileForTrack(const TrackId tid)
{
    const std::shared_ptr<const SessionSnapshot> snap = session_.loadSessionSnapshotForAudioThread();
    if (snap == nullptr)
    {
        juce::AlertWindow::showMessageBoxAsync(
            juce::AlertWindow::WarningIcon,
            "Import MIDI file",
            "Session is not ready.");
        return;
    }
    const int ix = snap->findTrackIndexById(tid);
    if (ix < 0 || !trackKindOwnsTimelineMidiClips(snap->getTrack(ix).getKind()))
    {
        juce::AlertWindow::showMessageBoxAsync(
            juce::AlertWindow::WarningIcon,
            "Import MIDI file",
            "That track does not hold MIDI clips. Import onto a MIDI or instrument track.");
        return;
    }

    InstrumentTrackController* ctl = instrumentRuntime_.getMidiClipControllerForTrack(tid);
    if (ctl == nullptr || !ctl->hasInstrumentTrack())
    {
        juce::AlertWindow::showMessageBoxAsync(
            juce::AlertWindow::WarningIcon,
            "Import MIDI file",
            "The MIDI clip controller is not available for this track.");
        return;
    }

    if (importInFlight_)
    {
        return;
    }
    importInFlight_ = true;

    const auto fileChooserFlags
        = juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles;

    auto chooser = std::make_shared<juce::FileChooser>(
        "Import MIDI file",
        juce::File{},
        "*.mid;*.midi");

    chooser->launchAsync(fileChooserFlags, [this, chooser, tid,
                                            guard = asyncLifetime_.guard()](const juce::FileChooser& fc) {
        juce::ignoreUnused(chooser);
        if (!guard.isAlive())
        {
            juce::Logger::writeToLog("[stale-async] skipped: MIDI import file chooser");
            return;
        }
        struct ClearImportInFlight
        {
            bool& b;
            explicit ClearImportInFlight(bool& ref) noexcept
                : b(ref)
            {
            }
            ~ClearImportInFlight() { b = false; }
        } clearImport{ importInFlight_ };

        const juce::File file = fc.getResult();
        if (!file.existsAsFile())
        {
            return;
        }
        (void)importMidiFileOntoTrackNow(tid, file);
    });
}

InstrumentMidiImportCoordinator::ImportOutcome
InstrumentMidiImportCoordinator::importMidiFileOntoTrackNow(const TrackId tid, const juce::File& file)
{
    ImportOutcome outcome;
    const std::int64_t startSamples = transport_.readPlayheadSamplesForUi();

    ExperimentalMidiImportResult parseResult
        = experimentalImportMidiFile(file, kDefaultExperimentalTicksPerQuarter);
    if (!parseResult.ok)
    {
        outcome.userMessage = parseResult.combinedUserMessageLine();
        juce::AlertWindow::showMessageBoxAsync(
            juce::AlertWindow::WarningIcon, "MIDI import failed", outcome.userMessage);
        return outcome;
    }

    juce::String suggestedName = file.getFileNameWithoutExtension();
    if (suggestedName.length() > 48)
    {
        suggestedName = suggestedName.substring(0, 48);
    }

    const juce::String warningCopy = parseResult.warningMessage;
    outcome.notesParsed = static_cast<int>(parseResult.notes.size());
    std::vector<TimelineMidiNote> notes = std::move(parseResult.notes);

    auto execute = callbacks_.executeUndoableInstrumentEdit;
    auto syncVp = callbacks_.syncViewportFromSession;
    auto refreshInstr = callbacks_.refreshInstrumentUi;
    if (execute == nullptr)
    {
        outcome.userMessage = "Undo service unavailable.";
        return outcome;
    }

    InstrumentMidiClipId* const createdIdOut = &outcome.createdClipId;
    execute("Import MIDI file", [this,
                                 tid,
                                 startSamples,
                                 suggestedName,
                                 notes = std::move(notes),
                                 syncVp,
                                 refreshInstr,
                                 createdIdOut]() mutable -> bool {
        InstrumentTrackController* c = instrumentRuntime_.getMidiClipControllerForTrack(tid);
        if (c == nullptr || !c->hasInstrumentTrack())
        {
            return false;
        }

        const InstrumentMidiClipId newId = c->appendImportedTimelineMidiClipAtSamples(
            std::move(notes), startSamples, suggestedName);
        if (newId == 0)
        {
            return false;
        }
        *createdIdOut = newId;
        c->setSelectedClipIdsExclusive(newId);

        if (syncVp != nullptr)
        {
            syncVp();
        }
        trackLanesView_.syncTracksFromSession();
        rulerView_.repaint();
        trackLanesView_.repaint();
        inspectorView_.refreshFromSession();
        if (refreshInstr != nullptr)
        {
            refreshInstr();
        }
        return true;
    });
    outcome.ok = outcome.createdClipId != 0;

    if (warningCopy.isNotEmpty())
    {
        juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::InfoIcon, "MIDI import", warningCopy);
    }
    return outcome;
}
