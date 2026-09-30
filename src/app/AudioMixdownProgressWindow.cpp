// =============================================================================
// AudioMixdownProgressWindow.cpp — paint, Cancel and own-window message servicing
// =============================================================================
// See the header for the role of this window and the "own HWND only" invariant.
// [Message thread] only.
// =============================================================================

#include "app/AudioMixdownProgressWindow.h"

#include "diagnostics/StabilityDiagnosticLog.h"

#if JUCE_WINDOWS
 #define WIN32_LEAN_AND_MEAN
 #define NOMINMAX
 #include <windows.h>
#endif

namespace
{
    constexpr int kWindowWidth = 460;
    constexpr int kWindowHeight = 150;

    /// Same green family as the track-header power-on / "MP3 encoder ready" accents.
    [[nodiscard]] juce::Colour progressAccentColour() noexcept
    {
        return juce::Colour(0xff2d9d53);
    }
} // namespace

AudioMixdownProgressWindow::AudioMixdownProgressWindow()
{
    setOpaque(true);
    setSize(kWindowWidth, kWindowHeight);
    setAlwaysOnTop(true);
    setWantsKeyboardFocus(true);

    cancelButton_.onClick = [this] { requestCancel(); };
    addAndMakeVisible(cancelButton_);

    addToDesktop(juce::ComponentPeer::windowHasDropShadow);
    if (auto* peer = getPeer())
    {
        // JUCE 8 draws Windows peers with Direct2D by default. That engine only *defers* WM_PAINT
        // and actually renders on the next vblank message — a message carried by JUCE's internal
        // message window, which never gets dispatched while the export owns the message thread
        // (and its `performAnyPendingRepaintsNow()` is a no-op). The result was a window that never
        // painted: the "white box". The software renderer paints synchronously inside WM_PAINT,
        // which `serviceOwnWindowMessages()` dispatches, so this one window pins that engine.
        const int softwareRenderer = peer->getAvailableRenderingEngines().indexOf("Software Renderer");
        if (softwareRenderer >= 0)
        {
            peer->setCurrentRenderingEngine(softwareRenderer);
        }
    }
    if (auto* display = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay())
    {
        setCentrePosition(display->userArea.getCentre());
    }
    setVisible(true);
    toFront(true);
    grabKeyboardFocus();
    appendMixdownDiagnosticLine("progress ui shown");
    repaint();
    serviceOwnWindowMessages();
}

AudioMixdownProgressWindow::~AudioMixdownProgressWindow()
{
    appendMixdownDiagnosticLine(juce::String("progress ui closed")
                                + (cancelRequested_ ? " (cancel was requested)" : ""));
}

void AudioMixdownProgressWindow::setMixdownProgress(const juce::String& statusText,
                                                    const double fraction01)
{
    // A cancel that is already pending keeps its own status text: the exporter will end the
    // export at its next poll, and the user should see that the click was registered.
    if (!cancelRequested_)
    {
        statusText_ = statusText;
    }
    fraction_ = fraction01;
    ++pulseCounter_;
    repaint();
    serviceOwnWindowMessages();
}

bool AudioMixdownProgressWindow::isMixdownCancelRequested() const noexcept
{
    serviceOwnWindowMessages();
    return cancelRequested_;
}

void AudioMixdownProgressWindow::requestCancel()
{
    if (cancelRequested_)
    {
        return;
    }
    cancelRequested_ = true;
    statusText_ = "Cancelling...";
    cancelButton_.setEnabled(false);
    appendMixdownDiagnosticLine("progress ui: cancel requested by user");
    repaint();
}

juce::Rectangle<int> AudioMixdownProgressWindow::getCancelButtonScreenBounds() const
{
    return cancelButton_.getScreenBounds();
}

