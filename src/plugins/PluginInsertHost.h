#pragma once

// =============================================================================
// PluginInsertHost — message-thread owner of per-track VST3 insert chains (Slice A)
// =============================================================================
//
// ROLE
//   Loads ordered `juce::AudioPluginInstance` rows per `TrackId` (Pre before Post), prepares them
//   for the current device rate / block size, publishes an atomic read-only view for
//   `PlaybackEngine`, and owns pre-allocated **scratch** buffers the audio thread writes clip sums
//   into before `processBlock`.
//
// NOT IN `SessionSnapshot`
//   Live plugins are mutable and may show UI — they never appear inside immutable timeline snapshots.
//
// THREADING
//   Construct / load / remove / editors / `prepareForDevice` / `importChain`: [Message thread].
//   `audioThread_clearScratch`, `audioThread_getScratchWritePointers`,
//   `audioThread_processChainForTrack`, `audioThread_hasActivePluginForTrack`:
//   [Audio thread] — no locks, no allocation; only touches pre-sized buffers and the atomic map.
//
// See `docs/PHASE_PLAN.md` Phase 8 and `docs/ARCHITECTURE_PRINCIPLES.md` Plugin host section.
// =============================================================================

#include "plugins/InsertSlotRuntime.h"
#include "plugins/PluginEditorWindows.h"
#include "plugins/PluginTrackSlot.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

struct InsertRowView
{
    InsertSlotId slotId = kInvalidInsertSlotId;
    InsertStage stage = InsertStage::Post;
    juce::String displayName;
    /// True for a slot whose plugin could not be instantiated on project load (bundle missing,
    /// scan/instantiation failure, or identity mismatch). The slot still occupies its chain
    /// position and keeps the saved identity + state (see `PluginInsertHost::importChain`); it is
    /// silent in the audio path and has no editor until the user removes it or the plugin returns.
    bool unavailable = false;
    /// Host Active / Bypass (see `InsertSlotRuntime`). Both survive on an unavailable row; the row
    /// still does not process until the plug-in can be instantiated.
    bool hostActive = true;
    bool hostBypassed = false;
};

/// A complete, block-start transport description for hosted effect processors.
/// `PlaybackEngine` creates this from the immutable session snapshot plus the current live or
/// offline render position. It is passed only on the processing thread immediately before an
/// insert chain runs; it is not session state and it is never shared with the message thread.
struct PluginProcessTransportContext
{
    std::int64_t timelineSample = 0;
    double sampleRate = 0.0;
    double bpm = 120.0;
    int timeSignatureNumerator = 4;
    int timeSignatureDenominator = 4;
    bool isPlaying = false;
    bool isRecording = false;
    bool isLooping = false;
    std::int64_t loopStartSample = 0;
    std::int64_t loopEndSample = 0;
};

/// Non-owning `AudioProcessor::setPlayHead` target. Stage A1: ONE instance per track chain
/// (created on the message thread, co-owned by every published `PluginAudioThreadMap::Entry`
/// that references it, so it outlives every instance that may query it). The thread that runs a
/// chain updates the chain's own playhead immediately before `processBlock`; two concurrently
/// running chains therefore never share a mutable transport context.
class InsertProcessPlayHead final : public juce::AudioPlayHead
{
public:
    /// [Processing thread that owns this chain for the current job/segment] Materialize only
    /// facts DAL knows for this block. JUCE turns the engaged fields into the VST3
    /// ProcessContext validity flags.
    void setContext(const PluginProcessTransportContext& context) noexcept;

    /// [Processing thread] JUCE calls this synchronously from hosted `processBlock`. It returns
    /// a value copy, so processors cannot retain mutable host state.
    [[nodiscard]] juce::Optional<PositionInfo> getPosition() const override;

private:
    juce::AudioPlayHead::PositionInfo position_;
};

// Immutable view exchanged with the audio callback (release-store / acquire-load).
struct PluginAudioThreadMap
{
    struct SlotProc
    {
        juce::AudioProcessor* processor = nullptr;
        bool layoutOk = false;
        InsertStage stage = InsertStage::Post;
        /// Opt-in audio profiler instance index (`diagnostics/AudioThreadProfiler.h`); −1 = none.
        int profileSlot = -1;
        /// Host mix for this slot. Shared with the message-thread slot so a republished map keeps
        /// the ramp and the delay line. Null only if a slot was published without one.
        std::shared_ptr<InsertSlotRuntime> runtime;
    };

