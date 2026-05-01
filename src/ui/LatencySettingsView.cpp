#include "ui/LatencySettingsView.h"

#include "audio/LatencySettingsStore.h"
#include "engine/PlaybackEngine.h"

#include <cerrno>
#include <cmath>
#include <cstring>
#include <optional>

namespace
{
    constexpr double kClampAbsMsEditor = 500.0;

    [[nodiscard]] std::optional<double> parseFiniteDoubleTrimmed(const juce::String& s)
    {
        const juce::String t = s.trim();
        if (t.isEmpty())
        {
            return std::nullopt;
        }
        const double d = t.getDoubleValue();
        if (!std::isfinite(d))
        {
            return std::nullopt;
        }
        return d;
    }

    [[nodiscard]] std::optional<std::int64_t> parseInt64SamplesTrimmed(const juce::String& s)
    {
        const juce::String t = s.trim();
        if (t.isEmpty())
        {
            return std::nullopt;
        }
        const char* const utf = t.toRawUTF8();
        if (utf == nullptr || *utf == 0)
        {
            return std::nullopt;
        }
        char* endPtr = nullptr;
        errno = 0;
        const long long v = std::strtoll(utf, &endPtr, 10);
        if (errno == ERANGE)
        {
            return std::nullopt;
        }
        if (endPtr == utf)
        {
            return std::nullopt;
        }
        while (*endPtr != 0 && std::isspace(static_cast<unsigned char>(*endPtr)))
        {
            ++endPtr;
        }
        if (*endPtr != 0)
        {
            return std::nullopt;
        }
        return static_cast<std::int64_t>(v);
    }

    [[nodiscard]] juce::String millisFromSamplesFormatted(std::int64_t samp, double sr)
    {
        if (!(sr > 0.0))
        {
            return "-";
        }
        const double ms = 1000.0 * static_cast<double>(samp) / sr;
        if (!std::isfinite(ms))
        {
            return "-";
        }
        return juce::String(ms, 4);
    }

    [[nodiscard]] juce::String latencyHumanLine(int latencySamples,
                                                  double sr,
                                                  const char* unknownLabel)
    {
        if (latencySamples < 0)
        {
            return unknownLabel;
        }
        const juce::String ms = millisFromSamplesFormatted(
            static_cast<std::int64_t>(latencySamples), sr);
        return juce::String(latencySamples) + " samp, " + ms + " ms";
    }

    void placePairRow(juce::Rectangle<int>& bounds,
                      juce::Component& title,
                      juce::Component& field,
                      int rowH)
    {
        auto row = bounds.removeFromTop(rowH);
        title.setBounds(row.removeFromLeft(150).withHeight(rowH));
        field.setBounds(row.withHeight(rowH));
    }
} // namespace

