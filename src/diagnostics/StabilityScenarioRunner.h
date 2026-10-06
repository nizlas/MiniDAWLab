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
    /// Export level diagnosis on a real project copy: measures the REALTIME Stereo Out (device
    /// output, via the engine's master meter accumulator) over the active loop, then exports the
    /// same loop as float WAV, 24-bit WAV and MP3 through the production exporter (files kept in
    /// `%TEMP%\dal-export-levels` for the offline analyzer) with each render's level report, then
    /// repeats float WAV + realtime with the Stereo Out fader 12 dB lower (diagnostic, restored
    /// afterwards) to prove the master fader reaches both paths identically. Nothing is saved.
    ExportLevels,
    /// Inspector channel panel in the real app: per row kind (audio / instrument / MIDI / master)
    /// the fixed bottom panel shows the right controls, meters move while playing, the overload
    /// latch sets and resets, typed fader values and the reset gesture reach the session, every
    /// Inspector control is reachable by scrolling, and the layout survives a low window. PNG
    /// evidence of the real Inspector column is written for the report.
    InspectorPanel,
    /// Organ residual signal: with the transport STOPPED, measures the selected instrument row's
    /// post-strip stage and the Stereo Out (DC / varying AC component / peak, separately) in the
    /// current audio samples — not the UI's hold values — before, during and after a header mute,
    /// then after a short playback. Shows whether the "stuck" meter is real signal and how big the
    /// step is that a mute/unmute switches (the click).
    OrganDc,
    /// Live MIDI input through the production path: builds an instrument shell (capture sink) with
    /// two routed `Midi` rows, assigns MIDI inputs, injects MIDI from a separate thread (or a real
    /// loopback port when one is present), and verifies monitoring with a stopped transport (no
    /// dirty flag), channel mapping, Monitor off releasing held notes, a recorded take through the
    /// real Record / count-in / Stop path (clip positions, lengths, velocities, channels, CC,
    /// sustain, pitch bend), save / reload, MIDI export, undo / redo, the empty take and the Cycle
    /// guard. Nothing of the user's project is modified (sibling copy).
    LiveMidi,
    /// `--stability-midi-cycle-takes <project>`: MIDI cycle recording through the real Record /
    /// count-in / Stop path with Cycle on (instrument row + routed MIDI row, ≥ 3 passes, a key /
    /// sustain / wheel held across a wrap, a silent pass, a controller-only pass, a partial last
    /// pass), one take per pass as new topmost layers, ONE undo step, the layering rule on
    /// playback (topmost take heard; delete top → previous heard; undo/redo), the running-take
    /// preview geometry (start / after wrap / zoom / scroll, PNG evidence), save / reload, the
    /// MIDI editor open across delete / undo / move, offline mixdown and a fresh proxy sequencer
    /// selecting the same events as realtime. Sibling copy only.
    MidiCycleTakes,
    /// `--stability-proxy-recording <project>`: a project with schema-1 proxy generations (TSE
    /// copy): a provably layer-insensitive generation stays Current without a re-render when the
    /// Primary is unavailable (no-Primary machine simulated), a non-comparable one never does;
    /// recording on a proxy-backed destination without a Secondary keeps the whole proxy and says
    /// so, with a Secondary switches to it for the take (Monitor off: nothing delivered live, the
    /// row's own clip silent; Monitor on: live heard, clip silent), the override ends with the
    /// take and real currency decides afterwards. Sibling copy only.
    ProxyRecording,
    /// `--stability-proxy-render-probe <project> <trackId> <outDir> [--repeat N] [--wait-after-prepare ms]
    /// [--publish]`: diagnostic — renders ONE destination repeatedly through the production engine
    /// (capture → isolated instance → worker render), keeps every artifact under `outDir` with a
    /// content analysis (first audible sample, last sample above the tail threshold, music / tail
    /// energy, per-second profile), documents the frozen identity (fingerprint, canonical bytes,
    /// exact plugin-state blob hash, plugin version, rates, block, tail policy), and optionally
    /// publishes each result through the production publication so a generation-name collision
    /// is exercised with the FIRST file kept. The project copy is kept between runs (Debug then
    /// Release against the same proxy directory). Sibling copy only.
    ProxyRenderProbe,
    /// `--stability-proxy-playback-edges <project> <trackId> <outDir>`: diagnostic — with the
    /// destination's PUBLISHED proxy forced as the transport source, measures the row's
    /// pre-insert and post-strip (after the user's insert chain) levels and the Stereo Out in
    /// windows around play start, the asset's end (EOF), continued transport past EOF, Stop,
    /// restart, a loop wrap across EOF, and an offline mixdown across EOF; then the same
    /// windows through the Primary for comparison. Reports peaks / DC / first / last samples per
    /// window and analyses the mixdown WAV around EOF. Sibling copy only.
    ProxyPlaybackEdges,
    /// `--stability-mixer <project>`: the mixer window in the real app — F3 / close / reopen with
    /// one instance, every row kind as a strip in arrangement order with the Stereo Out strip
    /// fixed at the right across horizontal scrolling, independent section toggles with aligned
    /// strips (geometry verified, PNG evidence), fader / pan / routing / send / pre-gain / insert
    /// edits through the strips of a NON-active row reaching that TrackId and the Inspector,
    /// concurrent meters (two rows + Stereo Out, Inspector and mixer agreeing on one row), audio
    /// runtime untouched by open / close, strips following track deletion and a project switch,
    /// and the machine-local layout persistence (bounds + section flags, header width kept).
    /// Sibling copy only.
    Mixer,
};

