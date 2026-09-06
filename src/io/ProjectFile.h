#pragma once

// =============================================================================
// ProjectFile — v1 JSON encode/decode for minimal project persistence (io layer)
// =============================================================================
// `sourcePath` must be **relative** under `Audio/` using **forward slashes**, e.g.
// `Audio/take_YYYYMMDD_HHMMSS.wav`. Absolute paths and paths outside `Audio/` are invalid — enforced
// when saving/loading the session in `Session` (not at JSON parse time alone).
// =============================================================================

#include "domain/PlacedClip.h"
#include "domain/Track.h"
#include "plugins/InsertSlotId.h"

#include <juce_core/juce_core.h>

#include <cstdint>
#include <optional>
#include <vector>

struct ProjectFileClipV1
{
    PlacedClipId id = kInvalidPlacedClipId;
    std::int64_t startSample = 0;
    juce::String sourcePath;
    // v2: 0 = full material (read path uses `PlacedClip` default); if >0, right-edge **visible** span
    // in samples (clamped to material on load). Omitted in v1 JSON; treated as 0.
    std::int64_t visibleLengthSamples = 0;
    // v4: non-destructive left trim (file indices skipped); 0 = absent in JSON.
    std::int64_t leftTrimSamples = 0;
    // v7: bounds on permissible trim within shared material [start, end) in file indices; omit for full-material.
    std::int64_t materialWindowStartSamples = 0;
    std::int64_t materialWindowEndExclusiveSamples = 0;
    bool hasMaterialWindowInFile = false;
    // Optional user-facing event name (project metadata only; never renames the source file).
    // Empty = absent in JSON; UI falls back to the source file stem.
    juce::String name;
};

struct ProjectFileInsertV1
{
    InsertSlotId slotId = kInvalidInsertSlotId;
    InsertStage stage = InsertStage::Post;
    juce::String pluginVst3Path;
    juce::String pluginIdentifier;
    juce::String pluginStateBase64;
};

/// v15: one additive send row (`tap` written as `"postChannelStrip"`).
struct ProjectFileSendV1
{
    TrackId destTrackId = kInvalidTrackId;
    float amount = 0.0f;
    bool enabled = true;
    juce::String tap { "postChannelStrip" };
    /// Inspector slot 0..3; `-1` means assign compactly on load.
    int uiSlotIndex = -1;
};

struct ProjectFileTrackV1
{
    TrackId id = kInvalidTrackId;
    juce::String name;
    /// v13: `"audio"` (default when absent), `"instrument"`, `"group"`, or `"master"`.
    juce::String kind;
    /// v14: main output bus (`Group` or `Master` track id). Omitted on master row.
    TrackId routedOutputTrackId = kInvalidTrackId;
    std::vector<ProjectFileClipV1> clips;
    // v5: linear gain at channel-fader point (mixer). Omitted in JSON when ~ unity (see writer).
    float channelFaderGain = kTrackChannelVolumeUnityGain;
    /// Optional stereo pan [-1,+1]; omitted when ~ center (`pan` JSON key).
    float stereoPan = 0.0f;
    /// Skipped entirely by playback (JSON key `"off"`). Omitted when false.
    bool off = false;
    /// Effective output muted at engine; fader untouched (JSON `"muted"`). Omitted when false.
    bool muted = false;
    // v8: legacy single insert (read + migration). v9+ writers emit `inserts` only, not these keys.
    juce::String pluginVst3Path;
    juce::String pluginIdentifier;
    juce::String pluginStateBase64;
    // v9: ordered insert chain (Pre before Post in array order).
    std::vector<ProjectFileInsertV1> inserts;
    /// v15: ordered send list; omitted in JSON when empty.
    std::vector<ProjectFileSendV1> sends;
    /// v17: MIDI output channel (`midiChannel`) — `0` = Any/preserve each event's own channel,
    /// `1 … 16` = remap all outgoing events. Pre-v17 files have no key and load as `0`, which
    /// reproduces their old playback exactly (notes then carried their own channel, in practice 10).
    int midiOutputChannel = kTrackMidiOutputChannelAny;
    /// v18: **MIDI To** (`midiTo`) — id of the Instrument row this `"midi"` track feeds.
    /// Absent key = None (disconnected). Only meaningful when `kind == "midi"`; identity-based
    /// (a missing/non-instrument id is repaired to None on load, never retargeted).
    TrackId midiDestinationTrackId = kInvalidTrackId;
};

