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

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

#include <juce_events/juce_events.h>

class ExperimentalInstrumentHost; // IWYU: full type in .cpp for autoload + I3e transport MIDI

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

    /// MIDI piano-roll UI viewport (message thread). `midiRollSamplesPerPixel <= 0` = never set / use default seed.
    std::int64_t midiRollVisibleStartSamples = 0;
    double midiRollSamplesPerPixel = 0.0;
    bool midiRollFollowEnabled = false;
};

/// I3e: immutable copy of all experimental-clip MIDI for the audio thread (no raw `clips_` access).
/// I3f+: `midiChannel` is 1‑based (Groove Agent + standard MIDI numbering).
struct InstrumentNoteRenderEvent
{
    std::int64_t absSample = 0;
    /// Timeline notes: absolute sample for note-off. **0** means use `InstrumentTrackRenderSnapshot::gateSamples`
    /// after `absSample` (step grid / legacy).
    std::int64_t noteOffAbsSample = 0;
    std::uint8_t midiNote = 60;
    std::uint8_t velocity = 100;
    std::uint8_t midiChannel = 1;
};

struct InstrumentClipRenderPlan
{
    std::int64_t startSamples = 0;
    std::int64_t endSamplesExclusive = 0;
    std::vector<InstrumentNoteRenderEvent> notes;
};

struct InstrumentTrackRenderSnapshot
{
    std::uint32_t revision = 0;
    bool playbackEnabled = false;
    int midiChannel = 1;
    int gateSamples = 4800;
    /// Sorted by `startSamples`. Notes sorted by `absSample` within each clip.
    std::vector<InstrumentClipRenderPlan> clips;
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

    /// [Message thread] Piano roll / pattern edits: republish audio snapshot (note grid + gate).
    void notifyClipPatternMutated(InstrumentMidiClipId clipId) noexcept;

    /// BPM / timeline note timing changed (ticks→samples); does **not** rewrite `lengthSamples` from grid.
    void notifyClipExperimentalMusicalTimingChanged() noexcept;

    [[nodiscard]] std::shared_ptr<const InstrumentTrackRenderSnapshot> loadRenderSnapshotForAudioThread() const noexcept
    {
        return std::atomic_load_explicit(&renderSnapshot_, std::memory_order_acquire);
    }

    /// [Audio thread] Sample-accurate Groove Agent MIDI for one render segment (half-open times).
    void audioThread_scheduleTransportMidiForSegment(ExperimentalInstrumentHost& host,
                                                     std::int64_t timelineSegStart,
                                                     int segNumSamples,
                                                     int bufferOffsetInDevice,
                                                     bool forceDiscontinuity,
                                                     int deviceBlockNumSamples) noexcept;

    /// [Audio thread] Stop/flush: pending transport offs + allNotesOff(1).
    void audioThread_flushTransportMidi(ExperimentalInstrumentHost& host,
                                        int offsetInDevice,
                                        int deviceBlockNumSamples) noexcept;

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
    juce::String pendingPluginStateBase64_;
    bool pendingProjectGrooveAutoload_ = false;
    juce::String pendingAdvisoryPluginBundlePath_;
    juce::String pendingInstrumentKind_;

    double timelineSampleRate_ = 48000.0;

    std::atomic<std::shared_ptr<const InstrumentTrackRenderSnapshot>> renderSnapshot_;
    std::uint32_t nextSnapshotRevision_ = 1;

    void publishRenderSnapshot();
    void clearExperimentalInstrumentStateForProjectLoad();

    // --- Audio thread only (I3e transport MIDI; `audioThread_*` entry points) ---
    struct PendingTransportNoteOff
    {
        std::int64_t dueAbsSample = 0;
        int midiNote = 0;
        int midiChannel = 1;
    };

    static constexpr int kMaxPendingTransportOffs = 256;
    std::array<PendingTransportNoteOff, kMaxPendingTransportOffs> rtPendingOffs_{};
    int rtPendingOffCount_ = 0;
    std::int64_t rtLastSegEndTimeline_ = -1;
    std::uint32_t rtLastSnapshotRevision_ = 0;
};
