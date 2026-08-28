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

/// A non-existing unique sibling of `destination` (same folder, so the final move is a cheap
/// same-volume rename). Returns an invalid File if no free name could be found.
[[nodiscard]] juce::File allocateUniqueSiblingTempFile(const juce::File& destination,
                                                       const juce::String& tag,
                                                       const juce::String& extension)
{
    for (int attempt = 0; attempt < 16; ++attempt)
    {
        (void)attempt;
        const juce::String unique
            = juce::String::toHexString(juce::Random::getSystemRandom().nextInt64());
        const juce::File candidate = destination.getSiblingFile(
            destination.getFileNameWithoutExtension() + tag + unique + extension);
        if (!candidate.existsAsFile())
        {
            return candidate;
        }
    }
    return {};
}

/// Safe-overwrite finalize: the fully rendered/encoded `tempFile` replaces `destination`. The old
/// destination is only deleted *after* the new content exists completely, so a failed export can
/// never destroy the previous file. Logs every step; on failure the temp is removed.
[[nodiscard]] juce::Result replaceDestinationWithRenderedTemp(const juce::File& tempFile,
                                                              const juce::File& destination)
{
    if (destination.existsAsFile() && !destination.deleteFile())
    {
        appendMixdownDiagnosticLine("FAIL could not delete existing output for replace (locked?) path=\""
                                    + destination.getFullPathName() + "\"");
        (void)tempFile.deleteFile();
        return juce::Result::fail(
            "Could not replace the existing file (it may be open in another program):\n"
            + destination.getFullPathName());
    }
    if (!tempFile.moveFileTo(destination))
    {
        appendMixdownDiagnosticLine("FAIL could not move rendered temp into place temp=\""
                                    + tempFile.getFullPathName() + "\"");
        (void)tempFile.deleteFile();
        return juce::Result::fail("Could not move the rendered file into place:\n"
                                  + destination.getFullPathName());
    }
    if (!destination.existsAsFile() || destination.getSize() <= 0)
    {
        appendMixdownDiagnosticLine("FAIL final output missing or empty after replace path=\""
                                    + destination.getFullPathName() + "\"");
        return juce::Result::fail("Export finished but the output file is missing or empty:\n"
                                  + destination.getFullPathName());
    }
    appendMixdownDiagnosticLine(
        "replace ok final path=\"" + destination.getFullPathName() + "\" size="
        + juce::String(destination.getSize()) + " mtime="
        + destination.getLastModificationTime().toISO8601(true));
    return juce::Result::ok();
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
    const bool destinationExisted = request.outputFile.existsAsFile();
    appendMixdownDiagnosticLine(juce::String("wav destination existed=")
                                + (destinationExisted ? "yes" : "no")
                                + " overwriteConfirmed=" + (request.overwriteConfirmed ? "yes" : "no"));
    if (destinationExisted && !request.overwriteConfirmed)
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

    // Safe overwrite: render into a unique sibling temp file, then replace the destination only
    // after the render fully succeeded. The old file survives any render/disk failure, and
    // FileOutputStream never sees an existing file (it would append to one).
    const juce::File renderTempFile
        = allocateUniqueSiblingTempFile(request.outputFile, ".__dal_wav_render_", ".wav");
    if (renderTempFile == juce::File{})
    {
        return juce::Result::fail("Could not allocate a temporary render file path.");
    }
    appendMixdownDiagnosticLine("wav render temp=\"" + renderTempFile.getFullPathName() + "\"");

    auto fileStream = std::make_unique<juce::FileOutputStream>(renderTempFile);
    if (fileStream->failedToOpen())
    {
        appendMixdownDiagnosticLine("FAIL could not open render temp for writing");
        return juce::Result::fail("Could not open file for writing:\n"
                                  + renderTempFile.getFullPathName());
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
        (void)renderTempFile.deleteFile();
        return juce::Result::fail("Could not create WAV writer for the requested format.");
    }

    // Closes the writer (releasing the file handle) and removes the temp; the destination is
    // untouched on every one of these failure paths.
    const auto failAndDiscardTemp = [&writer, &renderTempFile](const juce::String& message) {
        writer.reset();
        (void)renderTempFile.deleteFile();
        return juce::Result::fail(message);
    };

    if (request.bits == MixdownWaveBits::IeeeFloat32)
    {
        if (!writer->isFloatingPoint())
        {
            return failAndDiscardTemp(
                "This build cannot write IEEE float WAV (writer is not floating-point).");
        }
        if (writer->getBitsPerSample() != 32)
        {
            return failAndDiscardTemp("Unexpected WAV writer bit depth for float export.");
        }
    }
    else
    {
        if (writer->isFloatingPoint())
        {
            return failAndDiscardTemp("WAV writer unexpectedly reported floating-point for PCM export.");
        }
        if (writer->getBitsPerSample() != bits)
        {
            return failAndDiscardTemp("WAV writer bit depth does not match the requested format.");
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
            return failAndDiscardTemp("Disk write failed during mixdown.");
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

    // Only now (render fully succeeded, file handle closed) does the old destination get replaced.
    const juce::Result replaceResult
        = replaceDestinationWithRenderedTemp(renderTempFile, request.outputFile);
    if (replaceResult.failed())
    {
        return replaceResult;
    }
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
    const bool destinationExisted = mp3OutputFile.existsAsFile();
    appendMixdownDiagnosticLine(juce::String("mp3 destination existed=")
                                + (destinationExisted ? "yes" : "no")
                                + " overwriteConfirmed=" + (overwriteConfirmed ? "yes" : "no"));
    if (destinationExisted && !overwriteConfirmed)
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

    const juce::File tempWav
        = allocateUniqueSiblingTempFile(mp3OutputFile, ".__dal_mp3_source_", ".wav");
    if (tempWav == juce::File{})
    {
        return juce::Result::fail("Could not allocate a temporary WAV file path.");
    }
    // LAME encodes into a temp MP3 too; the real destination is only replaced after a successful
    // encode, so a LAME failure can never leave the old MP3 half-overwritten or deleted.
    const juce::File tempMp3
        = allocateUniqueSiblingTempFile(mp3OutputFile, ".__dal_mp3_encode_", ".mp3");
    if (tempMp3 == juce::File{})
    {
        return juce::Result::fail("Could not allocate a temporary MP3 file path.");
    }
    appendMixdownDiagnosticLine("mp3 temps wav=\"" + tempWav.getFullPathName() + "\" mp3=\""
                                + tempMp3.getFullPathName() + "\"");

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
    args.add(tempMp3.getFullPathName());

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
        (void)tempMp3.deleteFile();
        const juce::String kept = "\n\nTemporary WAV kept for debugging:\n" + tempWav.getFullPathName();
        return juce::Result::fail("MP3 encoding timed out." + kept);
    }

    const auto exitCode = static_cast<int>(lameProcess.getExitCode());
    const juce::String lameStderr = lameProcess.readAllProcessOutput().trim();
    appendMixdownDiagnosticLine("lame complete exitCode=" + juce::String(exitCode));

    if (exitCode != 0)
    {
        (void)tempMp3.deleteFile();
        juce::String msg = "MP3 encoding failed.";
        if (lameStderr.isNotEmpty())
        {
            msg << "\n\n" << lameStderr;
        }
        msg << "\n\nTemporary WAV kept for debugging:\n" << tempWav.getFullPathName();
        return juce::Result::fail(msg);
    }

    if (!tempMp3.existsAsFile() || tempMp3.getSize() == 0)
    {
        (void)tempMp3.deleteFile();
        juce::String msg = "MP3 output file was not created.";
        if (lameStderr.isNotEmpty())
        {
            msg << "\n\n" << lameStderr;
        }
        msg << "\n\nTemporary WAV kept for debugging:\n" << tempWav.getFullPathName();
        return juce::Result::fail(msg);
    }

    (void)tempWav.deleteFile();

    // Encode fully succeeded; only now is the old destination replaced (see WAV path).
    const juce::Result replaceResult = replaceDestinationWithRenderedTemp(tempMp3, mp3OutputFile);
    if (replaceResult.failed())
    {
        return replaceResult;
    }
    appendMixdownDiagnosticLine("mp3 export ok path=\"" + mp3OutputFile.getFullPathName() + "\"");
    return juce::Result::ok();
}

} // namespace mini_daw_audio_mixdown
