#pragma once

// Flip to true locally to trace Ctrl+Z/Y routing, SessionHistory record/pop, and executeUndoableSessionEdit.
// Must stay false in committed builds (no log spam).
// Log file: %APPDATA%\MiniDAWLab\undo-diagnostics.log (see diagnostics/UndoDiagnosticFileLog.h).

namespace undo_diagnostic
{
inline constexpr bool kUndoDiag = false;
}
