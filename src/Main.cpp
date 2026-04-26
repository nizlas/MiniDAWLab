// =============================================================================
// Main.cpp  —  application entry, object wiring, and the main window (all on the message thread)
// =============================================================================
//
// ROLE
//   This is the "composition root" for MiniDAWLab: it creates Transport, Session,
//   `RecorderService` (optional Phase 4 capture; not wired to UI yet), and `PlaybackEngine`,
//   connects them to juce::AudioDeviceManager, and shows the one window. It does
//   not implement playback math or file decoding — that lives in the engine / session / io layers.
//
// STARTUP ORDER (see initialise) — read before changing tear order
//   1. transport, session, recorderService, playbackEngine  (non-owning: engine points at
//      transport+session+recorder; recorder does not own Transport/Session)
//   2. deviceManager.initialiseWithDefaultDevices  —  **1 in / 2 out** when possible; falls back
//      to output-only (0, 2) with a log if the system cannot open an input (playback still works).
//   3. addAudioCallback(playbackEngine)  —  the engine will now receive audioDeviceIO* calls
//   4. main window with TransportControlsContent  —  UI can load files and send Transport commands
//
// SHUTDOWN ORDER (see shutdown) — JUCE: remove callback before closing device, then release objects
//   1. destroy main window
//   2. removeAudioCallback(playbackEngine)
//   3. closeAudioDevice
//   4. destroy playbackEngine, then recorderService, then session, transport
//
// THREADING
//   juce::JUCEApplication::initialise / shutdown and all UI (buttons, file chooser, paint) are
//   the [Message thread] in a desktop JUCE app. The audio path is only in PlaybackEngine’s
//   callback, which we do not call from here.
//
// NESTED TYPES
//   TransportControlsContent  —  buttons + timeline ruler + lane; FileChooser → read playhead, Session add-clip.
//   MainWindow  —  juce::DocumentWindow shell around the content.
//
// Method bodies in this file add plain-language notes next to start/stop order and the async
// file dialog path so the composition root is navigable, not just listed.
// `TransportControlsContent` also hosts the recording coordinator (`numpadRecordToggled`); the
// **shortcut** to invoke it is handled only in `MainWindow` as a single `KeyListener` on the
// window (JUCE key dispatch walks from focus → parent; content-only listeners are not always hit).
// =============================================================================

#include <JuceHeader.h>
#include <juce_audio_devices/juce_audio_devices.h>

#include "domain/Session.h"
#include "engine/CountInClickOutput.h"
#include "engine/PlaybackEngine.h"
#include "engine/RecorderService.h"
#include "transport/Transport.h"
#include "ui/TimelineRulerView.h"
#include "ui/TimelineViewportModel.h"
#include "ui/TrackLanesView.h"

#include <optional>

namespace
{
    // Temporary: show last key in a small local label (transport row). Leave `false` in normal use.
    constexpr bool kShowKeyDiagnostic = false;

    // JUCE (Windows) uses e.g. VK_* | 0x10000 for numpad keys; `KeyPress::numberPadMultiply` matches
    // that, but some paths deliver VK_MULTIPLY (0x6A) without the high bit — match both.
    [[nodiscard]] bool isNumpadMultiplyKey(const juce::KeyPress& k) noexcept
    {
        if (k == juce::KeyPress::numberPadMultiply)
        {
            return true;
        }
        constexpr int kVkMultiply = 0x6A; // winuser.h VK_MULTIPLY
        if ((k.getKeyCode() & 0xffff) == kVkMultiply)
        {
            return true;
        }
        return false;
    }

    // Numpad * or (for laptops) top-row * character (e.g. Shift+8) — not used for all keys with `*`
    // in text outside this narrow set (handled only at MainWindow).
    [[nodiscard]] bool isRecordToggleShortcut(const juce::KeyPress& k) noexcept
    {
        if (isNumpadMultiplyKey(k))
        {
            return true;
        }
        if (k.getTextCharacter() == juce_wchar{ '*' })
        {
            return true;
        }
        return false;
    }

    // Unmodified Space → play/pause toggle (not recording). Ctrl/Cmd/Alt+Space are ignored here.
    [[nodiscard]] bool isSpacePlayPauseShortcut(const juce::KeyPress& k) noexcept
    {
        const juce::ModifierKeys m = k.getModifiers();
        if (m.isCommandDown() || m.isCtrlDown() || m.isAltDown())
        {
            return false;
        }
        if (k.getKeyCode() == 32 || k.getTextCharacter() == juce_wchar{ ' ' })
        {
            return true;
        }
        return false;
    }

    [[nodiscard]] juce::File makeUniqueTakeWavInProjectAudioDir(const juce::File& audioDir)
    {
        const juce::String t = juce::Time::getCurrentTime().formatted("%Y%m%d_%H%M%S");
        juce::File f = audioDir.getChildFile("take_" + t + ".wav");
        if (!f.existsAsFile())
        {
            return f;
        }
        for (int i = 1; i < 10000; ++i)
        {
            f = audioDir.getChildFile("take_" + t + "_" + juce::String(i) + ".wav");
            if (!f.existsAsFile())
            {
                return f;
            }
        }
        return audioDir.getChildFile("take_" + t + "_9999.wav");
    }

