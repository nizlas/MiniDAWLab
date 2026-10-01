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

// Immutable view exchanged with the audio callback (release-store / acquire-load).
struct PluginAudioThreadMap
{
    struct SlotProc
    {
        juce::AudioProcessor* processor = nullptr;
        bool layoutOk = false;
        InsertStage stage = InsertStage::Post;
    };

    struct Entry
    {
        TrackId trackId = kInvalidTrackId;
        /// Pre slots first, then Post — in order.
        std::vector<SlotProc> slots;
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

    [[nodiscard]] PluginTrackChain exportChain(TrackId trackId) const;

    /// Replace chain from project or undo — clears or loads rows + `setStateInformation` when occupied.
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

    /// [Audio thread] Runs `layoutOk` slots for one stage in published order (Pre / Post).
    void audioThread_processChainForTrack(TrackId trackId, InsertStage stage, int numSamples) noexcept;

    /// [Audio thread, or offline render while the callback gate is held] Sets the context returned
    /// by the host-owned JUCE `AudioPlayHead` during the immediately following insert processing.
    /// The caller must refresh this before every timeline segment, including stopped monitoring;
    /// this only writes pre-existing scalar storage and never allocates, locks or touches Session.
    void audioThread_setProcessTransportContext(const PluginProcessTransportContext& context) noexcept;

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

    /// Non-owning `AudioProcessor::setPlayHead` target, owned by this host for longer than every
    /// live plugin instance. The audio callback and gated offline renderer update it immediately
    /// before `processBlock`; message-thread lifecycle code never mutates its position state.
    class InsertProcessPlayHead final : public juce::AudioPlayHead
    {
    public:
        /// [Audio/offline processing thread] Materialize only facts DAL knows for this block.
        /// JUCE turns the engaged fields into the VST3 ProcessContext validity flags.
        void setContext(const PluginProcessTransportContext& context) noexcept;

        /// [Audio/offline processing thread] JUCE calls this synchronously from hosted
        /// `processBlock`. It returns a value copy, so processors cannot retain mutable host state.
        [[nodiscard]] juce::Optional<PositionInfo> getPosition() const override;

    private:
        juce::AudioPlayHead::PositionInfo position_;
    };

    struct LiveInsertSlot
    {
        InsertSlotId slotId = kInvalidInsertSlotId;
        InsertStage stage = InsertStage::Post;
        std::unique_ptr<juce::AudioPluginInstance> instance;
        bool layoutOk = false;
    };

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
    /// Stable playhead pointer installed in every instance before that instance is published to
    /// the audio thread. Its lifetime exceeds all `LiveInsertSlot::instance` lifetimes.
    InsertProcessPlayHead processPlayHead_;
    /// Reused empty MIDI buffer for `processBlock`; cleared after each call — avoids constructing
    /// `MidiBuffer` on the audio thread (default construction is cheap; `clear` does not grow).
    juce::MidiBuffer midiScratch_;
    /// Set true after the one-shot `callAsync` mismatch warning; cleared in `prepareForDevice`.
    std::atomic<bool> scratchMismatchNotified_{ false };
    /// Stability C2B diagnostics: set around each `processBlock` call on the audio thread
    /// (relaxed; -1 = idle). Never used for synchronization.
    std::atomic<std::int64_t> audioThreadInsertTrackId_{ -1 };
    std::atomic<int> audioThreadInsertSlotIndex_{ -1 };
    std::atomic<int> audioThreadInsertStage_{ -1 };
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
    /// [Audio thread] Fold the scratch peak and sum of squares of the first `numSamples` into the
    /// tap accumulators (relaxed CAS max / fetch_add; no locks, no allocation).
    void audioThread_foldScratchLevelsInto(std::atomic<float>& peakHold,
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
