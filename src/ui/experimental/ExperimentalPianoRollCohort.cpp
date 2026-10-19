#include "ui/experimental/ExperimentalPianoRollView.h"

#include "instruments/InstrumentTrackController.h"

#include <juce_audio_devices/juce_audio_devices.h>

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace
{
[[nodiscard]] double cohortSampleRate(juce::AudioDeviceManager* dm) noexcept
{
    if (dm != nullptr)
    {
        if (juce::AudioIODevice* d = dm->getCurrentAudioDevice())
        {
            const double r = d->getCurrentSampleRate();
            if (r > 0.0 && std::isfinite(r))
            {
                return r;
            }
        }
    }
    return 48000.0;
}

[[nodiscard]] bool pointInDiamond(const float cx, const float cy, const float halfW, const float halfH,
                                  const float px, const float py) noexcept
{
    if (halfW <= 0.f || halfH <= 0.f)
    {
        return false;
    }
    juce::Path diamond;
    diamond.addQuadrilateral(cx, cy - halfH, cx + halfW, cy, cx, cy + halfH, cx - halfW, cy);
    return diamond.contains(px, py);
}

[[nodiscard]] midi_editor_cohort::ProjectSpan projectSpanForNote(const InstrumentMidiClip& clip,
                                                                  const ExperimentalMidiPattern& pattern,
                                                                  const TimelineMidiNote& note,
                                                                  const double sampleRate) noexcept
{
    const std::int64_t a0 = absoluteSampleForTimelineNote(clip.timelineAnchorSamples, note, pattern, sampleRate);
    const double bpm = pattern.bpm > 0.0 ? pattern.bpm : 120.0;
    const int tpq = experimentalEffectiveTicksPerQuarter(pattern);
    const std::int64_t dur = ticksToSignedSamples(juce::jmax<std::int64_t>(1, note.durationTicks), bpm, tpq, sampleRate);
    midi_editor_cohort::ProjectSpan span;
    span.startSamples = a0;
    span.endSamples = a0 + juce::jmax<std::int64_t>(1, dur);
    return span;
}
} // namespace

void ExperimentalPianoRollView::setAuditionPlayer(ExperimentalMidiPatternPlayer* player) noexcept
{
    player_ = player;
}

void ExperimentalPianoRollView::copyTimelineClipboard(std::vector<TimelineClipboardNote>& out) const
{
    out.clear();
    out.reserve(timelineInternalClipboard_.size());
    for (const auto& item : timelineInternalClipboard_)
    {
        if (!item.useProjectSamples)
        {
            continue;
        }
        TimelineClipboardNote n;
        n.deltaProjectSamples = item.deltaProjectSamples;
        n.durationSamples = item.durationSamples;
        n.midiNote = item.midiNote;
        n.velocity = item.velocity;
        n.offVelocity = item.offVelocity;
        n.channel = item.channel;
        out.push_back(n);
    }
}

void ExperimentalPianoRollView::setTimelineClipboard(std::vector<TimelineClipboardNote> items)
{
    timelineInternalClipboard_.clear();
    timelineInternalClipboard_.reserve(items.size());
    for (const auto& item : items)
    {
        InternalTimelineClipboardItem row;
        row.deltaProjectSamples = item.deltaProjectSamples;
        row.durationSamples = item.durationSamples > 0 ? item.durationSamples : 1;
        row.midiNote = item.midiNote;
        row.velocity = item.velocity;
        row.offVelocity = item.offVelocity;
        row.channel = item.channel;
        row.durationTicks = 240;
        row.useProjectSamples = true;
        timelineInternalClipboard_.push_back(row);
    }
}

ExperimentalMidiPattern* ExperimentalPianoRollView::patternForTrackClip(const TrackId trackId,
                                                                         const std::uint64_t clipId) const noexcept
{
    if (timelineClip_ != nullptr && pattern_ != nullptr
        && (clipId == 0 || clipId == static_cast<std::uint64_t>(timelineClip_->id))
        && (trackId == kInvalidTrackId || boundTrackId_ == kInvalidTrackId || trackId == boundTrackId_))
    {
        return pattern_;
    }
    for (const auto& member : cohort_)
    {
        if (member.trackId == trackId && member.clipId == clipId && member.clip != nullptr)
        {
            return &member.clip->pattern;
        }
    }
    return nullptr;
}

