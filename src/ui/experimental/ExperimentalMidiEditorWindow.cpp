#include "ExperimentalMidiEditorWindow.h"
#include "ExperimentalMidiImport.h"
#include "io/InstrumentMidiClipExport.h"
#include "ExperimentalMidiPattern.h"
#include "ExperimentalMidiPatternPlayer.h"
#include "ExperimentalPianoRollView.h"
#include "instruments/InstrumentTrackController.h"
#include "plugins/ExperimentalInstrumentHost.h"

#include "domain/Session.h"
#include "transport/Transport.h"
#include "ui/TimelineViewportModel.h"
#include "ui/TransportShortcutKeys.h"

#include <juce_audio_devices/juce_audio_devices.h>

#include <algorithm>
#include <cmath>

namespace
{
    constexpr int kToolbarH = 40;
    constexpr int kInitialEditorWidth = 1100;
    constexpr int kInitialEditorHeight = 760;

    [[nodiscard]] juce::String ptrToLog(const void* p) noexcept
    {
        return p != nullptr ? juce::String::formatted("%p", p) : juce::String("null");
    }
} // namespace

class ExperimentalMidiEditorWindow::Body final : public juce::Component, private juce::Timer
{
    friend class ExperimentalMidiEditorWindow;

public:
    explicit Body(ExperimentalInstrumentHost& hostIn)
        : host_(hostIn)
        , player_(std::make_unique<ExperimentalMidiPatternPlayer>(hostIn, pattern_))
    {
        pattern_.numSteps = 16;
        pattern_.stepDenom = 16;
        pattern_.bpm = 110.0;
        pattern_.loop = true;

        player_->setPlaybackUiCallback([this] { updateDebugStopButtonState(); });

        addAndMakeVisible(viewport_);

        addAndMakeVisible(transportPlayButton_);
        transportPlayButton_.setButtonText("Play");
        transportPlayButton_.onClick = [this] {
            if (transportCommands_.onTogglePlayPause)
            {
                transportCommands_.onTogglePlayPause();
            }
        };

        addAndMakeVisible(transportStopButton_);
        transportStopButton_.setButtonText("Stop");
        transportStopButton_.onClick = [this] {
            if (transportCommands_.onStop)
            {
                transportCommands_.onStop();
            }
        };

        addAndMakeVisible(cycleToggleButton_);
        cycleToggleButton_.setButtonText("Cycle: Off");
        cycleToggleButton_.setTooltip("Toggle loop/cycle (upper half of MIDI roll ruler also toggles).");
        cycleToggleButton_.onClick = [this] {
            if (transportCommands_.onToggleCycle)
            {
                transportCommands_.onToggleCycle();
            }
        };

        addAndMakeVisible(recordButton_);
        recordButton_.setButtonText("Record");
        recordButton_.setTooltip("Record armed audio track (same as main window; uses numpad * or * key).");
        recordButton_.onClick = [this] {
            if (transportCommands_.onToggleRecord)
            {
                transportCommands_.onToggleRecord();
            }
        };
        recordButton_.setEnabled(false);

        transportPlayButton_.setEnabled(false);
        transportStopButton_.setEnabled(false);
        cycleToggleButton_.setEnabled(false);

        addAndMakeVisible(debugPreviewButton_);
        debugPreviewButton_.setButtonText("Debug Preview");
        debugPreviewButton_.setTooltip("Local pattern preview only; does not start main transport.");
        debugPreviewButton_.onClick = [this] {
            player_->startPlayback();
        };

        addAndMakeVisible(debugStopButton_);
        debugStopButton_.setButtonText("Preview Stop");
        debugStopButton_.setEnabled(false);
        debugStopButton_.onClick = [this] {
            player_->stopPlayback("user");
        };

        addAndMakeVisible(bpmLabel_);
        bpmLabel_.setText("BPM", juce::dontSendNotification);
        bpmLabel_.setJustificationType(juce::Justification::centredRight);
        bpmLabel_.setColour(juce::Label::textColourId, juce::Colours::lightgrey);

        addAndMakeVisible(bpmSlider_);
        bpmSlider_.setRange(60.0, 200.0, 0.1);
        bpmSlider_.setValue(110.0);
        bpmSlider_.setTextBoxStyle(juce::Slider::TextBoxLeft, false, 56, 22);
        bpmSlider_.onValueChange = [this] {
            activePattern().bpm = bpmSlider_.getValue();
            if (externalPattern_ != nullptr && instrumentTrackForClipBind_ != nullptr
                && activePattern().usesTimelineNotes())
            {
                instrumentTrackForClipBind_->notifyClipExperimentalMusicalTimingChanged();
            }
            if (auto* rv = dynamic_cast<ExperimentalPianoRollView*>(viewport_.getViewedComponent()))
            {
                rv->repaint();
            }
        };

        addAndMakeVisible(importButton_);
        importButton_.setButtonText("Import MIDI...");
        importButton_.onClick = [this] { beginImportMidi(); };

        addAndMakeVisible(exportButton_);
        exportButton_.setButtonText("Export MIDI...");
        exportButton_.onClick = [this] { beginExportMidi(); };

        addAndMakeVisible(snapLabel_);
        snapLabel_.setText("Snap", juce::dontSendNotification);
        snapLabel_.setJustificationType(juce::Justification::centredRight);
        snapLabel_.setColour(juce::Label::textColourId, juce::Colours::lightgrey);

        addAndMakeVisible(snapBox_);
        snapBox_.addItem("Off", 1);
        snapBox_.addItem("1/8", 2);
        snapBox_.addItem("1/16", 3);
        snapBox_.addItem("1/32", 4);
        snapBox_.setSelectedId(1, juce::dontSendNotification);
        snapBox_.onChange = [this] { pushSnapToRoll(); };

        addAndMakeVisible(displayLabel_);
        displayLabel_.setText("Display", juce::dontSendNotification);
        displayLabel_.setJustificationType(juce::Justification::centredRight);
        displayLabel_.setColour(juce::Label::textColourId, juce::Colours::lightgrey);

        addAndMakeVisible(displayBox_);
        displayBox_.addItem("Hits", 1);
        displayBox_.addItem("Bars", 2);
        displayBox_.setSelectedId(1, juce::dontSendNotification);
        displayBox_.onChange = [this] { pushDisplayToRoll(); };

        addAndMakeVisible(stepsLabel_);
        stepsLabel_.setText("Steps", juce::dontSendNotification);
        stepsLabel_.setJustificationType(juce::Justification::centredRight);
        stepsLabel_.setColour(juce::Label::textColourId, juce::Colours::lightgrey);

        addAndMakeVisible(stepsBox_);
        stepsBox_.addItem("16 steps", 1);
        stepsBox_.addItem("32 steps", 2);
        stepsBox_.setSelectedId(1, juce::dontSendNotification);
        stepsBox_.onChange = [this] {
            if (activePattern().usesTimelineNotes())
            {
                return;
            }
            const int id = stepsBox_.getSelectedId();
            const int newSteps = (id == 2) ? 32 : 16;
            if (newSteps == activePattern().numSteps)
            {
                return;
            }
            activePattern().numSteps = newSteps;
            activePattern().notes.erase(
                std::remove_if(
                    activePattern().notes.begin(),
                    activePattern().notes.end(),
                    [newSteps](const PrototypeMidiNote& n) { return n.step >= newSteps; }),
                activePattern().notes.end());
            if (boundTimelineClip_ != nullptr && instrumentTrackForClipBind_ != nullptr)
            {
                instrumentTrackForClipBind_->recomputeLockedClipLengthFromPatternGrid(*boundTimelineClip_);
                instrumentTrackForClipBind_->sendChangeMessage();
            }
            if (auto* rv = dynamic_cast<ExperimentalPianoRollView*>(viewport_.getViewedComponent()))
            {
                rv->seedOrResetViewport();
                rv->repaint();
            }
        };

        addAndMakeVisible(followPlayheadToggle_);
        followPlayheadToggle_.setButtonText("Follow");
        followPlayheadToggle_.setClickingTogglesState(true);
        followPlayheadToggle_.onClick = [this] {
            if (auto* rv = dynamic_cast<ExperimentalPianoRollView*>(viewport_.getViewedComponent()))
            {
                rv->setFollowPlayheadEnabled(followPlayheadToggle_.getToggleState());
            }
        };

        addAndMakeVisible(modeLabel_);
        modeLabel_.setFont(juce::FontOptions(11.0f));
        modeLabel_.setJustificationType(juce::Justification::centredLeft);
        modeLabel_.setColour(juce::Label::textColourId, juce::Colour(0xffc8c8d8));

        rebuildRollViewOnly();

        ExperimentalMidiPatternPlayer::writeMidiEditorLogLine(
            "midi-editor: window opened steps=" + juce::String(pattern_.numSteps) + " bpm="
            + juce::String(pattern_.bpm, 2));

        ExperimentalMidiPatternPlayer::writeMidiEditorLogLine(
            juce::String("midi-editor: opened hasInstrument=") + (hostIn.hasInstrument() ? "true" : "false"));

        syncInstrumentUiFromHost();
    }

