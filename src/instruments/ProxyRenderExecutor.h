#pragma once

// =============================================================================
// ProxyRenderExecutor — the deterministic P1D block loop (worker-thread body)
// (steering docs/PORTABLE_INSTRUMENTS_AND_PROXIES.md §13, §15; SPIKE-02 evidence)
// =============================================================================
// Renders one complete destination from the canonical project-start boundary
// (sample 0) through the final relevant event and its detected tail into a
// temporary 32-bit-float stereo WAV, returning a structured ProxyRenderResult.
//
// THREAD AFFINITY: `renderProxyDestination` runs on ONE dedicated render worker
// which has exclusive ownership of the prepared isolated processor for the whole
// call (the SPIKE-02 measured lifecycle; ProxyRenderInstanceLifecycle.h owns the
// message-thread halves). It never touches the live plugin instance, any Session
// or Track object, routing containers, editors or UI.
//
// The processor type is a template seam so the deterministic selftests exercise
// the complete loop (scheduling, scratch rule, latency preservation, tail policy,
// WAV write/validation, cancellation, failure paths) with lightweight fake
// processors — production instantiates it with juce::AudioProcessor (the
// isolated juce::AudioPluginInstance). Required Proc surface:
//   int getTotalNumInputChannels() / getTotalNumOutputChannels()
//   int getLatencySamples()
//   void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&)
//
// AUDIO BOUNDARY (§5, Locked §9.1): the WAV records ONLY the Primary instrument
// boundary. The scratch buffer spans max(2, totalIn, totalOut) channels (SPIKE-02
// hazard H1: multi-bus instruments crash with a main-pair-only buffer) and the
// WAV reads ONLY channels 0/1 — the main stereo pair — exactly like the live
// ExperimentalInstrumentHost seam (audioThread_processBlockAndAddToOutputs mixes
// scratch channels 0/1 into DAL's stereo bus). A plugin exposing more than two
// output channels therefore reaches the same stereo boundary as live DAL: extra
// bus channels are processed (the plugin sees its full layout) but not recorded.
// DAL Pre/Post inserts, fader, pan and group/master processing are structurally
// absent from this path (they live in the engine mix stage, after this boundary).
//
// LATENCY (§7, PI-014): the plugin's reported latency stays IN the audio — no
// trim, shift or compensation — and the reported sample count is recorded in the
// result. Full PDC remains deferred.

#include "instruments/ProxyOfflineSequencer.h"
#include "instruments/ProxyRenderTypes.h"
#include "instruments/ProxyRenderSnapshot.h"

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_formats/juce_audio_formats.h>

#include <cmath>
#include <cstdint>
#include <vector>