    struct Entry
    {
        TrackId trackId = kInvalidTrackId;
        /// Pre slots first, then Post — in order.
        std::vector<SlotProc> slots;
        /// This chain's transport playhead, installed on every instance of the entry and co-owned
        /// here so it outlives every published map that references it (Stage A1, see class note).
        std::shared_ptr<InsertProcessPlayHead> playHead;
    };
    std::vector<Entry> entries;
};

class PluginInsertHost
{
public:
    /// Phase 8: insert chain is always stereo (device output channel count is independent).
    static constexpr int kInsertChannels = 2;

    PluginInsertHost();
    ~PluginInsertHost();

    PluginInsertHost(const PluginInsertHost&) = delete;
    PluginInsertHost& operator=(const PluginInsertHost&) = delete;

    // -------------------------------------------------------------------------
    // [Message thread] Load / state / editors
    // -------------------------------------------------------------------------

    /// Replaces the track chain with a single new **Post** insert (legacy one-slot UX).
    [[nodiscard]] juce::Result loadVst3FromFile(TrackId trackId, const juce::File& vst3File);

    [[nodiscard]] juce::Result addInsertFromVst3File(TrackId trackId,
                                                     InsertStage stage,
                                                     const juce::File& vst3File);

    void removeInsert(TrackId trackId, InsertSlotId slotId);

    /// [Message thread] Move insert to `targetStage` at gap index in the target stage in [0, targetStageCount].
    /// Preserves instance and slot id; undo label "Move insert" if chain changes.
    void moveInsertToStageAtGap(TrackId trackId,
                                InsertSlotId slotId,
                                InsertStage targetStage,
                                int gapIndexInTargetStage);

    /// [Message thread] Move an occupied insert to the end of Pre or Post; delegates to `moveInsertToStageAtGap` (append).
    void moveInsertToStage(TrackId trackId, InsertSlotId slotId, InsertStage newStage);

    /// [Message thread] Reorder within the slot's current stage. `gapIndexInStage` is the visual gap before removal,
    /// in [0, stageCount]. No-op if gap is same position (gap == srcIndex or gap == srcIndex + 1).
    void reorderInsertWithinStage(TrackId trackId, InsertSlotId slotId, int gapIndexInStage);

    /// Live rows export their current `getStateInformation`; unavailable placeholder rows (see
    /// `importChain`) re-emit the saved identity + state byte-for-byte, so saving a project while a
    /// plugin is temporarily missing never erases that insert's configuration.
    [[nodiscard]] PluginTrackChain exportChain(TrackId trackId) const;

    /// Replace chain from project or undo — clears or loads rows + `setStateInformation` when occupied.
    /// A row whose plugin cannot be instantiated (bundle missing, scan/instantiation failure, or the
    /// plugin found at the path is a different identity than the saved `pluginIdentifier`) is kept as
    /// an **unavailable placeholder**: same slot id / stage / chain position, saved identity and
    /// opaque state preserved, no processor published, shown as "<name> (unavailable)". A saved state
    /// is never applied to a plugin of another identity. Explicit `removeInsert` on a placeholder
    /// removes it for good (the next save drops it) — only the user removes inserts.
    void importChain(TrackId trackId, const PluginTrackChain& chain);

    [[nodiscard]] std::vector<InsertRowView> getInsertRowsForTrack(TrackId trackId) const;

    [[nodiscard]] bool hasAnyInsertOnTrack(TrackId trackId) const noexcept;

    /// Clears every instance and closes editors (used before loading a new project).
    void removeAllPlugins() noexcept;

    /// [Message thread] Project close, step 1: close editors, move every live instance into a
    /// pending list, publish an empty realtime map. Does not drain and does not releaseResources.
    /// Returns how many instances were parked. A second call while instances are pending parks nothing.
    int detachAllInstancesForProjectClose() noexcept;

    /// [Message thread] Display name of the next parked instance (empty when none remain).
    [[nodiscard]] juce::String nextDetachedInsertNameForProjectClose() const;

    /// [Message thread] releaseResources and destroy exactly one parked instance.
    /// False when none remain. The caller must already have drained the callback and read-ahead.
    bool releaseNextDetachedInsertForProjectClose() noexcept;

    /// Remove instances without undo (e.g. track deleted from session).
    void evictPluginForTrackNoUndo(TrackId trackId) noexcept;

    void removePlugin(TrackId trackId);

    void openNativeEditor(TrackId trackId);
    void openNativeEditor(TrackId trackId, InsertSlotId slotId);

