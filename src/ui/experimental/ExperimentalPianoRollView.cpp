#include "ExperimentalPianoRollView.h"
#include "ExperimentalMidiPatternPlayer.h"
#include "instruments/InstrumentTrackController.h"
#include "ui/TimelineRulerView.h"
#include "ui/TimelineLocatorPainter.h"
#include "ui/PlayheadPixelMapping.h"

#include "diagnostics/DiagnosticBuildFlags.h"
#include "diagnostics/PlaybackUiLoadLog.h"
#include "domain/Session.h"
#include "domain/MusicalTimeConversions.h"
#include "domain/ProjectMusicalTime.h"
#include "domain/ArrangementMusicalSnap.h"
#include "transport/Transport.h"
#include "ui/SnapSettings.h"
#include "ui/TimelineViewportModel.h"
#include "ui/ForbiddenCursor.h"

#include <cmath>
#include <limits>

#include <algorithm>
#include <vector>

namespace
{
    constexpr bool kMidiEditorVerbosePianoRollMouseLog = false;
    /// Plain wheel over keys/grid: target ~1 pitch row per **physical** notch (Conny item 2: fine vertical
    /// positioning). Many hosts/OSes report `wheel.deltaY` ≈ 0.25 per notch, so scale by 4 so one notch ≈ one
    /// note/drum row; high-resolution trackpad sub-line deltas accumulate in `pitchWheelScrollRemainder_`.
    constexpr float kPitchScrollRowsPerWheelDelta = 4.0f;
    /// Timeline marquee: ignore sub-pixel jitter so a plain click does not start a drag rect.
    constexpr float kTimelineMarqueeDragThresholdPx = 5.0f;

    [[nodiscard]] double effectiveDeviceSampleRate(juce::AudioDeviceManager* dm) noexcept
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

    [[nodiscard]] bool isBlackKey(const int midiNote) noexcept
    {
        const int k = ((midiNote % 12) + 12) % 12;
        return k == 1 || k == 3 || k == 6 || k == 8 || k == 10;
    }

    /// Idle UI poll rate (playhead/locators when stopped, cheap full repaints).
    constexpr int kMidiRollTimerHzIdle = 6;
    /// During main transport or Debug Preview playback — ~display refresh for smooth playhead.
    constexpr int kMidiRollTimerHzAnimating = 60;

    /// Follow auto-scroll: edge-style follow — only pan when the playhead nears the right or left
    /// edge of the visible grid. Avoids micro-recentering; forward playback leaves the line crossing
    /// most of the view between scroll jumps.
    constexpr double kFollowRightThreshold = 0.92;
    constexpr double kFollowLeftThreshold = 0.08;
    constexpr double kFollowForwardResetPosition = 0.20;
    /// After a backward/seek correction, place slightly further right than forward reset (20–30%).
    constexpr double kFollowBackwardResetPosition = 0.25;

    /// Resync extrapolation when `|transport - predicted|` exceeds this (seek / cycle / dropout).
    constexpr double kPlayheadHardResyncSamples = 8192.0;

    /// Piano-roll vertical timeline grid: minimum screen spacing before drawing a line at this tier.
    constexpr double kTimelineGridMinorMinPx = 8.0;

    /// Hit-test only: must match diamond path used when painting timeline hits (compact diamonds).
    [[nodiscard]] inline bool beatGridNearBarBoundary(const double posBeats, const double barLenBeats) noexcept
    {
        if (!(barLenBeats > 1.0e-9) || !std::isfinite(posBeats) || !std::isfinite(barLenBeats))
        {
            return false;
        }
        double r = std::fmod(posBeats, barLenBeats);
        if (r < 0.0)
        {
            r += barLenBeats;
        }
        return r < 1.0e-4 || (barLenBeats - r) < 1.0e-4;
    }

    [[nodiscard]] inline bool beatGridNearBeatBoundary(const double posBeats) noexcept
    {
        if (!std::isfinite(posBeats))
        {
            return false;
        }
        double r = std::fmod(posBeats, 1.0);
        if (r < 0.0)
        {
            r += 1.0;
        }
        return r < 1.0e-4 || (1.0 - r) < 1.0e-4;
    }

    [[nodiscard]] bool pointInTimelineNoteDiamond(float cx,
                                                  float cy,
                                                  float halfW,
                                                  float halfH,
                                                  float px,
                                                  float py) noexcept
    {
        if (halfW <= 0.f || halfH <= 0.f)
        {
            return false;
        }
        juce::Path diamond;
        diamond.addQuadrilateral(cx, cy - halfH, cx + halfW, cy, cx, cy + halfH, cx - halfW, cy);
        return diamond.contains(px, py);
    }
} // namespace

ExperimentalPianoRollView::ExperimentalPianoRollView(ExperimentalMidiPattern& pattern,
                                                       ExperimentalMidiPatternPlayer* player)
    : pattern_(pattern)
    , player_(player)
{
    setOpaque(true);
    setWantsKeyboardFocus(true);
    setMouseClickGrabsKeyboardFocus(false);
    pitchScrollbar_.setAutoHide(false);
    pitchScrollbar_.setSingleStepSize(1.0);
    pitchScrollbar_.addListener(this);
    addAndMakeVisible(pitchScrollbar_);
    uiTimerHzConfigured_ = kMidiRollTimerHzIdle;
    startTimerHz(kMidiRollTimerHzIdle);
}

bool ExperimentalPianoRollView::keyPressed(const juce::KeyPress& key)
{
    if (dynamic_cast<juce::TextEditor*>(juce::Component::getCurrentlyFocusedComponent()) != nullptr)
    {
        return false;
    }
    if ((key.getKeyCode() == juce::KeyPress::deleteKey || key.getKeyCode() == juce::KeyPress::backspaceKey)
        && !key.getModifiers().isAnyModifierKeyDown())
    {
        return handleTimelineNotesDeleteSelectionShortcut();
    }
    const bool cmd = key.getModifiers().isCommandDown();
    if (!cmd || key.getModifiers().isShiftDown())
    {
        return false;
    }
    if (key.getKeyCode() == 'c' || key.getKeyCode() == 'C')
    {
        return handleTimelineNotesCopyShortcut();
    }
    if (key.getKeyCode() == 'v' || key.getKeyCode() == 'V')
    {
        return handleTimelineNotesPasteShortcut();
    }
    return false;
}

void ExperimentalPianoRollView::setEditablePitchRange(const int lowInclusive, const int highInclusive) noexcept
{
    int lo = juce::jlimit(0, 126, juce::jmin(lowInclusive, highInclusive));
    int hi = juce::jlimit(1, 127, juce::jmax(lowInclusive, highInclusive));
    if (hi <= lo)
    {
        hi = juce::jmin(127, lo + 1);
    }
    if (pitchLow_ == lo && pitchHigh_ == hi)
    {
        return;
    }
    pitchLow_ = lo;
    pitchHigh_ = hi;
    pitchWheelScrollRemainder_ = 0.0f;
    clampPitchScrollOffset();
    syncPitchScrollbarFromState();
    dismissRowLabelEditor(false);
    repaint();
    if (auto* vp = getParentComponent())
    {
        if (auto* chrome = vp->getParentComponent())
        {
            chrome->resized();
        }
    }
}

void ExperimentalPianoRollView::setRowLabelMode(const int comboId) noexcept
{
    const int id = juce::jlimit(1, 2, comboId);
    if (rowLabelMode_ == id)
    {
        return;
    }
    dismissRowLabelEditor(false);
    rowLabelMode_ = id;
    repaint();
}

void ExperimentalPianoRollView::setRowLabelProvider(std::function<juce::String(int)> fn) noexcept
{
    rowLabelProvider_ = std::move(fn);
    repaint();
}

void ExperimentalPianoRollView::setRowLabelTooltipProvider(std::function<juce::String(int)> fn) noexcept
{
    rowLabelTooltipProvider_ = std::move(fn);
}

void ExperimentalPianoRollView::setOnCommitRowLabelEdit(
    std::function<void(int, juce::String)> fn) noexcept
{
    onCommitRowLabelEdit_ = std::move(fn);
}

void ExperimentalPianoRollView::dismissRowLabelEditor(const bool commit)
{
    if (rowLabelEditor_ == nullptr)
    {
        return;
    }
    const int pitch = rowLabelEditorPitch_;
    const juce::String text = rowLabelEditor_->getText().trim();
    rowLabelEditor_->removeListener(this);
    removeChildComponent(rowLabelEditor_.get());
    rowLabelEditor_.reset();
    rowLabelEditorPitch_ = -1;
    if (commit && onCommitRowLabelEdit_)
    {
        onCommitRowLabelEdit_(pitch, text);
    }
    repaint();
}

void ExperimentalPianoRollView::beginRowLabelInlineEdit(const int midiNote)
{
    if (midiNote < pitchLow_ || midiNote > pitchHigh_ || rowLabelMode_ != 2 || !onCommitRowLabelEdit_)
    {
        return;
    }
    dismissRowLabelEditor(false);
    rowLabelEditorPitch_ = midiNote;
    rowLabelEditor_ = std::make_unique<juce::TextEditor>("rowLabelEdit");
    rowLabelEditor_->setMultiLine(false);
    rowLabelEditor_->setReturnKeyStartsNewLine(false);
    const juce::String initial = rowLabelProvider_ ? rowLabelProvider_(midiNote)
                                                   : juce::MidiMessage::getMidiNoteName(midiNote, true, true, 3);
    rowLabelEditor_->setText(initial, false);
    rowLabelEditor_->setSelectAllWhenFocused(true);
    rowLabelEditor_->setFont(juce::Font(juce::FontOptions().withHeight(9.0f)));
    rowLabelEditor_->addListener(this);
    addAndMakeVisible(*rowLabelEditor_);
    resized();
    rowLabelEditor_->toFront(false);
    rowLabelEditor_->grabKeyboardFocus();
}

void ExperimentalPianoRollView::textEditorReturnKeyPressed(juce::TextEditor& ed)
{
    if (rowLabelEditor_.get() == &ed)
    {
        dismissRowLabelEditor(true);
    }
    else if (velocityValueEditor_.get() == &ed)
    {
        dismissVelocityValueEditor(true);
    }
}

void ExperimentalPianoRollView::textEditorEscapeKeyPressed(juce::TextEditor& ed)
{
    if (rowLabelEditor_.get() == &ed)
    {
        dismissRowLabelEditor(false);
    }
    else if (velocityValueEditor_.get() == &ed)
    {
        dismissVelocityValueEditor(false);
    }
}

void ExperimentalPianoRollView::textEditorFocusLost(juce::TextEditor& ed)
{
    if (velocityValueEditor_.get() == &ed)
    {
        // Click-away cancels: velocity changes must be explicit (Return only).
        dismissVelocityValueEditor(false);
        return;
    }
    if (rowLabelEditor_.get() != &ed)
    {
        return;
    }
    // Escape / click-away: do not auto-commit renames (explicit Return only).
    dismissRowLabelEditor(false);
}

void ExperimentalPianoRollView::beginVelocityValueEdit(const int noteIndex, const juce::Point<int> anchorPos)
{
    if (!useAbsoluteTimeline() || timelineClip_ == nullptr
        || !isTimelineClipBindingFresh() || noteIndex < 0
        || noteIndex >= (int)pattern_.timelineNotes.size())
    {
        return;
    }
    dismissRowLabelEditor(false);
    dismissVelocityValueEditor(false);

    normalizeTimelineNoteSelection();
    velocityEditorTargetIndices_.clear();
    if (isTimelineNoteIndexSelected(noteIndex))
    {
        velocityEditorTargetIndices_.assign(selectedTimelineNoteIndices_.begin(),
                                            selectedTimelineNoteIndices_.end());
        std::sort(velocityEditorTargetIndices_.begin(), velocityEditorTargetIndices_.end());
    }
    else
    {
        // Unselected note: edit it alone and leave the existing selection untouched.
        velocityEditorTargetIndices_.push_back(noteIndex);
    }

    velocityValueEditor_ = std::make_unique<juce::TextEditor>("velocityValueEdit");
    velocityValueEditor_->setMultiLine(false);
    velocityValueEditor_->setReturnKeyStartsNewLine(false);
    velocityValueEditor_->setInputRestrictions(3, "0123456789");
    velocityValueEditor_->setJustification(juce::Justification::centred);
    velocityValueEditor_->setFont(juce::Font(juce::FontOptions().withHeight(12.0f)));
    velocityValueEditor_->setText(juce::String(pattern_.timelineNotes[(size_t)noteIndex].velocity), false);
    velocityValueEditor_->setSelectAllWhenFocused(true);
    velocityValueEditor_->addListener(this);

    // Slightly above/right of the click, clamped so it is never cut off at the editor edges.
    juce::Rectangle<int> box(anchorPos.getX() + 8, anchorPos.getY() - 24, 46, 20);
    box = box.constrainedWithin(getLocalBounds().reduced(2));
    velocityValueEditor_->setBounds(box);
    addAndMakeVisible(*velocityValueEditor_);
    velocityValueEditor_->toFront(false);
    velocityValueEditor_->grabKeyboardFocus();
    repaint();
}

void ExperimentalPianoRollView::dismissVelocityValueEditor(const bool commit)
{
    if (velocityValueEditor_ == nullptr)
    {
        return;
    }
    const juce::String text = velocityValueEditor_->getText().trim();
    const std::vector<int> targets = std::move(velocityEditorTargetIndices_);
    velocityEditorTargetIndices_.clear();
    velocityValueEditor_->removeListener(this);
    removeChildComponent(velocityValueEditor_.get());
    velocityValueEditor_.reset();
    repaint();

    if (!commit || targets.empty() || text.isEmpty() || !text.containsOnly("0123456789"))
    {
        return;
    }
    const int newVelocity = juce::jlimit(1, 127, text.getIntValue());

    std::vector<int> validTargets;
    validTargets.reserve(targets.size());
    bool anyChange = false;
    for (const int idx : targets)
    {
        if (idx < 0 || idx >= (int)pattern_.timelineNotes.size())
        {
            continue;
        }
        validTargets.push_back(idx);
        anyChange = anyChange || pattern_.timelineNotes[(size_t)idx].velocity != newVelocity;
    }
    if (validTargets.empty())
    {
        return;
    }

    if (anyChange)
    {
        auto applyVelocities = [this, validTargets, newVelocity]() -> bool {
            for (const int idx : validTargets)
            {
                if (idx < 0 || idx >= (int)pattern_.timelineNotes.size())
                {
                    return false;
                }
                pattern_.timelineNotes[(size_t)idx].velocity = newVelocity;
            }
            if (instrumentTrackController_ != nullptr)
            {
                instrumentTrackController_->notifyClipExperimentalMusicalTimingChanged();
            }
            repaint();
            return true;
        };
        const char* undoLabel
            = validTargets.size() > 1 ? "Set MIDI note velocities" : "Set MIDI note velocity";
        if (undoablePatternEditHandler_ != nullptr && instrumentTrackController_ != nullptr
            && timelineClip_ != nullptr)
        {
            undoablePatternEditHandler_(undoLabel, std::move(applyVelocities));
        }
        else
        {
            applyVelocities();
        }
    }

    // Audition feedback with the velocity-drag rule: chord only when every target shares one
    // startTick; mixed start times stay silent (no meaningful timing reference).
    if (player_ != nullptr)
    {
        bool sameStart = true;
        const std::int64_t t0 = pattern_.timelineNotes[(size_t)validTargets.front()].startTick;
        for (const int idx : validTargets)
        {
            if (pattern_.timelineNotes[(size_t)idx].startTick != t0)
            {
                sameStart = false;
                break;
            }
        }
        if (sameStart)
        {
            std::vector<ExperimentalMidiPatternPlayer::PreviewNoteRequest> chord;
            chord.reserve(validTargets.size());
            for (const int idx : validTargets)
            {
                const auto& tn = pattern_.timelineNotes[(size_t)idx];
                chord.push_back(
                    { tn.midiNote, tn.velocity, effectiveAuditionChannelForNote(tn), tn.offVelocity });
            }
            player_->previewNotesChord(chord);
        }
    }
}

ExperimentalPianoRollView::SelectedNotesVelocitySummary
ExperimentalPianoRollView::summarizeSelectedNotesVelocities() const noexcept
{
    SelectedNotesVelocitySummary s;
    bool velMixed = false;
    bool offMixed = false;
    for (const int idx : selectedTimelineNoteIndices_)
    {
        if (idx < 0 || idx >= (int)pattern_.timelineNotes.size())
        {
            continue;
        }
        const auto& n = pattern_.timelineNotes[(size_t)idx];
        const int off = sanitizeMidiNoteOffVelocity(n.offVelocity);
        ++s.selectedCount;
        if (s.selectedCount == 1)
        {
            s.velocity = n.velocity;
            s.offVelocity = off;
            continue;
        }
        velMixed = velMixed || (s.velocity.has_value() && *s.velocity != n.velocity);
        offMixed = offMixed || (s.offVelocity.has_value() && *s.offVelocity != off);
    }
    if (velMixed)
    {
        s.velocity.reset();
    }
    if (offMixed)
    {
        s.offVelocity.reset();
    }
    return s;
}

bool ExperimentalPianoRollView::applyVelocityToSelectedNotes(const int velocity)
{
    return applyVelocityFieldToSelectedNotes(false, velocity);
}

bool ExperimentalPianoRollView::applyOffVelocityToSelectedNotes(const int offVelocity)
{
    return applyVelocityFieldToSelectedNotes(true, offVelocity);
}

bool ExperimentalPianoRollView::applyVelocityFieldToSelectedNotes(const bool offField, const int value)
{
    normalizeTimelineNoteSelection();
    if (selectedTimelineNoteIndices_.empty())
    {
        return false;
    }
    const int newValue = offField ? sanitizeMidiNoteOffVelocity(value) : juce::jlimit(1, 127, value);

    std::vector<int> targets(selectedTimelineNoteIndices_.begin(), selectedTimelineNoteIndices_.end());
    std::sort(targets.begin(), targets.end());
    bool anyChange = false;
    for (const int idx : targets)
    {
        const auto& n = pattern_.timelineNotes[(size_t)idx];
        anyChange = anyChange || ((offField ? sanitizeMidiNoteOffVelocity(n.offVelocity) : n.velocity)
                                  != newValue);
    }
    if (!anyChange)
    {
        return false;
    }

    auto applyValues = [this, targets, newValue, offField]() -> bool {
        for (const int idx : targets)
        {
            if (idx < 0 || idx >= (int)pattern_.timelineNotes.size())
            {
                return false;
            }
            auto& n = pattern_.timelineNotes[(size_t)idx];
            if (offField)
            {
                n.offVelocity = newValue;
            }
            else
            {
                n.velocity = newValue;
            }
        }
        if (instrumentTrackController_ != nullptr)
        {
            instrumentTrackController_->notifyClipExperimentalMusicalTimingChanged();
        }
        repaint();
        return true;
    };

    const char* undoLabel = offField
                                ? (targets.size() > 1 ? "Set MIDI note-off velocities"
                                                      : "Set MIDI note-off velocity")
                                : (targets.size() > 1 ? "Set MIDI note velocities"
                                                      : "Set MIDI note velocity");
    if (undoablePatternEditHandler_ != nullptr && instrumentTrackController_ != nullptr
        && timelineClip_ != nullptr)
    {
        undoablePatternEditHandler_(undoLabel, std::move(applyValues));
    }
    else
    {
        applyValues();
    }
    return true;
}

ExperimentalPianoRollView::SelectedNotesDurationSummary
ExperimentalPianoRollView::summarizeSelectedNotesDurations() const noexcept
{
    SelectedNotesDurationSummary s;
    bool mixed = false;
    for (const int idx : selectedTimelineNoteIndices_)
    {
        if (idx < 0 || idx >= (int)pattern_.timelineNotes.size())
        {
            continue;
        }
        const std::int64_t dur
            = juce::jmax<std::int64_t>(1, pattern_.timelineNotes[(size_t)idx].durationTicks);
        ++s.selectedCount;
        if (s.selectedCount == 1)
        {
            s.durationTicks = dur;
            continue;
        }
        // Exact tick comparison: two notes that only *display* as the same `n.p.q.r` (rounded to the
        // nearest r unit) are still mixed, so committing the field can never silently requantize the
        // note the user did not mean to touch.
        mixed = mixed || (s.durationTicks.has_value() && *s.durationTicks != dur);
    }
    if (mixed)
    {
        s.durationTicks.reset();
    }
    return s;
}

midi_note_length::BarGrid ExperimentalPianoRollView::noteLengthBarGrid() const noexcept
{
    midi_note_length::BarGrid g;
    g.ticksPerQuarter = experimentalEffectiveTicksPerQuarter(pattern_);
    g.quartersPerBar = session_ != nullptr ? beatsPerBar(session_->getProjectMusicalTime()) : 4.0;
    return g;
}

std::int64_t ExperimentalPianoRollView::minimumNoteLengthTicks() const noexcept
{
    return minTimelineNoteDurationTicks();
}

int ExperimentalPianoRollView::trackMidiOutputChannel() const noexcept
{
    if (instrumentTrackController_ == nullptr || session_ == nullptr)
    {
        return kTrackMidiOutputChannelAny;
    }
    const TrackId tid = instrumentTrackController_->getExperimentalInstrumentDomainTrackId();
    if (tid == kInvalidTrackId)
    {
        return kTrackMidiOutputChannelAny;
    }
    const auto snap = session_->loadSessionSnapshotForAudioThread();
    if (snap == nullptr)
    {
        return kTrackMidiOutputChannelAny;
    }
    const int idx = snap->findTrackIndexById(tid);
    return idx >= 0 ? snap->getTrack(idx).getMidiOutputChannel() : kTrackMidiOutputChannelAny;
}

midi_channel_diag::NativeChannelSummary
ExperimentalPianoRollView::summarizeSelectedNotesNativeChannels() const noexcept
{
    midi_channel_diag::NativeChannelSummary s;
    for (const int idx : selectedTimelineNoteIndices_)
    {
        if (idx < 0 || idx >= (int)pattern_.timelineNotes.size())
        {
            continue;
        }
        midi_channel_diag::addNativeChannel(s, (int)pattern_.timelineNotes[(size_t)idx].channel);
    }
    return s;
}

ExperimentalPianoRollView::ChannelRemapOutcome
ExperimentalPianoRollView::remapNativeChannelsToTrackChannel(const ChannelRemapScope scope)
{
    ChannelRemapOutcome out;
    const int target = trackMidiOutputChannel();
    if (!midi_channel_diag::canRemapToTrackChannel(target))
    {
        out.result = ChannelRemapResult::NoFixedTrackChannel;
        return out;
    }
    out.targetChannel = target;

    struct ClipPlan
    {
        ExperimentalMidiPattern* pattern = nullptr;
        std::vector<int> indices;
    };
    const auto planForClip = [target](ExperimentalMidiPattern& pat,
                                      const std::vector<int>* restrictToIndices) {
        ClipPlan plan;
        plan.pattern = &pat;
        plan.indices
            = midi_channel_diag::planNativeChannelRemap(pat.timelineNotes, target, restrictToIndices);
        return plan;
    };

    std::vector<ClipPlan> plans;
    if (scope == ChannelRemapScope::SelectedNotes)
    {
        normalizeTimelineNoteSelection();
        out.notesInScope = (int)selectedTimelineNoteIndices_.size();
        if (out.notesInScope == 0)
        {
            out.result = ChannelRemapResult::NothingInScope;
            return out;
        }
        std::vector<int> sel(selectedTimelineNoteIndices_.begin(), selectedTimelineNoteIndices_.end());
        plans.push_back(planForClip(pattern_, &sel));
    }
    else
    {
        // Track-wide: every clip, including the open one. The musical undo snapshot already covers
        // the whole track, so this stays a single undo step.
        if (instrumentTrackController_ == nullptr)
        {
            out.result = ChannelRemapResult::NothingInScope;
            return out;
        }
        for (const auto& clip : instrumentTrackController_->getClips())
        {
            if (clip == nullptr)
            {
                continue;
            }
            out.notesInScope += (int)clip->pattern.timelineNotes.size();
            plans.push_back(planForClip(clip->pattern, nullptr));
        }
        if (out.notesInScope == 0)
        {
            out.result = ChannelRemapResult::NothingInScope;
            return out;
        }
    }

    for (const auto& plan : plans)
    {
        out.notesChanged += (int)plan.indices.size();
    }
    if (out.notesChanged == 0)
    {
        out.result = ChannelRemapResult::NoChange;
        return out;
    }

    // All-or-nothing across every clip in scope: channel is part of note identity here, so the
    // rewrite must not collapse two stacked notes into one.
    const std::int64_t minDur = minTimelineNoteDurationTicks();
    for (const auto& plan : plans)
    {
        if (midi_channel_diag::nativeChannelRemapWouldOverlap(
                plan.pattern->timelineNotes, plan.indices, target, minDur))
        {
            out.result = ChannelRemapResult::RejectedOverlap;
            out.notesChanged = 0;
            flashForbiddenNoDropCursor();
            return out;
        }
    }

    auto applyRemap = [this, plans, target]() -> bool {
        for (const auto& plan : plans)
        {
            if (plan.pattern == nullptr
                || !midi_channel_diag::applyNativeChannelRemap(
                    plan.pattern->timelineNotes, plan.indices, target))
            {
                return false;
            }
        }
        if (instrumentTrackController_ != nullptr)
        {
            instrumentTrackController_->notifyClipExperimentalMusicalTimingChanged();
        }
        repaint();
        return true;
    };

    const char* undoLabel = scope == ChannelRemapScope::SelectedNotes
                                ? "Remap selected MIDI notes to track channel"
                                : "Remap all MIDI notes to track channel";
    if (undoablePatternEditHandler_ != nullptr && instrumentTrackController_ != nullptr
        && timelineClip_ != nullptr)
    {
        undoablePatternEditHandler_(undoLabel, std::move(applyRemap));
    }
    else
    {
        applyRemap();
    }
    out.result = ChannelRemapResult::Applied;
    return out;
}

int ExperimentalPianoRollView::channelForNewlyCreatedNotes() const noexcept
{
    return midi_channel_diag::channelForNewNotes(trackMidiOutputChannel());
}

ExperimentalPianoRollView::NoteLengthApplyResult
ExperimentalPianoRollView::applyLengthTicksToSelectedNotes(const std::int64_t requestedTicks)
{
    normalizeTimelineNoteSelection();
    if (selectedTimelineNoteIndices_.empty())
    {
        return NoteLengthApplyResult::NoSelection;
    }
    // Same floor as mouse create/resize: numeric entry must not be a back door around the
    // (snap-aware) minimum, and a zero/short entry becomes the minimum rather than a dead note.
    const std::int64_t newDuration
        = juce::jmax<std::int64_t>(minTimelineNoteDurationTicks(), requestedTicks);

    std::vector<int> targets(selectedTimelineNoteIndices_.begin(), selectedTimelineNoteIndices_.end());
    std::sort(targets.begin(), targets.end());

    std::vector<TimelineMidiNote> originals;
    std::vector<TimelineMidiNote> candidates;
    originals.reserve(targets.size());
    candidates.reserve(targets.size());
    bool anyChange = false;
    for (const int idx : targets)
    {
        const auto& n = pattern_.timelineNotes[(size_t)idx];
        originals.push_back(n);
        TimelineMidiNote cand = n;
        cand.durationTicks = newDuration;
        candidates.push_back(cand);
        anyChange = anyChange || n.durationTicks != newDuration;
    }
    if (!anyChange)
    {
        return NoteLengthApplyResult::NoChange;
    }

    // All-or-nothing, and grandfathered: a pair of selected notes that already overlapped in an old
    // project must not block the edit, but no *new* overlap may be introduced.
    if (currentEditCandidatesOverlap(targets, candidates, &originals))
    {
        flashForbiddenNoDropCursor();
        return NoteLengthApplyResult::RejectedOverlap;
    }

    auto applyLengths = [this, targets, candidates]() -> bool {
        for (size_t k = 0; k < targets.size(); ++k)
        {
            const int idx = targets[k];
            if (idx < 0 || idx >= (int)pattern_.timelineNotes.size())
            {
                return false;
            }
            pattern_.timelineNotes[(size_t)idx].durationTicks = candidates[k].durationTicks;
        }
        if (instrumentTrackController_ != nullptr)
        {
            instrumentTrackController_->notifyClipExperimentalMusicalTimingChanged();
        }
        repaint();
        return true;
    };

    const char* undoLabel = targets.size() > 1 ? "Set MIDI note lengths" : "Set MIDI note length";
    if (undoablePatternEditHandler_ != nullptr && instrumentTrackController_ != nullptr
        && timelineClip_ != nullptr)
    {
        undoablePatternEditHandler_(undoLabel, std::move(applyLengths));
    }
    else
    {
        applyLengths();
    }
    return NoteLengthApplyResult::Applied;
}

