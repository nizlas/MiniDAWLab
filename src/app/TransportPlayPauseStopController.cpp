#include "app/TransportPlayPauseStopController.h"

#include "transport/Transport.h"

TransportPlayPauseStopController::TransportPlayPauseStopController(Transport& transport,
                                                                   juce::TextButton& playPauseButton,
                                                                   Callbacks callbacks)
    : transport_(transport)
    , playPauseButton_(playPauseButton)
    , callbacks_(std::move(callbacks))
{
}

void TransportPlayPauseStopController::invokePlayPauseToggleFromWindowShortcut()
{
    if (callbacks_.isRecording())
    {
        callbacks_.stopRecordingAndCommitFromUi("space");
        return;
    }
    togglePlayPauseTransportOnly();
}

void TransportPlayPauseStopController::togglePlayPauseTransportOnly()
{
    if (transport_.readPlaybackIntentForUi() == PlaybackIntent::Playing)
    {
        transport_.requestPlaybackIntent(PlaybackIntent::Paused);
    }
    else
    {
        transport_.requestPlaybackIntent(PlaybackIntent::Playing);
    }
    updatePlayPauseButtonFromTransport();
}

void TransportPlayPauseStopController::togglePlayPauseFromUi()
{
    if (callbacks_.isCountInActive())
    {
        callbacks_.cancelCountIn();
        return;
    }
    if (callbacks_.isRecording())
    {
        callbacks_.stopRecordingAndCommitFromUi("play_pause");
        return;
    }
    togglePlayPauseTransportOnly();
}

void TransportPlayPauseStopController::stopOrSeekFromStopButton()
{
    if (callbacks_.isCountInActive())
    {
        callbacks_.cancelCountIn();
        return;
    }
    if (callbacks_.isRecording())
    {
        callbacks_.stopRecordingAndCommitFromUi("stop");
    }
    else
    {
        transport_.requestPlaybackIntent(PlaybackIntent::Stopped);
    }
    transport_.requestSeek(0);
    updatePlayPauseButtonFromTransport();
}

void TransportPlayPauseStopController::updatePlayPauseButtonFromTransport()
{
    const bool playing = transport_.readPlaybackIntentForUi() == PlaybackIntent::Playing;
    const juce::String t = playing ? "Pause" : "Play";
    if (t != playPauseButton_.getButtonText())
    {
        playPauseButton_.setButtonText(t);
    }
}
