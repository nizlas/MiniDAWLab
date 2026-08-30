#include "diagnostics/PlaybackUiLoadLog.h"

juce::File getPlaybackUiLoadDiagnosticLogFile()
{
    return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
        .getChildFile("MiniDAWLab")
        .getChildFile("playback-ui-load.log");
}

void appendPlaybackUiLoadDiagnosticLine(const juce::String& message)
{
    try
    {
        const juce::File f = getPlaybackUiLoadDiagnosticLogFile();
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
}