int ExperimentalPianoRollView::timelineRulerHeight() const noexcept
{
    return useAbsoluteTimeline() ? kRulerHeight : 0;
}

int ExperimentalPianoRollView::sideStripTotalNow() const noexcept
{
    return currentSideStripTotal_;
}

int ExperimentalPianoRollView::sideStripContentWidthNow() const noexcept
{
    const int S = sideStripTotalNow();
    if (S <= 0)
    {
        return 0;
    }
    const int splitW = juce::jmin(collapsible_side_strip::kSplitterWidth, S);
    return juce::jmax(0, S - splitW);
}

void ExperimentalPianoRollView::setSideStripTotalWidthForUiOnly(const int totalIncludingSplitter) noexcept
{
    const int cap = rowLabelMode_ == 2
                        ? (kMidiEditorKeyboardLaneWidthDrumNamesMax + collapsible_side_strip::kSplitterWidth)
                        : (kMidiEditorKeyboardLaneWidthPianoMax + collapsible_side_strip::kSplitterWidth);
    const int nw = juce::jlimit(0, cap, totalIncludingSplitter);
    if (nw == currentSideStripTotal_)
    {
        return;
    }
    if (nw == 0)
    {
        dismissRowLabelEditor(false);
    }
    currentSideStripTotal_ = nw;
    resized();
    repaint();
}

int ExperimentalPianoRollView::keyboardColumnWidth() const noexcept
{
    return sideStripContentWidthNow();
}

juce::Rectangle<int> ExperimentalPianoRollView::rulerCornerBounds() const
{
    auto r = getLocalBounds();
    const int rh = timelineRulerHeight();
    if (rh <= 0)
    {
        return {};
    }
    auto top = r.removeFromTop(rh);
    return top.removeFromLeft(keyboardColumnWidth());
}

juce::Rectangle<int> ExperimentalPianoRollView::rulerTrackBounds() const
{
    auto r = getLocalBounds();
    const int rh = timelineRulerHeight();
    if (rh <= 0)
    {
        return {};
    }
    auto top = r.removeFromTop(rh);
    top.removeFromLeft(keyboardColumnWidth());
    return top;
}

juce::Rectangle<int> ExperimentalPianoRollView::keyboardBounds() const
{
    auto r = getLocalBounds();
    r.removeFromTop(timelineRulerHeight());
    r.removeFromBottom(velocityLaneTotalHeight() + ccLaneTotalHeight());
    return r.removeFromLeft(keyboardColumnWidth());
}

juce::Rectangle<int> ExperimentalPianoRollView::gridBounds() const
{
    auto r = getLocalBounds();
    r.removeFromTop(timelineRulerHeight());
    r.removeFromBottom(velocityLaneTotalHeight() + ccLaneTotalHeight());
    r.removeFromLeft(sideStripTotalNow());
    r.removeFromRight(kPitchScrollbarWidthPx);
    return r;
}

int ExperimentalPianoRollView::maxVelocityLaneHeightNow() const noexcept
{
    // Never let the lane swallow the grid: always keep at least a few pitch rows visible,
    // and cap at ~50% of the component so tiny windows stay usable.
    const int minGridPx = kRowHeight * 3;
    const int available = getHeight() - timelineRulerHeight() - ccLaneTotalHeight() - minGridPx;
    return juce::jlimit(0, getHeight() / 2, available);
}

int ExperimentalPianoRollView::velocityLaneTotalHeight() const noexcept
{
    return juce::jlimit(0, maxVelocityLaneHeightNow(), velocityLaneHeightPref_);
}

juce::Rectangle<int> ExperimentalPianoRollView::velocityLaneResizeBandBounds() const
{
    const int laneH = velocityLaneTotalHeight();
    if (laneH <= 0)
    {
        return {};
    }
    auto r = getLocalBounds();
    r.removeFromTop(timelineRulerHeight());
    r.removeFromBottom(ccLaneTotalHeight());
    auto lane = r.removeFromBottom(laneH);
    return lane.removeFromTop(kVelocityLaneResizeBandPx);
}

juce::Rectangle<int> ExperimentalPianoRollView::velocityLaneCollapsedKnobBounds() const
{
    if (velocityLaneTotalHeight() > 0 || maxVelocityLaneHeightNow() <= 0)
    {
        return {};
    }
    const auto gr = gridBounds();
    if (gr.isEmpty())
    {
        return {};
    }
    return { gr.getCentreX() - kVelocityLaneKnobWidth / 2,
             getHeight() - ccLaneTotalHeight() - kVelocityLaneKnobHeight,
             kVelocityLaneKnobWidth,
             kVelocityLaneKnobHeight };
}

juce::Rectangle<int> ExperimentalPianoRollView::velocityLaneBounds() const
{
    auto r = getLocalBounds();
    r.removeFromTop(timelineRulerHeight());
    r.removeFromBottom(ccLaneTotalHeight());
    auto lane = r.removeFromBottom(velocityLaneTotalHeight());
    lane.removeFromLeft(sideStripTotalNow());
    return lane;
}

juce::Rectangle<int> ExperimentalPianoRollView::velocityLaneHeaderBounds() const
{
    auto r = getLocalBounds();
    r.removeFromTop(timelineRulerHeight());
    r.removeFromBottom(ccLaneTotalHeight());
    auto lane = r.removeFromBottom(velocityLaneTotalHeight());
    return lane.removeFromLeft(sideStripTotalNow());
}

// --- MIDI CC automation lane (Stage D) --------------------------------------------------------

int ExperimentalPianoRollView::maxCcLaneHeightNow() const noexcept
{
    const int minGridPx = kRowHeight * 3;
    const int available
        = getHeight() - timelineRulerHeight() - velocityLaneTotalHeight() - minGridPx;
    return juce::jlimit(0, getHeight() / 2, available);
}

int ExperimentalPianoRollView::ccLaneTotalHeight() const noexcept
{
    // NOTE: deliberately not defined via maxCcLaneHeightNow() (which subtracts the velocity lane,
    // which subtracts this) — clamp against the raw available space to avoid recursion.
    const int minGridPx = kRowHeight * 3;
    const int available = getHeight() - timelineRulerHeight() - minGridPx;
    return juce::jlimit(0, juce::jmax(0, juce::jmin(available, getHeight() / 2)), ccLaneHeightPref_);
}

juce::Rectangle<int> ExperimentalPianoRollView::ccLaneBounds() const
{
    auto r = getLocalBounds();
    r.removeFromTop(timelineRulerHeight());
    auto lane = r.removeFromBottom(ccLaneTotalHeight());
    lane.removeFromLeft(sideStripTotalNow());
    return lane;
}

juce::Rectangle<int> ExperimentalPianoRollView::ccLaneHeaderBounds() const
{
    auto r = getLocalBounds();
    r.removeFromTop(timelineRulerHeight());
    auto lane = r.removeFromBottom(ccLaneTotalHeight());
    return lane.removeFromLeft(sideStripTotalNow());
}

juce::Rectangle<int> ExperimentalPianoRollView::ccLaneInnerBounds() const
{
    auto inner = ccLaneBounds();
    inner.removeFromTop(kCcLaneResizeBandPx + 2);
    inner.removeFromBottom(4);
    return inner;
}

juce::Rectangle<int> ExperimentalPianoRollView::ccLaneResizeBandBounds() const
{
    const int laneH = ccLaneTotalHeight();
    if (laneH <= 0)
    {
        return {};
    }
    auto r = getLocalBounds();
    auto lane = r.removeFromBottom(laneH);
    return lane.removeFromTop(kCcLaneResizeBandPx);
}

juce::Rectangle<int> ExperimentalPianoRollView::ccLaneCollapsedKnobBounds() const
{
    if (ccLaneTotalHeight() > 0 || maxCcLaneHeightNow() <= 0)
    {
        return {};
    }
    const auto gr = gridBounds();
    if (gr.isEmpty())
    {
        return {};
    }
    // Offset right of the velocity knob position so the two collapsed handles never overlap.
    return { gr.getCentreX() - kCcLaneKnobWidth / 2 + kVelocityLaneKnobWidth + 24,
             getHeight() - kCcLaneKnobHeight,
             kCcLaneKnobWidth,
             kCcLaneKnobHeight };
}

bool ExperimentalPianoRollView::ccLaneEditingAvailable() const noexcept
{
    return velocityLaneEditingAvailable();
}

int ExperimentalPianoRollView::ccValueFromLaneY(const int y) const noexcept
{
    const auto inner = ccLaneInnerBounds();
    if (inner.getHeight() <= 0)
    {
        return 0;
    }
    const double t = (double)(inner.getBottom() - y) / (double)inner.getHeight();
    return juce::jlimit(0, 127, (int)std::llround(t * 127.0));
}

float ExperimentalPianoRollView::ccLaneYForValue(const int value) const noexcept
{
    const auto inner = ccLaneInnerBounds();
    return (float)inner.getBottom()
           - (float)juce::jlimit(0, 127, value) / 127.0f * (float)inner.getHeight();
}

std::optional<float> ExperimentalPianoRollView::ccPointXForIndex(const int idx) const
{
    if (timelineClip_ == nullptr || idx < 0 || idx >= (int)pattern_.ccPoints.size())
    {
        return std::nullopt;
    }
    const auto& p = pattern_.ccPoints[(size_t)idx];
    if ((int)p.controller != ccLaneController_)
    {
        return std::nullopt;
    }
    const double sr = effectiveDeviceSampleRate(deviceManager_);
    const double bpm = pattern_.bpm > 0.0 ? pattern_.bpm : 120.0;
    const int tpq = experimentalEffectiveTicksPerQuarter(pattern_);
    const std::int64_t absS
        = timelineClip_->timelineAnchorSamples + ticksToSignedSamples(p.startTick, bpm, tpq, sr);
    return xForSessionSample(absS);
}

std::optional<int> ExperimentalPianoRollView::findCcPointIndexNear(const juce::Point<int> pos) const
{
    std::optional<int> best;
    double bestDist = 1.0e9;
    for (int i = 0; i < (int)pattern_.ccPoints.size(); ++i)
    {
        const auto xOpt = ccPointXForIndex(i);
        if (!xOpt)
        {
            continue;
        }
        const float py = ccLaneYForValue((int)pattern_.ccPoints[(size_t)i].value);
        const double dx = (double)pos.getX() - (double)*xOpt;
        const double dy = (double)pos.getY() - (double)py;
        const double d = std::sqrt(dx * dx + dy * dy);
        if (d <= (double)kCcPointHitRadiusPx + 2.0 && d < bestDist)
        {
            best = i;
            bestDist = d;
        }
    }
    return best;
}

void ExperimentalPianoRollView::normalizeCcSelection() noexcept
{
    for (auto it = selectedCcPointIndices_.begin(); it != selectedCcPointIndices_.end();)
    {
        if (*it < 0 || *it >= (int)pattern_.ccPoints.size()
            || (int)pattern_.ccPoints[(size_t)*it].controller != ccLaneController_)
        {
            it = selectedCcPointIndices_.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

void ExperimentalPianoRollView::handleCcLaneMouseDown(const juce::MouseEvent& e)
{
    if (!ccLaneEditingAvailable())
    {
        return;
    }
    normalizeCcSelection();
    const auto pos = e.getPosition();

    if (e.mods.isPopupMenu())
    {
        if (const auto hit = findCcPointIndexNear(pos))
        {
            if (selectedCcPointIndices_.count(*hit) == 0)
            {
                selectedCcPointIndices_.clear();
                selectedCcPointIndices_.insert(*hit);
            }
            showCcPointContextMenu(*hit);
        }
        else
        {
            showCcControllerMenu();
        }
        repaint();
        return;
    }
    if (!e.mods.isLeftButtonDown())
    {
        return;
    }

    if (const auto hit = findCcPointIndexNear(pos))
    {
        if (e.mods.isShiftDown() || e.mods.isCommandDown())
        {
            if (selectedCcPointIndices_.count(*hit) > 0)
            {
                selectedCcPointIndices_.erase(*hit);
                repaint();
                return;
            }
            selectedCcPointIndices_.insert(*hit);
        }
        else if (selectedCcPointIndices_.count(*hit) == 0)
        {
            selectedCcPointIndices_.clear();
            selectedCcPointIndices_.insert(*hit);
        }

        ccDragCaptures_.clear();
        for (const int i : selectedCcPointIndices_)
        {
            if (i >= 0 && i < (int)pattern_.ccPoints.size())
            {
                ccDragCaptures_.push_back({ i, pattern_.ccPoints[(size_t)i] });
            }
        }
        if (ccDragCaptures_.empty())
        {
            repaint();
            return;
        }
        ccPointDragActive_ = true;
        ccDragMoved_ = false;
        ccDragPrimaryIndex_ = *hit;
        ccDragAnchorTick_ = pattern_.ccPoints[(size_t)*hit].startTick;
        ccDragAnchorValue_ = ccValueFromLaneY(pos.getY());
        repaint();
        return;
    }

    // Empty lane: insert a new point at the snapped position/value (one undoable edit).
    insertCcPointAt(pos);
}

void ExperimentalPianoRollView::updateCcLaneDrag(const juce::Point<int> localPos)
{
    if (!ccPointDragActive_ || !ccLaneEditingAvailable() || ccDragCaptures_.empty()
        || timelineClip_ == nullptr)
    {
        return;
    }
    ccDragMoved_ = true;

    const double sr = effectiveDeviceSampleRate(deviceManager_);
    const double bpm = pattern_.bpm > 0.0 ? pattern_.bpm : 120.0;
    const int tpq = experimentalEffectiveTicksPerQuarter(pattern_);
    const std::int64_t cursorAbs = sampleAtGridX((float)localPos.getX());
    const std::int64_t cursorTickRaw
        = relativeSamplesToTicks(cursorAbs - timelineClip_->timelineAnchorSamples, bpm, tpq, sr);
    const std::int64_t cursorTick = snapTimelineTickForEdit(cursorTickRaw);
    const std::int64_t tickDelta = cursorTick - ccDragAnchorTick_;

    const int cursorValue = ccValueFromLaneY(localPos.getY());
    const int valueDelta = cursorValue - ccDragAnchorValue_;

    for (const auto& cap : ccDragCaptures_)
    {
        if (cap.index < 0 || cap.index >= (int)pattern_.ccPoints.size())
        {
            continue;
        }
        auto& p = pattern_.ccPoints[(size_t)cap.index];
        p.startTick = juce::jmax<std::int64_t>(0, cap.original.startTick + tickDelta);
        if (ccDragCaptures_.size() == 1)
        {
            p.value = (std::uint8_t)juce::jlimit(0, 127, cursorValue);
        }
        else
        {
            p.value = (std::uint8_t)juce::jlimit(0, 127, (int)cap.original.value + valueDelta);
        }
    }
}

void ExperimentalPianoRollView::finishCcLaneDragGesture()
{
    if (!ccPointDragActive_)
    {
        return;
    }
    ccPointDragActive_ = false;
    ccDragPrimaryIndex_ = -1;
    const auto captures = std::move(ccDragCaptures_);
    ccDragCaptures_.clear();
    if (captures.empty() || !ccDragMoved_)
    {
        repaint();
        return;
    }

    // Capture the drag results, rewind the live preview, then commit as ONE undoable edit so
    // undo/redo sees exactly one step (same pattern as velocity drags). Notes and note selection
    // are never touched.
    std::vector<MidiCcPoint> finals;
    finals.reserve(captures.size());
    bool anyChange = false;
    for (const auto& cap : captures)
    {
        if (cap.index < 0 || cap.index >= (int)pattern_.ccPoints.size())
        {
            finals.push_back(cap.original);
            continue;
        }
        finals.push_back(pattern_.ccPoints[(size_t)cap.index]);
        pattern_.ccPoints[(size_t)cap.index] = cap.original;
        anyChange = anyChange
                    || finals.back().startTick != cap.original.startTick
                    || finals.back().value != cap.original.value;
    }
    if (!anyChange)
    {
        repaint();
        return;
    }

    const auto applyEdit = [this, captures, finals]() -> bool {
        for (size_t i = 0; i < captures.size(); ++i)
        {
            const int idx = captures[i].index;
            if (idx >= 0 && idx < (int)pattern_.ccPoints.size())
            {
                pattern_.ccPoints[(size_t)idx] = finals[i];
            }
        }
        (void)midi_cc::normalizePoints(pattern_.ccPoints);
        if (instrumentTrackController_ != nullptr && timelineClip_ != nullptr)
        {
            instrumentTrackController_->notifyClipPatternMutated(timelineClip_->id);
        }
        repaint();
        return true;
    };

    selectedCcPointIndices_.clear();
    const std::vector<MidiCcPoint> reselect = finals;
    if (undoablePatternEditHandler_ != nullptr && instrumentTrackController_ != nullptr
        && timelineClip_ != nullptr)
    {
        undoablePatternEditHandler_("Edit CC points", applyEdit);
    }
    else
    {
        applyEdit();
    }
    // Re-select by identity: normalizePoints re-sorted the vector, so indices moved.
    for (const auto& f : reselect)
    {
        for (int i = 0; i < (int)pattern_.ccPoints.size(); ++i)
        {
            if (midi_cc::pointsShareIdentity(pattern_.ccPoints[(size_t)i], f))
            {
                selectedCcPointIndices_.insert(i);
                break;
            }
        }
    }
    repaint();
}

void ExperimentalPianoRollView::insertCcPointAt(const juce::Point<int> pos)
{
    if (!ccLaneEditingAvailable() || timelineClip_ == nullptr)
    {
        return;
    }
    const double sr = effectiveDeviceSampleRate(deviceManager_);
    const double bpm = pattern_.bpm > 0.0 ? pattern_.bpm : 120.0;
    const int tpq = experimentalEffectiveTicksPerQuarter(pattern_);
    const std::int64_t rawTick = relativeSamplesToTicks(
        sampleAtGridX((float)pos.getX()) - timelineClip_->timelineAnchorSamples, bpm, tpq, sr);

    MidiCcPoint p;
    p.startTick = juce::jmax<std::int64_t>(0, snapTimelineTickForCreate(rawTick));
    p.controller = (std::uint8_t)midi_cc::sanitizeController(ccLaneController_);
    p.value = (std::uint8_t)ccValueFromLaneY(pos.getY());
    // Same new-event channel rule as notes: the fixed track channel when set, else the legacy
    // default. Changing the track output later never rewrites this stored native channel.
    p.channel
        = (std::uint8_t)midi_channel_diag::channelForNewNotes(trackMidiOutputChannel());
    p.interpolationToNext = MidiCcInterpolation::linear;

    const auto applyInsert = [this, p]() -> bool {
        pattern_.ccPoints.push_back(p);
        (void)midi_cc::normalizePoints(pattern_.ccPoints);
        if (instrumentTrackController_ != nullptr && timelineClip_ != nullptr)
        {
            instrumentTrackController_->notifyClipPatternMutated(timelineClip_->id);
        }
        repaint();
        return true;
    };
    if (undoablePatternEditHandler_ != nullptr && instrumentTrackController_ != nullptr)
    {
        undoablePatternEditHandler_("Insert CC point", applyInsert);
    }
    else
    {
        applyInsert();
    }
    selectedCcPointIndices_.clear();
    for (int i = 0; i < (int)pattern_.ccPoints.size(); ++i)
    {
        if (midi_cc::pointsShareIdentity(pattern_.ccPoints[(size_t)i], p))
        {
            selectedCcPointIndices_.insert(i);
            break;
        }
    }
    repaint();
}

void ExperimentalPianoRollView::deleteSelectedCcPoints()
{
    normalizeCcSelection();
    if (selectedCcPointIndices_.empty() || !ccLaneEditingAvailable())
    {
        return;
    }
    std::vector<int> indices(selectedCcPointIndices_.begin(), selectedCcPointIndices_.end());
    std::sort(indices.begin(), indices.end(), std::greater<int>());
    selectedCcPointIndices_.clear();

    const auto applyDelete = [this, indices]() -> bool {
        for (const int i : indices)
        {
            if (i >= 0 && i < (int)pattern_.ccPoints.size())
            {
                pattern_.ccPoints.erase(pattern_.ccPoints.begin() + i);
            }
        }
        if (instrumentTrackController_ != nullptr && timelineClip_ != nullptr)
        {
            instrumentTrackController_->notifyClipPatternMutated(timelineClip_->id);
        }
        repaint();
        return true;
    };
    if (undoablePatternEditHandler_ != nullptr && instrumentTrackController_ != nullptr)
    {
        undoablePatternEditHandler_("Delete CC points", applyDelete);
    }
    else
    {
        applyDelete();
    }
    repaint();
}

bool ExperimentalPianoRollView::handleCcPointsDeleteSelectionShortcut()
{
    normalizeCcSelection();
    if (selectedCcPointIndices_.empty() || !ccLaneEditingAvailable())
    {
        return false;
    }
    deleteSelectedCcPoints();
    return true;
}

void ExperimentalPianoRollView::showCcPointContextMenu(const int pointIndex)
{
    if (pointIndex < 0 || pointIndex >= (int)pattern_.ccPoints.size())
    {
        return;
    }
    const auto& p = pattern_.ccPoints[(size_t)pointIndex];
    juce::PopupMenu menu;
    menu.addSectionHeader(midi_cc::controllerDisplayName((int)p.controller) + "  value "
                          + juce::String((int)p.value) + "  ch " + juce::String((int)p.channel));
    menu.addItem(1, "Delete point(s)");
    menu.addSeparator();
    menu.addItem(2, "Segment to next: Hold (step)", true,
                 p.interpolationToNext == MidiCcInterpolation::hold);
    menu.addItem(3, "Segment to next: Linear (ramp)", true,
                 p.interpolationToNext == MidiCcInterpolation::linear);

    juce::Component::SafePointer<ExperimentalPianoRollView> st(this);
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this),
                       [st, pointIndex](const int r) {
                           if (st == nullptr || r == 0)
                           {
                               return;
                           }
                           if (r == 1)
                           {
                               st->deleteSelectedCcPoints();
                               return;
                           }
                           const MidiCcInterpolation shape
                               = r == 2 ? MidiCcInterpolation::hold : MidiCcInterpolation::linear;
                           // Interpolation applies to every selected point (one undoable edit).
                           std::vector<int> indices(st->selectedCcPointIndices_.begin(),
                                                    st->selectedCcPointIndices_.end());
                           if (indices.empty())
                           {
                               indices.push_back(pointIndex);
                           }
                           const auto apply = [st, indices, shape]() -> bool {
                               if (st == nullptr)
                               {
                                   return false;
                               }
                               for (const int i : indices)
                               {
                                   if (i >= 0 && i < (int)st->pattern_.ccPoints.size())
                                   {
                                       st->pattern_.ccPoints[(size_t)i].interpolationToNext = shape;
                                   }
                               }
                               if (st->instrumentTrackController_ != nullptr
                                   && st->timelineClip_ != nullptr)
                               {
                                   st->instrumentTrackController_->notifyClipPatternMutated(
                                       st->timelineClip_->id);
                               }
                               st->repaint();
                               return true;
                           };
                           if (st->undoablePatternEditHandler_ != nullptr
                               && st->instrumentTrackController_ != nullptr)
                           {
                               st->undoablePatternEditHandler_("Set CC segment shape", apply);
                           }
                           else
                           {
                               apply();
                           }
                       });
}

void ExperimentalPianoRollView::showCcControllerMenu()
{
    juce::PopupMenu menu;
    menu.addSectionHeader("Controller lane");

    // Controllers already present in this clip first — the user's own data is one click away.
    {
        std::vector<int> present;
        for (const auto& p : pattern_.ccPoints)
        {
            if (std::find(present.begin(), present.end(), (int)p.controller) == present.end())
            {
                present.push_back((int)p.controller);
            }
        }
        std::sort(present.begin(), present.end());
        for (const int c : present)
        {
            menu.addItem(1000 + c, "In clip: " + midi_cc::controllerDisplayName(c), true,
                         c == ccLaneController_);
        }
        if (!present.empty())
        {
            menu.addSeparator();
        }
    }

    for (const int c : { 1, 2, 4, 7, 10, 11, 64, 65, 66, 67, 91, 93 })
    {
        menu.addItem(1000 + c, midi_cc::controllerDisplayName(c), true, c == ccLaneController_);
    }
    menu.addSeparator();
    for (int base = 0; base < 128; base += 32)
    {
        juce::PopupMenu sub;
        for (int c = base; c < base + 32; ++c)
        {
            sub.addItem(1000 + c, midi_cc::controllerDisplayName(c), true, c == ccLaneController_);
        }
        menu.addSubMenu("CC" + juce::String(base) + " - CC" + juce::String(base + 31), sub);
    }

    juce::Component::SafePointer<ExperimentalPianoRollView> st(this);
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this), [st](const int r) {
        if (st == nullptr || r < 1000 || r > 1127)
        {
            return;
        }
        st->ccLaneController_ = r - 1000;
        st->selectedCcPointIndices_.clear();
        st->repaint();
    });
}