LatencySettingsView::LatencySettingsView(LatencySettingsStore& store, PlaybackEngine& playbackEngine)
    : store_(store)
    , playbackEngine_(playbackEngine)
{
    addAndMakeVisible(latencyGroup_);
    latencyGroup_.setText("Latency / Timing");

    reportedRateLabel_ = std::make_unique<juce::Label>("", "(no device)");
    reportedBufLabel_ = std::make_unique<juce::Label>("", juce::String());
    reportedInLabel_ = std::make_unique<juce::Label>("", juce::String());
    reportedOutLabel_ = std::make_unique<juce::Label>("", juce::String());
    recExplainerLabel_ = std::make_unique<juce::Label>(
        "latencyRecExpl",
        "Recorded clips shift on the timeline by the placement offset below. Default is -(reported input latency).");

    auto styleReadOnlyCaption = [&](juce::Label& l)
    {
        l.setJustificationType(juce::Justification::centredLeft);
        l.setMinimumHorizontalScale(1.0f);
        l.setEditable(false);
    };

    reportedRateLabel_->setJustificationType(juce::Justification::centredLeft);
    reportedBufLabel_->setJustificationType(juce::Justification::centredLeft);
    reportedInLabel_->setJustificationType(juce::Justification::centredLeft);
    reportedOutLabel_->setJustificationType(juce::Justification::centredLeft);
    styleReadOnlyCaption(*recExplainerLabel_);
    recExplainerLabel_->setFont(juce::FontOptions(13.0f));

    auto makeEditorRow = [&](const char* cap,
                             std::unique_ptr<juce::Label>& ttl,
                             std::unique_ptr<juce::TextEditor>& ed,
                             bool integerSamplesOnly)
    {
        ttl = std::make_unique<juce::Label>("rowCap", cap);
        ttl->setJustificationType(juce::Justification::centredRight);
        ed = std::make_unique<juce::TextEditor>();
        ed->setColour(juce::TextEditor::focusedOutlineColourId, juce::Colours::orange);
        if (integerSamplesOnly)
            ed->setInputRestrictions(40, "-0123456789");
        ed->addListener(this);
        addAndMakeVisible(*ttl);
        addAndMakeVisible(*ed);
    };

    makeEditorRow("Effective placement (samples)", recSamplesTitle_, recSamples_, true);
    makeEditorRow("Effective placement (ms)", recMsTitle_, recMs_, false);

    makeEditorRow("Playback offset (samples)", playSamplesTitle_, playSamples_, true);
    makeEditorRow("Playback offset (ms)", playMsTitle_, playMs_, false);

    recResetMinusReportedIn_
        = std::make_unique<juce::TextButton>("Set to -reported input latency");
    playResetZero_ = std::make_unique<juce::TextButton>("Reset to 0");
    playSetReportedOut_ = std::make_unique<juce::TextButton>("Set to reported output latency");
    lowLatencyPlaceholder_ = std::make_unique<juce::ToggleButton>(
        "Low latency recording mode (not yet supported)");
    infoButton_ = std::make_unique<juce::TextButton>("?");

    for (auto* b : { recResetMinusReportedIn_.get(), playResetZero_.get(),
                     playSetReportedOut_.get(), infoButton_.get() })
    {
        addAndMakeVisible(*b);
        b->addListener(this);
    }

    addAndMakeVisible(*lowLatencyPlaceholder_);
    lowLatencyPlaceholder_->setEnabled(false);

    addAndMakeVisible(*reportedRateLabel_);
    addAndMakeVisible(*reportedBufLabel_);
    addAndMakeVisible(*reportedInLabel_);
    addAndMakeVisible(*reportedOutLabel_);
    addAndMakeVisible(*recExplainerLabel_);

    setSize(600, 360);
}

void LatencySettingsView::syncFromStore()
{
    suppressCommits_ = true;
    store_.refreshFromCurrentDevice();

    reportedRateLabel_->setFont(juce::FontOptions(13.0f));

    double sr = store_.getCurrentSampleRate();

    reportedRateLabel_->setText(juce::String("Current sample rate: ")
                                    + (sr > 0.0 ? juce::String(sr, 4) + " Hz" : "Not available"),
                                juce::dontSendNotification);
    reportedBufLabel_->setText(
        juce::String("Buffer size: ")
            + ((store_.getCurrentBufferSizeSamples() > 0)
                   ? juce::String(store_.getCurrentBufferSizeSamples()) + " samples"
                   : juce::String("Not available")),
        juce::dontSendNotification);
    reportedInLabel_->setText(
        juce::String("Reported input latency: ")
            + latencyHumanLine(store_.getReportedInputLatencySamples(),
                               sr > 0.0 ? sr : 44100.0,
                               "unknown"),
        juce::dontSendNotification);
    reportedOutLabel_->setText(
        juce::String("Reported output latency: ")
            + latencyHumanLine(store_.getReportedOutputLatencySamples(),
                               sr > 0.0 ? sr : 44100.0,
                               "unknown"),
        juce::dontSendNotification);

    const auto recSamples = store_.getCurrentRecordingOffsetSamples();
    recSamples_->setText(juce::String(recSamples), juce::dontSendNotification);

    playSamples_->setText(juce::String(store_.getCurrentPlaybackOffsetSamples()),
                          juce::dontSendNotification);

    if (!(sr > 0.0))
    {
        recMs_->clear();
        playMs_->clear();
        recMs_->setReadOnly(true);
        playMs_->setReadOnly(true);
        recMs_->setCaretVisible(false);
        playMs_->setCaretVisible(false);
        const auto hint = juce::String("Ms editing needs a valid sample rate (+- ")
                          + juce::String(static_cast<int>(kClampAbsMsEditor)) + " ms clamp).";
        const juce::Colour ph = juce::Colours::grey;
        recMs_->setTextToShowWhenEmpty(hint, ph);
        playMs_->setTextToShowWhenEmpty(hint, ph);
    }
    else
    {
        recMs_->setReadOnly(false);
        playMs_->setReadOnly(false);
        recMs_->setTextToShowWhenEmpty({}, juce::Colours::grey);
        playMs_->setTextToShowWhenEmpty({}, juce::Colours::grey);
        recMs_->setText(millisFromSamplesFormatted(recSamples, sr), juce::dontSendNotification);
        playMs_->setText(
            millisFromSamplesFormatted(store_.getCurrentPlaybackOffsetSamples(), sr),
            juce::dontSendNotification);
    }

    suppressCommits_ = false;
}

