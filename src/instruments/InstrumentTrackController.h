#pragma once

// =============================================================================
// InstrumentTrackController — experimental instrument track + runtime MIDI clips (I3b)
// =============================================================================
//
// Message-thread only. Holds MIDI clip state + render snapshots for **one** timeline `TrackId`
// (paired with that lane’s `ExperimentalInstrumentHost`). Many instrument lanes ⇒ many controllers.
// Project persistence: experimental `experimentalInstrumentTracks[]` payloads, v11+ conventions.
// `instrumentLoaded_`: whether **this** lane’s paired host currently has an instrument loaded—not a global singleton flag.
//
// =============================================================================

#include "io/ProjectFile.h"
#include "ui/experimental/ExperimentalMidiPattern.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include <juce_events/juce_events.h>

class ExperimentalInstrumentHost; // IWYU: full type in .cpp for autoload + I3e transport MIDI
class Session;

using InstrumentMidiClipId = std::uint64_t;

struct InstrumentMidiClip
{
    InstrumentMidiClipId id = 0;
    juce::String name { "MIDI 1" };
    ExperimentalMidiPattern pattern;
    /// Session-timeline anchor (samples). I3d1: piano roll + lane use absolute samples.
    std::int64_t startSamples = 0;
    /// Sample where `timelineNotes` tick 0 sits on the session timeline (non-negative).
    /// Matches `startSamples` until left trim moves the visible edge; outward left trim may place
    /// `startSamples` earlier than this anchor (empty lead-in before tick zero).
    std::int64_t timelineAnchorSamples = 0;
    /// Locked pattern span in samples (explicit; user trim deferred). Recomputed only on clip create,
    /// load when missing/zero, or **numSteps / stepDenom** change — **not** on BPM-only edits.
    std::int64_t lengthSamples = 0;
    /// Legacy fractional lane layout when main timeline mapping is unavailable (fallback).
    int laneStartFractionPermille = 0;
    int laneEndFractionPermille = 250;

    /// MIDI piano-roll UI viewport (message thread). `midiRollSamplesPerPixel <= 0` = never set / use default seed.
    std::int64_t midiRollVisibleStartSamples = 0;
    double midiRollSamplesPerPixel = 0.0;
    bool midiRollFollowEnabled = false;
};

/// I3e: immutable copy of all experimental-clip MIDI for the audio thread (no raw `clips_` access).
/// I3f+: `midiChannel` is 1‑based (Groove Agent + standard MIDI numbering).
struct InstrumentNoteRenderEvent
{
    std::int64_t absSample = 0;
    /// Timeline notes: absolute sample for note-off. **0** means use `InstrumentTrackRenderSnapshot::gateSamples`
    /// after `absSample` (step grid / legacy).
    std::int64_t noteOffAbsSample = 0;
    std::uint8_t midiNote = 60;
    std::uint8_t velocity = 100;
    std::uint8_t midiChannel = 1;
};

struct InstrumentClipRenderPlan
{
    std::int64_t startSamples = 0;
    std::int64_t endSamplesExclusive = 0;
    std::vector<InstrumentNoteRenderEvent> notes;
};

struct InstrumentTrackRenderSnapshot
{
    std::uint32_t revision = 0;
    bool playbackEnabled = false;
    int midiChannel = 1;
    int gateSamples = 4800;
    /// Sorted by `startSamples`. Notes sorted by `absSample` within each clip.
    std::vector<InstrumentClipRenderPlan> clips;
};

/// Tracks whether a MIDI editor drum-row label came from the user vs plugin discovery (`mergeAutoPluginDrumLabels`).
enum class DrumLabelSource
{
    manual,
    autoPlugin
};

/// One visible drum label + its source (what `getEffectiveDrumLabel` returns).
/// Per-note storage is layered: manual and autoPlugin may both exist internally; manual shadows autoPlugin until cleared.
struct InstrumentTrackDrumLabel
{
    juce::String name;
    DrumLabelSource source = DrumLabelSource::manual;
};

class InstrumentTrackController : public juce::ChangeBroadcaster
{
public:
    explicit InstrumentTrackController(ExperimentalInstrumentHost& host) noexcept;