ExperimentalMidiPattern* ExperimentalPianoRollView::patternForCapture(const TimelineNoteMoveCapture& cap) const noexcept
{
    if (ExperimentalMidiPattern* pattern = patternForTrackClip(cap.trackId, cap.clipId))
    {
        return pattern;
    }
    return pattern_;
}

InstrumentMidiClip* ExperimentalPianoRollView::clipForTrackClip(const TrackId trackId,
                                                                 const std::uint64_t clipId) const noexcept
{
    if (timelineClip_ != nullptr
        && (clipId == 0 || clipId == static_cast<std::uint64_t>(timelineClip_->id))
        && (trackId == kInvalidTrackId || boundTrackId_ == kInvalidTrackId || trackId == boundTrackId_))
    {
        return timelineClip_;
    }
    for (const auto& member : cohort_)
    {
        if (member.trackId == trackId && member.clipId == clipId)
        {
            return member.clip;
        }
    }
    return nullptr;
}

void ExperimentalPianoRollView::syncEditorSelectionFlags() noexcept
{
    auto clearPattern = [](ExperimentalMidiPattern* pattern) {
        if (pattern == nullptr)
        {
            return;
        }
        for (auto& note : pattern->timelineNotes)
        {
            note.editorSelected = false;
        }
    };
    clearPattern(pattern_);
    for (const auto& member : cohort_)
    {
        if (member.clip != nullptr)
        {
            clearPattern(&member.clip->pattern);
        }
    }
    if (pattern_ != nullptr)
    {
        for (const int index : selectedTimelineNoteIndices_)
        {
            if (index >= 0 && index < (int)pattern_->timelineNotes.size())
            {
                auto& note = pattern_->timelineNotes[(size_t)index];
                ensureTimelineNoteUid(note);
                note.editorSelected = true;
            }
        }
    }
    for (const auto& foreign : foreignSelectedNotes_)
    {
        ExperimentalMidiPattern* pattern = patternForTrackClip(foreign.trackId, foreign.clipId);
        if (pattern == nullptr || foreign.index < 0 || foreign.index >= (int)pattern->timelineNotes.size())
        {
            continue;
        }
        auto& note = pattern->timelineNotes[(size_t)foreign.index];
        if (foreign.uid != 0 && note.uid != foreign.uid)
        {
            continue;
        }
        ensureTimelineNoteUid(note);
        note.editorSelected = true;
    }
}

void ExperimentalPianoRollView::rebuildSelectionFromEditorFlags() noexcept
{
    selectedTimelineNoteIndices_.clear();
    foreignSelectedNotes_.clear();
    auto take = [this](ExperimentalMidiPattern& pattern, const TrackId trackId, const std::uint64_t clipId,
                       const bool active) {
        for (int i = 0; i < (int)pattern.timelineNotes.size(); ++i)
        {
            auto& note = pattern.timelineNotes[(size_t)i];
            if (!note.editorSelected)
            {
                continue;
            }
            ensureTimelineNoteUid(note);
            if (active)
            {
                selectedTimelineNoteIndices_.insert(i);
            }
            else
            {
                foreignSelectedNotes_.push_back(ForeignSelectedNote{ trackId, clipId, note.uid, i });
            }
        }
    };
    if (cohort_.empty())
    {
        if (pattern_ != nullptr)
        {
            take(*pattern_, boundTrackId_, timelineClip_ != nullptr ? timelineClip_->id : 0, true);
        }
        return;
    }
    for (const auto& member : cohort_)
    {
        if (member.clip == nullptr)
        {
            continue;
        }
        const bool active = timelineClip_ != nullptr && member.clip == timelineClip_;
        take(member.clip->pattern, member.trackId, member.clipId, active);
    }
}

void ExperimentalPianoRollView::setCohort(std::vector<CohortMember> members) noexcept
{
    cohort_ = std::move(members);
    for (auto& member : cohort_)
    {
        member.active = timelineClip_ != nullptr && member.clip == timelineClip_;
        if (member.active)
        {
            boundTrackId_ = member.trackId;
        }
        if (member.clip == nullptr)
        {
            continue;
        }
        for (auto& note : member.clip->pattern.timelineNotes)
        {
            ensureTimelineNoteUid(note);
        }
    }
    rebuildSelectionFromEditorFlags();
    repaint();
}

