#pragma once

#include <cstdint>

// Verbose drum-name probe (Phase A/B/C, per-note 24-84, summaries). When `kDrumNamesDiag` is true, lines go to:
//   %APPDATA%\MiniDAWLab\drum-name-diagnostics.log
// via writeDrumNameDiagnosticLogLine (see DrumNameDiagnosticFileLog.cpp). Not juce::Logger.
// Normal builds keep `kDrumNamesDiag=false` so refresh/editor-open work without log I/O or Phase C audio probe.

namespace drum_name_diag
{
enum class DrumNameRefreshPhase
{
    immediate,
    delayed250,
    delayed1000,
    afterEditorOpen
};

/// Transient plugin drum pitch-name map trust (see `ExperimentalInstrumentHost` authoritative flag).
enum class PluginDrumNameMapTrust : std::uint8_t
{
    none,
    candidate,
    authoritative
};

[[nodiscard]] inline const char* pluginDrumNameMapTrustTag(const PluginDrumNameMapTrust t) noexcept
{
    switch (t)
    {
        case PluginDrumNameMapTrust::none:
            return "none";
        case PluginDrumNameMapTrust::candidate:
            return "candidate";
        case PluginDrumNameMapTrust::authoritative:
            return "authoritative";
    }
    return "none";
}

[[nodiscard]] inline const char* drumNameRefreshPhaseTag(const DrumNameRefreshPhase p) noexcept
{
    switch (p)
    {
        case DrumNameRefreshPhase::immediate:
            return "immediate";
        case DrumNameRefreshPhase::delayed250:
            return "delayed250";
        case DrumNameRefreshPhase::delayed1000:
            return "delayed1000";
        case DrumNameRefreshPhase::afterEditorOpen:
            return "afterEditorOpen";
    }
    return "immediate";
}

inline constexpr bool kDrumNamesDiag = false;
}
