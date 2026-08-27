#pragma once

#include <juce_core/juce_core.h>

// =============================================================================
// StabilityDiagnosticLog — post-crash forensics helpers (Stability Slice 1)
// =============================================================================
// All functions are [Message thread only] — never call from the audio callback.
// Files live in `%APPDATA%\MiniDAWLab\` next to the project-load/save diag logs.
// =============================================================================

/// Appends one timestamped line to `mixdown-diag.log` (mirrors to juce::Logger).
void appendMixdownDiagnosticLine(const juce::String& message);

/// Appends one timestamped line to `track-delete-diag.log` (mirrors to juce::Logger).
void appendTrackDeleteDiagnosticLine(const juce::String& message);

/// Overwrites `last-operation.txt` with one timestamped status line. Written at the
/// start/end/failure of risky operations so a hard crash leaves a breadcrumb of what
/// the app was doing last. Keep messages short (e.g. "mixdown wav start").
void writeLastOperationBreadcrumb(const juce::String& status);

/// Appends one timestamped line to `stability-run.log` (mirrors to juce::Logger).
/// Written by the Stability C2 scenario runner (`--stability-*` command-line modes).
void appendStabilityRunLine(const juce::String& message);

/// Appends one timestamped line to `stability-invariant.log` (mirrors to juce::Logger).
/// Written by the Stability C3 runtime invariant checks (`StabilityInvariants`).
void appendStabilityInvariantLine(const juce::String& message);
