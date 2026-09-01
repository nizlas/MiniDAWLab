#pragma once

// =============================================================================
// ExperimentalMidiChannelDiagnostics — native vs effective MIDI channel, as text
// =============================================================================
//
// ROLE IN THE ARCHITECTURE
//   Pure logic behind the MIDI editor's compact channel readout and behind the two explicit
//   "remap native channels to the track channel" commands. No JUCE components, no session access,
//   no note mutation: callers hand in the channels they already read from the notes, and get back a
//   summary plus display strings. Header-only so the whole thing can be unit-tested without a
//   plugin, an audio device or a running app (see `_winboundstest/midi-channel-selftest.cpp`).
//
// THE THREE CHANNEL CONCEPTS (kept deliberately distinct — conflating them was the original bug)
//   * **Native channel** — stored in the MIDI event itself (`TimelineMidiNote::channel`, 1 … 16).
//     Part of the note data, and part of note *identity* in the editor.
//   * **Track output channel** — `Track::getMidiOutputChannel()`: `kTrackMidiOutputChannelAny`
//     (preserve each event's native channel) or a fixed 1 … 16.
//   * **Effective channel** — what the plugin actually receives: the native channel when the track
//     is `Any`, otherwise the track's fixed channel. Nothing rewrites the note in that case; the
//     remap happens when the render snapshot is published.
//
//   A track set to a fixed channel therefore *hides* its native mixture, and switching back to
//   `Any` reveals it again. That is exactly why this readout exists.
//
// THREADING
//   Pure functions; safe anywhere. Used from the [Message thread] only in practice.
// =============================================================================

#include "domain/Track.h"
#include "ui/experimental/ExperimentalMidiPattern.h"

#include <juce_core/juce_core.h>

#include <algorithm>
#include <optional>
#include <vector>

namespace midi_channel_diag
{
    /// Longest `Mixed (…)` list the compact toolbar readout will spell out. Beyond this the readout
    /// collapses to bare `Mixed` and the full set moves into the tooltip, so the control cannot grow
    /// wide enough to push the editing area around.
    inline constexpr int kMaxSpelledOutMixedChannels = 4;

    /// What the selected notes natively store, independent of any track setting.
    struct NativeChannelSummary
    {
        int selectedCount = 0;
        /// Every distinct native channel present, ascending, de-duplicated.
        std::vector<int> distinctChannels;

        /// True when there is a selection and every selected note shares one native channel.
        [[nodiscard]] bool isUniform() const noexcept
        {
            return selectedCount > 0 && distinctChannels.size() == 1U;
        }
        /// True when the selection spans more than one native channel.
        [[nodiscard]] bool isMixed() const noexcept { return distinctChannels.size() > 1U; }
        /// The single shared native channel, when there is one.
        [[nodiscard]] std::optional<int> uniformChannel() const noexcept
        {
            return isUniform() ? std::optional<int>(distinctChannels.front()) : std::nullopt;
        }
    };

    /// Folds one note's native channel into a summary. Out-of-range values are clamped the same way
    /// the playback path clamps them, so the readout never claims a channel the plugin cannot get.
    inline void addNativeChannel(NativeChannelSummary& s, const int rawChannel) noexcept
    {
        const int ch = juce::jlimit(kTrackMidiOutputChannelMin, kTrackMidiOutputChannelMax, rawChannel);
        ++s.selectedCount;
        const auto at = std::lower_bound(s.distinctChannels.begin(), s.distinctChannels.end(), ch);
        if (at == s.distinctChannels.end() || *at != ch)
        {
            s.distinctChannels.insert(at, ch);
        }
    }

    /// Builds a summary from a note-channel sequence (any iterable of integer-ish channels).
    template <typename Range>
    [[nodiscard]] NativeChannelSummary summarizeNativeChannels(const Range& channels) noexcept
    {
        NativeChannelSummary s;
        for (const auto& ch : channels)
        {
            addNativeChannel(s, static_cast<int>(ch));
        }
        return s;
    }

    /// The channel the plugin actually receives for a note with this native channel.
    [[nodiscard]] inline int effectiveChannel(const int nativeChannel,
                                              const int trackOutputChannel) noexcept
    {
        return trackOutputChannel == kTrackMidiOutputChannelAny
                   ? juce::jlimit(kTrackMidiOutputChannelMin, kTrackMidiOutputChannelMax, nativeChannel)
                   : trackOutputChannel;
    }

