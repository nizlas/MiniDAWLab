#pragma once

// =============================================================================
// InstrumentTrackController — I3a-minimal experimental instrument track shell
// =============================================================================
//
// Message-thread only. Does not own plugins or MIDI data. Tracks whether the UI
// shows a single "instrument track" row linked to the existing global
// ExperimentalInstrumentHost (no second instance, no loadInstrument here).
//
// =============================================================================

class ExperimentalInstrumentHost;

class InstrumentTrackController
{
public:
    static constexpr const char* kGrooveAgentShellDisplayName
        = "Groove Agent SE - Instrument Track (experimental, not saved)";

    explicit InstrumentTrackController(ExperimentalInstrumentHost& host) noexcept;

    /// True after Add Instrument Track -> Groove Agent SE succeeds.
    [[nodiscard]] bool hasInstrumentTrackShell() const noexcept { return shellActive_; }

    /// Adds shell if not already present. Returns false if shell already exists.
    [[nodiscard]] bool tryAddGrooveAgentInstrumentTrackShell();

    /// Drop shell if the host no longer holds Groove Agent (unload or other instrument).
    void syncShellWithHostState();

private:
    ExperimentalInstrumentHost& host_;
    bool shellActive_ = false;
};
