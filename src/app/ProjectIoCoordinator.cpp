#include "app/ProjectIoCoordinator.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include "diagnostics/ProjectLoadDiagnosticLog.h"
#include "diagnostics/StabilityDiagnosticLog.h"
#include "diagnostics/StabilityInvariants.h"
#include "diagnostics/StabilityScenarioRunner.h"
#include "domain/AudioClip.h"
#include "domain/Session.h"
#include "domain/SessionSnapshot.h"
#include "domain/Track.h"
#include "engine/PlaybackEngine.h"
#include "instruments/InstrumentTrackController.h"
#include "io/AudioFileLoader.h"
#include "io/ProjectFile.h"
#include "io/ProxyMetadataCheckpoint.h"
#include "plugins/ExperimentalInstrumentHost.h"
#include "plugins/PluginInsertHost.h"
#include "transport/Transport.h"

#include <atomic>
#include <functional>
#include <thread>
#include <unordered_set>

namespace
{
    /// "<text>…" with a real U+2026 (juce::String treats `const char*` as ASCII, so the UTF-8 bytes
    /// go through CharPointer_UTF8 explicitly - same convention as the rest of the UI).
    [[nodiscard]] juce::String withEllipsisUtf8(const char* asciiText)
    {
        return juce::String(asciiText) + juce::String(juce::CharPointer_UTF8("\xe2\x80\xa6"));
    }

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

    /// Plugin-state capture window around every project write (Save, Save As, autosave): the
    /// experimental read-ahead worker is quiesced — gapless drain while playing, acknowledged
    /// pause otherwise — before `exportChain` reads `getStateInformation`, and resumed after
    /// the file is written (docs/READAHEAD_PROTOTYPE.md §9). A no-op without the CLI flag.
    class ScopedPluginStateCaptureWindow final
    {
    public:
        explicit ScopedPluginStateCaptureWindow(PlaybackEngine& playbackEngine) noexcept
            : playbackEngine_(playbackEngine)
        {
            playbackEngine_.beginPluginStateCaptureWindow();
        }
        ~ScopedPluginStateCaptureWindow() noexcept
        {
            playbackEngine_.endPluginStateCaptureWindow();
        }
        ScopedPluginStateCaptureWindow(const ScopedPluginStateCaptureWindow&) = delete;
        ScopedPluginStateCaptureWindow& operator=(const ScopedPluginStateCaptureWindow&) = delete;

    private:
        PlaybackEngine& playbackEngine_;
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

