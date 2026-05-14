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
