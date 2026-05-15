#include "app/AudioClipImportCoordinator.h"

#include <memory>

#include "domain/Session.h"
#include "io/ProjectAudioImport.h"
#include "transport/Transport.h"
#include "ui/InspectorView.h"
#include "ui/TimelineRulerView.h"
#include "ui/TrackLanesView.h"

AudioClipImportCoordinator::AudioClipImportCoordinator(Session& session,
                                                       Transport& transport,
                                                       juce::AudioDeviceManager& deviceManager,
                                                       TrackLanesView& trackLanesView,
                                                       TimelineRulerView& rulerView,
                                                       InspectorView& inspectorView,
                                                       Callbacks callbacks)
    : session_(session)
    , transport_(transport)
    , deviceManager_(deviceManager)
    , trackLanesView_(trackLanesView)
    , rulerView_(rulerView)
    , inspectorView_(inspectorView)
    , callbacks_(std::move(callbacks))
{
}

void AudioClipImportCoordinator::addClipAtPlayheadClicked()
{
    if (!session_.hasKnownProjectFile())
    {
        juce::AlertWindow::showMessageBoxAsync(
            juce::AlertWindow::InfoIcon,
            "Add clip",
            "Save the project before importing audio.");
        return;
    }
    if (importInFlight_)
    {
        return;
    }
    importInFlight_ = true;

    const auto fileChooserFlags
        = juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles;

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
            return;
        }

        juce::AudioIODevice* const device = deviceManager_.getCurrentAudioDevice();
        if (device == nullptr)
        {
            juce::AlertWindow::showMessageBoxAsync(
                juce::AlertWindow::WarningIcon,
                "Audio",
                "No active audio device. Cannot validate sample rate for load.");
            return;
        }

        // Snapshot once: this value becomes `PlacedClip::startSampleOnTimeline` for the
        // new row (see Session / `PHASE_PLAN` add-at-playhead).
        const std::int64_t startSampleOnTimeline = transport_.readPlayheadSamplesForUi();

        // Loader must match the *running* device rate (Phase 1 contract).
        const double sampleRate = device->getCurrentSampleRate();

        const juce::File audioDir = mini_daw::getProjectAudioDir(session_.getCurrentProjectFolder());
        juce::File pathToUse;
        const juce::Result importRes
            = mini_daw::importAudioIntoProjectAudioDir(file, audioDir, pathToUse);
        if (!importRes.wasOk())
        {
            juce::AlertWindow::showMessageBoxAsync(
                juce::AlertWindow::WarningIcon,
                "Could not import audio",
                importRes.getErrorMessage());
            return;
        }
        callbacks_.executeUndoableSessionEdit("Import clip", [&]() -> bool {
            const juce::Result loadResult = session_.addClipFromFileAtPlayhead(
                pathToUse, sampleRate, startSampleOnTimeline);
            if (!loadResult.wasOk())
            {
                juce::AlertWindow::showMessageBoxAsync(
                    juce::AlertWindow::WarningIcon,
                    "Could not open file",
                    loadResult.getErrorMessage());
                return false;
            }
            // New **front** clip is on the active track; playhead/transport are unchanged.
            callbacks_.syncViewportFromSession();
            trackLanesView_.syncTracksFromSession();
            rulerView_.repaint();
            trackLanesView_.repaint();
            inspectorView_.refreshFromSession();
            return true;
        });
    });
}
