#pragma once

// =============================================================================
// ProxyUpdatePolicyService — the P1H per-destination proxy update-policy engine
// (steering docs/PORTABLE_INSTRUMENTS_AND_PROXIES.md §18.1/§18.2, §9.4.4, §20)
// =============================================================================
// One MESSAGE-THREAD runtime service that turns the persisted per-destination
// `proxyUpdateMode` ("auto" | "onSave" | "manual" | "off", v20 §12.2) into
// scheduler requests through the narrow P1E API. It owns NO jobs, NO plugin
// state and NO persisted data — everything here is runtime-only policy state
// (§20: countdowns, last-edit timestamps and pending flags never dirty the
// project and never create undo entries, PI-027).
//
// RENDER-RELEVANT INVALIDATION (complete by construction): the service polls
// each destination's CURRENT canonical identity (expected fingerprint + Primary
// semantic revision — the same §9.4.2 pair the scheduler compares) and treats
// any observed change as the render-relevant edit signal. Because the canonical
// fingerprint already covers every render-relevant input (notes, timing,
// velocity, Note Off gate, channels, routed sources and their order, CC, MIDI
// To, output channel, mute/enable per the §8.4 gate, Primary identity/version/
// revision, timeline reference conversion, policy versions — §11.1) and
// excludes everything after the process boundary (DAL inserts, fader, pan,
// routing, unrelated tracks, viewport state — §11.2/§11.3), per-destination
// relevance filtering is exact and can never miss a source or trigger on an
// excluded one. The whole-project dirty flag is deliberately NOT an input.
//
// MODES (§18.1, Locked):
//   * Auto after idle — an observed render-relevant change starts/resets the
//     destination's FIXED five-minute idle window (P1 constant, not user
//     configurable); the render request is issued only after five continuous
//     minutes without a further relevant change, and only when the destination
//     is not already Current. Same-identity render failures are not auto-
//     retried (no failure loop); the next relevant change or an explicit Retry
//     re-arms. Recording pauses request eligibility (the scheduler already
//     pauses start/progress); transport playback pauses nothing (§14.3).
//   * On Save — staleness is detected normally; only an explicit successful
//     user Save queues stale destinations (autosave NEVER does, §18.2).
//   * Manual — staleness is detected normally; only Render now / Retry request.
//   * Off — no automatic or manual maintenance (the P1J portable flow MAY run
//     an explicit one-shot render without changing the persisted mode, §16.6);
//     existing metadata/assets stay untouched and honest staleness detection
//     continues through the normal derived-state queries.
//
// SNAPSHOT ELIGIBILITY (§9.4.4): when a request becomes due, capture is
// deferred while the destination's host is not observably quiescent (active
// host MIDI/CC delivery within the debounce window). The pending request is
// retained and started on a later tick WITHOUT another edit; the runtime
// status exposes WaitingForSnapshot. Host-notifier silence is a practical
// observation, never proof of internal plugin quiescence (§9.4.5).
//
// CLOCK: injectable monotonic milliseconds (production:
// juce::Time::getMillisecondCounterHiRes) so tests — and the automated
// integration plan — advance time without waiting five real minutes. The
// production ticker is a dedicated juce::Timer owned HERE (never the
// MainWindow/content-view UI tick).
//
// OWNERSHIP: constructed by the same project-runtime owner as
// AppProxyRenderEngine / ProxyPlaybackCoordinator, wired only through
// message-thread std::function dependencies (no Session/host/UI references).

#include "domain/Track.h"
#include "instruments/ProxyRenderScheduler.h"

#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <utility>
#include <vector>

