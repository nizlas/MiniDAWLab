#pragma once

// =============================================================================
// ReadAheadPreferenceSection — Audio Settings checkbox for the next start
// =============================================================================
//
// ROLE
//   The saved on/off preference for read-ahead. Toggling writes
//   `%APPDATA%\MiniDAWLab\read-ahead.xml` and nothing else: no project dirty flag,
//   no undo step, and no change to the renderer already running in this process.
//   "Active this session" is the engine's actual mode, passed in by the dialog.
//
// PARALLEL PROCESSING
//   This control is not the render-pool strip path. That path stays on either way.
// =============================================================================

#include <juce_gui_basics/juce_gui_basics.h>

class ReadAheadPreferenceSection final : public juce::Component
{
public:
    static constexpr int kPreferredHeightPx = 148;

    /// `preferenceFile` is where the checkbox is saved. `activeThisSession` must be the
    /// engine's actual mode (`experimentalReadAhead() != nullptr`), not the checkbox.
    ReadAheadPreferenceSection(juce::File preferenceFile, bool activeThisSession, bool commandLineOverride);

    void resized() override;

    [[nodiscard]] juce::ToggleButton& enableToggle() noexcept { return enable_; }
    [[nodiscard]] juce::String activeSessionText() const { return activeLabel_.getText(); }
    [[nodiscard]] juce::String noticeText() const { return noticeLabel_.getText(); }

private:
    void refreshNotices();
    void onToggleClicked();

    juce::File preferenceFile_;
    bool activeThisSession_ = false;
    bool commandLineOverride_ = false;

    juce::ToggleButton enable_;
    juce::Label help_;
    juce::Label helpRestart_;
    juce::Label parallelNote_;
    juce::Label activeLabel_;
    juce::Label noticeLabel_;
};
