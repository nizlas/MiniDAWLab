#include "app/InstrumentTimelineRowCoordinator.h"

#include "app/InstrumentRuntimeCoordinator.h"
#include "domain/Session.h"
#include "domain/SessionSnapshot.h"
#include "domain/Track.h"
#include "instruments/InstrumentTrackController.h"
#include "plugins/ExperimentalInstrumentHost.h"
#include "transport/Transport.h"
#include "ui/ForbiddenCursor.h"
#include "ui/InspectorView.h"
#include "ui/TimelineClipEventChrome.h"
#include "ui/TimelineRulerView.h"
#include "ui/TimelineViewportModel.h"
#include "ui/TrackLanesView.h"

#include <cmath>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

namespace
{
/// Sample<->pixel mapping for the in-clip note preview; mirrors `getEventBoundsForClip`'s inputs so
/// note bars land in the same coordinate space as the clip rect. `sampleRate <= 0` disables preview.
struct MidiClipNotePreviewContext
{
    std::int64_t previewMoveDeltaSamples = 0;
    std::int64_t visibleStartSamples = 0;
    double samplesPerPixel = 0.0;
    float originX = 0.0f;
    double sampleRate = 0.0;
};

/// Cubase-like note preview inside a MIDI clip rect (Slice 1, visual only): one bar per note,
/// x/width from note timing, y from pitch with **per-clip** min..max pitch scaling. Timeline notes
/// map through the shared timeline viewport; legacy step clips subdivide the locked clip length.
/// Drawing is clipped to `eb`; skipped for tiny rects or when no valid sample mapping exists
/// (permille fallback layout).
void paintMidiClipNotePreview(juce::Graphics& g,
                              const juce::Rectangle<float>& eb,
                              const InstrumentMidiClip& clip,
                              const MidiClipNotePreviewContext& ctx)
{
    constexpr float kMinNoteBarPx = 2.0f;
    constexpr float kVertPadPx = 3.0f;
    if (eb.getWidth() < 8.0f || ctx.samplesPerPixel <= 0.0 || !std::isfinite(ctx.samplesPerPixel)
        || ctx.sampleRate <= 0.0)
    {
        return;
    }
    const bool timeline = clip.pattern.usesTimelineNotes();
    const auto& timelineNotes = clip.pattern.timelineNotes;
    const auto& stepNotes = clip.pattern.notes;
    if (timeline ? timelineNotes.empty() : stepNotes.empty())
    {
        return;
    }

    int minPitch = 128;
    int maxPitch = -1;
    if (timeline)
    {
        for (const auto& n : timelineNotes)
        {
            minPitch = juce::jmin(minPitch, n.midiNote);
            maxPitch = juce::jmax(maxPitch, n.midiNote);
        }
    }
    else
    {
        for (const auto& n : stepNotes)
        {
            minPitch = juce::jmin(minPitch, n.midiNote);
            maxPitch = juce::jmax(maxPitch, n.midiNote);
        }
    }
    if (minPitch > maxPitch)
    {
        return;
    }
    if (minPitch == maxPitch)
    {
        // Single-pitch clip: widen the range so the one row reads as a centered band.
        minPitch = juce::jmax(0, minPitch - 2);
        maxPitch = juce::jmin(127, maxPitch + 2);
    }

    const juce::Rectangle<float> inner = eb.reduced(1.0f, kVertPadPx);
    if (inner.isEmpty())
    {
        return;
    }
    const int rows = maxPitch - minPitch + 1;
    const float rowH = inner.getHeight() / (float)rows;
    const float barH = juce::jlimit(1.5f, 7.0f, rowH * 0.8f);

    const juce::Graphics::ScopedSaveState save(g);
    g.reduceClipRegion(eb.toNearestInt());
    // Same colour family as the audio lane's waveform (lightblue on the shared clip body fill).
    g.setColour(juce::Colours::lightblue.withAlpha(0.75f));

    const auto drawNoteBar
        = [&](const std::int64_t absStart, const std::int64_t absEnd, const int pitch) noexcept {
              float x0 = TimelineRulerView::sessionSampleToLocalX(
                  absStart, ctx.originX, ctx.visibleStartSamples, ctx.samplesPerPixel);
              float x1 = TimelineRulerView::sessionSampleToLocalX(
                  absEnd, ctx.originX, ctx.visibleStartSamples, ctx.samplesPerPixel);
              if (x1 < x0)
              {
                  std::swap(x0, x1);
              }
              x1 = juce::jmax(x1, x0 + kMinNoteBarPx);
              if (x1 < eb.getX() || x0 > eb.getRight())
              {
                  return;
              }
              const float yCentre = inner.getBottom() - ((float)(pitch - minPitch) + 0.5f) * rowH;
              g.fillRect(x0, yCentre - barH * 0.5f, x1 - x0, barH);
          };

    if (timeline)
    {
        const double bpm = clip.pattern.bpm > 0.0 ? clip.pattern.bpm : 120.0;
        const int tpq = experimentalEffectiveTicksPerQuarter(clip.pattern);
        const std::int64_t anchor = clip.timelineAnchorSamples + ctx.previewMoveDeltaSamples;
        for (const auto& n : timelineNotes)
        {
            const std::int64_t s = anchor + ticksToSignedSamples(n.startTick, bpm, tpq, ctx.sampleRate);
            const std::int64_t d = juce::jmax(
                std::int64_t{ 1 }, ticksToSignedSamples(n.durationTicks, bpm, tpq, ctx.sampleRate));
            drawNoteBar(s, s + d, n.midiNote);
        }
    }
    else
    {
        const int numSteps = juce::jmax(1, clip.pattern.numSteps);
        const std::int64_t clipStart = clip.startSamples + ctx.previewMoveDeltaSamples;
        const std::int64_t len = juce::jmax(std::int64_t{ 1 }, clip.lengthSamples);
        // Short centered tick per step hit (drums have no meaningful duration on the step grid).
        const std::int64_t halfBar = juce::jmax(
            std::int64_t{ 1 }, (std::int64_t)std::llround(0.3 * (double)len / (double)numSteps));
        for (const auto& n : stepNotes)
        {
            const std::int64_t centre
                = clipStart + clipRelativeSampleAtStepCenter(n.step, numSteps, len);
            drawNoteBar(centre - halfBar, centre + halfBar, n.midiNote);
        }
    }
}

/// MIDI runtime clip: same outer chrome sequence as placed audio clips (`ClipWaveformView`); label only inside.
/// Paint order: body fill -> note preview -> selection overlay -> label (border/text stay readable).
void paintRuntimeMidiClipEventBlock(juce::Graphics& g,
                                    juce::Rectangle<float> eb,
                                    bool selected,
                                    const juce::String& clipName,
                                    const InstrumentMidiClip* clipForNotePreview,
                                    const MidiClipNotePreviewContext& notePreviewCtx)
{
    using namespace mini_daw::timeline_clip_chrome;
    paintEventChromeBody(g, eb, midiLaneEventBodyFill());
    if (clipForNotePreview != nullptr)
    {
        paintMidiClipNotePreview(g, eb, *clipForNotePreview, notePreviewCtx);
    }
    if (selected)
    {
        paintEventChromeSelectionOverlay(g, eb);
    }
    const juce::String label = clipName.trim().isNotEmpty() ? clipName.trim() : juce::String("MIDI");
    paintEventTopLeftNameLabel(g, eb, label);
}
} // namespace

