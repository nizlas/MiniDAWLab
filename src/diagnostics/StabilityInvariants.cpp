// =============================================================================
// StabilityInvariants.cpp — runtime invariants after high-risk operations (C3)
// =============================================================================

#include "diagnostics/StabilityInvariants.h"

#include "diagnostics/StabilityDiagnosticLog.h"
#include "diagnostics/StabilityScenarioRunner.h"
#include "domain/SessionRouting.h"
#include "domain/SessionSnapshot.h"
#include "engine/PlaybackEngine.h"
#include "engine/RoutingPlan.h"

#include <algorithm>
#include <atomic>

namespace stability_invariants
{
namespace
{
    std::atomic<int> g_invariantFailureCount{ 0 };
    std::function<bool(const juce::String&)> g_registeredChecker;

    [[nodiscard]] const char* trackKindName(const TrackKind k) noexcept
    {
        switch (k)
        {
            case TrackKind::Audio: return "Audio";
            case TrackKind::Instrument: return "Instrument";
            case TrackKind::Group: return "Group";
            case TrackKind::Master: return "Master";
        }
        return "?";
    }

    /// Collects failures/notes for one verify pass and writes them with a shared prefix.
    struct CheckReport
    {
        const juce::String& reason;
        int failCount = 0;

        void fail(const juce::String& check, const juce::String& detail)
        {
            ++failCount;
            g_invariantFailureCount.fetch_add(1, std::memory_order_relaxed);
            appendStabilityInvariantLine("INVARIANT FAIL reason=" + reason + " check=" + check
                                         + " " + detail);
        }

        void note(const juce::String& check, const juce::String& detail)
        {
            appendStabilityInvariantLine("note reason=" + reason + " check=" + check + " "
                                         + detail);
        }
    };

    [[nodiscard]] juce::String describeSnapshotTrackIds(const SessionSnapshot& snap)
    {
        juce::String s = "snapshotTracks=[";
        for (int i = 0; i < snap.getNumTracks(); ++i)
        {
            const Track& t = snap.getTrack(i);
            if (i > 0)
            {
                s << " ";
            }
            s << juce::String((juce::int64)(std::int64_t) t.getId()) << ":"
              << trackKindName(t.getKind());
        }
        s << "]";
        return s;
    }

