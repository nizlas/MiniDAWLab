#include "app/TrackPowerCoordinator.h"

#include "instruments/InstrumentTrackController.h"
#include "plugins/ExperimentalInstrumentHost.h"
#include "plugins/PluginInsertHost.h"
#include "diagnostics/StabilityDiagnosticLog.h"

namespace
{
constexpr int kReconcileIntervalMs = 250;
constexpr int kWakeStepIntervalMs = 1;
constexpr int kUnloadStepIntervalMs = 20;
} // namespace

TrackPowerCoordinator::TrackPowerCoordinator(PluginInsertHost& inserts, Callbacks callbacks)
    : inserts_(inserts), cb_(std::move(callbacks))
{
    startTimer(kReconcileIntervalMs);
}

TrackPowerCoordinator::~TrackPowerCoordinator()
{
    stopTimer();
}

bool TrackPowerCoordinator::requestPowerOn(const TrackId tid)
{
    if (cb_.transportBusy && cb_.transportBusy())
    {
        return false;
    }
    const auto snap = cb_.loadSnapshot ? cb_.loadSnapshot() : nullptr;
    const int ix = snap != nullptr ? snap->findTrackIndexById(tid) : -1;
    if (ix < 0)
    {
        return false;
    }
    if (!snap->getTrack(ix).isTrackOff() || jobs_.count(tid) != 0)
    {
        return true;
    }
    failedInstrumentLoad_.erase(tid);
    if (!rowHasUnloadedPlugins(tid))
    {
        cb_.setTrackOff(tid, false);
        return true;
    }
    WakeJob job;
    job.token = nextToken_++;
    job.insertsTotal = inserts_.countDormantInserts(tid);
    jobs_[tid] = std::move(job);
    if (cb_.showProgress)
    {
        cb_.showProgress("Loading " + rowName(tid) + "...");
    }
    scheduleTimer();
    return true;
}

void TrackPowerCoordinator::noteProjectChanged()
{
    jobs_.clear();
    failedInstrumentLoad_.clear();
    lastReconciled_.reset();
    retryPending_ = false;
    if (cb_.showProgress)
    {
        cb_.showProgress({});
    }
    scheduleTimer();
}

bool TrackPowerCoordinator::rowHasLivePlugins(const TrackId tid) const
{
    if (inserts_.hasLiveInsertInstances(tid))
    {
        return true;
    }
    if (ExperimentalInstrumentHost* const h = cb_.instrumentHost ? cb_.instrumentHost(tid) : nullptr;
        h != nullptr && h->hasInstrument())
    {
        return true;
    }
    return cb_.hasSecondaryInstance && cb_.hasSecondaryInstance(tid);
}

bool TrackPowerCoordinator::rowHasUnloadedPlugins(const TrackId tid) const
{
    if (inserts_.hasDormantInserts(tid))
    {
        return true;
    }
    if (failedInstrumentLoad_.count(tid) != 0)
    {
        return false; // already tried; the row keeps the saved plug-in as data
    }
    InstrumentTrackController* const ctl = cb_.instrumentController ? cb_.instrumentController(tid) : nullptr;
    return ctl != nullptr && ctl->retainedPrimaryRowNeedsLoad();
}

juce::String TrackPowerCoordinator::rowName(const TrackId tid) const
{
    const auto snap = cb_.loadSnapshot ? cb_.loadSnapshot() : nullptr;
    const int ix = snap != nullptr ? snap->findTrackIndexById(tid) : -1;
    return ix >= 0 ? snap->getTrack(ix).getName() : juce::String("track");
}