    ~Body() override
    {
        stopTimer();
        player_->stopPlayback("window-closed");
    }

    void stopForHostUnload()
    {
        player_->stopPlayback("instrument-unloaded");
    }

    void prepareBeforeHostUnload()
    {
        player_->stopPlayback("instrument-unloaded");
    }

    void syncInstrumentUiFromHost()
    {
        applyInstrumentUiState();
        refreshTimelineSampleRateOnTrack();
        if (auto* rv = dynamic_cast<ExperimentalPianoRollView*>(viewport_.getViewedComponent()))
        {
            rv->setSessionTimelineContext(
                boundTimelineClip_,
                sessionForRoll_,
                transportForRoll_,
                deviceManagerForRoll_,
                instrumentTrackForClipBind_,
                mainTimelineViewportForRoll_);
            pushTransportGestureBlockToRoll();
        }
    }

    void bindExternal(ExperimentalMidiPattern* p,
                      InstrumentMidiClip* timelineClip,
                      InstrumentTrackController* trackForClipGate,
                      Session* session,
                      Transport* transport,
                      juce::AudioDeviceManager* deviceManager,
                      const TimelineViewportModel* mainTimelineViewport)
    {
        InstrumentTrackController* const gatePtr = (p != nullptr) ? trackForClipGate : nullptr;
        if (externalPattern_ == p && instrumentTrackForClipBind_ == gatePtr && boundTimelineClip_ == timelineClip
            && sessionForRoll_ == session && transportForRoll_ == transport && deviceManagerForRoll_ == deviceManager
            && mainTimelineViewportForRoll_ == mainTimelineViewport)
        {
            syncSlidersFromActivePattern();
            syncInstrumentUiFromHost();
            return;
        }

        if (player_ != nullptr)
        {
            player_->stopPlayback("rebind");
        }

        if (auto* oldRoll = dynamic_cast<ExperimentalPianoRollView*>(viewport_.getViewedComponent()))
        {
            if (boundTimelineClip_ != nullptr)
            {
                oldRoll->syncViewportToBoundClip();
            }
        }

        externalPattern_ = p;
        instrumentTrackForClipBind_ = gatePtr;
        boundTimelineClip_ = timelineClip;
        sessionForRoll_ = session;
        transportForRoll_ = transport;
        deviceManagerForRoll_ = deviceManager;
        mainTimelineViewportForRoll_ = mainTimelineViewport;
        if (p != nullptr && timelineClip != nullptr)
        {
            ExperimentalMidiPatternPlayer::writeMidiEditorLogLine(
                "midi-editor: bindExternal patternPtr=" + ptrToLog(p) + " noteCount="
                + juce::String((int)p->notes.size()) + " clipStart=" + juce::String(timelineClip->startSamples)
                + " clipLength=" + juce::String(timelineClip->lengthSamples));
        }
        rebuildPlayerAndRoll();
        syncSlidersFromActivePattern();
        syncInstrumentUiFromHost();
    }

