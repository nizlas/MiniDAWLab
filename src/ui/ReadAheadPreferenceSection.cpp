#include "ui/ReadAheadPreferenceSection.h"

#include "engine/ReadAheadStartupConfig.h"

ReadAheadPreferenceSection::ReadAheadPreferenceSection(juce::File preferenceFile,
                                                       const bool activeThisSession,
                                                       const bool commandLineOverride)
    : preferenceFile_(std::move(preferenceFile))
    , activeThisSession_(activeThisSession)
    , commandLineOverride_(commandLineOverride)
{
    const auto saved = readahead::loadReadAheadEnabledPreference(preferenceFile_);
    enable_.setButtonText("Enable read-ahead processing");
    enable_.setToggleState(saved.value_or(true), juce::dontSendNotification);
    enable_.onClick = [this] { onToggleClicked(); };
    addAndMakeVisible(enable_);

    help_.setText("Pre-processes eligible audio tracks to reduce real-time CPU load.",
                  juce::dontSendNotification);
    help_.setJustificationType(juce::Justification::centredLeft);
    help_.setInterceptsMouseClicks(false, false);
    addAndMakeVisible(help_);

    helpRestart_.setText("Restart DAL to apply changes.", juce::dontSendNotification);
    helpRestart_.setJustificationType(juce::Justification::centredLeft);
    helpRestart_.setInterceptsMouseClicks(false, false);
    addAndMakeVisible(helpRestart_);

    parallelNote_.setText("Parallel processing stays on either way. This setting is only read-ahead.",
                          juce::dontSendNotification);
    parallelNote_.setJustificationType(juce::Justification::centredLeft);
    parallelNote_.setMinimumHorizontalScale(0.85f);
    parallelNote_.setInterceptsMouseClicks(false, false);
    addAndMakeVisible(parallelNote_);

    activeLabel_.setJustificationType(juce::Justification::centredLeft);
    activeLabel_.setInterceptsMouseClicks(false, false);
    addAndMakeVisible(activeLabel_);

    noticeLabel_.setJustificationType(juce::Justification::centredLeft);
    noticeLabel_.setColour(juce::Label::textColourId, juce::Colour(0xffe0b040));
    noticeLabel_.setInterceptsMouseClicks(false, false);
    addAndMakeVisible(noticeLabel_);

    refreshNotices();
    setSize(640, kPreferredHeightPx);
}

void ReadAheadPreferenceSection::resized()
{
    auto area = getLocalBounds().reduced(8, 4);
    enable_.setBounds(area.removeFromTop(24));
    area.removeFromTop(2);
    help_.setBounds(area.removeFromTop(18));
    helpRestart_.setBounds(area.removeFromTop(18));
    parallelNote_.setBounds(area.removeFromTop(22));
    activeLabel_.setBounds(area.removeFromTop(20));
    noticeLabel_.setBounds(area.removeFromTop(20));
}

void ReadAheadPreferenceSection::refreshNotices()
{
    activeLabel_.setText(activeThisSession_ ? "Active this session: On" : "Active this session: Off",
                         juce::dontSendNotification);
    const bool restartRequired = enable_.getToggleState() != activeThisSession_;
    juce::String notice;
    if (restartRequired)
    {
        notice = "Restart required";
    }
    if (commandLineOverride_)
    {
        notice = notice.isEmpty() ? juce::String("Controlled by start argument.")
                                  : notice + ". Controlled by start argument.";
    }
    noticeLabel_.setText(notice, juce::dontSendNotification);
    noticeLabel_.setVisible(notice.isNotEmpty());
}

void ReadAheadPreferenceSection::onToggleClicked()
{
    const bool want = enable_.getToggleState();
    if (!readahead::saveReadAheadEnabledPreference(preferenceFile_, want))
    {
        enable_.setToggleState(!want, juce::dontSendNotification);
        noticeLabel_.setText("Could not save the setting.", juce::dontSendNotification);
        noticeLabel_.setVisible(true);
        return;
    }
    refreshNotices();
}
