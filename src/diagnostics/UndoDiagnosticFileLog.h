#pragma once

#include <juce_core/juce_core.h>

// Appends one line to `%APPDATA%\MiniDAWLab\undo-diagnostics.log` (ISO 8601 timestamp prefix).
// [Message thread only] — do not call from the audio callback.

void writeUndoDiagnosticLogLine(const juce::String& line);
