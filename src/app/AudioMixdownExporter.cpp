#include "app/AudioMixdownExporter.h"

#include "diagnostics/StabilityDiagnosticLog.h"
#include "domain/MixdownWavProbe.h"
#include "domain/Session.h"
#include "domain/SessionSnapshot.h"
#include "engine/PlaybackEngine.h"
#include "transport/Transport.h"

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <cmath>
#include <memory>

namespace mini_daw_audio_mixdown
{

namespace
{

constexpr int kMp3EncodeTimeoutMs = 600000; // 10 minutes

[[nodiscard]] bool isAllowedMp3BitrateKbps(const int kbps) noexcept
{
    switch (kbps)
    {
    case 128:
    case 160:
    case 192:
    case 224:
    case 256:
    case 320:
        return true;
    default:
        return false;
    }
}

[[nodiscard]] MixdownWaveBits mixdownIntermediateWavBitsForLame(const double sampleRate) noexcept
{
    return probeStereoFloatWavSupportedMixdown(sampleRate) ? MixdownWaveBits::IeeeFloat32
                                                             : MixdownWaveBits::Pcm24;
}

/// RAII offline-render gate (Stability Slice 2). On the outermost enter it silences the realtime
/// callback and then *drains* any in-flight callback: `beginOfflineRenderGate` (seq_cst) pairs with
/// the `audioCallbackInProcessingSection` flag set at the top of the device callback, so after the
/// bounded wait below no callback is touching plugin hosts / scratch buffers. Nested construction
/// (WAV render inside MP3 export) only bumps the depth — realtime resumes when the outermost gate
/// destructs, on every return path.
class ScopedOfflineRenderGate final
{
public:
    explicit ScopedOfflineRenderGate(PlaybackEngine& engine) noexcept
        : engine_(engine)
    {
        const bool outermost = engine_.beginOfflineRenderGate();
        if (!outermost)
        {
            appendMixdownDiagnosticLine("export gate enter (nested; realtime already suspended)");
            return;
        }
        appendMixdownDiagnosticLine("export gate enter; realtime suspend set");
        appendMixdownDiagnosticLine("audio callback idle wait begin");
        const double waitStartMs = juce::Time::getMillisecondCounterHiRes();
        constexpr double kMaxWaitMs = 250.0;
        bool timedOut = false;
        while (engine_.isAudioCallbackInProcessingSection())
        {
            if (juce::Time::getMillisecondCounterHiRes() - waitStartMs >= kMaxWaitMs)
            {
                timedOut = true;
                break;
            }
            juce::Thread::sleep(1);
        }
        const double waitedMs = juce::Time::getMillisecondCounterHiRes() - waitStartMs;
        appendMixdownDiagnosticLine("audio callback idle wait end waitedMs="
                                    + juce::String(waitedMs, 2)
                                    + " timeout=" + (timedOut ? "YES (proceeding anyway)" : "no"));
    }

    ~ScopedOfflineRenderGate()
    {
        const bool resumed = engine_.endOfflineRenderGate();
        appendMixdownDiagnosticLine(resumed ? "export gate exit; realtime resumed"
                                            : "export gate exit (nested; realtime still suspended)");
    }

    ScopedOfflineRenderGate(const ScopedOfflineRenderGate&) = delete;
    ScopedOfflineRenderGate& operator=(const ScopedOfflineRenderGate&) = delete;

private:
    PlaybackEngine& engine_;
};

} // namespace

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
    appendMixdownDiagnosticLine("wav export requested path=\"" + request.outputFile.getFullPathName()
                                + "\" sampleRate=" + juce::String(request.sampleRate)
                                + " bits=" + juce::String(static_cast<int>(request.bits)));
    writeLastOperationBreadcrumb("mixdown wav render start: " + request.outputFile.getFullPathName());

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

    // Overwrite consent is collected by the dialog (async prompt) before export starts. The old
    // synchronous NativeMessageBox here returned instantly with an arbitrary answer, so callers
    // must confirm up front; refuse rather than silently overwrite.
    if (request.outputFile.existsAsFile() && !request.overwriteConfirmed)
    {
        return juce::Result::fail("Export cancelled (existing file was not confirmed for overwrite):\n"
                                  + request.outputFile.getFullPathName());
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

    // FileOutputStream appends to an existing file (stream starts at the end), which would leave
    // the old audio in place and write a second WAV after it. Replace the file instead.
    if (request.outputFile.existsAsFile() && !request.outputFile.deleteFile())
    {
        appendMixdownDiagnosticLine("FAIL could not delete existing output file for overwrite");
        return juce::Result::fail("Could not replace the existing file:\n"
                                  + request.outputFile.getFullPathName());
    }

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

    appendMixdownDiagnosticLine("render start span=" + juce::String((juce::int64)loopSpan.startSample)
                                + "+" + juce::String((juce::int64)loopSpan.lengthSamples)
                                + " quantum=" + juce::String(renderQuantum));
    if (request.progressSink != nullptr)
    {
        appendMixdownDiagnosticLine("progress phase: render (determinate)");
        request.progressSink->setMixdownProgress("Mixing down... 0%", 0.0);
    }

    bool firstBlock = true;
    std::int64_t pos = 0;
    double lastProgressUpdateMs = juce::Time::getMillisecondCounterHiRes();
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
            appendMixdownDiagnosticLine("FAIL disk write at pos=" + juce::String((juce::int64)pos));
            request.outputFile.deleteFile();
            return juce::Result::fail("Disk write failed during mixdown.");
        }
        pos += static_cast<std::int64_t>(n);

