#pragma once

// Optional compile-time flags for `%APPDATA%/MiniDAWLab/` **diagnostic file logs** (instrument lifecycle
// and playback-routing traces). Defaults are **off** (`0`). They are **not** part of the product /
// architecture contract — enable explicitly when triaging (`CMake`/`cl`/`#define` before include).

#ifndef MINIDAW_DIAG_PLAYBACK_ROUTING
#define MINIDAW_DIAG_PLAYBACK_ROUTING 0 // `experimental-playback-routing.log` (off unless overridden)
#endif

#ifndef MINIDAW_DIAG_INSTRUMENT_LIFECYCLE
#define MINIDAW_DIAG_INSTRUMENT_LIFECYCLE 0 // `experimental-instrument.log` (off unless overridden)
#endif

// `playback-ui-load.log`: once-per-second aggregate of audio callback duration / block budget and
// UI playhead timer + invalidation behaviour. Used to separate UI render jitter from audio load.
#ifndef MINIDAW_DIAG_PLAYBACK_UI_LOAD
#define MINIDAW_DIAG_PLAYBACK_UI_LOAD 0
#endif

// `ui-hang-diag.log`: background watchdog that detects a stalled message thread (no heartbeat for
// >500 ms / >2 s while the process keeps running) and logs the last known UI state (follow on/off,
// last follow pan / user viewport change, visible range, playhead). Used to triage UI freezes.
#ifndef MINIDAW_DIAG_UI_HANG
#define MINIDAW_DIAG_UI_HANG 0
#endif

// `transport-shortcut.log`: per-keypress trace of the main-window shortcut router and the
// jump-to-left-locator invocation (key codes, focus, dispatch/ignore reason, playhead
// before/after). Used to triage "shortcut only works sometimes" reports.
#ifndef MINIDAW_DIAG_TRANSPORT_SHORTCUT
#define MINIDAW_DIAG_TRANSPORT_SHORTCUT 0
#endif