    /// Channel stamped on notes the editor creates: the track's fixed output channel when it has
    /// one, otherwise 10. On a fixed-channel track the stored channel only decides note *identity*
    /// (overlap rules), so matching the track keeps data and audible behavior describing the same
    /// thing. On an `Any` track the stored channel *is* the behavior, and 10 is what every pre-v17
    /// version stamped — so new notes stay consistent with the notes already in a migrated project.
    [[nodiscard]] inline int channelForNewNotes(const int trackOutputChannel) noexcept
    {
        return trackOutputChannel != kTrackMidiOutputChannelAny ? trackOutputChannel
                                                               : kTrackMidiOutputChannelDrums;
    }

    /// The one effective channel the whole selection resolves to, when that is knowable: the track's
    /// fixed channel, or a uniform native channel under `Any`. Empty for a mixed selection under
    /// `Any`, where each note keeps its own channel and no single number would be honest.
    [[nodiscard]] inline std::optional<int> selectionEffectiveChannel(
        const NativeChannelSummary& s, const int trackOutputChannel) noexcept
    {
        if (s.selectedCount <= 0)
        {
            return std::nullopt;
        }
        if (trackOutputChannel != kTrackMidiOutputChannelAny)
        {
            return trackOutputChannel;
        }
        return s.uniformChannel();
    }

    /// `"1, 2, 3"`.
    [[nodiscard]] inline juce::String channelListText(const std::vector<int>& channels)
    {
        juce::StringArray parts;
        for (const int c : channels)
        {
            parts.add(juce::String(c));
        }
        return parts.joinIntoString(", ");
    }

    /// `"Any (Preserve)"` / `"3"` — how a track output channel is named to the user. Matches the
    /// Inspector's dropdown wording so help text, tooltip and control agree.
    [[nodiscard]] inline juce::String trackOutputChannelText(const int trackOutputChannel)
    {
        return trackOutputChannel == kTrackMidiOutputChannelAny ? juce::String("Any (Preserve)")
                                                               : juce::String(trackOutputChannel);
    }

    /// The compact readout plus the tooltip that always carries the full detail.
    struct ReadoutText
    {
        /// Toolbar text. Empty when nothing is selected — an empty readout is better than a number
        /// the user might read as "the track is on channel X".
        juce::String compact;
        /// Always spells out the complete channel set and the three concepts, however long.
        juce::String tooltip;
    };

    /// Renders the readout. Shape follows the spec examples:
    ///   `Native Ch: 2 · Output: Preserve · Effective Ch: 2`
    ///   `Native Ch: 2 · 7 notes · Output Ch: 3 · Effective Ch: 3`
    ///   `Native Ch: Mixed (1, 2, 3) · 12 notes · Output: Preserve`
    /// The note count appears only for multi-note selections, and `Effective Ch` is omitted exactly
    /// when it is not a single knowable value (mixed native channels on an `Any` track).
    [[nodiscard]] inline ReadoutText buildReadoutText(const NativeChannelSummary& s,
                                                      const int trackOutputChannel)
    {
        ReadoutText out;
        const juce::String sep = juce::String::fromUTF8(" \xc2\xb7 ");
        const bool any = (trackOutputChannel == kTrackMidiOutputChannelAny);
        const juce::String outputPart
            = any ? juce::String("Output: Preserve")
                  : juce::String("Output Ch: ") + juce::String(trackOutputChannel);

        if (s.selectedCount <= 0)
        {
            // No selection: say what the track will do, claim nothing about note data.
            out.compact = outputPart;
            out.tooltip = juce::String("No notes selected.\n\nTrack output channel: ")
                          + trackOutputChannelText(trackOutputChannel)
                          + (any ? juce::String("\nEach note plays on its own stored (native) channel.")
                                 : juce::String("\nEvery note from this track plays on channel ")
                                       + juce::String(trackOutputChannel)
                                       + ", whatever it stores natively.");
            return out;
        }

        const juce::String fullList = channelListText(s.distinctChannels);
        juce::String nativePart;
        if (s.isMixed())
        {
            nativePart = juce::String("Native Ch: Mixed");
            if ((int)s.distinctChannels.size() <= kMaxSpelledOutMixedChannels)
            {
                nativePart += " (" + fullList + ")";
            }
        }
        else
        {
            nativePart = juce::String("Native Ch: ") + juce::String(s.distinctChannels.front());
        }

        out.compact = nativePart;
        if (s.selectedCount > 1)
        {
            out.compact += sep + juce::String(s.selectedCount) + " notes";
        }
        out.compact += sep + outputPart;
        if (const auto eff = selectionEffectiveChannel(s, trackOutputChannel))
        {
            out.compact += sep + "Effective Ch: " + juce::String(*eff);
        }

        out.tooltip = juce::String("Selected notes: ") + juce::String(s.selectedCount)
                      + "\nNative channel(s) stored in the notes: " + fullList
                      + "\nTrack output channel: " + trackOutputChannelText(trackOutputChannel)
                      + "\nEffective channel sent to the plugin: ";
        if (const auto eff = selectionEffectiveChannel(s, trackOutputChannel))
        {
            out.tooltip += juce::String(*eff);
        }
        else
        {
            out.tooltip += "each note's own native channel (" + fullList + ")";
        }
        return out;
    }

