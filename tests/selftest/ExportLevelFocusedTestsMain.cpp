// =============================================================================
// ExportLevelFocusedTests — export level / click diagnosis + level-meter regression (production code)
// =============================================================================
//
// User report (1.1.7): "MP3 clicks at start and end, MP3 (and WAV) sound distorted, playback in
// DAL is fine, lowering the Stereo Out fader seemed to help."
//
// Two jobs:
//   --analyze <dir>  Offline analysis of the files `MiniDAWLab.exe --stability-export-levels` left
//                    in `%TEMP%\dal-export-levels`: every WAV (JUCE reader, float) and every MP3
//                    (JUCE's MP3 decoder, decoded to float so decoder clipping cannot hide peaks).
//                    Per channel: sample peak, RMS, samples above full scale, NaN/Inf, first/last
//                    samples, largest sample-to-sample step at both file edges, and a 4x
//                    oversampled peak estimate (an inter-sample peak *estimate*, labelled as such —
//                    not a certified true-peak meter). MP3s are aligned to their float WAV by
//                    cross-correlation and compared (level + edge transients).
//   (default)        Deterministic regression: the production engine's realtime callback (stub
//                    device) vs `renderOfflineMixdownBlock` on a tone session (fader / pan / master
//                    / mute), the level-meter accumulator, the export level report, fader dB
//                    mapping and the −∞ / 0 / +6 dB round trip through the project file.
//
// Exit 0 = all checks green (analysis mode always exits 0; it reports, it does not judge).
// =============================================================================

#include "domain/AudioClip.h"
#include "domain/PlacedClip.h"
#include "domain/Session.h"
#include "domain/SessionSnapshot.h"
#include "domain/Track.h"
#include "domain/TrackStereoPan.h"
#include "engine/LevelMeterAccumulator.h"
#include "engine/PlaybackEngine.h"
#include "io/MonoWavFileWriter.h"
#include "transport/Transport.h"
#include "ui/ChannelFaderScale.h"

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>

