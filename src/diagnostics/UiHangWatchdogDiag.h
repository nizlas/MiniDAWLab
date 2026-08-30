#pragma once

// =============================================================================
// UiHangWatchdogDiag — gated message-thread stall detector (UI freeze triage)
// =============================================================================
// When `MINIDAW_DIAG_UI_HANG` is 1 (see DiagnosticBuildFlags.h), `install()` starts one
// background thread that watches a message-thread heartbeat. If the heartbeat has not advanced
// for >500 ms (and again at >2 s) while the process keeps running, it appends the last known UI
// state to `%APPDATA%/MiniDAWLab/ui-hang-diag.log`: follow on/off, milliseconds since the last
// follow pan / user viewport change, visible range and playhead sample.
//
// All setters are relaxed-atomic writes so they are safe to call every playhead frame; with the
// flag off everything compiles to nothing. The watchdog thread never touches JUCE UI objects —
// it only reads atomics and appends to the log file.
// =============================================================================

#include "diagnostics/DiagnosticBuildFlags.h"

#include <juce_core/juce_core.h>

#include <atomic>

namespace ui_hang_watchdog
{

inline constexpr bool kEnabled = MINIDAW_DIAG_UI_HANG != 0;

struct State
{
    std::atomic<double> lastHeartbeatMs { 0.0 };
    std::atomic<bool> followOn { false };
    std::atomic<double> lastFollowPanMs { 0.0 };
    std::atomic<double> lastUserViewportChangeMs { 0.0 };
    std::atomic<long long> visibleStartSamples { 0 };
    std::atomic<double> samplesPerPixel { 0.0 };
    std::atomic<double> playheadDisplaySamples { 0.0 };
};

inline State& state() noexcept
{
    static State s;
    return s;
}

inline void heartbeat() noexcept
{
    if constexpr (kEnabled)
    {
        state().lastHeartbeatMs.store(
            juce::Time::getMillisecondCounterHiRes(), std::memory_order_relaxed);
    }
}

inline void noteFollowState(const bool on) noexcept
{
    if constexpr (kEnabled)
    {
        state().followOn.store(on, std::memory_order_relaxed);
    }
}

inline void noteFollowPan() noexcept
{
    if constexpr (kEnabled)
    {
        state().lastFollowPanMs.store(
            juce::Time::getMillisecondCounterHiRes(), std::memory_order_relaxed);
    }
}

inline void noteUserViewportChange() noexcept
{
    if constexpr (kEnabled)
    {
        state().lastUserViewportChangeMs.store(
            juce::Time::getMillisecondCounterHiRes(), std::memory_order_relaxed);
    }
}

inline void notePlayheadFrame(
    const long long visibleStartSamples, const double samplesPerPixel, const double playheadDisplaySamples) noexcept
{
    if constexpr (kEnabled)
    {
        state().visibleStartSamples.store(visibleStartSamples, std::memory_order_relaxed);
        state().samplesPerPixel.store(samplesPerPixel, std::memory_order_relaxed);
        state().playheadDisplaySamples.store(playheadDisplaySamples, std::memory_order_relaxed);
    }
}

inline void appendLine(const juce::String& message)
{
    try
    {
        const juce::File f = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
                                 .getChildFile("MiniDAWLab")
                                 .getChildFile("ui-hang-diag.log");
        if (!f.getParentDirectory().isDirectory())
        {
            (void)f.getParentDirectory().createDirectory();
        }
        (void)f.appendText(juce::Time::getCurrentTime().toISO8601(true) + " " + message + "\n");
    }
    catch (...)
    {
    }
}

class WatchdogThread final : public juce::Thread
{
public:
    WatchdogThread() : juce::Thread("UiHangWatchdog") {}

    ~WatchdogThread() override { stopThread(2000); }

    void run() override
    {
        // Escalating stall report: one line when the stall crosses 500 ms, another at 2 s, then
        // one line per ~2 s while it persists (bounded log volume during a long freeze).
        double lastReportedStallMs = 0.0;
        while (!threadShouldExit())
        {
            wait(250);
            const double now = juce::Time::getMillisecondCounterHiRes();
            const double hb = state().lastHeartbeatMs.load(std::memory_order_relaxed);
            if (hb <= 0.0)
            {
                continue; // message thread has not started heartbeating yet
            }
            const double age = now - hb;
            if (age < 500.0)
            {
                lastReportedStallMs = 0.0;
                continue;
            }
            const bool firstReport = lastReportedStallMs <= 0.0;
            const bool escalate = age - lastReportedStallMs >= 2000.0;
            if (!firstReport && !escalate)
            {
                continue;
            }
            lastReportedStallMs = age;
            const double lastPan = state().lastFollowPanMs.load(std::memory_order_relaxed);
            const double lastUser = state().lastUserViewportChangeMs.load(std::memory_order_relaxed);
            appendLine(
                juce::String("UI STALL ageMs=") + juce::String(age, 0)
                + " followOn=" + (state().followOn.load(std::memory_order_relaxed) ? "1" : "0")
                + " sinceFollowPanMs=" + (lastPan > 0.0 ? juce::String(now - lastPan, 0) : "never")
                + " sinceUserViewportMs=" + (lastUser > 0.0 ? juce::String(now - lastUser, 0) : "never")
                + " visStart="
                + juce::String((juce::int64)state().visibleStartSamples.load(std::memory_order_relaxed))
                + " spp=" + juce::String(state().samplesPerPixel.load(std::memory_order_relaxed), 4)
                + " playhead="
                + juce::String(state().playheadDisplaySamples.load(std::memory_order_relaxed), 0));
        }
    }
};

/// Starts the watchdog thread once (no-op when the flag is off). Never stopped explicitly: the
/// thread only reads atomics/appends to a file and is terminated with the process.
inline void install()
{
    if constexpr (kEnabled)
    {
        static WatchdogThread thread;
        if (!thread.isThreadRunning())
        {
            appendLine("watchdog installed");
            thread.startThread();
        }
    }
}

} // namespace ui_hang_watchdog