/// Live MIDI scenario: one row's MIDI clips as the runner asserts them.
struct StabilityMidiClipSummary
{
    int clipCount = 0;
    std::int64_t firstClipStartSamples = 0;
    std::int64_t firstClipLengthSamples = 0;
    double bpm = 0.0;
    int ticksPerQuarter = 0;
    struct Note
    {
        int note = 0;
        int velocity = 0;
        int offVelocity = 0;
        int channel = 0;
        std::int64_t startTick = 0;
        std::int64_t durationTicks = 0;
    };
    std::vector<Note> notes; ///< first clip
    struct Cc
    {
        std::int64_t tick = 0;
        int controller = 0;
        int value = 0;
        int channel = 0;
    };
    std::vector<Cc> cc; ///< first clip
    struct Pb
    {
        std::int64_t tick = 0;
        int value = 0;
        int channel = 0;
    };
    std::vector<Pb> pitchBend; ///< first clip
};

/// Mirror of the exporter's level report / the engine's meter reading for scenario logging.
struct StabilityLevelStats
{
    float peak[2] = { 0.0f, 0.0f };
    std::uint32_t overs[2] = { 0, 0 };
    double rms[2] = { 0.0, 0.0 };
    double dcOffset[2] = { 0.0, 0.0 };
    std::uint32_t nonFinite = 0;
    std::uint64_t frames = 0;
    float firstSample[2] = { 0.0f, 0.0f };
    float lastSample[2] = { 0.0f, 0.0f };
    bool valid = false;
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
    // ProxyRenderProbe only.
    TrackId probeTrackId = kInvalidTrackId;
    juce::File probeOutDir;
    int probeRepeat = 2;
    int probeWaitAfterPrepareMs = 0;
    bool probePublish = false;
    juce::File probeStateBlobOverride; ///< `--state-blob <file>`: render with these exact state bytes
    bool probeNoReadiness = false;     ///< `--no-readiness`: reproduce the pre-1.1.14 render (diagnostic)
    bool probeRealtimeIndication = false; ///< `--realtime-indication`: clone prepared without the offline flag (diagnostic)
    bool probeTailPolicyV1 = false;    ///< `--tail-policy-v1`: judge the tail on the raw peak (pre-1.1.15 detector)
    bool probeRetainFailed = false;    ///< `--retain-failed`: keep a failed render's artifact for analysis

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
    /// Same tap, full reading: adds the per-channel mean (DC) BEFORE the first insert — the offset
    /// the instrument / proxy delivered at the chain boundary (L and R separately).
    struct InsertTapReading
    {
        float peakBefore = 0.0f;
        float peakAfter = 0.0f;
        double rmsBefore = 0.0;
        double rmsAfter = 0.0;
        double dcBefore[2] = { 0.0, 0.0 };
        std::uint32_t preBlocks = 0;
        std::uint32_t postBlocks = 0;
    };
    std::function<InsertTapReading()> readAndResetInsertLevelTapFull;
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