    void setSession(Session* session) noexcept { session_ = session; }

    /// Timeline `TrackId` of **`this` controller's** `TrackKind::Instrument` lane (session snapshot order).
    /// `kInvalidTrackId` when this controller row is inactive / unbound.
    [[nodiscard]] TrackId getExperimentalInstrumentDomainTrackId() const noexcept
    {
        return experimentalDomainTrackId_;
    }

    /// Instrument track row exists (Add Instrument Track succeeded).
    [[nodiscard]] bool hasInstrumentTrack() const noexcept { return trackActive_; }

    /// Alias for older call sites.
    [[nodiscard]] bool hasInstrumentTrackShell() const noexcept { return trackActive_; }

    /// True when **`this` controller's** paired host reports a Groove Agent–class instrument (by name heuristic).
    [[nodiscard]] bool isInstrumentLoaded() const noexcept { return instrumentLoaded_; }

    /// Binds runtime clips + mute/power shell state to an existing `TrackKind::Instrument` row (`sessionInstrumentTrackId`).
    /// Caller must publish the session instrument shell first (`Session::appendExperimentalInstrumentShellTrack`).
    [[nodiscard]] bool bootstrapGrooveAgentShellForSessionTrack(TrackId sessionInstrumentTrackId) noexcept;

    /// HALion Sonic instrument lane: same session shell as Groove; no kit name; `instrumentKind` HALionSonic.
    [[nodiscard]] bool bootstrapHalionSonicShellForSessionTrack(TrackId sessionInstrumentTrackId) noexcept;

    /// Generic catalog VST3 instrument lane (`instrumentKind` GenericVst3).
    [[nodiscard]] bool bootstrapGenericCatalogInstrumentShellForSessionTrack(
        TrackId sessionInstrumentTrackId) noexcept;

    [[nodiscard]] bool isGenericCatalogInstrument() const noexcept
    {
        return experimentalInstrumentKind_ == "GenericVst3";
    }

    /// Legacy path: asks `Session` to append one instrument shell, then binds this controller row.
    /// Prefer `bootstrapGrooveAgentShellForSessionTrack` when naming / ordering is orchestrated externally.
    [[nodiscard]] bool tryAddGrooveAgentInstrumentTrackShell();

    /// Refresh instrumentLoaded_ from host. Never deletes the track or clips.
    void syncShellWithHostState();

    /// Legacy lane-header hooks: display name is **`Track::getName()`** from the session (`InstrumentTimelineRowCoordinator`).
    /// These return empty; subtitles are unused.
    [[nodiscard]] juce::String getLaneHeaderTitle() const;
    [[nodiscard]] juce::String getLaneHeaderSubtitle() const;
    /// Empty when `getLaneHeaderTitle()` is empty; otherwise legacy two-line concatenation.
    [[nodiscard]] juce::String getLaneHeaderText() const;

    /// Experimental lane header **Power** (on = processed); default on when the track shell exists.
    [[nodiscard]] bool isPowerOn() const noexcept { return powerOn_; }
    void setPowerOn(bool on) noexcept;

    /// Experimental lane header **Mute**; default off when the track shell exists.
    [[nodiscard]] bool isMuted() const noexcept { return muted_; }
    void setMuted(bool muted) noexcept;

    /// UI-only "active row" highlight (mutex with audio header selection in `TransportControlsContent`).
    /// `TrackHeaderView` reads this to render the slate-blue active variant.
    [[nodiscard]] bool isActive() const noexcept { return isActive_; }
    void setActive(bool active) noexcept;

    /// Persisted project DTO kind (`GrooveAgentSE` / `HALionSonic`). Message thread; UI/editor only.
    [[nodiscard]] const juce::String& getExperimentalInstrumentKind() const noexcept
    {
        return experimentalInstrumentKind_;
    }

    [[nodiscard]] const std::vector<std::unique_ptr<InstrumentMidiClip>>& getClips() const noexcept
    {
        return clips_;
    }

    [[nodiscard]] InstrumentMidiClip* getClipById(InstrumentMidiClipId id) noexcept;
    [[nodiscard]] const InstrumentMidiClip* getClipById(InstrumentMidiClipId id) const noexcept;

