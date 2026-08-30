#pragma once

// =============================================================================
// TransportShortcutDiagLog — gated per-keypress trace for transport shortcut triage
// =============================================================================
// Appends to `%APPDATA%/MiniDAWLab/transport-shortcut.log` when
// `MINIDAW_DIAG_TRANSPORT_SHORTCUT` is 1 (see DiagnosticBuildFlags.h). Callers must wrap
// string building in `if constexpr (transport_shortcut_diag::kEnabled)` so normal builds pay
// nothing. Message thread only; not for the audio callback.
// =============================================================================

#include "diagnostics/DiagnosticBuildFlags.h"

#include <juce_core/juce_core.h>

namespace transport_shortcut_diag
{

inline constexpr bool kEnabled = MINIDAW_DIAG_TRANSPORT_SHORTCUT != 0;

inline void appendLine(const juce::String& message)
{
    try
    {
        const juce::File f = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
                                 .getChildFile("MiniDAWLab")
                                 .getChildFile("transport-shortcut.log");
        if (!f.getParentDirectory().isDirectory())
        {
            (void)f.getParentDirectory().createDirectory();
        }
        (void)f.appendText(juce::Time::getCurrentTime().toISO8601(true) + " " + message + "\n");
    }
    catch (...)
    {
    }
}

} // namespace transport_shortcut_diag