    // --- Export levels scenario -------------------------------------------------------
    /// Drains the engine's Stereo Out meter accumulator (everything since the previous drain).
    std::function<StabilityLevelStats()> drainMasterMeter;
    /// Selects the row the engine's track meter follows (post-strip stage) / drains it.
    std::function<void(TrackId)> setMeteredTrack;
    std::function<StabilityLevelStats()> drainTrackMeter;
    /// Production export with the level report returned. `bits` 16 / 24 / 32 (float); `mp3` uses
    /// the MP3 pipeline (bits ignored). Overwrite auto-confirmed, no progress window.
    std::function<juce::Result(const juce::File& out, bool mp3, int bits, StabilityLevelStats& report)>
        runMixdownWithLevelReport;
    /// Channel fader (linear) of a row, read / set through the Session setter the Inspector uses.
    std::function<float(TrackId)> getTrackChannelFaderGain;
    std::function<void(TrackId, float)> setTrackChannelFaderGain;
    /// Active loop span as the exporter resolves it (false when no valid cycle range).
    std::function<bool(std::int64_t& startSample, std::int64_t& lengthSamples, double& sampleRate)> getActiveLoopSpan;

    // --- Inspector panel scenario -------------------------------------------------------
    /// Geometry + mode check of the real Inspector column for the active row; appends a report.
    std::function<bool(juce::String& report, juce::String& failReason)> verifyInspectorPanelLayout;
    /// PNG of the Inspector column (scroll area + channel panel) as rendered.
    std::function<bool(const juce::File& png)> captureInspectorPng;
    /// Meter state line: displayed dB, held peak text, overload latch, DC tag of the output meter.
    std::function<juce::String()> describeInspectorMeters;
    std::function<bool()> isInspectorOutputMeterOverloadLatched;
    std::function<bool()> inspectorOutputMeterShowsSignal;
    std::function<void()> resetInspectorOverloadLatches;
    /// Same handlers the fader's value field (Return) and Ctrl/Cmd+click use.
    std::function<void(const juce::String&)> inspectorFaderTypeValue;
    std::function<void()> inspectorFaderResetGesture;
    std::function<juce::String()> inspectorFaderValueText;
    /// Scrolls the Inspector content to its bottom; true when the whole content is reachable.
    std::function<bool(juce::String& detail)> inspectorScrollToBottomAndVerify;
    /// Main window size (restored by the scenario).
    std::function<juce::Rectangle<int>()> getMainWindowBounds;
    std::function<void(int w, int h)> setMainWindowSize;

    // --- Organ DC scenario -----------------------------------------------------------------
    /// Monotonic count of blocks the row's instrument host has processed (`processOkBlocks`);
    /// a muted row must keep processing (gain 0), so this must still advance while muted.
    std::function<std::uint64_t(TrackId)> instrumentProcessedBlocks;
    /// Largest number of MIDI events the row's host delivered to its instance in one block since
    /// the last reset (`resetTo0 == true` resets after reading). Must stay bounded across a
    /// mute → unmute while playing: no stale burst.
    std::function<std::uint32_t(TrackId, bool resetTo0)> instrumentMaxMidiEventsInOneBlock;

