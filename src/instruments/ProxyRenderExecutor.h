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
//
// READINESS (§14.2 step 4b, 1.1.14): before the block loop the prepared instance
// is asked to answer the content's own distinct notes until its response has
// settled (`verifyInstrumentReadiness`); an instrument that is still loading
// its content asynchronously after prepareToPlay therefore no longer renders
// silence into the asset. A render whose scheduled notes produced nothing above
// the tail threshold fails with NoAudibleOutput instead of publishing silence.

#include "instruments/ProxyOfflineSequencer.h"
#include "instruments/ProxyRenderTypes.h"
#include "instruments/ProxyRenderSnapshot.h"

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_formats/juce_audio_formats.h>

#include <algorithm>
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
    /// P1E recording-pause gate (§14.3 resource policy): called once per block, before
    /// processing. Production blocks inside it while recording is active (and returns promptly
    /// on cancellation); null = never pause. Never affects correctness, only pacing.
    std::function<void()> blockBoundaryPauseGate;
    /// Diagnostic retention of a tail-limit-failed artifact (§15.2: MAY be retained for
    /// diagnostics, never published). Default false ⇒ failures always delete the temp file.
    bool retainFailedTailArtifactForDiagnostics = false;
    /// P1I live progress (PI-013): called once per block with the milliseconds of
    /// destination material rendered so far. Must be cheap (production stores to a
    /// scheduler-owned atomic); null = no progress reporting.
    std::function<void(std::int64_t renderedMs)> progressSink;
    /// Instrument readiness verification before the block loop (ProxyRenderTypes.h).
    ProxyReadinessPolicy readiness;
    /// DIAGNOSTICS ONLY (probe): judge the tail on the raw peak (tail policy v1 behaviour).
    bool diagnosticAbsolutePeakTail = false;
};

