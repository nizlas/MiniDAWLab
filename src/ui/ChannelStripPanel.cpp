#include "ui/ChannelStripPanel.h"

#include "domain/Session.h"
#include "domain/SessionSnapshot.h"

namespace
{
    constexpr int kTimerHz = 30;
    constexpr int kPadPx = 4;
    constexpr int kCaptionHeightPx = 14;
    constexpr int kNameHeightPx = 18;
    constexpr int kColumnGapPx = 8;
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

    meterCaption_.setText("Out", juce::dontSendNotification);
    meterCaption_.setFont(juce::FontOptions(9.0f));
    meterCaption_.setJustificationType(juce::Justification::centred);
    meterCaption_.setInterceptsMouseClicks(false, false);
    addAndMakeVisible(meterCaption_);

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

    outputMeter_.setTooltip("Audio output of the selected track after inserts, fader and pan (Stereo Out: the final master signal before the audio device). Sample peak in dBFS; the number is the held peak and is not capped at 0. Click to reset the overload lamp.");
    addAndMakeVisible(outputMeter_);

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
    const bool visibilityFlips = (hasAudioPath != trackStripVisible_);
    const bool switched = (id != shownTrackId_) || (isMaster != shownIsMaster_) || visibilityFlips;
    if (switched)
    {
        shownTrackId_ = id;
        shownIsMaster_ = isMaster;
        trackStripVisible_ = hasAudioPath;
        // Never show the previous row's levels: clear the UI meter and re-point the engine tap.
        // The master row reads the Stereo Out accumulator instead (no track tap needed).
        outputMeter_.clear();
        if (hooks_.setMeteredTrack != nullptr)
        {
            hooks_.setMeteredTrack(hasAudioPath && !isMaster ? id : kInvalidTrackId);
        }
        meterCaption_.setText(isMaster ? "Stereo Out" : "Out", juce::dontSendNotification);
        resized();
        if (visibilityFlips && onAudioStripVisibilityChanged != nullptr)
        {
            onAudioStripVisibilityChanged();
        }
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
    if (trackStripVisible_)
    {
        if (shownIsMaster_)
        {
            if (hooks_.drainMasterMeter != nullptr)
            {
                outputMeter_.pushReading(hooks_.drainMasterMeter(), now);
            }
        }
        else if (hooks_.drainTrackMeter != nullptr)
        {
            outputMeter_.pushReading(hooks_.drainTrackMeter(), now);
        }
    }
    else if (hooks_.drainMasterMeter != nullptr)
    {
        // Not displayed, but keep the UI window drained so the engine accumulator never carries a
        // stale maximum into the moment the master row is selected again.
        (void)hooks_.drainMasterMeter();
    }
    outputMeter_.tick(now);
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

    const bool show = trackStripVisible_;
    fader_.setVisible(show);
    outputMeter_.setVisible(show);
    meterCaption_.setVisible(show);
    if (!show)
    {
        return;
    }

    // Two columns: fader | output meter. Scale labels are dropped first when the strip is narrow.
    const int w = area.getWidth();
    const int faderWideW = ChannelFaderComponent::preferredWidth();              // 52
    const int faderNarrowW = ChannelFaderComponent::kTravelColumnWidthPx + 10;  // 32 (ticks only)
    const int meterLabelsW = LevelMeterComponent::preferredWidthFor(2, true);   // ~46
    const int meterCompactW = LevelMeterComponent::preferredWidthFor(2, false); // ~24
    int faderW = faderWideW;
    int meterW = meterLabelsW;
    if (faderW + meterW + kColumnGapPx > w)
    {
        faderW = faderNarrowW;
    }
    if (faderW + meterW + kColumnGapPx > w)
    {
        meterW = meterCompactW;
    }
    outputMeter_.setShowScale(true);
    outputMeter_.setShowScaleLabels(meterW >= meterLabelsW);

    auto captionRow = area.removeFromTop(kCaptionHeightPx);
    auto body = area;
    // Centre the pair horizontally so a wide Inspector does not leave the strip hugging the left edge.
    const int pairW = faderW + kColumnGapPx + meterW;
    const int x0 = body.getX() + juce::jmax(0, (w - pairW) / 2);
    fader_.setBounds(x0, body.getY(), faderW, body.getHeight());
    outputMeter_.setBounds(x0 + faderW + kColumnGapPx, body.getY(), meterW, body.getHeight());
    meterCaption_.setBounds(x0 + faderW + kColumnGapPx - 8, captionRow.getY(), meterW + 16, kCaptionHeightPx);
}
