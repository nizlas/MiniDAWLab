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

ExperimentalMidiPatternPlayer::ExperimentalMidiPatternPlayer(ExperimentalInstrumentHost& host) noexcept
    : host_(host)
{
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

void ExperimentalMidiPatternPlayer::timerCallback()
{
    // Audition-only: deliver gated note-offs, then go idle.
    drainNoteOffs(juce::Time::getMillisecondCounterHiRes());
    if (pendingOffs_.empty())
    {
        stopTimer();
    }
}