    // NOTE: successful Save intentionally shows NO informational dialog for generic VST3
    // tracks without a loaded plugin — a missing Primary is an expected collaboration
    // scenario (steering §12). Descriptors, saved state, MIDI and proxy metadata are
    // persisted exactly as always; per-track status stays visible in the Inspector, and
    // actual save FAILURES are still reported through the WarningIcon error dialogs below.

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
    if (loadJob_ != nullptr)
    {
        appendProjectSaveDiagnosticLine("save refused: project load in progress");
        reportDone(false);
        return;
    }
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
        std::optional<ProjectFileTrackRowHeightsV1> trackRowHeights;
        if (callbacks_.getTrackRowHeightsForProjectSave != nullptr)
        {
            trackRowHeights = callbacks_.getTrackRowHeightsForProjectSave();
        }
        writeLastOperationBreadcrumb("project save start: "
                                     + session_.getCurrentProjectFile().getFullPathName());
        juce::Result r = juce::Result::ok();
        {
            const ScopedPluginStateCaptureWindow captureWindow(playbackEngine_);
            r = session_.saveProjectToFile(
                transport_,
                session_.getCurrentProjectFile(),
                sampleRate,
                &pluginHost_,
                ctlLookup,
                snapRoot.enabled,
                snapRoot.resolutionKey,
                mainWinBounds,
                midiEditorWinBounds,
                midiEditorWorkspace,
                trackRowHeights);
        }
        if (!r.wasOk())
        {
            writeLastOperationBreadcrumb("project save failed");
            // First-generation pairing: a failed Save makes the association between any
            // captured candidate and the on-disk blob unprovable — invalidate it.
            noteMainProjectSaveOutcomeForInstrumentControllers(false);
            juce::AlertWindow::showMessageBoxAsync(
                juce::AlertWindow::WarningIcon, "Save project", r.getErrorMessage());
            reportDone(false);
        }
        else
        {
            writeLastOperationBreadcrumb("project save end ok");
            markProjectCleanNow();
            // P1 first-generation pairing: promote each controller's captured blob-revision
            // candidate — the blob it describes is now provably inside the main `.dalproj`.
            noteMainProjectSaveOutcomeForInstrumentControllers(true);
            // P1 acceptance correction: record the freshly written file's identity so a later
            // automatic proxy-metadata checkpoint can prove the file is still this exact save.
            refreshKnownProjectDiskIdentity();
            deleteAutosaveArtifactsAfterSuccessfulSave();
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
        std::optional<ProjectFileTrackRowHeightsV1> trackRowHeights;
        if (callbacks_.getTrackRowHeightsForProjectSave != nullptr)
        {
            trackRowHeights = callbacks_.getTrackRowHeightsForProjectSave();
        }
        writeLastOperationBreadcrumb("project save start: " + projectFile.getFullPathName());
        juce::Result r = juce::Result::ok();
        {
            const ScopedPluginStateCaptureWindow captureWindow(playbackEngine_);
            r = session_.saveProjectToFile(
                transport_,
                projectFile,
                sampleRate,
                &pluginHost_,
                ctlLookup,
                snapRoot.enabled,
                snapRoot.resolutionKey,
                mainWinBounds,
                midiEditorWinBounds,
                midiEditorWorkspace,
                trackRowHeights);
        }
        if (!r.wasOk())
        {
            writeLastOperationBreadcrumb("project save failed");
            noteMainProjectSaveOutcomeForInstrumentControllers(false);
            juce::AlertWindow::showMessageBoxAsync(
                juce::AlertWindow::WarningIcon, "Save project", r.getErrorMessage());
            reportDone(false);
        }
        else
        {
            writeLastOperationBreadcrumb("project save end ok");
            markProjectCleanNow();
            // First-time Save As also writes the captured blob into a real main `.dalproj`.
            noteMainProjectSaveOutcomeForInstrumentControllers(true);
            // P1 acceptance correction: the new project file's identity (Save As included —
            // a later checkpoint must never write into a replaced/different project file).
            refreshKnownProjectDiskIdentity();
            deleteAutosaveArtifactsAfterSuccessfulSave();
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
    if (loadJob_ != nullptr)
    {
        appendProjectLoadDiagnosticLine("load: chooser refused (a load is already in progress)");
        return;
    }
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

// =============================================================================
// Staged project load - units, progress window, drives (see the header's public section)
// =============================================================================

/// Modal progress window: phase + detail labels and a `juce::ProgressBar` (indeterminate when the
/// fraction is negative). No Cancel (nothing here could honour it); the close button is inert.
class ProjectIoCoordinator::LoadProgressWindow final : public juce::DialogWindow
{
public:
    LoadProgressWindow()
        : juce::DialogWindow("Opening project", juce::Colour(0xff2b2d31), /*escapeKeyTriggersCloseButton*/ false,
                             /*addToDesktop*/ true)
    {
        auto content = std::make_unique<Content>(progressValue_);
        content_ = content.get();
        setContentOwned(content.release(), true);
        setUsingNativeTitleBar(false);
        setTitleBarButtonsRequired(0, false);
        setResizable(false, false);
        setAlwaysOnTop(true);
    }

    void closeButtonPressed() override {} // no cancel path exists; the load always finishes or fails

    void setProgress(const juce::String& phase, const juce::String& detail, const double fraction)
    {
        progressValue_ = fraction < 0.0 ? -1.0 : juce::jlimit(0.0, 1.0, fraction);
        if (content_ != nullptr)
        {
            content_->phase.setText(phase, juce::dontSendNotification);
            content_->detail.setText(detail, juce::dontSendNotification);
            content_->repaint();
        }
        repaint();
        // Paint NOW (no nested message loop): the next unit may block for a plug-in's whole
        // instantiation, and the window must already show what is being loaded.
        if (juce::ComponentPeer* peer = getPeer())
        {
            peer->performAnyPendingRepaintsNow();
        }
    }

    [[nodiscard]] juce::String phaseText() const { return content_ != nullptr ? content_->phase.getText() : juce::String(); }
    [[nodiscard]] juce::String detailText() const { return content_ != nullptr ? content_->detail.getText() : juce::String(); }
    [[nodiscard]] double fraction() const noexcept { return progressValue_; }

private:
    struct Content final : juce::Component
    {
        explicit Content(double& value) : bar(value)
        {
            phase.setFont(juce::FontOptions(16.0f, juce::Font::bold));
            phase.setJustificationType(juce::Justification::centredLeft);
            detail.setFont(juce::FontOptions(13.0f));
            detail.setJustificationType(juce::Justification::centredLeft);
            detail.setColour(juce::Label::textColourId, juce::Colours::whitesmoke.withAlpha(0.85f));
            bar.setPercentageDisplay(false);
            addAndMakeVisible(phase);
            addAndMakeVisible(detail);
            addAndMakeVisible(bar);
            setSize(460, 118);
        }
        void resized() override
        {
            auto r = getLocalBounds().reduced(16, 14);
            phase.setBounds(r.removeFromTop(24));
            r.removeFromTop(4);
            detail.setBounds(r.removeFromTop(20));
            r.removeFromTop(12);
            bar.setBounds(r.removeFromTop(18));
        }
        juce::Label phase;
        juce::Label detail;
        juce::ProgressBar bar;
    };

    double progressValue_ = -1.0;
    Content* content_ = nullptr;
};

struct ProjectIoCoordinator::LoadJob
{
    enum class Unit
    {
        ParseAndDecode,       ///< background thread (Interactive) or inline (Synchronous)
        ClosePrevious,        ///< stop, gate, retire the previous project's instrument runtimes
        ApplyModel,           ///< session model replace + inventory
        RestoreInstrument,    ///< one `experimentalInstrumentTracks` row per unit
        OrphansAndUi,         ///< orphan lanes, MIDI content controllers, editor sync, UI refresh
        RestoreInsertChain,   ///< one row's insert chain per unit
        Finalize,
        Done,
    };

    juce::File file;
    LoadDrive drive = LoadDrive::Interactive;
    double sampleRate = 0.0;
    std::uint64_t generation = 0; ///< `Session::beginProjectLoadGeneration` (stale-guards every async unit)
    std::uint64_t serial = 0;     ///< monotonically increasing per load (stale-guards worker posts)
    std::function<void(bool ok)> completion; ///< per-load continuation (recovery prompt), fired from finish
    Unit unit = Unit::ParseAndDecode;

    // Background stage results (written by the worker, read on the message thread after its post).
    std::thread worker;
    std::atomic<bool> workerDone{ false };
    juce::Result parseResult = juce::Result::ok();
    ProjectFileV1 parsed;
    Session::PreDecodedMaterialByPath material;
    int audioFilesTotal = 0;
    std::atomic<int> audioFilesDecoded{ 0 };

    // Message-thread stage state.
    std::unique_ptr<ScopedInstrumentProcessingLoadGate> gate;
    juce::StringArray skipped;
    juce::String infoNote;
    juce::String instrumentAutoloadNoteAcc;
    std::vector<const ProjectFileExperimentalInstrumentTrackV1*> instrumentRows; ///< enabled rows, file order
    int instrumentIndex = 0;
    std::vector<Session::PendingPluginInsertRestore> insertRows;
    int insertIndex = 0;
    bool announced = false; ///< Interactive: a unit is first announced (painted), then executed
    double startMs = 0.0;
    juce::String lastPhase;
    juce::String lastDetail;
    double lastFraction = -1.0;

    ~LoadJob()
    {
        if (worker.joinable())
        {
            worker.join();
        }
    }
};

ProjectIoCoordinator::~ProjectIoCoordinator()
{
    stopTimer();
    // A load still in flight at shutdown: the gate is released by the job's destructor order
    // (gate before worker join); async units check the lifetime guard and never run after this.
    loadJob_.reset();
    loadProgressWindow_.reset();
}

juce::String ProjectIoCoordinator::loadProgressTextForDiagnostics() const
{
    if (loadJob_ == nullptr)
    {
        return {};
    }
    return loadJob_->lastPhase + " | " + loadJob_->lastDetail + " | "
           + (loadJob_->lastFraction < 0.0 ? juce::String("indeterminate")
                                           : juce::String(loadJob_->lastFraction * 100.0, 1) + "%")
           + " | elapsedMs=" + juce::String((int) (juce::Time::getMillisecondCounterHiRes() - loadJob_->startMs))
           + " | window=" + (loadProgressWindow_ != nullptr && loadProgressWindow_->isShowing() ? "showing" : "none");
}

void ProjectIoCoordinator::loadProjectFromFile(const juce::File& projectFile)
{
    loadProjectFromFile(projectFile,
                        isStabilityTestModeActive() ? LoadDrive::Synchronous : LoadDrive::Interactive);
}

void ProjectIoCoordinator::loadProjectFromFile(const juce::File& projectFile, const LoadDrive drive)
{
    if (loadJob_ != nullptr)
    {
        appendProjectLoadDiagnosticLine("load: refused (a load is already in progress) file=\""
                                        + projectFile.getFullPathName() + "\"");
        return;
    }
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
    loadJob_start(projectFile, drive, {});
}

void ProjectIoCoordinator::loadProjectFromFileThen(const juce::File& projectFile,
                                                   std::function<void(bool ok)> completion)
{
    if (loadJob_ != nullptr || !projectFile.existsAsFile() || deviceManager_.getCurrentAudioDevice() == nullptr)
    {
        loadProjectFromFile(projectFile); // reports the refusal / alert exactly like the plain entry
        if (completion)
        {
            completion(false);
        }
        return;
    }
    loadJob_start(projectFile,
                  isStabilityTestModeActive() ? LoadDrive::Synchronous : LoadDrive::Interactive,
                  std::move(completion));
}

void ProjectIoCoordinator::loadJob_setProgress(const juce::String& phase, const juce::String& detail,
                                               const double fraction)
{
    if (loadJob_ == nullptr)
    {
        return;
    }
    loadJob_->lastPhase = phase;
    loadJob_->lastDetail = detail;
    loadJob_->lastFraction = fraction;
    if (loadProgressWindow_ != nullptr)
    {
        loadProgressWindow_->setProgress(phase, detail, fraction);
    }
}

void ProjectIoCoordinator::loadJob_start(const juce::File& projectFile, const LoadDrive drive,
                                         std::function<void(bool ok)> completion)
{
    auto job = std::make_unique<LoadJob>();
    job->file = projectFile;
    job->drive = drive;
    job->sampleRate = deviceManager_.getCurrentAudioDevice()->getCurrentSampleRate();
    job->startMs = juce::Time::getMillisecondCounterHiRes();
    job->serial = ++loadJobSerialCounter_;
    job->completion = std::move(completion);
    loadJob_ = std::move(job);
    writeLastOperationBreadcrumb("project load start: " + projectFile.getFullPathName());
    appendProjectLoadDiagnosticLine(juce::String("load: staged load begin drive=")
                                    + (drive == LoadDrive::Interactive ? "interactive" : "synchronous")
                                    + " file=\"" + projectFile.getFullPathName() + "\"");

    if (drive == LoadDrive::Interactive)
    {
        loadProgressWindow_ = std::make_unique<LoadProgressWindow>();
        juce::Component* anchor = callbacks_.getProgressWindowAnchor ? callbacks_.getProgressWindowAnchor() : nullptr;
        if (anchor != nullptr)
        {
            loadProgressWindow_->centreAroundComponent(anchor, loadProgressWindow_->getWidth(),
                                                       loadProgressWindow_->getHeight());
        }
        else
        {
            loadProgressWindow_->centreWithSize(loadProgressWindow_->getWidth(), loadProgressWindow_->getHeight());
        }
        loadProgressWindow_->setVisible(true);
        // Modal: mouse input to every other window is refused while the load runs; no nested
        // message loop is ever run (the units are scheduled on the normal loop).
        loadProgressWindow_->enterModalState(true, nullptr, false);
        loadProgressWindow_->toFront(true);
    }
    loadJob_setProgress(withEllipsisUtf8("Reading project"), projectFile.getFileName(), -1.0);

    // Unit 1 - parse + decode. The decode set is the UNIQUE audio files of the project (a file
    // shared by many clips is decoded once); results are handed to `applyLoadedProjectModel`.
    const auto parseAndDecode = [](LoadJob& j, const std::function<void(int done, int total, const juce::String& name)>& progress) {
        j.parseResult = readProjectFile(j.file, j.parsed);
        if (!j.parseResult.wasOk())
        {
            return;
        }
        std::vector<juce::File> files;
        const juce::File folder = j.file.getParentDirectory();
        for (const auto& tr : j.parsed.tracks)
        {
            if (!tr.kind.equalsIgnoreCase("audio"))
            {
                continue;
            }
            for (const auto& c : tr.clips)
            {
                const juce::File f = Session::resolveProjectAudioFile(c.sourcePath, folder);
                if (f == juce::File() || !f.existsAsFile())
                {
                    continue; // the model apply reports the skip with the real reason
                }
                bool seen = false;
                for (const auto& k : files)
                {
                    if (k == f) { seen = true; break; }
                }
                if (!seen)
                {
                    files.push_back(f);
                }
            }
        }
        j.audioFilesTotal = (int)files.size();
        int done = 0;
        for (const juce::File& f : files)
        {
            progress(done, (int)files.size(), f.getFileName());
            std::unique_ptr<AudioClip> clip;
            if (AudioFileLoader::loadFromFile(f, j.sampleRate, clip).wasOk() && clip != nullptr)
            {
                j.material.emplace(f.getFullPathName(), std::shared_ptr<const AudioClip>(std::move(clip)));
            }
            ++done;
            j.audioFilesDecoded.store(done, std::memory_order_relaxed);
        }
    };

    if (drive == LoadDrive::Synchronous)
    {
        parseAndDecode(*loadJob_, [](int, int, const juce::String&) {});
        loadJob_->workerDone.store(true, std::memory_order_release);
        loadJob_->unit = LoadJob::Unit::ClosePrevious;
        // Run every remaining unit to completion before returning (scenario contract).
        while (loadJob_ != nullptr)
        {
            loadJob_runNextUnit();
        }
        return;
    }

    LoadJob* const jobPtr = loadJob_.get();
    const std::uint64_t jobStamp = jobPtr->serial;
    auto guard = asyncLifetime_.guard();
    loadJob_->worker = std::thread([this, jobPtr, jobStamp, guard, parseAndDecode] {
        const auto post = [this, jobPtr, jobStamp, guard](std::function<void()> fn) {
            juce::MessageManager::callAsync([this, jobPtr, jobStamp, guard, fn = std::move(fn)] {
                if (!guard.isAlive() || loadJob_.get() != jobPtr
                    || loadJob_->serial != jobStamp)
                {
                    return; // coordinator gone or a different load - nothing to update
                }
                fn();
            });
        };
        parseAndDecode(*jobPtr, [this, &post](const int done, const int total, const juce::String& name) {
            post([this, done, total, name] {
                loadJob_setProgress(withEllipsisUtf8("Loading audio material"),
                                    juce::String(done + 1) + " of " + juce::String(total) + ": " + name,
                                    total > 0 ? (double)done / (double)total : -1.0);
            });
        });
        jobPtr->workerDone.store(true, std::memory_order_release);
        post([this] {
            if (loadJob_ == nullptr || !loadJob_->workerDone.load(std::memory_order_acquire))
            {
                return;
            }
            loadJob_->unit = LoadJob::Unit::ClosePrevious;
            loadJob_runNextUnit();
        });
    });
}

void ProjectIoCoordinator::loadJob_scheduleNextUnit()
{
    if (loadJob_ == nullptr || loadJob_->drive != LoadDrive::Interactive)
    {
        return; // Synchronous: the start loop drives
    }
    // A short timer (not callAsync) so the queue drains between units: WM_PAINT and the UI
    // timers get their turn, which is what keeps the main window painting and responsive.
    LoadJob* const jobPtr = loadJob_.get();
    const std::uint64_t jobStamp = loadJob_->serial;
    juce::Timer::callAfterDelay(4, [this, jobPtr, jobStamp, guard = asyncLifetime_.guard()] {
        // Pointer AND serial: a later job could be allocated at the same address.
        if (!guard.isAlive() || loadJob_.get() != jobPtr || loadJob_->serial != jobStamp)
        {
            return;
        }
        loadJob_runNextUnit();
    });
}

void ProjectIoCoordinator::loadJob_runNextUnit()
{
    if (loadJob_ == nullptr)
    {
        return;
    }
    LoadJob& j = *loadJob_;
    const double sampleRate = j.sampleRate;
    const juce::File f = j.file;
    const bool interactive = j.drive == LoadDrive::Interactive;

    // Interactive: every unit is announced in its own turn (the window paints the text), then
    // executed in the next turn. The Synchronous drive skips the extra turn.
    const auto announce = [&](const juce::String& phase, const juce::String& detail, const double fraction) -> bool {
        if (!interactive)
        {
            loadJob_setProgress(phase, detail, fraction);
            return true; // execute now
        }
        if (j.announced)
        {
            j.announced = false;
            return true; // announced last turn: execute now
        }
        loadJob_setProgress(phase, detail, fraction);
        j.announced = true;
        loadJob_scheduleNextUnit();
        return false; // paint first
    };

    switch (j.unit)
    {
        case LoadJob::Unit::ParseAndDecode:
            return; // the worker posts ApplyModel when done

        case LoadJob::Unit::ClosePrevious:
        {
            if (!j.parseResult.wasOk())
            {
                writeLastOperationBreadcrumb("project load failed (parse)");
                loadJob_finish(false, j.parseResult.getErrorMessage());
                return;
            }
            // Retiring a large previous project (64 instrument hosts, 18 AmpliTube inserts) is
            // seconds of synchronous plug-in teardown; it gets its own announced phase so the
            // window says what is happening instead of "Preparing project".
            if (!announce(withEllipsisUtf8("Closing previous project"), juce::String(), -1.0))
            {
                return;
            }
            appendProjectLoadDiagnosticLine("load: parsed file=\"" + f.getFullPathName() + "\" experimentalInstrumentTracks="
                                            + juce::String((int)j.parsed.experimentalInstrumentTracks.size())
                                            + " predecodedAudioFiles=" + juce::String((int)j.material.size()) + "/"
                                            + juce::String(j.audioFilesTotal));
            transport_.requestPlaybackIntent(PlaybackIntent::Stopped);
            appendProjectLoadDiagnosticLine("load: transport stopped");

            // Monitor is a runtime-only control and defaults OFF on project opening/restoration -
            // never carry live input monitoring across a project replacement.
            playbackEngine_.clearAllInputMonitoring();

            // P1H project replacement (--13.3): obsolete/cancel every proxy job of the OLD project
            // and drop the runtime-only policy timers BEFORE the runtimes they reference are
            // cleared. Queued work is re-derivable from fingerprints on reopen; nothing waits.
            if (callbacks_.onProjectAboutToBeReplaced != nullptr)
            {
                callbacks_.onProjectAboutToBeReplaced();
            }

            // The gate (instrument processing suspended + idle ack) is held for the WHOLE staged
            // load: plug-in instantiation / state restore never overlaps the realtime instrument
            // section or its render workers (jobs run inside the callback, which skips the section).
            j.gate = std::make_unique<ScopedInstrumentProcessingLoadGate>(playbackEngine_, session_);
            j.generation = j.gate->loadGeneration();

            appendProjectLoadDiagnosticLine("load: clearExperimentalInstrumentRuntimes begin");
            callbacks_.clearExperimentalInstrumentRuntimesPreserveBridgeOnly();
            appendProjectLoadDiagnosticLine("load: clearExperimentalInstrumentRuntimes end");
            j.unit = LoadJob::Unit::ApplyModel;
            loadJob_scheduleNextUnit(); // Synchronous drive: no-op, the start loop runs it
            return;
        }

        case LoadJob::Unit::ApplyModel:
        {
            if (!announce(withEllipsisUtf8("Preparing project"), f.getFileName(), -1.0))
            {
                return;
            }
            appendProjectLoadDiagnosticLine("load: before applyLoadedProjectModel (session model replace)");
            appendProjectLoadDiagnosticLine("load: applyLoadedProjectModel begin");
            const juce::Result r = session_.applyLoadedProjectModel(
                transport_, f, j.parsed, sampleRate, j.skipped, j.infoNote, &pluginHost_, j.generation,
                &j.material, &j.insertRows);
            if (!r.wasOk())
            {
                writeLastOperationBreadcrumb("project load failed (apply model)");
                loadJob_finish(false, r.getErrorMessage());
                return;
            }
            j.material.clear(); // the session owns the material now
            appendProjectLoadDiagnosticLine("load: applyLoadedProjectModel end");
            appendProjectLoadDiagnosticLine("load: after applyLoadedProjectModel (session model replaced)");
            // The routing plan is published separately from the snapshot; rebuild it NOW so the
            // audio callback never runs the restore phases (seconds, not one block) with a plan
            // built from the previous project against the new snapshot's rows.
            playbackEngine_.rebuildRoutingPlanFromSession();
            callbacks_.restoreSnapProjectRootFieldsToUi({ j.parsed.snapEnabled, j.parsed.snapResolution });

            // Inventory log (unchanged diagnostics) + the ordered restore list.
            j.instrumentRows.clear();
            if (!j.parsed.experimentalInstrumentTracks.empty())
            {
                std::unordered_set<TrackId> seenExperimentalTrackIds;
                seenExperimentalTrackIds.reserve(j.parsed.experimentalInstrumentTracks.size());
                for (const auto& etRow : j.parsed.experimentalInstrumentTracks)
                {
                    const TrackId rowTid = etRow.trackId;
                    const juce::String rowName = etRow.name.isNotEmpty() ? etRow.name : juce::String("(empty)");
                    appendProjectLoadDiagnosticLine(
                        "load: experimentalInstrumentTrack row trackId=" + juce::String((juce::int64)rowTid)
                        + " instrumentKind=" + etRow.instrumentKind + " name=\"" + rowName + "\" clips="
                        + juce::String((int)etRow.clips.size()) + " enabled="
                        + juce::String(etRow.enabled ? "true" : "false"));
                    if (rowTid != kInvalidTrackId && !seenExperimentalTrackIds.insert(rowTid).second)
                    {
                        appendProjectLoadDiagnosticLine("load: duplicate experimentalInstrumentTrack trackId="
                                                        + juce::String((juce::int64)rowTid));
                    }
                    bool tracksRowMatch = false;
                    juce::String tracksRowKind;
                    for (const auto& tr : j.parsed.tracks)
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
                    if (etRow.enabled)
                    {
                        j.instrumentRows.push_back(&etRow);
                    }
                }
                for (const auto& tr : j.parsed.tracks)
                {
                    if (!tr.kind.equalsIgnoreCase("instrument"))
                    {
                        continue;
                    }
                    bool hasExperimentalRow = false;
                    for (const auto& etRow : j.parsed.experimentalInstrumentTracks)
                    {
                        if (etRow.trackId == tr.id) { hasExperimentalRow = true; break; }
                    }
                    if (!hasExperimentalRow)
                    {
                        appendProjectLoadDiagnosticLine(
                            "load: tracks[] instrument without experimentalInstrumentTrack trackId="
                            + juce::String((juce::int64)tr.id) + " name=\"" + tr.name + "\"");
                    }
                }
            }
            j.instrumentIndex = 0;
            j.unit = j.instrumentRows.empty() ? LoadJob::Unit::OrphansAndUi : LoadJob::Unit::RestoreInstrument;
            loadJob_scheduleNextUnit();
            return;
        }

        case LoadJob::Unit::RestoreInstrument:
        {
            if (j.instrumentIndex >= (int)j.instrumentRows.size())
            {
                j.unit = LoadJob::Unit::OrphansAndUi;
                loadJob_scheduleNextUnit();
                return;
            }
            const ProjectFileExperimentalInstrumentTrackV1& etRow = *j.instrumentRows[(size_t)j.instrumentIndex];
            const int total = (int)j.instrumentRows.size();
            const juce::String shown = etRow.instrumentKind == "MidiContent"
                                           ? juce::String("MIDI track")
                                           : (etRow.name.isNotEmpty() ? etRow.name : etRow.instrumentKind);
            if (!announce(withEllipsisUtf8("Loading instruments"),
                          juce::String(j.instrumentIndex + 1) + " of " + juce::String(total) + ": " + shown,
                          (double)j.instrumentIndex / (double)juce::jmax(1, total)))
            {
                return;
            }
            ++j.instrumentIndex;

            const bool isGroove = etRow.instrumentKind == "GrooveAgentSE";
            const bool isHalion = etRow.instrumentKind == "HALionSonic";
            const bool isGeneric = etRow.instrumentKind == "GenericVst3";
            const bool isMidiContent = etRow.instrumentKind == "MidiContent";
            if (isMidiContent)
            {
                // Phase B: plugin-less MIDI content row - restore clips onto a MIDI content
                // controller bound to the TrackKind::Midi session row. No plugin autoload.
                const TrackId midiTid = etRow.trackId;
                const std::shared_ptr<const SessionSnapshot> midiSnap = session_.loadSessionSnapshotForAudioThread();
                const int midiTix = (midiSnap != nullptr && midiTid != kInvalidTrackId) ? midiSnap->findTrackIndexById(midiTid) : -1;
                if (midiTix < 0 || midiSnap->getTrack(midiTix).getKind() != TrackKind::Midi
                    || callbacks_.getOrCreateMidiContentControllerForTrack == nullptr)
                {
                    appendProjectLoadDiagnosticLine("load: skip MidiContent restore (row missing or not Midi) trackId="
                                                    + juce::String((juce::int64)midiTid));
                }
                else if (InstrumentTrackController* const midiCtl = callbacks_.getOrCreateMidiContentControllerForTrack(midiTid))
                {
                    midiCtl->setTimelineSampleRate(sampleRate);
                    midiCtl->restoreExperimentalInstrumentSingleProjectRow(etRow, &j.parsed.tracks);
                    appendProjectLoadDiagnosticLine("load: after MidiContent restore trackId=" + juce::String((juce::int64)midiTid));
                }
                else
                {
                    appendProjectLoadDiagnosticLine("load: skip MidiContent restore (controller create failed) trackId="
                                                    + juce::String((juce::int64)midiTid));
                }
                loadJob_scheduleNextUnit();
                return;
            }
            if (!isGroove && !isHalion && !isGeneric)
            {
                appendProjectLoadDiagnosticLine("load: skip unknown experimental instrumentKind=\"" + etRow.instrumentKind
                                                + "\" trackId=" + juce::String((juce::int64)etRow.trackId));
                loadJob_scheduleNextUnit();
                return;
            }
            const TrackId bindTid = InstrumentTrackController::resolveExperimentalInstrumentLaneIdFromProjectFields(
                &session_, etRow.trackId, &j.parsed.tracks);
            const std::shared_ptr<const SessionSnapshot> postSnap = session_.loadSessionSnapshotForAudioThread();
            if (bindTid == kInvalidTrackId || postSnap == nullptr)
            {
                appendProjectLoadDiagnosticLine("load: skip experimental restore unresolved trackId="
                                                + juce::String((juce::int64)etRow.trackId));
                loadJob_scheduleNextUnit();
                return;
            }
            const int tix = postSnap->findTrackIndexById(bindTid);
            if (tix < 0 || postSnap->getTrack(tix).getKind() != TrackKind::Instrument)
            {
                appendProjectLoadDiagnosticLine("load: skip experimental restore non-instrument lane trackId="
                                                + juce::String((juce::int64)bindTid));
                loadJob_scheduleNextUnit();
                return;
            }
            if (isGeneric)
            {
                appendProjectLoadDiagnosticLine("load: before GenericVst3 restore trackId=" + juce::String((juce::int64)bindTid));
                restoreGenericVst3InstrumentTrack(session_, callbacks_, bindTid, sampleRate, &etRow, &j.parsed.tracks,
                                                  j.instrumentAutoloadNoteAcc);
                appendProjectLoadDiagnosticLine("load: after GenericVst3 restore trackId=" + juce::String((juce::int64)bindTid));
                loadJob_scheduleNextUnit();
                return;
            }
            appendProjectLoadDiagnosticLine("load: before GrooveAgent/HALion restore trackId=" + juce::String((juce::int64)bindTid)
                                            + " kind=" + etRow.instrumentKind);
            const auto runtime = callbacks_.getOrCreateInstrumentRuntimeForTrack(bindTid);
            InstrumentTrackController* const ctl = runtime.second;
            ExperimentalInstrumentHost* const mh = runtime.first;
            if (ctl == nullptr || mh == nullptr)
            {
                appendProjectLoadDiagnosticLine("load: skip GrooveAgent/HALion restore missing runtime trackId="
                                                + juce::String((juce::int64)bindTid));
                loadJob_scheduleNextUnit();
                return;
            }
            ctl->setTimelineSampleRate(sampleRate);
            ctl->restoreExperimentalInstrumentSingleProjectRow(etRow, &j.parsed.tracks);
            juce::String noteOne;
            if (isGroove)
            {
                ctl->runPendingGrooveAgentProjectAutoload(*mh, noteOne);
            }
            else
            {
                ctl->runPendingHalionSonicProjectAutoload(*mh, noteOne);
            }
            appendProjectLoadDiagnosticLine("load: after GrooveAgent/HALion restore trackId=" + juce::String((juce::int64)bindTid)
                                            + " kind=" + etRow.instrumentKind);
            if (noteOne.isNotEmpty())
            {
                if (j.instrumentAutoloadNoteAcc.isNotEmpty())
                {
                    j.instrumentAutoloadNoteAcc << "\n\n";
                }
                j.instrumentAutoloadNoteAcc << noteOne;
            }
            loadJob_scheduleNextUnit();
            return;
        }

        case LoadJob::Unit::OrphansAndUi:
        {
            if (!announce(withEllipsisUtf8("Preparing tracks"), juce::String(), -1.0))
            {
                return;
            }
            appendProjectLoadDiagnosticLine("load: before orphan instrument lane restore");
            restoreOrphanInstrumentLanesWithoutRuntime(session_, j.parsed, callbacks_, sampleRate, j.instrumentAutoloadNoteAcc);
            appendProjectLoadDiagnosticLine("load: after orphan instrument lane restore");
            // Phase B: every TrackKind::Midi row needs its plugin-less controller, including rows
            // whose project block was missing (e.g. hand-edited files) - otherwise the lane would
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
                            appendProjectLoadDiagnosticLine("load: midi content controller create FAILED trackId="
                                                            + juce::String((juce::int64)tr.getId()));
                        }
                    }
                }
            }
            appendProjectLoadDiagnosticLine("load: instrument restore complete");
            appendProjectLoadDiagnosticLine("load: before syncMidiEditorInstrumentStateFromHost");
            callbacks_.syncMidiEditorInstrumentStateFromHost();
            appendProjectLoadDiagnosticLine("load: after syncMidiEditorInstrumentStateFromHost");
            // Missing/unavailable Primary instruments are an EXPECTED portable-project state (proxy
            // playback and the per-track status already communicate availability), so the per-track
            // "could not be loaded / placeholder" notes are no longer surfaced as a blocking
            // informational dialog after an otherwise successful load. They stay in the project-load
            // diagnostic log; real load errors (sample-rate note in `infoNote`, skipped audio files)
            // still show in finalize.
            if (j.instrumentAutoloadNoteAcc.isNotEmpty())
            {
                appendProjectLoadDiagnosticLine("load: instrument availability notes (not shown as dialog): "
                                                + j.instrumentAutoloadNoteAcc.replace("\n", " | "));
            }
            callbacks_.clearSessionHistory();
            appendProjectLoadDiagnosticLine("load: refreshAllUiAfterLoadedProject begin");
            callbacks_.refreshAllUiAfterLoadedProject();
            appendProjectLoadDiagnosticLine("load: refreshAllUiAfterLoadedProject end");
            j.insertIndex = 0;
            if (!j.insertRows.empty())
            {
                appendProjectLoadDiagnosticLine("load: deferred plugin insert restore begin count="
                                                + juce::String((int)j.insertRows.size()));
            }
            j.unit = j.insertRows.empty() ? LoadJob::Unit::Finalize : LoadJob::Unit::RestoreInsertChain;
            loadJob_scheduleNextUnit();
            return;
        }

