#include "diagnostics/ProjectLoadDiagnosticLog.h"

juce::File getProjectLoadDiagnosticLogFile()
{
    return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
        .getChildFile("MiniDAWLab")
        .getChildFile("project-load-diag.log");
}

void appendProjectLoadDiagnosticLine(const juce::String& message)
{
    try
    {
        const juce::File f = getProjectLoadDiagnosticLogFile();
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
    juce::Logger::writeToLog("[project-load] " + message);
}

juce::File getProjectSaveDiagnosticLogFile()
{
    return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
        .getChildFile("MiniDAWLab")
        .getChildFile("project-save-diag.log");
}

void appendProjectSaveDiagnosticLine(const juce::String& message)
{
    try
    {
        const juce::File f = getProjectSaveDiagnosticLogFile();
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
    juce::Logger::writeToLog("[project-save] " + message);
}
