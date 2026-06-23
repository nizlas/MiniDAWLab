#include "app/ProjectIoCoordinator.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include "diagnostics/ProjectLoadDiagnosticLog.h"
#include "domain/Session.h"
#include "domain/SessionSnapshot.h"
#include "domain/Track.h"
#include "engine/PlaybackEngine.h"
#include "instruments/InstrumentTrackController.h"
#include "io/ProjectFile.h"
#include "plugins/ExperimentalInstrumentHost.h"
#include "plugins/PluginInsertHost.h"
#include "transport/Transport.h"

#include <unordered_set>

namespace
{
    class ScopedInstrumentProcessingLoadGate final
    {
    public:
        ScopedInstrumentProcessingLoadGate(PlaybackEngine& playbackEngine, Session& session) noexcept
            : playbackEngine_(playbackEngine)
            , loadGeneration_(session.beginProjectLoadGeneration())
        {
            appendProjectLoadDiagnosticLine("load: instrument processing suspended gen="
                                            + juce::String((juce::int64)loadGeneration_));
            playbackEngine_.setInstrumentProcessingSuspended(true);

            const double waitStartMs = juce::Time::getMillisecondCounterHiRes();
            constexpr double kMaxWaitMs = 50.0;
            while (playbackEngine_.isAudioInsideInstrumentSection())
            {
                if (juce::Time::getMillisecondCounterHiRes() - waitStartMs >= kMaxWaitMs)
                {
                    break;
                }
                juce::Thread::sleep(1);
            }
            const int waitedMs
                = static_cast<int>(juce::Time::getMillisecondCounterHiRes() - waitStartMs + 0.5);
            appendProjectLoadDiagnosticLine("load: audio instrument section idle ack waited="
                                            + juce::String(waitedMs) + "ms");
        }

        ~ScopedInstrumentProcessingLoadGate() noexcept
        {
            playbackEngine_.setInstrumentProcessingSuspended(false);
            appendProjectLoadDiagnosticLine("load: instrument processing resumed gen="
                                            + juce::String((juce::int64)loadGeneration_));
        }

        ScopedInstrumentProcessingLoadGate(const ScopedInstrumentProcessingLoadGate&) = delete;
        ScopedInstrumentProcessingLoadGate& operator=(const ScopedInstrumentProcessingLoadGate&) = delete;

        [[nodiscard]] std::uint64_t loadGeneration() const noexcept
        {
            return loadGeneration_;
        }

    private:
        PlaybackEngine& playbackEngine_;
        std::uint64_t loadGeneration_;
    };

    // First-time Save As: abort with a non-empty message if we cannot write without clobbering.
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

