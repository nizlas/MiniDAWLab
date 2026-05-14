#include "app/ProjectIoCoordinator.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include "domain/Session.h"
#include "domain/SessionSnapshot.h"
#include "domain/Track.h"
#include "instruments/InstrumentTrackController.h"
#include "io/ProjectFile.h"
#include "plugins/ExperimentalInstrumentHost.h"
#include "plugins/PluginInsertHost.h"
#include "transport/Transport.h"

namespace
{
    // First-time Save As: abort with a non-empty message if we cannot write without clobbering.
    // `projectFile` = `<projectFolder>/<projectName>.dalproj`.
    [[nodiscard]] juce::String firstTimeSaveConflictMessage(const juce::File& projectFolder,
                                                            const juce::File& projectFile)
    {
        if (projectFile.existsAsFile())
        {
            return "A project file already exists at:\n" + projectFile.getFullPathName()
                   + "\n\nChoose a different name or delete the existing file first.";
        }
        if (projectFolder.exists() && !projectFolder.isDirectory())
        {
            return "Cannot create the project folder; a file already exists at:\n"
                   + projectFolder.getFullPathName();
        }
        if (projectFolder.isDirectory())
        {
            juce::Array<juce::File> files;
            projectFolder.findChildFiles(files, juce::File::findFiles, false);
            for (const auto& c : files)
            {
                const juce::String n = c.getFileName();
                if (n.endsWithIgnoreCase(".dalproj") || n.endsWithIgnoreCase(".mdlproj"))
                {
                    if (!(c == projectFile))
                    {
                        return "The project folder already contains a different project file:\n"
                               + c.getFullPathName()
                               + "\n\nChoose a different folder or name.";
                    }
                }
            }
        }
        return {};
    }
} // namespace

ProjectIoCoordinator::ProjectIoCoordinator(Transport& transport,
                                           Session& session,
                                           juce::AudioDeviceManager& deviceManager,
                                           PluginInsertHost& pluginHost,
                                           Callbacks callbacks)
    : transport_(transport)
    , session_(session)
    , deviceManager_(deviceManager)
    , pluginHost_(pluginHost)
    , callbacks_(std::move(callbacks))
{
}

void ProjectIoCoordinator::saveProject()
{
    juce::AudioIODevice* const device = deviceManager_.getCurrentAudioDevice();
    if (device == nullptr)
    {
        juce::AlertWindow::showMessageBoxAsync(
            juce::AlertWindow::WarningIcon,
            "Save project",
            "No active audio device; cannot include device sample rate in the project file.");
        return;
    }
    const double sampleRate = device->getCurrentSampleRate();

    // Normal save: no chooser. Explicit "Save As" / "New project" is deferred.
    if (session_.hasKnownProjectFile())
    {
        callbacks_.snapshotOpenClipViewportFromMidiEditor();
        const ExperimentalInstrumentCtlLookupFn ctlLookup([this](const TrackId laneId) noexcept {
            return callbacks_.instrumentCtlByTrackId(laneId);
        });
        const juce::Result r = session_.saveProjectToFile(
            transport_, session_.getCurrentProjectFile(), sampleRate, &pluginHost_, ctlLookup);
        if (!r.wasOk())
        {
            juce::AlertWindow::showMessageBoxAsync(
                juce::AlertWindow::WarningIcon, "Save project", r.getErrorMessage());
        }
        return;
    }

    // First-time save: DAW-style `<Parent>/<ProjectName>/<ProjectName>.dalproj`
    const auto fileChooserFlags = juce::FileBrowserComponent::saveMode
                                  | juce::FileBrowserComponent::canSelectFiles;
    auto chooser = std::make_shared<juce::FileChooser>(
        "Save project as…",
        juce::File{},
        "*.dalproj");
    chooser->launchAsync(fileChooserFlags, [this, chooser, sampleRate](const juce::FileChooser& fc) {
        juce::ignoreUnused(chooser);
        juce::File userPick = fc.getResult();
        if (userPick.getFullPathName().isEmpty())
        {
            return;
        }
        if (!userPick.hasFileExtension("dalproj"))
        {
            userPick = userPick.getSiblingFile(
                userPick.getFileNameWithoutExtension() + ".dalproj");
        }
        const juce::String projectName = userPick.getFileNameWithoutExtension();
        if (projectName.isEmpty())
        {
            juce::AlertWindow::showMessageBoxAsync(
                juce::AlertWindow::WarningIcon,
                "Save project",
                "Invalid project name.");
            return;
        }
        const juce::File parentDir = userPick.getParentDirectory();
        const juce::File projectFolder = parentDir.getChildFile(projectName);
        const juce::File projectFile = projectFolder.getChildFile(projectName + ".dalproj");
        {
            const juce::String conflict = firstTimeSaveConflictMessage(projectFolder, projectFile);
            if (conflict.isNotEmpty())
            {
                juce::AlertWindow::showMessageBoxAsync(
                    juce::AlertWindow::WarningIcon, "Save project", conflict);
                return;
            }
        }
        if (!projectFolder.isDirectory() && !projectFolder.createDirectory())
        {
            juce::AlertWindow::showMessageBoxAsync(
                juce::AlertWindow::WarningIcon,
                "Save project",
                "Could not create the project folder:\n" + projectFolder.getFullPathName());
            return;
        }
        {
            const juce::String conflict2 = firstTimeSaveConflictMessage(projectFolder, projectFile);
            if (conflict2.isNotEmpty())
            {
                juce::AlertWindow::showMessageBoxAsync(
                    juce::AlertWindow::WarningIcon, "Save project", conflict2);
                return;
            }
        }
        callbacks_.snapshotOpenClipViewportFromMidiEditor();
        const ExperimentalInstrumentCtlLookupFn ctlLookup([this](const TrackId laneId) noexcept {
            return callbacks_.instrumentCtlByTrackId(laneId);
        });
        const juce::Result r = session_.saveProjectToFile(
            transport_, projectFile, sampleRate, &pluginHost_, ctlLookup);
        if (!r.wasOk())
        {
            juce::AlertWindow::showMessageBoxAsync(
                juce::AlertWindow::WarningIcon, "Save project", r.getErrorMessage());
        }
    });
}

