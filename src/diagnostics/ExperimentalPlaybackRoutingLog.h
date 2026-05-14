#pragma once

#include <juce_core/juce_core.h>

/// Appends one timestamped line to `%APPDATA%\\MiniDAWLab\\experimental-playback-routing.log`.
/// Intended for the **message thread** (or message-thread lambdas). Do not call from the audio callback.
void appendExperimentalPlaybackRoutingLogLine(const juce::String& line);
