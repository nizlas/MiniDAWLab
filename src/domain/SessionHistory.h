#pragma once

// =============================================================================
// SessionHistory — message-thread undo/redo stack of SessionSnapshot pairs (Undo-1)
// =============================================================================
//
// ROLE
//   Stores discrete edit steps as (`before`, `after`) snapshot pointers. Undo restores `before`;
//   redo restores `after`. Never touches disk, Transport, or the audio thread — callers invoke
//   `Session::restoreSessionSnapshotForUndo` on the message thread only.
//
// PHASE 8 / Slice A
//   A step may optionally include a **plugin insert chain** before/after for one `TrackId`.
//   Plugin-only edits use the **same** snapshot pointer for `before` and `after` with a non-empty
//   plugin delta (`pluginSides.has_value()`). Apply order on undo: restore timeline snapshot, then
//   `PluginInsertHost::importChain(trackId, pluginSides->before)`.
//
// RECORD
//   `record` clears the redo deque. Steps are dropped from the front when over capacity.
//
// NO-OP / CHANGE DETECTION
//   Without a plugin delta: ignores if `before.get() == after.get()`. With a plugin delta: records
//   when `before` and `after` slot snapshots differ; timeline pointers may match.
// =============================================================================

#include "domain/SessionSnapshot.h"
#include "domain/TrackColour.h"
#include "domain/VisualTrackGroup.h"
#include "io/ProjectFile.h"
#include "plugins/PluginTrackSlot.h"

#include <juce_core/juce_core.h>

#include <deque>
#include <memory>
#include <optional>

struct InstrumentUndoStepSides
{
    std::vector<ProjectFileExperimentalInstrumentTrackV1> before;
    std::vector<ProjectFileExperimentalInstrumentTrackV1> after;
};

/// Delete-Track undo payload for an *instrument* track: the full project row (MIDI clips, plugin
/// identity + state Base64, drum labels) captured **before** runtime teardown. Undo restores the
/// session row from the timeline snapshot, then recreates the runtime from `row` via the same
/// restore path project load uses; redo re-runs the hardened runtime teardown for `trackId`.
struct InstrumentTrackDeleteUndoSides
{
    TrackId trackId = kInvalidTrackId;
    ProjectFileExperimentalInstrumentTrackV1 row;
};

/// Narrow Solo-memory undo payload (Solo spec §7): the content change of ONE persistent Solo
/// memory. Deliberately decoupled from the timeline — a solo-memory step records the SAME
/// snapshot pointer for before/after, so undoing a memory edit restores exactly that memory's
/// TrackId set and can never erase a recording take or any other session change. Undo always
/// targets `memoryIndex` directly (never "the currently active memory"), so it stays correct
/// even after the user switched the active memory. Temporary-set edits and memory SELECTION
/// changes are never recorded (not undoable, not dirty).
struct SoloMemoryUndoSides
{
    int memoryIndex = -1; ///< 0 … `Session::kSoloMemoryCount`-1.
    std::vector<TrackId> before;
    std::vector<TrackId> after;
};

/// Narrow visual-track-group undo payload (groups spec §7): the FULL group list before/after one
/// bounded metadata edit (Create / Rename / Ungroup). Like `SoloMemoryUndoSides` this is
/// deliberately decoupled from the timeline — the step records the SAME snapshot pointer for
/// before/after, so undoing a group edit restores exactly the group metadata and can never erase
/// a recording take or any other session change. Collapse/expand is a display change and is NEVER
/// recorded here (dirty only, no undo entry).
struct VisualTrackGroupsUndoSides
{
    std::vector<VisualTrackGroup> before;
    std::vector<VisualTrackGroup> after;
};

/// Narrow track-colour undo payload: ONE track's palette key before/after. Same discipline as the
/// Solo-memory and group steps — identical snapshot pointer on both sides, so undoing a colour
/// change restores exactly that colour and never an instrument, a take or the whole Session.
struct TrackColourUndoSides
{
    TrackId trackId = kInvalidTrackId;
    TrackColourKey before = TrackColourKey::DefaultGrey;
    TrackColourKey after = TrackColourKey::DefaultGrey;
};