/// v12: editable tick-domain notes (I3f).
struct ProjectFileExperimentalTimelineNoteV12
{
    int midiNote = 60;
    int velocity = 100;
    /// Note-off velocity 0 … 127. Optional in JSON: projects saved before off-velocity support
    /// load as 64 (no schema bump; the key is simply absent).
    int offVelocity = 64;
    /// 1 … 16 saved as JSON int.
    int channel = 1;
    std::int64_t startTick = 0;
    std::int64_t durationTicks = 240;
};

/// v19: one sparse MIDI CC automation point (clip-owned, tick domain of `timelineNotes`).
/// The JSON array key is `ccPoints`; omitted when empty, absent in v18-and-older files → clips
/// load with no CC automation and identical sound (no migration function needed).
struct ProjectFileExperimentalMidiCcPointV19
{
    std::int64_t startTick = 0;
    /// Controller number 0 … 127.
    int controller = 11;
    /// Controller value 0 … 127.
    int value = 0;
    /// Native MIDI channel 1 … 16 (same convention as note `channel`).
    int channel = 1;
    /// `"hold"` or `"linear"` in JSON (`interp`); anything else repairs to hold on load.
    int interpolationToNext = 1;
};

struct ProjectFileExperimentalInstrumentClipV1
{
    std::uint64_t id = 0;
    juce::String name { "MIDI 1" };
    double bpm = 110.0;
    /// I3d1: optional in v11 JSON (default 0); load path may derive length when absent/zero.
    std::int64_t startSamples = 0;
    std::int64_t lengthSamples = 0;
    /// When set: sample where `timelineNotes` tick 0 maps on the session timeline (non-destructive left trim).
    /// Omit in JSON for legacy clips → anchor equals `startSamples` on load.
    std::optional<std::int64_t> timelineAnchorSamples;
    int laneStartFractionPermille = 0;
    int laneEndFractionPermille = 250;
    /// v12: internal PPQ domain (default 960).
    int ticksPerQuarter = 960;
    std::vector<ProjectFileExperimentalTimelineNoteV12> timelineNotes;
    /// v19+ optional: sparse MIDI CC automation points; omitted when empty.
    std::vector<ProjectFileExperimentalMidiCcPointV19> ccPoints;
    /// v12+ optional: MIDI roll horizontal scroll (samples). Omitted when no saved roll viewport.
    std::int64_t midiRollVisibleStartSamples = 0;
    /// v12+ optional: MIDI roll zoom; absence or 0 = no per-clip roll viewport in file.
    double midiRollSamplesPerPixel = 0.0;
    /// v12+ optional: piano-roll Follow playhead. Always written on save; absent on load → **true**
    /// (Follow defaults ON; older files that omitted the field when false load as ON by design).
    bool midiRollFollowEnabled = true;
};

/// v16+: persisted `PluginDescription` identity for `instrumentKind` = `"GenericVst3"`.
/// Plugin binary state uses the shared optional `pluginStateBase64` on the experimental track row (v11+).
struct ProjectFileGenericVst3DescriptorV1
{
    juce::String name;
    juce::String descriptiveName;
    juce::String manufacturerName;
    juce::String pluginFormatName;
    juce::String category;
    juce::String fileOrIdentifier;
    int uniqueId = 0;
    int deprecatedUid = 0;
    bool isInstrument = true;
};