    /// [Message thread] Host Active for one slot. Off passes the buffer through and, once the
    /// 10 ms ramp has finished, stops calling processBlock. The instance, its parameters and the
    /// slot stay. Turning it back on reset()s the instance first (no frozen tail) and does not
    /// rescan or rebuild it. A stored Bypass choice is kept and shown, but it does not process
    /// while Active is off. One undo step. Marks dirty through the existing undo recorder.
    void setInsertHostActive(TrackId trackId, InsertSlotId slotId, bool active);

    /// [Message thread] Host Bypass for one slot. While Active, processBlock keeps running and
    /// the chain hears this slot's input (latency-aligned inside the slot, up to the runtime
    /// cap). Does not write the plug-in's own bypass parameter. One undo step.
    void setInsertHostBypassed(TrackId trackId, InsertSlotId slotId, bool bypassed);

    /// [Message thread] Current host choices. `available` is false for an unknown id or a row
    /// with no processor (unavailable placeholder).
    struct InsertHostAudible
    {
        bool active = true;
        bool bypassed = false;
        bool available = false;
    };
    [[nodiscard]] InsertHostAudible insertHostAudible(TrackId trackId, InsertSlotId slotId) const;

    /// Identity of one live insert instance. Captured when a preset dialog opens and checked
    /// again when it returns, so a slot that was removed, replaced, or rebuilt by another
    /// project cannot receive that dialog's file. `instanceEpoch` is unique for each instance
    /// DAL places in a slot; it is not reused when the same slot id comes back.
    struct InsertInstanceToken
    {
        TrackId trackId = kInvalidTrackId;
        InsertSlotId slotId = kInvalidInsertSlotId;
        std::uint64_t instanceEpoch = 0;

        [[nodiscard]] bool namesALiveInstance() const noexcept
        {
            return trackId != kInvalidTrackId && slotId != kInvalidInsertSlotId && instanceEpoch != 0;
        }
    };

    /// [Message thread] Token for the instance currently in the slot, or a token that
    /// `namesALiveInstance()` rejects when the slot is missing or only an unavailable placeholder.
    [[nodiscard]] InsertInstanceToken insertInstanceToken(TrackId trackId, InsertSlotId slotId) const;

    /// [Message thread] Write this instance's VST3 preset to `destination`. Uses the wrapper's
    /// own .vstpreset bytes (component class id, processor state, controller state). Does not
    /// mark the project dirty, does not record undo, and does not change the project path.
    /// A write failure leaves an existing file in place. Host Active / Bypass are not stored.
    [[nodiscard]] juce::Result saveInsertPresetToFile(InsertInstanceToken token, const juce::File& destination);

    /// [Message thread] Replace this instance's plug-in state from a .vstpreset file. The slot
    /// and chain order stay. Host Active / Bypass stay. A successful load records one
    /// "Load preset" undo step (before / after plug-in state) and marks the project dirty
    /// through the existing undo recorder. A rejected file or a missed exclusive-access
    /// window changes nothing and records nothing.
    [[nodiscard]] juce::Result loadInsertPresetFromFile(InsertInstanceToken token, const juce::File& source);

    /// [Message thread] Copy this instance's VST3 preset bytes and component class id into the
    /// process-wide insert clipboard. The copy is the container `getPreset` returns, including
    /// state that is not a parameter. It is not the system clipboard, not project state, and it
    /// is not written to disk. A successful copy replaces the previous one. A failed copy leaves
    /// that previous copy in place. Does not record undo and does not mark the project dirty.
    /// The clipboard lasts until the process exits.
    [[nodiscard]] juce::Result copyInsertSettings(InsertInstanceToken token);

    /// [Message thread] Apply the insert clipboard to this instance through the same preset apply
    /// as Load preset. The class id in the clipboard must match the class id in a preset this
    /// instance produces; the plug-in name and path are not the check. Host Active / Bypass,
    /// stage and chain position stay. A successful paste records one "Paste plugin settings"
    /// undo step and marks the project dirty through the existing undo recorder. An empty
    /// clipboard, a different class, a missing instance, or a missed exclusive-access window
    /// changes nothing and records nothing.
    [[nodiscard]] juce::Result pasteInsertSettings(InsertInstanceToken token);

    /// [Message thread] Whether Paste settings can be offered for this slot. Compares the
    /// clipboard's class id with the class id cached when the instance was placed. Does not
    /// capture plug-in state and does not take exclusive access.
    [[nodiscard]] bool canPasteInsertSettings(TrackId trackId, InsertSlotId slotId) const;

    /// [Message thread] Runs `fn` while the audio callback is drained and the read-ahead worker
    /// is paused, when the app has installed a hook. Tests with no device call `fn` directly.
    /// Used so reset() on reactivation cannot overlap processBlock.
    void setExclusiveInstanceAccess(std::function<void(std::function<void()>)> hook) noexcept
    {
        exclusiveInstanceAccess_ = std::move(hook);
    }

