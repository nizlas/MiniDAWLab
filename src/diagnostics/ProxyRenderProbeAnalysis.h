#pragma once

// =============================================================================
// ProxyRenderProbeAnalysis — content measurements of a rendered proxy artifact
// (diagnostics only; used by `--stability-proxy-render-probe` and its report)
// =============================================================================
// Answers the questions a render-length discrepancy raises without assuming
// bit-identical output (instruments legitimately vary between runs, SPIKE-02
// H5): where the first audible sample is, where the last sample above the tail
// threshold is, how much energy the musical section carries, what the tail
// section contains, and a per-second profile for side-by-side reading.
// Every measurement is PER CHANNEL (L and R separately — never a mid-sum, so
// anti-phase content cannot cancel) and distinguishes the raw peak, the running
// offset (DC) and the varying part: the DC-tracked residual of the tail policy
// (`proxy_render::DcTrackingPeakMeter`, same time constant as the detector) and
// the per-second RMS of the signal minus its per-second mean.
// Message thread or any non-audio thread; reads the whole file.

#include "instruments/ProxyRenderTypes.h"

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_cryptography/juce_cryptography.h>

#include <cmath>
#include <cstdint>
#include <vector>

namespace proxy_probe
{

[[nodiscard]] inline double linearToDb(const double v) noexcept
{
    return v > 0.0 ? 20.0 * std::log10(v) : -200.0;
}

/// One second of one channel.
struct ChannelSecond
{
    double rawPeakDb = -200.0;   ///< max |x|
    double dc = 0.0;             ///< mean of x over the second
    double acRmsDb = -200.0;     ///< RMS of (x − mean of the second)
    double residualPeakDb = -200.0; ///< tail-policy residual peak (DC-tracked, continuous state)
};

struct SecondProfile
{
    ChannelSecond ch[2];
    /// Folded across channels (the louder one) for the compact views.
    double peakDb = -200.0;
    double rmsDb = -200.0;
    double residualPeakDb = -200.0;
};

struct WavAnalysis
{
    bool ok = false;
    juce::String error;
    double sampleRate = 0.0;
    int channels = 0;
    std::int64_t lengthSamples = 0;
    std::int64_t spanEndSamples = 0;
    double overallPeakDb = -200.0;
    /// First sample whose DC-tracked residual exceeds −60 dBFS on either channel (−1 = never).
    std::int64_t firstAudibleSample = -1;
    /// Last sample whose DC-tracked residual is at or above the tail threshold (−1 = never).
    std::int64_t lastAboveTailThresholdSample = -1;
    /// Last sample whose RAW value is at or above the tail threshold (what tail policy v1 judged).
    std::int64_t lastRawAboveTailThresholdSample = -1;
    double musicPeakDb = -200.0;        ///< [0, spanEnd), raw
    double musicResidualPeakDb = -200.0; ///< [0, spanEnd), DC-tracked residual
    double musicRmsDb = -200.0;         ///< [0, spanEnd), raw RMS (includes any offset)
    int musicSecondsAudible = 0;        ///< seconds inside [0, spanEnd) whose residual peak exceeds −60 dBFS
    int musicSecondsTotal = 0;
    double tailPeakDb = -200.0;         ///< [spanEnd, end), raw
    double tailResidualPeakDb = -200.0; ///< [spanEnd, end), DC-tracked residual
    double tailRmsDb = -200.0;
    float firstSample[2] = {};
    float lastSample[2] = {};
    double dcFirstSecond[2] = {};       ///< mean of the first second per channel
    double dcLastSecond[2] = {};        ///< mean of the last second per channel
    std::vector<SecondProfile> perSecond;
    juce::String sha256;
};

/// Reads `file` completely and measures it against `spanEndSamples` (the render's last relevant
/// event, from the result).
[[nodiscard]] inline WavAnalysis analyzeRenderedWav(const juce::File& file, const std::int64_t spanEndSamples)
{
    WavAnalysis a;
    a.spanEndSamples = spanEndSamples;
    if (!file.existsAsFile())
    {
        a.error = "file missing";
        return a;
    }
    juce::WavAudioFormat fmt;
    std::unique_ptr<juce::AudioFormatReader> reader(fmt.createReaderFor(file.createInputStream().release(), true));
    if (reader == nullptr)
    {
        a.error = "not a readable WAV";
        return a;
    }
    a.sampleRate = reader->sampleRate;
    a.channels = (int)reader->numChannels;
    a.lengthSamples = (std::int64_t)reader->lengthInSamples;
    if (a.sampleRate <= 0.0 || a.channels <= 0)
    {
        a.error = "invalid format";
        return a;
    }
    const int nch = juce::jmin(2, a.channels);
    const double audibleLinear = proxy_render::dbToLinear(-60.0);
    const double tailLinear = proxy_render::dbToLinear(proxy_render::kTailThresholdDb);
    const std::int64_t secondLen = (std::int64_t)std::llround(a.sampleRate);
    const int numSeconds = (int)((a.lengthSamples + secondLen - 1) / juce::jmax<std::int64_t>(1, secondLen));
    a.perSecond.assign((size_t)juce::jmax(0, numSeconds), SecondProfile{});
    // Per-second accumulators per channel.
    std::vector<double> secSum[2], secSumSq[2];
    std::vector<std::int64_t> secCount((size_t)juce::jmax(0, numSeconds), 0);
    for (int c = 0; c < 2; ++c)
    {
        secSum[c].assign((size_t)juce::jmax(0, numSeconds), 0.0);
        secSumSq[c].assign((size_t)juce::jmax(0, numSeconds), 0.0);
    }
    double musicSumSq = 0.0, tailSumSq = 0.0;
    std::int64_t musicCount = 0, tailCount = 0;
    proxy_render::DcTrackingPeakMeter meter(a.sampleRate);

    juce::AudioBuffer<float> chunk(a.channels, 16384);
    for (std::int64_t pos = 0; pos < a.lengthSamples;)
    {
        const int n = (int)juce::jmin<std::int64_t>(chunk.getNumSamples(), a.lengthSamples - pos);
        if (!reader->read(&chunk, 0, n, pos, true, a.channels > 1))
        {
            a.error = "read failed";
            return a;
        }
        if (pos == 0 && n > 0)
        {
            for (int c = 0; c < nch; ++c)
            {
                a.firstSample[c] = chunk.getSample(c, 0);
            }
        }
        for (int c = 0; c < nch; ++c)
        {
            a.lastSample[c] = chunk.getSample(c, n - 1);
        }
        // Sample-accurate residual: feed the meter one sample at a time so the first/last
        // residual positions are exact (cost is irrelevant for a diagnostic).
        for (int i = 0; i < n; ++i)
        {
            const std::int64_t s = pos + i;
            const size_t sec = (size_t)(s / secondLen);
            float one[2] = { chunk.getSample(0, i), nch > 1 ? chunk.getSample(1, i) : chunk.getSample(0, i) };
            const float* ptrs[2] = { &one[0], &one[1] };
            const auto reading = meter.feedBlock(ptrs, 2, 1);
            double rawFolded = 0.0;
            for (int c = 0; c < nch; ++c)
            {
                const double v = (double)one[c];
                rawFolded = juce::jmax(rawFolded, std::abs(v));
                if (sec < a.perSecond.size())
                {
                    auto& cs = a.perSecond[sec].ch[c];
                    cs.rawPeakDb = juce::jmax(cs.rawPeakDb, linearToDb(std::abs(v)));
                    const double res = std::abs(v - reading.dcAtEnd[c]);
                    cs.residualPeakDb = juce::jmax(cs.residualPeakDb, linearToDb(res));
                    secSum[c][sec] += v;
                    secSumSq[c][sec] += v * v;
                }
            }
            if (sec < a.perSecond.size())
            {
                ++secCount[sec];
            }
            const double residual = reading.residualPeak;
            a.overallPeakDb = juce::jmax(a.overallPeakDb, linearToDb(rawFolded));
            if (residual > audibleLinear && a.firstAudibleSample < 0)
            {
                a.firstAudibleSample = s;
            }
            if (residual >= tailLinear)
            {
                a.lastAboveTailThresholdSample = s;
            }
            if (rawFolded >= tailLinear)
            {
                a.lastRawAboveTailThresholdSample = s;
            }
            const double sumSqSample = (double)one[0] * one[0] + (nch > 1 ? (double)one[1] * one[1] : (double)one[0] * one[0]);
            if (s < spanEndSamples)
            {
                a.musicPeakDb = juce::jmax(a.musicPeakDb, linearToDb(rawFolded));
                a.musicResidualPeakDb = juce::jmax(a.musicResidualPeakDb, linearToDb(residual));
                musicSumSq += sumSqSample / 2.0;
                ++musicCount;
            }
            else
            {
                a.tailPeakDb = juce::jmax(a.tailPeakDb, linearToDb(rawFolded));
                a.tailResidualPeakDb = juce::jmax(a.tailResidualPeakDb, linearToDb(residual));
                tailSumSq += sumSqSample / 2.0;
                ++tailCount;
            }
        }
        pos += n;
    }
    for (size_t sec = 0; sec < a.perSecond.size(); ++sec)
    {
        auto& p = a.perSecond[sec];
        for (int c = 0; c < nch; ++c)
        {
            auto& cs = p.ch[c];
            if (secCount[sec] > 0)
            {
                const double mean = secSum[c][sec] / (double)secCount[sec];
                const double ms = secSumSq[c][sec] / (double)secCount[sec];
                cs.dc = mean;
                cs.acRmsDb = linearToDb(std::sqrt(juce::jmax(0.0, ms - mean * mean)));
            }
            p.peakDb = juce::jmax(p.peakDb, cs.rawPeakDb);
            p.rmsDb = juce::jmax(p.rmsDb, cs.acRmsDb);
            p.residualPeakDb = juce::jmax(p.residualPeakDb, cs.residualPeakDb);
        }
        if ((std::int64_t)sec * secondLen < spanEndSamples)
        {
            ++a.musicSecondsTotal;
            if (p.residualPeakDb > -60.0)
            {
                ++a.musicSecondsAudible;
            }
        }
    }
    if (!a.perSecond.empty())
    {
        for (int c = 0; c < nch; ++c)
        {
            a.dcFirstSecond[c] = a.perSecond.front().ch[c].dc;
            a.dcLastSecond[c] = a.perSecond.back().ch[c].dc;
        }
    }
    a.musicRmsDb = musicCount > 0 ? linearToDb(std::sqrt(musicSumSq / (double)musicCount)) : -200.0;
    a.tailRmsDb = tailCount > 0 ? linearToDb(std::sqrt(tailSumSq / (double)tailCount)) : -200.0;
    {
        juce::FileInputStream in(file);
        if (in.openedOk())
        {
            a.sha256 = juce::SHA256(in).toHexString();
        }
    }
    a.ok = true;
    return a;
}

/// One line per second and channel: "s012 L raw=-18.2 dc=+0.0012 ac=-27.4 res=-18.3 | R …".
/// `maxSeconds` <= 0 prints everything.
[[nodiscard]] inline juce::String formatPerSecondProfile(const WavAnalysis& a, const int maxSeconds = 0)
{
    juce::String out;
    const int n = maxSeconds > 0 ? juce::jmin((int)a.perSecond.size(), maxSeconds) : (int)a.perSecond.size();
    const int nch = juce::jmin(2, a.channels);
    for (int s = 0; s < n; ++s)
    {
        out << "s" << juce::String(s).paddedLeft('0', 3);
        for (int c = 0; c < nch; ++c)
        {
            const auto& cs = a.perSecond[(size_t)s].ch[c];
            out << (c == 0 ? " L" : " | R") << " raw=" << juce::String(cs.rawPeakDb, 1) << " dc=" << juce::String(cs.dc, 4)
                << " ac=" << juce::String(cs.acRmsDb, 1) << " res=" << juce::String(cs.residualPeakDb, 1);
        }
        out << "\n";
    }
    return out;
}

[[nodiscard]] inline juce::String summarizeAnalysis(const WavAnalysis& a)
{
    if (!a.ok)
    {
        return "analysis failed: " + a.error;
    }
    const auto sec = [&a](const std::int64_t s) { return s < 0 ? juce::String("never") : juce::String((double)s / a.sampleRate, 3) + " s"; };
    juce::String out;
    out << "length=" << juce::String(a.lengthSamples) << " (" << juce::String((double)a.lengthSamples / a.sampleRate, 3) << " s)"
        << " spanEnd=" << juce::String(a.spanEndSamples) << " (" << juce::String((double)a.spanEndSamples / a.sampleRate, 3) << " s)"
        << " rawPeak=" << juce::String(a.overallPeakDb, 1) << " dBFS"
        << " firstAudible(res>-60)=" << sec(a.firstAudibleSample)
        << " lastAbove(res>=-70)=" << sec(a.lastAboveTailThresholdSample)
        << " lastRawAbove(-70)=" << sec(a.lastRawAboveTailThresholdSample)
        << " | music: rawPeak=" << juce::String(a.musicPeakDb, 1) << " resPeak=" << juce::String(a.musicResidualPeakDb, 1)
        << " rms=" << juce::String(a.musicRmsDb, 1) << " audibleSeconds=" << a.musicSecondsAudible << "/" << a.musicSecondsTotal
        << " | tail: rawPeak=" << juce::String(a.tailPeakDb, 1) << " resPeak=" << juce::String(a.tailResidualPeakDb, 1)
        << " rms=" << juce::String(a.tailRmsDb, 1)
        << " | edges: first L/R=" << juce::String(a.firstSample[0], 4) << "/" << juce::String(a.firstSample[1], 4)
        << " last L/R=" << juce::String(a.lastSample[0], 4) << "/" << juce::String(a.lastSample[1], 4)
        << " dcFirstSec L/R=" << juce::String(a.dcFirstSecond[0], 4) << "/" << juce::String(a.dcFirstSecond[1], 4)
        << " dcLastSec L/R=" << juce::String(a.dcLastSecond[0], 4) << "/" << juce::String(a.dcLastSecond[1], 4)
        << " | sha256=" << a.sha256.substring(0, 16);
    return out;
}

} // namespace proxy_probe