/// v20: persisted Primary proxy metadata for one instrument destination (steering §12.2).
/// The whole object is **optional** (absent key `proxy` ⇒ no proxy). Proxy audio itself is never
/// embedded in JSON (PI-024): this is metadata plus a project-relative path only. A missing or
/// malformed object never fails project load — the reader degrades it to "no proxy" (PI-025).
struct ProjectFileProxyMetadataV20
{
    /// Content-addressed generation identity == the canonical fingerprint hash of the published
    /// render ("sha256:…"). Empty ⇒ invalid ⇒ whole object treated as absent on load.
    juce::String generationId;
    int fingerprintSchemaVersion = 1;
    /// Fingerprint hash algorithm id (1 = SHA-256, `proxy_fingerprint::kFingerprintAlgorithmId`).
    /// Optional v20 key (P1D preflight); absent loads as 1.
    int fingerprintAlgorithmId = 1;
    /// Project-relative path under `InstrumentProxies/` (forward slashes). Empty ⇒ invalid.
    juce::String relativePath;
    /// Recorded RENDER sample rate — generation identity (PI-030). Playback at another engine
    /// rate adapts (derived representation); the mismatch never invalidates the generation.
    double sampleRate = 0.0;
    /// Asset length at the recorded render rate. The asset ends when the accepted tail completes
    /// (§15.6) — it is never zero-padded to the project end.
    std::int64_t lengthSamples = 0;
    /// Rendered audio channel count (proxy v1 assets are stereo, §15.5). Optional v20 key
    /// (P1D preflight); absent loads as 2. The concrete sample format is versioned by
    /// `proxyFormatVersion` (format v1 = 32-bit-float WAV).
    int channels = 2;
    /// PI-014: `getLatencySamples` of the prepared render instance; recorded, never pre-trimmed.
    int pluginLatencySamples = 0;
    int latencyPolicyVersion = 1;
    int tailPolicyVersion = 1;
    int renderPolicyVersion = 1;
    int proxyFormatVersion = 1;
    /// ISO-8601 UTC render timestamp (informational only; never part of validity identity).
    juce::String renderedUtc;
    /// P1F optional (§15.7): explicit silent generation — a valid published generation with NO
    /// audio asset (empty `relativePath`, `lengthSamples` 0). Playback of a silent generation is
    /// silence by definition. Absent loads as false; a silent generation is valid only with an
    /// empty path (no ambiguous fake paths), a non-silent one only with a non-empty path.
    bool silentGeneration = false;

    // ------------------------------------------------------------------ P1G
    // Recorded identity inputs of the published generation (steering §12.3):
    // currency is evaluated by recomputing the expected fingerprint UNDER THE
    // GENERATION'S RECORDED RENDER CONFIGURATION — these fields make that
    // recomputation possible when the Primary plugin is not loaded (the
    // portable case, PI-030). All keys are optional/additive; absent values
    // load as the defaults below. When `pluginFileOrIdentifier` is empty the
    // recorded identity is unknown (pre-P1G metadata) and missing-Primary
    // currency conservatively evaluates to Stale.
    /// F1 identity inputs exactly as hashed at render time.
    juce::String pluginFileOrIdentifier;
    int pluginUniqueId = 0;
    int pluginDeprecatedUid = 0;
    juce::String pluginFormatName;
    bool pluginIsInstrument = true;
    juce::String pluginVersionAtRender;
    /// F2 state-identity component recorded at publication (§9.4.2). Never a blob hash.
    std::int64_t primaryStateRevisionAtPublish = 0;
    bool pairedWithSavedStateAtRender = false;
    /// Save pairing stamp: the destination host's live semantic revision at the moment this
    /// metadata was last SAVED with a loaded Primary. Pairing holds when it equals
    /// `primaryStateRevisionAtPublish` — the saved plugin state blob is then by construction
    /// the state this generation rendered (§12.3 "persisted save-pairing"). 0 = never stamped.
    std::int64_t primaryStateRevisionAtSave = 0;
    /// F10/F11 render-config inputs as recorded (0/defaults = unknown → fall back to
    /// the current session/policy constants).
    double timelineReferenceRate = 0.0;
    int renderBlockSize = 512;
    int noteOffGateMs = 100;
};