struct InstrumentTimelineRowCoordinator::MidiEventLane final : public juce::Component,
                                                                 private juce::ChangeListener,
                                                                 private juce::Timer
{
    friend class InstrumentTimelineRowCoordinator;

    static constexpr bool kLogInstrumentLane = false;
    static constexpr float kClipDragThresholdPx = 3.0f;
    static constexpr int kTrimLaneEdgeHitPx = 7;

    explicit MidiEventLane(InstrumentTimelineRowCoordinator& ownerIn,
                            InstrumentTrackController* ctl,
                            TrackId timelineInstrumentTrackId) noexcept
        : owner_(ownerIn)
        , boundCtl_(ctl)
        , laneTimelineTrackId_(timelineInstrumentTrackId)
    {
        startTimerHz(20);
        setOpaque(false);
        if (boundCtl_ != nullptr)
        {
            boundCtl_->addChangeListener(this);
        }
    }

    ~MidiEventLane() override
    {
        restoreNormalCursorAfterInvalidMidiDrop();
        stopTimer();
        if (boundCtl_ != nullptr)
        {
            boundCtl_->removeChangeListener(this);
            boundCtl_ = nullptr;
        }
    }

    [[nodiscard]] TrackId laneTimelineTrackId() const noexcept { return laneTimelineTrackId_; }

    void attachControllerIfStillValid(InstrumentTrackController* ctl) noexcept
    {
        if (ctl == boundCtl_)
        {
            return;
        }
        if (boundCtl_ != nullptr)
        {
            boundCtl_->removeChangeListener(this);
        }
        boundCtl_ = ctl;
        if (boundCtl_ != nullptr)
        {
            boundCtl_->addChangeListener(this);
        }
    }

    [[nodiscard]] InstrumentTrackController* fixedControllerNullable() const noexcept { return boundCtl_; }

    void setInvalidMidiDropCursor() noexcept
    {
        if (cursorOverriddenForInvalidMidiDrop_)
        {
            return;
        }
        setMouseCursor(getForbiddenNoDropMouseCursor());
        cursorOverriddenForInvalidMidiDrop_ = true;
    }

    void restoreNormalCursorAfterInvalidMidiDrop() noexcept
    {
        if (!cursorOverriddenForInvalidMidiDrop_)
        {
            return;
        }
        setMouseCursor(
            juce::MouseCursor(juce::MouseCursor::StandardCursorType::NormalCursor));
        cursorOverriddenForInvalidMidiDrop_ = false;
    }

private:
    void changeListenerCallback(juce::ChangeBroadcaster*) override
    {
        owner_.repaintInstrumentTrackRow();
        owner_.refreshMidiEditorInstrumentUiIfOpen();
    }

    void timerCallback() override
    {
        if (owner_.transport_.readPlaybackIntentForUi() == PlaybackIntent::Playing)
        {
            repaint();
        }
    }

    void paint(juce::Graphics& g) override
    {
        using namespace mini_daw::timeline_clip_chrome;

        const auto laneContent = getLaneContentBounds();
        if (laneContent.isEmpty())
        {
            return;
        }

        if (!crossTrackDropGhostSpans_.empty())
        {
            constexpr float kGhostCorner = mini_daw::timeline_clip_chrome::kEventCorner;
            for (const auto& span : crossTrackDropGhostSpans_)
            {
                const auto eb = getEventBoundsForSessionSpan(span.first, span.second, laneContent);
                if (eb.isEmpty())
                {
                    continue;
                }
                const juce::Rectangle<float> ghostRect = eb.toFloat();
                g.setColour(juce::Colour(0xff5a7a9a).withAlpha(0.28f));
                g.fillRoundedRectangle(ghostRect, kGhostCorner);
                g.setColour(juce::Colour(0xffa0b8d8).withAlpha(0.5f));
                g.drawRoundedRectangle(ghostRect, kGhostCorner, 1.0f);
            }
        }

        InstrumentTrackController* const ac = activeControllerNullable();
        if (ac == nullptr)
        {
            return;
        }

        // Shared per-paint mapping for the in-clip note preview; matches `getEventBoundsForClip`
        // (same viewport, same origin x — the vertical event margin does not shift x).
        MidiClipNotePreviewContext noteCtx;
        noteCtx.visibleStartSamples = owner_.timelineViewport_.getVisibleStartSamples();
        noteCtx.samplesPerPixel = owner_.timelineViewport_.getSamplesPerPixel();
        noteCtx.originX = (float)laneContent.getX();
        noteCtx.sampleRate = ac->getTimelineSampleRate();

        for (const auto& up : ac->getClips())
        {
            const auto* c = up.get();
            if (c == nullptr)
            {
                continue;
            }
            const bool sel = ac->isClipSelected(c->id);
            const std::int64_t previewDelta
                = (dragDragging_ && sel) ? dragEffectivePreviewDeltaSamples_ : std::int64_t{ 0 };
            std::optional<std::int64_t> trimStart;
            std::optional<std::int64_t> trimLen;
            if (trimLaneGestureActive_ && trimLaneClipId_ == c->id)
            {
                trimStart = trimLanePreviewStartSamples_;
                trimLen = trimLanePreviewLengthSamples_;
            }
            const auto eb = getEventBoundsForClip(*c, laneContent, previewDelta, trimStart, trimLen);
            if (eb.isEmpty())
            {
                continue;
            }
            if (kLogInstrumentLane)
            {
                juce::Logger::writeToLog(
                    "instrument-lane: paint clip id=" + juce::String((juce::int64)c->id)
                    + " selected=" + juce::String(sel ? "true" : "false") + " eventBounds=" + eb.toString());
            }

            noteCtx.previewMoveDeltaSamples = previewDelta;
            paintRuntimeMidiClipEventBlock(g, eb.toFloat(), sel, c->name, c, noteCtx);

            if (!c->pattern.usesTimelineNotes())
            {
                continue;
            }
            const bool hideTrimCues = trimLaneGestureActive_ && trimLaneClipId_ == c->id;
            const bool onEventBodyTrimCue = hoverEventTrimCueId_ == c->id
                                            && !hoverLeftTrimHandleId_.has_value()
                                            && !hoverRightTrimHandleId_.has_value();
            const bool showLeftTrimHoverCue
                = !hideTrimCues && (hoverLeftTrimHandleId_ == c->id || onEventBodyTrimCue);
            const bool showRightTrimHoverCue
                = !hideTrimCues && (hoverRightTrimHandleId_ == c->id || onEventBodyTrimCue);
            if (showLeftTrimHoverCue)
            {
                paintEventChromeTrimHandle(g, eb.toFloat(), true);
            }
            if (showRightTrimHoverCue)
            {
                paintEventChromeTrimHandle(g, eb.toFloat(), false);
            }
        }
    }

    void mouseDown(const juce::MouseEvent& e) override
    {
        if (e.mods.isMiddleButtonDown())
        {
            // Middle button = TrackLanesView hand-pan; never select/edit MIDI clips.
            return;
        }
        // Any new press invalidates a pending delayed rename open (fast double clicks never rename).
        ++renameArmGeneration_;
        mouseDownOnSelectedClipNameLabel_ = false;
        renameArmedClipId_ = 0;
        hoverEventTrimCueId_.reset();
        hoverLeftTrimHandleId_.reset();
        hoverRightTrimHandleId_.reset();

        const auto pos = e.getPosition();
        if (kLogInstrumentLane)
        {
            juce::Logger::writeToLog("instrument-lane: mouseDown x=" + juce::String(pos.x) + " y="
                                     + juce::String(pos.y));
        }

        if (!getLocalBounds().contains(pos))
        {
            return;
        }

        InstrumentTrackController* const ac = activeControllerNullable();
        if (ac == nullptr)
        {
            return;
        }

        dragCouldMove_ = false;
        dragDragging_ = false;
        dragEffectivePreviewDeltaSamples_ = 0;
        trimLaneGestureActive_ = false;
        trimLaneDragging_ = false;
        owner_.clearInstrumentMidiCrossTrackDropGhosts();
        restoreNormalCursorAfterInvalidMidiDrop();

        const bool midiMoveBlocked = owner_.trackLanes_.isInstrumentMidiClipMoveBlocked();
        const auto laneContent = getLaneContentBounds();

        if (!midiMoveBlocked && timelineMappingAvailableForClipDrag_() && !laneContent.isEmpty())
        {
            // Iterate front-to-back paint order is oldest-first; hits must prefer the topmost clip (last in
            // z-order), otherwise another clip's minimum-width hit rect can steal edges from a newer paste.
            const auto& clips = ac->getClips();
            for (auto it = clips.rbegin(); it != clips.rend(); ++it)
            {
                InstrumentMidiClip* const c = it->get();
                if (c == nullptr || !c->pattern.usesTimelineNotes())
                {
                    continue;
                }
                const auto eb = getEventBoundsForClip(*c, laneContent, 0);
                if (eb.isEmpty() || !eb.contains(pos.toInt()))
                {
                    continue;
                }
                const int px = pos.x;
                const bool leftZone = px <= eb.getX() + kTrimLaneEdgeHitPx;
                const bool rightZone = px >= eb.getRight() - kTrimLaneEdgeHitPx;
                if (!leftZone && !rightZone)
                {
                    continue;
                }

                trimLaneGestureActive_ = true;
                trimLaneLeftEdge_ = leftZone;
                trimLaneClipId_ = c->id;
                trimLaneInitialStartSamples_ = c->startSamples;
                trimLaneInitialLengthSamples_ = c->lengthSamples;
                trimLanePreviewStartSamples_ = c->startSamples;
                trimLanePreviewLengthSamples_ = c->lengthSamples;

                const bool toggleMultiTrim = e.mods.isShiftDown();
                if (toggleMultiTrim)
                {
                    const bool thisTrackHasSelection = !ac->getSelectedClipIds().empty();
                    if (!thisTrackHasSelection)
                    {
                        if (owner_.callbacks_.clearAudioAndOtherInstrumentSelectionsForMidiTrack != nullptr)
                        {
                            owner_.callbacks_.clearAudioAndOtherInstrumentSelectionsForMidiTrack(
                                laneTimelineTrackId_);
                        }
                        ac->setSelectedClipIdsExclusive(c->id);
                    }
                    else
                    {
                        ac->toggleClipSelection(c->id);
                    }
                }
                else
                {
                    if (owner_.callbacks_.clearAudioAndOtherInstrumentSelectionsForMidiTrack != nullptr)
                    {
                        owner_.callbacks_.clearAudioAndOtherInstrumentSelectionsForMidiTrack(laneTimelineTrackId_);
                    }
                    if (ac->isClipSelected(c->id))
                    {
                        ac->setActiveSelectedClipId(c->id);
                    }
                    else
                    {
                        ac->setSelectedClipIdsExclusive(c->id);
                    }
                }

                repaint();
                return;
            }
        }

        if (auto* clip = hitTestClipAtEvent(e.position))
        {
            const bool toggleMulti = e.mods.isShiftDown();
            if (toggleMulti)
            {
                const bool thisTrackHasSelection = !ac->getSelectedClipIds().empty();
                if (!thisTrackHasSelection)
                {
                    if (owner_.callbacks_.clearAudioAndOtherInstrumentSelectionsForMidiTrack != nullptr)
                    {
                        owner_.callbacks_.clearAudioAndOtherInstrumentSelectionsForMidiTrack(
                            laneTimelineTrackId_);
                    }
                    ac->setSelectedClipIdsExclusive(clip->id);
                }
                else
                {
                    ac->toggleClipSelection(clip->id);
                }
            }
            else
            {
                if (owner_.callbacks_.clearAudioAndOtherInstrumentSelectionsForMidiTrack != nullptr)
                {
                    owner_.callbacks_.clearAudioAndOtherInstrumentSelectionsForMidiTrack(laneTimelineTrackId_);
                }
                if (ac->isClipSelected(clip->id))
                {
                    // Plain click on an already-selected clip: activate only (preserves multi-selection,
                    // including for double-click's first click).
                    ac->setActiveSelectedClipId(clip->id);
                    // Explorer-style rename arm: single click on the name label of the selected clip.
                    if (e.getNumberOfClicks() == 1)
                    {
                        const auto nameR = mini_daw::timeline_clip_chrome::clipEventTopLeftNameBounds(
                            getEventBoundsForClip(*clip, getLaneContentBounds(), 0).toFloat());
                        if (!nameR.isEmpty() && nameR.contains(e.position))
                        {
                            mouseDownOnSelectedClipNameLabel_ = true;
                            renameArmedClipId_ = clip->id;
                        }
                    }
                }
                else
                {
                    ac->setSelectedClipIdsExclusive(clip->id);
                }
            }
            if (kLogInstrumentLane)
            {
                juce::Logger::writeToLog(
                    "instrument-lane: hit clip id=" + juce::String((juce::int64)clip->id) + " selected=true");
            }

            dragMouseDownLocal_ = e.position.toInt();
            dragMouseDownScreen_ = e.getScreenPosition().toFloat();
            dragCouldMove_ = !midiMoveBlocked && ac->isClipSelected(clip->id)
                             && timelineMappingAvailableForClipDrag_();
        }
        else
        {
            if (owner_.callbacks_.clearAllArrangementEventSelections != nullptr)
            {
                owner_.callbacks_.clearAllArrangementEventSelections();
            }
            else
            {
                ac->clearClipSelection();
            }
            if (kLogInstrumentLane)
            {
                juce::Logger::writeToLog("instrument-lane: no hit");
            }
        }

        repaint();
    }

    void mouseDrag(const juce::MouseEvent& e) override
    {
        if (e.mods.isMiddleButtonDown())
        {
            return;
        }
        if (trimLaneGestureActive_)
        {
            owner_.clearInstrumentMidiCrossTrackDropGhosts();
            restoreNormalCursorAfterInvalidMidiDrop();
            if (owner_.trackLanes_.isInstrumentMidiClipMoveBlocked())
            {
                return;
            }
            constexpr std::int64_t kMinLen = 1;
            const std::int64_t sPtr = laneTimelineSampleAtLocalX(e.position.toInt());
            const std::int64_t initEndEx = trimLaneInitialStartSamples_ + trimLaneInitialLengthSamples_;
            if (trimLaneLeftEdge_)
            {
                std::int64_t ns = juce::jmin(sPtr, initEndEx - kMinLen);
                ns = juce::jmax(std::int64_t{ 0 }, ns);
                ns = snapTimelineSample(ns);
                ns = juce::jmin(ns, initEndEx - kMinLen);
                ns = juce::jmax(std::int64_t{ 0 }, ns);
                trimLanePreviewStartSamples_ = ns;
                trimLanePreviewLengthSamples_ = initEndEx - ns;
            }
            else
            {
                std::int64_t ne = juce::jmax(sPtr, trimLaneInitialStartSamples_ + kMinLen);
                ne = snapTimelineSample(ne);
                ne = juce::jmax(ne, trimLaneInitialStartSamples_ + kMinLen);
                trimLanePreviewStartSamples_ = trimLaneInitialStartSamples_;
                trimLanePreviewLengthSamples_ = ne - trimLaneInitialStartSamples_;
            }
            trimLaneDragging_ = true;
            repaint();
            return;
        }

        if (!dragCouldMove_)
        {
            return;
        }

        InstrumentTrackController* const ac = activeControllerNullable();
        if (ac == nullptr || owner_.trackLanes_.isInstrumentMidiClipMoveBlocked())
        {
            owner_.clearInstrumentMidiCrossTrackDropGhosts();
            restoreNormalCursorAfterInvalidMidiDrop();
            return;
        }

        const auto p0 = getLocalPoint(nullptr, dragMouseDownScreen_).toInt();
        const auto p1 = getLocalPoint(nullptr, e.getScreenPosition().toFloat()).toInt();
        const std::int64_t s0 = laneTimelineSampleAtLocalX(p0);
        const std::int64_t s1 = laneTimelineSampleAtLocalX(p1);
        const std::int64_t rawDelta = s1 - s0;
        std::int64_t effDelta = ac->clampInstrumentMidiClipMoveDeltaForCurrentSelection(rawDelta);
        const std::int64_t minSelStart = earliestSelectedClipStartSamples(*ac);
        if (minSelStart != std::numeric_limits<std::int64_t>::max())
        {
            const std::int64_t targetEarliest = minSelStart + effDelta;
            const std::int64_t snappedEarliest = snapTimelineSample(targetEarliest);
            effDelta = snappedEarliest - minSelStart;
            effDelta = ac->clampInstrumentMidiClipMoveDeltaForCurrentSelection(effDelta);
        }

        if (!dragDragging_)
        {
            if (e.getDistanceFromDragStart() < kClipDragThresholdPx)
            {
                owner_.clearInstrumentMidiCrossTrackDropGhosts();
                restoreNormalCursorAfterInvalidMidiDrop();
                return;
            }
            dragDragging_ = true;
        }

        dragEffectivePreviewDeltaSamples_ = effDelta;

        const std::optional<TrackId> hoverDest
            = owner_.instrumentMidiLaneHitAtScreen(e.getScreenPosition().toFloat());
        std::vector<std::pair<std::int64_t, std::int64_t>> ghostSpans;
        if (hoverDest.has_value() && *hoverDest != laneTimelineTrackId_)
        {
            ghostSpans.reserve(ac->getSelectedClipIds().size());
            for (const InstrumentMidiClipId cid : ac->getSelectedClipIds())
            {
                const InstrumentMidiClip* const c = ac->getClipById(cid);
                if (c == nullptr || !c->pattern.usesTimelineNotes() || c->lengthSamples <= 0)
                {
                    continue;
                }
                ghostSpans.emplace_back(c->startSamples + effDelta, c->lengthSamples);
            }
        }
        owner_.syncInstrumentMidiCrossTrackDropGhostPreview(laneTimelineTrackId_, hoverDest, std::move(ghostSpans));

        if (hoverDest.has_value())
        {
            restoreNormalCursorAfterInvalidMidiDrop();
        }
        else
        {
            setInvalidMidiDropCursor();
        }

        owner_.repaintInstrumentTrackRow();
    }

    void mouseUp(const juce::MouseEvent& e) override
    {
        if (e.mods.isMiddleButtonDown())
        {
            return;
        }
        owner_.clearInstrumentMidiCrossTrackDropGhosts();
        restoreNormalCursorAfterInvalidMidiDrop();
        InstrumentTrackController* const ac = activeControllerNullable();

        if (trimLaneGestureActive_)
        {
            const bool changed = trimLanePreviewStartSamples_ != trimLaneInitialStartSamples_
                                 || trimLanePreviewLengthSamples_ != trimLaneInitialLengthSamples_;
            if (changed && ac != nullptr && !owner_.trackLanes_.isInstrumentMidiClipMoveBlocked())
            {
                const TrackId laneTid = laneTimelineTrackId_;
                const InstrumentMidiClipId cid = trimLaneClipId_;
                const std::int64_t ns = trimLanePreviewStartSamples_;
                const std::int64_t nl = trimLanePreviewLengthSamples_;
                auto execute = owner_.callbacks_.executeUndoableInstrumentEdit;
                if (execute != nullptr)
                {
                    execute(juce::String("Trim MIDI clip"), [this, laneTid, cid, ns, nl]() mutable -> bool {
                        InstrumentRuntimeCoordinator& rc = owner_.instrumentRuntime_;
                        InstrumentTrackController* ctl = rc.getInstrumentControllerForTrack(laneTid);
                        if (ctl == nullptr || !ctl->hasInstrumentTrack())
                        {
                            return false;
                        }
                        const bool ok = ctl->applyInstrumentMidiClipVisibleTrim(cid, ns, nl);
                        if (ok)
                        {
                            owner_.trackLanes_.repaint();
                            owner_.inspector_.refreshFromSession();
                        }
                        return ok;
                    });
                }
                else if (ac->applyInstrumentMidiClipVisibleTrim(cid, ns, nl))
                {
                    owner_.trackLanes_.repaint();
                    owner_.inspector_.refreshFromSession();
                }
            }
            trimLaneGestureActive_ = false;
            trimLaneDragging_ = false;
        }

        if (ac != nullptr && !owner_.trackLanes_.isInstrumentMidiClipMoveBlocked() && dragCouldMove_
            && (dragDragging_ || e.getDistanceFromDragStart() >= kClipDragThresholdPx))
        {
            const auto p0 = getLocalPoint(nullptr, dragMouseDownScreen_).toInt();
            const auto p1 = getLocalPoint(nullptr, e.getScreenPosition().toFloat()).toInt();
            const std::int64_t s0 = laneTimelineSampleAtLocalX(p0);
            const std::int64_t s1 = laneTimelineSampleAtLocalX(p1);
            std::int64_t dCommit = ac->clampInstrumentMidiClipMoveDeltaForCurrentSelection(s1 - s0);

            const std::int64_t minSelStart = earliestSelectedClipStartSamples(*ac);
            if (minSelStart != std::numeric_limits<std::int64_t>::max())
            {
                const std::int64_t targetEarliest = minSelStart + dCommit;
                const std::int64_t snappedEarliest = snapTimelineSample(targetEarliest);
                dCommit = snappedEarliest - minSelStart;
                dCommit = ac->clampInstrumentMidiClipMoveDeltaForCurrentSelection(dCommit);
            }

            const std::optional<TrackId> dropLane
                = owner_.instrumentMidiLaneHitAtScreen(e.getScreenPosition().toFloat());
            const TrackId destTid = dropLane.value_or(laneTimelineTrackId_);
            const std::vector<InstrumentMidiClipId> clipIdsOrdered = ac->getSelectedClipIds();

            if (destTid != laneTimelineTrackId_)
            {
                auto execute = owner_.callbacks_.executeUndoableInstrumentEdit;
                if (execute != nullptr)
                {
                    const TrackId srcTid = laneTimelineTrackId_;
                    execute(
                        juce::String("Move MIDI clip"),
                        [this, srcTid, destTid, clipIdsOrdered, dCommit]() mutable -> bool {
                            if (clipIdsOrdered.empty())
                            {
                                return false;
                            }
                            InstrumentRuntimeCoordinator& rc = owner_.instrumentRuntime_;
                            std::vector<InstrumentMidiClipId> ids = std::move(clipIdsOrdered);
                            const bool ok
                                = rc.moveInstrumentMidiClipsBetweenTracks(srcTid, destTid, std::move(ids), dCommit);
                            if (ok)
                            {
                                if (owner_.callbacks_.clearAudioAndOtherInstrumentSelectionsForMidiTrack != nullptr)
                                {
                                    owner_.callbacks_.clearAudioAndOtherInstrumentSelectionsForMidiTrack(destTid);
                                }
                                owner_.trackLanes_.repaint();
                                owner_.inspector_.refreshFromSession();
                            }
                            return ok;
                        });
                }
                else
                {
                    std::vector<InstrumentMidiClipId> ids = clipIdsOrdered;
                    if (!ids.empty()
                        && owner_.instrumentRuntime_.moveInstrumentMidiClipsBetweenTracks(
                               laneTimelineTrackId_, destTid, std::move(ids), dCommit))
                    {
                        if (owner_.callbacks_.clearAudioAndOtherInstrumentSelectionsForMidiTrack != nullptr)
                        {
                            owner_.callbacks_.clearAudioAndOtherInstrumentSelectionsForMidiTrack(destTid);
                        }
                        owner_.trackLanes_.repaint();
                        owner_.inspector_.refreshFromSession();
                    }
                }
            }
            else if (dCommit != 0)
            {
                auto execute = owner_.callbacks_.executeUndoableInstrumentEdit;
                if (execute != nullptr)
                {
                    TrackId laneTid = laneTimelineTrackId_;
                    execute(juce::String("Move MIDI clip"), [this, laneTid, dCommit]() mutable -> bool {
                        InstrumentRuntimeCoordinator& rc = owner_.instrumentRuntime_;
                        InstrumentTrackController* ctl = rc.getInstrumentControllerForTrack(laneTid);
                        if (ctl == nullptr || !ctl->hasInstrumentTrack())
                        {
                            return false;
                        }
                        const bool ok = ctl->moveSelectedInstrumentMidiClipsByDeltaSamples(dCommit);
                        if (ok)
                        {
                            owner_.trackLanes_.repaint();
                            owner_.inspector_.refreshFromSession();
                        }
                        return ok;
                    });
                }
                else
                {
                    if (ac->moveSelectedInstrumentMidiClipsByDeltaSamples(dCommit))
                    {
                        owner_.trackLanes_.repaint();
                        owner_.inspector_.refreshFromSession();
                    }
                }
            }
        }

        if (mouseDownOnSelectedClipNameLabel_ && renameArmedClipId_ != 0 && !dragDragging_
            && e.getDistanceFromDragStart() < kClipDragThresholdPx && e.getNumberOfClicks() == 1)
        {
            // Slow second click on the selected clip's name label: open the inline rename editor
            // after the double-click window (a newer press bumps the generation and cancels this,
            // so a fast double-click keeps opening the MIDI editor instead).
            const InstrumentMidiClipId cid = renameArmedClipId_;
            const int gen = renameArmGeneration_;
            juce::Component::SafePointer<MidiEventLane> safe(this);
            juce::Timer::callAfterDelay(
                mini_daw::timeline_clip_chrome::kClipRenameSecondClickDelayMs, [safe, gen, cid] {
                    if (safe != nullptr && gen == safe->renameArmGeneration_)
                    {
                        safe->beginInlineClipRename(cid);
                    }
                });
        }
        mouseDownOnSelectedClipNameLabel_ = false;
        renameArmedClipId_ = 0;
        dragCouldMove_ = false;
        dragDragging_ = false;
        dragEffectivePreviewDeltaSamples_ = 0;
        [[maybe_unused]] const bool hoverDirty = refreshMidiLaneTrimHoverAffordances(e.getPosition());
        repaint();
    }

    void beginInlineClipRename(const InstrumentMidiClipId clipId)
    {
        InstrumentTrackController* const ac = activeControllerNullable();
        if (ac == nullptr || clipId == 0 || dragDragging_ || trimLaneGestureActive_)
        {
            return;
        }
        const InstrumentMidiClip* clip = nullptr;
        for (const auto& up : ac->getClips())
        {
            if (up != nullptr && up->id == clipId)
            {
                clip = up.get();
                break;
            }
        }
        if (clip == nullptr)
        {
            return;
        }
        const auto laneContent = getLaneContentBounds();
        if (laneContent.isEmpty())
        {
            return;
        }
        const auto eb = getEventBoundsForClip(*clip, laneContent, 0);
        if (eb.isEmpty())
        {
            return;
        }
        dismissInlineClipRename(false);

        if (clipRenameEditor_ == nullptr)
        {
            clipRenameEditor_ = std::make_unique<juce::TextEditor>("midiClipRenameEdit");
            clipRenameEditor_->setMultiLine(false);
            clipRenameEditor_->setReturnKeyStartsNewLine(false);
            clipRenameEditor_->setFont(juce::Font(juce::FontOptions().withHeight(12.0f)));
            clipRenameEditor_->setSelectAllWhenFocused(true);
            clipRenameEditor_->onReturnKey = [this] { dismissInlineClipRename(true); };
            clipRenameEditor_->onEscapeKey = [this] { dismissInlineClipRename(false); };
            // Focus loss commits — same convention as track-header rename.
            clipRenameEditor_->onFocusLost = [this] { dismissInlineClipRename(true); };
            addChildComponent(*clipRenameEditor_);
        }

        renameEditingClipId_ = clipId;
        clipRenameEditor_->setText(clip->name.trim(), false);
        juce::Rectangle<int> box = mini_daw::timeline_clip_chrome::clipEventTopLeftNameBounds(
            eb.toFloat()).toNearestInt();
        box.setHeight(20);
        box.setWidth(juce::jmax(box.getWidth(), 96));
        box = box.constrainedWithin(getLocalBounds().reduced(1));
        clipRenameEditor_->setBounds(box);
        clipRenameEditor_->setVisible(true);
        clipRenameEditor_->toFront(false);
        clipRenameEditor_->grabKeyboardFocus();
        repaint();
    }

    void dismissInlineClipRename(const bool commit)
    {
        if (clipRenameEditor_ == nullptr || !clipRenameEditor_->isVisible() || renameDismissInProgress_)
        {
            return;
        }
        renameDismissInProgress_ = true;
        const InstrumentMidiClipId cid = renameEditingClipId_;
        const juce::String text = clipRenameEditor_->getText().trim();
        renameEditingClipId_ = 0;
        clipRenameEditor_->setVisible(false);

        // Empty text = cancel (keep the old name): the safest behavior for accidental clears.
        if (commit && cid != 0 && text.isNotEmpty())
        {
            auto execute = owner_.callbacks_.executeUndoableInstrumentEdit;
            const TrackId laneTid = laneTimelineTrackId_;
            if (execute != nullptr)
            {
                execute(juce::String("Rename MIDI clip"), [this, laneTid, cid, text]() -> bool {
                    InstrumentRuntimeCoordinator& rc = owner_.instrumentRuntime_;
                    InstrumentTrackController* ctl = rc.getInstrumentControllerForTrack(laneTid);
                    if (ctl == nullptr || !ctl->hasInstrumentTrack())
                    {
                        return false;
                    }
                    const bool ok = ctl->renameInstrumentMidiClip(cid, text);
                    if (ok)
                    {
                        owner_.trackLanes_.repaint();
                    }
                    return ok;
                });
            }
            else if (InstrumentTrackController* const ac = activeControllerNullable(); ac != nullptr)
            {
                (void)ac->renameInstrumentMidiClip(cid, text);
            }
        }
        renameDismissInProgress_ = false;
        repaint();
    }

    void mouseMove(const juce::MouseEvent& e) override
    {
        if (trimLaneDragging_ || dragDragging_)
        {
            return;
        }

        restoreNormalCursorAfterInvalidMidiDrop();

        if (trimLaneGestureActive_)
        {
            setMouseCursor(juce::MouseCursor::LeftRightResizeCursor);
            return;
        }

        const bool hoverDirty = refreshMidiLaneTrimHoverAffordances(e.getPosition());
        if (hoverDirty)
        {
            repaint();
        }

        InstrumentTrackController* const ac = activeControllerNullable();
        if (ac == nullptr || owner_.trackLanes_.isInstrumentMidiClipMoveBlocked()
            || !timelineMappingAvailableForClipDrag_())
        {
            setMouseCursor(juce::MouseCursor::NormalCursor);
            return;
        }

        const auto laneContent = getLaneContentBounds();
        if (laneContent.isEmpty())
        {
            setMouseCursor(juce::MouseCursor::NormalCursor);
            return;
        }

        if (hoverLeftTrimHandleId_.has_value() || hoverRightTrimHandleId_.has_value())
        {
            setMouseCursor(juce::MouseCursor::LeftRightResizeCursor);
            return;
        }

        setMouseCursor(juce::MouseCursor::NormalCursor);
    }

    void mouseExit(const juce::MouseEvent& e) override
    {
        juce::ignoreUnused(e);
        const bool hoverDirty = refreshMidiLaneTrimHoverAffordances(std::nullopt);
        if (hoverDirty)
        {
            repaint();
        }
        if (!trimLaneDragging_ && !dragDragging_)
        {
            restoreNormalCursorAfterInvalidMidiDrop();
        }
    }

    void mouseDoubleClick(const juce::MouseEvent& e) override
    {
        if (e.mods.isMiddleButtonDown())
        {
            return;
        }
        // Double-click never renames: cancel any pending delayed rename open.
        ++renameArmGeneration_;
        mouseDownOnSelectedClipNameLabel_ = false;
        renameArmedClipId_ = 0;
        InstrumentTrackController* const ac = activeControllerNullable();
        if (ac == nullptr)
        {
            return;
        }

        if (auto* clip = hitTestClipAtEvent(e.position))
        {
            if (!ac->isClipSelected(clip->id))
            {
                ac->setSelectedClipIdsExclusive(clip->id);
            }
            else
            {
                ac->setActiveSelectedClipId(clip->id);
            }
            owner_.openMidiEditorForInstrumentClip(laneTimelineTrackId_, clip->id);
            repaint();
            return;
        }

        createEmptyMidiClipAtEventAndOpenEditor(e);
    }

    /// Double-click on empty lane space: create a default-length empty MIDI clip at the (snapped) click
    /// position, select it, and open the MIDI editor so the user can start entering notes immediately.
    void createEmptyMidiClipAtEventAndOpenEditor(const juce::MouseEvent& e)
    {
        InstrumentTrackController* const ac = activeControllerNullable();
        if (ac == nullptr || !ac->hasInstrumentTrack())
        {
            return;
        }
        if (owner_.trackLanes_.isInstrumentMidiClipMoveBlocked() || !timelineMappingAvailableForClipDrag_()
            || !getLaneContentBounds().contains(e.getPosition()))
        {
            return;
        }

        const std::int64_t clickSample = laneTimelineSampleAtLocalX(e.getPosition());
        const std::int64_t startSample = snapTimelineSample(clickSample);

        const TrackId laneTid = laneTimelineTrackId_;
        auto createdId = std::make_shared<InstrumentMidiClipId>(0);
        auto createClip = [this, laneTid, startSample, createdId]() -> bool {
            InstrumentRuntimeCoordinator& rc = owner_.instrumentRuntime_;
            InstrumentTrackController* ctl = rc.getInstrumentControllerForTrack(laneTid);
            if (ctl == nullptr || !ctl->hasInstrumentTrack())
            {
                return false;
            }
            const InstrumentMidiClipId newId = ctl->createEmptyTimelineMidiClipAtSamples(startSample);
            if (newId == 0)
            {
                return false;
            }
            *createdId = newId;
            if (owner_.callbacks_.clearAudioAndOtherInstrumentSelectionsForMidiTrack != nullptr)
            {
                owner_.callbacks_.clearAudioAndOtherInstrumentSelectionsForMidiTrack(laneTid);
            }
            ctl->setSelectedClipIdsExclusive(newId);
            owner_.trackLanes_.repaint();
            owner_.inspector_.refreshFromSession();
            return true;
        };

        if (auto execute = owner_.callbacks_.executeUndoableInstrumentEdit; execute != nullptr)
        {
            execute(juce::String("Create MIDI clip"), std::move(createClip));
        }
        else
        {
            createClip();
        }

        if (*createdId != 0)
        {
            owner_.openMidiEditorForInstrumentClip(laneTimelineTrackId_, *createdId);
        }
        repaint();
    }

    [[nodiscard]] InstrumentTrackController* activeControllerNullable() const noexcept { return boundCtl_; }

    [[nodiscard]] bool timelineMappingAvailableForClipDrag_() const noexcept
    {
        const double spp = owner_.timelineViewport_.getSamplesPerPixel();
        return spp > 0.0 && std::isfinite(spp);
    }

    [[nodiscard]] std::int64_t laneTimelineSampleAtLocalX(juce::Point<int> localPt) const noexcept
    {
        const auto lc = getLaneContentBounds();
        const float relX = (float)(localPt.x - lc.getX());
        TimelineViewportModel& vp = owner_.timelineViewport_;
        const double spp = vp.getSamplesPerPixel();
        const std::int64_t visStart = vp.getVisibleStartSamples();
        if (!timelineMappingAvailableForClipDrag_())
        {
            return visStart;
        }
        return TimelineRulerView::xToSessionSampleClamped(relX, (float)lc.getWidth(), visStart, spp);
    }

    [[nodiscard]] std::int64_t snapTimelineSample(std::int64_t s) const noexcept
    {
        s = juce::jmax(std::int64_t{ 0 }, s);
        if (owner_.callbacks_.snapArrangementTimelineSample != nullptr)
        {
            return owner_.callbacks_.snapArrangementTimelineSample(s);
        }
        return s;
    }

    [[nodiscard]] std::int64_t earliestSelectedClipStartSamples(
        const InstrumentTrackController& ctl) const noexcept
    {
        std::int64_t minStart = std::numeric_limits<std::int64_t>::max();
        for (const InstrumentMidiClipId id : ctl.getSelectedClipIds())
        {
            if (const InstrumentMidiClip* c = ctl.getClipById(id))
            {
                minStart = juce::jmin(minStart, c->startSamples);
            }
        }
        return minStart;
    }

    [[nodiscard]] juce::Rectangle<int> getLaneContentBounds() const
    {
        return getLocalBounds().reduced(0, 6);
    }

    [[nodiscard]] juce::Rectangle<int> getEventBoundsForSessionSpan(std::int64_t startSamples,
                                                                   std::int64_t lengthSamples,
                                                                   juce::Rectangle<int> laneContent) const
    {
        using namespace mini_daw::timeline_clip_chrome;
        const auto band = laneContent.toFloat().reduced(0.0f, kEventVerticalMargin);
        TimelineViewportModel& vp = owner_.timelineViewport_;
        const double spp = vp.getSamplesPerPixel();
        const std::int64_t spanLen = juce::jmax(std::int64_t{ 1 }, lengthSamples);
        if (spp > 0.0 && std::isfinite(spp) && spanLen > 0)
        {
            const std::int64_t visStart = vp.getVisibleStartSamples();
            const float originX = band.getX();
            const std::int64_t len = juce::jmax(std::int64_t{ 1 }, spanLen);
            const std::int64_t anchor = juce::jmax(std::int64_t{ 0 }, startSamples);
            const float x0 = TimelineRulerView::sessionSampleToLocalX(anchor, originX, visStart, spp);
            const float x1
                = TimelineRulerView::sessionSampleToLocalX(anchor + len, originX, visStart, spp);
            float left = juce::jmin(x0, x1);
            float right = juce::jmax(x0, x1);
            constexpr float minW = 40.0f;
            if (right - left < minW)
            {
                const float mid = 0.5f * (left + right);
                left = mid - minW * 0.5f;
                right = mid + minW * 0.5f;
            }
            left = juce::jlimit(band.getX(), band.getRight(), left);
            right = juce::jlimit(band.getX(), band.getRight(), right);
            if (right <= band.getX() + 0.5f || left >= band.getRight() - 0.5f)
            {
                return {};
            }
            const int y = juce::roundToInt(band.getY());
            const int h = juce::jmax(1, juce::roundToInt(band.getHeight()));
            return { juce::roundToInt(left), y, juce::jmax(1, juce::roundToInt(right - left)), h };
        }
        return {};
    }

    [[nodiscard]] juce::Rectangle<int> getEventBoundsForClip(const InstrumentMidiClip& c,
                                                             juce::Rectangle<int> laneContent,
                                                             std::int64_t previewMoveDeltaSamples,
                                                             const std::optional<std::int64_t>& spanStartOverride = {},
                                                             const std::optional<std::int64_t>& spanLengthOverride = {}) const
    {
        using namespace mini_daw::timeline_clip_chrome;
        const auto band = laneContent.toFloat().reduced(0.0f, kEventVerticalMargin);
        TimelineViewportModel& vp = owner_.timelineViewport_;
        const double spp = vp.getSamplesPerPixel();
        const std::int64_t spanLen = spanLengthOverride.has_value()
                                         ? juce::jmax(std::int64_t{ 1 }, *spanLengthOverride)
                                         : c.lengthSamples;
        if (spp > 0.0 && std::isfinite(spp) && spanLen > 0)
        {
            const std::int64_t visStart = vp.getVisibleStartSamples();
            const float originX = band.getX();
            const std::int64_t len = juce::jmax(std::int64_t{ 1 }, spanLen);
            const std::int64_t anchor
                = spanStartOverride.has_value()
                      ? juce::jmax(std::int64_t{ 0 }, *spanStartOverride)
                      : juce::jmax(std::int64_t{ 0 }, c.startSamples + previewMoveDeltaSamples);
            const float x0 = TimelineRulerView::sessionSampleToLocalX(anchor, originX, visStart, spp);
            const float x1 = TimelineRulerView::sessionSampleToLocalX(
                anchor + len, originX, visStart, spp);
            float left = juce::jmin(x0, x1);
            float right = juce::jmax(x0, x1);
            constexpr float minW = 40.0f;
            if (right - left < minW)
            {
                const float mid = 0.5f * (left + right);
                left = mid - minW * 0.5f;
                right = mid + minW * 0.5f;
            }
            left = juce::jlimit(band.getX(), band.getRight(), left);
            right = juce::jlimit(band.getX(), band.getRight(), right);
            if (right <= band.getX() + 0.5f || left >= band.getRight() - 0.5f)
            {
                return {};
            }
            const int y = juce::roundToInt(band.getY());
            const int h = juce::jmax(1, juce::roundToInt(band.getHeight()));
            return { juce::roundToInt(left), y, juce::jmax(1, juce::roundToInt(right - left)), h };
        }

        const int laneCW = juce::jmax(1, juce::roundToInt(band.getWidth()));
        const float s = (float)c.laneStartFractionPermille / 1000.f;
        const float e = (float)c.laneEndFractionPermille / 1000.f;
        const float span = juce::jlimit(0.02f, 1.f, e - s);
        int w = juce::roundToInt((float)laneCW * span);
        w = juce::jmax(40, juce::jmin(w, laneCW));
        const int minX0 = juce::roundToInt(band.getX());
        const int maxX0 = juce::roundToInt(band.getRight()) - w;
        if (maxX0 < minX0)
        {
            return {};
        }
        const int avail = juce::jmax(0, maxX0 - minX0);
        const int x0 = minX0 + (avail > 0 ? juce::roundToInt(s * (float)avail) : 0);
        const int clampedX0 = juce::jlimit(minX0, maxX0, x0);
        const int y = juce::roundToInt(band.getY());
        const int h = juce::jmax(1, juce::roundToInt(band.getHeight()));
        return { clampedX0, y, w, h };
    }

    [[nodiscard]] InstrumentMidiClip* hitTestClipAtEvent(juce::Point<float> pos) const
    {
        const auto laneContent = getLaneContentBounds();
        if (!laneContent.contains(pos.toInt()))
        {
            return nullptr;
        }

        InstrumentTrackController* const ac = activeControllerNullable();
        if (ac == nullptr)
        {
            return nullptr;
        }

        // Match paint z-order: clips draw oldest→newest; prefer topmost hit so a pasted clip is not
        // shadowed by another clip's minimum-width hit rectangle.
        const auto& clips = ac->getClips();
        for (auto it = clips.rbegin(); it != clips.rend(); ++it)
        {
            auto* c = it->get();
            if (c == nullptr)
            {
                continue;
            }

            if (getEventBoundsForClip(*c, laneContent, 0).contains(pos.toInt()))
            {
                return c;
            }
        }

        return nullptr;
    }

    [[nodiscard]] bool refreshMidiLaneTrimHoverAffordances(const std::optional<juce::Point<int>>& localPos)
    {
        const auto assignAndDiff = [&](std::optional<InstrumentMidiClipId> cue,
                                       std::optional<InstrumentMidiClipId> left,
                                       std::optional<InstrumentMidiClipId> right) -> bool {
            const bool changed = (cue != hoverEventTrimCueId_) || (left != hoverLeftTrimHandleId_)
                                 || (right != hoverRightTrimHandleId_);
            hoverEventTrimCueId_ = cue;
            hoverLeftTrimHandleId_ = left;
            hoverRightTrimHandleId_ = right;
            return changed;
        };

        if (!localPos.has_value())
        {
            return assignAndDiff(std::nullopt, std::nullopt, std::nullopt);
        }

        if (trimLaneDragging_ || dragDragging_)
        {
            return assignAndDiff(std::nullopt, std::nullopt, std::nullopt);
        }

        InstrumentTrackController* const ac = activeControllerNullable();
        if (ac == nullptr || owner_.trackLanes_.isInstrumentMidiClipMoveBlocked()
            || !timelineMappingAvailableForClipDrag_())
        {
            return assignAndDiff(std::nullopt, std::nullopt, std::nullopt);
        }

        const auto laneContent = getLaneContentBounds();
        if (laneContent.isEmpty())
        {
            return assignAndDiff(std::nullopt, std::nullopt, std::nullopt);
        }

        std::optional<InstrumentMidiClipId> newCue;
        std::optional<InstrumentMidiClipId> newL;
        std::optional<InstrumentMidiClipId> newR;

        const auto& clips = ac->getClips();
        for (auto it = clips.rbegin(); it != clips.rend(); ++it)
        {
            const auto* c = it->get();
            if (c == nullptr || !c->pattern.usesTimelineNotes())
            {
                continue;
            }
            const auto eb = getEventBoundsForClip(*c, laneContent, 0);
            if (eb.isEmpty() || !eb.contains(localPos->toInt()))
            {
                continue;
            }
            const int px = localPos->x;
            const bool leftZone = px <= eb.getX() + kTrimLaneEdgeHitPx;
            const bool rightZone = px >= eb.getRight() - kTrimLaneEdgeHitPx;
            if (leftZone)
            {
                newL = c->id;
            }
            else if (rightZone)
            {
                newR = c->id;
            }
            else
            {
                newCue = c->id;
            }
            break;
        }

        return assignAndDiff(newCue, newL, newR);
    }

    InstrumentTimelineRowCoordinator& owner_;
    InstrumentTrackController* boundCtl_ = nullptr;
    TrackId laneTimelineTrackId_ = kInvalidTrackId;

    bool dragCouldMove_ = false;
    bool dragDragging_ = false;
    // Explorer-style inline clip rename (mirrors `ClipWaveformView`): a slow second click on the
    // top-left name label of the already-selected clip opens a `TextEditor` (Return commits,
    // Escape cancels, focus loss commits). `renameArmGeneration_` invalidates the pending delayed
    // open on any newer press so a fast double-click (opens the MIDI editor) never renames.
    std::unique_ptr<juce::TextEditor> clipRenameEditor_;
    InstrumentMidiClipId renameEditingClipId_ = 0;
    InstrumentMidiClipId renameArmedClipId_ = 0;
    bool mouseDownOnSelectedClipNameLabel_ = false;
    int renameArmGeneration_ = 0;
    bool renameDismissInProgress_ = false;
    juce::Point<int> dragMouseDownLocal_;
    juce::Point<float> dragMouseDownScreen_{};
    std::int64_t dragEffectivePreviewDeltaSamples_ = 0;
    bool cursorOverriddenForInvalidMidiDrop_ = false;

    bool trimLaneGestureActive_ = false;
    bool trimLaneDragging_ = false;
    bool trimLaneLeftEdge_ = false;
    InstrumentMidiClipId trimLaneClipId_ = 0;
    std::int64_t trimLaneInitialStartSamples_ = 0;
    std::int64_t trimLaneInitialLengthSamples_ = 0;
    std::int64_t trimLanePreviewStartSamples_ = 0;
    std::int64_t trimLanePreviewLengthSamples_ = 0;

    std::optional<InstrumentMidiClipId> hoverEventTrimCueId_;
    std::optional<InstrumentMidiClipId> hoverLeftTrimHandleId_;
    std::optional<InstrumentMidiClipId> hoverRightTrimHandleId_;

    std::vector<std::pair<std::int64_t, std::int64_t>> crossTrackDropGhostSpans_;

    void clearCrossTrackDropGhost() noexcept { crossTrackDropGhostSpans_.clear(); }

    void setCrossTrackDropGhost(std::vector<std::pair<std::int64_t, std::int64_t>> spans) noexcept
    {
        crossTrackDropGhostSpans_ = std::move(spans);
    }

    [[nodiscard]] bool hasCrossTrackDropGhost() const noexcept { return !crossTrackDropGhostSpans_.empty(); }
};

