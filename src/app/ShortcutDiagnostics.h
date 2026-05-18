#pragma once

#include <JuceHeader.h>

namespace shortcut_diagnostics
{
inline constexpr bool kShowKeyDiagnostic = false;

inline constexpr bool kShowShortcutDiagnostics = false;

/// Master bus integration: UI active track vs canonical master row vs engine strip (off by default).
inline constexpr bool kShowMasterRoutingDiag = false;

[[nodiscard]] inline juce::String hex8(const juce::uint32 x)
{
    return juce::String::toHexString(x).toUpperCase();
}

// [ShortcutDiag] Lines are parseable tokens for correlating WM/JUCE conversions with matchers.
inline void logShortcutRouterKey(const juce::KeyPress& key)
{
    if (!kShowShortcutDiagnostics)
    {
        return;
    }
    const int kc = key.getKeyCode();
    const int lowWord = kc & 0xffff;
    const juce_wchar tc = key.getTextCharacter();
    const juce::uint32 kcU = static_cast<juce::uint32>(kc);
    const juce::uint32 tcU = static_cast<juce::uint32>(static_cast<juce::uint16>(tc));
    const juce::uint32 lowU = static_cast<juce::uint32>(lowWord) & 0xffffu;

    const juce::ModifierKeys mods = key.getModifiers();
    const juce::uint32 modRaw = static_cast<juce::uint32>(mods.getRawFlags());

    const int canonNp1 = juce::KeyPress::numberPad1;
    const int canonMul = juce::KeyPress::numberPadMultiply;
    const juce::uint32 canonNp1U = static_cast<juce::uint32>(canonNp1);
    const juce::uint32 canonMulU = static_cast<juce::uint32>(canonMul);

    juce::String msg;
    msg += "[ShortcutDiag] ";
    msg += "keyCode=";
    msg += juce::String(kc);
    msg += " (0x";
    msg += hex8(kcU);
    msg += ") lowWord=";
    msg += juce::String(lowWord);
    msg += " (0x";
    msg += hex8(lowU);
    msg += ") textChar=";
    msg += juce::String(static_cast<int>(tcU & 0xffffu));
    msg += " (0x";
    msg += hex8(tcU);
    msg += ") np1Canon=";
    msg += juce::String(canonNp1);
    msg += " (0x";
    msg += hex8(canonNp1U);
    msg += ") mulCanon=";
    msg += juce::String(canonMul);
    msg += " (0x";
    msg += hex8(canonMulU);
    msg += ") modShift=";
    msg += mods.isShiftDown() ? juce::String("Y") : juce::String("n");
    msg += " modCtrl=";
    msg += mods.isCtrlDown() ? juce::String("Y") : juce::String("n");
    msg += " modAlt=";
    msg += mods.isAltDown() ? juce::String("Y") : juce::String("n");
    msg += " modCmd=";
    msg += mods.isCommandDown() ? juce::String("Y") : juce::String("n");
    msg += " modRaw=0x";
    msg += hex8(modRaw);
    msg += " desc=\"";
    msg += key.getTextDescription();
    msg += "\"";
    juce::Logger::writeToLog(msg);
}

// Single-line caption for temporary on-screen shortcut diagnostic (transport area).
[[nodiscard]] inline juce::String makeShortcutDiagVisibleCaption(const juce::KeyPress& key)
{
    const int kc = key.getKeyCode();
    const int lowWord = kc & 0xffff;
    const juce_wchar tc = key.getTextCharacter();
    const auto kcU = static_cast<juce::uint32>(kc);
    const auto tcU = static_cast<juce::uint32>(static_cast<juce::uint16>(tc));
    const auto lowU = static_cast<juce::uint32>(lowWord) & 0xffffu;

    juce::String cap;
    cap << "[ShortcutDiag ui] ";
    cap << "keyCode=" << juce::String(kc) << " (0x" << hex8(kcU) << ") ";
    cap << "lowWord=" << juce::String(lowWord) << " (0x" << hex8(lowU) << ") ";
    cap << "textChar=" << juce::String(static_cast<int>(tcU & 0xffffu)) << " (0x"
        << hex8(tcU) << ") ";
    cap << "desc=\"" << key.getTextDescription() << "\"";
    return cap;
}
} // namespace shortcut_diagnostics
