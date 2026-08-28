// =============================================================================
// StabilityDiagnosticLog.cpp — mixdown / track-delete logs + last-operation breadcrumb
// =============================================================================

#include "diagnostics/StabilityDiagnosticLog.h"

namespace
{
[[nodiscard]] juce::File stabilityDiagnosticsDirectory()
{
    return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
        .getChildFile("MiniDAWLab");
}

void appendTimestampedLine(const juce::String& fileName,
                           const juce::String& loggerPrefix,
                           const juce::String& message)
{
    try
    {
        const juce::File f = stabilityDiagnosticsDirectory().getChildFile(fileName);
        if (!f.getParentDirectory().isDirectory())
        {
            (void)f.getParentDirectory().createDirectory();
        }
        const juce::String line = juce::Time::getCurrentTime().toISO8601(true) + " " + message + "\n";
        (void)f.appendText(line);
    }
    catch (...)
    {
    }
    juce::Logger::writeToLog(loggerPrefix + " " + message);
}
} // namespace

void appendMixdownDiagnosticLine(const juce::String& message)
{
    appendTimestampedLine("mixdown-diag.log", "[mixdown]", message);
}

void appendTrackDeleteDiagnosticLine(const juce::String& message)
{
    appendTimestampedLine("track-delete-diag.log", "[track-delete]", message);
}

void appendStabilityRunLine(const juce::String& message)
{
    appendTimestampedLine("stability-run.log", "[stability]", message);
}

void appendStabilityInvariantLine(const juce::String& message)
{
    appendTimestampedLine("stability-invariant.log", "[invariant]", message);
}

void appendAutosaveDiagnosticLine(const juce::String& message)
{
    appendTimestampedLine("autosave-diag.log", "[autosave]", message);
}

void writeLastOperationBreadcrumb(const juce::String& status)
{
    try
    {
        const juce::File f = stabilityDiagnosticsDirectory().getChildFile("last-operation.txt");
        if (!f.getParentDirectory().isDirectory())
        {
            (void)f.getParentDirectory().createDirectory();
        }
        // Overwrite (not append): the file always holds the most recent operation marker.
        (void)f.replaceWithText(juce::Time::getCurrentTime().toISO8601(true) + " " + status + "\n");
    }
    catch (...)
    {
    }
}
