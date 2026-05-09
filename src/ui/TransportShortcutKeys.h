#pragma once

// =============================================================================
// TransportShortcutKeys — Space / numpad * / jump-to-L predicates
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

// Jump-to-left-locator: numpad 1, top-row "1", numpad-as-End (NumLock off), and End / VK variants
// (mirrors main-window `routeShortcut` historically in Main.cpp).
[[nodiscard]] inline bool isJumpToLeftLocatorShortcut(const juce::KeyPress& k) noexcept
{
    if (k == juce::KeyPress::numberPad1)
    {
        return true;
    }

    const int canonNumpad1Code = juce::KeyPress::numberPad1;
    const int raw = k.getKeyCode();
    if (raw == canonNumpad1Code)
    {
        return true;
    }
    if ((raw & 0xffff) == (canonNumpad1Code & 0xffff))
    {
        return true;
    }

    constexpr int kVkNumpad1 = 0x61; // winuser.h VK_NUMPAD1
    if (((raw & 0xffff) == kVkNumpad1) || raw == kVkNumpad1)
    {
        return true;
    }

    const auto topRowDigit1 = k.getTextCharacter();
    if (topRowDigit1 == static_cast<decltype(topRowDigit1)>('1'))
    {
        return true;
    }

    constexpr int kAsciiDigit1 = 49; // 0x31 main-row
    if (((raw & 0xffff) == kAsciiDigit1) || raw == kAsciiDigit1)
    {
        return true;
    }

    if (k == juce::KeyPress::endKey)
    {
        return true;
    }
    const int canonEnd = juce::KeyPress::endKey;
    if (raw == canonEnd)
    {
        return true;
    }
    if ((raw & 0xffff) == (canonEnd & 0xffff))
    {
        return true;
    }
    constexpr int kVkEnd = 0x23; // winuser.h VK_END
    if (((raw & 0xffff) == kVkEnd) || raw == kVkEnd)
    {
        return true;
    }
    constexpr int kObservedEndExtended = 0x10023;
    if (raw == kObservedEndExtended)
    {
        return true;
    }

    return false;
}

} // namespace midi_transport_shortcuts
