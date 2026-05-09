#include "ExperimentalPianoRollView.h"
#include "ExperimentalMidiPatternPlayer.h"
#include "instruments/InstrumentTrackController.h"
#include "ui/TimelineRulerView.h"

#include "domain/Session.h"
#include "transport/Transport.h"
#include "ui/TimelineViewportModel.h"

#include <cmath>

#include <algorithm>

namespace
{
    constexpr bool kMidiEditorVerbosePianoRollMouseLog = false;

    constexpr float kMinTickSpacingPx = 6.0f;

    constexpr double kStepCandidatesSec[]
        = { 0.1,  0.25, 0.5,  1.0,  2.0,  5.0,  10.0, 30.0,
            60.0, 300.0, 600.0, 3600.0 };
    constexpr int kNumStepCandidates
        = (int)(sizeof(kStepCandidatesSec) / sizeof(kStepCandidatesSec[0]));

    [[nodiscard]] float glyphLayoutWidthPx(const juce::Font& f, const juce::String& text) noexcept
    {
        juce::GlyphArrangement glyphs;
        glyphs.addLineOfText(f, text, 0.0f, 0.0f);
        const int n = glyphs.getNumGlyphs();
        if (n <= 0)
        {
            return 0.0f;
        }
        return glyphs.getBoundingBox(0, n, true).getWidth();
    }

    [[nodiscard]] float referenceLabelMinSpacingPx() noexcept
    {
        const juce::Font f(juce::FontOptions(10.0f));
        return glyphLayoutWidthPx(f, "00:00.000") + 8.0f;
    }

    [[nodiscard]] double pickTickStepSec(const double pxPerSec) noexcept
    {
        if (pxPerSec <= 0.0 || !std::isfinite(pxPerSec))
        {
            return kStepCandidatesSec[kNumStepCandidates - 1];
        }
        double chosen = kStepCandidatesSec[kNumStepCandidates - 1];
        for (const double step : kStepCandidatesSec)
        {
            if (step * pxPerSec >= (double)kMinTickSpacingPx)
            {
                chosen = step;
                break;
            }
        }
        return chosen;
    }

    [[nodiscard]] double pickLabelStepSec(const double pxPerSec, const double tickStepSec) noexcept
    {
        const float minLabelPx = referenceLabelMinSpacingPx();
        if (pxPerSec <= 0.0 || !std::isfinite(pxPerSec))
        {
            return juce::jmax(tickStepSec, kStepCandidatesSec[kNumStepCandidates - 1]);
        }
        for (const double step : kStepCandidatesSec)
        {
            if (step + 1e-15 < tickStepSec)
            {
                continue;
            }
            if (step * pxPerSec >= (double)minLabelPx)
            {
                return step;
            }
        }
        double coarsestGeTick = tickStepSec;
        for (const double step : kStepCandidatesSec)
        {
            if (step + 1e-15 >= tickStepSec)
            {
                coarsestGeTick = step;
            }
        }
        return coarsestGeTick;
    }

    [[nodiscard]] juce::String formatRulerTimeLabel(const double seconds, const double stepSec) noexcept
    {
        if (!std::isfinite(seconds) || !std::isfinite(stepSec))
        {
            return {};
        }
        if (std::abs(seconds) < 1e-12)
        {
            return "0s";
        }
        if (seconds < 60.0)
        {
            if (stepSec >= 1.0 - 1e-15)
            {
                return juce::String((juce::int64)std::llround(seconds)) + "s";
            }
            if (stepSec >= 0.1 - 1e-15)
            {
                return juce::String(seconds, 1) + "s";
            }
            return juce::String(seconds, 2) + "s";
        }
        const auto totalMs = (std::int64_t)std::llround(seconds * 1000.0);
        const std::int64_t m = totalMs / 60000;
        const std::int64_t s = (totalMs % 60000) / 1000;
        const std::int64_t ms = totalMs % 1000;
        if (stepSec >= 1.0 - 1e-15)
        {
            return juce::String::formatted("%lld:%02lld", (long long)m, (long long)s);
        }
        return juce::String::formatted("%lld:%02lld.%03lld", (long long)m, (long long)s, (long long)ms);
    }

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
} // namespace