void ProjectIoCoordinator::loadProject()
{
    juce::AudioIODevice* const device = deviceManager_.getCurrentAudioDevice();
    if (device == nullptr)
    {
        juce::AlertWindow::showMessageBoxAsync(
            juce::AlertWindow::WarningIcon,
            "Load project",
            "No active audio device; cannot match sample rate to decode project clips.");
        return;
    }
    const double sampleRate = device->getCurrentSampleRate();

    const auto fileChooserFlags = juce::FileBrowserComponent::openMode
                                  | juce::FileBrowserComponent::canSelectFiles;
    auto chooser = std::make_shared<juce::FileChooser>(
        "Load project",
        juce::File{},
        "*.dalproj;*.mdlproj");
    chooser->launchAsync(fileChooserFlags, [this, chooser, sampleRate](const juce::FileChooser& fc) {
        juce::ignoreUnused(chooser);
        const juce::File f = fc.getResult();
        if (!f.existsAsFile())
        {
            return;
        }
        ProjectFileV1 parsedLoad;
        const juce::Result parsedRes = readProjectFile(f, parsedLoad);
        if (!parsedRes.wasOk())
        {
            juce::AlertWindow::showMessageBoxAsync(
                juce::AlertWindow::WarningIcon, "Load project", parsedRes.getErrorMessage());
            return;
        }
        callbacks_.clearExperimentalInstrumentRuntimesPreserveBridgeOnly();

        juce::StringArray skipped;
        juce::String infoNote;
        const juce::Result r = session_.applyLoadedProjectModel(
            transport_,
            f,
            parsedLoad,
            sampleRate,
            skipped,
            infoNote,
            &pluginHost_);
        if (!r.wasOk())
        {
            juce::AlertWindow::showMessageBoxAsync(
                juce::AlertWindow::WarningIcon, "Load project", r.getErrorMessage());
            return;
        }
        juce::String instrumentAutoloadNoteAcc;
        if (!parsedLoad.experimentalInstrumentTracks.empty())
        {
            for (const auto& etRow : parsedLoad.experimentalInstrumentTracks)
            {
                if (!etRow.enabled || etRow.instrumentKind != "GrooveAgentSE")
                {
                    continue;
                }
                const TrackId bindTid = InstrumentTrackController::resolveExperimentalInstrumentLaneIdFromProjectFields(
                    &session_,
                    etRow.trackId,
                    &parsedLoad.tracks);
                const std::shared_ptr<const SessionSnapshot> postSnap
                    = session_.loadSessionSnapshotForAudioThread();
                if (bindTid == kInvalidTrackId || postSnap == nullptr)
                {
                    continue;
                }
                const int tix = postSnap->findTrackIndexById(bindTid);
                if (tix < 0 || postSnap->getTrack(tix).getKind() != TrackKind::Instrument)
                {
                    continue;
                }
                const auto runtime = callbacks_.getOrCreateInstrumentRuntimeForTrack(bindTid);
                InstrumentTrackController* ctl = runtime.second;
                ExperimentalInstrumentHost* mh = runtime.first;
                if (ctl == nullptr || mh == nullptr)
                {
                    continue;
                }
                ctl->setTimelineSampleRate(sampleRate);
                ctl->restoreExperimentalInstrumentSingleProjectRow(etRow, &parsedLoad.tracks);
                juce::String noteOne;
                ctl->runPendingGrooveAgentProjectAutoload(*mh, noteOne);
                if (noteOne.isNotEmpty())
                {
                    if (instrumentAutoloadNoteAcc.isNotEmpty())
                    {
                        instrumentAutoloadNoteAcc << "\n\n";
                    }
                    instrumentAutoloadNoteAcc << noteOne;
                }
            }
        }
        callbacks_.syncMidiEditorInstrumentStateFromHost();
        const juce::String instrumentAutoloadNote(instrumentAutoloadNoteAcc);
        {
            if (infoNote.isNotEmpty())
            {
                infoNote << "\n\n";
            }
            infoNote << instrumentAutoloadNote;
        }
        callbacks_.clearSessionHistory();
        callbacks_.refreshAllUiAfterLoadedProject();
        if (infoNote.isNotEmpty() || skipped.size() > 0)
        {
            juce::String body;
            if (infoNote.isNotEmpty())
            {
                body = infoNote;
            }
            if (skipped.size() > 0)
            {
                if (body.isNotEmpty())
                {
                    body << "\n\n";
                }
                body << "Could not load " + juce::String(skipped.size())
                     + (skipped.size() == 1 ? " file:" : " files:") + "\n\n";
                for (int i = 0; i < skipped.size(); ++i)
                {
                    body << skipped[i] << (i < skipped.size() - 1 ? "\n" : "");
                }
            }
            juce::AlertWindow::showMessageBoxAsync(
                juce::AlertWindow::InfoIcon, "Load project (partial or note)", body);
        }
    });
}
