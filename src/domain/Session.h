#pragma once

// =============================================================================
// Session — message-thread owner of the *current* immutable session snapshot; no transport, no UI
// =============================================================================
//
// ROLE IN THE ARCHITECTURE
//   `Session` is the **sole publisher** of the timeline answer for playback: an atomic pointer to
//   a `const SessionSnapshot` that replaces the earlier “one `const AudioClip`” field. The
//   **audio thread** does not “open the session for editing” — it only **acquire**‑loads the
//   shared_ptr and reads placements + PCM. The **message thread** (file chooser, future editors)
//   **release**‑stores a **new** snapshot after decode succeeds, or points at the **shared empty**
//   snapshot on clear, using the same lock-free `shared_ptr` idiom as Phase 1 with a larger
//   immutable value type. That is how the steering “immutable atomic snapshot” handoff is
//   realized in code: no mutex on the hot path, no half-published session graph.
//
// RELATION TO `PlacedClip` / `SessionSnapshot` / `AudioClip`
//   `AudioFileLoader` still produces `AudioClip` only. `Session` assembles *snapshots* that wrap
//   that material in `PlacedClip` rows (start time on the session timeline) inside a
//   `SessionSnapshot`. Separation keeps decode concerns out of the snapshot type and leaves
//   placement policy in one place (`Session` / future session commands) rather than inside PCM.
//
// THREAD MODEL
//   • addClipFromFileAtPlayhead / clearClip: [Message thread]; decode may block on load.
//   • loadSessionSnapshotForAudioThread: [Audio thread] or [Message thread] — acquire-load;
//     refcount only on the hot path; no decode.
//   • getCurrentClip: [Message thread] **bridge** API — front clip’s `AudioClip` only; the
//     timeline **view** uses `loadSessionSnapshotForAudioThread` + `getPlacedClips` (Step 7).
//
// OWNERSHIP
//   `Session` owns `std::atomic<std::shared_ptr<const SessionSnapshot>>` only. It does not own
//   `Transport` or the audio device. Clip PCM lifetime is through `shared_ptr` inside the snapshot.
//
// In-body: `Session.cpp` explains failure vs success at the atomic store and what “empty” means.
// See also: `SessionSnapshot`, `PlacedClip`, `AudioFileLoader`, `PlaybackEngine`, `status/DECISION_LOG.md`.
// =============================================================================

#include "domain/PlacedClip.h"
#include "domain/SessionSnapshot.h"
#include "domain/Track.h"
#include "domain/AudioMixdownProjectSettings.h"
#include "io/ProjectFile.h"
#include "plugins/PluginTrackSlot.h"
#include "ui/SnapSettings.h"

#include <juce_core/juce_core.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

class AudioClip;
class Transport;
class PluginInsertHost;
class InstrumentTrackController;

/// When default-uninitialized / empty callable, skips `experimentalInstrumentTracks`.
using ExperimentalInstrumentCtlLookupFn = std::function<InstrumentTrackController*(TrackId)>;

// ---------------------------------------------------------------------------
// Session — sole publisher of `std::shared_ptr<const SessionSnapshot>` to readers (engine + UI)
// ---------------------------------------------------------------------------
// Responsibility: after decode (or on clear), **release**-store a new immutable snapshot; readers
// **acquire**-load. Ordering rules are documented in `Session.cpp` at each `atomic_store` / load.
// Does not own `Transport` or the audio device. Clip **ordering** in the snapshot (newest at 0) is
// defined when building snapshots, not in this class’s public API text — see
// `SessionSnapshot::withClipAddedAsNewest`.
// ---------------------------------------------------------------------------
class Session
{
public:
    Session();
    ~Session();

    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    Session(Session&&) = delete;
    Session& operator=(Session&&) = delete;

    // [Message thread] Decode file → on success, append to *current* session by prepending a new
    // `PlacedClip` at `startSampleOnTimeline` (Phase 2: **newest** = index 0). On failure, **do not**
    // replace the pointer — the last known-good snapshot remains.
    juce::Result addClipFromFileAtPlayhead(const juce::File& file,
                                         double deviceSampleRate,
                                         std::int64_t startSampleOnTimeline);

