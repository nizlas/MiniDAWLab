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
#include "instruments/InstrumentTrackController.h" // InstrumentMidiClipId in hook signatures

#include <functional>
#include <memory>
#include <optional>
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
    Autosave,        // C5: load, dirty edit, forced autosave, verify file/pointer/original.
    RecoverAutosave, // C5: as Autosave, then in-process recovery and post-recovery verification.
    MidiRouting,     // Phase B: MIDI track -> instrument routing, capture-seam delivery + v18 roundtrip.
    /// MIDI-clip parity between `TrackKind::Midi` and instrument rows: file import onto a MIDI row
    /// and cross-track clip moves in both directions, then a save/reload check.
    MidiTrackParity,
    /// Global audio health across "add MIDI track -> import a MIDI file": device-output peak,
    /// callback count and playhead advance are probed while playing before and after the import.
    MidiImportAudio,
    /// Copy/paste must not open the MIDI editor, and moving a clip whose editor is open must not
    /// crash (use-after-free on the freed clip). Reproduces the user's B+C sequence through the
    /// production paste, editor-open and cross-track move paths, with a settle between move and
    /// the controller's async change callback.
    MidiEditorMoveCrash,
    /// Audio-track pre-gain through the REAL engine: device-output peak while playing and the RMS
    /// of an offline WAV export at 0 dB vs −24 dB must both scale by 10^(−24/20) ≈ 0.0631 — i.e.
    /// pre-gain is applied, exactly once, on the realtime clip path and the mixdown path. Also
    /// checks that the value survives save/reload. Expects an audio-only fixture project whose
    /// active loop contains a steady tone (see MixdownPreGainFocusedTests --make-fixture).
    PreGain,
    /// The user's flow on a real project: activate the audio track that hosts AmpliTube (or any
    /// insert) like a header click, type 0 → −24 dB into the REAL Inspector field (TextEditor key
    /// path + Return), and compare displayed vs committed value and track id; then measure, with
    /// the insert level tap, the signal entering the first insert and leaving the last one while
    /// the transport plays — first with the project's own mute state, then with the track unmuted.
    PreGainInspector,
    /// VST3 inserts across save / reload in the REAL app (ProjectIoCoordinator Save, reload,
    /// forced autosave + recovery): builds an instrument shell row in the loaded project, adds DAL
    /// Mono Delay as a Post insert there, as a Pre insert on the audio row and as a Post insert on
    /// the master through the picker's call, verifies every row after each round trip, then
    /// simulates an uninstalled plugin by re-pointing the saved path (user plugins untouched) and
    /// asserts the insert survives as an "(unavailable)" row through load → save → load.
    Inserts,
    /// Shared track-header column: on a real project, verifies every header/lane/ruler/overlay sits
    /// on one boundary at the startup width, the default, the minimum and a wide setting (set
    /// through the same path the drag handle uses), that adding/removing a track keeps the width,
    /// that the width is persisted app-wide, and writes PNG evidence of the arrangement.
    HeaderColumn,
};

/// One insert row as the runner sees it (mirrors `InsertRowView`).
struct StabilityInsertRowInfo
{
    bool pre = false;
    juce::String displayName;
    bool unavailable = false;
};

struct StabilityScenarioRequest
{
    StabilityScenarioKind kind = StabilityScenarioKind::None;
    juce::File projectA;
    juce::File projectB; // LoadAlternate only.
    juce::File midiFile; // MidiImportAudio (`--midi <file>`); a built-in fixture when absent.
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

    // --- Stability C5: autosave/recovery hooks (ProjectIoCoordinator test surface) ---
    /// Writes an autosave immediately (project must be dirty). False + reason on failure.
    std::function<bool(juce::String& failReason)> forceAutosaveNow;
    /// Runs the recovery-prompt "Recover" steps without a prompt. False + reason on failure.
    std::function<bool(juce::String& failReason)> recoverAutosaveNow;
    /// Where the next autosave for the current project would be written.
    std::function<juce::File()> getAutosaveFilePath;
    /// The %APPDATA% autosave pointer file.
    std::function<juce::File()> getAutosavePointerFilePath;
    std::function<bool()> isProjectDirty;
    /// Full path of the session's current save target; empty when never saved / after recovery.
    std::function<juce::String()> getCurrentProjectPath;

