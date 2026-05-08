#include "ExperimentalPianoRollView.h"
#include "ExperimentalMidiPatternPlayer.h"
#include "instruments/InstrumentTrackController.h"
#include "ui/TimelineRulerView.h"

#include "domain/Session.h"
#include "transport/Transport.h"

#include <cmath>

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_devices/juce_audio_devices.h>

namespace
{
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
} // namespace

ExperimentalPianoRollView::ExperimentalPianoRollView(ExperimentalMidiPattern& pattern,
                                                       ExperimentalMidiPatternPlayer* player)
    : pattern_(pattern)
    , player_(player)
{
    setOpaque(true);
    startTimerHz(20);
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
                                                          juce::AudioDeviceManager* deviceManager) noexcept
{
    const bool changed = timelineClip_ != timelineClip || session_ != session || transport_ != transport
                         || deviceManager_ != deviceManager;
    timelineClip_ = timelineClip;
    session_ = session;
    transport_ = transport;
    deviceManager_ = deviceManager;
    if (changed)
    {
        viewportInitialized_ = false;
    }
    sessionTransportSnapshotValid_ = false;
    clipGeometrySnapshotValid_ = false;
    lastObservedNoteCountUi_ = -1;
    ensureViewportSeeded();
    repaint();
}

void ExperimentalPianoRollView::setFollowPlayheadEnabled(const bool on) noexcept
{
    if (followPlayhead_ == on)
    {
        return;
    }
    followPlayhead_ = on;
    repaint();
}

void ExperimentalPianoRollView::seedOrResetViewport()
{
    viewportInitialized_ = false;
    ensureViewportSeeded();
    sessionTransportSnapshotValid_ = false;
    clipGeometrySnapshotValid_ = false;
    lastObservedNoteCountUi_ = -1;
    repaint();
}

bool ExperimentalPianoRollView::useAbsoluteTimeline() const noexcept
{
    return timelineClip_ != nullptr && session_ != nullptr && transport_ != nullptr;
}