    // [Message thread] Decode `file` and prepend a clip on `targetTrackId` (not `activeTrackId`).
    // L = 0, V = `intendedVisibleLengthSamples` (clamped in `PlacedClip` to material). Publishes
    // a new snapshot on success.
    juce::Result addRecordedTakeAtSample(const juce::File& file,
                                      double deviceSampleRate,
                                      std::int64_t startSampleOnTimeline,
                                      TrackId targetTrackId,
                                      std::int64_t intendedVisibleLengthSamples);

    // [Message thread] Attach a placement using existing decoded material (non-destructive trims).
    // Used when cycle-recording commits multiple passes into one WAV; callers share one AudioClip.
    [[nodiscard]] juce::Result addPlacedClipFromExistingMaterial(std::shared_ptr<const AudioClip> material,
                                                                  std::int64_t startSampleOnTimeline,
                                                                  std::int64_t leftTrimSamples,
                                                                  std::int64_t visibleLengthSamples,
                                                                  TrackId targetTrackId);

    [[nodiscard]] juce::Result addPlacedClipFromExistingMaterial(std::shared_ptr<const AudioClip> material,
                                                                  std::int64_t startSampleOnTimeline,
                                                                  std::int64_t leftTrimSamples,
                                                                  std::int64_t visibleLengthSamples,
                                                                  TrackId targetTrackId,
                                                                  std::int64_t materialWindowStartSamples,
                                                                  std::int64_t materialWindowEndExclusiveSamples);

    // [Message thread] Append a new **empty** track and make it the active track for
    // `addClipFromFileAtPlayhead` (newest = front within that track when a clip is added).
    void addTrack() noexcept;

    /// [Message thread] Append an empty `TrackKind::Group` bus row (routes to Master by default).
    void addGroupTrack() noexcept;

    /// [Message thread] Duplicate Track (single-row command, see `SessionSnapshot::withTrackDuplicated`):
    /// the copy is inserted directly below `sourceTrackId`, gets the next track id and fresh ids for
    /// every copied clip (material shared), inherits every persisted row property incl. the SAME
    /// audio output / sends / MIDI To destinations, and becomes the active track. Empty name →
    /// `uniqueDuplicateTrackName(source name)`. Returns the new id, or nullopt (unknown source,
    /// Master, or a snapshot build failure — nothing changed, no ids consumed).
    [[nodiscard]] std::optional<TrackId> duplicateTrack(TrackId sourceTrackId, juce::String newTrackName = {}) noexcept;
    /// "<name> — kopia", then "<name> — kopia 2", … — first name not used by any current row.
    [[nodiscard]] juce::String uniqueDuplicateTrackName(const juce::String& sourceName) const;

    /// [Message thread] Append an empty `TrackKind::Midi` lane (MIDI destination = None, no audio
    /// routing). Returns the new stable `TrackId` when published, else `nullopt`.
    [[nodiscard]] std::optional<TrackId> addMidiTrack() noexcept;

    /// [Message thread] Set main output routing (`destTrackId` must be a legal Group or Master target).
    /// Returns false when the route is illegal or repair rejected the target (no session change).
    [[nodiscard]] bool setTrackRoutedOutput(TrackId trackId, TrackId destTrackId) noexcept;

    [[nodiscard]] bool insertTrackSend(TrackId trackId,
                                       int uiSlotIndex,
                                       TrackId destTrackId,
                                       float amountLinear) noexcept;
    [[nodiscard]] bool removeTrackSend(TrackId trackId, int uiSlotIndex) noexcept;
    [[nodiscard]] bool setTrackSendDestination(TrackId trackId,
                                               int uiSlotIndex,
                                               TrackId destTrackId) noexcept;
    [[nodiscard]] bool setTrackSendAmount(TrackId trackId, int uiSlotIndex, float amountLinear) noexcept;
    [[nodiscard]] bool setTrackSendEnabled(TrackId trackId, int uiSlotIndex, bool enabled) noexcept;

