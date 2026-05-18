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
};

struct ProjectFileInsertV1
{
    InsertSlotId slotId = kInvalidInsertSlotId;
    InsertStage stage = InsertStage::Post;
    juce::String pluginVst3Path;
    juce::String pluginIdentifier;
    juce::String pluginStateBase64;
};

struct ProjectFileTrackV1
{
    TrackId id = kInvalidTrackId;
    juce::String name;
    /// v13: `"audio"` (default when absent) or `"instrument"` (hosted instrument timeline lane; multiple allowed).
    juce::String kind;
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
};

struct ProjectFileExperimentalInstrumentNoteV1
{
    int midiNote = 60;
    int step = 0;
    int velocity = 100;
    int lengthSteps = 1;
};

/// v12: editable tick-domain notes (I3f). Omitted in JSON when empty (legacy step-only clips).
struct ProjectFileExperimentalTimelineNoteV12
{
    int midiNote = 60;
    int velocity = 100;
    /// 1 … 16 saved as JSON int.
    int channel = 1;
    std::int64_t startTick = 0;
    std::int64_t durationTicks = 240;
};

struct ProjectFileExperimentalInstrumentClipV1
{
    std::uint64_t id = 0;
    juce::String name { "MIDI 1" };
    int numSteps = 16;
    int stepDenom = 16;
    double bpm = 110.0;
    bool loop = true;
    /// I3d1: optional in v11 JSON (default 0); load path may derive length when absent/zero.
    std::int64_t startSamples = 0;
    std::int64_t lengthSamples = 0;
    /// When set: sample where `timelineNotes` tick 0 maps on the session timeline (non-destructive left trim).
    /// Omit in JSON for legacy clips → anchor equals `startSamples` on load.
    std::optional<std::int64_t> timelineAnchorSamples;
    int laneStartFractionPermille = 0;
    int laneEndFractionPermille = 250;
    std::vector<ProjectFileExperimentalInstrumentNoteV1> notes;
    /// v12: internal PPQ domain (default 960).
    int ticksPerQuarter = 960;
    std::vector<ProjectFileExperimentalTimelineNoteV12> timelineNotes;
    /// v12+ optional: MIDI roll horizontal scroll (samples). Omitted when no saved roll viewport.
    std::int64_t midiRollVisibleStartSamples = 0;
    /// v12+ optional: MIDI roll zoom; absence or 0 = no per-clip roll viewport in file.
    double midiRollSamplesPerPixel = 0.0;
    /// v12+ optional: piano-roll Follow playhead. Omitted when false.
    bool midiRollFollowEnabled = false;
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
};

/// Optional main application window placement (root `mainWindow` object); omitted in older projects.
/// Values are **screen** coordinates in pixels (same as `DocumentWindow::getScreenBounds()`).
struct ProjectFileMainWindowBoundsV1
{
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
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
    /// Current JSON writer version (**14** adds `tracks[].kind` = `"master"` / Stereo Out row).
    /// **13** introduced mixed `tracks[].kind` + `experimentalInstrumentTracks[].trackId`.
    /// Read path accepts older versions; see migrations in `ProjectFile.cpp`.
    static constexpr int kCurrentVersion = 14;

    int version = kCurrentVersion;
    PlacedClipId nextPlacedClipId = 1;
    TrackId nextTrackId = 2;
    TrackId activeTrackId = 1;
    std::int64_t playheadSamples = 0;
    double deviceSampleRateAtSave = 0.0;
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