void ExperimentalPianoRollView::paintCcLane(juce::Graphics& g)
{
    // Collapsed: labeled restore knob (discoverability; mirrors the velocity lane's knob).
    {
        const auto knob = ccLaneCollapsedKnobBounds();
        if (!knob.isEmpty())
        {
            g.setColour(juce::Colour(0xff3a4048));
            g.fillRoundedRectangle(knob.toFloat(), 3.0f);
            g.setColour(juce::Colours::white.withAlpha(0.55f));
            g.drawRoundedRectangle(knob.toFloat(), 3.0f, 1.0f);
            g.setFont(juce::Font(juce::FontOptions().withHeight(7.0f)));
            g.drawText("CC", knob, juce::Justification::centred, false);
        }
    }

    const auto lane = ccLaneBounds();
    const auto header = ccLaneHeaderBounds();
    if (ccLaneTotalHeight() <= 0)
    {
        return;
    }

    g.setColour(juce::Colour(0xff20242a));
    g.fillRect(header);
    g.fillRect(lane);
    g.setColour(juce::Colours::white.withAlpha(0.16f));
    g.drawHorizontalLine(lane.getY(), (float)header.getX(), (float)lane.getRight());

    // Header: current controller (click = selector) + selected point value readout.
    if (header.getWidth() >= 46)
    {
        g.setColour(juce::Colours::white.withAlpha(0.75f));
        g.setFont(juce::Font(juce::FontOptions().withHeight(11.0f)));
        g.drawText(midi_cc::controllerDisplayName(ccLaneController_) + juce::String::fromUTF8(" \xe2\x96\xbe"),
                   header.reduced(5, 4), juce::Justification::topLeft, true);
        normalizeCcSelection();
        if (selectedCcPointIndices_.size() == 1)
        {
            const int idx = *selectedCcPointIndices_.begin();
            const auto& p = pattern_.ccPoints[(size_t)idx];
            g.setColour(juce::Colours::white.withAlpha(0.55f));
            g.drawText("value " + juce::String((int)p.value) + "  ch " + juce::String((int)p.channel),
                       header.reduced(5, 4).withTrimmedTop(14), juce::Justification::topLeft, true);
        }
        else if (selectedCcPointIndices_.size() > 1)
        {
            g.setColour(juce::Colours::white.withAlpha(0.55f));
            g.drawText(juce::String((int)selectedCcPointIndices_.size()) + " points",
                       header.reduced(5, 4).withTrimmedTop(14), juce::Justification::topLeft, true);
        }
    }

    if (lane.isEmpty())
    {
        return;
    }
    if (!ccLaneEditingAvailable())
    {
        g.setColour(juce::Colours::white.withAlpha(0.3f));
        g.setFont(juce::Font(juce::FontOptions().withHeight(11.0f)));
        g.drawText("CC editing is available for timeline MIDI clips", lane,
                   juce::Justification::centred, true);
        return;
    }

    // Clip-span shading, matching the velocity lane / grid trim hint.
    {
        const std::int64_t vis0 = timelineClip_->startSamples;
        const std::int64_t vis1 = vis0 + juce::jmax(std::int64_t{ 1 }, timelineClip_->lengthSamples);
        float xL = xForSessionSample(vis0);
        float xR = xForSessionSample(vis1);
        if (xR < xL)
        {
            std::swap(xL, xR);
        }
        const float gx0 = (float)lane.getX();
        const float gx1 = (float)lane.getRight();
        const float bandL = juce::jlimit(gx0, gx1, xL);
        const float bandR = juce::jlimit(gx0, gx1, xR);
        g.setColour(juce::Colours::black.withAlpha(0.36f));
        if (bandL > gx0 + 0.5f)
        {
            g.fillRect(gx0, (float)lane.getY(), bandL - gx0, (float)lane.getHeight());
        }
        if (gx1 > bandR + 0.5f)
        {
            g.fillRect(bandR, (float)lane.getY(), gx1 - bandR, (float)lane.getHeight());
        }
    }

    const auto inner = ccLaneInnerBounds();
    if (inner.getHeight() <= 0)
    {
        return;
    }

    // Value reference lines 0 / 64 / 127.
    g.setColour(juce::Colours::white.withAlpha(0.10f));
    for (const int v : { 0, 64, 127 })
    {
        const float y = ccLaneYForValue(v);
        g.drawHorizontalLine((int)y, (float)inner.getX(), (float)inner.getRight());
    }

    // Segments + points of the shown controller, drawn per native-channel stream. A distinct
    // accent color separates automation from note-velocity bars.
    const juce::Colour ccColour(0xff4fc3f7);
    for (const auto& key : midi_cc::distinctStreams(pattern_.ccPoints))
    {
        if (key.controller != ccLaneController_)
        {
            continue;
        }
        const MidiCcPoint* prev = nullptr;
        float prevX = 0.0f;
        float prevY = 0.0f;
        for (int i = 0; i < (int)pattern_.ccPoints.size(); ++i)
        {
            const auto& p = pattern_.ccPoints[(size_t)i];
            if ((int)p.controller != key.controller || (int)p.channel != key.channel)
            {
                continue;
            }
            const auto xOpt = ccPointXForIndex(i);
            if (!xOpt)
            {
                continue;
            }
            const float x = *xOpt;
            const float y = ccLaneYForValue((int)p.value);
            if (prev != nullptr)
            {
                g.setColour(ccColour.withAlpha(0.8f));
                if (prev->interpolationToNext == MidiCcInterpolation::linear)
                {
                    g.drawLine(prevX, prevY, x, y, 1.6f);
                }
                else
                {
                    g.drawLine(prevX, prevY, x, prevY, 1.6f); // hold: flat …
                    g.drawLine(x, prevY, x, y, 1.6f);         // … then step at the next point
                }
            }
            prev = &p;
            prevX = x;
            prevY = y;
        }
        // Held value after the final point: draw the tail to the lane's right edge.
        if (prev != nullptr && prevX < (float)inner.getRight())
        {
            g.setColour(ccColour.withAlpha(0.45f));
            g.drawLine(prevX, prevY, (float)inner.getRight(), prevY, 1.2f);
        }
    }

    for (int i = 0; i < (int)pattern_.ccPoints.size(); ++i)
    {
        const auto xOpt = ccPointXForIndex(i);
        if (!xOpt)
        {
            continue;
        }
        const float x = *xOpt;
        const float y = ccLaneYForValue((int)pattern_.ccPoints[(size_t)i].value);
        const bool sel = selectedCcPointIndices_.count(i) > 0;
        const float r = sel ? 5.0f : 3.5f;
        g.setColour(sel ? juce::Colours::white : ccColour);
        g.fillEllipse(x - r, y - r, r * 2.0f, r * 2.0f);
        if (sel)
        {
            g.setColour(ccColour);
            g.drawEllipse(x - r, y - r, r * 2.0f, r * 2.0f, 1.4f);
        }
    }

    // Numeric readout for the grabbed point while dragging.
    if (ccPointDragActive_ && ccDragPrimaryIndex_ >= 0
        && ccDragPrimaryIndex_ < (int)pattern_.ccPoints.size())
    {
        if (const auto xOpt = ccPointXForIndex(ccDragPrimaryIndex_))
        {
            const int v = (int)pattern_.ccPoints[(size_t)ccDragPrimaryIndex_].value;
            const juce::Rectangle<float> textR(*xOpt - 16.0f, (float)lane.getY() + 1.0f, 32.0f, 12.0f);
            g.setColour(juce::Colours::white.withAlpha(0.92f));
            g.setFont(juce::Font(juce::FontOptions().withHeight(11.0f)));
            g.drawText(juce::String(v), textR, juce::Justification::centred, false);
        }
    }
}

juce::Rectangle<int> ExperimentalPianoRollView::velocityLaneInnerBounds() const
{
    auto inner = velocityLaneBounds();
    inner.removeFromTop(6);
    inner.removeFromBottom(4);
    return inner;
}

std::int64_t ExperimentalPianoRollView::visibleEndSamples() const noexcept
{
    const auto gr = gridBounds();
    const double w = juce::jmax(1.0, (double)juce::jmax(1, gr.getWidth()));
    if (samplesPerPixel_ <= 0.0 || !std::isfinite(samplesPerPixel_))
    {
        return visibleStartSamples_;
    }
    return visibleStartSamples_ + (std::int64_t)std::llround(w * samplesPerPixel_);
}

void ExperimentalPianoRollView::setSessionTimelineContext(InstrumentMidiClip* timelineClip,
                                                          Session* session,
                                                          Transport* transport,
                                                          juce::AudioDeviceManager* deviceManager,
                                                          InstrumentTrackController* trackController,
                                                          const TimelineViewportModel* mainTimelineViewport) noexcept
{
    const bool changed = timelineClip_ != timelineClip || session_ != session || transport_ != transport
                         || deviceManager_ != deviceManager || instrumentTrackController_ != trackController
                         || mainTimelineViewport_ != mainTimelineViewport;
    if (changed)
    {
        dismissRowLabelEditor(false);
        dismissVelocityValueEditor(false);
        selectedTimelineNoteIndices_.clear();
        timelineNoteResizeActive_ = false;
        timelineResizeNoteIndex_ = -1;
        timelineMovePending_ = false;
        timelineMovePrimaryIndex_ = -1;
        timelineNoteMoveActive_ = false;
        timelineMoveCaptures_.clear();
        timelineInternalClipboard_.clear();
        velocityLaneDragActive_ = false;
        velocityDragCaptures_.clear();
        velocityDragPrimaryIndex_ = -1;
        velocityLaneResizeActive_ = false;
        velocityLaneResizeFromCollapsedKnob_ = false;
        velocityDragAuditionSameStart_ = false;
        ccPointDragActive_ = false;
        ccDragCaptures_.clear();
        ccDragPrimaryIndex_ = -1;
        ccLaneResizeActive_ = false;
        ccLaneResizeFromCollapsedKnob_ = false;
        selectedCcPointIndices_.clear();
        middlePanActive_ = false;
    }
    timelineClip_ = timelineClip;
    session_ = session;
    transport_ = transport;
    deviceManager_ = deviceManager;
    instrumentTrackController_ = trackController;
    mainTimelineViewport_ = mainTimelineViewport;
    boundClipIdForSafety_
        = (timelineClip != nullptr) ? static_cast<std::uint64_t>(timelineClip->id) : std::uint64_t{0};
    if (changed && useAbsoluteTimeline())
    {
        applyViewportAfterContextBound();
    }
    sessionTransportSnapshotValid_ = false;
    uiRulerSeekDisplayHold_.reset();
    clipGeometrySnapshotValid_ = false;
    lastObservedTimelineNoteCountUi_ = -1;
    if (transport_ != nullptr && timelineClip_ != nullptr && session_ != nullptr)
    {
        const std::int64_t ph = transport_->readPlayheadSamplesForUi();
        const double wall = juce::Time::getMillisecondCounterHiRes() * 0.001;
        uiPlayheadDisplaySamples_ = (double)ph;
        uiPlayheadExtrapBaseSample_ = (double)ph;
        uiPlayheadExtrapWallSec_ = wall;
        uiPlayheadLastRawPh_ = ph;
        lastOffscreenGatePlayheadInView_ = true;
    }
    repaint();
}

bool ExperimentalPianoRollView::isTimelineClipBindingFresh() const noexcept
{
    if (!useAbsoluteTimeline())
    {
        return true;
    }
    if (instrumentTrackController_ == nullptr || boundClipIdForSafety_ == 0 || timelineClip_ == nullptr)
    {
        return false;
    }
    InstrumentMidiClip* const c = instrumentTrackController_->getClipById(
        static_cast<InstrumentMidiClipId>(boundClipIdForSafety_));
    return c != nullptr && c == timelineClip_;
}

void ExperimentalPianoRollView::setFollowPlayheadEnabled(const bool on) noexcept
{
    if (followPlayhead_ == on)
    {
        return;
    }
    followPlayhead_ = on;
    syncViewportToBoundClip();
    repaint();
}

void ExperimentalPianoRollView::seedOrResetViewport()
{
    if (timelineClip_ != nullptr)
    {
        const bool canWriteClipFields = !useAbsoluteTimeline() || isTimelineClipBindingFresh();
        if (canWriteClipFields)
        {
            timelineClip_->midiRollVisibleStartSamples = 0;
            timelineClip_->midiRollSamplesPerPixel = 0.0;
            timelineClip_->midiRollFollowEnabled = true;
        }
    }
    if (useAbsoluteTimeline())
    {
        seedViewportFromMainTimelineOrFallback();
        syncViewportToBoundClip();
    }
    else
    {
        visibleStartSamples_ = 0;
        samplesPerPixel_ = 0.0;
    }
    sessionTransportSnapshotValid_ = false;
    uiRulerSeekDisplayHold_.reset();
    clipGeometrySnapshotValid_ = false;
    lastObservedTimelineNoteCountUi_ = -1;
    repaint();
}

void ExperimentalPianoRollView::setViewportState(
    const std::int64_t visibleStartSamples, const double samplesPerPixel) noexcept
{
    if (!useAbsoluteTimeline())
    {
        return;
    }
    if (samplesPerPixel > 0.0 && std::isfinite(samplesPerPixel))
    {
        visibleStartSamples_ = juce::jmax(std::int64_t{0}, visibleStartSamples);
        samplesPerPixel_ = samplesPerPixel;
    }
}

bool ExperimentalPianoRollView::hasValidViewportState() const noexcept
{
    return samplesPerPixel_ > 0.0 && std::isfinite(samplesPerPixel_);
}

void ExperimentalPianoRollView::resetUiPlayheadAnchorToSample(const std::int64_t targetSample) noexcept
{
    syncUiPlayheadAfterRulerSeek(targetSample);
}

void ExperimentalPianoRollView::syncViewportToBoundClip() noexcept
{
    if (timelineClip_ == nullptr || !useAbsoluteTimeline())
    {
        return;
    }
    if (!isTimelineClipBindingFresh())
    {
        return;
    }
    if (!hasValidViewportState())
    {
        return;
    }
    timelineClip_->midiRollVisibleStartSamples = visibleStartSamples_;
    timelineClip_->midiRollSamplesPerPixel = samplesPerPixel_;
    timelineClip_->midiRollFollowEnabled = followPlayhead_;
}

bool ExperimentalPianoRollView::useAbsoluteTimeline() const noexcept
{
    return timelineClip_ != nullptr && session_ != nullptr && transport_ != nullptr;
}

void ExperimentalPianoRollView::seedViewportFromMainTimelineOrFallback()
{
    if (!useAbsoluteTimeline())
    {
        return;
    }
    if (timelineClip_ != nullptr && !isTimelineClipBindingFresh())
    {
        return;
    }

    const auto gr = gridBounds();
    const int gw = juce::jmax(1, gr.getWidth());
    if (gw < 32)
    {
        samplesPerPixel_ = 0.0;
        return;
    }

    const double w = (double)gw;
    const double sr = effectiveDeviceSampleRate(deviceManager_);

    const ProjectMusicalTime mt = sanitizeProjectMusicalTime(session_->getProjectMusicalTime());
    const double bpm = mt.bpm;
    const int num = mt.numerator;
    const int den = juce::jmax(1, mt.denominator);
    const double quartersPerBar = 4.0 * (double)num / (double)den;
    const double samplesPerBar = sr * (60.0 / bpm) * quartersPerBar;

    constexpr double kMidiRollDefaultVisibleBars = 5.0;
    const double targetVisibleSamples = samplesPerBar * kMidiRollDefaultVisibleBars;
    double spp = targetVisibleSamples / w;
    spp = juce::jlimit(0.25, 1.0e7, spp);
    samplesPerPixel_ = spp;

    const std::int64_t visibleLen = (std::int64_t)std::llround(w * samplesPerPixel_);
    std::int64_t visStart = 0;

    if (timelineClip_ != nullptr && transport_ != nullptr)
    {
        const std::int64_t ph = transport_->readPlayheadSamplesForUi();
        const std::int64_t c0 = timelineClip_->startSamples;
        const std::int64_t c1 = c0 + juce::jmax(std::int64_t{1}, timelineClip_->lengthSamples);
        const std::int64_t margin = juce::jmax(std::int64_t{1}, visibleLen / 10);
        const bool phNearClip = ph >= c0 - margin && ph <= c1 + margin;
        if (phNearClip)
        {
            visStart = ph - (std::int64_t)std::llround(0.25 * (double)visibleLen);
        }
        else
        {
            visStart = c0 - (std::int64_t)std::llround(0.1 * (double)visibleLen);
        }
    }
    else if (timelineClip_ != nullptr)
    {
        visStart = timelineClip_->startSamples - (std::int64_t)std::llround(0.1 * (double)visibleLen);
    }

    visibleStartSamples_ = juce::jmax(std::int64_t{0}, visStart);
}

void ExperimentalPianoRollView::applyViewportAfterContextBound()
{
    if (!useAbsoluteTimeline() || timelineClip_ == nullptr)
    {
        return;
    }

    if (timelineClip_->midiRollSamplesPerPixel > 0.0 && std::isfinite(timelineClip_->midiRollSamplesPerPixel))
    {
        visibleStartSamples_ = juce::jmax(std::int64_t{0}, timelineClip_->midiRollVisibleStartSamples);
        samplesPerPixel_ = timelineClip_->midiRollSamplesPerPixel;
        followPlayhead_ = timelineClip_->midiRollFollowEnabled;
        return;
    }

    seedViewportFromMainTimelineOrFallback();
    syncViewportToBoundClip();
}

std::int64_t ExperimentalPianoRollView::sampleAtGridX(const float localX) const noexcept
{
    const auto gr = gridBounds();
    const float ox = (float)gr.getX();
    const double rel = (double)(localX - ox);
    if (samplesPerPixel_ <= 0.0 || !std::isfinite(samplesPerPixel_))
    {
        return visibleStartSamples_;
    }
    return visibleStartSamples_ + (std::int64_t)std::llround(rel * samplesPerPixel_);
}

float ExperimentalPianoRollView::xForSessionSample(const std::int64_t s) const noexcept
{
    const auto gr = gridBounds();
    return TimelineRulerView::sessionSampleToLocalX(
        s, (float)gr.getX(), visibleStartSamples_, samplesPerPixel_);
}

float ExperimentalPianoRollView::xForSessionSampleD(const double s) const noexcept
{
    const auto gr = gridBounds();
    if (samplesPerPixel_ <= 0.0 || !std::isfinite(samplesPerPixel_) || !std::isfinite(s))
    {
        return (float)gr.getX();
    }
    return (float)gr.getX()
           + (float)(((s - (double)visibleStartSamples_) / samplesPerPixel_));
}

double ExperimentalPianoRollView::currentPlayheadDisplaySampleForPaint() const noexcept
{
    double s = uiPlayheadDisplaySamples_;
    if (!std::isfinite(s))
    {
        return 0.0;
    }
    if (session_ != nullptr)
    {
        const double arrLen = (double)juce::jmax(std::int64_t{0}, session_->getArrangementExtentSamples());
        s = juce::jlimit(0.0, juce::jmax(0.0, arrLen), s);
    }
    return juce::jmax(0.0, s);
}

void ExperimentalPianoRollView::repaintPlayheadColumnsOnly(const float newCentreX)
{
    const auto gr = gridBounds();
    const int top = 0;
    const int bottom = juce::jmax(gr.getBottom(), timelineRulerHeight());
    if (bottom <= top)
    {
        return;
    }
    constexpr float kStrokeThicknessPx = 1.5f;
    const float prev = lastPaintedPlayheadCentreX_;
    if (std::isfinite(prev) && (int)std::floor(prev) == (int)std::floor(newCentreX))
    {
        // Same pixel column: nothing would change on screen. Skipping the repaint is what removes
        // the per-tick full-surface redraw that read as flicker.
        return;
    }
    if (std::isfinite(prev))
    {
        repaint(playhead_pixel::dirtyStripe(prev, top, bottom, kStrokeThicknessPx));
    }
    else
    {
        repaint();
        return;
    }
    repaint(playhead_pixel::dirtyStripe(newCentreX, top, bottom, kStrokeThicknessPx));
}

void ExperimentalPianoRollView::timerCallback()
{
    if (forbiddenCursorFlashUntilMs_ > 0.0
        && juce::Time::getMillisecondCounterHiRes() >= forbiddenCursorFlashUntilMs_)
    {
        forbiddenCursorFlashUntilMs_ = 0.0;
        if (!timelineResizeInvalid_ && !timelineMoveInvalid_)
        {
            setMouseCursor(juce::MouseCursor::NormalCursor);
        }
    }
    const double nowSec = juce::Time::getMillisecondCounterHiRes() * 0.001;
    const bool absTime = useAbsoluteTimeline();
    if (absTime && timelineClip_ != nullptr && !isTimelineClipBindingFresh())
    {
        return;
    }
    const bool transportPlaying = absTime && transport_ != nullptr
                                  && transport_->readPlaybackIntentForUi() == PlaybackIntent::Playing;
    const bool wasTransportPlaying = wasTransportPlayingUi_;
    if (transportPlaying)
    {
        // Follow-governor capacity signal: the tick interval after a follow pan includes that
        // pan's full-roll repaint cost. Only sampled while animating (idle rate is 6 Hz by design).
        followGovernor_.noteFrameTick(nowSec * 1000.0);
    }

    bool structuralRepaint = false;
    bool viewportMoved = false;

    if (absTime && transport_ != nullptr)
    {
        const std::int64_t phRaw = transport_->readPlayheadSamplesForUi();
        const std::int64_t prevRawPh = uiPlayheadLastRawPh_;
        const double sr = effectiveDeviceSampleRate(deviceManager_);

        if (uiRulerSeekDisplayHold_.has_value())
        {
            const std::int64_t held = *uiRulerSeekDisplayHold_;
            if (std::llabs(phRaw - held) <= 64)
            {
                uiRulerSeekDisplayHold_.reset();
            }
        }

        if (!uiRulerSeekDisplayHold_.has_value())
        {
            if (transportPlaying && !wasTransportPlaying)
            {
                uiPlayheadExtrapBaseSample_ = (double)phRaw;
                uiPlayheadExtrapWallSec_ = nowSec;
                uiPlayheadLastRawPh_ = phRaw;
                uiPlayheadDisplaySamples_ = (double)phRaw;
                lastOffscreenGatePlayheadInView_ = true;
            }
            else if (!transportPlaying)
            {
                uiPlayheadExtrapBaseSample_ = (double)phRaw;
                uiPlayheadExtrapWallSec_ = nowSec;
                uiPlayheadLastRawPh_ = phRaw;
                uiPlayheadDisplaySamples_ = (double)phRaw;
            }
            else
            {
                const double predicted = uiPlayheadExtrapBaseSample_
                                        + (nowSec - uiPlayheadExtrapWallSec_) * sr;
                const double deltaToRaw = (double)phRaw - predicted;
                if (std::abs(deltaToRaw) > kPlayheadHardResyncSamples)
                {
                    uiPlayheadExtrapBaseSample_ = (double)phRaw;
                    uiPlayheadExtrapWallSec_ = nowSec;
                    uiPlayheadLastRawPh_ = phRaw;
                }
                else if (phRaw != uiPlayheadLastRawPh_)
                {
                    uiPlayheadExtrapBaseSample_ = (double)phRaw;
                    uiPlayheadExtrapWallSec_ = nowSec;
                    uiPlayheadLastRawPh_ = phRaw;
                }
                uiPlayheadDisplaySamples_
                    = uiPlayheadExtrapBaseSample_ + (nowSec - uiPlayheadExtrapWallSec_) * sr;

                const std::int64_t hardThr = (std::int64_t)std::llround(kPlayheadHardResyncSamples);
                if (std::llabs(phRaw - prevRawPh) >= hardThr)
                {
                    uiPlayheadDisplaySamples_ = (double)phRaw;
                    uiPlayheadExtrapBaseSample_ = (double)phRaw;
                    uiPlayheadExtrapWallSec_ = nowSec;
                    uiPlayheadLastRawPh_ = phRaw;
                    lastObservedPlayheadUi_ = phRaw;
                    lastOffscreenGatePlayheadInView_ = true;
                    if (followPlayhead_)
                    {
                        maybeFollowViewportToAnchorSample((double)phRaw);
                    }
                }
            }
        }
        else
        {
            const std::int64_t held = *uiRulerSeekDisplayHold_;
            const double wall = uiPlayheadExtrapWallSec_;
            uiPlayheadLastRawPh_ = phRaw;
            if (!transportPlaying)
            {
                uiPlayheadDisplaySamples_ = (double)held;
            }
            else
            {
                uiPlayheadDisplaySamples_ = (double)held + (nowSec - wall) * sr;
            }
        }

        if (session_ != nullptr)
        {
            const std::int64_t l = session_->getLeftLocatorSamples();
            const std::int64_t r = session_->getRightLocatorSamples();
            const bool cy = transport_->readCycleEnabledForUi();
            const bool firstSnap = !sessionTransportSnapshotValid_;
            const bool locOrCycle = firstSnap || l != lastObservedLocLUi_ || r != lastObservedLocRUi_
                                    || cy != lastObservedCycleUi_;
            const bool playheadMovedRaw = firstSnap || phRaw != lastObservedPlayheadUi_;
            if (firstSnap || playheadMovedRaw || locOrCycle)
            {
                lastObservedPlayheadUi_ = phRaw;
                lastObservedLocLUi_ = l;
                lastObservedLocRUi_ = r;
                lastObservedCycleUi_ = cy;
                sessionTransportSnapshotValid_ = true;
                if (locOrCycle)
                {
                    structuralRepaint = true;
                }
            }
        }

        if (followPlayhead_ && transportPlaying)
        {
            const auto gr = gridBounds();
            const double wpx = juce::jmax(1.0, (double)juce::jmax(1, gr.getWidth()));
            if (samplesPerPixel_ > 0.0 && std::isfinite(samplesPerPixel_))
            {
                const double spanSamples = wpx * samplesPerPixel_;
                const double ph = uiPlayheadDisplaySamples_;
                const double rel
                    = spanSamples > 1e-9 ? (ph - (double)visibleStartSamples_) / spanSamples : 0.5;

                std::int64_t targetStart = visibleStartSamples_;
                bool needScroll = false;
                if (rel >= kFollowRightThreshold)
                {
                    needScroll = true;
                    targetStart = (std::int64_t)std::llround(ph - kFollowForwardResetPosition * spanSamples);
                }
                else if (rel <= kFollowLeftThreshold)
                {
                    needScroll = true;
                    targetStart = (std::int64_t)std::llround(ph - kFollowBackwardResetPosition * spanSamples);
                }

                // Follow hardening: follow is page/event-driven — every page full-repaints the
                // roll, so admission goes through the governor (boundary trigger + re-arm,
                // capacity gates, gesture holdoff) *and* the cross-window coordinator (one page
                // across all DAL windows per interval; yield while the user pans/zooms the main
                // window). A hidden or minimised editor skips pages entirely.
                if (needScroll && !followUiWorthUpdating())
                {
                    needScroll = false;
                    ++statsFollowSkipsHidden_;
                }
                const double nowMs = nowSec * 1000.0;
                if (needScroll)
                {
                    switch (followGovernor_.decidePage(
                        nowMs, ph, (double)visibleStartSamples_, spanSamples))
                    {
                        case FollowAutoscrollGovernor::Decision::apply:
                            break;
                        case FollowAutoscrollGovernor::Decision::skipNotNeeded:
                            needScroll = false;
                            break;
                        case FollowAutoscrollGovernor::Decision::skipUserGestureHoldoff:
                            needScroll = false;
                            ++statsFollowSkipsGesture_;
                            break;
                        case FollowAutoscrollGovernor::Decision::skipLateFrame:
                            needScroll = false;
                            ++statsFollowSkipsLateFrame_;
                            break;
                        case FollowAutoscrollGovernor::Decision::skipAwaitCleanFrame:
                            needScroll = false;
                            ++statsFollowSkipsAwaitClean_;
                            break;
                        case FollowAutoscrollGovernor::Decision::skipBoundaryWait:
                            needScroll = false;
                            ++statsFollowSkipsBoundary_;
                            break;
                        case FollowAutoscrollGovernor::Decision::skipMinInterval:
                            needScroll = false;
                            ++statsFollowSkipsPacing_;
                            break;
                    }
                }
                if (needScroll)
                {
                    auto& global = GlobalFollowWorkCoordinator::instance();
                    if (global.otherWindowGestureActive(this, nowMs))
                    {
                        needScroll = false;
                        ++statsFollowSkipsCrossWindow_;
                    }
                    else if (!global.pageSlotAvailable(nowMs))
                    {
                        needScroll = false;
                        ++statsFollowSkipsGlobalBudget_;
                    }
                }
                if (needScroll)
                {
                    const std::int64_t clamped = juce::jmax(std::int64_t{0}, targetStart);
                    if (clamped != visibleStartSamples_)
                    {
                        visibleStartSamples_ = clamped;
                        viewportMoved = true;
                        structuralRepaint = true;
                        followGovernor_.notePageApplied(nowMs, ph, (double)clamped, spanSamples);
                        GlobalFollowWorkCoordinator::instance().notePageApplied(nowMs);
                        ++statsFollowPans_;
                    }
                    else
                    {
                        // Page could not move the viewport (clamp): drop to the sparse re-arm
                        // cadence instead of re-deciding every minimum interval.
                        followGovernor_.notePageApplied(
                            nowMs, ph, (double)visibleStartSamples_, spanSamples);
                    }
                }
            }
        }
    }
    maybeLogFollowDiagnostics(nowSec * 1000.0, transportPlaying);

    if (absTime && timelineClip_ != nullptr && isTimelineClipBindingFresh()
        && (!clipGeometrySnapshotValid_
            || timelineClip_->startSamples != lastObservedClipStartSamplesUi_
            || timelineClip_->lengthSamples != lastObservedClipLengthSamplesUi_))
    {
        lastObservedClipStartSamplesUi_ = timelineClip_->startSamples;
        lastObservedClipLengthSamplesUi_ = timelineClip_->lengthSamples;
        clipGeometrySnapshotValid_ = true;
        structuralRepaint = true;
    }

    const int tnCount = (int)pattern_.timelineNotes.size();
    if (tnCount != lastObservedTimelineNoteCountUi_)
    {
        lastObservedTimelineNoteCountUi_ = tnCount;
        structuralRepaint = true;
    }

    if (viewportMoved)
    {
        syncViewportToBoundClip();
    }

    wasTransportPlayingUi_ = transportPlaying;

    if (structuralRepaint || viewportMoved)
    {
        lastOffscreenGatePlayheadInView_ = true;
        repaint();
    }
    else if (wasTransportPlaying && !transportPlaying)
    {
        lastOffscreenGatePlayheadInView_ = true;
        repaint();
    }
    else if (transportPlaying)
    {
        const auto gr = gridBounds();
        constexpr float kMarginPx = 24.0f;
        const float x = playhead_pixel::snapToPixelCentre(
            xForSessionSampleD(currentPlayheadDisplaySampleForPaint()));
        const bool nowNear = x >= (float)gr.getX() - kMarginPx && x <= (float)gr.getRight() + kMarginPx;
        const bool wasNear = lastOffscreenGatePlayheadInView_;
        if (!nowNear && !wasNear)
        {
            lastOffscreenGatePlayheadInView_ = false;
        }
        else
        {
            // Playhead-only frame: two narrow columns, not the whole roll.
            repaintPlayheadColumnsOnly(x);
            lastOffscreenGatePlayheadInView_ = nowNear;
        }
    }

    const int wantHz = transportPlaying ? kMidiRollTimerHzAnimating : kMidiRollTimerHzIdle;
    if (wantHz != uiTimerHzConfigured_)
    {
        uiTimerHzConfigured_ = wantHz;
        startTimerHz(wantHz);
    }
}

