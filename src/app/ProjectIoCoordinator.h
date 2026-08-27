#pragma once

#include <JuceHeader.h>

#include <functional>
#include <memory>
#include <optional>
#include <utility>

#include "domain/Track.h"
#include "io/ProjectFile.h"
#include "ui/SnapSettings.h"
#include "util/AsyncLifetimeToken.h"

class Transport;
class Session;
class SessionSnapshot;
class PlaybackEngine;
class PluginInsertHost;
class InstrumentTrackController;
class ExperimentalInstrumentHost;

class ProjectIoCoordinator
{
public:
    struct Callbacks
    {
        std::function<InstrumentTrackController*(TrackId)> instrumentCtlByTrackId;
        std::function<void()> snapshotOpenClipViewportFromMidiEditor;

        std::function<void()> clearExperimentalInstrumentRuntimesPreserveBridgeOnly;
        std::function<std::pair<ExperimentalInstrumentHost*, InstrumentTrackController*>(TrackId)> getOrCreateInstrumentRuntimeForTrack;
        std::function<void()> syncMidiEditorInstrumentStateFromHost;
        std::function<void()> clearSessionHistory;
        std::function<void()> refreshAllUiAfterLoadedProject;

        std::function<SnapProjectRootFields()> getSnapProjectRootFieldsForSave;
        std::function<void(const SnapProjectRootFields&)> restoreSnapProjectRootFieldsToUi;

        std::function<std::optional<ProjectFileMainWindowBoundsV1>()> getMainWindowBoundsForProjectSave;
        std::function<void(const ProjectFileV1&)> applyMainWindowBoundsFromLoadedProject;

        /// Optional: transient "Saving project" indicator, invoked right before the project file write.
        std::function<void()> showSavingProjectIndicator;
    };

    ProjectIoCoordinator(Transport& transport,
                           Session& session,
                           juce::AudioDeviceManager& deviceManager,
                           PluginInsertHost& pluginHost,
                           PlaybackEngine& playbackEngine,
                           Callbacks callbacks);

    void saveProject();
    void loadProject();

    /// [Message thread] Load `projectFile` directly (no chooser) — same pipeline as `loadProject`,
    /// so project-relative paths resolve identically. Used by the command-line ".dalproj" open path.
    /// Shows a non-fatal alert when the file is missing or unreadable.
    void loadProjectFromFile(const juce::File& projectFile);

    // ---------------------------------------------------------------------
    // Stability Slice 5: unsaved-work protection (dirty flag, prompts, autosave, recovery).
    // ---------------------------------------------------------------------

    /// [Message thread] Same as `saveProject`, but reports the outcome. `onDone(true)` runs after a
    /// successful write; `onDone(false)` after any failure or a cancelled Save As chooser.
    void saveProjectThen(std::function<void(bool saved)> onDone);

    /// [Message thread] True when the project differs from the last saved/loaded/startup state.
    /// Session-level edits are detected by snapshot-pointer comparison; instrument/plugin edits
    /// are flagged through `markProjectDirtyFromEdit`. Prefers false positives.
    [[nodiscard]] bool isProjectDirty() const noexcept;

    /// [Message thread] Capture the current state as "clean" (after new/load/save).
    void markProjectCleanNow() noexcept;

    /// [Message thread] Flag an instrument/plugin-side edit that does not swap the session snapshot.
    void markProjectDirtyFromEdit() noexcept;

    enum class UnsavedGuardKind
    {
        LoadProject,
        QuitApp,
        Export,
    };

    /// [Message thread] If the project is clean, runs `proceed` immediately. Otherwise shows the
    /// three-button prompt ("Save and X" / "X Without Saving" / "Cancel"). "Save and X" continues
    /// only after a successful save; "X Without Saving" writes an autosave first, then continues.
    void confirmUnsavedChangesThen(UnsavedGuardKind kind, std::function<void()> proceed);

    /// [Message thread] Quit interception: returns false when the app may quit right away (clean
    /// project). When dirty, shows the quit prompt (quit resumes via `JUCEApplication::quit`) and
    /// returns true.
    [[nodiscard]] bool interceptQuitForUnsavedChanges();

    /// [Message thread] Write an autosave snapshot if the project is dirty. Never changes the
    /// user's normal project path. Failure is logged, not surfaced.
    void writeAutosaveIfDirty(const juce::String& reason);

    /// [Message thread, startup] Offer to recover an existing autosave. When a command-line
    /// ".dalproj" open is already queued, the prompt is skipped (breadcrumb only).
    void offerAutosaveRecoveryOnStartup(bool commandLineProjectOpenQueued);

private:
    void launchLoadProjectChooser();
    [[nodiscard]] juce::File resolveAutosaveTargetFile() const;
    void deleteAutosaveArtifactsAfterSuccessfulSave();
    Transport& transport_;
    Session& session_;
    juce::AudioDeviceManager& deviceManager_;
    PluginInsertHost& pluginHost_;
    PlaybackEngine& playbackEngine_;
    Callbacks callbacks_;
    /// Stability Slice 4: FileChooser completions check this before touching the coordinator.
    mini_daw::AsyncLifetimeOwnerToken asyncLifetime_;

    /// Stability Slice 5: snapshot pointer captured at the last clean point (new/load/save).
    /// Session snapshots are immutable, so any session mutation swaps the pointer.
    std::shared_ptr<const SessionSnapshot> cleanSessionSnapshot_;
    /// Instrument/plugin edits do not swap the session snapshot; they set this flag instead.
    bool instrumentOrPluginEditsSinceClean_ = false;
};
