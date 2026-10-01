#include "ui/ChannelStripPanel.h"

#include "domain/Session.h"
#include "domain/SessionSnapshot.h"

namespace
{
    constexpr int kTimerHz = 30;
    constexpr int kPadPx = 4;
    constexpr int kCaptionHeightPx = 14;
    constexpr int kNameHeightPx = 18;
    constexpr int kColumnGapPx = 6;
    constexpr juce::uint32 kPanelBgArgb = 0xff232528;
    constexpr juce::uint32 kPanelTopLineArgb = 0xff3a3d42;
} // namespace

ChannelStripPanel::ChannelStripPanel(Session& session)
    : session_(session)
{
    setOpaque(true);

    nameLabel_.setFont(juce::FontOptions(12.0f, juce::Font::bold));
    nameLabel_.setJustificationType(juce::Justification::centredLeft);
    nameLabel_.setMinimumHorizontalScale(0.7f);
    nameLabel_.setInterceptsMouseClicks(false, false);
    addAndMakeVisible(nameLabel_);

    trackMeterCaption_.setText("Track", juce::dontSendNotification);
    trackMeterCaption_.setFont(juce::FontOptions(9.0f));
    trackMeterCaption_.setJustificationType(juce::Justification::centred);
    trackMeterCaption_.setInterceptsMouseClicks(false, false);
    trackMeterCaption_.setTooltip("Track output level after inserts, fader and pan (sample peak, dBFS). Click the red lamp to reset the overload indicator.");
    addAndMakeVisible(trackMeterCaption_);

    masterCaption_.setText("Stereo Out", juce::dontSendNotification);
    masterCaption_.setFont(juce::FontOptions(9.0f));
    masterCaption_.setJustificationType(juce::Justification::centred);
    masterCaption_.setMinimumHorizontalScale(0.6f);
    masterCaption_.setInterceptsMouseClicks(false, false);
    addAndMakeVisible(masterCaption_);

    fader_.onGainChanged = [this](const float linear) {
        if (faderWiredGuard_ || shownTrackId_ == kInvalidTrackId)
        {
            return;
        }
        // Same path as the former Channel-volume text field: direct Session setter (snapshot
        // publish marks the project dirty; no undo step, same as before).
        session_.setTrackChannelFaderGain(shownTrackId_, linear);
    };
    addAndMakeVisible(fader_);

    trackMeter_.setTooltip("Track output: after inserts, fader and pan, before the output bus. Sample peak in dBFS; the number is the held peak and is not capped at 0. Click to reset the overload lamp.");
    addAndMakeVisible(trackMeter_);
    masterMeter_.setTooltip("Stereo Out: the final master signal before the audio device / export file converts it. Sample peak in dBFS; click to reset the overload lamp.");
    addAndMakeVisible(masterMeter_);

    startTimerHz(kTimerHz);
}

ChannelStripPanel::~ChannelStripPanel()
{
    stopTimer();
    if (hooks_.setMeteredTrack != nullptr)
    {
        hooks_.setMeteredTrack(kInvalidTrackId);
    }
}

void ChannelStripPanel::setHooks(Hooks hooks)
{
    hooks_ = std::move(hooks);
    if (hooks_.setMeteredTrack != nullptr)
    {
        hooks_.setMeteredTrack(trackStripVisible_ && !shownIsMaster_ ? shownTrackId_ : kInvalidTrackId);
    }
}

void ChannelStripPanel::applyModeForTrack(const Track* const track)
{
    const TrackId id = track != nullptr ? track->getId() : kInvalidTrackId;
    const bool isMaster = track != nullptr && track->getKind() == TrackKind::Master;
    const bool hasAudioPath = track != nullptr && track->getKind() != TrackKind::Midi;
    const bool switched = (id != shownTrackId_) || (isMaster != shownIsMaster_) || (hasAudioPath != trackStripVisible_);
    if (switched)
    {
        shownTrackId_ = id;
        shownIsMaster_ = isMaster;
        trackStripVisible_ = hasAudioPath;
        // Never show the previous row's levels: clear the UI meter and re-point the engine tap.
        trackMeter_.clear();
        if (hooks_.setMeteredTrack != nullptr)
        {
            hooks_.setMeteredTrack(hasAudioPath && !isMaster ? id : kInvalidTrackId);
        }
        resized();
    }
    if (track == nullptr)
    {
        nameLabel_.setText("(no track)", juce::dontSendNotification);
    }
    else if (isMaster)
    {
        nameLabel_.setText(kMasterTrackDisplayName, juce::dontSendNotification);
    }
    else
    {
        nameLabel_.setText(track->getName(), juce::dontSendNotification);
    }
    if (track != nullptr && hasAudioPath)
    {
        faderWiredGuard_ = true;
        fader_.setLinearGain(track->getChannelFaderGain(), juce::dontSendNotification);
        faderWiredGuard_ = false;
    }
}

