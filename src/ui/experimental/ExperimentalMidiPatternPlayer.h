#pragma once

#include <juce_events/juce_events.h>

#include <functional>
#include <queue>
#include <string>
#include <vector>

class ExperimentalInstrumentHost;

/// MIDI editor audition engine: one-shot note previews with a message-thread gated Note Off.
/// (The legacy step-sequencer playback that used to live here was removed; timeline playback
/// goes through the main transport.)
class ExperimentalMidiPatternPlayer final : public juce::Timer
{
public:
    static constexpr int kMidiChannel = 1;
    static constexpr double kDrumGateMs = 100.0;

    explicit ExperimentalMidiPatternPlayer(ExperimentalInstrumentHost& host) noexcept;

    /// One audition entry (MIDI editor note click / velocity-drag preview / key-strip click).
    struct PreviewNoteRequest
    {
        int midiNote = 60;
        int velocity = 100;
        int channel = kMidiChannel;
    };

    /// [Message thread] One-shot audition: Note On now, gated Note Off after `kDrumGateMs`. Sends a
    /// Note Off first so rapid re-auditions restart the attack. Routes through the loaded instrument
    /// only (no transport/timeline side effects); silent no-op when no instrument is loaded.
    void previewSingleNote(int midiNote, int velocity, int channel = kMidiChannel);
    /// Same gate, several notes enqueued together (same-start-time chord preview during velocity drag).
    void previewNotesChord(const std::vector<PreviewNoteRequest>& notes);

    /// When set, audition consults this instead of `host.hasInstrument()` (I3b clip gate).
    void setPlaybackAllowed(std::function<bool()> fn) noexcept { playbackAllowed_ = std::move(fn); }

    void timerCallback() override;

    static void writeMidiEditorLogLine(const juce::String& message);

private:
    struct PendingOff
    {
        double dueMs = 0.0;
        int midiNote = 0;
        int channel = kMidiChannel;
    };
    struct OffOrder
    {
        [[nodiscard]] bool operator()(const PendingOff& a, const PendingOff& b) const noexcept
        {
            return a.dueMs > b.dueMs;
        }
    };

    void drainNoteOffs(double nowMs);

    ExperimentalInstrumentHost& host_;

    std::priority_queue<PendingOff, std::vector<PendingOff>, OffOrder> pendingOffs_;

    std::function<bool()> playbackAllowed_;
};
