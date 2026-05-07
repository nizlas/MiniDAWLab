#include "ExperimentalMidiEditorWindow.h"
#include "ExperimentalMidiPattern.h"
#include "ExperimentalMidiPatternPlayer.h"
#include "ExperimentalPianoRollView.h"
#include "plugins/ExperimentalInstrumentHost.h"

#include <algorithm>

namespace
{
    constexpr int kToolbarH = 40;
    constexpr int kEditorTotalWidth = 720;
    constexpr int kEditorTotalHeight = 780;
} // namespace

class ExperimentalMidiEditorWindow::Body final : public juce::Component
{
public:
    explicit Body(ExperimentalInstrumentHost& hostIn)
        : player_(std::make_unique<ExperimentalMidiPatternPlayer>(hostIn, pattern_))
    {
        pattern_.numSteps = 16;
        pattern_.stepDenom = 16;
        pattern_.bpm = 110.0;
        pattern_.loop = true;

        addAndMakeVisible(playButton_);
        playButton_.setButtonText("Play pattern");
        playButton_.onClick = [this] {
            player_->startPlayback();
        };

        addAndMakeVisible(stopButton_);
        stopButton_.setButtonText("Stop");
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
            pattern_.bpm = bpmSlider_.getValue();
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
            if (newSteps == pattern_.numSteps)
            {
                return;
            }
            pattern_.numSteps = newSteps;
            pattern_.notes.erase(
                std::remove_if(
                    pattern_.notes.begin(),
                    pattern_.notes.end(),
                    [newSteps](const PrototypeMidiNote& n) { return n.step >= newSteps; }),
                pattern_.notes.end());
            if (auto* rv = dynamic_cast<ExperimentalPianoRollView*>(viewport_.getViewedComponent()))
            {
                rv->repaint();
            }
        };

        addAndMakeVisible(modeLabel_);
        modeLabel_.setText(
            "Mode: Drum hits (100 ms gate)\n"
            "Timing is message-thread / ~4 ms — not sample-accurate.",
            juce::dontSendNotification);
        modeLabel_.setFont(juce::FontOptions(11.0f));
        modeLabel_.setJustificationType(juce::Justification::centredLeft);
        modeLabel_.setColour(juce::Label::textColourId, juce::Colour(0xffc8c8d8));

        auto* roll = new ExperimentalPianoRollView(pattern_, player_.get());
        viewport_.setViewedComponent(roll, true);
        addAndMakeVisible(viewport_);

        ExperimentalMidiPatternPlayer::writeMidiEditorLogLine(
            "midi-editor: window opened steps=" + juce::String(pattern_.numSteps) + " bpm="
            + juce::String(pattern_.bpm, 2));
    }

    ~Body() override
    {
        player_->stopPlayback("window-closed");
    }

    void stopForHostUnload()
    {
        player_->stopPlayback("instrument-unloaded");
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
    }

private:
    ExperimentalMidiPattern pattern_;
    std::unique_ptr<ExperimentalMidiPatternPlayer> player_;

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

void ExperimentalMidiEditorWindow::notifyInstrumentUnloaded()
{
    if (auto* b = dynamic_cast<Body*>(getContentComponent()))
    {
        b->stopForHostUnload();
    }
}
