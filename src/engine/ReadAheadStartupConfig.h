#pragma once

// =============================================================================
// ReadAheadStartupConfig — how this process decides whether read-ahead exists
// =============================================================================
//
// ROLE
//   One decision, made on the message thread before `PlaybackEngine` is constructed.
//   The renderer itself is unchanged: it still exists only when
//   `setConfiguredReadAheadDepth` was given a depth greater than 0, and that global
//   still starts at 0 so tests and any path that skips this resolver stay off.
//
// PRIORITY (first match wins)
//   1. An explicit start argument. `--experimental-readahead` (depth 3) and
//      `--experimental-readahead=N` (N clamped to the renderer's 2..8) turn it on.
//      `--no-readahead` turns it off. If several of these appear, the last one wins.
//      A start argument applies to this process only and is never written to the
//      preference file.
//   2. The saved preference, `%APPDATA%\MiniDAWLab\read-ahead.xml`. `enabled="0"` is
//      off. `enabled="1"` is on at depth 3.
//   3. No saved choice (missing file, or no recognisable `enabled` attribute): on,
//      depth 3. That is also the upgrade path from 1.3.0, which had no preference file.
//
// The saved choice is the checkbox in Audio Settings. It is the next start, not the
// renderer that is already running. Parallel strip processing does not read this.
// =============================================================================

#include <juce_core/juce_core.h>

#include <optional>

namespace readahead
{

/// Product default when read-ahead is on and the user did not pass a depth.
/// The explicit-depth clamp matches `ReadAheadRenderer::kMinDepth` / `kMaxDepth`
/// (asserted in `ReadAheadStartupConfig.cpp`).
inline constexpr int kDefaultReadAheadDepth = 3;
inline constexpr int kReadAheadDepthMin = 2;
inline constexpr int kReadAheadDepthMax = 8;

struct ReadAheadProcessConfig
{
    /// 0 = this process builds no read-ahead renderer. Otherwise 2..8.
    int activeDepth = 0;
    /// True when argv contained `--no-readahead` and/or `--experimental-readahead[=N]`.
    bool commandLineOverride = false;
    /// True when more than one of those arguments appeared. The last one still wins.
    bool conflictingArguments = false;
    /// Checkbox / next start without a start argument. True when nothing is saved.
    bool nextStartEnabled = true;
    /// True only when the file held a recognisable enabled attribute.
    bool hasSavedChoice = false;
};

/// `%APPDATA%\MiniDAWLab\read-ahead.xml`. Does not create the file.
[[nodiscard]] juce::File defaultReadAheadPreferenceFile();

/// `nullopt` = no saved choice (missing file, unreadable XML, or no recognisable value).
[[nodiscard]] std::optional<bool> loadReadAheadEnabledPreference(const juce::File& file);

/// Writes `enabled="1"` or `enabled="0"`. Creates the folder. False on failure.
bool saveReadAheadEnabledPreference(const juce::File& file, bool enabled);

[[nodiscard]] ReadAheadProcessConfig resolveReadAheadProcessConfig(const juce::StringArray& args,
                                                                   const juce::File& preferenceFile);

/// Message thread, once, after resolve and before the engine is constructed.
void publishReadAheadProcessConfig(const ReadAheadProcessConfig& config) noexcept;
[[nodiscard]] ReadAheadProcessConfig publishedReadAheadProcessConfig() noexcept;

} // namespace readahead
