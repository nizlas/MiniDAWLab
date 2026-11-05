#pragma once

// =============================================================================
// IsolatedHostWorker — the helper process side (plan B.2, B.5)
// =============================================================================
//
// DAL's own executable started by `IsolatedHostConnection` with the JUCE child-process prefix for
// `kHelperCommandLineUniqueId`. It opens no audio device, reads and writes no user settings or
// projects, and owns the isolated plug-in instances: one processing thread per instance, all
// creation, state, preset and editor work on its message thread.
//
// It never runs plug-in destructors on exit: when the pipe to DAL goes away (DAL closed it, DAL
// ended, or DAL crashed) the helper terminates its own process at once. Only an explicit Destroy
// request runs a destructor.

#include <juce_core/juce_core.h>

namespace mini_daw::isolation
{
/// True when `commandLine` starts the helper.
[[nodiscard]] bool isIsolatedHelperCommandLine(const juce::String& commandLine);

/// [Message thread] Start the helper inside the running JUCE application. Returns false when the
/// pipe could not be connected; the caller should quit then.
[[nodiscard]] bool startIsolatedHelper(const juce::String& commandLine);
} // namespace mini_daw::isolation