    void refreshTimelineSampleRateOnTrack()
    {
        double sr = 48000.0;
        if (deviceManagerForRoll_ != nullptr)
        {
            if (juce::AudioIODevice* d = deviceManagerForRoll_->getCurrentAudioDevice())
            {
                const double r = d->getCurrentSampleRate();
                if (r > 0.0 && std::isfinite(r))
                {
                    sr = r;
                }
            }
        }
        if (instrumentTrackForClipBind_ != nullptr)
        {
            instrumentTrackForClipBind_->setTimelineSampleRate(sr);
        }
    }

    void unbindExternal()
    {
        if (externalPattern_ == nullptr && boundTimelineClip_ == nullptr)
        {
            return;
        }
        if (player_ != nullptr)
        {
            player_->stopPlayback("rebind");
        }
        if (auto* rv = dynamic_cast<ExperimentalPianoRollView*>(viewport_.getViewedComponent()))
        {
            if (boundTimelineClip_ != nullptr)
            {
                rv->syncViewportToBoundClip();
            }
        }
        externalPattern_ = nullptr;
        instrumentTrackForClipBind_ = nullptr;
        boundTimelineClip_ = nullptr;
        sessionForRoll_ = nullptr;
        transportForRoll_ = nullptr;
        deviceManagerForRoll_ = nullptr;
        mainTimelineViewportForRoll_ = nullptr;
        rebuildPlayerAndRoll();
        syncSlidersFromActivePattern();
        syncInstrumentUiFromHost();
    }

    void snapshotOpenClipViewportFromRoll() noexcept
    {
        if (auto* rv = dynamic_cast<ExperimentalPianoRollView*>(viewport_.getViewedComponent()))
        {
            if (boundTimelineClip_ != nullptr)
            {
                rv->syncViewportToBoundClip();
            }
        }
    }

    [[nodiscard]] ExperimentalMidiPattern& activePattern() noexcept
    {
        return externalPattern_ != nullptr ? *externalPattern_ : pattern_;
    }

    [[nodiscard]] const ExperimentalMidiPattern& activePattern() const noexcept
    {
        return externalPattern_ != nullptr ? *externalPattern_ : pattern_;
    }

    [[nodiscard]] bool editorInstrumentGate() const noexcept
    {
        if (externalPattern_ != nullptr && instrumentTrackForClipBind_ != nullptr)
        {
            const auto* tr = instrumentTrackForClipBind_;
            return tr->isInstrumentLoaded() && tr->isPowerOn() && !tr->isMuted();
        }
        return host_.hasInstrument();
    }

