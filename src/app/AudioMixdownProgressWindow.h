#pragma once

// =============================================================================
// AudioMixdownProgressWindow — the progress + Cancel window shown during a blocking export
// =============================================================================
//
// ROLE
//   The audio mixdown runs synchronously on the message thread (see AudioMixdownExporter.cpp):
//   while it runs, the normal message loop is not spinning, so an ordinary JUCE window would
//   neither paint nor react to clicks — the "white box" users saw. This component is the one
//   piece of UI that stays alive during the export: it shows the phase ("Rendering...",
//   "Encoding MP3...", "Finalizing...") with a real percentage when one is measurable, and it
//   owns the Cancel button / Escape key that the exporter polls cooperatively.
//
// HOW IT STAYS RESPONSIVE WITHOUT BREAKING THE EXPORT INVARIANT
//   Every progress update and cancel poll calls `serviceOwnWindowMessages()`, which on Windows
//   dispatches ONLY the messages queued for this window's own HWND (paint, mouse, keyboard,
//   activation). Messages for every other window — the main window, plugin editors and JUCE's
//   internal message window that carries timers and `callAsync` — stay queued until the export
//   returns. So no session edit, plugin lifecycle, autosave or device change can interleave with
//   the offline render, exactly as before, while this window paints and clicks like a live one.
//
// OWNERSHIP / LIFETIME
//   Created on the stack by the export runner in AudioMixdownDialog.cpp for exactly the duration
//   of one export; destroyed before the completion alert is shown. Nothing else holds a pointer
//   to it, so there is no completion callback that could outlive it.
//
// THREADING
//   [Message thread] only. Implements `MixdownProgressSink`; the exporter calls it from its
//   blocking loops on the same thread. Never touched from the audio thread.
// =============================================================================

#include "app/AudioMixdownExporter.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <cstdint>

class AudioMixdownProgressWindow final : public juce::Component,
                                         public mini_daw_audio_mixdown::MixdownProgressSink
{
public:
    /// Shows the window centred on the primary display and paints it once immediately.
    AudioMixdownProgressWindow();
    ~AudioMixdownProgressWindow() override;

    // --- MixdownProgressSink ---------------------------------------------------------------------
    /// [Message thread] Updates the phase text and bar, paints synchronously and services this
    /// window's own input so a Cancel click is registered even during a long phase.
    void setMixdownProgress(const juce::String& statusText, double fraction01) override;
    /// [Message thread] Services this window's input first, then reports whether Cancel was
    /// pressed (button or Escape). Polled by the exporter between blocks and while encoding.
    [[nodiscard]] bool isMixdownCancelRequested() const noexcept override;

    /// True once the user asked to cancel; the exporter turns that into "Export cancelled.".
    [[nodiscard]] bool wasCancelRequested() const noexcept { return cancelRequested_; }

    /// Programmatic cancel (same path as the button/Escape); used by the focused UI test.
    void requestCancel();

    /// Screen-space bounds of the Cancel button, for the focused UI test's real click.
    [[nodiscard]] juce::Rectangle<int> getCancelButtonScreenBounds() const;

    /// Number of completed `paint()` calls (diagnostics for the focused UI test: proves the
    /// window really paints while the export blocks the message thread).
    [[nodiscard]] int getPaintCount() const noexcept { return paintCount_; }

    // --- juce::Component -------------------------------------------------------------------------
    void paint(juce::Graphics& g) override;
    void resized() override;
    bool keyPressed(const juce::KeyPress& key) override;
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;
    /// Alt+F4 / a close request on this window means Cancel, never "kill the export".
    void userTriedToCloseWindow() override;

private:
    /// Dispatches this window's own queued OS messages (Windows) or just flushes pending
    /// repaints (other platforms). See the header comment for why only this HWND is serviced.
    void serviceOwnWindowMessages() const;

    juce::String statusText_ { "Preparing..." };
    double fraction_ = -1.0;
    std::uint64_t pulseCounter_ = 0;
    int paintCount_ = 0;
    bool cancelRequested_ = false;
    juce::TextButton cancelButton_ { "Cancel" };
    juce::ComponentDragger dragger_;
};
