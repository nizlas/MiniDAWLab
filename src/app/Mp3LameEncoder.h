#pragma once

// =============================================================================
// Mp3LameEncoder — runs the bundled LAME child process for the MP3 mixdown
// =============================================================================
//
// ROLE
//   The MP3 export renders a WAV first and then encodes it with the external `lame.exe`. This
//   module owns everything about that child process: argument construction, the console-output
//   drain that keeps LAME from blocking, progress reporting, cooperative cancel, the safety
//   timeout, and the exit-code / output-file checks. It never touches Session, Transport or the
//   audio engine — it is a pure "file in, file out" step of the export pipeline.
//
// WHY A DRAIN THREAD EXISTS (the 1.1.4 export hang)
//   LAME writes its banner and a progress line to stderr for every ~100 frames. For a 40 s file
//   that is ~6.5 KB — more than the anonymous pipe Windows gives a child process (4 KiB). The
//   old code captured stderr but never read it while waiting, so LAME blocked on a full pipe at
//   the very end of the encode and the export sat in "Encoding MP3..." until a 10-minute timeout
//   killed it. `LameConsoleDrain` reads the pipe continuously on a small helper thread, so LAME
//   can always finish; the drained text also yields the real percentage for the progress bar.
//
// THREADING
//   `runLameMp3EncodeBlocking` is [Message thread] and blocks until LAME exits, is cancelled, or
//   times out. The drain thread is created and joined inside that call; it touches only the
//   `juce::ChildProcess` pipe and a mutex-protected byte buffer. No audio-thread interaction.
// =============================================================================

#include "app/AudioMixdownExporter.h"

#include <juce_core/juce_core.h>

#include <optional>

namespace mini_daw_audio_mixdown
{

struct Mp3LameEncodeRequest
{
    juce::File lameExecutable;
    /// Complete, closed WAV file to encode.
    juce::File inputWav;
    /// Written by LAME; callers pass their own working file and move it into place afterwards.
    juce::File outputMp3;
    int bitrateKbps = 192;
    /// Audio duration for the size-based progress fallback (0 = unknown → indeterminate until
    /// LAME's own percentage appears in its console output).
    double expectedDurationSeconds = 0.0;
    /// Safety net only; LAME can no longer deadlock on its console pipe, so a healthy encode
    /// never gets near this.
    int timeoutMs = 600000;
};

struct Mp3LameEncodeOutcome
{
    bool cancelled = false;
    bool timedOut = false;
    bool processStarted = false;
    int exitCode = -1;
    /// Tail of LAME's console output (bounded), used for error messages and diagnostics.
    juce::String consoleTail;
};

/// [Message thread] Encodes `request.inputWav` to `request.outputMp3` with CBR `bitrateKbps`.
/// Reports "Encoding MP3... NN%" through `sink` (nullable) and honours `sink->isMixdownCancelRequested()`
/// — a cancel terminates LAME and deletes its partial output. Returns ok() only when LAME exited
/// with code 0 and produced a non-empty file; every failure path leaves no `outputMp3` behind.
[[nodiscard]] juce::Result runLameMp3EncodeBlocking(const Mp3LameEncodeRequest& request,
                                                    MixdownProgressSink* sink,
                                                    Mp3LameEncodeOutcome& outcome);

/// Pure helper: the most recent "( NN%)" token in LAME's console text, if any. LAME prints its
/// progress as e.g. "  1600/1667  ( 96%)|..."; the text may contain many such lines separated by
/// carriage returns, so the LAST token is the current state.
[[nodiscard]] std::optional<int> parseLatestLameProgressPercent(const juce::String& consoleText) noexcept;

/// Pure helper: expected CBR MP3 size for the size-based progress fallback (bytes), 0 if unknown.
[[nodiscard]] juce::int64 expectedCbrMp3SizeBytes(double durationSeconds, int bitrateKbps) noexcept;

} // namespace mini_daw_audio_mixdown
