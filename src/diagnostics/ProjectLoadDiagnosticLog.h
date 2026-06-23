#pragma once

#include <juce_core/juce_core.h>

// Appends one line to `%APPDATA%\MiniDAWLab\project-load-diag.log` (ISO 8601 timestamp prefix).
// [Message thread only] — do not call from the audio callback.

void appendProjectLoadDiagnosticLine(const juce::String& message);

[[nodiscard]] juce::File getProjectLoadDiagnosticLogFile();

// Appends one line to `%APPDATA%\MiniDAWLab\project-save-diag.log`.
void appendProjectSaveDiagnosticLine(const juce::String& message);

[[nodiscard]] juce::File getProjectSaveDiagnosticLogFile();