    // [Message thread] `activeTrackId_` is the lane that receives the next "Add clip" (see
    // `addClipFromFileAtPlayhead`); it becomes the new id after `addTrack()`. **Does not** publish
    // a new snapshot — UI-only / command targeting; keep separate from `SessionSnapshot`.
    [[nodiscard]] TrackId getActiveTrackId() const noexcept;
    // [Message thread] Make `id` the add-clip target if it exists in the current snapshot. No-op if
    // unknown. **No** `sessionSnapshot_` republish; audio thread is unaffected.
    void setActiveTrack(TrackId id) noexcept;

    // [Message thread] How many `Track` rows exist in the current snapshot (UI lane count).
    [[nodiscard]] int getNumTracks() const noexcept;
    // [Message thread] The id of the i-th `Track` in snapshot order, or `kInvalidTrackId` if out
    // of range.
    [[nodiscard]] TrackId getTrackIdAtIndex(int index) const noexcept;

    [[nodiscard]] TrackKind getTrackKindAtIndex(int index) const noexcept;

    /// Canonical `TrackKind::Master` row (last in snapshot order), or `kInvalidTrackId`.
    [[nodiscard]] TrackId findCanonicalMasterTrackId() const noexcept;

    /// Append an empty `TrackKind::Instrument` lane at the end of **`SessionSnapshot::tracks_`**
    /// (timeline order preserved on save/load and undo/redo).
    /// **Multiple instrument tracks** may exist — this does **not** refuse when another `Instrument`
    /// row is already present. Returns the new stable `TrackId` when published, else `nullopt`.
    /// When `trackDisplayName` is empty, the name defaults to **`"Track " + newId`**, matching `addTrack()`.
    [[nodiscard]] std::optional<TrackId> appendExperimentalInstrumentShellTrack(juce::String trackDisplayName) noexcept;

    // [Message thread] Move one placed clip in **timeline sample** space. Ordering (promote to 0
    // if isolated) is **only** in `SessionSnapshot::withClipMoved` **within the clip’s own track** —
    // the UI does not implement policy. Invalid or unknown id: no publish (see factory jasserts).
    void moveClip(PlacedClipId id, std::int64_t newStartSampleOnTimeline) noexcept;

    // [Message thread] Move a clip to a **different** `TrackId` as front-most in that track (see
    // `SessionSnapshot::withClipMovedToTrack`). **Does not** change `activeTrackId_` (Add clip
    // target). Same-track moves must use `moveClip` only — this path is a no-op if the clip
    // already lives on `targetTrackId` (defensive, see .cpp).
    void moveClipToTrack(
        PlacedClipId id, std::int64_t newStartSampleOnTimeline, TrackId targetTrackId) noexcept;

    // [Message thread] Non-destructive right-edge **trim** (shorter or longer `visible` window in
    // [0, material) on the `PlacedClip` only; PCM unchanged). One snapshot publish; no lane reorder.
    void setClipRightEdgeVisibleLength(PlacedClipId id, std::int64_t newVisibleLengthSamples) noexcept;

    // [Message thread] Non-destructive left-edge trim (material offset L); one snapshot publish.
    void setClipLeftEdgeTrim(PlacedClipId id, std::int64_t newLeftTrimSamples) noexcept;

    // [Message thread] Split one placement into two at `splitSampleOnTimeline` (strictly inside the
    // clip's audible span). Returns the new ids `{left, right}` on success; no publish on failure.
    [[nodiscard]] std::optional<std::pair<PlacedClipId, PlacedClipId>> splitClip(
        PlacedClipId id,
        std::int64_t splitSampleOnTimeline) noexcept;

    // [Message thread] Reorder the **track list** only. Each track’s clips and name are unchanged.
    // **Does not** change `activeTrackId_` (add-clip target follows the same id in the new row).
    void moveTrack(TrackId movedTrackId, int destIndex) noexcept;

    // [Message thread] Removes one track row and every `PlacedClip` on it. No disk / file changes.
    // Unknown id: no-op. When `removedTrackId` was `activeTrackId_`, the active lane becomes the
    // clip at the former index slot (fallback: neighbor above).
    void removeTrack(TrackId removedTrackId) noexcept;

    // [Message thread] Removes one `PlacedClip` row from `trackId` only. PCM / `AudioClip` and disk
    // are unchanged. Unknown track or placement id: no-op.
    void removePlacedClip(TrackId trackId, PlacedClipId placedClipId) noexcept;

