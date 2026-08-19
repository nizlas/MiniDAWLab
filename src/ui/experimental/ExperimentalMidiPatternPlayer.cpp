#include "ExperimentalMidiPatternPlayer.h"
#include "plugins/ExperimentalInstrumentHost.h"

#include "diagnostics/DiagnosticBuildFlags.h"

#include <cmath>

// `experimental-instrument.log` (midi-editor lines): `writeMidiEditorLogLine` touches the file only when
// MINIDAW_DIAG_INSTRUMENT_LIFECYCLE != 0 at compile time (default **`0`** in DiagnosticBuildFlags.h).

namespace
{
#if MINIDAW_DIAG_INSTRUMENT_LIFECYCLE
    [[nodiscard]] juce::File getExperimentalInstrumentLogFile()
    {
        return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
            .getChildFile("MiniDAWLab")
            .getChildFile("experimental-instrument.log");
    }
#endif
} // namespace

void ExperimentalMidiPatternPlayer::writeMidiEditorLogLine(const juce::String& message)
{
#if !MINIDAW_DIAG_INSTRUMENT_LIFECYCLE
    (void)message;
#else
    try
    {
        const juce::File f = getExperimentalInstrumentLogFile();
        if (!f.getParentDirectory().isDirectory())
        {
            (void)f.getParentDirectory().createDirectory();
        }
        const juce::String line = juce::Time::getCurrentTime().toISO8601(true) + " " + message + "\n";
        (void)f.appendText(line);
    }
    catch (...)
    {
    }
#endif
}

ExperimentalMidiPatternPlayer::ExperimentalMidiPatternPlayer(ExperimentalInstrumentHost& host,
                                                             ExperimentalMidiPattern& pattern) noexcept
    : host_(host)
    , pattern_(pattern)
{
}

void ExperimentalMidiPatternPlayer::startPlayback()
{
    if (juce::MessageManager::getInstanceWithoutCreating() == nullptr
        || !juce::MessageManager::getInstance()->isThisTheMessageThread())
    {
        return;
    }
    const bool hasNow = playbackAllowed_ ? playbackAllowed_() : host_.hasInstrument();
    writeMidiEditorLogLine(juce::String("midi-editor: play requested hasInstrument=") + (hasNow ? "true" : "false"));
    if (!hasNow)
    {
        writeMidiEditorLogLine("midi-editor: play blocked reason=no-instrument");
        if (playbackUiCallback_)
        {
            playbackUiCallback_();
        }
        return;
    }

    flushAllSound("restart");
    playing_ = true;
    lastEmittedStep_ = -1;
    playStartMs_ = juce::Time::getMillisecondCounterHiRes();
    writeMidiEditorLogLine("midi-editor: play start currentInstrument=\"" + host_.getInstrumentNameForUi() + "\"");
    startTimer(4);
    if (playbackUiCallback_)
    {
        playbackUiCallback_();
    }
}

void ExperimentalMidiPatternPlayer::stopPlayback(const char* reason)
{
    if (juce::MessageManager::getInstanceWithoutCreating() == nullptr
        || !juce::MessageManager::getInstance()->isThisTheMessageThread())
    {
        return;
    }

    const bool wasPlaying = playing_;
    playing_ = false;
    stopTimer();
    flushAllSound(reason);
    lastEmittedStep_ = -1;
    if (wasPlaying)
    {
        writeMidiEditorLogLine(juce::String{ "midi-editor: play stop reason=" }
                               + (reason != nullptr ? reason : ""));
    }
    if (playbackUiCallback_)
    {
        playbackUiCallback_();
    }
}

float ExperimentalMidiPatternPlayer::getPlayheadNormalized() const noexcept
{
    if (!playing_)
    {
        return 0.0f;
    }
    const double loopDur = pattern_.loopDurationMs();
    if (loopDur <= 0.0)
    {
        return 0.0f;
    }
    const double now = juce::Time::getMillisecondCounterHiRes();
    const double elapsed = now - playStartMs_;
    if (!pattern_.loop && elapsed >= loopDur)
    {
        return 1.0f;
    }
    const double frac = std::fmod(elapsed, loopDur) / loopDur;
    return juce::jlimit(0.0f, 1.0f, (float)frac);
}

void ExperimentalMidiPatternPlayer::drainNoteOffs(const double nowMs)
{
    while (!pendingOffs_.empty() && pendingOffs_.top().dueMs <= nowMs)
    {
        const PendingOff p = pendingOffs_.top();
        pendingOffs_.pop();
        if (host_.hasInstrument())
        {
            host_.enqueueMidiMessageFromMessageThread(juce::MidiMessage::noteOff(p.channel, p.midiNote, 0.0f));
        }
    }
}