void ExperimentalPianoRollView::resized()
{
    Component::resized();
    if (useAbsoluteTimeline() && timelineClip_ != nullptr && isTimelineClipBindingFresh()
        && timelineClip_->midiRollSamplesPerPixel <= 0.0 && !hasValidViewportState())
    {
        seedViewportFromMainTimelineOrFallback();
        if (hasValidViewportState())
        {
            syncViewportToBoundClip();
        }
        repaint();
    }
    clampPitchScrollOffset();
    {
        // Scrollbar strip: same vertical extent as the note grid, flush against the right edge.
        auto strip = getLocalBounds();
        strip.removeFromTop(timelineRulerHeight());
        strip.removeFromBottom(velocityLaneTotalHeight());
        pitchScrollbar_.setBounds(strip.removeFromRight(kPitchScrollbarWidthPx));
        syncPitchScrollbarFromState();
    }
    if (rowLabelEditor_ != nullptr && rowLabelEditorPitch_ >= pitchLow_ && rowLabelEditorPitch_ <= pitchHigh_)
    {
        const auto kb = keyboardBounds();
        const int topP = topVisiblePitch();
        const int rowFromTop = topP - rowLabelEditorPitch_;
        const int nVis = countVisiblePitchRows();
        if (rowFromTop < 0 || rowFromTop >= nVis)
        {
            dismissRowLabelEditor(false);
        }
        else
        {
            const int y = kb.getY() + rowFromTop * kRowHeight;
            rowLabelEditor_->setBounds(kb.withY(y).withHeight(kRowHeight).reduced(1, 1));
            rowLabelEditor_->toFront(false);
        }
    }
    if (velocityValueEditor_ != nullptr)
    {
        // Layout changed under the popup (lane resize, window resize): cancel instead of drifting.
        dismissVelocityValueEditor(false);
    }
}

int ExperimentalPianoRollView::pitchAtY(const int y) const
{
    const auto gr = gridBounds();
    const int relY = y - gr.getY();
    const int row = relY / kRowHeight;
    const int maxRow = juce::jmax(0, countVisiblePitchRows() - 1);
    const int clampedRow = juce::jlimit(0, maxRow, row);
    return topVisiblePitch() - clampedRow;
}

int ExperimentalPianoRollView::countVisiblePitchRows() const noexcept
{
    const auto gr = gridBounds();
    const int h = juce::jmax(0, gr.getHeight());
    const int n = h / kRowHeight;
    return juce::jmax(1, n);
}

int ExperimentalPianoRollView::maxPitchScrollOffsetRows() const noexcept
{
    const int total = pitchHigh_ - pitchLow_ + 1;
    const int vis = countVisiblePitchRows();
    return juce::jmax(0, total - vis);
}

void ExperimentalPianoRollView::clampPitchScrollOffset() noexcept
{
    pitchScrollOffsetRows_ = juce::jlimit(0, maxPitchScrollOffsetRows(), pitchScrollOffsetRows_);
}

void ExperimentalPianoRollView::syncPitchScrollbarFromState()
{
    const juce::ScopedValueSetter<bool> guard(pitchScrollbarSyncing_, true);
    const int totalRows = juce::jmax(1, pitchHigh_ - pitchLow_ + 1);
    pitchScrollbar_.setRangeLimits({0.0, (double)totalRows}, juce::dontSendNotification);
    pitchScrollbar_.setCurrentRange((double)pitchScrollOffsetRows_,
                                    (double)juce::jmin(totalRows, countVisiblePitchRows()),
                                    juce::dontSendNotification);
}

void ExperimentalPianoRollView::scrollBarMoved(juce::ScrollBar* bar, const double newRangeStart)
{
    if (bar != &pitchScrollbar_ || pitchScrollbarSyncing_)
    {
        return;
    }
    const int newOffset = (int)std::llround(newRangeStart);
    if (newOffset == pitchScrollOffsetRows_)
    {
        return;
    }
    pitchScrollOffsetRows_ = newOffset;
    pitchWheelScrollRemainder_ = 0.0f;
    clampPitchScrollOffset();
    resized();
    repaint();
}

int ExperimentalPianoRollView::topVisiblePitch() const noexcept
{
    return pitchHigh_ - pitchScrollOffsetRows_;
}

int ExperimentalPianoRollView::topVisibleMidiPitch() const noexcept
{
    return topVisiblePitch();
}

void ExperimentalPianoRollView::restoreVerticalPitchScrollToPriorTopPitch(const int previousTopVisibleMidiPitch) noexcept
{
    pitchScrollOffsetRows_ = pitchHigh_ - previousTopVisibleMidiPitch;
    clampPitchScrollOffset();
    syncPitchScrollbarFromState();
    repaint();
}

void ExperimentalPianoRollView::seedDefaultVerticalScroll(const int fallbackCenterPitch) noexcept
{
    // Reveal existing content (centre of the clip's pitch span); an empty editor starts at a
    // musically useful register instead of pinned to G8 (spec B, "scrolling and initial view").
    int centerPitch = juce::jlimit(pitchLow_, pitchHigh_, fallbackCenterPitch);
    if (!pattern_.timelineNotes.empty())
    {
        int lo = 127;
        int hi = 0;
        for (const auto& tn : pattern_.timelineNotes)
        {
            lo = juce::jmin(lo, tn.midiNote);
            hi = juce::jmax(hi, tn.midiNote);
        }
        centerPitch = juce::jlimit(pitchLow_, pitchHigh_, (lo + hi) / 2);
    }
    const int topPitch = centerPitch + countVisiblePitchRows() / 2;
    pitchScrollOffsetRows_ = pitchHigh_ - topPitch;
    clampPitchScrollOffset();
    syncPitchScrollbarFromState();
    repaint();
}

int ExperimentalPianoRollView::preferredComponentHeightForPitchRows(const int rows) const noexcept
{
    return timelineRulerHeight() + juce::jmax(1, rows) * kRowHeight + juce::jmax(0, velocityLaneHeightPref_)
           + juce::jmax(0, ccLaneHeightPref_);
}

void ExperimentalPianoRollView::setVelocityLaneHeightPreference(const int heightPx) noexcept
{
    // Same snap-to-minimized policy as the interactive resize. No upper clamp here: the component
    // may not be laid out yet at restore time; `velocityLaneTotalHeight()` clamps at use time.
    int h = heightPx;
    if (h < kVelocityLaneMinUsableHeight)
    {
        h = 0;
    }
    velocityLaneHeightPref_ = juce::jmax(0, h);
    clampPitchScrollOffset();
    resized();
    repaint();
}

std::optional<juce::Rectangle<int>> ExperimentalPianoRollView::visibleRowStripRect(
    const juce::Rectangle<int>& strip, const int midiNote) const noexcept
{
    if (midiNote < pitchLow_ || midiNote > pitchHigh_)
    {
        return std::nullopt;
    }
    const int rowIndexFull = pitchHigh_ - midiNote;
    const int visRow = rowIndexFull - pitchScrollOffsetRows_;
    const int nVis = countVisiblePitchRows();
    if (visRow < 0 || visRow >= nVis)
    {
        return std::nullopt;
    }
    const int y = strip.getY() + visRow * kRowHeight;
    return strip.withY(y).withHeight(kRowHeight);
}

void ExperimentalPianoRollView::setMusicalSnapComboId(const int /*id*/) noexcept
{
    repaint();
}

void ExperimentalPianoRollView::setTimelineNotesDisplayComboId(const int id) noexcept
{
    timelineNotesDisplayComboId_ = juce::jlimit(1, 2, id);
    repaint();
}

void ExperimentalPianoRollView::setUndoablePatternEditHandler(
    std::function<void(const juce::String&, std::function<bool()>)> handler) noexcept
{
    undoablePatternEditHandler_ = std::move(handler);
}

std::int64_t ExperimentalPianoRollView::musicalSnapGridTicks() const noexcept
{
    if (!useAbsoluteTimeline() || timelineClip_ == nullptr || session_ == nullptr || deviceManager_ == nullptr)
    {
        return 0;
    }
    const SnapSettings snap = session_->getArrangementSnapSettings();
    if (!snap.enabled)
    {
        return 0;
    }

    const ProjectMusicalTime mt = session_->getProjectMusicalTime();
    const double stepBeats = arrangementSnapGridStepBeats(snap.resolution, mt);
    const double sr = effectiveDeviceSampleRate(deviceManager_);
    const double spb = samplesPerBeat(mt, sr);
    if (!std::isfinite(stepBeats) || stepBeats <= 0.0 || !std::isfinite(spb) || spb <= 0.0)
    {
        return 0;
    }
    const std::int64_t stepSamples = (std::int64_t)std::llround(stepBeats * spb);
    if (stepSamples <= 0)
    {
        return 0;
    }

    const double bpm = pattern_.bpm > 0.0 ? pattern_.bpm : 120.0;
    const int tpq = experimentalEffectiveTicksPerQuarter(pattern_);
    return juce::jmax<std::int64_t>(1, relativeSamplesToTicks(stepSamples, bpm, tpq, sr));
}

std::int64_t ExperimentalPianoRollView::referenceTimelineGridTicks() const noexcept
{
    const int tpq = experimentalEffectiveTicksPerQuarter(pattern_);
    const std::int64_t snap = musicalSnapGridTicks();
    if (snap > 0)
    {
        return snap;
    }
    return juce::jmax<std::int64_t>(1, (std::int64_t)(tpq / 4));
}

std::optional<int> ExperimentalPianoRollView::findTimelineNoteIndexAtPoint(juce::Point<int> pos) const
{
    if (timelineClip_ == nullptr || !useAbsoluteTimeline())
    {
        return std::nullopt;
    }
    if (samplesPerPixel_ <= 0.0 || !std::isfinite(samplesPerPixel_))
    {
        return std::nullopt;
    }
    if (!isTimelineClipBindingFresh())
    {
        return std::nullopt;
    }

    const auto gr = gridBounds();
    const double sr = effectiveDeviceSampleRate(deviceManager_);
    const double bpm = pattern_.bpm > 0.0 ? pattern_.bpm : 120.0;
    const int tpq = experimentalEffectiveTicksPerQuarter(pattern_);
    const bool paintBars = (timelineNotesDisplayComboId_ == 2);
    const bool pianoRowMode = (rowLabelMode_ == 1);
    const float hitHalfW = pianoRowMode ? juce::jmax(4.0f, juce::jmin(6.8f, (float)kRowHeight * 0.48f))
                                        : juce::jmax(3.5f, juce::jmin(6.0f, (float)kRowHeight * 0.42f));
    const float hitHalfH = pianoRowMode ? juce::jmax(4.0f, (float)kRowHeight * 0.45f)
                                        : juce::jmax(3.5f, (float)kRowHeight * 0.4f);

    const float px = (float)pos.x;
    const float py = (float)pos.y;

    for (int i = (int)pattern_.timelineNotes.size() - 1; i >= 0; --i)
    {
        const auto& tn = pattern_.timelineNotes[(size_t)i];
        if (tn.midiNote < pitchLow_ || tn.midiNote > pitchHigh_)
        {
            continue;
        }
        const auto rrOpt = visibleRowStripRect(gr, tn.midiNote);
        if (!rrOpt)
        {
            continue;
        }
        const auto& rr = *rrOpt;
        const std::int64_t a0 = absoluteSampleForTimelineNote(timelineClip_->timelineAnchorSamples, tn, pattern_, sr);
        const std::int64_t vis0 = timelineClip_->startSamples;
        const std::int64_t vis1 = vis0 + juce::jmax(std::int64_t{1}, timelineClip_->lengthSamples);
        if (a0 < vis0 || a0 >= vis1)
        {
            continue;
        }

        if (paintBars)
        {
            const std::int64_t durS = ticksToRelativeSamples(
                juce::jmax<std::int64_t>(1, tn.durationTicks), bpm, tpq, sr);
            const std::int64_t a1 = a0 + juce::jmax<std::int64_t>(1, durS);
            float xL = xForSessionSample(a0);
            float xR = xForSessionSample(a1);
            if (xR < xL)
            {
                std::swap(xL, xR);
            }
            xL = juce::jmax(xL, (float)gr.getX());
            xR = juce::jmin(xR, (float)gr.getRight());
            if (xR <= (float)gr.getX() || xL >= (float)gr.getRight())
            {
                continue;
            }
            const float notePadY = pianoRowMode ? 1.0f : 2.0f;
            const float noteInsetV = pianoRowMode ? 2.0f : 4.0f;
            auto noteRect = juce::Rectangle<float>(
                xL, (float)rr.getY() + notePadY, xR - xL, (float)rr.getHeight() - noteInsetV);
            if (noteRect.getWidth() < 3.0f)
            {
                noteRect = noteRect.withSizeKeepingCentre(4.0f, noteRect.getHeight());
            }
            if (noteRect.contains(px, py))
            {
                return i;
            }
        }
        else
        {
            const float cx = xForSessionSample(a0);
            if (cx < (float)gr.getX() - hitHalfW - 2.0f || cx > (float)gr.getRight() + hitHalfW + 2.0f)
            {
                continue;
            }
            const float cy = (float)rr.getCentreY();
            if (pointInTimelineNoteDiamond(cx, cy, hitHalfW, hitHalfH, px, py))
            {
                return i;
            }
        }
    }
    return std::nullopt;
}

std::optional<juce::Rectangle<float>> ExperimentalPianoRollView::getTimelineNoteVisualBounds(const int noteIndex) const
{
    if (timelineClip_ == nullptr || !useAbsoluteTimeline())
    {
        return std::nullopt;
    }
    if (samplesPerPixel_ <= 0.0 || !std::isfinite(samplesPerPixel_))
    {
        return std::nullopt;
    }
    if (!isTimelineClipBindingFresh())
    {
        return std::nullopt;
    }
    if (noteIndex < 0 || noteIndex >= (int)pattern_.timelineNotes.size())
    {
        return std::nullopt;
    }

    const auto gr = gridBounds();
    const double sr = effectiveDeviceSampleRate(deviceManager_);
    const double bpm = pattern_.bpm > 0.0 ? pattern_.bpm : 120.0;
    const int tpq = experimentalEffectiveTicksPerQuarter(pattern_);
    const bool paintBars = (timelineNotesDisplayComboId_ == 2);
    const bool pianoRowMode = (rowLabelMode_ == 1);
    const float hitHalfW = pianoRowMode ? juce::jmax(4.0f, juce::jmin(6.8f, (float)kRowHeight * 0.48f))
                                        : juce::jmax(3.5f, juce::jmin(6.0f, (float)kRowHeight * 0.42f));
    const float hitHalfH = pianoRowMode ? juce::jmax(4.0f, (float)kRowHeight * 0.45f)
                                        : juce::jmax(3.5f, (float)kRowHeight * 0.4f);

    const auto& tn = pattern_.timelineNotes[(size_t)noteIndex];
    if (tn.midiNote < pitchLow_ || tn.midiNote > pitchHigh_)
    {
        return std::nullopt;
    }
    const auto rrOpt = visibleRowStripRect(gr, tn.midiNote);
    if (!rrOpt)
    {
        return std::nullopt;
    }
    const auto& rr = *rrOpt;
    const std::int64_t a0 = absoluteSampleForTimelineNote(timelineClip_->timelineAnchorSamples, tn, pattern_, sr);
    const std::int64_t vis0 = timelineClip_->startSamples;
    const std::int64_t vis1 = vis0 + juce::jmax(std::int64_t{1}, timelineClip_->lengthSamples);
    if (a0 < vis0 || a0 >= vis1)
    {
        return std::nullopt;
    }

    if (paintBars)
    {
        const std::int64_t durS = ticksToRelativeSamples(
            juce::jmax<std::int64_t>(1, tn.durationTicks), bpm, tpq, sr);
        const std::int64_t a1 = a0 + juce::jmax<std::int64_t>(1, durS);
        float xL = xForSessionSample(a0);
        float xR = xForSessionSample(a1);
        if (xR < xL)
        {
            std::swap(xL, xR);
        }
        xL = juce::jmax(xL, (float)gr.getX());
        xR = juce::jmin(xR, (float)gr.getRight());
        if (xR <= (float)gr.getX() || xL >= (float)gr.getRight())
        {
            return std::nullopt;
        }
        const float notePadY = pianoRowMode ? 1.0f : 2.0f;
        const float noteInsetV = pianoRowMode ? 2.0f : 4.0f;
        auto noteRect = juce::Rectangle<float>(
            xL, (float)rr.getY() + notePadY, xR - xL, (float)rr.getHeight() - noteInsetV);
        if (noteRect.getWidth() < 3.0f)
        {
            noteRect = noteRect.withSizeKeepingCentre(4.0f, noteRect.getHeight());
        }
        return noteRect;
    }

    const float cx = xForSessionSample(a0);
    if (cx < (float)gr.getX() - hitHalfW - 2.0f || cx > (float)gr.getRight() + hitHalfW + 2.0f)
    {
        return std::nullopt;
    }
    const float cy = (float)rr.getCentreY();
    return juce::Rectangle<float>(cx - hitHalfW, cy - hitHalfH, 2.f * hitHalfW, 2.f * hitHalfH);
}

bool ExperimentalPianoRollView::timelineBarsResizeEnabled() const noexcept
{
    return useAbsoluteTimeline() && timelineClip_ != nullptr
        && isTimelineClipBindingFresh() && timelineNotesDisplayComboId_ == 2
        && samplesPerPixel_ > 0.0 && std::isfinite(samplesPerPixel_);
}

std::optional<std::pair<int, ExperimentalPianoRollView::TimelineNoteResizeEdge>>
ExperimentalPianoRollView::findTimelineBarResizeEdgeAtPoint(const juce::Point<int> pos) const
{
    if (!timelineBarsResizeEnabled())
    {
        return std::nullopt;
    }

    const auto gr = gridBounds();
    const double sr = effectiveDeviceSampleRate(deviceManager_);
    const double bpm = pattern_.bpm > 0.0 ? pattern_.bpm : 120.0;
    const int tpq = experimentalEffectiveTicksPerQuarter(pattern_);
    const bool pianoRowMode = (rowLabelMode_ == 1);
    const float px = (float)pos.x;
    const float py = (float)pos.y;

    for (int i = (int)pattern_.timelineNotes.size() - 1; i >= 0; --i)
    {
        const auto& tn = pattern_.timelineNotes[(size_t)i];
        if (tn.midiNote < pitchLow_ || tn.midiNote > pitchHigh_)
        {
            continue;
        }
        const auto rrOpt = visibleRowStripRect(gr, tn.midiNote);
        if (!rrOpt)
        {
            continue;
        }
        const auto& rr = *rrOpt;
        const std::int64_t a0 = absoluteSampleForTimelineNote(timelineClip_->timelineAnchorSamples, tn, pattern_, sr);
        const std::int64_t vis0 = timelineClip_->startSamples;
        const std::int64_t vis1 = vis0 + juce::jmax(std::int64_t{1}, timelineClip_->lengthSamples);
        if (a0 < vis0 || a0 >= vis1)
        {
            continue;
        }

        const std::int64_t durS = ticksToRelativeSamples(
            juce::jmax<std::int64_t>(1, tn.durationTicks), bpm, tpq, sr);
        const std::int64_t a1 = a0 + juce::jmax<std::int64_t>(1, durS);
        float xL = xForSessionSample(a0);
        float xR = xForSessionSample(a1);
        if (xR < xL)
        {
            std::swap(xL, xR);
        }
        xL = juce::jmax(xL, (float)gr.getX());
        xR = juce::jmin(xR, (float)gr.getRight());
        if (xR <= (float)gr.getX() || xL >= (float)gr.getRight())
        {
            continue;
        }
        const float notePadY = pianoRowMode ? 1.0f : 2.0f;
        const float noteInsetV = pianoRowMode ? 2.0f : 4.0f;
        auto noteRect = juce::Rectangle<float>(
            xL, (float)rr.getY() + notePadY, xR - xL, (float)rr.getHeight() - noteInsetV);
        if (noteRect.getWidth() < 3.0f)
        {
            noteRect = noteRect.withSizeKeepingCentre(4.0f, noteRect.getHeight());
        }
        if (!noteRect.contains(px, py))
        {
            continue;
        }

        constexpr float kMinBodyPx = 4.0f;
        const float w = noteRect.getWidth();
        float edgeW = juce::jmin(6.0f, juce::jmax(4.0f, w * 0.2f));
        if (edgeW * 2.0f + kMinBodyPx > w)
        {
            edgeW = juce::jmax(2.0f, (w - kMinBodyPx) * 0.5f);
        }
        if (w < 9.0f)
        {
            edgeW = juce::jmin(edgeW, juce::jmax(2.5f, w * 0.38f));
        }
        const float nxl = noteRect.getX();
        const float nxr = noteRect.getRight();
        if (px <= nxl + edgeW)
        {
            return std::make_pair(i, TimelineNoteResizeEdge::Left);
        }
        if (px >= nxr - edgeW)
        {
            return std::make_pair(i, TimelineNoteResizeEdge::Right);
        }
    }
    return std::nullopt;
}

std::int64_t ExperimentalPianoRollView::snapTimelineTickForEdit(const std::int64_t tick) const noexcept
{
    if (!useAbsoluteTimeline() || timelineClip_ == nullptr || session_ == nullptr || deviceManager_ == nullptr)
    {
        return tick;
    }
    const SnapSettings snap = session_->getArrangementSnapSettings();
    if (!snap.enabled)
    {
        return tick;
    }

    const std::int64_t anchor = timelineClip_->timelineAnchorSamples;
    const double sr = effectiveDeviceSampleRate(deviceManager_);
    const double bpm = pattern_.bpm > 0.0 ? pattern_.bpm : 120.0;
    const int tpq = experimentalEffectiveTicksPerQuarter(pattern_);
    const std::int64_t absS = anchor + ticksToSignedSamples(tick, bpm, tpq, sr);
    const std::int64_t snappedAbs
        = snapSampleToGridIfEnabled(absS, snap, session_->getProjectMusicalTime(), sr);
    const std::int64_t relS = snappedAbs - anchor;
    return relativeSamplesToTicks(relS, bpm, tpq, sr);
}

std::int64_t ExperimentalPianoRollView::snapTimelineTickForCreate(const std::int64_t tick) const noexcept
{
    if (!useAbsoluteTimeline() || timelineClip_ == nullptr || session_ == nullptr || deviceManager_ == nullptr)
    {
        return tick;
    }
    const SnapSettings snap = session_->getArrangementSnapSettings();
    if (!snap.enabled)
    {
        return tick;
    }

    const std::int64_t anchor = timelineClip_->timelineAnchorSamples;
    const double sr = effectiveDeviceSampleRate(deviceManager_);
    const double bpm = pattern_.bpm > 0.0 ? pattern_.bpm : 120.0;
    const int tpq = experimentalEffectiveTicksPerQuarter(pattern_);
    const std::int64_t absS = anchor + ticksToSignedSamples(tick, bpm, tpq, sr);
    const std::int64_t snappedAbs
        = snapSampleToGridFloorIfEnabled(absS, snap, session_->getProjectMusicalTime(), sr);
    const std::int64_t relS = snappedAbs - anchor;
    return relativeSamplesToTicks(relS, bpm, tpq, sr);
}

void ExperimentalPianoRollView::sortTimelineNotesForEditing() noexcept
{
    std::sort(
        pattern_.timelineNotes.begin(), pattern_.timelineNotes.end(),
        [](const TimelineMidiNote& a, const TimelineMidiNote& b) noexcept {
            if (a.startTick != b.startTick)
            {
                return a.startTick < b.startTick;
            }
            if (a.midiNote != b.midiNote)
            {
                return a.midiNote < b.midiNote;
            }
            return a.channel < b.channel;
        });
}

void ExperimentalPianoRollView::replaceTimelineSelectionWithNotesMatching(
    const std::vector<TimelineMidiNote>& matches) noexcept
{
    selectedTimelineNoteIndices_.clear();
    std::unordered_set<int> used;
    for (const auto& want : matches)
    {
        for (int i = 0; i < (int)pattern_.timelineNotes.size(); ++i)
        {
            if (used.count(i) != 0u)
            {
                continue;
            }
            const auto& n = pattern_.timelineNotes[(size_t)i];
            if (n.startTick == want.startTick && n.durationTicks == want.durationTicks && n.midiNote == want.midiNote
                && n.channel == want.channel && n.velocity == want.velocity)
            {
                selectedTimelineNoteIndices_.insert(i);
                used.insert(i);
                break;
            }
        }
    }
}

std::int64_t ExperimentalPianoRollView::computeTimelinePasteAnchorTick() const
{
    if (timelineClip_ == nullptr)
    {
        return 0;
    }
    const std::int64_t anchor = timelineClip_->timelineAnchorSamples;
    const double bpm = pattern_.bpm > 0.0 ? pattern_.bpm : 120.0;
    const int tpq = experimentalEffectiveTicksPerQuarter(pattern_);
    const double sr = effectiveDeviceSampleRate(deviceManager_);

    std::int64_t rawTick = 0;
    if (transport_ != nullptr && useAbsoluteTimeline())
    {
        rawTick = relativeSamplesToTicks(
            (std::int64_t)std::llround(uiPlayheadDisplaySamples_) - anchor, bpm, tpq, sr);
    }
    else if (hasValidViewportState())
    {
        rawTick = relativeSamplesToTicks(visibleStartSamples_ - anchor, bpm, tpq, sr);
    }
    else
    {
        const std::int64_t step = juce::jmax<std::int64_t>(
            1,
            musicalSnapGridTicks() > 0 ? musicalSnapGridTicks() : referenceTimelineGridTicks());
        rawTick = timelineClipboardSourceMinStartTick_ + step;
    }
    return snapTimelineTickForEdit(rawTick);
}

bool ExperimentalPianoRollView::handleTimelineNotesCopyShortcut() noexcept
{
    if (!useAbsoluteTimeline() || timelineClip_ == nullptr || !isTimelineClipBindingFresh()
       )
    {
        return false;
    }
    if (selectedTimelineNoteIndices_.empty())
    {
        return true;
    }

    std::vector<std::pair<int, const TimelineMidiNote*>> items;
    items.reserve(selectedTimelineNoteIndices_.size());
    for (const int i : selectedTimelineNoteIndices_)
    {
        if (i >= 0 && i < (int)pattern_.timelineNotes.size())
        {
            items.push_back({i, &pattern_.timelineNotes[(size_t)i]});
        }
    }
    if (items.empty())
    {
        return true;
    }
    std::sort(items.begin(), items.end(), [](const auto& a, const auto& b) noexcept {
        if (a.second->startTick != b.second->startTick)
        {
            return a.second->startTick < b.second->startTick;
        }
        if (a.second->midiNote != b.second->midiNote)
        {
            return a.second->midiNote < b.second->midiNote;
        }
        return a.second->channel < b.second->channel;
    });

    const std::int64_t minStart = items.front().second->startTick;
    timelineClipboardSourceMinStartTick_ = minStart;
    timelineInternalClipboard_.clear();
    timelineInternalClipboard_.reserve(items.size());
    for (const auto& it : items)
    {
        const auto& n = *it.second;
        InternalTimelineClipboardItem row;
        row.deltaStartTicks = n.startTick - minStart;
        row.midiNote = n.midiNote;
        row.velocity = n.velocity;
        row.offVelocity = sanitizeMidiNoteOffVelocity(n.offVelocity);
        row.channel = n.channel;
        row.durationTicks = n.durationTicks;
        timelineInternalClipboard_.push_back(row);
    }
    return true;
}