    void warnIfGenericCatalogInstrumentsUnloadedOnSave(
        const Session& session,
        const ProjectIoCoordinator::Callbacks& callbacks)
    {
        if (callbacks.instrumentCtlByTrackId == nullptr)
        {
            return;
        }
        const std::shared_ptr<const SessionSnapshot> snap = session.loadSessionSnapshotForAudioThread();
        if (snap == nullptr)
        {
            return;
        }
        int unloadedGenericCount = 0;
        for (int i = 0; i < snap->getNumTracks(); ++i)
        {
            const Track& tr = snap->getTrack(i);
            if (tr.getKind() != TrackKind::Instrument)
            {
                continue;
            }
            InstrumentTrackController* const ctl = callbacks.instrumentCtlByTrackId(tr.getId());
            if (ctl != nullptr && ctl->hasInstrumentTrack() && ctl->isGenericCatalogInstrument()
                && !ctl->isInstrumentLoaded())
            {
                ++unloadedGenericCount;
            }
        }
        if (unloadedGenericCount <= 0)
        {
            return;
        }
        juce::String msg = "Note: ";
        msg << juce::String(unloadedGenericCount) << " generic VST3 instrument track";
        if (unloadedGenericCount != 1)
        {
            msg << "s";
        }
        msg << " had no loaded plugin at save time.\n\nMIDI clips are saved, but reload will use a placeholder unless the plugin can be resolved from the catalog.";
        juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::InfoIcon, "Save project", msg);
    }

    [[nodiscard]] juce::String stripGenericVst3PlaceholderSuffix(juce::String name)
    {
        const juce::String oldSuffix = " (session-only plugin not loaded)";
        const juce::String newSuffix = " (plugin not loaded)";
        if (name.endsWith(newSuffix))
        {
            return name.dropLastCharacters(newSuffix.length()).trimEnd();
        }
        const int oldIdx = name.indexOfIgnoreCase(oldSuffix);
        if (oldIdx >= 0)
        {
            return name.substring(0, oldIdx).trimEnd();
        }
        return name;
    }

    [[nodiscard]] juce::String ensureGenericVst3PlaceholderTrackName(juce::String name)
    {
        if (name.containsIgnoreCase("plugin not loaded"))
        {
            return name;
        }
        return stripGenericVst3PlaceholderSuffix(name) + " (plugin not loaded)";
    }

    [[nodiscard]] juce::String genericVst3DisplayNameFromRow(
        const Session& session,
        const TrackId bindTid,
        const ProjectFileExperimentalInstrumentTrackV1* rowMaybe)
    {
        if (rowMaybe != nullptr && rowMaybe->name.isNotEmpty())
        {
            return stripGenericVst3PlaceholderSuffix(rowMaybe->name);
        }
        if (const std::shared_ptr<const SessionSnapshot> snap = session.loadSessionSnapshotForAudioThread())
        {
            const int ix = snap->findTrackIndexById(bindTid);
            if (ix >= 0)
            {
                return stripGenericVst3PlaceholderSuffix(snap->getTrack(ix).getName());
            }
        }
        return juce::String("Instrument");
    }

    void restoreGenericVst3InstrumentTrack(
        Session& session,
        const ProjectIoCoordinator::Callbacks& callbacks,
        const TrackId bindTid,
        const double sampleRate,
        const ProjectFileExperimentalInstrumentTrackV1* rowMaybe,
        const std::vector<ProjectFileTrackV1>* trackRows,
        juce::String& noteAcc)
    {
        if (callbacks.getOrCreateInstrumentRuntimeForTrack == nullptr)
        {
            return;
        }
        const auto runtime = callbacks.getOrCreateInstrumentRuntimeForTrack(bindTid);
        ExperimentalInstrumentHost* mh = runtime.first;
        InstrumentTrackController* ctl = runtime.second;
        if (ctl == nullptr || mh == nullptr)
        {
            return;
        }
        ctl->setTimelineSampleRate(sampleRate);
        if (rowMaybe != nullptr)
        {
            ctl->restoreExperimentalInstrumentSingleProjectRow(*rowMaybe, trackRows);
        }

        juce::String noteOne;
        ctl->runPendingGenericVst3ProjectAutoload(*mh, noteOne);
        (void)ctl->bootstrapGenericCatalogInstrumentShellForSessionTrack(bindTid);

        const juce::String displayName = genericVst3DisplayNameFromRow(session, bindTid, rowMaybe);
        if (mh->hasInstrument())
        {
            juce::String restoredName = displayName;
            if (restoredName.isEmpty())
            {
                restoredName = mh->getInstrumentNameForUi();
            }
            if (restoredName.isNotEmpty())
            {
                session.setTrackName(bindTid, restoredName);
            }
            ctl->syncShellWithHostState();
        }
        else
        {
            const juce::String placeholderName = ensureGenericVst3PlaceholderTrackName(
                displayName.isNotEmpty() ? displayName : juce::String("Instrument"));
            session.setTrackName(bindTid, placeholderName);
            if (noteOne.isEmpty())
            {
                noteOne = displayName + " could not be loaded. MIDI clips were preserved on a placeholder track.";
            }
        }

        if (noteOne.isNotEmpty())
        {
            if (noteAcc.isNotEmpty())
            {
                noteAcc << "\n\n";
            }
            noteAcc << noteOne;
        }
    }

    void restoreGenericCatalogPlaceholderLane(
        Session& session,
        const ProjectIoCoordinator::Callbacks& callbacks,
        const TrackId bindTid,
        const double sampleRate,
        const ProjectFileExperimentalInstrumentTrackV1* rowMaybe,
        const std::vector<ProjectFileTrackV1>* trackRows,
        juce::String& noteAcc)
    {
        if (callbacks.getOrCreateInstrumentRuntimeForTrack == nullptr)
        {
            return;
        }
        const auto runtime = callbacks.getOrCreateInstrumentRuntimeForTrack(bindTid);
        InstrumentTrackController* ctl = runtime.second;
        if (ctl == nullptr)
        {
            return;
        }
        ctl->setTimelineSampleRate(sampleRate);
        if (rowMaybe != nullptr)
        {
            ctl->restoreExperimentalInstrumentSingleProjectRow(*rowMaybe, trackRows);
        }
        (void)ctl->bootstrapGenericCatalogInstrumentShellForSessionTrack(bindTid);

        juce::String laneName;
        if (const std::shared_ptr<const SessionSnapshot> snap = session.loadSessionSnapshotForAudioThread())
        {
            const int ix = snap->findTrackIndexById(bindTid);
            if (ix >= 0)
            {
                laneName = snap->getTrack(ix).getName();
            }
        }
        if (laneName.isEmpty() && rowMaybe != nullptr && rowMaybe->name.isNotEmpty())
        {
            laneName = rowMaybe->name;
        }
        if (laneName.isEmpty())
        {
            laneName = "Instrument";
        }
        const juce::String placeholderName = ensureGenericVst3PlaceholderTrackName(laneName);
        session.setTrackName(bindTid, placeholderName);

        juce::String note = "Generic catalog instrument \"" + placeholderName
                            + "\" was restored without a loaded plugin (session-only).";
        if (rowMaybe != nullptr && !rowMaybe->clips.empty())
        {
            note << " MIDI clips on this lane were kept.";
        }
        if (noteAcc.isNotEmpty())
        {
            noteAcc << "\n\n";
        }
        noteAcc << note;
    }

    void restoreOrphanInstrumentLanesWithoutRuntime(
        Session& session,
        const ProjectFileV1& parsedLoad,
        const ProjectIoCoordinator::Callbacks& callbacks,
        const double sampleRate,
        juce::String& noteAcc)
    {
        const std::shared_ptr<const SessionSnapshot> snap = session.loadSessionSnapshotForAudioThread();
        if (snap == nullptr || callbacks.instrumentCtlByTrackId == nullptr
            || callbacks.getOrCreateInstrumentRuntimeForTrack == nullptr)
        {
            return;
        }

        for (int ti = 0; ti < snap->getNumTracks(); ++ti)
        {
            const Track& tr = snap->getTrack(ti);
            if (tr.getKind() != TrackKind::Instrument)
            {
                continue;
            }
            const TrackId tid = tr.getId();
            InstrumentTrackController* const existing = callbacks.instrumentCtlByTrackId(tid);
            if (existing != nullptr && existing->hasInstrumentTrack()
                && existing->getExperimentalInstrumentDomainTrackId() == tid)
            {
                continue;
            }

            bool payloadExpectedForLane = false;
            for (const auto& etRow : parsedLoad.experimentalInstrumentTracks)
            {
                if (!etRow.enabled)
                {
                    continue;
                }
                if (etRow.instrumentKind != "GrooveAgentSE" && etRow.instrumentKind != "HALionSonic"
                    && etRow.instrumentKind != "GenericVst3")
                {
                    continue;
                }
                const TrackId resolved
                    = InstrumentTrackController::resolveExperimentalInstrumentLaneIdFromProjectFields(
                        &session,
                        etRow.trackId,
                        &parsedLoad.tracks);
                if (resolved == tid)
                {
                    payloadExpectedForLane = true;
                    break;
                }
            }
            if (payloadExpectedForLane)
            {
                continue;
            }

            restoreGenericCatalogPlaceholderLane(
                session, callbacks, tid, sampleRate, nullptr, nullptr, noteAcc);
        }
    }
} // namespace