/// v11: experimental Groove Agent instrument row + in-memory MIDI clips (advisory `pluginBundlePath` only).
/// Optional `pluginStateBase64`: Base64 `AudioPluginInstance::getStateInformation` when saved with plug-in loaded.
struct ProjectFileExperimentalInstrumentTrackV1
{
    /// v13+: binding row in `tracks[]`; 0 before migration / legacy payloads.
    TrackId trackId = kInvalidTrackId;
    bool enabled = true;
    juce::String name { "Groove Agent SE" };
    juce::String instrumentKind { "GrooveAgentSE" };
    juce::String requiredKitName { "FiftySixDegreesModified" };
    /// Local hint only; may be missing on another machine (path repair may still find the plugin).
    juce::String pluginBundlePath;
    /// Optional Base64 blob from `AudioPluginInstance::getStateInformation` when saved with plugin loaded.
    juce::String pluginStateBase64;
    bool pluginWasLoadedOnSave = false;
    bool powerOn = true;
    bool muted = false;
    /// v12+ optional: manual drum-row labels (JSON `"drumNoteNames"`); user overrides keyed by MIDI note (0–127).
    /// Omitted when empty.
    std::vector<std::pair<int, juce::String>> drumNoteNameOverrides;
    /// v12+ optional: persisted `autoPlugin` kit names (JSON `"drumNoteNamesAutoPlugin"`). Omitted when empty / absent on load.
    std::vector<std::pair<int, juce::String>> drumNoteNameAutoPlugin;
    std::vector<ProjectFileExperimentalInstrumentClipV1> clips;
    /// v16+: present when JSON carries `genericVst3Descriptor` for catalog instrument restore.
    bool hasGenericVst3Descriptor = false;
    ProjectFileGenericVst3DescriptorV1 genericVst3Descriptor;
    /// v20+ optional (`pluginVersion`): `PluginDescription::version` captured at save while the
    /// plugin was loaded (fingerprint input F1v, steering §9.3). Empty = unknown/absent — older
    /// files and saves without a loaded plugin carry no version. Same-version-different-binary
    /// upgrades are accepted as undetectable in v1 (documented limitation).
    juce::String pluginVersion;
    /// v20+ optional (`proxy` object): present only when `hasProxy`. Absent/malformed ⇒ no proxy.
    bool hasProxy = false;
    ProjectFileProxyMetadataV20 proxy;
    /// v20+ optional (`proxyUpdateMode`): "auto" | "onSave" | "manual" | "off" (steering §18.1).
    /// Absent or unrecognized loads as "auto"; the key is omitted on save when it equals "auto".
    juce::String proxyUpdateMode { "auto" };
};

/// Optional main application window placement (root `mainWindow` object); omitted in older projects.
/// Values are **screen** coordinates in pixels (same as `DocumentWindow::getScreenBounds()`).
struct ProjectFileMainWindowBoundsV1
{
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    /// Optional `maximized` JSON property (absent in older files → false). When true, the saved
    /// x/y/w/h are the maximized bounds; restore applies bounds first, then re-maximizes.
    bool maximized = false;
    /// Optional `followPlayhead` JSON property, only used on the root `mainWindow` object (the
    /// MIDI editor persists its Follow per clip). Absent in older files → **true** (Follow ON).
    bool followPlayhead = true;
};

/// Optional MIDI editor workspace (root `midiEditorWorkspace`); omitted in older projects and when
/// the editor was closed at save. Window bounds live separately in `midiEditorWindow`. Horizontal
/// roll scroll/zoom/Follow are per-clip (`midiRoll*` clip fields), not duplicated here.
struct ProjectFileMidiEditorWorkspaceV1
{
    /// True when the MIDI editor window was open (visible) at save; load may auto-reopen it.
    bool open = false;
    /// Timeline instrument track the editor was bound to (0 = unknown).
    juce::int64 instrumentTrackId = 0;
    /// Bound `InstrumentMidiClip` id (0 = unknown/scratch).
    juce::int64 clipId = 0;
    /// Vertical pitch scroll: topmost visible MIDI pitch at the grid (-1 = absent).
    int topVisibleMidiPitch = -1;
    /// Velocity lane height in px; 0 = minimized, -1 = absent (use default).
    int velocityLaneHeight = -1;
    /// Row label mode: 1 = piano, 2 = drum names, 0 = absent (auto from kit).
    int rowLabelMode = 0;
};

/// Optional Audio Mixdown UI settings (root `audioMixdown`); omitted in older projects.
struct ProjectFileAudioMixdownV1
{
    juce::String fileNameWithoutExtension;
    /// Relative path under project folder (forward slashes), e.g. `"Mixdown"`, or an absolute OS path.
    juce::String outputDirectory;
    /// `"wave"` (default) or `"mpeg1Layer3"`.
    juce::String fileType;
    /// 16 / 24 / 32, or **0** when absent or invalid in JSON.
    int wavBitDepth = 0;
    int mp3BitRateKbps = 320;
};

