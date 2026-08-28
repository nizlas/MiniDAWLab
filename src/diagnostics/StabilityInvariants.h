#pragma once

// =============================================================================
// StabilityInvariants — runtime invariants after high-risk operations (Stability C3)
// =============================================================================
// [Message thread only] — never call from the audio callback.
//
// `verifyStableState` runs a battery of consistency checks over the session
// snapshot, routing plan, instrument runtime registry, insert plugin realtime
// map, UI attachments, selection, and MIDI editor binding. Failures are logged
// to `%APPDATA%\MiniDAWLab\stability-invariant.log` and counted; in Debug a
// jassert fires after logging (never crashes Release builds).
//
// A checker closure (built by MainAppWindow, which can see all subsystems) is
// registered globally so coordinators (delete/undo/load) can trigger a check
// without threading callbacks through every struct.
// =============================================================================

#include <JuceHeader.h>

#include "domain/Track.h"

#include <functional>
#include <memory>
#include <vector>

class SessionSnapshot;
struct RoutingPlan;
struct ExperimentalInstrumentPlaybackSnapshot;

namespace stability_invariants
{

/// One keyed instrument runtime as seen by the invariant checks (registry row).
struct InstrumentRuntimeInfo
{
    TrackId trackId = kInvalidTrackId;
    const void* host = nullptr;
    const void* controller = nullptr;
};

/// One insert chain / realtime-map entry: track id + opaque processor identities.
struct InsertEntryInfo
{
    TrackId trackId = kInvalidTrackId;
    std::vector<const void*> processors;
};

/// Everything `verifyStableState` needs. Each getter runs on the message thread
/// at check time; leave a getter empty to skip that family of checks.
struct Context
{
    std::function<std::shared_ptr<const SessionSnapshot>()> getSessionSnapshot;
    std::function<TrackId()> getActiveTrackId;
    std::function<std::shared_ptr<const RoutingPlan>()> getRoutingPlan;
    std::function<std::shared_ptr<const ExperimentalInstrumentPlaybackSnapshot>()>
        getPlaybackBridgeSnapshot;
    std::function<std::vector<InstrumentRuntimeInfo>()> listInstrumentRuntimes;
    std::function<std::vector<InsertEntryInfo>()> listInsertChains;
    std::function<std::vector<InsertEntryInfo>()> listPublishedInsertMap;
    std::function<std::vector<TrackId>()> listInstrumentTimelineAttachmentTrackIds;
    std::function<bool()> hasStaleHeaderDragSource;
    /// kInvalidTrackId when no MIDI editor is open.
    std::function<TrackId()> getOpenMidiEditorTrackId;
    /// Empty string when the autosave pointer state is consistent; otherwise a description.
    std::function<juce::String()> describeAutosavePointerIssue;
    /// Stability C5: the session's current save target path (empty when never saved / recovered).
    /// A recovered autosave must never claim the save path, so a current path whose file name is
    /// "autosave.dalproj" is an invariant failure.
    std::function<juce::String()> getCurrentProjectFilePath;
};

/// Runs all checks; logs every failure ("INVARIANT FAIL ...") and allowed anomalies
/// ("note: ...") to stability-invariant.log. Returns true when no invariant failed.
bool verifyStableState(const juce::String& reason, const Context& ctx);

/// Total "INVARIANT FAIL" count since process start (read by the stability runner so a
/// failure at any call site fails the scenario even if the runner's own check passed).
[[nodiscard]] int getStabilityInvariantFailureCount() noexcept;

/// MainAppWindow registers a closure wrapping `verifyStableState` with its live Context;
/// pass nullptr on teardown. Coordinators call `runRegisteredStabilityInvariantsCheck`.
void registerGlobalStabilityInvariantChecker(std::function<bool(const juce::String&)> checker);

/// Invokes the registered checker (true when none is registered). [Message thread]
bool runRegisteredStabilityInvariantsCheck(const juce::String& reason);

/// Stability C5: while true, invariant 8 (save path must not be an autosave file) is downgraded
/// to a note — mid-recovery the autosave *is* briefly the current file until the path is cleared.
/// Set/cleared by ProjectIoCoordinator around the recovery load. [Message thread]
void setAutosaveRecoveryInProgress(bool inProgress) noexcept;
[[nodiscard]] bool isAutosaveRecoveryInProgress() noexcept;

} // namespace stability_invariants
