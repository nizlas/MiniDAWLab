#pragma once

// ============================================================================
// SPIKE-01 DIAGNOSTIC SCAFFOLDING — NOT PRODUCT CODE.
//
// Manual measurement panel for SPIKE-01 "Authoritative Plugin-State Capture"
// (roadmap slice P0/P1A; canonical steering docs/PORTABLE_INSTRUMENTS_AND_PROXIES.md
// §9.2 / PID-001). Launched ONLY by the hidden `--spike01-state-capture`
// command-line flag; nothing in the product path creates or references this
// window otherwise.
//
// What it does (all on the message thread, operator-driven, no polling):
//   * timed `getStateInformation` captures on the selected instrument track,
//     labelled with a measurement phase (editor open/closed, transport
//     playing/stopped, after GUI edits, …);
//   * timed captures through the production Save path
//     (`ExperimentalInstrumentHost::getCurrentInstrumentStateBase64`) so Save
//     agreement can be verified byte-for-byte (by hash);
//   * optional `juce::AudioProcessorListener` attachment to the live instance
//     to count/inspect host parameter notifications (kind, index, thread);
//   * a sanitized markdown report (sizes, SHA-256 hashes, timings,
//     notification metadata — NEVER raw state bytes) written to the Desktop.
//
// PRIVACY: raw state bytes live only in short-lived stack buffers inside the
// capture functions and are hashed and discarded. See Spike01ReportFormat.h.
//
// Removal: delete src/diagnostics/Spike01*.*, the two `spike01*ForDiagnostics`
// accessors in ExperimentalInstrumentHost, and the flag-gated launch hook.
// ============================================================================

#include <JuceHeader.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "domain/Track.h"
#include "instruments/ProxyRenderScheduler.h"     // narrow P1E status vocabulary (by value)
#include "instruments/ProxyUpdatePolicyService.h" // narrow P1H policy vocabulary (by value)
#include "io/ProjectFile.h"                       // ProjectFileProxyMetadataV20 (verification copy)

class ExperimentalInstrumentHost;

/// One selectable instrument runtime (an instrument track with its host).
struct Spike01RuntimeChoice
{
    TrackId trackId = kInvalidTrackId;
    juce::String label;
};

/// Decoupling seam: the panel never touches coordinators/session directly.
struct Spike01PanelCallbacks
{
    /// [Message thread] All current instrument runtimes (track id + display label).
    std::function<std::vector<Spike01RuntimeChoice>()> listInstrumentRuntimes;
    /// [Message thread] Fresh host lookup per action; never cached across callbacks.
    std::function<ExperimentalInstrumentHost*(TrackId)> resolveHostForTrack;
    /// [Message thread] Whether the transport is currently playing (for sample labels).
    std::function<bool()> isTransportPlaying;
    /// [Message thread; SPIKE-01B-M auto mode only] Start/stop playback through the same
    /// controller the transport UI uses. May be null (auto plans then fail their step).
    std::function<void()> startTransport;
    std::function<void()> stopTransport;
    /// [Message thread; SPIKE-01B-M M2V only] Seek the transport playhead to a sample index
    /// (through `Transport::requestSeek`), and read the cycle wrap count. May be null.
    std::function<void(std::int64_t)> seekTransport;
    std::function<std::uint32_t()> readCycleWrapCount;
    /// [Message thread; P1EF integration plan only] The NARROW production service API — the
    /// panel never owns jobs or touches plugin instances (P1E: job ownership lives in the
    /// application-owned ProxyRenderScheduler; no instances cross this seam).
    std::function<proxy_render::ProxyJobStatus(TrackId)> requestProxyRender;
    std::function<proxy_render::ProxyJobStatus(TrackId)> queryProxyJobStatus;
    std::function<proxy_render::ProxyDestinationState(TrackId)> queryProxyDestinationState;
    /// [Message thread] Copy of the destination's published v20 proxy metadata (false = none).
    std::function<bool(TrackId, ProjectFileProxyMetadataV20&)> getPublishedProxyMetadata;
    /// [Message thread] Current project folder (invalid File when the project is unsaved).
    std::function<juce::File()> getProjectFolder;

