// =============================================================================
// Mp3LameEncoder.cpp — LAME child process: drained console, progress, cancel, timeout
// =============================================================================
// See the header for the role of this file and why the console drain exists.
// [Message thread] throughout, except `LameConsoleDrain::run` (its own helper thread).
// =============================================================================

#include "app/Mp3LameEncoder.h"

#include "diagnostics/StabilityDiagnosticLog.h"

#include <juce_events/juce_events.h>

#include <algorithm>
#include <string>

namespace mini_daw_audio_mixdown
{

namespace
{

/// Reads LAME's console pipe continuously so the child can never block on a full pipe.
///
/// Producer/consumer contract: this thread is the only reader of the `ChildProcess` pipe and the
/// only writer of `tail_`; the message thread reads `tail_` under `lock_` for progress parsing and
/// error text. The buffer keeps only the last `kMaxTailBytes` so a very chatty encoder cannot grow
/// memory without bound (the newest text is what both consumers need). `readProcessOutput`
/// returns 0 once the child has exited and the pipe is empty, which ends the thread naturally; a
/// cancelled (killed) child ends it the same way.
class LameConsoleDrain final : public juce::Thread
{
public:
    explicit LameConsoleDrain(juce::ChildProcess& process)
        : juce::Thread("DAL LAME console drain")
        , process_(process)
    {
    }

    ~LameConsoleDrain() override
    {
        // The child is always finished (exited or killed) before the drain is destroyed, so the
        // pending read returns promptly; the timeout is a guard, not an expected wait.
        stopThread(5000);
    }

    void run() override
    {
        // Small chunks keep the progress readout current: JUCE's read returns only when the
        // requested number of bytes arrived (or the child exited), so a large request would sit
        // on partial progress lines.
        char chunk[64];
        while (!threadShouldExit())
        {
            const int numRead = process_.readProcessOutput(chunk, static_cast<int>(sizeof(chunk)));
            if (numRead <= 0)
            {
                return; // child exited and the pipe is drained
            }
            const juce::ScopedLock sl(lock_);
            tail_.append(chunk, static_cast<size_t>(numRead));
            if (tail_.size() > kMaxTailBytes)
            {
                tail_.erase(0, tail_.size() - kMaxTailBytes);
            }
        }
    }

    /// [Message thread] Snapshot of the newest console text.
    [[nodiscard]] juce::String tailText() const
    {
        const juce::ScopedLock sl(lock_);
        return juce::String::fromUTF8(tail_.data(), static_cast<int>(tail_.size()));
    }

private:
    static constexpr size_t kMaxTailBytes = 64 * 1024;