    // --- Phase B/B.1: MIDI-track routing scenario hooks ---------------------
    /// Builds the many-to-one fixture in the loaded project: one plugin-less instrument shell
    /// (capture sink installed) with its OWN channel-1 clip, plus two `TrackKind::Midi` sources
    /// ("Lower" fixed output channel 2, "Pedal" fixed 3) routed to it. Boundary pitches 0 and 127
    /// ride along for the full-range persistence check.
    std::function<bool(juce::String& failReason)> midiRoutingFixtureSetup;
    /// After a playback window: asserts exact per-channel note-ons (1/2/3 distinct, mask 0x07),
    /// stop-flushed note-offs and a once-per-block destination boundary.
    std::function<bool(juce::String& failReason)> midiRoutingVerifyDelivery;
    /// Phase B.1: re-renders the fixture span offline (mixdown path) and asserts the capture sink
    /// saw the same routed MIDI — realtime and offline paths must be equivalent.
    std::function<bool(juce::String& failReason)> midiRoutingRunOfflineParity;
    /// After save + reload: asserts both Midi rows, destinations, fixed channels, native channels
    /// and exact stored pitches survived (v18; pitches must be untouched by the full-range editor).
    std::function<bool(juce::String& failReason)> midiRoutingVerifyAfterReload;

    // --- MIDI-clip parity for plain `TrackKind::Midi` rows ------------------
    /// On the routing fixture: writes a Standard MIDI File from the MIDI row's clip, imports it
    /// back onto that same MIDI row through the production parse+append path, then moves the
    /// imported clip MIDI row -> instrument row and back through the production cross-track move.
    /// Asserts note content survives every hop and that neither row keeps a stale copy.
    std::function<bool(juce::String& failReason)> midiTrackParityVerify;
    /// After save + reload: asserts the imported clip is still owned by the MIDI row with its
    /// notes intact (MidiContent blocks persist clips exactly like instrument rows).
    std::function<bool(juce::String& failReason)> midiTrackParityVerifyAfterReload;

    // --- Global audio health probe ---------------------------------------------
    /// Arms the probe: resets the engine's output peak hold and remembers callback count/playhead.
    std::function<void()> audioHealthProbeBegin;
    /// Reads the probe after a playing window and asserts: callbacks advanced, playhead advanced,
    /// device output peak above the audible floor and finite. Logs every value under `label`.
    std::function<bool(const juce::String& label, juce::String& failReason)> audioHealthProbeVerify;
    /// Same entry the transport "add track" menu uses for a MIDI track; returns the new row id.
    std::function<std::optional<TrackId>()> addMidiTrackLikeUi;
    /// Production import (parse + undoable append + UI sync) onto `tid`, without the file chooser.
    std::function<bool(TrackId tid, const juce::File& midiFile, juce::String& failReason)>
        importMidiFileOntoTrack;