    /// [Message thread] Fired after a host Active/Bypass change so the read-ahead queue for the
    /// old wet mix can be discarded. Wired to `ReadAheadRenderer::requestFullReset`.
    void setAudibleQueueInvalidation(std::function<void()> hook) noexcept
    {
        audibleQueueInvalidation_ = std::move(hook);
    }
    void openGenericParamsEditor(TrackId trackId);
    void openGenericParamsEditor(TrackId trackId, InsertSlotId slotId);
    void editorWindowClosing(TrackId trackId, InsertSlotId slotId, bool wasGenericEditor);

    [[nodiscard]] bool hasPluginOnTrack(TrackId trackId) const noexcept;

    /// [Message thread] `AudioPluginInstance::getName()` for the primary UI slot, else empty.
    [[nodiscard]] juce::String getPluginDisplayNameForTrack(TrackId trackId) const;

    /// [Message thread] If any open editor's live state differs from the snapshot taken at open, record
    /// one "Plugin parameters" undo step per key and refresh the snapshot. Does not close editors.
    void flushOpenEditorParameterUndoSteps();

    // Called from `PlaybackEngine::audioDeviceAboutToStop` / device restart path.
    void prepareForDevice(double sampleRate, int blockSize, int numOutputChannels);
    void releaseResources();

    // -------------------------------------------------------------------------
    // [Audio thread]
    // -------------------------------------------------------------------------

    void audioThread_clearScratch(int numChannels, int numSamples) noexcept;

    [[nodiscard]] float* const* audioThread_getScratchWritePointers() noexcept;

    /// [Audio thread] Runs `layoutOk` slots for one stage in published order (Pre / Post), on the
    /// host-owned shared scratch + MIDI scratch (the serial paths' entry point; the callback's
    /// lane). Delegates to `audioThread_processEntryChain`.
    void audioThread_processChainForTrack(TrackId trackId, InsertStage stage, int numSamples) noexcept;

    // -------------------------------------------------------------------------
    // Stage A1 — per-entry chain processing with caller-owned buffers (render-pool jobs)
    // -------------------------------------------------------------------------
    /// Number of distinguishable processing lanes (render-pool workers + the callback thread).
    /// Matches `instrument_render::InstrumentRenderPool::kNumLanes`; the callback uses the last.
    /// Lanes 0..14 = render-pool workers, 15 = the audio callback, 16 = the experimental
    /// read-ahead worker (docs/READAHEAD_PROTOTYPE.md — exists only with the CLI flag).
    static constexpr int kMaxProcessingLanes = 17;
    static constexpr int kCallbackProcessingLane = 15;
    static constexpr int kReadAheadProcessingLane = 16;

    /// [Audio thread] ONE acquire-load of the published map for this block; jobs receive the
    /// already-resolved entry pointers (no map loads on workers). The returned shared_ptr must be
    /// retained by the callback until every job of the block has joined.
    [[nodiscard]] std::shared_ptr<const PluginAudioThreadMap> audioThread_acquireMapForBlock() const noexcept
    {
        return std::atomic_load_explicit(&audioThreadMap_, std::memory_order_acquire);
    }
    [[nodiscard]] static const PluginAudioThreadMap::Entry*
        audioThread_findEntry(const PluginAudioThreadMap& map, TrackId trackId) noexcept;

    /// True iff the entry has at least one stereo-ready slot — the entry-resolved equivalent of
    /// `audioThread_hasActivePluginForTrack` (identical gating semantics).
    [[nodiscard]] static bool audioThread_entryHasActiveSlot(const PluginAudioThreadMap::Entry& entry) noexcept
    {
        for (const auto& sp : entry.slots)
        {
            if (sp.processor != nullptr && sp.layoutOk)
            {
                return true;
            }
        }
        return false;
    }

    /// [Audio thread, callback lane only] The serial paths' shared chain buffers, for building a
    /// chain-access bundle that runs through the same strip core as the render-pool jobs.
    [[nodiscard]] int audioThread_sharedChainScratchCapacity() const noexcept { return scratch_.getNumSamples(); }
    [[nodiscard]] juce::MidiBuffer& audioThread_sharedChainMidiScratch() noexcept { return midiScratch_; }

    /// [Job thread that owns `entry` for the current segment] Set the chain's own playhead.
    /// Writes pre-existing scalar storage only.
    static void audioThread_setEntryTransportContext(const PluginAudioThreadMap::Entry& entry,
                                                     const PluginProcessTransportContext& context) noexcept;

