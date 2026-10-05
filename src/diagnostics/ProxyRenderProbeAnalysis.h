#pragma once

// =============================================================================
// ProxyRenderProbeAnalysis — content measurements of a rendered proxy artifact
// (diagnostics only; used by `--stability-proxy-render-probe` and its report)
// =============================================================================
// Answers the questions a render-length discrepancy raises without assuming
// bit-identical output (instruments legitimately vary between runs, SPIKE-02
// H5): where the first audible sample is, where the last sample above the tail
// threshold is, how much energy the musical section carries, what the tail
// section contains, and a per-second peak/RMS profile for side-by-side reading.
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

struct SecondProfile
{
    double peakDb = -200.0;
    double rmsDb = -200.0;
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
    /// First sample whose absolute value exceeds −60 dBFS (−1 = never).
    std::int64_t firstAudibleSample = -1;
    /// Last sample above the tail threshold (−70 dBFS, `proxy_render::kTailThresholdDb`); −1 = never.
    std::int64_t lastAboveTailThresholdSample = -1;
    double musicPeakDb = -200.0;   ///< [0, spanEnd)
    double musicRmsDb = -200.0;    ///< [0, spanEnd)
    int musicSecondsAudible = 0;   ///< seconds inside [0, spanEnd) whose peak exceeds −60 dBFS
    int musicSecondsTotal = 0;
    double tailPeakDb = -200.0;    ///< [spanEnd, end)
    double tailRmsDb = -200.0;
    std::vector<SecondProfile> perSecond;
    juce::String sha256;
};

/// Reads `file` completely and measures it against `spanEndSamples` (the render's last relevant
/// event, from the result). Both channels are folded (max of |L|,|R| per sample; RMS over both).
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
    const double audibleLinear = proxy_render::dbToLinear(-60.0);
    const double tailLinear = proxy_render::dbToLinear(proxy_render::kTailThresholdDb);
    const std::int64_t secondLen = (std::int64_t)std::llround(a.sampleRate);
    const int numSeconds = (int)((a.lengthSamples + secondLen - 1) / juce::jmax<std::int64_t>(1, secondLen));
    a.perSecond.assign((size_t)juce::jmax(0, numSeconds), SecondProfile{});
    std::vector<double> secondSumSq((size_t)juce::jmax(0, numSeconds), 0.0);
    std::vector<std::int64_t> secondCount((size_t)juce::jmax(0, numSeconds), 0);
    double musicSumSq = 0.0, tailSumSq = 0.0;
    std::int64_t musicCount = 0, tailCount = 0;

    juce::AudioBuffer<float> chunk(a.channels, 16384);
    for (std::int64_t pos = 0; pos < a.lengthSamples;)
    {
        const int n = (int)juce::jmin<std::int64_t>(chunk.getNumSamples(), a.lengthSamples - pos);
        if (!reader->read(&chunk, 0, n, pos, true, a.channels > 1))
        {
            a.error = "read failed";
            return a;
        }
        for (int i = 0; i < n; ++i)
        {
            double folded = 0.0;
            double sumSqSample = 0.0;
            for (int c = 0; c < a.channels; ++c)
            {
                const double v = (double)chunk.getSample(c, i);
                folded = juce::jmax(folded, std::abs(v));
                sumSqSample += v * v;
            }
            const std::int64_t s = pos + i;
            const size_t sec = (size_t)(s / secondLen);
            if (sec < a.perSecond.size())
            {
                a.perSecond[sec].peakDb = juce::jmax(a.perSecond[sec].peakDb, linearToDb(folded));
                secondSumSq[sec] += sumSqSample / (double)a.channels;
                ++secondCount[sec];
            }
            a.overallPeakDb = juce::jmax(a.overallPeakDb, linearToDb(folded));
            if (folded > audibleLinear && a.firstAudibleSample < 0)
            {
                a.firstAudibleSample = s;
            }
            if (folded >= tailLinear)
            {
                a.lastAboveTailThresholdSample = s;
            }
            if (s < spanEndSamples)
            {
                a.musicPeakDb = juce::jmax(a.musicPeakDb, linearToDb(folded));
                musicSumSq += sumSqSample / (double)a.channels;
                ++musicCount;
            }
            else
            {
                a.tailPeakDb = juce::jmax(a.tailPeakDb, linearToDb(folded));
                tailSumSq += sumSqSample / (double)a.channels;
                ++tailCount;
            }
        }
        pos += n;
    }
    for (size_t sec = 0; sec < a.perSecond.size(); ++sec)
    {
        a.perSecond[sec].rmsDb = secondCount[sec] > 0 ? linearToDb(std::sqrt(secondSumSq[sec] / (double)secondCount[sec])) : -200.0;
        if ((std::int64_t)sec * secondLen < spanEndSamples)
        {
            ++a.musicSecondsTotal;
            if (a.perSecond[sec].peakDb > -60.0)
            {
                ++a.musicSecondsAudible;
            }
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

/// One line per second: "s12 peak=-18.2 rms=-27.4". `maxSeconds` <= 0 prints everything.
[[nodiscard]] inline juce::String formatPerSecondProfile(const WavAnalysis& a, const int maxSeconds = 0)
{
    juce::String out;
    const int n = maxSeconds > 0 ? juce::jmin((int)a.perSecond.size(), maxSeconds) : (int)a.perSecond.size();
    for (int s = 0; s < n; ++s)
    {
        out << "s" << juce::String(s).paddedLeft('0', 3) << " peak=" << juce::String(a.perSecond[(size_t)s].peakDb, 1)
            << " rms=" << juce::String(a.perSecond[(size_t)s].rmsDb, 1) << "\n";
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
        << " peak=" << juce::String(a.overallPeakDb, 1) << " dBFS"
        << " firstAudible(-60)=" << sec(a.firstAudibleSample)
        << " lastAbove(-70)=" << sec(a.lastAboveTailThresholdSample)
        << " | music: peak=" << juce::String(a.musicPeakDb, 1) << " rms=" << juce::String(a.musicRmsDb, 1)
        << " audibleSeconds=" << a.musicSecondsAudible << "/" << a.musicSecondsTotal
        << " | tail: peak=" << juce::String(a.tailPeakDb, 1) << " rms=" << juce::String(a.tailRmsDb, 1)
        << " | sha256=" << a.sha256.substring(0, 16);
    return out;
}

} // namespace proxy_probe
