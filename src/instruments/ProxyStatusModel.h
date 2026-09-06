#pragma once

// =============================================================================
// ProxyStatusModel — the P1I pure presentation model for instrument proxy
// status (steering docs/PORTABLE_INSTRUMENTS_AND_PROXIES.md §19, §20, PI-021/
// PI-022; roadmap P1I). NO JUCE components here: one pure function maps the
// runtime inputs (P1G playback-source state, P1E derived destination state and
// job status, P1H policy status, metadata presence) to compact labels,
// explanatory tooltips and control availability. The UI (InspectorView proxy
// section) only displays the result and invokes the service — every wording
// and availability rule is therefore deterministic and unit-tested.
//
// Two DISTINCT axes, never merged (§19: "a track may currently play Primary
// while its proxy is Stale or Rendering"):
//   * source  — what the transport is playing NOW (P1G selector verdict);
//   * cache   — the proxy maintenance state (currency, policy, job).
// A stale retained proxy is never labelled current or playable (PI-020); a
// source change is always a visible status change (PI-021). Secondary does
// not exist in P1 and never appears in any string.
// =============================================================================

#include "instruments/ProxyPlaybackSource.h"
#include "instruments/ProxyRenderScheduler.h"
#include "instruments/ProxyUpdatePolicyService.h"

#include <juce_core/juce_core.h>

#include <cstdint>

