#include "ui/TrackAutomationLaneView.h"

#include "ui/ChannelFaderScale.h"

#include <algorithm>
#include <cmath>

namespace
{
constexpr float kPointHitPx = 7.0f;
constexpr float kCurveHitPx = 6.0f;

[[nodiscard]] juce::String panText(const float pan)
{
    const int pct = juce::roundToInt(std::fabs(pan) * 100.0f);
    if (pct == 0)
    {
        return "C";
    }
    return (pan < 0.0f ? "L" : "R") + juce::String(pct);
}
} // namespace

TrackAutomationLaneView::TrackAutomationLaneView(const int parameterId, const TrackId trackId)
    : parameterId_(parameterId)
    , trackId_(trackId)
{
    juce::ignoreUnused(trackId_);
    setOpaque(true);
    setMouseClickGrabsKeyboardFocus(false);
}

void TrackAutomationLaneView::setCurve(const TrackAutomationCurve& curve,
                                       const float manual,
                                       const juce::Colour colour)
{
    if (drag_ != Drag::None)
    {
        colour_ = colour;
        return;
    }
    curve_ = curve;
    manual_ = manual;
    colour_ = colour;
    repaint();
}

void TrackAutomationLaneView::setTimeMap(const double samplesPerPixel,
                                         const std::int64_t visibleStartSamples,
                                         const double engineRate) noexcept
{
    samplesPerPixel_ = samplesPerPixel > 0.0 ? samplesPerPixel : 1.0;
    visibleStartSamples_ = visibleStartSamples;
    engineRate_ = engineRate > 0.0 ? engineRate : 48000.0;
    repaint();
}

void TrackAutomationLaneView::setPlayheadSeconds(const double seconds) noexcept
{
    playheadSeconds_ = seconds;
    playheadValid_ = std::isfinite(seconds);
    repaint();
}

juce::Rectangle<int> TrackAutomationLaneView::getPlotBounds() const noexcept
{
    return getLocalBounds().withTrimmedLeft(kLabelWidthPx).reduced(4, 6);
}

juce::Rectangle<int> TrackAutomationLaneView::getParameterMenuBounds() const noexcept
{
    return { 4, 4, kLabelWidthPx - 28, 16 };
}

float TrackAutomationLaneView::valueToY(const float value) const noexcept
{
    const auto plot = getPlotBounds().toFloat();
    double position = 0.0;
    if (parameterId_ == kTrackAutomationPanId)
    {
        position = (static_cast<double>(sanitizeTrackStereoPan(value)) + 1.0) * 0.5;
    }
    else
    {
        position = channel_fader_scale::Scale::positionForLinearGain(value);
    }
    return plot.getBottom() - static_cast<float>(position) * plot.getHeight();
}

float TrackAutomationLaneView::yToValue(const float y) const noexcept
{
    const auto plot = getPlotBounds().toFloat();
    const float h = juce::jmax(1.0f, plot.getHeight());
    const double position = juce::jlimit(0.0, 1.0, static_cast<double>((plot.getBottom() - y) / h));
    if (parameterId_ == kTrackAutomationPanId)
    {
        return sanitizeTrackStereoPan(position * 2.0 - 1.0);
    }
    return sanitizeTrackAutomationValue(kTrackAutomationVolumeId,
                                        channel_fader_scale::Scale::linearGainForPosition(position));
}

float TrackAutomationLaneView::secondsToX(const double seconds) const noexcept
{
    const double sample = static_cast<double>(trackAutomationSecondsToEngineSample(seconds, engineRate_));
    const auto plot = getPlotBounds().toFloat();
    return plot.getX()
           + static_cast<float>((sample - static_cast<double>(visibleStartSamples_)) / samplesPerPixel_);
}

double TrackAutomationLaneView::xToSeconds(const float x) const noexcept
{
    const auto plot = getPlotBounds().toFloat();
    const double sample = static_cast<double>(visibleStartSamples_)
                          + static_cast<double>(x - plot.getX()) * samplesPerPixel_;
    return trackAutomationEngineSampleToSeconds(std::llround(sample), engineRate_);
}

int TrackAutomationLaneView::pointAt(const juce::Point<float> p) const noexcept
{
    int found = -1;
    float best = kPointHitPx;
    for (int i = 0; i < static_cast<int>(curve_.points.size()); ++i)
    {
        const auto& pt = curve_.points[static_cast<size_t>(i)];
        const juce::Point<float> at(secondsToX(pt.timeSeconds), valueToY(pt.value));
        const float d = at.getDistanceFrom(p);
        if (d <= best)
        {
            best = d;
            found = i;
        }
    }
    return found;
}

