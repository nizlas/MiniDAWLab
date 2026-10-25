#pragma once

// =============================================================================
// MidiEditorCohort — shared MIDI-editor view over several events
// =============================================================================
//
// The editor is one window over a set of MIDI events. It does not merge them and does not
// change track ownership, instruments, routing, MIDI channels, or playback trim rules.
//
// Three facts stay separate:
//   * which events are members of the editor
//   * which one member is active (paste target, new notes, velocity, CC, background)
//   * which individual notes are selected (may span several owners)
//
// Note identity is (track, clip, uid). An index into a pattern is only a cache.
//
// Visual rule for a note that crosses its owner's trim:
//   A note is coloured when its start sample lies inside its OWN audible window
//   [start, start + length). The tail may cross the end and still use that colour, matching
//   playback (a note-on inside the window is heard until the trim). A note whose start is
//   outside its own window is drawn neutral grey for the whole glyph. The background shade is
//   the ACTIVE event's window only and never recolours another event's notes.
//
// No JUCE types: the piano roll and the focused test both call these functions.
// =============================================================================

#include <algorithm>
#include <cstdint>
#include <vector>

namespace midi_editor_cohort
{

struct EventKey
{
    std::uint64_t trackId = 0;
    std::uint64_t clipId = 0;
    int trackOrder = 0;
    std::int64_t startSamples = 0;
    bool lastClicked = false;
};

struct NoteId
{
    std::uint64_t trackId = 0;
    std::uint64_t clipId = 0;
    std::uint64_t noteUid = 0;

