#pragma once

#include <juce_core/juce_core.h>

/// Append one timestamped line to `%APPDATA%\MiniDAWLab\drum-name-diagnostics.log`.
/// No-op if not on the message thread. Does not use juce::Logger.
void writeDrumNameDiagnosticLogLine(const juce::String& line);