    /// Whether the two "remap native channels to the track channel" commands may run. They need a
    /// concrete target, so `Any` disables them rather than guessing a channel.
    [[nodiscard]] inline bool canRemapToTrackChannel(const int trackOutputChannel) noexcept
    {
        return trackOutputChannel >= kTrackMidiOutputChannelMin
               && trackOutputChannel <= kTrackMidiOutputChannelMax;
    }

    /// Why the remap commands are greyed out, for a tooltip.
    [[nodiscard]] inline juce::String remapUnavailableReason()
    {
        return "Set the track's MIDI Channel to a fixed 1-16 value first (Inspector). "
               "\"Any (Preserve)\" has no single target channel to write into the notes.";
    }

    // -------------------------------------------------------------------------
    // Destructive native-channel remap — the pure part
    // -------------------------------------------------------------------------
    // One clip's worth of planning and validation, kept free of components and undo plumbing so it
    // can be unit-tested directly (`_winboundstest/midi-channel-selftest.cpp`). The caller decides
    // scope (selection vs. every clip on the track), wraps the applies in one undo step, and
    // republishes the render snapshot.

    /// Which slots of `notes` a remap to `targetChannel` would actually rewrite. Notes already on
    /// the target are excluded so a no-op cannot produce an undo step. Ascending, de-duplicated.
    /// `restrictToIndices == nullptr` means "every note"; otherwise only those slots are considered
    /// and out-of-range entries are ignored.
    [[nodiscard]] inline std::vector<int> planNativeChannelRemap(
        const std::vector<TimelineMidiNote>& notes,
        const int targetChannel,
        const std::vector<int>* restrictToIndices = nullptr)
    {
        std::vector<int> out;
        if (!canRemapToTrackChannel(targetChannel))
        {
            return out;
        }
        const auto target = (std::uint8_t)targetChannel;
        const int n = (int)notes.size();
        const auto consider = [&](const int idx) {
            if (idx >= 0 && idx < n && notes[(size_t)idx].channel != target)
            {
                out.push_back(idx);
            }
        };
        if (restrictToIndices != nullptr)
        {
            for (const int idx : *restrictToIndices)
            {
                consider(idx);
            }
        }
        else
        {
            for (int idx = 0; idx < n; ++idx)
            {
                consider(idx);
            }
        }
        std::sort(out.begin(), out.end());
        out.erase(std::unique(out.begin(), out.end()), out.end());
        return out;
    }

    /// True when rewriting `plan`'s channels would leave two overlapping notes on one pitch *and*
    /// channel — which this editor treats as a single note, so the batch must be refused. Pairs that
    /// already overlapped are grandfathered, matching every other batch edit in the roll.
    [[nodiscard]] inline bool nativeChannelRemapWouldOverlap(
        const std::vector<TimelineMidiNote>& notes,
        const std::vector<int>& plan,
        const int targetChannel,
        const std::int64_t minDurationTicks)
    {
        std::vector<TimelineMidiNote> originals;
        std::vector<TimelineMidiNote> candidates;
        originals.reserve(plan.size());
        candidates.reserve(plan.size());
        for (const int idx : plan)
        {
            const auto& n = notes[(size_t)idx];
            originals.push_back(n);
            TimelineMidiNote cand = n;
            cand.channel = (std::uint8_t)targetChannel;
            candidates.push_back(cand);
        }
        return !validateTimelineNotesNoOverlap(notes, plan, candidates, minDurationTicks, &originals)
                    .valid;
    }

    /// Writes the target channel into the planned slots. Channel only: start, duration, pitch,
    /// velocity and off-velocity are untouched, and the vector is deliberately not re-sorted so the
    /// caller's note indices (the editor's selection) stay valid. False = a stale plan index.
    [[nodiscard]] inline bool applyNativeChannelRemap(std::vector<TimelineMidiNote>& notes,
                                                      const std::vector<int>& plan,
                                                      const int targetChannel)
    {
        if (!canRemapToTrackChannel(targetChannel))
        {
            return false;
        }
        for (const int idx : plan)
        {
            if (idx < 0 || idx >= (int)notes.size())
            {
                return false;
            }
            notes[(size_t)idx].channel = (std::uint8_t)targetChannel;
        }
        return true;
    }
} // namespace midi_channel_diag
