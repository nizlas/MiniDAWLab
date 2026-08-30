#pragma once

#include <juce_core/juce_core.h>

// Appends one aggregated line to `%APPDATA%\MiniDAWLab\playback-ui-load.log`.
//
// Purpose: answer "is playback audio-light or audio-stressed, and is playhead flicker UI-side?"
// without logging per callback or per frame. Written at most once per second from the transport UI
// timer while the audio callback is running, and only when `MINIDAW_DIAG_PLAYBACK_UI_LOAD` is 1.
// [Message thread only] — never call from the audio callback.

void appendPlaybackUiLoadDiagnosticLine(const juce::String& message);

[[nodiscard]] juce::File getPlaybackUiLoadDiagnosticLogFile();
