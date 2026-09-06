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

#include <functional>
#include <memory>
#include <vector>

#include "domain/Track.h"

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