int TrackAutomationLaneView::segmentAt(const double seconds) const noexcept
{
    if (curve_.points.size() < 2)
    {
        return -1;
    }
    const int i = trackAutomationPointIndexAtOrBefore(curve_, seconds);
    if (i < 0 || i + 1 >= static_cast<int>(curve_.points.size()))
    {
        return -1;
    }
    return i;
}

void TrackAutomationLaneView::showHandle(const int segment, const double u) noexcept
{
    if (segment < 0 || segment + 1 >= static_cast<int>(curve_.points.size()))
    {
        handleVisible_ = false;
        return;
    }
    const float y0 = curve_.points[static_cast<size_t>(segment)].value;
    const float y1 = curve_.points[static_cast<size_t>(segment + 1)].value;
    const float control = segment < static_cast<int>(curve_.control.size())
                              ? curve_.control[static_cast<size_t>(segment)]
                              : trackAutomationLinearControl(y0, y1);
    const double cu = trackAutomationClampGrabU(u);
    const float y = trackAutomationQuadratic(y0, control, y1, cu);
    const double t0 = curve_.points[static_cast<size_t>(segment)].timeSeconds;
    const double t1 = curve_.points[static_cast<size_t>(segment + 1)].timeSeconds;
    handleVisible_ = true;
    handleSegment_ = segment;
    handleX_ = secondsToX(t0 + cu * (t1 - t0));
    handleY_ = valueToY(y);
}

void TrackAutomationLaneView::commit() noexcept
{
    if (onCurveCommitted != nullptr)
    {
        onCurveCommitted(curve_);
    }
}

void TrackAutomationLaneView::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colour(0xff1c1e21));
    g.setColour(juce::Colour(0xff3a3d42));
    g.drawRect(getLocalBounds());

    const juce::String name = parameterId_ == kTrackAutomationPanId ? "Pan" : "Volume";
    const float shown = curve_.points.empty()
                            ? manual_
                            : trackAutomationEvaluate(curve_, playheadValid_ ? playheadSeconds_ : 0.0, manual_);
    const juce::String value = parameterId_ == kTrackAutomationPanId
                                   ? panText(shown)
                                   : channel_fader_scale::Scale::gainText(shown);
    g.setColour(juce::Colours::white.withAlpha(0.88f));
    g.setFont(juce::FontOptions(11.0f));
    g.drawText(name, getParameterMenuBounds(), juce::Justification::centredLeft);
    g.setFont(juce::FontOptions(10.0f));
    g.setColour(juce::Colours::white.withAlpha(0.7f));
    g.drawText(value, 4, 20, kLabelWidthPx - 8, 14, juce::Justification::centredLeft);
    g.setColour(juce::Colours::white.withAlpha(0.55f));
    g.drawText("+", kLabelWidthPx - 22, 4, 16, 16, juce::Justification::centred);

    const auto plot = getPlotBounds().toFloat();
    if (plot.isEmpty())
    {
        return;
    }
    g.setColour(juce::Colour(0xff2a2d31));
    g.fillRect(plot);

    if (curve_.points.empty())
    {
        g.setColour(colour_.withAlpha(0.85f));
        g.drawHorizontalLine(juce::roundToInt(valueToY(manual_)), plot.getX(), plot.getRight());
    }
    else
    {
        juce::Path path;
        bool started = false;
        const int x0 = static_cast<int>(std::floor(plot.getX()));
        const int x1 = static_cast<int>(std::ceil(plot.getRight()));
        for (int x = x0; x <= x1; ++x)
        {
            const double seconds = xToSeconds(static_cast<float>(x));
            const float y = valueToY(trackAutomationEvaluate(curve_, seconds, manual_));
            if (!started)
            {
                path.startNewSubPath(static_cast<float>(x), y);
                started = true;
            }
            else
            {
                path.lineTo(static_cast<float>(x), y);
            }
        }
        g.setColour(colour_);
        g.strokePath(path, juce::PathStrokeType(1.6f));
        g.setColour(colour_.brighter(0.15f));
        for (const TrackAutomationPoint& pt : curve_.points)
        {
            const float x = secondsToX(pt.timeSeconds);
            const float y = valueToY(pt.value);
            g.fillRect(x - 3.0f, y - 3.0f, 6.0f, 6.0f);
        }
    }

    if (handleVisible_)
    {
        g.setColour(juce::Colours::white);
        g.drawEllipse(handleX_ - 4.0f, handleY_ - 4.0f, 8.0f, 8.0f, 1.4f);
    }

    if (playheadValid_)
    {
        const float x = secondsToX(playheadSeconds_);
        if (x >= plot.getX() && x <= plot.getRight())
        {
            g.setColour(juce::Colours::white.withAlpha(0.35f));
            g.drawVerticalLine(juce::roundToInt(x), plot.getY(), plot.getBottom());
        }
    }
}