void ExperimentalMidiPatternPlayer::previewSingleNote(const int midiNote, const int velocity, const int channel)
{
    previewNotesChord({ PreviewNoteRequest{ midiNote, velocity, channel } });
}

void ExperimentalMidiPatternPlayer::previewNotesChord(const std::vector<PreviewNoteRequest>& notes)
{
    if (juce::MessageManager::getInstanceWithoutCreating() == nullptr
        || !juce::MessageManager::getInstance()->isThisTheMessageThread())
    {
        return;
    }
    const bool canSend = playbackAllowed_ ? playbackAllowed_() : host_.hasInstrument();
    if (!canSend || notes.empty())
    {
        return;
    }

    const double nowMs = juce::Time::getMillisecondCounterHiRes();
    for (const auto& req : notes)
    {
        const int pitch = juce::jlimit(0, 127, req.midiNote);
        const int vel = juce::jlimit(1, 127, req.velocity);
        const int ch = juce::jlimit(1, 16, req.channel);
        // Note Off first: a rapid re-audition restarts the attack instead of stacking voices.
        host_.enqueueMidiMessageFromMessageThread(juce::MidiMessage::noteOff(ch, pitch, 0.0f));
        host_.enqueueMidiMessageFromMessageThread(
            juce::MidiMessage::noteOn(ch, pitch, (float)vel / 127.0f));
        PendingOff off;
        off.dueMs = nowMs + kDrumGateMs;
        off.midiNote = pitch;
        off.channel = ch;
        pendingOffs_.push(off);
    }

    // Audition works while pattern playback is stopped: the timer delivers the gated note-offs
    // and stops itself once the queue is empty (see timerCallback).
    if (!isTimerRunning())
    {
        startTimer(4);
    }
}

void ExperimentalMidiPatternPlayer::emitStepIfNeeded(const double nowMs,
                                                       const double stepMs,
                                                       const int numSteps)
{
    if (numSteps <= 0 || stepMs <= 0.0)
    {
        return;
    }

    const double elapsed = nowMs - playStartMs_;
    if (!pattern_.loop)
    {
        const double loopDur = stepMs * (double)numSteps;
        if (elapsed >= loopDur)
        {
            stopPlayback("pattern-end");
            return;
        }
    }

    const int rawStep = (int)(elapsed / stepMs);
    const int step = ((rawStep % numSteps) + numSteps) % numSteps;

    if (step == lastEmittedStep_)
    {
        return;
    }

    lastEmittedStep_ = step;

    for (const auto& n : pattern_.notes)
    {
        if (n.step != step)
        {
            continue;
        }
        const bool canSend = playbackAllowed_ ? playbackAllowed_() : host_.hasInstrument();
        if (!canSend)
        {
            stopPlayback("instrument-unloaded");
            return;
        }
        const int vel = juce::jlimit(1, 127, n.velocity);
        const float vf = (float)vel / 127.0f;
        host_.enqueueMidiMessageFromMessageThread(
            juce::MidiMessage::noteOn(kMidiChannel, n.midiNote, vf));
        PendingOff off;
        off.dueMs = nowMs + kDrumGateMs;
        off.midiNote = n.midiNote;
        pendingOffs_.push(off);
    }
}

void ExperimentalMidiPatternPlayer::flushAllSound(const char* reason)
{
    juce::ignoreUnused(reason);
    while (!pendingOffs_.empty())
    {
        const PendingOff p = pendingOffs_.top();
        pendingOffs_.pop();
        if (host_.hasInstrument())
        {
            host_.enqueueMidiMessageFromMessageThread(juce::MidiMessage::noteOff(p.channel, p.midiNote, 0.0f));
        }
    }
    if (host_.hasInstrument())
    {
        host_.enqueueMidiMessageFromMessageThread(juce::MidiMessage::allNotesOff(kMidiChannel));
    }
}

void ExperimentalMidiPatternPlayer::timerCallback()
{
    const double nowMs = juce::Time::getMillisecondCounterHiRes();

    if (!playing_)
    {
        // Audition-only mode: deliver gated note-offs, then go idle.
        drainNoteOffs(nowMs);
        if (pendingOffs_.empty())
        {
            stopTimer();
        }
        return;
    }
    if (!host_.hasInstrument())
    {
        stopPlayback("instrument-unloaded");
        return;
    }

    drainNoteOffs(nowMs);

    const double stepMs = pattern_.stepDurationMs();
    emitStepIfNeeded(nowMs, stepMs, pattern_.numSteps);
}
