#include "app/MainAppDialogs.h"

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_utils/juce_audio_utils.h>

#include "app/RecordingCoordinator.h"
#include "audio/LatencySettingsStore.h"
#include "engine/PlaybackEngine.h"
#include "engine/RecorderService.h"
#include "transport/Transport.h"
#include "ui/LatencySettingsView.h"

namespace
{
class AudioSettingsDialogContent final : public juce::Component
{
public:
    AudioSettingsDialogContent(juce::AudioDeviceManager& dm,
                               LatencySettingsStore& latencyStore,
                               PlaybackEngine& playbackEngine)
        : selector_(dm, 0, 2, 2, 2, false, false, false, false)
        , latencyView_(latencyStore, playbackEngine)
    {
        addAndMakeVisible(selector_);
        addAndMakeVisible(latencyView_);
        setSize(640, 680);
    }

    void resized() override
    {
        constexpr int kGapBelowSelectorPx = 10;
        auto area = getLocalBounds();
        const int w = area.getWidth();
        const int topY = area.getY();

        // AudioDeviceSelectorComponent ends resized() by setSize(w, intrinsicHeight). Lay it out
        // with enough vertical slack first so internal controls measure correctly; then tighten
        // its bounds to that height so we do not leave a tall empty band above the latency panel.
        const int provisionalH = juce::jmax(1, area.getHeight() - kGapBelowSelectorPx);
        selector_.setBounds(area.getX(), topY, w, provisionalH);
        const int selectorH = juce::jmax(1, selector_.getHeight());
        selector_.setBounds(area.getX(), topY, w, selectorH);

        const int latencyY = topY + selectorH + kGapBelowSelectorPx;
        const int latencyH = juce::jmax(1, area.getBottom() - latencyY);
        latencyView_.setBounds(area.getX(), latencyY, w, latencyH);
    }

    [[nodiscard]] LatencySettingsView& getLatencyPane() noexcept { return latencyView_; }

private:
    juce::AudioDeviceSelectorComponent selector_;
    LatencySettingsView latencyView_;
};

[[nodiscard]] juce::String undoBehaviorHelpBodyText()
{
    return juce::String(
        "Undo will restore previous session/timeline states, such as:\n"
        "  - clip moves\n"
        "  - clip trims\n"
        "  - split clips\n"
        "  - pasted clips\n"
        "  - deleted events\n"
        "  - deleted tracks\n"
        "  - track mute / off / fader changes\n"
        "  - locator / range edits\n"
        "\n"
        "Undo will NOT automatically delete or restore external files on disk.\n"
        "\n"
        "For safety:\n"
        "  - recorded audio files in the project Audio/ folder remain on disk\n"
        "  - imported audio files copied into Audio/ remain on disk\n"
        "  - undoing a recording or import placement may remove the timeline event,\n"
        "    but not the underlying audio file\n"
        "  - cleanup of unused files will be a separate future command\n"
        "    (e.g. \"Clean Unused Media\")\n"
        "\n"
        "Note: undo/redo is not implemented yet. This dialog explains the planned behavior.");
}

} // namespace

namespace mini_daw_app_dialogs
{

void showAudioSettingsDialog(juce::Component& parent,
                             Transport& transport,
                             std::function<void()> updatePlayPauseButtonFromTransport,
                             RecorderService& recorder,
                             RecordingCoordinator& recordingCoordinator,
                             juce::AudioDeviceManager& deviceManager,
                             LatencySettingsStore& latencyStore,
                             PlaybackEngine& playbackEngine,
                             juce::Component::SafePointer<LatencySettingsView>& audioLatencySettingsWeakSlot)
{
    if (recorder.isRecording() || recordingCoordinator.isCountInActive())
    {
        juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::WarningIcon,
                                               "Audio Settings",
                                               "Audio settings cannot be changed while recording "
                                               "or count-in is active.");
        return;
    }
    if (transport.readPlaybackIntentForUi() == PlaybackIntent::Playing)
    {
        transport.requestPlaybackIntent(PlaybackIntent::Stopped);
        updatePlayPauseButtonFromTransport();
    }

    auto* body = new AudioSettingsDialogContent(deviceManager, latencyStore, playbackEngine);
    audioLatencySettingsWeakSlot = &body->getLatencyPane();
    body->getLatencyPane().syncFromStore();
    juce::DialogWindow::LaunchOptions opt;
    opt.content.setOwned(body);
    opt.dialogTitle = "Audio Settings";
    opt.dialogBackgroundColour
        = parent.getLookAndFeel().findColour(juce::ResizableWindow::backgroundColourId);
    opt.componentToCentreAround = &parent;
    opt.escapeKeyTriggersCloseButton = true;
    opt.useNativeTitleBar = true;
    opt.resizable = true;
    opt.launchAsync();
}

void showHelpMenuPopup(juce::Component& helpButtonAnchor,
                       juce::Component::SafePointer<juce::Component> menuOwnerLifetime,
                       std::function<void()> showUndoBehaviorDialog)
{
    juce::PopupMenu menu;
    constexpr int kUndoBehaviorMenuItemId = 1;
    menu.addItem(kUndoBehaviorMenuItemId, "Undo Behavior...");
    menu.showMenuAsync(
        juce::PopupMenu::Options().withTargetComponent(&helpButtonAnchor),
        [menuOwnerLifetime, showUndoBehaviorDialog, kUndoBehaviorMenuItemId](const int result) {
            if (menuOwnerLifetime == nullptr)
            {
                return;
            }
            if (result != kUndoBehaviorMenuItemId)
            {
                return;
            }
            showUndoBehaviorDialog();
        });
}

void showUndoBehaviorDialog()
{
    juce::AlertWindow::showMessageBoxAsync(
        juce::AlertWindow::InfoIcon, "Undo Behavior", undoBehaviorHelpBodyText());
}

} // namespace mini_daw_app_dialogs