void ExperimentalPianoRollView::reseatActiveMember(const CohortMember& member) noexcept
{
    if (member.clip == nullptr || member.clip == timelineClip_ || pattern_ == nullptr)
    {
        return;
    }
    const std::uint64_t visibleStart = (std::uint64_t)visibleStartSamples_;
    const double samplesPerPixel = samplesPerPixel_;
    juce::ignoreUnused(visibleStart, samplesPerPixel);
    pattern_ = &member.clip->pattern;
    timelineClip_ = member.clip;
    instrumentTrackController_ = member.controller;
    boundClipIdForSafety_ = member.clipId;
    boundTrackId_ = member.trackId;
    player_ = member.auditionPlayer;
    for (auto& other : cohort_)
    {
        other.active = other.clip == member.clip;
    }
    rebuildSelectionFromEditorFlags();
    if (onActiveCohortMemberChanged)
    {
        onActiveCohortMemberChanged(member.trackId, member.clipId);
    }
    repaint();
}

bool ExperimentalPianoRollView::noteIsSelected(const TrackId trackId, const std::uint64_t clipId,
                                               const int noteIndex) const noexcept
{
    if (timelineClip_ != nullptr && clipId == static_cast<std::uint64_t>(timelineClip_->id)
        && (trackId == boundTrackId_ || boundTrackId_ == kInvalidTrackId))
    {
        return selectedTimelineNoteIndices_.count(noteIndex) != 0u;
    }
    for (const auto& foreign : foreignSelectedNotes_)
    {
        if (foreign.trackId == trackId && foreign.clipId == clipId && foreign.index == noteIndex)
        {
            return true;
        }
    }
    return false;
}

std::optional<ExperimentalPianoRollView::CohortHit> ExperimentalPianoRollView::findTopCohortNote(
    const juce::Point<int> pos) const
{
    if (timelineClip_ == nullptr || pattern_ == nullptr || !useAbsoluteTimeline())
    {
        return std::nullopt;
    }
    std::vector<CohortMember> members = cohort_;
    if (members.empty())
    {
        CohortMember only;
        only.trackId = boundTrackId_;
        only.clipId = timelineClip_->id;
        only.clip = timelineClip_;
        only.active = true;
        members.push_back(only);
    }
    std::vector<midi_editor_cohort::ZMember> z;
    z.reserve(members.size());
    for (int i = 0; i < (int)members.size(); ++i)
    {
        midi_editor_cohort::ZMember row;
        row.active = members[(size_t)i].active || members[(size_t)i].clip == timelineClip_;
        row.trackOrder = members[(size_t)i].trackOrder;
        row.startSamples = members[(size_t)i].clip != nullptr ? members[(size_t)i].clip->startSamples : 0;
        row.clipId = members[(size_t)i].clipId;
        row.ordinal = i;
        z.push_back(row);
    }
    const std::vector<int> backToFront = midi_editor_cohort::paintOrderBackToFront(z);
    const auto gr = gridBounds();
    const double sr = cohortSampleRate(deviceManager_);
    const bool paintBars = timelineNotesDisplayComboId_ == 2;
    const bool pianoRowMode = rowLabelMode_ == 1;
    const float hitHalfW = pianoRowMode ? juce::jmax(4.0f, juce::jmin(6.8f, (float)kRowHeight * 0.48f))
                                        : juce::jmax(3.5f, juce::jmin(6.0f, (float)kRowHeight * 0.42f));
    const float hitHalfH = pianoRowMode ? juce::jmax(4.0f, (float)kRowHeight * 0.45f)
                                        : juce::jmax(3.5f, (float)kRowHeight * 0.4f);
    const float px = (float)pos.x;
    const float py = (float)pos.y;

    for (int step = (int)backToFront.size() - 1; step >= 0; --step)
    {
        const int memberIndex = z[(size_t)backToFront[(size_t)step]].ordinal;
        const CohortMember& member = members[(size_t)memberIndex];
        if (member.clip == nullptr)
        {
            continue;
        }
        ExperimentalMidiPattern& pattern = member.clip->pattern;
        const double bpm = pattern.bpm > 0.0 ? pattern.bpm : 120.0;
        const int tpq = experimentalEffectiveTicksPerQuarter(pattern);
        for (int i = (int)pattern.timelineNotes.size() - 1; i >= 0; --i)
        {
            const auto& note = pattern.timelineNotes[(size_t)i];
            if (note.midiNote < pitchLow_ || note.midiNote > pitchHigh_)
            {
                continue;
            }
            const auto row = visibleRowStripRect(gr, note.midiNote);
            if (!row)
            {
                continue;
            }
            const std::int64_t a0 = absoluteSampleForTimelineNote(member.clip->timelineAnchorSamples, note, pattern, sr);
            if (paintBars)
            {
                const std::int64_t dur = ticksToSignedSamples(juce::jmax<std::int64_t>(1, note.durationTicks), bpm, tpq, sr);
                float xL = xForSessionSample(a0);
                float xR = xForSessionSample(a0 + juce::jmax<std::int64_t>(1, dur));
                if (xR < xL)
                {
                    std::swap(xL, xR);
                }
                const float notePadY = pianoRowMode ? 1.0f : 2.0f;
                const float noteInsetV = pianoRowMode ? 2.0f : 4.0f;
                auto rect = juce::Rectangle<float>(xL, (float)row->getY() + notePadY, juce::jmax(4.0f, xR - xL),
                                                   (float)row->getHeight() - noteInsetV);
                if (rect.contains(px, py))
                {
                    return CohortHit{ memberIndex, i };
                }
            }
            else
            {
                const float cx = xForSessionSample(a0);
                const float cy = (float)row->getCentreY();
                if (pointInDiamond(cx, cy, hitHalfW, hitHalfH, px, py))
                {
                    return CohortHit{ memberIndex, i };
                }
            }
        }
    }
    return std::nullopt;
}