    // [Message thread] Undo/redo only: atomically replace the published snapshot with `restored`
    // (same release-store idiom as other mutators). Does **not** change `nextPlacedClipId_` /
    // `nextTrackId_` (monotonic ids must never decrease). If `activeTrackId_` is absent from
    // `restored`, clamps to the first track or `kInvalidTrackId`. No-op if `restored == nullptr`.
    void restoreSessionSnapshotForUndo(std::shared_ptr<const SessionSnapshot> restored) noexcept;

    // [Message thread] Mixer channel volume: linear gain at the channel-fader point (see `Track`).
    // Clamped to [0, kTrackChannelFaderGainMax]; 0 = fader at −∞ (not the same as mute flag).
    void setTrackChannelFaderGain(TrackId trackId, float linearGain) noexcept;

    /// [Message thread] Stereo pan [-1,+1] (full left … full right); applied after fader in playback.
    void setTrackStereoPan(TrackId trackId, float stereoPan) noexcept;

    /// [Message thread] Pre-gain in dB, sanitized to [-24,+24]: applied BEFORE Pre inserts and
    /// fader on Audio lanes (see `Track`). Returns false (nothing published) when the row is
    /// unknown or the sanitized value already matches — callers wrapping this in an undoable
    /// edit then skip the empty undo entry.
    [[nodiscard]] bool setTrackPreGainDb(TrackId trackId, float preGainDb) noexcept;

    /// [Message thread] Set an Audio track's input assignment (recording + monitoring source).
    /// Sanitized; returns false (no snapshot publish) when the track is missing, not an Audio
    /// lane, or the value is unchanged — callers use this for undo no-op suppression.
    [[nodiscard]] bool setTrackInputAssignment(TrackId trackId,
                                               TrackInputAssignment assignment) noexcept;

    /// [Message thread] Set an Instrument / Midi row's live MIDI input (device + channel filter).
    /// Sanitized; returns false (no snapshot publish) when the track is missing, cannot take live
    /// MIDI, or the value is unchanged — callers use this for undo no-op suppression.
    [[nodiscard]] bool setTrackMidiInputAssignment(TrackId trackId,
                                                   TrackMidiInputAssignment assignment) noexcept;

    // [Message thread] Lane off: skipped entirely by `PlaybackEngine` (distinct from mute).
    void setTrackOff(TrackId trackId, bool trackOff) noexcept;
    // [Message thread] Mute: engine effective gain zero; stored fader untouched.
    // SOLO LOCK: while Solo is active (`isSoloActive()`), this command REFUSES every change —
    // the common command path is locked, not just the buttons (spec §2). Project load and undo
    // restore stored mute flags through snapshot replacement (`applyLoadedProjectModel` /
    // `restoreSessionSnapshotForUndo`), which never pass through here and are never blocked.
    void setTrackMuted(TrackId trackId, bool muted) noexcept;

    // ------------------------------------------------------------------ Solo
    // Solo is a temporary LISTENING layer on top of the stored Mute flags: five separate sets of
    // explicitly soloed TrackIds — ONE temporary set plus FOUR persistent memories (1 … 4).
    // Exactly one set is "current" at a time: no memory selected (index -1) → the temporary set;
    // memory 0 … 3 selected → that memory. S buttons always show and edit the current set.
    // All of this state is message-thread-owned and NOT part of `SessionSnapshot` (like
    // `activeTrackId_`): the engine consumes a separately published derived view, stored Mute
    // flags are never rewritten, and whole-session undo snapshots never carry solo state.
    // Persistence: ONLY the four memories are saved (project v25); the temporary set, the active
    // selection, and all derived states are reset on load (`resetTransientSoloStateForProjectLoad`).
    static constexpr int kSoloMemoryCount = 4;

    /// Active memory index 0 … 3, or -1 when the temporary set is current.
    [[nodiscard]] int getActiveSoloMemoryIndex() const noexcept { return activeSoloMemoryIndex_; }
    /// [Message thread] Select a memory (0 … 3) or -1 for the temporary set. Switching NEVER
    /// copies content between sets and never touches the stored sets. Out-of-range → no-op.
    void setActiveSoloMemoryIndex(int indexOrMinusOne) noexcept;