namespace
{
int failures = 0;
int checks = 0;

void expect(const bool condition, const juce::String& label)
{
    ++checks;
    std::printf("[%s] %s\n", condition ? "PASS" : "FAIL", label.toRawUTF8());
    std::fflush(stdout);
    if (!condition)
    {
        ++failures;
    }
}

void info(const juce::String& text)
{
    std::printf("       %s\n", text.toRawUTF8());
    std::fflush(stdout);
}

constexpr double kRate = 48000.0;
constexpr int kBlock = 512;

[[nodiscard]] juce::String dbfs(const double linear)
{
    if (!std::isfinite(linear))
    {
        return "NaN";
    }
    if (linear <= 1.0e-7)
    {
        return "-inf";
    }
    const double db = 20.0 * std::log10(linear);
    return (db > 0.0 ? "+" : "") + juce::String(db, 2);
}

// ---------------------------------------------------------------------------------------------
// Analysis
// ---------------------------------------------------------------------------------------------
struct ChannelStats
{
    double peak = 0.0;
    double rms = 0.0;
    std::int64_t overs = 0;
    std::int64_t nonFinite = 0;
    double oversampledPeak = 0.0; ///< 4x windowed-sinc oversampling (inter-sample peak ESTIMATE)
    float first = 0.0f;
    float last = 0.0f;
    double maxStepHead = 0.0; ///< largest |x[i]-x[i-1]| within the first 2048 samples (incl. from 0)
    double maxStepTail = 0.0; ///< same within the last 2048 samples (incl. to 0)
    std::int64_t argmaxStepHead = -1;
    std::int64_t argmaxStepTail = -1;
    double dcHead = 0.0; ///< mean of the first 2048 samples
    double dcTail = 0.0;
    double dc = 0.0; ///< mean over the whole file
};

struct FileStats
{
    juce::String name;
    double sampleRate = 0.0;
    std::int64_t frames = 0;
    int channels = 0;
    int bits = 0;
    bool isFloat = false;
    std::vector<ChannelStats> ch;
    juce::AudioBuffer<float> audio; ///< kept for cross-file comparison
};

[[nodiscard]] double oversampledPeakEstimate(const float* x, const std::int64_t n)
{
    // 4x oversampling with a 48-tap windowed-sinc (Blackman) polyphase interpolator. Good enough to
    // reveal inter-sample overs on hot material; not a BS.1770 true-peak implementation.
    constexpr int kOs = 4;
    constexpr int kHalf = 12; // taps per side per phase
    double peak = 0.0;
    std::vector<double> taps[kOs];
    for (int p = 0; p < kOs; ++p)
    {
        taps[p].resize(2 * kHalf);
        double sum = 0.0;
        for (int k = -kHalf; k < kHalf; ++k)
        {
            const double t = (double)k - (double)p / kOs; // fractional offset
            const double sinc = std::fabs(t) < 1.0e-9 ? 1.0 : std::sin(juce::MathConstants<double>::pi * t) / (juce::MathConstants<double>::pi * t);
            const double w = 0.42 - 0.5 * std::cos(2.0 * juce::MathConstants<double>::pi * (t + kHalf) / (2.0 * kHalf))
                             + 0.08 * std::cos(4.0 * juce::MathConstants<double>::pi * (t + kHalf) / (2.0 * kHalf));
            taps[p][(size_t)(k + kHalf)] = sinc * juce::jmax(0.0, w);
            sum += taps[p][(size_t)(k + kHalf)];
        }
        for (auto& v : taps[p])
        {
            v /= sum;
        }
    }
    for (std::int64_t i = 0; i < n; ++i)
    {
        for (int p = 0; p < kOs; ++p)
        {
            double acc = 0.0;
            for (int k = -kHalf; k < kHalf; ++k)
            {
                const std::int64_t idx = i + k;
                if (idx >= 0 && idx < n)
                {
                    acc += taps[p][(size_t)(k + kHalf)] * (double)x[idx];
                }
            }
            peak = std::max(peak, std::fabs(acc));
        }
    }
    return peak;
}

[[nodiscard]] ChannelStats analyzeChannel(const float* x, const std::int64_t n)
{
    ChannelStats s;
    if (n <= 0)
    {
        return s;
    }
    double sumSq = 0.0;
    double sum = 0.0;
    for (std::int64_t i = 0; i < n; ++i)
    {
        const float v = x[i];
        if (!std::isfinite(v))
        {
            ++s.nonFinite;
            continue;
        }
        const double a = std::fabs((double)v);
        s.peak = std::max(s.peak, a);
        if (a > 1.0)
        {
            ++s.overs;
        }
        sumSq += (double)v * (double)v;
        sum += (double)v;
    }
    s.rms = std::sqrt(sumSq / (double)n);
    s.dc = sum / (double)n;
    s.first = x[0];
    s.last = x[n - 1];
    const std::int64_t edge = std::min<std::int64_t>(2048, n);
    double prev = 0.0; // the file "starts from silence": the step from 0 to x[0] counts
    for (std::int64_t i = 0; i < edge; ++i)
    {
        const double d = std::fabs((double)x[i] - prev);
        if (d > s.maxStepHead)
        {
            s.maxStepHead = d;
            s.argmaxStepHead = i;
        }
        s.dcHead += (double)x[i];
        prev = (double)x[i];
    }
    s.dcHead /= (double)edge;
    for (std::int64_t i = n - edge; i < n; ++i)
    {
        const double next = (i + 1 < n) ? (double)x[i + 1] : 0.0; // the player goes to silence after the end
        const double d = std::fabs(next - (double)x[i]);
        if (d > s.maxStepTail)
        {
            s.maxStepTail = d;
            s.argmaxStepTail = i;
        }
        s.dcTail += (double)x[i];
    }
    s.dcTail /= (double)edge;
    s.oversampledPeak = oversampledPeakEstimate(x, n);
    return s;
}

[[nodiscard]] bool readAudioFile(const juce::File& f, FileStats& out)
{
    juce::AudioFormatManager fm;
    fm.registerFormat(new juce::WavAudioFormat(), true);
#if JUCE_USE_MP3AUDIOFORMAT
    fm.registerFormat(new juce::MP3AudioFormat(), false);
#endif
    std::unique_ptr<juce::AudioFormatReader> reader(fm.createReaderFor(f));
    if (reader == nullptr)
    {
        return false;
    }
    out.name = f.getFileName();
    out.sampleRate = reader->sampleRate;
    out.frames = reader->lengthInSamples;
    out.channels = (int)reader->numChannels;
    out.bits = (int)reader->bitsPerSample;
    out.isFloat = reader->usesFloatingPointData;
    if (out.frames <= 0 || out.channels <= 0)
    {
        return false;
    }
    out.audio.setSize(out.channels, (int)out.frames);
    reader->read(&out.audio, 0, (int)out.frames, 0, true, out.channels > 1);
    out.ch.clear();
    for (int c = 0; c < out.channels; ++c)
    {
        out.ch.push_back(analyzeChannel(out.audio.getReadPointer(c), out.frames));
    }
    return true;
}

void printFileStats(const FileStats& s)
{
    info("=== " + s.name + " ===");
    info("  format: " + juce::String(s.sampleRate) + " Hz, " + juce::String(s.channels) + " ch, "
         + (s.isFloat ? "float32" : juce::String(s.bits) + "-bit PCM") + ", " + juce::String((juce::int64)s.frames)
         + " frames (" + juce::String((double)s.frames / s.sampleRate, 3) + " s)");
    for (int c = 0; c < s.channels; ++c)
    {
        const ChannelStats& ch = s.ch[(size_t)c];
        info("  ch" + juce::String(c) + ": samplePeak=" + juce::String(ch.peak, 5) + " (" + dbfs(ch.peak) + " dBFS)"
             + " oversampledPeakEst(4x)=" + juce::String(ch.oversampledPeak, 5) + " (" + dbfs(ch.oversampledPeak) + " dBFS)"
             + " rms=" + juce::String(ch.rms, 5) + " (" + dbfs(ch.rms) + " dBFS)" + " overs(|x|>1)=" + juce::String((juce::int64)ch.overs)
             + " nonFinite=" + juce::String((juce::int64)ch.nonFinite));
        info("       first=" + juce::String(ch.first, 6) + " last=" + juce::String(ch.last, 6) + " dc(all)=" + juce::String(ch.dc, 6)
             + " dcHead=" + juce::String(ch.dcHead, 6)
             + " dcTail=" + juce::String(ch.dcTail, 6) + " maxStepHead=" + juce::String(ch.maxStepHead, 5) + "@" + juce::String((juce::int64)ch.argmaxStepHead)
             + " maxStepTail=" + juce::String(ch.maxStepTail, 5) + "@" + juce::String((juce::int64)ch.argmaxStepTail));
    }
}

/// Lag (in samples) of `b` relative to `a` that maximises the normalised cross-correlation of the
/// first `window` samples of channel 0, searched over [0, maxLag]. MP3 decoders add encoder +
/// decoder delay, so b (decoded MP3) is expected to lag a (the WAV it was encoded from).
[[nodiscard]] std::int64_t findLag(const juce::AudioBuffer<float>& a, const juce::AudioBuffer<float>& b, const int window,
                                   const int maxLag, double& bestCorrOut)
{
    const float* x = a.getReadPointer(0);
    const float* y = b.getReadPointer(0);
    const int na = a.getNumSamples();
    const int nb = b.getNumSamples();
    std::int64_t bestLag = 0;
    double best = -2.0;
    for (int lag = 0; lag <= maxLag; ++lag)
    {
        double sxy = 0.0, sxx = 0.0, syy = 0.0;
        const int n = std::min(window, std::min(na, nb - lag));
        for (int i = 0; i < n; ++i)
        {
            const double xv = x[i];
            const double yv = y[i + lag];
            sxy += xv * yv;
            sxx += xv * xv;
            syy += yv * yv;
        }
        const double corr = (sxx > 0.0 && syy > 0.0) ? sxy / std::sqrt(sxx * syy) : 0.0;
        if (corr > best)
        {
            best = corr;
            bestLag = lag;
        }
    }
    bestCorrOut = best;
    return bestLag;
}

void compareMp3ToWav(const FileStats& wav, const FileStats& mp3)
{
    if (wav.channels < 1 || mp3.channels < 1)
    {
        return;
    }
    double corr = 0.0;
    const std::int64_t lag = findLag(wav.audio, mp3.audio, 48000, 4096, corr);
    info("  MP3 vs WAV alignment: decoded MP3 lags the WAV by " + juce::String((juce::int64)lag) + " samples (corr="
         + juce::String(corr, 4) + "); decoded frames " + juce::String((juce::int64)mp3.frames) + " vs WAV "
         + juce::String((juce::int64)wav.frames) + " (+" + juce::String((juce::int64)(mp3.frames - wav.frames)) + ")");
    for (int c = 0; c < std::min(wav.channels, mp3.channels); ++c)
    {
        info("  ch" + juce::String(c) + ": peak WAV " + dbfs(wav.ch[(size_t)c].peak) + " / MP3 " + dbfs(mp3.ch[(size_t)c].peak)
             + " dBFS (delta " + juce::String(20.0 * std::log10(std::max(1e-9, mp3.ch[(size_t)c].peak) / std::max(1e-9, wav.ch[(size_t)c].peak)), 2)
             + " dB); rms WAV " + dbfs(wav.ch[(size_t)c].rms) + " / MP3 " + dbfs(mp3.ch[(size_t)c].rms) + " dBFS; MP3 overs="
             + juce::String((juce::int64)mp3.ch[(size_t)c].overs));
    }
    // Edge transients: energy in the decoded MP3 BEFORE the aligned start of the WAV content
    // (pre-echo / encoder garbage) and AFTER the aligned end (post-echo / padding garbage).
    const float* y = mp3.audio.getReadPointer(0);
    double preMax = 0.0;
    for (std::int64_t i = 0; i < std::min<std::int64_t>(lag, mp3.frames); ++i)
    {
        preMax = std::max(preMax, std::fabs((double)y[i]));
    }
    const std::int64_t alignedEnd = lag + wav.frames;
    double postMax = 0.0;
    for (std::int64_t i = alignedEnd; i < mp3.frames; ++i)
    {
        postMax = std::max(postMax, std::fabs((double)y[i]));
    }
    info("  MP3 before aligned start (" + juce::String((juce::int64)lag) + " samples): max |x|=" + juce::String(preMax, 5) + " ("
         + dbfs(preMax) + " dBFS); after aligned end (" + juce::String((juce::int64)(mp3.frames - alignedEnd)) + " samples): max |x|="
         + juce::String(postMax, 5) + " (" + dbfs(postMax) + " dBFS)");
    // Sample-wise difference over the aligned body (first 5 s) — codec error level.
    const float* x = wav.audio.getReadPointer(0);
    const std::int64_t body = std::min<std::int64_t>({ wav.frames, mp3.frames - lag, 5 * 48000 });
    double diffSq = 0.0, refSq = 0.0;
    for (std::int64_t i = 0; i < body; ++i)
    {
        const double d = (double)y[i + lag] - (double)x[i];
        diffSq += d * d;
        refSq += (double)x[i] * (double)x[i];
    }
    if (refSq > 0.0)
    {
        info("  MP3 codec error over the first " + juce::String((double)body / 48000.0, 1) + " s (aligned): "
             + juce::String(10.0 * std::log10(diffSq / refSq), 2) + " dB relative to the WAV");
    }
}

/// Per-second DC / RMS / peak timeline of one file's channel 0 (where in time an offset lives).
int printTimeline(const juce::File& f)
{
    FileStats s;
    if (!readAudioFile(f, s))
    {
        std::printf("could not read %s\n", f.getFullPathName().toRawUTF8());
        return 1;
    }
    info("timeline of " + s.name + " (ch0, 1 s windows): t  dc  rms  peak");
    const float* x = s.audio.getReadPointer(0);
    const std::int64_t win = (std::int64_t)s.sampleRate;
    for (std::int64_t start = 0; start < s.frames; start += win)
    {
        const std::int64_t n = std::min(win, s.frames - start);
        double sum = 0.0, sumSq = 0.0, peak = 0.0;
        for (std::int64_t i = start; i < start + n; ++i)
        {
            sum += x[i];
            sumSq += (double)x[i] * (double)x[i];
            peak = std::max(peak, std::fabs((double)x[i]));
        }
        const double dc = sum / (double)n;
        const double rms = std::sqrt(sumSq / (double)n);
        const double acRms = std::sqrt(std::max(0.0, rms * rms - dc * dc));
        info("  " + juce::String((double)start / s.sampleRate, 0).paddedLeft(' ', 3) + " s  dc=" + juce::String(dc, 4)
             + "  rms=" + juce::String(rms, 4) + "  acRms=" + juce::String(acRms, 4) + "  peak=" + juce::String(peak, 4));
    }
    return 0;
}

/// Isolates a hosted VST3 instrument from DAL: instantiates `bundle` through plain JUCE, restores
/// the instrument state saved in `project` for `trackId` (experimentalInstrumentTracks[].pluginStateBase64,
/// JUCE host-wrapper format), then measures its raw output DC / peak over silence, over held notes
/// and over silence again — is a constant offset the plug-in's own behaviour?
int probeInstrumentDc(const juce::File& bundle, const juce::File& project, const int trackId)
{
    juce::AudioPluginFormatManager formats;
    formats.addFormat(new juce::VST3PluginFormat());
    juce::OwnedArray<juce::PluginDescription> found;
    for (int i = 0; i < formats.getNumFormats(); ++i)
    {
        formats.getFormat(i)->findAllTypesForFile(found, bundle.getFullPathName());
    }
    if (found.isEmpty())
    {
        std::printf("no plugin types in %s\n", bundle.getFullPathName().toRawUTF8());
        return 1;
    }
    juce::String err;
    std::unique_ptr<juce::AudioPluginInstance> inst = formats.createPluginInstance(*found[0], kRate, kBlock, err);
    if (inst == nullptr)
    {
        std::printf("instantiate failed: %s\n", err.toRawUTF8());
        return 1;
    }
    info("probe: " + inst->getName() + " outputs=" + juce::String(inst->getTotalNumOutputChannels()));
    inst->setPlayConfigDetails(0, juce::jmax(2, inst->getTotalNumOutputChannels()), kRate, kBlock);
    inst->prepareToPlay(kRate, kBlock);

    bool stateApplied = false;
    if (project.existsAsFile())
    {
        const juce::var root = juce::JSON::parse(project);
        const juce::var rows = root.getProperty("experimentalInstrumentTracks", {});
        if (rows.isArray())
        {
            for (const juce::var& r : *rows.getArray())
            {
                if ((int)r.getProperty("trackId", -1) == trackId)
                {
                    const juce::String b64 = r.getProperty("pluginStateBase64", "").toString();
                    juce::MemoryOutputStream mos;
                    if (b64.isNotEmpty() && juce::Base64::convertFromBase64(mos, b64))
                    {
                        inst->setStateInformation(mos.getData(), (int)mos.getDataSize());
                        stateApplied = true;
                        info("probe: applied saved state (" + juce::String((juce::int64)mos.getDataSize()) + " bytes) from track "
                             + juce::String(trackId));
                    }
                }
            }
        }
    }
    if (!stateApplied)
    {
        info("probe: no saved state applied (plug-in defaults)");
    }

    const int chans = juce::jmax(2, inst->getTotalNumOutputChannels());
    juce::AudioBuffer<float> buf(chans, kBlock);
    const auto measure = [&](const juce::String& label, const int blocks, const bool holdNote) {
        double sum = 0.0, sumSq = 0.0, peak = 0.0;
        std::int64_t n = 0;
        for (int b = 0; b < blocks; ++b)
        {
            buf.clear();
            juce::MidiBuffer midi;
            if (holdNote && b == 0)
            {
                midi.addEvent(juce::MidiMessage::noteOn(1, 60, (juce::uint8)100), 0);
                midi.addEvent(juce::MidiMessage::noteOn(1, 64, (juce::uint8)100), 0);
                midi.addEvent(juce::MidiMessage::noteOn(1, 67, (juce::uint8)100), 0);
            }
            if (!holdNote && b == 0)
            {
                midi.addEvent(juce::MidiMessage::allNotesOff(1), 0);
                midi.addEvent(juce::MidiMessage::noteOff(1, 60), 0);
                midi.addEvent(juce::MidiMessage::noteOff(1, 64), 0);
                midi.addEvent(juce::MidiMessage::noteOff(1, 67), 0);
            }
            inst->processBlock(buf, midi);
            const float* x = buf.getReadPointer(0);
            for (int i = 0; i < kBlock; ++i)
            {
                sum += x[i];
                sumSq += (double)x[i] * (double)x[i];
                peak = std::max(peak, std::fabs((double)x[i]));
            }
            n += kBlock;
        }
        const double dc = sum / (double)n;
        const double rms = std::sqrt(sumSq / (double)n);
        info("probe " + label + ": " + juce::String((double)n / kRate, 1) + " s  dc=" + juce::String(dc, 5) + " (" + dbfs(std::fabs(dc))
             + " dBFS)  rms=" + juce::String(rms, 5) + "  peak=" + juce::String(peak, 5) + " (" + dbfs(peak) + " dBFS)");
    };
    measure("silence right after prepare", 94, false);     // ~1 s
    measure("silence, later", 282, false);                 // ~3 s
    measure("three held notes", 282, true);                // ~3 s
    measure("silence after notes", 282, false);            // ~3 s
    measure("silence after notes, later", 470, false);     // ~5 s
    inst->reset();
    measure("silence after reset()", 282, false);
    inst->releaseResources();
    return 0;
}

// ---------------------------------------------------------------------------------------------
// Organ DC isolation: fresh instances, one parameter at a time, plus a .vstpreset for Cubase
// ---------------------------------------------------------------------------------------------
struct DcProbeResult
{
    double dcBefore[2] = { 0.0, 0.0 };     ///< silence before the first note (channel 0 / 1)
    double dcDuring[2] = { 0.0, 0.0 };     ///< while notes are held
    double dcAfter[2] = { 0.0, 0.0 };      ///< silence after note-off (3 s window)
    double acRmsAfter[2] = { 0.0, 0.0 };   ///< sqrt(rms^2 - dc^2) of the same window: the varying part
    double peakAfter[2] = { 0.0, 0.0 };
    double dcAfterLate[2] = { 0.0, 0.0 };  ///< a further 3 s later (does it decay?)
};

struct ProbeWindow
{
    double sum[2] = { 0.0, 0.0 };
    double sumSq[2] = { 0.0, 0.0 };
    double peak[2] = { 0.0, 0.0 };
    std::int64_t n = 0;
    void fold(const juce::AudioBuffer<float>& buf, const int numSamples)
    {
        for (int ch = 0; ch < 2 && ch < buf.getNumChannels(); ++ch)
        {
            const float* x = buf.getReadPointer(ch);
            for (int i = 0; i < numSamples; ++i)
            {
                sum[ch] += x[i];
                sumSq[ch] += (double)x[i] * (double)x[i];
                peak[ch] = std::max(peak[ch], std::fabs((double)x[i]));
            }
        }
        n += numSamples;
    }
    [[nodiscard]] double dc(const int ch) const { return n > 0 ? sum[ch] / (double)n : 0.0; }
    [[nodiscard]] double acRms(const int ch) const
    {
        if (n <= 0) return 0.0;
        const double rms2 = sumSq[ch] / (double)n;
        const double d = dc(ch);
        return std::sqrt(std::max(0.0, rms2 - d * d));
    }
};

enum class NoteOffStyle { NoteOffsOnly, NoteOffsPlusAllNotesOff, AllSoundOff };

/// One complete protocol on ONE fresh instance: [state] -> [mutations] -> 1 s silence -> 2 s chord
/// -> note-off (style) -> 3 s silence -> 3 s more silence.
[[nodiscard]] DcProbeResult runDcProtocol(juce::AudioPluginInstance& inst, const NoteOffStyle style)
{
    const int chans = juce::jmax(2, inst.getTotalNumOutputChannels());
    juce::AudioBuffer<float> buf(chans, kBlock);
    const auto blocksFor = [](const double seconds) { return (int)std::lround(seconds * kRate / kBlock); };
    DcProbeResult r;
    ProbeWindow before, during, after, late;
    const auto run = [&](ProbeWindow& w, const int blocks, const std::function<void(juce::MidiBuffer&, int)>& midiFor) {
        for (int b = 0; b < blocks; ++b)
        {
            buf.clear();
            juce::MidiBuffer midi;
            midiFor(midi, b);
            inst.processBlock(buf, midi);
            w.fold(buf, kBlock);
        }
    };
    run(before, blocksFor(1.0), [](juce::MidiBuffer&, int) {});
    run(during, blocksFor(2.0), [](juce::MidiBuffer& m, const int b) {
        if (b == 0)
        {
            m.addEvent(juce::MidiMessage::noteOn(1, 60, (juce::uint8)100), 0);
            m.addEvent(juce::MidiMessage::noteOn(1, 64, (juce::uint8)100), 0);
            m.addEvent(juce::MidiMessage::noteOn(1, 67, (juce::uint8)100), 0);
        }
    });
    run(after, blocksFor(3.0), [style](juce::MidiBuffer& m, const int b) {
        if (b == 0)
        {
            if (style == NoteOffStyle::AllSoundOff)
            {
                m.addEvent(juce::MidiMessage::allSoundOff(1), 0);
            }
            else
            {
                m.addEvent(juce::MidiMessage::noteOff(1, 60), 0);
                m.addEvent(juce::MidiMessage::noteOff(1, 64), 0);
                m.addEvent(juce::MidiMessage::noteOff(1, 67), 0);
                if (style == NoteOffStyle::NoteOffsPlusAllNotesOff)
                {
                    m.addEvent(juce::MidiMessage::allNotesOff(1), 0);
                }
            }
        }
    });
    run(late, blocksFor(3.0), [](juce::MidiBuffer&, int) {});
    for (int ch = 0; ch < 2; ++ch)
    {
        r.dcBefore[ch] = before.dc(ch);
        r.dcDuring[ch] = during.dc(ch);
        r.dcAfter[ch] = after.dc(ch);
        r.acRmsAfter[ch] = after.acRms(ch);
        r.peakAfter[ch] = after.peak[ch];
        r.dcAfterLate[ch] = late.dc(ch);
    }
    return r;
}

[[nodiscard]] juce::String describeDcResult(const DcProbeResult& r)
{
    return "dcBefore=[" + juce::String(r.dcBefore[0], 4) + "," + juce::String(r.dcBefore[1], 4) + "] dcDuring=["
           + juce::String(r.dcDuring[0], 4) + "," + juce::String(r.dcDuring[1], 4) + "] dcAfter=[" + juce::String(r.dcAfter[0], 4) + ","
           + juce::String(r.dcAfter[1], 4) + "] (" + dbfs(std::fabs(r.dcAfter[0])) + " dBFS) acRmsAfter=[" + juce::String(r.acRmsAfter[0], 5) + ","
           + juce::String(r.acRmsAfter[1], 5) + "] peakAfter=" + juce::String(r.peakAfter[0], 4) + " dcAfterLate=[" + juce::String(r.dcAfterLate[0], 4)
           + "," + juce::String(r.dcAfterLate[1], 4) + "]";
}

[[nodiscard]] juce::MemoryBlock loadSavedInstrumentState(const juce::File& project, const int trackId, juce::String& nameOut)
{
    juce::MemoryBlock state;
    if (!project.existsAsFile())
    {
        return state;
    }
    const juce::var root = juce::JSON::parse(project);
    const juce::var rows = root.getProperty("experimentalInstrumentTracks", {});
    if (!rows.isArray())
    {
        return state;
    }
    for (const juce::var& r : *rows.getArray())
    {
        if ((int)r.getProperty("trackId", -1) == trackId)
        {
            nameOut = r.getProperty("name", "").toString();
            const juce::String b64 = r.getProperty("pluginStateBase64", "").toString();
            juce::MemoryOutputStream mos;
            if (b64.isNotEmpty() && juce::Base64::convertFromBase64(mos, b64))
            {
                state.replaceAll(mos.getData(), mos.getDataSize());
            }
        }
    }
    return state;
}

[[nodiscard]] std::unique_ptr<juce::AudioPluginInstance> freshInstance(juce::AudioPluginFormatManager& formats,
                                                                      const juce::PluginDescription& desc,
                                                                      const juce::MemoryBlock* state)
{
    juce::String err;
    std::unique_ptr<juce::AudioPluginInstance> inst = formats.createPluginInstance(desc, kRate, kBlock, err);
    if (inst == nullptr)
    {
        info("instantiate failed: " + err);
        return nullptr;
    }
    inst->setPlayConfigDetails(0, juce::jmax(2, inst->getTotalNumOutputChannels()), kRate, kBlock);
    inst->prepareToPlay(kRate, kBlock);
    if (state != nullptr && state->getSize() > 0)
    {
        inst->setStateInformation(state->getData(), (int)state->getSize());
    }
    return inst;
}

/// Writes a Steinberg .vstpreset (header 'VST3', version 1, 32-char class ID, chunk list with
/// 'Comp' + 'Cont') from the JUCE "VST3PluginState" blob a DAL project stores, so the same state
/// can be loaded in another host (Cubase) for an independent comparison.
[[nodiscard]] bool writeVstPresetFromJuceState(const juce::MemoryBlock& juceState, const juce::String& classId32,
                                               const juce::File& out, juce::String& detail)
{
    std::unique_ptr<juce::XmlElement> xml = juce::AudioProcessor::getXmlFromBinary(juceState.getData(), (int)juceState.getSize());
    if (xml == nullptr || !xml->hasTagName("VST3PluginState"))
    {
        detail = "state blob is not a JUCE VST3PluginState block";
        return false;
    }
    juce::MemoryBlock comp, cont;
    if (auto* c = xml->getChildByName("IComponent"))
    {
        comp.fromBase64Encoding(c->getAllSubText().trim());
    }
    if (auto* c = xml->getChildByName("IEditController"))
    {
        cont.fromBase64Encoding(c->getAllSubText().trim());
    }
    if (comp.getSize() == 0)
    {
        detail = "no IComponent state in the blob";
        return false;
    }
    if (classId32.length() != 32)
    {
        detail = "class ID must be 32 hex characters";
        return false;
    }
    juce::MemoryOutputStream s;
    s.write("VST3", 4);
    s.writeInt(1);
    s.write(classId32.toRawUTF8(), 32);
    const juce::int64 compOffset = 48;
    const juce::int64 contOffset = compOffset + (juce::int64)comp.getSize();
    const juce::int64 listOffset = contOffset + (juce::int64)cont.getSize();
    s.writeInt64(listOffset);
    s.write(comp.getData(), comp.getSize());
    s.write(cont.getData(), cont.getSize());
    s.write("List", 4);
    s.writeInt(cont.getSize() > 0 ? 2 : 1);
    s.write("Comp", 4);
    s.writeInt64(compOffset);
    s.writeInt64((juce::int64)comp.getSize());
    if (cont.getSize() > 0)
    {
        s.write("Cont", 4);
        s.writeInt64(contOffset);
        s.writeInt64((juce::int64)cont.getSize());
    }
    (void)out.deleteFile();
    if (!out.replaceWithData(s.getData(), s.getDataSize()))
    {
        detail = "could not write " + out.getFullPathName();
        return false;
    }
    detail = "component " + juce::String((juce::int64)comp.getSize()) + " bytes, controller " + juce::String((juce::int64)cont.getSize())
             + " bytes -> " + out.getFullPathName();
    return true;
}

int probeInstrumentDcIsolation(const juce::File& bundle, const juce::File& project, const int trackId, const juce::File& outDir)
{
    juce::AudioPluginFormatManager formats;
    formats.addFormat(new juce::VST3PluginFormat());
    juce::OwnedArray<juce::PluginDescription> found;
    for (int i = 0; i < formats.getNumFormats(); ++i)
    {
        formats.getFormat(i)->findAllTypesForFile(found, bundle.getFullPathName());
    }
    if (found.isEmpty())
    {
        std::printf("no plugin types in %s\n", bundle.getFullPathName().toRawUTF8());
        return 1;
    }
    const juce::PluginDescription desc = *found[0];
    juce::String trackName;
    const juce::MemoryBlock saved = loadSavedInstrumentState(project, trackId, trackName);
    info("isolation: " + desc.name + " " + desc.version + " state for track " + juce::String(trackId) + " \"" + trackName + "\" = "
         + juce::String((juce::int64)saved.getSize()) + " bytes");
    (void)outDir.createDirectory();

    // 0. Reproduction package for another host: the saved state as a .vstpreset.
    {
        juce::String classId;
        const juce::File moduleInfo = bundle.getChildFile("Contents").getChildFile("Resources").getChildFile("moduleinfo.json");
        if (moduleInfo.existsAsFile())
        {
            // moduleinfo.json has trailing commas (not strict JSON) — pick the Audio Module Class CID by text.
            const juce::String text = moduleInfo.loadFileAsString();
            const int audioModule = text.indexOf("\"Audio Module Class\"");
            const int before = audioModule >= 0 ? text.substring(0, audioModule).lastIndexOf("\"CID\"") : -1;
            if (before >= 0)
            {
                classId = text.substring(before).fromFirstOccurrenceOf(":", false, false).upToFirstOccurrenceOf(",", false, false)
                              .removeCharacters(" \"\t\r\n");
            }
        }
        juce::String detail;
        if (classId.isNotEmpty() && writeVstPresetFromJuceState(saved, classId, outDir.getChildFile(desc.name + "-track" + juce::String(trackId) + ".vstpreset"), detail))
        {
            info("vstpreset: class " + classId + ": " + detail);
        }
        else
        {
            info("vstpreset: not written (" + (classId.isEmpty() ? juce::String("no class ID") : detail) + ")");
        }
    }

    // 1. Parameter inventory: default (fresh) vs saved.
    struct ParamDiff { int index; juce::String name; float defaultValue; float savedValue; juce::String defaultText; juce::String savedText; int steps; bool discrete; };
    std::vector<ParamDiff> diffs;
    {
        auto fresh = freshInstance(formats, desc, nullptr);
        auto withState = freshInstance(formats, desc, &saved);
        if (fresh == nullptr || withState == nullptr)
        {
            return 1;
        }
        const auto& pf = fresh->getParameters();
        const auto& ps = withState->getParameters();
        info("parameters: " + juce::String(pf.size()) + " (listing those whose saved value differs from the default)");
        for (int i = 0; i < pf.size() && i < ps.size(); ++i)
        {
            const float dv = pf[i]->getValue();
            const float sv = ps[i]->getValue();
            if (std::fabs(dv - sv) > 1.0e-4f)
            {
                ParamDiff d{ i, ps[i]->getName(64), dv, sv, pf[i]->getText(dv, 32), ps[i]->getText(sv, 32), ps[i]->getNumSteps(), ps[i]->isDiscrete() };
                diffs.push_back(d);
                info("  [" + juce::String(i) + "] \"" + d.name + "\": default " + juce::String(dv, 4) + " (" + d.defaultText + ") -> saved "
                     + juce::String(sv, 4) + " (" + d.savedText + ")" + (d.discrete ? " discrete" : ""));
            }
        }
    }

    // 2. Baselines on fresh instances.
    {
        auto inst = freshInstance(formats, desc, &saved);
        info("A  saved state, note-offs + all-notes-off : " + describeDcResult(runDcProtocol(*inst, NoteOffStyle::NoteOffsPlusAllNotesOff)));
    }
    {
        auto inst = freshInstance(formats, desc, &saved);
        info("A2 saved state, note-offs only            : " + describeDcResult(runDcProtocol(*inst, NoteOffStyle::NoteOffsOnly)));
    }
    {
        auto inst = freshInstance(formats, desc, &saved);
        info("A3 saved state, all-sound-off             : " + describeDcResult(runDcProtocol(*inst, NoteOffStyle::AllSoundOff)));
    }
    {
        auto inst = freshInstance(formats, desc, nullptr);
        info("B  plug-in defaults                       : " + describeDcResult(runDcProtocol(*inst, NoteOffStyle::NoteOffsPlusAllNotesOff)));
    }
    // C. Defaults + every differing parameter set individually to its saved value (bypasses the
    //    state-restore mechanism: is it the parameter VALUES or the restore path?).
    {
        auto inst = freshInstance(formats, desc, nullptr);
        for (const ParamDiff& d : diffs)
        {
            inst->getParameters()[d.index]->setValue(d.savedValue);
        }
        info("C  defaults + saved values set via parameters: " + describeDcResult(runDcProtocol(*inst, NoteOffStyle::NoteOffsPlusAllNotesOff)));
    }
    // D. Saved state, ONE differing parameter reverted to its default per fresh instance.
    info("D  saved state with ONE parameter reverted to default (fresh instance each):");
    struct Ranked { juce::String name; double dcAfter; };
    std::vector<Ranked> ranked;
    for (const ParamDiff& d : diffs)
    {
        auto inst = freshInstance(formats, desc, &saved);
        inst->getParameters()[d.index]->setValue(d.defaultValue);
        const DcProbeResult r = runDcProtocol(*inst, NoteOffStyle::NoteOffsPlusAllNotesOff);
        const double worst = std::max(std::fabs(r.dcAfter[0]), std::fabs(r.dcAfter[1]));
        ranked.push_back({ d.name, worst });
        info("   revert \"" + d.name + "\" -> " + d.defaultText + ": dcAfter=[" + juce::String(r.dcAfter[0], 4) + "," + juce::String(r.dcAfter[1], 4)
             + "] acRmsAfter=" + juce::String(r.acRmsAfter[0], 5) + " dcLate=" + juce::String(r.dcAfterLate[0], 4));
    }
    std::sort(ranked.begin(), ranked.end(), [](const Ranked& a, const Ranked& b) { return a.dcAfter < b.dcAfter; });
    if (!ranked.empty())
    {
        info("   -> smallest residual DC when reverting: \"" + ranked.front().name + "\" (" + juce::String(ranked.front().dcAfter, 4) + ")");
    }
    // E. Named suspects swept through all their values on the saved state.
    {
        auto probeInst = freshInstance(formats, desc, &saved);
        const auto& params = probeInst->getParameters();
        for (int i = 0; i < params.size(); ++i)
        {
            const juce::String name = params[i]->getName(64);
            const bool suspect = name.containsIgnoreCase("tube") || name.containsIgnoreCase("feedback") || name.containsIgnoreCase("amp")
                                 || name.containsIgnoreCase("model") || name.containsIgnoreCase("V.1") || name.containsIgnoreCase("V.2");
            if (!suspect)
            {
                continue;
            }
            const juce::StringArray values = params[i]->getAllValueStrings();
            const int steps = params[i]->getNumSteps();
            std::vector<float> tryValues;
            if (!values.isEmpty() && values.size() <= 16)
            {
                for (int v = 0; v < values.size(); ++v)
                {
                    tryValues.push_back(values.size() > 1 ? (float)v / (float)(values.size() - 1) : 0.0f);
                }
            }
            else if (params[i]->isDiscrete() && steps > 1 && steps <= 16)
            {
                for (int v = 0; v < steps; ++v)
                {
                    tryValues.push_back((float)v / (float)(steps - 1));
                }
            }
            else
            {
                tryValues = { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f };
            }
            info("E  sweep \"" + name + "\" (saved " + params[i]->getText(params[i]->getValue(), 32) + "):");
            for (const float v : tryValues)
            {
                auto inst = freshInstance(formats, desc, &saved);
                inst->getParameters()[i]->setValue(v);
                const juce::String text = inst->getParameters()[i]->getText(v, 32);
                const DcProbeResult r = runDcProtocol(*inst, NoteOffStyle::NoteOffsPlusAllNotesOff);
                info("     = " + text.paddedRight(' ', 14) + " dcAfter=[" + juce::String(r.dcAfter[0], 4) + "," + juce::String(r.dcAfter[1], 4)
                     + "] acRmsAfter=" + juce::String(r.acRmsAfter[0], 5) + " dcLate=" + juce::String(r.dcAfterLate[0], 4));
            }
        }
    }
    return 0;
}

int analyzeFolder(const juce::File& dir)
{
    if (!dir.isDirectory())
    {
        std::printf("not a folder: %s\n", dir.getFullPathName().toRawUTF8());
        return 1;
    }
    std::vector<FileStats> wavs;
    std::vector<FileStats> mp3s;
    for (const auto& entry : juce::RangedDirectoryIterator(dir, false, "*.wav;*.mp3", juce::File::findFiles))
    {
        FileStats s;
        if (!readAudioFile(entry.getFile(), s))
        {
            info("could not read " + entry.getFile().getFullPathName());
            continue;
        }
        printFileStats(s);
        if (entry.getFile().hasFileExtension("mp3"))
        {
            mp3s.push_back(std::move(s));
        }
        else
        {
            wavs.push_back(std::move(s));
        }
    }
    // Pair each MP3 with the float WAV of the same prefix ("A-master-192.mp3" <-> "A-master-float32.wav").
    for (const FileStats& m : mp3s)
    {
        const juce::String prefix = m.name.upToFirstOccurrenceOf("-master", false, false);
        for (const FileStats& w : wavs)
        {
            if (w.name.startsWith(prefix + "-master") && w.name.contains("float32"))
            {
                info("--- " + m.name + " vs " + w.name + " ---");
                compareMp3ToWav(w, m);
            }
        }
    }
    return 0;
}

// ---------------------------------------------------------------------------------------------
// Deterministic regression: realtime callback (stub device) vs offline render
// ---------------------------------------------------------------------------------------------
class StubDevice final : public juce::AudioIODevice
{
public:
    StubDevice() : juce::AudioIODevice("StubDevice", "Stub") {}
    juce::StringArray getOutputChannelNames() override { return { "L", "R" }; }
    juce::StringArray getInputChannelNames() override { return {}; }
    juce::Array<double> getAvailableSampleRates() override { return { kRate }; }
    juce::Array<int> getAvailableBufferSizes() override { return { kBlock }; }
    int getDefaultBufferSize() override { return kBlock; }
    juce::String open(const juce::BigInteger&, const juce::BigInteger&, double, int) override { return {}; }
    void close() override {}
    bool isOpen() override { return true; }
    void start(juce::AudioIODeviceCallback*) override {}
    void stop() override {}
    bool isPlaying() override { return false; }
    juce::String getLastError() override { return {}; }
    int getCurrentBufferSizeSamples() override { return kBlock; }
    double getCurrentSampleRate() override { return kRate; }
    int getCurrentBitDepth() override { return 32; }
    juce::BigInteger getActiveOutputChannels() const override { juce::BigInteger b; b.setRange(0, 2, true); return b; }
    juce::BigInteger getActiveInputChannels() const override { return {}; }
    int getOutputLatencyInSamples() override { return 0; }
    int getInputLatencyInSamples() override { return 0; }
};

[[nodiscard]] juce::File writeToneWav(const juce::File& dir, const juce::String& name, const double seconds, const float amp,
                                      const double hz)
{
    const int n = static_cast<int>(seconds * kRate);
    std::vector<float> pcm(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i)
    {
        pcm[(size_t)i] = static_cast<float>(amp * std::sin(2.0 * juce::MathConstants<double>::pi * hz * i / kRate));
    }
    const juce::File audioDir = dir.getChildFile("Audio");
    (void)audioDir.createDirectory();
    const juce::File wav = audioDir.getChildFile(name);
    const float* chans[1] = { pcm.data() };
    (void)MonoWavFileWriter::writeMulti24BitWavSegment(wav, chans, 1, n, kRate);
    return wav;
}

struct RenderedPair
{
    std::vector<float> realtimeL, realtimeR;
    std::vector<float> offlineL, offlineR;
};

/// Renders [start, start+len) through BOTH production paths of one engine: the device callback
/// (transport playing, stub device geometry) and `renderOfflineMixdownBlock`.
[[nodiscard]] RenderedPair renderBothPaths(Session& session, Transport& transport, PlaybackEngine& engine,
                                           const std::int64_t start, const int len)
{
    RenderedPair out;
    StubDevice dev;
    engine.audioDeviceAboutToStart(&dev);
    const auto snap = session.loadSessionSnapshotForAudioThread();

    // Offline.
    {
        juce::AudioBuffer<float> blk(2, kBlock);
        float* ptrs[2] = { blk.getWritePointer(0), blk.getWritePointer(1) };
        bool first = true;
        for (int pos = 0; pos < len; pos += kBlock)
        {
            const int n = std::min(kBlock, len - pos);
            engine.renderOfflineMixdownBlock(*snap, nullptr, start + pos, n, ptrs, first);
            first = false;
            out.offlineL.insert(out.offlineL.end(), ptrs[0], ptrs[0] + n);
            out.offlineR.insert(out.offlineR.end(), ptrs[1], ptrs[1] + n);
        }
    }
    // Realtime: seek, play, run callbacks.
    {
        transport.requestSeek(start);
        transport.requestPlaybackIntent(PlaybackIntent::Playing);
        juce::AudioBuffer<float> blk(2, kBlock);
        float* ptrs[2] = { blk.getWritePointer(0), blk.getWritePointer(1) };
        juce::AudioIODeviceCallbackContext ctx;
        for (int pos = 0; pos < len; pos += kBlock)
        {
            blk.clear();
            engine.audioDeviceIOCallbackWithContext(nullptr, 0, ptrs, 2, kBlock, ctx);
            const int n = std::min(kBlock, len - pos);
            out.realtimeL.insert(out.realtimeL.end(), ptrs[0], ptrs[0] + n);
            out.realtimeR.insert(out.realtimeR.end(), ptrs[1], ptrs[1] + n);
        }
        transport.requestPlaybackIntent(PlaybackIntent::Stopped);
        blk.clear();
        engine.audioDeviceIOCallbackWithContext(nullptr, 0, ptrs, 2, kBlock, ctx);
    }
    engine.audioDeviceStopped();
    return out;
}

[[nodiscard]] double maxAbsDiff(const std::vector<float>& a, const std::vector<float>& b)
{
    const size_t n = std::min(a.size(), b.size());
    double d = 0.0;
    for (size_t i = 0; i < n; ++i)
    {
        d = std::max(d, std::fabs((double)a[i] - (double)b[i]));
    }
    return d;
}

[[nodiscard]] double peakOf(const std::vector<float>& v)
{
    double p = 0.0;
    for (const float x : v)
    {
        p = std::max(p, std::fabs((double)x));
    }
    return p;
}

void testRealtimeVsOfflineDeterministic()
{
    const juce::File root = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("dal-export-level-tests");
    (void)root.deleteRecursively();
    (void)root.createDirectory();
    // Two tracks: 0.6 and 0.7 amplitude tones (sum 1.3 > full scale when both at 0 dB).
    const juce::File wavA = writeToneWav(root, "a.wav", 2.0, 0.6f, 440.0);
    const juce::File wavB = writeToneWav(root, "b.wav", 2.0, 0.7f, 660.0);

    Session session;
    Transport transport;
    PlaybackEngine engine(transport, session);
    const TrackId t1 = session.getActiveTrackId();
    expect(session.addRecordedTakeAtSample(wavA, kRate, 0, t1, (std::int64_t)(2.0 * kRate)).wasOk(), "fixture: take A on track 1");
    session.addTrack();
    const TrackId t2 = session.getActiveTrackId();
    expect(t2 != t1 && session.addRecordedTakeAtSample(wavB, kRate, 0, t2, (std::int64_t)(2.0 * kRate)).wasOk(),
           "fixture: take B on track 2");
    TrackId master = kInvalidTrackId;
    {
        const auto snap = session.loadSessionSnapshotForAudioThread();
        for (int i = 0; i < snap->getNumTracks(); ++i)
        {
            if (snap->getTrack(i).getKind() == TrackKind::Master)
            {
                master = snap->getTrack(i).getId();
            }
        }
    }
    expect(master != kInvalidTrackId, "fixture: master row present");
    engine.rebuildRoutingPlanFromSession();

    const int len = kBlock * 40; // ~0.43 s, well inside the 2 s takes
    const std::int64_t start = (std::int64_t)(0.5 * kRate);

    // 1. Everything at 0 dB, centre pan: both paths identical, and the sum exceeds full scale.
    {
        const RenderedPair p = renderBothPaths(session, transport, engine, start, len);
        const double d = std::max(maxAbsDiff(p.realtimeL, p.offlineL), maxAbsDiff(p.realtimeR, p.offlineR));
        info("0 dB: realtime peak=" + juce::String(peakOf(p.realtimeL), 5) + " offline peak=" + juce::String(peakOf(p.offlineL), 5)
             + " max|rt-off|=" + juce::String(d, 7));
        expect(d < 1.0e-6, "deterministic mix: realtime callback and offline render are sample-identical");
        const double expectedPeak = (0.6 + 0.7) * trackPanLawGainLeft(0.0f); // same tone phase at start
        info("    expected peak order ~" + juce::String(expectedPeak, 4) + " (pan law centre gain " + juce::String(trackPanLawGainLeft(0.0f), 4) + ")");
        expect(peakOf(p.offlineL) > 0.85, "deterministic mix: two 0 dB tones sum to a hot level (overload is REAL mix content, not a path bug)");
    }
    // 2. Master fader -12 dB scales both paths by exactly 0.2512.
    {
        const RenderedPair ref = renderBothPaths(session, transport, engine, start, len);
        session.setTrackChannelFaderGain(master, 0.251189f);
        engine.rebuildRoutingPlanFromSession();
        const RenderedPair p = renderBothPaths(session, transport, engine, start, len);
        const double ratioOff = peakOf(p.offlineL) / peakOf(ref.offlineL);
        const double ratioRt = peakOf(p.realtimeL) / peakOf(ref.realtimeL);
        info("master -12 dB: offline ratio=" + juce::String(ratioOff, 5) + " realtime ratio=" + juce::String(ratioRt, 5));
        expect(std::fabs(ratioOff - 0.251189) < 1.0e-4, "master fader: offline render scales by exactly the fader gain");
        expect(std::fabs(ratioRt - 0.251189) < 1.0e-4, "master fader: realtime output scales by exactly the fader gain");
        expect(std::max(maxAbsDiff(p.realtimeL, p.offlineL), maxAbsDiff(p.realtimeR, p.offlineR)) < 1.0e-6,
               "master fader: both paths still sample-identical");
        session.setTrackChannelFaderGain(master, 1.0f);
    }
    // 3. Track fader + pan + mute follow in both paths.
    {
        session.setTrackChannelFaderGain(t2, 0.5f);
        session.setTrackStereoPan(t1, -1.0f); // hard left
        engine.rebuildRoutingPlanFromSession();
        const RenderedPair p = renderBothPaths(session, transport, engine, start, len);
        expect(std::max(maxAbsDiff(p.realtimeL, p.offlineL), maxAbsDiff(p.realtimeR, p.offlineR)) < 1.0e-6,
               "track fader + pan: both paths sample-identical");
        session.setTrackMuted(t2, true);
        const RenderedPair m = renderBothPaths(session, transport, engine, start, len);
        expect(std::max(maxAbsDiff(m.realtimeL, m.offlineL), maxAbsDiff(m.realtimeR, m.offlineR)) < 1.0e-6,
               "mute: both paths sample-identical");
        expect(peakOf(m.offlineR) < 1.0e-6 && peakOf(m.offlineL) > 0.1,
               "mute + hard-left pan: right channel silent, left carries track 1 only");
        session.setTrackMuted(t2, false);
        session.setTrackStereoPan(t1, 0.0f);
        session.setTrackChannelFaderGain(t2, 1.0f);
    }
    (void)root.deleteRecursively();
}

void testLevelMeterAccumulator()
{
    level_meter::Accumulator acc;
    std::vector<float> l(kBlock, 0.0f), r(kBlock, 0.0f);
    l[10] = 0.5f;
    r[20] = -1.25f; // over full scale
    acc.audioThread_fold(l.data(), r.data(), kBlock);
    std::fill(l.begin(), l.end(), 0.1f);
    std::fill(r.begin(), r.end(), 0.1f);
    acc.audioThread_fold(l.data(), r.data(), kBlock); // quieter block must not lower the hold
    const level_meter::Reading rd = acc.drainAndReset();
    expect(std::fabs(rd.peak[0] - 0.5f) < 1e-6f && std::fabs(rd.peak[1] - 1.25f) < 1e-6f,
           "meter: peak hold keeps the loudest sample across blocks (a short peak survives a slower UI)");
    expect(rd.overs[0] == 0 && rd.overs[1] == 1, "meter: overs count samples above full scale per channel");
    expect(rd.blocks == 2 && rd.samples == 2u * kBlock && rd.channels == 2, "meter: block/sample/channel bookkeeping");
    const level_meter::Reading empty = acc.drainAndReset();
    expect(empty.blocks == 0 && empty.peak[0] == 0.0f && empty.peak[1] == 0.0f, "meter: drain resets the window");
    acc.audioThread_fold(l.data(), nullptr, kBlock);
    expect(acc.drainAndReset().channels == 1, "meter: mono fold reports one channel");
    l[3] = std::numeric_limits<float>::quiet_NaN();
    acc.audioThread_fold(l.data(), r.data(), kBlock);
    const level_meter::Reading nan = acc.drainAndReset();
    expect(nan.nonFinite == 1 && !std::isfinite(nan.peak[0]), "meter: NaN is reported, never hidden");
    expect(level_meter::peakToDbfsText(1.0f) == "0.0" && level_meter::peakToDbfsText(0.5f) == "-6.0"
               && level_meter::peakToDbfsText(1.4125f) == "+3.0" && level_meter::peakToDbfsText(0.0f).startsWith("-"),
           "meter: dBFS text keeps the sign and never caps a value above 0 dBFS");
}

void testFaderScale()
{
    using S = channel_fader_scale::Scale;
    expect(S::positionForDb(S::kMaxDb) == 1.0 && S::positionForDb(S::kMinDb) == 0.0, "fader scale: +6 dB at top, -inf at bottom");
    for (const double db : { 6.0, 3.0, 0.0, -3.0, -6.0, -10.0, -20.0, -30.0, -40.0, -50.0 })
    {
        const double pos = S::positionForDb(db);
        const double back = S::dbForPosition(pos);
        expect(std::fabs(back - db) < 1e-6, "fader scale: round trip at " + juce::String(db, 1) + " dB (pos " + juce::String(pos, 3) + ")");
    }
    expect(S::positionForDb(0.0) > 0.7 && S::positionForDb(-10.0) > 0.5 && S::positionForDb(-20.0) > 0.35,
           "fader scale: the 0 .. -20 dB mixing range occupies most of the travel (graded like the reference)");
    expect(S::positionForDb(-5.0) - S::positionForDb(-10.0) > S::positionForDb(-45.0) - S::positionForDb(-50.0),
           "fader scale: more travel per dB near 0 dB than near the bottom");
    expect(S::linearGainForPosition(0.0) == 0.0f, "fader scale: bottom = exactly zero linear gain (-inf)");
    expect(std::fabs(S::linearGainForPosition(S::positionForDb(0.0)) - 1.0f) < 1e-6f, "fader scale: 0 dB = unity gain");
    expect(std::fabs(S::linearGainForPosition(1.0) - 1.99526f) < 1e-4f, "fader scale: top = +6 dB (1.995)");
    expect(S::positionForLinearGain(0.0f) == 0.0 && std::fabs(S::positionForLinearGain(1.0f) - S::positionForDb(0.0)) < 1e-9,
           "fader scale: linear gain -> position (0 -> bottom, 1 -> 0 dB mark)");
    expect(S::gainText(0.0f) == juce::String(juce::CharPointer_UTF8("-\xe2\x88\x9e")) && S::gainText(1.0f) == "0.00"
               && S::gainText(1.99526f) == "+6.00" && S::gainText(0.5f) == "-6.02",
           "fader scale: gain text -inf / 0.00 / +6.00 / -6.02");
    float g = -1.0f;
    expect(S::parseGainText("-inf", g) && g == 0.0f, "fader scale: parse -inf -> 0 linear");
    expect(S::parseGainText("+6", g) && std::fabs(g - 1.99526f) < 1e-4f, "fader scale: parse +6 -> 1.995");
    expect(S::parseGainText("12", g) && std::fabs(g - 1.99526f) < 1e-4f, "fader scale: parse 12 clamps to +6 dB");
    expect(S::parseGainText("-3 dB", g) && std::fabs(g - 0.70795f) < 1e-4f, "fader scale: parse '-3 dB'");
    expect(!S::parseGainText("abc", g), "fader scale: garbage is rejected (field keeps the old value)");
}

void testFaderRoundTripThroughProjectFile()
{
    const juce::File root = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("dal-export-level-tests-rt");
    (void)root.deleteRecursively();
    (void)root.createDirectory();
    const juce::File wav = writeToneWav(root, "t.wav", 0.5, 0.3f, 440.0);
    const juce::File proj = root.getChildFile("fader.dalproj");
    TrackId t1 = kInvalidTrackId, t2 = kInvalidTrackId, master = kInvalidTrackId;
    {
        Session session;
        Transport transport;
        t1 = session.getActiveTrackId();
        (void)session.addRecordedTakeAtSample(wav, kRate, 0, t1, (std::int64_t)(0.5 * kRate));
        session.addTrack();
        t2 = session.getActiveTrackId();
        const auto snap = session.loadSessionSnapshotForAudioThread();
        for (int i = 0; i < snap->getNumTracks(); ++i)
        {
            if (snap->getTrack(i).getKind() == TrackKind::Master)
            {
                master = snap->getTrack(i).getId();
            }
        }
        session.setTrackChannelFaderGain(t1, 0.0f);      // -inf
        session.setTrackChannelFaderGain(t2, 1.99526f);  // +6 dB
        session.setTrackChannelFaderGain(master, 1.0f);  // 0 dB
        expect(session.saveProjectToFile(transport, proj, kRate).wasOk(), "fader persistence: saved");
        const juce::String json = proj.loadFileAsString();
        expect(!json.containsIgnoreCase("Infinity") && !json.containsIgnoreCase("NaN"),
               "fader persistence: -inf is stored as linear 0, never as Infinity/NaN text");
    }
    {
        Session session;
        Transport transport;
        juce::StringArray skipped;
        juce::String note;
        expect(session.loadProjectFromFile(transport, proj, kRate, skipped, note).wasOk(), "fader persistence: reloaded");
        const auto snap = session.loadSessionSnapshotForAudioThread();
        const int i1 = snap->findTrackIndexById(t1), i2 = snap->findTrackIndexById(t2), im = snap->findTrackIndexById(master);
        expect(i1 >= 0 && snap->getTrack(i1).getChannelFaderGain() == 0.0f, "fader persistence: -inf (0 linear) survives");
        expect(i2 >= 0 && std::fabs(snap->getTrack(i2).getChannelFaderGain() - 1.99526f) < 1e-4f, "fader persistence: +6 dB survives");
        expect(im >= 0 && std::fabs(snap->getTrack(im).getChannelFaderGain() - 1.0f) < 1e-6f, "fader persistence: 0 dB survives");
    }
    (void)root.deleteRecursively();
}
} // namespace

