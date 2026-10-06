#pragma once

// =============================================================================
// UiLayoutSettingsStore — app-wide (machine-local) persisted UI layout preferences
// =============================================================================
//
// ROLE
//   Small XML store for workstation-level layout preferences that are NOT part of a project:
//   the shared track-header column width, the mixer window's bounds and the mixer's global
//   section flags. Lives next to the other app-level settings in `%APPDATA%\MiniDAWLab\`
//   (`audio-device.xml`, `audio-latency.xml`) as `ui-layout.xml`.
//
// SCOPE DECISION
//   Project files persist *project-bound* window state (main-window bounds, follow toggle, MIDI
//   editor bounds/workspace). The header column width is a per-machine preference (depends on the
//   user's screen, DPI and taste, not on the song), so it is stored app-wide and restored on every
//   start regardless of which project is opened. It never creates an undo step and never marks a
//   project dirty.
//
// I/O DISCIPLINE
//   `save()` writes the whole file once; callers persist at gesture end (handle release), never per
//   mouse move. Absent / malformed file or value ⇒ `std::nullopt` ⇒ the view's safe default.
//
// THREADING
//   [Message thread] only.
// =============================================================================

#include <juce_core/juce_core.h>
#include <juce_graphics/juce_graphics.h>

#include <map>
#include <optional>

class UiLayoutSettingsStore
{
public:
    explicit UiLayoutSettingsStore(juce::File persistenceFile);

    /// `%APPDATA%\MiniDAWLab\ui-layout.xml`.
    [[nodiscard]] static juce::File defaultFile();

    /// Reads the file; missing or malformed content leaves every value absent (safe defaults apply).
    void loadFromFile();

    /// Shared track-header column width in logical px, when a valid positive integer was stored.
    [[nodiscard]] std::optional<int> getTrackHeaderColumnWidthPx() const noexcept
    {
        return trackHeaderColumnWidthPx_;
    }
    /// In-memory only; call `save()` to write (one write per gesture end).
    void setTrackHeaderColumnWidthPx(int widthPx) noexcept;

    /// Mixer window bounds (logical px, screen coordinates) when a valid rectangle was stored.
    /// Callers clamp to the available displays on restore.
    [[nodiscard]] std::optional<juce::Rectangle<int>> getMixerWindowBounds() const noexcept { return mixerWindowBounds_; }
    void setMixerWindowBounds(juce::Rectangle<int> bounds) noexcept;

    /// Global mixer section flags by stable key ("routing", "preGain", "preInserts",
    /// "postInserts", "sends", "faders", "meters"); absent = the mixer's default (shown).
    [[nodiscard]] std::optional<bool> getMixerSectionShown(const juce::String& key) const;
    void setMixerSectionShown(const juce::String& key, bool shown);

    /// Writes the whole file (creates the folder if needed). Logs and keeps going on failure.
    void save();

    [[nodiscard]] const juce::File& getFile() const noexcept { return persistenceFile_; }

private:
    juce::File persistenceFile_;
    std::optional<int> trackHeaderColumnWidthPx_;
    std::optional<juce::Rectangle<int>> mixerWindowBounds_;
    std::map<juce::String, bool> mixerSectionShown_;
};