    // --- Editor / paste / move crash reproduction ------------------------------
    /// True while the MIDI editor window is open and visible.
    std::function<bool()> isMidiEditorOpen;
    /// Number of clips currently on the track's controller (instrument or MIDI content).
    std::function<int(TrackId)> clipCountOnTrack;
    /// Selects the track's first clip on its controller; returns its id (0 if none).
    std::function<InstrumentMidiClipId(TrackId)> selectFirstClipOnTrack;
    /// Opens the MIDI editor on the track's first clip (production open path); false if none.
    std::function<bool(TrackId)> openMidiEditorOnFirstClipOfTrack;
    /// Runs the production clipboard copy then paste of the current MIDI selection.
    std::function<void()> copyThenPasteSelectedMidiClipLikeUi;
    /// Moves `clipId` from `src` to `dst` through the production cross-track move, wrapped as the
    /// same undoable instrument edit the arrangement drag commits.
    std::function<bool(TrackId src, TrackId dst, InstrumentMidiClipId clipId)> moveMidiClipCrossTrackLikeUi;
    /// Fixture ids created by `midiRoutingFixtureSetup` (destination instrument row / a routed MIDI row).
    std::function<TrackId()> fixtureInstrumentTrackId;
    std::function<TrackId()> fixtureMidiTrackId;
    /// Production "refresh the open MIDI editor from its host" (syncInstrumentStateFromHost). Used
    /// after a move to exercise the roll rebuild that dereferences the editor's bound clip.
    std::function<void()> refreshInstrumentEditorUi;

    // --- Pre-gain scenario -------------------------------------------------------
    /// Same session setter the Inspector's undoable "Set pre-gain" edit ends in
    /// (`Session::setTrackPreGainDb`); returns false when the row is unknown or the value is a no-op.
    std::function<bool(TrackId, float dB)> setTrackPreGainDb;
    /// Current stored pre-gain of a row from the published snapshot (NaN when the row is unknown).
    std::function<float(TrackId)> getTrackPreGainDb;
    /// Device-output peak hold since the previous call (see PlaybackEngine diagnostics), then reset.
    std::function<float()> readOutputPeakHoldAndReset;

    // --- Pre-gain through the REAL Inspector (user-flow reproduction) --------------
    /// Activates `tid` exactly like a click on its header name strip (Session active track plus the
    /// header-activated callback that refreshes the Inspector).
    std::function<void(TrackId)> activateTrackLikeHeaderClick;
    /// Types `text` into the Inspector's pre-gain field through the TextEditor key path + Return.
    std::function<void(const juce::String&)> inspectorTypePreGainAndReturn;
    /// Current text of the Inspector's pre-gain field, and whether the field is visible.
    std::function<juce::String()> inspectorPreGainFieldText;
    std::function<bool()> inspectorPreGainFieldVisible;
    /// Mute/unmute exactly like the header Mute button (Session mute + header refresh).
    std::function<void(TrackId, bool)> setTrackMutedLikeHeader;
    /// One line describing a track as the engine sees it: kind, name, muted, off, fader, pre-gain,
    /// output, monitor, insert rows and whether the chain is active for the audio thread.
    std::function<juce::String(TrackId)> describeTrackForDiagnostics;
    /// First audio track whose insert chain has a plug-in whose name contains `fragment` (or invalid).
    std::function<TrackId(const juce::String& fragment)> findAudioTrackWithInsertNamed;
    /// PluginInsertHost level tap: select the tapped track; read+reset "before first insert" /
    /// "after last insert" peak holds and the processed-block counters.
    std::function<void(TrackId)> setInsertLevelTapTrack;
    std::function<void(float& peakBefore, float& peakAfter, double& rmsBefore, double& rmsAfter,
                       std::uint32_t& preBlocks, std::uint32_t& postBlocks)>
        readAndResetInsertLevelTap;
    /// Transport seek request (consumed by the audio callback at the next block).
    std::function<void(std::int64_t)> seekTransportTo;

    // --- Inserts scenario ----------------------------------------------------------
    /// Every session row including Master (the delete-list hook excludes it).
    std::function<std::vector<StabilityTrackInfo>()> listAllTracks;
    /// The exact `PluginInsertHost::addInsertFromVst3File` call the VST3 picker makes on a pick.
    std::function<juce::Result(TrackId, bool pre, const juce::File& vst3)> addInsertLikePicker;
    /// Rows of a track's insert chain as the Inspector lists them.
    std::function<std::vector<StabilityInsertRowInfo>(TrackId)> listInsertRows;

