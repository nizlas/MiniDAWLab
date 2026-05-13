#pragma once

// =============================================================================
// ExperimentalInstrumentHost — single global VST3 instrument slot (I1 feasibility)
// =============================================================================
//
// ROLE
//   Message-thread owner of one optional juce::AudioPluginInstance intended for instruments
//   (e.g. Groove Agent SE): MIDI in from the message thread via a small locked MidiBuffer
//   queue (I1), stereo audio out mixed by PlaybackEngine after the normal clip/insert sum.
//
// NOT in Session / ProjectFile; no undo; experimental UX only.
//
// THREADING
//   load / unload / editor / prepareForDevice: [Message thread].
//   audioThread_processBlockAndAddToOutputs: [Audio thread] — reads atomic plugin holder;
//   drains UI MIDI into a block buffer under a short CriticalSection, then processBlock.
//   (We avoid juce::MidiMessageCollector here: VST3 host headers can break `juce::` lookup on MSVC.)
//
// See docs/PHASE_PLAN.md — this is a narrow vertical slice before MIDI tracks / instruments.
// =============================================================================

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>

#include "diagnostics/DrumNameDiagnosticConfig.h"
#include "plugins/Vst3ChildProcessScan.h"

namespace experimental_instrument_host_detail
{
struct DrumNameVst3ProbeDetails;
}

class ExperimentalInstrumentHost
{
public:
    static constexpr int kStereoChannels = 2;

    /// Fallback MIDI pad row range when `derivePrimaryDrumPadDisplayNotesFromRawMap` cannot find a confident
    /// primary cluster (legacy Groove Agent SE layout). Prefer metadata-derived display notes when available.
    static constexpr int kFallbackPrimaryDrumPadDisplayMin = 36;
    static constexpr int kFallbackPrimaryDrumPadDisplayMax = 51;

    ExperimentalInstrumentHost();
    ~ExperimentalInstrumentHost();

    ExperimentalInstrumentHost(const ExperimentalInstrumentHost&) = delete;
    ExperimentalInstrumentHost& operator=(const ExperimentalInstrumentHost&) = delete;

    /// [Message thread] Loads from a .vst3 bundle path; replaces any existing instrument.
    /// On failure the previous slot (if any) is cleared.
    [[nodiscard]] juce::Result loadInstrumentFromVst3File(const juce::File& vst3File);

    /// [Message thread] Loads from a description obtained out-of-process (e.g. raw OOP scan or XML cache).
    /// Skips in-process findAllTypesForFile. If `desc.fileOrIdentifier` is empty, uses `originalPath`.
    /// `sourceTag` is copied into experimental-instrument.log (e.g. "oop-description", "cached-oop-description").
    /// If `pluginStateToRestore` is non-null and non-empty, `setStateInformation` runs on the message thread
    /// after layout `prepare` and before publishing `activeOwner_` (audio thread never sees pre-restored instance).
    /// On restore failure the instance is reset via `releaseResources` + `tryPrepareInstrumentLayout` (default patch).
    /// `outPluginStateRestoreWarning` receives a user-facing message if restore fails (load still succeeds).
    [[nodiscard]] juce::Result loadInstrumentFromDescription(
        const juce::PluginDescription& desc,
        const juce::File& originalPath,
        const char* sourceTag = "oop-description",
        const juce::MemoryBlock* pluginStateToRestore = nullptr,
        juce::String* outPluginStateRestoreWarning = nullptr);

    /// [Message thread] Appends one line to `experimental-instrument.log` (shared with load/unload diagnostics).
    static void appendInstrumentHostLogLine(const juce::String& message);

    /// [Message thread] Base64 `getStateInformation` for the loaded instrument, or empty.
    [[nodiscard]] juce::String getCurrentInstrumentStateBase64() const;

    /// [Message thread] Best-effort per-MIDI-note label from the **display** pitch-name map (derived primary-pad
    /// set when confident, else [`kFallbackPrimaryDrumPadDisplayMin`, `kFallbackPrimaryDrumPadDisplayMax`]). Empty →
    /// no plugin label for that row (GM/piano policy is decided in the MIDI editor from `hasPluginDrumNameMapAvailable`).
    [[nodiscard]] std::optional<juce::String> getPluginNoteNameIfAvailable(int midiNote,
                                                                           int midiChannel) const;