void ExperimentalPianoRollView::ensureViewportSeeded()
{
    if (!useAbsoluteTimeline())
    {
        return;
    }
    if (viewportInitialized_ && samplesPerPixel_ > 0.0)
    {
        return;
    }
    const auto gr = gridBounds();
    const double w = juce::jmax(1.0, (double)juce::jmax(1, gr.getWidth()));
    const double sr = effectiveDeviceSampleRate(deviceManager_);
    samplesPerPixel_ = juce::jmax(1.0, sr / 10.0);
    const std::int64_t visibleLen = (std::int64_t)std::llround(w * samplesPerPixel_);
    const std::int64_t anchor = timelineClip_->startSamples;
    visibleStartSamples_ = juce::jmax(std::int64_t{0}, anchor - visibleLen / 4);
    viewportInitialized_ = true;
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

void ExperimentalPianoRollView::timerCallback()
{
    bool dirty = false;

    if (player_ != nullptr && player_->isPlaying())
    {
        dirty = true;
    }

    if (useAbsoluteTimeline() && transport_ != nullptr)
    {
        if (transport_->readPlaybackIntentForUi() == PlaybackIntent::Playing)
        {
            dirty = true;
        }

        if (session_ != nullptr)
        {
            const std::int64_t ph = transport_->readPlayheadSamplesForUi();
            const std::int64_t l = session_->getLeftLocatorSamples();
            const std::int64_t r = session_->getRightLocatorSamples();
            const bool cy = transport_->readCycleEnabledForUi();
            if (!sessionTransportSnapshotValid_ || ph != lastObservedPlayheadUi_ || l != lastObservedLocLUi_
                || r != lastObservedLocRUi_ || cy != lastObservedCycleUi_)
            {
                lastObservedPlayheadUi_ = ph;
                lastObservedLocLUi_ = l;
                lastObservedLocRUi_ = r;
                lastObservedCycleUi_ = cy;
                sessionTransportSnapshotValid_ = true;
                dirty = true;
            }
        }

        if (followPlayhead_ && transport_->readPlaybackIntentForUi() == PlaybackIntent::Playing)
        {
            const auto gr = gridBounds();
            const double w = juce::jmax(1.0, (double)juce::jmax(1, gr.getWidth()));
            if (samplesPerPixel_ > 0.0 && std::isfinite(samplesPerPixel_))
            {
                const std::int64_t ph = transport_->readPlayheadSamplesForUi();
                const std::int64_t half = (std::int64_t)std::llround(0.5 * w * samplesPerPixel_);
                const std::int64_t target = juce::jmax(std::int64_t{0}, ph - half);
                if (target != visibleStartSamples_)
                {
                    visibleStartSamples_ = target;
                    dirty = true;
                }
            }
        }
    }

    if (useAbsoluteTimeline() && timelineClip_ != nullptr)
    {
        if (!clipGeometrySnapshotValid_
            || timelineClip_->startSamples != lastObservedClipStartSamplesUi_
            || timelineClip_->lengthSamples != lastObservedClipLengthSamplesUi_)
        {
            lastObservedClipStartSamplesUi_ = timelineClip_->startSamples;
            lastObservedClipLengthSamplesUi_ = timelineClip_->lengthSamples;
            clipGeometrySnapshotValid_ = true;
            dirty = true;
        }
    }

    const int noteCount = (int)pattern_.notes.size();
    if (noteCount != lastObservedNoteCountUi_)
    {
        lastObservedNoteCountUi_ = noteCount;
        dirty = true;
    }

    if (dirty)
    {
        repaint();
    }
}

void ExperimentalPianoRollView::resized()
{
    Component::resized();
    if (useAbsoluteTimeline())
    {
        ensureViewportSeeded();
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

void ExperimentalPianoRollView::mouseDown(const juce::MouseEvent& e)
{
    const auto pos = e.getPosition();
    const auto gr = gridBounds();

    if (gr.contains(pos))
    {
        const int step = useAbsoluteTimeline() ? stepAtTimelineX(pos.getX()) : stepAtPatternX(pos.getX());
        const int pitch = pitchAtY(pos.getY());
        ExperimentalMidiPatternPlayer::writeMidiEditorLogLine(
            "piano-roll: mouseDown x=" + juce::String(pos.getX()) + " y=" + juce::String(pos.getY())
            + " note=" + juce::String(pitch) + " step=" + juce::String(step));
        pattern_.toggleHit(pitch, step);
        ExperimentalMidiPatternPlayer::writeMidiEditorLogLine(
            "piano-roll: toggle note=" + juce::String(pitch) + " step=" + juce::String(step) + " noteCount="
            + juce::String((int)pattern_.notes.size()));
        repaint();
    }
}

void ExperimentalPianoRollView::mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    if (!useAbsoluteTimeline())
    {
        return;
    }
    ensureViewportSeeded();
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
    repaint();
}

void ExperimentalPianoRollView::paint(juce::Graphics& g)
{
    if (!loggedFirstPaint_)
    {
        loggedFirstPaint_ = true;
        ExperimentalMidiPatternPlayer::writeMidiEditorLogLine(
            "piano-roll: paint width=" + juce::String(getWidth()) + " height=" + juce::String(getHeight()));
    }

    const auto kb = keyboardBounds();
    const auto gr = gridBounds();
    const auto rulerCorner = rulerCornerBounds();
    const auto rulerTrack = rulerTrackBounds();

    const bool absTime = useAbsoluteTimeline();
    if (absTime)
    {
        ensureViewportSeeded();
        // Ruler ticks/mapping must not depend on a valid prior zoom state; repair before any xForSessionSample use.
        if (samplesPerPixel_ <= 0.0 || !std::isfinite(samplesPerPixel_))
        {
            const auto grSeed = gridBounds();
            const double w = juce::jmax(1.0, (double)juce::jmax(1, grSeed.getWidth()));
            const double sr = effectiveDeviceSampleRate(deviceManager_);
            samplesPerPixel_ = juce::jmax(1.0, sr / 10.0);
            viewportInitialized_ = true;
            const std::int64_t visibleLen = (std::int64_t)std::llround(w * samplesPerPixel_);
            const std::int64_t anchor = timelineClip_ != nullptr ? timelineClip_->startSamples : std::int64_t{0};
            visibleStartSamples_ = juce::jmax(std::int64_t{0}, anchor - visibleLen / 4);
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

        // Global playhead through ruler (line only)
        if (transport_ != nullptr)
        {
            const std::int64_t ph = transport_->readPlayheadSamplesForUi();
            const float px = xForSessionSample(ph);
            if (px >= rb.getX() - 2.0f && px <= rb.getRight() + 2.0f)
            {
                g.setColour(juce::Colour(0xff66ddff));
                g.drawLine(px, rb.getY() + 1.0f, px, rb.getBottom() - 1.0f, 1.35f);
            }
        }

        if (player_ != nullptr && player_->isPlaying() && timelineClip_ != nullptr
            && timelineClip_->lengthSamples > 0)
        {
            const float ph = player_->getPlayheadNormalized();
            const std::int64_t lenClip = timelineClip_->lengthSamples;
            const std::int64_t absPrev
                = timelineClip_->startSamples + (std::int64_t)std::llround(ph * (double)lenClip);
            const float px = xForSessionSample(absPrev);
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
        const std::int64_t lenPat = juce::jmax(std::int64_t{1}, timelineClip_->lengthSamples);
        for (int s = 0; s <= nSteps; ++s)
        {
            const std::int64_t rel
                = (std::int64_t)std::llround((double)s * (double)lenPat / (double)nSteps);
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

    // --- drawNotes
    float cellWForDiamond = cw;
    if (absTime && timelineClip_ != nullptr && nSteps > 0 && samplesPerPixel_ > 0.0)
    {
        const std::int64_t len = juce::jmax(std::int64_t{1}, timelineClip_->lengthSamples);
        const float x0 = xForSessionSample(timelineClip_->startSamples);
        const float x1 = xForSessionSample(timelineClip_->startSamples + len / nSteps);
        cellWForDiamond = juce::jmax(3.0f, std::fabs(x1 - x0));
    }
    const float halfW
        = juce::jmax(3.0f, juce::jmin(cellWForDiamond * 0.55f, (float)kRowHeight * 0.85f) * 0.5f);
    const float halfH = juce::jmax(3.0f, (float)kRowHeight * 0.75f * 0.5f);

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

    // --- drawGlobalPlayhead (grid)
    if (absTime && transport_ != nullptr && samplesPerPixel_ > 0.0)
    {
        const std::int64_t ph = transport_->readPlayheadSamplesForUi();
        const float px = xForSessionSample(ph);
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
            const std::int64_t lenClip = timelineClip_->lengthSamples;
            const std::int64_t absPrev = timelineClip_->startSamples
                                         + (std::int64_t)std::llround(ph * (double)lenClip);
            const float px = xForSessionSample(absPrev);
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
