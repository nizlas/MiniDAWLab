#pragma once

// =============================================================================
// ChannelStripPanel — fixed bottom panel of the Inspector: name, channel fader, track meter, Stereo Out meter
// =============================================================================
//
// ROLE
//   Always-visible compact channel panel under the scrollable Inspector content. Shows, for the
//   active row: its name, its EXISTING Channel Volume as a vertical fader (`ChannelFaderComponent`
//   → `Session::setTrackChannelFaderGain`, the same setter the old text field used), its actual
//   post-strip output level (`LevelMeterComponent`, 2 bars), and — clearly separate — a compact
//   Stereo Out meter that is ALWAYS visible.
//
// MODES (decided from the published snapshot on every tick)
//   * Audio / Instrument / Group row: fader + track meter + compact Stereo Out meter.
//   * Master row ("Stereo Out"): the main fader and the main meter ARE the master's — no second copy.
//   * MIDI row / no active row: only the Stereo Out meter (MIDI rows have no audio and get no fader;
//     this introduces no MIDI volume function).
//
// DATA FLOW
//   A 30 Hz timer drains the engine's two accumulators through injected hooks (owner wires them to
//   `PlaybackEngine::drainMeteredTrackLevels` / `drainMasterOutputLevels`) and tells the engine which
//   row to meter (`setMeteredTrack`). Track switch, track delete and project replace all go through
//   `refreshFromSession`, which re-selects the metered row and clears the meter so the previous row's
//   data is never shown. No audio-thread code lives here.
//
// THREADING
//   [Message thread] only.
// =============================================================================

#include "domain/Track.h"
#include "engine/LevelMeterAccumulator.h"
#include "ui/ChannelFaderComponent.h"
#include "ui/LevelMeterComponent.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

class Session;

class ChannelStripPanel final : public juce::Component,
                                private juce::Timer
{
public:
    struct Hooks
    {
        std::function<level_meter::Reading()> drainTrackMeter;
        std::function<level_meter::Reading()> drainMasterMeter;
        std::function<void(TrackId)> setMeteredTrack;
    };

    explicit ChannelStripPanel(Session& session);
    ~ChannelStripPanel() override;

    void setHooks(Hooks hooks);

    /// Re-reads the active row / fader value / kind from the published snapshot (also runs on the
    /// panel's own tick, so callers may but need not call it after an edit).
    void refreshFromSession();

    /// Layout heights (logical px). The owner gives the panel `preferredHeight()` when the Inspector
    /// is tall enough and shrinks it towards `minimumHeight()` on low windows.
    [[nodiscard]] static int preferredHeight() noexcept { return 210; }
    [[nodiscard]] static int minimumHeight() noexcept { return 150; }

    // --- Test / diagnostics surfaces -------------------------------------------------------------
    [[nodiscard]] bool isTrackStripVisible() const noexcept { return trackStripVisible_; }
    [[nodiscard]] bool isFaderVisible() const noexcept { return fader_.isVisible(); }
    [[nodiscard]] TrackId getShownTrackId() const noexcept { return shownTrackId_; }
    [[nodiscard]] ChannelFaderComponent& fader() noexcept { return fader_; }
    [[nodiscard]] LevelMeterComponent& trackMeter() noexcept { return trackMeter_; }
    [[nodiscard]] LevelMeterComponent& masterMeter() noexcept { return masterMeter_; }
    [[nodiscard]] juce::String getNameText() const { return nameLabel_.getText(); }
    /// Runs one meter tick now (drain + animate) — for tests that cannot wait for the timer.
    void tickNowForTest() { timerCallback(); }

    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    void timerCallback() override;
    void applyModeForTrack(const Track* track);

    Session& session_;
    Hooks hooks_;
    juce::Label nameLabel_;
    juce::Label trackMeterCaption_;
    juce::Label masterCaption_;
    ChannelFaderComponent fader_;
    LevelMeterComponent trackMeter_;
    LevelMeterComponent masterMeter_;
    TrackId shownTrackId_ = kInvalidTrackId;
    bool shownIsMaster_ = false;
    bool trackStripVisible_ = false;
    bool faderWiredGuard_ = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ChannelStripPanel)
};
