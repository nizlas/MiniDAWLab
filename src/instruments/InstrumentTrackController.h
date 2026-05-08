#pragma once

// =============================================================================
// InstrumentTrackController — experimental instrument track + runtime MIDI clips (I3b)
// =============================================================================
//
// Message-thread only. Owns zero or one Groove-Agent instrument track and its MIDI
// clips in memory (experimental ProjectFile v11 + optional project-load autoload).
// instrumentLoaded_ reflects whether the global Groove Agent slot is filled.
//
// =============================================================================

#include "io/ProjectFile.h"
#include "ui/experimental/ExperimentalMidiPattern.h"

#include <cstdint>
#include <memory>
#include <vector>

#include <juce_events/juce_events.h>

class ExperimentalInstrumentHost; // IWYU: full type in .cpp for autoload

using InstrumentMidiClipId = std::uint64_t;

struct InstrumentMidiClip
{
    InstrumentMidiClipId id = 0;
    juce::String name { "MIDI 1" };
    ExperimentalMidiPattern pattern;
    /// Session-timeline anchor (samples). I3d1: piano roll + lane use absolute samples.
    std::int64_t startSamples = 0;
    /// Locked pattern span in samples (explicit; user trim deferred). Recomputed only on clip create,
    /// load when missing/zero, or **numSteps / stepDenom** change — **not** on BPM-only edits.
    std::int64_t lengthSamples = 0;
    /// Legacy fractional lane layout when main timeline mapping is unavailable (fallback).
    int laneStartFractionPermille = 0;
    int laneEndFractionPermille = 250;
};

class InstrumentTrackController : public juce::ChangeBroadcaster
{
public:
    explicit InstrumentTrackController(ExperimentalInstrumentHost& host) noexcept;

    /// Instrument track row exists (Add Instrument Track succeeded).
    [[nodiscard]] bool hasInstrumentTrack() const noexcept { return trackActive_; }

    /// Alias for older call sites.
    [[nodiscard]] bool hasInstrumentTrackShell() const noexcept { return trackActive_; }

    /// Global slot currently has Groove Agent (by name heuristic).
    [[nodiscard]] bool isInstrumentLoaded() const noexcept { return instrumentLoaded_; }

    /// Adds track + default clip if not already present. Returns false if track already exists.
    [[nodiscard]] bool tryAddGrooveAgentInstrumentTrackShell();

    /// Refresh instrumentLoaded_ from host. Never deletes the track or clips.
    void syncShellWithHostState();

    [[nodiscard]] juce::String getLaneHeaderTitle() const;
    [[nodiscard]] juce::String getLaneHeaderSubtitle() const;
    /// Two lines: title + newline + subtitle (for logging or simple labels).
    [[nodiscard]] juce::String getLaneHeaderText() const;

    /// Experimental lane header **Power** (on = processed); default on when the track shell exists.
    [[nodiscard]] bool isPowerOn() const noexcept { return powerOn_; }
    void setPowerOn(bool on) noexcept;

    /// Experimental lane header **Mute**; default off when the track shell exists.
    [[nodiscard]] bool isMuted() const noexcept { return muted_; }
    void setMuted(bool muted) noexcept;

    /// UI-only "active row" highlight (mutex with audio header selection in `TransportControlsContent`).
    /// `TrackHeaderView` reads this to render the slate-blue active variant.
    [[nodiscard]] bool isActive() const noexcept { return isActive_; }
    void setActive(bool active) noexcept;

    [[nodiscard]] const std::vector<std::unique_ptr<InstrumentMidiClip>>& getClips() const noexcept
    {
        return clips_;
    }

    [[nodiscard]] InstrumentMidiClip* getClipById(InstrumentMidiClipId id) noexcept;
    [[nodiscard]] const InstrumentMidiClip* getClipById(InstrumentMidiClipId id) const noexcept;

    /// t in [0,1] along the event lane (excluding header).
    [[nodiscard]] InstrumentMidiClip* findClipAtLaneFraction(float t) noexcept;

    [[nodiscard]] InstrumentMidiClipId getSelectedClipId() const noexcept { return selectedClipId_; }
    void setSelectedClipId(InstrumentMidiClipId id) noexcept;
    void clearClipSelection() noexcept { setSelectedClipId(0); }

    [[nodiscard]] const juce::String& getRequiredKitName() const noexcept { return requiredKitName_; }
    void setRequiredKitName(juce::String name) noexcept;

    /// One enabled row for `experimentalInstrumentTracks` when `hasInstrumentTrack()`.
    [[nodiscard]] ProjectFileExperimentalInstrumentTrackV1 buildExperimentalInstrumentProjectBlock() const;

    /// Replace in-memory track + clips from project (message thread). Clears track when `tracks` empty
    /// or no enabled GrooveAgentSE row.
    void restoreExperimentalInstrumentFromProject(const std::vector<ProjectFileExperimentalInstrumentTrackV1>& tracks);

    /// After `loadProjectFromFile`, attempts cache load + optional Windows path repair + host load.
    void runPendingGrooveAgentProjectAutoload(ExperimentalInstrumentHost& host, juce::String& outWarning);

    /// Device sample rate for **musical** length derivation (message thread). Does not rescale clips.
    void setTimelineSampleRate(double sampleRate) noexcept;

    /// Recompute `lengthSamples` from pattern grid + `timelineSampleRate_` (create / load repair /
    /// numSteps or stepDenom edits only — not BPM-only).
    void recomputeLockedClipLengthFromPatternGrid(InstrumentMidiClip& clip) noexcept;

private:
    [[nodiscard]] bool computeInstrumentLoadedFromHost() const noexcept;

    ExperimentalInstrumentHost& host_;
    bool trackActive_ = false;
    bool instrumentLoaded_ = false;
    InstrumentMidiClipId nextClipId_ = 1;
    InstrumentMidiClipId selectedClipId_ = 0;
    std::vector<std::unique_ptr<InstrumentMidiClip>> clips_;
    bool powerOn_ = true;
    bool muted_ = false;
    bool isActive_ = false;

    juce::String requiredKitName_;
    bool pendingProjectGrooveAutoload_ = false;
    juce::String pendingAdvisoryPluginBundlePath_;
    juce::String pendingInstrumentKind_;

    double timelineSampleRate_ = 48000.0;

    void clearExperimentalInstrumentStateForProjectLoad();
};