    /// t in [0,1] along the event lane (excluding header).
    [[nodiscard]] InstrumentMidiClip* findClipAtLaneFraction(float t) noexcept;

    /// Active / keyboard-focused MIDI clip on this lane (`selectedClipIds_.back()`, or 0 when none).
    [[nodiscard]] InstrumentMidiClipId getSelectedClipId() const noexcept;

    /// Compatibility shim — replaces selection with a single clip, or clears when `id == 0`.
    void setSelectedClipId(InstrumentMidiClipId id) noexcept;

    [[nodiscard]] const std::vector<InstrumentMidiClipId>& getSelectedClipIds() const noexcept
    {
        return selectedClipIds_;
    }

    [[nodiscard]] bool isClipSelected(InstrumentMidiClipId id) const noexcept;

    void setSelectedClipIdsExclusive(InstrumentMidiClipId activeClipId) noexcept;
    void addClipToSelection(InstrumentMidiClipId id) noexcept;
    void toggleClipSelection(InstrumentMidiClipId id) noexcept;

    /// Reorders selection only if `id` is already selected (otherwise no-op).
    void setActiveSelectedClipId(InstrumentMidiClipId id) noexcept;

    void clearClipSelection() noexcept;

    [[nodiscard]] const juce::String& getRequiredKitName() const noexcept { return requiredKitName_; }
    void setRequiredKitName(juce::String name) noexcept;

    /// MIDI channel for `AudioPluginInstance::getNameForMidiNoteNumber` (1-based). Defaults to 10 (GM drums);
    /// when `contextClip` has timeline notes and they share a single channel, that channel is used.
    [[nodiscard]] int pluginNoteNameQueryChannel(const InstrumentMidiClip* contextClip = nullptr) const noexcept;

    /// User drum-row label overrides (shim → `setDrumLabelManual` / manual layer only). MIDI note 0–127 only.
    /// Empty string erases the manual label only (`autoPlugin` may still appear via `getEffectiveDrumLabel`).
    [[nodiscard]] juce::String getDrumNoteUserOverride(int midiNote) const noexcept;
    void setDrumNoteUserOverride(int midiNote, juce::String displayName) noexcept;

    /// Manual drum label for `midiNote` (0–127). Non-empty replaces manual; empty erases manual and reveals any
    /// `autoPlugin` label for that note. Does not mutate `autoPlugin` entries except via `mergeAutoPluginDrumLabels`.
    void setDrumLabelManual(int midiNote, juce::String name) noexcept;

    /// Merge discovered names from the live plugin instance (future). Does not overwrite manual labels, does not
    /// erase existing autoPlugin rows for notes absent from `discovered`, ignores empty discovered names.
    /// `pluginIdentifier` is accepted for forward use; not persisted in this slice.
    void mergeAutoPluginDrumLabels(const std::map<int, juce::String>& discovered,
                                   const juce::String& pluginIdentifier);

    /// Effective label for Drum Names UI: manual if set, else autoPlugin if set; `std::nullopt` if neither.
    [[nodiscard]] std::optional<std::pair<juce::String, DrumLabelSource>> getEffectiveDrumLabel(int midiNote) const;

    /// True when at least one note has an effective drum label (manual or plugin-discovered). Used by the
    /// MIDI editor to default its rows to Drum Names when readable names exist (e.g. a Groove Agent kit).
    [[nodiscard]] bool hasAnyEffectiveDrumLabels() const noexcept;

    /// When this track has no drum labels yet but the host holds a loaded instrument, ask the host to
    /// re-probe plugin drum names (kit may have been picked after the last probe). Rate-limited to one
    /// request per 2 s; results arrive via `mergeAutoPluginDrumLabels` → change message. Message thread.
    void requestPluginDrumNameProbeIfUnlabeled() noexcept;

    /// One enabled row for `experimentalInstrumentTracks` when `hasInstrumentTrack()`.
    [[nodiscard]] ProjectFileExperimentalInstrumentTrackV1 buildExperimentalInstrumentProjectBlock() const;