        case LoadJob::Unit::RestoreInsertChain:
        {
            if (j.insertIndex >= (int)j.insertRows.size())
            {
                appendProjectLoadDiagnosticLine("load: deferred plugin insert restore complete");
                j.unit = LoadJob::Unit::Finalize;
                loadJob_scheduleNextUnit();
                return;
            }
            const Session::PendingPluginInsertRestore& row = j.insertRows[(size_t)j.insertIndex];
            const int total = (int)j.insertRows.size();
            juce::String names;
            for (const auto& s : row.chain.slots)
            {
                if (!s.occupied)
                {
                    continue;
                }
                const juce::String n = juce::File(s.vst3AbsolutePath).getFileNameWithoutExtension();
                names << (names.isEmpty() ? "" : ", ") << (n.isNotEmpty() ? n : s.pluginIdentifier);
            }
            if (!announce(withEllipsisUtf8("Restoring effects"),
                          juce::String(j.insertIndex + 1) + " of " + juce::String(total) + ": " + names,
                          (double)j.insertIndex / (double)juce::jmax(1, total)))
            {
                return;
            }
            ++j.insertIndex;
            appendProjectLoadDiagnosticLine("load: deferred before importChain trackId=" + juce::String((juce::int64)row.trackId)
                                            + " slots=" + juce::String((int)row.chain.slots.size()));
            pluginHost_.importChain(row.trackId, row.chain);
            appendProjectLoadDiagnosticLine("load: deferred after importChain trackId=" + juce::String((juce::int64)row.trackId));
            loadJob_scheduleNextUnit();
            return;
        }

