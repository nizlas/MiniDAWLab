#pragma once

// =============================================================================
// MidiLayeredRenderBake — per-source MIDI clip layering for the render bake (pure, header-only)
// =============================================================================
//
// ROLE IN THE ARCHITECTURE
//   The ONE definition of the overlap rule for MIDI clips, shared by the live bake
//   (`InstrumentTrackController::publishRenderSnapshot`) and the offline proxy bake
//   (`proxy_render::ProxyOfflineSequencer`), so both render paths — and therefore realtime
//   playback, offline mixdown (which drives the live scheduler) and proxy rendering — select the
//   same audible events. Nothing here touches sessions, hosts or threads: inputs are sample
//   windows and sample-positioned events, outputs are sample spans and event lists.
//
// THE RULE (docs/CURRENT_ARCHITECTURE.md "MIDI clip layering")
//   On ONE source track the clips form a layer stack in STORED order (`clips_`; last = topmost —
//   the same order the lane paints and hit-tests, the order a new take or paste appends to, and
//   the order that round-trips through save / undo). The topmost clip owns its whole window
//   `[start, end)` — including rests and completely empty clips — and a clip underneath is heard
//   only outside the windows of the clips above it. Selection, focus and the MIDI editor never
//   change the stack. The selection is per SOURCE track: an instrument row's own clips and each
//   routed MIDI row are layered independently and still sound together through one instrument.
//
//   Notes:  a note is cut into one segment per audible span of its clip; a segment that starts
//           later than the note (the clip became audible again while the note was still running)
//           is a NEW Note On with the note's channel and velocity (the instrument's attack
//           restarts — accepted), and every segment ends at its span end or the note end.
//   CC / pitch bend: a clip's events are delivered only inside its audible spans; at the start
//           of each span the clip's current value (latest event strictly before the span) is
//           restated, so the controller state is chased whenever the winning clip changes.
//           Events at exactly the clip's own end sample belong to the clip when that edge is
//           audible (a recorded take releases a held pedal and ends held notes exactly there).
//           A controller the winning clip never defines keeps its last delivered value — there
//           is no universal reset and none is invented (the sticky-controller rule of the CC
//           model); recorded takes restate sustain / expression / wheel at their start so they
//           carry their own state. Hidden clips' events are never sent during a cover.
//
// REALTIME
//   Everything here runs at bake time on the message thread (or the proxy builder); the audio
//   thread only walks the resulting pre-sorted, pre-segmented event lists.
// =============================================================================

#include <algorithm>
#include <cstdint>
#include <vector>

namespace midi_layer_bake
{
    struct SampleSpan
    {
        std::int64_t start = 0;
        std::int64_t endExclusive = 0;
        [[nodiscard]] bool empty() const noexcept { return endExclusive <= start; }
    };

    /// One clip's timeline window (samples at the bake rate), bottom-to-top stack order.
    struct ClipWindow
    {
        std::int64_t start = 0;
        std::int64_t endExclusive = 0;
    };

    /// Subtract `cover` from every span of `spans` (all half-open); keeps spans sorted + disjoint.
    inline void subtractCoverFromSpans(std::vector<SampleSpan>& spans, const ClipWindow& cover)
    {
        if (cover.endExclusive <= cover.start)
        {
            return;
        }
        std::vector<SampleSpan> out;
        out.reserve(spans.size() + 1);
        for (const SampleSpan& s : spans)
        {
            if (cover.endExclusive <= s.start || cover.start >= s.endExclusive)
            {
                out.push_back(s); // no overlap
                continue;
            }
            if (cover.start > s.start)
            {
                out.push_back({ s.start, cover.start });
            }
            if (cover.endExclusive < s.endExclusive)
            {
                out.push_back({ cover.endExclusive, s.endExclusive });
            }
        }
        spans.swap(out);
    }

    /// Audible spans of every clip: its window minus the union of the windows of the clips ABOVE
    /// it (`windowsBottomToTop[i]` is covered by every `j > i`). Windows with non-positive length
    /// neither sound nor cover (they are skipped by the bake, matching the existing rule).
    [[nodiscard]] inline std::vector<std::vector<SampleSpan>> audibleSpansPerClip(
        const std::vector<ClipWindow>& windowsBottomToTop)
    {
        std::vector<std::vector<SampleSpan>> result(windowsBottomToTop.size());
        for (std::size_t i = 0; i < windowsBottomToTop.size(); ++i)
        {
            const ClipWindow& w = windowsBottomToTop[i];
            if (w.endExclusive <= w.start)
            {
                continue;
            }
            std::vector<SampleSpan>& spans = result[i];
            spans.push_back({ w.start, w.endExclusive });
            for (std::size_t j = i + 1; j < windowsBottomToTop.size() && !spans.empty(); ++j)
            {
                subtractCoverFromSpans(spans, windowsBottomToTop[j]);
            }
        }
        return result;
    }