    /// I3i: Musical-only undo snapshot (no plugin path, state blob, or autoload hints). Empty when
    /// no instrument track exists.
    [[nodiscard]] std::vector<ProjectFileExperimentalInstrumentTrackV1>
    buildExperimentalInstrumentMusicalUndoBlock() const;

    /// I3i: Replace clips + patterns + musical timing from a musical undo/rest **without** touching
    /// the loaded instrument plugin or project-load autoload state.
    void applyExperimentalInstrumentMusicalUndoBlock(
        const std::vector<ProjectFileExperimentalInstrumentTrackV1>& tracks);

    /// Replace in-memory track + clips from project (message thread). Clears track when `tracks` empty
    /// or no enabled GrooveAgentSE row (**first** GA row wins — retained for callers that pass a sliced vector).
    void restoreExperimentalInstrumentFromProject(
        const std::vector<ProjectFileExperimentalInstrumentTrackV1>& tracks,
        const std::vector<ProjectFileTrackV1>* persistedSerializedTrackRowsForBind = nullptr);

    /// Restore a **single** `experimentalInstrumentTracks[]` row onto **this** controller instance.
    void restoreExperimentalInstrumentSingleProjectRow(
        const ProjectFileExperimentalInstrumentTrackV1& row,
        const std::vector<ProjectFileTrackV1>* persistedSerializedTrackRowsForBind);
    /// [Message thread] True when serialized experimental payload carries an enabled Groove Agent lane.
    [[nodiscard]] static bool serializedProjectUsesEnabledGrooveAgentRow(
        const std::vector<ProjectFileExperimentalInstrumentTrackV1>& tracks) noexcept;

    /// Resolve which persisted `tracks[]` row should bind **one** `experimentalInstrumentTracks[]` payload
    /// — **before** `Session::applyLoadedProjectModel` (no live snapshot yet). Uses serialized rows +
    /// optional live `session` validation when reloading (pass `nullptr` pre-load).
    [[nodiscard]] static TrackId peekExperimentalInstrumentBindLaneId(
        Session* sessionNullable,
        const std::vector<ProjectFileExperimentalInstrumentTrackV1>& payloads,
        const std::vector<ProjectFileTrackV1>& persistedTracks) noexcept;

    /// Resolve `dto.trackId` against the persisted track list and (when non-null) the live snapshot.
    [[nodiscard]] static TrackId resolveExperimentalInstrumentLaneIdFromProjectFields(
        Session* sessionNullable,
        TrackId dtoTrackField,
        const std::vector<ProjectFileTrackV1>* persistedSerializedTracksMaybe) noexcept;

    /// After `loadProjectFromFile`, attempts cache load + optional Windows path repair + host load.
    void runPendingGrooveAgentProjectAutoload(ExperimentalInstrumentHost& host, juce::String& outWarning);

    /// HALion Sonic project restore path (peer to Groove).
    void runPendingHalionSonicProjectAutoload(ExperimentalInstrumentHost& host, juce::String& outWarning);

    /// Generic catalog VST3 project restore (descriptor + catalog repair; no plugin binary state).
    void runPendingGenericVst3ProjectAutoload(ExperimentalInstrumentHost& host, juce::String& outWarning);

    /// Device sample rate for **musical** length derivation (message thread). Does not rescale clips.
    void setTimelineSampleRate(double sampleRate) noexcept;

    /// Same rate as set by `setTimelineSampleRate` (arrangement note-preview tick->sample mapping).
    [[nodiscard]] double getTimelineSampleRate() const noexcept { return timelineSampleRate_; }

    /// Recompute `lengthSamples` from pattern grid + `timelineSampleRate_` (create / load repair /
    /// numSteps or stepDenom edits only — not BPM-only).
    void recomputeLockedClipLengthFromPatternGrid(InstrumentMidiClip& clip) noexcept;

    /// [Message thread] Piano roll / pattern edits: republish audio snapshot (note grid + gate).
    void notifyClipPatternMutated(InstrumentMidiClipId clipId) noexcept;

    /// BPM / timeline note timing changed (ticks→samples); does **not** rewrite `lengthSamples` from grid.
    void notifyClipExperimentalMusicalTimingChanged() noexcept;

