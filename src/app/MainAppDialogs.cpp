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

/// Scrollable read-only help page. The Undo Behavior text still fits an `AlertWindow`; a reference
/// page does not, and a message box cannot be scrolled or resized. Same `DialogWindow` shell as
/// Audio Settings, with a fixed-pitch editor so the text's column alignment survives.
class HelpPageDialogContent final : public juce::Component
{
public:
    explicit HelpPageDialogContent(const juce::String& body)
    {
        text_.setMultiLine(true, false);
        text_.setReadOnly(true);
        text_.setScrollbarsShown(true);
        text_.setCaretVisible(false);
        text_.setPopupMenuEnabled(true); // Copy works; read-only blocks the editing items.
        text_.setFont(juce::Font(juce::FontOptions(juce::Font::getDefaultMonospacedFontName(), 13.0f,
                                                  juce::Font::plain)));
        text_.setColour(juce::TextEditor::backgroundColourId, juce::Colour(0xff1b1b20));
        text_.setColour(juce::TextEditor::outlineColourId, juce::Colours::transparentBlack);
        text_.setText(body, false);
        text_.moveCaretToTop(false);
        addAndMakeVisible(text_);
        setSize(760, 620);
    }

    void resized() override { text_.setBounds(getLocalBounds().reduced(8)); }

private:
    juce::TextEditor text_;
};

