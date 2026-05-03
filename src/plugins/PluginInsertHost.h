#pragma once

// =============================================================================
// PluginInsertHost — message-thread owner of per-track VST3 insert instances (Phase 8)
// =============================================================================
//
// ROLE
//   Loads at most one `juce::AudioPluginInstance` per `TrackId`, prepares it for the current
//   device rate / block size, publishes an atomic read-only view for `PlaybackEngine`, and
//   owns pre-allocated **scratch** buffers the audio thread writes clip sums into before
//   `processBlock`.
//
// NOT IN `SessionSnapshot`
//   Live plugins are mutable and may show UI — they never appear inside immutable timeline snapshots.
//
// THREADING
//   Construct / load / remove / editors / `prepareForDevice` / `importSlot`: [Message thread].
//   `audioThread_clearScratch`, `audioThread_getScratchWritePointers`, `audioThread_processForTrack`,
//   `audioThread_hasActivePluginForTrack`:
//   [Audio thread] — no locks, no allocation; only touches pre-sized buffers and the atomic map.
//
// See `docs/PHASE_PLAN.md` Phase 8 and `docs/ARCHITECTURE_PRINCIPLES.md` Plugin host section.
// =============================================================================

#include "plugins/PluginTrackSlot.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <atomic>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

class PluginEditorWindow;
class PluginParamsWindow;

// Immutable view exchanged with the audio callback (release-store / acquire-load).
struct PluginAudioThreadMap
{
    struct Entry
    {
        TrackId trackId = kInvalidTrackId;
        juce::AudioProcessor* processor = nullptr; // non-owning; instance kept alive by message thread
        /// Phase 8: true only when main bus in/out are both stereo after negotiation.
        bool layoutOk = false;
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

    [[nodiscard]] juce::Result loadVst3FromFile(TrackId trackId, const juce::File& vst3File);

    /// Clears every instance and closes editors (used before loading a new project).
    void removeAllPlugins() noexcept;

    /// Remove instance without undo (e.g. track deleted from session).
    void evictPluginForTrackNoUndo(TrackId trackId) noexcept;

    void removePlugin(TrackId trackId);

    void openNativeEditor(TrackId trackId);
    void openGenericParamsEditor(TrackId trackId);
    void editorWindowClosing(TrackId trackId, bool wasGenericEditor);

    [[nodiscard]] PluginTrackSlot exportSlot(TrackId trackId) const;

    /// Replace slot from project or undo — clears or loads + `setStateInformation` when occupied.
    void importSlot(TrackId trackId, const PluginTrackSlot& slot);

    [[nodiscard]] bool hasPluginOnTrack(TrackId trackId) const noexcept;

    // Called from `PlaybackEngine::audioDeviceAboutToStop` / device restart path.
    void prepareForDevice(double sampleRate, int blockSize, int numOutputChannels);
    void releaseResources();

    // -------------------------------------------------------------------------
    // [Audio thread]
    // -------------------------------------------------------------------------

    void audioThread_clearScratch(int numChannels, int numSamples) noexcept;

    [[nodiscard]] float* const* audioThread_getScratchWritePointers() noexcept;

    void audioThread_processForTrack(TrackId trackId, int numSamples) noexcept;

    /// [Audio thread] Acquire-loads the published map; true iff this track has `layoutOk` stereo insert.
    [[nodiscard]] bool audioThread_hasActivePluginForTrack(TrackId trackId) const noexcept;

private:
    void rebuildAudioThreadMapAndPublish();
    void closeEditorsForTrack(TrackId trackId);
    void snapshotEditorOpenStateForUndo(TrackId trackId);
    void maybeRecordEditorCloseUndo(TrackId trackId);
    /// [Message thread] Logs negotiated layout after `prepareToPlay` (not on the audio callback).
    void logPluginInstanceLayout(const char* context, juce::AudioPluginInstance& inst) const;

    /// [Message thread] Release resources, set stereo I/O, `setBusesLayout` when supported, `prepareToPlay`.
    /// Returns whether main bus is 2-in / 2-out.
    [[nodiscard]] bool tryPrepareStereoInsert(juce::AudioPluginInstance& inst, double sr, int bs);
    void logStereoLayoutFailure(TrackId trackId) const;

    juce::AudioPluginFormatManager formatManager_;
    std::unordered_map<TrackId, std::unique_ptr<juce::AudioPluginInstance>> instances_;
    std::unordered_map<TrackId, juce::MemoryBlock> editorOpenState_;

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

    /// Owned by the message thread only; mirrored into `PluginAudioThreadMap::Entry::layoutOk` when publishing.
    std::unordered_map<TrackId, bool> insertLayoutOk_;

    std::unordered_map<TrackId, std::unique_ptr<PluginEditorWindow>> editorWindows_;
    std::unordered_map<TrackId, std::unique_ptr<PluginParamsWindow>> paramsWindows_;

public:
    using PluginUndoRecorder = void (*)(void* context, const juce::String& label, const PluginUndoStepSides& sides);
    void setUndoRecorder(void* context, PluginUndoRecorder recorder) noexcept
    {
        undoContext_ = context;
        undoRecorder_ = recorder;
    }

private:
    void* undoContext_ = nullptr;
    PluginUndoRecorder undoRecorder_ = nullptr;

    void recordPluginSlotUndo(const juce::String& label, const PluginUndoStepSides& sides);
};