    // --- Header column scenario --------------------------------------------------------
    /// Same calls the boundary handle makes: anchor at the effective width, apply `deltaPx`,
    /// then the drag-ended notification (owner re-layout + app-wide persistence).
    std::function<void(int deltaPx)> dragHeaderColumnLikeHandle;
    /// Stored preference / effective width of the shared header column.
    std::function<int()> getHeaderColumnWidthPreference;
    std::function<int()> getHeaderColumnEffectiveWidth;
    /// Re-reads the persisted app-wide value from disk (nullopt when absent/invalid).
    std::function<std::optional<int>()> readPersistedHeaderColumnWidth;
    /// Geometry check across headers, lanes, ruler, add-track corner and playhead overlay; appends a
    /// human-readable report. False with `failReason` on the first violation.
    std::function<bool(juce::String& report, juce::String& failReason)> verifyHeaderColumnLayout;
    /// Writes a PNG snapshot of the whole arrangement window content to `png`.
    std::function<bool(const juce::File& png)> captureArrangementPng;
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
    /// C5. `withRecovery` selects the recover-autosave variant.
    void appendAutosaveSteps(const juce::File& project, bool withRecovery);
    /// Phase B: MIDI routing fixture + capture-seam playback verification + v18 roundtrip.
    void appendMidiRoutingSteps(const juce::File& project);
    /// MIDI-clip parity: import onto a `TrackKind::Midi` row + cross-track moves + save/reload.
    void appendMidiTrackParitySteps(const juce::File& project);
    /// Audio health before/after "add MIDI track -> import MIDI file" while playing.
    void appendMidiImportAudioSteps(const juce::File& project, const juce::File& midiFile);
    /// Paste-opens-editor (B) and move-with-open-editor crash (C) reproduction.
    void appendMidiEditorMoveCrashSteps(const juce::File& project);
    /// Pre-gain through the real engine: realtime peak ratio, offline RMS ratio, save/reload.
    void appendPreGainSteps(const juce::File& project);
    /// Pre-gain through the real Inspector on a real project (see `PreGainInspector`).
    void appendPreGainInspectorSteps(const juce::File& project);
    /// VST3 inserts across Save / reload / autosave-recovery + the unavailable-plugin placeholder.
    void appendInsertsSteps(const juce::File& project);
    /// Shared header column geometry, resize path, persistence and PNG evidence.
    void appendHeaderColumnSteps(const juce::File& project);

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
    /// MidiImportAudio: the MIDI row created by the scenario (target of the import step).
    TrackId scenarioMidiTrackId_ = kInvalidTrackId;
    /// PreGain: measurements carried between steps (realtime peaks and offline RMS at 0 / −24 dB).
    float preGainPeakAt0dB_ = 0.0f;
    float preGainPeakAtMinus24dB_ = 0.0f;
    double preGainRmsAt0dB_ = 0.0;
    double preGainRmsAtMinus24dB_ = 0.0;
    /// PreGain / Mixdown: dedicated empty output folder so the exact result file set can be asserted.
    juce::File scenarioOutputDir_;
    /// PreGainInspector: the audio track hosting the insert under test and its measurements.
    TrackId inspectorTargetTrackId_ = kInvalidTrackId;
    float inspectorPeakBefore0dB_ = 0.0f;
    float inspectorPeakAfter0dB_ = 0.0f;
    double inspectorRmsBefore0dB_ = 0.0;
    double inspectorRmsAfter0dB_ = 0.0;
    /// Inserts: fixture rows (instrument shell / audio / master) and the files produced.
    TrackId insertsInstrumentTrackId_ = kInvalidTrackId;
    TrackId insertsAudioTrackId_ = kInvalidTrackId;
    TrackId insertsMasterTrackId_ = kInvalidTrackId;
    juce::File insertsMissingPluginCopy_;
    /// HeaderColumn: the user's preference at scenario start (restored at the end).
    int headerColumnWidthAtStart_ = 0;

    JUCE_DECLARE_NON_COPYABLE(StabilityScenarioRunner)
};