    /// True when `sample` lies inside one of the (sorted, disjoint) spans.
    [[nodiscard]] inline bool spansContain(const std::vector<SampleSpan>& spans, const std::int64_t sample) noexcept
    {
        for (const SampleSpan& s : spans)
        {
            if (sample >= s.start && sample < s.endExclusive)
            {
                return true;
            }
            if (sample < s.start)
            {
                break;
            }
        }
        return false;
    }

    /// Cut the note `[on, offExclusive)` into its audible segments; `emit(segOn, segOffExclusive)`
    /// is called once per non-empty intersection, in time order. A segment with `segOn > on` is
    /// a resumed note (new Note On, same channel / velocity).
    template <typename Emit>
    inline void forEachAudibleNoteSegment(const std::int64_t on,
                                          const std::int64_t offExclusive,
                                          const std::vector<SampleSpan>& spans,
                                          Emit&& emit)
    {
        if (offExclusive <= on)
        {
            return;
        }
        for (const SampleSpan& s : spans)
        {
            const std::int64_t a = std::max(on, s.start);
            const std::int64_t b = std::min(offExclusive, s.endExclusive);
            if (b > a)
            {
                emit(a, b);
            }
            if (s.start >= offExclusive)
            {
                break;
            }
        }
    }

    /// One sample-positioned controller / pitch-bend value of a clip stream (already baked to
    /// absolute samples by the caller; sorted by `absSample`, stored order for equal samples).
    struct StreamEvent
    {
        std::int64_t absSample = 0;
        int value = 0;
    };

    /// Restrict one clip's stream to its audible spans with the chase rule:
    ///   * events inside a span are kept verbatim;
    ///   * at a span start with no event exactly there, the latest event STRICTLY before the span
    ///     start (the clip's current value — it may lie in a covered gap or before a trimmed
    ///     window) is restated at the span start, so the winning clip's state is chased;
    ///   * when `clipEndExclusive` is the end of the clip's last audible span (the clip's own edge
    ///     is audible, not covered), events at exactly that sample are kept too: a recorded take
    ///     releases a held pedal there. Events beyond are dropped (the window ends the state).
    /// Output is sorted by `absSample` (stable with respect to the input order).
    [[nodiscard]] inline std::vector<StreamEvent> restrictStreamToAudibleSpans(
        const std::vector<StreamEvent>& sortedClipEvents,
        const std::vector<SampleSpan>& spans,
        const std::int64_t clipEndExclusive)
    {
        std::vector<StreamEvent> out;
        if (spans.empty() || sortedClipEvents.empty())
        {
            return out;
        }
        out.reserve(sortedClipEvents.size() + spans.size());
        const bool ownEdgeAudible = spans.back().endExclusive == clipEndExclusive;
        for (const SampleSpan& s : spans)
        {
            // Chase value at the span start: latest event strictly before `s.start`.
            const auto firstAtOrAfter = std::lower_bound(
                sortedClipEvents.begin(), sortedClipEvents.end(), s.start,
                [](const StreamEvent& e, const std::int64_t v) { return e.absSample < v; });
            const bool hasEventExactlyAtStart
                = firstAtOrAfter != sortedClipEvents.end() && firstAtOrAfter->absSample == s.start;
            if (!hasEventExactlyAtStart && firstAtOrAfter != sortedClipEvents.begin())
            {
                // Several equal-sample events before the start: the last one is the current value.
                auto latest = firstAtOrAfter - 1;
                out.push_back({ s.start, latest->value });
            }
            for (auto it = firstAtOrAfter; it != sortedClipEvents.end() && it->absSample < s.endExclusive; ++it)
            {
                out.push_back(*it);
            }
        }
        if (ownEdgeAudible)
        {
            for (const StreamEvent& e : sortedClipEvents)
            {
                if (e.absSample == clipEndExclusive)
                {
                    out.push_back(e);
                }
            }
        }
        // Spans are visited in order and each span's events are ascending, so `out` is sorted;
        // the stable sort only guards the ownEdgeAudible tail against an earlier equal sample.
        std::stable_sort(out.begin(), out.end(),
                         [](const StreamEvent& a, const StreamEvent& b) { return a.absSample < b.absSample; });
        return out;
    }
} // namespace midi_layer_bake
