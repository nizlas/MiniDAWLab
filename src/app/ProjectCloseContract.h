#pragma once

// Project close may release or destroy a plugin instance only after every realtime holder has
// left it. Each flag is an observed exit (callback section clear, callback-not-in-processing,
// acknowledged read-ahead pause). A timed-out wait is never a true value.

namespace project_close
{

enum class InstrumentCloseUnloadStep
{
    NoneLeft,  ///< no host still has a loaded plugin
    Unloaded,  ///< exactly one plugin was released
    StillHeld, ///< the owner is still retained; the instance was NOT released
};

[[nodiscard]] inline bool mayReleaseInstances(const bool instrumentSectionIdle,
                                              const bool audioCallbackExited,
                                              const bool readAheadPauseAcknowledged) noexcept
{
    return instrumentSectionIdle && audioCallbackExited && readAheadPauseAcknowledged;
}

} // namespace project_close