void ExperimentalPianoRollView::commitEditOrPromptExtend(const juce::String& label,
                                                         const bool notesSitOutside,
                                                         std::function<bool(bool extend)> apply)
{
    auto run = [this, label, apply](const bool extend) {
        if (apply == nullptr)
        {
            return;
        }
        if (undoablePatternEditHandler_ != nullptr && instrumentTrackController_ != nullptr && timelineClip_ != nullptr)
        {
            undoablePatternEditHandler_(label, [apply, extend]() { return apply(extend); });
        }
        else
        {
            apply(extend);
        }
    };
    if (!notesSitOutside)
    {
        run(false);
        return;
    }

    auto* box = new juce::AlertWindow("Notes outside the event",
                                      "Some edited notes sit outside their event bounds.",
                                      juce::AlertWindow::QuestionIcon);
    box->addButton("Extend event(s)", 1);
    box->addButton("Keep event boundaries", 2);
    box->addButton("Cancel", 0);
    juce::Component::SafePointer<ExperimentalPianoRollView> self(this);
    box->enterModalState(true,
                         juce::ModalCallbackFunction::create([self, run](const int result) {
                             if (self == nullptr || result == 0)
                             {
                                 return;
                             }
                             run(result == 1);
                         }),
                         true);
}

bool ExperimentalPianoRollView::capturedSpansNeedExtend(
    const std::vector<std::pair<InstrumentMidiClip*, midi_editor_cohort::ProjectSpan>>& spans) const noexcept
{
    for (const auto& item : spans)
    {
        if (item.first == nullptr)
        {
            continue;
        }
        const midi_editor_cohort::AudibleWindow window{ item.first->startSamples, item.first->lengthSamples,
                                                         item.first->timelineAnchorSamples };
        if (midi_editor_cohort::spanNeedsWindowExtend(window, item.second))
        {
            return true;
        }
    }
    return false;
}

void ExperimentalPianoRollView::extendClipsToCover(
    const std::vector<std::pair<InstrumentMidiClip*, midi_editor_cohort::ProjectSpan>>& spans) noexcept
{
    std::unordered_map<InstrumentMidiClip*, std::vector<midi_editor_cohort::ProjectSpan>> byClip;
    for (const auto& item : spans)
    {
        if (item.first != nullptr)
        {
            byClip[item.first].push_back(item.second);
        }
    }
    for (const auto& entry : byClip)
    {
        const midi_editor_cohort::AudibleWindow window{ entry.first->startSamples, entry.first->lengthSamples,
                                                         entry.first->timelineAnchorSamples };
        const midi_editor_cohort::AudibleWindow grown = midi_editor_cohort::extendWindowToCover(window, entry.second);
        entry.first->startSamples = grown.startSamples;
        entry.first->lengthSamples = grown.lengthSamples;
    }
}

