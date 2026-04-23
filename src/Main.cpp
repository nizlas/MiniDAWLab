// =============================================================================
// Main.cpp  —  application entry, object wiring, and the main window (all on the message thread)
// =============================================================================
//
// ROLE
//   This is the "composition root" for MiniDAWLab: it creates Transport, Session, and
//   PlaybackEngine, connects them to juce::AudioDeviceManager, and shows the one window. It does
//   not implement playback math or file decoding — that lives in the engine / session / io layers.
//
// STARTUP ORDER (see initialise) — read before changing tear order
//   1. transport, session, playbackEngine  (non-owning refs: engine points at transport+session)
//   2. deviceManager.initialiseWithDefaultDevices  —  opens default audio I/O
//   3. addAudioCallback(playbackEngine)  —  the engine will now receive audioDeviceIO* calls
//   4. main window with TransportControlsContent  —  UI can load files and send Transport commands
//
// SHUTDOWN ORDER (see shutdown) — JUCE: remove callback before closing device, then release objects
//   1. destroy main window
//   2. removeAudioCallback(playbackEngine)
//   3. closeAudioDevice
//   4. destroy playbackEngine, session, transport
//
// THREADING
//   juce::JUCEApplication::initialise / shutdown and all UI (buttons, file chooser, paint) are
//   the [Message thread] in a desktop JUCE app. The audio path is only in PlaybackEngine’s
//   callback, which we do not call from here.
//
// NESTED TYPES
//   TransportControlsContent  —  buttons + waveform; opens files via FileChooser, calls Session.
//   MainWindow  —  juce::DocumentWindow shell around the content.
//
// Method bodies in this file add plain-language notes next to start/stop order and the async
// file dialog path so the composition root is navigable, not just listed.
// =============================================================================

#include <JuceHeader.h>
#include <juce_audio_devices/juce_audio_devices.h>

#include "domain/Session.h"
#include "engine/PlaybackEngine.h"
#include "transport/Transport.h"
#include "ui/ClipWaveformView.h"

// ---------------------------------------------------------------------------
// MiniDAWLabApplication — process-wide singleton, owns top-level subsystems
// ---------------------------------------------------------------------------
class MiniDAWLabApplication : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override { return "MiniDAWLab"; }

    const juce::String getApplicationVersion() override
    {
        return { ProjectInfo::versionString };
    }

    bool moreThanOneInstanceAllowed() override { return true; }

    // [Message thread] Wires the stack described in the file header. jassert on empty init error
    // in debug: device must open for Phase 1 playback to make sense.
    void initialise(const juce::String&) override
    {
        // Domain objects first: the engine only holds references; safe because we create them
        // in dependency order and tear down in reverse in shutdown.
        transport = std::make_unique<Transport>();
        session = std::make_unique<Session>();
        playbackEngine = std::make_unique<PlaybackEngine>(*transport, *session);

        // JUCE: open the default audio device before we register the engine — otherwise the
        // callback would have nowhere to go and sample rate for file load would be unknown.
        const juce::String audioInitError =
            deviceManager.initialiseWithDefaultDevices(0, 2);
        jassert(audioInitError.isEmpty());
        juce::ignoreUnused(audioInitError);

        // After this line, the audio thread can call our PlaybackEngine; keep UI after so we do
        // not paint or load files before the device exists.
        deviceManager.addAudioCallback(playbackEngine.get());

        mainWindow = std::make_unique<MainWindow>(
            getApplicationName(),
            *transport,
            *session,
            deviceManager);
    }

    // [Message thread] Reverse of initialise; see file header.
    void shutdown() override
    {
        // Window first so no UI code runs while we tear down audio (matches JUCE’s typical order).
        mainWindow.reset();

        if (playbackEngine != nullptr)
        {
            // Unregister *before* closing the device so the engine is never called after destroy.
            deviceManager.removeAudioCallback(playbackEngine.get());
        }

        deviceManager.closeAudioDevice();

        // Safe to drop engine then session/transport: nothing references them from audio anymore.
        playbackEngine.reset();
        session.reset();
        transport.reset();
    }

    void systemRequestedQuit() override { quit(); }

private:
    // [Message thread only] Child component: file chooser, transport buttons, ClipWaveformView.
    // Holds non-owning refs; MainWindow and application own lifetime. load path: FileChooser
    // (async) → session.replaceClipFromFile with sample rate from getCurrentAudioDevice.
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
            // Stop: user expectation is "playback off *and* playhead back to the start" for
            // this small UI — we queue both the intent and a seek to sample 0 (applied next block).
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

        // [Message thread] Layout: one row of buttons, remainder for waveform.
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
        // [Message thread] Presents a native file dialog; when the user picks a file, decodes
        // into session if sample rate matches the active device. juce::FileChooser::launchAsync
        // returns on the message thread in the callback lambda.
        void openFileClicked()
        {
            const auto fileChooserFlags = juce::FileBrowserComponent::openMode
                                          | juce::FileBrowserComponent::canSelectFiles;

            auto chooser = std::make_shared<juce::FileChooser>(
                "Open audio file",
                juce::File{},
                "*.wav;*.aif;*.aiff;*.flac;*.ogg;*.mp3");

            // JUCE: the dialog is async; this lambda runs on the *message* thread when the user
            // dismisses the picker. No decoding happens until we explicitly call replaceClipFromFile.
            chooser->launchAsync(fileChooserFlags, [this, chooser](const juce::FileChooser& fc) {
                juce::ignoreUnused(chooser);

                const juce::File file = fc.getResult();
                if (!file.existsAsFile())
                {
                    // Cancel or empty selection — not an error, keep the current session.
                    return;
                }

                juce::AudioIODevice* const device = deviceManager.getCurrentAudioDevice();
                if (device == nullptr)
                {
                    juce::AlertWindow::showMessageBoxAsync(
                        juce::AlertWindow::WarningIcon,
                        "Audio",
                        "No active audio device. Cannot validate sample rate for load.");
                    return;
                }

                // Phase 1: loader must match the *running* device rate; we use it as the contract
                // for AudioFileLoader::loadFromFile, not a hard-coded constant.
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
                    // New clip is in session — redraw the waveform; playhead/transport unchanged
                    // until the user hits play or seek.
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

    // [Message thread] Top-level JUCE window: title bar, resize, content is TransportControlsContent.
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

        // [Message thread] User clicked the window close: end the application.
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

// JUCE: generate WinMain / main and the app singleton; DO NOT add another main().
START_JUCE_APPLICATION(MiniDAWLabApplication)