bool LatencySettingsView::sampleRateKnown() const noexcept
{
    return store_.getCurrentSampleRate() > 0.0;
}

void LatencySettingsView::commitRecordingFromSamplesEditor(bool focusNext)
{
    if (suppressCommits_)
        return;
    if (auto v = parseInt64SamplesTrimmed(recSamples_->getText()); v.has_value())
    {
        store_.setCurrentRecordingOffsetSamples(*v);
        if (sampleRateKnown())
        {
            suppressCommits_ = true;
            recMs_->setText(
                millisFromSamplesFormatted(store_.getCurrentRecordingOffsetSamples(),
                                           store_.getCurrentSampleRate()),
                juce::dontSendNotification);
            suppressCommits_ = false;
        }
    }
    else
        syncFromStore();
    juce::ignoreUnused(focusNext);
}

void LatencySettingsView::commitRecordingFromMsEditor(bool focusNext)
{
    if (suppressCommits_)
        return;
    const double sr = store_.getCurrentSampleRate();
    if (!(sr > 0.0))
    {
        syncFromStore();
        return;
    }
    auto dOpt = parseFiniteDoubleTrimmed(recMs_->getText());
    if (!dOpt.has_value())
    {
        syncFromStore();
        return;
    }
    double d = *dOpt;
    d = juce::jlimit(-kClampAbsMsEditor, kClampAbsMsEditor, d);
    const auto samp = static_cast<std::int64_t>(std::llround(sr * (d / 1000.0)));

    suppressCommits_ = true;
    recMs_->setText(juce::String(d, 4), juce::dontSendNotification);
    suppressCommits_ = false;

    store_.setCurrentRecordingOffsetSamples(samp);
    suppressCommits_ = true;
    recSamples_->setText(juce::String(store_.getCurrentRecordingOffsetSamples()),
                         juce::dontSendNotification);
    suppressCommits_ = false;
    juce::ignoreUnused(focusNext);
}

void LatencySettingsView::commitPlaybackFromSamplesEditor(bool focusNext)
{
    if (suppressCommits_)
        return;
    if (auto v = parseInt64SamplesTrimmed(playSamples_->getText()); v.has_value())
    {
        store_.setCurrentPlaybackOffsetSamples(*v);
        playbackEngine_.setPlaybackOffsetSamples(*v);
        if (sampleRateKnown())
        {
            suppressCommits_ = true;
            playMs_->setText(
                millisFromSamplesFormatted(store_.getCurrentPlaybackOffsetSamples(),
                                           store_.getCurrentSampleRate()),
                juce::dontSendNotification);
            suppressCommits_ = false;
        }
    }
    else
        syncFromStore();
    juce::ignoreUnused(focusNext);
}

void LatencySettingsView::commitPlaybackFromMsEditor(bool focusNext)
{
    if (suppressCommits_)
        return;
    const double sr = store_.getCurrentSampleRate();
    if (!(sr > 0.0))
    {
        syncFromStore();
        return;
    }
    auto dOpt = parseFiniteDoubleTrimmed(playMs_->getText());
    if (!dOpt.has_value())
    {
        syncFromStore();
        return;
    }
    double d = *dOpt;
    d = juce::jlimit(-kClampAbsMsEditor, kClampAbsMsEditor, d);

    const auto samp = static_cast<std::int64_t>(std::llround(sr * (d / 1000.0)));

    suppressCommits_ = true;
    playMs_->setText(juce::String(d, 4), juce::dontSendNotification);
    suppressCommits_ = false;

    store_.setCurrentPlaybackOffsetSamples(samp);
    playbackEngine_.setPlaybackOffsetSamples(samp);

    suppressCommits_ = true;
    playSamples_->setText(juce::String(store_.getCurrentPlaybackOffsetSamples()),
                          juce::dontSendNotification);
    suppressCommits_ = false;
    juce::ignoreUnused(focusNext);
}

void LatencySettingsView::textEditorReturnKeyPressed(juce::TextEditor& editor)
{
    if (&editor == recSamples_.get())
        commitRecordingFromSamplesEditor(true);
    else if (&editor == recMs_.get())
        commitRecordingFromMsEditor(true);
    else if (&editor == playSamples_.get())
        commitPlaybackFromSamplesEditor(true);
    else if (&editor == playMs_.get())
        commitPlaybackFromMsEditor(true);
}