//==============================================================================
// Readiness verification (§14.2 step 4b) — worker thread, exclusive instance
//==============================================================================
/// Plays the content's distinct notes into the prepared instance until its response has
/// settled (see ProxyReadinessPolicy), then lets it settle / flushes it. Returns false ONLY
/// when cancelled (the caller returns Cancelled); every other outcome is recorded in `outcome`
/// and the render proceeds. Processes blocks into `scratch` (never written to the WAV).
/// All level decisions use the tail policy's DC-tracking meter (`DcTrackingPeakMeter`, same
/// time constant as the render's detector): a plug-in that parks an offset after playing
/// (VB3-II: −0.605) neither masks later notes nor prevents the settle before the render.
template <typename Proc>
[[nodiscard]] bool verifyInstrumentReadiness(Proc& proc,
                                             const ProxyOfflineSequencer& sequencer,
                                             const ProxyRenderExecutionConfig& cfg,
                                             const ProxyRenderCancellationToken& cancel,
                                             juce::AudioBuffer<float>& scratch,
                                             ProxyReadinessOutcome& outcome)
{
    const ProxyReadinessPolicy& pol = cfg.readiness;
    outcome = ProxyReadinessOutcome{};
    const auto stimulus = sequencer.collectDistinctNoteOns((std::size_t)juce::jmax(1, pol.maxStimulusNotes));
    outcome.stimulusNotes = (int)stimulus.size();
    if (!pol.enabled || stimulus.empty())
    {
        outcome.note = pol.enabled ? "skipped (content schedules no notes)" : "disabled";
        return true;
    }
    outcome.attempted = true;
    const double wallStart = juce::Time::getMillisecondCounterHiRes();
    const double floorLinear = dbToLinear(pol.floorDb);
    const double tailLinear = dbToLinear(kTailThresholdDb);
    const int n = cfg.blockSize;
    const int stimulusBlocks = juce::jmax(1, (int)std::ceil(pol.stimulusSeconds * cfg.renderSampleRate / (double)n));
    const int settleBlocks = juce::jmax(1, (int)std::ceil(pol.settleSeconds * cfg.renderSampleRate / (double)n));
    const int totalCh = scratch.getNumChannels();
    std::vector<float*> chans((size_t)totalCh, nullptr);
    juce::MidiBuffer midi;
    DcTrackingPeakMeter meter(cfg.renderSampleRate);
    double lastBlockPeak = 0.0;   ///< raw |x| (diagnostics only)
    double lastBlockPeakAc = 0.0; ///< DC-tracked residual (every decision below)

    const auto processOne = [&](juce::MidiBuffer& m) -> double {
        for (int c = 0; c < totalCh; ++c)
        {
            chans[(size_t)c] = scratch.getWritePointer(c);
        }
        juce::AudioBuffer<float> view(chans.data(), totalCh, n);
        view.clear();
        proc.processBlock(view, m);
        ++outcome.blocksProcessed;
        const float* const stereo[2] = { chans[0], chans[1] };
        const DcTrackingPeakMeter::BlockReading reading = meter.feedBlock(stereo, kRenderChannels, n);
        lastBlockPeak = reading.rawPeak;
        lastBlockPeakAc = reading.residualPeak;
        return lastBlockPeakAc;
    };
    const auto elapsedMs = [&] { return juce::Time::getMillisecondCounterHiRes() - wallStart; };
    /// Sleep in cancellable slices; false = cancelled.
    const auto waitMs = [&](const int ms) -> bool {
        const double until = juce::Time::getMillisecondCounterHiRes() + (double)ms;
        while (juce::Time::getMillisecondCounterHiRes() < until)
        {
            if (cancel.isCancelled())
            {
                return false;
            }
            juce::Thread::sleep(juce::jmin(50, ms));
        }
        return true;
    };

    std::vector<bool> everSounded(stimulus.size(), false);
    int passesWithoutNewNote = 0;
    bool firstPass = true;
    double lastSoundMs = -1.0;
    for (;;)
    {
        if (cfg.blockBoundaryPauseGate)
        {
            cfg.blockBoundaryPauseGate();
        }
        ++outcome.passes;
        int newNotes = 0;
        int soundingThisPass = 0;
        for (std::size_t i = 0; i < stimulus.size(); ++i)
        {
            const auto& s = stimulus[i];
            // Let the previous stimulus settle (AC content under the tail threshold; bounded), so
            // the rise criterion below judges THIS note and not what is still ringing.
            for (int b = 0; b < settleBlocks && lastBlockPeakAc >= tailLinear; ++b)
            {
                midi.clear();
                (void)processOne(midi);
                if (cancel.isCancelled())
                {
                    return false;
                }
            }
            if (firstPass && i == 0)
            {
                // The content's initial controller state (reset prefix + first CC / wheel values)
                // in a block of its OWN: an instrument that applies All Sound Off / Reset All
                // Controllers at block granularity must never see them beside a Note On (measured:
                // VB3-II then loses the key and the stimulus note never releases).
                midi.clear();
                sequencer.emitInitialControllerState(midi);
                (void)processOne(midi);
                if (cancel.isCancelled())
                {
                    return false;
                }
            }
            midi.clear();
            const double residual = lastBlockPeakAc;
            midi.addEvent(juce::MidiMessage::noteOn(s.midiChannel, s.midiNote, (float)s.velocity / 127.0f), 0);
            double maxPeak = 0.0;
            for (int b = 0; b < stimulusBlocks; ++b)
            {
                (void)processOne(midi);
                maxPeak = juce::jmax(maxPeak, lastBlockPeakAc);
                midi.clear();
                if (cancel.isCancelled())
                {
                    return false;
                }
            }
            // Sounding = AC content above the floor AND a clear rise over whatever was still ringing.
            const bool sounding = maxPeak > floorLinear && maxPeak > residual * 4.0;
            if (outcome.trace.length() < 4000)
            {
                outcome.trace << (i == 0 ? "p" + juce::String(outcome.passes) + ":" : juce::String(" "))
                              << " ch" << s.midiChannel << "n" << s.midiNote << "="
                              << juce::String(residual > 0.0 ? 20.0 * std::log10(residual) : -200.0, 0) << "->"
                              << juce::String(maxPeak > 0.0 ? 20.0 * std::log10(maxPeak) : -200.0, 0) << (sounding ? "*" : "");
                if (i + 1 == stimulus.size())
                {
                    outcome.trace << "\n";
                }
            }
            if (sounding)
            {
                ++soundingThisPass;
                if (!everSounded[i])
                {
                    everSounded[i] = true;
                    ++newNotes;
                }
            }
            // Release the stimulus note exactly like the schedule releases notes: a Note Off with
            // the default release velocity, nothing else. (Measured: VB3-II freezes a releasing
            // voice at its current level when All Sound Off arrives during the release — the
            // controller flush below therefore only ever follows a settled, silent instrument.)
            midi.clear();
            midi.addEvent(juce::MidiMessage::noteOff(s.midiChannel, s.midiNote, (juce::uint8)64), 0);
            (void)processOne(midi);
            if (cancel.isCancelled())
            {
                return false;
            }
        }
        firstPass = false;
        outcome.soundingNotes = soundingThisPass;
        if (soundingThisPass > 0)
        {
            lastSoundMs = elapsedMs();
        }
        passesWithoutNewNote = newNotes == 0 ? passesWithoutNewNote + 1 : 0;

        const int everCount = (int)std::count(everSounded.begin(), everSounded.end(), true);
        if (everCount > 0 && passesWithoutNewNote >= pol.settledPasses)
        {
            outcome.verified = true;
            outcome.note = "verified: " + juce::String(everCount) + "/" + juce::String((int)stimulus.size())
                           + " notes answer, settled after " + juce::String(outcome.passes) + " passes ("
                           + juce::String(elapsedMs(), 0) + " ms)";
            break;
        }
        if (everCount == 0 && elapsedMs() >= (double)pol.maxSilentWaitMs)
        {
            outcome.note = "no note produced output within " + juce::String(elapsedMs(), 0) + " ms ("
                           + juce::String(outcome.passes) + " passes) - rendering unverified";
            break;
        }
        if (elapsedMs() >= (double)pol.maxTotalWaitMs)
        {
            outcome.note = "response still changing after " + juce::String(elapsedMs(), 0) + " ms ("
                           + juce::String(everCount) + "/" + juce::String((int)stimulus.size())
                           + " notes answer) - rendering unverified";
            break;
        }
        if (!waitMs(pol.passIntervalMs))
        {
            return false;
        }
    }

    // Flush: every stimulus note has been released; let the instrument settle — DC-tracked
    // residual under the tail threshold for a full silence window, exactly the render's own tail
    // criterion (bounded). Only if it does NOT settle, All Sound Off / All Notes Off / sustain
    // off are sent as a last resort and the settle is retried briefly. The render's own reset
    // prefix follows in its first block, on an instrument without ringing content (a parked
    // offset may remain — it is the instrument's state, judged by the same policy as the asset).
    {
        const std::int64_t windowBlocks = juce::jmax<std::int64_t>(1, (std::int64_t)std::ceil(kTailSilenceWindowSec * cfg.renderSampleRate / (double)n));
        const auto settleUntilQuiet = [&](const double seconds) -> bool {
            const std::int64_t maxBlocks = juce::jmax<std::int64_t>(1, (std::int64_t)std::ceil(seconds * cfg.renderSampleRate / (double)n));
            std::int64_t quietRun = 0;
            for (std::int64_t b = 0; b < maxBlocks; ++b)
            {
                midi.clear();
                const double peak = processOne(midi);
                if (cancel.isCancelled())
                {
                    return false;
                }
                quietRun = peak < tailLinear ? quietRun + 1 : 0;
                if (quietRun >= windowBlocks)
                {
                    return true;
                }
            }
            return false;
        };
        outcome.flushReachedSilence = settleUntilQuiet(pol.flushMaxSeconds);
        if (cancel.isCancelled())
        {
            return false;
        }
        if (!outcome.flushReachedSilence)
        {
            midi.clear();
            sequencer.emitAllSoundOff(midi);
            (void)processOne(midi);
            outcome.flushReachedSilence = settleUntilQuiet(2.0);
            if (cancel.isCancelled())
            {
                return false;
            }
        }
        outcome.flushResidualDb = lastBlockPeakAc > 0.0 ? 20.0 * std::log10(lastBlockPeakAc) : -200.0;
        outcome.flushRawPeakDb = lastBlockPeak > 0.0 ? 20.0 * std::log10(lastBlockPeak) : -200.0;
        outcome.parkedDcAtRenderStart[0] = meter.dcEstimate(0);
        outcome.parkedDcAtRenderStart[1] = meter.dcEstimate(1);
    }
    outcome.waitMs = elapsedMs();
    return true;
}

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

    // §14.2 step 4b: the instance answers its own content before the asset is rendered (an
    // instrument still loading after prepare would otherwise render silence into the asset).
    if (!verifyInstrumentReadiness(proc, sequencer, cfg, cancel, scratch, r.readiness))
    {
        wavWriter.reset();
        r.status = ProxyRenderStatus::Cancelled;
        r.failureReason = ProxyRenderFailureReason::None;
        r.message = "cancelled during readiness verification";
        r.wallMs = juce::Time::getMillisecondCounterHiRes() - wallStart;
        return r; // tempGuard deletes the (empty) artifact
    }

    ProxyTailDetector tail(cfg.renderSampleRate);
    tail.setJudgeRawPeakForDiagnostics(cfg.diagnosticAbsolutePeakTail);
    if (r.readiness.attempted)
    {
        // The instrument's offset as the verification left it is a KNOWN state, not content: the
        // detector starts from it instead of letting the asset's first block look like a step.
        tail.seedDcEstimate(r.readiness.parkedDcAtRenderStart[0], r.readiness.parkedDcAtRenderStart[1]);
    }
    const std::int64_t spanEnd = r.spanEndRenderSamples;

    std::int64_t pos = 0;
    bool tailDone = false;
    bool capReached = false;
    bool firstBlock = true;

    while (!tailDone && !capReached)
    {
        // P1E recording pause (resource policy): hold progress at the block boundary.
        if (cfg.blockBoundaryPauseGate)
        {
            cfg.blockBoundaryPauseGate();
        }
        // P1I live progress (PI-013): rendered-material milliseconds so far.
        if (cfg.progressSink)
        {
            cfg.progressSink((std::int64_t)(1000.0 * (double)pos / cfg.renderSampleRate));
        }
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

        // Finiteness on the recorded stereo boundary (channels 0/1 only).
        for (int c = 0; c < kRenderChannels; ++c)
        {
            const float* d = view.getReadPointer(c);
            for (int i = 0; i < n; ++i)
            {
                if (!std::isfinite(d[i]))
                {
                    r.allFinite = false;
                }
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
        if (pos == 0 && n > 0)
        {
            r.firstSample[0] = view.getSample(0, 0);
            r.firstSample[1] = view.getSample(1, 0);
        }
        r.lastSample[0] = view.getSample(0, n - 1);
        r.lastSample[1] = view.getSample(1, n - 1);

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

        // Tail policy v2: the detector observes EVERY block (its offset estimate must track the
        // music), and decides only in the tail phase — the whole block enters the tail once pos
        // passed spanEnd (block granularity; the window is far larger than one block).
        {
            const float* const stereoConst[2] = { viewChans[0], viewChans[1] };
            const bool inTail = pos > spanEnd;
            const ProxyTailDetector::Verdict verdict = tail.feedBlock(stereoConst, kRenderChannels, n, inTail);
            r.maxPeakLinear = juce::jmax(r.maxPeakLinear, tail.lastReading().rawPeak);
            r.maxResidualPeakLinear = juce::jmax(r.maxResidualPeakLinear, tail.lastReading().residualPeak);
            if (inTail)
            {
                switch (verdict)
                {
                    case ProxyTailDetector::Verdict::Continue: break;
                    case ProxyTailDetector::Verdict::TailComplete: tailDone = true; break;
                    case ProxyTailDetector::Verdict::CapReached: capReached = true; break;
                }
            }
        }
    }

    wavWriter.reset(); // flush + close before validation
    r.renderedLengthSamples = pos;
    r.tailLengthSamples = tail.tailSamplesConsumed();
    r.dcEstimateAtEnd[0] = tail.dcEstimate(0);
    r.dcEstimateAtEnd[1] = tail.dcEstimate(1);
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

    // Plausibility (§14.2 step 8): notes were scheduled but the instrument never produced
    // material output — judged on the DC-TRACKED residual over the RENDER's own blocks (the
    // readiness stimulus is measured separately and never counts), so an asset carrying only a
    // parked offset is as implausible as digital silence. Not ready, muted, or content it cannot
    // sound: an incomplete render dressed as Current — it fails here instead (§15.7 keeps the
    // explicit silent generation for EMPTY destinations only). A changing offset produces
    // transients that are real, audible output and therefore count.
    {
        std::int64_t noteOns = 0;
        for (const auto count : r.midi.noteOnsByChannel)
        {
            noteOns += count;
        }
        if (noteOns > 0 && r.maxResidualPeakLinear < dbToLinear(kTailThresholdDb))
        {
            r.status = ProxyRenderStatus::Failed;
            r.failureReason = ProxyRenderFailureReason::NoAudibleOutput;
            r.message = "the instrument produced no audible output for its " + juce::String(noteOns)
                        + " scheduled notes (raw peak " + juce::String(r.maxPeakLinear > 0.0 ? 20.0 * std::log10(r.maxPeakLinear) : -200.0, 1)
                        + " dBFS, DC-tracked residual " + juce::String(r.maxResidualPeakLinear > 0.0 ? 20.0 * std::log10(r.maxResidualPeakLinear) : -200.0, 1)
                        + " dBFS; readiness: " + r.readiness.note + ") - render incomplete, not published";
            if (cfg.retainFailedTailArtifactForDiagnostics)
            {
                r.temporaryWavFile = tempGuard.release();
                r.wavBytes = r.temporaryWavFile.getSize();
            }
            return r;
        }
    }

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
