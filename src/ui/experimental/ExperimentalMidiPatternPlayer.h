#pragma once

#include "MidiEditorAuditionModel.h"

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_events/juce_events.h>

#include <functional>
#include <vector>

class ExperimentalInstrumentHost;

/// MIDI editor audition engine: gesture-driven note previews delivered through the editor's bound
/// `ExperimentalInstrumentHost` (for a `TrackKind::Midi` row that is the routed `MIDI To`
/// *destination* host, so previews take the destination's normal audio path). Timing/ownership
/// rules live in `midi_audition::AuditionScheduler` (pure, fake-clock-testable); this class adds
/// the real clock, the drain timer and host delivery. Destroying the player releases every active
/// preview at the destination/channel captured at Note On — no stuck notes across rebinds or
/// destination changes. (The legacy step-sequencer playback that used to live here was removed;
/// timeline playback goes through the main transport.)
class ExperimentalMidiPatternPlayer final : public juce::Timer
{
public:
    static constexpr int kMidiChannel = 1;

    /// Production: deliver through the bound destination host's message-thread MIDI queue
    /// (`enqueueMidiMessageFromMessageThread`); availability = `host.hasInstrument()` unless a
    /// `setPlaybackAllowed` gate overrides it. Defined in
    /// `ExperimentalMidiPatternPlayerHostBinding.cpp` (app target only) so the scheduling and
    /// dispatch below can be linked into `MiniDAWSelftests` without the plugin host.
    explicit ExperimentalMidiPatternPlayer(ExperimentalInstrumentHost& host) noexcept;

    /// Deterministic integration seam (selftests): the SAME scheduling/dispatch code as the live
    /// editor, but with an injected clock and a MIDI-capture delivery boundary. No host, no real
    /// timer (tests advance `nowMs` and call `timerCallback()` directly), no message loop — the
    /// message-thread gates are bypassed because the seam is single-threaded by construction.
    struct TestSeams
    {
        std::function<double()> nowMs;
        std::function<void(const juce::MidiMessage&)> deliver;
    };
    explicit ExperimentalMidiPatternPlayer(TestSeams seams) noexcept;

    ~ExperimentalMidiPatternPlayer() override;

    /// One audition entry (velocity-drag chord rattle).
    struct PreviewNoteRequest
    {
        int midiNote = 60;
        int velocity = 100;
        int channel = kMidiChannel;
        int offVelocity = 64;
    };

    // --- Gesture previews (Phase B.1 semantics; see MidiEditorAuditionModel.h) ---
    /// [Message thread] Arranged-note preview: Note On now; sounds for at least
    /// `midi_audition::kArrangedAuditionMs`, longer holds extend until `endNotePreview`.
    void beginArrangedNotePreview(int midiNote, int velocity, int channel, int offVelocity);
    /// [Message thread] Piano-key / drum-row preview: Note On now, Note Off exactly at
    /// `endNotePreview` (no minimum duration).
    void beginHeldKeyPreview(int midiNote, int velocity, int channel, int offVelocity);
    /// [Message thread] Mouse Up for either preview kind (keyed by channel + pitch).
    void endNotePreview(int midiNote, int channel);
    /// [Message thread] Stage D audition chase: deliver one Control Change immediately. Callers
    /// invoke this BEFORE `beginArrangedNotePreview` — the host's message-thread MIDI queue is
    /// FIFO, so the controller value is active before the preview's Note On. `channel` is the
    /// EFFECTIVE channel (same source semantics as transport playback).
    void sendControllerChangeNow(int channel, int controller, int value);
    /// [Message thread] One-shot preview with the default arranged audition duration
    /// (create-note feedback; no Mouse Up tracking).
    void oneShotArrangedPreview(int midiNote, int velocity, int channel, int offVelocity);
    /// [Message thread] Velocity-drag rattle: short-gate one-shots for the same-start chord.
    void previewNotesChord(const std::vector<PreviewNoteRequest>& notes);
    /// [Message thread] Immediate Note Off for every active preview at its captured
    /// destination/channel (editor close, teardown). Also runs on destruction.
    void releaseAllActivePreviews();
    /// [Message thread] Focus-loss safety: immediate Note Off only for previews still waiting for
    /// Mouse Up; quick-click previews keep their already-scheduled safe Note Off.
    void releaseHeldPreviewsForFocusLoss();

    /// When set, audition consults this instead of `host.hasInstrument()` (I3b clip gate; Phase
    /// B.1 destination-aware gate for `TrackKind::Midi` rows).
    void setPlaybackAllowed(std::function<bool()> fn) noexcept { playbackAllowed_ = std::move(fn); }

    void timerCallback() override;

    static void writeMidiEditorLogLine(const juce::String& message);

private:
    [[nodiscard]] bool canSendPreviewNow() const;
    [[nodiscard]] bool callerThreadAllowed() const;
    [[nodiscard]] double currentTimeMs() const;
    void emitAndPump(const std::vector<midi_audition::AuditionEvent>& events);
    void deliverEvents(const std::vector<midi_audition::AuditionEvent>& events);

    /// Delivery boundary: host queue in production, MIDI capture in the test seam.
    std::function<void(const juce::MidiMessage&)> deliver_;
    /// Default availability gate when `playbackAllowed_` is unset (`hasInstrument()` in production).
    std::function<bool()> deliveryAvailable_;
    /// Injected clock (test seam only); null = `juce::Time::getMillisecondCounterHiRes()`.
    std::function<double()> testClock_;
    /// True in the test seam: no message loop exists, so thread gates and the drain timer are
    /// bypassed (tests advance the clock and invoke `timerCallback()` themselves).
    bool testMode_ = false;

    midi_audition::AuditionScheduler scheduler_;
    std::function<bool()> playbackAllowed_;
};