    // --- Live MIDI scenario ----------------------------------------------------------------
    /// Builds the fixture: plugin-less instrument shell (event-recording capture sink) + "Lower"
    /// (Force 2) and "Pedal" (Force 3) Midi rows routed to it; MIDI Input = All MIDI inputs with
    /// channel filters 1 / 5 / 6. Returns the three ids.
    std::function<bool(TrackId& inst, TrackId& lower, TrackId& pedal, juce::String& failReason)> liveMidiFixtureSetup;
    /// Enumeration of real MIDI inputs/outputs (logged) and whether a loopback pair was found.
    std::function<juce::String()> liveMidiDescribeDevices;
    /// Inject one message as a device would: through a real loopback MIDI output when present,
    /// otherwise straight into the bus from a dedicated thread with a device-style timestamp.
    std::function<void(const juce::MidiMessage&)> liveMidiInject;
    std::function<bool()> liveMidiInjectUsesRealPort;
    std::function<void(TrackId, bool)> liveMidiSetMonitor;
    std::function<void(TrackId, bool)> liveMidiSetArm;
    /// Capture sink of the fixture destination: counts of a (channel, note, on/off) and a reset.
    std::function<int(int channel, int note, bool noteOn)> liveMidiCapturedNoteCount;
    std::function<int(int channel, int controller)> liveMidiCapturedCcCount;
    std::function<int(int channel)> liveMidiCapturedPitchBendCount;
    std::function<void()> liveMidiCaptureReset;
    /// After a project reload the destination runtime is new: re-install the capture sink on it.
    std::function<bool(TrackId, juce::String& failReason)> liveMidiAttachCaptureSink;
    /// Same entry points as the Record key / Stop button.
    std::function<void()> recordToggleLikeKey;
    std::function<bool()> isCountInActive;
    std::function<bool()> isRecordingInProgress;
    std::function<std::int64_t()> getTransportPlayheadSamples;
    std::function<std::int64_t()> liveMidiTakeStartSample;
    std::function<int()> reportedOutputLatencySamples;
    std::function<StabilityMidiClipSummary(TrackId)> liveMidiSummarizeClips;
    /// Export the row's first clip as SMF to `out`; returns counts via the summary-like string.
    std::function<bool(TrackId, const juce::File& out, int& notes, int& cc, int& pb, juce::String& failReason)>
        liveMidiExportFirstClip;
    std::function<int()> undoStackSize;
    std::function<void(bool)> setCycleEnabled;
    std::function<bool()> isCycleEnabled;
    /// Inspector texts for the active row (MIDI Input combo / channel combo / status line).
    std::function<juce::String()> inspectorMidiInputTexts;
    std::function<void(TrackId)> selectTrackLikeHeaderClick;
    /// Header geometry check for the live-MIDI cells of the row at the current column width.
    std::function<bool(TrackId, juce::String& report, juce::String& failReason)> verifyLiveMidiHeaderCells;
    /// First REAL MIDI input device present (identifier + name); false when none.
    std::function<bool(juce::String& identifier, juce::String& name)> liveMidiFirstRealInputDevice;
    /// Session edit: assign one specific device (Device mode) or back to All (empty identifier).
    std::function<bool(TrackId, const juce::String& identifier, const juce::String& name)> liveMidiSetTrackInputDevice;
    /// True when the device manager has the device enabled AND the coordinator's slot callback is
    /// registered for it (the real device-callback path is wired).
    std::function<bool(const juce::String& identifier, juce::String& detail)> liveMidiIsDeviceOpen;
    // --- Live MIDI: the user's REAL control paths (header cell clicks, Inspector combo picks) ---
    /// Click the row header's Monitor / Arm cell exactly like a mouse press at its centre
    /// (hit test → enabled → the header's own callback). False when the cell is absent/disabled.
    std::function<bool(TrackId, const juce::String& cell)> clickHeaderCellLikeMouse;
    /// Pick a MIDI Input item by its visible text in the Inspector (combo `onChange` → handler).
    std::function<bool(const juce::String& itemText)> inspectorChooseMidiInput;
    /// Pick the Input Channel in the Inspector (0 = All).
    std::function<bool(int channelOrZeroForAll)> inspectorChooseMidiInputChannel;
    /// The session's current active row (what the Inspector edits).
    std::function<TrackId()> getActiveTrackId;
    /// Session truth of a row's MIDI input: "none" | "all" | "device:<name>" plus " ch=<n|all>".
    std::function<juce::String(TrackId)> describeTrackMidiInputFromSession;
    /// Runtime truth: is the row's TrackId present in the bus's published routing, and with which
    /// monitor/capture flags ("absent" when not routed).
    std::function<juce::String(TrackId)> describePublishedRouteForTrack;
    /// Text of the last refused Record start (empty = the last press was accepted).
    std::function<juce::String()> lastRecordStartRefusal;
    /// First Instrument row whose Primary is a LOADED plug-in (for the audible measurement), or
    /// kInvalidTrackId.
    std::function<TrackId()> firstLoadedInstrumentRow;
    /// Audio record-arm like the header R button (`RecorderService`); `kInvalidTrackId` disarms.
    std::function<void(TrackId)> armAudioTrackForRecording;
    /// Number of timeline audio clips on a row.
    std::function<int(TrackId)> audioClipCountForTrack;