    void applyInstrumentUiState()
    {
        const bool clipBound = externalPattern_ != nullptr && instrumentTrackForClipBind_ != nullptr;
        const bool canPlayPattern = editorInstrumentGate();
        const juce::String instrumentName = host_.getInstrumentNameForUi();
        const juce::String requiredKitForUi
            = clipBound && instrumentTrackForClipBind_ != nullptr
                  ? instrumentTrackForClipBind_->getRequiredKitName()
                  : juce::String();
        const bool changed = !instrumentUiInitialized_ || (canPlayPattern != lastPlayGate_)
                             || (instrumentName != lastInstrumentName_)
                             || (requiredKitForUi != lastRequiredKitName_);
        instrumentUiInitialized_ = true;
        lastPlayGate_ = canPlayPattern;
        lastInstrumentName_ = instrumentName;
        lastRequiredKitName_ = requiredKitForUi;

        debugPreviewButton_.setEnabled(canPlayPattern);
        const bool clipTimelineBound =
            externalPattern_ != nullptr && boundTimelineClip_ != nullptr;
        importButton_.setEnabled(clipTimelineBound);
        exportButton_.setEnabled(clipTimelineBound);

        juce::String instPart;
        if (!clipBound)
        {
            instPart = host_.hasInstrument()
                           ? (instrumentName.isNotEmpty() ? (juce::String("Instrument: ") + instrumentName)
                                                          : juce::String("Instrument: (loaded)"))
                           : juce::String("No instrument loaded");
        }
        else
        {
            const auto* tr = instrumentTrackForClipBind_;
            if (tr == nullptr || !tr->isInstrumentLoaded())
            {
                instPart = juce::String("No instrument loaded");
            }
            else if (!tr->isPowerOn())
            {
                instPart = juce::String("Instrument track powered off (playback disabled)");
            }
            else if (tr->isMuted())
            {
                instPart = juce::String("Instrument track muted (playback disabled)");
            }
            else
            {
                instPart = instrumentName.isNotEmpty() ? (juce::String("Instrument: ") + instrumentName)
                                                        : juce::String("Instrument: (loaded)");
            }
        }

        juce::String kitLine;
        if (clipBound)
        {
            const auto* trk = instrumentTrackForClipBind_;
            if (trk != nullptr && trk->getRequiredKitName().isNotEmpty())
            {
                if (!trk->isInstrumentLoaded())
                {
                    kitLine = "\nRequired kit: " + trk->getRequiredKitName() + " (Groove Agent not loaded)";
                }
                else
                {
                    kitLine = "\nLoad kit in Groove Agent: " + trk->getRequiredKitName();
                }
            }
        }

        const juce::Colour labelColour =
            canPlayPattern ? juce::Colour(0xffc8c8d8) : juce::Colour(0xffffaa88);

        modeLabel_.setText(
            juce::String("I3f: timeline MIDI — Import MIDI for tick-accurate clips (960 PPQ internal). ")
                + "When the clip has timeline notes they drive transport; step grid is for empty/legacy clips only. "
                  "Snap applies to click‑to‑add timing. Preview uses step timing; main Play uses the timeline.\n"
                + instPart + "\n"
                + "Timing: ~4 ms message timer; not sample-accurate." + kitLine,
            juce::dontSendNotification);
        modeLabel_.setColour(juce::Label::textColourId, labelColour);

        updateDebugStopButtonState();

        if (!changed)
        {
            return;
        }

        ExperimentalMidiPatternPlayer::writeMidiEditorLogLine(
            juce::String("midi-editor: instrument state changed canPlay=") + (canPlayPattern ? "true" : "false")
            + " name=\"" + instrumentName + "\"");
        if (!canPlayPattern)
        {
            ExperimentalMidiPatternPlayer::writeMidiEditorLogLine(
                "midi-editor: instrument unavailable, playback disabled");
        }
        else
        {
            ExperimentalMidiPatternPlayer::writeMidiEditorLogLine(
                "midi-editor: instrument available name=\"" + instrumentName + "\"");
        }
    }

    void updateDebugStopButtonState()
    {
        debugStopButton_.setEnabled(editorInstrumentGate() && player_ != nullptr && player_->isPlaying());
    }