bool ExperimentalPianoRollView::handleTimelineNotesPasteShortcut()
{
    if (!useAbsoluteTimeline() || timelineClip_ == nullptr || !isTimelineClipBindingFresh()
       )
    {
        return false;
    }
    if (timelineInternalClipboard_.empty())
    {
        return true;
    }

    const std::int64_t anchor = timelineClip_->timelineAnchorSamples;
    const std::int64_t vis0 = timelineClip_->startSamples;
    const std::int64_t vis1 = vis0 + juce::jmax(std::int64_t{1}, timelineClip_->lengthSamples);
    const double bpm = pattern_.bpm > 0.0 ? pattern_.bpm : 120.0;
    const int tpq = experimentalEffectiveTicksPerQuarter(pattern_);
    const double sr = effectiveDeviceSampleRate(deviceManager_);
    const std::int64_t pasteTick = computeTimelinePasteAnchorTick();

    std::vector<TimelineMidiNote> toAdd;
    toAdd.reserve(timelineInternalClipboard_.size());
    for (const auto& it : timelineInternalClipboard_)
    {
        TimelineMidiNote n;
        n.midiNote = it.midiNote;
        n.velocity = it.velocity;
        n.offVelocity = sanitizeMidiNoteOffVelocity(it.offVelocity);
        n.channel = it.channel;
        n.durationTicks = it.durationTicks;
        n.startTick = pasteTick + it.deltaStartTicks;

        const std::int64_t a0 = absoluteSampleForTimelineNote(anchor, n, pattern_, sr);
        if (a0 < vis0 || a0 >= vis1)
        {
            continue;
        }
        const std::int64_t durS = ticksToRelativeSamples(
            juce::jmax<std::int64_t>(1, n.durationTicks), bpm, tpq, sr);
        const std::int64_t a1 = a0 + juce::jmax<std::int64_t>(1, durS);
        if (a1 > vis1)
        {
            const std::int64_t maxSam = juce::jmax(std::int64_t{1}, vis1 - a0);
            n.durationTicks = juce::jmax<std::int64_t>(
                1, relativeSamplesToTicks(maxSam, bpm, tpq, sr));
        }
        toAdd.push_back(n);
    }

    if (toAdd.empty())
    {
        return true;
    }

    if (currentEditCandidatesOverlap({}, toAdd, nullptr))
    {
        flashForbiddenNoDropCursor();
        return true;
    }

    const std::vector<TimelineMidiNote> selectionSnapshot = toAdd;

    auto applyPaste = [this, toAdd = std::move(toAdd), selectionSnapshot]() mutable -> bool {
        for (auto& n : toAdd)
        {
            pattern_.timelineNotes.push_back(std::move(n));
        }
        sortTimelineNotesForEditing();
        if (instrumentTrackController_ != nullptr)
        {
            instrumentTrackController_->notifyClipExperimentalMusicalTimingChanged();
        }
        replaceTimelineSelectionWithNotesMatching(selectionSnapshot);
        repaint();
        return true;
    };

    if (undoablePatternEditHandler_ != nullptr && instrumentTrackController_ != nullptr && timelineClip_ != nullptr)
    {
        undoablePatternEditHandler_("Paste MIDI notes", std::move(applyPaste));
    }
    else
    {
        applyPaste();
    }
    return true;
}

bool ExperimentalPianoRollView::handleTimelineNotesDeleteSelectionShortcut()
{
    if (!useAbsoluteTimeline() || timelineClip_ == nullptr || !isTimelineClipBindingFresh()
       )
    {
        return false;
    }
    normalizeTimelineNoteSelection();
    if (selectedTimelineNoteIndices_.empty())
    {
        return false;
    }

    // Descending order keeps the remaining indices valid while erasing.
    std::vector<int> indices(selectedTimelineNoteIndices_.begin(), selectedTimelineNoteIndices_.end());
    std::sort(indices.begin(), indices.end(), std::greater<int>());

    auto eraseAndNotify = [this, indices]() -> bool {
        for (const int i : indices)
        {
            if (i < 0 || i >= (int)pattern_.timelineNotes.size())
            {
                return false;
            }
        }
        for (const int i : indices)
        {
            pattern_.timelineNotes.erase(pattern_.timelineNotes.begin() + i);
        }
        selectedTimelineNoteIndices_.clear();
        if (instrumentTrackController_ != nullptr)
        {
            instrumentTrackController_->notifyClipExperimentalMusicalTimingChanged();
        }
        repaint();
        return true;
    };

    const juce::String label = indices.size() > 1 ? "Delete MIDI notes" : "Delete MIDI note";
    if (undoablePatternEditHandler_ != nullptr && instrumentTrackController_ != nullptr && timelineClip_ != nullptr)
    {
        undoablePatternEditHandler_(label, std::move(eraseAndNotify));
    }
    else
    {
        eraseAndNotify();
    }
    return true;
}

std::int64_t ExperimentalPianoRollView::minTimelineNoteDurationTicks() const noexcept
{
    const int tpq = experimentalEffectiveTicksPerQuarter(pattern_);
    const std::int64_t snap = musicalSnapGridTicks();
    if (snap > 0)
    {
        return juce::jmax<std::int64_t>(1, snap);
    }
    return juce::jmax<std::int64_t>(1, (std::int64_t)(tpq / 16));
}

bool ExperimentalPianoRollView::currentEditCandidatesOverlap(
    const std::vector<int>& ignoreIndices,
    const std::vector<TimelineMidiNote>& candidates,
    const std::vector<TimelineMidiNote>* grandfatherOriginals) const noexcept
{
    return !validateTimelineNotesNoOverlap(pattern_.timelineNotes,
                                           ignoreIndices,
                                           candidates,
                                           minTimelineNoteDurationTicks(),
                                           grandfatherOriginals)
                .valid;
}

void ExperimentalPianoRollView::flashForbiddenNoDropCursor()
{
    setMouseCursor(getForbiddenNoDropMouseCursor());
    forbiddenCursorFlashUntilMs_ = juce::Time::getMillisecondCounterHiRes() + 450.0;
}

void ExperimentalPianoRollView::updateTimelineNoteEditCursor()
{
    if (timelineResizeInvalid_ || timelineMoveInvalid_)
    {
        setMouseCursor(getForbiddenNoDropMouseCursor());
        return;
    }
    if (timelineNoteResizeActive_)
    {
        setMouseCursor(juce::MouseCursor::LeftRightResizeCursor);
        return;
    }
    setMouseCursor(juce::MouseCursor::NormalCursor);
}

void ExperimentalPianoRollView::beginTimelineNoteResizeGesture(
    const int noteIndex,
    const ExperimentalPianoRollView::TimelineNoteResizeEdge edge)
{
    if (noteIndex < 0 || noteIndex >= (int)pattern_.timelineNotes.size())
    {
        return;
    }
    timelineMarqueeInteraction_ = TimelineMarqueeInteraction::None;
    clearTimelineNoteMovePending();
    timelineNoteResizeActive_ = true;
    timelineResizeInvalid_ = false;
    timelineResizeEdge_ = edge;
    timelineResizeNoteIndex_ = noteIndex;
    const auto& n = pattern_.timelineNotes[(size_t)noteIndex];
    timelineResizeOriginalStartTick_ = n.startTick;
    timelineResizeOriginalDurationTicks_ = n.durationTicks;
    timelineResizeAnchorEndTick_ = n.startTick + juce::jmax<std::int64_t>(1, n.durationTicks);

    // Click-to-resize of an unselected note already replaced the selection with that note,
    // so capturing the selection always matches "resize all selected" vs "resize this one".
    timelineResizeCaptures_.clear();
    std::vector<int> order;
    order.reserve(selectedTimelineNoteIndices_.size());
    for (const int i : selectedTimelineNoteIndices_)
    {
        if (i >= 0 && i < (int)pattern_.timelineNotes.size())
        {
            order.push_back(i);
        }
    }
    if (std::find(order.begin(), order.end(), noteIndex) == order.end())
    {
        order.push_back(noteIndex);
    }
    std::sort(order.begin(), order.end());
    timelineResizeCaptures_.reserve(order.size());
    for (const int i : order)
    {
        TimelineNoteResizeCapture cap;
        cap.index = i;
        cap.original = pattern_.timelineNotes[(size_t)i];
        timelineResizeCaptures_.push_back(std::move(cap));
    }
}

void ExperimentalPianoRollView::updateTimelineNoteResizeGesture(const juce::Point<int> localPos)
{
    if (!timelineNoteResizeActive_ || timelineClip_ == nullptr || timelineResizeNoteIndex_ < 0
        || timelineResizeNoteIndex_ >= (int)pattern_.timelineNotes.size()
        || timelineResizeCaptures_.empty())
    {
        return;
    }

    const std::int64_t anchor = timelineClip_->timelineAnchorSamples;
    const std::int64_t vis0 = timelineClip_->startSamples;
    const std::int64_t vis1 = vis0 + juce::jmax(std::int64_t{1}, timelineClip_->lengthSamples);
    const double sr = effectiveDeviceSampleRate(deviceManager_);
    const double bpm = pattern_.bpm > 0.0 ? pattern_.bpm : 120.0;
    const int tpq = experimentalEffectiveTicksPerQuarter(pattern_);
    const std::int64_t minD = minTimelineNoteDurationTicks();
    const std::int64_t clipLowTick = relativeSamplesToTicks(vis0 - anchor, bpm, tpq, sr);
    const std::int64_t clipHighTick = relativeSamplesToTicks(vis1 - anchor, bpm, tpq, sr);

    const std::int64_t mouseAbs = sampleAtGridX((float)localPos.getX());
    std::int64_t rawTick = relativeSamplesToTicks(mouseAbs - anchor, bpm, tpq, sr);
    const std::int64_t tSnap = snapTimelineTickForEdit(rawTick);

    std::int64_t startDelta = 0;
    std::int64_t durationDelta = 0;
    if (timelineResizeEdge_ == TimelineNoteResizeEdge::Right)
    {
        std::int64_t endTick = (musicalSnapGridTicks() <= 0) ? rawTick : tSnap;
        endTick = juce::jmax(endTick, timelineResizeOriginalStartTick_ + minD);
        endTick = juce::jmin(endTick, clipHighTick);
        endTick = juce::jmax(endTick, timelineResizeOriginalStartTick_ + minD);
        durationDelta = juce::jmax(minD, endTick - timelineResizeOriginalStartTick_)
                        - timelineResizeOriginalDurationTicks_;
        for (const auto& cap : timelineResizeCaptures_)
        {
            const std::int64_t origDur = juce::jmax<std::int64_t>(1, cap.original.durationTicks);
            durationDelta = juce::jmax(durationDelta, minD - origDur);
            durationDelta = juce::jmin(durationDelta, clipHighTick - cap.original.startTick - origDur);
        }
    }
    else
    {
        const std::int64_t endT = timelineResizeAnchorEndTick_;
        std::int64_t newStart = (musicalSnapGridTicks() <= 0) ? rawTick : tSnap;
        newStart = juce::jmin(newStart, endT - minD);
        newStart = juce::jmax(newStart, clipLowTick);
        newStart = juce::jmin(newStart, endT - minD);
        startDelta = newStart - timelineResizeOriginalStartTick_;
        // Same start-edge delta for every selected note; end stays fixed. Clamp the shared
        // delta so no note drops below min length or leaves the clip.
        for (const auto& cap : timelineResizeCaptures_)
        {
            const std::int64_t origEnd
                = cap.original.startTick + juce::jmax<std::int64_t>(1, cap.original.durationTicks);
            startDelta = juce::jmax(startDelta, clipLowTick - cap.original.startTick);
            startDelta = juce::jmin(startDelta, origEnd - minD - cap.original.startTick);
        }
    }

    std::vector<int> ignore;
    std::vector<TimelineMidiNote> candidates;
    std::vector<TimelineMidiNote> originals;
    ignore.reserve(timelineResizeCaptures_.size());
    candidates.reserve(timelineResizeCaptures_.size());
    originals.reserve(timelineResizeCaptures_.size());

    for (const auto& cap : timelineResizeCaptures_)
    {
        if (cap.index < 0 || cap.index >= (int)pattern_.timelineNotes.size())
        {
            continue;
        }
        TimelineMidiNote next = cap.original;
        const std::int64_t origEnd
            = cap.original.startTick + juce::jmax<std::int64_t>(1, cap.original.durationTicks);
        if (timelineResizeEdge_ == TimelineNoteResizeEdge::Right)
        {
            next.startTick = cap.original.startTick;
            next.durationTicks = juce::jmax(minD, origEnd - cap.original.startTick + durationDelta);
        }
        else
        {
            next.startTick = cap.original.startTick + startDelta;
            next.durationTicks = juce::jmax(minD, origEnd - next.startTick);
        }
        ignore.push_back(cap.index);
        originals.push_back(cap.original);
        candidates.push_back(next);
        pattern_.timelineNotes[(size_t)cap.index] = next;
    }

    timelineResizeInvalid_ = currentEditCandidatesOverlap(ignore, candidates, &originals);
}

void ExperimentalPianoRollView::finishTimelineNoteResizeGesture()
{
    if (!timelineNoteResizeActive_)
    {
        return;
    }

    const auto captures = std::move(timelineResizeCaptures_);
    const bool invalid = timelineResizeInvalid_;
    timelineNoteResizeActive_ = false;
    timelineResizeInvalid_ = false;
    timelineResizeNoteIndex_ = -1;
    timelineResizeCaptures_.clear();

    if (captures.empty())
    {
        updateTimelineNoteEditCursor();
        repaint();
        return;
    }

    const auto restore = [this, &captures]() {
        for (const auto& cap : captures)
        {
            if (cap.index >= 0 && cap.index < (int)pattern_.timelineNotes.size())
            {
                pattern_.timelineNotes[(size_t)cap.index] = cap.original;
            }
        }
    };

    if (invalid)
    {
        restore();
        updateTimelineNoteEditCursor();
        repaint();
        return;
    }

    bool anyChange = false;
    std::vector<TimelineMidiNote> finals;
    finals.reserve(captures.size());
    for (const auto& cap : captures)
    {
        if (cap.index < 0 || cap.index >= (int)pattern_.timelineNotes.size())
        {
            restore();
            updateTimelineNoteEditCursor();
            repaint();
            return;
        }
        const auto& n = pattern_.timelineNotes[(size_t)cap.index];
        finals.push_back(n);
        if (n.startTick != cap.original.startTick || n.durationTicks != cap.original.durationTicks)
        {
            anyChange = true;
        }
    }
    if (!anyChange)
    {
        restore();
        updateTimelineNoteEditCursor();
        repaint();
        return;
    }

    restore();

    auto applyResize = [this, captures, finals]() -> bool {
        for (size_t k = 0; k < captures.size(); ++k)
        {
            const int idx = captures[k].index;
            if (idx < 0 || idx >= (int)pattern_.timelineNotes.size())
            {
                return false;
            }
            pattern_.timelineNotes[(size_t)idx] = finals[k];
        }
        if (instrumentTrackController_ != nullptr)
        {
            instrumentTrackController_->notifyClipExperimentalMusicalTimingChanged();
        }
        repaint();
        return true;
    };

    const juce::String label = captures.size() > 1 ? "Resize MIDI notes" : "Resize MIDI note";
    if (undoablePatternEditHandler_ != nullptr && instrumentTrackController_ != nullptr && timelineClip_ != nullptr)
    {
        undoablePatternEditHandler_(label, std::move(applyResize));
    }
    else
    {
        applyResize();
    }
    updateTimelineNoteEditCursor();
}

void ExperimentalPianoRollView::clearTimelineNoteMovePending() noexcept
{
    timelineMovePending_ = false;
    timelineMovePrimaryIndex_ = -1;
}

void ExperimentalPianoRollView::beginTimelineNoteMoveGesture(const juce::MouseEvent& e)
{
    if (timelineClip_ == nullptr || !isTimelineClipBindingFresh() || timelineMovePrimaryIndex_ < 0)
    {
        clearTimelineNoteMovePending();
        return;
    }
    normalizeTimelineNoteSelection();
    if (selectedTimelineNoteIndices_.count(timelineMovePrimaryIndex_) == 0u)
    {
        clearTimelineNoteMovePending();
        return;
    }

    timelineMarqueeInteraction_ = TimelineMarqueeInteraction::None;
    timelineMovePending_ = false;
    timelineNoteMoveActive_ = true;
    timelineMoveInvalid_ = false;
    timelineMoveCaptures_.clear();

    std::vector<int> order;
    order.reserve(selectedTimelineNoteIndices_.size());
    for (const int i : selectedTimelineNoteIndices_)
    {
        if (i >= 0 && i < (int)pattern_.timelineNotes.size())
        {
            order.push_back(i);
        }
    }
    std::sort(order.begin(), order.end());
    if (order.empty())
    {
        timelineNoteMoveActive_ = false;
        timelineMoveCaptures_.clear();
        clearTimelineNoteMovePending();
        return;
    }
    timelineMoveCaptures_.reserve(order.size());
    for (const int i : order)
    {
        TimelineNoteMoveCapture cap;
        cap.index = i;
        cap.original = pattern_.timelineNotes[(size_t)i];
        timelineMoveCaptures_.push_back(std::move(cap));
    }

    const juce::Point<int> down = e.getMouseDownPosition();
    timelineMoveAnchorAbsSample_ = sampleAtGridX((float)down.x);
    timelineMoveAnchorPitch_ = pitchAtY(down.y);
    if (timelineMovePrimaryIndex_ >= 0 && timelineMovePrimaryIndex_ < (int)pattern_.timelineNotes.size())
    {
        timelineMovePrimaryOrigStartTick_
            = pattern_.timelineNotes[(size_t)timelineMovePrimaryIndex_].startTick;
    }
    else
    {
        timelineMovePrimaryOrigStartTick_ = 0;
    }
}

void ExperimentalPianoRollView::updateTimelineNoteMoveGesture(const juce::Point<int> localPos)
{
    if (!timelineNoteMoveActive_ || timelineClip_ == nullptr || !isTimelineClipBindingFresh()
        || timelineMoveCaptures_.empty())
    {
        return;
    }

    const std::int64_t anchorSamp = timelineClip_->timelineAnchorSamples;
    const std::int64_t vis0 = timelineClip_->startSamples;
    const std::int64_t vis1 = vis0 + juce::jmax(std::int64_t{1}, timelineClip_->lengthSamples);
    const double sr = effectiveDeviceSampleRate(deviceManager_);
    const double bpm = pattern_.bpm > 0.0 ? pattern_.bpm : 120.0;
    const int tpq = experimentalEffectiveTicksPerQuarter(pattern_);
    const std::int64_t clipLowTick = relativeSamplesToTicks(vis0 - anchorSamp, bpm, tpq, sr);
    const std::int64_t clipHighTick = relativeSamplesToTicks(vis1 - anchorSamp, bpm, tpq, sr);

    const std::int64_t curAbs = sampleAtGridX((float)localPos.getX());
    std::int64_t deltaTick = relativeSamplesToTicks(curAbs - timelineMoveAnchorAbsSample_, bpm, tpq, sr);
    if (musicalSnapGridTicks() > 0)
    {
        const std::int64_t rawPrimaryTarget = timelineMovePrimaryOrigStartTick_ + deltaTick;
        const std::int64_t snapped = snapTimelineTickForEdit(rawPrimaryTarget);
        deltaTick = snapped - timelineMovePrimaryOrigStartTick_;
    }

    int deltaPitch = pitchAtY(localPos.getY()) - timelineMoveAnchorPitch_;

    std::int64_t deltaTickMin = std::numeric_limits<std::int64_t>::min() / 4;
    std::int64_t deltaTickMax = std::numeric_limits<std::int64_t>::max() / 4;
    int deltaPitchMin = -128;
    int deltaPitchMax = 128;
    for (const auto& cap : timelineMoveCaptures_)
    {
        const auto& o = cap.original;
        deltaTickMin = juce::jmax(deltaTickMin, clipLowTick - o.startTick);
        deltaTickMax = juce::jmin(
            deltaTickMax, clipHighTick - o.startTick - juce::jmax<std::int64_t>(1, o.durationTicks));
        deltaPitchMin = juce::jmax(deltaPitchMin, pitchLow_ - o.midiNote);
        deltaPitchMax = juce::jmin(deltaPitchMax, pitchHigh_ - o.midiNote);
    }
    deltaTick = juce::jlimit(deltaTickMin, deltaTickMax, deltaTick);
    deltaPitch = juce::jlimit(deltaPitchMin, deltaPitchMax, deltaPitch);

    std::vector<int> ignore;
    std::vector<TimelineMidiNote> candidates;
    std::vector<TimelineMidiNote> originals;
    ignore.reserve(timelineMoveCaptures_.size());
    candidates.reserve(timelineMoveCaptures_.size());
    originals.reserve(timelineMoveCaptures_.size());

    for (const auto& cap : timelineMoveCaptures_)
    {
        if (cap.index < 0 || cap.index >= (int)pattern_.timelineNotes.size())
        {
            continue;
        }
        auto& n = pattern_.timelineNotes[(size_t)cap.index];
        const auto& o = cap.original;
        n.startTick = o.startTick + deltaTick;
        n.midiNote = o.midiNote + deltaPitch;
        n.durationTicks = o.durationTicks;
        n.velocity = o.velocity;
        n.channel = o.channel;
        ignore.push_back(cap.index);
        originals.push_back(o);
        candidates.push_back(n);
    }

    timelineMoveInvalid_ = currentEditCandidatesOverlap(ignore, candidates, &originals);
}

void ExperimentalPianoRollView::finishTimelineNoteMoveGesture()
{
    if (!timelineNoteMoveActive_)
    {
        return;
    }
    timelineNoteMoveActive_ = false;
    const bool invalid = timelineMoveInvalid_;
    timelineMoveInvalid_ = false;
    const auto captures = std::move(timelineMoveCaptures_);
    timelineMovePrimaryIndex_ = -1;
    if (captures.empty())
    {
        updateTimelineNoteEditCursor();
        repaint();
        return;
    }

    if (invalid)
    {
        for (const auto& cap : captures)
        {
            if (cap.index >= 0 && cap.index < (int)pattern_.timelineNotes.size())
            {
                pattern_.timelineNotes[(size_t)cap.index] = cap.original;
            }
        }
        updateTimelineNoteEditCursor();
        repaint();
        return;
    }

    bool anyChange = false;
    for (const auto& cap : captures)
    {
        if (cap.index < 0 || cap.index >= (int)pattern_.timelineNotes.size())
        {
            continue;
        }
        const auto& n = pattern_.timelineNotes[(size_t)cap.index];
        const auto& o = cap.original;
        if (n.startTick != o.startTick || n.midiNote != o.midiNote)
        {
            anyChange = true;
            break;
        }
    }
    if (!anyChange)
    {
        for (const auto& cap : captures)
        {
            if (cap.index >= 0 && cap.index < (int)pattern_.timelineNotes.size())
            {
                pattern_.timelineNotes[(size_t)cap.index] = cap.original;
            }
        }
        updateTimelineNoteEditCursor();
        repaint();
        return;
    }

    std::vector<TimelineMidiNote> finalNotes;
    finalNotes.reserve(captures.size());
    for (const auto& cap : captures)
    {
        if (cap.index >= 0 && cap.index < (int)pattern_.timelineNotes.size())
        {
            finalNotes.push_back(pattern_.timelineNotes[(size_t)cap.index]);
        }
    }
    if (finalNotes.size() != captures.size())
    {
        for (const auto& cap : captures)
        {
            if (cap.index >= 0 && cap.index < (int)pattern_.timelineNotes.size())
            {
                pattern_.timelineNotes[(size_t)cap.index] = cap.original;
            }
        }
        updateTimelineNoteEditCursor();
        repaint();
        return;
    }

    for (const auto& cap : captures)
    {
        pattern_.timelineNotes[(size_t)cap.index] = cap.original;
    }

    auto applyMove = [this, captures, finalNotes]() -> bool {
        for (size_t k = 0; k < captures.size(); ++k)
        {
            const int idx = captures[k].index;
            if (idx < 0 || idx >= (int)pattern_.timelineNotes.size())
            {
                return false;
            }
            pattern_.timelineNotes[(size_t)idx] = finalNotes[k];
        }
        sortTimelineNotesForEditing();
        replaceTimelineSelectionWithNotesMatching(finalNotes);
        if (instrumentTrackController_ != nullptr)
        {
            instrumentTrackController_->notifyClipExperimentalMusicalTimingChanged();
        }
        repaint();
        return true;
    };

    if (undoablePatternEditHandler_ != nullptr && instrumentTrackController_ != nullptr && timelineClip_ != nullptr)
    {
        undoablePatternEditHandler_("Move MIDI notes", std::move(applyMove));
    }
    else
    {
        applyMove();
    }
    updateTimelineNoteEditCursor();
}

juce::Colour colourForMidiVelocity(const int velocity) noexcept
{
    const float t = (float)(juce::jlimit(1, 127, velocity) - 1) / 126.0f;
    const juce::Colour low(0xff5d7fae);  // muted blue (soft)
    const juce::Colour mid(0xff9a5a96);  // violet / muted red (medium)
    const juce::Colour high(0xffe0483e); // bright red (hard)
    return t < 0.5f ? low.interpolatedWith(mid, t * 2.0f)
                    : mid.interpolatedWith(high, (t - 0.5f) * 2.0f);
}

bool ExperimentalPianoRollView::velocityLaneEditingAvailable() const noexcept
{
    return useAbsoluteTimeline() && timelineClip_ != nullptr
           && samplesPerPixel_ > 0.0 && std::isfinite(samplesPerPixel_) && isTimelineClipBindingFresh();
}

int ExperimentalPianoRollView::velocityFromLaneY(const int y) const noexcept
{
    const auto inner = velocityLaneInnerBounds();
    if (inner.getHeight() <= 0)
    {
        return 100;
    }
    const float t = (float)(inner.getBottom() - y) / (float)inner.getHeight();
    return juce::jlimit(1, 127, juce::roundToInt(t * 127.0f));
}

std::optional<float> ExperimentalPianoRollView::velocityBarCentreXForNoteIndex(const int noteIndex) const
{
    if (!velocityLaneEditingAvailable() || noteIndex < 0
        || noteIndex >= (int)pattern_.timelineNotes.size())
    {
        return std::nullopt;
    }
    const auto& tn = pattern_.timelineNotes[(size_t)noteIndex];
    if (tn.midiNote < pitchLow_ || tn.midiNote > pitchHigh_)
    {
        return std::nullopt;
    }
    const double sr = effectiveDeviceSampleRate(deviceManager_);
    const std::int64_t a0
        = absoluteSampleForTimelineNote(timelineClip_->timelineAnchorSamples, tn, pattern_, sr);
    const std::int64_t vis0 = timelineClip_->startSamples;
    const std::int64_t vis1 = vis0 + juce::jmax(std::int64_t{1}, timelineClip_->lengthSamples);
    if (a0 < vis0 || a0 >= vis1)
    {
        return std::nullopt;
    }
    return xForSessionSample(a0);
}

std::optional<int> ExperimentalPianoRollView::findVelocityBarIndexNearX(const int x,
                                                                        const bool selectedOnly) const
{
    if (!velocityLaneEditingAvailable())
    {
        return std::nullopt;
    }
    const auto lane = velocityLaneBounds();
    if (lane.isEmpty())
    {
        return std::nullopt;
    }
    std::optional<int> best;
    float bestDist = (float)kVelocityBarHitToleranceX + 0.5f;
    int bestVelocity = -1;
    for (int ti = 0; ti < (int)pattern_.timelineNotes.size(); ++ti)
    {
        if (selectedOnly && !isTimelineNoteIndexSelected(ti))
        {
            continue;
        }
        const auto bxOpt = velocityBarCentreXForNoteIndex(ti);
        if (!bxOpt)
        {
            continue;
        }
        const float bx = *bxOpt;
        if (bx < (float)lane.getX() - (float)kVelocityBarHitToleranceX
            || bx > (float)lane.getRight() + (float)kVelocityBarHitToleranceX)
        {
            continue;
        }
        const float dist = std::abs((float)x - bx);
        if (dist > (float)kVelocityBarHitToleranceX)
        {
            continue;
        }
        const int vel = pattern_.timelineNotes[(size_t)ti].velocity;
        const bool closer = dist < bestDist - 0.5f;
        const bool tieButTaller = std::abs(dist - bestDist) <= 0.5f && vel > bestVelocity;
        if (closer || tieButTaller)
        {
            best = ti;
            bestDist = dist;
            bestVelocity = vel;
        }
    }
    return best;
}