        if (request.progressSink != nullptr)
        {
            const double nowMs = juce::Time::getMillisecondCounterHiRes();
            if (nowMs - lastProgressUpdateMs >= 50.0 || pos >= loopSpan.lengthSamples)
            {
                lastProgressUpdateMs = nowMs;
                const double frac = static_cast<double>(pos)
                                    / static_cast<double>(loopSpan.lengthSamples);
                request.progressSink->setMixdownProgress(
                    "Mixing down... " + juce::String(static_cast<int>(frac * 100.0 + 0.5)) + "%",
                    frac);
            }
        }
    }

    appendMixdownDiagnosticLine("render complete samples=" + juce::String((juce::int64)pos));
    appendMixdownDiagnosticLine("writer close begin");
    writer.reset();
    appendMixdownDiagnosticLine("writer close end");
    appendMixdownDiagnosticLine("wav export ok path=\"" + request.outputFile.getFullPathName() + "\"");
    return juce::Result::ok();
}

juce::File findBundledLameExecutable() noexcept
{
    const juce::File exeDir
        = juce::File::getSpecialLocation(juce::File::currentExecutableFile).getParentDirectory();
    const juce::File lameDir = exeDir.getChildFile("Tools").getChildFile("lame");
    const juce::File winExe = lameDir.getChildFile("lame.exe");
    if (winExe.existsAsFile())
    {
        return winExe;
    }
    const juce::File posixExe = lameDir.getChildFile("lame");
    if (posixExe.existsAsFile())
    {
        return posixExe;
    }
    return {};
}

bool isBundledLameEncoderAvailable() noexcept
{
    return findBundledLameExecutable().existsAsFile();
}