    // -------------------------------------------------------------------------
    // Invariant 1: RoutingPlan / SessionSnapshot compatibility (mandatory, C2B class).
    // -------------------------------------------------------------------------
    void checkRoutingPlanAgainstSnapshot(CheckReport& report,
                                         const SessionSnapshot& snap,
                                         const RoutingPlan* plan)
    {
        if (plan == nullptr)
        {
            report.note("routing-plan", "no routing plan published (allowed when empty session)");
            return;
        }
        const int n = snap.getNumTracks();
        const int busCount = static_cast<int>(plan->busScratchL.size());

        const auto checkStep = [&](const char* stepFamily,
                                   const int stepIndex,
                                   const int trackIndex,
                                   const TrackId builtFromId,
                                   const TrackKind builtFromKind)
        {
            if (trackIndex < 0 || trackIndex >= n)
            {
                report.fail("routing-plan",
                            juce::String(stepFamily) + "[" + juce::String(stepIndex)
                                + "] trackIndex=" + juce::String(trackIndex)
                                + " out of range (snapshot trackCount=" + juce::String(n) + ") "
                                + describeSnapshotTrackIds(snap));
                return;
            }
            const Track& tr = snap.getTrack(trackIndex);
            if (builtFromId != kInvalidTrackId && tr.getId() != builtFromId)
            {
                report.fail("routing-plan",
                            juce::String(stepFamily) + "[" + juce::String(stepIndex)
                                + "] trackIndex=" + juce::String(trackIndex)
                                + " id drift: builtFromId="
                                + juce::String((juce::int64)(std::int64_t) builtFromId)
                                + " nowId=" + juce::String((juce::int64)(std::int64_t) tr.getId())
                                + " " + describeSnapshotTrackIds(snap));
                return;
            }
            if (tr.getKind() != builtFromKind)
            {
                report.fail("routing-plan",
                            juce::String(stepFamily) + "[" + juce::String(stepIndex)
                                + "] trackIndex=" + juce::String(trackIndex) + " kind drift: builtFrom="
                                + trackKindName(builtFromKind) + " now=" + trackKindName(tr.getKind()));
            }
        };

        for (size_t i = 0; i < plan->sourceSteps.size(); ++i)
        {
            const RoutingPlan::SourceStep& st = plan->sourceSteps[i];
            checkStep("sourceStep", (int) i, st.trackIndex, st.builtFromTrackId, st.builtFromTrackKind);
            if (st.destBusIndex < 0 || st.destBusIndex >= busCount)
            {
                report.fail("routing-plan",
                            "sourceStep[" + juce::String((int) i) + "] destBusIndex="
                                + juce::String(st.destBusIndex) + " out of range (busCount="
                                + juce::String(busCount) + ")");
            }
        }
        for (size_t i = 0; i < plan->busSteps.size(); ++i)
        {
            const RoutingPlan::BusStep& st = plan->busSteps[i];
            checkStep("busStep", (int) i, st.trackIndex, st.builtFromTrackId, st.builtFromTrackKind);
            if (st.sourceBusIndex < 0 || st.sourceBusIndex >= busCount)
            {
                report.fail("routing-plan",
                            "busStep[" + juce::String((int) i) + "] sourceBusIndex="
                                + juce::String(st.sourceBusIndex) + " out of range (busCount="
                                + juce::String(busCount) + ")");
            }
            // destBusIndex == -1 means "device outputs" (Master step) and is legal.
            if (st.destBusIndex >= busCount)
            {
                report.fail("routing-plan",
                            "busStep[" + juce::String((int) i) + "] destBusIndex="
                                + juce::String(st.destBusIndex) + " out of range (busCount="
                                + juce::String(busCount) + ")");
            }
        }
        if (!plan->busScratchL.empty() && plan->masterBusIndex >= plan->busScratchL.size())
        {
            report.fail("routing-plan",
                        "masterBusIndex=" + juce::String((int) plan->masterBusIndex)
                            + " out of range (busCount=" + juce::String(busCount) + ")");
        }
    }

    // -------------------------------------------------------------------------
    // Invariant 5: session routing/sends/master legality.
    // -------------------------------------------------------------------------
    void checkSessionRouting(CheckReport& report, const SessionSnapshot& snap)
    {
        int masterCount = 0;
        for (int i = 0; i < snap.getNumTracks(); ++i)
        {
            if (snap.getTrack(i).getKind() == TrackKind::Master)
            {
                ++masterCount;
            }
        }
        if (masterCount != 1)
        {
            report.fail("session-routing",
                        "master row count=" + juce::String(masterCount) + " (expected exactly 1) "
                            + describeSnapshotTrackIds(snap));
        }

        for (int i = 0; i < snap.getNumTracks(); ++i)
        {
            const Track& tr = snap.getTrack(i);
            const TrackId tid = tr.getId();

            const TrackId outDest = tr.getRoutedOutputTrackId();
            if (tr.getKind() != TrackKind::Master && outDest != kInvalidTrackId
                && !session_routing::isLegalRoutedOutputTarget(snap, tid, outDest))
            {
                report.fail("session-routing",
                            "trackId=" + juce::String((juce::int64)(std::int64_t) tid)
                                + " kind=" + trackKindName(tr.getKind()) + " routedOutput="
                                + juce::String((juce::int64)(std::int64_t) outDest)
                                + " is not a legal destination");
            }

            for (const TrackSend& send : tr.getSends())
            {
                if (!send.enabled || send.destTrackId == kInvalidTrackId)
                {
                    continue;
                }
                if (!session_routing::isLegalSendDestination(snap, tid, send.destTrackId))
                {
                    report.fail("session-routing",
                                "trackId=" + juce::String((juce::int64)(std::int64_t) tid)
                                    + " send dest="
                                    + juce::String((juce::int64)(std::int64_t) send.destTrackId)
                                    + " is not a legal send destination (missing/non-Group/cycle)");
                }
            }
        }
    }

