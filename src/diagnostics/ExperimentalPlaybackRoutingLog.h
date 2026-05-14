#pragma once

#include <juce_core/juce_core.h>

#include "DiagnosticBuildFlags.h"

/// Appends one timestamped line to `%APPDATA%\\MiniDAWLab\\experimental-playback-routing.log`
/// **only when** `MINIDAW_DIAG_PLAYBACK_ROUTING` is non-zero (compile-time default: 0 in
/// DiagnosticBuildFlags.h). Intended for the **message thread** (or message-thread lambdas).
/// Do not call from the audio callback.
void appendExperimentalPlaybackRoutingLogLine(const juce::String& line);