    /// Clips always play at the project tempo: overwrite every clip's `pattern.bpm` with the session's
    /// project BPM and republish timing when anything changed. Message thread only; no-op without a session.
    void alignClipTemposToProjectTempo() noexcept;

    /// Arrangement import: append one timeline-MIDI clip anchored at `startSamples`. Message thread only.
    /// The clip adopts the **project tempo** (file tempo events are ignored; note ticks preserve bar/beat
    /// positions). Returns **0** when no instrument shell is active (`!trackActive_`).
    [[nodiscard]] InstrumentMidiClipId appendImportedTimelineMidiClipAtSamples(
        std::vector<TimelineMidiNote> timelineNotes,
        std::int64_t startSamples,
        juce::String suggestedName);

    /// Arrangement double-click on empty lane space: append an empty timeline-mode clip (no notes yet)
    /// anchored at `startSamples`, defaulting to two project-tempo bars. Message thread only.
    /// Returns **0** when no instrument shell is active (`!trackActive_`).
    [[nodiscard]] InstrumentMidiClipId createEmptyTimelineMidiClipAtSamples(std::int64_t startSamples);

    /// Arrangement move: shift all currently selected MIDI clips by `deltaSamples`. Clamps as a group so no
    /// clip starts before sample 0. Returns false if nothing changed.
    [[nodiscard]] bool moveSelectedInstrumentMidiClipsByDeltaSamples(std::int64_t deltaSamples) noexcept;

    /// Arrangement delete: remove clips by id on this lane. Prunes selection and republishes render snapshot.
    /// Returns false when nothing was removed.
    [[nodiscard]] bool removeInstrumentMidiClipsByIds(const std::vector<InstrumentMidiClipId>& ids) noexcept;

    /// Arrangement paste: append copies of `snapshotsInOrder` with new ids and supplied timeline positions.
    /// `startAndAnchorsInOrder[i]` is `(startSamples, timelineAnchorSamples)` aligned with `snapshotsInOrder`.
    /// Returns new ids in the same order (empty on failure / inactive track / size mismatch).
    [[nodiscard]] std::vector<InstrumentMidiClipId> appendDeepCopiedInstrumentMidiClips(
        const std::vector<InstrumentMidiClip>& snapshotsInOrder,
        const std::vector<std::pair<std::int64_t, std::int64_t>>& startAndAnchorsInOrder) noexcept;

    /// Replace multi-selection in traversal order; active clip is the last element (keyboard/editor focus).
    void replaceInstrumentMidiClipSelectionOrdered(std::vector<InstrumentMidiClipId> orderedIds) noexcept;

    /// Timeline-authoritative clips only (`timelineNotes` non-empty): set visible arrangement bounds without
    /// moving `timelineAnchorSamples`. Supports inward trim (hide notes) and outward trim (empty regions
    /// before the anchor or after the natural pattern end). Note timing stays fixed in session samples.
    /// Returns false when unchanged or clip not eligible.
    [[nodiscard]] bool applyInstrumentMidiClipVisibleTrim(InstrumentMidiClipId id,
                                                           std::int64_t newVisibleStartSamples,
                                                           std::int64_t newVisibleLengthSamples) noexcept;

    /// Effective delta after clamping the current selection so every clip stays at or past sample 0.
    [[nodiscard]] std::int64_t clampInstrumentMidiClipMoveDeltaForCurrentSelection(
        std::int64_t deltaSamples) const noexcept;

    /// Set the user-facing clip name (trimmed). Returns false when unknown id, empty after trim,
    /// or unchanged. Display/persistence metadata only — playback and note data untouched.
    [[nodiscard]] bool renameInstrumentMidiClip(InstrumentMidiClipId id, juce::String newName) noexcept;

    [[nodiscard]] std::shared_ptr<const InstrumentTrackRenderSnapshot> loadRenderSnapshotForAudioThread() const noexcept
    {
        return std::atomic_load_explicit(&renderSnapshot_, std::memory_order_acquire);
    }

