#include "ExperimentalMidiEditorWindow.h"
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
#include "ui/CollapsibleSideStrip.h"
#include "diagnostics/UndoDiagnosticConfig.h"
#include "diagnostics/UndoDiagnosticFileLog.h"
#include "diagnostics/DrumNameDiagnosticConfig.h"

#include <juce_audio_devices/juce_audio_devices.h>

#include <algorithm>
#include <cmath>
#include <optional>

namespace
{
    constexpr int kToolbarH = 40;
    constexpr int kInitialEditorWidth = 1100;
    constexpr int kInitialEditorHeight = 760;

    [[nodiscard]] juce::String ptrToLog(const void* p) noexcept
    {
        return p != nullptr ? juce::String::formatted("%p", p) : juce::String("null");
    }

    void applyPianoRollPitchRangeForInstrument(ExperimentalPianoRollView& roll,
                                               const InstrumentTrackController* track) noexcept
    {
        if (track == nullptr)
        {
            roll.setEditablePitchRange(ExperimentalPianoRollView::kDrumPitchLow,
                                       ExperimentalPianoRollView::kDrumPitchHigh);
            return;
        }
        if (track->getExperimentalInstrumentKind() == "HALionSonic")
        {
            roll.setEditablePitchRange(ExperimentalPianoRollView::kMelodicPitchLow,
                                       ExperimentalPianoRollView::kMelodicPitchHigh);
        }
        else
        {
            roll.setEditablePitchRange(ExperimentalPianoRollView::kDrumPitchLow,
                                       ExperimentalPianoRollView::kDrumPitchHigh);
        }
    }
} // namespace

