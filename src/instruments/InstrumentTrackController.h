#pragma once

// =============================================================================
// InstrumentTrackController — experimental instrument track + runtime MIDI clips (I3b)
// =============================================================================
//
// Message-thread only. Owns zero or one Groove-Agent instrument track and its MIDI
// clips in memory (no Session / ProjectFile). Track and clips survive host unload;
// instrumentLoaded_ only reflects whether the global Groove Agent slot is filled.
//
// =============================================================================

#include "ui/experimental/ExperimentalMidiPattern.h"

#include <cstdint>
#include <memory>
#include <vector>

#include <juce_events/juce_events.h>

class ExperimentalInstrumentHost;

using InstrumentMidiClipId = std::uint64_t;

struct InstrumentMidiClip
{
    InstrumentMidiClipId id = 0;
    juce::String name { "MIDI 1" };
    ExperimentalMidiPattern pattern;
    /// 0..1000, visual lane placement only until timeline samples exist (I3c+).
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
};