    /// True when a plugin drum pitch-name map has been confirmed authoritative (not merely non-empty cache).
    /// Drum row resolver suppresses GM fallback only in this state; see `refreshPluginNoteNamesFromActiveInstrumentImpl`.
    [[nodiscard]] bool hasPluginDrumNameMapAvailable() const noexcept;

    /// [Message thread] Rebuilds the transient plugin pitch-name map from the **current** `activeOwner_`
    /// via VST3 unit-info / pitch-name probes. **Not invoked automatically in production** (load, rescan,
    /// cached load, editor open); reserved for future explicit “Import names from loaded plugin” and for
    /// `kDrumNamesDiag` scheduling when enabled.
    void refreshPluginNoteNamesFromActiveInstrument();

    /// [Message thread] Experimental/bootstrap only: seeds drum row maps from parsed v2 `<capabilities>`.
    /// **Not invoked in production.** Automatic plugin-reported names are also disabled; future model is
    /// per-track / project-local maps and optional explicit import.
    void seedDrumDisplayFromCachedCapability(const mini_daw::PluginCapabilities& caps);

    /// [Message thread] Optional hook: invoked after diagnostic `afterEditorOpen` refresh when
    /// `kDrumNamesDiag` is enabled (production keeps this unset). Not invoked from the audio thread.
    void setOnPluginPitchNamesCacheMayHaveChanged(std::function<void()> callback);

    /// [Message thread] After native plugin editor opens, a deferred harvest probes the **loaded instance**
    /// (VST3 IUnitInfo / program pitch names) and merges non-empty names into `InstrumentTrackController`
    /// as `autoPlugin` labels via this callback. Does not populate global v2 capabilities.
    void setOnPluginDrumNamesDiscovered(
        std::function<void(const std::map<int, juce::String>&)> callback);

    /// [Message thread] When non-null, returning true means Phase C isolated audio probe must be skipped
    /// (playback, recording, or count-in). Cleared on destroy / unload.
    void setDrumNamePhaseCAudioProbeShouldSkip(std::function<bool()> shouldSkip);

    /// [Message thread] Runs `findAllTypesForFile` only (no `createPluginInstance`, no bus prep).
    /// Writes flushed boundary lines to `%APPDATA%\\MiniDAWLab\\experimental-vst3-scan-diagnostic.log`.
    /// For comparing plugins that crash during scan vs effects/instruments that return.
    [[nodiscard]] juce::Result diagnosticScanVst3FileOnly(const juce::File& vst3File);

    /// [Message thread] Closes editor, releases instance. Brief wait lets the audio callback
    /// finish any in-flight block (feasibility slice — not a production RT unload protocol).
    void unloadInstrument();

    [[nodiscard]] bool hasInstrument() const noexcept;
    [[nodiscard]] juce::String getInstrumentNameForUi() const;

    /// Bundle / file path last passed to a successful `loadInstrumentFromVst3File` or
    /// `loadInstrumentFromDescription` (empty after unload). Used for advisory project save only.
    [[nodiscard]] juce::String getLastLoadedVst3OriginalPath() const noexcept;

    /// Last successful load description (valid until unload). For cache rescan / capability lookup only.
    [[nodiscard]] bool getLastLoadedPluginDescription(juce::PluginDescription& out) const noexcept;

    void openNativeEditor();
    void editorWindowClosing();

    /// [Message thread] I2: enqueue MIDI for the next audio block. No-op if not on message thread
    /// or no instrument is loaded. Does not touch the plugin on the message thread.
    void enqueueMidiMessageFromMessageThread(const juce::MidiMessage& message);

    /// [Audio thread] Must be called at the start of each device callback before any
    /// `audioThread_addMidiEventForCurrentBlock` (transport I3e scheduling). Sets the sample clamp
    /// window to match this block's `numSamples`.
    void audioThread_beginAudioBlock(int numSamples) noexcept;

    /// [Audio thread] Sample-offset MIDI for the **current** device block (same block as the next
    /// `audioThread_processBlockAndAddToOutputs`). Uses a separate buffer from UI/preview MIDI.
    void audioThread_addMidiEventForCurrentBlock(int sampleOffsetInBlock,
                                                 const juce::MidiMessage& message) noexcept;

    void prepareForDevice(double sampleRate, int blockSize);
    void releaseResources();