namespace proxy_status
{

//==============================================================================
// Inputs (all copied by value from the runtime services on the message thread)
//==============================================================================
struct ProxyStatusInputs
{
    /// P1G playback-source verdict (what is audible NOW).
    proxy_playback::ProxyPlaybackSourceState sourceState
        = proxy_playback::ProxyPlaybackSourceState::Primary;
    /// P1E derived destination proxy state (identity verdict, never "latest file").
    proxy_render::ProxyDestinationState destinationState
        = proxy_render::ProxyDestinationState::Absent;
    /// P1E job snapshot of this destination (may be terminal or absent).
    proxy_render::ProxyJobStatus job;
    /// P1H policy status (mode + runtime policy state + idle countdown).
    proxy_policy::ProxyPolicyStatus policy;
    /// Published v20 metadata presence for this destination.
    bool hasMetadata = false;
    /// The published generation is an explicit silent generation (§15.7).
    bool silentGeneration = false;
};

//==============================================================================
// Output view (immutable value; UI displays verbatim)
//==============================================================================
struct ProxyStatusView
{
    juce::String sourceLabel;  ///< compact "playing now" label
    juce::String cacheLabel;   ///< compact proxy cache/maintenance label
    juce::String progressText; ///< non-empty only while rendering ("12.3 s rendered")
    juce::String tooltip;      ///< full explanation (source + cache + mode semantics)
    int modeComboIndex = 0;    ///< 0 Auto / 1 On Save / 2 Manual / 3 Off
    bool canRenderNow = false; ///< Manual mode, stale/absent/failed, no active job
    bool canCancel = false;    ///< an active (queued…finalizing) job exists
    bool canRetry = false;     ///< last job Failed, maintaining mode, no active job
    bool showProgress = false;
};

[[nodiscard]] inline bool isTerminalPhase(const proxy_render::ProxyJobPhase p) noexcept
{
    return p == proxy_render::ProxyJobPhase::Published
           || p == proxy_render::ProxyJobPhase::Obsolete
           || p == proxy_render::ProxyJobPhase::Cancelled
           || p == proxy_render::ProxyJobPhase::Failed;
}

[[nodiscard]] inline const char* failureReasonText(const proxy_render::ProxyRenderFailureReason r)
{
    using FR = proxy_render::ProxyRenderFailureReason;
    switch (r)
    {
        case FR::None: return "unspecified";
        case FR::SnapshotInvalid: return "the render request was not usable";
        case FR::PluginCreationFailed: return "the plugin could not be instantiated";
        case FR::StateRestoreFailed: return "the plugin state could not be restored";
        case FR::PrepareFailed: return "the plugin rejected the render configuration";
        case FR::TailLimitReached: return "tail limit reached — render incomplete";
        case FR::NonFiniteAudio: return "the plugin produced invalid audio";
        case FR::WavWriteFailed: return "the audio file could not be written (disk)";
        case FR::WavValidationFailed: return "the rendered file failed validation";
    }
    return "unspecified";
}

[[nodiscard]] inline juce::String formatRenderedMs(const std::int64_t ms)
{
    return juce::String((double)ms / 1000.0, 1) + " s rendered";
}

/// Fixed §18.1 Auto-mode explanation (the five-minute tooltip contract).
[[nodiscard]] inline juce::String autoModeTooltipText()
{
    return "Auto after idle: an edit marks the proxy stale immediately; rendering "
           "begins after five minutes without further relevant edits. Saving never "
           "waits for rendering, and playback can continue while it renders.";
}

//==============================================================================
// The mapping
//==============================================================================
[[nodiscard]] inline ProxyStatusView buildProxyStatusView(const ProxyStatusInputs& in)
{
    using Src = proxy_playback::ProxyPlaybackSourceState;
    using Dst = proxy_render::ProxyDestinationState;
    using Phase = proxy_render::ProxyJobPhase;
    using Mode = proxy_policy::ProxyUpdateMode;
    using RS = proxy_policy::ProxyPolicyRuntimeState;

    ProxyStatusView v;
    v.modeComboIndex = (int)in.policy.mode;

    // ---------------------------------------------------------------- source
    juce::String sourceTip;
    switch (in.sourceState)
    {
        case Src::Primary:
            v.sourceLabel = "Primary";
            sourceTip = "Playing the live Primary instrument.";
            break;
        case Src::ProxyPreparing:
            v.sourceLabel = "Proxy preparing";
            sourceTip = "The proxy is selected; its playback representation is still "
                        "being prepared. Silence until ready (never wrong-speed audio).";
            break;
        case Src::ProxyCurrent:
            if (in.silentGeneration)
            {
                v.sourceLabel = "Proxy current (silent)";
                sourceTip = "Playing the current proxy: an intentional silent generation "
                            "(this instrument renders no audio for the current material).";
            }
            else
            {
                v.sourceLabel = "Proxy current";
                sourceTip = "Primary is unavailable; playing the current rendered proxy "
                            "through the normal track chain.";
            }
            break;
        case Src::ProxyStale:
            v.sourceLabel = "Proxy stale";
            sourceTip = "Primary is unavailable and the retained proxy no longer matches "
                        "the current musical content — honest silence (a stale proxy is "
                        "never played).";
            break;
        case Src::ProxyMissing:
            v.sourceLabel = "Proxy missing";
            sourceTip = "Primary is unavailable and the proxy audio file is missing on "
                        "disk — honest silence. The project stays fully editable.";
            break;
        case Src::ProxyCorrupt:
            v.sourceLabel = "Proxy corrupt";
            sourceTip = "Primary is unavailable and the proxy asset or its metadata is "
                        "unreadable — honest silence. Nothing was deleted.";
            break;
        case Src::MissingPrimary:
            v.sourceLabel = "Primary missing";
            sourceTip = "The Primary instrument is unavailable and no usable proxy "
                        "exists — honest silence.";
            break;
        case Src::PlaybackUnderrun:
            v.sourceLabel = "Playback underrun";
            sourceTip = "Proxy playback could not read audio in time (disk starvation); "
                        "output was silence until recovery. This is not end-of-file.";
            break;
    }

    // ----------------------------------------------------------------- cache
    const bool activeJob = in.job.exists && !isTerminalPhase(in.job.phase);
    juce::String cacheTip;
    if (activeJob)
    {
        switch (in.job.phase)
        {
            case Phase::Queued:
                v.cacheLabel = "Render queued";
                cacheTip = "A proxy render is queued on the background worker.";
                break;
            case Phase::Preparing:
                v.cacheLabel = "Render preparing";
                cacheTip = "Capturing state and preparing the isolated render instance.";
                break;
            case Phase::Rendering:
                v.cacheLabel = "Rendering";
                v.showProgress = true;
                v.progressText = formatRenderedMs(in.job.progressRenderedMs);
                cacheTip = "Rendering the proxy in the background at low priority. "
                           "Playback and editing continue normally.";
                break;
            case Phase::Finalizing:
                v.cacheLabel = "Publishing";
                cacheTip = "Validating and publishing the rendered proxy.";
                break;
            default: break;
        }
        v.canCancel = true;
    }
    else if (in.policy.state == RS::WaitingForSnapshot)
    {
        v.cacheLabel = "Waiting for quiescence";
        cacheTip = "A render is due, but the instrument is still receiving MIDI/CC; "
                   "state capture waits for a quiet moment and then starts by itself.";
    }
    else if (in.policy.state == RS::WaitingForIdle)
    {
        const int totalSec = (int)((in.policy.idleRemainingMs + 999.0) / 1000.0);
        v.cacheLabel = "Waiting for idle ("
                       + juce::String(totalSec / 60) + ":"
                       + juce::String(totalSec % 60).paddedLeft('0', 2) + ")";
        cacheTip = juce::String("The proxy is stale. ") + autoModeTooltipText();
        if (in.policy.recordingPaused)
        {
            v.cacheLabel = "Waiting (recording)";
            cacheTip << " Rendering is paused while recording.";
        }
    }
    else
    {
        switch (in.destinationState)
        {
            case Dst::Current:
                v.cacheLabel = in.silentGeneration ? "Proxy current (silent)" : "Proxy current";
                cacheTip = in.silentGeneration
                               ? "The published proxy is an intentional silent generation "
                                 "matching the current musical content."
                               : "The published proxy matches the current musical content.";
                break;
            case Dst::Stale:
                switch (in.policy.mode)
                {
                    case Mode::OnSave:
                        v.cacheLabel = "Stale (renders on Save)";
                        cacheTip = "The proxy no longer matches the musical content. The "
                                   "next Save queues the render; Save never waits for it.";
                        break;
                    case Mode::Manual:
                        v.cacheLabel = "Stale (manual)";
                        cacheTip = "The proxy no longer matches the musical content. Use "
                                   "Render now to update it.";
                        break;
                    case Mode::Off:
                        v.cacheLabel = "Stale (updates off)";
                        cacheTip = "The proxy no longer matches the musical content and "
                                   "automatic updates are off. The retained files stay "
                                   "safe; a stale proxy is never played.";
                        break;
                    case Mode::AutoAfterIdle:
                        v.cacheLabel = "Proxy stale";
                        cacheTip = juce::String("The proxy no longer matches the musical "
                                                "content. ")
                                   + autoModeTooltipText();
                        break;
                }
                break;
            case Dst::Failed:
            {
                v.cacheLabel = "Render failed";
                juce::String reason;
                if (in.job.exists && in.job.phase == Phase::Failed)
                {
                    reason = failureReasonText(in.job.result.failureReason);
                    if (in.job.message.isNotEmpty())
                    {
                        reason << " (" << in.job.message << ")";
                    }
                }
                cacheTip = "The last proxy render failed: "
                           + (reason.isNotEmpty() ? reason : juce::String("see log"))
                           + ". The previous proxy was kept. Use Retry to render again.";
                break;
            }
            case Dst::Rendering: // active job already handled; defensive only
                v.cacheLabel = "Rendering";
                cacheTip = "Rendering the proxy in the background.";
                break;
            case Dst::Absent:
                if (in.job.exists && in.job.phase == Phase::Cancelled)
                {
                    v.cacheLabel = "Cancelled";
                    cacheTip = "The proxy render was cancelled. Nothing was published; "
                               "any previous proxy was kept.";
                }
                else if (in.hasMetadata)
                {
                    // Metadata exists but no valid current generation is derivable
                    // (missing/corrupt asset cases surface through the source axis).
                    v.cacheLabel = "Proxy unavailable";
                    cacheTip = "Proxy metadata exists but no valid current proxy is "
                               "available (missing or unreadable asset). Nonfatal: the "
                               "project stays fully editable.";
                }
                else
                {
                    v.cacheLabel = "No proxy";
                    cacheTip = "No proxy has been rendered for this instrument yet.";
                }
                break;
        }
        if (in.job.exists && in.job.phase == Phase::Cancelled
            && in.destinationState == Dst::Stale)
        {
            v.cacheLabel = "Cancelled (stale)";
            cacheTip << " The last render was cancelled before completion.";
        }
    }

    // -------------------------------------------------------------- controls
    const bool needsRender = in.destinationState == Dst::Stale
                             || in.destinationState == Dst::Failed
                             || in.destinationState == Dst::Absent;
    v.canRenderNow = !activeJob && in.policy.mode == Mode::Manual && needsRender;
    v.canRetry = !activeJob && in.policy.mode != Mode::Off && in.job.exists
                 && in.job.phase == Phase::Failed;

    // --------------------------------------------------------------- tooltip
    v.tooltip = "Playing: " + v.sourceLabel + ". " + sourceTip + "\n\nProxy: "
                + v.cacheLabel + ". " + cacheTip;
    if (in.policy.mode == Mode::AutoAfterIdle && in.policy.state != RS::WaitingForIdle)
    {
        v.tooltip << "\n\n" << autoModeTooltipText();
    }
    return v;
}

} // namespace proxy_status
