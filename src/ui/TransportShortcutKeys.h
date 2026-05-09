#pragma once

// =============================================================================
// TransportShortcutKeys — Space / numpad * predicates
// =============================================================================
//
// Shared by `MainWindow::routeShortcut` and `ExperimentalMidiEditorWindow` so editor focus
// routes shortcuts identically to the main transport entry points.
// =============================================================================

#include <juce_gui_basics/juce_gui_basics.h>

namespace midi_transport_shortcuts
{

// JUCE (Windows) uses e.g. VK_* | 0x10000 for numpad keys; `KeyPress::numberPadMultiply` matches
// that, but some paths deliver VK_MULTIPLY (0x6A) without the high bit — match both.
[[nodiscard]] inline bool isNumpadMultiplyKey(const juce::KeyPress& k) noexcept
{
    if (k == juce::KeyPress::numberPadMultiply)
    {
        return true;
    }
    constexpr int kVkMultiply = 0x6A; // winuser.h VK_MULTIPLY
    if ((k.getKeyCode() & 0xffff) == kVkMultiply)
    {
        return true;
    }
    return false;
}

[[nodiscard]] inline bool isRecordToggleShortcut(const juce::KeyPress& k) noexcept
{
    if (isNumpadMultiplyKey(k))
    {
        return true;
    }
    const auto ch = k.getTextCharacter();
    if (ch == static_cast<decltype(ch)>('*'))
    {
        return true;
    }
    return false;
}

// Unmodified Space -> play/pause toggle. Ctrl/Cmd/Alt+Space are ignored here.
[[nodiscard]] inline bool isSpacePlayPauseShortcut(const juce::KeyPress& k) noexcept
{
    const juce::ModifierKeys m = k.getModifiers();
    if (m.isCommandDown() || m.isCtrlDown() || m.isAltDown())
    {
        return false;
    }
    const auto ch = k.getTextCharacter();
    if (k.getKeyCode() == 32 || ch == static_cast<decltype(ch)>(' '))
    {
        return true;
    }
    return false;
}

} // namespace midi_transport_shortcuts