namespace proxy_policy
{

//==============================================================================
// Mode vocabulary (persisted spelling is the v20 string; §18.1)
//==============================================================================
enum class ProxyUpdateMode : int
{
    AutoAfterIdle = 0,
    OnSave,
    Manual,
    Off,
};

[[nodiscard]] inline ProxyUpdateMode parseProxyUpdateMode(const juce::String& persisted) noexcept
{
    if (persisted == "onSave")
    {
        return ProxyUpdateMode::OnSave;
    }
    if (persisted == "manual")
    {
        return ProxyUpdateMode::Manual;
    }
    if (persisted == "off")
    {
        return ProxyUpdateMode::Off;
    }
    return ProxyUpdateMode::AutoAfterIdle; // "auto" + unrecognized repair (matches ProjectFile)
}

[[nodiscard]] inline const char* proxyUpdateModePersistedString(const ProxyUpdateMode m) noexcept
{
    switch (m)
    {
        case ProxyUpdateMode::AutoAfterIdle: return "auto";
        case ProxyUpdateMode::OnSave: return "onSave";
        case ProxyUpdateMode::Manual: return "manual";
        case ProxyUpdateMode::Off: return "off";
    }
    return "auto";
}

/// P1 policy constants (Locked): the Auto idle delay is FIXED at five minutes;
/// the quiescence debounce follows the §9.4.4 host-observable window.
inline constexpr double kAutoIdleDelayMs = 5.0 * 60.0 * 1000.0;
inline constexpr double kSnapshotQuiescenceDebounceMs = 500.0;

//==============================================================================
// Runtime-only policy status (never persisted, never undoable — §20)
//==============================================================================
enum class ProxyPolicyRuntimeState : int
{
    Passive = 0,          ///< nothing pending (current / absent / mode-passive)
    WaitingForIdle,       ///< Auto: stale; five-minute idle window running
    WaitingForSave,       ///< On Save: stale; the next explicit Save queues it
    WaitingForManual,     ///< Manual: stale; waiting for Render now / Retry
    WaitingForSnapshot,   ///< request due; snapshot capture deferred (§9.4.4)
    RequestedToScheduler, ///< an active job exists (queued … finalizing)
};

[[nodiscard]] inline const char* proxyPolicyRuntimeStateName(const ProxyPolicyRuntimeState s)
{
    switch (s)
    {
        case ProxyPolicyRuntimeState::Passive: return "Passive";
        case ProxyPolicyRuntimeState::WaitingForIdle: return "WaitingForIdle";
        case ProxyPolicyRuntimeState::WaitingForSave: return "WaitingForSave";
        case ProxyPolicyRuntimeState::WaitingForManual: return "WaitingForManual";
        case ProxyPolicyRuntimeState::WaitingForSnapshot: return "WaitingForSnapshot";
        case ProxyPolicyRuntimeState::RequestedToScheduler: return "RequestedToScheduler";
    }
    return "?";
}

struct ProxyPolicyStatus
{
    ProxyUpdateMode mode = ProxyUpdateMode::AutoAfterIdle;
    ProxyPolicyRuntimeState state = ProxyPolicyRuntimeState::Passive;
    double idleRemainingMs = 0.0; ///< meaningful in WaitingForIdle (runtime-only)
    bool recordingPaused = false; ///< a due request is being held by recording
};

//==============================================================================
// The service
//==============================================================================
class ProxyUpdatePolicyService final
{
public:
    /// The §9.4.2 identity pair of one destination (mirrors ProxyCurrentIdentity;
    /// duplicated here so tests drive the service without a scheduler engine).
    struct DestinationIdentity
    {
        bool exists = false; ///< destination present AND renderable (usable Primary)
        juce::String fingerprint;
        std::uint64_t revision = 0;
    };

    /// All callables run on the MESSAGE thread and must not retain UI state.
    struct Dependencies
    {
        /// Injectable monotonic clock in milliseconds (production:
        /// juce::Time::getMillisecondCounterHiRes). Tests use a fake.
        std::function<double()> nowMs;
        /// Every current instrument destination (order irrelevant).
        std::function<std::vector<TrackId>()> listDestinations;
        /// Persisted v20 mode string of a destination ("auto" | "onSave" | …).
        std::function<juce::String(TrackId)> modeForTrack;
        /// Current expected identity (fingerprint + revision); !exists when the
        /// destination is missing or not renderable (no usable Primary).
        std::function<DestinationIdentity(TrackId)> identityForTrack;
        /// Scheduler seams (narrow P1E API).
        std::function<proxy_render::ProxyDestinationState(TrackId)> destinationState;
        std::function<proxy_render::ProxyJobStatus(TrackId)> jobStatus;
        std::function<proxy_render::ProxyJobStatus(TrackId)> requestRender;
        std::function<void(TrackId)> cancelDestination;
        /// Scheduler obsolescence notification on an observed identity change.
        std::function<void(TrackId)> notifyIdentityChanged;
        /// Recording pauses request eligibility (§14.3); transport playback never.
        std::function<bool()> recordingActive;
        /// §9.4.4 host-observable quiescence; null = always eligible. False defers
        /// snapshot capture (status WaitingForSnapshot) — retried without an edit.
        std::function<bool(TrackId)> snapshotEligible;
        /// Optional: fired when a render-relevant change is observed for a
        /// destination (production: playback-coordinator refresh for honest
        /// ProxyStale status while Primary is unavailable).
        std::function<void(TrackId)> onRenderRelevantChangeObserved;
    };