    /// [Audio thread] Clears stereo scratch, drains MIDI collector, runs processBlock if loaded
    /// and layout ok, adds first stereo output bus to device buffers (frame 0..numSamples-1).
    void audioThread_processBlockAndAddToOutputs(float* const* outputChannelData,
                                                   int numOutputChannels,
                                                   int numSamples) noexcept;

private:
    struct InstrumentOwner
    {
        std::unique_ptr<juce::AudioPluginInstance> inst;
        bool layoutOk = false;
    };

    [[nodiscard]] bool tryPrepareInstrumentLayout(juce::AudioPluginInstance& inst,
                                                  double sampleRate,
                                                  int blockSize);

    void closeNativeEditor();

    struct MidiIoState;
    std::unique_ptr<MidiIoState> midiIo_;

    juce::AudioPluginFormatManager formatManager_;

    juce::AudioBuffer<float> scratch_;
    std::vector<float*> scratchPtrs_;

    /// Transport-driven MIDI for the current audio callback (no lock; audio thread only).
    juce::MidiBuffer rtBlockMidi_;
    int audioCallbackBlockSamples_ = 0;

    std::atomic<std::shared_ptr<InstrumentOwner>> activeOwner_;

    std::unique_ptr<juce::DocumentWindow> editorWindow_;

    /// [Message thread] Forwards into MidiMessageCollector (implementation in .cpp).
    void queueMidiFromMessageThread(const ::juce::MidiMessage& message);

    double sampleRate_ = 0.0;
    int blockSize_ = 0;

    /// Advisory path for ProjectFile (experimental); not authoritative for loading.
    juce::String lastLoadedVst3OriginalPath_;

    /// Full raw map from VST3 `getProgramPitchName` (diagnostics / authority heuristics). Cleared on unload / refresh.
    std::map<int, juce::String> rawPluginPitchNamesByNote_;

    /// Filtered map for UI and `getPluginNoteNameIfAvailable`: metadata-derived primary pads when confident, else
    /// fallback range only. Cleared on unload / refresh.
    std::map<int, juce::String> pluginPitchNamesByNote_;

    /// Notes that receive plugin labels in `pluginPitchNamesByNote_` after the last transient refresh (derived set
    /// or fallback-only notes present in the raw map).
    std::set<int> primaryPadDisplayActiveNotes_;

    bool pluginDrumNameMapAuthoritative_ = false;

    /// Stable key for v2 drum capability merge deduplication (cleared on unload).
    juce::String lastGrooveDrumCapabilityPersistKey_;

    std::function<void()> onPluginPitchNamesCacheMayHaveChanged_;
    std::function<void(const std::map<int, juce::String>&)> onPluginDrumNamesDiscovered_;
    std::function<bool()> drumNamePhaseCAudioProbeShouldSkip_;

    juce::PluginDescription lastLoadedPluginDescription_{};
    bool lastLoadedPluginDescriptionValid_ = false;

    /// Armed by `schedulePluginPitchNamesRefreshAfterNativeEditorOpened`; consumed by the next
    /// `afterEditorOpen` refresh so Phase C runs at most once per scheduled editor-open probe.
    bool drumNamePhaseCPendingAfterEditorOpen_ = false;

    void refreshPluginNoteNamesFromActiveInstrumentImpl(drum_name_diag::DrumNameRefreshPhase phase,
                                                        bool updateTransientNameCache);

    /// Gated by `kPersistGlobalDrumCapabilityHints` (default false): global v2 drum `<capabilities>` are not
    /// used for production drum-row display even if present on disk.
    void maybePersistGrooveDrumCapabilitiesToV2Cache(
        const experimental_instrument_host_detail::DrumNameVst3ProbeDetails& probe,
        const juce::String& derivationLogReason);

    void runDrumNamePhaseCDiagnosticsIfEligible(drum_name_diag::DrumNameRefreshPhase phase,
                                                bool updateTransientNameCache,
                                                juce::AudioPluginInstance& liveInst);
    void runDrumNamePhaseCAudioProbeIsolated(const std::set<int>& metadataCandidateNotes,
                                             juce::AudioPluginInstance& liveInst);

    void scheduleDrumNameDiagLifecyclePhasesAfterRefreshIfEnabled();

    /// After native VST3 editor is shown (or brought to front): schedules a deferred `afterEditorOpen` harvest from
    /// the loaded instance for track-local discovery (`setOnPluginDrumNamesDiscovered`). When `kDrumNamesDiag` is true,
    /// the transient host pitch-name caches are also rebuilt for diagnostics only.
    void schedulePluginPitchNamesRefreshAfterNativeEditorOpened();
};