void ChannelStripPanel::refreshFromSession()
{
    const std::shared_ptr<const SessionSnapshot> snap = session_.loadSessionSnapshotForAudioThread();
    const Track* track = nullptr;
    if (snap != nullptr)
    {
        const int idx = snap->findTrackIndexById(session_.getActiveTrackId());
        if (idx >= 0)
        {
            track = &snap->getTrack(idx);
        }
    }
    applyModeForTrack(track);
}

void ChannelStripPanel::timerCallback()
{
    refreshFromSession();
    const double now = juce::Time::getMillisecondCounterHiRes() * 0.001;
    if (hooks_.drainMasterMeter != nullptr)
    {
        masterMeter_.pushReading(hooks_.drainMasterMeter(), now);
    }
    if (trackStripVisible_ && !shownIsMaster_ && hooks_.drainTrackMeter != nullptr)
    {
        trackMeter_.pushReading(hooks_.drainTrackMeter(), now);
    }
    masterMeter_.tick(now);
    trackMeter_.tick(now);
}

void ChannelStripPanel::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colour(kPanelBgArgb));
    g.setColour(juce::Colour(kPanelTopLineArgb));
    g.fillRect(0, 0, getWidth(), 1);
}

void ChannelStripPanel::resized()
{
    auto area = getLocalBounds().reduced(kPadPx, kPadPx);
    area.removeFromTop(1); // top line
    nameLabel_.setBounds(area.removeFromTop(kNameHeightPx));
    area.removeFromTop(2);

    // Column plan (left → right): fader | track meter | Stereo Out meter. The master row uses the
    // fader + ONE big master meter; MIDI / no row uses one centred master meter only.
    const bool showFader = trackStripVisible_;
    const bool showTrackMeter = trackStripVisible_ && !shownIsMaster_;
    fader_.setVisible(showFader);
    trackMeter_.setVisible(showTrackMeter);
    trackMeterCaption_.setVisible(showTrackMeter);
    masterMeter_.setVisible(true);
    masterCaption_.setVisible(true);

    const int w = area.getWidth();
    // Width budget: decide which optional scale labels fit.
    const int faderWideW = ChannelFaderComponent::preferredWidth();                 // 52
    const int faderNarrowW = ChannelFaderComponent::kTravelColumnWidthPx + 10;     // 32 (ticks only)
    const int meterLabelsW = LevelMeterComponent::preferredWidthFor(2, true);      // ~46
    const int meterCompactW = LevelMeterComponent::preferredWidthFor(2, false);    // ~24

    int faderW = 0;
    int trackMeterW = 0;
    int masterMeterW = 0;
    if (showTrackMeter)
    {
        // Three columns.
        faderW = faderWideW;
        trackMeterW = meterLabelsW;
        masterMeterW = meterCompactW;
        if (faderW + trackMeterW + masterMeterW + 2 * kColumnGapPx > w)
        {
            faderW = faderNarrowW;
        }
        if (faderW + trackMeterW + masterMeterW + 2 * kColumnGapPx > w)
        {
            trackMeterW = meterCompactW;
        }
        trackMeter_.setShowScaleLabels(trackMeterW >= meterLabelsW);
        masterMeter_.setShowScaleLabels(false);
        masterMeter_.setShowScale(true);
    }
    else if (showFader)
    {
        // Master selected: fader + one big master meter.
        faderW = (faderWideW + meterLabelsW + kColumnGapPx <= w) ? faderWideW : faderNarrowW;
        masterMeterW = juce::jmax(meterCompactW, juce::jmin(meterLabelsW, w - faderW - kColumnGapPx));
        masterMeter_.setShowScaleLabels(masterMeterW >= meterLabelsW);
        masterMeter_.setShowScale(true);
    }
    else
    {
        masterMeterW = juce::jmin(meterLabelsW, w);
        masterMeter_.setShowScaleLabels(masterMeterW >= meterLabelsW);
        masterMeter_.setShowScale(true);
    }

    auto captionRow = area.removeFromTop(kCaptionHeightPx);
    auto body = area;
    int x = body.getX();
    if (showFader)
    {
        fader_.setBounds(x, body.getY(), faderW, body.getHeight());
        captionRow.removeFromLeft(faderW + kColumnGapPx);
        x += faderW + kColumnGapPx;
    }
    if (showTrackMeter)
    {
        trackMeter_.setBounds(x, body.getY(), trackMeterW, body.getHeight());
        trackMeterCaption_.setBounds(x, captionRow.getY(), trackMeterW, kCaptionHeightPx);
        x += trackMeterW + kColumnGapPx;
    }
    // The Stereo Out meter hugs the right edge so it sits in the same place in every mode.
    const int masterX = showTrackMeter || showFader ? juce::jmax(x, body.getRight() - masterMeterW) : body.getX() + (w - masterMeterW) / 2;
    masterMeter_.setBounds(masterX, body.getY(), masterMeterW, body.getHeight());
    masterCaption_.setBounds(masterX - 6, captionRow.getY(), masterMeterW + 12, kCaptionHeightPx);
}