    // -------------------------------------------------------------------------
    // Invariant 2: instrument runtime registry / session / playback bridge.
    // -------------------------------------------------------------------------
    void checkInstrumentRuntime(CheckReport& report,
                                const SessionSnapshot& snap,
                                const std::vector<InstrumentRuntimeInfo>& runtimes,
                                const ExperimentalInstrumentPlaybackSnapshot* bridge)
    {
        for (const InstrumentRuntimeInfo& rt : runtimes)
        {
            const int ti = snap.findTrackIndexById(rt.trackId);
            if (ti < 0)
            {
                report.fail("instrument-runtime",
                            "runtime trackId=" + juce::String((juce::int64)(std::int64_t) rt.trackId)
                                + " has no session track " + describeSnapshotTrackIds(snap));
                continue;
            }
            if (snap.getTrack(ti).getKind() != TrackKind::Instrument)
            {
                report.fail("instrument-runtime",
                            "runtime trackId=" + juce::String((juce::int64)(std::int64_t) rt.trackId)
                                + " session kind=" + trackKindName(snap.getTrack(ti).getKind())
                                + " (expected Instrument)");
            }
        }

        // Session Instrument rows without runtime: allowed placeholder (e.g. GenericVst3 restored
        // without its plugin). Logged, never failed.
        for (int i = 0; i < snap.getNumTracks(); ++i)
        {
            const Track& tr = snap.getTrack(i);
            if (tr.getKind() != TrackKind::Instrument)
            {
                continue;
            }
            const bool hasRuntime = std::any_of(runtimes.begin(), runtimes.end(),
                                                [&](const InstrumentRuntimeInfo& rt)
                                                { return rt.trackId == tr.getId(); });
            if (!hasRuntime)
            {
                report.note("instrument-runtime",
                            "allowed placeholder: Instrument trackId="
                                + juce::String((juce::int64)(std::int64_t) tr.getId()) + " name=\""
                                + tr.getName() + "\" has no keyed runtime");
            }
        }

        if (bridge != nullptr)
        {
            for (const ExperimentalInstrumentPlaybackEntry& e : bridge->entries)
            {
                const auto it = std::find_if(runtimes.begin(), runtimes.end(),
                                             [&](const InstrumentRuntimeInfo& rt)
                                             { return rt.trackId == e.trackId; });
                if (it == runtimes.end())
                {
                    report.fail("playback-bridge",
                                "bridge entry trackId="
                                    + juce::String((juce::int64)(std::int64_t) e.trackId)
                                    + " has no keyed runtime (stale bridge publish)");
                    continue;
                }
                if (static_cast<const void*>(e.host) != it->host
                    || static_cast<const void*>(e.midiController) != it->controller)
                {
                    report.fail("playback-bridge",
                                "bridge entry trackId="
                                    + juce::String((juce::int64)(std::int64_t) e.trackId)
                                    + " host/controller pointers differ from registry (stale publish)");
                }
            }
        }
    }

    // -------------------------------------------------------------------------
    // Invariant 3: insert plugin realtime map vs chain registry vs session.
    // -------------------------------------------------------------------------
    void checkInsertPlugins(CheckReport& report,
                            const SessionSnapshot& snap,
                            const std::vector<InsertEntryInfo>& chains,
                            const std::vector<InsertEntryInfo>& publishedMap)
    {
        for (const InsertEntryInfo& chain : chains)
        {
            if (snap.findTrackIndexById(chain.trackId) < 0)
            {
                report.fail("insert-chains",
                            "chain trackId=" + juce::String((juce::int64)(std::int64_t) chain.trackId)
                                + " has no session track (missed evict?) "
                                + describeSnapshotTrackIds(snap));
            }
        }

        for (const InsertEntryInfo& entry : publishedMap)
        {
            const auto chainIt = std::find_if(chains.begin(), chains.end(),
                                              [&](const InsertEntryInfo& c)
                                              { return c.trackId == entry.trackId; });
            if (chainIt == chains.end())
            {
                report.fail("insert-map",
                            "published map entry trackId="
                                + juce::String((juce::int64)(std::int64_t) entry.trackId)
                                + " references a retired/missing chain");
                continue;
            }
            for (const void* proc : entry.processors)
            {
                if (std::find(chainIt->processors.begin(), chainIt->processors.end(), proc)
                    == chainIt->processors.end())
                {
                    report.fail("insert-map",
                                "published map entry trackId="
                                    + juce::String((juce::int64)(std::int64_t) entry.trackId)
                                    + " holds a processor not present in the live chain "
                                      "(retired instance still published)");
                }
            }
        }
    }

