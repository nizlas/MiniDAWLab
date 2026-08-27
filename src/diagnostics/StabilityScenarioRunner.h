#pragma once

// =============================================================================
// StabilityScenarioRunner — in-process stability scenarios (Stability C2)
// =============================================================================
// Message-loop-driven state machine that repeatedly exercises real app code
// paths (project load, track delete/undo/redo, save/reload, mixdown) via hooks
// installed by `TransportControlsContent`. One operation per timer tick with a
// settle window between steps so async callbacks, timers, and repaints run
// exactly as they do for a real user. Progress goes to
// `%APPDATA%\MiniDAWLab\stability-run.log`; the app quits with exit code 0
// (PASS) or 1 (FAIL) when the scenario completes.
//
// Started from the `--stability-*` command line (see Main.cpp). This is an
// internal test mode only; nothing here is reachable from the UI.
// =============================================================================

#include <JuceHeader.h>

#include "domain/Track.h"

#include <functional>
#include <memory>
#include <vector>

enum class StabilityScenarioKind
{
    None,
    LoadLoop,
    LoadAlternate,
    DeleteLoop,
    OpenSaveClose,
    Smoke,
    Mixdown,
};

struct StabilityScenarioRequest
{
    StabilityScenarioKind kind = StabilityScenarioKind::None;
    juce::File projectA;
    juce::File projectB; // LoadAlternate only.
    int iterations = 1;
    bool mixdownMp3 = false; // Mixdown only (`--format mp3`).

    [[nodiscard]] bool isActive() const noexcept { return kind != StabilityScenarioKind::None; }
};

/// Parses `--stability-*` flags (see file header for the list). Returns an inactive request when
/// no stability flag is present. On malformed arguments, `errorOut` is set and the request is
/// inactive. Non-flag arguments directly after a scenario flag are its project path(s).
[[nodiscard]] StabilityScenarioRequest parseStabilityScenarioFromCommandLine(
    const juce::StringArray& args, juce::String& errorOut);

/// True while a stability scenario runs. Prompt sites (unsaved-changes guard, startup recovery)
/// check this to auto-answer deterministically instead of blocking the run; the chosen answer is
/// logged to stability-run.log.
[[nodiscard]] bool isStabilityTestModeActive() noexcept;
void setStabilityTestModeActive(bool active) noexcept;

/// One deletable session track as seen by the runner (Master excluded by the hook).
struct StabilityTrackInfo
{
    TrackId id = kInvalidTrackId;
    juce::String kindName;
    juce::String name;
    bool isInstrument = false;
};

/// All hooks run on the message thread and call the same entry points the UI uses.
struct StabilityRunnerHooks
{
    std::function<void(const juce::File&)> loadProjectFromFile;
    std::function<void()> saveProject;
    std::function<int()> getTrackCount;
    std::function<std::vector<StabilityTrackInfo>()> listDeletableTracks;
    /// Same path as the header context menu "Delete Track".
    std::function<void(TrackId)> requestDeleteTrack;
    std::function<void()> invokeUndo;
    std::function<void()> invokeRedo;
    /// true = start playback, false = stop (with transport button UI sync).
    std::function<void(bool)> setPlaybackActive;
    /// Undoable rename via the real TrackLanesView path; returns false when refused.
    std::function<bool(TrackId, juce::String)> renameTrackUndoable;
    /// Opens the MIDI editor on the track's first clip; false when not an instrument track or no clips.
    std::function<bool(TrackId)> openMidiEditorOnFirstClip;
    std::function<void()> closeMidiEditor;
    /// Blocking mixdown of the active loop range via the real exporter (no dialog). `mp3` selects format.
    std::function<juce::Result(const juce::File& outputFile, bool mp3)> runMixdownBlocking;
    /// Stability C3: runs `stability_invariants::verifyStableState` over the live app state.
    /// Called after every step; a false return fails the scenario.
    std::function<bool(const juce::String& reason)> verifyInvariants;
};

class StabilityScenarioRunner final : private juce::Timer
{
public:
    explicit StabilityScenarioRunner(StabilityRunnerHooks hooks);
    ~StabilityScenarioRunner() override;

    /// Builds the step list for `request` and starts stepping on the message loop.
    void start(const StabilityScenarioRequest& request);

private:
    struct Step
    {
        juce::String name;
        /// Returns false to fail the run; may set `failReason`.
        std::function<bool(juce::String& failReason)> action;
        int settleMsAfter = 250;
    };

    void timerCallback() override;
    void finish(bool pass, const juce::String& reason);

    void appendLoadLoopSteps(const juce::File& project, int iterations);
    void appendLoadAlternateSteps(const juce::File& a, const juce::File& b, int iterations);
    void appendDeleteLoopSteps(const juce::File& project, int iterations);
    void appendOpenSaveCloseSteps(const juce::File& project);
    void appendMixdownSteps(const juce::File& project, bool mp3);

    void appendLoadAndVerifySteps(const juce::File& project, const juce::String& label);
    /// Inserts the delete/undo/redo/undo cycle steps for one track at `insertAt`.
    /// Returns the number of steps inserted.
    size_t insertDeleteCycleSteps(size_t insertAt,
                                  const StabilityTrackInfo& track,
                                  bool withPlayback,
                                  bool withMidiEditor,
                                  const juce::String& label);

    StabilityRunnerHooks hooks_;
    std::vector<Step> steps_;
    size_t nextStepIndex_ = 0;
    /// Stability C3: failure-count baseline at `start`; any increase during a step fails the run.
    int invariantFailuresAtStart_ = 0;
    juce::int64 resumeAtMs_ = 0;
    juce::int64 runStartMs_ = 0;
    juce::String scenarioName_;
    bool finished_ = false;
    juce::File openSaveCloseCopy_; // Temp project copy; deleted at scenario end.

    JUCE_DECLARE_NON_COPYABLE(StabilityScenarioRunner)
};