    /// Raw stored content of the current set (may contain ids of deleted tracks; see
    /// `getEffectiveSoloedTrackIds` for the validity-filtered view).
    [[nodiscard]] std::vector<TrackId> getCurrentSoloSetTrackIds() const;
    [[nodiscard]] std::vector<TrackId> getSoloMemoryTrackIds(int memoryIndex) const;
    /// [Message thread] Replace one memory's content (narrow undo + project load). Deduplicates;
    /// invalid ids (0) dropped. Out-of-range index → no-op. Does NOT change the active selection.
    void setSoloMemoryTrackIds(int memoryIndex, std::vector<TrackId> ids) noexcept;

    /// True when `trackId` is explicitly in the CURRENT set (raw membership; red S face).
    [[nodiscard]] bool isTrackInCurrentSoloSet(TrackId trackId) const noexcept;
    /// [Message thread] Toggle explicit membership of `trackId` in the current set. Refuses ids
    /// not in the current snapshot and the Master row (no S on Stereo Out). Returns true when the
    /// set changed. When a memory is current this edits the MEMORY directly (callers wrap it in
    /// the narrow solo-memory undo command); edits of the temporary set are not undoable.
    [[nodiscard]] bool toggleTrackInCurrentSoloSet(TrackId trackId) noexcept;

    /// Current set filtered to tracks that exist in the current snapshot (deduplicated). Stale
    /// ids of deleted tracks are invisible here — no ghost solo — but stay stored so undoing a
    /// track deletion restores membership without any extra bookkeeping.
    [[nodiscard]] std::vector<TrackId> getEffectiveSoloedTrackIds() const;
    /// Solo is ACTIVE only when the current set contains ≥1 existing track. A selected but empty
    /// (or fully stale) memory does not restrict playback and does not lock Mute.
    [[nodiscard]] bool isSoloActive() const noexcept;
    /// Normal Mute changes are locked exactly while Solo is active (spec §2).
    [[nodiscard]] bool isMuteChangeLockedBySolo() const noexcept { return isSoloActive(); }

    /// [Message thread] Project load/new: temporary set cleared, no memory selected. The four
    /// memories are NOT touched here — the load path assigns them from the parsed file.
    void resetTransientSoloStateForProjectLoad() noexcept;

    /// [Message thread] MIDI output channel for this row's own timeline MIDI:
    /// `kTrackMidiOutputChannelAny` preserves each event's stored channel, 1 … 16 remaps every
    /// event to that channel. Unrelated to `setTrackRoutedOutput` (audio bus). Returns false when
    /// the row is unknown or already on that channel, in which case nothing is published.
    [[nodiscard]] bool setTrackMidiOutputChannel(TrackId trackId, int midiOutputChannel) noexcept;

    /// [Message thread] **MIDI To** for a `Midi` row: `kInvalidTrackId` = None, or the id of an
    /// existing `Instrument` row. Returns false (nothing published) when the row is not a Midi
    /// track, the destination is illegal, or the destination is unchanged.
    [[nodiscard]] bool setTrackMidiDestination(TrackId trackId, TrackId destinationTrackId) noexcept;

    // [Message thread] Rename one lane (`Track::getName()`). Trim-only no-op / empty after trim / unknown id: no publish.
    void setTrackName(TrackId trackId, juce::String newName) noexcept;

    // [Message thread] Set one placed clip's display name (project metadata only; the source audio
    // file on disk is never renamed). Empty after trim / unchanged / unknown id: no publish.
    void setPlacedClipName(PlacedClipId clipId, juce::String newDisplayName) noexcept;

    // [Message thread] Publish the *shared* empty `SessionSnapshot` (see
    // `SessionSnapshot::createEmpty`) — no clips, nothing to play or paint as waveform material.
    void clearClip() noexcept;

    // [Message thread] Front clip’s `AudioClip` (index 0); **bridge** for legacy call sites.
    // `ClipWaveformView` reads the full snapshot for multi-clip layout (Step 7).
    [[nodiscard]] const AudioClip* getCurrentClip() const noexcept;

