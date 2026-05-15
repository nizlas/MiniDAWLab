#pragma once

#include <JuceHeader.h>

#include <functional>

class Transport;

/// Message-thread play/pause/stop transport UI behavior for the main transport strip.
/// Does not own `Transport`, the play button, or recording — only coordinates calls.
class TransportPlayPauseStopController final
{
public:
    struct Callbacks
    {
        std::function<bool()> isRecording;
        std::function<bool()> isCountInActive;
        std::function<void(const char* sourceContext)> stopRecordingAndCommitFromUi;
        std::function<void()> cancelCountIn;
    };

    TransportPlayPauseStopController(Transport& transport,
                                     juce::TextButton& playPauseButton,
                                     Callbacks callbacks);

    void invokePlayPauseToggleFromWindowShortcut();

    void togglePlayPauseTransportOnly();
    void togglePlayPauseFromUi();
    void stopOrSeekFromStopButton();
    void updatePlayPauseButtonFromTransport();

private:
    Transport& transport_;
    juce::TextButton& playPauseButton_;
    Callbacks callbacks_;
};