    // --- MIDI cycle takes / layering scenario --------------------------------------------------
    /// Session locators (same setters the ruler drags use).
    std::function<void(std::int64_t left, std::int64_t right)> setLocatorsSamples;
    /// Current device sample rate (0 when no device).
    std::function<double()> getDeviceSampleRate;
    /// The transport's wrap count (the counter the engine advances at every cycle wrap).
    std::function<std::uint32_t()> getCycleWrapCount;
    /// Every MIDI clip of a row in STACK order (bottom → top), one summary per clip.
    std::function<std::vector<StabilityMidiClipSummary>(TrackId)> liveMidiSummarizeAllClips;
    /// Select the row's topmost clip and delete it through the Delete-key path.
    std::function<bool(TrackId)> deleteTopmostMidiClipLikeUi;
    /// Select the row's topmost clip and move it by `deltaSamples` through the undoable edit path.
    std::function<bool(TrackId, std::int64_t deltaSamples)> moveTopmostMidiClipLikeUi;
    /// Note Ons a FRESH proxy render of `destination` would deliver (P1C snapshot + offline
    /// sequencer, no plugin): "ch:note@sample" tokens separated by spaces, in emission order.
    std::function<juce::String(TrackId destination)> proxySequencerNoteOnsForDestination;
    /// Lane-local geometry of the running take preview on a row: current-pass `[x0, x1]`, the
    /// lane's content origin x and the viewport it was computed with. False when no take runs.
    std::function<bool(TrackId, int& x0, int& x1, float& originX, std::int64_t& visibleStart, double& samplesPerPixel)>
        liveMidiTakePreviewGeometry;
    /// The overlay's current playhead frame position (what the line and the preview edge draw).
    std::function<double()> playheadDisplaySamples;
    /// Zoom the main timeline around the column centre (`factor > 1` = zoom in) / pan it.
    std::function<void(double factor)> zoomTimelineLikeWheel;
    std::function<void(std::int64_t deltaSamples)> panTimelineBySamples;
    /// Audio clip windows `(start, effective length)` of a row, newest first (track order).
    std::function<std::vector<std::pair<std::int64_t, std::int64_t>>(TrackId)> audioClipWindowsForTrack;

