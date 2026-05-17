#include "ExperimentalPianoRollView.h"
#include "ExperimentalMidiPatternPlayer.h"
#include "instruments/InstrumentTrackController.h"
#include "ui/TimelineRulerView.h"
#include "ui/TimelineLocatorPainter.h"

#include "domain/Session.h"
#include "domain/ProjectMusicalTime.h"
#include "transport/Transport.h"
#include "ui/TimelineViewportModel.h"

#include <cmath>
#include <limits>

#include <algorithm>

namespace
{
    constexpr bool kMidiEditorVerbosePianoRollMouseLog = false;
    /// Plain wheel over keys/grid: target ~12 pitch rows per **physical** notch. Many hosts/OSes report
    /// `wheel.deltaY` ≈ 0.25 per notch, so scale by 48 so one notch ≈ one octave; sub-line deltas accumulate.
    constexpr float kPitchScrollRowsPerWheelDelta = 48.0f;
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
    constexpr double kTimelineGridBeatMinPx = 11.0;
    constexpr double kTimelineGridBarMinPx = 5.0;

    [[nodiscard]] inline double spacingPxForTickDelta(const std::int64_t deltaTicks,
                                                      const double bpm,
                                                      const int tpq,
                                                      const double sr,
                                                      const double samplesPerPixel) noexcept
    {
        if (deltaTicks <= 0 || samplesPerPixel <= 0.0 || !std::isfinite(samplesPerPixel))
        {
            return 0.0;
        }
        const std::int64_t ds = ticksToRelativeSamples(deltaTicks, bpm, tpq, sr);
        if (ds <= 0)
        {
            return 0.0;
        }
        return (double)ds / samplesPerPixel;
    }

    [[nodiscard]] inline std::int64_t floorTickToGridStep(const std::int64_t tk,
                                                         const std::int64_t step) noexcept
    {
        if (step <= 1)
        {
            return tk;
        }
        if (tk >= 0)
        {
            return (tk / step) * step;
        }
        return ((tk + 1) / step - 1) * step;
    }

    [[nodiscard]] inline int stepGridStrideForMinPx(const float pxPerStepColumn,
                                                    const double minPx) noexcept
    {
        if (!(pxPerStepColumn > 1.0e-6f) || !std::isfinite(pxPerStepColumn))
        {
            return 1;
        }
        const int stride = (int)std::ceil(minPx / (double)pxPerStepColumn);
        return juce::jmax(1, stride);
    }

    /// Hit-test only: must match diamond path used when painting timeline hits (compact diamonds).
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
    setMouseClickGrabsKeyboardFocus(false);
    uiTimerHzConfigured_ = kMidiRollTimerHzIdle;
    startTimerHz(kMidiRollTimerHzIdle);
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
}

void ExperimentalPianoRollView::textEditorEscapeKeyPressed(juce::TextEditor& ed)
{
    if (rowLabelEditor_.get() == &ed)
    {
        dismissRowLabelEditor(false);
    }
}

void ExperimentalPianoRollView::textEditorFocusLost(juce::TextEditor& ed)
{
    if (rowLabelEditor_.get() != &ed)
    {
        return;
    }
    // Escape / click-away: do not auto-commit renames (explicit Return only).
    dismissRowLabelEditor(false);
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
    return r.removeFromLeft(keyboardColumnWidth());
}