    void resized() override
    {
        auto a = getLocalBounds();
        auto toolbar = a.removeFromTop(kToolbarH);
        toolbar.reduce(6, 4);
        transportPlayButton_.setBounds(toolbar.removeFromLeft(58).reduced(0, 2));
        transportStopButton_.setBounds(toolbar.removeFromLeft(52).reduced(0, 2));
        cycleToggleButton_.setBounds(toolbar.removeFromLeft(56).reduced(0, 2));
        recordButton_.setBounds(toolbar.removeFromLeft(72).reduced(0, 2));
        importButton_.setBounds(toolbar.removeFromLeft(92).reduced(0, 2));
        exportButton_.setBounds(toolbar.removeFromLeft(96).reduced(0, 2));
        snapLabel_.setBounds(toolbar.removeFromLeft(40).reduced(0, 4));
        snapBox_.setBounds(toolbar.removeFromLeft(76).reduced(0, 2));
        displayLabel_.setBounds(toolbar.removeFromLeft(52).reduced(0, 4));
        displayBox_.setBounds(toolbar.removeFromLeft(72).reduced(0, 2));
        stepsLabel_.setBounds(toolbar.removeFromLeft(44).reduced(0, 4));
        stepsBox_.setBounds(toolbar.removeFromLeft(100).reduced(0, 2));
        followPlayheadToggle_.setBounds(toolbar.removeFromLeft(72).reduced(0, 2));
        bpmLabel_.setBounds(toolbar.removeFromLeft(36).reduced(0, 4));
        bpmSlider_.setBounds(toolbar.removeFromLeft(132).reduced(0, 2));
        debugPreviewButton_.setBounds(toolbar.removeFromLeft(98).reduced(0, 2));
        debugStopButton_.setBounds(toolbar.removeFromLeft(90).reduced(0, 2));
        modeLabel_.setBounds(toolbar.reduced(8, 0));

        // Lay out the viewport first, then size the roll to match the viewport's *content budget*.
        //
        // JUCE note: `Viewport::getViewWidth/Height()` return `lastVisibleArea` — the intersection of
        // what you can see with the *current* child size. If the piano roll is still narrow, that value
        // stays pegged to the child (e.g. 400px), so `jmax(400, getViewWidth())` never grows and the
        // editor leaves a grey gutter. `getMaximumVisibleWidth/Height()` is the client area minus
        // scrollbar chrome; use that to size the viewed component so it fills the window.
        viewport_.setBounds(a);

        const int rollMinH = ExperimentalPianoRollView::kRulerHeight
                             + (ExperimentalPianoRollView::kPitchHigh - ExperimentalPianoRollView::kPitchLow + 1)
                                   * ExperimentalPianoRollView::kRowHeight;
        if (auto* rv = dynamic_cast<ExperimentalPianoRollView*>(viewport_.getViewedComponent()))
        {
            const int outerW = juce::jmax(1, viewport_.getWidth());
            const int outerH = juce::jmax(1, viewport_.getHeight());

            int budgetW = viewport_.getMaximumVisibleWidth();
            int budgetH = viewport_.getMaximumVisibleHeight();
            if (budgetW < 1)
            {
                budgetW = outerW;
            }
            if (budgetH < 1)
            {
                budgetH = outerH;
            }

            const int rw = juce::jmax(1, budgetW);
            const int rh = juce::jmax(rollMinH, budgetH);
            rv->setSize(rw, rh);
            viewport_.setViewPosition(0, 0);

            ExperimentalMidiPatternPlayer::writeMidiEditorLogLine(
                "midi-editor: resized viewportBounds=" + viewport_.getBounds().toString() + " maxVisible="
                + juce::String(budgetW) + "x" + juce::String(budgetH) + " viewArea="
                + juce::String(viewport_.getViewWidth()) + "x" + juce::String(viewport_.getViewHeight())
                + " viewPos=" + viewport_.getViewPosition().toString() + " outer=" + juce::String(outerW) + "x"
                + juce::String(outerH) + " rollBounds=" + rv->getBounds().toString() + " boundExternal="
                + juce::String(externalPattern_ != nullptr ? "true" : "false") + " patternPtr="
                + ptrToLog(&activePattern()) + " externalPatternPtr=" + ptrToLog(externalPattern_)
                + " noteCount=" + juce::String((int)activePattern().notes.size()) + " visibleStart="
                + juce::String(rv->getViewportVisibleStartSamples()) + " spp="
                + juce::String(rv->getViewportSamplesPerPixel()));
        }
    }

private:
    void timerCallback() override;
    void setTransportCommands(ExperimentalMidiTransportCommands commands);
    void pushTransportGestureBlockToRoll();
    [[nodiscard]] bool handleTopLevelShortcut(const juce::KeyPress& key);

    void syncStepsAndSnapUiForPattern();
    void pushSnapToRoll();
    void pushDisplayToRoll();
    void beginImportMidi();
    void launchMidiFileChooserAfterConfirm();
    void applyMidiImportResult(const ExperimentalMidiImportResult& result);
    void beginExportMidi();
    void launchMidiExportFileChooser();

    InstrumentTrackController* instrumentTrackForClipBind_ = nullptr;
    InstrumentMidiClip* boundTimelineClip_ = nullptr;
    Session* sessionForRoll_ = nullptr;
    Transport* transportForRoll_ = nullptr;
    juce::AudioDeviceManager* deviceManagerForRoll_ = nullptr;
    const TimelineViewportModel* mainTimelineViewportForRoll_ = nullptr;

    void syncSlidersFromActivePattern()
    {
        bpmSlider_.setValue(activePattern().bpm, juce::dontSendNotification);
        stepsBox_.setSelectedId(activePattern().numSteps == 32 ? 2 : 1, juce::dontSendNotification);
        syncStepsAndSnapUiForPattern();
    }

    void rebuildPlayerAndRoll()
    {
        player_ = std::make_unique<ExperimentalMidiPatternPlayer>(host_, activePattern());
        player_->setPlaybackUiCallback([this] { updateDebugStopButtonState(); });
        player_->setPlaybackAllowed([this] { return editorInstrumentGate(); });
        rebuildRollViewOnly();
        resized();
    }

    void rebuildRollViewOnly()
    {
        ExperimentalMidiPattern& ap = activePattern();
        ExperimentalMidiPatternPlayer::writeMidiEditorLogLine(
            "midi-editor: rebuild roll begin activePatternPtr=" + ptrToLog(&ap) + " externalPatternPtr="
            + ptrToLog(externalPattern_) + " internalPatternPtr=" + ptrToLog(&pattern_)
            + " noteCount=" + juce::String((int)ap.notes.size()));

        auto* roll = new ExperimentalPianoRollView(ap, player_.get());
        roll->setSessionTimelineContext(boundTimelineClip_,
                                        sessionForRoll_,
                                        transportForRoll_,
                                        deviceManagerForRoll_,
                                        instrumentTrackForClipBind_,
                                        mainTimelineViewportForRoll_);
        followPlayheadToggle_.setToggleState(
            boundTimelineClip_ != nullptr && boundTimelineClip_->midiRollFollowEnabled,
            juce::dontSendNotification);
        roll->setFollowPlayheadEnabled(followPlayheadToggle_.getToggleState());
        viewport_.setViewedComponent(roll, true);
        roll->setMusicalSnapComboId(snapBox_.getSelectedId());
        roll->setTimelineNotesDisplayComboId(displayBox_.getSelectedId());
        pushTransportGestureBlockToRoll();

        ExperimentalMidiPatternPlayer::writeMidiEditorLogLine(
            "midi-editor: setViewedComponent rollPtr=" + ptrToLog(roll) + " width="
            + juce::String(roll->getWidth()) + " height=" + juce::String(roll->getHeight()));
    }