void ExperimentalPianoRollView::handleVelocityLaneMouseDown(const juce::MouseEvent& e)
{
    if (!velocityLaneEditingAvailable() || e.mods.isPopupMenu() || !e.mods.isLeftButtonDown())
    {
        return;
    }
    normalizeTimelineNoteSelection();
    const int x = e.getPosition().getX();

    // Selected bars win within the tolerance (group edit); otherwise the nearest bar is edited alone.
    std::vector<int> editIndices;
    int primary = -1;
    if (const auto sel = findVelocityBarIndexNearX(x, true))
    {
        primary = *sel;
        editIndices.assign(selectedTimelineNoteIndices_.begin(), selectedTimelineNoteIndices_.end());
        std::sort(editIndices.begin(), editIndices.end());
    }
    else if (const auto any = findVelocityBarIndexNearX(x, false))
    {
        primary = *any;
        // No selection relevant to this gesture: expand to every note starting on exactly the same
        // tick (stacked hits like kick+snare edit and audition as one drum event). More than one
        // note in the stack behaves as if the user had selected that stack manually.
        const std::int64_t stackTick = pattern_.timelineNotes[(size_t)primary].startTick;
        for (int ti = 0; ti < (int)pattern_.timelineNotes.size(); ++ti)
        {
            if (pattern_.timelineNotes[(size_t)ti].startTick == stackTick)
            {
                editIndices.push_back(ti);
            }
        }
        if (editIndices.size() > 1)
        {
            selectedTimelineNoteIndices_.clear();
            for (const int ti : editIndices)
            {
                selectedTimelineNoteIndices_.insert(ti);
            }
        }
    }
    else
    {
        return;
    }

    velocityDragCaptures_.clear();
    velocityDragCaptures_.reserve(editIndices.size());
    for (const int i : editIndices)
    {
        if (i < 0 || i >= (int)pattern_.timelineNotes.size())
        {
            continue;
        }
        VelocityDragCapture cap;
        cap.index = i;
        cap.originalVelocity = pattern_.timelineNotes[(size_t)i].velocity;
        velocityDragCaptures_.push_back(cap);
    }
    if (velocityDragCaptures_.empty())
    {
        return;
    }

    // Chord audition only when every captured note starts on the same tick; mixed start times
    // would preview as a meaningless cacophony.
    velocityDragAuditionSameStart_ = true;
    if (velocityDragCaptures_.size() > 1)
    {
        const std::int64_t t0
            = pattern_.timelineNotes[(size_t)velocityDragCaptures_.front().index].startTick;
        for (const auto& cap : velocityDragCaptures_)
        {
            if (pattern_.timelineNotes[(size_t)cap.index].startTick != t0)
            {
                velocityDragAuditionSameStart_ = false;
                break;
            }
        }
    }
    velocityDragLastAuditionMs_ = 0.0;
    velocityDragLastAuditionVelocity_ = -1;

    velocityLaneDragActive_ = true;
    velocityDragPrimaryIndex_ = primary;
    velocityDragAnchorVelocity_ = velocityFromLaneY(e.getPosition().getY());
    updateVelocityLaneDrag(e.getPosition());
    maybeAuditionVelocityDrag(true);
    repaint();
}

void ExperimentalPianoRollView::updateVelocityLaneDrag(const juce::Point<int> localPos)
{
    if (!velocityLaneDragActive_ || !velocityLaneEditingAvailable() || velocityDragCaptures_.empty())
    {
        return;
    }
    const int cursorVelocity = velocityFromLaneY(localPos.getY());
    if (velocityDragCaptures_.size() == 1)
    {
        // Single note: absolute — the bar top tracks the cursor.
        const auto& cap = velocityDragCaptures_.front();
        if (cap.index >= 0 && cap.index < (int)pattern_.timelineNotes.size())
        {
            pattern_.timelineNotes[(size_t)cap.index].velocity = cursorVelocity;
        }
        return;
    }
    // Multi-selection: same delta for all, relative differences preserved, each clamped 1..127.
    const int delta = cursorVelocity - velocityDragAnchorVelocity_;
    for (const auto& cap : velocityDragCaptures_)
    {
        if (cap.index < 0 || cap.index >= (int)pattern_.timelineNotes.size())
        {
            continue;
        }
        pattern_.timelineNotes[(size_t)cap.index].velocity
            = juce::jlimit(1, 127, cap.originalVelocity + delta);
    }
}

void ExperimentalPianoRollView::finishVelocityLaneDragGesture()
{
    if (!velocityLaneDragActive_)
    {
        return;
    }
    velocityLaneDragActive_ = false;
    velocityDragPrimaryIndex_ = -1;
    const auto captures = std::move(velocityDragCaptures_);
    velocityDragCaptures_.clear();
    if (captures.empty())
    {
        repaint();
        return;
    }

    std::vector<int> finalVelocities;
    finalVelocities.reserve(captures.size());
    bool anyChange = false;
    for (const auto& cap : captures)
    {
        if (cap.index < 0 || cap.index >= (int)pattern_.timelineNotes.size())
        {
            finalVelocities.push_back(cap.originalVelocity);
            continue;
        }
        const int fv = pattern_.timelineNotes[(size_t)cap.index].velocity;
        finalVelocities.push_back(fv);
        anyChange = anyChange || fv != cap.originalVelocity;
    }
    if (!anyChange)
    {
        repaint();
        return;
    }

    // Rewind the live preview, then commit once so undo captures the pre-gesture state.
    for (const auto& cap : captures)
    {
        if (cap.index >= 0 && cap.index < (int)pattern_.timelineNotes.size())
        {
            pattern_.timelineNotes[(size_t)cap.index].velocity = cap.originalVelocity;
        }
    }

    auto applyVelocities = [this, captures, finalVelocities]() -> bool {
        for (size_t k = 0; k < captures.size(); ++k)
        {
            const int idx = captures[k].index;
            if (idx < 0 || idx >= (int)pattern_.timelineNotes.size())
            {
                return false;
            }
            pattern_.timelineNotes[(size_t)idx].velocity = juce::jlimit(1, 127, finalVelocities[k]);
        }
        if (instrumentTrackController_ != nullptr)
        {
            instrumentTrackController_->notifyClipExperimentalMusicalTimingChanged();
        }
        repaint();
        return true;
    };

    if (undoablePatternEditHandler_ != nullptr && instrumentTrackController_ != nullptr && timelineClip_ != nullptr)
    {
        undoablePatternEditHandler_("Edit MIDI velocity", std::move(applyVelocities));
    }
    else
    {
        applyVelocities();
    }
}

int ExperimentalPianoRollView::effectiveAuditionChannelForNote(const TimelineMidiNote& tn) const noexcept
{
    return midi_channel_diag::effectiveChannel((int)tn.channel, trackMidiOutputChannel());
}

void ExperimentalPianoRollView::beginArrangedNoteAuditionGesture(const TimelineMidiNote& tn) noexcept
{
    if (player_ == nullptr)
    {
        return;
    }
    // Event audition, not playback: stored pitch/velocities + effective channel; the arranged
    // duration is deliberately ignored (spec D).
    const int channel = effectiveAuditionChannelForNote(tn);
    // Stage D: chase the clip's own CC automation at the note's position and deliver it BEFORE
    // the preview Note On (FIFO message-thread queue), with the same source-channel semantics as
    // transport playback. Before a stream's first point no value exists and nothing is sent.
    {
        const int trackOut = trackMidiOutputChannel();
        for (const auto& key : midi_cc::distinctStreams(pattern_.ccPoints))
        {
            if (const auto v =
                    midi_cc::valueAtTick(pattern_.ccPoints, key.controller, key.channel, tn.startTick))
            {
                player_->sendControllerChangeNow(
                    midi_channel_diag::effectiveChannel(key.channel, trackOut), key.controller, *v);
            }
        }
    }
    player_->beginArrangedNotePreview(tn.midiNote, tn.velocity, channel, tn.offVelocity);
    activeAuditionGestureKey_ = std::make_pair(channel, tn.midiNote);
    activeAuditionGestureIsKeyStrip_ = false;
}

void ExperimentalPianoRollView::beginKeyStripAuditionGesture(const int midiNote) noexcept
{
    if (player_ == nullptr)
    {
        return;
    }
    // Exact Mouse Down/Mouse Up manual test instrument (spec E): current toolbar Vel/Off values,
    // and the channel a newly created note on this source would get, under normal track output
    // semantics (`effectiveChannel(channelForNewNotes(out), out) == channelForNewNotes(out)`).
    int velocity = 100;
    int offVelocity = kDefaultMidiNoteOffVelocity;
    if (keyStripAuditionVelocityProvider_)
    {
        const auto [vel, off] = keyStripAuditionVelocityProvider_();
        velocity = vel;
        offVelocity = off;
    }
    const int channel = channelForNewlyCreatedNotes();
    player_->beginHeldKeyPreview(midiNote, velocity, channel, offVelocity);
    activeAuditionGestureKey_ = std::make_pair(channel, midiNote);
    activeAuditionGestureIsKeyStrip_ = true;
}

void ExperimentalPianoRollView::endActiveAuditionGestureOnMouseUp() noexcept
{
    if (!activeAuditionGestureKey_.has_value())
    {
        return;
    }
    const auto [channel, pitch] = *activeAuditionGestureKey_;
    activeAuditionGestureKey_.reset();
    activeAuditionGestureIsKeyStrip_ = false;
    if (player_ != nullptr)
    {
        player_->endNotePreview(pitch, channel);
    }
}

void ExperimentalPianoRollView::oneShotAuditionForCreatedNote(const TimelineMidiNote& tn) noexcept
{
    if (player_ != nullptr)
    {
        player_->oneShotArrangedPreview(
            tn.midiNote, tn.velocity, effectiveAuditionChannelForNote(tn), tn.offVelocity);
    }
}

void ExperimentalPianoRollView::maybeAuditionVelocityDrag(const bool force) noexcept
{
    if (player_ == nullptr || velocityDragCaptures_.empty() || !velocityDragAuditionSameStart_)
    {
        return;
    }
    const int primary = velocityDragPrimaryIndex_;
    if (primary < 0 || primary >= (int)pattern_.timelineNotes.size())
    {
        return;
    }
    const int primaryVelocity = pattern_.timelineNotes[(size_t)primary].velocity;
    const double nowMs = juce::Time::getMillisecondCounterHiRes();
    if (!force)
    {
        if (nowMs - velocityDragLastAuditionMs_ < kVelocityDragAuditionThrottleMs
            || primaryVelocity == velocityDragLastAuditionVelocity_)
        {
            return;
        }
    }
    velocityDragLastAuditionMs_ = nowMs;
    velocityDragLastAuditionVelocity_ = primaryVelocity;

    std::vector<ExperimentalMidiPatternPlayer::PreviewNoteRequest> chord;
    chord.reserve(velocityDragCaptures_.size());
    for (const auto& cap : velocityDragCaptures_)
    {
        if (cap.index < 0 || cap.index >= (int)pattern_.timelineNotes.size())
        {
            continue;
        }
        const auto& tn = pattern_.timelineNotes[(size_t)cap.index];
        chord.push_back({ tn.midiNote, tn.velocity, effectiveAuditionChannelForNote(tn), tn.offVelocity });
    }
    if (!chord.empty())
    {
        player_->previewNotesChord(chord);
    }
}

void ExperimentalPianoRollView::paintVelocityLane(juce::Graphics& g)
{
    // Minimized: only a small centered handle at the bottom edge (click restores, drag reopens).
    if (const auto knob = velocityLaneCollapsedKnobBounds(); !knob.isEmpty())
    {
        g.setColour(juce::Colour(0xff3a3a44));
        g.fillRoundedRectangle(knob.toFloat(), 3.0f);
        g.setColour(juce::Colours::white.withAlpha(0.35f));
        g.drawRoundedRectangle(knob.toFloat().reduced(0.5f), 3.0f, 1.0f);
        return;
    }

    const auto lane = velocityLaneBounds();
    const auto header = velocityLaneHeaderBounds();
    if (lane.isEmpty() && header.isEmpty())
    {
        return;
    }

    g.setColour(juce::Colour(0xff17171c));
    g.fillRect(lane);
    g.setColour(juce::Colour(0xff23232a));
    g.fillRect(header);
    g.setColour(juce::Colours::white.withAlpha(0.14f));
    g.drawHorizontalLine(lane.getY(), (float)juce::jmin(header.getX(), lane.getX()), (float)lane.getRight());

    // Resize grip affordance in the grab band at the lane's top edge. Horizontal centre matches the
    // minimized knob: both are centred on the editor area right of the key/name strip (the knob uses
    // gridBounds() and `lane` here excludes the side strip, so the two share one coordinate basis).
    if (const auto band = velocityLaneResizeBandBounds(); !band.isEmpty())
    {
        const int gripCentreX = !lane.isEmpty() ? lane.getCentreX() : band.getCentreX();
        g.setColour(juce::Colours::white.withAlpha(velocityLaneResizeActive_ ? 0.45f : 0.28f));
        g.fillRoundedRectangle((float)(gripCentreX - 18), (float)band.getY() + 2.0f, 36.0f, 3.0f, 1.5f);
    }

    if (header.getWidth() >= 46)
    {
        g.setColour(juce::Colours::white.withAlpha(0.6f));
        g.setFont(juce::Font(juce::FontOptions().withHeight(11.0f)));
        g.drawText("Velocity", header.reduced(5, 4), juce::Justification::topLeft, true);
    }

    if (lane.isEmpty())
    {
        return;
    }

    if (!velocityLaneEditingAvailable())
    {
        g.setColour(juce::Colours::white.withAlpha(0.3f));
        g.setFont(juce::Font(juce::FontOptions().withHeight(11.0f)));
        g.drawText("Velocity editing is available for timeline MIDI clips", lane,
                   juce::Justification::centred, true);
        return;
    }

    // Match the grid trim hint: shade lane time outside the clip's visible span.
    {
        const std::int64_t vis0 = timelineClip_->startSamples;
        const std::int64_t vis1 = vis0 + juce::jmax(std::int64_t{1}, timelineClip_->lengthSamples);
        float xL = xForSessionSample(vis0);
        float xR = xForSessionSample(vis1);
        if (xR < xL)
        {
            std::swap(xL, xR);
        }
        const float gx0 = (float)lane.getX();
        const float gx1 = (float)lane.getRight();
        const float bandL = juce::jlimit(gx0, gx1, xL);
        const float bandR = juce::jlimit(gx0, gx1, xR);
        g.setColour(juce::Colours::black.withAlpha(0.36f));
        if (bandL > gx0 + 0.5f)
        {
            g.fillRect(gx0, (float)lane.getY(), bandL - gx0, (float)lane.getHeight());
        }
        if (gx1 > bandR + 0.5f)
        {
            g.fillRect(bandR, (float)lane.getY(), gx1 - bandR, (float)lane.getHeight());
        }
    }

    const auto inner = velocityLaneInnerBounds();
    if (inner.getHeight() <= 0)
    {
        return;
    }

    // Unselected first, then selected on top so selected bars stay grabbable in dense overlaps.
    for (const bool selectedPass : { false, true })
    {
        for (int ti = 0; ti < (int)pattern_.timelineNotes.size(); ++ti)
        {
            const bool noteSelected = isTimelineNoteIndexSelected(ti);
            if (noteSelected != selectedPass)
            {
                continue;
            }
            const auto bxOpt = velocityBarCentreXForNoteIndex(ti);
            if (!bxOpt)
            {
                continue;
            }
            const float bx = *bxOpt;
            const float halfW = (float)kVelocityBarWidthPx * 0.5f;
            if (bx < (float)lane.getX() - halfW || bx > (float)lane.getRight() + halfW)
            {
                continue;
            }
            const int vel = juce::jlimit(1, 127, pattern_.timelineNotes[(size_t)ti].velocity);
            const float h = juce::jmax(2.0f, (float)vel / 127.0f * (float)inner.getHeight());
            const juce::Rectangle<float> bar(
                bx - halfW, (float)inner.getBottom() - h, (float)kVelocityBarWidthPx, h);
            const juce::Colour velC = colourForMidiVelocity(vel);
            if (noteSelected)
            {
                g.setColour(velC.withAlpha(0.95f));
                g.fillRect(bar);
                g.setColour(juce::Colours::white.withAlpha(0.85f));
                g.drawRect(bar, 1.1f);
            }
            else
            {
                g.setColour(velC.withAlpha(0.8f));
                g.fillRect(bar);
                g.setColour(velC.darker(0.6f).withAlpha(0.9f));
                g.drawRect(bar, 1.0f);
            }
        }
    }

    // Numeric readout for the grabbed bar while dragging.
    if (velocityLaneDragActive_ && velocityDragPrimaryIndex_ >= 0
        && velocityDragPrimaryIndex_ < (int)pattern_.timelineNotes.size())
    {
        if (const auto bxOpt = velocityBarCentreXForNoteIndex(velocityDragPrimaryIndex_))
        {
            const int vel = pattern_.timelineNotes[(size_t)velocityDragPrimaryIndex_].velocity;
            const juce::Rectangle<float> textR(*bxOpt - 16.0f, (float)lane.getY() + 1.0f, 32.0f, 12.0f);
            g.setColour(juce::Colours::white.withAlpha(0.92f));
            g.setFont(juce::Font(juce::FontOptions().withHeight(11.0f)));
            g.drawText(juce::String(vel), textR, juce::Justification::centred, false);
        }
    }
}