juce::Rectangle<int> ExperimentalPianoRollView::gridBounds() const
{
    auto r = getLocalBounds();
    r.removeFromTop(timelineRulerHeight());
    r.removeFromLeft(sideStripTotalNow());
    return r;
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
        selectedTimelineNoteIndices_.clear();
        timelineNoteResizeActive_ = false;
        timelineResizeNoteIndex_ = -1;
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
    lastObservedNoteCountUi_ = -1;
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
            timelineClip_->midiRollFollowEnabled = false;
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
    lastObservedNoteCountUi_ = -1;
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

void ExperimentalPianoRollView::timerCallback()
{
    const double nowSec = juce::Time::getMillisecondCounterHiRes() * 0.001;
    const bool absTime = useAbsoluteTimeline();
    if (absTime && timelineClip_ != nullptr && !isTimelineClipBindingFresh())
    {
        return;
    }
    const bool transportPlaying = absTime && transport_ != nullptr
                                  && transport_->readPlaybackIntentForUi() == PlaybackIntent::Playing;
    const bool previewPlaying = player_ != nullptr && player_->isPlaying();
    const bool wasTransportPlaying = wasTransportPlayingUi_;

    if (previewPlaying && absTime && timelineClip_ != nullptr && timelineClip_->lengthSamples > 0)
    {
        const double lenClip = (double)juce::jmax(std::int64_t{1}, timelineClip_->lengthSamples);
        uiPreviewDisplayAbsSample_ = (double)timelineClip_->startSamples
                                   + (double)player_->getPlayheadNormalized() * lenClip;
    }
    else
    {
        uiPreviewDisplayAbsSample_
            = timelineClip_ != nullptr ? (double)timelineClip_->startSamples : 0.0;
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

                if (needScroll)
                {
                    const std::int64_t clamped = juce::jmax(std::int64_t{0}, targetStart);
                    if (clamped != visibleStartSamples_)
                    {
                        visibleStartSamples_ = clamped;
                        viewportMoved = true;
                        structuralRepaint = true;
                    }
                }
            }
        }
    }

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

    const int noteCount = (int)pattern_.notes.size();
    const int tnCount = (int)pattern_.timelineNotes.size();
    if (noteCount != lastObservedNoteCountUi_ || tnCount != lastObservedTimelineNoteCountUi_)
    {
        lastObservedNoteCountUi_ = noteCount;
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
    else if (transportPlaying || previewPlaying)
    {
        if (!absTime)
        {
            lastOffscreenGatePlayheadInView_ = true;
            repaint();
        }
        else if (previewPlaying)
        {
            lastOffscreenGatePlayheadInView_ = true;
            repaint();
        }
        else if (transportPlaying)
        {
            const auto gr = gridBounds();
            constexpr float kMarginPx = 24.0f;
            const float x = xForSessionSampleD(uiPlayheadDisplaySamples_);
            const bool nowNear = x >= (float)gr.getX() - kMarginPx && x <= (float)gr.getRight() + kMarginPx;
            const bool wasNear = lastOffscreenGatePlayheadInView_;
            if (!nowNear && !wasNear)
            {
                lastOffscreenGatePlayheadInView_ = false;
            }
            else
            {
                repaint();
                lastOffscreenGatePlayheadInView_ = nowNear;
            }
        }
    }

    const int wantHz = (transportPlaying || previewPlaying) ? kMidiRollTimerHzAnimating : kMidiRollTimerHzIdle;
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
}

