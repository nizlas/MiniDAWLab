// =============================================================================
// ExperimentalPlaybackRoutingLog — file-based routing triage (message thread)
// =============================================================================

#include "diagnostics/ExperimentalPlaybackRoutingLog.h"

#include <juce_core/juce_core.h>

namespace
{
#if MINIDAW_DIAG_PLAYBACK_ROUTING
[[nodiscard]] juce::File getExperimentalPlaybackRoutingLogFile()
{
    return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
        .getChildFile("MiniDAWLab")
        .getChildFile("experimental-playback-routing.log");
}

juce::CriticalSection gExperimentalPlaybackRoutingLogLock;
#endif
} // namespace

void appendExperimentalPlaybackRoutingLogLine(const juce::String& line)
{
#if !MINIDAW_DIAG_PLAYBACK_ROUTING
    (void)line;
    return;
#else
    const juce::ScopedLock sl(gExperimentalPlaybackRoutingLogLock);
    try
    {
        const juce::File f = getExperimentalPlaybackRoutingLogFile();
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
#endif
}