    ExperimentalInstrumentHost& host_;
    ExperimentalMidiPattern pattern_;
    ExperimentalMidiPattern* externalPattern_ = nullptr;
    std::unique_ptr<ExperimentalMidiPatternPlayer> player_;

    bool instrumentUiInitialized_ = false;
    bool lastPlayGate_ = false;
    juce::String lastInstrumentName_;
    juce::String lastRequiredKitName_;

    juce::TextButton transportPlayButton_;
    juce::TextButton transportStopButton_;
    juce::TextButton cycleToggleButton_;
    juce::TextButton recordButton_;
    juce::TextButton debugPreviewButton_;
    juce::TextButton debugStopButton_;
    juce::TextButton importButton_;
    juce::TextButton exportButton_;
    juce::Label snapLabel_;
    juce::ComboBox snapBox_;
    juce::Label displayLabel_;
    juce::ComboBox displayBox_;
    juce::Label bpmLabel_;
    juce::Slider bpmSlider_;
    juce::Label stepsLabel_;
    juce::ComboBox stepsBox_;
    juce::TextButton followPlayheadToggle_;
    juce::Label modeLabel_;
    juce::Viewport viewport_;

    std::unique_ptr<juce::FileChooser> midiImportChooser_;
    std::unique_ptr<juce::FileChooser> midiExportChooser_;

    ExperimentalMidiTransportCommands transportCommands_{};
};

void ExperimentalMidiEditorWindow::Body::timerCallback()
{
    if (transportCommands_.transport == nullptr)
    {
        return;
    }
    const bool playing =
        transportCommands_.transport->readPlaybackIntentForUi() == PlaybackIntent::Playing;
    const juce::String want = playing ? "Pause" : "Play";
    if (transportPlayButton_.getButtonText() != want)
    {
        transportPlayButton_.setButtonText(want);
    }
    const bool cyc = transportCommands_.transport->readCycleEnabledForUi();
    cycleToggleButton_.setButtonText(cyc ? "Cycle: On" : "Cycle: Off");
}

void ExperimentalMidiEditorWindow::Body::setTransportCommands(ExperimentalMidiTransportCommands commands)
{
    transportCommands_ = std::move(commands);
    const bool ok = transportCommands_.transport != nullptr;
    transportPlayButton_.setEnabled(ok && static_cast<bool>(transportCommands_.onTogglePlayPause));
    transportStopButton_.setEnabled(ok && static_cast<bool>(transportCommands_.onStop));
    cycleToggleButton_.setEnabled(ok && static_cast<bool>(transportCommands_.onToggleCycle));
    recordButton_.setEnabled(ok && static_cast<bool>(transportCommands_.onToggleRecord));
    pushTransportGestureBlockToRoll();
    if (ok)
    {
        timerCallback();
        startTimerHz(10);
    }
    else
    {
        stopTimer();
    }
}

void ExperimentalMidiEditorWindow::Body::pushTransportGestureBlockToRoll()
{
    if (auto* rv = dynamic_cast<ExperimentalPianoRollView*>(viewport_.getViewedComponent()))
    {
        rv->setTransportGestureBlockPredicate(transportCommands_.isUiInputBlockedByRecording);
    }
}

[[nodiscard]] bool ExperimentalMidiEditorWindow::Body::handleTopLevelShortcut(const juce::KeyPress& key)
{
    if (dynamic_cast<juce::TextEditor*>(juce::Component::getCurrentlyFocusedComponent()) != nullptr)
    {
        return false;
    }
    if (midi_transport_shortcuts::isRecordToggleShortcut(key))
    {
        if (transportCommands_.onToggleRecord)
        {
            transportCommands_.onToggleRecord();
            return true;
        }
        return false;
    }
    if (midi_transport_shortcuts::isSpacePlayPauseShortcut(key))
    {
        if (transportCommands_.onTogglePlayPause)
        {
            transportCommands_.onTogglePlayPause();
            return true;
        }
        return false;
    }
    return false;
}

void ExperimentalMidiEditorWindow::Body::syncStepsAndSnapUiForPattern()
{
    const bool tl = activePattern().usesTimelineNotes();
    stepsBox_.setEnabled(!tl);
    stepsLabel_.setEnabled(!tl);
}

void ExperimentalMidiEditorWindow::Body::pushSnapToRoll()
{
    if (auto* rv = dynamic_cast<ExperimentalPianoRollView*>(viewport_.getViewedComponent()))
    {
        rv->setMusicalSnapComboId(snapBox_.getSelectedId());
    }
}