void AudioMixdownProgressWindow::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colour(0xff2a2a33));
    g.setColour(juce::Colours::white.withAlpha(0.25f));
    g.drawRect(getLocalBounds(), 1);

    // Title row, then the phase text: the phase carries the measurable percentage when there is
    // one ("Rendering... 42%"), so the bar and the text always agree.
    g.setColour(juce::Colours::white.withAlpha(0.7f));
    g.setFont(juce::FontOptions(13.0f));
    g.drawText("Audio Mixdown", 16, 10, getWidth() - 32, 18, juce::Justification::centredLeft);

    g.setColour(juce::Colours::white);
    g.setFont(juce::FontOptions(15.0f));
    g.drawText(statusText_, 16, 32, getWidth() - 32, 22, juce::Justification::centredLeft);

    const juce::Rectangle<int> barArea(16, 64, getWidth() - 32, 22);
    g.setColour(juce::Colours::black.withAlpha(0.35f));
    g.fillRect(barArea);
    g.setColour(progressAccentColour());
    if (fraction_ >= 0.0)
    {
        // Determinate: the filled width is the measured fraction of the current phase.
        const int w = juce::roundToInt(barArea.getWidth() * juce::jlimit(0.0, 1.0, fraction_));
        g.fillRect(barArea.withWidth(w));
    }
    else
    {
        // Indeterminate: a segment bouncing left-right, advanced by each progress pulse, so the
        // user can see the export is alive even when the phase has no measurable percentage.
        const int segW = juce::jmax(24, barArea.getWidth() / 4);
        const int span = juce::jmax(1, barArea.getWidth() - segW);
        const int offset = static_cast<int>((pulseCounter_ * 10) % static_cast<std::uint64_t>(2 * span));
        const int x = offset <= span ? offset : (2 * span - offset);
        g.fillRect(barArea.getX() + x, barArea.getY(), segW, barArea.getHeight());
    }
    g.setColour(juce::Colours::white.withAlpha(0.4f));
    g.drawRect(barArea, 1);

    g.setColour(juce::Colours::white.withAlpha(0.5f));
    g.setFont(juce::FontOptions(12.0f));
    g.drawText("Esc = Cancel", 16, getHeight() - 42, 200, 18, juce::Justification::centredLeft);
    ++paintCount_;
}

void AudioMixdownProgressWindow::resized()
{
    cancelButton_.setBounds(getWidth() - 16 - 110, getHeight() - 16 - 30, 110, 30);
}

bool AudioMixdownProgressWindow::keyPressed(const juce::KeyPress& key)
{
    if (key == juce::KeyPress::escapeKey)
    {
        requestCancel();
        return true;
    }
    return false;
}

void AudioMixdownProgressWindow::mouseDown(const juce::MouseEvent& e)
{
    // The window has no native title bar; dragging its body moves it.
    dragger_.startDraggingComponent(this, e);
}

void AudioMixdownProgressWindow::mouseDrag(const juce::MouseEvent& e)
{
    dragger_.dragComponent(this, e, nullptr);
}

void AudioMixdownProgressWindow::userTriedToCloseWindow()
{
    requestCancel();
}

void AudioMixdownProgressWindow::serviceOwnWindowMessages() const
{
    auto* const peer = getPeer();
    if (peer == nullptr || !isVisible())
    {
        return;
    }
    // Step 1 — paint now. JUCE's Windows peers turn `repaint()` into a real window invalidation
    // only on their vblank message, which never arrives while the export owns the message thread.
    // With the software renderer pinned in the constructor this call invalidates and paints
    // synchronously (for Direct2D it would be a no-op — the pre-1.1.5 white box).
    peer->performAnyPendingRepaintsNow();
#if JUCE_WINDOWS
    // Step 2 — dispatch only what the OS queued for THIS window (mouse, keyboard, activation,
    // remaining paint). `PeekMessage` with an explicit HWND never returns thread messages or
    // messages for other windows, so JUCE's timer/callAsync carrier window and every other DAL
    // window stay untouched until the export returns. This also keeps the process from being
    // flagged "not responding" (and ghosted) by Windows.
    const auto hwnd = reinterpret_cast<HWND>(peer->getNativeHandle());
    MSG msg;
    int budget = 64; // bounded: an input storm must not stall the export loop
    while (budget-- > 0 && ::PeekMessageW(&msg, hwnd, 0, 0, PM_REMOVE))
    {
        ::TranslateMessage(&msg);
        ::DispatchMessageW(&msg);
    }
#endif
}