    // First-time Save As: abort with a non-empty message if we cannot write without clobbering.
    // `projectFile` = `<projectFolder>/<projectName>.dalproj`.
    [[nodiscard]] juce::String firstTimeSaveConflictMessage(
        const juce::File& projectFolder,
        const juce::File& projectFile)
    {
        if (projectFile.existsAsFile())
        {
            return "A project file already exists at:\n" + projectFile.getFullPathName()
                   + "\n\nChoose a different name or delete the existing file first.";
        }
        if (projectFolder.exists() && !projectFolder.isDirectory())
        {
            return "Cannot create the project folder; a file already exists at:\n"
                   + projectFolder.getFullPathName();
        }
        if (projectFolder.isDirectory())
        {
            juce::Array<juce::File> files;
            projectFolder.findChildFiles(files, juce::File::findFiles, false);
            for (const auto& c : files)
            {
                const juce::String n = c.getFileName();
                if (n.endsWithIgnoreCase(".dalproj") || n.endsWithIgnoreCase(".mdlproj"))
                {
                    if (!(c == projectFile))
                    {
                        return "The project folder already contains a different project file:\n"
                               + c.getFullPathName()
                               + "\n\nChoose a different folder or name.";
                    }
                }
            }
        }
        return {};
    }
} // namespace

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
    // in debug: some audio output must open for playback; input is optional (see device init).
    void initialise(const juce::String&) override
    {
        // Domain objects first: the engine only holds references; safe because we create them
        // in dependency order and tear down in reverse in shutdown.
        transport = std::make_unique<Transport>();
        session = std::make_unique<Session>();
        // Phase 4: owned by the app; `PlaybackEngine` gets a non-owning pointer for input push
        // (only while `isRecording()`; see `PlaybackEngine` gate). Input channel count comes from
        // device init below.
        recorderService = std::make_unique<RecorderService>();
        countInOutput_ = std::make_unique<CountInClickOutput>();
        playbackEngine = std::make_unique<PlaybackEngine>(
            *transport, *session, recorderService.get(), countInOutput_.get());

        // JUCE: open the default audio device before we register the engine. Prefer **1 input, 2
        // outputs** (mono in for future record path; stereo out unchanged). If the default device
        // cannot provide an input, fall back to output-only so existing playback is not broken.
        juce::String audioInitError = deviceManager.initialiseWithDefaultDevices(1, 2);
        if (audioInitError.isNotEmpty())
        {
            juce::Logger::writeToLog(
                juce::String{"[Audio] 1-in/2-out not available: "} + audioInitError
                + " — retrying output-only (0 in / 2 out). Input capture disabled until a suitable device exists.");
            audioInitError = deviceManager.initialiseWithDefaultDevices(0, 2);
        }
        jassert(audioInitError.isEmpty());
        juce::ignoreUnused(audioInitError);

        // After this line, the audio thread can call our PlaybackEngine; keep UI after so we do
        // not paint or load files before the device exists.
        deviceManager.addAudioCallback(playbackEngine.get());

        mainWindow = std::make_unique<MainWindow>(
            getApplicationName(),
            *transport,
            *session,
            deviceManager,
            *recorderService,
            *countInOutput_);
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

        // After callback removal, drop engine (it held `recorderService.get()` for audio thread) then
        // the recorder, then the rest. `RecorderService` is independent of Transport/Session.
        playbackEngine.reset();
        countInOutput_.reset();
        recorderService.reset();
        session.reset();
        transport.reset();
    }

    void systemRequestedQuit() override { quit(); }