InstrumentTimelineRowCoordinator::InstrumentTimelineRowCoordinator(
    Session& session,
    Transport& transport,
    TrackLanesView& trackLanesView,
    InspectorView& inspectorView,
    TimelineViewportModel& timelineViewport,
    InstrumentRuntimeCoordinator& instrumentRuntime,
    Callbacks callbacks)
    : session_(session)
    , transport_(transport)
    , trackLanes_(trackLanesView)
    , inspector_(inspectorView)
    , timelineViewport_(timelineViewport)
    , instrumentRuntime_(instrumentRuntime)
    , callbacks_(std::move(callbacks))
{
}

InstrumentTimelineRowCoordinator::~InstrumentTimelineRowCoordinator() = default;

void InstrumentTimelineRowCoordinator::refreshMidiEditorInstrumentUiIfOpen()
{
    if (callbacks_.refreshMidiEditorInstrumentUiIfOpen != nullptr)
    {
        callbacks_.refreshMidiEditorInstrumentUiIfOpen();
    }
}

void InstrumentTimelineRowCoordinator::openMidiEditorForInstrumentClip(const TrackId timelineInstrumentTrackId,
                                                                       const InstrumentMidiClipId clipId)
{
    if (callbacks_.openMidiEditorForInstrumentClip != nullptr)
    {
        callbacks_.openMidiEditorForInstrumentClip(timelineInstrumentTrackId, clipId);
    }
}