float ExperimentalPianoRollView::cellWidth() const
{
    const auto gr = gridBounds();
    const int n = juce::jmax(1, pattern_.numSteps);
    return (float)gr.getWidth() / (float)n;
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

int ExperimentalPianoRollView::topVisiblePitch() const noexcept
{
    return pitchHigh_ - pitchScrollOffsetRows_;
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

int ExperimentalPianoRollView::stepAtPatternX(const int x) const
{
    const auto gr = gridBounds();
    const int relX = x - gr.getX();
    const float cw = cellWidth();
    if (cw <= 0.0f)
    {
        return 0;
    }
    const int s = (int)((float)relX / cw);
    return juce::jlimit(0, juce::jmax(0, pattern_.numSteps - 1), s);
}

int ExperimentalPianoRollView::stepAtTimelineX(const int x) const
{
    if (timelineClip_ == nullptr)
    {
        return 0;
    }
    const std::int64_t len = juce::jmax(std::int64_t{1}, timelineClip_->lengthSamples);
    const std::int64_t absS = sampleAtGridX((float)x);
    const std::int64_t rel = absS - timelineClip_->startSamples;
    const std::int64_t clampedRel = juce::jlimit(std::int64_t{0}, len - 1, rel);
    const int ns = juce::jmax(1, pattern_.numSteps);
    const int step = (int)juce::jlimit(
        0,
        juce::jmax(0, ns - 1),
        (int)std::floor((double)clampedRel * (double)ns / (double)len));
    return step;
}

void ExperimentalPianoRollView::setMusicalSnapComboId(const int id) noexcept
{
    musicalSnapComboId_ = juce::jlimit(1, 4, id);
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
    const int tpq = experimentalEffectiveTicksPerQuarter(pattern_);
    switch (musicalSnapComboId_)
    {
    case 2:
        return juce::jmax<std::int64_t>(1, (std::int64_t)(tpq / 2));
    case 3:
        return juce::jmax<std::int64_t>(1, (std::int64_t)(tpq / 4));
    case 4:
        return juce::jmax<std::int64_t>(1, (std::int64_t)(tpq / 8));
    default:
        return 0;
    }
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
    if (timelineClip_ == nullptr || !pattern_.usesTimelineNotes() || !useAbsoluteTimeline())
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
    if (timelineClip_ == nullptr || !pattern_.usesTimelineNotes() || !useAbsoluteTimeline())
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

bool ExperimentalPianoRollView::pianoMelodicTimelineBarsResizeEnabled() const noexcept
{
    return useAbsoluteTimeline() && pattern_.usesTimelineNotes() && timelineClip_ != nullptr
        && isTimelineClipBindingFresh() && timelineNotesDisplayComboId_ == 2 && rowLabelMode_ == 1
        && samplesPerPixel_ > 0.0 && std::isfinite(samplesPerPixel_);
}

std::optional<std::pair<int, ExperimentalPianoRollView::TimelineNoteResizeEdge>>
ExperimentalPianoRollView::findPianoBarResizeEdgeAtPoint(const juce::Point<int> pos) const
{
    if (!pianoMelodicTimelineBarsResizeEnabled())
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
    const std::int64_t g = musicalSnapGridTicks();
    if (g <= 0)
    {
        return tick;
    }
    return (std::int64_t)std::llround((double)tick / (double)g) * g;
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

void ExperimentalPianoRollView::beginTimelineNoteResizeGesture(
    const int noteIndex,
    const ExperimentalPianoRollView::TimelineNoteResizeEdge edge)
{
    if (noteIndex < 0 || noteIndex >= (int)pattern_.timelineNotes.size())
    {
        return;
    }
    timelineMarqueeInteraction_ = TimelineMarqueeInteraction::None;
    timelineNoteResizeActive_ = true;
    timelineResizeEdge_ = edge;
    timelineResizeNoteIndex_ = noteIndex;
    const auto& n = pattern_.timelineNotes[(size_t)noteIndex];
    timelineResizeOriginalStartTick_ = n.startTick;
    timelineResizeOriginalDurationTicks_ = n.durationTicks;
    timelineResizeAnchorEndTick_ = n.startTick + juce::jmax<std::int64_t>(1, n.durationTicks);
}

void ExperimentalPianoRollView::updateTimelineNoteResizeGesture(const juce::Point<int> localPos)
{
    if (!timelineNoteResizeActive_ || timelineClip_ == nullptr || timelineResizeNoteIndex_ < 0
        || timelineResizeNoteIndex_ >= (int)pattern_.timelineNotes.size())
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

    auto& n = pattern_.timelineNotes[(size_t)timelineResizeNoteIndex_];

    if (timelineResizeEdge_ == TimelineNoteResizeEdge::Right)
    {
        const std::int64_t start = timelineResizeOriginalStartTick_;
        std::int64_t endTick = tSnap;
        if (musicalSnapGridTicks() <= 0)
        {
            endTick = rawTick;
        }
        endTick = juce::jmax(endTick, start + minD);
        endTick = juce::jmin(endTick, clipHighTick);
        endTick = juce::jmax(endTick, start + minD);
        n.startTick = start;
        n.durationTicks = endTick - start;
        if (n.durationTicks < minD)
        {
            n.durationTicks = minD;
        }
    }
    else
    {
        const std::int64_t endT = timelineResizeAnchorEndTick_;
        std::int64_t newStart = tSnap;
        if (musicalSnapGridTicks() <= 0)
        {
            newStart = rawTick;
        }
        newStart = juce::jmin(newStart, endT - minD);
        newStart = juce::jmax(newStart, clipLowTick);
        newStart = juce::jmin(newStart, endT - minD);
        n.startTick = newStart;
        n.durationTicks = endT - newStart;
        if (n.durationTicks < minD)
        {
            n.startTick = endT - minD;
            n.durationTicks = minD;
        }
    }
}

void ExperimentalPianoRollView::finishTimelineNoteResizeGesture()
{
    if (!timelineNoteResizeActive_)
    {
        return;
    }

    const int idx = timelineResizeNoteIndex_;
    timelineNoteResizeActive_ = false;
    timelineResizeNoteIndex_ = -1;

    if (idx < 0 || idx >= (int)pattern_.timelineNotes.size())
    {
        repaint();
        return;
    }

    auto& n = pattern_.timelineNotes[(size_t)idx];
    const std::int64_t finalS = n.startTick;
    const std::int64_t finalD = n.durationTicks;
    const std::int64_t origS = timelineResizeOriginalStartTick_;
    const std::int64_t origD = timelineResizeOriginalDurationTicks_;

    if (finalS == origS && finalD == origD)
    {
        repaint();
        return;
    }

    if (undoablePatternEditHandler_ != nullptr && instrumentTrackController_ != nullptr && timelineClip_ != nullptr)
    {
        n.startTick = origS;
        n.durationTicks = origD;

        undoablePatternEditHandler_(
            "Resize MIDI note",
            [this, idx, finalS, finalD]() -> bool {
                if (idx < 0 || idx >= (int)pattern_.timelineNotes.size())
                {
                    return false;
                }
                auto& nn = pattern_.timelineNotes[(size_t)idx];
                nn.startTick = finalS;
                nn.durationTicks = finalD;
                if (instrumentTrackController_ != nullptr)
                {
                    instrumentTrackController_->notifyClipExperimentalMusicalTimingChanged();
                }
                repaint();
                return true;
            });
    }
    else
    {
        n.startTick = finalS;
        n.durationTicks = finalD;
        if (instrumentTrackController_ != nullptr)
        {
            instrumentTrackController_->notifyClipExperimentalMusicalTimingChanged();
        }
        repaint();
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
    if (!pattern_.usesTimelineNotes() || timelineClip_ == nullptr || r.isEmpty())
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

    if (!(e.mods.isCtrlDown() || e.mods.isShiftDown()))
    {
        if (const auto edgeHit = findPianoBarResizeEdgeAtPoint(e.getPosition()))
        {
            const int ni = edgeHit->first;
            if (selectedTimelineNoteIndices_.count(ni) == 0u)
            {
                replaceTimelineNoteSelectionWithSingle(ni);
            }
            beginTimelineNoteResizeGesture(ni, edgeHit->second);
            repaint();
            return;
        }
    }

    if (const auto hit = findTimelineNoteIndexAtPoint(e.getPosition()))
    {
        if (e.mods.isCtrlDown() || e.mods.isShiftDown())
        {
            toggleTimelineNoteInSelection(*hit);
        }
        else
        {
            replaceTimelineNoteSelectionWithSingle(*hit);
        }
        repaint();
        return;
    }

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
    const std::int64_t snapG = musicalSnapGridTicks();
    if (snapG > 0)
    {
        tickOffset = (std::int64_t)std::llround((double)tickOffset / (double)snapG) * snapG;
    }

    const std::int64_t absSnappedNote = oldAnchor + ticksToSignedSamples(tickOffset, bpm, tpq, sr);
    if (absSnappedNote < vis0 || absSnappedNote >= vis1)
    {
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
        nn.channel = 10;
        nn.startTick = tickOffset;
        nn.durationTicks = snapG > 0 ? snapG : 240;

        auto addAndNotify = [this, nn, commitSortedNotes]() mutable -> bool {
            pattern_.timelineNotes.push_back(nn);
            commitSortedNotes();

            if (instrumentTrackController_ != nullptr)
            {
                instrumentTrackController_->notifyClipExperimentalMusicalTimingChanged();
            }
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
    nn.channel = 10;
    nn.startTick = 0;
    nn.durationTicks = snapG > 0 ? snapG : 240;

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
    const std::int64_t s =
        TimelineRulerView::xToSessionSampleClamped(xInTrack, trackWidth, visibleStartSamples_, samplesPerPixel_);
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
    const std::int64_t s =
        TimelineRulerView::xToSessionSampleClamped(xInTrack, trackWidth, visibleStartSamples_, samplesPerPixel_);
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
        repaint();
    }
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

    const auto rt = rulerTrackBounds();
    if (useAbsoluteTimeline() && !rt.isEmpty() && rt.contains(pos))
    {
        timelineMarqueeInteraction_ = TimelineMarqueeInteraction::None;
        handleTimelineRulerMouseDown(e, rt);
        return;
    }

    const auto gr = gridBounds();

    if (gr.contains(pos))
    {
        if (useAbsoluteTimeline() && pattern_.usesTimelineNotes() && timelineClip_ != nullptr)
        {
            handleTimelineNotesMouseDown(e);
            return;
        }
        timelineMarqueeInteraction_ = TimelineMarqueeInteraction::None;
        const int step = useAbsoluteTimeline() ? stepAtTimelineX(pos.getX()) : stepAtPatternX(pos.getX());
        const int pitch = pitchAtY(pos.getY());
        if (kMidiEditorVerbosePianoRollMouseLog)
        {
            ExperimentalMidiPatternPlayer::writeMidiEditorLogLine(
                "piano-roll: mouseDown x=" + juce::String(pos.getX()) + " y=" + juce::String(pos.getY())
                + " note=" + juce::String(pitch) + " step=" + juce::String(step));
        }
        auto toggleAndNotify = [this, pitch, step]() -> bool {
            pattern_.toggleHit(pitch, step);
            if (instrumentTrackController_ != nullptr && timelineClip_ != nullptr)
            {
                instrumentTrackController_->notifyClipPatternMutated(timelineClip_->id);
            }
            repaint();
            return true;
        };
        if (undoablePatternEditHandler_ && instrumentTrackController_ != nullptr && timelineClip_ != nullptr)
        {
            undoablePatternEditHandler_("Toggle drum step", std::move(toggleAndNotify));
        }
        else
        {
            toggleAndNotify();
        }
        if (kMidiEditorVerbosePianoRollMouseLog)
        {
            ExperimentalMidiPatternPlayer::writeMidiEditorLogLine(
                "piano-roll: toggle note=" + juce::String(pitch) + " step=" + juce::String(step) + " noteCount="
                + juce::String((int)pattern_.notes.size()));
        }
    }
}

void ExperimentalPianoRollView::mouseDrag(const juce::MouseEvent& e)
{
    if (timelineNoteResizeActive_)
    {
        updateTimelineNoteResizeGesture(e.getPosition());
        repaint();
        return;
    }

    if (useAbsoluteTimeline() && pattern_.usesTimelineNotes() && timelineClip_ != nullptr
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
    finishTimelineNoteResizeGesture();
    finishMarqueeSelection();

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
    if (gr.contains(pos) && useAbsoluteTimeline() && pattern_.usesTimelineNotes() && timelineClip_ != nullptr
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
                    instrumentTrackController_->notifyClipPatternMutated(timelineClip_->id);
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
    if (timelineNoteResizeActive_)
    {
        setMouseCursor(juce::MouseCursor::LeftRightResizeCursor);
        setTooltip(juce::String{});
        return;
    }

    if (rowLabelMode_ == 2 && rowLabelTooltipProvider_)
    {
        const auto kb = keyboardBounds();
        if (kb.contains(e.getPosition()))
        {
            const int pitch = pitchAtY(e.getPosition().getY());
            if (pitch >= pitchLow_ && pitch <= pitchHigh_)
            {
                const juce::String tip = rowLabelTooltipProvider_(pitch);
                if (tip.isNotEmpty())
                {
                    setTooltip(tip);
                    return;
                }
            }
        }
    }
    const auto gr = gridBounds();
    if (gr.contains(e.getPosition()) && pianoMelodicTimelineBarsResizeEnabled()
        && findPianoBarResizeEdgeAtPoint(e.getPosition()))
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
            syncViewportToBoundClip();
            repaint();
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
            syncViewportToBoundClip();
            repaint();
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
        repaint();
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
    syncViewportToBoundClip();
    repaint();
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

    const float cw = cellWidth();
    const int nSteps = juce::jmax(1, pattern_.numSteps);

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
            const std::int64_t ph = transport_->readPlayheadSamplesForUi();
            const std::int64_t phClamped
                = juce::jlimit(std::int64_t{0}, juce::jmax(std::int64_t{0}, arrLen), ph);
            const double phDrawD = uiRulerSeekDisplayHold_.has_value() ? uiPlayheadDisplaySamples_
                                                                      : (double)phClamped;
            if (phDrawD >= (double)visStart && phDrawD < (double)(visStart + visLen))
            {
                const float xLine = xForSessionSampleD(phDrawD);
                g.setColour(juce::Colours::white.withAlpha(0.92f));
                g.drawLine(xLine,
                           rb.getY(),
                           xLine,
                           rb.getY() + timeline_locator_paint::kRulerPlayheadMarkerLengthPx,
                           1.5f);
            }
        }

        if (player_ != nullptr && player_->isPlaying() && timelineClip_ != nullptr
            && timelineClip_->lengthSamples > 0)
        {
            const float px = xForSessionSampleD(uiPreviewDisplayAbsSample_);
            if (px >= rb.getX() - 2.0f && px <= rb.getRight() + 2.0f)
            {
                g.setColour(juce::Colour(0xffe85566));
                g.drawLine(
                    px,
                    rb.getY() + 1.0f,
                    px,
                    rb.getY() + timeline_locator_paint::kRulerPlayheadMarkerLengthPx + 1.0f,
                    1.15f);
            }
        }
    }

    // --- drawStepGrid
    if (!absTime)
    {
        const int stride = stepGridStrideForMinPx(cw, kTimelineGridMinorMinPx);
        for (int s = 0; s <= nSteps; s += stride)
        {
            const int x = gr.getX() + (int)((float)s * cw);
            juce::Colour c = juce::Colour(0xff333340);
            if (s % 4 == 0)
            {
                c = juce::Colour(0xff454552);
            }
            if (s % nSteps == 0)
            {
                c = juce::Colour(0xff505060);
            }
            g.setColour(c);
            g.drawVerticalLine(x, (float)gr.getY(), (float)gr.getBottom());
        }
        if (stride > 1 && (nSteps % stride) != 0)
        {
            const int x = gr.getX() + (int)((float)nSteps * cw);
            g.setColour(juce::Colour(0xff505060));
            g.drawVerticalLine(x, (float)gr.getY(), (float)gr.getBottom());
        }
    }
    else if (timelineClip_ != nullptr)
    {
        if (pattern_.usesTimelineNotes() && samplesPerPixel_ > 0.0 && std::isfinite(samplesPerPixel_))
        {
            const double sr = effectiveDeviceSampleRate(deviceManager_);
            const double bpm = pattern_.bpm > 0.0 ? pattern_.bpm : 120.0;
            const int tpqI = experimentalEffectiveTicksPerQuarter(pattern_);
            const std::int64_t tpq = (std::int64_t)tpqI;
            const double spp = samplesPerPixel_;

            std::int64_t minorStep = referenceTimelineGridTicks();
            minorStep = juce::jmax<std::int64_t>(1, minorStep);
            while (minorStep < tpq * 1024)
            {
                const double px = spacingPxForTickDelta(minorStep, bpm, tpqI, sr, spp);
                if (px >= kTimelineGridMinorMinPx || minorStep >= tpq)
                {
                    break;
                }
                minorStep *= 2;
            }

            const double beatPx = spacingPxForTickDelta(tpq, bpm, tpqI, sr, spp);
            std::int64_t lineStep = minorStep;
            bool coarseBarsOnly = false;
            if (beatPx < kTimelineGridBeatMinPx && tpq > 0)
            {
                coarseBarsOnly = true;
                std::int64_t barStep = 4 * tpq;
                barStep = juce::jmax<std::int64_t>(1, barStep);
                while (barStep <= tpq * 16384)
                {
                    const double pxBar = spacingPxForTickDelta(barStep, bpm, tpqI, sr, spp);
                    if (pxBar >= kTimelineGridBarMinPx)
                    {
                        break;
                    }
                    barStep *= 2;
                }
                lineStep = barStep;
            }

            const std::int64_t anchor = timelineClip_->timelineAnchorSamples;
            const std::int64_t visHi = visibleEndSamples();
            const std::int64_t tickEnd = relativeSamplesToTicks(visHi - anchor, bpm, tpqI, sr) + lineStep * 4;

            const std::int64_t visLo = visibleStartSamples_;
            std::int64_t tickStartRaw = relativeSamplesToTicks(visLo - anchor, bpm, tpqI, sr) - lineStep * 2;
            std::int64_t tk = floorTickToGridStep(tickStartRaw, lineStep);

            const juce::Colour colMinor = juce::Colour(0xff333340);
            const juce::Colour colHalf = juce::Colour(0xff454552);
            const juce::Colour colBeat = juce::Colour(0xff505060);

            for (; tk <= tickEnd; tk += lineStep)
            {
                const std::int64_t absS = anchor + ticksToSignedSamples(tk, bpm, tpqI, sr);
                const float x = xForSessionSample(absS);
                if (x < (float)gr.getX() - 2.0f || x > (float)gr.getRight() + 2.0f)
                {
                    continue;
                }

                juce::Colour c = colMinor;
                if (coarseBarsOnly)
                {
                    const bool barHit = tpq > 0 && (tk % (4 * tpq) == 0);
                    c = barHit ? colBeat : colMinor;
                }
                else
                {
                    const bool beat = tpq > 0 && (tk % tpq == 0);
                    if (beat)
                    {
                        c = colBeat;
                    }
                    else if (tpq >= 4 && (tk % (tpq / 2) == 0))
                    {
                        c = colHalf;
                    }
                }

                g.setColour(c);
                g.drawVerticalLine(juce::roundToInt(x), (float)gr.getY(), (float)gr.getBottom());
            }
        }
        else if (samplesPerPixel_ > 0.0 && std::isfinite(samplesPerPixel_))
        {
            const std::int64_t lenPat = juce::jmax(std::int64_t{ 1 }, timelineClip_->lengthSamples);
            const float spanPx =
                std::fabs(xForSessionSample(timelineClip_->startSamples + lenPat)
                          - xForSessionSample(timelineClip_->startSamples));
            const float pxPerStepCol = spanPx / (float)juce::jmax(1, nSteps);
            const int stride = stepGridStrideForMinPx(pxPerStepCol, kTimelineGridMinorMinPx);

            auto drawStepLine = [&](const int s) {
                const std::int64_t rel =
                    (std::int64_t)std::llround((double)s * (double)lenPat / (double)nSteps);
                const std::int64_t absS = timelineClip_->startSamples + rel;
                const float x = xForSessionSample(absS);
                juce::Colour c = juce::Colour(0xff333340);
                if (s % 4 == 0)
                {
                    c = juce::Colour(0xff454552);
                }
                if (s == 0 || s == nSteps)
                {
                    c = juce::Colour(0xff505060);
                }
                g.setColour(c);
                g.drawVerticalLine(juce::roundToInt(x), (float)gr.getY(), (float)gr.getBottom());
            };

            for (int s = 0; s <= nSteps; s += stride)
            {
                drawStepLine(s);
            }
            if (stride > 1 && (nSteps % stride) != 0)
            {
                drawStepLine(nSteps);
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

    // --- drawNotes (step diamonds and/or timeline bars)
    float cellWForDiamond = cw;
    if (!pattern_.usesTimelineNotes() && absTime && timelineClip_ != nullptr && nSteps > 0
        && samplesPerPixel_ > 0.0)
    {
        const std::int64_t len = juce::jmax(std::int64_t{1}, timelineClip_->lengthSamples);
        const float x0 = xForSessionSample(timelineClip_->startSamples);
        const float x1 = xForSessionSample(timelineClip_->startSamples + len / nSteps);
        cellWForDiamond = juce::jmax(3.0f, std::fabs(x1 - x0));
    }
    const bool pianoRowMode = (rowLabelMode_ == 1);
    const float diamondScale = pianoRowMode ? 1.1f : 1.0f;
    const float halfW =
        juce::jmax(3.0f, juce::jmin(cellWForDiamond * 0.55f, (float)kRowHeight * 0.85f) * 0.5f) * diamondScale;
    const float halfH = juce::jmax(3.0f, (float)kRowHeight * 0.75f * 0.5f) * diamondScale;

    if (pattern_.usesTimelineNotes() && timelineClip_ != nullptr && samplesPerPixel_ > 0.0)
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
                if (noteSelected)
                {
                    g.setColour(juce::Colour(0xff101012));
                    g.fillRoundedRectangle(noteRect, 2.0f);
                    g.setColour(juce::Colour(0xffe05a7a).withAlpha(0.97f));
                    g.drawRoundedRectangle(noteRect, 2.0f, 2.35f);
                    g.setColour(juce::Colour(0xff8a2c46).withAlpha(0.88f));
                    g.drawRoundedRectangle(noteRect, 2.0f, 1.05f);
                }
                else
                {
                    g.setColour(juce::Colour(0xffe05a7a).withAlpha(0.78f * velA));
                    g.fillRoundedRectangle(noteRect, 2.0f);
                    g.setColour(juce::Colour(0xff8a2c46).withAlpha(0.9f));
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
                if (noteSelected)
                {
                    g.setColour(juce::Colour(0xff101012));
                    g.fillPath(diamond);
                    g.setColour(juce::Colour(0xffe05a7a).withAlpha(0.97f));
                    g.strokePath(diamond, juce::PathStrokeType(2.1f));
                    g.setColour(juce::Colour(0xff8a2c46).withAlpha(0.82f));
                    g.strokePath(diamond, juce::PathStrokeType(1.2f));
                }
                else
                {
                    g.setColour(juce::Colour(0xff8a2c46).withAlpha(0.85f + 0.15f * velA));
                    g.strokePath(diamond, juce::PathStrokeType(1.15f));
                    g.setColour(juce::Colour(0xffe05a7a).withAlpha(0.65f + 0.35f * velA));
                    g.fillPath(diamond);
                }
            }
        }
    }
    else
    {
        for (const auto& hit : pattern_.notes)
        {
            if (hit.midiNote < pitchLow_ || hit.midiNote > pitchHigh_)
            {
                continue;
            }
            const auto rrOpt = visibleRowStripRect(gr, hit.midiNote);
            if (!rrOpt)
            {
                continue;
            }
            const auto& rr = *rrOpt;
            float cx;
            if (absTime && timelineClip_ != nullptr)
            {
                const std::int64_t lenClip = juce::jmax(std::int64_t{1}, timelineClip_->lengthSamples);
                const std::int64_t absNote = absoluteSampleForNoteInClip(
                    timelineClip_->startSamples, hit.step, pattern_.numSteps, lenClip);
                cx = xForSessionSample(absNote);
            }
            else
            {
                cx = (float)gr.getX() + ((float)hit.step + 0.5f) * cw;
            }
            const float cy = (float)rr.getCentreY();
            juce::Path diamond;
            diamond.addQuadrilateral(cx, cy - halfH, cx + halfW, cy, cx, cy + halfH, cx - halfW, cy);
            g.setColour(juce::Colour(0xff8a2c46));
            g.strokePath(diamond, juce::PathStrokeType(1.2f));
            g.setColour(juce::Colour(0xffe05a7a).withAlpha(0.92f));
            g.fillPath(diamond);
            if (pianoRowMode && timelineNotesDisplayComboId_ != 1 && absTime && timelineClip_ != nullptr
                && cw >= 28.0f && (float)kRowHeight >= 12.0f)
            {
                const float lh = (float)juce::jmax(0, rr.getHeight() - 2);
                if (lh >= 9.0f)
                {
                    const juce::String label = juce::MidiMessage::getMidiNoteName(hit.midiNote, true, true, 3);
                    const float lw = juce::jmax(24.0f, cw - 4.0f);
                    const juce::Rectangle<float> labelR(cx - lw * 0.5f, (float)rr.getY() + 1.0f, lw, lh);
                    g.setColour(juce::Colours::white.withAlpha(0.92f));
                    g.setFont(juce::Font(juce::FontOptions().withHeight(12.0f)));
                    g.drawText(label, labelR, juce::Justification::centred, true);
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

    // --- drawGlobalPlayhead (grid)
    if (absTime && transport_ != nullptr && samplesPerPixel_ > 0.0)
    {
        const float px = xForSessionSampleD(uiPlayheadDisplaySamples_);
        if (px >= (float)gr.getX() - 2.0f && px <= (float)gr.getRight() + 2.0f)
        {
            g.setColour(juce::Colour(0xff66ddff));
            g.drawLine(px, (float)gr.getY(), px, (float)gr.getBottom(), 1.35f);
        }
    }

    // --- drawPreviewPlayhead (grid)
    if (player_ != nullptr && player_->isPlaying())
    {
        const float ph = player_->getPlayheadNormalized();
        if (absTime && timelineClip_ != nullptr && timelineClip_->lengthSamples > 0)
        {
            const float px = xForSessionSampleD(uiPreviewDisplayAbsSample_);
            if (px >= (float)gr.getX() - 2.0f && px <= (float)gr.getRight() + 2.0f)
            {
                g.setColour(juce::Colour(0xffe85566));
                g.drawLine(px, (float)gr.getY(), px, (float)gr.getBottom(), 1.15f);
            }
        }
        else if (cw > 0.0f)
        {
            const float px = (float)gr.getX() + ph * (float)gr.getWidth();
            g.setColour(juce::Colour(0xffe85566));
            g.drawLine(px, (float)gr.getY(), px, (float)gr.getBottom(), 1.2f);
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
}