bool ExperimentalPianoRollView::groupedCapturesOverlap(const std::vector<TimelineNoteMoveCapture>& captures,
                                                        const std::vector<TimelineMidiNote>& candidates) const noexcept
{
    struct Group
    {
        ExperimentalMidiPattern* pattern = nullptr;
        std::vector<int> ignore;
        std::vector<TimelineMidiNote> next;
        std::vector<TimelineMidiNote> original;
    };
    std::vector<Group> groups;
    for (size_t i = 0; i < captures.size() && i < candidates.size(); ++i)
    {
        ExperimentalMidiPattern* pattern = patternForTrackClip(captures[i].trackId, captures[i].clipId);
        if (pattern == nullptr)
        {
            continue;
        }
        Group* group = nullptr;
        for (auto& existing : groups)
        {
            if (existing.pattern == pattern)
            {
                group = &existing;
                break;
            }
        }
        if (group == nullptr)
        {
            groups.push_back(Group{ pattern, {}, {}, {} });
            group = &groups.back();
        }
        group->ignore.push_back(captures[i].index);
        group->next.push_back(candidates[i]);
        group->original.push_back(captures[i].original);
    }
    for (const auto& group : groups)
    {
        if (!validateTimelineNotesNoOverlap(group.pattern->timelineNotes, group.ignore, group.next,
                                            minTimelineNoteDurationTicks(), &group.original)
                 .valid)
        {
            return true;
        }
    }
    return false;
}

std::optional<juce::Rectangle<float>> ExperimentalPianoRollView::cohortNoteBounds(const CohortMember& member,
                                                                                const int noteIndex) const
{
    if (member.clip == nullptr || noteIndex < 0 || noteIndex >= (int)member.clip->pattern.timelineNotes.size()
        || samplesPerPixel_ <= 0.0 || !std::isfinite(samplesPerPixel_))
    {
        return std::nullopt;
    }
    if (member.clip == timelineClip_)
    {
        return getTimelineNoteVisualBounds(noteIndex);
    }
    const auto grid = gridBounds();
    const auto& note = member.clip->pattern.timelineNotes[(size_t)noteIndex];
    if (note.midiNote < pitchLow_ || note.midiNote > pitchHigh_)
    {
        return std::nullopt;
    }
    const auto row = visibleRowStripRect(grid, note.midiNote);
    if (!row)
    {
        return std::nullopt;
    }
    const double sr = cohortSampleRate(deviceManager_);
    const double bpm = member.clip->pattern.bpm > 0.0 ? member.clip->pattern.bpm : 120.0;
    const int tpq = experimentalEffectiveTicksPerQuarter(member.clip->pattern);
    const std::int64_t a0 = absoluteSampleForTimelineNote(member.clip->timelineAnchorSamples, note, member.clip->pattern, sr);
    const bool pianoRowMode = rowLabelMode_ == 1;
    if (timelineNotesDisplayComboId_ == 2)
    {
        const std::int64_t dur = ticksToSignedSamples(juce::jmax<std::int64_t>(1, note.durationTicks), bpm, tpq, sr);
        float xL = xForSessionSample(a0);
        float xR = xForSessionSample(a0 + juce::jmax<std::int64_t>(1, dur));
        if (xR < xL)
        {
            std::swap(xL, xR);
        }
        auto rect = juce::Rectangle<float>(xL, (float)row->getY() + 2.0f, juce::jmax(4.0f, xR - xL),
                                           (float)row->getHeight() - (pianoRowMode ? 2.0f : 4.0f));
        return rect;
    }
    const float halfW = pianoRowMode ? juce::jmax(4.0f, juce::jmin(6.8f, (float)kRowHeight * 0.48f))
                                     : juce::jmax(3.5f, juce::jmin(6.0f, (float)kRowHeight * 0.42f));
    const float halfH = pianoRowMode ? juce::jmax(4.0f, (float)kRowHeight * 0.45f)
                                     : juce::jmax(3.5f, (float)kRowHeight * 0.4f);
    const float cx = xForSessionSample(a0);
    const float cy = (float)row->getCentreY();
    return juce::Rectangle<float>(cx - halfW, cy - halfH, 2.0f * halfW, 2.0f * halfH);
}

