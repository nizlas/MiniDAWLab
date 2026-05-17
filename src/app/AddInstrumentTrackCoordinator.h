#pragma once

#include <functional>

#include "plugins/Vst3ChildProcessScan.h"

class InstrumentRuntimeCoordinator;
class Session;

/// Groove Agent / HALion Sonic "Add instrument track from menu" orchestration (owns no session/runtime state).
class AddInstrumentTrackCoordinator final
{
public:
    struct Refs
    {
        Session& session;
        InstrumentRuntimeCoordinator& instrumentRuntimeCoordinator;
    };

    struct Callbacks
    {
        std::function<void()> refreshInstrumentUi;
        std::function<void()> requestLayoutResized;
        std::function<void()> syncMidiEditorInstrumentClipTimelineFromDeviceIfOpen;
    };

    AddInstrumentTrackCoordinator(Refs refs, Callbacks callbacks);

    void addGrooveAgentInstrumentTrackFromMenu();
    void addHalionSonicInstrumentTrackFromMenu();

private:
    void finishAddGrooveAgentInstrumentTrackAfterInstrumentResolved();
    void beginAsyncGrooveAgentOopScanForAddTrack(mini_daw::Vst3GrooveCacheLoadCandidate v1Cand);

    void finishAddHalionSonicInstrumentTrackAfterInstrumentResolved();
    void beginAsyncHalionSonicOopScanForAddTrack(mini_daw::Vst3GrooveCacheLoadCandidate v1Cand);

    Refs refs_;
    Callbacks callbacks_;
};
