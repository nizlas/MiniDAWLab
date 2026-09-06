#include "app/ProjectIoCoordinator.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include "diagnostics/ProjectLoadDiagnosticLog.h"
#include "diagnostics/StabilityDiagnosticLog.h"
#include "diagnostics/StabilityInvariants.h"
#include "diagnostics/StabilityScenarioRunner.h"
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

    // -----------------------------------------------------------------------
    // Stability Slice 5: autosave locations.
    //
    // Project saves require audio clip paths to be `Audio/`-relative to the *target* file's
    // folder, so an autosave of a project with audio clips must live next to the project file.
    // A pointer file in %APPDATA% records where the most recent autosave was written so the
    // startup recovery check can find per-project autosaves.
    // -----------------------------------------------------------------------

    // Autosave polish: adaptive periodic-autosave policy. All thresholds live here; nothing
    // else in the file hardcodes an interval. The periodic timer ticks once per minute, but a
    // write only happens when the adaptive due time has passed, so large/slow projects are not
    // autosaved every minute forever.
    namespace autosave_policy
    {
        /// Internal tick; also the retry cadence when a due autosave is blocked or fails.
        constexpr int kTickMs = 60 * 1000;
        /// Minimum delay between the first tick that observes a dirty project and the first write.
        constexpr int kFirstDelayMs = 60 * 1000;

        // Last-write-duration thresholds -> next interval.
        constexpr int kFastWriteMs = 250;
        constexpr int kSlowWriteMs = 1000;
        constexpr int kVerySlowWriteMs = 3000;
        constexpr int kIntervalFastMs = 2 * 60 * 1000;
        constexpr int kIntervalDefaultMs = 5 * 60 * 1000;
        constexpr int kIntervalSlowMs = 10 * 60 * 1000;
        constexpr int kIntervalVerySlowMs = 15 * 60 * 1000;

        [[nodiscard]] constexpr int intervalForElapsedMs(const int elapsedMs) noexcept
        {
            if (elapsedMs < kFastWriteMs) { return kIntervalFastMs; }
            if (elapsedMs <= kSlowWriteMs) { return kIntervalDefaultMs; }
            if (elapsedMs <= kVerySlowWriteMs) { return kIntervalSlowMs; }
            return kIntervalVerySlowMs;
        }
    } // namespace autosave_policy

    [[nodiscard]] juce::File autosaveAppDataDirectory()
    {
        return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
            .getChildFile("MiniDAWLab");
    }

    [[nodiscard]] juce::File autosavePointerFile()
    {
        return autosaveAppDataDirectory().getChildFile("autosave-location.txt");
    }

    /// Autosave target for projects that have never been saved (no project folder yet).
    /// Works for MIDI-only projects; audio-clip projects fail the relative-path check (logged).
    [[nodiscard]] juce::File defaultAppDataAutosaveFile()
    {
        return autosaveAppDataDirectory().getChildFile("autosave.dalproj");
    }

    /// The autosave file recorded by the pointer file, or the %APPDATA% default; a non-existing
    /// return means "no autosave present". Stability C5: a stale/malformed pointer (recorded
    /// autosave missing) is logged and removed so it cannot confuse later startups.
    [[nodiscard]] juce::File findExistingAutosaveFile()
    {
        const juce::File pointer = autosavePointerFile();
        if (pointer.existsAsFile())
        {
            // Line 1 = autosave path; line 2 (optional, C5) = the original project it belongs to.
            juce::StringArray lines;
            pointer.readLines(lines);
            const juce::String recorded = lines.size() > 0 ? lines[0].trim() : juce::String{};
            if (recorded.isNotEmpty() && juce::File::isAbsolutePath(recorded))
            {
                const juce::File f(recorded);
                if (f.existsAsFile())
                {
                    // Backward compatibility: autosaves written before the project-specific
                    // naming ("<stem>_autosave.dalproj") were all called "autosave.dalproj".
                    // The pointer is authoritative, so they still recover fine; log for triage.
                    if (f.getFileName().equalsIgnoreCase("autosave.dalproj")
                        && f != defaultAppDataAutosaveFile())
                    {
                        appendAutosaveDiagnosticLine("legacy pointer accepted: path="
                                                     + f.getFullPathName());
                    }
                    return f;
                }
                appendAutosaveDiagnosticLine(
                    "recovery scan: stale pointer (recorded autosave missing): " + recorded
                    + " - pointer removed");
                (void)pointer.deleteFile();
            }
            else
            {
                appendAutosaveDiagnosticLine(
                    "recovery scan: malformed pointer file - pointer removed");
                (void)pointer.deleteFile();
            }
        }
        const juce::File fallback = defaultAppDataAutosaveFile();
        if (fallback.existsAsFile() && !pointer.existsAsFile())
        {
            appendAutosaveDiagnosticLine("recovery scan: autosave without pointer file found: "
                                         + fallback.getFullPathName() + " (informational)");
        }
        return fallback;
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
    // Startup baseline: the empty/default session counts as clean. Re-capture once the message
    // queue settles, in case remaining startup wiring publishes fresh session snapshots.
    markProjectCleanNow();
    juce::MessageManager::callAsync([this, guard = asyncLifetime_.guard()] {
        if (!guard.isAlive())
        {
            return;
        }
        if (!instrumentOrPluginEditsSinceClean_)
        {
            markProjectCleanNow();
        }
    });

    // Stability C5 + autosave polish: the timer ticks once per minute, but writes follow the
    // adaptive schedule in timerCallback (first write >= 60s after dirty is observed, then an
    // interval derived from how long the last write took; see autosave_policy). The first tick
    // fires a full minute after startup, so the window/session are stable by then; the timer
    // dies with this coordinator (before app shutdown tears the session down).
    startTimer(autosave_policy::kTickMs);
}