        case LoadJob::Unit::Finalize:
        {
            if (!announce(withEllipsisUtf8("Finalizing project"), juce::String(), -1.0))
            {
                return;
            }
            // P1H: capture proxy asset source hints while the on-disk location is known (the
            // autosave recovery flow clears the save path AFTER this, so a later first-time
            // Save As can still copy the referenced generations from here).
            if (callbacks_.onProjectLoaded != nullptr)
            {
                callbacks_.onProjectLoaded(f.getParentDirectory());
            }
            // The insert chains are in: refresh the views that show them once more.
            if (!j.insertRows.empty())
            {
                callbacks_.refreshAllUiAfterLoadedProject();
            }
            appendProjectLoadDiagnosticLine("load: complete elapsedMs="
                                            + juce::String((int)(juce::Time::getMillisecondCounterHiRes() - j.startMs)));
            markProjectCleanNow();
            // P1 acceptance correction: a freshly loaded project counts as "saved" for the
            // automatic proxy-metadata checkpoint guard; record the loaded file's identity.
            refreshKnownProjectDiskIdentity();
            writeLastOperationBreadcrumb("project load end ok: " + f.getFullPathName());
            // Stability C3: verify runtime invariants right after the load completed.
            (void) stability_invariants::runRegisteredStabilityInvariantsCheck("project-load-end");
            // Always invoked (even without saved bounds) so the MIDI editor bounds memo is seeded or
            // cleared per project; the callee no-ops per window when the project has no bounds.
            if (callbacks_.applyMainWindowBoundsFromLoadedProject != nullptr)
            {
                callbacks_.applyMainWindowBoundsFromLoadedProject(j.parsed);
            }
            // Row heights (v26): always invoked — pre-v26 files carry no keys and reset the
            // arrangement to the Medium default, reproducing their historical look exactly.
            if (callbacks_.applyTrackRowHeightsFromLoadedProject != nullptr)
            {
                callbacks_.applyTrackRowHeightsFromLoadedProject(j.parsed);
            }
            // Conny 1B: reopen the MIDI editor when the project saved it as open (after the session,
            // instrument runtimes, clips and main window are all restored). Skips safely when the
            // saved track/clip no longer exists; never opens plugin editor windows.
            if (callbacks_.restoreMidiEditorWorkspaceFromLoadedProject != nullptr)
            {
                callbacks_.restoreMidiEditorWorkspaceFromLoadedProject(j.parsed);
            }
            juce::String body;
            if (j.infoNote.isNotEmpty() || j.skipped.size() > 0)
            {
                if (j.infoNote.isNotEmpty())
                {
                    body = j.infoNote;
                }
                if (j.skipped.size() > 0)
                {
                    if (body.isNotEmpty())
                    {
                        body << "\n\n";
                    }
                    body << "Could not load " + juce::String(j.skipped.size())
                         + (j.skipped.size() == 1 ? " file:" : " files:") + "\n\n";
                    for (int i = 0; i < j.skipped.size(); ++i)
                    {
                        body << j.skipped[i] << (i < j.skipped.size() - 1 ? "\n" : "");
                    }
                }
            }
            loadJob_finish(true, {});
            if (body.isNotEmpty())
            {
                juce::AlertWindow::showMessageBoxAsync(
                    juce::AlertWindow::InfoIcon, "Load project (partial or note)", body);
            }
            return;
        }

