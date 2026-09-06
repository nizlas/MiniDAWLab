#pragma once

// =============================================================================
// ExperimentalInstrumentHost — one VST3 instrument **instance per timeline instrument track**
// =============================================================================
//
// ROLE
//   **Per timeline instrument track:** message-thread owner of **at most one loaded**
//   `juce::AudioPluginInstance` **in this host object** (e.g. Groove Agent SE). There is **no** single
//   app-wide “global instrument slot”: the app keeps **`ExperimentalInstrumentHost` per `TrackId`**
//   (see `docs/CURRENT_ARCHITECTURE.md`). MIDI from the message thread via a short locked `MidiBuffer`
//   queue; stereo out is summed by `PlaybackEngine` for each `TrackKind::Instrument` row that resolves
//   to this host via `ExperimentalInstrumentPlaybackSnapshot`.
//
// Instrument UI state and blobs follow `InstrumentTrackController` / project `experimentalInstrumentTracks[]`;
// this file is runtime hosting only (`activeOwner_` is the one plugin slot **inside this host instance**).
//
// THREADING
//   load / unload / editor / prepareForDevice: [Message thread].
//   audioThread_processBlockAndAddToOutputs: [Audio thread] — reads atomic plugin holder;
//   drains UI MIDI into a block buffer under a short CriticalSection, then processBlock.
//   (We avoid juce::MidiMessageCollector here: VST3 host headers can break `juce::` lookup on MSVC.)
//
// See `docs/CURRENT_ARCHITECTURE.md` for the TrackId-keyed multi-instrument model (`docs/PHASE_PLAN.md`
// is planning/history only—not live wiring).
// =============================================================================

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>

#include "diagnostics/DrumNameDiagnosticConfig.h"
#include "instruments/PrimarySemanticRevision.h"
#include "plugins/Vst3ChildProcessScan.h"
#include "util/AsyncLifetimeToken.h"

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
    /// When `MINIDAW_DIAG_INSTRUMENT_LIFECYCLE` is enabled (non-zero at compile time; default **`0`** in
    /// `DiagnosticBuildFlags.h`), `sourceTag` is copied into `experimental-instrument.log`
    /// (e.g. "oop-description", "cached-oop-description").
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

    /// [Message thread] When `MINIDAW_DIAG_INSTRUMENT_LIFECYCLE` is enabled (default compile-time **`0`**),
    /// appends one line to `experimental-instrument.log` (shared with load/unload diagnostics); otherwise no-op.
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

    /// [Message thread] Schedules the same deferred drum-name harvest as native-editor open. For cases
    /// where a kit may have loaded after the last probe (picked in the already-open editor, state restore).
    /// Result reaches the track via `setOnPluginDrumNamesDiscovered`.
    void requestDeferredPluginDrumNameHarvest();

    /// [Message thread] Clears callbacks wired to external controllers/UI (safe before unload/teardown).
    void clearControllerWireCallbacks() noexcept;

    /// [Message thread] Closes editor, releases instance. Brief wait lets the audio callback
    /// finish any in-flight block (feasibility slice — not a production RT unload protocol).
    void unloadInstrument();

    [[nodiscard]] bool hasInstrument() const noexcept;
    [[nodiscard]] juce::String getInstrumentNameForUi() const;

    // -----------------------------------------------------------------------
    // MIDI delivery capture seam (deterministic Level-1 tests; Phase B)
    // -----------------------------------------------------------------------
    /// Observes the exact merged `juce::MidiBuffer` handed to this host's instrument-processing
    /// boundary, once per processed block, so automated MIDI-routing tests can assert what a
    /// destination would receive **without loading a real VST3**. Implementations must be
    /// RT-safe for the duration of a gated offline test run (no locks/alloc in the callback).
    struct MidiDeliveryCaptureSink
    {
        virtual ~MidiDeliveryCaptureSink() = default;
        /// [Audio/offline render thread] `merged` is the finalized buffer (UI + transport MIDI).
        virtual void onMidiBlockDelivered(const juce::MidiBuffer& merged, int numSamples) = 0;
    };

    /// [Message thread, engine stopped/gated] Install (or clear with `nullptr`) the capture sink.
    /// While installed, the process boundary assembles and delivers the merged MIDI buffer even
    /// when no plugin instance is loaded (audio output is produced only by a real instrument).
    void installMidiDeliveryCaptureSinkForTests(MidiDeliveryCaptureSink* sinkOrNull) noexcept
    {
        midiCaptureSink_.store(sinkOrNull, std::memory_order_release);
    }

    /// True when transport MIDI scheduled into this host is consumed this block: a loaded
    /// instrument, or an installed capture sink (tests without a real VST3).
    [[nodiscard]] bool acceptsTransportMidi() const noexcept
    {
        return hasInstrument() || midiCaptureSink_.load(std::memory_order_acquire) != nullptr;
    }

    /// Cumulative count of blocks whose merged MIDI reached the processing boundary (plugin
    /// process or capture-only). Relaxed; diagnostics/tests.
    [[nodiscard]] std::uint64_t getMidiDeliveryBoundaryBlockCountRelaxed() const noexcept
    {
        return rtMidiDeliveryBoundaryBlocks_.load(std::memory_order_relaxed);
    }

    /// Bundle / file path last passed to a successful `loadInstrumentFromVst3File` or
    /// `loadInstrumentFromDescription` (empty after unload). Used for advisory project save only.
    [[nodiscard]] juce::String getLastLoadedVst3OriginalPath() const noexcept;

    /// Last successful load description (valid until unload). For cache rescan / capability lookup only.
    [[nodiscard]] bool getLastLoadedPluginDescription(juce::PluginDescription& out) const noexcept;

    void openNativeEditor();
    /// [Message thread] Closes the native editor window if open (e.g. before track teardown). No-op if closed.
    void closeNativeEditor();
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

    /// [Audio thread] Samples cap set by `audioThread_beginAudioBlock` for transport MIDI clamps (same thread).
    [[nodiscard]] int audioThread_peekTransportMidiBlockCap() const noexcept
    {
        return audioCallbackBlockSamples_;
    }

    /// Cumulative discard count when `audioThread_addMidiEventForCurrentBlock` rejects (bad block/offets).
    /// Relaxed atomic; diagnostics only — read anytime from UI or routed message-thread logs.
    [[nodiscard]] std::uint64_t getTransportMidiAddEventDiscardedCountRelaxed() const noexcept;

    /// [Message thread] One-line routing/mix diagnostics from atomics/peek (`instrument-audio:` style). Performs
    /// **no file I/O**; callers combine it into `experimental-playback-routing.log` only when
    /// `MINIDAW_DIAG_PLAYBACK_ROUTING` is non-zero at compile time (default **`0`** — `DiagnosticBuildFlags.h`).
    [[nodiscard]] juce::String peekInstrumentAudioRoutingDiagLineForMessageThread() const noexcept;

    /// [Message thread] Throttled native-editor activity hook; **`appendExperimentalPlaybackRoutingLogLine` only when**
    /// `MINIDAW_DIAG_PLAYBACK_ROUTING` is non-zero at compile time (default **`0`**).
    void messageThreadOnNativeEditorUserActivity();

    void prepareForDevice(double sampleRate, int blockSize);
    void releaseResources();

    /// [Audio thread] Clears stereo scratch, drains MIDI collector, runs processBlock if loaded
    /// and layout ok, adds first stereo output bus to device buffers (frame 0..numSamples-1).
    void audioThread_processBlockAndAddToOutputs(float* const* outputChannelData,
                                                 int numOutputChannels,
                                                 int numSamples,
                                                 float outputGain = 1.0f,
                                                 float stereoPan = 0.0f) noexcept;