    explicit ProxyUpdatePolicyService(Dependencies deps) : deps_(std::move(deps))
    {
        jassert(deps_.nowMs != nullptr);
    }

    ~ProxyUpdatePolicyService() { stopProductionTicker(); }

    //==========================================================================
    // Production ticker (dedicated timer — NEVER the UI tick)
    //==========================================================================
    void startProductionTicker(const int intervalMs = 1000)
    {
        if (ticker_ == nullptr)
        {
            ticker_ = std::make_unique<Ticker>(*this);
        }
        ticker_->startTimer(intervalMs);
    }

    void stopProductionTicker()
    {
        if (ticker_ != nullptr)
        {
            ticker_->stopTimer();
        }
    }

    //==========================================================================
    // Tick (message thread; tests call this directly with a fake clock)
    //==========================================================================
    /// Observe identity changes, run the mode policies, issue due requests.
    void tick()
    {
        if (!deps_.listDestinations)
        {
            return;
        }
        const double now = deps_.nowMs();
        const std::vector<TrackId> live = deps_.listDestinations();
        std::set<TrackId> liveSet;
        for (const TrackId tid : live)
        {
            liveSet.insert(tid);
            pollOne(tid, now);
        }
        // Destinations that vanished (deleted / project replaced) drop all state.
        for (auto it = states_.begin(); it != states_.end();)
        {
            it = liveSet.count(it->first) == 0 ? states_.erase(it) : std::next(it);
        }
    }

    //==========================================================================
    // External events (message thread)
    //==========================================================================
    /// A successful EXPLICIT user Save (§18.2). On Save destinations queue their
    /// stale work now; Auto destinations only get an immediate evaluation pass
    /// (Save never forces a not-yet-idle Auto destination — no render storm).
    /// Autosave MUST NOT call this.
    void noteSuccessfulUserSave()
    {
        if (!deps_.listDestinations)
        {
            return;
        }
        for (const TrackId tid : deps_.listDestinations())
        {
            if (parseProxyUpdateMode(deps_.modeForTrack ? deps_.modeForTrack(tid)
                                                        : juce::String())
                != ProxyUpdateMode::OnSave)
            {
                continue;
            }
            if (destinationNeedsRender(tid))
            {
                states_[tid].explicitPending = true;
            }
        }
        tick(); // queues On Save work + any already-eligible Auto work; waits for nothing
    }

    /// Explicit "Render now" (Manual mode only — §19; Off never maintains).
    /// Returns false when the mode does not permit it or nothing is renderable.
    bool renderNow(const TrackId destination)
    {
        const ProxyUpdateMode mode = modeOf(destination);
        if (mode != ProxyUpdateMode::Manual)
        {
            return false;
        }
        DestState& st = states_[destination];
        st.explicitPending = true;
        st.suppressedFailureFingerprint.clear();
        tick();
        return true;
    }

    /// Explicit "Retry" after a failure — available in every maintaining mode
    /// (Auto / On Save / Manual), never in Off (§19).
    bool retry(const TrackId destination)
    {
        const ProxyUpdateMode mode = modeOf(destination);
        if (mode == ProxyUpdateMode::Off)
        {
            return false;
        }
        DestState& st = states_[destination];
        st.explicitPending = true;
        st.suppressedFailureFingerprint.clear();
        tick();
        return true;
    }

    /// Explicit "Cancel" of the destination's active job (any maintaining mode).
    void cancel(const TrackId destination)
    {
        if (deps_.cancelDestination)
        {
            deps_.cancelDestination(destination);
        }
        // An explicitly cancelled request is no longer pending; staleness remains
        // honest through the derived destination state.
        if (const auto it = states_.find(destination); it != states_.end())
        {
            it->second.explicitPending = false;
            it->second.autoDue = false;
        }
    }

    /// Project close/replacement: every timer/pending flag is runtime-only state
    /// of the OLD project (the scheduler's notifyProjectChanged handles jobs).
    void noteProjectChanged() { states_.clear(); }