// Minimal project snapshot: multi-track, placed clips, monotonic id seeds, transport hints.
struct ProjectFileV1
{
    /// Current JSON writer version (**20** adds the root `timelineSampleRate` timeline reference
    /// rate — TLD-1, steering §10.1 — plus `experimentalInstrumentTracks[].pluginVersion`,
    /// optional `.proxy` metadata, and `.proxyUpdateMode`; all additive with absent-key defaults).
    /// **19** adds `experimentalInstrumentTracks[].clips[].ccPoints`
    /// — sparse MIDI CC automation. **18** adds `tracks[].kind == "midi"` rows with `midiTo`.
    /// **17** adds `tracks[].midiChannel`. **16** adds `experimentalInstrumentTracks[].genericVst3Descriptor`.
    /// **15** adds `tracks[].sends[]`.
    static constexpr int kCurrentVersion = 20;

    int version = kCurrentVersion;
    PlacedClipId nextPlacedClipId = 1;
    TrackId nextTrackId = 2;
    TrackId activeTrackId = 1;
    std::int64_t playheadSamples = 0;
    double deviceSampleRateAtSave = 0.0;
    /// v20: the **timeline reference sample rate** (TLD-1, steering §10.1): the rate under which
    /// every persisted sample-domain timeline field (clip placements, MIDI clip anchors/windows,
    /// locators, arrangement extent, playhead) is interpreted. Separate coordinate domain from the
    /// live audio-device rate: a device-rate change MUST NOT re-stamp this field and MUST NOT
    /// reinterpret the stored integers. The reader always normalizes this field to a valid rate:
    /// v19-and-older files (and malformed values) initialize it from `deviceSampleRateAtSave`
    /// (best available historical reference; 48000 when that is also invalid). Note the Locked
    /// honesty limitation: migration cannot reconstruct timing a historical v19 re-save at a
    /// different device rate already destroyed — v20 pins the interpretation from migration on.
    double timelineSampleRate = 0.0;
    // v3: effective arrangement extent in samples (optional in JSON; 0 = treat as “absent / floor
    // from content only” on load — see `SessionSnapshot::withTracks`).
    std::int64_t arrangementExtentSamples = 0;
    // v6: timeline locators (samples). Omitted in JSON when 0; `right == 0` = right locator unset.
    std::int64_t leftLocatorSamples = 0;
    std::int64_t rightLocatorSamples = 0;
    // v10: cycle/loop armed (Transport). Omitted in JSON when false (default).
    bool cycleEnabled = false;
    /// Optional root (Slice A): global musical metadata; absent in older files → struct defaults.
    double bpm = 120.0;
    int timeSignatureNumerator = 4;
    int timeSignatureDenominator = 4;
    int ticksPerQuarter = 960;
    /// Optional root (Slice C): arrangement snap UI; absent in older files → defaults below.
    bool snapEnabled = false;
    juce::String snapResolution { "1_4" };
    /// Optional root `mainWindow` object; absent in older files → do not restore window geometry.
    bool hasMainWindowBounds = false;
    ProjectFileMainWindowBoundsV1 mainWindowBounds;
    /// Optional root `midiEditorWindow` object; same shape/coordinates as `mainWindow`. Absent in
    /// older files → MIDI editor opens with its default centred size. Never auto-opens the editor.
    bool hasMidiEditorWindowBounds = false;
    ProjectFileMainWindowBoundsV1 midiEditorWindowBounds;
    /// Optional root `midiEditorWorkspace` object; absent in older files → no auto-reopen.
    bool hasMidiEditorWorkspace = false;
    ProjectFileMidiEditorWorkspaceV1 midiEditorWorkspace;
    std::vector<ProjectFileTrackV1> tracks;
    // v11+: optional; omitted in older files — empty after read.
    std::vector<ProjectFileExperimentalInstrumentTrackV1> experimentalInstrumentTracks;
    /// Present when JSON root contains optional `audioMixdown` object (any version).
    bool hasAudioMixdown = false;
    ProjectFileAudioMixdownV1 audioMixdown;
};

[[nodiscard]] juce::Result writeProjectFile(const juce::File& file, const ProjectFileV1& data);
[[nodiscard]] juce::Result readProjectFile(const juce::File& file, ProjectFileV1& outData);

// --- Musical undo (I3i): same DTO shape as project save, but never carries plugin state ----------
void stripExperimentalInstrumentTrackPluginFieldsForUndo(ProjectFileExperimentalInstrumentTrackV1& t) noexcept;
[[nodiscard]] bool experimentalInstrumentTracksMusicalUndoEqual(
    const std::vector<ProjectFileExperimentalInstrumentTrackV1>& a,
    const std::vector<ProjectFileExperimentalInstrumentTrackV1>& b) noexcept;
