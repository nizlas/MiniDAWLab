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

    /// [Message thread] Move an occupied insert to the end of Pre or Post; keeps the live instance and slot id.
    void moveInsertToStage(TrackId trackId, InsertSlotId slotId, InsertStage newStage);

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

    /// [Audio thread] Acquire-loads the published map; true iff any insert on this track is stereo-ready.
    [[nodiscard]] bool audioThread_hasActivePluginForTrack(TrackId trackId) const noexcept;

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

    juce::AudioPluginFormatManager formatManager_;
    std::unordered_map<TrackId, std::vector<LiveInsertSlot>> chains_;
    InsertSlotId nextInsertSlotId_ = 1;

    double sampleRate_ = 0.0;
    int blockSize_ = 0;
    int numOutChannels_ = 2;

    juce::AudioBuffer<float> scratch_;
    std::vector<float*> scratchPtrs_;
    /// Reused empty MIDI buffer for `processBlock`; cleared after each call — avoids constructing
    /// `MidiBuffer` on the audio thread (default construction is cheap; `clear` does not grow).
    juce::MidiBuffer midiScratch_;
    /// Set true after the one-shot `callAsync` mismatch warning; cleared in `prepareForDevice`.
    std::atomic<bool> scratchMismatchNotified_{ false };
    /// At most one stereo-layout warning while re-preparing instances for a device (message thread).
    std::atomic<bool> stereoPrepareFailureOneShot_{ false };

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