int main(int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI juceGui;
    for (int i = 1; i < argc; ++i)
    {
        const juce::String a(argv[i]);
        if (a == "--analyze" && i + 1 < argc)
        {
            return analyzeFolder(juce::File(juce::String(argv[i + 1])));
        }
        if (a == "--timeline" && i + 1 < argc)
        {
            return printTimeline(juce::File(juce::String(argv[i + 1])));
        }
        if (a == "--probe-vst3-dc" && i + 3 < argc)
        {
            return probeInstrumentDc(juce::File(juce::String(argv[i + 1])), juce::File(juce::String(argv[i + 2])),
                                     juce::String(argv[i + 3]).getIntValue());
        }
        if (a == "--probe-vst3-isolate" && i + 4 < argc)
        {
            return probeInstrumentDcIsolation(juce::File(juce::String(argv[i + 1])), juce::File(juce::String(argv[i + 2])),
                                              juce::String(argv[i + 3]).getIntValue(), juce::File(juce::String(argv[i + 4])));
        }
    }
    testLevelMeterAccumulator();
    testFaderScale();
    testFaderRoundTripThroughProjectFile();
    testRealtimeVsOfflineDeterministic();
    std::printf("\n%d checks, %d failure(s)\n", checks, failures);
    return failures == 0 ? 0 : 1;
}

