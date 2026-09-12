#pragma once

// =============================================================================
// InstrumentAlternativesPopup — per-track callout for instrument destinations
// =============================================================================
// Anchored to the small "Instrument alternatives" button in the track header
// (bottom-left control strip). Contains everything that used to live in the
// Inspector's "Instrument Proxy" and "Instrument alternatives" sections:
//   * Primary identity + availability,
//   * Secondary select/replace/remove, Editor, channel mapping (steering §17,
//     PID-008/PID-009), load-failure reason + Retry,
//   * actual current sound source, proxy status, update mode and the existing
//     Render now / Cancel / Retry render controls.
// The popup is a juce::CallOutBox: closing it never stops playback, never
// unloads an instrument and never cancels rendering — it only stops LOOKING at
// the same seams the Inspector used. Content self-dismisses when the track
// disappears (deletion / project replacement).
// =============================================================================

#include "domain/Track.h"
#include "instruments/ProxyStatusModel.h" // proxy_status::ProxyStatusView (pure model)

#include <functional>

#include <juce_gui_basics/juce_gui_basics.h>

/// [Message thread] Instrument-proxy seams (wired from Main). The popup never touches the
/// scheduler/policy services directly: it displays the precomputed ProxyStatusView and invokes
/// these narrow actions. All optional (null = row hidden).
struct InstrumentProxyUiHost
{
    /// True only for instrument destinations with a proxy-capable runtime.
    std::function<bool(TrackId)> isProxyDestination;
    /// Complete precomputed status view (labels, tooltip, control availability).
    std::function<proxy_status::ProxyStatusView(TrackId)> getStatusView;
    /// Persisted per-destination update mode (0 Auto / 1 On Save / 2 Manual / 3 Off).
    /// Dirty semantics live behind this seam (§18.3), never in the view.
    std::function<void(TrackId, int modeComboIndex)> setUpdateMode;
    std::function<void(TrackId)> renderNow;
    std::function<void(TrackId)> cancelRender;
    std::function<void(TrackId)> retryRender;
};

/// [Message thread] P2 Secondary-instrument seams (steering §17/§19, PID-008/PID-009). Shows the
/// Primary identity, lets the user assign an OPTIONAL Secondary working instrument, open its
/// editor, choose the Secondary-only channel mapping, and retry a failed load. Dirty semantics
/// live behind the seams. All optional (null = row hidden).
struct InstrumentSecondaryUiHost
{
    /// Precomputed compact view for one destination.
    struct View
    {
        juce::String primaryText;   ///< Primary identity + availability, e.g. "VB3-II (missing)"
        juce::String secondaryText; ///< assigned Secondary name, or "None"
        bool hasSecondary = false;
        int forcedMidiChannel = 0;  ///< 0 = Preserve channels; 1..16 = Force channel N
    };
    /// Same visibility rule as the proxy rows (instrument destinations only).
    std::function<bool(TrackId)> isInstrumentDestination;
    std::function<View(TrackId)> getView;
    /// Display names of the existing instrument catalogue (picker menu order == index).
    std::function<juce::StringArray()> listCatalogInstrumentNames;
    std::function<void(TrackId, int catalogIndex)> selectSecondaryFromCatalog;
    std::function<void(TrackId)> removeSecondary;
    /// EXPLICIT user action: loads the Secondary if needed (bypassing the automatic failure
    /// latch), then opens its native editor.
    std::function<void(TrackId)> openSecondaryEditor;
    /// 0 = Preserve channels; 1..16 = Force channel N (Secondary delivery only).
    std::function<void(TrackId, int forcedChannel)> setChannelMapping;
    /// Human-readable reason for the most recent failed Secondary load (empty = none).
    std::function<juce::String(TrackId)> getLoadFailureReason;
    /// EXPLICIT retry: clears the automatic failure latch and attempts one fresh load.
    std::function<bool(TrackId)> retryLoad;
};

namespace instrument_alternatives_popup
{
    /// [Message thread] Launch the callout anchored at `screenAnchor` (the header button's screen
    /// bounds). The callout sizes itself to fit the display and scrolls internally if necessary;
    /// it owns its content and self-dismisses when `tid` stops being an instrument destination.
    void show(TrackId tid,
              juce::Rectangle<int> screenAnchor,
              InstrumentProxyUiHost proxyHost,
              InstrumentSecondaryUiHost secondaryHost);
} // namespace instrument_alternatives_popup