std::optional<TrackId> InstrumentTimelineRowCoordinator::instrumentMidiLaneHitAtScreen(
    const juce::Point<float> screenPt) const noexcept
{
    const std::shared_ptr<const SessionSnapshot> snap = session_.loadSessionSnapshotForAudioThread();
    if (snap == nullptr)
    {
        return std::nullopt;
    }
    const juce::Point<int> p{ juce::roundToInt(screenPt.x), juce::roundToInt(screenPt.y) };
    for (int ti = 0; ti < snap->getNumTracks(); ++ti)
    {
        const Track& tr = snap->getTrack(ti);
        if (tr.getKind() != TrackKind::Instrument)
        {
            continue;
        }
        const TrackId tid = tr.getId();
        const auto it = instrumentMidiEventLanesByTrackId_.find(tid);
        if (it == instrumentMidiEventLanesByTrackId_.end() || it->second == nullptr)
        {
            continue;
        }
        if (it->second->getScreenBounds().contains(p))
        {
            return tid;
        }
    }
    return std::nullopt;
}

void InstrumentTimelineRowCoordinator::clearInstrumentMidiCrossTrackDropGhosts() noexcept
{
    bool changed = false;
    for (auto& kv : instrumentMidiEventLanesByTrackId_)
    {
        if (kv.second != nullptr && kv.second->hasCrossTrackDropGhost())
        {
            kv.second->clearCrossTrackDropGhost();
            changed = true;
        }
    }
    if (changed)
    {
        repaintInstrumentTrackRow();
    }
}

