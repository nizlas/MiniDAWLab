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

    /// Remove instances without undo (e.g. track deleted from session).
    void evictPluginForTrackNoUndo(TrackId trackId) noexcept;

    void removePlugin(TrackId trackId);

    void openNativeEditor(TrackId trackId);
    void openNativeEditor(TrackId trackId, InsertSlotId slotId);
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
    /// pan). Block counters show whether the chain was processed at all — a muted or silent track
    /// never reaches `audioThread_processChainForTrack`, so its counters stay 0.
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

    [[nodiscard]] bool tryInPlaceParameterStateRestore(TrackId trackId, const PluginTrackChain& targetChain);

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

    void recordPluginSlotUndo(const juce::String& label, const PluginUndoStepSides& sides);
};