    // [Message thread] **Content end** — max of (start+length) over placed clips (derived). Zero
    // with no material. For backward compat, `getTimelineLengthSamples()` is an alias; prefer
    // `getContentEndSamples()`.
    [[nodiscard]] std::int64_t getContentEndSamples() const noexcept;
    // Deprecated: use `getContentEndSamples()`.
    [[nodiscard]] std::int64_t getTimelineLengthSamples() const noexcept
    {
        return getContentEndSamples();
    }

    // [Message thread] **Arrangement** extent (playable / navigable): max of stored snapshot
    // `arrangementExtentSamples` and `getContentEndSamples()`. Also used by the audio engine run-end.
    [[nodiscard]] std::int64_t getArrangementExtentSamples() const noexcept;
    // Raw stored floor on the snapshot (for default seeding: do not clobber a loaded v3 value).
    [[nodiscard]] std::int64_t getStoredArrangementExtentSamples() const noexcept;
    // [Message thread] Grow-only stored arrangement extent; publishes a new snapshot. No-op if
    // `v` is not greater than the current stored value.
    void setArrangementExtentSamples(std::int64_t v) noexcept;
    // [Message thread] End of a recording run: the stored extent becomes
    // `max(storedExtentBeforeRun, audio content end, recordedResultEndSamples)` — the display
    // headroom the run added is dropped, an older project's saved extent is never shrunk, and the
    // recorded result (audio clips and the MIDI take end, which lives outside the snapshot) keeps
    // the room it needs. Publishes a new snapshot only when the stored value changes.
    void restoreArrangementExtentAfterRecording(std::int64_t storedExtentBeforeRun,
                                                std::int64_t recordedResultEndSamples) noexcept;

    // [Message thread] Timeline locator samples (Cubase-style markers). Clamped to
    // `getArrangementExtentSamples()`; `right == 0` means right locator unset. No swap/normalize
    // between L/R; playback does **not** use locators yet.
    void setLeftLocatorAtSample(std::int64_t s) noexcept;
    void setRightLocatorAtSample(std::int64_t s) noexcept;
    [[nodiscard]] std::int64_t getLeftLocatorSamples() const noexcept;
    [[nodiscard]] std::int64_t getRightLocatorSamples() const noexcept;

    // [Message thread] TLD-1 (steering §10.1): the project's **timeline reference sample rate** —
    // the rate under which every persisted sample-domain timeline field (clip placements, MIDI
    // clip anchors/windows, locators, extent, playhead) is interpreted. Initialized **once** (from
    // the loaded project's normalized v20 field, or from the device rate on a fresh session) and
    // never silently re-stamped by device/engine-rate changes; an explicit future change must
    // rescale all sample-domain fields atomically in the same operation. 0 = not yet initialized.
    [[nodiscard]] double getTimelineSampleRate() const noexcept { return timelineSampleRate_; }
    /// [Message thread] Reference rate when initialized, else `fallback` (typically the device rate).
    [[nodiscard]] double timelineSampleRateOr(double fallback) const noexcept;
    /// [Message thread] Adopts `rate` only while uninitialized (fresh session before first
    /// load/save). Ignores invalid rates. Never overwrites an existing reference.
    void initializeTimelineSampleRateIfUnset(double rate) noexcept;

    /// [Message thread] Global tempo/meter metadata on the snapshot (independent of MIDI clip `pattern.bpm`).
    [[nodiscard]] ProjectMusicalTime getProjectMusicalTime() const noexcept;
    /// [Message thread] Updates BPM only; publishes `SessionSnapshot::withMusicalTime`.
    void setProjectBpm(double bpm) noexcept;
    /// [Message thread] Replace full musical metadata (sanitized on snapshot).
    void setProjectMusicalTime(ProjectMusicalTime musicalTime) noexcept;

    /// [Message thread] Global arrangement/MIDI snap — mirrored from the main transport strip (not part of `SessionSnapshot`).
    [[nodiscard]] SnapSettings getArrangementSnapSettings() const noexcept { return arrangementSnapSettings_; }
    void setArrangementSnapSettings(SnapSettings settings) noexcept { arrangementSnapSettings_ = settings; }