void InstrumentTimelineRowCoordinator::syncInstrumentMidiCrossTrackDropGhostPreview(
    const TrackId dragSourceTrackId,
    const std::optional<TrackId> hoverDestTrackId,
    std::vector<std::pair<std::int64_t, std::int64_t>> sessionStartLenSamples) noexcept
{
    for (auto& kv : instrumentMidiEventLanesByTrackId_)
    {
        if (kv.second != nullptr)
        {
            kv.second->clearCrossTrackDropGhost();
        }
    }

    const bool show = hoverDestTrackId.has_value() && *hoverDestTrackId != kInvalidTrackId
                      && *hoverDestTrackId != dragSourceTrackId && !sessionStartLenSamples.empty();
    if (show)
    {
        auto it = instrumentMidiEventLanesByTrackId_.find(*hoverDestTrackId);
        if (it != instrumentMidiEventLanesByTrackId_.end() && it->second != nullptr)
        {
            it->second->setCrossTrackDropGhost(std::move(sessionStartLenSamples));
        }
    }
}

void InstrumentTimelineRowCoordinator::repaintInstrumentTrackRow()
{
    for (auto& kv : instrumentTrackHeadersByTrackId_)
    {
        if (kv.second != nullptr)
        {
            kv.second->repaint();
        }
    }
    for (auto& kv : instrumentMidiEventLanesByTrackId_)
    {
        if (kv.second != nullptr)
        {
            kv.second->repaint();
        }
    }
}

