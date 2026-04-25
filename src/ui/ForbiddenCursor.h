#pragma once

// =============================================================================
// ForbiddenCursor  —  shared “no drop / not allowed” mouse cursor (message thread)
// =============================================================================
// JUCE has no portable `StandardCursorType` for no-drop on all platforms. A small generated
// 32x32 image is used; hotspot is the center. Used by `ClipWaveformView` (invalid cross-lane
// drop) and `TrackHeaderView` (track-reorder outside valid header strip), never on the audio path.
// =============================================================================

#include <juce_gui_basics/juce_gui_basics.h>

// [Message thread] One static `MouseCursor` for the app; first call creates the image.
[[nodiscard]] juce::MouseCursor getForbiddenNoDropMouseCursor() noexcept;