void ExperimentalPianoRollView::normalizeTimelineNoteSelection() noexcept
{
    for (auto it = selectedTimelineNoteIndices_.begin(); it != selectedTimelineNoteIndices_.end();)
    {
        if (*it < 0 || *it >= (int)pattern_.timelineNotes.size())
        {
            it = selectedTimelineNoteIndices_.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

void ExperimentalPianoRollView::clearTimelineNoteSelection() noexcept
{
    selectedTimelineNoteIndices_.clear();
}

void ExperimentalPianoRollView::replaceTimelineNoteSelectionWithSingle(const int noteIndex) noexcept
{
    selectedTimelineNoteIndices_.clear();
    selectedTimelineNoteIndices_.insert(noteIndex);
}

void ExperimentalPianoRollView::toggleTimelineNoteInSelection(const int noteIndex) noexcept
{
    if (selectedTimelineNoteIndices_.count(noteIndex) != 0u)
    {
        selectedTimelineNoteIndices_.erase(noteIndex);
    }
    else
    {
        selectedTimelineNoteIndices_.insert(noteIndex);
    }
}

bool ExperimentalPianoRollView::isTimelineNoteIndexSelected(const int noteIndex) const noexcept
{
    return selectedTimelineNoteIndices_.count(noteIndex) != 0u;
}

void ExperimentalPianoRollView::adjustTimelineNoteSelectionAfterErase(const int erasedIndex) noexcept
{
    std::unordered_set<int> out;
    for (const int j : selectedTimelineNoteIndices_)
    {
        if (j < erasedIndex)
        {
            out.insert(j);
        }
        else if (j > erasedIndex)
        {
            out.insert(j - 1);
        }
    }
    selectedTimelineNoteIndices_ = std::move(out);
}

void ExperimentalPianoRollView::selectTimelineNotesIntersecting(const juce::Rectangle<int>& r) noexcept
{
    selectedTimelineNoteIndices_.clear();
    if (timelineClip_ == nullptr || r.isEmpty())
    {
        return;
    }
    const auto rf = r.toFloat();
    for (int i = 0; i < (int)pattern_.timelineNotes.size(); ++i)
    {
        if (const auto b = getTimelineNoteVisualBounds(i))
        {
            if (b->intersects(rf))
            {
                selectedTimelineNoteIndices_.insert(i);
            }
        }
    }
}

void ExperimentalPianoRollView::beginMarqueeSelection(const juce::Point<int> localPos)
{
    clearTimelineNoteMovePending();
    clearTimelineNoteSelection();
    timelineMarqueeAnchor_ = localPos;
    timelineMarqueeRect_ = {};
    timelineMarqueeInteraction_ = TimelineMarqueeInteraction::Pending;
    repaint();
}

void ExperimentalPianoRollView::updateMarqueeSelection(const juce::MouseEvent& e)
{
    const auto gr = gridBounds();
    if (timelineMarqueeInteraction_ == TimelineMarqueeInteraction::Pending)
    {
        if (e.getDistanceFromDragStart() > kTimelineMarqueeDragThresholdPx)
        {
            timelineMarqueeInteraction_ = TimelineMarqueeInteraction::Dragging;
        }
    }
    if (timelineMarqueeInteraction_ == TimelineMarqueeInteraction::Dragging)
    {
        timelineMarqueeRect_
            = juce::Rectangle<int>(timelineMarqueeAnchor_, e.getPosition()).getIntersection(gr);
        repaint();
    }
}

void ExperimentalPianoRollView::finishMarqueeSelection()
{
    if (timelineMarqueeInteraction_ == TimelineMarqueeInteraction::Dragging)
    {
        selectTimelineNotesIntersecting(getNormalizedMarqueeRect());
        timelineMarqueeInteraction_ = TimelineMarqueeInteraction::None;
        timelineMarqueeRect_ = {};
        repaint();
    }
    else if (timelineMarqueeInteraction_ == TimelineMarqueeInteraction::Pending)
    {
        timelineMarqueeInteraction_ = TimelineMarqueeInteraction::None;
    }
}

juce::Rectangle<int> ExperimentalPianoRollView::getNormalizedMarqueeRect() const noexcept
{
    return timelineMarqueeRect_;
}

void ExperimentalPianoRollView::handleTimelineNotesMouseDown(const juce::MouseEvent& e)
{
    if (timelineClip_ == nullptr)
    {
        return;
    }
    if (!isTimelineClipBindingFresh())
    {
        juce::Logger::writeToLog("[MIDI roll] timeline note click ignored (stale clip binding)");
        return;
    }
    if (!e.mods.isLeftButtonDown())
    {
        return;
    }

    timelineMarqueeInteraction_ = TimelineMarqueeInteraction::None;

    const bool multiNoteSelectModifier
        = e.mods.isCtrlDown() || e.mods.isCommandDown() || e.mods.isShiftDown();
    // The second click of a double-click (delete gesture) must not re-audition the note.
    const bool auditionThisClick = e.getNumberOfClicks() <= 1;

    if (!multiNoteSelectModifier)
    {
        if (const auto edgeHit = findTimelineBarResizeEdgeAtPoint(e.getPosition()))
        {
            const int ni = edgeHit->first;
            if (selectedTimelineNoteIndices_.count(ni) == 0u)
            {
                replaceTimelineNoteSelectionWithSingle(ni);
            }
            if (auditionThisClick && ni >= 0 && ni < (int)pattern_.timelineNotes.size())
            {
                beginArrangedNoteAuditionGesture(pattern_.timelineNotes[(size_t)ni]);
            }
            beginTimelineNoteResizeGesture(ni, edgeHit->second);
            repaint();
            return;
        }
    }

    if (const auto hit = findTimelineNoteIndexAtPoint(e.getPosition()))
    {
        if (auditionThisClick && *hit >= 0 && *hit < (int)pattern_.timelineNotes.size())
        {
            beginArrangedNoteAuditionGesture(pattern_.timelineNotes[(size_t)*hit]);
        }
        if (multiNoteSelectModifier)
        {
            toggleTimelineNoteInSelection(*hit);
            clearTimelineNoteMovePending();
        }
        else
        {
            if (selectedTimelineNoteIndices_.count(*hit) == 0u)
            {
                replaceTimelineNoteSelectionWithSingle(*hit);
            }
            timelineMovePrimaryIndex_ = *hit;
            timelineMovePending_ = true;
        }
        repaint();
        return;
    }

    clearTimelineNoteMovePending();
    beginMarqueeSelection(e.getPosition());
}

void ExperimentalPianoRollView::tryAddTimelineNoteAtGridClick(const juce::Point<int> pos)
{
    if (timelineClip_ == nullptr)
    {
        return;
    }
    if (!isTimelineClipBindingFresh())
    {
        return;
    }

    const double sr = effectiveDeviceSampleRate(deviceManager_);
    const double bpm = pattern_.bpm > 0.0 ? pattern_.bpm : 120.0;
    const int tpq = experimentalEffectiveTicksPerQuarter(pattern_);

    const std::int64_t vis0 = timelineClip_->startSamples;
    const std::int64_t vis1 = vis0 + juce::jmax(std::int64_t{1}, timelineClip_->lengthSamples);

    const int pitch = pitchAtY(pos.getY());
    if (pitch < pitchLow_ || pitch > pitchHigh_)
    {
        return;
    }

    const std::int64_t absClick = sampleAtGridX((float)pos.getX());
    const std::int64_t oldAnchor = timelineClip_->timelineAnchorSamples;
    std::int64_t tickOffset = relativeSamplesToTicks(absClick - oldAnchor, bpm, tpq, sr);
    // Create uses floor-to-cell-start: the snap cell visually under the pointer is the target
    // (nearest-snap here made clicks past a cell midpoint land in the next cell).
    tickOffset = snapTimelineTickForCreate(tickOffset);
    const std::int64_t snapDur = musicalSnapGridTicks();

    const std::int64_t absSnappedNote = oldAnchor + ticksToSignedSamples(tickOffset, bpm, tpq, sr);
    if (absSnappedNote < vis0 || absSnappedNote >= vis1)
    {
        return;
    }

    // Toggle guard: a note already sitting exactly on the create target (tick + pitch + channel)
    // is deleted instead of stacked. This makes double-click in the same cell create/delete
    // consistently even when the painted glyph (narrow Hits diamond) misses the raw hit-test.
    const auto kCreateNoteChannel = (std::uint8_t)channelForNewlyCreatedNotes();
    for (int i = (int)pattern_.timelineNotes.size() - 1; i >= 0; --i)
    {
        const auto& tn = pattern_.timelineNotes[(size_t)i];
        if (tn.startTick != tickOffset || tn.midiNote != pitch || tn.channel != kCreateNoteChannel)
        {
            continue;
        }
        auto eraseAndNotify = [this, i]() -> bool {
            if (i < 0 || i >= (int)pattern_.timelineNotes.size())
            {
                return false;
            }
            pattern_.timelineNotes.erase(pattern_.timelineNotes.begin() + i);
            if (instrumentTrackController_ != nullptr && timelineClip_ != nullptr)
            {
                instrumentTrackController_->notifyClipExperimentalMusicalTimingChanged();
            }
            adjustTimelineNoteSelectionAfterErase(i);
            repaint();
            return true;
        };
        if (undoablePatternEditHandler_ && instrumentTrackController_ != nullptr && timelineClip_ != nullptr)
        {
            undoablePatternEditHandler_("Delete MIDI note", std::move(eraseAndNotify));
        }
        else
        {
            eraseAndNotify();
        }
        return;
    }

    auto commitSortedNotes = [this]() {
        std::sort(
            pattern_.timelineNotes.begin(), pattern_.timelineNotes.end(),
            [](const TimelineMidiNote& a, const TimelineMidiNote& b) noexcept {
                if (a.startTick != b.startTick)
                {
                    return a.startTick < b.startTick;
                }
                if (a.midiNote != b.midiNote)
                {
                    return a.midiNote < b.midiNote;
                }
                return a.channel < b.channel;
            });
    };

    if (tickOffset >= 0)
    {
        TimelineMidiNote nn;
        nn.midiNote = pitch;
        nn.velocity = 100;
        nn.channel = kCreateNoteChannel;
        nn.startTick = tickOffset;
        nn.durationTicks = snapDur > 0 ? snapDur : 240;

        if (currentEditCandidatesOverlap({}, {nn}, nullptr))
        {
            flashForbiddenNoDropCursor();
            return;
        }

        auto addAndNotify = [this, nn, commitSortedNotes]() mutable -> bool {
            pattern_.timelineNotes.push_back(nn);
            commitSortedNotes();

            if (instrumentTrackController_ != nullptr)
            {
                instrumentTrackController_->notifyClipExperimentalMusicalTimingChanged();
            }
            oneShotAuditionForCreatedNote(nn);
            repaint();
            return true;
        };
        if (undoablePatternEditHandler_ && instrumentTrackController_ != nullptr && timelineClip_ != nullptr)
        {
            undoablePatternEditHandler_("Add MIDI note", std::move(addAndNotify));
        }
        else
        {
            addAndNotify();
        }
        return;
    }

    const std::int64_t newAnchor = absSnappedNote;
    if (newAnchor >= oldAnchor || newAnchor < 0)
    {
        return;
    }
    const std::int64_t deltaShift = relativeSamplesToTicks(oldAnchor - newAnchor, bpm, tpq, sr);
    if (deltaShift < 1 || oldAnchor - newAnchor < 1)
    {
        return;
    }
    for (const auto& tn : pattern_.timelineNotes)
    {
        if (tn.startTick > std::numeric_limits<std::int64_t>::max() - deltaShift)
        {
            return;
        }
    }

    TimelineMidiNote nn;
    nn.midiNote = pitch;
    nn.velocity = 100;
    nn.channel = kCreateNoteChannel;
    nn.startTick = 0;
    nn.durationTicks = snapDur > 0 ? snapDur : 240;

    {
        std::vector<TimelineMidiNote> shifted = pattern_.timelineNotes;
        for (auto& tn : shifted)
        {
            tn.startTick += deltaShift;
        }
        if (!validateTimelineNotesNoOverlap(shifted, {}, {nn}, minTimelineNoteDurationTicks()).valid)
        {
            flashForbiddenNoDropCursor();
            return;
        }
    }

    auto rebaseAnchorAndAddNote = [this, newAnchor, deltaShift, nn, commitSortedNotes]() mutable -> bool {
        timelineClip_->timelineAnchorSamples = newAnchor;
        for (auto& tn : pattern_.timelineNotes)
        {
            tn.startTick += deltaShift;
        }
        pattern_.timelineNotes.push_back(nn);
        commitSortedNotes();

        if (instrumentTrackController_ != nullptr)
        {
            instrumentTrackController_->notifyClipExperimentalMusicalTimingChanged();
        }
        oneShotAuditionForCreatedNote(nn);
        repaint();
        return true;
    };

    if (undoablePatternEditHandler_ && instrumentTrackController_ != nullptr && timelineClip_ != nullptr)
    {
        undoablePatternEditHandler_("Add MIDI note", std::move(rebaseAnchorAndAddNote));
    }
    else
    {
        rebaseAnchorAndAddNote();
    }
}

void ExperimentalPianoRollView::setTransportGestureBlockPredicate(std::function<bool()> f) noexcept
{
    transportGestureBlock_ = std::move(f);
}

void ExperimentalPianoRollView::applyRulerSeekAtXInTrack(const float xInTrack, const float trackWidth) noexcept
{
    if (transportGestureBlock_ && transportGestureBlock_())
    {
        juce::Logger::writeToLog("[MIDI-roll ruler] seek ignored (recording or count-in)");
        return;
    }
    if (session_ == nullptr || transport_ == nullptr)
    {
        return;
    }
    const std::int64_t arr = session_->getArrangementExtentSamples();
    if (arr <= 0 || trackWidth <= 0.0f || samplesPerPixel_ <= 0.0 || !std::isfinite(samplesPerPixel_))
    {
        return;
    }
    const std::int64_t s =
        TimelineRulerView::xToSessionSampleClamped(xInTrack, trackWidth, visibleStartSamples_, samplesPerPixel_);
    const std::int64_t seekTarget = juce::jlimit(std::int64_t{0}, juce::jmax(std::int64_t{0}, arr), s);
    transport_->requestSeek(seekTarget);
    syncUiPlayheadAfterRulerSeek(seekTarget);
}

void ExperimentalPianoRollView::applyLeftLocatorRulerX(const float xInTrack, const float trackWidth) noexcept
{
    if (transportGestureBlock_ && transportGestureBlock_())
    {
        juce::Logger::writeToLog("[MIDI-roll ruler] Ctrl L locator edit ignored (recording or count-in)");
        return;
    }
    if (session_ == nullptr || transport_ == nullptr)
    {
        return;
    }
    const std::int64_t arr = session_->getArrangementExtentSamples();
    if (arr <= 0 || trackWidth <= 0.0f || samplesPerPixel_ <= 0.0 || !std::isfinite(samplesPerPixel_))
    {
        return;
    }
    std::int64_t s =
        TimelineRulerView::xToSessionSampleClamped(xInTrack, trackWidth, visibleStartSamples_, samplesPerPixel_);
    // Loop boundaries obey the shared arrangement snap state, exactly like the main-timeline ruler.
    if (deviceManager_ != nullptr)
    {
        s = snapSampleToGridIfEnabled(s,
                                      session_->getArrangementSnapSettings(),
                                      session_->getProjectMusicalTime(),
                                      effectiveDeviceSampleRate(deviceManager_));
    }
    const std::int64_t t = juce::jlimit(std::int64_t{0}, juce::jmax(std::int64_t{0}, arr), s);

    const std::int64_t oldR = session_->getRightLocatorSamples();
    session_->setLeftLocatorAtSample(t);
    const std::int64_t newR = session_->getRightLocatorSamples();
    const bool newValid = newR > t && newR > 0;
    if (oldR == 0 && newValid && !transport_->readCycleEnabledForUi())
    {
        transport_->requestCycleEnabled(true);
        juce::Logger::writeToLog("[Cycle] auto-enabled by L locator edit (first-creation: oldR==0) [MIDI roll]");
    }
    repaint();
}

void ExperimentalPianoRollView::applyRightLocatorRulerX(const float xInTrack, const float trackWidth) noexcept
{
    if (transportGestureBlock_ && transportGestureBlock_())
    {
        juce::Logger::writeToLog("[MIDI-roll ruler] Alt R locator edit ignored (recording or count-in)");
        return;
    }
    if (session_ == nullptr || transport_ == nullptr)
    {
        return;
    }
    const std::int64_t arr = session_->getArrangementExtentSamples();
    if (arr <= 0 || trackWidth <= 0.0f || samplesPerPixel_ <= 0.0 || !std::isfinite(samplesPerPixel_))
    {
        return;
    }
    std::int64_t s =
        TimelineRulerView::xToSessionSampleClamped(xInTrack, trackWidth, visibleStartSamples_, samplesPerPixel_);
    if (deviceManager_ != nullptr)
    {
        s = snapSampleToGridIfEnabled(s,
                                      session_->getArrangementSnapSettings(),
                                      session_->getProjectMusicalTime(),
                                      effectiveDeviceSampleRate(deviceManager_));
    }
    const std::int64_t t = juce::jlimit(std::int64_t{0}, juce::jmax(std::int64_t{0}, arr), s);

    const std::int64_t oldR = session_->getRightLocatorSamples();
    session_->setRightLocatorAtSample(t);
    const std::int64_t newL = session_->getLeftLocatorSamples();
    const bool newValid = t > newL && t > 0;
    if (oldR == 0 && newValid && !transport_->readCycleEnabledForUi())
    {
        transport_->requestCycleEnabled(true);
        juce::Logger::writeToLog("[Cycle] auto-enabled by R locator edit (first-creation: oldR==0) [MIDI roll]");
    }
    repaint();
}

void ExperimentalPianoRollView::tryToggleCycleFromRuler() noexcept
{
    if (transportGestureBlock_ && transportGestureBlock_())
    {
        juce::Logger::writeToLog("[Cycle] MIDI-roll toggle ignored (recording or count-in)");
        return;
    }
    if (transport_ == nullptr)
    {
        return;
    }
    transport_->requestCycleEnabled(!transport_->readCycleEnabledForUi());
    juce::Logger::writeToLog(juce::String{"[Cycle] "} + (transport_->readCycleEnabledForUi() ? "on" : "off")
                             + " [MIDI roll]");
    repaint();
}

void ExperimentalPianoRollView::maybeFollowViewportToAnchorSample(const double anchorSamples) noexcept
{
    if (!followPlayhead_ || !useAbsoluteTimeline())
    {
        return;
    }
    const auto gr = gridBounds();
    const double wpx = juce::jmax(1.0, (double)juce::jmax(1, gr.getWidth()));
    if (samplesPerPixel_ <= 0.0 || !std::isfinite(samplesPerPixel_))
    {
        return;
    }
    const double spanSamples = wpx * samplesPerPixel_;
    const double rel = spanSamples > 1e-9 ? (anchorSamples - (double)visibleStartSamples_) / spanSamples : 0.5;

    std::int64_t targetStart = visibleStartSamples_;
    bool needScroll = false;
    if (rel >= kFollowRightThreshold)
    {
        needScroll = true;
        targetStart = (std::int64_t)std::llround(anchorSamples - kFollowForwardResetPosition * spanSamples);
    }
    else if (rel <= kFollowLeftThreshold)
    {
        needScroll = true;
        targetStart = (std::int64_t)std::llround(anchorSamples - kFollowBackwardResetPosition * spanSamples);
    }

    if (!needScroll)
    {
        return;
    }
    const std::int64_t clamped = juce::jmax(std::int64_t{0}, targetStart);
    if (clamped != visibleStartSamples_)
    {
        visibleStartSamples_ = clamped;
        syncViewportToBoundClip();
        sessionTransportSnapshotValid_ = false;
        // Explicit single-shot move (seek/toggle/hard resync) bypasses the gates by design, but
        // still registers locally and globally so frame-driven paging pauses right afterwards.
        const double nowMs = juce::Time::getMillisecondCounterHiRes();
        followGovernor_.notePageApplied(nowMs, anchorSamples, (double)clamped, spanSamples);
        GlobalFollowWorkCoordinator::instance().notePageApplied(nowMs);
        repaint();
    }
}

void ExperimentalPianoRollView::noteUserRollViewportGesture() noexcept
{
    const double nowMs = juce::Time::getMillisecondCounterHiRes();
    followGovernor_.noteUserViewportChange(nowMs);
    GlobalFollowWorkCoordinator::instance().noteUserViewportGesture(this, nowMs);
}

bool ExperimentalPianoRollView::followUiWorthUpdating() const noexcept
{
    if (!isShowing())
    {
        return false;
    }
    if (const juce::ComponentPeer* const peer = getPeer(); peer != nullptr && peer->isMinimised())
    {
        return false;
    }
    return true;
}

/// One `playback-ui-load.log` line per ~2 s while playing (compiled out unless
/// `MINIDAW_DIAG_PLAYBACK_UI_LOAD` is 1): roll follow pages + per-reason skips, span, frame lateness.
void ExperimentalPianoRollView::maybeLogFollowDiagnostics(const double nowMs, const bool transportPlaying) noexcept
{
#if MINIDAW_DIAG_PLAYBACK_UI_LOAD
    if (!transportPlaying)
    {
        return;
    }
    if (lastFollowDiagLogMs_ > 0.0 && nowMs - lastFollowDiagLogMs_ < 2000.0)
    {
        return;
    }
    lastFollowDiagLogMs_ = nowMs;
    const auto gr = gridBounds();
    const double span = juce::jmax(1.0, (double)juce::jmax(1, gr.getWidth())) * samplesPerPixel_;
    const juce::ComponentPeer* const peer = getPeer();
    appendPlaybackUiLoadDiagnosticLine(
        juce::String("roll follow.on=") + (followPlayhead_ ? "1" : "0")
        + " follow.pages=" + juce::String((int)statsFollowPans_)
        + " follow.skip.gesture=" + juce::String((int)statsFollowSkipsGesture_)
        + " follow.skip.late=" + juce::String((int)statsFollowSkipsLateFrame_)
        + " follow.skip.clean=" + juce::String((int)statsFollowSkipsAwaitClean_)
        + " follow.skip.boundary=" + juce::String((int)statsFollowSkipsBoundary_)
        + " follow.skip.pace=" + juce::String((int)statsFollowSkipsPacing_)
        + " follow.skip.xwin=" + juce::String((int)statsFollowSkipsCrossWindow_)
        + " follow.skip.budget=" + juce::String((int)statsFollowSkipsGlobalBudget_)
        + " follow.skip.hidden=" + juce::String((int)statsFollowSkipsHidden_)
        + " follow.span=" + juce::String(span, 0)
        + " follow.frameMs=" + juce::String(followGovernor_.lastFrameIntervalMs(), 1)
        + " roll.focused=" + ((peer != nullptr && peer->isFocused()) ? "1" : "0"));
    statsFollowPans_ = 0;
    statsFollowSkipsGesture_ = 0;
    statsFollowSkipsLateFrame_ = 0;
    statsFollowSkipsAwaitClean_ = 0;
    statsFollowSkipsBoundary_ = 0;
    statsFollowSkipsPacing_ = 0;
    statsFollowSkipsCrossWindow_ = 0;
    statsFollowSkipsGlobalBudget_ = 0;
    statsFollowSkipsHidden_ = 0;
#else
    juce::ignoreUnused(nowMs, transportPlaying);
#endif
}

void ExperimentalPianoRollView::syncUiPlayheadAfterRulerSeek(const std::int64_t seekTargetSamples) noexcept
{
    uiRulerSeekDisplayHold_ = seekTargetSamples;
    const double wall = juce::Time::getMillisecondCounterHiRes() * 0.001;
    uiPlayheadDisplaySamples_ = (double)seekTargetSamples;
    uiPlayheadExtrapBaseSample_ = (double)seekTargetSamples;
    uiPlayheadExtrapWallSec_ = wall;
    if (transport_ != nullptr)
    {
        uiPlayheadLastRawPh_ = transport_->readPlayheadSamplesForUi();
    }
    lastObservedPlayheadUi_ = seekTargetSamples;
    sessionTransportSnapshotValid_ = true;
    lastOffscreenGatePlayheadInView_ = true;
    maybeFollowViewportToAnchorSample((double)seekTargetSamples);
    repaint();
}

void ExperimentalPianoRollView::handleTimelineRulerMouseDown(const juce::MouseEvent& e,
                                                             const juce::Rectangle<int>& rt)
{
    rulerGestureMode_ = RulerGestureMode::None;
    const float h = (float)rt.getHeight();
    const float yRel = (float)e.position.getY() - (float)rt.getY();
    const bool upperHalf = h > 0.f && yRel < h * 0.5f;
    const float w = (float)rt.getWidth();
    const float xInTrack = (float)e.position.getX() - (float)rt.getX();

    if (e.mods.isAltDown())
    {
        applyRightLocatorRulerX(xInTrack, w);
        rulerGestureMode_ = RulerGestureMode::RightLocator;
    }
    else if (e.mods.isCtrlDown())
    {
        applyLeftLocatorRulerX(xInTrack, w);
        rulerGestureMode_ = RulerGestureMode::LeftLocator;
    }
    else if (upperHalf)
    {
        tryToggleCycleFromRuler();
    }
    else
    {
        applyRulerSeekAtXInTrack(xInTrack, w);
        rulerGestureMode_ = RulerGestureMode::Seek;
    }
}

void ExperimentalPianoRollView::handleTimelineRulerMouseDrag(const juce::MouseEvent& e,
                                                             const juce::Rectangle<int>& rt)
{
    if (rulerGestureMode_ == RulerGestureMode::None)
    {
        return;
    }
    const float h = (float)rt.getHeight();
    const float yRel = (float)e.position.getY() - (float)rt.getY();
    const bool lowerHalf = h > 0.f && yRel >= h * 0.5f;
    const float w = (float)juce::jmax(1, rt.getWidth());
    const float xRaw = (float)e.position.getX() - (float)rt.getX();
    const float xInTrack = juce::jlimit(0.f, w - 1.f, xRaw);

    if (rulerGestureMode_ == RulerGestureMode::RightLocator)
    {
        applyRightLocatorRulerX(xInTrack, w);
    }
    else if (rulerGestureMode_ == RulerGestureMode::LeftLocator)
    {
        applyLeftLocatorRulerX(xInTrack, w);
    }
    else if (rulerGestureMode_ == RulerGestureMode::Seek && lowerHalf)
    {
        applyRulerSeekAtXInTrack(xInTrack, w);
    }
}

void ExperimentalPianoRollView::mouseDown(const juce::MouseEvent& e)
{
    const auto pos = e.getPosition();
    if (velocityValueEditor_ != nullptr)
    {
        // Any click outside the popup cancels it (the popup itself is a child and never gets here).
        dismissVelocityValueEditor(false);
    }
    if (e.mods.isMiddleButtonDown())
    {
        // Middle-button hand-pan (grab-style); never selects/creates/edits notes.
        if (useAbsoluteTimeline())
        {
            if (!hasValidViewportState())
            {
                seedViewportFromMainTimelineOrFallback();
            }
            if (hasValidViewportState())
            {
                middlePanActive_ = true;
                middlePanLastX_ = e.position.x;
            }
        }
        return;
    }
    if (useAbsoluteTimeline() && timelineClip_ != nullptr && instrumentTrackController_ != nullptr
        && !isTimelineClipBindingFresh())
    {
        juce::Logger::writeToLog("[MIDI roll] mouseDown ignored (stale clip binding)");
        return;
    }

    const auto kb = keyboardBounds();
    if (!kb.isEmpty() && kb.contains(pos) && rowLabelMode_ == 2 && e.mods.isPopupMenu() && onCommitRowLabelEdit_)
    {
        const int pitch = pitchAtY(pos.getY());
        if (pitch >= pitchLow_ && pitch <= pitchHigh_)
        {
            juce::PopupMenu menu;
            menu.addItem(1, "Reset to default name");
            juce::Component::SafePointer<ExperimentalPianoRollView> st(this);
            menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this),
                               [st, pitch](const int r) {
                                   if (st == nullptr || r != 1 || !st->onCommitRowLabelEdit_)
                                   {
                                       return;
                                   }
                                   st->onCommitRowLabelEdit_(pitch, {});
                                   st->repaint();
                               });
        }
        return;
    }

    if (!kb.isEmpty() && kb.contains(pos) && e.mods.isLeftButtonDown() && !e.mods.isPopupMenu())
    {
        // Piano-key / drum-name row press: exact Mouse Down/Mouse Up audition with the toolbar's
        // current Vel/Off values (spec E). No note is created and timeline events are untouched.
        const int pitch = pitchAtY(pos.getY());
        if (pitch >= pitchLow_ && pitch <= pitchHigh_)
        {
            beginKeyStripAuditionGesture(pitch);
        }
        return;
    }

    const auto rt = rulerTrackBounds();
    if (useAbsoluteTimeline() && !rt.isEmpty() && rt.contains(pos))
    {
        timelineMarqueeInteraction_ = TimelineMarqueeInteraction::None;
        handleTimelineRulerMouseDown(e, rt);
        return;
    }

    // CC lane first: it sits at the very bottom, below the velocity lane.
    const auto ccResizeBand = ccLaneResizeBandBounds();
    const auto ccKnob = ccLaneCollapsedKnobBounds();
    if ((!ccResizeBand.isEmpty() && ccResizeBand.contains(pos))
        || (!ccKnob.isEmpty() && ccKnob.contains(pos)))
    {
        if (!e.mods.isPopupMenu())
        {
            timelineMarqueeInteraction_ = TimelineMarqueeInteraction::None;
            ccLaneResizeActive_ = true;
            ccLaneResizeFromCollapsedKnob_ = ccLaneTotalHeight() <= 0;
            ccLaneResizeAnchorY_ = pos.getY();
            ccLaneResizeAnchorHeight_ = ccLaneTotalHeight();
        }
        return;
    }
    const auto ccHeader = ccLaneHeaderBounds();
    if (!ccHeader.isEmpty() && ccHeader.contains(pos))
    {
        timelineMarqueeInteraction_ = TimelineMarqueeInteraction::None;
        showCcControllerMenu();
        return;
    }
    const auto ccLane = ccLaneBounds();
    if (!ccLane.isEmpty() && ccLane.contains(pos))
    {
        timelineMarqueeInteraction_ = TimelineMarqueeInteraction::None;
        handleCcLaneMouseDown(e);
        return;
    }

    const auto laneResizeBand = velocityLaneResizeBandBounds();
    const auto laneCollapsedKnob = velocityLaneCollapsedKnobBounds();
    if ((!laneResizeBand.isEmpty() && laneResizeBand.contains(pos))
        || (!laneCollapsedKnob.isEmpty() && laneCollapsedKnob.contains(pos)))
    {
        if (!e.mods.isPopupMenu())
        {
            timelineMarqueeInteraction_ = TimelineMarqueeInteraction::None;
            velocityLaneResizeActive_ = true;
            velocityLaneResizeFromCollapsedKnob_ = velocityLaneTotalHeight() <= 0;
            velocityLaneResizeAnchorY_ = pos.getY();
            velocityLaneResizeAnchorHeight_ = velocityLaneTotalHeight();
        }
        return;
    }

    const auto velLane = velocityLaneBounds();
    if (!velLane.isEmpty() && velLane.contains(pos))
    {
        timelineMarqueeInteraction_ = TimelineMarqueeInteraction::None;
        handleVelocityLaneMouseDown(e);
        return;
    }

    const auto gr = gridBounds();

    if (gr.contains(pos))
    {
        if (useAbsoluteTimeline() && timelineClip_ != nullptr)
        {
            if (e.mods.isPopupMenu())
            {
                // Right-click on a note: exact-velocity popup. Empty grid right-click does nothing
                // (reserved for a future context menu).
                if (const auto hit = findTimelineNoteIndexAtPoint(pos))
                {
                    beginVelocityValueEdit(*hit, pos);
                }
                return;
            }
            handleTimelineNotesMouseDown(e);
            return;
        }
        // Unbound editor (no clip): nothing to edit.
        timelineMarqueeInteraction_ = TimelineMarqueeInteraction::None;
    }
}

void ExperimentalPianoRollView::mouseDrag(const juce::MouseEvent& e)
{
    if (middlePanActive_)
    {
        if (samplesPerPixel_ > 0.0 && std::isfinite(samplesPerPixel_))
        {
            const float dx = e.position.x - middlePanLastX_;
            // Grab-style: content follows the mouse (drag right shows earlier time). Anchor only
            // advances when at least one sample was consumed, so sub-pixel motion accumulates.
            const std::int64_t step = (std::int64_t)std::llround((double)dx * samplesPerPixel_);
            if (step != 0)
            {
                middlePanLastX_ = e.position.x;
                visibleStartSamples_ = juce::jmax(std::int64_t{0}, visibleStartSamples_ - step);
                sessionTransportSnapshotValid_ = false;
                noteUserRollViewportGesture();
                syncViewportToBoundClip();
                rollViewportRepaintFlush_.requestFlush();
            }
        }
        return;
    }
    if (velocityLaneResizeActive_)
    {
        // Dragging up (smaller y) grows the lane; snap unusably small heights to fully minimized.
        int newHeight = velocityLaneResizeAnchorHeight_ + (velocityLaneResizeAnchorY_ - e.getPosition().getY());
        if (newHeight < kVelocityLaneMinUsableHeight)
        {
            newHeight = 0;
        }
        velocityLaneHeightPref_ = juce::jlimit(0, maxVelocityLaneHeightNow(), newHeight);
        clampPitchScrollOffset();
        resized();
        repaint();
        return;
    }

    if (ccLaneResizeActive_)
    {
        int newHeight = ccLaneResizeAnchorHeight_ + (ccLaneResizeAnchorY_ - e.getPosition().getY());
        if (newHeight < kCcLaneMinUsableHeight)
        {
            newHeight = 0;
        }
        ccLaneHeightPref_ = juce::jlimit(0, maxCcLaneHeightNow(), newHeight);
        clampPitchScrollOffset();
        resized();
        repaint();
        return;
    }

    if (ccPointDragActive_)
    {
        updateCcLaneDrag(e.getPosition());
        repaint();
        return;
    }

    if (velocityLaneDragActive_)
    {
        updateVelocityLaneDrag(e.getPosition());
        maybeAuditionVelocityDrag(false);
        repaint();
        return;
    }

    if (timelineNoteResizeActive_)
    {
        updateTimelineNoteResizeGesture(e.getPosition());
        updateTimelineNoteEditCursor();
        repaint();
        return;
    }

    if (timelineNoteMoveActive_)
    {
        updateTimelineNoteMoveGesture(e.getPosition());
        updateTimelineNoteEditCursor();
        repaint();
        return;
    }

    if (timelineMovePending_ && useAbsoluteTimeline() && timelineClip_ != nullptr
        && isTimelineClipBindingFresh())
    {
        if (e.getDistanceFromDragStart() > kTimelineMarqueeDragThresholdPx)
        {
            beginTimelineNoteMoveGesture(e);
            if (timelineNoteMoveActive_)
            {
                updateTimelineNoteMoveGesture(e.getPosition());
                updateTimelineNoteEditCursor();
                repaint();
                return;
            }
        }
    }

    if (useAbsoluteTimeline() && timelineClip_ != nullptr
        && isTimelineClipBindingFresh())
    {
        updateMarqueeSelection(e);
        if (timelineMarqueeInteraction_ == TimelineMarqueeInteraction::Dragging)
        {
            return;
        }
    }

    const auto rt = rulerTrackBounds();
    if (!useAbsoluteTimeline() || rt.isEmpty() || rulerGestureMode_ == RulerGestureMode::None)
    {
        return;
    }
    if (timelineClip_ != nullptr && instrumentTrackController_ != nullptr
        && !isTimelineClipBindingFresh())
    {
        return;
    }
    handleTimelineRulerMouseDrag(e, rt);
}

void ExperimentalPianoRollView::mouseUp(const juce::MouseEvent& e)
{
    // FIRST, before any early return: Mouse Up ends the active audition gesture. This is what
    // schedules the arranged-note Note Off at max(Note On + 850 ms, now) and releases piano-key /
    // drum-row previews exactly on Mouse Up. Missing this call was the Phase B.1 integration
    // regression: previews stayed "held" forever (drainDue skips held notes by design), so only
    // the focus-loss cleanup ever sent Note Off. No-op when no gesture is active.
    endActiveAuditionGestureOnMouseUp();

    if (middlePanActive_)
    {
        middlePanActive_ = false;
        return;
    }
    if (velocityLaneResizeActive_)
    {
        // A plain click on the minimized knob (no real drag) restores the default lane height.
        const bool restoreDefault = velocityLaneResizeFromCollapsedKnob_
                                    && e.getDistanceFromDragStart() < 4
                                    && velocityLaneTotalHeight() <= 0;
        velocityLaneResizeActive_ = false;
        velocityLaneResizeFromCollapsedKnob_ = false;
        if (restoreDefault)
        {
            velocityLaneHeightPref_ = kVelocityLaneHeight;
            clampPitchScrollOffset();
            resized();
        }
        repaint();
        return;
    }

    if (ccLaneResizeActive_)
    {
        const bool restoreDefault = ccLaneResizeFromCollapsedKnob_
                                    && e.getDistanceFromDragStart() < 4 && ccLaneTotalHeight() <= 0;
        ccLaneResizeActive_ = false;
        ccLaneResizeFromCollapsedKnob_ = false;
        if (restoreDefault)
        {
            ccLaneHeightPref_ = kCcLaneHeight;
            clampPitchScrollOffset();
            resized();
        }
        repaint();
        return;
    }

    finishCcLaneDragGesture();
    finishVelocityLaneDragGesture();
    finishTimelineNoteMoveGesture();
    finishTimelineNoteResizeGesture();
    finishMarqueeSelection();
    if (!timelineNoteMoveActive_)
    {
        clearTimelineNoteMovePending();
    }

    juce::ignoreUnused(e);
    rulerGestureMode_ = RulerGestureMode::None;
}

void ExperimentalPianoRollView::mouseDoubleClick(const juce::MouseEvent& e)
{
    const auto pos = e.getPosition();
    if (useAbsoluteTimeline() && timelineClip_ != nullptr && instrumentTrackController_ != nullptr
        && !isTimelineClipBindingFresh())
    {
        return;
    }

    const auto kb = keyboardBounds();
    if (!kb.isEmpty() && kb.contains(pos) && rowLabelMode_ == 2)
    {
        const int pitch = pitchAtY(pos.getY());
        if (pitch >= pitchLow_ && pitch <= pitchHigh_)
        {
            beginRowLabelInlineEdit(pitch);
        }
        return;
    }

    const auto gr = gridBounds();
    if (gr.contains(pos) && useAbsoluteTimeline() && timelineClip_ != nullptr
        && isTimelineClipBindingFresh())
    {
        if (const auto hit = findTimelineNoteIndexAtPoint(pos))
        {
            const int i = *hit;
            auto eraseAndNotify = [this, i]() -> bool {
                if (i < 0 || i >= (int)pattern_.timelineNotes.size())
                {
                    return false;
                }
                pattern_.timelineNotes.erase(pattern_.timelineNotes.begin() + i);
                if (instrumentTrackController_ != nullptr && timelineClip_ != nullptr)
                {
                    instrumentTrackController_->notifyClipExperimentalMusicalTimingChanged();
                }
                adjustTimelineNoteSelectionAfterErase(i);
                repaint();
                return true;
            };
            if (undoablePatternEditHandler_ && instrumentTrackController_ != nullptr && timelineClip_ != nullptr)
            {
                undoablePatternEditHandler_("Delete MIDI note", std::move(eraseAndNotify));
            }
            else
            {
                eraseAndNotify();
            }
            return;
        }

        tryAddTimelineNoteAtGridClick(pos);
        return;
    }
}

void ExperimentalPianoRollView::mouseMove(const juce::MouseEvent& e)
{
    if (timelineNoteResizeActive_ || timelineNoteMoveActive_)
    {
        updateTimelineNoteEditCursor();
        setTooltip(juce::String{});
        return;
    }

    if (velocityLaneDragActive_ || velocityLaneResizeActive_)
    {
        setMouseCursor(juce::MouseCursor::UpDownResizeCursor);
        setTooltip(juce::String{});
        return;
    }

    {
        const auto laneResizeBand = velocityLaneResizeBandBounds();
        const auto laneCollapsedKnob = velocityLaneCollapsedKnobBounds();
        if ((!laneResizeBand.isEmpty() && laneResizeBand.contains(e.getPosition()))
            || (!laneCollapsedKnob.isEmpty() && laneCollapsedKnob.contains(e.getPosition())))
        {
            setMouseCursor(juce::MouseCursor::UpDownResizeCursor);
            setTooltip(juce::String{});
            return;
        }
    }

    {
        const auto velLane = velocityLaneBounds();
        if (!velLane.isEmpty() && velLane.contains(e.getPosition()) && velocityLaneEditingAvailable())
        {
            const int x = e.getPosition().getX();
            const bool nearBar = findVelocityBarIndexNearX(x, true).has_value()
                                 || findVelocityBarIndexNearX(x, false).has_value();
            setMouseCursor(nearBar ? juce::MouseCursor::UpDownResizeCursor
                                   : juce::MouseCursor::NormalCursor);
            setTooltip(juce::String{});
            return;
        }
    }

    {
        const auto kb = keyboardBounds();
        if (!kb.isEmpty() && kb.contains(e.getPosition()))
        {
            const int pitch = pitchAtY(e.getPosition().getY());
            if (pitch >= pitchLow_ && pitch <= pitchHigh_)
            {
                if (rowLabelMode_ == 2 && rowLabelTooltipProvider_)
                {
                    const juce::String tip = rowLabelTooltipProvider_(pitch);
                    if (tip.isNotEmpty())
                    {
                        setTooltip(tip);
                        return;
                    }
                }
                // Note name + raw MIDI number, e.g. `C3 (60)` — Cubase octave convention
                // (middle C = C3), useful when checking boundary rows C-2 / G8.
                setTooltip(juce::MidiMessage::getMidiNoteName(pitch, true, true, 3) + " ("
                           + juce::String(pitch) + ")");
                return;
            }
        }
    }
    const auto gr = gridBounds();
    if (gr.contains(e.getPosition()) && timelineBarsResizeEnabled()
        && findTimelineBarResizeEdgeAtPoint(e.getPosition()))
    {
        setMouseCursor(juce::MouseCursor::LeftRightResizeCursor);
        setTooltip(juce::String{});
        return;
    }
    setMouseCursor(juce::MouseCursor::NormalCursor);
    setTooltip(juce::String{});
}

void ExperimentalPianoRollView::mouseExit(const juce::MouseEvent& e)
{
    juce::ignoreUnused(e);
    setMouseCursor(juce::MouseCursor::NormalCursor);
    setTooltip(juce::String{});
}

void ExperimentalPianoRollView::mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    const auto pos = e.getPosition();
    const auto kb = keyboardBounds();
    const auto gr = gridBounds();
    const auto rc = rulerCornerBounds();
    const auto rt = rulerTrackBounds();

    const bool inKeysOrGrid = kb.contains(pos) || gr.contains(pos);
    const bool inRulerChrome = (!rc.isEmpty() && rc.contains(pos)) || (!rt.isEmpty() && rt.contains(pos));

    // Match main timeline (TrackLanesView / TimelineRulerView): wheel delta sign + optional platform invert.
    const double d = (wheel.isReversed ? -(double)wheel.deltaY : (double)wheel.deltaY);

    if (e.mods.isCtrlDown() && useAbsoluteTimeline())
    {
        if (timelineClip_ != nullptr && instrumentTrackController_ != nullptr && !isTimelineClipBindingFresh())
        {
            return;
        }
        if (!hasValidViewportState())
        {
            seedViewportFromMainTimelineOrFallback();
        }
        if ((inKeysOrGrid || inRulerChrome) && samplesPerPixel_ > 0.0 && std::isfinite(samplesPerPixel_)
            && std::abs(d) > 1.0e-9)
        {
            const double factor = std::pow(0.85, d);
            const float x = (float)e.position.getX();
            const float ox = (float)gr.getX();
            const std::int64_t sAtPointer = sampleAtGridX(x);
            const double spp1 = juce::jlimit(0.25, 1.0e7, samplesPerPixel_ * factor);
            visibleStartSamples_
                = sAtPointer - (std::int64_t)std::llround((double)(x - ox) * spp1);
            visibleStartSamples_ = juce::jmax(std::int64_t{0}, visibleStartSamples_);
            samplesPerPixel_ = spp1;
            sessionTransportSnapshotValid_ = false;
            noteUserRollViewportGesture();
            syncViewportToBoundClip();
            rollViewportRepaintFlush_.requestFlush();
        }
        return;
    }

    if (e.mods.isShiftDown() && useAbsoluteTimeline())
    {
        if (timelineClip_ != nullptr && instrumentTrackController_ != nullptr && !isTimelineClipBindingFresh())
        {
            return;
        }
        if (!hasValidViewportState())
        {
            seedViewportFromMainTimelineOrFallback();
        }
        if (samplesPerPixel_ > 0.0 && std::isfinite(samplesPerPixel_) && (inKeysOrGrid || inRulerChrome))
        {
            const double panPx = (double)wheel.deltaY * 32.0;
            visibleStartSamples_ = juce::jmax(
                std::int64_t{0},
                visibleStartSamples_ + (std::int64_t)std::llround(panPx * samplesPerPixel_));
            sessionTransportSnapshotValid_ = false;
            noteUserRollViewportGesture();
            syncViewportToBoundClip();
            rollViewportRepaintFlush_.requestFlush();
        }
        return;
    }

    if (inKeysOrGrid && std::abs(wheel.deltaY) > 1.0e-6f)
    {
        pitchWheelScrollRemainder_ += wheel.deltaY * kPitchScrollRowsPerWheelDelta;
        const int rowSteps = (int)std::trunc((double)pitchWheelScrollRemainder_);
        pitchWheelScrollRemainder_ -= (float)rowSteps;
        pitchScrollOffsetRows_ -= rowSteps;
        clampPitchScrollOffset();
        resized();
        // Coalesced: one full-roll dirty-marking per message batch (repaint-storm fix).
        rollViewportRepaintFlush_.requestFlush();
        return;
    }

    if (!useAbsoluteTimeline())
    {
        return;
    }
    if (timelineClip_ != nullptr && instrumentTrackController_ != nullptr && !isTimelineClipBindingFresh())
    {
        return;
    }
    if (!hasValidViewportState())
    {
        seedViewportFromMainTimelineOrFallback();
    }
    if (!inRulerChrome || samplesPerPixel_ <= 0.0 || !std::isfinite(samplesPerPixel_))
    {
        return;
    }
    const float x = (float)e.position.getX();
    const float ox = (float)gr.getX();
    const std::int64_t sAtPointer = sampleAtGridX(x);

    const double factor = wheel.deltaY > 0 ? 0.92 : 1.08;
    const double spp1 = juce::jlimit(0.25, 1.0e7, samplesPerPixel_ * factor);
    visibleStartSamples_
        = sAtPointer - (std::int64_t)std::llround((double)(x - ox) * spp1);
    visibleStartSamples_ = juce::jmax(std::int64_t{0}, visibleStartSamples_);
    samplesPerPixel_ = spp1;
    sessionTransportSnapshotValid_ = false;
    noteUserRollViewportGesture();
    syncViewportToBoundClip();
    rollViewportRepaintFlush_.requestFlush();
}