void ExperimentalMidiEditorWindow::Body::pushDisplayToRoll()
{
    if (auto* rv = dynamic_cast<ExperimentalPianoRollView*>(viewport_.getViewedComponent()))
    {
        rv->setTimelineNotesDisplayComboId(displayBox_.getSelectedId());
    }
}

void ExperimentalMidiEditorWindow::Body::beginImportMidi()
{
    if (externalPattern_ == nullptr || boundTimelineClip_ == nullptr)
    {
        juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::WarningIcon,
                                               "Import MIDI",
                                               "Open this editor from a clip on the instrument track to import MIDI.");
        return;
    }

    const bool haveContent =
        activePattern().usesTimelineNotes() || !activePattern().notes.empty();
    if (haveContent)
    {
        const int ok = juce::AlertWindow::showOkCancelBox(
            juce::AlertWindow::WarningIcon,
            "Import MIDI",
            "Replace this clip's pattern with the imported MIDI?\n\n"
            "Existing step and timeline notes will be cleared.",
            "Replace",
            "Cancel",
            nullptr,
            nullptr);
        if (ok == 0)
        {
            return;
        }
    }

    launchMidiFileChooserAfterConfirm();
}

void ExperimentalMidiEditorWindow::Body::launchMidiFileChooserAfterConfirm()
{
    midiImportChooser_ = std::make_unique<juce::FileChooser>(
        "Import MIDI", juce::File{}, "*.mid;*.midi", true, false, this);

    const auto flags = juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles;

    const juce::Component::SafePointer<Body> safeThis(this);
    midiImportChooser_->launchAsync(flags, [safeThis](const juce::FileChooser& fc) {
        if (safeThis == nullptr)
        {
            return;
        }
        const juce::File file = fc.getResult();
        safeThis->midiImportChooser_.reset();

        if (!file.existsAsFile())
        {
            return;
        }

        ExperimentalMidiImportResult r =
            experimentalImportMidiFile(file, kDefaultExperimentalTicksPerQuarter);
        if (!r.ok)
        {
            juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::WarningIcon,
                                                   "MIDI import failed",
                                                   r.combinedUserMessageLine());
            return;
        }

        safeThis->applyMidiImportResult(r);

        if (r.warningMessage.isNotEmpty())
        {
            juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::InfoIcon,
                                                   "MIDI import",
                                                   r.warningMessage);
        }
    });
}

void ExperimentalMidiEditorWindow::Body::beginExportMidi()
{
    if (externalPattern_ == nullptr || boundTimelineClip_ == nullptr)
    {
        juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::WarningIcon,
                                               "Export MIDI",
                                               "Open this editor from a clip on the instrument track to export MIDI.");
        return;
    }

    launchMidiExportFileChooser();
}

void ExperimentalMidiEditorWindow::Body::launchMidiExportFileChooser()
{
    refreshTimelineSampleRateOnTrack();

    juce::File startDir = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory);
    juce::String fileName =
        (boundTimelineClip_ != nullptr && boundTimelineClip_->name.isNotEmpty()) ? boundTimelineClip_->name
                                                                                : juce::String("MIDI clip");
    fileName = fileName.removeCharacters("\\/:*?\"<>|");
    if (fileName.trim().isEmpty())
    {
        fileName = "MIDI clip";
    }
    if (!fileName.endsWithIgnoreCase(".mid") && !fileName.endsWithIgnoreCase(".midi"))
    {
        fileName = fileName + ".mid";
    }

    midiExportChooser_ = std::make_unique<juce::FileChooser>(
        "Export MIDI", startDir.getChildFile(fileName), "*.mid;*.midi", true, false, this);

    const auto flags =
        juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles;

    const juce::Component::SafePointer<Body> safeThis(this);
    midiExportChooser_->launchAsync(flags, [safeThis](const juce::FileChooser& fc) {
        if (safeThis == nullptr)
        {
            return;
        }
        const juce::File file = fc.getResult();
        safeThis->midiExportChooser_.reset();

        if (file.getFullPathName().isEmpty())
        {
            return;
        }

        double sr = 48000.0;
        if (safeThis->deviceManagerForRoll_ != nullptr)
        {
            if (juce::AudioIODevice* d = safeThis->deviceManagerForRoll_->getCurrentAudioDevice())
            {
                const double r = d->getCurrentSampleRate();
                if (r > 0.0 && std::isfinite(r))
                {
                    sr = r;
                }
            }
        }

        if (safeThis->boundTimelineClip_ == nullptr)
        {
            return;
        }

        const InstrumentMidiClipExportResult exportResult =
            exportInstrumentMidiClipToMidiFile(*safeThis->boundTimelineClip_, file, sr);
        if (!exportResult.ok)
        {
            juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::WarningIcon,
                                                   "MIDI export failed",
                                                   exportResult.errorMessage);
            return;
        }

        ExperimentalMidiPatternPlayer::writeMidiEditorLogLine(
            "midi-editor: export wrote path=\"" + file.getFullPathName() + "\" notes="
            + juce::String(exportResult.notesExported));
    });
}

