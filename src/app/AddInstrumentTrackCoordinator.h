#pragma once

#include <functional>

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
        std::function<bool()> anyHeldGrooveAgentLoaded;
        std::function<void()> refreshInstrumentUi;
        std::function<void()> requestLayoutResized;
        std::function<void()> syncMidiEditorInstrumentClipTimelineFromDeviceIfOpen;
    };

    AddInstrumentTrackCoordinator(Refs refs, Callbacks callbacks);

    void addGrooveAgentInstrumentTrackFromMenu();

private:
    Refs refs_;
    Callbacks callbacks_;
};