public:
    /// Stability Slice 4: capture this guard in Timer::callAfterDelay / MessageManager::callAsync
    /// lambdas and check `isAlive()` before touching the host. Invalidated when the host is
    /// destroyed (`clearControllerWireCallbacks` alone does not cover raw-`this` timer captures).
    [[nodiscard]] mini_daw::AsyncCallbackGuard asyncAliveGuard() const noexcept
    {
        return asyncLifetime_.guard();
    }

    // -----------------------------------------------------------------------
    // Primary semantic revision + render-state capture (production; steering
    // §9.4.2 hybrid identity contract, wired in the P1D preflight)
    // -----------------------------------------------------------------------
    /// [Any thread] The host-managed monotonic Primary revision for THIS host slot. Bumped by
    /// every DAL-observable Primary sound-state change (see PrimarySemanticRevision.h for the
    /// authoritative bump-source list). 0 = nothing observed yet. Proxy currency compares a
    /// captured revision against this — never freshly captured state bytes (§9.4.2, PI-028).
    [[nodiscard]] std::uint64_t getPrimarySemanticRevision() const noexcept
    {
        return primarySemanticRevision_.current();
    }

    /// [Any thread] Explicit DAL-initiated Primary state operation (§9.4.2 bump source) that is
    /// not already covered by the load/unload/editor/listener wiring inside this host.
    void bumpPrimarySemanticRevision() noexcept
    {
        (void)primarySemanticRevision_.bump();
    }

    /// [Message thread] Capture the live Primary's exact opaque state bytes for a proxy render
    /// request (§9.4.1 authoritative render state; same `getStateInformation` boundary as Save).
    /// Returns false when no processable instrument is loaded or called off the message thread.
    /// The bytes are the RENDER PAYLOAD restored into the isolated render instance only — never
    /// a semantic currency check.
    [[nodiscard]] bool captureInstrumentStateForRender(juce::MemoryBlock& out) const;

    // -----------------------------------------------------------------------
    // SPIKE-01 diagnostics (P0/P1A validation spike; removable with the spike)
    // -----------------------------------------------------------------------
    /// [Message thread] SPIKE-01 state-capture probe ONLY (see
    /// docs/audits/SPIKE_01_AUTHORITATIVE_PLUGIN_STATE_CAPTURE.md): the live plugin instance,
    /// or nullptr when no instrument is loaded / layout failed / called off the message thread.
    /// The caller must use the pointer synchronously inside the current message-thread callback
    /// and must never hand it to another thread. Not for product code.
    [[nodiscard]] juce::AudioPluginInstance* spike01LiveInstanceForDiagnostics() const noexcept;

    /// [Message thread] SPIKE-01 ONLY: whether the native plugin editor window is currently open.
    [[nodiscard]] bool spike01IsNativeEditorOpenForDiagnostics() const noexcept
    {
        return editorWindow_ != nullptr;
    }