        case LoadJob::Unit::Done:
            return;
    }
}

void ProjectIoCoordinator::loadJob_finish(const bool ok, const juce::String& errorBodyRef)
{
    if (loadJob_ == nullptr)
    {
        return;
    }
    // `errorBodyRef` may refer INTO the job (e.g. `parseResult.getErrorMessage()`), which is
    // destroyed below - take an owning copy first.
    const juce::String errorBody = errorBodyRef;
    // Release order: the progress window first (modal state ends), then the gate (instrument
    // processing resumes) with the job. The worker thread (if any) has already posted its result.
    if (loadProgressWindow_ != nullptr)
    {
        loadProgressWindow_->exitModalState(0);
        loadProgressWindow_->setVisible(false);
        loadProgressWindow_.reset();
    }
    const juce::File f = loadJob_->file;
    appendProjectLoadDiagnosticLine(juce::String("load: staged load ") + (ok ? "finished ok" : "FAILED")
                                    + " file=\"" + f.getFullPathName() + "\""
                                    + (ok ? juce::String() : " error=\"" + errorBody.replace("\n", " ") + "\""));
    std::function<void(bool)> completion = std::move(loadJob_->completion);
    loadJob_.reset();
    if (completion)
    {
        completion(ok);
    }
    if (!ok)
    {
        if (isStabilityTestModeActive())
        {
            // Scenario runs must not leave a modal box behind; the diagnostic log has the error.
            juce::Logger::writeToLog("[Load] failed (stability mode, alert suppressed): " + errorBody.replace("\n", " "));
        }
        else
        {
            juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::WarningIcon, "Load project", errorBody);
        }
    }
    if (onProjectLoadFinished_)
    {
        onProjectLoadFinished_(ok);
    }
}

