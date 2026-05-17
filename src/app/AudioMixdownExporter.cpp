#include "app/AudioMixdownExporter.h"

#include "domain/Session.h"
#include "domain/SessionSnapshot.h"
#include "engine/PlaybackEngine.h"
#include "transport/Transport.h"

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <cmath>
#include <memory>

namespace
{
class ScopedOfflineRenderGate final
{
public:
    explicit ScopedOfflineRenderGate(PlaybackEngine& engine) noexcept
        : engine_(engine)
    {
        engine_.setOfflineRenderInProgress(true);
    }

    ~ScopedOfflineRenderGate()
    {
        engine_.setOfflineRenderInProgress(false);
    }

private:
    PlaybackEngine& engine_;
};
} // namespace

namespace mini_daw_audio_mixdown
{

juce::Result resolveActiveLoopMixdownSpan(const bool cycleEnabledFromTransport,
                                          const std::int64_t leftLocatorSamples,
                                          const std::int64_t rightLocatorSamples,
                                          ActiveLoopMixdownSpan& out) noexcept
{
    if (!cycleEnabledFromTransport)
    {
        return juce::Result::fail("Set an active loop range before mixdown.");
    }

    // Match `PlaybackEngine` `validCycle`: cycleOn && locR > locL && locR > 0  ([L,R) playback span).
    if (leftLocatorSamples < 0 || !(rightLocatorSamples > leftLocatorSamples && rightLocatorSamples > 0))
    {
        return juce::Result::fail("Loop range is empty or invalid.");
    }

    out.startSample = leftLocatorSamples;
    out.lengthSamples = rightLocatorSamples - leftLocatorSamples;
    jassert(out.lengthSamples > 0);
    return juce::Result::ok();
}

juce::Result exportStereoMixdownWavBlocking(
    Transport& transport,
    Session& session,
    PlaybackEngine& playbackEngine,
    juce::AudioDeviceManager& deviceManager,
    const std::function<void()>& syncTransportUiFromDomain,
    const MixdownExportRequest& request)
{
    if (request.outputFile == juce::File())
    {
        return juce::Result::fail("Invalid export path.");
    }
    if (!std::isfinite(request.sampleRate) || request.sampleRate <= 0.0)
    {
        return juce::Result::fail("Invalid sample rate.");
    }

    juce::AudioIODevice* const device = deviceManager.getCurrentAudioDevice();
    const int deviceBlockCap = device != nullptr ? device->getCurrentBufferSizeSamples() : 0;
    if (deviceBlockCap <= 0)
    {
        return juce::Result::fail("Audio device is not open (cannot determine safe render block size).");
    }

    const int renderQuantum = juce::jmin(4096, deviceBlockCap);

    if (transport.readPlaybackIntentForUi() == PlaybackIntent::Playing)
    {
        transport.requestPlaybackIntent(PlaybackIntent::Stopped);
        if (syncTransportUiFromDomain != nullptr)
        {
            syncTransportUiFromDomain();
        }
    }

    const std::shared_ptr<const SessionSnapshot> sessionSnap = session.loadSessionSnapshotForAudioThread();
    if (sessionSnap == nullptr)
    {
        return juce::Result::fail("Session snapshot is not available.");
    }

    ActiveLoopMixdownSpan loopSpan;
    {
        const juce::Result spanRes = resolveActiveLoopMixdownSpan(
            transport.readCycleEnabledForUi(),
            sessionSnap->getLeftLocatorSamples(),
            sessionSnap->getRightLocatorSamples(),
            loopSpan);
        if (spanRes.failed())
        {
            return spanRes;
        }
    }

    const std::shared_ptr<const ExperimentalInstrumentPlaybackSnapshot> instrumentSnap
        = playbackEngine.loadExperimentalInstrumentPlaybackSnapshotForAudioThread();

    if (request.outputFile.existsAsFile())
    {
        const bool overwrite = juce::NativeMessageBox::showYesNoBox(
            juce::AlertWindow::WarningIcon,
            "Audio Mixdown",
            "A file already exists at:\n\n"
                + request.outputFile.getFullPathName()
                + "\n\nOverwrite it?",
            nullptr,
            nullptr);
        if (!overwrite)
        {
            return juce::Result::fail("Export cancelled.");
        }
    }

    const juce::File parentDir = request.outputFile.getParentDirectory();
    if (!parentDir.isDirectory())
    {
        if (!parentDir.createDirectory())
        {
            return juce::Result::fail("Could not create folder:\n" + parentDir.getFullPathName());
        }
    }

    ScopedOfflineRenderGate offlineGate(playbackEngine);

    auto fileStream = std::make_unique<juce::FileOutputStream>(request.outputFile);
    if (fileStream->failedToOpen())
    {
        return juce::Result::fail("Could not open file for writing:\n" + request.outputFile.getFullPathName());
    }

    juce::WavAudioFormat wavFormat;
    const int bits = static_cast<int>(request.bits);
    std::unique_ptr<juce::AudioFormatWriter> writer(wavFormat.createWriterFor(fileStream.release(),
                                                                              request.sampleRate,
                                                                              2u,
                                                                              bits,
                                                                              juce::StringPairArray(),
                                                                              0));
    if (writer == nullptr)
    {
        return juce::Result::fail("Could not create WAV writer for the requested format.");
    }

    if (request.bits == MixdownWaveBits::IeeeFloat32)
    {
        if (!writer->isFloatingPoint())
        {
            return juce::Result::fail("This build cannot write IEEE float WAV (writer is not floating-point).");
        }
        if (writer->getBitsPerSample() != 32)
        {
            return juce::Result::fail("Unexpected WAV writer bit depth for float export.");
        }
    }
    else
    {
        if (writer->isFloatingPoint())
        {
            return juce::Result::fail("WAV writer unexpectedly reported floating-point for PCM export.");
        }
        if (writer->getBitsPerSample() != bits)
        {
            return juce::Result::fail("WAV writer bit depth does not match the requested format.");
        }
    }

    juce::AudioBuffer<float> stereoBlock(2, renderQuantum);
    float* stereoPtrs[2] = { stereoBlock.getWritePointer(0), stereoBlock.getWritePointer(1) };

    bool firstBlock = true;
    std::int64_t pos = 0;
    while (pos < loopSpan.lengthSamples)
    {
        const int n = static_cast<int>(
            juce::jmin(static_cast<std::int64_t>(renderQuantum), loopSpan.lengthSamples - pos));
        playbackEngine.renderOfflineMixdownBlock(*sessionSnap,
                                                 instrumentSnap.get(),
                                                 loopSpan.startSample + pos,
                                                 n,
                                                 stereoPtrs,
                                                 firstBlock);
        firstBlock = false;

        if (!writer->writeFromAudioSampleBuffer(stereoBlock, 0, n))
        {
            request.outputFile.deleteFile();
            return juce::Result::fail("Disk write failed during mixdown.");
        }
        pos += static_cast<std::int64_t>(n);
    }

    writer.reset();
    return juce::Result::ok();
}

} // namespace mini_daw_audio_mixdown
