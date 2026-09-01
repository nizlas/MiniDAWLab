// NOTE: deliberately no ExperimentalInstrumentHost include/reference here — this TU is also
// compiled into MiniDAWSelftests. The production host binding (enqueue + hasInstrument) lives in
// ExperimentalMidiPatternPlayerHostBinding.cpp, app target only.
#include "ExperimentalMidiPatternPlayer.h"

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

    [[nodiscard]] bool isOnMessageThread() noexcept
    {
        return juce::MessageManager::getInstanceWithoutCreating() != nullptr
               && juce::MessageManager::getInstance()->isThisTheMessageThread();
    }
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

ExperimentalMidiPatternPlayer::ExperimentalMidiPatternPlayer(TestSeams seams) noexcept
    : deliver_(std::move(seams.deliver))
    , deliveryAvailable_([] { return true; })
    , testClock_(std::move(seams.nowMs))
    , testMode_(true)
{
}

ExperimentalMidiPatternPlayer::~ExperimentalMidiPatternPlayer()
{
    // Editor close / rebind / destination change: release at the host and channels captured at
    // Note On, so a preview can never outlive its owner as a stuck note.
    releaseAllActivePreviews();
}

bool ExperimentalMidiPatternPlayer::callerThreadAllowed() const
{
    return testMode_ || isOnMessageThread();
}

double ExperimentalMidiPatternPlayer::currentTimeMs() const
{
    return testClock_ ? testClock_() : juce::Time::getMillisecondCounterHiRes();
}

bool ExperimentalMidiPatternPlayer::canSendPreviewNow() const
{
    if (!callerThreadAllowed())
    {
        return false;
    }
    if (playbackAllowed_)
    {
        return playbackAllowed_();
    }
    return deliveryAvailable_ ? deliveryAvailable_() : false;
}

void ExperimentalMidiPatternPlayer::deliverEvents(const std::vector<midi_audition::AuditionEvent>& events)
{
    if (!deliver_)
    {
        return;
    }
    for (const auto& ev : events)
    {
        if (ev.noteOn)
        {
            deliver_(juce::MidiMessage::noteOn(ev.channel, ev.pitch, (float)ev.velocity / 127.0f));
        }
        else
        {
            deliver_(juce::MidiMessage::noteOff(ev.channel, ev.pitch, (float)ev.velocity / 127.0f));
        }
    }
}

void ExperimentalMidiPatternPlayer::emitAndPump(const std::vector<midi_audition::AuditionEvent>& events)
{
    deliverEvents(events);
    if (testMode_)
    {
        // No message loop in the deterministic seam: the test advances the clock and calls
        // timerCallback() itself.
        return;
    }
    // The drain timer only needs to run while a scheduled or held Note Off is outstanding.
    if (scheduler_.hasActivePreviews())
    {
        if (!isTimerRunning())
        {
            startTimer(4);
        }
    }
    else if (isTimerRunning())
    {
        stopTimer();
    }
}

void ExperimentalMidiPatternPlayer::beginArrangedNotePreview(const int midiNote,
                                                             const int velocity,
                                                             const int channel,
                                                             const int offVelocity)
{
    if (!canSendPreviewNow())
    {
        return;
    }
    std::vector<midi_audition::AuditionEvent> emitNow;
    scheduler_.beginPreview(channel, midiNote, velocity, offVelocity,
                            currentTimeMs(),
                            midi_audition::kArrangedAuditionMs, emitNow);
    emitAndPump(emitNow);
}

void ExperimentalMidiPatternPlayer::beginHeldKeyPreview(const int midiNote,
                                                        const int velocity,
                                                        const int channel,
                                                        const int offVelocity)
{
    if (!canSendPreviewNow())
    {
        return;
    }
    std::vector<midi_audition::AuditionEvent> emitNow;
    scheduler_.beginPreview(channel, midiNote, velocity, offVelocity,
                            currentTimeMs(), 0.0, emitNow);
    emitAndPump(emitNow);
}

void ExperimentalMidiPatternPlayer::sendControllerChangeNow(const int channel,
                                                            const int controller,
                                                            const int value)
{
    if (!canSendPreviewNow())
    {
        return;
    }
    if (deliver_)
    {
        deliver_(juce::MidiMessage::controllerEvent(
            juce::jlimit(1, 16, channel), juce::jlimit(0, 127, controller),
            juce::jlimit(0, 127, value)));
    }
}

void ExperimentalMidiPatternPlayer::endNotePreview(const int midiNote, const int channel)
{
    if (!callerThreadAllowed())
    {
        return;
    }
    // No gate check: a preview that managed to start must always be allowed to end.
    const double nowMs = currentTimeMs();
    scheduler_.endPreview(channel, midiNote, nowMs);
    std::vector<midi_audition::AuditionEvent> emitNow;
    scheduler_.drainDue(nowMs, emitNow);
    emitAndPump(emitNow);
}

void ExperimentalMidiPatternPlayer::oneShotArrangedPreview(const int midiNote,
                                                           const int velocity,
                                                           const int channel,
                                                           const int offVelocity)
{
    if (!canSendPreviewNow())
    {
        return;
    }
    std::vector<midi_audition::AuditionEvent> emitNow;
    scheduler_.oneShotPreview(channel, midiNote, velocity, offVelocity,
                              currentTimeMs(),
                              midi_audition::kArrangedAuditionMs, emitNow);
    emitAndPump(emitNow);
}

void ExperimentalMidiPatternPlayer::previewNotesChord(const std::vector<PreviewNoteRequest>& notes)
{
    if (!canSendPreviewNow() || notes.empty())
    {
        return;
    }
    const double nowMs = currentTimeMs();
    std::vector<midi_audition::AuditionEvent> emitNow;
    for (const auto& req : notes)
    {
        scheduler_.oneShotPreview(req.channel, req.midiNote, req.velocity, req.offVelocity, nowMs,
                                  midi_audition::kVelocityRattleMs, emitNow);
    }
    emitAndPump(emitNow);
}

void ExperimentalMidiPatternPlayer::releaseHeldPreviewsForFocusLoss()
{
    std::vector<midi_audition::AuditionEvent> emitNow;
    scheduler_.releaseHeldActive(emitNow);
    emitAndPump(emitNow);
}

void ExperimentalMidiPatternPlayer::releaseAllActivePreviews()
{
    std::vector<midi_audition::AuditionEvent> emitNow;
    scheduler_.releaseAllActive(emitNow);
    deliverEvents(emitNow);
    if (isTimerRunning())
    {
        stopTimer();
    }
}

void ExperimentalMidiPatternPlayer::timerCallback()
{
    std::vector<midi_audition::AuditionEvent> emitNow;
    scheduler_.drainDue(currentTimeMs(), emitNow);
    emitAndPump(emitNow);
}