void InstrumentTimelineRowCoordinator::rewireInstrumentTrackRenameHandlers() noexcept
{
    for (auto& kv : instrumentTrackHeadersByTrackId_)
    {
        TrackHeaderView* const h = kv.second.get();
        if (h == nullptr)
        {
            continue;
        }
        const TrackId laneTid = kv.first;
        h->patchRenameCallbacks(
            [this] { return !trackLanes_.isStructuralTimelineEditBlocked(); },
            [this, laneTid](const juce::String raw) -> bool {
                return trackLanes_.invokeUndoableRenameTrackRequested(laneTid, raw);
            });
    }
}

void InstrumentTimelineRowCoordinator::tearDownExperimentalInstrumentTimelineUiForTrack(const TrackId tid) noexcept
{
    if (tid == kInvalidTrackId)
    {
        return;
    }
    // Detach from the lanes view while the components are still alive: its attachment map stores
    // raw header/lane pointers, and a later sync would call getParentComponent() on freed
    // components (UAF crash seen on Delete Track after project load).
    trackLanes_.detachInstrumentTimelineRowForTrack(tid);
    instrumentMidiEventLanesByTrackId_.erase(tid);
    instrumentTrackHeadersByTrackId_.erase(tid);
}

void InstrumentTimelineRowCoordinator::clearInstrumentTimelineLanesAndHeaders() noexcept
{
    instrumentMidiEventLanesByTrackId_.clear();
    instrumentTrackHeadersByTrackId_.clear();
}