    // --- P1G proxy-playback end-to-end integration plan only (plan "P1G") ---
    /// Simulate Primary unavailable WITHOUT unloading the plugin (coordinator test
    /// hook + destination refresh; the expected musical identity stays intact).
    std::function<void(TrackId, bool)> setProxyPrimaryForcedUnavailable;
    /// (int)proxy_playback::ProxyPlaybackSourceState; -1 when the coordinator is absent.
    std::function<int(TrackId)> queryProxyPlaybackRuntimeState;
    /// True when the coordinator's published view for the track selects Proxy.
    std::function<bool(TrackId)> isProxyViewSelected;
    /// Cumulative proxy-reader underrun count for the track's published view
    /// (-1 when no reader). A normally PREPARED loop wrap must not increase it.
    std::function<std::int64_t(TrackId)> queryProxyReaderUnderrunCount;
    /// Save through the normal ProjectIoCoordinator path (persists the save pairing).
    std::function<bool()> saveProjectNow;
    /// Blocking offline WAV mixdown through the real exporter (device rate, overwrite ok).
    std::function<juce::Result(juce::File)> runOfflineMixdownWav;
    /// Track mute through the normal Session seam (shared strip evidence).
    std::function<void(TrackId, bool)> setTrackMuted;
    /// All Audio/Instrument track ids (mixdown isolation: mute everything else).
    std::function<std::vector<TrackId>()> listSoundProducingTracks;
    /// Real render-relevant musical edit (appends a 1-note clip) + destination refresh.
    std::function<bool(TrackId)> appendStaleTestClip;
    std::function<double()> getEngineSampleRate;
    /// Attempt a device sample-rate switch; false when the device rejects the rate.
    std::function<bool(double)> trySetEngineSampleRate;

    // --- P1H update-policy end-to-end integration plan only (plan "P1H") ---
    /// Advance the policy service's injectable clock by `ms` and tick it once
    /// (deterministic five-minute boundary without waiting; §18.1).
    std::function<void(double)> advanceProxyPolicyClockMs;
    /// Immutable P1H policy status (mode + runtime state + idle countdown).
    std::function<proxy_policy::ProxyPolicyStatus(TrackId)> queryProxyPolicyStatus;
    /// Persist a new update mode through the SAME seam the Inspector uses
    /// (0 Auto / 1 On Save / 2 Manual / 3 Off); returns true when it changed.
    std::function<bool(TrackId, int)> setProxyUpdateMode;
    /// Policy-service actions (obey mode rules: Render now = Manual only, …).
    std::function<bool(TrackId)> policyRenderNow;
    std::function<void(TrackId)> policyCancel;
    /// Real autosave checkpoint (dirty policy applies). MUST NOT trigger renders.
    std::function<bool()> forceAutosaveNow;
    /// Reload a project through the normal ProjectIoCoordinator pipeline.
    std::function<void(juce::File)> loadProjectNow;
    /// The current project FILE (invalid when unsaved).
    std::function<juce::File()> getProjectFile;
    juce::String appVersion;
};

class Spike01StateCapturePanel final : public juce::DocumentWindow
{
public:
    /// `autoPlanId` (SPIKE-01B-M, `--spike01-auto=<id>`): when non-empty, the panel runs the
    /// named scripted measurement plan unattended (timer-driven on the message thread) and
    /// writes the sanitized report automatically. Empty = normal operator-driven panel.
    explicit Spike01StateCapturePanel(Spike01PanelCallbacks callbacks,
                                      juce::String autoPlanId = {});
    ~Spike01StateCapturePanel() override;

    void closeButtonPressed() override;

private:
    class Content;
    Content* content_ = nullptr; // owned via setContentOwned

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(Spike01StateCapturePanel)
};