private:
    // [Message thread only] Child component: file chooser, transport buttons, timeline ruler, lane.
    // Holds non-owning refs; MainWindow and application own lifetime. Add path: FileChooser
    // (async) → `Transport::readPlayheadSamplesForUi` once, then `addClipFromFileAtPlayhead`.
    class TransportControlsContent : public juce::Component, private juce::Timer
    {
    public:
        TransportControlsContent(Transport& transportIn,
                                 Session& sessionIn,
                                 juce::AudioDeviceManager& deviceManagerIn,
                                 RecorderService& recorderIn,
                                 CountInClickOutput& countInClicksIn)
            : transport(transportIn)
            , session(sessionIn)
            , deviceManager(deviceManagerIn)
            , recorder_(recorderIn)
            , countInClicks_(countInClicksIn)
            , timelineViewport_()
            , rulerView(sessionIn, transportIn, deviceManagerIn, timelineViewport_)
            , trackLanesView(sessionIn, transportIn, timelineViewport_, deviceManagerIn, recorderIn)
        {
            setWantsKeyboardFocus(true);
            timelineViewport_.setOnVisibleRangeChanged([this] {
                rulerView.repaint();
                trackLanesView.repaint();
            });
            addClipButton.onClick = [this] { addClipAtPlayheadClicked(); };
            addTrackButton.onClick = [this] {
                session.addTrack();
                syncViewportFromSession();
                trackLanesView.syncTracksFromSession();
            };
            saveProjectButton.onClick = [this] { saveProjectClicked(); };
            loadProjectButton.onClick = [this] { loadProjectClicked(); };
            playPauseButton.onClick = [this] { togglePlayPauseFromUi(); };
            // Stop: "playback off + playhead to start" when idle; if recording, finalize/commit first
            // so RecorderService is never left recording while transport is Stopped.
            stopButton.onClick = [this] { stopOrSeekFromStopButton(); };

            addAndMakeVisible(addClipButton);
            addAndMakeVisible(addTrackButton);
            addAndMakeVisible(saveProjectButton);
            addAndMakeVisible(loadProjectButton);
            addAndMakeVisible(playPauseButton);
            addAndMakeVisible(stopButton);
            if (kShowKeyDiagnostic)
            {
                addAndMakeVisible(keyDiagLabel_);
                keyDiagLabel_.setFont(juce::FontOptions(11.0f));
                keyDiagLabel_.setJustificationType(juce::Justification::centredLeft);
                keyDiagLabel_.setText("key: —", juce::dontSendNotification);
            }
            countInStatusLabel_.setFont(juce::FontOptions(12.0f));
            countInStatusLabel_.setJustificationType(juce::Justification::centredLeft);
            addAndMakeVisible(countInStatusLabel_);
            addAndMakeVisible(rulerView);
            addAndMakeVisible(trackLanesView);
            updatePlayPauseButtonFromTransport();
            startTimerHz(10);
            syncViewportFromSession();
        }

        ~TransportControlsContent() override { cancelCountIn(); }

        // [Message thread] Invoked only from `MainWindow` shortcut router (not from child
        // `keyPressed` — avoids duplicate `numpadRecordToggled` on one physical keypress).
        void invokeRecordToggleFromWindowShortcut() { numpadRecordToggled(); }
        // [Message thread] Space: when recording, commit (source tag `space`); else same as Play/Pause.
        void invokePlayPauseToggleFromWindowShortcut()
        {
            if (recorder_.isRecording())
            {
                stopRecordingAndCommitFromUi("space");
                return;
            }
            togglePlayPauseTransportOnly();
        }
        void setKeyDiagnosticLine(const juce::String& line)
        {
            if (kShowKeyDiagnostic)
            {
                keyDiagLabel_.setText(line, juce::dontSendNotification);
            }
        }

        // [Message thread] Layout: one row of buttons, fixed-height time ruler, then event lane.
        void resized() override
        {
            auto area = getLocalBounds().reduced(8);
            auto row = area.removeFromTop(32);
            if (kShowKeyDiagnostic)
            {
                keyDiagLabel_.setBounds(row.removeFromRight(300).reduced(2, 0));
            }
            constexpr int kCountInLabelWidth = 140;
            countInStatusLabel_.setBounds(row.removeFromRight(kCountInLabelWidth).reduced(4, 0));
            const int buttonWidth = juce::jmax(48, row.getWidth() / 6);

            addClipButton.setBounds(row.removeFromLeft(buttonWidth).reduced(2));
            addTrackButton.setBounds(row.removeFromLeft(buttonWidth).reduced(2));
            saveProjectButton.setBounds(row.removeFromLeft(buttonWidth).reduced(2));
            loadProjectButton.setBounds(row.removeFromLeft(buttonWidth).reduced(2));
            playPauseButton.setBounds(row.removeFromLeft(buttonWidth).reduced(2));
            stopButton.setBounds(row.removeFromLeft(buttonWidth).reduced(2));
            constexpr int kTimelineRulerHeight = 20;
            auto timelineRow = area.removeFromTop(kTimelineRulerHeight);
            timelineRow.removeFromLeft(TrackLanesView::kTrackHeaderWidth);
            rulerView.setBounds(timelineRow);
            trackLanesView.setBounds(area);
        }

    private:
        struct CountInTimer final : juce::Timer
        {
            explicit CountInTimer(TransportControlsContent& o)
                : owner(o)
            {
            }
            void timerCallback() override { owner.onCountInTimerTick(); }
            TransportControlsContent& owner;
        };
        friend struct CountInTimer;

        void timerCallback() override { updatePlayPauseButtonFromTransport(); }

        // [Message thread] Transport intent: Playing → Paused, else (Stopped or Paused) → Playing.
        // If a take is in progress, the button stops/commits (never Paused+recording).
        void togglePlayPauseTransportOnly()
        {
            if (transport.readPlaybackIntentForUi() == PlaybackIntent::Playing)
            {
                transport.requestPlaybackIntent(PlaybackIntent::Paused);
            }
            else
            {
                transport.requestPlaybackIntent(PlaybackIntent::Playing);
            }
            updatePlayPauseButtonFromTransport();
        }

        void togglePlayPauseFromUi()
        {
            if (isCountInActive())
            {
                cancelCountIn();
                return;
            }
            if (recorder_.isRecording())
            {
                stopRecordingAndCommitFromUi("play_pause");
                return;
            }
            togglePlayPauseTransportOnly();
        }

        // [Message thread] Stop button: normal stop+seek, or end recording and commit (then seek 0).
        void stopOrSeekFromStopButton()
        {
            if (isCountInActive())
            {
                cancelCountIn();
                return;
            }
            if (recorder_.isRecording())
            {
                stopRecordingAndCommitFromUi("stop");
            }
            else
            {
                transport.requestPlaybackIntent(PlaybackIntent::Stopped);
            }
            transport.requestSeek(0);
            updatePlayPauseButtonFromTransport();
        }

        void updatePlayPauseButtonFromTransport()
        {
            const bool playing = transport.readPlaybackIntentForUi() == PlaybackIntent::Playing;
            const juce::String t = playing ? "Pause" : "Play";
            if (t != playPauseButton.getButtonText())
            {
                playPauseButton.setButtonText(t);
            }
        }

        // [Message thread] Seed default arrangement + samples-per-pixel once sample rate is known;
        // clamp the pan window to the current arrangement extent (when ruler width is known).
        void syncViewportFromSession()
        {
            juce::AudioIODevice* const dev = deviceManager.getCurrentAudioDevice();
            if (dev != nullptr)
            {
                const double sr = dev->getCurrentSampleRate();
                if (sr > 0.0)
                {
                    if (session.getStoredArrangementExtentSamples() == 0
                        && session.getContentEndSamples() == 0)
                    {
                        session.setArrangementExtentSamples(
                            (std::int64_t)std::llround(3600.0 * sr));
                    }
                    // Default: 10 pixels per second of session time; visible **length in samples** is
                    // derived as `round(rulerWidthPx * (sr/10))` and grows/shrinks with window width.
                    constexpr double kDefaultPixelsPerSecond = 10.0;
                    timelineViewport_.setSamplesPerPixelIfUnset(sr / kDefaultPixelsPerSecond);
                }
            }
            {
                const double rw = (double)rulerView.getWidth();
                if (rw > 0.0)
                {
                    timelineViewport_.clampToExtent(rw, session.getArrangementExtentSamples());
                }
            }
        }

        // [Message thread] Presents a native file dialog; on success, new clip is placed on the
        // **session** timeline at the current `Transport` playhead (read once, here, not on audio).
        void addClipAtPlayheadClicked()
        {
            if (importInFlight_)
            {
                juce::Logger::writeToLog(
                    juce::String("[CLIMPORT] STAGE:ui:ignored_second_add_while_chooser_in_flight"));
                return;
            }
            importInFlight_ = true;

            const auto fileChooserFlags = juce::FileBrowserComponent::openMode
                                          | juce::FileBrowserComponent::canSelectFiles;

            auto chooser = std::make_shared<juce::FileChooser>(
                "Add audio at playhead",
                juce::File{},
                "*.wav;*.aif;*.aiff;*.flac;*.ogg;*.mp3");

            // JUCE: async dialog; the lambda runs on the *message* thread when the user dismisses
            // the picker. We record playhead and decode in this callback — the agreed “at add
            // time” read for placement (not the audio thread).
            chooser->launchAsync(fileChooserFlags, [this, chooser](const juce::FileChooser& fc) {
                juce::ignoreUnused(chooser);
                struct ClearImportInFlight
                {
                    bool& b;
                    explicit ClearImportInFlight(bool& ref) noexcept
                        : b(ref)
                    {
                    }
                    ~ClearImportInFlight() { b = false; }
                } clearImport{importInFlight_};

                const juce::File file = fc.getResult();
                if (!file.existsAsFile())
                {
                    // Cancel or empty selection — not an error, keep the current session.
                    juce::Logger::writeToLog(
                        juce::String("[CLIMPORT] STAGE:ui:chooser_dismissed_cancel_or_no_file"));
                    return;
                }

                juce::AudioIODevice* const device = deviceManager.getCurrentAudioDevice();
                if (device == nullptr)
                {
                    juce::Logger::writeToLog("[CLIMPORT] STAGE:ui:fail no_audio_device");
                    juce::AlertWindow::showMessageBoxAsync(
                        juce::AlertWindow::WarningIcon,
                        "Audio",
                        "No active audio device. Cannot validate sample rate for load.");
                    return;
                }

                juce::Logger::writeToLog(
                    juce::String("[CLIMPORT] STAGE:ui:chooser_ok file=") + file.getFullPathName());
                // Snapshot once: this value becomes `PlacedClip::startSampleOnTimeline` for the
                // new row (see Session / `PHASE_PLAN` add-at-playhead).
                const std::int64_t startSampleOnTimeline = transport.readPlayheadSamplesForUi();

                // Loader must match the *running* device rate (Phase 1 contract).
                const double sampleRate = device->getCurrentSampleRate();
                const juce::Result loadResult =
                    session.addClipFromFileAtPlayhead(file, sampleRate, startSampleOnTimeline);

                if (!loadResult.wasOk())
                {
                    juce::Logger::writeToLog(
                        juce::String("[CLIMPORT] STAGE:ui:session_add_failed err=") + loadResult.getErrorMessage());
                    juce::AlertWindow::showMessageBoxAsync(
                        juce::AlertWindow::WarningIcon,
                        "Could not open file",
                        loadResult.getErrorMessage());
                }
                else
                {
                    juce::Logger::writeToLog(
                        juce::String("[CLIMPORT] STAGE:ui:sync:begin file=") + file.getFileName());
                    // New **front** clip is on the active track; playhead/transport are unchanged.
                    syncViewportFromSession();
                    trackLanesView.syncTracksFromSession();
                    trackLanesView.repaint();
                    juce::Logger::writeToLog(juce::String("[CLIMPORT] STAGE:ui:sync:done file=") + file.getFileName());
                }
            });
        }

        void saveProjectClicked()
        {
            juce::AudioIODevice* const device = deviceManager.getCurrentAudioDevice();
            if (device == nullptr)
            {
                juce::AlertWindow::showMessageBoxAsync(
                    juce::AlertWindow::WarningIcon,
                    "Save project",
                    "No active audio device; cannot include device sample rate in the project file.");
                return;
            }
            const double sampleRate = device->getCurrentSampleRate();

            // Normal save: no chooser. Explicit "Save As" / "New project" is deferred.
            if (session.hasKnownProjectFile())
            {
                const juce::Result r
                    = session.saveProjectToFile(transport, session.getCurrentProjectFile(), sampleRate);
                if (!r.wasOk())
                {
                    juce::AlertWindow::showMessageBoxAsync(
                        juce::AlertWindow::WarningIcon, "Save project", r.getErrorMessage());
                }
                return;
            }

            // First-time save: DAW-style `<Parent>/<ProjectName>/<ProjectName>.dalproj`
            const auto fileChooserFlags = juce::FileBrowserComponent::saveMode
                                          | juce::FileBrowserComponent::canSelectFiles;
            auto chooser = std::make_shared<juce::FileChooser>(
                "Save project as…",
                juce::File{},
                "*.dalproj");
            chooser->launchAsync(fileChooserFlags, [this, chooser, sampleRate](const juce::FileChooser& fc) {
                juce::ignoreUnused(chooser);
                juce::File userPick = fc.getResult();
                if (userPick.getFullPathName().isEmpty())
                {
                    return;
                }
                if (!userPick.hasFileExtension("dalproj"))
                {
                    userPick = userPick.getSiblingFile(
                        userPick.getFileNameWithoutExtension() + ".dalproj");
                }
                const juce::String projectName = userPick.getFileNameWithoutExtension();
                if (projectName.isEmpty())
                {
                    juce::AlertWindow::showMessageBoxAsync(
                        juce::AlertWindow::WarningIcon,
                        "Save project",
                        "Invalid project name.");
                    return;
                }
                const juce::File parentDir = userPick.getParentDirectory();
                const juce::File projectFolder = parentDir.getChildFile(projectName);
                const juce::File projectFile
                    = projectFolder.getChildFile(projectName + ".dalproj");
                {
                    const juce::String conflict = firstTimeSaveConflictMessage(projectFolder, projectFile);
                    if (conflict.isNotEmpty())
                    {
                        juce::AlertWindow::showMessageBoxAsync(
                            juce::AlertWindow::WarningIcon, "Save project", conflict);
                        return;
                    }
                }
                if (!projectFolder.isDirectory() && !projectFolder.createDirectory())
                {
                    juce::AlertWindow::showMessageBoxAsync(
                        juce::AlertWindow::WarningIcon,
                        "Save project",
                        "Could not create the project folder:\n" + projectFolder.getFullPathName());
                    return;
                }
                {
                    const juce::String conflict2
                        = firstTimeSaveConflictMessage(projectFolder, projectFile);
                    if (conflict2.isNotEmpty())
                    {
                        juce::AlertWindow::showMessageBoxAsync(
                            juce::AlertWindow::WarningIcon, "Save project", conflict2);
                        return;
                    }
                }
                const juce::Result r = session.saveProjectToFile(transport, projectFile, sampleRate);
                if (!r.wasOk())
                {
                    juce::AlertWindow::showMessageBoxAsync(
                        juce::AlertWindow::WarningIcon, "Save project", r.getErrorMessage());
                }
            });
        }

        void loadProjectClicked()
        {
            juce::AudioIODevice* const device = deviceManager.getCurrentAudioDevice();
            if (device == nullptr)
            {
                juce::AlertWindow::showMessageBoxAsync(
                    juce::AlertWindow::WarningIcon,
                    "Load project",
                    "No active audio device; cannot match sample rate to decode project clips.");
                return;
            }
            const double sampleRate = device->getCurrentSampleRate();

            const auto fileChooserFlags = juce::FileBrowserComponent::openMode
                                          | juce::FileBrowserComponent::canSelectFiles;
            auto chooser = std::make_shared<juce::FileChooser>(
                "Load project",
                juce::File{},
                "*.dalproj;*.mdlproj");
            chooser->launchAsync(fileChooserFlags, [this, chooser, sampleRate](const juce::FileChooser& fc) {
                juce::ignoreUnused(chooser);
                const juce::File f = fc.getResult();
                if (!f.existsAsFile())
                {
                    return;
                }
                juce::StringArray skipped;
                juce::String infoNote;
                const juce::Result r
                    = session.loadProjectFromFile(transport, f, sampleRate, skipped, infoNote);
                if (!r.wasOk())
                {
                    juce::AlertWindow::showMessageBoxAsync(
                        juce::AlertWindow::WarningIcon, "Load project", r.getErrorMessage());
                    return;
                }
                syncViewportFromSession();
                trackLanesView.syncTracksFromSession();
                rulerView.repaint();
                trackLanesView.repaint();
                if (infoNote.isNotEmpty() || skipped.size() > 0)
                {
                    juce::String body;
                    if (infoNote.isNotEmpty())
                    {
                        body = infoNote;
                    }
                    if (skipped.size() > 0)
                    {
                        if (body.isNotEmpty())
                        {
                            body << "\n\n";
                        }
                        body << "Could not load " + juce::String(skipped.size())
                             + (skipped.size() == 1 ? " file:" : " files:") + "\n\n";
                        for (int i = 0; i < skipped.size(); ++i)
                        {
                            body << skipped[i] << (i < skipped.size() - 1 ? "\n" : "");
                        }
                    }
                    juce::AlertWindow::showMessageBoxAsync(
                        juce::AlertWindow::InfoIcon, "Load project (partial or note)", body);
                }
            });
        }

        // [Message thread] One path to end a take: stop transport first, then finalize, then commit.
        // Call only when `recorder_.isRecording()`; no-op if not.
        void stopRecordingAndCommitFromUi(const char* sourceContext)
        {
            if (!recorder_.isRecording())
            {
                return;
            }
            if (sourceContext != nullptr)
            {
                juce::Logger::writeToLog(
                    juce::String{"[Rec] stop/commit source="} + sourceContext);
            }

            transport.requestPlaybackIntent(PlaybackIntent::Stopped);
            updatePlayPauseButtonFromTransport();
            const RecordedTakeResult r = recorder_.stopRecordingAndFinalize();
            if (!r.success)
            {
                juce::AlertWindow::showMessageBoxAsync(
                    juce::AlertWindow::WarningIcon,
                    "Recording",
                    r.errorMessage.isNotEmpty() ? r.errorMessage : "Could not finalize recording.");
                juce::Logger::writeToLog(
                    juce::String{"[Rec] stop/finalize failed: "} + r.errorMessage);
                return;
            }
            if (r.droppedSampleCount > 0)
            {
                const juce::String w = "Recording overrun: " + juce::String(r.droppedSampleCount)
                                        + (r.droppedSampleCount == 1 ? " sample was" : " samples were")
                                        + " replaced with silence.";
                juce::Logger::writeToLog(juce::String{"[Rec] "} + w);
                juce::AlertWindow::showMessageBoxAsync(
                    juce::AlertWindow::InfoIcon, "Recording", w);
            }
            const juce::Result ar = session.addRecordedTakeAtSample(
                r.takeFile,
                r.sampleRate,
                r.recordingStartSample,
                r.targetTrackId,
                r.intendedSampleCount);
            if (!ar.wasOk())
            {
                juce::AlertWindow::showMessageBoxAsync(
                    juce::AlertWindow::WarningIcon, "Session", ar.getErrorMessage());
                juce::Logger::writeToLog(
                    juce::String{"[Rec] addRecordedTakeAtSample failed: "} + ar.getErrorMessage());
            }
            else
            {
                syncViewportFromSession();
            }
            rulerView.repaint();
            trackLanesView.repaint();
        }

        void numpadRecordToggled()
        {
            if (recorder_.isRecording())
            {
                stopRecordingAndCommitFromUi("numpad_*");
                return;
            }
            if (isCountInActive())
            {
                cancelCountIn();
                juce::Logger::writeToLog("[Rec] count-in cancelled (numpad_*)");
                return;
            }

            const TrackId armed = recorder_.getArmedTrackId();
            if (armed == kInvalidTrackId)
            {
                juce::AlertWindow::showMessageBoxAsync(
                    juce::AlertWindow::InfoIcon,
                    "Recording",
                    "Arm a track for recording (use the R control on a track header) first.");
                juce::Logger::writeToLog("[Rec] start blocked: no armed track");
                return;
            }
            if (!session.hasKnownProjectFile())
            {
                juce::AlertWindow::showMessageBoxAsync(
                    juce::AlertWindow::InfoIcon,
                    "Recording",
                    "Save the project before recording.");
                juce::Logger::writeToLog("[Rec] start blocked: project not saved to disk");
                return;
            }
            juce::File projectFile = session.getCurrentProjectFile();
            if (projectFile.getFullPathName().isEmpty())
            {
                juce::Logger::writeToLog("[Rec] start blocked: empty project file path");
                return;
            }
            juce::File audioDir = session.getCurrentProjectFolder().getChildFile("Audio");
            if (audioDir.getFullPathName().isEmpty())
            {
                juce::Logger::writeToLog("[Rec] start blocked: could not build Audio/ path");
                return;
            }
            if (!audioDir.isDirectory() && !audioDir.createDirectory())
            {
                juce::AlertWindow::showMessageBoxAsync(
                    juce::AlertWindow::WarningIcon,
                    "Recording",
                    "Could not create the project Audio folder: " + audioDir.getFullPathName());
                juce::Logger::writeToLog("[Rec] start blocked: createDirectory Audio/ failed");
                return;
            }
            const juce::File takeWav = makeUniqueTakeWavInProjectAudioDir(audioDir);
            juce::AudioIODevice* const dev = deviceManager.getCurrentAudioDevice();
            if (dev == nullptr)
            {
                juce::AlertWindow::showMessageBoxAsync(
                    juce::AlertWindow::WarningIcon, "Audio", "No active audio device.");
                return;
            }
            if (dev->getActiveInputChannels().countNumberOfSetBits() < 1)
            {
                juce::AlertWindow::showMessageBoxAsync(
                    juce::AlertWindow::WarningIcon,
                    "Audio",
                    "No input channel is active. Enable an input in your audio device, then try again.");
                juce::Logger::writeToLog("[Rec] start blocked: no active input channels");
                return;
            }
            const double sr = dev->getCurrentSampleRate();
            if (sr <= 0.0)
            {
                juce::AlertWindow::showMessageBoxAsync(
                    juce::AlertWindow::WarningIcon, "Audio", "Invalid device sample rate.");
                return;
            }
            BeginRecordingRequest req;
            req.takeFile = takeWav;
            req.targetTrackId = armed;
            // Filled in `completeCountInAndStartRecording` from the playhead at the moment
            // `beginRecording` runs (after 8 count-in clicks + 375 ms silent tail).
            req.recordingStartSample = 0;
            req.sampleRate = sr;
            // Count-in: no `beginRecording` until clicks + post-click delay; Session not touched before.
            startCountInAfterValidation(std::move(req));
        }

        [[nodiscard]] bool isCountInActive() const noexcept { return pendingCountIn_.has_value(); }

        void cancelCountIn()
        {
            if (countInTimer_ != nullptr)
            {
                countInTimer_->stopTimer();
            }
            countInAwaitingPostClickDelay_ = false;
            pendingCountIn_.reset();
            countInClicks_.cancel();
            countInStatusLabel_.setText({}, juce::dontSendNotification);
            juce::Logger::writeToLog("[Rec] count-in cancelled");
        }

        void onCountInTimerTick()
        {
            if (!pendingCountIn_.has_value())
            {
                if (countInTimer_ != nullptr)
                {
                    countInTimer_->stopTimer();
                }
                return;
            }
            // After the 8th click, one more interval of silence, then `beginRecording` (no 9th click).
            if (countInAwaitingPostClickDelay_)
            {
                countInAwaitingPostClickDelay_ = false;
                if (countInTimer_ != nullptr)
                {
                    countInTimer_->stopTimer();
                }
                completeCountInAndStartRecording();
                return;
            }
            // Cubase-like: tick tock tock tock | tick tock tock tock; 375 ms per step; first click
            // one interval after * so the keydown is not on-mic. Extra 375 ms after last click
            // before arming the recorder (reduces headphone bleed from the final click).
            static constexpr int kClicks = 8;
            ++countInBeat_;
            if (countInBeat_ < 1 || countInBeat_ > kClicks)
            {
                if (countInTimer_ != nullptr)
                {
                    countInTimer_->stopTimer();
                }
                return;
            }
            const bool useTick = (countInBeat_ == 1 || countInBeat_ == 5);
            if (useTick)
            {
                countInClicks_.triggerTick();
            }
            else
            {
                countInClicks_.triggerTock();
            }
            countInStatusLabel_.setText("Count-in: " + juce::String(countInBeat_) + "/"
                                        + juce::String(kClicks),
                                        juce::dontSendNotification);
            if (countInBeat_ == kClicks)
            {
                countInAwaitingPostClickDelay_ = true;
                countInStatusLabel_.setText("Get ready…", juce::dontSendNotification);
            }
        }

        void startCountInAfterValidation(BeginRecordingRequest&& req)
        {
            countInClicks_.prepare(req.sampleRate);
            pendingCountIn_ = std::move(req);
            countInBeat_ = 0;
            countInAwaitingPostClickDelay_ = false;
            if (countInTimer_ == nullptr)
            {
                countInTimer_ = std::make_unique<CountInTimer>(*this);
            }
            // First audible click is after kCountInIntervalMs, not on keydown.
            countInStatusLabel_.setText("Count-in…", juce::dontSendNotification);
            static constexpr int kCountInIntervalMs = 375;
            countInTimer_->startTimer(kCountInIntervalMs);
            juce::Logger::writeToLog(
                "[Rec] count-in started (8 clicks, 375 ms, +375 ms pre-roll before record)");
        }

        void completeCountInAndStartRecording()
        {
            if (!pendingCountIn_.has_value())
            {
                return;
            }
            BeginRecordingRequest req = *pendingCountIn_;
            req.recordingStartSample = transport.readPlayheadSamplesForUi();
            pendingCountIn_.reset();
            countInStatusLabel_.setText({}, juce::dontSendNotification);
            if (!recorder_.beginRecording(req))
            {
                juce::String err = recorder_.getLastError();
                if (err.isEmpty())
                {
                    err = "beginRecording failed";
                }
                juce::AlertWindow::showMessageBoxAsync(
                    juce::AlertWindow::WarningIcon, "Recording", err);
                juce::Logger::writeToLog(juce::String{"[Rec] beginRecording failed: "} + err);
                return;
            }
            transport.requestPlaybackIntent(PlaybackIntent::Playing);
            updatePlayPauseButtonFromTransport();
        }

        Transport& transport;
        Session& session;
        juce::AudioDeviceManager& deviceManager;
        RecorderService& recorder_;
        CountInClickOutput& countInClicks_;
        std::optional<BeginRecordingRequest> pendingCountIn_;
        int countInBeat_ = 0;
        /// True after the 8th click: next timer fire ends count-in and starts `beginRecording`.
        bool countInAwaitingPostClickDelay_ = false;
        std::unique_ptr<CountInTimer> countInTimer_;
        juce::Label countInStatusLabel_;

        /// Set while a file chooser for Add clip is in flight; blocks overlapping Add clip clicks.
        bool importInFlight_ = false;

        juce::TextButton addClipButton{ "Add clip..." };
        juce::TextButton addTrackButton{ "Add track" };
        juce::TextButton saveProjectButton{ "Save Project..." };
        juce::TextButton loadProjectButton{ "Load Project..." };
        juce::TextButton playPauseButton{ "Play" };
        juce::TextButton stopButton{ "Stop" };
        juce::Label keyDiagLabel_;

        /// UI-only: shared x–span for ruler and lanes; never stored in `Session` (see `PHASE_PLAN`).
        TimelineViewportModel timelineViewport_;
        TimelineRulerView rulerView;
        TrackLanesView trackLanesView;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TransportControlsContent)
    };

    // [Message thread] Top-level JUCE window: one `juce::KeyListener` on **this** so shortcuts are
    // always visited in `ComponentPeer::handleKeyPress` (including when focus is null → peer root).
    class MainWindow : public juce::DocumentWindow, public juce::KeyListener
    {
    public:
        MainWindow(const juce::String& name,
                   Transport& transport,
                   Session& session,
                   juce::AudioDeviceManager& deviceManager,
                   RecorderService& recorderService,
                   CountInClickOutput& countInClicks)
            : DocumentWindow(
                  name,
                  juce::Desktop::getInstance().getDefaultLookAndFeel().findColour(
                      juce::ResizableWindow::backgroundColourId),
                  DocumentWindow::allButtons)
        {
            setUsingNativeTitleBar(true);
            setContentOwned(
                new TransportControlsContent(
                    transport, session, deviceManager, recorderService, countInClicks),
                true);
            setResizable(true, true);
            setResizeLimits(320, 240, 10000, 10000);
            centreWithSize(640, 400);
            addKeyListener(this);
            if (juce::Component* c = getContentComponent())
            {
                c->setWantsKeyboardFocus(true);
            }
            juce::MessageManager::callAsync([this] {
                if (juce::Component* c = getContentComponent())
                {
                    c->grabKeyboardFocus();
                }
            });
            setVisible(true);
        }

        ~MainWindow() override { removeKeyListener(this); }

        // [Message thread] User clicked the window close: end the application.
        void closeButtonPressed() override
        {
            juce::JUCEApplication::getInstance()->systemRequestedQuit();
        }

        void activeWindowStatusChanged() override
        {
            juce::DocumentWindow::activeWindowStatusChanged();
            if (isActiveWindow())
            {
                juce::MessageManager::callAsync([this] {
                    if (juce::Component* c = getContentComponent())
                    {
                        c->grabKeyboardFocus();
                    }
                });
            }
        }

        bool keyPressed(const juce::KeyPress& key, juce::Component* originating) override
        {
            juce::ignoreUnused(originating);
            if (kShowKeyDiagnostic)
            {
                if (auto* tcc = dynamic_cast<TransportControlsContent*>(getContentComponent()))
                {
                    tcc->setKeyDiagnosticLine(
                        juce::String{"0x" } + juce::String::toHexString((juce::uint32)key.getKeyCode())
                        + " ch=0x" + juce::String::toHexString((juce::uint32)key.getTextCharacter()) + " "
                        + key.getTextDescription());
                }
            }
            return routeShortcut(key);
        }

    private:
        [[nodiscard]] bool routeShortcut(const juce::KeyPress& key)
        {
            if (isRecordToggleShortcut(key))
            {
                if (auto* tcc = dynamic_cast<TransportControlsContent*>(getContentComponent()))
                {
                    tcc->invokeRecordToggleFromWindowShortcut();
                    juce::Logger::writeToLog(
                        juce::String{"[Shortcut] record toggle: "} + key.getTextDescription());
                    return true;
                }
                return false;
            }
            if (isSpacePlayPauseShortcut(key))
            {
                if (auto* tcc = dynamic_cast<TransportControlsContent*>(getContentComponent()))
                {
                    tcc->invokePlayPauseToggleFromWindowShortcut();
                    juce::Logger::writeToLog(
                        juce::String{"[Shortcut] play/pause: "} + key.getTextDescription());
                    return true;
                }
                return false;
            }
            return false;
        }

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainWindow)
    };

    std::unique_ptr<Transport> transport;
    std::unique_ptr<Session> session;
    // Phase 4: recording capture (not user-wired in this file yet); engine holds non-owning `get()`.
    std::unique_ptr<RecorderService> recorderService;
    /// Count-in metronome clicks to device only; coordinator state lives in `TransportControlsContent`.
    std::unique_ptr<CountInClickOutput> countInOutput_;
    std::unique_ptr<PlaybackEngine> playbackEngine;
    juce::AudioDeviceManager deviceManager;
    std::unique_ptr<MainWindow> mainWindow;
};

// JUCE: generate WinMain / main and the app singleton; DO NOT add another main().
START_JUCE_APPLICATION(MiniDAWLabApplication)