void InstrumentTimelineRowCoordinator::tickStructuralEditBlockedHeaderStripRepaint(
    const bool structuralTimelineEditBlockedUi) noexcept
{
    if (structuralTimelineEditBlockedUi != lastStructuralTimelineBlockedForHeaderStripUi_)
    {
        lastStructuralTimelineBlockedForHeaderStripUi_ = structuralTimelineEditBlockedUi;
        for (auto& kv : instrumentTrackHeadersByTrackId_)
        {
            if (kv.second != nullptr)
            {
                kv.second->repaint();
            }
        }
        trackLanes_.repaint();
    }
}

void InstrumentTimelineRowCoordinator::syncInstrumentTimelineRowAttachmentToSession() noexcept
{
    std::vector<InstrumentTimelineAttachment> rows;
    const std::shared_ptr<const SessionSnapshot> snap = session_.loadSessionSnapshotForAudioThread();
    if (snap != nullptr)
    {
        for (int ti = 0; ti < snap->getNumTracks(); ++ti)
        {
            const Track& tr = snap->getTrack(ti);
            if (tr.getKind() != TrackKind::Instrument)
            {
                continue;
            }
            const TrackId laneTid = tr.getId();
            InstrumentTrackController* ctl = instrumentRuntime_.getInstrumentControllerForTrack(laneTid);
            if (ctl == nullptr || !ctl->hasInstrumentTrack()
                || ctl->getExperimentalInstrumentDomainTrackId() != laneTid)
            {
                continue;
            }
            ensureInstrumentTimelineHeaderAndLaneForTrack(laneTid);
            auto itLane = instrumentMidiEventLanesByTrackId_.find(laneTid);
            auto itHdr = instrumentTrackHeadersByTrackId_.find(laneTid);
            if (itLane == instrumentMidiEventLanesByTrackId_.end() || itLane->second == nullptr
                || itHdr == instrumentTrackHeadersByTrackId_.end() || itHdr->second == nullptr)
            {
                continue;
            }
            itLane->second->attachControllerIfStillValid(ctl);
            rows.push_back(
                InstrumentTimelineAttachment{ laneTid, ctl, itHdr->second.get(), itLane->second.get() });
        }
    }
    trackLanes_.syncInstrumentTimelineAttachments(rows);
}

