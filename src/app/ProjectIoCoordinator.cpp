#include "app/ProjectIoCoordinator.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include "domain/Session.h"
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