    // --- Proxy + recording, shared stop boundary, extent growth, schema-1 proxies -------------
    /// Simulate a machine without the Primary for one destination (`ProxyPlaybackCoordinator`
    /// test seam + refresh). The plug-in stays loaded; only the policy treats it as unavailable.
    std::function<void(TrackId, bool unavailable)> proxyForcePrimaryUnavailable;
    /// `proxyPlaybackSourceStateName(runtimeStateForTrack)`: Primary / ProxyCurrent / ProxyStale / SecondaryLive / …
    std::function<juce::String(TrackId)> proxyRuntimeStateName;
    /// Render-scheduler destination verdict: Absent / Current / Stale / Rendering / Failed.
    std::function<juce::String(TrackId)> proxyDestinationStateName;
    std::function<bool(TrackId)> proxyIsLiveRecordingOverrideActive;
    std::function<bool(TrackId)> proxyIsLiveMonitorOverrideActive;
    /// Published generation id + its recorded fingerprint schema ("<id> schema=<n>"), empty = none.
    std::function<juce::String(TrackId)> proxyGenerationInfo;
    /// Configure `target`'s Secondary from `from`'s persisted Secondary descriptor (temp project only).
    std::function<bool(TrackId target, TrackId from, juce::String& failReason)> proxyCopySecondaryConfigFromTrack;
    /// Install the scenario's capture sink on the destination's SECONDARY host (the transport host
    /// while SecondaryLive), so delivered events can be counted there.
    std::function<bool(TrackId, juce::String& failReason)> liveMidiAttachCaptureSinkToSecondary;
    /// `LiveMidiInputCoordinator::describeInputStatus` for any row (no Inspector activation).
    std::function<juce::String(TrackId)> midiInputStatusTextForTrack;
    /// Add a `TrackKind::Midi` row routed (MIDI To) to `destination` carrying one clip of
    /// `noteCount` quarter notes of `pitch` from the start (temp project only). Invalid id = failed.
    std::function<TrackId(TrackId destination, int pitch, int noteCount, juce::String& failReason)> addMidiSourceRowRoutedTo;
    /// The Inspector's "Render now" for a destination (`ProxyUpdatePolicyService::renderNow`).
    std::function<bool(TrackId)> proxyRenderNow;
    /// Render scheduler job status of a destination ("phase=… expected=… msg=… renderedMs=…").
    std::function<juce::String(TrackId)> proxyJobStatusText;
    /// The destination's proxy render snapshot sources ("own clips=N notes=M | src <id> ch=.. clips=.. notes=.. …").
    std::function<juce::String(TrackId)> proxySnapshotSourcesText;
    /// Published generation length in asset samples and its sample rate ("len=… rate=…"); empty = none.
    std::function<bool(TrackId, std::int64_t& lengthSamples, double& sampleRate)> proxyPublishedAssetShape;
    /// First note-on / last note-off of the destination's own clips in timeline seconds (false
    /// when the row has no notes). Used to place measurement windows around real content.
    std::function<bool(TrackId, double& firstNoteOnSec, double& lastNoteOffSec)> proxyTrackNoteSpanSeconds;
    /// Delete generation files of one row in the loaded project's `InstrumentProxies` folder that
    /// are NOT the published generation (leftovers of earlier test runs; the folder is shared by
    /// the sibling test copy). Returns the number of files removed. Temp project copies only.
    std::function<int(TrackId)> proxyDeleteUnpublishedGenerationFiles;
    /// Update mode → Manual for a destination through the Inspector's path (temp project only).
    std::function<bool(TrackId)> proxySetUpdateModeManual;

    // --- Proxy render probe (diagnostics: repeated isolated renders with kept artifacts) ------
    /// Capture the destination's render request through the production engine (exact state blob,
    /// snapshot, fingerprint); writes `identity-<build>.txt` into `outDir`; returns the identity
    /// text, or "ERROR: …".
    struct ProxyRenderProbeOptions
    {
        juce::File stateBlobOverride;
        bool readinessEnabled = true;
        bool nonRealtimeIndication = true;
        bool tailPolicyV1 = false;
        bool retainFailedArtifact = false;
    };
    std::function<juce::String(TrackId, const juce::File& outDir, const ProxyRenderProbeOptions&)> proxyRenderProbeCapture;
    /// Create + restore + prepare the isolated render instance for the captured request.
    std::function<bool(juce::String& failReason)> proxyRenderProbePrepare;
    /// SHA-256 + size of the prepared isolated instance's CURRENT state bytes (readiness probe).
    std::function<juce::String()> proxyRenderProbeInstanceStateHash;
    /// Start the worker render on the prepared instance (the temp artifact is kept for `finish`).
    std::function<bool(juce::String& failReason)> proxyRenderProbeStartWorker;
    std::function<bool()> proxyRenderProbeIsDone;
    /// Progress in rendered milliseconds while the worker runs.
    std::function<std::int64_t()> proxyRenderProbeProgressMs;
    /// Join + tear down, copy the artifact to `<outDir>/<label>.wav`, write `<label>.txt`
    /// (result fields, timings, content analysis, per-second profile); returns a summary line.
    std::function<juce::String(const juce::String& label)> proxyRenderProbeFinish;
    /// Publish the last finished result through the production publication (collision rule
    /// included); returns the outcome + the metadata now describing the destination.
    std::function<juce::String()> proxyRenderProbePublish;
    /// The audio take's placement compensation (latency store) — separate from the MIDI offset.
    std::function<std::int64_t()> recordingPlacementOffsetSamples;
    std::function<std::int64_t()> getStoredArrangementExtentSamples;
    std::function<std::int64_t()> getArrangementExtentSamples;
    /// The engine-acknowledged boundaries of the last finished run ("start tl=.. mono=.. wrap=.. | stop …").
    std::function<juce::String()> lastRecordRunBoundaries;
    /// Current device block size (0 = no device).
    std::function<int()> deviceBlockSizeSamples;
    /// Stop / restart the audio device (device-loss handling during a take).
    std::function<bool()> closeAudioDeviceForTest;
    std::function<bool(juce::String& failReason)> restartAudioDeviceForTest;