    //==========================================================================
    // Runtime status (message thread; immutable value for P1I)
    //==========================================================================
    [[nodiscard]] ProxyPolicyStatus statusForTrack(const TrackId destination) const
    {
        ProxyPolicyStatus out;
        out.mode = modeOf(destination);
        const auto it = states_.find(destination);
        const DestState* st = it != states_.end() ? &it->second : nullptr;

        if (deps_.jobStatus)
        {
            const auto job = deps_.jobStatus(destination);
            if (job.exists && !isTerminal(job.phase))
            {
                out.state = ProxyPolicyRuntimeState::RequestedToScheduler;
                return out;
            }
        }
        if (st != nullptr && st->waitingForSnapshot)
        {
            out.state = ProxyPolicyRuntimeState::WaitingForSnapshot;
            out.recordingPaused = deps_.recordingActive && deps_.recordingActive();
            return out;
        }
        const bool needsRender = destinationNeedsRender(destination);
        switch (out.mode)
        {
            case ProxyUpdateMode::AutoAfterIdle:
                if (st != nullptr && st->autoDue && needsRender)
                {
                    out.state = ProxyPolicyRuntimeState::WaitingForIdle;
                    const double now = deps_.nowMs ? deps_.nowMs() : 0.0;
                    out.idleRemainingMs = juce::jmax(
                        0.0, kAutoIdleDelayMs - (now - st->lastRelevantChangeMs));
                    out.recordingPaused = out.idleRemainingMs <= 0.0
                                          && deps_.recordingActive && deps_.recordingActive();
                }
                break;
            case ProxyUpdateMode::OnSave:
                if (needsRender)
                {
                    out.state = ProxyPolicyRuntimeState::WaitingForSave;
                }
                break;
            case ProxyUpdateMode::Manual:
                if (needsRender)
                {
                    out.state = ProxyPolicyRuntimeState::WaitingForManual;
                }
                break;
            case ProxyUpdateMode::Off:
                break; // honest staleness shows through the derived destination state
        }
        return out;
    }

private:
    struct DestState
    {
        bool identityKnown = false;
        juce::String fingerprint;
        std::uint64_t revision = 0;
        double lastRelevantChangeMs = -1.0;
        bool autoDue = false;          ///< Auto: change observed; idle window running
        bool explicitPending = false;  ///< Manual/Retry/On-Save request awaiting eligibility
        bool waitingForSnapshot = false;
        juce::String suppressedFailureFingerprint; ///< Auto: no same-identity failure loop
    };

    [[nodiscard]] static bool isTerminal(const proxy_render::ProxyJobPhase p) noexcept
    {
        return p == proxy_render::ProxyJobPhase::Published
               || p == proxy_render::ProxyJobPhase::Obsolete
               || p == proxy_render::ProxyJobPhase::Cancelled
               || p == proxy_render::ProxyJobPhase::Failed;
    }

    [[nodiscard]] ProxyUpdateMode modeOf(const TrackId tid) const
    {
        return parseProxyUpdateMode(deps_.modeForTrack ? deps_.modeForTrack(tid)
                                                       : juce::String());
    }

    /// "Needs render" = the derived destination state is not Current for the
    /// expected identity (Stale / Absent / Failed) — never "latest file on disk".
    [[nodiscard]] bool destinationNeedsRender(const TrackId tid) const
    {
        if (!deps_.destinationState)
        {
            return false;
        }
        switch (deps_.destinationState(tid))
        {
            case proxy_render::ProxyDestinationState::Current:
            case proxy_render::ProxyDestinationState::Rendering:
                return false;
            case proxy_render::ProxyDestinationState::Absent:
            case proxy_render::ProxyDestinationState::Stale:
            case proxy_render::ProxyDestinationState::Failed:
                return true;
        }
        return false;
    }