void ExperimentalPianoRollView::paintOtherCohortNotes(juce::Graphics& g, const juce::Rectangle<int> grid) const
{
    if (cohort_.size() < 2 || timelineClip_ == nullptr)
    {
        return;
    }
    std::vector<midi_editor_cohort::ZMember> z;
    for (int i = 0; i < (int)cohort_.size(); ++i)
    {
        if (cohort_[(size_t)i].clip == nullptr || cohort_[(size_t)i].clip == timelineClip_)
        {
            continue;
        }
        midi_editor_cohort::ZMember row;
        row.active = false;
        row.trackOrder = cohort_[(size_t)i].trackOrder;
        row.startSamples = cohort_[(size_t)i].clip->startSamples;
        row.clipId = cohort_[(size_t)i].clipId;
        row.ordinal = i;
        z.push_back(row);
    }
    const std::vector<int> order = midi_editor_cohort::paintOrderBackToFront(z);
    const double sr = cohortSampleRate(deviceManager_);
    const bool paintBars = timelineNotesDisplayComboId_ == 2;
    const bool pianoRowMode = rowLabelMode_ == 1;
    const float hitHalfW = pianoRowMode ? juce::jmax(4.0f, juce::jmin(6.8f, (float)kRowHeight * 0.48f))
                                        : juce::jmax(3.5f, juce::jmin(6.0f, (float)kRowHeight * 0.42f));
    const float hitHalfH = pianoRowMode ? juce::jmax(4.0f, (float)kRowHeight * 0.45f)
                                        : juce::jmax(3.5f, (float)kRowHeight * 0.4f);
    for (const int ordinal : order)
    {
        const CohortMember& member = cohort_[(size_t)z[(size_t)ordinal].ordinal];
        if (member.clip == nullptr)
        {
            continue;
        }
        ExperimentalMidiPattern& pattern = member.clip->pattern;
        const double bpm = pattern.bpm > 0.0 ? pattern.bpm : 120.0;
        const int tpq = experimentalEffectiveTicksPerQuarter(pattern);
        const midi_editor_cohort::AudibleWindow window{ member.clip->startSamples, member.clip->lengthSamples,
                                                         member.clip->timelineAnchorSamples };
        for (int i = 0; i < (int)pattern.timelineNotes.size(); ++i)
        {
            const auto& note = pattern.timelineNotes[(size_t)i];
            if (note.midiNote < pitchLow_ || note.midiNote > pitchHigh_)
            {
                continue;
            }
            const auto row = visibleRowStripRect(grid, note.midiNote);
            if (!row)
            {
                continue;
            }
            const std::int64_t a0 = absoluteSampleForTimelineNote(member.clip->timelineAnchorSamples, note, pattern, sr);
            const bool inside = midi_editor_cohort::noteStartInsideAudibleWindow(window, a0);
            const bool selected = noteIsSelected(member.trackId, member.clipId, i);
            const midi_editor_cohort::NoteGlyphColours glyph = midi_editor_cohort::noteGlyphColours(
                midi_editor_cohort::roleForNote(selected, false, inside));
            const juce::Colour fill = juce::Colour::fromRGB(glyph.fill.r, glyph.fill.g, glyph.fill.b);
            const juce::Colour border = juce::Colour::fromRGB(glyph.edge.r, glyph.edge.g, glyph.edge.b);
            if (paintBars)
            {
                const std::int64_t dur = ticksToSignedSamples(juce::jmax<std::int64_t>(1, note.durationTicks), bpm, tpq, sr);
                float xL = xForSessionSample(a0);
                float xR = xForSessionSample(a0 + juce::jmax<std::int64_t>(1, dur));
                if (xR < xL)
                {
                    std::swap(xL, xR);
                }
                xL = juce::jmax(xL, (float)grid.getX());
                xR = juce::jmin(xR, (float)grid.getRight());
                if (xR <= (float)grid.getX() || xL >= (float)grid.getRight())
                {
                    continue;
                }
                auto rect = juce::Rectangle<float>(xL, (float)row->getY() + (pianoRowMode ? 1.0f : 2.0f), xR - xL,
                                                   (float)row->getHeight() - (pianoRowMode ? 2.0f : 4.0f));
                if (rect.getWidth() < 3.0f)
                {
                    rect = rect.withSizeKeepingCentre(4.0f, rect.getHeight());
                }
                g.setColour(fill);
                g.fillRoundedRectangle(rect, 2.0f);
                g.setColour(border);
                g.drawRoundedRectangle(rect, 2.0f, selected ? 2.2f : 1.0f);
            }
            else
            {
                const float cx = xForSessionSample(a0);
                if (cx < (float)grid.getX() - hitHalfW || cx > (float)grid.getRight() + hitHalfW)
                {
                    continue;
                }
                const float cy = (float)row->getCentreY();
                juce::Path diamond;
                diamond.addQuadrilateral(cx, cy - hitHalfH, cx + hitHalfW, cy, cx, cy + hitHalfH, cx - hitHalfW, cy);
                g.setColour(fill);
                g.fillPath(diamond);
                g.setColour(border);
                g.strokePath(diamond, juce::PathStrokeType(selected ? 2.0f : 1.1f));
            }
        }
    }
}

