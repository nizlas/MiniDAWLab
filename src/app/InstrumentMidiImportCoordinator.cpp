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

void InstrumentMidiImportCoordinator::importMidiFileForInstrumentTrack(const TrackId tid)
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
    if (ix < 0 || snap->getTrack(ix).getKind() != TrackKind::Instrument)
    {
        juce::AlertWindow::showMessageBoxAsync(
            juce::AlertWindow::WarningIcon,
            "Import MIDI file",
            "That track is not an instrument lane.");
        return;
    }

    InstrumentTrackController* ctl = instrumentRuntime_.getInstrumentControllerForTrack(tid);
    if (ctl == nullptr || !ctl->hasInstrumentTrack())
    {
        juce::AlertWindow::showMessageBoxAsync(
            juce::AlertWindow::WarningIcon,
            "Import MIDI file",
            "Instrument controller is not available for this track.");
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

    chooser->launchAsync(fileChooserFlags, [this, chooser, tid](const juce::FileChooser& fc) {
        juce::ignoreUnused(chooser);
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

        const std::int64_t startSamples = transport_.readPlayheadSamplesForUi();

        ExperimentalMidiImportResult parseResult
            = experimentalImportMidiFile(file, kDefaultExperimentalTicksPerQuarter);
        if (!parseResult.ok)
        {
            juce::AlertWindow::showMessageBoxAsync(
                juce::AlertWindow::WarningIcon,
                "MIDI import failed",
                parseResult.combinedUserMessageLine());
            return;
        }

        juce::String suggestedName = file.getFileNameWithoutExtension();
        if (suggestedName.length() > 48)
        {
            suggestedName = suggestedName.substring(0, 48);
        }

        const juce::String warningCopy = parseResult.warningMessage;
        std::vector<TimelineMidiNote> notes = std::move(parseResult.notes);

        auto execute = callbacks_.executeUndoableInstrumentEdit;
        auto syncVp = callbacks_.syncViewportFromSession;
        auto refreshInstr = callbacks_.refreshInstrumentUi;
        if (execute == nullptr)
        {
            return;
        }

        execute("Import MIDI file", [this,
                                     tid,
                                     startSamples,
                                     suggestedName,
                                     notes = std::move(notes),
                                     syncVp,
                                     refreshInstr]() mutable -> bool {
            InstrumentTrackController* c = instrumentRuntime_.getInstrumentControllerForTrack(tid);
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

        if (warningCopy.isNotEmpty())
        {
            juce::AlertWindow::showMessageBoxAsync(
                juce::AlertWindow::InfoIcon,
                "MIDI import",
                warningCopy);
        }
    });
}