    // --- Mixer scenario (`--stability-mixer`) ------------------------------------------------------
    /// Every hook drives the REAL mixer window / strips (the user's controls through their own
    /// notification paths); strips are addressed by TrackId.
    struct MixerHooks
    {
        std::function<void()> toggleLikeF3;
        std::function<bool()> isVisible;
        /// Number of top-level windows titled "Mixer" on the desktop (single-instance check).
        std::function<int()> windowInstanceCount;
        std::function<bool(const juce::File& png)> capturePng;
        std::function<bool(juce::String& report, juce::String& failReason)> verifyLayout;
        std::function<std::vector<TrackId>()> stripOrder;
        std::function<juce::Rectangle<int>()> masterStripScreenBounds;
        /// Scroll the strips viewport fully to the right (returns the new x position).
        std::function<int()> scrollStripsToRight;
        std::function<void(const juce::String& sectionKey)> clickSectionToggle;
        std::function<bool(const juce::String& sectionKey)> sectionShown;
        std::function<bool(TrackId, const juce::String& text)> stripFaderType;
        std::function<juce::String(TrackId)> stripFaderValueText;
        std::function<bool(TrackId, float pan)> stripPanSet;
        std::function<bool(TrackId, int row, const juce::String& itemText)> stripChooseRouting;
        std::function<juce::String(TrackId, int row)> stripRoutingText;
        std::function<juce::String(TrackId, int row)> stripRoutingCaption;
        std::function<bool(TrackId, int row, const juce::String& itemText)> stripChooseSendDestination;
        std::function<juce::String(TrackId, int row)> stripSendDestinationText;
        std::function<bool(TrackId, int row, const juce::String& text)> stripCommitSendAmount;
        std::function<juce::String(TrackId, int row)> stripSendAmountText;
        std::function<bool(TrackId, const juce::String& text)> stripCommitPreGain;
        std::function<juce::String(TrackId)> stripPreGainText;
        /// "Pre:<name> Post:<name> …" as the strip shows it (same format as describeTrackForDiagnostics).
        std::function<juce::String(TrackId)> stripInsertRowsText;
        /// Run an insert row's context-menu action (1 open, 2 up, 3 down, 4 other stage, 5 remove).
        std::function<bool(TrackId, bool preStage, int row, int actionId)> stripInsertMenuAction;
        /// Click a base button ("power" / "mute" / "monitor" / "arm") through the cell's own click.
        std::function<bool(TrackId, const juce::String& button)> stripClickButton;
        std::function<bool(TrackId, const juce::String& button)> stripButtonActive;
        std::function<bool(TrackId, const juce::String& button)> stripButtonVisible;
        std::function<float(TrackId)> stripMeterHeldPeak;
        std::function<bool(TrackId)> stripMeterOverloadLatched;
        /// Click the strip's meter lamp (acknowledgement) through the component's own mouse path.
        std::function<void(TrackId)> stripClickMeter;
        std::function<float()> inspectorMeterHeldPeak;
        std::function<bool()> inspectorMeterOverloadLatched;
        /// Rows currently published to the engine by the meter hub.
        std::function<std::vector<TrackId>()> meterHubInterest;
        std::function<juce::Rectangle<int>()> windowBounds;
        std::function<void(juce::Rectangle<int>)> setWindowBounds;
        /// Transport intent + plug-in instance pointers + callback overrun count: a fingerprint of
        /// the audio runtime that opening / closing the mixer must not change.
        std::function<juce::String()> audioRuntimeFingerprint;
        std::function<juce::String()> stripKindTexts; ///< "id:KIND,id:KIND,…" in strip order
        /// Add a Group row exactly like the add-track menu (temp copy only); returns its id.
        std::function<TrackId()> addGroupTrackLikeUi;
    };
    MixerHooks mixer;
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
    /// Realtime Stereo Out vs offline export levels (float / 24-bit / MP3) + master fader check.
    void appendExportLevelsSteps(const juce::File& project);
    /// Inspector channel panel: modes per row kind, meters, latch, fader paths, scrolling, low window.
    void appendInspectorPanelSteps(const juce::File& project);
    void appendMixerSteps(const juce::File& project);
    /// Organ residual DC / AC before, during and after mute with the transport stopped (+ after playback).
    void appendOrganDcSteps(const juce::File& project);
    /// Live MIDI input: monitoring, routing, a recorded take, persistence, export, undo, guards.
    void appendLiveMidiSteps(const juce::File& project);
    /// MIDI cycle recording + layering (see `StabilityScenarioKind::MidiCycleTakes`).
    void appendMidiCycleTakesSteps(const juce::File& project);
    /// Proxy-backed destinations during recording + schema-1 generations (see `ProxyRecording`).
    void appendProxyRecordingSteps(const juce::File& project);
    void appendProxyRenderProbeSteps(const StabilityScenarioRequest& request);
    void appendProxyPlaybackEdgesSteps(const StabilityScenarioRequest& request);

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
    /// Set by a step action to replace its static `settleMsAfter` for this run (-1 = not set).
    int settleOverrideMsForCurrentStep_ = -1;
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
    /// ExportLevels: measurements carried between steps.
    StabilityLevelStats exportRealtimeA_;
    StabilityLevelStats exportRealtimeB_;
    StabilityLevelStats exportFloatA_;
    StabilityLevelStats exportFloatB_;
    TrackId exportMasterTrackId_ = kInvalidTrackId;
    float exportMasterFaderAtStart_ = 1.0f;
    std::int64_t exportLoopLengthSamples_ = 0;
    double exportSampleRate_ = 0.0;
    /// InspectorPanel: window bounds / fader values restored at the end.
    juce::Rectangle<int> inspectorWindowBoundsAtStart_;
    float inspectorAudioFaderAtStart_ = 1.0f;
    TrackId inspectorAudioTrackId_ = kInvalidTrackId;
    /// LiveMidi: fixture rows + the per-injection playhead stamps of the recorded take.
    TrackId liveMidiInstTid_ = kInvalidTrackId;
    TrackId liveMidiLowerTid_ = kInvalidTrackId;
    TrackId liveMidiPedalTid_ = kInvalidTrackId;
    std::int64_t liveMidiTakeStart_ = 0;
    std::int64_t liveMidiNoteOnPlayhead_ = 0;
    std::int64_t liveMidiNoteOffPlayhead_ = 0;
    std::int64_t liveMidiLowerOnPlayhead_ = 0;
    std::int64_t liveMidiLowerOffPlayhead_ = 0;
    std::int64_t liveMidiStopPlayhead_ = 0;
    int liveMidiUndoSizeBeforeTake_ = 0;
    juce::File liveMidiExportFile_;
    /// MidiCycleTakes: loop range, start boundary, clip counts before the run, evidence folder.
    std::int64_t cycleLocL_ = 0;
    std::int64_t cycleLocR_ = 0;
    std::int64_t cycleRecordStart_ = 0;
    std::uint32_t cycleWrapsAtRecordStart_ = 0;
    int cycleInstClipsBefore_ = 0;
    int cycleLowerClipsBefore_ = 0;
    int cycleUndoSizeBefore_ = 0;
    juce::File cycleEvidenceDir_;

    JUCE_DECLARE_NON_COPYABLE(StabilityScenarioRunner)
};
