#include "ui/mixer/MixerWindow.h"

#include "ui/TransportShortcutKeys.h"

namespace
{
    constexpr int kDefaultWidthPx = 1200;
    constexpr int kDefaultHeightPx = 720;
    constexpr int kMinWidthPx = 480;
    constexpr int kMinHeightPx = 320;
    constexpr int kBoundsSettleMs = 300;

    /// Move / shrink `b` so it lies inside a display's user area (keeps the size when it fits).
    [[nodiscard]] juce::Rectangle<int> clampToDisplays(juce::Rectangle<int> b)
    {
        const juce::Displays& displays = juce::Desktop::getInstance().getDisplays();
        const juce::Displays::Display* d = displays.getDisplayForRect(b);
        if (d == nullptr)
        {
            d = displays.getPrimaryDisplay();
        }
        if (d == nullptr)
        {
            return b;
        }
        const juce::Rectangle<int> area = d->userArea;
        b.setWidth(juce::jlimit(kMinWidthPx, juce::jmax(kMinWidthPx, area.getWidth()), b.getWidth()));
        b.setHeight(juce::jlimit(kMinHeightPx, juce::jmax(kMinHeightPx, area.getHeight()), b.getHeight()));
        if (b.getRight() > area.getRight())
        {
            b.setX(area.getRight() - b.getWidth());
        }
        if (b.getBottom() > area.getBottom())
        {
            b.setY(area.getBottom() - b.getHeight());
        }
        if (b.getX() < area.getX())
        {
            b.setX(area.getX());
        }
        if (b.getY() < area.getY())
        {
            b.setY(area.getY());
        }
        return b;
    }
} // namespace

MixerWindow::MixerWindow(const MixerStripBindings& bindings, LevelMeterHub* const hub, Shortcuts shortcuts)
    : DocumentWindow("Mixer",
                     juce::Desktop::getInstance().getDefaultLookAndFeel().findColour(juce::ResizableWindow::backgroundColourId),
                     DocumentWindow::allButtons)
    , shortcuts_(std::move(shortcuts))
{
    setUsingNativeTitleBar(true);
    content_ = std::make_unique<MixerContentComponent>(bindings, hub);
    setContentNonOwned(content_.get(), false);
    setResizable(true, true);
    setResizeLimits(kMinWidthPx, kMinHeightPx, 10000, 10000);
    applyingBounds_ = true;
    setBounds(defaultBoundsForDisplay());
    applyingBounds_ = false;
    addKeyListener(this);
    content_->setWantsKeyboardFocus(true);
}

MixerWindow::~MixerWindow()
{
    stopTimer();
    removeKeyListener(this);
    clearContentComponent();
    content_.reset();
}

juce::Rectangle<int> MixerWindow::defaultBoundsForDisplay()
{
    juce::Rectangle<int> b(0, 0, kDefaultWidthPx, kDefaultHeightPx);
    const juce::Displays::Display* d = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay();
    if (d != nullptr)
    {
        const juce::Rectangle<int> area = d->userArea;
        b.setWidth(juce::jmin(b.getWidth(), area.getWidth() - 40));
        b.setHeight(juce::jmin(b.getHeight(), area.getHeight() - 40));
        b.setCentre(area.getCentre());
    }
    return clampToDisplays(b);
}

void MixerWindow::applyStoredBounds(const juce::Rectangle<int> bounds)
{
    if (bounds.isEmpty())
    {
        return;
    }
    applyingBounds_ = true;
    setBounds(clampToDisplays(bounds));
    applyingBounds_ = false;
}

void MixerWindow::showMixer()
{
    if (!isVisible())
    {
        setVisible(true);
    }
    toFront(true);
    if (content_ != nullptr)
    {
        content_->refreshFromSession();
        content_->grabKeyboardFocus();
    }
}

void MixerWindow::hideMixer()
{
    if (isVisible())
    {
        setVisible(false);
    }
}

void MixerWindow::toggleMixer()
{
    if (isVisible())
    {
        hideMixer();
    }
    else
    {
        showMixer();
    }
}

void MixerWindow::closeButtonPressed()
{
    // Hide only: the one instance keeps its layout for the next F3.
    hideMixer();
}

void MixerWindow::visibilityChanged()
{
    juce::DocumentWindow::visibilityChanged();
    if (onVisibilityChanged != nullptr)
    {
        onVisibilityChanged(isVisible());
    }
}

void MixerWindow::moved()
{
    juce::DocumentWindow::moved();
    scheduleBoundsSettle();
}

void MixerWindow::resized()
{
    juce::DocumentWindow::resized();
    scheduleBoundsSettle();
}

void MixerWindow::scheduleBoundsSettle()
{
    if (applyingBounds_ || !isVisible())
    {
        return;
    }
    startTimer(kBoundsSettleMs);
}

void MixerWindow::timerCallback()
{
    stopTimer();
    if (onBoundsSettled != nullptr && isVisible() && !isMinimised())
    {
        onBoundsSettled(getBounds());
    }
}

bool MixerWindow::keyPressed(const juce::KeyPress& key, juce::Component*)
{
    // F3 toggles from here as well (hides, since this window is visible when it has focus).
    if (key.isKeyCode(juce::KeyPress::F3Key) && !key.getModifiers().isAnyModifierKeyDown())
    {
        if (shortcuts_.toggleMixerWindow != nullptr)
        {
            shortcuts_.toggleMixerWindow();
            return true;
        }
    }
    const bool editorHasFocus = dynamic_cast<juce::TextEditor*>(juce::Component::getCurrentlyFocusedComponent()) != nullptr;
    if (!editorHasFocus)
    {
        const bool cmd = key.getModifiers().isCommandDown();
        const bool shift = key.getModifiers().isShiftDown();
        const int code = key.getKeyCode();
        if (cmd && !shift && (code == 's' || code == 'S') && shortcuts_.saveProject != nullptr)
        {
            shortcuts_.saveProject();
            return true;
        }
        if (cmd && !shift && (code == 'z' || code == 'Z') && shortcuts_.undo != nullptr)
        {
            shortcuts_.undo();
            return true;
        }
        if (cmd && ((code == 'y' || code == 'Y') || (shift && (code == 'z' || code == 'Z'))) && shortcuts_.redo != nullptr)
        {
            shortcuts_.redo();
            return true;
        }
    }
    // Transport shortcuts behave exactly like in the main window (they also run with a focused
    // text field there, through the same predicates).
    if (midi_transport_shortcuts::isRecordToggleShortcut(key) && shortcuts_.recordToggle != nullptr)
    {
        if (editorHasFocus)
        {
            return false;
        }
        shortcuts_.recordToggle();
        return true;
    }
    if (midi_transport_shortcuts::isJumpToLeftLocatorShortcut(key) && shortcuts_.jumpToLeftLocator != nullptr)
    {
        if (editorHasFocus)
        {
            return false;
        }
        shortcuts_.jumpToLeftLocator();
        return true;
    }
    if (midi_transport_shortcuts::isSpacePlayPauseShortcut(key) && shortcuts_.playPauseToggle != nullptr)
    {
        if (editorHasFocus)
        {
            return false; // a space typed into a value field stays a space
        }
        shortcuts_.playPauseToggle();
        return true;
    }
    return false;
}
