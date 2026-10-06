#pragma once

// =============================================================================
// MixerWindow — the single, hide-on-close, resizable mixer window (F3)
// =============================================================================
//
// ROLE
//   Top-level `DocumentWindow` owned by the composition root for the whole app lifetime (one
//   instance; `closeButtonPressed` HIDES it, so reopening keeps size, position, scroll and the
//   section toggles). Content: `MixerContentComponent`. Opening or closing the window touches no
//   transport, recording, plug-in or audio state — strips only read the snapshot and the hub
//   re-publishes meter interest.
//
// KEYBOARD
//   The window is its own `KeyListener`: F3 toggles the window from here too, Ctrl+S / undo /
//   redo and the transport shortcuts (Space, record toggle, jump to left locator) are routed to
//   the main shell through `Shortcuts` — never while a `TextEditor` has focus (typing in a
//   value field must not start playback or hide the window).
//
// PERSISTENCE
//   Bounds changes are reported through `onBoundsSettled` ~300 ms after the last move / resize
//   (one write per gesture, never per mouse move); the owner stores them in `ui-layout.xml`.
//
// THREADING
//   [Message thread] only.
// =============================================================================

#include "ui/mixer/MixerContentComponent.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>

class MixerWindow final : public juce::DocumentWindow,
                          public juce::KeyListener,
                          private juce::Timer
{
public:
    /// Shell entry points the window forwards keys to (installed by the composition root).
    struct Shortcuts
    {
        std::function<void()> toggleMixerWindow;
        std::function<void()> saveProject;
        std::function<void()> undo;
        std::function<void()> redo;
        std::function<void()> recordToggle;
        std::function<void()> jumpToLeftLocator;
        std::function<void()> playPauseToggle;
    };

    MixerWindow(const MixerStripBindings& bindings, LevelMeterHub* hub, Shortcuts shortcuts);
    ~MixerWindow() override;

    [[nodiscard]] MixerContentComponent& content() noexcept { return *content_; }

    /// Show (restoring `bounds` clamped to the available displays when given) or hide.
    void showMixer();
    void hideMixer();
    void toggleMixer();
    /// Apply stored bounds (clamped to a display); call before the first `showMixer`.
    void applyStoredBounds(juce::Rectangle<int> bounds);
    /// Default size fitted to the main display's user area (~1200×720, smaller on small screens).
    [[nodiscard]] static juce::Rectangle<int> defaultBoundsForDisplay();

    std::function<void(juce::Rectangle<int>)> onBoundsSettled;
    std::function<void(bool visible)> onVisibilityChanged;

    void closeButtonPressed() override;
    void moved() override;
    void resized() override;
    bool keyPressed(const juce::KeyPress& key, juce::Component* originating) override;
    void visibilityChanged() override;

private:
    void timerCallback() override;
    void scheduleBoundsSettle();

    std::unique_ptr<MixerContentComponent> content_;
    Shortcuts shortcuts_;
    bool applyingBounds_ = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MixerWindow)
};