ProjectIoCoordinator::ProjectIoCoordinator(Transport& transport,
                                           Session& session,
                                           juce::AudioDeviceManager& deviceManager,
                                           PluginInsertHost& pluginHost,
                                           PlaybackEngine& playbackEngine,
                                           Callbacks callbacks)
    : transport_(transport)
    , session_(session)
    , deviceManager_(deviceManager)
    , pluginHost_(pluginHost)
    , playbackEngine_(playbackEngine)
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
        const SnapProjectRootFields snapRoot = callbacks_.getSnapProjectRootFieldsForSave();
        std::optional<ProjectFileMainWindowBoundsV1> mainWinBounds;
        if (callbacks_.getMainWindowBoundsForProjectSave != nullptr)
        {
            mainWinBounds = callbacks_.getMainWindowBoundsForProjectSave();
        }
        const juce::Result r = session_.saveProjectToFile(
            transport_,
            session_.getCurrentProjectFile(),
            sampleRate,
            &pluginHost_,
            ctlLookup,
            snapRoot.enabled,
            snapRoot.resolutionKey,
            mainWinBounds);
        if (!r.wasOk())
        {
            juce::AlertWindow::showMessageBoxAsync(
                juce::AlertWindow::WarningIcon, "Save project", r.getErrorMessage());
        }
        else
        {
            warnIfGenericCatalogInstrumentsUnloadedOnSave(session_, callbacks_);
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
        const SnapProjectRootFields snapRoot = callbacks_.getSnapProjectRootFieldsForSave();
        std::optional<ProjectFileMainWindowBoundsV1> mainWinBounds;
        if (callbacks_.getMainWindowBoundsForProjectSave != nullptr)
        {
            mainWinBounds = callbacks_.getMainWindowBoundsForProjectSave();
        }
        const juce::Result r = session_.saveProjectToFile(
            transport_,
            projectFile,
            sampleRate,
            &pluginHost_,
            ctlLookup,
            snapRoot.enabled,
            snapRoot.resolutionKey,
            mainWinBounds);
        if (!r.wasOk())
        {
            juce::AlertWindow::showMessageBoxAsync(
                juce::AlertWindow::WarningIcon, "Save project", r.getErrorMessage());
        }
        else
        {
            warnIfGenericCatalogInstrumentsUnloadedOnSave(session_, callbacks_);
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
        appendProjectLoadDiagnosticLine("load: parsed file=\"" + f.getFullPathName() + "\" experimentalInstrumentTracks="
                                        + juce::String((int)parsedLoad.experimentalInstrumentTracks.size()));
        transport_.requestPlaybackIntent(PlaybackIntent::Stopped);
        appendProjectLoadDiagnosticLine("load: transport stopped");

        const ScopedInstrumentProcessingLoadGate instrumentLoadGate(playbackEngine_, session_);
        const std::uint64_t loadGeneration = instrumentLoadGate.loadGeneration();

        appendProjectLoadDiagnosticLine("load: clearExperimentalInstrumentRuntimes begin");
        callbacks_.clearExperimentalInstrumentRuntimesPreserveBridgeOnly();
        appendProjectLoadDiagnosticLine("load: clearExperimentalInstrumentRuntimes end");

        appendProjectLoadDiagnosticLine("load: before applyLoadedProjectModel (session model replace)");
        juce::StringArray skipped;
        juce::String infoNote;
        appendProjectLoadDiagnosticLine("load: applyLoadedProjectModel begin");
        const juce::Result r = session_.applyLoadedProjectModel(
            transport_,
            f,
            parsedLoad,
            sampleRate,
            skipped,
            infoNote,
            &pluginHost_,
            loadGeneration);
        if (!r.wasOk())
        {
            juce::AlertWindow::showMessageBoxAsync(
                juce::AlertWindow::WarningIcon, "Load project", r.getErrorMessage());
            return;
        }
        appendProjectLoadDiagnosticLine("load: applyLoadedProjectModel end");
        appendProjectLoadDiagnosticLine("load: after applyLoadedProjectModel (session model replaced)");
        callbacks_.restoreSnapProjectRootFieldsToUi(
            { parsedLoad.snapEnabled, parsedLoad.snapResolution });
        juce::String instrumentAutoloadNoteAcc;
        if (!parsedLoad.experimentalInstrumentTracks.empty())
        {
            std::unordered_set<TrackId> seenExperimentalTrackIds;
            seenExperimentalTrackIds.reserve(parsedLoad.experimentalInstrumentTracks.size());
            for (const auto& etRow : parsedLoad.experimentalInstrumentTracks)
            {
                const TrackId rowTid = etRow.trackId;
                const juce::String rowName = etRow.name.isNotEmpty() ? etRow.name : juce::String("(empty)");
                appendProjectLoadDiagnosticLine(
                    "load: experimentalInstrumentTrack row trackId=" + juce::String((juce::int64)rowTid)
                    + " instrumentKind=" + etRow.instrumentKind + " name=\"" + rowName + "\" clips="
                    + juce::String((int)etRow.clips.size()) + " enabled="
                    + juce::String(etRow.enabled ? "true" : "false"));
                if (rowTid != kInvalidTrackId)
                {
                    if (!seenExperimentalTrackIds.insert(rowTid).second)
                    {
                        appendProjectLoadDiagnosticLine(
                            "load: duplicate experimentalInstrumentTrack trackId="
                            + juce::String((juce::int64)rowTid));
                    }
                }
                bool tracksRowMatch = false;
                juce::String tracksRowKind;
                for (const auto& tr : parsedLoad.tracks)
                {
                    if (tr.id == rowTid)
                    {
                        tracksRowMatch = true;
                        tracksRowKind = tr.kind;
                        break;
                    }
                }
                appendProjectLoadDiagnosticLine(
                    "load: experimental row trackId=" + juce::String((juce::int64)rowTid)
                    + " tracks[] match=" + juce::String(tracksRowMatch ? "yes" : "NO")
                    + (tracksRowMatch ? (" kind=" + tracksRowKind) : juce::String{}));
            }
            for (const auto& tr : parsedLoad.tracks)
            {
                if (!tr.kind.equalsIgnoreCase("instrument"))
                {
                    continue;
                }
                bool hasExperimentalRow = false;
                for (const auto& etRow : parsedLoad.experimentalInstrumentTracks)
                {
                    if (etRow.trackId == tr.id)
                    {
                        hasExperimentalRow = true;
                        break;
                    }
                }
                if (!hasExperimentalRow)
                {
                    appendProjectLoadDiagnosticLine(
                        "load: tracks[] instrument without experimentalInstrumentTrack trackId="
                        + juce::String((juce::int64)tr.id) + " name=\"" + tr.name + "\"");
                }
            }
            for (const auto& etRow : parsedLoad.experimentalInstrumentTracks)
            {
                if (!etRow.enabled)
                {
                    continue;
                }
                const bool isGroove = etRow.instrumentKind == "GrooveAgentSE";
                const bool isHalion = etRow.instrumentKind == "HALionSonic";
                const bool isGeneric = etRow.instrumentKind == "GenericVst3";
                if (!isGroove && !isHalion && !isGeneric)
                {
                    appendProjectLoadDiagnosticLine(
                        "load: skip unknown experimental instrumentKind=\"" + etRow.instrumentKind
                        + "\" trackId=" + juce::String((juce::int64)etRow.trackId));
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
                    appendProjectLoadDiagnosticLine(
                        "load: skip experimental restore unresolved trackId="
                        + juce::String((juce::int64)etRow.trackId));
                    continue;
                }
                const int tix = postSnap->findTrackIndexById(bindTid);
                if (tix < 0 || postSnap->getTrack(tix).getKind() != TrackKind::Instrument)
                {
                    appendProjectLoadDiagnosticLine(
                        "load: skip experimental restore non-instrument lane trackId="
                        + juce::String((juce::int64)bindTid));
                    continue;
                }
                if (isGeneric)
                {
                    appendProjectLoadDiagnosticLine("load: before GenericVst3 restore trackId="
                                                    + juce::String((juce::int64)bindTid));
                    restoreGenericVst3InstrumentTrack(
                        session_,
                        callbacks_,
                        bindTid,
                        sampleRate,
                        &etRow,
                        &parsedLoad.tracks,
                        instrumentAutoloadNoteAcc);
                    appendProjectLoadDiagnosticLine("load: after GenericVst3 restore trackId="
                                                    + juce::String((juce::int64)bindTid));
                    continue;
                }
                appendProjectLoadDiagnosticLine(
                    "load: before GrooveAgent/HALion restore trackId=" + juce::String((juce::int64)bindTid)
                    + " kind=" + etRow.instrumentKind);
                const auto runtime = callbacks_.getOrCreateInstrumentRuntimeForTrack(bindTid);
                InstrumentTrackController* ctl = runtime.second;
                ExperimentalInstrumentHost* mh = runtime.first;
                if (ctl == nullptr || mh == nullptr)
                {
                    appendProjectLoadDiagnosticLine(
                        "load: skip GrooveAgent/HALion restore missing runtime trackId="
                        + juce::String((juce::int64)bindTid));
                    continue;
                }
                ctl->setTimelineSampleRate(sampleRate);
                ctl->restoreExperimentalInstrumentSingleProjectRow(etRow, &parsedLoad.tracks);
                juce::String noteOne;
                if (isGroove)
                {
                    ctl->runPendingGrooveAgentProjectAutoload(*mh, noteOne);
                }
                else
                {
                    ctl->runPendingHalionSonicProjectAutoload(*mh, noteOne);
                }
                appendProjectLoadDiagnosticLine(
                    "load: after GrooveAgent/HALion restore trackId=" + juce::String((juce::int64)bindTid)
                    + " kind=" + etRow.instrumentKind);
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
        appendProjectLoadDiagnosticLine("load: before orphan instrument lane restore");
        restoreOrphanInstrumentLanesWithoutRuntime(
            session_, parsedLoad, callbacks_, sampleRate, instrumentAutoloadNoteAcc);
        appendProjectLoadDiagnosticLine("load: after orphan instrument lane restore");
        appendProjectLoadDiagnosticLine("load: instrument restore complete");
        appendProjectLoadDiagnosticLine("load: before syncMidiEditorInstrumentStateFromHost");
        callbacks_.syncMidiEditorInstrumentStateFromHost();
        appendProjectLoadDiagnosticLine("load: after syncMidiEditorInstrumentStateFromHost");
        const juce::String instrumentAutoloadNote(instrumentAutoloadNoteAcc);
        {
            if (infoNote.isNotEmpty())
            {
                infoNote << "\n\n";
            }
            infoNote << instrumentAutoloadNote;
        }
        callbacks_.clearSessionHistory();
        appendProjectLoadDiagnosticLine("load: refreshAllUiAfterLoadedProject begin");
        callbacks_.refreshAllUiAfterLoadedProject();
        appendProjectLoadDiagnosticLine("load: refreshAllUiAfterLoadedProject end");
        appendProjectLoadDiagnosticLine("load: complete");
        if (parsedLoad.hasMainWindowBounds && callbacks_.applyMainWindowBoundsFromLoadedProject != nullptr)
        {
            callbacks_.applyMainWindowBoundsFromLoadedProject(parsedLoad);
        }
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