    /// [Exactly one thread per entry per block — a render-pool job (its lane) or the callback]
    /// Runs `layoutOk` slots of one stage in published order on CALLER-owned stereo scratch
    /// (`scratchChannels[0..1]`, capacity `scratchCapacitySamples`) and a CALLER-owned MIDI
    /// scratch (cleared after every `processBlock`, exactly like the serial path). `laneIndex`
    /// selects the per-lane C2B diagnostics marker. No locks, no allocation.
    void audioThread_processEntryChain(const PluginAudioThreadMap::Entry& entry,
                                       InsertStage stage,
                                       int numSamples,
                                       float* const* scratchChannels,
                                       int scratchCapacitySamples,
                                       juce::MidiBuffer& midiScratch,
                                       int laneIndex) noexcept;

    /// [Audio thread, or offline render while the callback gate is held] Sets the transport
    /// context observed by EVERY published chain's playhead (the serial paths' refresh; render-
    /// pool jobs refresh their own entry via `audioThread_setEntryTransportContext` instead and
    /// never run concurrently with this). The caller must refresh this before every timeline
    /// segment, including stopped monitoring; this only writes pre-existing scalar storage and
    /// never allocates, locks or touches Session.
    void audioThread_setProcessTransportContext(const PluginProcessTransportContext& context) noexcept;

    /// [Audio thread] Same as above, but SKIPS the chains of `excludedTrackIds` (the experimental
    /// read-ahead rows, docs/READAHEAD_PROTOTYPE.md §8): while a row is owned, its entry playhead
    /// has exactly one writer (the worker via `audioThread_setEntryTransportContext`), so the
    /// serial refresh must not race it. The shared default playhead is still written (owned rows'
    /// published chains never read it).
    void audioThread_setProcessTransportContextExcept(const PluginProcessTransportContext& context,
                                                      const TrackId* excludedTrackIds,
                                                      int excludedCount) noexcept;

    /// [Message thread, device prepared] TEST SEAM (same contract as
    /// `ExperimentalInstrumentHost::installInstrumentInstanceForTests`): append `instance` as a
    /// live insert of `stage` on `trackId`, prepare it for the current device format and publish.
    /// Lets focused tests run REAL chain processing with deterministic processors, no VST3 files.
    bool installInsertInstanceForTests(TrackId trackId,
                                       InsertStage stage,
                                       std::unique_ptr<juce::AudioPluginInstance> instance);

    /// [Audio thread] Acquire-loads the published map; true iff any insert on this track is stereo-ready.
    [[nodiscard]] bool audioThread_hasActivePluginForTrack(TrackId trackId) const noexcept;

    /// [Any thread] Stability C2B diagnostics: which insert (track/slot/stage) the audio thread is
    /// currently inside, or "insert=idle". Read by gate-timeout logging only.
    [[nodiscard]] juce::String describeAudioThreadInsertStateForDiagnostics() const noexcept;

    /// Peak-hold levels of ONE track's insert chain as the audio thread processed it: the scratch
    /// BEFORE the first Pre insert (i.e. after pre-gain) and AFTER the last Post insert (before
    /// pan). Block counters show whether the chain was processed at all.
    /// A muted row, an Off row, or a row with no active insert never reaches
    /// `audioThread_processChainForTrack`. An audible row does, including intervals
    /// where no clip is feeding source.
    struct InsertLevelTapSnapshot
    {
        float peakBeforeFirstInsert = 0.0f;
        float peakAfterLastInsert = 0.0f;
        /// RMS over all tapped samples of the window (both channels) — stable across musical
        /// material where a peak hold is not.
        double rmsBeforeFirstInsert = 0.0;
        double rmsAfterLastInsert = 0.0;
        /// Mean sample value per channel (L, R) BEFORE the first insert over the window — the
        /// offset the instrument / proxy delivered at the chain boundary (no mid-sum).
        double dcBeforeFirstInsert[2] = { 0.0, 0.0 };
        std::uint32_t preStageBlocks = 0;
        std::uint32_t postStageBlocks = 0;
    };
    /// [Message thread] Diagnostics only: selects the tapped track (`kInvalidTrackId` = off).
    void setInsertLevelTapTrackForDiagnostics(TrackId trackId) noexcept;
    /// [Message thread] Diagnostics only: returns the peak holds since the previous call and resets
    /// them. The audio thread folds one SIMD min/max per tapped stage into relaxed atomics.
    [[nodiscard]] InsertLevelTapSnapshot readAndResetInsertLevelTapForDiagnostics() noexcept;

