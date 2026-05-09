// =============================================================================
// UndoDiagnosticFileLog — append-only file log for undo/redo triage (message thread)
// =============================================================================

#include "diagnostics/UndoDiagnosticFileLog.h"

namespace
{
    [[nodiscard]] juce::File getUndoDiagnosticLogFile()
    {
        return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
            .getChildFile("MiniDAWLab")
            .getChildFile("undo-diagnostics.log");
    }
}

void writeUndoDiagnosticLogLine(const juce::String& line)
{
    try
    {
        const juce::File f = getUndoDiagnosticLogFile();
        if (!f.getParentDirectory().isDirectory())
        {
            (void)f.getParentDirectory().createDirectory();
        }
        const juce::String ts = juce::Time::getCurrentTime().toISO8601(true);
        const juce::String out = ts + " " + line + "\n";
        (void)f.appendText(out);
    }
    catch (...)
    {
    }
}