namespace proxy_render
{

//==============================================================================
// Temporary-WAV validation (§8) — reopen and structurally verify the artifact.
//==============================================================================
struct WavValidationOutcome
{
    bool ok = false;
    juce::String error;
    double sampleRate = 0.0;
    unsigned int channels = 0;
    std::int64_t lengthSamples = 0;
    unsigned int bitsPerSample = 0;
    bool isFloat = false;
};

[[nodiscard]] inline WavValidationOutcome validateTemporaryWav(const juce::File& f,
                                                               const double expectedRate,
                                                               const int expectedChannels,
                                                               const std::int64_t expectedLength)
{
    WavValidationOutcome out;
    if (!f.existsAsFile() || f.getSize() <= 0)
    {
        out.error = "file missing or empty";
        return out;
    }
    juce::WavAudioFormat fmt;
    std::unique_ptr<juce::AudioFormatReader> reader(
        fmt.createReaderFor(f.createInputStream().release(), true));
    if (reader == nullptr)
    {
        out.error = "file cannot be reopened as WAV";
        return out;
    }
    out.sampleRate = reader->sampleRate;
    out.channels = reader->numChannels;
    out.lengthSamples = (std::int64_t)reader->lengthInSamples;
    out.bitsPerSample = reader->bitsPerSample;
    out.isFloat = reader->usesFloatingPointData;
    if (reader->sampleRate != expectedRate)
    {
        out.error = "sample rate mismatch";
        return out;
    }
    if ((int)reader->numChannels != expectedChannels)
    {
        out.error = "channel count mismatch";
        return out;
    }
    if (!reader->usesFloatingPointData || reader->bitsPerSample != 32)
    {
        out.error = "not 32-bit float";
        return out;
    }
    if (out.lengthSamples < 0 || out.lengthSamples != expectedLength)
    {
        out.error = "length inconsistent with rendered length";
        return out;
    }
    // Finite-sample sweep (bounded read chunks; the render loop also checks per block).
    juce::AudioBuffer<float> chunk(expectedChannels, 8192);
    for (std::int64_t pos = 0; pos < out.lengthSamples;)
    {
        const int n = (int)juce::jmin<std::int64_t>(chunk.getNumSamples(), out.lengthSamples - pos);
        if (!reader->read(&chunk, 0, n, pos, true, expectedChannels > 1))
        {
            out.error = "read failed during finite-sample sweep";
            return out;
        }
        for (int c = 0; c < expectedChannels; ++c)
        {
            const float* d = chunk.getReadPointer(c);
            for (int i = 0; i < n; ++i)
            {
                if (!std::isfinite(d[i]))
                {
                    out.error = "non-finite sample in artifact";
                    return out;
                }
            }
        }
        pos += n;
    }
    out.ok = true;
    return out;
}

//==============================================================================
// Render configuration handed to the worker (owned copies only)
//==============================================================================
struct ProxyRenderExecutionConfig
{
    double renderSampleRate = 48000.0;
    int blockSize = kRenderBlockSize;
    juce::File temporaryWavFile;      ///< where the temp artifact is written
    juce::String expectedFingerprint; ///< echoed into the result (§8 pairing)
    std::uint64_t primarySemanticRevision = 0;
    /// Diagnostic retention of a tail-limit-failed artifact (§15.2: MAY be retained for
    /// diagnostics, never published). Default false ⇒ failures always delete the temp file.
    bool retainFailedTailArtifactForDiagnostics = false;
};

//==============================================================================
// The block loop
//==============================================================================
/// [Render worker ONLY — exclusive owner of `proc` for the duration of the call.]
/// `proc` must already be restored + prepared (+ reset/flushed) by the message-thread
/// lifecycle. Renders from sample 0 (canonical project-start boundary) and returns a
/// structured result. Never throws; failures come back as status/reason.
template <typename Proc>
[[nodiscard]] ProxyRenderResult renderProxyDestination(Proc& proc,
                                                       const proxy_snapshot::ProxyRenderSnapshot& snapshot,
                                                       const ProxyRenderExecutionConfig& cfg,
                                                       const ProxyRenderCancellationToken& cancel)
{
    ProxyRenderResult r;
    r.expectedFingerprint = cfg.expectedFingerprint;
    r.primarySemanticRevision = cfg.primarySemanticRevision;
    r.renderSampleRate = cfg.renderSampleRate;
    r.blockSize = cfg.blockSize;
    r.channels = kRenderChannels;
    r.workerThreadId = juce::String::toHexString(
        (juce::pointer_sized_int)juce::Thread::getCurrentThreadId());
    const double wallStart = juce::Time::getMillisecondCounterHiRes();

    if (!(cfg.renderSampleRate > 0.0) || cfg.blockSize <= 0)
    {
        r.failureReason = ProxyRenderFailureReason::SnapshotInvalid;
        r.message = "invalid render configuration";
        return r;
    }

    // §6 empty destination: the explicit silent generation (no WAV) is allowed ONLY when the
    // snapshot is eligible under the revision 6 host-event-driven contract. An event-empty
    // snapshot on an unclassified instrument falls through to the normal full render below
    // (span 0 + tail), which honestly captures — or honestly FAILS on — autonomous output.
    if (!snapshot.spanAndSilence.hasHostScheduledEvents
        && snapshot.spanAndSilence.silentGenerationEligible)
    {
        r.status = ProxyRenderStatus::SucceededSilent;
        r.failureReason = ProxyRenderFailureReason::None;
        r.message = "explicit silent generation (empty destination, host-event-driven instrument)";
        r.pluginLatencySamplesAtStart = r.pluginLatencySamplesAtEnd = proc.getLatencySamples();
        r.tailCompleted = true;
        r.wallMs = juce::Time::getMillisecondCounterHiRes() - wallStart;
        return r;
    }

    // Deterministic offline scheduler (live-parity semantics; §10.1 boundary conversion of
    // persisted timeline-reference coordinates to the RENDER rate happens inside).
    ProxyOfflineSequencer sequencer(snapshot, cfg.renderSampleRate);
    r.spanEndRenderSamples = sequencer.lastEventRenderSample();

    // Scratch rule (§5 / SPIKE-02 H1 / steering §15.4): max(2, totalIn, totalOut) channels.
    const int totalCh = juce::jmax(2, juce::jmax(proc.getTotalNumInputChannels(),
                                                 proc.getTotalNumOutputChannels()));
    juce::AudioBuffer<float> scratch(totalCh, cfg.blockSize);
    std::vector<float*> viewChans((size_t)totalCh, nullptr);
    juce::MidiBuffer midi;

    // Temporary WAV writer (32-bit-float stereo at the render rate, §15.5). RAII guard:
    // cancellation/failure paths delete the artifact unless explicitly retained.
    ScopedTempFileGuard tempGuard(cfg.temporaryWavFile);
    std::unique_ptr<juce::AudioFormatWriter> wavWriter;
    {
        const juce::File& f = tempGuard.file();
        if (f == juce::File())
        {
            r.failureReason = ProxyRenderFailureReason::WavWriteFailed;
            r.message = "no temporary WAV path";
            return r;
        }
        (void)f.getParentDirectory().createDirectory();
        (void)f.deleteFile();
        juce::WavAudioFormat fmt;
        if (auto stream = f.createOutputStream())
        {
            wavWriter.reset(fmt.createWriterFor(stream.release(), cfg.renderSampleRate,
                                                (unsigned int)kRenderChannels, 32,
                                                juce::StringPairArray(), 0));
        }
        if (wavWriter == nullptr)
        {
            r.failureReason = ProxyRenderFailureReason::WavWriteFailed;
            r.message = "temporary WAV writer creation failed";
            return r;
        }
    }

    r.pluginLatencySamplesAtStart = proc.getLatencySamples();
    ProxyTailDetector tail(cfg.renderSampleRate);
    const std::int64_t spanEnd = r.spanEndRenderSamples;

    std::int64_t pos = 0;
    bool tailDone = false;
    bool capReached = false;
    bool firstBlock = true;

    while (!tailDone && !capReached)
    {
        // §9 cooperative cancellation at every block boundary (P1E seam): prompt stop,
        // Cancelled (never Failed), temp cleanup via the guard, live plugin untouched.
        if (cancel.isCancelled())
        {
            wavWriter.reset();
            r.status = ProxyRenderStatus::Cancelled;
            r.failureReason = ProxyRenderFailureReason::None;
            r.message = "cancelled at block boundary";
            r.renderedLengthSamples = pos;
            r.wallMs = juce::Time::getMillisecondCounterHiRes() - wallStart;
            return r; // tempGuard deletes the partial artifact
        }

        const int n = cfg.blockSize;
        midi.clear();
        if (firstBlock)
        {
            // §4 validated initial-state sequence tail end: reset/flush prefix + CC chase
            // before the first musical event (restore/prepare/reset ran on the message thread).
            sequencer.emitResetAndChasePrefix(midi);
            firstBlock = false;
        }
        sequencer.emitBlock(pos, n, midi);
        if (!midi.isEmpty())
        {
            ++r.blocksWithMidi;
            for (const auto meta : midi)
            {
                ++r.midi.totalEvents;
                const auto* d = meta.data;
                if (meta.numBytes >= 3)
                {
                    const int status = d[0] & 0xF0;
                    const int ch = d[0] & 0x0F; // 0-based
                    if (status == 0x90 && d[2] > 0)
                    {
                        ++r.midi.noteOnsByChannel[ch];
                    }
                    else if (status == 0x80 || (status == 0x90 && d[2] == 0))
                    {
                        ++r.midi.noteOffsByChannel[ch];
                    }
                    else if (status == 0xB0)
                    {
                        ++r.midi.ccByController[d[1] & 0x7F];
                    }
                }
            }
        }

        for (int c = 0; c < totalCh; ++c)
        {
            viewChans[(size_t)c] = scratch.getWritePointer(c);
        }
        juce::AudioBuffer<float> view(viewChans.data(), totalCh, n);
        view.clear();
        proc.processBlock(view, midi);
        ++r.blocksProcessed;

        // Levels + finiteness on the recorded stereo boundary (channels 0/1 only).
        double blockPeak = 0.0;
        for (int c = 0; c < kRenderChannels; ++c)
        {
            const float* d = view.getReadPointer(c);
            for (int i = 0; i < n; ++i)
            {
                const float v = d[i];
                if (!std::isfinite(v))
                {
                    r.allFinite = false;
                    continue;
                }
                blockPeak = juce::jmax(blockPeak, std::abs((double)v));
            }
        }
        if (!r.allFinite)
        {
            wavWriter.reset();
            r.failureReason = ProxyRenderFailureReason::NonFiniteAudio;
            r.message = "isolated instance produced non-finite samples";
            r.renderedLengthSamples = pos;
            r.wallMs = juce::Time::getMillisecondCounterHiRes() - wallStart;
            return r;
        }
        r.maxPeakLinear = juce::jmax(r.maxPeakLinear, blockPeak);

        // Main stereo pair → temp WAV.
        {
            float* stereo[2] = { viewChans[0], viewChans[1] };
            juce::AudioBuffer<float> stereoView(stereo, kRenderChannels, n);
            if (!wavWriter->writeFromAudioSampleBuffer(stereoView, 0, n))
            {
                wavWriter.reset();
                r.failureReason = ProxyRenderFailureReason::WavWriteFailed;
                r.message = "temporary WAV write failed";
                r.renderedLengthSamples = pos;
                r.wallMs = juce::Time::getMillisecondCounterHiRes() - wallStart;
                return r;
            }
        }

        pos += n;

        // §6 tail phase: after the final relevant event, feed the detector. The whole block
        // enters the tail once pos passed spanEnd (block granularity — the detector windows
        // are far larger than one 512-sample block).
        if (pos > spanEnd)
        {
            switch (tail.feedBlock(blockPeak, n))
            {
                case ProxyTailDetector::Verdict::Continue: break;
                case ProxyTailDetector::Verdict::TailComplete: tailDone = true; break;
                case ProxyTailDetector::Verdict::CapReached: capReached = true; break;
            }
        }
    }

    wavWriter.reset(); // flush + close before validation
    r.renderedLengthSamples = pos;
    r.tailLengthSamples = tail.tailSamplesConsumed();
    r.pluginLatencySamplesAtEnd = proc.getLatencySamples();
    r.wallMs = juce::Time::getMillisecondCounterHiRes() - wallStart;

    if (capReached)
    {
        // §15.2 Locked: reaching the cap with material output is a diagnosed INCOMPLETE render.
        r.status = ProxyRenderStatus::Failed;
        r.failureReason = ProxyRenderFailureReason::TailLimitReached;
        r.message = "tail limit reached — render incomplete";
        r.tailCompleted = false;
        if (cfg.retainFailedTailArtifactForDiagnostics)
        {
            r.temporaryWavFile = tempGuard.release(); // explicitly diagnostic artifact only
            r.wavBytes = r.temporaryWavFile.getSize();
        }
        return r;
    }
    r.tailCompleted = true;

    // §8 validation before returning success.
    const WavValidationOutcome v = validateTemporaryWav(tempGuard.file(), cfg.renderSampleRate,
                                                        kRenderChannels, r.renderedLengthSamples);
    if (!v.ok)
    {
        r.status = ProxyRenderStatus::Failed;
        r.failureReason = ProxyRenderFailureReason::WavValidationFailed;
        r.message = "WAV validation failed: " + v.error;
        return r;
    }

    r.status = ProxyRenderStatus::Succeeded;
    r.failureReason = ProxyRenderFailureReason::None;
    r.temporaryWavFile = tempGuard.release();
    r.wavBytes = r.temporaryWavFile.getSize();
    return r;
}

} // namespace proxy_render