    /// [Message thread] Stability C3 introspection: `{trackId, live AudioPluginInstance pointers}`
    /// per chain in the message-thread registry. Diagnostics only; no locks.
    [[nodiscard]] std::vector<std::pair<TrackId, std::vector<const void*>>>
        exportChainInstancePointersForDiagnostics() const;

    /// [Message thread] Stability C3 introspection: `{trackId, processor pointers}` per entry in
    /// the currently *published* realtime map. Diagnostics only; no locks.
    [[nodiscard]] std::vector<std::pair<TrackId, std::vector<const void*>>>
        exportPublishedMapPointersForDiagnostics() const;

    /// [Message thread] Stability / diagnostics: the live instance at chain position `chainIndex`
    /// of a track (nullptr for an unavailable placeholder or out of range). Parameter reads /
    /// writes on it follow the generic-editor rules (message thread, host-owned instance).
    [[nodiscard]] juce::AudioPluginInstance* liveInstanceAtChainIndexForDiagnostics(TrackId trackId,
                                                                                    int chainIndex) const noexcept;

    /// [Message thread] Optional hook run after publishing a realtime map that dropped live plugin
    /// instances, *before* those instances are released/destroyed (Stability Slice 3, publish-before-
    /// destroy). Wired to `PlaybackEngine::waitForAudioCallbackExit` so an in-flight audio callback
    /// still holding the old map cannot touch freed AudioProcessor pointers.
    void setRealtimeDrainAfterPublish(std::function<void()> drain) noexcept
    {
        realtimeDrainAfterPublish_ = std::move(drain);
    }

    using PluginUndoRecorder = void (*)(void* context, const juce::String& label, const PluginUndoStepSides& sides);
    void setUndoRecorder(void* context, PluginUndoRecorder recorder) noexcept
    {
        undoContext_ = context;
        undoRecorder_ = recorder;
    }

    void setEditorShortcutCallbacks(PluginEditorWindowHostShortcuts callbacks) noexcept
    {
        editorShortcutCallbacks_ = std::move(callbacks);
    }

private:
    using EditorKey = std::pair<TrackId, InsertSlotId>;

    struct LiveInsertSlot
    {
        InsertSlotId slotId = kInvalidInsertSlotId;
        InsertStage stage = InsertStage::Post;
        std::unique_ptr<juce::AudioPluginInstance> instance;
        bool layoutOk = false;
        /// Host Active / Bypass. Kept on an unavailable placeholder as well (also copied onto
        /// `unavailableDescriptor` so export/import round-trips them).
        bool hostActive = true;
        bool hostBypassed = false;
        /// Bumped once per placed instance. Preset dialogs bind to this, not to the slot id alone.
        std::uint64_t instanceEpoch = 0;
        /// Component class id from one `getPreset` when this instance was placed, before it was
        /// published. The insert menu compares this with the clipboard. Empty when the instance
        /// produced no preset. The apply path reads a fresh preset and does not trust this alone.
        juce::String vst3ClassId;
        /// Mix state co-owned by every published map entry for this slot.
        std::shared_ptr<InsertSlotRuntime> runtime;
        /// Unavailable placeholder (see `importChain`): `instance == nullptr` and this descriptor
        /// (`occupied == true`) carries the saved path / identifier / opaque state untouched so
        /// `exportChain` can re-emit it. Default (`occupied == false`) for live rows.
        PluginInsertDescriptor unavailableDescriptor;

        [[nodiscard]] bool isUnavailablePlaceholder() const noexcept
        {
            return instance == nullptr && unavailableDescriptor.occupied;
        }
    };

    /// Among the plugin types found in `vst3File`, pick the one matching `savedIdentifier` (exact
    /// JUCE identifier string, or same format/name/uid when only the bundle path hash differs —
    /// a relocated copy of the same plugin). Empty `savedIdentifier` = first type (legacy rows).
    /// Returns an empty description (and `err`) when nothing matches — the saved state then stays
    /// on an unavailable placeholder instead of being applied to a different plugin.
    [[nodiscard]] static juce::PluginDescription pickDescriptionForSavedIdentity(
        const juce::File& vst3File,
        juce::AudioPluginFormatManager& fm,
        const juce::String& savedIdentifier,
        juce::String& err);

    void rebuildAudioThreadMapAndPublish();
    void closeEditorsForTrack(TrackId trackId);
    void closeEditorForSlot(TrackId trackId, InsertSlotId slotId);
    void importChainNoUndo(TrackId trackId, const PluginTrackChain& chain);