[[nodiscard]] juce::String midiChannelsHelpBodyText()
{
    return juce::String(
        "MIDI CHANNELS IN DANIELSSONS AUDIO LAB\n"
        "======================================\n"
        "\n"
        "There are three different things called \"channel\", and keeping them apart explains\n"
        "everything else on this page:\n"
        "\n"
        "  Native channel        The MIDI channel stored inside the note itself (1-16).\n"
        "                        It is part of the note data, like pitch and velocity.\n"
        "  Track output channel  The instrument track's \"MIDI Channel\" setting in the\n"
        "                        Inspector: \"Any (Preserve)\" or a fixed 1-16.\n"
        "  Effective channel     What the plugin actually receives, after the track setting\n"
        "                        has been applied to the note.\n"
        "\n"
        "\n"
        "TRACK MIDI CHANNEL\n"
        "------------------\n"
        "\n"
        "  Any (Preserve)   Every note is sent on the channel it stores natively. A track can\n"
        "                   therefore send several different channels at once.\n"
        "  1-16             Every note from the track is sent on the channel you picked. The\n"
        "                   stored notes are NOT rewritten - only what is sent changes.\n"
        "\n"
        "Because a fixed channel does not touch the note data, it hides whatever mixture the\n"
        "notes contain. Switching back to \"Any (Preserve)\" reveals that mixture again.\n"
        "\n"
        "\n"
        "CREATING NOTES\n"
        "--------------\n"
        "\n"
        "  - A new ordinary instrument track starts on channel 1.\n"
        "  - A new Groove Agent SE track starts on channel 10.\n"
        "  - A note drawn while channel 2 is selected stores channel 2 natively.\n"
        "  - Changing the track to channel 3 does NOT rewrite that existing note.\n"
        "  - Notes drawn after the change store channel 3 natively.\n"
        "  - While the track output stays fixed to channel 3, every note - including the older\n"
        "    channel-2 ones - is nevertheless sent to the plugin on channel 3.\n"
        "  - Selecting \"Any (Preserve)\" makes each note use its own native channel again, so\n"
        "    the first group plays on 2 and the second on 3.\n"
        "\n"
        "The MIDI editor shows all of this for the current selection, in the toolbar:\n"
        "\n"
        "  Native Ch: 2 - 7 notes - Output Ch: 3 - Effective Ch: 3\n"
        "\n"
        "and for a selection that spans several stored channels:\n"
        "\n"
        "  Native Ch: Mixed (1, 2, 3) - 12 notes - Output: Preserve\n"
        "\n"
        "\n"
        "SEVERAL CHANNELS IN ONE TRACK\n"
        "-----------------------------\n"
        "\n"
        "One instrument track can hold notes on several native channels, which is how you drive\n"
        "a multi-timbral instrument from a single plugin instance.\n"
        "\n"
        "Example - VB3-II organ:\n"
        "\n"
        "  Upper manual   channel 1\n"
        "  Lower manual   channel 2\n"
        "  Pedalboard     channel 3\n"
        "\n"
        "Record or draw each part with the track set to the matching fixed channel, then set the\n"
        "track to \"Any (Preserve)\". VB3-II now receives all three native channels through one\n"
        "plugin instance, and each part plays on its own manual.\n"
        "\n"
        "\n"
        "MIDI TRACKS (MIDI TO)\n"
        "---------------------\n"
        "\n"
        "\"Add MIDI Track\" creates a track that holds MIDI clips but no plugin of its own.\n"
        "In the Inspector it has a \"MIDI To\" setting: pick which instrument track it plays\n"
        "through. Several MIDI tracks can point at the same instrument, so the organ example\n"
        "above can also be three separate arrangement tracks:\n"
        "\n"
        "  Upper  (MIDI track, channel 1)  --+\n"
        "  Lower  (MIDI track, channel 2)  --+--> VB3-II instrument track\n"
        "  Pedal  (MIDI track, channel 3)  --+\n"
        "\n"
        "Rules that keep this predictable:\n"
        "\n"
        "  - A MIDI track's own \"MIDI Channel\" setting works exactly as on instrument tracks\n"
        "    (\"Any (Preserve)\" or a fixed 1-16) and is applied before the notes reach the\n"
        "    destination instrument.\n"
        "  - With \"No destination\" the track is silent but stays fully editable.\n"
        "  - Deleting the destination instrument silences the MIDI tracks that pointed at it\n"
        "    (their \"MIDI To\" resets to \"No destination\"); their clips are untouched.\n"
        "  - Muting the MIDI track silences only that track's notes. Muting the destination\n"
        "    instrument silences everything it plays, including all routed MIDI tracks.\n"
        "  - When several tracks feed one instrument in the same moment, the instrument's own\n"
        "    clips are delivered first, then each MIDI track in the order the tracks appear in\n"
        "    the arrangement, top to bottom.\n"
        "  - Opening the MIDI editor on a MIDI track requires a destination: audition and drum\n"
        "    names come from the destination instrument.\n"
        "\n"
        "\n"
        "THE MIDI EDITOR: TITLE, STATUS, RANGE AND AUDITION\n"
        "--------------------------------------------------\n"
        "\n"
        "The editor window is titled \"MIDI Editor - <track name>\" and follows renames. The\n"
        "status line shows what the editor will actually sound through:\n"
        "\n"
        "  Instrument track   \"Instrument: VB3-II\", or \"No instrument loaded\".\n"
        "  MIDI track         \"MIDI To: Organ - VB3-II\" (destination and its plugin),\n"
        "                     \"No MIDI destination\" when MIDI To is None, or\n"
        "                     \"Organ: No instrument loaded\" when the destination has no plugin.\n"
        "\n"
        "Both Piano and Drum views cover the full MIDI range 0-127. Octaves follow the Cubase\n"
        "convention: MIDI 0 = C-2, MIDI 60 (middle C) = C3, MIDI 127 = G8. Piano and Drum are\n"
        "only display modes - neither restricts which pitches you may use, and switching modes\n"
        "never rewrites your notes.\n"
        "\n"
        "Auditioning (clicking to hear notes) always plays through the track's real route: an\n"
        "instrument track through its own plugin, a MIDI track through its MIDI To destination,\n"
        "with the normal channel rules applied (\"Any (Preserve)\" keeps the note's native\n"
        "channel; a fixed 1-16 uses that channel).\n"
        "\n"
        "  Arranged notes   Clicking a note rectangle plays a standard short audition (just\n"
        "                   under a second) regardless of the note's arranged length. Holding\n"
        "                   the mouse button keeps the note sounding until you release it.\n"
        "  Piano keys and   Follow the mouse exactly: Note On when you press, Note Off when\n"
        "  drum rows        you release, with no minimum length. They use the editor's current\n"
        "                   Vel and Off values.\n"
        "\n"
        "Audition notes are cleaned up automatically: closing the editor, changing MIDI To or\n"
        "the MIDI Channel, deleting the destination, losing window focus or pressing Stop all\n"
        "release any sounding audition notes without touching normal playback.\n"
        "\n"
        "\n"
        "MIDI EXPORT AND CHANNELS\n"
        "------------------------\n"
        "\n"
        "Exporting MIDI writes what you actually hear. Every channel-voice event (notes and\n"
        "controller changes) is written on the track's EFFECTIVE channel:\n"
        "\n"
        "  Any (Preserve)   the event's own native channel is written unchanged.\n"
        "  Fixed 1-16       every event is written on that fixed channel.\n"
        "\n"
        "The channel belongs to the MIDI SOURCE track being exported, never to the destination\n"
        "instrument. Export never rewrites your project: stored notes and controller points\n"
        "keep their native channels, and switching back to \"Any (Preserve)\" reveals them\n"
        "again. Meta events (tempo, time signature, names) are never remapped.\n"
        "\n"
        "\n"
        "MIDI CONTROLLER AUTOMATION (CC LANE)\n"
        "------------------------------------\n"
        "\n"
        "The MIDI editor has a controller lane below the velocity lane. When collapsed, a small\n"
        "\"CC\" handle sits at the bottom edge - click it to open the lane; drag its top edge to\n"
        "resize; drag it very small to collapse again.\n"
        "\n"
        "Click the lane's header to pick any controller CC0-127. Common controllers are named\n"
        "(CC1 Modulation, CC7 Volume, CC10 Pan, CC11 Expression, CC64 Sustain); the number is\n"
        "always shown. CC11 Expression is the default when you first open the lane - ideal for\n"
        "an organ swell pedal (VB3-II).\n"
        "\n"
        "  Insert    click empty lane space (snaps to the grid; value from the click height).\n"
        "  Select    click a point; Shift-click adds/removes; drag moves in time and value.\n"
        "  Delete    Delete/Backspace with points selected, or right-click > Delete.\n"
        "  Shape     right-click a point: Hold (step) or Linear (ramp) to the next point.\n"
        "\n"
        "All edits are undoable and never touch notes. Values are whole numbers 0-127.\n"
        "\n"
        "Playback evaluates the curve exactly: a point's own position plays its exact value,\n"
        "Hold keeps a value until the next point, Linear ramps between points as the minimal\n"
        "set of whole-value changes. Before your first point NOTHING is sent (there is no\n"
        "universal default value); after the last point its value holds.\n"
        "\n"
        "Controller state is \"chased\": starting playback, seeking, looping or rendering from\n"
        "the middle of a curve first sends the value the curve has at that position, before any\n"
        "note there. Auditioning an arranged note likewise applies its CC state first. MIDI\n"
        "controllers are sticky inside instruments - stopping, muting or rerouting a source\n"
        "does not send any \"reset\" value, because none exists generically.\n"
        "\n"
        "CC points follow the same channel rules as notes (native channel stored; effective\n"
        "channel sent/exported), route through MIDI To like notes, and are included in MIDI\n"
        "export (a CC at a note's start is always written before that Note On).\n"
        "\n"
        "\n"
        "CHANNEL 10\n"
        "----------\n"
        "\n"
        "MIDI channel 10 is conventionally used for drums. Danielssons Audio Lab picks it only\n"
        "when you explicitly create a Groove Agent SE track. It never guesses \"this is drums\"\n"
        "from a track name or a plugin name.\n"
        "\n"
        "\n"
        "OLDER PROJECTS\n"
        "--------------\n"
        "\n"
        "Older versions created every note on channel 10, whatever the instrument was. Projects\n"
        "from those versions may therefore contain melodic parts natively stored on channel 10.\n"
        "\n"
        "Such tracks load as \"Any (Preserve)\", so an existing project sounds exactly as before.\n"
        "If the instrument only listens on channel 1, selecting fixed channel 1 makes the track\n"
        "play correctly straight away - but it does not rewrite the stored notes. To change the\n"
        "note data itself, use the remap commands below.\n"
        "\n"
        "\n"
        "REMAPPING STORED CHANNELS (DESTRUCTIVE)\n"
        "---------------------------------------\n"
        "\n"
        "The MIDI editor toolbar has a \"Ch...\" button with two commands:\n"
        "\n"
        "  Remap Selected Notes to Track Channel\n"
        "      Rewrites the native channel of the selected notes only.\n"
        "\n"
        "  Remap All Notes to Track Channel...\n"
        "      Rewrites the native channel of every note in every clip on that track.\n"
        "\n"
        "Both commands require the track to have a fixed channel 1-16; with \"Any (Preserve)\"\n"
        "there is no single target channel, so they are greyed out.\n"
        "\n"
        "These commands change the note DATA. Nothing else is touched: position, length, pitch,\n"
        "velocity, which clip a note belongs to, your selection, and the track's own output\n"
        "setting all stay as they were. Both commands are a single undo step (Ctrl+Z).\n"
        "\n"
        "Typical clean-up of an old melodic track:\n"
        "\n"
        "  1. Open the track in the MIDI editor.\n"
        "  2. Set the track's MIDI Channel to 1 in the Inspector.\n"
        "  3. Ch... > Remap All Notes to Track Channel.\n"
        "  4. The notes now store channel 1 natively.\n"
        "  5. Switching the track back to \"Any (Preserve)\" keeps them playing on channel 1.\n"
        "\n"
        "If a remap would put two overlapping notes on the same pitch AND channel - which this\n"
        "editor treats as one single note - the whole command is refused and nothing changes.\n"
        "Move or shorten the stacked notes first.\n"
        "\n"
        "\n"
        "EXPORT AND MIXDOWN\n"
        "------------------\n"
        "\n"
        "Audio mixdown uses the effective channel, exactly like normal playback, so an exported\n"
        "audio file sounds like what you hear.\n"
        "\n"
        "\"Export MIDI...\" in the MIDI editor currently writes the NATIVE channels stored in the\n"
        "notes. With \"Any (Preserve)\" that is the same thing as the effective channel. With a\n"
        "fixed track channel it is not: the exported file keeps the original stored channels\n"
        "rather than the channel you hear. Use \"Remap All Notes to Track Channel\" first if you\n"
        "want the exported file to match playback.");
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
                       std::function<void()> showUndoBehaviorDialog,
                       std::function<void()> showMidiChannelsDialog)
{
    juce::PopupMenu menu;
    constexpr int kUndoBehaviorMenuItemId = 1;
    constexpr int kMidiChannelsMenuItemId = 2;
    menu.addItem(kUndoBehaviorMenuItemId, "Undo Behavior...");
    menu.addItem(kMidiChannelsMenuItemId, "MIDI Channels...");
    menu.showMenuAsync(
        juce::PopupMenu::Options().withTargetComponent(&helpButtonAnchor),
        [menuOwnerLifetime, showUndoBehaviorDialog, showMidiChannelsDialog](const int result) {
            if (menuOwnerLifetime == nullptr)
            {
                return;
            }
            if (result == kUndoBehaviorMenuItemId && showUndoBehaviorDialog != nullptr)
            {
                showUndoBehaviorDialog();
            }
            else if (result == kMidiChannelsMenuItemId && showMidiChannelsDialog != nullptr)
            {
                showMidiChannelsDialog();
            }
        });
}

void showUndoBehaviorDialog()
{
    juce::AlertWindow::showMessageBoxAsync(
        juce::AlertWindow::InfoIcon, "Undo Behavior", undoBehaviorHelpBodyText());
}

void showMidiChannelsHelpDialog(juce::Component& parent)
{
    juce::DialogWindow::LaunchOptions opt;
    opt.content.setOwned(new HelpPageDialogContent(midiChannelsHelpBodyText()));
    opt.dialogTitle = "Help: MIDI Channels";
    opt.dialogBackgroundColour
        = parent.getLookAndFeel().findColour(juce::ResizableWindow::backgroundColourId);
    opt.componentToCentreAround = &parent;
    opt.escapeKeyTriggersCloseButton = true;
    opt.useNativeTitleBar = true;
    opt.resizable = true;
    opt.launchAsync();
}

} // namespace mini_daw_app_dialogs