    /// [Audio thread] Sample-accurate Groove Agent MIDI for one render segment (half-open times).
    /// When `outMidiEventsEmitted` is non-null, increments once per successful `audioThread_addMidiEventForCurrentBlock`.
    void audioThread_scheduleTransportMidiForSegment(ExperimentalInstrumentHost& host,
                                                     std::int64_t timelineSegStart,
                                                     int segNumSamples,
                                                     int bufferOffsetInDevice,
                                                     bool forceDiscontinuity,
                                                     int deviceBlockNumSamples,
                                                     int* outMidiEventsEmitted = nullptr) noexcept;

    /// [Audio thread] Stop/flush: pending transport offs + allNotesOff(1).
    void audioThread_flushTransportMidi(ExperimentalInstrumentHost& host,
                                        int offsetInDevice,
                                        int deviceBlockNumSamples) noexcept;

private:
    void pruneInstrumentMidiClipSelectionToExistingClips() noexcept;

    [[nodiscard]] bool computeInstrumentLoadedFromHost() const noexcept;

    ExperimentalInstrumentHost& host_;
    Session* session_ = nullptr;
    TrackId experimentalDomainTrackId_ = kInvalidTrackId;
    bool trackActive_ = false;
    bool instrumentLoaded_ = false;
    InstrumentMidiClipId nextClipId_ = 1;
    /// UI-only multi-selection; active clip is `back()`. Not serialized or part of musical undo snapshots.
    std::vector<InstrumentMidiClipId> selectedClipIds_;
    std::vector<std::unique_ptr<InstrumentMidiClip>> clips_;
    bool powerOn_ = true;
    bool muted_ = false;
    bool isActive_ = false;

    juce::String requiredKitName_;
    juce::String pendingPluginStateBase64_;
    bool pendingProjectGrooveAutoload_ = false;
    bool pendingProjectHalionSonicAutoload_ = false;
    bool pendingProjectGenericVst3Autoload_ = false;
    bool pendingGenericVst3DescriptorValid_ = false;
    ProjectFileGenericVst3DescriptorV1 pendingGenericVst3Descriptor_;
    juce::String pendingAdvisoryPluginBundlePath_;
    juce::String pendingInstrumentKind_;
    /// Persisted/display DTO kind for this lane (`GrooveAgentSE` / `HALionSonic`). Not consulted on audio thread.
    juce::String experimentalInstrumentKind_;

    double timelineSampleRate_ = 48000.0;

    /// Per MIDI note (0–127): optional manual label and optional autoPlugin label (both may exist; manual wins in UI).
    /// Distinct keys are erased when both strings are empty. Not part of musical undo snapshots.
    struct DrumLabelLayers
    {
        juce::String manual;
        juce::String autoPlugin;
    };

    std::map<int, DrumLabelLayers> drumLabels_;

    /// Rate limit for `requestPluginDrumNameProbeIfUnlabeled` (millisecond counter of the last request).
    std::uint32_t lastDrumNameProbeRequestMs_ = 0;

    void pruneDrumLabelLayersIfUnused(int midiNote) noexcept;

    std::atomic<std::shared_ptr<const InstrumentTrackRenderSnapshot>> renderSnapshot_;
    std::uint32_t nextSnapshotRevision_ = 1;
    /// [Message thread] Dedup fingerprint for instrument-render snapshots; **`appendExperimentalPlaybackRoutingLogLine`
    /// runs only when** `MINIDAW_DIAG_PLAYBACK_ROUTING` is non-zero (default **`0`** — `DiagnosticBuildFlags.h`).
    /// When disabled, fingerprints still advance but nothing is written to `experimental-playback-routing.log`.
    juce::String lastExperimentalPlaybackRoutingRenderFingerprint_;

    void publishRenderSnapshot();
    void clearExperimentalInstrumentStateForProjectLoad();

    // --- Audio thread only (I3e transport MIDI; `audioThread_*` entry points) ---
    struct PendingTransportNoteOff
    {
        std::int64_t dueAbsSample = 0;
        int midiNote = 0;
        int midiChannel = 1;
    };

    static constexpr int kMaxPendingTransportOffs = 256;
    std::array<PendingTransportNoteOff, kMaxPendingTransportOffs> rtPendingOffs_{};
    int rtPendingOffCount_ = 0;
    std::int64_t rtLastSegEndTimeline_ = -1;
    std::uint32_t rtLastSnapshotRevision_ = 0;
};