ExperimentalPianoRollView::ExperimentalPianoRollView(ExperimentalMidiPattern& pattern,
                                                       ExperimentalMidiPatternPlayer* player)
    : pattern_(pattern)
    , player_(player)
{
    setOpaque(true);
    uiTimerHzConfigured_ = kMidiRollTimerHzIdle;
    startTimerHz(kMidiRollTimerHzIdle);
}

int ExperimentalPianoRollView::timelineRulerHeight() const noexcept
{
    return useAbsoluteTimeline() ? kRulerHeight : 0;
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
    return top.removeFromLeft(kKeyboardWidth);
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
    top.removeFromLeft(kKeyboardWidth);
    return top;
}

juce::Rectangle<int> ExperimentalPianoRollView::keyboardBounds() const
{
    auto r = getLocalBounds();
    r.removeFromTop(timelineRulerHeight());
    return r.removeFromLeft(kKeyboardWidth);
}

juce::Rectangle<int> ExperimentalPianoRollView::gridBounds() const
{
    auto r = getLocalBounds();
    r.removeFromTop(timelineRulerHeight());
    r.removeFromLeft(kKeyboardWidth);
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
    timelineClip_ = timelineClip;
    session_ = session;
    transport_ = transport;
    deviceManager_ = deviceManager;
    instrumentTrackController_ = trackController;
    mainTimelineViewport_ = mainTimelineViewport;
    if (changed && useAbsoluteTimeline())
    {
        applyViewportAfterContextBound();
    }
    sessionTransportSnapshotValid_ = false;
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
        timelineClip_->midiRollVisibleStartSamples = 0;
        timelineClip_->midiRollSamplesPerPixel = 0.0;
        timelineClip_->midiRollFollowEnabled = false;
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

void ExperimentalPianoRollView::syncViewportToBoundClip() noexcept
{
    if (timelineClip_ == nullptr || !useAbsoluteTimeline())
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

    const auto gr = gridBounds();
    const double w = juce::jmax(1.0, (double)juce::jmax(1, gr.getWidth()));
    const double sr = effectiveDeviceSampleRate(deviceManager_);

    double spp = 0.0;
    if (mainTimelineViewport_ != nullptr)
    {
        spp = mainTimelineViewport_->getSamplesPerPixel();
    }
    if (spp <= 0.0 || !std::isfinite(spp))
    {
        spp = juce::jmax(1.0, sr / 10.0);
    }
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
        const double sr = effectiveDeviceSampleRate(deviceManager_);

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
            const double predicted = uiPlayheadExtrapBaseSample_ + (nowSec - uiPlayheadExtrapWallSec_) * sr;
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
            uiPlayheadDisplaySamples_ = uiPlayheadExtrapBaseSample_ + (nowSec - uiPlayheadExtrapWallSec_) * sr;
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

    if (absTime && timelineClip_ != nullptr)
    {
        if (!clipGeometrySnapshotValid_
            || timelineClip_->startSamples != lastObservedClipStartSamplesUi_
            || timelineClip_->lengthSamples != lastObservedClipLengthSamplesUi_)
        {
            lastObservedClipStartSamplesUi_ = timelineClip_->startSamples;
            lastObservedClipLengthSamplesUi_ = timelineClip_->lengthSamples;
            clipGeometrySnapshotValid_ = true;
            structuralRepaint = true;
        }
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
    const int span = kPitchHigh - kPitchLow + 1;
    const int clampedRow = juce::jlimit(0, span - 1, row);
    return kPitchHigh - clampedRow;
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

void ExperimentalPianoRollView::handleTimelineNotesMouseDown(const juce::MouseEvent& e)
{
    if (timelineClip_ == nullptr)
    {
        return;
    }
    const int pitch = pitchAtY(e.getPosition().getY());
    if (pitch < kPitchLow || pitch > kPitchHigh)
    {
        return;
    }

    const double sr = effectiveDeviceSampleRate(deviceManager_);
    const double bpm = pattern_.bpm > 0.0 ? pattern_.bpm : 120.0;
    const int tpq = experimentalEffectiveTicksPerQuarter(pattern_);

    const std::int64_t absClick = sampleAtGridX((float)e.getPosition().getX());
    const std::int64_t relSamples = absClick - timelineClip_->startSamples;
    if (relSamples < 0)
    {
        return;
    }

    for (int i = (int)pattern_.timelineNotes.size() - 1; i >= 0; --i)
    {
        const auto& tn = pattern_.timelineNotes[(size_t)i];
        if (tn.midiNote != pitch)
        {
            continue;
        }
        const std::int64_t a0 = absoluteSampleForTimelineNote(timelineClip_->startSamples, tn, pattern_, sr);
        const std::int64_t durS = ticksToRelativeSamples(
            juce::jmax<std::int64_t>(1, tn.durationTicks), bpm, tpq, sr);
        const std::int64_t a1 = a0 + juce::jmax<std::int64_t>(1, durS);
        if (absClick >= a0 && absClick < a1)
        {
            pattern_.timelineNotes.erase(pattern_.timelineNotes.begin() + i);
            if (instrumentTrackController_ != nullptr)
            {
                instrumentTrackController_->notifyClipPatternMutated(timelineClip_->id);
            }
            repaint();
            return;
        }
    }

    std::int64_t rawTick = relativeSamplesToTicks(relSamples, bpm, tpq, sr);
    const std::int64_t snapG = musicalSnapGridTicks();
    if (snapG > 0)
    {
        rawTick = (std::int64_t)std::llround((double)rawTick / (double)snapG) * snapG;
    }
    rawTick = juce::jmax<std::int64_t>(0, rawTick);

    TimelineMidiNote nn;
    nn.midiNote = pitch;
    nn.velocity = 100;
    nn.channel = 10;
    nn.startTick = rawTick;
    nn.durationTicks = snapG > 0 ? snapG : 240;

    pattern_.timelineNotes.push_back(nn);
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

    if (instrumentTrackController_ != nullptr)
    {
        instrumentTrackController_->notifyClipExperimentalMusicalTimingChanged();
    }
    repaint();
}

void ExperimentalPianoRollView::mouseDown(const juce::MouseEvent& e)
{
    const auto pos = e.getPosition();
    const auto gr = gridBounds();

    if (gr.contains(pos))
    {
        if (useAbsoluteTimeline() && pattern_.usesTimelineNotes() && timelineClip_ != nullptr)
        {
            handleTimelineNotesMouseDown(e);
            return;
        }
        const int step = useAbsoluteTimeline() ? stepAtTimelineX(pos.getX()) : stepAtPatternX(pos.getX());
        const int pitch = pitchAtY(pos.getY());
        if (kMidiEditorVerbosePianoRollMouseLog)
        {
            ExperimentalMidiPatternPlayer::writeMidiEditorLogLine(
                "piano-roll: mouseDown x=" + juce::String(pos.getX()) + " y=" + juce::String(pos.getY())
                + " note=" + juce::String(pitch) + " step=" + juce::String(step));
        }
        pattern_.toggleHit(pitch, step);
        if (kMidiEditorVerbosePianoRollMouseLog)
        {
            ExperimentalMidiPatternPlayer::writeMidiEditorLogLine(
                "piano-roll: toggle note=" + juce::String(pitch) + " step=" + juce::String(step) + " noteCount="
                + juce::String((int)pattern_.notes.size()));
        }
        if (instrumentTrackController_ != nullptr && timelineClip_ != nullptr)
        {
            instrumentTrackController_->notifyClipPatternMutated(timelineClip_->id);
        }
        repaint();
    }
}

void ExperimentalPianoRollView::mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    if (!useAbsoluteTimeline())
    {
        return;
    }
    if (!hasValidViewportState())
    {
        seedViewportFromMainTimelineOrFallback();
    }
    const auto gr = gridBounds();
    const auto rt = rulerTrackBounds();
    const bool inTimeline = gr.contains(e.getPosition())
                            || (!rt.isEmpty() && rt.contains(e.getPosition()));
    if (!inTimeline || samplesPerPixel_ <= 0.0)
    {
        return;
    }
    const float x = (float)e.position.getX();
    const float ox = (float)gr.getX();
    const std::int64_t sAtPointer = sampleAtGridX(x);

    if (e.mods.isShiftDown())
    {
        const double panPx = (double)wheel.deltaY * 32.0;
        visibleStartSamples_ = juce::jmax(
            std::int64_t{0},
            visibleStartSamples_ + (std::int64_t)std::llround(panPx * samplesPerPixel_));
        sessionTransportSnapshotValid_ = false;
        syncViewportToBoundClip();
        repaint();
        return;
    }

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

    if (absTime)
    {
        if (!hasValidViewportState())
        {
            seedViewportFromMainTimelineOrFallback();
        }
    }

    const float cw = cellWidth();
    const int nSteps = juce::jmax(1, pattern_.numSteps);

    auto rowRect = [&](const int midiNote) -> juce::Rectangle<int> {
        const int rowFromTop = kPitchHigh - midiNote;
        return gr.withY(gr.getY() + rowFromTop * kRowHeight).withHeight(kRowHeight);
    };

    // --- Base + grid rows (gridBounds only; never tint the ruler strip)
    g.fillAll(juce::Colour(0xff1a1a1e));
    for (int pitch = kPitchHigh; pitch >= kPitchLow; --pitch)
    {
        const auto rr = rowRect(pitch);
        if (isBlackKey(pitch))
        {
            g.setColour(juce::Colour(0xff25252d));
        }
        else
        {
            g.setColour(juce::Colour(0xff1f1f26));
        }
        g.fillRect(rr);
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

    // --- Timeline ruler: filled ranges + ticks + labels + playheads (ruler strip only; never use gridBounds for fills)
    if (absTime && !rulerTrack.isEmpty())
    {
        const double sampleRate = effectiveDeviceSampleRate(deviceManager_);
        const std::int64_t visStart = visibleStartSamples_;
        const std::int64_t visEnd = visibleEndSamples();
        const juce::Rectangle<float> rb = rulerTrack.toFloat();
        const double pxPerSec = sampleRate / samplesPerPixel_;

        // --- Ruler-only filled range markers (same semantics as main timeline ruler; never tint
        // gridBounds — see prior grid fills: bound clip span + cycle/locator band.)
        if (session_ != nullptr && transport_ != nullptr)
        {
            const std::int64_t locL = session_->getLeftLocatorSamples();
            const std::int64_t locR = session_->getRightLocatorSamples();
            const bool cycleOn = transport_->readCycleEnabledForUi();
            if (locR > 0)
            {
                const float x0 = xForSessionSample(locL);
                const float x1 = xForSessionSample(locR);
                const float fillLeft = juce::jmin(x0, x1);
                const float fillRight = juce::jmax(x0, x1);
                const float clipL = juce::jmax(fillLeft, rb.getX());
                const float clipR = juce::jmin(fillRight, rb.getRight());
                const float bandW = clipR - clipL;
                const bool validInterval = locR > locL;
                if (bandW > 0.5f)
                {
                    const juce::Colour fillCol = validInterval
                        ? (cycleOn ? juce::Colour(0xff7058e8).withAlpha(0.52f)
                                   : juce::Colour(0xffb8c2d8).withAlpha(0.36f))
                        : juce::Colour(0xfff06828).withAlpha(0.52f);
                    g.setColour(fillCol);
                    g.fillRect(clipL, rb.getY(), bandW, rb.getHeight());
                }
            }
        }

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

        if (pxPerSec > 0.0 && std::isfinite(pxPerSec))
        {
            const double tickStepSec = pickTickStepSec(pxPerSec);
            const double labelStepSec = pickLabelStepSec(pxPerSec, tickStepSec);
            const float hShortMinor = juce::jmax(3.0f, rb.getHeight() * 0.28f);
            const float hShortMajor = juce::jmax(5.0f, rb.getHeight() * 0.42f);

            const auto xAtSample = [&](const std::int64_t s) {
                return xForSessionSample(s);
            };

            // Minor ticks (major positions are drawn again below for stronger ticks + labels)
            g.setColour(juce::Colour(0xff7a8aa0).withAlpha(0.42f));
            for (int k = 0;; ++k)
            {
                const std::int64_t s
                    = (std::int64_t)std::llround((double)k * tickStepSec * sampleRate);
                if (s > visEnd + (std::int64_t)std::llround(labelStepSec * sampleRate * 2.0))
                {
                    break;
                }
                if (s < visStart)
                {
                    continue;
                }
                const float x = xAtSample(s);
                if (x < rb.getX() - 1.0f || x > rb.getRight() + 1.0f)
                {
                    continue;
                }
                g.drawLine(x, rb.getBottom() - 1.0f, x, rb.getBottom() - 1.0f - hShortMinor, 1.0f);
            }

            // Major ticks + label cadence
            g.setColour(juce::Colour(0xff7a8aa0).withAlpha(0.58f));
            const juce::Font labelFont(juce::FontOptions(10.0f));
            g.setFont(labelFont);
            g.setColour(juce::Colours::white.withAlpha(0.52f));
            const float labelBaselineY = rb.getBottom() - 2.0f - hShortMajor - 2.0f;

            for (int k = 0;; ++k)
            {
                const std::int64_t samp
                    = (std::int64_t)std::llround((double)k * labelStepSec * sampleRate);
                if (samp > visEnd + (std::int64_t)std::llround(labelStepSec * sampleRate))
                {
                    break;
                }
                if (samp < visStart)
                {
                    continue;
                }
                const float x = xAtSample(samp);
                if (x < rb.getX() - 1.0f || x > rb.getRight() + 1.0f)
                {
                    continue;
                }
                g.setColour(juce::Colour(0xff7a8aa0).withAlpha(0.58f));
                g.drawLine(x, rb.getBottom() - 1.0f, x, rb.getBottom() - 1.0f - hShortMajor, 1.0f);

                const double sec = (double)samp / sampleRate;
                const juce::String text = formatRulerTimeLabel(sec, labelStepSec);
                const float tw = glyphLayoutWidthPx(labelFont, text);
                if (x - tw * 0.5f <= rb.getX() || x + tw * 0.5f >= rb.getRight())
                {
                    continue;
                }
                g.setColour(juce::Colours::white.withAlpha(0.52f));
                g.drawText(text, juce::Rectangle<float>(x - tw * 0.5f, labelBaselineY - 11.0f, tw, 11.0f),
                           juce::Justification::centredBottom, true);
            }
        }

        // Locator verticals through ruler strip (grid gets thin lines separately; no filled band in grid)
        if (session_ != nullptr && samplesPerPixel_ > 0.0)
        {
            const std::int64_t locL = session_->getLeftLocatorSamples();
            const std::int64_t locR = session_->getRightLocatorSamples();
            auto rulerLine = [&](const std::int64_t s, const juce::Colour& col) {
                const float x = xForSessionSample(s);
                if (x >= rb.getX() - 1.0f && x <= rb.getRight() + 1.0f)
                {
                    g.setColour(col);
                    g.drawVerticalLine(juce::roundToInt(x), rb.getY() + 0.5f, rb.getBottom() - 0.5f);
                }
            };
            rulerLine(locL, juce::Colours::white.withAlpha(0.42f));
            if (locR > 0)
            {
                rulerLine(locR, juce::Colours::white.withAlpha(0.36f));
            }
        }

        // Global playhead through ruler (line only): draw using UI-extrapolated sample (smooth sub-step motion).
        if (transport_ != nullptr)
        {
            const float px = xForSessionSampleD(uiPlayheadDisplaySamples_);
            if (px >= rb.getX() - 2.0f && px <= rb.getRight() + 2.0f)
            {
                g.setColour(juce::Colour(0xff66ddff));
                g.drawLine(px, rb.getY() + 1.0f, px, rb.getBottom() - 1.0f, 1.35f);
            }
        }

        if (player_ != nullptr && player_->isPlaying() && timelineClip_ != nullptr
            && timelineClip_->lengthSamples > 0)
        {
            const float px = xForSessionSampleD(uiPreviewDisplayAbsSample_);
            if (px >= rb.getX() - 2.0f && px <= rb.getRight() + 2.0f)
            {
                g.setColour(juce::Colour(0xffe85566));
                g.drawLine(px, rb.getY() + 2.0f, px, rb.getBottom() - 2.0f, 1.1f);
            }
        }
    }

    // --- drawStepGrid
    if (!absTime)
    {
        for (int s = 0; s <= nSteps; ++s)
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
    }
    else if (timelineClip_ != nullptr)
    {
        if (pattern_.usesTimelineNotes() && samplesPerPixel_ > 0.0)
        {
            const double sr = effectiveDeviceSampleRate(deviceManager_);
            const double bpm = pattern_.bpm > 0.0 ? pattern_.bpm : 120.0;
            const int tpq = experimentalEffectiveTicksPerQuarter(pattern_);
            const std::int64_t stepTicks = referenceTimelineGridTicks();
            const std::int64_t lenPat = juce::jmax(std::int64_t{1}, timelineClip_->lengthSamples);
            const std::int64_t maxTick =
                relativeSamplesToTicks(lenPat, bpm, tpq, sr) + stepTicks * 4;
            for (std::int64_t tk = 0; tk <= maxTick; tk += stepTicks)
            {
                const std::int64_t absS =
                    timelineClip_->startSamples + ticksToRelativeSamples(tk, bpm, tpq, sr);
                const float x = xForSessionSample(absS);
                if (x < (float)gr.getX() - 2.0f || x > (float)gr.getRight() + 2.0f)
                {
                    continue;
                }
                const bool beat = (tpq > 0) && (tk % (std::int64_t)tpq == 0);
                juce::Colour c = beat ? juce::Colour(0xff505060) : juce::Colour(0xff333340);
                if (!beat && tpq >= 4 && (tk % ((std::int64_t)tpq / 2) == 0))
                {
                    c = juce::Colour(0xff454552);
                }
                g.setColour(c);
                g.drawVerticalLine(juce::roundToInt(x), (float)gr.getY(), (float)gr.getBottom());
            }
        }
        else
        {
            const std::int64_t lenPat = juce::jmax(std::int64_t{1}, timelineClip_->lengthSamples);
            for (int s = 0; s <= nSteps; ++s)
            {
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
    const float halfW =
        juce::jmax(3.0f, juce::jmin(cellWForDiamond * 0.55f, (float)kRowHeight * 0.85f) * 0.5f);
    const float halfH = juce::jmax(3.0f, (float)kRowHeight * 0.75f * 0.5f);

    if (pattern_.usesTimelineNotes() && timelineClip_ != nullptr && samplesPerPixel_ > 0.0)
    {
        const double sr = effectiveDeviceSampleRate(deviceManager_);
        const double bpm = pattern_.bpm > 0.0 ? pattern_.bpm : 120.0;
        const int tpq = experimentalEffectiveTicksPerQuarter(pattern_);
        const bool paintBars = (timelineNotesDisplayComboId_ == 2);
        const float hitHalfW = juce::jmax(3.5f, juce::jmin(6.0f, (float)kRowHeight * 0.42f));
        const float hitHalfH = juce::jmax(3.5f, (float)kRowHeight * 0.4f);
        for (const auto& tn : pattern_.timelineNotes)
        {
            if (tn.midiNote < kPitchLow || tn.midiNote > kPitchHigh)
            {
                continue;
            }
            const auto rr = rowRect(tn.midiNote);
            const std::int64_t a0 = absoluteSampleForTimelineNote(timelineClip_->startSamples, tn, pattern_, sr);
            const float velA = juce::jlimit(0.28f, 1.0f, (float)tn.velocity / 127.0f);

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
                auto noteRect =
                    juce::Rectangle<float>(xL, (float)rr.getY() + 2.0f, xR - xL, (float)rr.getHeight() - 4.0f);
                if (noteRect.getWidth() < 3.0f)
                {
                    noteRect = noteRect.withSizeKeepingCentre(4.0f, noteRect.getHeight());
                }
                g.setColour(juce::Colour(0xffe05a7a).withAlpha(0.78f * velA));
                g.fillRoundedRectangle(noteRect, 2.0f);
                g.setColour(juce::Colour(0xff8a2c46).withAlpha(0.9f));
                g.drawRoundedRectangle(noteRect, 2.0f, 1.1f);
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
                g.setColour(juce::Colour(0xff8a2c46).withAlpha(0.85f + 0.15f * velA));
                g.strokePath(diamond, juce::PathStrokeType(1.15f));
                g.setColour(juce::Colour(0xffe05a7a).withAlpha(0.65f + 0.35f * velA));
                g.fillPath(diamond);
            }
        }
    }
    else
    {
        for (const auto& hit : pattern_.notes)
        {
            if (hit.midiNote < kPitchLow || hit.midiNote > kPitchHigh)
            {
                continue;
            }
            const auto rr = rowRect(hit.midiNote);
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

    for (int pitch = kPitchHigh; pitch >= kPitchLow; --pitch)
    {
        const int rowFromTop = kPitchHigh - pitch;
        const int y = kb.getY() + rowFromTop * kRowHeight;
        auto wr = kb.withY(y).withHeight(kRowHeight);

        if (pitch == kPitchLow || pitch == kPitchHigh)
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
        if (kk == 0)
        {
            const juce::String label = juce::MidiMessage::getMidiNoteName(pitch, true, true, 3);
            g.setColour(isBlackKey(pitch) ? juce::Colours::lightgrey : juce::Colours::black);
            g.setFont(10.0f);
            g.drawText(label, wr.reduced(2, 0), juce::Justification::centredLeft, true);
        }
    }
}