void LatencySettingsView::textEditorEscapeKeyPressed(juce::TextEditor&)
{
    syncFromStore();
}

void LatencySettingsView::textEditorFocusLost(juce::TextEditor& editor)
{
    textEditorReturnKeyPressed(editor);
}

void LatencySettingsView::buttonClicked(juce::Button* b)
{
    if (b == recResetMinusReportedIn_.get())
    {
        store_.resetRecordingToMinusReportedInput();
    }
    else if (b == playResetZero_.get())
    {
        store_.resetPlaybackToZero();
        playbackEngine_.setPlaybackOffsetSamples(0);
    }
    else if (b == playSetReportedOut_.get())
    {
        store_.setPlaybackToReportedOutputLatency();
        playbackEngine_.setPlaybackOffsetSamples(store_.getCurrentPlaybackOffsetSamples());
    }
    else if (b == infoButton_.get())
    {
        showLatencyInfoBox();
        return;
    }
    syncFromStore();
}

void LatencySettingsView::showLatencyInfoBox()
{
    constexpr const char* kBody
        = "1. Live monitoring latency\r\n\r\n"
          "This is the delay between playing/singing into the input and hearing yourself through "
          "the computer's audio pipeline. It depends on buffer size, driver behavior, routing, "
          "and plugins. These offset controls do not make live monitoring faster.\r\n\r\n"
          "2. Recording placement compensation\r\n\r\n"
          "After each take finishes, committed clips shift earlier or later on the timeline by the "
          "placement offset samples. Your WAV recording is untouched; playback while recording isn't altered. "
          "The usual default equals negative reported input latency, compensating for audio arriving slightly late "
          "from the hardware/driver.\r\n\r\n"
          "3. Playback audible alignment offset\r\n\r\n"
          "Reads session audio from a shifted timeline sample relative to the visible playhead. Default is zero. "
          "Positive offsets read farther ahead on the timeline; negative offsets read earlier. Useful for aligning "
          "what you hear with what you see. Large swings can detach audio from the visual playhead, so keep values "
          "small unless you are experimenting.\r\n\r\n"
          "4. Low latency recording mode\r\n\r\n"
          "Inspired by constrained-delay / low-latency monitoring workflows in commercial DAWs. It is planned as "
          "future work; this preview build does not change processing yet.";

    juce::AlertWindow::showMessageBoxAsync(
        juce::AlertWindow::InfoIcon, "Latency / timing", juce::String(kBody));
}

void LatencySettingsView::resized()
{
    auto area = getLocalBounds().reduced(8);
    latencyGroup_.setBounds(area);

    auto bounds = latencyGroup_.getBounds().reduced(14, 24);
    const int lh = juce::jmax(21, proportionOfHeight(0.058f));

    auto headRow = bounds.removeFromTop(lh + 2);
    infoButton_->setBounds(headRow.removeFromRight(34).withHeight(lh));

    bounds.removeFromTop(2);
    reportedRateLabel_->setBounds(bounds.removeFromTop(lh));
    reportedBufLabel_->setBounds(bounds.removeFromTop(lh));
    reportedInLabel_->setBounds(bounds.removeFromTop(lh));
    reportedOutLabel_->setBounds(bounds.removeFromTop(lh));

    bounds.removeFromTop(6);
    const int explainH = juce::jmax(lh + lh / 2, 36);
    recExplainerLabel_->setBounds(bounds.removeFromTop(explainH));

    bounds.removeFromTop(6);
    placePairRow(bounds, *recSamplesTitle_, *recSamples_, lh);
    bounds.removeFromTop(4);
    placePairRow(bounds, *recMsTitle_, *recMs_, lh);
    bounds.removeFromTop(6);

    recResetMinusReportedIn_->setBounds(bounds.removeFromTop(juce::jmax(24, lh + 2)));

    bounds.removeFromTop(10);
    placePairRow(bounds, *playSamplesTitle_, *playSamples_, lh);
    bounds.removeFromTop(4);
    placePairRow(bounds, *playMsTitle_, *playMs_, lh);
    bounds.removeFromTop(6);

    {
        auto row = bounds.removeFromTop(juce::jmax(26, lh + 4));
        playResetZero_->setBounds(row.removeFromLeft(112));
        row.removeFromLeft(8);
        playSetReportedOut_->setBounds(row.removeFromLeft(260));
    }

    bounds.removeFromTop(8);
    lowLatencyPlaceholder_->setBounds(bounds.removeFromTop(lh + 8));
}
