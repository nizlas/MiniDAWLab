// =============================================================================
// DrumNameDiagnosticFileLog — append-only drum name probe log (message thread)
// =============================================================================

#include "diagnostics/DrumNameDiagnosticFileLog.h"
#include "diagnostics/DrumNameDiagnosticConfig.h"

#include <juce_events/juce_events.h>

namespace
{
    [[nodiscard]] juce::File getDrumNameDiagnosticLogFile()
    {
        return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
            .getChildFile("MiniDAWLab")
            .getChildFile("drum-name-diagnostics.log");
    }
}

void writeDrumNameDiagnosticLogLine(const juce::String& line)
{
    if (!drum_name_diag::kDrumNamesDiag)
    {
        return;
    }
    if (juce::MessageManager::getInstanceWithoutCreating() == nullptr
        || !juce::MessageManager::getInstance()->isThisTheMessageThread())
    {
        return;
    }

    try
    {
        const juce::File f = getDrumNameDiagnosticLogFile();
        if (!f.getParentDirectory().isDirectory())
        {
            (void)f.getParentDirectory().createDirectory();
        }
        const juce::String ts = juce::Time::getCurrentTime().toISO8601(true);
        (void)f.appendText(ts + " " + line + "\n");
    }
    catch (...)
    {
    }
}