    // [Audio thread] and [Message thread] Acquire the current `SessionSnapshot` pointer; no
    // decode, no session mutation. This is the main handoff the engine uses each block.
    [[nodiscard]] std::shared_ptr<const SessionSnapshot> loadSessionSnapshotForAudioThread() const noexcept;

    // [Message thread] Last successfully **saved** or **loaded** project file (empty if never set).
    // Used by the app for project-relative paths (e.g. `Audio/`). Not part of the snapshot.
    [[nodiscard]] juce::File getCurrentProjectFile() const noexcept { return currentProjectFile_; }
    // [Message thread] Stability Slice 5: `saveProjectToFile` records the written file as current;
    // an autosave must not hijack the user's save target, so the caller restores it with this.
    // Also used to detach a recovered autosave so plain Save goes through Save As (empty file).
    void setCurrentProjectFile(const juce::File& f) noexcept { currentProjectFile_ = f; }
    // [Message thread] Parent directory of `getCurrentProjectFile()`; empty if no project file.
    [[nodiscard]] juce::File getCurrentProjectFolder() const noexcept;
    // [Message thread] True if the user has a known on-disk project (save or load completed).
    [[nodiscard]] bool hasKnownProjectFile() const noexcept;

    // [Message thread] Write minimal project v1 (tracks, clip placements, strict **`Audio/`-relative**
    // source paths only**, monotonic id seeds, active track, playhead and device rate metadata).
    // `transport` is read for the playhead only (single owner of playhead state).
    // Optional `pluginHost`: when non-null, **v8** saves per-track VST3 path, identifier, and Base64 state.
    [[nodiscard]] juce::Result saveProjectToFile(
        Transport& transport,
        const juce::File& file,
        double deviceSampleRate,
        PluginInsertHost* pluginHost = nullptr,
        ExperimentalInstrumentCtlLookupFn instrumentCtlByTrackId = {},
        bool arrangementSnapEnabled = false,
        juce::String arrangementSnapResolutionKey = "1_4",
        std::optional<ProjectFileMainWindowBoundsV1> mainWindowBoundsForSave = std::nullopt,
        std::optional<ProjectFileMainWindowBoundsV1> midiEditorWindowBoundsForSave = std::nullopt,
        std::optional<ProjectFileMidiEditorWorkspaceV1> midiEditorWorkspaceForSave = std::nullopt);

    // Optional `pluginHost`: clears all plugin instances first, then after a successful timeline load
    // restores inserts from **v8** track fields (missing files append `[plugin]` lines to `outSkippedClipDetails`).
    [[nodiscard]] juce::Result loadProjectFromFile(
        Transport& transport,
        const juce::File& file,
        double deviceSampleRate,
        juce::StringArray& outSkippedClipDetails,
        juce::String& outInfoNote,
        PluginInsertHost* pluginHost = nullptr);

    [[nodiscard]] AudioMixdownProjectSettings getAudioMixdownSettings() const noexcept;
    void setAudioMixdownSettings(AudioMixdownProjectSettings settings) noexcept;

    /// [Message thread] Shared ruler labeling: main timeline + MIDI clip editor stay in sync.
    enum class TimelineRulerTimeDisplay : std::uint8_t
    {
        MusicalBarsBeats = 0,
        TimeSeconds = 1,
    };

    /// Shared `juce::ComboBox` item ids (arrangement toolbar + MIDI editor).
    static constexpr int kTimelineRulerFormatComboIdBarsBeats = 1;
    static constexpr int kTimelineRulerFormatComboIdSeconds = 2;

    [[nodiscard]] TimelineRulerTimeDisplay getTimelineRulerTimeDisplay() const noexcept
    {
        return timelineRulerTimeDisplay_;
    }
    void setTimelineRulerTimeDisplay(TimelineRulerTimeDisplay d) noexcept;

    /// [Message thread] Called after the display mode changes (e.g. repaint main + MIDI rulers).
    void setOnTimelineRulerTimeDisplayChanged(std::function<void()> callback) noexcept;