    /// In-place undo of plug-in state. `Busy` means exclusive access was not obtained, so the
    /// live instance is left as it is; the caller must not rebuild the chain on that result.
    enum class InPlaceRestore
    {
        Applied,
        DifferentChain,
        Busy
    };
    [[nodiscard]] InPlaceRestore tryInPlaceParameterStateRestore(TrackId trackId, const PluginTrackChain& targetChain);

    /// [Message thread] Runs `fn` only when the audio callback has left and the read-ahead worker
    /// has acknowledged a pause. False means `fn` did not run and no pause remains held.
    [[nodiscard]] bool withExclusiveInstanceAccess(const std::function<void()>& fn);

    void stampNewInstanceEpoch(LiveInsertSlot& slot) noexcept;

    [[nodiscard]] const LiveInsertSlot* findToken(const InsertInstanceToken& token) const noexcept;
    [[nodiscard]] LiveInsertSlot* findTokenMutable(const InsertInstanceToken& token) noexcept;

    /// Put one slot's processor state back from a chain snapshot. Host flags are not rewritten.
    void restoreSlotOpaqueState(TrackId trackId, LiveInsertSlot& slot, const PluginTrackChain& chain);

    void rememberHostAppliedState(TrackId trackId, InsertSlotId slotId, const juce::MemoryBlock& state);

    void pushPluginParameterUndoStep(TrackId trackId,
                                   InsertSlotId slotId,
                                   const juce::MemoryBlock& baselineOpaqueState);

    [[nodiscard]] juce::Result addInsertFromVst3FileNoUndo(TrackId trackId,
                                                          InsertStage stage,
                                                          const juce::File& vst3File);

    [[nodiscard]] InsertSlotId allocateSlotId() noexcept;

    void insertLiveSlotSorted(TrackId trackId, LiveInsertSlot slot);

    [[nodiscard]] LiveInsertSlot* findLiveMutable(TrackId trackId, InsertSlotId slotId) noexcept;
    [[nodiscard]] const LiveInsertSlot* findLiveConst(TrackId trackId, InsertSlotId slotId) const noexcept;
    [[nodiscard]] const LiveInsertSlot* findPrimaryUiSlotConst(TrackId trackId) const noexcept;

    /// [Message thread] Logs negotiated layout after `prepareToPlay` (not on the audio callback).
    void logPluginInstanceLayout(const char* context, juce::AudioPluginInstance& inst) const;

    /// [Message thread] Release resources, set stereo I/O, `setBusesLayout` when supported, `prepareToPlay`.
    /// Returns whether main bus is 2-in / 2-out.
    [[nodiscard]] bool tryPrepareStereoInsert(juce::AudioPluginInstance& inst, double sr, int bs);
    void logStereoLayoutFailure(TrackId trackId) const;

    /// [Message thread] Retires one track's live slots: moves them out of `chains_`, republishes the
    /// realtime map, runs the drain hook, then releases/destroys the retired instances (F5 fix).
    void retireChainPublishDrainAndDestroy(TrackId trackId);

    juce::AudioPluginFormatManager formatManager_;
    std::unordered_map<TrackId, std::vector<LiveInsertSlot>> chains_;
    std::function<void()> realtimeDrainAfterPublish_;
    /// Parked by `detachAllInstancesForProjectClose` until released one at a time.
    std::vector<std::unique_ptr<juce::AudioPluginInstance>> projectClosePending_;
    std::vector<juce::String> projectClosePendingNames_;
    InsertSlotId nextInsertSlotId_ = 1;

    double sampleRate_ = 0.0;
    int blockSize_ = 0;
    int numOutChannels_ = 2;

