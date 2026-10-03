#pragma once

// =============================================================================
// ChannelStripPanel — fixed bottom panel of the Inspector: name, channel fader, output meter
// =============================================================================
//
// ROLE
//   Always-visible compact channel panel under the scrollable Inspector content, for the active
//   row ONLY: its name, its EXISTING Channel Volume as a vertical fader (`ChannelFaderComponent` →
//   `Session::setTrackChannelFaderGain`, the same setter the old text field used) and its actual
//   audio output level (`LevelMeterComponent`, L/R bars). There is no separate always-on Stereo Out
//   meter: the master's level is shown when the Stereo Out row itself is selected.
//
// MODES (decided from the published snapshot on every tick)
//   * Audio / Instrument / Group row: fader + the row's post-strip output meter.
//   * Master row ("Stereo Out"): the master fader + the Stereo Out output meter (device output).
//   * MIDI row / no active row: no audio output — the panel reports `hasAudioStrip() == false` and
//     the owner hides it (MIDI rows get no fader; no MIDI volume function is introduced).
//
// DATA FLOW
//   A 30 Hz timer drains the engine's accumulators through injected hooks (owner wires them to
//   `PlaybackEngine::drainMeteredTrackLevels` / `drainMasterOutputLevels`) and tells the engine which
//   row to meter (`setMeteredTrack`). The engine keeps folding the Stereo Out accumulator whether or
//   not it is displayed (export diagnostics and the master row's meter read it). Track switch, delete
//   and project replace go through `refreshFromSession`, which re-points the tap and clears the meter
//   so the previous row's data is never shown. No audio-thread code lives here.
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

    /// True when the active row carries audio (audio / instrument / group / master): the owner shows
    /// the panel. False for MIDI rows and when no row is active: the owner hides it.
    [[nodiscard]] bool hasAudioStrip() const noexcept { return trackStripVisible_; }
    /// Owner hook: fired when `hasAudioStrip()` flips, so the Inspector column can re-layout.
    std::function<void()> onAudioStripVisibilityChanged;

    /// Layout heights (logical px). The owner gives the panel `preferredHeight()` when the Inspector
    /// is tall enough and shrinks it towards `minimumHeight()` on low windows.
    [[nodiscard]] static int preferredHeight() noexcept { return 210; }
    [[nodiscard]] static int minimumHeight() noexcept { return 150; }

    // --- Test / diagnostics surfaces -------------------------------------------------------------
    [[nodiscard]] bool isTrackStripVisible() const noexcept { return trackStripVisible_; }
    [[nodiscard]] bool isFaderVisible() const noexcept { return fader_.isVisible(); }
    [[nodiscard]] bool isShowingMaster() const noexcept { return shownIsMaster_; }
    [[nodiscard]] TrackId getShownTrackId() const noexcept { return shownTrackId_; }
    [[nodiscard]] ChannelFaderComponent& fader() noexcept { return fader_; }
    /// The single output meter: the row's post-strip level, or the Stereo Out level for the master row.
    [[nodiscard]] LevelMeterComponent& outputMeter() noexcept { return outputMeter_; }
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
    juce::Label meterCaption_;
    ChannelFaderComponent fader_;
    LevelMeterComponent outputMeter_;
    TrackId shownTrackId_ = kInvalidTrackId;
    bool shownIsMaster_ = false;
    bool trackStripVisible_ = false;
    bool faderWiredGuard_ = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ChannelStripPanel)
};