    // -------------------------------------------------------------------------
    // Invariants 4 + 7: UI attachments, selection, MIDI editor binding.
    // -------------------------------------------------------------------------
    void checkUiAndSelection(CheckReport& report,
                             const SessionSnapshot& snap,
                             const Context& ctx,
                             const std::vector<InstrumentRuntimeInfo>& runtimes)
    {
        if (ctx.listInstrumentTimelineAttachmentTrackIds)
        {
            for (const TrackId tid : ctx.listInstrumentTimelineAttachmentTrackIds())
            {
                const int ti = snap.findTrackIndexById(tid);
                if (ti < 0)
                {
                    report.fail("ui-attachments",
                                "instrument timeline attachment trackId="
                                    + juce::String((juce::int64)(std::int64_t) tid)
                                    + " has no session track (stale attachment)");
                }
                else if (snap.getTrack(ti).getKind() != TrackKind::Instrument)
                {
                    report.fail("ui-attachments",
                                "instrument timeline attachment trackId="
                                    + juce::String((juce::int64)(std::int64_t) tid)
                                    + " session kind=" + trackKindName(snap.getTrack(ti).getKind()));
                }
            }
        }

        if (ctx.hasStaleHeaderDragSource && ctx.hasStaleHeaderDragSource())
        {
            report.fail("ui-drag",
                        "headerTrackDragSourceView_ set while no header drag is active (stale pointer)");
        }

        if (ctx.getActiveTrackId)
        {
            const TrackId active = ctx.getActiveTrackId();
            if (active != kInvalidTrackId && snap.findTrackIndexById(active) < 0)
            {
                report.fail("selection",
                            "activeTrackId=" + juce::String((juce::int64)(std::int64_t) active)
                                + " has no session track " + describeSnapshotTrackIds(snap));
            }
        }

        if (ctx.getOpenMidiEditorTrackId)
        {
            const TrackId editorTid = ctx.getOpenMidiEditorTrackId();
            if (editorTid != kInvalidTrackId)
            {
                const int ti = snap.findTrackIndexById(editorTid);
                if (ti < 0)
                {
                    report.fail("midi-editor",
                                "MIDI editor open on trackId="
                                    + juce::String((juce::int64)(std::int64_t) editorTid)
                                    + " which has no session track");
                }
                else if (snap.getTrack(ti).getKind() != TrackKind::Instrument)
                {
                    report.fail("midi-editor",
                                "MIDI editor open on trackId="
                                    + juce::String((juce::int64)(std::int64_t) editorTid)
                                    + " kind=" + trackKindName(snap.getTrack(ti).getKind()));
                }
                else if (!std::any_of(runtimes.begin(), runtimes.end(),
                                      [&](const InstrumentRuntimeInfo& rt)
                                      { return rt.trackId == editorTid; }))
                {
                    report.fail("midi-editor",
                                "MIDI editor open on trackId="
                                    + juce::String((juce::int64)(std::int64_t) editorTid)
                                    + " which has no keyed instrument runtime");
                }
            }
        }
    }
} // namespace

bool verifyStableState(const juce::String& reason, const Context& ctx)
{
    CheckReport report{ reason };

    std::shared_ptr<const SessionSnapshot> snap;
    if (ctx.getSessionSnapshot)
    {
        snap = ctx.getSessionSnapshot();
    }
    if (snap == nullptr)
    {
        report.fail("session", "no session snapshot published");
        return false;
    }

    // Invariant 1 + plan bus indices.
    if (ctx.getRoutingPlan)
    {
        const std::shared_ptr<const RoutingPlan> plan = ctx.getRoutingPlan();
        checkRoutingPlanAgainstSnapshot(report, *snap, plan.get());
    }

    // Invariant 5.
    checkSessionRouting(report, *snap);

    // Invariant 2.
    std::vector<InstrumentRuntimeInfo> runtimes;
    if (ctx.listInstrumentRuntimes)
    {
        runtimes = ctx.listInstrumentRuntimes();
    }
    std::shared_ptr<const ExperimentalInstrumentPlaybackSnapshot> bridge;
    if (ctx.getPlaybackBridgeSnapshot)
    {
        bridge = ctx.getPlaybackBridgeSnapshot();
    }
    checkInstrumentRuntime(report, *snap, runtimes, bridge.get());

    // Invariant 3.
    if (ctx.listInsertChains && ctx.listPublishedInsertMap)
    {
        checkInsertPlugins(report, *snap, ctx.listInsertChains(), ctx.listPublishedInsertMap());
    }

    // Invariants 4 + 7.
    checkUiAndSelection(report, *snap, ctx, runtimes);

    // Invariant 6 (partial): autosave pointer consistency, informational only — a dangling
    // pointer file is handled cleanly by the recovery scan, so it is not a failure.
    if (ctx.describeAutosavePointerIssue)
    {
        const juce::String issue = ctx.describeAutosavePointerIssue();
        if (issue.isNotEmpty())
        {
            report.note("autosave", issue);
        }
    }

    // Invariant 8 (Stability C5): a recovered autosave must never claim the session's save path —
    // recovery clears it so plain Save goes through Save As. If the current save target *is* an
    // autosave file, a Ctrl+S would silently overwrite the recovery snapshot.
    if (ctx.getCurrentProjectFilePath)
    {
        const juce::String currentPath = ctx.getCurrentProjectFilePath();
        if (currentPath.isNotEmpty()
            && juce::File(currentPath).getFileName().equalsIgnoreCase("autosave.dalproj"))
        {
            if (isAutosaveRecoveryInProgress())
            {
                // Transient mid-recovery state: the load just completed and the coordinator
                // clears the save path immediately afterwards.
                report.note("autosave-save-path",
                            "save path is the autosave (recovery in progress): " + currentPath);
            }
            else
            {
                report.fail("autosave-save-path",
                            "current project save path is an autosave file: " + currentPath);
            }
        }
    }

    if (report.failCount > 0)
    {
        appendStabilityInvariantLine("verify FAILED reason=" + reason + " failures="
                                     + juce::String(report.failCount));
        // Debug builds assert after logging; Release only logs (never crash user builds).
        jassertfalse;
        return false;
    }
    // In stability test mode, leave proof that the battery actually ran (normal use stays silent
    // on success so the log only grows when something is wrong).
    if (isStabilityTestModeActive())
    {
        appendStabilityInvariantLine("verify ok reason=" + reason);
    }
    return true;
}

int getStabilityInvariantFailureCount() noexcept
{
    return g_invariantFailureCount.load(std::memory_order_relaxed);
}

namespace
{
    bool g_autosaveRecoveryInProgress = false; // Message thread only.
}

void setAutosaveRecoveryInProgress(const bool inProgress) noexcept
{
    g_autosaveRecoveryInProgress = inProgress;
}

bool isAutosaveRecoveryInProgress() noexcept
{
    return g_autosaveRecoveryInProgress;
}

void registerGlobalStabilityInvariantChecker(std::function<bool(const juce::String&)> checker)
{
    g_registeredChecker = std::move(checker);
}

bool runRegisteredStabilityInvariantsCheck(const juce::String& reason)
{
    if (!g_registeredChecker)
    {
        return true;
    }
    return g_registeredChecker(reason);
}

} // namespace stability_invariants
