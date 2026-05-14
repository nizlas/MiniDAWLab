#pragma once

// Compile-time guards for noisy always-on `%APPDATA%/MiniDAWLab/` file logs used during instrument
// / routing diagnostics. Override by defining macros before compilation (CMake or command line).

#ifndef MINIDAW_DIAG_PLAYBACK_ROUTING
#define MINIDAW_DIAG_PLAYBACK_ROUTING 0
#endif

#ifndef MINIDAW_DIAG_INSTRUMENT_LIFECYCLE
#define MINIDAW_DIAG_INSTRUMENT_LIFECYCLE 0
#endif
