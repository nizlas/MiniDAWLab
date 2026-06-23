#pragma once

#include <atomic>
#include <functional>

#include "plugins/Vst3ChildProcessScan.h"

namespace mini_daw
{
struct InstrumentCatalogEntry;
}

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

    /// Slice 2: create an instrument track from a catalog entry (generic VST3; not GA/HALion product paths).
    void addGenericInstrumentTrackFromCatalog(const mini_daw::InstrumentCatalogEntry& entry);

    /// Slice 1: discovery/catalog only — no track creation. Runs OOP scan on a background thread.
    void rescanInstrumentPluginsFromMenu();

private:
    void finishAddGrooveAgentInstrumentTrackAfterInstrumentResolved();
    void beginAsyncGrooveAgentOopScanForAddTrack(mini_daw::Vst3GrooveCacheLoadCandidate v1Cand);

    void finishAddHalionSonicInstrumentTrackAfterInstrumentResolved();
    void beginAsyncHalionSonicOopScanForAddTrack(mini_daw::Vst3GrooveCacheLoadCandidate v1Cand);

    Refs refs_;
    Callbacks callbacks_;
    std::atomic<bool> instrumentCatalogRescanBusy_{false};
};
