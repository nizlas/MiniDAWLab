#include <JuceHeader.h>
#include <juce_audio_devices/juce_audio_devices.h>

#include "domain/Session.h"
#include "engine/PlaybackEngine.h"
#include "transport/Transport.h"
#include "ui/ClipWaveformView.h"

class MiniDAWLabApplication : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override { return "MiniDAWLab"; }

    const juce::String getApplicationVersion() override
    {
        return { ProjectInfo::versionString };
    }

    bool moreThanOneInstanceAllowed() override { return true; }

    void initialise(const juce::String&) override
    {
        transport = std::make_unique<Transport>();
        session = std::make_unique<Session>();
        playbackEngine = std::make_unique<PlaybackEngine>(*transport, *session);

        const juce::String audioInitError =
            deviceManager.initialiseWithDefaultDevices(0, 2);
        jassert(audioInitError.isEmpty());
        juce::ignoreUnused(audioInitError);

        deviceManager.addAudioCallback(playbackEngine.get());

        mainWindow = std::make_unique<MainWindow>(
            getApplicationName(),
            *transport,
            *session,
            deviceManager);
    }

    void shutdown() override
    {
        mainWindow.reset();

        if (playbackEngine != nullptr)
            deviceManager.removeAudioCallback(playbackEngine.get());

        deviceManager.closeAudioDevice();

        playbackEngine.reset();
        session.reset();
        transport.reset();
    }

    void systemRequestedQuit() override { quit(); }

private:
    class TransportControlsContent : public juce::Component
    {
    public:
        TransportControlsContent(Transport& transportIn,
                                 Session& sessionIn,
                                 juce::AudioDeviceManager& deviceManagerIn)
            : transport(transportIn)
            , session(sessionIn)
            , deviceManager(deviceManagerIn)
            , waveformView(sessionIn, transportIn)
        {
            openButton.onClick = [this] { openFileClicked(); };
            playButton.onClick = [this] { transport.requestPlaybackIntent(PlaybackIntent::Playing); };
            pauseButton.onClick = [this] { transport.requestPlaybackIntent(PlaybackIntent::Paused); };
            stopButton.onClick = [this] {
                transport.requestPlaybackIntent(PlaybackIntent::Stopped);
                transport.requestSeek(0);
            };

            addAndMakeVisible(openButton);
            addAndMakeVisible(playButton);
            addAndMakeVisible(pauseButton);
            addAndMakeVisible(stopButton);
            addAndMakeVisible(waveformView);
        }

        void resized() override
        {
            auto area = getLocalBounds().reduced(8);
            auto row = area.removeFromTop(32);
            const int buttonWidth = juce::jmax(48, row.getWidth() / 4);

            openButton.setBounds(row.removeFromLeft(buttonWidth).reduced(2));
            playButton.setBounds(row.removeFromLeft(buttonWidth).reduced(2));
            pauseButton.setBounds(row.removeFromLeft(buttonWidth).reduced(2));
            stopButton.setBounds(row.removeFromLeft(buttonWidth).reduced(2));
            waveformView.setBounds(area);
        }

    private:
        void openFileClicked()
        {
            const auto fileChooserFlags = juce::FileBrowserComponent::openMode
                                          | juce::FileBrowserComponent::canSelectFiles;

            auto chooser = std::make_shared<juce::FileChooser>(
                "Open audio file",
                juce::File{},
                "*.wav;*.aif;*.aiff;*.flac;*.ogg;*.mp3");

            chooser->launchAsync(fileChooserFlags, [this, chooser](const juce::FileChooser& fc) {
                juce::ignoreUnused(chooser);

                const juce::File file = fc.getResult();
                if (!file.existsAsFile())
                    return;

                juce::AudioIODevice* const device = deviceManager.getCurrentAudioDevice();
                if (device == nullptr)
                {
                    juce::AlertWindow::showMessageBoxAsync(
                        juce::AlertWindow::WarningIcon,
                        "Audio",
                        "No active audio device. Cannot validate sample rate for load.");
                    return;
                }

                const double sampleRate = device->getCurrentSampleRate();
                const juce::Result loadResult = session.replaceClipFromFile(file, sampleRate);

                if (!loadResult.wasOk())
                {
                    juce::AlertWindow::showMessageBoxAsync(
                        juce::AlertWindow::WarningIcon,
                        "Could not open file",
                        loadResult.getErrorMessage());
                }
                else
                {
                    waveformView.repaint();
                }
            });
        }

        Transport& transport;
        Session& session;
        juce::AudioDeviceManager& deviceManager;

        juce::TextButton openButton{ "Open..." };
        juce::TextButton playButton{ "Play" };
        juce::TextButton pauseButton{ "Pause" };
        juce::TextButton stopButton{ "Stop" };

        ClipWaveformView waveformView;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TransportControlsContent)
    };

    class MainWindow : public juce::DocumentWindow
    {
    public:
        MainWindow(const juce::String& name,
                   Transport& transport,
                   Session& session,
                   juce::AudioDeviceManager& deviceManager)
            : DocumentWindow(
                  name,
                  juce::Desktop::getInstance().getDefaultLookAndFeel().findColour(
                      juce::ResizableWindow::backgroundColourId),
                  DocumentWindow::allButtons)
        {
            setUsingNativeTitleBar(true);
            setContentOwned(
                new TransportControlsContent(transport, session, deviceManager),
                true);
            setResizable(true, true);
            setResizeLimits(320, 240, 10000, 10000);
            centreWithSize(640, 400);
            setVisible(true);
        }

        void closeButtonPressed() override
        {
            juce::JUCEApplication::getInstance()->systemRequestedQuit();
        }

    private:
        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainWindow)
    };

    std::unique_ptr<Transport> transport;
    std::unique_ptr<Session> session;
    std::unique_ptr<PlaybackEngine> playbackEngine;
    juce::AudioDeviceManager deviceManager;
    std::unique_ptr<MainWindow> mainWindow;
};

START_JUCE_APPLICATION(MiniDAWLabApplication)
