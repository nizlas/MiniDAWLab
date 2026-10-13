#pragma once

#include <atomic>
#include <functional>
#include <memory>

#include <juce_gui_basics/juce_gui_basics.h>

#include "plugins/Vst3ChildProcessScan.h"
#include "util/AsyncLifetimeToken.h"

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
        /// The shell append (and a failed add's `removeTrack`) publishes a new snapshot before any
        /// plugin work. The live callback walks the previous `RoutingPlan` by track index; the new
        /// row sits in front of Stereo Out, so the master's index now names the instrument and the
        /// device copy is skipped. Republish in the same turn, before the slow load.
        std::function<void()> republishRoutingPlan;
    };

    AddInstrumentTrackCoordinator(Refs refs, Callbacks callbacks);

    void addGrooveAgentInstrumentTrackFromMenu();
    void addHalionSonicInstrumentTrackFromMenu();

    /// Slice 2: create an instrument track from a catalog entry (generic VST3; not GA/HALion product paths).
    void addGenericInstrumentTrackFromCatalog(const mini_daw::InstrumentCatalogEntry& entry);

    /// Slice 1: discovery/catalog only — no track creation. Runs OOP scan on a background thread.
    void rescanInstrumentPluginsFromMenu();

    /// "Import plugin cache...": pick a portable descriptions-cache XML, repair paths to this machine and
    /// merge into the local v2 cache on a background thread (never runs the OOP scan). Shows a summary dialog.
    void importPluginCacheFromMenu();

private:
    void finishAddGrooveAgentInstrumentTrackAfterInstrumentResolved();
    void beginAsyncGrooveAgentOopScanForAddTrack(mini_daw::Vst3GrooveCacheLoadCandidate v1Cand);

    void finishAddHalionSonicInstrumentTrackAfterInstrumentResolved();
    void beginAsyncHalionSonicOopScanForAddTrack(mini_daw::Vst3GrooveCacheLoadCandidate v1Cand);
    void republishRoutingPlanAfterStructuralEdit();

    Refs refs_;
    Callbacks callbacks_;
    std::atomic<bool> instrumentCatalogRescanBusy_{false};
    std::atomic<bool> pluginCacheImportBusy_{false};
    /// Kept alive across the async native dialog (juce::FileChooser requirement).
    std::unique_ptr<juce::FileChooser> pluginCacheImportChooser_;

    /// Stability Slice 4: invalidated on destruction; every detached-thread / callAsync / chooser
    /// completion checks the guard before touching `this`.
    mini_daw::AsyncLifetimeOwnerToken asyncLifetime_;
};