private:
    mini_daw::AsyncLifetimeOwnerToken asyncLifetime_;

    struct InstrumentOwner
    {
        std::unique_ptr<juce::AudioPluginInstance> inst;
        bool layoutOk = false;
    };

    [[nodiscard]] bool tryPrepareInstrumentLayout(juce::AudioPluginInstance& inst,
                                                  double sampleRate,
                                                  int blockSize);

    struct MidiIoState;
    std::unique_ptr<MidiIoState> midiIo_;

    juce::AudioPluginFormatManager formatManager_;

    juce::AudioBuffer<float> scratch_;
    std::vector<float*> scratchPtrs_;

    /// Transport-driven MIDI for the current audio callback (no lock; audio thread only).
    juce::MidiBuffer rtBlockMidi_;
    std::atomic<std::uint64_t> rtTransportMidiAddEventDiscarded_{ 0 };
    int audioCallbackBlockSamples_ = 0;

    /// Test-only capture sink for the MIDI delivery boundary (null in production).
    std::atomic<MidiDeliveryCaptureSink*> midiCaptureSink_{ nullptr };
    std::atomic<std::uint64_t> rtMidiDeliveryBoundaryBlocks_{ 0 };

    std::atomic<std::shared_ptr<InstrumentOwner>> activeOwner_;

    /// §9.4.2 host-observed identity: relays juce::AudioProcessorListener notifications from the
    /// live instance into the semantic revision. Callbacks may arrive on ANY thread (plugins may
    /// notify from the audio thread) — the relay only performs one relaxed atomic increment.
    struct PrimaryRevisionBumpListener final : juce::AudioProcessorListener
    {
        explicit PrimaryRevisionBumpListener(mini_daw::PrimarySemanticRevision& r) noexcept
            : rev(r)
        {
        }
        void audioProcessorParameterChanged(juce::AudioProcessor*, int, float) override
        {
            (void)rev.bump(); // host-observed parameter change
        }
        void audioProcessorChanged(juce::AudioProcessor*, const ChangeDetails&) override
        {
            // Covers nonParameterStateChanged (setDirty hints), programChanged and
            // latencyChanged — all conservative §9.4 sound-state-change signals.
            (void)rev.bump();
        }
        mini_daw::PrimarySemanticRevision& rev;
    };

    mini_daw::PrimarySemanticRevision primarySemanticRevision_;
    PrimaryRevisionBumpListener primaryRevisionBumpListener_{ primarySemanticRevision_ };

    std::unique_ptr<juce::DocumentWindow> editorWindow_;

    // --- [Audio thread] Instrument routing diagnostics (peek from message thread; no file I/O on audio thread).
    std::atomic<std::uint64_t> rtDiag_skipBadIoArgs_{ 0 };
    std::atomic<std::uint64_t> rtDiag_skipNoOwnerBadLayout_{ 0 };
    std::atomic<std::uint64_t> rtDiag_skipScratchLayout_{ 0 };
    std::atomic<std::uint64_t> rtDiag_skipScratchTooSmallForCallback_{ 0 };
    std::atomic<std::uint64_t> rtDiag_skipNoMidiIo_{ 0 };
    std::atomic<std::uint64_t> rtDiag_processOkBlocks_{ 0 };
    std::atomic<std::int32_t> rtDiag_lastTotalOut_{ 0 };
    std::atomic<std::int32_t> rtDiag_lastMainOut_{ 0 };
    std::atomic<std::int32_t> rtDiag_lastScratchChAllocated_{ 0 };
    std::atomic<std::int32_t> rtDiag_lastProcessRun_{ 0 };
    std::atomic<std::uint32_t> rtDiag_lastScratchPeakBits_{ 0 }; // bitwise float
    std::atomic<std::uint8_t> rtDiag_lastScratchAllZero_{ 1 };

    mutable std::int64_t diagLastRoutingLogBumpMs_{ 0 };

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