bool ProjectIoCoordinator::captureLoadProgressWindowPng(const juce::File& png) const
{
    if (loadProgressWindow_ == nullptr || !loadProgressWindow_->isShowing())
    {
        return false;
    }
    const juce::Image img = loadProgressWindow_->createComponentSnapshot(loadProgressWindow_->getLocalBounds(), true, 1.0f);
    if (!img.isValid())
    {
        return false;
    }
    png.getParentDirectory().createDirectory();
    juce::FileOutputStream out(png);
    if (!out.openedOk())
    {
        return false;
    }
    juce::PNGImageFormat fmt;
    return fmt.writeImageToStream(img, out);
}

// =============================================================================
// Stability Slice 5: unsaved-work protection (dirty flag, prompts, autosave, recovery)
// =============================================================================

bool ProjectIoCoordinator::isProjectDirty() const noexcept
{
    if (loadJob_ != nullptr)
    {
        // A load in progress holds a partially applied model; the on-disk file is authoritative
        // and nothing user-made exists yet - never prompt to save (or autosave) this state.
        return false;
    }
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

void ProjectIoCoordinator::refreshKnownProjectDiskIdentity()
{
    const juce::File f = session_.getCurrentProjectFile();
    knownProjectDiskIdentity_ = f.existsAsFile()
                                    ? proxy_checkpoint::sha256HexOfFileForCheckpoint(f)
                                    : juce::String();
}

void ProjectIoCoordinator::noteMainProjectSaveOutcomeForInstrumentControllers(const bool savedOk)
{
    if (callbacks_.instrumentCtlByTrackId == nullptr)
    {
        return;
    }
    const std::shared_ptr<const SessionSnapshot> snap = session_.loadSessionSnapshotForAudioThread();
    if (snap == nullptr)
    {
        return;
    }
    for (int i = 0; i < snap->getNumTracks(); ++i)
    {
        const Track& tr = snap->getTrack(i);
        if (tr.getKind() != TrackKind::Instrument && tr.getKind() != TrackKind::Midi)
        {
            continue;
        }
        InstrumentTrackController* const ctl = callbacks_.instrumentCtlByTrackId(tr.getId());
        if (ctl == nullptr)
        {
            continue;
        }
        if (savedOk)
        {
            ctl->noteMainProjectSavePersisted();
        }
        else
        {
            ctl->invalidateSavedPrimaryBlobAssociation();
        }
    }
}

bool ProjectIoCoordinator::persistPublishedProxyMetadataIfSafe(
    const TrackId trackId, const ProjectFileProxyMetadataV20& metadata,
    const bool callerProvedSavedStatePairing)
{
    // P1 acceptance correction (§18.3/§18.4). Guard evidence, in order:
    //  * `hasProjectFile`/`projectFileExists` — never-saved projects and autosave-recovered
    //    sessions (save path detached) have no legitimate checkpoint target;
    //  * `isProjectDirty()` — set by session-snapshot swaps (musical edits, mute/fader/track
    //    edits) and by `markProjectDirtyFromEdit` (plugin edits, mode changes, recovery). The
    //    publication path itself no longer marks dirty BEFORE this call, so a true value here
    //    reliably means UNSAVED USER EDITS — exactly what must never be saved silently;
    //  * disk identity — the file must still be byte-identical to what the last successful
    //    Save/load/checkpoint produced (external modification / replacement detection).
    // Refusal is not an error: the metadata stays in controller memory (the normal Save DTO
    // reads it) and the caller marks the project dirty so close prompts + next Save persist it.
    const juce::File projectFile = session_.getCurrentProjectFile();
    proxy_checkpoint::CheckpointGuardState guard;
    guard.hasProjectFile = projectFile.getFullPathName().isNotEmpty();
    guard.projectFileExists = guard.hasProjectFile && projectFile.existsAsFile();
    guard.projectDirty = isProjectDirty();
    guard.knownDiskIdentity = knownProjectDiskIdentity_;
    guard.actualDiskIdentity
        = guard.projectFileExists
              ? proxy_checkpoint::sha256HexOfFileForCheckpoint(projectFile)
              : juce::String();
    const juce::String logHead = "proxy-metadata checkpoint trackId="
                                 + juce::String((juce::int64)trackId)
                                 + " generation=" + metadata.generationId;

    const juce::String refusal = proxy_checkpoint::checkpointRefusalReason(guard);
    if (refusal.isNotEmpty())
    {
        appendProjectSaveDiagnosticLine(logHead + " REFUSED (kept pending for the next Save): "
                                        + refusal);
        juce::Logger::writeToLog("[proxy-metadata] checkpoint refused: " + refusal);
        return false;
    }

    // The transaction re-reads the LAST SAVED representation and replaces only this track's
    // proxy metadata — live-session edits cannot leak in. It never fires save callbacks, so
    // `onSuccessfulUserSave` (the On Save render trigger) cannot recurse, and it never calls
    // `markProjectCleanNow`, so the dirty state stays exactly as the guard proved it (clean).
    const proxy_checkpoint::CheckpointOutcome outcome
        = proxy_checkpoint::checkpointProxyMetadataOnDisk(projectFile,
                                                          knownProjectDiskIdentity_,
                                                          trackId,
                                                          metadata,
                                                          callerProvedSavedStatePairing);
    if (!outcome.ok)
    {
        // Failure never damages the previous `.dalproj` (temp+rename discipline) and never
        // touches the published WAV; the proxy stays usable in this session and the metadata
        // stays pending for the next explicit Save. Not silent: diagnostics + app log.
        appendProjectSaveDiagnosticLine(logHead + " FAILED (previous file intact; metadata "
                                        "pending for the next Save): " + outcome.error);
        juce::Logger::writeToLog("[proxy-metadata] checkpoint failed: " + outcome.error);
        return false;
    }

    knownProjectDiskIdentity_ = outcome.newDiskIdentity;
    appendProjectSaveDiagnosticLine(logHead + " ok (metadata-only atomic update)");
    writeLastOperationBreadcrumb("proxy metadata checkpoint ok: "
                                 + projectFile.getFullPathName());
    return true;
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
    std::optional<ProjectFileTrackRowHeightsV1> trackRowHeights;
    if (callbacks_.getTrackRowHeightsForProjectSave != nullptr)
    {
        trackRowHeights = callbacks_.getTrackRowHeightsForProjectSave();
    }
    juce::Result r = juce::Result::ok();
    {
        // Autosave uses the same bounded capture window as explicit saves: inaudible (gapless
        // drain while playing), never a callback wait, bounded on the message thread.
        const ScopedPluginStateCaptureWindow captureWindow(playbackEngine_);
        r = session_.saveProjectToFile(
            transport_,
            autosaveFile,
            sampleRate,
            &pluginHost_,
            ctlLookup,
            snapRoot.enabled,
            snapRoot.resolutionKey,
            mainWinBounds,
            midiEditorWinBounds,
            midiEditorWorkspace,
            trackRowHeights);
    }
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
    // A staged project load spans many message-loop turns: the model is only partially applied
    // until finalize, so an autosave written now would capture a half-built project.
    if (loadJob_ != nullptr)
    {
        return "project load in progress";
    }
    // Covers the recovery prompt, unsaved-changes prompts, alerts, and modal pickers. Save/
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
                    // The staged load finishes asynchronously (progress window); the detach /
                    // dirty steps run from its completion, never before the project is in.
                    loadProjectFromFileThen(autosaveFile, [this, autosaveFile, guard](const bool ok) {
                        if (!guard.isAlive())
                        {
                            return;
                        }
                        if (ok && session_.getCurrentProjectFile() == autosaveFile)
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
                    });
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