    juce::ChildProcess& process_;
    mutable juce::CriticalSection lock_;
    std::string tail_;
};

/// Terminates a still-running child and waits for the OS to release its output file handle, so
/// the caller can delete the partial MP3 without racing the dying process.
void killAndAwaitExit(juce::ChildProcess& process)
{
    if (process.isRunning())
    {
        (void)process.kill();
        (void)process.waitForProcessToFinish(3000);
    }
}

/// Final progress text for the sink: the percentage is shown only when it is real.
void reportEncodeProgress(MixdownProgressSink* const sink, const double fraction01)
{
    if (sink == nullptr)
    {
        return;
    }
    if (fraction01 >= 0.0)
    {
        const int pct = juce::jlimit(0, 100, juce::roundToInt(fraction01 * 100.0));
        sink->setMixdownProgress("Encoding MP3... " + juce::String(pct) + "%", fraction01);
    }
    else
    {
        sink->setMixdownProgress("Encoding MP3...", -1.0);
    }
}

} // namespace

std::optional<int> parseLatestLameProgressPercent(const juce::String& consoleText) noexcept
{
    // Scan backwards for the last "%)" and read the digits in front of it back to the "(".
    // Anything that does not match the "( NN%)" shape is ignored rather than guessed.
    const std::string text = consoleText.toStdString();
    const size_t percentSign = text.rfind("%)");
    if (percentSign == std::string::npos)
    {
        return std::nullopt;
    }
    const auto isDigit = [](const char c) noexcept { return c >= '0' && c <= '9'; };

    size_t i = percentSign;
    while (i > 0 && text[i - 1] == ' ')
    {
        --i;
    }
    const size_t digitsEnd = i;
    while (i > 0 && isDigit(text[i - 1]))
    {
        --i;
    }
    const size_t digitsBegin = i;
    if (digitsEnd == digitsBegin || digitsEnd - digitsBegin > 3)
    {
        return std::nullopt;
    }
    while (i > 0 && text[i - 1] == ' ')
    {
        --i;
    }
    if (i == 0 || text[i - 1] != '(')
    {
        return std::nullopt;
    }
    const int value = std::stoi(text.substr(digitsBegin, digitsEnd - digitsBegin));
    return juce::jlimit(0, 100, value);
}

juce::int64 expectedCbrMp3SizeBytes(const double durationSeconds, const int bitrateKbps) noexcept
{
    if (!(durationSeconds > 0.0) || bitrateKbps <= 0)
    {
        return 0;
    }
    // kbps → bytes per second is kbps * 1000 / 8 = kbps * 125.
    return static_cast<juce::int64>(durationSeconds * static_cast<double>(bitrateKbps) * 125.0);
}

juce::Result runLameMp3EncodeBlocking(const Mp3LameEncodeRequest& request,
                                      MixdownProgressSink* const sink,
                                      Mp3LameEncodeOutcome& outcome)
{
    outcome = {};

    if (!request.lameExecutable.existsAsFile())
    {
        return juce::Result::fail("MP3 encoder not found. Expected Tools/lame/lame.exe beside the application.");
    }
    if (!request.inputWav.existsAsFile() || request.inputWav.getSize() <= 0)
    {
        return juce::Result::fail("MP3 encoding failed: the rendered WAV is missing or empty.");
    }
    if (request.outputMp3 == juce::File{})
    {
        return juce::Result::fail("MP3 encoding failed: no output path.");
    }
    (void)request.outputMp3.deleteFile();

    // CBR at the chosen bitrate, exactly the command line used since the first MP3 release; the
    // console output stays enabled deliberately because it carries the real progress percentage.
    juce::StringArray args;
    args.add(request.lameExecutable.getFullPathName());
    args.add("-b");
    args.add(juce::String(request.bitrateKbps));
    args.add(request.inputWav.getFullPathName());
    args.add(request.outputMp3.getFullPathName());

    juce::ChildProcess lame;
    appendMixdownDiagnosticLine("lame start exe=\"" + request.lameExecutable.getFullPathName()
                                + "\" kbps=" + juce::String(request.bitrateKbps));
    if (!lame.start(args, juce::ChildProcess::wantStdErr | juce::ChildProcess::wantStdOut))
    {
        appendMixdownDiagnosticLine("FAIL lame could not start");
        return juce::Result::fail("MP3 encoding failed (could not start LAME).");
    }
    outcome.processStarted = true;
    reportEncodeProgress(sink, -1.0);

    // From here on the child owns `outputMp3` until it exits; the drain keeps its pipe empty.
    LameConsoleDrain drain(lame);
    drain.startThread();

    const juce::int64 expectedBytes
        = expectedCbrMp3SizeBytes(request.expectedDurationSeconds, request.bitrateKbps);
    const double startMs = juce::Time::getMillisecondCounterHiRes();
    bool finished = false;
    for (;;)
    {
        // ~50 ms cadence: `waitForProcessToFinish` polls the process handle in 2 ms steps and
        // returns early on exit, so the UI is refreshed about 20 times per second.
        if (lame.waitForProcessToFinish(50))
        {
            finished = true;
            break;
        }
        if (sink != nullptr && sink->isMixdownCancelRequested())
        {
            outcome.cancelled = true;
            break;
        }
        if (juce::Time::getMillisecondCounterHiRes() - startMs >= static_cast<double>(request.timeoutMs))
        {
            outcome.timedOut = true;
            break;
        }

        // Progress: LAME's own "( NN%)" is authoritative; the CBR size estimate covers the moments
        // before the first status line; otherwise the bar shows activity without a number.
        double fraction = -1.0;
        if (const auto pct = parseLatestLameProgressPercent(drain.tailText()); pct.has_value())
        {
            fraction = static_cast<double>(*pct) / 100.0;
        }
        else if (expectedBytes > 0)
        {
            const juce::int64 written = request.outputMp3.existsAsFile() ? request.outputMp3.getSize() : 0;
            fraction = juce::jlimit(0.0, 0.99, static_cast<double>(written) / static_cast<double>(expectedBytes));
        }
        reportEncodeProgress(sink, fraction);
    }

    if (!finished)
    {
        killAndAwaitExit(lame);
    }
    drain.stopThread(5000);
    outcome.consoleTail = drain.tailText().trim();
    outcome.exitCode = finished ? static_cast<int>(lame.getExitCode()) : -1;

    if (outcome.cancelled)
    {
        appendMixdownDiagnosticLine("lame cancelled by user; partial output removed");
        (void)request.outputMp3.deleteFile();
        return juce::Result::fail("Export cancelled.");
    }
    if (outcome.timedOut)
    {
        appendMixdownDiagnosticLine("FAIL lame timed out");
        (void)request.outputMp3.deleteFile();
        return juce::Result::fail("MP3 encoding timed out.");
    }

    appendMixdownDiagnosticLine("lame complete exitCode=" + juce::String(outcome.exitCode)
                                + " consoleBytes=" + juce::String(outcome.consoleTail.length()));
    if (outcome.exitCode != 0)
    {
        (void)request.outputMp3.deleteFile();
        juce::String msg = "MP3 encoding failed (LAME exit code " + juce::String(outcome.exitCode) + ").";
        // The last console lines are where LAME explains itself (unsupported format, disk full...).
        juce::StringArray lines;
        lines.addLines(outcome.consoleTail.replaceCharacter('\r', '\n'));
        lines.removeEmptyStrings(true);
        const int keep = juce::jmin(4, lines.size());
        if (keep > 0)
        {
            msg << "\n\n";
            for (int i = lines.size() - keep; i < lines.size(); ++i)
            {
                msg << lines[i].trim() << "\n";
            }
        }
        return juce::Result::fail(msg.trimEnd());
    }
    if (!request.outputMp3.existsAsFile() || request.outputMp3.getSize() <= 0)
    {
        (void)request.outputMp3.deleteFile();
        return juce::Result::fail("MP3 output file was not created.");
    }
    if (sink != nullptr)
    {
        sink->setMixdownProgress("Encoding MP3... 100%", 1.0);
    }
    return juce::Result::ok();
}

} // namespace mini_daw_audio_mixdown