bool TrackPowerCoordinator::unloadRow(const Track& tr)
{
    const TrackId tid = tr.getId();
    bool complete = true;
    if (inserts_.hasLiveInsertInstances(tid)
        && inserts_.makeTrackInsertsDormant(tid) == PluginInsertHost::DormancyResult::NotDrained)
    {
        complete = false;
    }
    if (tr.getKind() != TrackKind::Instrument)
    {
        return complete;
    }
    ExperimentalInstrumentHost* const host = cb_.instrumentHost ? cb_.instrumentHost(tid) : nullptr;
    InstrumentTrackController* const ctl = cb_.instrumentController ? cb_.instrumentController(tid) : nullptr;
    if (host != nullptr && ctl != nullptr && host->hasInstrument())
    {
        // The Off snapshot is published, so the engine stops calling this host. One observed
        // exit of the callback means no block that started before it is still inside.
        if (!cb_.waitForAudioCallbackExit || !cb_.waitForAudioCallbackExit())
        {
            complete = false;
        }
        else if (!ctl->captureRetainedPrimaryRowForUnload())
        {
            // The state only lives in the plug-in: keep it loaded (Off still silences it).
            failedInstrumentLoad_.insert(tid);
        }
        else
        {
            appendTrackPowerDiagnosticLine("Off: unload instrument \"" + host->getInstrumentNameForUi() + "\" track "
                                           + juce::String((juce::int64)tid));
            writeLastOperationBreadcrumb("track power: unload instrument track " + juce::String((juce::int64)tid));
            if (host->unloadInstrumentAfterAcknowledgedDrain())
            {
                ctl->syncShellWithHostState();
            }
            else
            {
                complete = false; // still referenced; the retained row is refreshed on the retry
            }
        }
    }
    if (cb_.hasSecondaryInstance && cb_.hasSecondaryInstance(tid))
    {
        if (ctl != nullptr)
        {
            (void)ctl->buildExperimentalInstrumentProjectBlock(); // refreshes the Secondary blob
        }
        if (cb_.retireSecondaryInstance)
        {
            cb_.retireSecondaryInstance(tid);
        }
    }
    return complete;
}

void TrackPowerCoordinator::reconcileNow()
{
    const auto snap = cb_.loadSnapshot ? cb_.loadSnapshot() : nullptr;
    if (snap == nullptr || (snap == lastReconciled_ && !retryPending_))
    {
        return;
    }
    lastReconciled_ = snap;
    retryPending_ = false;
    bool changed = false;

    for (auto it = jobs_.begin(); it != jobs_.end();)
    {
        it = snap->findTrackIndexById(it->first) < 0 ? jobs_.erase(it) : std::next(it);
    }
    for (int i = 0; i < snap->getNumTracks(); ++i)
    {
        const Track& tr = snap->getTrack(i);
        const TrackId tid = tr.getId();
        if (InstrumentTrackController* const ctl = cb_.instrumentController ? cb_.instrumentController(tid) : nullptr)
        {
            ctl->syncPowerMirrorFromSession();
        }
        if (jobs_.count(tid) != 0)
        {
            continue;
        }
        if (tr.isTrackOff())
        {
            failedInstrumentLoad_.erase(tid);
            if (rowHasLivePlugins(tid))
            {
                if (changed)
                {
                    retryPending_ = true; // one row per turn keeps the UI responsive
                    continue;
                }
                retryPending_ = !unloadRow(tr) || retryPending_;
                changed = true;
            }
        }
        else if (rowHasUnloadedPlugins(tid))
        {
            // A snapshot restore turned the row On while its plug-ins are unloaded: keep it Off
            // until they are back, exactly like a Power-On request.
            cb_.setTrackOff(tid, true);
            WakeJob job;
            job.token = nextToken_++;
            job.insertsTotal = inserts_.countDormantInserts(tid);
            jobs_[tid] = std::move(job);
            changed = true;
        }
    }
    if (changed && cb_.onRuntimeChanged)
    {
        cb_.onRuntimeChanged();
    }
    scheduleTimer();
}