juce::Result exportStereoMixdownMp3Blocking(Transport& transport,
                                           Session& session,
                                           PlaybackEngine& playbackEngine,
                                           juce::AudioDeviceManager& deviceManager,
                                           const std::function<void()>& syncTransportUiFromDomain,
                                           const juce::File& mp3OutputFile,
                                           const int bitrateKbps,
                                           MixdownProgressSink* const progressSink,
                                           const bool overwriteConfirmed)
{
    appendMixdownDiagnosticLine("mp3 export requested path=\"" + mp3OutputFile.getFullPathName()
                                + "\" kbps=" + juce::String(bitrateKbps));
    writeLastOperationBreadcrumb("mixdown mp3 start: " + mp3OutputFile.getFullPathName());

    const juce::File lameExe = findBundledLameExecutable();
    if (!lameExe.existsAsFile())
    {
        return juce::Result::fail(
            "MP3 encoder not found. Expected Tools/lame/lame.exe beside the application.");
    }

    if (mp3OutputFile == juce::File{})
    {
        return juce::Result::fail("Invalid export path.");
    }

    if (!isAllowedMp3BitrateKbps(bitrateKbps))
    {
        return juce::Result::fail("Invalid MP3 bitrate.");
    }

    juce::AudioIODevice* const device = deviceManager.getCurrentAudioDevice();
    if (device == nullptr)
    {
        return juce::Result::fail("No audio device is open.");
    }

    const double sampleRate = device->getCurrentSampleRate();
    if (!std::isfinite(sampleRate) || sampleRate <= 0.0)
    {
        return juce::Result::fail("Invalid sample rate.");
    }

    // See the WAV exporter: overwrite consent must be collected by the caller before export.
    if (mp3OutputFile.existsAsFile() && !overwriteConfirmed)
    {
        return juce::Result::fail("Export cancelled (existing file was not confirmed for overwrite):\n"
                                  + mp3OutputFile.getFullPathName());
    }

    const juce::File parentDir = mp3OutputFile.getParentDirectory();
    if (!parentDir.isDirectory())
    {
        if (!parentDir.createDirectory())
        {
            return juce::Result::fail("Could not create folder:\n" + parentDir.getFullPathName());
        }
    }

    // Outer gate: keeps realtime suspended through the *entire* MP3 pipeline (temp WAV render,
    // writer close, LAME encode, temp cleanup), not just the inner WAV render. The nested gate
    // inside exportStereoMixdownWavBlocking only bumps the depth.
    appendMixdownDiagnosticLine("mp3 outer gate: hold realtime through render + LAME");
    ScopedOfflineRenderGate mp3OuterGate(playbackEngine);

    juce::File tempWav;
    for (int attempt = 0; attempt < 16; ++attempt)
    {
        (void)attempt;
        const juce::String unique = juce::String::toHexString(juce::Random::getSystemRandom().nextInt64());
        tempWav = mp3OutputFile.getSiblingFile(mp3OutputFile.getFileNameWithoutExtension()
                                               + ".__dal_mp3_source_" + unique + ".wav");
        if (!tempWav.existsAsFile())
        {
            break;
        }
    }

    if (tempWav == juce::File{} || tempWav.existsAsFile())
    {
        return juce::Result::fail("Could not allocate a temporary WAV file path.");
    }

    MixdownExportRequest wavRequest;
    wavRequest.outputFile = tempWav;
    wavRequest.sampleRate = sampleRate;
    wavRequest.bits = mixdownIntermediateWavBitsForLame(sampleRate);
    wavRequest.progressSink = progressSink;
    wavRequest.overwriteConfirmed = true; // temp file was verified non-existent above

    const juce::Result wavResult = exportStereoMixdownWavBlocking(
        transport,
        session,
        playbackEngine,
        deviceManager,
        syncTransportUiFromDomain,
        wavRequest);

    if (wavResult.failed())
    {
        appendMixdownDiagnosticLine("FAIL mp3 temp wav render: " + wavResult.getErrorMessage());
        (void)tempWav.deleteFile();
        const juce::String msg = wavResult.getErrorMessage();
        if (msg == "Export cancelled." || msg.startsWith("Export cancelled"))
        {
            return wavResult;
        }
        return juce::Result::fail("Temporary WAV creation failed.\n\n" + msg);
    }

    juce::ChildProcess lameProcess;
    juce::StringArray args;
    args.add(lameExe.getFullPathName());
    args.add("-b");
    args.add(juce::String(bitrateKbps));
    args.add(tempWav.getFullPathName());
    args.add(mp3OutputFile.getFullPathName());

    appendMixdownDiagnosticLine("lame start exe=\"" + lameExe.getFullPathName() + "\"");
    if (progressSink != nullptr)
    {
        appendMixdownDiagnosticLine("progress phase: mp3 encode (indeterminate)");
        progressSink->setMixdownProgress("Encoding MP3...", -1.0);
    }
    if (!lameProcess.start(args, juce::ChildProcess::wantStdErr))
    {
        appendMixdownDiagnosticLine("FAIL lame could not start");
        const juce::String kept = "\n\nTemporary WAV kept for debugging:\n" + tempWav.getFullPathName();
        return juce::Result::fail("MP3 encoding failed (could not start LAME)." + kept);
    }

    // Wait in short slices so the indeterminate progress indicator keeps animating (LAME itself
    // reports no usable progress); total wait is still bounded by kMp3EncodeTimeoutMs.
    bool lameFinished = false;
    {
        const double lameWaitStartMs = juce::Time::getMillisecondCounterHiRes();
        for (;;)
        {
            if (lameProcess.waitForProcessToFinish(100))
            {
                lameFinished = true;
                break;
            }
            if (juce::Time::getMillisecondCounterHiRes() - lameWaitStartMs
                >= static_cast<double>(kMp3EncodeTimeoutMs))
            {
                break;
            }
            if (progressSink != nullptr)
            {
                progressSink->setMixdownProgress("Encoding MP3...", -1.0);
            }
        }
    }
    if (!lameFinished)
    {
        appendMixdownDiagnosticLine("FAIL lame timed out");
        (void)lameProcess.kill();
        const juce::String kept = "\n\nTemporary WAV kept for debugging:\n" + tempWav.getFullPathName();
        return juce::Result::fail("MP3 encoding timed out." + kept);
    }

    const auto exitCode = static_cast<int>(lameProcess.getExitCode());
    const juce::String lameStderr = lameProcess.readAllProcessOutput().trim();
    appendMixdownDiagnosticLine("lame complete exitCode=" + juce::String(exitCode));

    if (exitCode != 0)
    {
        juce::String msg = "MP3 encoding failed.";
        if (lameStderr.isNotEmpty())
        {
            msg << "\n\n" << lameStderr;
        }
        msg << "\n\nTemporary WAV kept for debugging:\n" << tempWav.getFullPathName();
        return juce::Result::fail(msg);
    }

    if (!mp3OutputFile.existsAsFile() || mp3OutputFile.getSize() == 0)
    {
        juce::String msg = "MP3 output file was not created.";
        if (lameStderr.isNotEmpty())
        {
            msg << "\n\n" << lameStderr;
        }
        msg << "\n\nTemporary WAV kept for debugging:\n" << tempWav.getFullPathName();
        return juce::Result::fail(msg);
    }

    (void)tempWav.deleteFile();
    appendMixdownDiagnosticLine("mp3 export ok path=\"" + mp3OutputFile.getFullPathName() + "\"");
    return juce::Result::ok();
}

} // namespace mini_daw_audio_mixdown