    friend bool operator==(const NoteId& a, const NoteId& b) noexcept
    {
        return a.trackId == b.trackId && a.clipId == b.clipId && a.noteUid == b.noteUid;
    }
};

enum class ArrangementClick : std::uint8_t
{
    Plain,
    Add,
    Toggle
};

[[nodiscard]] inline ArrangementClick classifyArrangementClick(const bool shift, const bool ctrl) noexcept
{
    if (ctrl)
    {
        return ArrangementClick::Toggle;
    }
    if (shift)
    {
        return ArrangementClick::Add;
    }
    return ArrangementClick::Plain;
}

/// Last-clicked member when it is in the set; otherwise track order, start, then clip id.
[[nodiscard]] inline int chooseInitialActiveIndex(const std::vector<EventKey>& members) noexcept
{
    int chosen = -1;
    for (int i = 0; i < (int)members.size(); ++i)
    {
        const bool better = chosen < 0
                            || (members[(size_t)i].lastClicked && !members[(size_t)chosen].lastClicked)
                            || (members[(size_t)i].lastClicked == members[(size_t)chosen].lastClicked
                                && (members[(size_t)i].trackOrder < members[(size_t)chosen].trackOrder
                                    || (members[(size_t)i].trackOrder == members[(size_t)chosen].trackOrder
                                        && (members[(size_t)i].startSamples < members[(size_t)chosen].startSamples
                                            || (members[(size_t)i].startSamples == members[(size_t)chosen].startSamples
                                                && members[(size_t)i].clipId < members[(size_t)chosen].clipId)))));
        if (better)
        {
            chosen = i;
        }
    }
    return chosen;
}

struct ZMember
{
    bool active = false;
    int trackOrder = 0;
    std::int64_t startSamples = 0;
    std::uint64_t clipId = 0;
    int ordinal = 0;
};

/// True when `a` is painted above `b` and hit-tested first.
[[nodiscard]] inline bool isAbove(const ZMember& a, const ZMember& b) noexcept
{
    if (a.active != b.active)
    {
        return a.active;
    }
    if (a.trackOrder != b.trackOrder)
    {
        return a.trackOrder < b.trackOrder;
    }
    if (a.startSamples != b.startSamples)
    {
        return a.startSamples < b.startSamples;
    }
    if (a.clipId != b.clipId)
    {
        return a.clipId < b.clipId;
    }
    return a.ordinal < b.ordinal;
}

/// Indices of `members` from back to front (the last index is hit first).
[[nodiscard]] inline std::vector<int> paintOrderBackToFront(const std::vector<ZMember>& members)
{
    std::vector<int> order((size_t)members.size());
    for (int i = 0; i < (int)members.size(); ++i)
    {
        order[(size_t)i] = i;
    }
    std::sort(order.begin(), order.end(), [&](const int a, const int b) {
        return isAbove(members[(size_t)b], members[(size_t)a]);
    });
    return order;
}

struct AudibleWindow
{
    std::int64_t startSamples = 0;
    std::int64_t lengthSamples = 1;
    std::int64_t anchorSamples = 0;
};

struct ProjectSpan
{
    std::int64_t startSamples = 0;
    std::int64_t endSamples = 0;
};

[[nodiscard]] inline std::int64_t audibleEnd(const AudibleWindow& w) noexcept
{
    const std::int64_t len = w.lengthSamples > 0 ? w.lengthSamples : 1;
    return w.startSamples + len;
}

/// Start inside the owner's audible window. A tail that crosses the end is still valid.
[[nodiscard]] inline bool noteStartInsideAudibleWindow(const AudibleWindow& w,
                                                       const std::int64_t noteStartSamples) noexcept
{
    return noteStartSamples >= w.startSamples && noteStartSamples < audibleEnd(w);
}

[[nodiscard]] inline bool spanNeedsWindowExtend(const AudibleWindow& w, const ProjectSpan& span) noexcept
{
    return span.startSamples < w.startSamples || span.endSamples > audibleEnd(w);
}

/// Grows the window just enough to cover every span. The anchor is unchanged, so existing
/// notes keep their absolute project times. Left growth can make previously trimmed material
/// audible again; that is the trim rule, not a rewrite of note ticks.
[[nodiscard]] inline AudibleWindow extendWindowToCover(const AudibleWindow& w,
                                                       const std::vector<ProjectSpan>& spans) noexcept
{
    std::int64_t newStart = w.startSamples;
    std::int64_t newEnd = audibleEnd(w);
    for (const ProjectSpan& s : spans)
    {
        if (s.startSamples < newStart)
        {
            newStart = s.startSamples;
        }
        if (s.endSamples > newEnd)
        {
            newEnd = s.endSamples;
        }
    }
    if (newStart < 0)
    {
        newStart = 0;
    }
    if (newEnd <= newStart)
    {
        newEnd = newStart + 1;
    }
    AudibleWindow out = w;
    out.startSamples = newStart;
    out.lengthSamples = newEnd - newStart;
    return out;
}

struct ClipboardSourceNote
{
    std::int64_t projectStartSamples = 0;
    std::int64_t durationSamples = 1;
    int midiNote = 60;
    int velocity = 100;
    int offVelocity = 64;
    int channel = 1;
};

struct ClipboardEntry
{
    std::int64_t deltaProjectSamples = 0;
    std::int64_t durationSamples = 1;
    int midiNote = 60;
    int velocity = 100;
    int offVelocity = 64;
    int channel = 1;
};

[[nodiscard]] inline std::vector<ClipboardEntry> buildProjectClipboard(
    const std::vector<ClipboardSourceNote>& notes)
{
    std::vector<ClipboardEntry> out;
    if (notes.empty())
    {
        return out;
    }
    std::int64_t minStart = notes.front().projectStartSamples;
    for (const auto& n : notes)
    {
        if (n.projectStartSamples < minStart)
        {
            minStart = n.projectStartSamples;
        }
    }
    out.reserve(notes.size());
    for (const auto& n : notes)
    {
        ClipboardEntry e;
        e.deltaProjectSamples = n.projectStartSamples - minStart;
        e.durationSamples = n.durationSamples > 0 ? n.durationSamples : 1;
        e.midiNote = n.midiNote;
        e.velocity = n.velocity;
        e.offVelocity = n.offVelocity;
        e.channel = n.channel;
        out.push_back(e);
    }
    return out;
}

[[nodiscard]] inline std::int64_t pastedProjectStart(const std::int64_t pasteAnchorProjectSamples,
                                                     const std::int64_t deltaProjectSamples) noexcept
{
    return pasteAnchorProjectSamples + deltaProjectSamples;
}

enum class NotePaintRole : std::uint8_t
{
    ActiveValid,
    OtherValid,
    OutsideOwner,
    Selected
};

struct Rgba
{
    unsigned char r = 0;
    unsigned char g = 0;
    unsigned char b = 0;
    unsigned char a = 255;
};

/// Velocity-lane bars only. Note glyphs do not use this: a note's colour is its role.
[[nodiscard]] inline Rgba velocityRgb(const int velocity) noexcept
{
    int v = velocity;
    if (v < 1)
    {
        v = 1;
    }
    if (v > 127)
    {
        v = 127;
    }
    const float t = (float)(v - 1) / 126.0f;
    const int lr = 0x5d, lg = 0x7f, lb = 0xae;
    const int mr = 0x9a, mg = 0x5a, mb = 0x96;
    const int hr = 0xe0, hg = 0x48, hb = 0x3e;
    auto mix = [](const int a, const int b, const float u) {
        return (unsigned char)(a + (int)((float)(b - a) * u + 0.5f));
    };
    Rgba c;
    if (t < 0.5f)
    {
        const float u = t * 2.0f;
        c.r = mix(lr, mr, u);
        c.g = mix(lg, mg, u);
        c.b = mix(lb, mb, u);
    }
    else
    {
        const float u = (t - 0.5f) * 2.0f;
        c.r = mix(mr, hr, u);
        c.g = mix(mg, hg, u);
        c.b = mix(mb, hb, u);
    }
    return c;
}

/// DAL's note red from before the velocity blend (`0xffe05a7a` / edge `0xff8a2c46`).
inline constexpr Rgba kActiveNoteFill{ 0xe0, 0x5a, 0x7a, 255 };
inline constexpr Rgba kActiveNoteEdge{ 0x8a, 0x2c, 0x46, 255 };
/// Fixed quieter red for another event's valid notes. Not a velocity mix.
inline constexpr Rgba kOtherNoteFill{ 0x9a, 0x48, 0x5c, 255 };
inline constexpr Rgba kOtherNoteEdge{ 0x5c, 0x2a, 0x36, 255 };
inline constexpr Rgba kSelectedNoteFill{ 0x10, 0x10, 0x12, 255 };
inline constexpr Rgba kSelectedNoteBorder{ 0xe0, 0x5a, 0x7a, 255 };
inline constexpr Rgba kOutsideNoteFill{ 0x6e, 0x6e, 0x76, 255 };
inline constexpr Rgba kOutsideNoteEdge{ 0x4a, 0x4a, 0x52, 255 };

struct NoteGlyphColours
{
    Rgba fill{};
    Rgba edge{};
};

/// Fill and border for one paint role. Velocity does not change either colour.
[[nodiscard]] inline NoteGlyphColours noteGlyphColours(const NotePaintRole role) noexcept
{
    switch (role)
    {
        case NotePaintRole::Selected:
            return NoteGlyphColours{ kSelectedNoteFill, kSelectedNoteBorder };
        case NotePaintRole::OutsideOwner:
            return NoteGlyphColours{ kOutsideNoteFill, kOutsideNoteEdge };
        case NotePaintRole::OtherValid:
            return NoteGlyphColours{ kOtherNoteFill, kOtherNoteEdge };
        case NotePaintRole::ActiveValid:
            break;
    }
    return NoteGlyphColours{ kActiveNoteFill, kActiveNoteEdge };
}

[[nodiscard]] inline Rgba mixRgb(const Rgba& a, const Rgba& b, const float u) noexcept
{
    auto m = [u](const unsigned char x, const unsigned char y) {
        return (unsigned char)(x + (int)((float)(y - x) * u + 0.5f));
    };
    return Rgba{ m(a.r, b.r), m(a.g, b.g), m(a.b, b.b), 255 };
}

[[nodiscard]] inline Rgba noteFillRgb(const NotePaintRole role, const int velocity) noexcept
{
    (void)velocity;
    return noteGlyphColours(role).fill;
}

[[nodiscard]] inline NotePaintRole roleForNote(const bool selected,
                                               const bool ownerIsActive,
                                               const bool startInsideOwner) noexcept
{
    if (selected)
    {
        return NotePaintRole::Selected;
    }
    if (!startInsideOwner)
    {
        return NotePaintRole::OutsideOwner;
    }
    return ownerIsActive ? NotePaintRole::ActiveValid : NotePaintRole::OtherValid;
}

/// Inside the active audible span the rows are darkened; outside they are lifted slightly.
[[nodiscard]] inline Rgba activeSpanShadeRgb(const bool insideActiveSpan) noexcept
{
    return insideActiveSpan ? Rgba{ 0, 0, 0, 56 } : Rgba{ 255, 255, 255, 18 };
}

} // namespace midi_editor_cohort