void TrackPowerCoordinator::finishJob(const TrackId tid, WakeJob& job)
{
    const juce::StringArray problems = job.problems;
    jobs_.erase(tid);
    cb_.setTrackOff(tid, false);
    if (jobs_.empty() && cb_.showProgress)
    {
        cb_.showProgress({});
    }
    if (cb_.onRuntimeChanged)
    {
        cb_.onRuntimeChanged();
    }
    if (!problems.isEmpty() && cb_.reportLoadProblem)
    {
        cb_.reportLoadProblem(rowName(tid) + " is On, but some plug-ins could not be loaded. Their settings "
                              "are kept and saved with the project.\n\n" + problems.joinIntoString("\n"));
    }
}

void TrackPowerCoordinator::stepOneWakeJob()
{
    if (jobs_.empty())
    {
        return;
    }
    const auto snap = cb_.loadSnapshot ? cb_.loadSnapshot() : nullptr;
    const TrackId tid = jobs_.begin()->first;
    WakeJob& job = jobs_.begin()->second;
    const int ix = snap != nullptr ? snap->findTrackIndexById(tid) : -1;
    if (ix < 0)
    {
        jobs_.erase(tid); // deleted while loading
        return;
    }
    const std::uint64_t token = job.token;
    const juce::String name = snap->getTrack(ix).getName();

    if (!job.instrumentDone)
    {
        job.instrumentDone = true;
        ExperimentalInstrumentHost* const host = cb_.instrumentHost ? cb_.instrumentHost(tid) : nullptr;
        InstrumentTrackController* const ctl = cb_.instrumentController ? cb_.instrumentController(tid) : nullptr;
        if (host != nullptr && ctl != nullptr && ctl->hasRetainedPrimaryRow())
        {
            if (cb_.showProgress)
            {
                cb_.showProgress("Loading " + name + ": instrument");
            }
            appendTrackPowerDiagnosticLine("On: load instrument track " + juce::String((juce::int64)tid));
            writeLastOperationBreadcrumb("track power: load instrument track " + juce::String((juce::int64)tid));
            juce::String warning;
            if (!ctl->reloadPrimaryFromRetainedRow(*host, warning))
            {
                failedInstrumentLoad_.insert(tid);
                job.problems.add("Instrument: " + warning);
            }
            ctl->syncShellWithHostState();
            return; // one plug-in per message-loop turn
        }
    }
    if (inserts_.hasDormantInserts(tid))
    {
        if (cb_.showProgress)
        {
            cb_.showProgress("Loading " + name + ": insert " + juce::String(job.insertsDone + 1) + " of "
                             + juce::String(juce::jmax(job.insertsTotal, job.insertsDone + 1)));
        }
        juce::String pluginName;
        juce::String error;
        (void)inserts_.wakeNextDormantInsert(tid, pluginName, error);
        // The job may have been dropped while the plug-in loaded (re-entrant message loop).
        if (const auto it = jobs_.find(tid); it != jobs_.end() && it->second.token == token)
        {
            ++it->second.insertsDone;
            if (error.isNotEmpty())
            {
                it->second.problems.add(error);
            }
        }
        return;
    }
    if (cb_.transportBusy && cb_.transportBusy())
    {
        return; // everything is loaded; Power flips when the transport stops
    }
    finishJob(tid, job);
}

void TrackPowerCoordinator::scheduleTimer()
{
    const int wanted = !jobs_.empty() ? kWakeStepIntervalMs
                                      : (retryPending_ || inserts_.offRetiredInsertCount() > 0 ? kUnloadStepIntervalMs
                                                                                                : kReconcileIntervalMs);
    if (getTimerInterval() != wanted)
    {
        startTimer(wanted);
    }
}

void TrackPowerCoordinator::timerCallback()
{
    // Released Off instances are destroyed one per tick, never in the turn that released them.
    if (inserts_.destroyNextOffRetiredInsert(0.0))
    {
        scheduleTimer();
        return;
    }
    if (!jobs_.empty())
    {
        stepOneWakeJob();
    }
    reconcileNow();
    scheduleTimer();
}
