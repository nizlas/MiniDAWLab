#pragma once

#include <juce_core/juce_core.h>

#include "DiagnosticBuildFlags.h"

/// Optional diagnostic: appends one timestamped line under `%APPDATA%\\MiniDAWLab\\` to file
/// `experimental-playback-routing.log` **only when** `MINIDAW_DIAG_PLAYBACK_ROUTING` is non-zero (same macro
/// gates the implementation; compile-time **default `0`** in `DiagnosticBuildFlags.h`).
/// No-op when the flag is **`0`** — never treated as guaranteed or architecturally required logging.
/// Intended for the **message thread** (or message-thread lambdas). Do not call from the audio callback.void appendExperimentalPlaybackRoutingLogLine(const juce::String& line);