void TrackAutomationLaneView::mouseMove(const juce::MouseEvent& e)
{
    if (drag_ != Drag::None)
    {
        return;
    }
    const auto p = e.position;
    if (pointAt(p) >= 0 || !getPlotBounds().contains(p.toInt()))
    {
        handleVisible_ = false;
        repaint();
        return;
    }
    const double seconds = xToSeconds(p.x);
    const int seg = segmentAt(seconds);
    if (seg < 0)
    {
        handleVisible_ = false;
        repaint();
        return;
    }
    const double t0 = curve_.points[static_cast<size_t>(seg)].timeSeconds;
    const double t1 = curve_.points[static_cast<size_t>(seg + 1)].timeSeconds;
    const double u = (t1 > t0) ? (seconds - t0) / (t1 - t0) : 0.5;
    showHandle(seg, u);
    const float curveY = handleY_;
    if (std::fabs(curveY - p.y) > kCurveHitPx)
    {
        handleVisible_ = false;
    }
    repaint();
}

void TrackAutomationLaneView::mouseExit(const juce::MouseEvent&)
{
    if (drag_ == Drag::None)
    {
        handleVisible_ = false;
        repaint();
    }
}

void TrackAutomationLaneView::mouseDown(const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu())
    {
        const int hit = pointAt(e.position);
        if (hit >= 0 && curve_.points.size() > 1)
        {
            curve_.points.erase(curve_.points.begin() + hit);
            if (!curve_.control.empty())
            {
                const int controlIndex = juce::jlimit(0, static_cast<int>(curve_.control.size()) - 1, hit - 1);
                if (hit > 0 && hit - 1 < static_cast<int>(curve_.control.size()))
                {
                    curve_.control.erase(curve_.control.begin() + (hit - 1));
                }
                else if (!curve_.control.empty())
                {
                    curve_.control.erase(curve_.control.begin() + controlIndex);
                }
            }
            trackAutomationNormalize(curve_, parameterId_);
            commit();
            repaint();
        }
        return;
    }

    if (getParameterMenuBounds().contains(e.getPosition()))
    {
        juce::PopupMenu menu;
        menu.addItem(1, "Volume", true, parameterId_ == kTrackAutomationVolumeId);
        menu.addItem(2, "Pan", true, parameterId_ == kTrackAutomationPanId);
        juce::Component::SafePointer<TrackAutomationLaneView> safe(this);
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this),
                           [safe](const int result) {
                               if (safe == nullptr || result == 0 || safe->onParameterChosen == nullptr)
                               {
                                   return;
                               }
                               const int chosen = result == 2 ? kTrackAutomationPanId : kTrackAutomationVolumeId;
                               if (chosen != safe->parameterId_)
                               {
                                   safe->onParameterChosen(chosen);
                               }
                           });
        return;
    }

    if (juce::Rectangle<int>(kLabelWidthPx - 22, 4, 16, 16).contains(e.getPosition()))
    {
        if (onToggleOtherLane != nullptr)
        {
            onToggleOtherLane();
        }
        return;
    }

    const int hit = pointAt(e.position);
    if (hit >= 0)
    {
        drag_ = Drag::Point;
        dragIndex_ = hit;
        handleVisible_ = false;
        return;
    }

    if (!getPlotBounds().contains(e.getPosition()) || curve_.points.size() < 2)
    {
        return;
    }
    const double seconds = xToSeconds(e.position.x);
    const int seg = segmentAt(seconds);
    if (seg < 0)
    {
        return;
    }
    const double t0 = curve_.points[static_cast<size_t>(seg)].timeSeconds;
    const double t1 = curve_.points[static_cast<size_t>(seg + 1)].timeSeconds;
    const double u = (t1 > t0) ? (seconds - t0) / (t1 - t0) : 0.5;
    const float y0 = curve_.points[static_cast<size_t>(seg)].value;
    const float y1 = curve_.points[static_cast<size_t>(seg + 1)].value;
    const float control = seg < static_cast<int>(curve_.control.size())
                              ? curve_.control[static_cast<size_t>(seg)]
                              : trackAutomationLinearControl(y0, y1);
    const float onCurve = trackAutomationQuadratic(y0, control, y1, trackAutomationClampGrabU(u));
    if (std::fabs(valueToY(onCurve) - e.position.y) > kCurveHitPx)
    {
        return;
    }
    drag_ = Drag::Curve;
    dragIndex_ = seg;
    grabU_ = trackAutomationClampGrabU(u);
    showHandle(seg, grabU_);
}