    void pollOne(const TrackId tid, const double now)
    {
        const DestinationIdentity id
            = deps_.identityForTrack ? deps_.identityForTrack(tid) : DestinationIdentity{};
        if (!id.exists)
        {
            // Not renderable (missing/unusable Primary): keep NO policy state.
            // Playback honesty for this case is the P1G selector's job.
            states_.erase(tid);
            return;
        }
        DestState& st = states_[tid];
        const bool changed = st.identityKnown
                             && (st.fingerprint != id.fingerprint || st.revision != id.revision);
        if (changed)
        {
            // Render-relevant change observed for THIS destination (§18.1): the
            // status verdict is immediate (derived queries recompute identity);
            // only the render start waits for the idle window.
            st.lastRelevantChangeMs = now;
            st.autoDue = true;
            st.suppressedFailureFingerprint.clear();
            if (deps_.notifyIdentityChanged)
            {
                deps_.notifyIdentityChanged(tid); // in-flight job obsolescence (PI-028)
            }
            if (deps_.onRenderRelevantChangeObserved)
            {
                deps_.onRenderRelevantChangeObserved(tid);
            }
        }
        st.identityKnown = true;
        st.fingerprint = id.fingerprint;
        st.revision = id.revision;
        evaluate(tid, st, now);
    }

    void evaluate(const TrackId tid, DestState& st, const double now)
    {
        // An active job means the request was already handed over.
        if (deps_.jobStatus)
        {
            const auto job = deps_.jobStatus(tid);
            if (job.exists && !isTerminal(job.phase))
            {
                st.waitingForSnapshot = false;
                return;
            }
            // Same-identity failure: Auto must not loop (§13.1 Failed is a terminal
            // verdict until an edit or an explicit Retry re-arms the destination).
            if (job.exists && job.phase == proxy_render::ProxyJobPhase::Failed
                && job.expectedFingerprint == st.fingerprint && !st.explicitPending)
            {
                st.suppressedFailureFingerprint = st.fingerprint;
            }
        }

        if (!destinationNeedsRender(tid))
        {
            // Already Current for the expected fingerprint (or nothing derivable):
            // do nothing — including after an undo that restored currency.
            st.autoDue = false;
            st.explicitPending = false;
            st.waitingForSnapshot = false;
            return;
        }

        bool wantRequest = false;
        switch (modeOf(tid))
        {
            case ProxyUpdateMode::AutoAfterIdle:
                // First in-session sight of an ALREADY-stale Auto destination (project
                // load with a stale proxy, or a mode change to Auto while stale): arm a
                // full five-minute window NOW. The idle timer is runtime-only (§20) and
                // never persists, so this is the deterministic re-arm on reopen; an
                // explicit edit later simply resets the window through pollOne.
                if (!st.autoDue && !st.explicitPending && st.lastRelevantChangeMs < 0.0)
                {
                    st.lastRelevantChangeMs = now;
                    st.autoDue = true;
                }
                wantRequest = st.explicitPending // Retry
                              || (st.autoDue && st.lastRelevantChangeMs >= 0.0
                                  && now - st.lastRelevantChangeMs >= kAutoIdleDelayMs
                                  && st.suppressedFailureFingerprint != st.fingerprint);
                break;
            case ProxyUpdateMode::OnSave:
            case ProxyUpdateMode::Manual:
                wantRequest = st.explicitPending
                              && st.suppressedFailureFingerprint != st.fingerprint;
                break;
            case ProxyUpdateMode::Off:
                st.explicitPending = false; // Off never maintains (§18.1)
                return;
        }
        if (!wantRequest)
        {
            st.waitingForSnapshot = false;
            return;
        }

        // Recording pauses request eligibility (§14.3). The pending flag/idle
        // expiry survives; the request starts on a later tick after recording.
        if (deps_.recordingActive && deps_.recordingActive())
        {
            return;
        }

        // §9.4.4 snapshot eligibility: defer while the host is not observably
        // quiescent; retained and retried WITHOUT another edit.
        if (deps_.snapshotEligible && !deps_.snapshotEligible(tid))
        {
            st.waitingForSnapshot = true;
            return;
        }
        st.waitingForSnapshot = false;

        if (deps_.requestRender)
        {
            const auto status = deps_.requestRender(tid);
            if (status.exists)
            {
                st.autoDue = false;
                st.explicitPending = false;
            }
        }
    }

    struct Ticker final : juce::Timer
    {
        explicit Ticker(ProxyUpdatePolicyService& s) : service(s) {}
        void timerCallback() override { service.tick(); }
        ProxyUpdatePolicyService& service;
    };

    Dependencies deps_;
    std::map<TrackId, DestState> states_;
    std::unique_ptr<Ticker> ticker_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ProxyUpdatePolicyService)
};

} // namespace proxy_policy