struct SessionHistoryRestoreBundle
{
    /// Always non-null when `popUndo` / `popRedo` returns has_value.
    std::shared_ptr<const SessionSnapshot> timelineSnapshot;
    /// When set, restore this plugin slot **after** applying `timelineSnapshot`.
    std::optional<PluginUndoStepSides> pluginSides {};
    /// I3i: experimental instrument musical state (no plugin blobs); apply after timeline (+ plugin).
    std::optional<InstrumentUndoStepSides> instrumentSides {};
    /// Delete Track: instrument runtime restore (undo) / teardown (redo) payload; apply last.
    std::optional<InstrumentTrackDeleteUndoSides> instrumentTrackDelete {};
    /// Solo memory content step: apply `before` (undo) / `after` (redo) to that memory only.
    std::optional<SoloMemoryUndoSides> soloMemorySides {};
    /// Visual-group metadata step: apply `before` (undo) / `after` (redo) as the full group list.
    std::optional<VisualTrackGroupsUndoSides> visualTrackGroupSides {};
    /// Track-colour step: apply `before` (undo) / `after` (redo) to that one track's colour.
    std::optional<TrackColourUndoSides> trackColourSides {};
    /// True: popped from redo stack (apply `pluginSides->after`), false: undo (apply `before`).
    bool isRedo = false;
};

class SessionHistory
{
public:
    explicit SessionHistory(int maxUndoSteps = 100) noexcept;

    void clear() noexcept;

    /// [Message thread] Pushes one undo step and clears redo. Without plugin delta: no-op if either
    /// snapshot pointer is null or both refer to the same instance. With plugin delta: requires
    /// valid `pluginSides->trackId` and allows identical snapshot pointers when slots differ.
    void record(juce::String label,
                std::shared_ptr<const SessionSnapshot> before,
                std::shared_ptr<const SessionSnapshot> after,
                std::optional<PluginUndoStepSides> pluginSides = std::nullopt,
                std::optional<InstrumentUndoStepSides> instrumentSides = std::nullopt,
                std::optional<InstrumentTrackDeleteUndoSides> instrumentTrackDelete
                = std::nullopt,
                std::optional<SoloMemoryUndoSides> soloMemorySides = std::nullopt,
                std::optional<VisualTrackGroupsUndoSides> visualTrackGroupSides
                = std::nullopt,
                std::optional<TrackColourUndoSides> trackColourSides = std::nullopt) noexcept;

    /// [Message thread] Pops one undo step onto redo; returns bundle with timeline + optional plugin
    /// restore (`pluginSides` present — caller applies `before` chain).
    [[nodiscard]] std::optional<SessionHistoryRestoreBundle> popUndo() noexcept;

    /// [Message thread] Pops one redo step back onto undo; returns bundle (`pluginSides->after` if
    /// present).
    [[nodiscard]] std::optional<SessionHistoryRestoreBundle> popRedo() noexcept;

    [[nodiscard]] int undoStackSize() const noexcept { return static_cast<int>(undo_.size()); }
    [[nodiscard]] int redoStackSize() const noexcept { return static_cast<int>(redo_.size()); }

private:
    struct Step
    {
        juce::String label;
        std::shared_ptr<const SessionSnapshot> before;
        std::shared_ptr<const SessionSnapshot> after;
        std::optional<PluginUndoStepSides> pluginSides;
        std::optional<InstrumentUndoStepSides> instrumentSides;
        std::optional<InstrumentTrackDeleteUndoSides> instrumentTrackDelete;
        std::optional<SoloMemoryUndoSides> soloMemorySides;
        std::optional<VisualTrackGroupsUndoSides> visualTrackGroupSides;
        std::optional<TrackColourUndoSides> trackColourSides;
    };

    int maxSteps_;
    std::deque<Step> undo_;
    std::deque<Step> redo_;
};