void ProjectIoCoordinator::saveProject()
{
    saveProjectThen({});
}

void ProjectIoCoordinator::saveProjectThen(std::function<void(bool)> onDone)
{
    const auto reportDone = [onDone](const bool saved) {
        if (onDone != nullptr)
        {
            onDone(saved);
        }
    };
    juce::AudioIODevice* const device = deviceManager_.getCurrentAudioDevice();
    if (device == nullptr)
    {
        juce::AlertWindow::showMessageBoxAsync(
            juce::AlertWindow::WarningIcon,
            "Save project",
            "No active audio device; cannot include device sample rate in the project file.");
        reportDone(false);
        return;
    }
    const double sampleRate = device->getCurrentSampleRate();

    // Normal save: no chooser. Explicit "Save As" / "New project" is deferred.
    if (session_.hasKnownProjectFile())
    {
        if (callbacks_.showSavingProjectIndicator != nullptr)
        {
            callbacks_.showSavingProjectIndicator();
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
        std::optional<ProjectFileMainWindowBoundsV1> midiEditorWinBounds;
        if (callbacks_.getMidiEditorWindowBoundsForProjectSave != nullptr)
        {
            midiEditorWinBounds = callbacks_.getMidiEditorWindowBoundsForProjectSave();
        }
        std::optional<ProjectFileMidiEditorWorkspaceV1> midiEditorWorkspace;
        if (callbacks_.getMidiEditorWorkspaceForProjectSave != nullptr)
        {
            midiEditorWorkspace = callbacks_.getMidiEditorWorkspaceForProjectSave();
        }
        writeLastOperationBreadcrumb("project save start: "
                                     + session_.getCurrentProjectFile().getFullPathName());
        const juce::Result r = session_.saveProjectToFile(
            transport_,
            session_.getCurrentProjectFile(),
            sampleRate,
            &pluginHost_,
            ctlLookup,
            snapRoot.enabled,
            snapRoot.resolutionKey,
            mainWinBounds,
            midiEditorWinBounds,
            midiEditorWorkspace);
        if (!r.wasOk())
        {
            writeLastOperationBreadcrumb("project save failed");
            juce::AlertWindow::showMessageBoxAsync(
                juce::AlertWindow::WarningIcon, "Save project", r.getErrorMessage());
            reportDone(false);
        }
        else
        {
            writeLastOperationBreadcrumb("project save end ok");
            markProjectCleanNow();
            deleteAutosaveArtifactsAfterSuccessfulSave();
            warnIfGenericCatalogInstrumentsUnloadedOnSave(session_, callbacks_);
            // P1H §18.2: queue proxy work per destination update mode. Never waits.
            if (callbacks_.onSuccessfulUserSave != nullptr)
            {
                callbacks_.onSuccessfulUserSave();
            }
            reportDone(true);
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
    chooser->launchAsync(fileChooserFlags, [this, chooser, sampleRate, reportDone,
                                            guard = asyncLifetime_.guard()](const juce::FileChooser& fc) {
        juce::ignoreUnused(chooser);
        if (!guard.isAlive())
        {
            juce::Logger::writeToLog("[stale-async] skipped: save-project-as file chooser");
            return;
        }
        juce::File userPick = fc.getResult();
        if (userPick.getFullPathName().isEmpty())
        {
            reportDone(false);
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
            reportDone(false);
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
                reportDone(false);
                return;
            }
        }
        if (!projectFolder.isDirectory() && !projectFolder.createDirectory())
        {
            juce::AlertWindow::showMessageBoxAsync(
                juce::AlertWindow::WarningIcon,
                "Save project",
                "Could not create the project folder:\n" + projectFolder.getFullPathName());
            reportDone(false);
            return;
        }
        {
            const juce::String conflict2 = firstTimeSaveConflictMessage(projectFolder, projectFile);
            if (conflict2.isNotEmpty())
            {
                juce::AlertWindow::showMessageBoxAsync(
                    juce::AlertWindow::WarningIcon, "Save project", conflict2);
                reportDone(false);
                return;
            }
        }
        if (callbacks_.showSavingProjectIndicator != nullptr)
        {
            callbacks_.showSavingProjectIndicator();
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
        std::optional<ProjectFileMainWindowBoundsV1> midiEditorWinBounds;
        if (callbacks_.getMidiEditorWindowBoundsForProjectSave != nullptr)
        {
            midiEditorWinBounds = callbacks_.getMidiEditorWindowBoundsForProjectSave();
        }
        std::optional<ProjectFileMidiEditorWorkspaceV1> midiEditorWorkspace;
        if (callbacks_.getMidiEditorWorkspaceForProjectSave != nullptr)
        {
            midiEditorWorkspace = callbacks_.getMidiEditorWorkspaceForProjectSave();
        }
        writeLastOperationBreadcrumb("project save start: " + projectFile.getFullPathName());
        const juce::Result r = session_.saveProjectToFile(
            transport_,
            projectFile,
            sampleRate,
            &pluginHost_,
            ctlLookup,
            snapRoot.enabled,
            snapRoot.resolutionKey,
            mainWinBounds,
            midiEditorWinBounds,
            midiEditorWorkspace);
        if (!r.wasOk())
        {
            writeLastOperationBreadcrumb("project save failed");
            juce::AlertWindow::showMessageBoxAsync(
                juce::AlertWindow::WarningIcon, "Save project", r.getErrorMessage());
            reportDone(false);
        }
        else
        {
            writeLastOperationBreadcrumb("project save end ok");
            markProjectCleanNow();
            deleteAutosaveArtifactsAfterSuccessfulSave();
            warnIfGenericCatalogInstrumentsUnloadedOnSave(session_, callbacks_);
            // P1H §16.6 Save As: rehome referenced proxy generation assets into the new
            // project layout (copy + validate only — never blocks on rendering, never
            // touches the original assets; failures degrade to honest ProxyMissing).
            if (callbacks_.rehomeProxyAssetsAfterSaveAs != nullptr)
            {
                callbacks_.rehomeProxyAssetsAfterSaveAs(projectFolder);
            }
            // P1H §18.2: queue proxy work per destination update mode. Never waits.
            if (callbacks_.onSuccessfulUserSave != nullptr)
            {
                callbacks_.onSuccessfulUserSave();
            }
            reportDone(true);
        }
    });
}

void ProjectIoCoordinator::loadProject()
{
    confirmUnsavedChangesThen(UnsavedGuardKind::LoadProject,
                              [this, guard = asyncLifetime_.guard()] {
                                  if (!guard.isAlive())
                                  {
                                      juce::Logger::writeToLog(
                                          "[stale-async] skipped: load-project after unsaved prompt");
                                      return;
                                  }
                                  launchLoadProjectChooser();
                              });
}

void ProjectIoCoordinator::launchLoadProjectChooser()
{
    const auto fileChooserFlags = juce::FileBrowserComponent::openMode
                                  | juce::FileBrowserComponent::canSelectFiles;
    auto chooser = std::make_shared<juce::FileChooser>(
        "Load project",
        juce::File{},
        "*.dalproj;*.mdlproj");
    chooser->launchAsync(fileChooserFlags, [this, chooser,
                                            guard = asyncLifetime_.guard()](const juce::FileChooser& fc) {
        juce::ignoreUnused(chooser);
        if (!guard.isAlive())
        {
            juce::Logger::writeToLog("[stale-async] skipped: load-project file chooser");
            return;
        }
        const juce::File f = fc.getResult();
        if (f.getFullPathName().isEmpty())
        {
            return;
        }
        loadProjectFromFile(f);
    });
}

void ProjectIoCoordinator::loadProjectFromFile(const juce::File& projectFile)
{
    if (!projectFile.existsAsFile())
    {
        juce::AlertWindow::showMessageBoxAsync(
            juce::AlertWindow::WarningIcon,
            "Load project",
            "Project file not found:\n" + projectFile.getFullPathName());
        return;
    }
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
    const juce::File f = projectFile;
    {
        writeLastOperationBreadcrumb("project load start: " + f.getFullPathName());
        ProjectFileV1 parsedLoad;
        const juce::Result parsedRes = readProjectFile(f, parsedLoad);
        if (!parsedRes.wasOk())
        {
            writeLastOperationBreadcrumb("project load failed (parse)");
            juce::AlertWindow::showMessageBoxAsync(
                juce::AlertWindow::WarningIcon, "Load project", parsedRes.getErrorMessage());
            return;
        }
        appendProjectLoadDiagnosticLine("load: parsed file=\"" + f.getFullPathName() + "\" experimentalInstrumentTracks="
                                        + juce::String((int)parsedLoad.experimentalInstrumentTracks.size()));
        transport_.requestPlaybackIntent(PlaybackIntent::Stopped);
        appendProjectLoadDiagnosticLine("load: transport stopped");

        // P1H project replacement (§13.3): obsolete/cancel every proxy job of the OLD project
        // and drop the runtime-only policy timers BEFORE the runtimes they reference are
        // cleared. Queued work is re-derivable from fingerprints on reopen; nothing waits.
        if (callbacks_.onProjectAboutToBeReplaced != nullptr)
        {
            callbacks_.onProjectAboutToBeReplaced();
        }

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
            writeLastOperationBreadcrumb("project load failed (apply model)");
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
                const bool isMidiContent = etRow.instrumentKind == "MidiContent";
                if (isMidiContent)
                {
                    // Phase B: plugin-less MIDI content row — restore clips onto a MIDI content
                    // controller bound to the TrackKind::Midi session row. No plugin autoload.
                    const TrackId midiTid = etRow.trackId;
                    const std::shared_ptr<const SessionSnapshot> midiSnap
                        = session_.loadSessionSnapshotForAudioThread();
                    const int midiTix = (midiSnap != nullptr && midiTid != kInvalidTrackId)
                                            ? midiSnap->findTrackIndexById(midiTid)
                                            : -1;
                    if (midiTix < 0 || midiSnap->getTrack(midiTix).getKind() != TrackKind::Midi
                        || callbacks_.getOrCreateMidiContentControllerForTrack == nullptr)
                    {
                        appendProjectLoadDiagnosticLine(
                            "load: skip MidiContent restore (row missing or not Midi) trackId="
                            + juce::String((juce::int64)midiTid));
                        continue;
                    }
                    InstrumentTrackController* const midiCtl
                        = callbacks_.getOrCreateMidiContentControllerForTrack(midiTid);
                    if (midiCtl == nullptr)
                    {
                        appendProjectLoadDiagnosticLine(
                            "load: skip MidiContent restore (controller create failed) trackId="
                            + juce::String((juce::int64)midiTid));
                        continue;
                    }
                    midiCtl->setTimelineSampleRate(sampleRate);
                    midiCtl->restoreExperimentalInstrumentSingleProjectRow(etRow, &parsedLoad.tracks);
                    appendProjectLoadDiagnosticLine("load: after MidiContent restore trackId="
                                                    + juce::String((juce::int64)midiTid));
                    continue;
                }
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
        // Phase B: every TrackKind::Midi row needs its plugin-less controller, including rows
        // whose project block was missing (e.g. hand-edited files) — otherwise the lane would
        // have no MIDI clip owner until restart.
        if (callbacks_.getOrCreateMidiContentControllerForTrack != nullptr)
        {
            if (const auto midiRowsSnap = session_.loadSessionSnapshotForAudioThread())
            {
                for (int ti = 0; ti < midiRowsSnap->getNumTracks(); ++ti)
                {
                    const Track& tr = midiRowsSnap->getTrack(ti);
                    if (tr.getKind() != TrackKind::Midi)
                    {
                        continue;
                    }
                    if (callbacks_.getOrCreateMidiContentControllerForTrack(tr.getId()) == nullptr)
                    {
                        appendProjectLoadDiagnosticLine(
                            "load: midi content controller create FAILED trackId="
                            + juce::String((juce::int64)tr.getId()));
                    }
                }
            }
        }
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
        // P1H: capture proxy asset source hints while the on-disk location is known (the
        // autosave recovery flow clears the save path AFTER this, so a later first-time
        // Save As can still copy the referenced generations from here).
        if (callbacks_.onProjectLoaded != nullptr)
        {
            callbacks_.onProjectLoaded(f.getParentDirectory());
        }
        appendProjectLoadDiagnosticLine("load: complete");
        markProjectCleanNow();
        writeLastOperationBreadcrumb("project load end ok: " + f.getFullPathName());
        // Stability C3: verify runtime invariants right after the load completed.
        (void) stability_invariants::runRegisteredStabilityInvariantsCheck("project-load-end");
        // Always invoked (even without saved bounds) so the MIDI editor bounds memo is seeded or
        // cleared per project; the callee no-ops per window when the project has no bounds.
        if (callbacks_.applyMainWindowBoundsFromLoadedProject != nullptr)
        {
            callbacks_.applyMainWindowBoundsFromLoadedProject(parsedLoad);
        }
        // Conny 1B: reopen the MIDI editor when the project saved it as open (after the session,
        // instrument runtimes, clips and main window are all restored). Skips safely when the
        // saved track/clip no longer exists; never opens plugin editor windows.
        if (callbacks_.restoreMidiEditorWorkspaceFromLoadedProject != nullptr)
        {
            callbacks_.restoreMidiEditorWorkspaceFromLoadedProject(parsedLoad);
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
    }
}

// =============================================================================
// Stability Slice 5: unsaved-work protection (dirty flag, prompts, autosave, recovery)
// =============================================================================

bool ProjectIoCoordinator::isProjectDirty() const noexcept
{
    if (instrumentOrPluginEditsSinceClean_)
    {
        return true;
    }
    const std::shared_ptr<const SessionSnapshot> live = session_.loadSessionSnapshotForAudioThread();
    return live.get() != cleanSessionSnapshot_.get();
}

void ProjectIoCoordinator::markProjectCleanNow() noexcept
{
    cleanSessionSnapshot_ = session_.loadSessionSnapshotForAudioThread();
    instrumentOrPluginEditsSinceClean_ = false;
    // A clean project needs no autosave; the next dirty phase starts from the initial delay.
    nextPeriodicAutosaveDueMs_ = 0;
}

void ProjectIoCoordinator::markProjectDirtyFromEdit() noexcept
{
    instrumentOrPluginEditsSinceClean_ = true;
}

void ProjectIoCoordinator::confirmUnsavedChangesThen(const UnsavedGuardKind kind,
                                                     std::function<void()> proceed)
{
    if (!isProjectDirty())
    {
        if (proceed != nullptr)
        {
            proceed();
        }
        return;
    }

    juce::String title, message, saveButton, withoutButton, kindName;
    switch (kind)
    {
        case UnsavedGuardKind::LoadProject:
            title = "Load project";
            message = "Project has unsaved changes. Save before loading another project?";
            saveButton = "Save and Load";
            withoutButton = "Load Without Saving";
            kindName = "load";
            break;
        case UnsavedGuardKind::QuitApp:
            title = "Quit";
            message = "Project has unsaved changes. Save before quitting?";
            saveButton = "Save and Quit";
            withoutButton = "Quit Without Saving";
            kindName = "quit";
            break;
        case UnsavedGuardKind::Export:
            title = "Audio mixdown";
            message = "Project has unsaved changes. Save before export?";
            saveButton = "Save and Export";
            withoutButton = "Export Without Saving";
            kindName = "export";
            break;
    }

    // Stability C2: never block a scenario run on a modal prompt. Deterministic auto-answer:
    // always "proceed without saving" (scenarios intentionally leave the session dirty).
    if (isStabilityTestModeActive())
    {
        appendStabilityRunLine("unsaved-changes prompt auto-answered: " + kindName
                               + " without saving");
        writeLastOperationBreadcrumb("unsaved-changes prompt auto-answered (stability test): "
                                     + kindName);
        if (proceed != nullptr)
        {
            proceed();
        }
        return;
    }

    writeLastOperationBreadcrumb("unsaved-changes prompt shown: " + kindName);
    juce::AlertWindow::showYesNoCancelBox(
        juce::AlertWindow::QuestionIcon,
        title,
        message,
        saveButton,
        withoutButton,
        "Cancel",
        nullptr,
        juce::ModalCallbackFunction::create(
            [this, proceed = std::move(proceed), kindName,
             guard = asyncLifetime_.guard()](const int result) {
                if (!guard.isAlive())
                {
                    juce::Logger::writeToLog("[stale-async] skipped: unsaved-changes prompt result");
                    return;
                }
                if (result == 1) // "Save and X"
                {
                    writeLastOperationBreadcrumb("unsaved-changes prompt: save-and-" + kindName);
                    saveProjectThen([this, proceed, kindName, guard](const bool saved) {
                        if (!guard.isAlive())
                        {
                            return;
                        }
                        if (!saved)
                        {
                            // Save failed or Save As was cancelled: the pending operation is aborted.
                            writeLastOperationBreadcrumb(
                                "unsaved-changes prompt: " + kindName
                                + " aborted (save failed or cancelled)");
                            return;
                        }
                        if (proceed != nullptr)
                        {
                            proceed();
                        }
                    });
                }
                else if (result == 2) // "X Without Saving"
                {
                    writeLastOperationBreadcrumb("unsaved-changes prompt: " + kindName
                                                 + "-without-saving");
                    writeAutosaveIfDirty(kindName + "-without-saving");
                    if (proceed != nullptr)
                    {
                        proceed();
                    }
                }
                else // Cancel
                {
                    writeLastOperationBreadcrumb("unsaved-changes prompt: cancelled (" + kindName
                                                 + ")");
                }
            }));
}

bool ProjectIoCoordinator::interceptQuitForUnsavedChanges()
{
    if (!isProjectDirty())
    {
        return false;
    }
    confirmUnsavedChangesThen(UnsavedGuardKind::QuitApp, [] {
        if (auto* app = juce::JUCEApplication::getInstance())
        {
            app->quit();
        }
    });
    return true;
}

juce::File ProjectIoCoordinator::resolveAutosaveTargetFile() const
{
    // Audio clip paths must stay `Audio/`-relative to the written file's folder, so a project
    // with a known on-disk location autosaves next to its own project file. The name is
    // project-specific ("<stem>_autosave.dalproj") so projects sharing a folder get distinct
    // autosaves; the stem comes from an existing on-disk file, so it needs no sanitizing.
    if (session_.hasKnownProjectFile())
    {
        const juce::File projectFile = session_.getCurrentProjectFile();
        return projectFile.getSiblingFile(projectFile.getFileNameWithoutExtension()
                                          + "_autosave.dalproj");
    }
    return defaultAppDataAutosaveFile();
}

void ProjectIoCoordinator::writeAutosaveIfDirty(const juce::String& reason)
{
    if (!isProjectDirty())
    {
        return;
    }
    (void)writeAutosaveNow(reason);
}

juce::Result ProjectIoCoordinator::writeAutosaveNow(const juce::String& reason)
{
    const double t0 = juce::Time::getMillisecondCounterHiRes();
    const juce::File autosaveFile = resolveAutosaveTargetFile();
    const juce::String projectDesc = session_.hasKnownProjectFile()
                                         ? session_.getCurrentProjectFile().getFullPathName()
                                         : juce::String("(never saved)");
    const juce::String targetKind
        = session_.hasKnownProjectFile() ? juce::String("project-specific") : juce::String("appdata");
    appendAutosaveDiagnosticLine("write begin (" + reason + "): project=" + projectDesc
                                 + " target=" + autosaveFile.getFullPathName()
                                 + " kind=" + targetKind
                                 + " dirty=" + (isProjectDirty() ? "yes" : "no"));

    juce::AudioIODevice* const device = deviceManager_.getCurrentAudioDevice();
    if (device == nullptr)
    {
        appendAutosaveDiagnosticLine("write FAIL: no active audio device (previous autosave, if "
                                     "any, left untouched)");
        return juce::Result::fail("no active audio device");
    }
    const double sampleRate = device->getCurrentSampleRate();
    if (!autosaveFile.getParentDirectory().isDirectory()
        && !autosaveFile.getParentDirectory().createDirectory())
    {
        appendAutosaveDiagnosticLine("write FAIL: cannot create folder "
                                     + autosaveFile.getParentDirectory().getFullPathName());
        return juce::Result::fail("cannot create autosave folder");
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
    std::optional<ProjectFileMainWindowBoundsV1> midiEditorWinBounds;
    if (callbacks_.getMidiEditorWindowBoundsForProjectSave != nullptr)
    {
        midiEditorWinBounds = callbacks_.getMidiEditorWindowBoundsForProjectSave();
    }
    std::optional<ProjectFileMidiEditorWorkspaceV1> midiEditorWorkspace;
    if (callbacks_.getMidiEditorWorkspaceForProjectSave != nullptr)
    {
        midiEditorWorkspace = callbacks_.getMidiEditorWorkspaceForProjectSave();
    }

    // `saveProjectToFile` records the written file as the current project on success; the autosave
    // must never hijack the user's normal save target, so restore it afterwards. The write itself
    // is atomic (temp file + move, see ProjectFile.cpp), so a crash mid-write can never leave a
    // half-written file at the autosave path.
    const juce::File normalProjectFile = session_.getCurrentProjectFile();
    writeLastOperationBreadcrumb("autosave start (" + reason + "): "
                                 + autosaveFile.getFullPathName());
    const juce::Result r = session_.saveProjectToFile(
        transport_,
        autosaveFile,
        sampleRate,
        &pluginHost_,
        ctlLookup,
        snapRoot.enabled,
        snapRoot.resolutionKey,
        mainWinBounds,
        midiEditorWinBounds,
        midiEditorWorkspace);
    session_.setCurrentProjectFile(normalProjectFile);

    const int elapsedMs = static_cast<int>(juce::Time::getMillisecondCounterHiRes() - t0 + 0.5);
    if (!r.wasOk())
    {
        // Dirty state is deliberately untouched: a failed autosave protects nothing, so the next
        // tick (or quit prompt) must still see the project as unsaved.
        appendAutosaveDiagnosticLine("write FAIL (" + reason + "): " + r.getErrorMessage()
                                     + " elapsedMs=" + juce::String(elapsedMs)
                                     + " (previous autosave, if any, left untouched)");
        writeLastOperationBreadcrumb("autosave failed (" + reason + ")");
        return r;
    }

    // Pointer file: line 1 = autosave path, line 2 = the project it belongs to (C5 metadata so a
    // stale autosave is distinguishable from the current project's).
    const bool pointerOk = autosavePointerFile().replaceWithText(
        autosaveFile.getFullPathName() + "\n" + projectDesc + "\n");
    lastAutosaveElapsedMs_ = elapsedMs;
    appendAutosaveDiagnosticLine("write ok (" + reason + "): " + autosaveFile.getFullPathName()
                                 + " kind=" + targetKind
                                 + " size=" + juce::String(autosaveFile.getSize())
                                 + " elapsedMs=" + juce::String(elapsedMs)
                                 + " pointer=" + (pointerOk ? "updated" : "WRITE FAILED"));
    if (elapsedMs > autosave_policy::kVerySlowWriteMs)
    {
        appendAutosaveDiagnosticLine("note: autosave write was slow (elapsedMs="
                                     + juce::String(elapsedMs)
                                     + "); periodic interval backs off to "
                                     + juce::String(autosave_policy::kIntervalVerySlowMs / 1000)
                                     + "s");
    }
    writeLastOperationBreadcrumb("autosave end ok (" + reason + ")");
    return juce::Result::ok();
}

// -----------------------------------------------------------------------------
// Stability C5: periodic autosave tick
// -----------------------------------------------------------------------------

juce::String ProjectIoCoordinator::periodicAutosaveBlockReason() const
{
    // Scenario runs drive autosave explicitly (via forceAutosaveNowForStabilityTest); a periodic
    // write in the middle of a delete/load loop would make runs nondeterministic.
    if (isStabilityTestModeActive())
    {
        return "stability-test-mode";
    }
    // Covers the recovery prompt, unsaved-changes prompts, alerts, and modal pickers. Load/save/
    // export/mixdown/undo/redo/track-delete all run synchronously on the message thread, so this
    // timer cannot fire in the middle of them.
    if (juce::ModalComponentManager::getInstance()->getNumModalComponents() > 0)
    {
        return "modal dialog open";
    }
    if (getAutosaveBlockReason_ != nullptr)
    {
        const juce::String appReason = getAutosaveBlockReason_();
        if (appReason.isNotEmpty())
        {
            return appReason;
        }
    }
    return {};
}

void ProjectIoCoordinator::timerCallback()
{
    namespace policy = autosave_policy;
    if (!isProjectDirty())
    {
        // Nothing to protect; drop any schedule so the next dirty phase starts from the initial
        // delay again. Stay silent so autosave-diag.log does not fill up while idle.
        nextPeriodicAutosaveDueMs_ = 0;
        return;
    }
    const juce::int64 nowMs = juce::Time::currentTimeMillis();
    if (nextPeriodicAutosaveDueMs_ == 0)
    {
        // First tick that observes the dirty state: schedule, do not write yet. Combined with
        // the minute tick this puts the first write 60-120s after the project became dirty.
        nextPeriodicAutosaveDueMs_ = nowMs + policy::kFirstDelayMs;
        appendAutosaveDiagnosticLine(
            "tick: dirty observed; first autosave due "
            + juce::Time(nextPeriodicAutosaveDueMs_).formatted("%H:%M:%S"));
        return;
    }
    if (nowMs < nextPeriodicAutosaveDueMs_)
    {
        appendAutosaveDiagnosticLine("tick skipped: not due nextDue="
                                     + juce::Time(nextPeriodicAutosaveDueMs_).formatted("%H:%M:%S")
                                     + " (dirty=yes)");
        return;
    }
    const juce::String blockReason = periodicAutosaveBlockReason();
    if (blockReason.isNotEmpty())
    {
        // Skip (never queue) and keep the due time in the past: the next tick retries in 60s.
        appendAutosaveDiagnosticLine("tick skipped: blocked reason=" + blockReason
                                     + " (dirty=yes, autosave due; retrying next tick)");
        return;
    }
    const juce::Result r = writeAutosaveNow("periodic");
    if (r.wasOk())
    {
        const int nextIntervalMs = policy::intervalForElapsedMs(lastAutosaveElapsedMs_);
        nextPeriodicAutosaveDueMs_ = nowMs + nextIntervalMs;
        appendAutosaveDiagnosticLine(
            "periodic schedule: elapsedMs=" + juce::String(lastAutosaveElapsedMs_)
            + " nextIntervalSec=" + juce::String(nextIntervalMs / 1000) + " nextDue="
            + juce::Time(nextPeriodicAutosaveDueMs_).formatted("%H:%M:%S"));
    }
    else
    {
        // Failed write protected nothing; retry on the next tick (due time stays in the past).
        appendAutosaveDiagnosticLine("periodic schedule: write failed; retrying next tick");
    }
}

bool ProjectIoCoordinator::forceAutosaveNowForStabilityTest(juce::String& failReasonOut)
{
    if (!isProjectDirty())
    {
        failReasonOut = "project is not dirty";
        return false;
    }
    if (getAutosaveBlockReason_ != nullptr)
    {
        const juce::String appReason = getAutosaveBlockReason_();
        if (appReason.isNotEmpty())
        {
            failReasonOut = "blocked: " + appReason;
            return false;
        }
    }
    const juce::Result r = writeAutosaveNow("stability-test-forced");
    if (!r.wasOk())
    {
        failReasonOut = r.getErrorMessage();
        return false;
    }
    return true;
}

bool ProjectIoCoordinator::recoverAutosaveNowForStabilityTest(juce::String& failReasonOut)
{
    const juce::File autosaveFile = findExistingAutosaveFile();
    if (!autosaveFile.existsAsFile())
    {
        failReasonOut = "no autosave file found";
        return false;
    }
    {
        juce::StringArray pointerLines;
        autosavePointerFile().readLines(pointerLines);
        const juce::String owner = pointerLines.size() > 1 ? pointerLines[1].trim()
                                                           : juce::String("(unknown/legacy)");
        appendAutosaveDiagnosticLine("recovery (stability test): loading "
                                     + autosaveFile.getFullPathName() + " owner=" + owner);
    }
    // Invariant 8 tolerates the save path *being* the autosave only while this flag is set.
    stability_invariants::setAutosaveRecoveryInProgress(true);
    loadProjectFromFile(autosaveFile);
    if (session_.getCurrentProjectFile() != autosaveFile)
    {
        stability_invariants::setAutosaveRecoveryInProgress(false);
        appendAutosaveDiagnosticLine("recovery (stability test): autosave load FAILED");
        failReasonOut = "autosave load failed (current project file was not updated)";
        return false;
    }
    // Same policy as the recovery prompt's "Recover" button: never claim the original save path.
    session_.setCurrentProjectFile(juce::File());
    stability_invariants::setAutosaveRecoveryInProgress(false);
    markProjectDirtyFromEdit();
    appendAutosaveDiagnosticLine(
        "recovery (stability test): loaded; save path cleared (Save goes through Save As)");
    (void) stability_invariants::runRegisteredStabilityInvariantsCheck("autosave-recovery-end");
    return true;
}

juce::File ProjectIoCoordinator::getCurrentAutosavePathForDiagnostics() const
{
    return resolveAutosaveTargetFile();
}

juce::File ProjectIoCoordinator::getAutosavePointerPathForDiagnostics()
{
    return autosavePointerFile();
}

void ProjectIoCoordinator::deleteAutosaveArtifactsAfterSuccessfulSave()
{
    const juce::File pointer = autosavePointerFile();
    juce::File recorded;
    juce::String recordedOwner;
    if (pointer.existsAsFile())
    {
        // Line 1 = autosave path; line 2 (optional) = the original project it belongs to.
        juce::StringArray lines;
        pointer.readLines(lines);
        const juce::String s = lines.size() > 0 ? lines[0].trim() : juce::String{};
        if (s.isNotEmpty() && juce::File::isAbsolutePath(s))
        {
            recorded = juce::File(s);
        }
        recordedOwner = lines.size() > 1 ? lines[1].trim() : juce::String{};
    }
    // With project-specific autosave names, the recorded autosave can belong to a *different*
    // project (e.g. the user saved project B while project A's autosave is still recorded).
    // Only delete the recorded file when the pointer's owner line matches this project (or is
    // missing, for pre-C5/legacy pointers); the pointer itself is always cleared.
    const juce::String currentProjectPath = session_.hasKnownProjectFile()
                                                ? session_.getCurrentProjectFile().getFullPathName()
                                                : juce::String{};
    if (recorded.getFullPathName().isNotEmpty() && recordedOwner.isNotEmpty()
        && currentProjectPath.isNotEmpty() && recordedOwner != currentProjectPath)
    {
        appendAutosaveDiagnosticLine("cleanup: recorded autosave kept (belongs to different "
                                     "project: " + recordedOwner + ")");
        recorded = juce::File{};
    }
    // Legacy sibling "autosave.dalproj" (pre-project-specific naming) is also cleaned up, so an
    // old autosave next to this project cannot trigger recovery prompts after a successful save.
    const juce::File legacySibling = session_.hasKnownProjectFile()
                                         ? session_.getCurrentProjectFile().getSiblingFile(
                                               "autosave.dalproj")
                                         : juce::File{};
    bool deletedAny = false;
    for (const juce::File& f :
         { recorded, defaultAppDataAutosaveFile(), resolveAutosaveTargetFile(), legacySibling })
    {
        if (f.getFullPathName().isNotEmpty() && f.existsAsFile() && f.deleteFile())
        {
            deletedAny = true;
        }
    }
    (void)pointer.deleteFile();
    if (deletedAny)
    {
        appendProjectSaveDiagnosticLine("autosave cleared after successful save");
        appendAutosaveDiagnosticLine("cleared after successful manual save (autosave + pointer)");
    }
}

void ProjectIoCoordinator::offerAutosaveRecoveryOnStartup(const bool commandLineProjectOpenQueued)
{
    const juce::File autosaveFile = findExistingAutosaveFile();
    if (!autosaveFile.existsAsFile())
    {
        return;
    }
    if (commandLineProjectOpenQueued)
    {
        // The explicitly requested project wins; the autosave is kept for the next plain startup.
        writeLastOperationBreadcrumb("autosave present but command-line project open takes priority: "
                                     + autosaveFile.getFullPathName());
        appendProjectSaveDiagnosticLine("recovery: skipped (command-line project open queued)");
        return;
    }
    writeLastOperationBreadcrumb("autosave recovery prompt shown: " + autosaveFile.getFullPathName());
    juce::AlertWindow::showYesNoCancelBox(
        juce::AlertWindow::QuestionIcon,
        "Recover autosaved project",
        "An autosaved project was found:\n" + autosaveFile.getFullPathName() + "\n\nRecover it?",
        "Recover",
        "Ignore",
        "Delete Autosave",
        nullptr,
        juce::ModalCallbackFunction::create(
            [this, autosaveFile, guard = asyncLifetime_.guard()](const int result) {
                if (!guard.isAlive())
                {
                    juce::Logger::writeToLog("[stale-async] skipped: autosave recovery prompt result");
                    return;
                }
                if (result == 1) // Recover
                {
                    writeLastOperationBreadcrumb("autosave recovery: recover chosen");
                    // Invariant 8 tolerates the transient "save path is the autosave" state
                    // only while this flag is set (cleared right after the path is detached).
                    stability_invariants::setAutosaveRecoveryInProgress(true);
                    loadProjectFromFile(autosaveFile);
                    if (session_.getCurrentProjectFile() == autosaveFile)
                    {
                        // Loaded OK. Detach the autosave path so plain Save goes through Save As
                        // (the original project is never overwritten silently), and flag the
                        // recovered state as unsaved so quit/load prompts protect it.
                        session_.setCurrentProjectFile(juce::File());
                        stability_invariants::setAutosaveRecoveryInProgress(false);
                        markProjectDirtyFromEdit();
                        appendProjectSaveDiagnosticLine(
                            "recovery: autosave loaded; save path cleared (use Save As)");
                        appendAutosaveDiagnosticLine(
                            "recovery (prompt): loaded " + autosaveFile.getFullPathName()
                            + "; save path cleared (Save goes through Save As)");
                        // Stability C3: verify invariants after autosave recovery completed.
                        (void) stability_invariants::runRegisteredStabilityInvariantsCheck(
                            "autosave-recovery-end");
                    }
                    else
                    {
                        stability_invariants::setAutosaveRecoveryInProgress(false);
                        appendProjectSaveDiagnosticLine("recovery: autosave load failed");
                        appendAutosaveDiagnosticLine("recovery (prompt): autosave load FAILED: "
                                                     + autosaveFile.getFullPathName());
                    }
                }
                else if (result == 2) // Ignore
                {
                    writeLastOperationBreadcrumb("autosave recovery: ignored (file kept)");
                    appendProjectSaveDiagnosticLine(
                        "recovery: ignored; autosave kept for next startup");
                    appendAutosaveDiagnosticLine(
                        "recovery (prompt): ignored; autosave kept for next startup: "
                        + autosaveFile.getFullPathName());
                }
                else // Delete Autosave
                {
                    const bool deleted = autosaveFile.deleteFile();
                    const bool pointerDeleted = autosavePointerFile().deleteFile();
                    writeLastOperationBreadcrumb("autosave recovery: autosave deleted");
                    appendProjectSaveDiagnosticLine(juce::String("recovery: delete autosave ")
                                                    + (deleted ? "ok" : "FAILED"));
                    appendAutosaveDiagnosticLine(juce::String("recovery (prompt): delete autosave ")
                                                 + (deleted ? "ok" : "FAILED") + ", pointer "
                                                 + (pointerDeleted ? "deleted" : "not deleted"));
                }
            }));
}
