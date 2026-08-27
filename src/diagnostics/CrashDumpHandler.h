#pragma once

// =============================================================================
// CrashDumpHandler — Windows minidump on unhandled exception (Stability C1)
// =============================================================================
// On crash, writes to `%APPDATA%\MiniDAWLab\crash-dumps\`:
//   MiniDAWLab-crash-YYYYMMDD-HHMMSS-pidXXXX.dmp                (minidump)
//   MiniDAWLab-crash-YYYYMMDD-HHMMSS-pidXXXX.txt                (metadata: version,
//       build config, exception code/address, faulting module + offset)
//   MiniDAWLab-crash-YYYYMMDD-HHMMSS-pidXXXX-last-operation.txt (breadcrumb copy)
// The handler uses only Win32 calls on preformatted static buffers (no JUCE, no
// message thread, no heap allocation of our own) and then returns
// EXCEPTION_CONTINUE_SEARCH so Windows Error Reporting still records the usual
// Event Log entry. Symbolize offsets with `scripts/symbolize-crash.ps1` against
// the PDB archived by `scripts/package-windows.ps1` (dist\symbols).
// =============================================================================

/// [Any thread, call once early in startup] Installs the process-wide unhandled
/// exception filter and precomputes crash-dump paths. Safe no-op on failure.
void installCrashDumpHandler() noexcept;

/// Hidden `--stability-crash-test` support: raises a deliberate access violation
/// (null write) so the crash-dump pipeline can be verified end to end. Never
/// called during normal use; not exposed in any UI.
void triggerIntentionalCrashForStabilityTest() noexcept;
