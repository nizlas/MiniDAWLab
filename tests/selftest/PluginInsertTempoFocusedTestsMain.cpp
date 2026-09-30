// =============================================================================
// PluginInsertTempoFocusedTests — production VST3 insert tempo regression check
// =============================================================================
//
// Loads the built DAL Mono Delay bundle through DAL's real PluginInsertHost, rather than JUCE's
// standalone test host. The test drives the host-owned AudioPlayHead exactly as PlaybackEngine
// does and measures its wet impulse delay. No audio device, project file or global VST3 install is
// used; the bundle path is supplied on the command line.
// =============================================================================

#include "plugins/PluginInsertHost.h"

#include <juce_events/juce_events.h>

#include <cmath>
#include <cstdio>
#include <vector>

namespace
{
    constexpr TrackId kTestTrackId = 701;
    constexpr double kSampleRate = 48000.0;
    constexpr int kBlockSize = 512;

    bool failed = false;

    void expect(const bool condition, const char* const message)
    {
        std::printf("%s %s\n", condition ? "[ ok ]" : "[FAIL]", message);
        failed = failed || !condition;
    }

    void setContext(PluginInsertHost& host, const double bpm, const std::int64_t timelineSample,
                    const bool playing)
    {
        PluginProcessTransportContext context;
        context.timelineSample = timelineSample;
        context.sampleRate = kSampleRate;
        context.bpm = bpm;
        context.timeSignatureNumerator = 4;
        context.timeSignatureDenominator = 4;
        context.isPlaying = playing;
        host.audioThread_setProcessTransportContext(context);
    }

    [[nodiscard]] int renderAndFindImpulse(PluginInsertHost& host, const double bpm,
                                           const bool playing, const int expectedDelaySamples)
    {
        // First let the plug-in's normal 50 ms time smoothing settle after the public state
        // restore. Then feed one impulse and examine only the real processed insert output.
        std::int64_t timelineSample = 0;
        for (int warmup = 0; warmup < 12; ++warmup)
        {
            setContext(host, bpm, timelineSample, playing);
            host.audioThread_clearScratch(PluginInsertHost::kInsertChannels, kBlockSize);
            host.audioThread_processChainForTrack(kTestTrackId, InsertStage::Post, kBlockSize);
            timelineSample += kBlockSize;
        }

        std::vector<float> rendered;
        rendered.reserve(static_cast<size_t>(expectedDelaySamples + kBlockSize * 4));
        bool injected = false;
        const int blocks = (expectedDelaySamples / kBlockSize) + 4;
        for (int block = 0; block < blocks; ++block)
        {
            setContext(host, bpm, timelineSample, playing);
            host.audioThread_clearScratch(PluginInsertHost::kInsertChannels, kBlockSize);
            float* const* const scratch = host.audioThread_getScratchWritePointers();
            if (!injected && scratch != nullptr && scratch[0] != nullptr && scratch[1] != nullptr)
            {
                scratch[0][0] = 1.0f;
                scratch[1][0] = 1.0f;
                injected = true;
            }
            host.audioThread_processChainForTrack(kTestTrackId, InsertStage::Post, kBlockSize);
            if (scratch != nullptr && scratch[0] != nullptr)
            {
                rendered.insert(rendered.end(), scratch[0], scratch[0] + kBlockSize);
            }
            timelineSample += kBlockSize;
        }

        int peakIndex = -1;
        float peak = 0.0f;
        for (int i = 1; i < static_cast<int>(rendered.size()); ++i)
        {
            if (std::abs(rendered[(size_t)i]) > peak)
            {
                peak = std::abs(rendered[(size_t)i]);
                peakIndex = i;
            }
        }
        std::printf("[info] BPM %.1f playing=%d peak=%f sample=%d\n",
                    bpm, playing ? 1 : 0, peak, peakIndex);
        // The first 180-BPM transition begins from the plug-in's default 120-BPM delay and is
        // intentionally smoothed, so interpolation can spread that first impulse. Its timing,
        // rather than amplitude, is the transport regression invariant.
        expect(peak > 0.30f, "wet insert produced a measurable default-mix echo");
        return peakIndex;
    }
} // namespace

int main(int argc, char** argv)
{
    if (argc != 2)
    {
        std::printf("usage: PluginInsertTempoFocusedTests <absolute-path-to-DALMonoDelay.vst3>\n");
        return 2;
    }

    juce::ScopedJuceInitialiser_GUI juceRuntime;
    PluginInsertHost host;
    host.prepareForDevice(kSampleRate, kBlockSize, 2);

    const juce::Result loadResult = host.addInsertFromVst3File(
        kTestTrackId, InsertStage::Post, juce::File(juce::String::fromUTF8(argv[1])));
    expect(loadResult.wasOk(), "DAL Mono Delay loads through PluginInsertHost");
    if (loadResult.failed())
    {
        std::printf("%s\n", loadResult.getErrorMessage().toRawUTF8());
        return 1;
    }

    // DAL Mono Delay's public defaults are Sync on, 1/4 Triplet and 49% wet. The source defect
    // made both cases use the 120 BPM fallback (16,000 samples), so the stopped 180-BPM assertion
    // fails before the host assigns a valid JUCE AudioPlayHead.
    const int stoppedPeak = renderAndFindImpulse(host, 180.0, false, 10667);
    expect(std::abs(stoppedPeak - 10667) <= 2,
           "stopped input-processing context delivers 180 BPM (1/4T ≈ 222.22 ms)");

    const int playingPeak = renderAndFindImpulse(host, 120.0, true, 16000);
    expect(std::abs(playingPeak - 16000) <= 2,
           "playing context updates to 120 BPM without reloading (1/4T ≈ 333.33 ms)");

    return failed ? 1 : 0;
}