class ExperimentalMidiEditorWindow::Body final : public juce::Component,
                                                 public collapsible_side_strip::Host,
                                                 private juce::Timer,
                                                 private juce::Slider::Listener,
                                                 private juce::ChangeListener
{
    friend class ExperimentalMidiEditorWindow;

public:
    explicit Body(ExperimentalInstrumentHost& hostIn)
        : host_(hostIn)
        , player_(std::make_unique<ExperimentalMidiPatternPlayer>(hostIn, pattern_))
        , midiRollResizeSplitter_(*this)
        , midiRollCollapsedKnob_(*this)
    {
        pattern_.numSteps = 16;
        pattern_.stepDenom = 16;
        pattern_.bpm = 110.0;
        pattern_.loop = true;

        player_->setPlaybackUiCallback([this] { updateDebugStopButtonState(); });

        addAndMakeVisible(viewport_);
        viewport_.setScrollBarsShown(false, true);
        addAndMakeVisible(midiRollResizeSplitter_);
        midiRollResizeSplitter_.setVisible(false);
        addAndMakeVisible(midiRollCollapsedKnob_);
        midiRollCollapsedKnob_.setVisible(false);

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
        bpmSlider_.addListener(this);

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

        addAndMakeVisible(rowsLabel_);
        rowsLabel_.setText("Rows", juce::dontSendNotification);
        rowsLabel_.setJustificationType(juce::Justification::centredRight);
        rowsLabel_.setColour(juce::Label::textColourId, juce::Colours::lightgrey);

        addAndMakeVisible(rowsBox_);
        rowsBox_.addItem("Piano", 1);
        rowsBox_.addItem("Drum Names", 2);
        rowsBox_.setSelectedId(1, juce::dontSendNotification);
        rowsBox_.onChange = [this] { pushRowsModeToRoll(); };

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

            auto applyStepsChange = [this, newSteps]() -> bool {
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
                return true;
            };

            if (canUseInstrumentUndo() && static_cast<bool>(instrumentUndoableMutate_))
            {
                instrumentUndoableMutate_("Change steps", std::move(applyStepsChange));
            }
            else
            {
                applyStepsChange();
            }
        };

        addAndMakeVisible(timelineRulerFormatCombo_);
        timelineRulerFormatCombo_.clear(juce::dontSendNotification);
        timelineRulerFormatCombo_.addItem("Bars + Beats", Session::kTimelineRulerFormatComboIdBarsBeats);
        timelineRulerFormatCombo_.addItem("Seconds", Session::kTimelineRulerFormatComboIdSeconds);
        timelineRulerFormatCombo_.setTooltip("Timeline ruler time format (shared with the main arrangement).");
        timelineRulerFormatCombo_.onChange = [this] {
            if (sessionForRoll_ == nullptr)
            {
                return;
            }
            const int id = timelineRulerFormatCombo_.getSelectedId();
            if (id == Session::kTimelineRulerFormatComboIdBarsBeats)
            {
                sessionForRoll_->setTimelineRulerTimeDisplay(Session::TimelineRulerTimeDisplay::MusicalBarsBeats);
            }
            else if (id == Session::kTimelineRulerFormatComboIdSeconds)
            {
                sessionForRoll_->setTimelineRulerTimeDisplay(Session::TimelineRulerTimeDisplay::TimeSeconds);
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
        syncTimelineRulerFormatFromSession();
    }

    ~Body() override
    {
        if (instrumentTrackForClipBind_ != nullptr)
            instrumentTrackForClipBind_->removeChangeListener(this);

        bpmSlider_.removeListener(this);
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
            applyPianoRollPitchRangeForInstrument(*rv, instrumentTrackForClipBind_);
            pushTransportGestureBlockToRoll();
            pushRowsModeToRoll();
        }
    }

    void syncTimelineRulerFormatFromSession()
    {
        if (sessionForRoll_ == nullptr)
        {
            timelineRulerFormatCombo_.setSelectedId(Session::kTimelineRulerFormatComboIdBarsBeats,
                                                    juce::dontSendNotification);
            timelineRulerFormatCombo_.setEnabled(false);
            timelineRulerFormatCombo_.setTooltip("Clip-bound session required to share ruler format with the main window.");
            return;
        }

        const int id = sessionForRoll_->getTimelineRulerTimeDisplay()
                               == Session::TimelineRulerTimeDisplay::MusicalBarsBeats
                           ? Session::kTimelineRulerFormatComboIdBarsBeats
                           : Session::kTimelineRulerFormatComboIdSeconds;
        timelineRulerFormatCombo_.setSelectedId(id, juce::dontSendNotification);
        timelineRulerFormatCombo_.setEnabled(true);
        timelineRulerFormatCombo_.setTooltip("Timeline ruler time format (shared with the main arrangement).");

        if (auto* rv = dynamic_cast<ExperimentalPianoRollView*>(viewport_.getViewedComponent()))
        {
            rv->repaint();
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
            persistentInstrumentClipIdForRebind_ = (timelineClip != nullptr)
                                                         ? static_cast<std::uint64_t>(timelineClip->id)
                                                         : std::uint64_t{0};
            syncSlidersFromActivePattern();
            syncInstrumentUiFromHost();
            syncTimelineRulerFormatFromSession();
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
        InstrumentTrackController* const oldInstrumentTrackGate = instrumentTrackForClipBind_;
        instrumentTrackForClipBind_ = gatePtr;
        if (oldInstrumentTrackGate != instrumentTrackForClipBind_)
        {
            if (oldInstrumentTrackGate != nullptr)
                oldInstrumentTrackGate->removeChangeListener(this);

            if (instrumentTrackForClipBind_ != nullptr)
                instrumentTrackForClipBind_->addChangeListener(this);
        }

        boundTimelineClip_ = timelineClip;
        persistentInstrumentClipIdForRebind_ = (timelineClip != nullptr)
                                                   ? static_cast<std::uint64_t>(timelineClip->id)
                                                   : std::uint64_t{0};
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
        syncTimelineRulerFormatFromSession();
        if constexpr (undo_diagnostic::kUndoDiag)
        {
            writeUndoDiagnosticLogLine(
                "[UndoDiag] midi-editor bindExternal storedClipId="
                + juce::String(static_cast<juce::int64>(persistentInstrumentClipIdForRebind_)) + " patternPtr="
                + ptrToLog(p) + " timelineNotes="
                + (p != nullptr ? juce::String((int)p->timelineNotes.size()) : juce::String("-")));
        }
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
        if (instrumentTrackForClipBind_ != nullptr)
        {
            instrumentTrackForClipBind_->removeChangeListener(this);
        }
        instrumentTrackForClipBind_ = nullptr;
        boundTimelineClip_ = nullptr;
        persistentInstrumentClipIdForRebind_ = 0;
        sessionForRoll_ = nullptr;
        transportForRoll_ = nullptr;
        deviceManagerForRoll_ = nullptr;
        mainTimelineViewportForRoll_ = nullptr;
        rebuildPlayerAndRoll();
        syncSlidersFromActivePattern();
        syncInstrumentUiFromHost();
        syncTimelineRulerFormatFromSession();
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
            juce::String("I3f: timeline MIDI — import MIDI from the instrument track header (arrangement). ")
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
        exportButton_.setBounds(toolbar.removeFromLeft(96).reduced(0, 2));
        snapLabel_.setBounds(toolbar.removeFromLeft(40).reduced(0, 4));
        snapBox_.setBounds(toolbar.removeFromLeft(76).reduced(0, 2));
        displayLabel_.setBounds(toolbar.removeFromLeft(52).reduced(0, 4));
        displayBox_.setBounds(toolbar.removeFromLeft(72).reduced(0, 2));
        rowsLabel_.setBounds(toolbar.removeFromLeft(40).reduced(0, 4));
        rowsBox_.setBounds(toolbar.removeFromLeft(104).reduced(0, 2));
        stepsLabel_.setBounds(toolbar.removeFromLeft(44).reduced(0, 4));
        stepsBox_.setBounds(toolbar.removeFromLeft(100).reduced(0, 2));
        timelineRulerFormatCombo_.setBounds(toolbar.removeFromLeft(128).reduced(0, 2));
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
            const int rh = juce::jmax(1, budgetH);
            rv->setSize(rw, rh);
            viewport_.setViewPosition(viewport_.getViewPositionX(), 0);

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
        pushActiveSideStripWidthToRoll();
        layoutMidiSideStripChrome();
    }

private:
    // collapsible_side_strip::Host (stable Body owns strip chrome + per-mode widths, like `TransportControlsContent`).
    [[nodiscard]] int getSideStripWidth() const noexcept override { return midiRollActiveSideStripTotal(); }
    void setSideStripWidth(int w) noexcept override { midiRollSetActiveSideStripTotal(w); }
    [[nodiscard]] int getSideStripMaxWidth() const noexcept override { return midiRollActiveSideStripMax(); }
    [[nodiscard]] int getSideStripDefaultWidth() const noexcept override;
    void sideStripLayoutChanged() override;

    void pushActiveSideStripWidthToRoll();
    void layoutMidiSideStripChrome();
    [[nodiscard]] int midiRollActiveSideStripTotal() const noexcept;
    [[nodiscard]] int midiRollActiveSideStripMax() const noexcept;
    void midiRollSetActiveSideStripTotal(int w) noexcept;

    void timerCallback() override;
    void changeListenerCallback(juce::ChangeBroadcaster* source) override;
    void setTransportCommands(ExperimentalMidiTransportCommands commands);
    void pushTransportGestureBlockToRoll();
    void applyInstrumentUndoGatewayToRoll();
    [[nodiscard]] bool canUseInstrumentUndo() const noexcept;
    void setInstrumentMusicalUndoUi(
        std::function<void(const juce::String&, std::function<bool()>)> onUndoableEdit,
        std::function<void(const juce::String&, std::vector<ProjectFileExperimentalInstrumentTrackV1>)>
            onCommitMusicalDragEnd,
        std::function<std::vector<ProjectFileExperimentalInstrumentTrackV1>()> captureMusical,
        std::function<void()> onUndoShortcut,
        std::function<void()> onRedoShortcut);
    [[nodiscard]] bool handleTopLevelShortcut(const juce::KeyPress& key);
    void notifyExternalTransportSeek(std::int64_t targetSample) noexcept;

    [[nodiscard]] std::optional<std::uint64_t> getBoundInstrumentClipId() const noexcept;

    void sliderValueChanged(juce::Slider* s) override;
    void sliderDragStarted(juce::Slider* s) override;
    void sliderDragEnded(juce::Slider* s) override;

    void syncStepsAndSnapUiForPattern();
    void pushSnapToRoll();
    void pushDisplayToRoll();
    void pushRowsModeToRoll();
    [[nodiscard]] juce::String resolveDrumRowDisplayName(int midiNote, int pluginQueryChannel) const noexcept;
    [[nodiscard]] juce::String resolveDrumRowHoverTooltip(int midiNote, int pluginQueryChannel) const noexcept;
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
        applyPianoRollPitchRangeForInstrument(*roll, instrumentTrackForClipBind_);
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
        applyInstrumentUndoGatewayToRoll();
        pushRowsModeToRoll();
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
    juce::TextButton exportButton_;
    juce::Label snapLabel_;
    juce::ComboBox snapBox_;
    juce::Label displayLabel_;
    juce::ComboBox displayBox_;
    juce::Label rowsLabel_;
    juce::ComboBox rowsBox_;
    juce::Label bpmLabel_;
    juce::Slider bpmSlider_;
    juce::Label stepsLabel_;
    juce::ComboBox stepsBox_;
    juce::ComboBox timelineRulerFormatCombo_;
    juce::TextButton followPlayheadToggle_;
    juce::Label modeLabel_;
    collapsible_side_strip::ResizeSplitter midiRollResizeSplitter_;
    collapsible_side_strip::CollapsedKnob midiRollCollapsedKnob_;
    juce::Viewport viewport_;

    std::unique_ptr<juce::FileChooser> midiExportChooser_;

    std::function<void(const juce::String&, std::function<bool()>)> instrumentUndoableMutate_;
    std::function<void(const juce::String&, std::vector<ProjectFileExperimentalInstrumentTrackV1>)>
        instrumentCommitMusicalDrag_;
    std::function<std::vector<ProjectFileExperimentalInstrumentTrackV1>()> instrumentCaptureMusical_;
    std::function<void()> onGlobalUndoRequested_;
    std::function<void()> onGlobalRedoRequested_;
    bool bpmSliderGestureActive_ = false;
    std::vector<ProjectFileExperimentalInstrumentTrackV1> bpmDragMusicalBefore_;

    /// Stable clip id for post–instrument-undo rebind. Never read `boundTimelineClip_->id` after the
    /// controller may have freed clip storage (`applyExperimentalInstrumentMusicalUndoBlock`).
    std::uint64_t persistentInstrumentClipIdForRebind_ = 0;

    /// Stable per–row-mode strip totals (content + splitter); survives piano roll rebuilds.
    int midiRollSideStripTotalPiano_ =
        ExperimentalPianoRollView::kMidiEditorKeyboardLaneWidthPianoDefault + collapsible_side_strip::kSplitterWidth;
    int midiRollSideStripTotalDrum_ = ExperimentalPianoRollView::kMidiEditorKeyboardLaneWidthDrumNamesDefault
                                      + collapsible_side_strip::kSplitterWidth;
    /// 1 = piano row labels, 2 = drum names — mirrors `rowsBox_`; selects which total width bucket is active.
    int midiRollRowLabelMode_ = 1;

    ExperimentalMidiTransportCommands transportCommands_{};
};

int ExperimentalMidiEditorWindow::Body::getSideStripDefaultWidth() const noexcept
{
    return midiRollRowLabelMode_ == 2
               ? (ExperimentalPianoRollView::kMidiEditorKeyboardLaneWidthDrumNamesDefault
                  + collapsible_side_strip::kSplitterWidth)
               : (ExperimentalPianoRollView::kMidiEditorKeyboardLaneWidthPianoDefault
                  + collapsible_side_strip::kSplitterWidth);
}

void ExperimentalMidiEditorWindow::Body::sideStripLayoutChanged()
{
    resized();
}

int ExperimentalMidiEditorWindow::Body::midiRollActiveSideStripTotal() const noexcept
{
    return midiRollRowLabelMode_ == 2 ? midiRollSideStripTotalDrum_ : midiRollSideStripTotalPiano_;
}

int ExperimentalMidiEditorWindow::Body::midiRollActiveSideStripMax() const noexcept
{
    return midiRollRowLabelMode_ == 2
               ? (ExperimentalPianoRollView::kMidiEditorKeyboardLaneWidthDrumNamesMax
                  + collapsible_side_strip::kSplitterWidth)
               : (ExperimentalPianoRollView::kMidiEditorKeyboardLaneWidthPianoMax
                  + collapsible_side_strip::kSplitterWidth);
}

void ExperimentalMidiEditorWindow::Body::midiRollSetActiveSideStripTotal(const int w) noexcept
{
    const int nw = juce::jlimit(0, midiRollActiveSideStripMax(), w);
    if (midiRollRowLabelMode_ == 2)
    {
        midiRollSideStripTotalDrum_ = nw;
    }
    else
    {
        midiRollSideStripTotalPiano_ = nw;
    }
}

void ExperimentalMidiEditorWindow::Body::pushActiveSideStripWidthToRoll()
{
    if (auto* rv = dynamic_cast<ExperimentalPianoRollView*>(viewport_.getViewedComponent()))
    {
        rv->setSideStripTotalWidthForUiOnly(midiRollActiveSideStripTotal());
    }
}

void ExperimentalMidiEditorWindow::Body::layoutMidiSideStripChrome()
{
    const int S = midiRollActiveSideStripTotal();
    const auto vr = viewport_.getBounds();
    const int rh = ExperimentalPianoRollView::kRulerHeight;
    if (S <= 0)
    {
        midiRollResizeSplitter_.setVisible(false);
        midiRollResizeSplitter_.setBounds(0, 0, 0, 0);
        const int kY = vr.getCentreY() - collapsible_side_strip::kCollapsedKnobHeight / 2;
        midiRollCollapsedKnob_.setBounds(
            vr.getX(),
            kY,
            collapsible_side_strip::kCollapsedKnobWidth,
            collapsible_side_strip::kCollapsedKnobHeight);
        midiRollCollapsedKnob_.setVisible(true);
        midiRollCollapsedKnob_.toFront(false);
    }
    else
    {
        midiRollCollapsedKnob_.setVisible(false);
        const int splitW = juce::jmin(collapsible_side_strip::kSplitterWidth, S);
        const int contentW = juce::jmax(0, S - splitW);
        const int bodyH = juce::jmax(0, vr.getHeight() - rh);
        midiRollResizeSplitter_.setBounds(vr.getX() + contentW, vr.getY() + rh, splitW, bodyH);
        midiRollResizeSplitter_.setVisible(true);
        midiRollResizeSplitter_.toFront(false);
    }
}

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

void ExperimentalMidiEditorWindow::Body::changeListenerCallback(juce::ChangeBroadcaster* source)
{
    juce::ignoreUnused(source);
    if (rowsBox_.getSelectedId() != 2)
    {
        return;
    }
    pushRowsModeToRoll();
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

bool ExperimentalMidiEditorWindow::Body::canUseInstrumentUndo() const noexcept
{
    return externalPattern_ != nullptr && instrumentTrackForClipBind_ != nullptr
           && instrumentTrackForClipBind_->hasInstrumentTrack();
}

void ExperimentalMidiEditorWindow::Body::setInstrumentMusicalUndoUi(
    std::function<void(const juce::String&, std::function<bool()>)> onUndoableEdit,
    std::function<void(const juce::String&, std::vector<ProjectFileExperimentalInstrumentTrackV1>)>
        onCommitMusicalDragEnd,
    std::function<std::vector<ProjectFileExperimentalInstrumentTrackV1>()> captureMusical,
    std::function<void()> onUndoShortcut,
    std::function<void()> onRedoShortcut)
{
    instrumentUndoableMutate_ = std::move(onUndoableEdit);
    instrumentCommitMusicalDrag_ = std::move(onCommitMusicalDragEnd);
    instrumentCaptureMusical_ = std::move(captureMusical);
    onGlobalUndoRequested_ = std::move(onUndoShortcut);
    onGlobalRedoRequested_ = std::move(onRedoShortcut);
    applyInstrumentUndoGatewayToRoll();
}

void ExperimentalMidiEditorWindow::Body::applyInstrumentUndoGatewayToRoll()
{
    if (auto* rv = dynamic_cast<ExperimentalPianoRollView*>(viewport_.getViewedComponent()))
    {
        if (instrumentUndoableMutate_)
        {
            rv->setUndoablePatternEditHandler(
                [this](const juce::String& lab, std::function<bool()> m) {
                    if (canUseInstrumentUndo())
                    {
                        instrumentUndoableMutate_(lab, std::move(m));
                    }
                    else
                    {
                        m();
                    }
                });
        }
        else
        {
            rv->setUndoablePatternEditHandler({});
        }
    }
}

void ExperimentalMidiEditorWindow::Body::sliderValueChanged(juce::Slider* s)
{
    if (s != &bpmSlider_)
    {
        return;
    }
    const double v = bpmSlider_.getValue();
    auto applyLocal = [this, v] {
        activePattern().bpm = v;
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
    if (!canUseInstrumentUndo())
    {
        applyLocal();
        return;
    }
    if (bpmSliderGestureActive_)
    {
        applyLocal();
        return;
    }
    if (instrumentUndoableMutate_)
    {
        instrumentUndoableMutate_("Edit BPM", [this, v]() -> bool {
            activePattern().bpm = v;
            if (externalPattern_ != nullptr && instrumentTrackForClipBind_ != nullptr
                && activePattern().usesTimelineNotes())
            {
                instrumentTrackForClipBind_->notifyClipExperimentalMusicalTimingChanged();
            }
            if (auto* rv = dynamic_cast<ExperimentalPianoRollView*>(viewport_.getViewedComponent()))
            {
                rv->repaint();
            }
            return true;
        });
    }
    else
    {
        applyLocal();
    }
}

void ExperimentalMidiEditorWindow::Body::sliderDragStarted(juce::Slider* s)
{
    if (s != &bpmSlider_ || !canUseInstrumentUndo() || !instrumentCaptureMusical_
        || !instrumentCommitMusicalDrag_)
    {
        return;
    }
    bpmSliderGestureActive_ = true;
    bpmDragMusicalBefore_ = instrumentCaptureMusical_();
}

void ExperimentalMidiEditorWindow::Body::sliderDragEnded(juce::Slider* s)
{
    if (s != &bpmSlider_)
    {
        return;
    }
    if (!bpmSliderGestureActive_)
    {
        return;
    }
    bpmSliderGestureActive_ = false;
    if (!canUseInstrumentUndo() || !instrumentCommitMusicalDrag_)
    {
        return;
    }
    instrumentCommitMusicalDrag_("Edit BPM", std::move(bpmDragMusicalBefore_));
}

std::optional<std::uint64_t> ExperimentalMidiEditorWindow::Body::getBoundInstrumentClipId() const noexcept
{
    if (persistentInstrumentClipIdForRebind_ == 0)
    {
        return std::nullopt;
    }
    return persistentInstrumentClipIdForRebind_;
}

bool ExperimentalMidiEditorWindow::Body::handleTopLevelShortcut(const juce::KeyPress& key)
{
    if (dynamic_cast<juce::TextEditor*>(juce::Component::getCurrentlyFocusedComponent()) != nullptr)
    {
        return false;
    }
    const bool cmd = key.getModifiers().isCommandDown();
    const bool z = (key.getKeyCode() == 'z' || key.getKeyCode() == 'Z');
    const bool y = (key.getKeyCode() == 'y' || key.getKeyCode() == 'Y');
    const bool undoCombo = cmd && !key.getModifiers().isShiftDown() && z;
    const bool redoCombo = cmd && (y || (key.getModifiers().isShiftDown() && z));
    if (undoCombo && onGlobalUndoRequested_)
    {
        onGlobalUndoRequested_();
        return true;
    }
    if (redoCombo && onGlobalRedoRequested_)
    {
        onGlobalRedoRequested_();
        return true;
    }
    if (const bool cKey = (key.getKeyCode() == 'c' || key.getKeyCode() == 'C');
        cmd && cKey && !key.getModifiers().isShiftDown())
    {
        if (auto* rv = dynamic_cast<ExperimentalPianoRollView*>(viewport_.getViewedComponent()))
        {
            if (rv->handleTimelineNotesCopyShortcut())
            {
                return true;
            }
        }
    }
    if (const bool vKey = (key.getKeyCode() == 'v' || key.getKeyCode() == 'V');
        cmd && vKey && !key.getModifiers().isShiftDown())
    {
        if (auto* rv = dynamic_cast<ExperimentalPianoRollView*>(viewport_.getViewedComponent()))
        {
            if (rv->handleTimelineNotesPasteShortcut())
            {
                return true;
            }
        }
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
    if (midi_transport_shortcuts::isJumpToLeftLocatorShortcut(key))
    {
        if (transportCommands_.onJumpToLeftLocator)
        {
            transportCommands_.onJumpToLeftLocator();
            return true;
        }
        return false;
    }
    return false;
}

void ExperimentalMidiEditorWindow::Body::notifyExternalTransportSeek(const std::int64_t targetSample) noexcept
{
    if (auto* rv = dynamic_cast<ExperimentalPianoRollView*>(viewport_.getViewedComponent()))
    {
        rv->resetUiPlayheadAnchorToSample(targetSample);
    }
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

juce::String ExperimentalMidiEditorWindow::Body::resolveDrumRowDisplayName(const int midiNote,
                                                                           const int pluginQueryChannel) const noexcept
{
    juce::ignoreUnused(pluginQueryChannel);

    if (instrumentTrackForClipBind_ == nullptr)
        return {};

    const auto eff = instrumentTrackForClipBind_->getEffectiveDrumLabel(midiNote);
    if (eff.has_value() && eff->first.isNotEmpty())
        return eff->first;

    return {};
}

juce::String ExperimentalMidiEditorWindow::Body::resolveDrumRowHoverTooltip(
    const int midiNote,
    const int pluginQueryChannel) const noexcept
{
    juce::ignoreUnused(midiNote, pluginQueryChannel);
    return {};
}

void ExperimentalMidiEditorWindow::Body::pushRowsModeToRoll()
{
    midiRollRowLabelMode_ = juce::jlimit(1, 2, rowsBox_.getSelectedId());
    if (auto* rv = dynamic_cast<ExperimentalPianoRollView*>(viewport_.getViewedComponent()))
    {
        rv->setRowLabelMode(midiRollRowLabelMode_);
        rv->setSideStripTotalWidthForUiOnly(midiRollActiveSideStripTotal());
        const int pluginCh = instrumentTrackForClipBind_ != nullptr
                                 ? instrumentTrackForClipBind_->pluginNoteNameQueryChannel(boundTimelineClip_)
                                 : 10;
        rv->setRowLabelProvider([this, pluginCh](const int note) {
            return resolveDrumRowDisplayName(note, pluginCh);
        });
        if (rowsBox_.getSelectedId() == 2)
        {
            rv->setRowLabelTooltipProvider([this, pluginCh](const int note) {
                return resolveDrumRowHoverTooltip(note, pluginCh);
            });
        }
        else
        {
            rv->setRowLabelTooltipProvider({});
        }
        rv->setOnCommitRowLabelEdit([this](const int note, juce::String name) {
            if (instrumentTrackForClipBind_ == nullptr)
            {
                return;
            }
            instrumentTrackForClipBind_->setDrumNoteUserOverride(note, std::move(name));
        });
        rv->repaint();
    }
    if (rowsBox_.getSelectedId() == 2)
    {
        int pitchLow = ExperimentalPianoRollView::kDrumPitchLow;
        int pitchHigh = ExperimentalPianoRollView::kDrumPitchHigh;
        if (auto* rvPitch = dynamic_cast<ExperimentalPianoRollView*>(viewport_.getViewedComponent()))
        {
            pitchLow = rvPitch->pitchLow();
            pitchHigh = rvPitch->pitchHigh();
        }
        int manualLabelled = 0;
        int autoPluginLabelled = 0;
        int blank = 0;
        for (int n = pitchLow; n <= pitchHigh; ++n)
        {
            if (instrumentTrackForClipBind_ == nullptr)
            {
                ++blank;
                continue;
            }

            const auto eff = instrumentTrackForClipBind_->getEffectiveDrumLabel(n);
            if (!eff.has_value() || eff->first.isEmpty())
            {
                ++blank;
            }
            else if (eff->second == DrumLabelSource::manual)
            {
                ++manualLabelled;
            }
            else
            {
                ++autoPluginLabelled;
            }
        }

        ExperimentalMidiPatternPlayer::writeMidiEditorLogLine(
            juce::String{"midi-editor: drum-rows mode=DrumNames labelSource=track_local manualLabelled="}
            + juce::String(manualLabelled) + juce::String{" autoPluginLabelled="} + juce::String(autoPluginLabelled)
            + juce::String{" blank="} + juce::String(blank)
            + juce::String{" gmPercussionFallback=disabled noteFallback=disabled hostMapConsulted=false"
                           " pitchRange="}
            + juce::String(pitchLow) + juce::String{"-"} + juce::String(pitchHigh));
    }
    layoutMidiSideStripChrome();
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

ExperimentalMidiEditorWindow::ExperimentalMidiEditorWindow(ExperimentalInstrumentHost& host)
    : DocumentWindow("I2 MIDI editor (Drum hits)",
                     juce::Desktop::getInstance().getDefaultLookAndFeel().findColour(
                         juce::ResizableWindow::backgroundColourId),
                     DocumentWindow::allButtons)
    , host_(host)
{
    setUsingNativeTitleBar(true);
    addKeyListener(this);
    setWantsKeyboardFocus(true);
    setContentOwned(new Body(host), true);
    setResizable(true, true);
    setResizeLimits(640, 420, 10000, 10000);
    centreWithSize(kInitialEditorWidth, kInitialEditorHeight);

    if (drum_name_diag::kDrumNamesDiag)
    {
        host_.setOnPluginPitchNamesCacheMayHaveChanged([this] { syncInstrumentStateFromHost(); });
    }
}

ExperimentalMidiEditorWindow::~ExperimentalMidiEditorWindow()
{
    host_.setOnPluginPitchNamesCacheMayHaveChanged({});
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

void ExperimentalMidiEditorWindow::syncTimelineRulerFormatFromSession()
{
    if (auto* b = dynamic_cast<Body*>(getContentComponent()))
    {
        b->syncTimelineRulerFormatFromSession();
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

void ExperimentalMidiEditorWindow::setInstrumentMusicalUndoUi(
    std::function<void(const juce::String&, std::function<bool()>)> onUndoableEdit,
    std::function<void(const juce::String&, std::vector<ProjectFileExperimentalInstrumentTrackV1>)>
        onCommitMusicalDragEnd,
    std::function<std::vector<ProjectFileExperimentalInstrumentTrackV1>()> captureMusical,
    std::function<void()> onUndoShortcut,
    std::function<void()> onRedoShortcut)
{
    if (auto* b = dynamic_cast<Body*>(getContentComponent()))
    {
        b->setInstrumentMusicalUndoUi(std::move(onUndoableEdit),
                                      std::move(onCommitMusicalDragEnd),
                                      std::move(captureMusical),
                                      std::move(onUndoShortcut),
                                      std::move(onRedoShortcut));
    }
}

std::optional<std::uint64_t> ExperimentalMidiEditorWindow::getBoundInstrumentClipId() const noexcept
{
    if (auto* b = dynamic_cast<const Body*>(getContentComponent()))
    {
        return b->getBoundInstrumentClipId();
    }
    return std::nullopt;
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

void ExperimentalMidiEditorWindow::notifyExternalTransportSeek(const std::int64_t targetSample) noexcept
{
    if (auto* b = dynamic_cast<Body*>(getContentComponent()))
    {
        b->notifyExternalTransportSeek(targetSample);
    }
}

void ExperimentalMidiEditorWindow::snapshotOpenClipViewportFromRoll() noexcept
{
    if (auto* b = dynamic_cast<Body*>(getContentComponent()))
    {
        b->snapshotOpenClipViewportFromRoll();
    }
}