void ExperimentalPianoRollView::paintActiveEventShade(juce::Graphics& g, const juce::Rectangle<float> area) const
{
    if (timelineClip_ == nullptr || samplesPerPixel_ <= 0.0 || !std::isfinite(samplesPerPixel_)
        || timelineClip_->lengthSamples <= 0 || area.isEmpty())
    {
        return;
    }
    const float gx0 = area.getX();
    const float gx1 = area.getRight();
    const std::int64_t vis0 = timelineClip_->startSamples;
    const std::int64_t vis1 = vis0 + juce::jmax(std::int64_t{ 1 }, timelineClip_->lengthSamples);
    float xL = xForSessionSample(vis0);
    float xR = xForSessionSample(vis1);
    if (xR < xL)
    {
        std::swap(xL, xR);
    }
    const float bandL = juce::jlimit(gx0, gx1, xL);
    const float bandR = juce::jlimit(gx0, gx1, xR);
    const juce::Colour lift = juce::Colours::white.withAlpha(18.0f / 255.0f);
    const juce::Colour sink = juce::Colours::black.withAlpha(56.0f / 255.0f);
    if (bandL > gx0 + 0.5f)
    {
        g.setColour(lift);
        g.fillRect(gx0, area.getY(), bandL - gx0, area.getHeight());
    }
    if (bandR > bandL + 0.5f)
    {
        g.setColour(sink);
        g.fillRect(bandL, area.getY(), bandR - bandL, area.getHeight());
    }
    if (gx1 > bandR + 0.5f)
    {
        g.setColour(lift);
        g.fillRect(bandR, area.getY(), gx1 - bandR, area.getHeight());
    }
}

void ExperimentalPianoRollView::selectAppliedCaptures(
    const std::vector<TimelineNoteMoveCapture>& captures,
    const std::vector<TimelineMidiNote>& notes) noexcept
{
    selectedTimelineNoteIndices_.clear();
    foreignSelectedNotes_.clear();
    for (size_t k = 0; k < captures.size() && k < notes.size(); ++k)
    {
        ExperimentalMidiPattern* pattern = patternForCapture(captures[k]);
        if (pattern == nullptr || notes[k].uid == 0)
        {
            continue;
        }
        int found = -1;
        for (int i = 0; i < (int)pattern->timelineNotes.size(); ++i)
        {
            if (pattern->timelineNotes[(size_t)i].uid == notes[k].uid)
            {
                found = i;
                break;
            }
        }
        if (found < 0)
        {
            continue;
        }
        if (pattern == pattern_)
        {
            selectedTimelineNoteIndices_.insert(found);
        }
        else
        {
            foreignSelectedNotes_.push_back(
                ForeignSelectedNote{ captures[k].trackId, captures[k].clipId, notes[k].uid, found });
        }
    }
    syncEditorSelectionFlags();
}

void ExperimentalPianoRollView::notifyCohortControllersMusicalChange() noexcept
{
    std::vector<InstrumentTrackController*> seen;
    auto once = [&](InstrumentTrackController* controller) {
        if (controller == nullptr)
        {
            return;
        }
        for (InstrumentTrackController* existing : seen)
        {
            if (existing == controller)
            {
                return;
            }
        }
        seen.push_back(controller);
        controller->notifyClipExperimentalMusicalTimingChanged();
    };
    once(instrumentTrackController_);
    for (const auto& member : cohort_)
    {
        once(member.controller);
    }
}