void ExperimentalPianoRollView::paint(juce::Graphics& g)
{
    const auto kb = keyboardBounds();
    const auto gr = gridBounds();
    const auto rulerCorner = rulerCornerBounds();
    const auto rulerTrack = rulerTrackBounds();

    const bool absTime = useAbsoluteTimeline();

    if (absTime && timelineClip_ != nullptr && instrumentTrackController_ != nullptr
        && !isTimelineClipBindingFresh())
    {
        g.fillAll(juce::Colour(0xff1a1a1e));
        return;
    }

    normalizeTimelineNoteSelection();

    if (absTime)
    {
        if (!hasValidViewportState())
        {
            seedViewportFromMainTimelineOrFallback();
        }
    }

    const int topP = topVisiblePitch();
    const int nVisRows = countVisiblePitchRows();
    const int bottomP = topP - (nVisRows - 1);
    const int paintHi = juce::jmin(pitchHigh_, topP);
    const int paintLo = juce::jmax(pitchLow_, bottomP);

    // --- Base + grid rows (gridBounds only; never tint the ruler strip)
    g.fillAll(juce::Colour(0xff1a1a1e));
    for (int pitch = paintHi; pitch >= paintLo; --pitch)
    {
        const auto rrOpt = visibleRowStripRect(gr, pitch);
        if (!rrOpt)
        {
            continue;
        }
        const auto rr = *rrOpt;
        if (isBlackKey(pitch))
        {
            // Black-key rows: darker band (swapped from previous mapping where black appeared lighter).
            g.setColour(juce::Colour(0xff1f1f26));
        }
        else
        {
            g.setColour(juce::Colour(0xff25252d));
        }
        g.fillRect(rr);
    }

    // --- Trim hint: darken piano-roll grid outside the clip's visible/playable span so trimmed-away
    // time is obvious (notes and playback are already culled; this is presentation only).
    if (absTime && timelineClip_ != nullptr && samplesPerPixel_ > 0.0 && std::isfinite(samplesPerPixel_)
        && timelineClip_->lengthSamples > 0)
    {
        const float gx0 = (float)gr.getX();
        const float gx1 = (float)gr.getRight();
        const float gy = (float)gr.getY();
        const float gh = (float)gr.getHeight();
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

        constexpr float kOutsideVisibleClipShadeAlpha = 0.36f;
        const juce::Colour shade = juce::Colours::black.withAlpha(kOutsideVisibleClipShadeAlpha);

        if (bandL > gx0 + 0.5f)
        {
            g.setColour(shade);
            g.fillRect(gx0, gy, bandL - gx0, gh);
        }
        if (gx1 > bandR + 0.5f)
        {
            g.setColour(shade);
            g.fillRect(bandR, gy, gx1 - bandR, gh);
        }
    }

    // --- Ruler chrome (always when absolute timeline; independent of cycle/selection/clip overlay)
    if (absTime && !rulerCorner.isEmpty())
    {
        g.setColour(juce::Colour(0xff1e1e24));
        g.fillRect(rulerCorner);
    }
    if (absTime && !rulerTrack.isEmpty())
    {
        g.setColour(juce::Colour(0xff1e1e24));
        g.fillRect(rulerTrack);
        g.setColour(juce::Colours::white.withAlpha(0.12f));
        g.drawHorizontalLine(rulerTrack.getBottom() - 1, (float)rulerTrack.getX(), (float)rulerTrack.getRight());
    }

    // --- Timeline ruler (strip only): clip underlay, shared locator/cycle + ticks/labels, playheads.
    if (absTime && !rulerTrack.isEmpty())
    {
        const double sampleRate = effectiveDeviceSampleRate(deviceManager_);
        const std::int64_t visStart = visibleStartSamples_;
        const std::int64_t visEndEx = visibleEndSamples();
        const std::int64_t visLen = juce::jmax(std::int64_t{0}, visEndEx - visStart);
        const juce::Rectangle<float> rb = rulerTrack.toFloat();
        const std::int64_t arrLen = session_ != nullptr ? session_->getArrangementExtentSamples() : 0;

        const auto xAtSample = [&](const std::int64_t s) { return xForSessionSample(s); };

        if (timelineClip_ != nullptr && timelineClip_->lengthSamples > 0)
        {
            const std::int64_t lenClip = timelineClip_->lengthSamples;
            const float x0 = xForSessionSample(timelineClip_->startSamples);
            const float x1 = xForSessionSample(timelineClip_->startSamples + lenClip);
            const float left = juce::jmin(x0, x1);
            const float right = juce::jmax(x0, x1);
            const float clipL = juce::jmax(left, rb.getX());
            const float clipR = juce::jmin(right, rb.getRight());
            const float bandW = clipR - clipL;
            if (bandW > 0.5f)
            {
                g.setColour(juce::Colour(0xff7088a8).withAlpha(0.22f));
                g.fillRect(clipL, rb.getY(), bandW, rb.getHeight());
            }
        }

        if (session_ != nullptr && transport_ != nullptr && arrLen > 0)
        {
            const std::int64_t locL = session_->getLeftLocatorSamples();
            const std::int64_t locR = session_->getRightLocatorSamples();
            const bool cycleOn = transport_->readCycleEnabledForUi();

            using namespace timeline_locator_paint;

            paintLocatorCycleBandAndStripe(
                g, rb, xAtSample, visStart, visLen, locL, locR, cycleOn);
            const bool musical
                = session_->getTimelineRulerTimeDisplay()
                  == Session::TimelineRulerTimeDisplay::MusicalBarsBeats;
            if (musical)
            {
                const ProjectMusicalTime musicalTime = session_->getProjectMusicalTime();
                paintRulerMusicalTickMarks(
                    g,
                    rb,
                    xAtSample,
                    arrLen,
                    visStart,
                    visLen,
                    sampleRate,
                    samplesPerPixel_,
                    musicalTime);
            }
            else
            {
                paintRulerTickMarks(g, rb, xAtSample, arrLen, visStart, visLen, sampleRate);
            }
            paintLocatorTriangleHandles(g, rb, xAtSample, visStart, visLen, locL, locR, cycleOn);
            if (musical)
            {
                const ProjectMusicalTime musicalTime = session_->getProjectMusicalTime();
                paintRulerMusicalLabels(
                    g,
                    rb,
                    xAtSample,
                    arrLen,
                    visStart,
                    visLen,
                    sampleRate,
                    samplesPerPixel_,
                    musicalTime,
                    locL,
                    locR);
            }
            else
            {
                paintRulerTimeLabels(
                    g, rb, xAtSample, arrLen, visStart, visLen, sampleRate, locL, locR);
            }
        }

        if (transport_ != nullptr && arrLen > 0)
        {
            // One display position for both indicators in this window: the ruler stroke used to read
            // the **raw** block-quantized transport playhead while the grid line below used the
            // smoothed value, which put them a few pixels apart and made the ruler stroke step while
            // the grid line glided.
            const double phDrawD = currentPlayheadDisplaySampleForPaint();
            if (phDrawD >= (double)visStart && phDrawD < (double)(visStart + visLen))
            {
                const float xLine = playhead_pixel::snapToPixelCentre(xForSessionSampleD(phDrawD));
                g.setColour(juce::Colours::white.withAlpha(0.92f));
                g.drawLine(xLine,
                           rb.getY(),
                           xLine,
                           rb.getY() + timeline_locator_paint::kRulerPlayheadMarkerLengthPx,
                           1.5f);
            }
        }

    }

    // --- drawTimelineGrid
    if (absTime && timelineClip_ != nullptr)
    {
        if (samplesPerPixel_ > 0.0 && std::isfinite(samplesPerPixel_)
            && session_ != nullptr)
        {
            const double sr = effectiveDeviceSampleRate(deviceManager_);
            const double spp = samplesPerPixel_;
            const ProjectMusicalTime mt = session_->getProjectMusicalTime();
            const SnapSettings snap = session_->getArrangementSnapSettings();

            const double spb = samplesPerBeat(mt, sr);
            if (spb > 0.0 && std::isfinite(spb))
            {
                double stepBeats = arrangementSnapGridStepBeats(SnapResolution::Straight_1_16, mt);
                if (!std::isfinite(stepBeats) || stepBeats <= 0.0)
                {
                    stepBeats = 0.25;
                }

                if (snap.enabled)
                {
                    const double pick = arrangementSnapGridStepBeats(snap.resolution, mt);
                    if (std::isfinite(pick) && pick > 0.0)
                    {
                        stepBeats = pick;
                    }
                }

                while (stepBeats * spb / spp < kTimelineGridMinorMinPx && stepBeats < 1.0e9)
                {
                    stepBeats *= 2.0;
                }

                const std::int64_t visLo = visibleStartSamples_;
                const std::int64_t visHi = visibleEndSamples();
                double beatLo = sampleToBeatPosition(visLo, mt, sr);
                double beatHi = sampleToBeatPosition(visHi, mt, sr);
                if (beatHi < beatLo)
                {
                    std::swap(beatLo, beatHi);
                }

                const std::int64_t kStart = (std::int64_t)std::floor(beatLo / stepBeats) - 2;
                const std::int64_t kEnd = (std::int64_t)std::ceil(beatHi / stepBeats) + 2;

                const double barLen = beatsPerBar(mt);
                const juce::Colour colMinor = juce::Colour(0xff333340);
                const juce::Colour colHalf = juce::Colour(0xff454552);
                const juce::Colour colBeat = juce::Colour(0xff505060);

                for (std::int64_t k = kStart; k <= kEnd; ++k)
                {
                    const double posBeats = (double)k * stepBeats;
                    const std::int64_t absS = beatToSample(posBeats, mt, sr);
                    const float x = xForSessionSample(absS);
                    if (x < (float)gr.getX() - 2.0f || x > (float)gr.getRight() + 2.0f)
                    {
                        continue;
                    }

                    juce::Colour c = colMinor;
                    if (beatGridNearBarBoundary(posBeats, barLen))
                    {
                        c = colBeat;
                    }
                    else if (beatGridNearBeatBoundary(posBeats))
                    {
                        c = colHalf;
                    }

                    g.setColour(c);
                    g.drawVerticalLine(juce::roundToInt(x), (float)gr.getY(), (float)gr.getBottom());
                }
            }
        }
    }

    // --- drawLocatorLines (grid — thin verticals only; filled ranges live in ruler strip only)
    if (absTime && session_ != nullptr && samplesPerPixel_ > 0.0)
    {
        const std::int64_t locL = session_->getLeftLocatorSamples();
        const std::int64_t locR = session_->getRightLocatorSamples();
        auto line = [&](const std::int64_t s, const juce::Colour& col) {
            const float x = xForSessionSample(s);
            if (x >= (float)gr.getX() - 1.0f && x <= (float)gr.getRight() + 1.0f)
            {
                g.setColour(col);
                g.drawVerticalLine(juce::roundToInt(x), (float)gr.getY(), (float)gr.getBottom());
            }
        };
        line(locL, juce::Colours::white.withAlpha(0.38f));
        if (locR > 0)
        {
            line(locR, juce::Colours::white.withAlpha(0.32f));
        }
    }

    // --- drawNotes (timeline hits/bars)
    const bool pianoRowMode = (rowLabelMode_ == 1);

    if (timelineClip_ != nullptr && samplesPerPixel_ > 0.0)
    {
        const double sr = effectiveDeviceSampleRate(deviceManager_);
        const double bpm = pattern_.bpm > 0.0 ? pattern_.bpm : 120.0;
        const int tpq = experimentalEffectiveTicksPerQuarter(pattern_);
        const bool paintBars = (timelineNotesDisplayComboId_ == 2);
        const float hitHalfW = pianoRowMode ? juce::jmax(4.0f, juce::jmin(6.8f, (float)kRowHeight * 0.48f))
                                            : juce::jmax(3.5f, juce::jmin(6.0f, (float)kRowHeight * 0.42f));
        const float hitHalfH = pianoRowMode ? juce::jmax(4.0f, (float)kRowHeight * 0.45f)
                                            : juce::jmax(3.5f, (float)kRowHeight * 0.4f);
        for (int ti = 0; ti < (int)pattern_.timelineNotes.size(); ++ti)
        {
            const auto& tn = pattern_.timelineNotes[(size_t)ti];
            if (tn.midiNote < pitchLow_ || tn.midiNote > pitchHigh_)
            {
                continue;
            }
            const auto rrOpt = visibleRowStripRect(gr, tn.midiNote);
            if (!rrOpt)
            {
                continue;
            }
            const auto& rr = *rrOpt;
            const std::int64_t a0 = absoluteSampleForTimelineNote(timelineClip_->timelineAnchorSamples, tn, pattern_, sr);
            const std::int64_t vis0 = timelineClip_->startSamples;
            const std::int64_t vis1 = vis0 + juce::jmax(std::int64_t{ 1 }, timelineClip_->lengthSamples);
            if (a0 < vis0 || a0 >= vis1)
            {
                continue;
            }
            const float velA = juce::jlimit(0.28f, 1.0f, (float)tn.velocity / 127.0f);
            const bool noteSelected = isTimelineNoteIndexSelected(ti);

            if (paintBars)
            {
                const std::int64_t durS = ticksToRelativeSamples(
                    juce::jmax<std::int64_t>(1, tn.durationTicks), bpm, tpq, sr);
                const std::int64_t a1 = a0 + juce::jmax<std::int64_t>(1, durS);
                float xL = xForSessionSample(a0);
                float xR = xForSessionSample(a1);
                if (xR < xL)
                {
                    std::swap(xL, xR);
                }
                xL = juce::jmax(xL, (float)gr.getX());
                xR = juce::jmin(xR, (float)gr.getRight());
                if (xR <= (float)gr.getX() || xL >= (float)gr.getRight())
                {
                    continue;
                }
                const float notePadY = pianoRowMode ? 1.0f : 2.0f;
                const float noteInsetV = pianoRowMode ? 2.0f : 4.0f;
                auto noteRect = juce::Rectangle<float>(
                    xL, (float)rr.getY() + notePadY, xR - xL, (float)rr.getHeight() - noteInsetV);
                if (noteRect.getWidth() < 3.0f)
                {
                    noteRect = noteRect.withSizeKeepingCentre(4.0f, noteRect.getHeight());
                }
                const juce::Colour velC = colourForMidiVelocity(tn.velocity);
                if (noteSelected)
                {
                    g.setColour(juce::Colour(0xff101012));
                    g.fillRoundedRectangle(noteRect, 2.0f);
                    g.setColour(velC.brighter(0.45f).withAlpha(0.97f));
                    g.drawRoundedRectangle(noteRect, 2.0f, 2.35f);
                    g.setColour(velC.darker(0.55f).withAlpha(0.88f));
                    g.drawRoundedRectangle(noteRect, 2.0f, 1.05f);
                }
                else
                {
                    g.setColour(velC.withAlpha(0.78f * velA));
                    g.fillRoundedRectangle(noteRect, 2.0f);
                    g.setColour(velC.darker(0.6f).withAlpha(0.9f));
                    g.drawRoundedRectangle(noteRect, 2.0f, 1.1f);
                }
                if (pianoRowMode && noteRect.getWidth() >= 24.0f && noteRect.getHeight() >= 11.0f)
                {
                    const juce::String label = juce::MidiMessage::getMidiNoteName(tn.midiNote, true, true, 3);
                    const float fh = juce::jlimit(11.0f, 13.0f, noteRect.getHeight() - 2.0f);
                    g.setColour(noteSelected ? juce::Colours::white.withAlpha(0.97f)
                                             : juce::Colours::white.withAlpha(0.94f));
                    g.setFont(juce::Font(juce::FontOptions().withHeight(fh)));
                    g.drawText(label, noteRect.reduced(2.0f, 1.25f), juce::Justification::centred, true);
                }
            }
            else
            {
                const float cx = xForSessionSample(a0);
                if (cx < (float)gr.getX() - hitHalfW - 2.0f || cx > (float)gr.getRight() + hitHalfW + 2.0f)
                {
                    continue;
                }
                /// Compact drum-style marker at note onset; `durationTicks` unchanged in model.
                const float cy = (float)rr.getCentreY();
                juce::Path diamond;
                diamond.addQuadrilateral(cx, cy - hitHalfH, cx + hitHalfW, cy, cx, cy + hitHalfH, cx - hitHalfW, cy);
                const juce::Colour velC = colourForMidiVelocity(tn.velocity);
                if (noteSelected)
                {
                    g.setColour(juce::Colour(0xff101012));
                    g.fillPath(diamond);
                    g.setColour(velC.brighter(0.45f).withAlpha(0.97f));
                    g.strokePath(diamond, juce::PathStrokeType(2.1f));
                    g.setColour(velC.darker(0.55f).withAlpha(0.82f));
                    g.strokePath(diamond, juce::PathStrokeType(1.2f));
                }
                else
                {
                    g.setColour(velC.darker(0.6f).withAlpha(0.85f + 0.15f * velA));
                    g.strokePath(diamond, juce::PathStrokeType(1.15f));
                    g.setColour(velC.withAlpha(0.65f + 0.35f * velA));
                    g.fillPath(diamond);
                }
            }
        }
    }
    if (timelineMarqueeInteraction_ == TimelineMarqueeInteraction::Dragging)
    {
        const auto marq = getNormalizedMarqueeRect();
        if (!marq.isEmpty())
        {
            const auto rf = marq.toFloat();
            g.setColour(juce::Colour(0xff3d4e63).withAlpha(0.22f));
            g.fillRect(rf);
            g.setColour(juce::Colour(0xffe8eef5).withAlpha(0.82f));
            g.drawRect(rf, 1.0f);
        }
    }

    // --- drawGlobalPlayhead (grid) — same display sample + rounding as the ruler stroke above.
    if (absTime && transport_ != nullptr && samplesPerPixel_ > 0.0)
    {
        const float px = playhead_pixel::snapToPixelCentre(
            xForSessionSampleD(currentPlayheadDisplaySampleForPaint()));
        lastPaintedPlayheadCentreX_ = px;
        if (px >= (float)gr.getX() - 2.0f && px <= (float)gr.getRight() + 2.0f)
        {
            g.setColour(juce::Colour(0xff66ddff));
            g.drawLine(px, (float)gr.getY(), px, (float)gr.getBottom(), 1.35f);
        }
    }

    // --- Keyboard column
    g.setColour(juce::Colour(0xff2a2a32));
    g.fillRect(kb);

    if (sideStripContentWidthNow() > 0)
    {
        for (int pitch = paintHi; pitch >= paintLo; --pitch)
        {
            const auto wrOpt = visibleRowStripRect(kb, pitch);
            if (!wrOpt)
            {
                continue;
            }
            auto wr = *wrOpt;

            if (pitch == pitchLow_ || pitch == pitchHigh_)
            {
                g.setColour(juce::Colours::black.withAlpha(0.5f));
                g.fillRect(wr);
            }

            if (isBlackKey(pitch))
            {
                g.setColour(juce::Colour(0xff111118));
                const int bh = juce::jmax(8, (int)((float)kRowHeight * 0.72f));
                g.fillRoundedRectangle(wr.withSizeKeepingCentre(wr.getWidth() - 4, bh).toFloat(), 2.0f);
            }
            else
            {
                g.setColour(juce::Colour(0xfff0f0f5));
                g.fillRoundedRectangle(wr.reduced(2, 1).toFloat(), 2.0f);
                g.setColour(juce::Colour(0xff888899));
                g.drawRoundedRectangle(wr.reduced(2, 1).toFloat(), 2.0f, 1.0f);
            }

            const int kk = ((pitch % 12) + 12) % 12;
            if (rowLabelMode_ == 2)
            {
                const juce::String label = rowLabelProvider_ ? rowLabelProvider_(pitch)
                                                            : juce::MidiMessage::getMidiNoteName(pitch, true, true, 3);
                g.setColour(isBlackKey(pitch) ? juce::Colours::lightgrey : juce::Colours::black);
                g.setFont(juce::Font(juce::FontOptions().withHeight(9.0f)));
                g.drawText(label, wr.reduced(4, 0), juce::Justification::centredLeft, true);
            }
            else if (kk == 0)
            {
                const juce::String label = juce::MidiMessage::getMidiNoteName(pitch, true, true, 3);
                g.setColour(isBlackKey(pitch) ? juce::Colours::lightgrey : juce::Colours::black);
                g.setFont(juce::Font(juce::FontOptions().withHeight(10.0f)));
                g.drawText(label, wr.reduced(2, 0), juce::Justification::centredLeft, true);
            }
        }
    }

    // --- Velocity controller lane (bottom strip; spatially disjoint from grid/keyboard)
    paintVelocityLane(g);
    paintCcLane(g);
}