    /// One row's insert chain the loader restores AFTER the timeline (same payload the deferred
    /// `callAsync` path imports; see `applyLoadedProjectModel`).
    struct PendingPluginInsertRestore
    {
        TrackId trackId = kInvalidTrackId;
        PluginTrackChain chain;
    };
    /// Pre-decoded clip material keyed by the ABSOLUTE source file path (`resolveProjectAudioFile`).
    /// Built off the message thread by the staged loader so `applyLoadedProjectModel` decodes nothing
    /// for files found here (a file is decoded once even when many clips share it).
    using PreDecodedMaterialByPath = std::unordered_map<juce::String, std::shared_ptr<const AudioClip>>;
    /// [Any thread] The on-disk file a project `sourcePath` refers to (empty File when the stored path
    /// is not a legal `Audio/...` relative path). Pure.
    [[nodiscard]] static juce::File resolveProjectAudioFile(const juce::String& storedSourcePath,
                                                            const juce::File& projectFolder) noexcept;

    /// [Message thread] Same effects as loading `file`, but uses an already-parsed model (caller read
    /// `file` beforehand). Caller restores `experimentalInstrumentTracks` rows after timeline + inserts.
    /// `preDecodedMaterial` (optional): clips whose resolved file is in the map reuse that material
    /// instead of decoding. `outDeferredInsertRestores` (optional): when non-null the insert chains are
    /// handed back here for the caller to import step by step (progress, UI responsiveness) and the
    /// internal deferred `callAsync` import is NOT scheduled; null keeps the previous behaviour.
    [[nodiscard]] juce::Result applyLoadedProjectModel(
        Transport& transport,
        const juce::File& loadedFromDisk,
        const ProjectFileV1& parsed,
        double deviceSampleRate,
        juce::StringArray& outSkippedClipDetails,
        juce::String& outInfoNote,
        PluginInsertHost* pluginHost = nullptr,
        std::uint64_t loadGenerationForDeferredRestore = 0,
        const PreDecodedMaterialByPath* preDecodedMaterial = nullptr,
        std::vector<PendingPluginInsertRestore>* outDeferredInsertRestores = nullptr);

    /// [Message thread] Increments and returns the current project-load generation (used to stale-guard
    /// deferred restore callbacks when a newer load begins).
    [[nodiscard]] std::uint64_t beginProjectLoadGeneration() noexcept;

    /// [Any thread] Current project-load generation (acquire-load).
    [[nodiscard]] std::uint64_t getProjectLoadGeneration() const noexcept;

private:
    // [Message thread] TLD-1 timeline reference rate (see getTimelineSampleRate above).
    double timelineSampleRate_ = 0.0;
    // [Message thread only] Monotonic ids for new `PlacedClip` rows (add path). Not reset on clear
    // so a long edit session does not reuse ids while UI might still hold an old `PlacedClipId`.
    PlacedClipId nextPlacedClipId_ = 1;
    // [Message thread] Monotonic `TrackId` for the **next** `addTrack` (default session already has
    // track 1).
    TrackId nextTrackId_ = 2;
    // [Message thread] `addClipFromFileAtPlayhead` places on this lane (default 1; `addTrack` updates).
    TrackId activeTrackId_ = 1;

    // Current world picture for the audio thread: always either the shared empty snapshot or a
    // user-built snapshot; swapped only from the message thread, read with acquire from any thread.
    mutable std::atomic<std::shared_ptr<const SessionSnapshot>> sessionSnapshot_;

    // [Message thread] Solo listening layer (see the Solo section above): one temporary set +
    // four persistent memories of explicitly soloed TrackIds, and which set is current (-1 =
    // temporary). Kept outside `SessionSnapshot` on purpose; stored Mute flags are never rewritten.
    std::vector<TrackId> temporarySoloSet_;
    std::array<std::vector<TrackId>, kSoloMemoryCount> soloMemories_;
    int activeSoloMemoryIndex_ = -1;

    juce::File currentProjectFile_;
    AudioMixdownProjectSettings audioMixdown_;

    TimelineRulerTimeDisplay timelineRulerTimeDisplay_{TimelineRulerTimeDisplay::MusicalBarsBeats};
    std::function<void()> onTimelineRulerTimeDisplayChanged_;

    SnapSettings arrangementSnapSettings_;

    std::atomic<std::uint64_t> loadGeneration_{ 0 };
};
