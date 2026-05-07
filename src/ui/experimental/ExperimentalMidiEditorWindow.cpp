#include "ExperimentalMidiEditorWindow.h"
#include "ExperimentalMidiPattern.h"
#include "ExperimentalMidiPatternPlayer.h"
#include "ExperimentalPianoRollView.h"
#include "instruments/InstrumentTrackController.h"
#include "plugins/ExperimentalInstrumentHost.h"

#include <algorithm>

namespace
{
    constexpr int kToolbarH = 40;
    constexpr int kEditorTotalWidth = 720;
    constexpr int kEditorTotalHeight = 780;

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
        playButton_.setButtonText("Play pattern");
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
            if (auto* rv = dynamic_cast<ExperimentalPianoRollView*>(viewport_.getViewedComponent()))
            {
                rv->repaint();
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
    }

    void bindExternal(ExperimentalMidiPattern* p, InstrumentTrackController* trackForClipGate)
    {
        InstrumentTrackController* const gatePtr = (p != nullptr) ? trackForClipGate : nullptr;
        if (externalPattern_ == p && instrumentTrackForClipBind_ == gatePtr)
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
        rebuildPlayerAndRoll();
        syncSlidersFromActivePattern();
        syncInstrumentUiFromHost();
    }

    void unbindExternal()
    {
        if (externalPattern_ == nullptr)
        {
            return;
        }
        if (player_ != nullptr)
        {
            player_->stopPlayback("rebind");
        }
        externalPattern_ = nullptr;
        instrumentTrackForClipBind_ = nullptr;
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
        const bool changed = !instrumentUiInitialized_ || (canPlayPattern != lastPlayGate_)
                             || (instrumentName != lastInstrumentName_);
        instrumentUiInitialized_ = true;
        lastPlayGate_ = canPlayPattern;
        lastInstrumentName_ = instrumentName;

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

        const juce::Colour labelColour =
            canPlayPattern ? juce::Colour(0xffc8c8d8) : juce::Colour(0xffffaa88);

        modeLabel_.setText(
            juce::String("Mode: Drum hits (100 ms gate) | ") + instPart + "\n"
                + "Timing: ~4 ms message timer; not sample-accurate.",
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
        bpmLabel_.setBounds(toolbar.removeFromLeft(36).reduced(0, 4));
        bpmSlider_.setBounds(toolbar.removeFromLeft(140).reduced(0, 2));
        modeLabel_.setBounds(toolbar.reduced(8, 0));

        const int rollH = (ExperimentalPianoRollView::kPitchHigh - ExperimentalPianoRollView::kPitchLow + 1)
                          * ExperimentalPianoRollView::kRowHeight;
        if (auto* rv = dynamic_cast<ExperimentalPianoRollView*>(viewport_.getViewedComponent()))
        {
            rv->setSize(juce::jmax(400, a.getWidth()), rollH);
        }
        viewport_.setBounds(a);
        if (auto* rv = dynamic_cast<ExperimentalPianoRollView*>(viewport_.getViewedComponent()))
        {
            ExperimentalMidiPatternPlayer::writeMidiEditorLogLine(
                "midi-editor: resized viewport=" + viewport_.getBounds().toString() + " roll="
                + rv->getBounds().toString());
        }
    }

private:
    InstrumentTrackController* instrumentTrackForClipBind_ = nullptr;

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
            "midi-editor: rebuild roll begin activePatternPtr=" + ptrToLog(&ap));

        auto* roll = new ExperimentalPianoRollView(ap, player_.get());
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

    juce::TextButton playButton_;
    juce::TextButton stopButton_;
    juce::Label bpmLabel_;
    juce::Slider bpmSlider_;
    juce::Label stepsLabel_;
    juce::ComboBox stepsBox_;
    juce::Label modeLabel_;
    juce::Viewport viewport_;
};

ExperimentalMidiEditorWindow::ExperimentalMidiEditorWindow(ExperimentalInstrumentHost& host)
    : DocumentWindow("I2 MIDI editor (Drum hits)",
                     juce::Desktop::getInstance().getDefaultLookAndFeel().findColour(
                         juce::ResizableWindow::backgroundColourId),
                     DocumentWindow::closeButton)
{
    setUsingNativeTitleBar(true);
    setContentOwned(new Body(host), true);
    centreWithSize(kEditorTotalWidth, kEditorTotalHeight);
    setResizable(true, true);
    setResizeLimits(520, 400, 2000, 1200);
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
                                                       const juce::String& titleSuffix,
                                                       InstrumentTrackController* instrumentTrackForClip)
{
    ExperimentalMidiPatternPlayer::writeMidiEditorLogLine(
        "midi-editor: bindExternalPattern begin patternPtr=" + ptrToLog(pattern) + " noteCount="
        + juce::String(pattern != nullptr ? (int)pattern->notes.size() : -1));

    if (auto* b = dynamic_cast<Body*>(getContentComponent()))
    {
        b->bindExternal(pattern, instrumentTrackForClip);
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
