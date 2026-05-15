#pragma once

#include <functional>

#include "plugins/Vst3ChildProcessScan.h"

class InstrumentRuntimeCoordinator;
class Session;

/// Groove Agent "Add instrument track from menu" orchestration (owns no session/runtime state).
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

private:
    void finishAddGrooveAgentInstrumentTrackAfterInstrumentResolved();
    void beginAsyncGrooveAgentOopScanForAddTrack(mini_daw::Vst3GrooveCacheLoadCandidate v1Cand);

    Refs refs_;
    Callbacks callbacks_;
};
