#include "instruments/InstrumentTrackController.h"
#include "plugins/ExperimentalInstrumentHost.h"

InstrumentTrackController::InstrumentTrackController(ExperimentalInstrumentHost& host) noexcept
    : host_(host)
{
}

bool InstrumentTrackController::tryAddGrooveAgentInstrumentTrackShell()
{
    if (shellActive_)
    {
        return false;
    }

    shellActive_ = true;
    return true;
}

void InstrumentTrackController::syncShellWithHostState()
{
    if (!shellActive_)
    {
        return;
    }

    if (!host_.hasInstrument())
    {
        shellActive_ = false;
        return;
    }

    if (!host_.getInstrumentNameForUi().containsIgnoreCase("Groove Agent"))
    {
        shellActive_ = false;
    }
}