    juce::AudioBuffer<float> scratch_;
    std::vector<float*> scratchPtrs_;
    /// Default playhead installed at instance creation, before the publish re-points the instance
    /// at its chain's own playhead. Outlives every instance; never queried after publication.
    InsertProcessPlayHead processPlayHead_;
    /// Per-chain playheads (Stage A1): created on demand on the message thread, co-owned by every
    /// published `Entry` that references them, NEVER erased until destruction — so an in-flight
    /// callback holding an older map keeps a valid pointer across chain removal / track deletion.
    std::unordered_map<TrackId, std::shared_ptr<InsertProcessPlayHead>> chainPlayHeads_;
    /// [Message thread] Get-or-create the chain playhead for a track.
    [[nodiscard]] std::shared_ptr<InsertProcessPlayHead> chainPlayHeadForTrack(TrackId trackId);
    /// Reused empty MIDI buffer for `processBlock` on the serial paths; cleared after each call —
    /// avoids constructing `MidiBuffer` on the audio thread (default construction is cheap;
    /// `clear` does not grow). Render-pool jobs use caller-owned per-lane MIDI buffers instead.
    juce::MidiBuffer midiScratch_;
    /// Set true after the one-shot `callAsync` mismatch warning; cleared in `prepareForDevice`.
    std::atomic<bool> scratchMismatchNotified_{ false };
    /// Stability C2B diagnostics, PER LANE (Stage A1): set around each `processBlock` call on the
    /// thread running that lane (relaxed; -1 = idle). Never used for synchronization; per-lane
    /// slots keep concurrent jobs from interleaving misleading markers.
    std::atomic<std::int64_t> laneInsertTrackId_[kMaxProcessingLanes];
    std::atomic<int> laneInsertSlotIndex_[kMaxProcessingLanes];
    std::atomic<int> laneInsertStage_[kMaxProcessingLanes];
    /// At most one stereo-layout warning while re-preparing instances for a device (message thread).
    std::atomic<bool> stereoPrepareFailureOneShot_{ false };
    /// Insert level tap (diagnostics, see `InsertLevelTapSnapshot`): relaxed atomics only; the audio
    /// thread folds peaks, the message thread reads and resets. Never used for synchronization.
    std::atomic<std::int64_t> insertLevelTapTrackId_{ -1 };
    std::atomic<float> insertLevelTapPeakBefore_{ 0.0f };
    std::atomic<float> insertLevelTapPeakAfter_{ 0.0f };
    std::atomic<std::uint32_t> insertLevelTapPreBlocks_{ 0 };
    std::atomic<std::uint32_t> insertLevelTapPostBlocks_{ 0 };
    std::atomic<double> insertLevelTapSumSqBefore_{ 0.0 };
    std::atomic<double> insertLevelTapSumSqAfter_{ 0.0 };
    std::atomic<std::uint64_t> insertLevelTapSamplesBefore_{ 0 };
    std::atomic<std::uint64_t> insertLevelTapSamplesAfter_{ 0 };
    std::atomic<double> insertLevelTapSumBefore_[2]{ 0.0, 0.0 };
    /// [Any processing thread] Fold the CALLER scratch's peak and sum of squares of the first
    /// `numSamples` into the tap accumulators (relaxed CAS max / fetch_add; no locks, no
    /// allocation). Parameterized by scratch so render-pool jobs fold their own lane buffers.
    void audioThread_foldScratchLevelsInto(const float* const* scratchChannels,
                                           std::atomic<float>& peakHold,
                                           std::atomic<double>& sumSquares,
                                           std::atomic<std::uint64_t>& sampleCount,
                                           int numSamples) noexcept;

    std::atomic<std::shared_ptr<const PluginAudioThreadMap>> audioThreadMap_;

    std::map<EditorKey, juce::MemoryBlock> editorOpenState_;
    /// Live `getStateInformation` blob immediately after the last host `setStateInformation` / in-place restore.
    /// Used to suppress spurious "Plugin parameters" undo when plugin export is not byte-stable or includes view state.
    std::map<EditorKey, juce::MemoryBlock> lastHostAppliedState_;
    std::map<EditorKey, std::unique_ptr<PluginEditorWindow>> editorWindows_;
    std::map<EditorKey, std::unique_ptr<PluginParamsWindow>> paramsWindows_;

    PluginEditorWindowHostShortcuts editorShortcutCallbacks_;

    void* undoContext_ = nullptr;
    PluginUndoRecorder undoRecorder_ = nullptr;
    std::function<void(std::function<void()>)> exclusiveInstanceAccess_;
    std::function<void()> audibleQueueInvalidation_;
    std::uint64_t nextInstanceEpoch_ = 1;

    /// In-memory .vstpreset plus the class id `inspect` read from it. One per host, which is one
    /// per process. Not cleared by a project change.
    struct SettingsClipboard
    {
        juce::String classId;
        juce::MemoryBlock preset;

        [[nodiscard]] bool occupied() const noexcept
        {
            return classId.isNotEmpty() && preset.getSize() > 0;
        }
    };
    SettingsClipboard settingsClipboard_;

    void rememberVst3ClassId(LiveInsertSlot& slot);
    [[nodiscard]] juce::Result applyPresetBytes(InsertInstanceToken token,
                                               const juce::MemoryBlock& preset,
                                               const juce::String& undoLabel);

    void ensureInsertRuntime(LiveInsertSlot& slot);
    void recordHostAudibleUndo(const juce::String& label, TrackId trackId, const PluginTrackChain& before);

    void recordPluginSlotUndo(const juce::String& label, const PluginUndoStepSides& sides);
};