void ExperimentalMidiEditorWindow::Body::applyMidiImportResult(const ExperimentalMidiImportResult& result)
{
    ExperimentalMidiPattern& p = activePattern();
    p.notes.clear();
    p.timelineNotes = result.notes;
    p.ticksPerQuarter = kDefaultExperimentalTicksPerQuarter;
    if (result.firstTempoBpm > 0.0 && std::isfinite(result.firstTempoBpm))
    {
        p.bpm = result.firstTempoBpm;
    }

    bpmSlider_.setValue(p.bpm, juce::dontSendNotification);
    syncStepsAndSnapUiForPattern();

    if (boundTimelineClip_ != nullptr && instrumentTrackForClipBind_ != nullptr)
    {
        instrumentTrackForClipBind_->notifyClipExperimentalMusicalTimingChanged();
        instrumentTrackForClipBind_->sendChangeMessage();
    }

    rebuildPlayerAndRoll();
    if (auto* rv = dynamic_cast<ExperimentalPianoRollView*>(viewport_.getViewedComponent()))
    {
        rv->seedOrResetViewport();
        rv->repaint();
    }

    ExperimentalMidiPatternPlayer::writeMidiEditorLogLine(
        "midi-editor: import applied timelineNotes=" + juce::String((int)p.timelineNotes.size()) + " bpm="
        + juce::String(p.bpm, 2));
}

ExperimentalMidiEditorWindow::ExperimentalMidiEditorWindow(ExperimentalInstrumentHost& host)
    : DocumentWindow("I2 MIDI editor (Drum hits)",
                     juce::Desktop::getInstance().getDefaultLookAndFeel().findColour(
                         juce::ResizableWindow::backgroundColourId),
                     DocumentWindow::allButtons)
{
    setUsingNativeTitleBar(true);
    addKeyListener(this);
    setWantsKeyboardFocus(true);
    setContentOwned(new Body(host), true);
    setResizable(true, true);
    setResizeLimits(640, 420, 10000, 10000);
    centreWithSize(kInitialEditorWidth, kInitialEditorHeight);
}

ExperimentalMidiEditorWindow::~ExperimentalMidiEditorWindow()
{
    removeKeyListener(this);
}

void ExperimentalMidiEditorWindow::closeButtonPressed()
{
    if (auto* b = dynamic_cast<Body*>(getContentComponent()))
    {
        b->snapshotOpenClipViewportFromRoll();
        b->stopForHostUnload();
    }
    setVisible(false);
}

void ExperimentalMidiEditorWindow::prepareInstrumentUnloadFromHost()
{
    if (auto* b = dynamic_cast<Body*>(getContentComponent()))
    {
        b->prepareBeforeHostUnload();
    }
}

void ExperimentalMidiEditorWindow::syncInstrumentStateFromHost()
{
    if (auto* b = dynamic_cast<Body*>(getContentComponent()))
    {
        b->syncInstrumentUiFromHost();
    }
}

void ExperimentalMidiEditorWindow::bindExternalPattern(ExperimentalMidiPattern* pattern,
                                                       InstrumentMidiClip* timelineClip,
                                                       InstrumentTrackController* instrumentTrackForClip,
                                                       Session* session,
                                                       Transport* transport,
                                                       juce::AudioDeviceManager* deviceManager,
                                                       const TimelineViewportModel* mainTimelineViewport,
                                                       const juce::String& titleSuffix)
{
    ExperimentalMidiPatternPlayer::writeMidiEditorLogLine(
        "midi-editor: bindExternalPattern begin patternPtr=" + ptrToLog(pattern) + " noteCount="
        + juce::String(pattern != nullptr ? (int)pattern->notes.size() : -1));

    if (auto* b = dynamic_cast<Body*>(getContentComponent()))
    {
        b->bindExternal(
            pattern, timelineClip, instrumentTrackForClip, session, transport, deviceManager, mainTimelineViewport);
    }

    juce::String winName { "I2 MIDI editor (Drum hits)" };
    if (pattern != nullptr && titleSuffix.isNotEmpty())
    {
        winName << " - " << titleSuffix;
    }
    setName(winName);
}

void ExperimentalMidiEditorWindow::unbindExternalPattern()
{
    if (auto* b = dynamic_cast<Body*>(getContentComponent()))
    {
        b->unbindExternal();
    }
    setName("I2 MIDI editor (Drum hits)");
}

void ExperimentalMidiEditorWindow::bindTransportCommands(ExperimentalMidiTransportCommands commands)
{
    if (auto* b = dynamic_cast<Body*>(getContentComponent()))
    {
        b->setTransportCommands(std::move(commands));
    }
}

bool ExperimentalMidiEditorWindow::keyPressed(const juce::KeyPress& key, juce::Component* originating)
{
    juce::ignoreUnused(originating);
    if (auto* b = dynamic_cast<Body*>(getContentComponent()))
    {
        return b->handleTopLevelShortcut(key);
    }
    return false;
}

void ExperimentalMidiEditorWindow::snapshotOpenClipViewportFromRoll() noexcept
{
    if (auto* b = dynamic_cast<Body*>(getContentComponent()))
    {
        b->snapshotOpenClipViewportFromRoll();
    }
}
