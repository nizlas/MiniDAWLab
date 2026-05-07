#pragma once

#include "ui/experimental/ExperimentalMidiPattern.h"

#include <juce_events/juce_events.h>

#include <functional>
#include <queue>
#include <string>

class ExperimentalInstrumentHost;

/// Drum hits mode: message-thread timer playback with fixed gate (no sample-accurate timing).
class ExperimentalMidiPatternPlayer final : public juce::Timer
{
public:
    static constexpr int kMidiChannel = 1;
    static constexpr double kDrumGateMs = 100.0;

    ExperimentalMidiPatternPlayer(ExperimentalInstrumentHost& host, ExperimentalMidiPattern& pattern) noexcept;

    void startPlayback();
    void stopPlayback(const char* reason);
    [[nodiscard]] bool isPlaying() const noexcept { return playing_; }

    /// Optional: invoked after start/stop so the editor toolbar can refresh (e.g. Stop enable).
    void setPlaybackUiCallback(std::function<void()> callback) noexcept { playbackUiCallback_ = std::move(callback); }

    /// When set, start/step emit consult this instead of `host.hasInstrument()` (I3b clip gate).
    void setPlaybackAllowed(std::function<bool()> fn) noexcept { playbackAllowed_ = std::move(fn); }

    /// 0..1 within the current loop window (or 0 when stopped).
    [[nodiscard]] float getPlayheadNormalized() const noexcept;

    void timerCallback() override;

    static void writeMidiEditorLogLine(const juce::String& message);

private:
    struct PendingOff
    {
        double dueMs = 0.0;
        int midiNote = 0;
    };
    struct OffOrder
    {
        [[nodiscard]] bool operator()(const PendingOff& a, const PendingOff& b) const noexcept
        {
            return a.dueMs > b.dueMs;
        }
    };

    void drainNoteOffs(double nowMs);
    void emitStepIfNeeded(double nowMs, double stepMs, int numSteps);
    void flushAllSound(const char* reason);

    ExperimentalInstrumentHost& host_;
    ExperimentalMidiPattern& pattern_;

    bool playing_ = false;
    double playStartMs_ = 0.0;
    int lastEmittedStep_ = -1;
    std::priority_queue<PendingOff, std::vector<PendingOff>, OffOrder> pendingOffs_;

    std::function<void()> playbackUiCallback_;
    std::function<bool()> playbackAllowed_;
};