void InstrumentTimelineRowCoordinator::ensureInstrumentTimelineHeaderAndLaneForTrack(const TrackId tid)
{
    if (tid == kInvalidTrackId)
    {
        return;
    }
    InstrumentTrackController* ctl = instrumentRuntime_.getInstrumentControllerForTrack(tid);
    if (ctl == nullptr || !ctl->hasInstrumentTrack())
    {
        return;
    }

    auto itLaneExisting = instrumentMidiEventLanesByTrackId_.find(tid);
    if (itLaneExisting == instrumentMidiEventLanesByTrackId_.end())
    {
        auto lane = std::make_unique<MidiEventLane>(*this, ctl, tid);
        instrumentMidiEventLanesByTrackId_.emplace(tid, std::move(lane));
    }
    else if (itLaneExisting->second != nullptr)
    {
        itLaneExisting->second->attachControllerIfStillValid(ctl);
    }

    auto itHdr = instrumentTrackHeadersByTrackId_.find(tid);
    if (itHdr != instrumentTrackHeadersByTrackId_.end() && itHdr->second != nullptr)
    {
        return;
    }

    const TrackId laneTid = tid;

    TrackHeaderModelProvider modelProvider = [this, ctl, laneTid]() -> TrackHeaderModel {
        TrackHeaderModel m;
        ExperimentalInstrumentHost* mh = instrumentRuntime_.getInstrumentHostForTrack(laneTid);
        m.subtitle = {};
        m.active = ctl->isActive();
        m.armed = false;
        m.muted = ctl->isMuted();
        m.off = !ctl->isPowerOn();
        m.powerInteractable = !trackLanes_.isStructuralTimelineEditBlocked();
        m.muteInteractable = true;
        m.armInteractable = false;
        if (const auto sn = session_.loadSessionSnapshotForAudioThread())
        {
            const int idx = sn->findTrackIndexById(laneTid);
            if (idx >= 0)
            {
                m.name = sn->getTrack(idx).getName();
                m.trackNameRenameEnabled = (sn->getTrack(idx).getKind() != TrackKind::Master);
            }
            else
            {
                m.name = juce::String("Track ") + juce::String((juce::int64)laneTid);
                m.trackNameRenameEnabled = true;
            }
        }
        else
        {
            m.name = juce::String("Track ") + juce::String((juce::int64)laneTid);
            m.trackNameRenameEnabled = true;
        }
        m.instrumentEditorAvailable = mh != nullptr && mh->hasInstrument();
        return m;
    };

    auto repaintExtras = [this] {
        repaintInstrumentTrackRow();
        trackLanes_.repaint();
        inspector_.refreshFromSession();
    };

    TrackHeaderCallbacks callbacks;
    callbacks.onActivateName = [this, ctl, laneTid, repaintExtras] {
        instrumentRuntime_.deactivateAllKeyedAndStagingControllers();
        session_.setActiveTrack(laneTid);
        ctl->setActive(true);
        repaintExtras();
    };
    callbacks.onToggleMute = [ctl, laneTid, this, repaintExtras] {
        ctl->setMuted(!ctl->isMuted());
        session_.setActiveTrack(laneTid);
        repaintExtras();
    };
    callbacks.onTogglePower = [ctl, laneTid, this, repaintExtras]() -> bool {
        if (trackLanes_.isStructuralTimelineEditBlocked())
        {
            return false;
        }
        ctl->setPowerOn(!ctl->isPowerOn());
        session_.setActiveTrack(laneTid);
        repaintExtras();
        return true;
    };
    callbacks.onToggleArm = [] {};
    callbacks.onOpenInstrumentEditor = [this, laneTid] {
        if (ExperimentalInstrumentHost* h = instrumentRuntime_.getInstrumentHostForTrack(laneTid))
        {
            h->openNativeEditor();
        }
    };
    callbacks.onShowContextMenu = [this, laneTid, repaintExtras](TrackHeaderView& self, const juce::MouseEvent&) {
        session_.setActiveTrack(laneTid);
        instrumentRuntime_.setKeyedInstrumentControllersActiveExclusive(laneTid);
        repaintExtras();

        juce::PopupMenu menu;
        constexpr int kDeleteTrackMenuId = 1;
        constexpr int kImportMidiFileMenuId = 3;
        constexpr int kRescanDescriptionsMenuId = 2;
        const bool editLocked = trackLanes_.isStructuralTimelineEditBlocked();
        juce::PopupMenu::Item deleteItem;
        deleteItem.itemID = kDeleteTrackMenuId;
        deleteItem.text = "Delete Track";
        deleteItem.isEnabled = !editLocked;
        menu.addItem(deleteItem);
        juce::PopupMenu::Item importMidiItem;
        importMidiItem.itemID = kImportMidiFileMenuId;
        importMidiItem.text = "Import MIDI file...";
        importMidiItem.isEnabled = !editLocked;
        menu.addItem(importMidiItem);
        menu.addSeparator();
        juce::PopupMenu::Item rescanItem;
        rescanItem.itemID = kRescanDescriptionsMenuId;
        rescanItem.text = "Rescan plugin description (out-of-process)…";
        rescanItem.isEnabled = !editLocked;
        menu.addItem(rescanItem);

        juce::Component::SafePointer<TrackLanesView> safeLanes(&trackLanes_);
        auto rescan = callbacks_.runExperimentalInstrumentPluginDescriptionRescanForTrack;
        auto runMidiImport = callbacks_.runInstrumentMidiFileImportForTrack;
        menu.showMenuAsync(
            juce::PopupMenu::Options().withTargetComponent(&self),
            [safeLanes,
             laneTid,
             kDeleteTrackMenuId,
             kImportMidiFileMenuId,
             kRescanDescriptionsMenuId,
             rescan,
             runMidiImport](const int result) {
                if (safeLanes == nullptr || result == 0)
                {
                    return;
                }
                if (result == kDeleteTrackMenuId)
                {
                    safeLanes->requestDeleteTrackForHeaderMenu(laneTid);
                    return;
                }
                if (result == kImportMidiFileMenuId)
                {
                    if (runMidiImport != nullptr)
                    {
                        runMidiImport(laneTid);
                    }
                    return;
                }
                if (result == kRescanDescriptionsMenuId)
                {
                    if (rescan != nullptr)
                    {
                        rescan(laneTid);
                    }
                }
            });
    };

    callbacks.onRowHeightDrag = [this, laneTid](const int startH, const int delta) {
        trackLanes_.applyTrackRowHeightDelta(laneTid, startH, delta);
    };
    callbacks.onRowHeightDragEnd = [this, laneTid, ctl] {
        trackLanes_.snapTrackHeaderRowHeightAfterResize(laneTid, false);
    };
    callbacks.canBeginRenameTrack = [this, laneTid]() {
        if (trackLanes_.isInstrumentMidiClipMoveBlocked())
        {
            return false;
        }
        if (const auto snap = session_.loadSessionSnapshotForAudioThread())
        {
            const int idx = snap->findTrackIndexById(laneTid);
            if (idx >= 0 && snap->getTrack(idx).getKind() == TrackKind::Master)
            {
                return false;
            }
        }
        return true;
    };
    callbacks.onCommitRenameTrack = [this, laneTid](const juce::String raw) -> bool {
        return trackLanes_.invokeUndoableRenameTrackRequested(laneTid, raw);
    };

    auto hdr = std::make_unique<TrackHeaderView>(
        std::move(modelProvider), std::move(callbacks), kInvalidTrackId, std::nullopt);
    instrumentTrackHeadersByTrackId_[tid] = std::move(hdr);
}