// Link seams — Session.cpp / PlaybackEngine.cpp / PlaybackMixHelpers.cpp reference instrument entry
// points this harness never exercises (no instrument tracks, no hosts).
#include "instruments/InstrumentTrackController.h"
#include "plugins/ExperimentalInstrumentHost.h"

ProjectFileExperimentalInstrumentTrackV1 InstrumentTrackController::buildExperimentalInstrumentProjectBlock() const
{
    jassertfalse;
    return {};
}
void InstrumentTrackController::audioThread_scheduleTransportMidiForSegment(ExperimentalInstrumentHost&, std::int64_t, int,
                                                                           int, bool, int, int*, bool, bool) noexcept {}
void InstrumentTrackController::audioThread_flushTransportMidi(ExperimentalInstrumentHost&, int, int) noexcept {}
void InstrumentTrackController::audioThread_flushPendingTransportOffsInto(ExperimentalInstrumentHost&, int, int) noexcept {}
void ExperimentalInstrumentHost::audioThread_processBlockAndAddToOutputs(float* const*, int, int, float, float) noexcept {}
void ExperimentalInstrumentHost::audioThread_renderGenerationStageForBlock(int) noexcept {}
void ExperimentalInstrumentHost::audioThread_beginAudioBlock(int) noexcept {}
void ExperimentalInstrumentHost::audioThread_addMidiEventForCurrentBlock(int, const juce::MidiMessage&) noexcept {}
void ExperimentalInstrumentHost::audioThread_noteProxyTimelineSegmentForCurrentBlock(std::int64_t, int, int) noexcept {}
void ExperimentalInstrumentHost::audioThread_noteProxyLoopRangeForCurrentBlock(std::int64_t, std::int64_t) noexcept {}
bool ExperimentalInstrumentHost::messageThread_prefetchProxyRangeForOffline(std::int64_t, int, int) noexcept { return true; }
