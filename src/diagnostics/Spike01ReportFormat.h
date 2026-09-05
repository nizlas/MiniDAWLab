#pragma once

// ============================================================================
// SPIKE-01 DIAGNOSTIC SCAFFOLDING — NOT PRODUCT CODE.
//
// Pure, dependency-free (no JUCE) data model, statistics, and sanitized
// markdown report formatting for the SPIKE-01 authoritative plugin-state
// capture probe (roadmap slice P0/P1A, canonical steering
// docs/PORTABLE_INSTRUMENTS_AND_PROXIES.md §9.2, PID-001).
//
// PRIVACY BY CONSTRUCTION: none of the types in this header can carry raw
// plugin-state bytes. A capture is represented only by {phase, duration,
// size, SHA-256 hex, thread/transport flags}. The report builder therefore
// cannot leak state content, private paths, or licensing material.
//
// Removal: delete src/diagnostics/Spike01*.* and the flag-gated hook in the
// app; nothing in the product path depends on this file.
// ============================================================================

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace spike01
{

//==============================================================================
// Capture phases (§3 of the SPIKE-01 task; canonical §9.2 / PID-001 evidence).
//==============================================================================

struct PhaseInfo
{
    const char* id;
    const char* label;
};

/// The predefined measurement phases the operator steps through. Free-text
/// phases are also allowed by the panel; these are the required ones.
inline const std::vector<PhaseInfo>& requiredPhases()
{
    static const std::vector<PhaseInfo> phases = {
        { "A1", "A1 - editor closed, transport stopped" },
        { "A2", "A2 - editor open, transport stopped" },
        { "A3", "A3 - editor open, transport playing" },
        { "A4", "A4 - repeated unchanged captures" },
        { "A5", "A5 - immediately after plugin-GUI parameter change" },
        { "B2", "B2 - editor reopened/closed, no intentional sound change" },
        { "B3", "B3 - after save + reload of unchanged state" },
        { "B4", "B4 - parameter returned to its original visible value" },
        { "B5", "B5 - after transport activity (played, then stopped)" },
        { "E1", "E1 - after preset/program change" },
        { "E2", "E2 - after non-parameter change (MIDI-learn/mode switch)" },
        { "F1", "F1 - snapshot-request checkpoint (conceptual enqueue)" },
    };
    return phases;
}

//==============================================================================
// Samples and events.
//==============================================================================

/// One timed state capture. Carries hash + size only — never blob bytes.
struct CaptureSample
{
    std::string phaseId;          // e.g. "A1"
    std::string capturePath;      // "raw-getStateInformation" | "save-path-base64"
    double durationMs = 0.0;
    std::uint64_t blobBytes = 0;
    std::string sha256Hex;        // 64 lowercase hex chars of the raw blob bytes
    bool onMessageThread = false;
    bool editorOpen = false;
    bool transportPlaying = false;
    std::string timestampIso;     // local wall-clock, informational only
};

/// One host-observed parameter/processor notification.
struct ParamEvent
{
    std::string kind;             // "paramChanged" | "gestureBegin" | "gestureEnd" | "processorChanged"
    int parameterIndex = -1;
    std::string parameterName;    // display name only (no values beyond normalized float)
    float newValue = 0.0f;        // normalized 0..1 as reported by JUCE
    bool onMessageThread = false;
    std::string detail;           // e.g. processorChanged change description
    std::string timestampIso;
};

//==============================================================================
// Statistics.
//==============================================================================

struct DurationStats
{
    std::size_t count = 0;
    double minMs = 0.0;
    double medianMs = 0.0;
    double p95Ms = 0.0;
    double maxMs = 0.0;
};

/// min / median / p95 (nearest-rank) / max over the given durations.
inline DurationStats computeDurationStats(std::vector<double> ms)
{
    DurationStats s;
    s.count = ms.size();
    if (ms.empty())
        return s;

    std::sort(ms.begin(), ms.end());
    s.minMs = ms.front();
    s.maxMs = ms.back();

    const std::size_t n = ms.size();
    if ((n % 2) == 1)
        s.medianMs = ms[n / 2];
    else
        s.medianMs = 0.5 * (ms[n / 2 - 1] + ms[n / 2]);

    // Nearest-rank p95: ceil(0.95 * n) as 1-based rank.
    std::size_t rank = static_cast<std::size_t>(std::ceil(0.95 * static_cast<double>(n)));
    if (rank < 1) rank = 1;
    if (rank > n) rank = n;
    s.p95Ms = ms[rank - 1];
    return s;
}

//==============================================================================
// Sanitized markdown report.
//==============================================================================

struct ReportHeader
{
    std::string appVersion;
    std::string pluginName;
    std::string pluginFormat;
    std::string pluginVersion;
    std::string pluginIdentifier;   // fileOrIdentifier (a path is acceptable to record;
                                    // it identifies the plugin binary, not user data)
    std::string generatedAtIso;
    std::string machineNotes;       // free text from the operator (e.g. device/SR)
};

namespace detail
{
    inline std::string fmt2(double v)
    {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%.2f", v);
        return std::string(buf);
    }

    inline std::string shortHash(const std::string& hex)
    {
        return hex.size() > 12 ? hex.substr(0, 12) + "…" : hex;
    }
} // namespace detail

/// Builds the complete sanitized markdown report. Inputs contain hashes and
/// sizes only; the function has no access to raw state bytes by construction.
inline std::string buildReportMarkdown(const ReportHeader& header,
                                       const std::vector<CaptureSample>& samples,
                                       const std::vector<ParamEvent>& events,
                                       const std::vector<std::string>& operatorNotes)
{
    std::string md;
    md += "# SPIKE-01 sanitized measurement report — authoritative plugin-state capture\n\n";
    md += "SPIKE-01 diagnostic output (roadmap slice P0/P1A; canonical steering §9.2 / PID-001).\n\n";
    md += "**Privacy:** this report contains only sizes, SHA-256 hashes, timings, and\n";
    md += "notification metadata. No raw plugin-state bytes, presets, or licensing\n";
    md += "material are captured to disk by the SPIKE-01 probe.\n\n";

    md += "## Environment\n\n";
    md += "| Field | Value |\n|---|---|\n";
    md += "| Generated | " + header.generatedAtIso + " |\n";
    md += "| App version | " + header.appVersion + " |\n";
    md += "| Plugin | " + header.pluginName + " |\n";
    md += "| Plugin format | " + header.pluginFormat + " |\n";
    md += "| Plugin version | " + header.pluginVersion + " |\n";
    md += "| Plugin identifier | " + header.pluginIdentifier + " |\n";
    md += "| Operator notes | " + header.machineNotes + " |\n\n";

    // --- Group samples by (phase, capture path). -----------------------------
    std::map<std::string, std::vector<const CaptureSample*>> groups;
    for (const auto& s : samples)
        groups[s.phaseId + " / " + s.capturePath].push_back(&s);

    md += "## Capture measurements per phase\n\n";
    if (groups.empty())
    {
        md += "*No captures recorded.*\n\n";
    }
    else
    {
        md += "| Phase / path | n | min ms | median ms | p95 ms | max ms | blob bytes (min..max) | distinct hashes | byte-stable |\n";
        md += "|---|---|---|---|---|---|---|---|---|\n";
        for (const auto& [key, list] : groups)
        {
            std::vector<double> durations;
            std::set<std::string> hashes;
            std::uint64_t minBytes = ~0ull, maxBytes = 0ull;
            for (const auto* s : list)
            {
                durations.push_back(s->durationMs);
                hashes.insert(s->sha256Hex);
                minBytes = std::min(minBytes, s->blobBytes);
                maxBytes = std::max(maxBytes, s->blobBytes);
            }
            const DurationStats st = computeDurationStats(durations);
            md += "| " + key + " | " + std::to_string(st.count)
                + " | " + detail::fmt2(st.minMs)
                + " | " + detail::fmt2(st.medianMs)
                + " | " + detail::fmt2(st.p95Ms)
                + " | " + detail::fmt2(st.maxMs)
                + " | " + std::to_string(minBytes) + ".." + std::to_string(maxBytes)
                + " | " + std::to_string(hashes.size())
                + " | " + (hashes.size() == 1 ? "yes" : "NO")
                + " |\n";
        }
        md += "\n";
    }

    // --- Per-phase hash inventory (short prefixes for cross-phase comparison).
    md += "## Hash inventory (12-hex prefixes; full hashes in raw sample log)\n\n";
    {
        std::map<std::string, std::set<std::string>> phaseHashes;
        for (const auto& s : samples)
            phaseHashes[s.phaseId].insert(s.sha256Hex);
        if (phaseHashes.empty())
            md += "*No captures recorded.*\n";
        for (const auto& [phase, hashes] : phaseHashes)
        {
            md += "- **" + phase + "**: ";
            bool first = true;
            for (const auto& h : hashes)
            {
                if (!first) md += ", ";
                md += "`" + detail::shortHash(h) + "`";
                first = false;
            }
            md += "\n";
        }
        md += "\n";
    }

    // --- Threading summary. ---------------------------------------------------
    md += "## Threading\n\n";
    {
        std::size_t onMsg = 0, offMsg = 0;
        for (const auto& s : samples) (s.onMessageThread ? onMsg : offMsg)++;
        md += "- Captures executed on the message thread: " + std::to_string(onMsg) + "\n";
        md += "- Captures executed on other threads: " + std::to_string(offMsg)
            + (offMsg == 0 ? " (expected: 0)\n" : "  **UNEXPECTED — investigate**\n");
        std::size_t evMsg = 0, evOther = 0;
        for (const auto& e : events) (e.onMessageThread ? evMsg : evOther)++;
        md += "- Parameter/processor notifications on the message thread: " + std::to_string(evMsg) + "\n";
        md += "- Parameter/processor notifications on other threads: " + std::to_string(evOther) + "\n\n";
    }

    // --- Notification log. -----------------------------------------------------
    md += "## Parameter/processor notifications (" + std::to_string(events.size()) + ")\n\n";
    if (events.empty())
    {
        md += "*None observed while the listener was attached.*\n\n";
    }
    else
    {
        md += "| Time | Kind | Param idx | Param name | Value | Thread | Detail |\n";
        md += "|---|---|---|---|---|---|---|\n";
        for (const auto& e : events)
        {
            md += "| " + e.timestampIso + " | " + e.kind + " | "
                + std::to_string(e.parameterIndex) + " | " + e.parameterName + " | "
                + detail::fmt2(static_cast<double>(e.newValue)) + " | "
                + (e.onMessageThread ? "message" : "other") + " | " + e.detail + " |\n";
        }
        md += "\n";
    }

    // --- Raw sample log (full hashes, still no bytes). --------------------------
    md += "## Raw sample log (" + std::to_string(samples.size()) + ")\n\n";
    if (samples.empty())
    {
        md += "*No captures recorded.*\n\n";
    }
    else
    {
        md += "| Time | Phase | Path | ms | bytes | editor | transport | thread | sha256 |\n";
        md += "|---|---|---|---|---|---|---|---|---|\n";
        for (const auto& s : samples)
        {
            md += "| " + s.timestampIso + " | " + s.phaseId + " | " + s.capturePath
                + " | " + detail::fmt2(s.durationMs)
                + " | " + std::to_string(s.blobBytes)
                + " | " + (s.editorOpen ? "open" : "closed")
                + " | " + (s.transportPlaying ? "playing" : "stopped")
                + " | " + (s.onMessageThread ? "message" : "other")
                + " | `" + s.sha256Hex + "` |\n";
        }
        md += "\n";
    }

    // --- Operator notes. ---------------------------------------------------------
    md += "## Operator notes\n\n";
    if (operatorNotes.empty())
        md += "*None.*\n";
    for (const auto& n : operatorNotes)
        md += "- " + n + "\n";
    md += "\n";

    md += "---\n";
    md += "*End of sanitized SPIKE-01 report. Verify before sharing: this file must\n";
    md += "contain no base64 blocks and no plugin-state byte dumps.*\n";
    return md;
}

} // namespace spike01