void TrackAutomationLaneView::mouseDrag(const juce::MouseEvent& e)
{
    if (drag_ == Drag::Point && dragIndex_ >= 0 && dragIndex_ < static_cast<int>(curve_.points.size()))
    {
        double t = xToSeconds(e.position.x);
        const double prev = dragIndex_ > 0
                                ? curve_.points[static_cast<size_t>(dragIndex_ - 1)].timeSeconds + 1.0e-4
                                : 0.0;
        const double next = dragIndex_ + 1 < static_cast<int>(curve_.points.size())
                                ? curve_.points[static_cast<size_t>(dragIndex_ + 1)].timeSeconds - 1.0e-4
                                : 1.0e9;
        t = juce::jlimit(prev, juce::jmax(prev, next), t);
        curve_.points[static_cast<size_t>(dragIndex_)].timeSeconds = t;
        curve_.points[static_cast<size_t>(dragIndex_)].value = yToValue(e.position.y);
        for (int s = juce::jmax(0, dragIndex_ - 1);
             s <= dragIndex_ && s < static_cast<int>(curve_.control.size());
             ++s)
        {
            if (s + 1 >= static_cast<int>(curve_.points.size()))
            {
                break;
            }
            const float y0 = curve_.points[static_cast<size_t>(s)].value;
            const float y1 = curve_.points[static_cast<size_t>(s + 1)].value;
            const float lo = std::min(y0, y1);
            const float hi = std::max(y0, y1);
            curve_.control[static_cast<size_t>(s)] = std::clamp(curve_.control[static_cast<size_t>(s)], lo, hi);
        }
        repaint();
        return;
    }

    if (drag_ == Drag::Curve && dragIndex_ >= 0 && dragIndex_ + 1 < static_cast<int>(curve_.points.size()))
    {
        const double t0 = curve_.points[static_cast<size_t>(dragIndex_)].timeSeconds;
        const double t1 = curve_.points[static_cast<size_t>(dragIndex_ + 1)].timeSeconds;
        const double span = t1 - t0;
        if (span > kTrackAutomationTimeMergeSeconds)
        {
            grabU_ = trackAutomationClampGrabU((xToSeconds(e.position.x) - t0) / span);
        }
        const float y0 = curve_.points[static_cast<size_t>(dragIndex_)].value;
        const float y1 = curve_.points[static_cast<size_t>(dragIndex_ + 1)].value;
        if (static_cast<int>(curve_.control.size()) <= dragIndex_)
        {
            curve_.control.resize(static_cast<size_t>(dragIndex_ + 1), trackAutomationLinearControl(y0, y1));
        }
        curve_.control[static_cast<size_t>(dragIndex_)]
            = trackAutomationControlFromGrab(y0, y1, grabU_, yToValue(e.position.y));
        showHandle(dragIndex_, grabU_);
        repaint();
    }
}

void TrackAutomationLaneView::mouseUp(const juce::MouseEvent&)
{
    if (drag_ != Drag::None)
    {
        drag_ = Drag::None;
        dragIndex_ = -1;
        commit();
        repaint();
    }
}

void TrackAutomationLaneView::mouseDoubleClick(const juce::MouseEvent& e)
{
    if (!getPlotBounds().contains(e.getPosition()) || drag_ != Drag::None)
    {
        return;
    }
    const double seconds = xToSeconds(e.position.x);
    if (curve_.points.size() >= 2 && segmentAt(seconds) >= 0)
    {
        trackAutomationSplitAt(curve_, seconds);
    }
    else
    {
        TrackAutomationPoint pt;
        pt.timeSeconds = juce::jmax(0.0, seconds);
        pt.value = yToValue(e.position.y);
        curve_.points.push_back(pt);
        trackAutomationNormalize(curve_, parameterId_);
    }
    commit();
    repaint();
}
