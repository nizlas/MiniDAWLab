#include "ExperimentalMidiEditorWindow.h"
#include "ExperimentalMidiPattern.h"
#include "ExperimentalMidiPatternPlayer.h"
#include "ExperimentalPianoRollView.h"
#include "instruments/InstrumentTrackController.h"
#include "plugins/ExperimentalInstrumentHost.h"

#include "domain/Session.h"
#include "transport/Transport.h"

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

class ExperimentalMidiEditorWindow::Body final : public juce::Component
{
public:
    explicit Body(ExperimentalInstrumentHost& hostIn)
        : host_(hostIn)
        , player_(std::make_unique<ExperimentalMidiPatternPlayer>(hostIn, pattern_))
    {
        pattern_.numSteps = 16;
        pattern_.stepDenom = 16;
        pattern_.bpm = 110.0;
        pattern_.loop = true;

        player_->setPlaybackUiCallback([this] { updateStopButtonState(); });

        addAndMakeVisible(viewport_);
        addAndMakeVisible(playButton_);
        playButton_.setButtonText("Debug Preview");
        playButton_.onClick = [this] {
            player_->startPlayback();
        };

        addAndMakeVisible(stopButton_);
        stopButton_.setButtonText("Stop");
        stopButton_.setEnabled(false);
        stopButton_.onClick = [this] {
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
        };

        addAndMakeVisible(stepsLabel_);
        stepsLabel_.setText("Steps", juce::dontSendNotification);
        stepsLabel_.setJustificationType(juce::Justification::centredRight);
        stepsLabel_.setColour(juce::Label::textColourId, juce::Colours::lightgrey);

        addAndMakeVisible(stepsBox_);
        stepsBox_.addItem("16 steps", 1);
        stepsBox_.addItem("32 steps", 2);
        stepsBox_.setSelectedId(1, juce::dontSendNotification);
        stepsBox_.onChange = [this] {
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
                boundTimelineClip_, sessionForRoll_, transportForRoll_, deviceManagerForRoll_, instrumentTrackForClipBind_);
        }
    }

    void bindExternal(ExperimentalMidiPattern* p,
                      InstrumentMidiClip* timelineClip,
                      InstrumentTrackController* trackForClipGate,
                      Session* session,
                      Transport* transport,
                      juce::AudioDeviceManager* deviceManager)
    {
        InstrumentTrackController* const gatePtr = (p != nullptr) ? trackForClipGate : nullptr;
        if (externalPattern_ == p && instrumentTrackForClipBind_ == gatePtr && boundTimelineClip_ == timelineClip
            && sessionForRoll_ == session && transportForRoll_ == transport && deviceManagerForRoll_ == deviceManager)
        {
            syncSlidersFromActivePattern();
            syncInstrumentUiFromHost();
            return;
        }

        if (player_ != nullptr)
        {
            player_->stopPlayback("rebind");
        }
        externalPattern_ = p;
        instrumentTrackForClipBind_ = gatePtr;
        boundTimelineClip_ = timelineClip;
        sessionForRoll_ = session;
        transportForRoll_ = transport;
        deviceManagerForRoll_ = deviceManager;
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
        externalPattern_ = nullptr;
        instrumentTrackForClipBind_ = nullptr;
        boundTimelineClip_ = nullptr;
        sessionForRoll_ = nullptr;
        transportForRoll_ = nullptr;
        deviceManagerForRoll_ = nullptr;
        rebuildPlayerAndRoll();
        syncSlidersFromActivePattern();
        syncInstrumentUiFromHost();
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

        playButton_.setEnabled(canPlayPattern);

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
            juce::String("I3d1: roll X = session samples (zoom/pan here is separate from main timeline). ")
                + "Clip length in samples is locked when BPM changes; grid splits that length into equal time steps. "
                  "Changing Steps recomputes musical length. Preview timing follows BPM and may diverge from the "
                  "absolute grid after BPM edits.\n"
                + instPart + "\n"
                + "Timing: ~4 ms message timer; not sample-accurate." + kitLine,
            juce::dontSendNotification);
        modeLabel_.setColour(juce::Label::textColourId, labelColour);

        updateStopButtonState();

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

    void updateStopButtonState()
    {
        stopButton_.setEnabled(editorInstrumentGate() && player_ != nullptr && player_->isPlaying());
    }

    void resized() override
    {
        auto a = getLocalBounds();
        auto toolbar = a.removeFromTop(kToolbarH);
        toolbar.reduce(6, 4);
        playButton_.setBounds(toolbar.removeFromLeft(110).reduced(0, 2));
        stopButton_.setBounds(toolbar.removeFromLeft(72).reduced(0, 2));
        stepsLabel_.setBounds(toolbar.removeFromLeft(44).reduced(0, 4));
        stepsBox_.setBounds(toolbar.removeFromLeft(100).reduced(0, 2));
        followPlayheadToggle_.setBounds(toolbar.removeFromLeft(72).reduced(0, 2));
        bpmLabel_.setBounds(toolbar.removeFromLeft(36).reduced(0, 4));
        bpmSlider_.setBounds(toolbar.removeFromLeft(140).reduced(0, 2));
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
    InstrumentTrackController* instrumentTrackForClipBind_ = nullptr;
    InstrumentMidiClip* boundTimelineClip_ = nullptr;
    Session* sessionForRoll_ = nullptr;
    Transport* transportForRoll_ = nullptr;
    juce::AudioDeviceManager* deviceManagerForRoll_ = nullptr;

    void syncSlidersFromActivePattern()
    {
        bpmSlider_.setValue(activePattern().bpm, juce::dontSendNotification);
        stepsBox_.setSelectedId(activePattern().numSteps == 32 ? 2 : 1, juce::dontSendNotification);
    }

    void rebuildPlayerAndRoll()
    {
        player_ = std::make_unique<ExperimentalMidiPatternPlayer>(host_, activePattern());
        player_->setPlaybackUiCallback([this] { updateStopButtonState(); });
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
        roll->setSessionTimelineContext(
            boundTimelineClip_, sessionForRoll_, transportForRoll_, deviceManagerForRoll_, instrumentTrackForClipBind_);
        roll->setFollowPlayheadEnabled(followPlayheadToggle_.getToggleState());
        viewport_.setViewedComponent(roll, true);

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

    juce::TextButton playButton_;
    juce::TextButton stopButton_;
    juce::Label bpmLabel_;
    juce::Slider bpmSlider_;
    juce::Label stepsLabel_;
    juce::ComboBox stepsBox_;
    juce::TextButton followPlayheadToggle_;
    juce::Label modeLabel_;
    juce::Viewport viewport_;
};

ExperimentalMidiEditorWindow::ExperimentalMidiEditorWindow(ExperimentalInstrumentHost& host)
    : DocumentWindow("I2 MIDI editor (Drum hits)",
                     juce::Desktop::getInstance().getDefaultLookAndFeel().findColour(
                         juce::ResizableWindow::backgroundColourId),
                     DocumentWindow::allButtons)
{
    setUsingNativeTitleBar(true);
    setContentOwned(new Body(host), true);
    setResizable(true, true);
    setResizeLimits(640, 420, 10000, 10000);
    centreWithSize(kInitialEditorWidth, kInitialEditorHeight);
}

ExperimentalMidiEditorWindow::~ExperimentalMidiEditorWindow() = default;

void ExperimentalMidiEditorWindow::closeButtonPressed()
{
    if (auto* b = dynamic_cast<Body*>(getContentComponent()))
    {
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
                                                       const juce::String& titleSuffix)
{
    ExperimentalMidiPatternPlayer::writeMidiEditorLogLine(
        "midi-editor: bindExternalPattern begin patternPtr=" + ptrToLog(pattern) + " noteCount="
        + juce::String(pattern != nullptr ? (int)pattern->notes.size() : -1));

    if (auto* b = dynamic_cast<Body*>(getContentComponent()))
    {
        b->bindExternal(pattern, timelineClip, instrumentTrackForClip, session, transport, deviceManager);
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
