#include "ui/InspectorView.h"

#include "domain/Session.h"
#include "domain/SessionSnapshot.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <cmath>
#include <cctype>
#include <string>

namespace
{
    constexpr int kDbValueFieldWidth = 39;
    constexpr int kGapValueToDbSuffix = 4;
    constexpr int kDbUnitLabelWidth = 22;
    constexpr int kDbValueFieldHeight = 24;
    constexpr double kVolumeMinDb = -60.0;
    constexpr double kVolumeMaxDb = 6.0;
    constexpr float kGainDriftEps = 5.0e-5f;

    [[nodiscard]] juce::String utf8InfinityChar()
    {
        return juce::String(juce::CharPointer_UTF8("\xe2\x88\x9e"));
    }

    [[nodiscard]] juce::String stripDbUnitSuffix(juce::String s)
    {
        s = s.trim();
        if (s.endsWithIgnoreCase("db"))
            s = s.substring(0, s.length() - 2).trimEnd();
        return s.trim();
    }

    [[nodiscard]] bool isNegativeInfinityText(const juce::String& stripped)
    {
        const juce::String t = stripped.trim();
        juce::String compact = t.toLowerCase();
        compact = compact.removeCharacters(" \t");
        if (compact == "-inf" || compact == "-infinity")
            return true;
        // User may paste the Unicode minus-infinity glyph; still maps to silent gain.
        const juce::String inf = utf8InfinityChar();
        return t == (juce::String("-") + inf);
    }

    /// Value fragment only — static "dB" label is beside the [`TextEditor`].
    [[nodiscard]] juce::String formatLinearGainToValueFieldOnly(const float linearGain)
    {
        if (linearGain <= 0.f)
        {
            return juce::String("-Inf");
        }
        const float db = juce::Decibels::gainToDecibels(
            linearGain, static_cast<float>(kVolumeMinDb));
        const double d = juce::jlimit(kVolumeMinDb, kVolumeMaxDb, static_cast<double>(db));

        const bool zeroish = std::fabs(d) <= 0.00005;

        juce::String digits;
        if (zeroish)
            digits = juce::String("0.0");
        else if (d < -0.00005)
            digits = juce::String(d, 1);
        else
            digits << "+" << juce::String(d, 1);

        return digits;
    }

    [[nodiscard]] bool tryParseCommittedText(const juce::String raw, float& outLinearGain)
    {
        const juce::String strippedUnit = stripDbUnitSuffix(raw);
        if (strippedUnit.isEmpty())
            return false;

        if (isNegativeInfinityText(strippedUnit))
        {
            outLinearGain = 0.f;
            return true;
        }

        const std::string buf = strippedUnit.toStdString();
        char* endPtr = nullptr;
        const double v = std::strtod(buf.c_str(), &endPtr);
        if (endPtr == buf.c_str())
            return false;
        while (endPtr != buf.c_str() + buf.size()
               && std::isspace(static_cast<unsigned char>(*endPtr)))
        {
            ++endPtr;
        }
        if (endPtr != buf.c_str() + buf.size())
            return false;

        double db = v;
        if (db < kVolumeMinDb)
            db = kVolumeMinDb;
        if (db > kVolumeMaxDb)
            db = kVolumeMaxDb;

        if (db <= kVolumeMinDb + 1.0e-9)
        {
            outLinearGain = 0.f;
            return true;
        }

        outLinearGain = juce::Decibels::decibelsToGain(static_cast<float>(db),
                                                        static_cast<float>(kVolumeMinDb));
        return true;
    }

    [[nodiscard]] float linearGainImpliedByEditorText(juce::String raw)
    {
        float lin = -1.f;
        if (!tryParseCommittedText(std::move(raw), lin))
            return -1.f;
        return lin;
    }


#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996)
#endif
    [[nodiscard]] juce::String elideTextToFitWidth(juce::String text,
                                                     const juce::Font& font,
                                                     float maxWidthPx)
    {
        text = text.trim();
        if (text.isEmpty())
            return {};

        if (font.getStringWidthFloat(text) <= maxWidthPx)
            return text;

        static const juce::String ellipsis("...");
        const float ellipsisW = font.getStringWidthFloat(ellipsis);
        if (maxWidthPx <= ellipsisW)
            return ellipsis;

        const float budget = maxWidthPx - ellipsisW;
        for (int len = text.length(); len > 0; --len)
        {
            const juce::String prefix = text.substring(0, len);
            if (font.getStringWidthFloat(prefix) <= budget)
                return prefix + ellipsis;
        }
        return ellipsis;
    }
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
}

InspectorView::InspectorView(Session& session)
    : session_(session)
{
    sectionTitleLabel_.setText("Inspector", juce::dontSendNotification);
    sectionTitleLabel_.setFont(
        juce::Font(juce::FontOptions{}.withHeight(13.5f)).boldened());
    addAndMakeVisible(sectionTitleLabel_);

    activeTrackTitleLabel_.setJustificationType(juce::Justification::centredLeft);
    activeTrackTitleLabel_.setFont(juce::FontOptions(13.0f));
    addAndMakeVisible(activeTrackTitleLabel_);

    channelVolumeCaptionLabel_.setText("Channel volume", juce::dontSendNotification);
    channelVolumeCaptionLabel_.setFont(juce::FontOptions(11.0f));
    addAndMakeVisible(channelVolumeCaptionLabel_);

    channelVolumeDbUnitLabel_.setText("dB", juce::dontSendNotification);
    channelVolumeDbUnitLabel_.setFont(juce::FontOptions(12.0f));
    channelVolumeDbUnitLabel_.setJustificationType(juce::Justification::centredLeft);
    channelVolumeDbUnitLabel_.setInterceptsMouseClicks(false, false);
    addAndMakeVisible(channelVolumeDbUnitLabel_);

    channelVolumeDbEditor_.setMultiLine(false);
    channelVolumeDbEditor_.setReturnKeyStartsNewLine(false);
    channelVolumeDbEditor_.setFont(juce::FontOptions(12.0f));
    channelVolumeDbEditor_.setJustification(juce::Justification::centred);
    channelVolumeDbEditor_.setIndents(0, 4);
    channelVolumeDbEditor_.setCaretVisible(true);
    channelVolumeDbEditor_.addListener(this);
    addAndMakeVisible(channelVolumeDbEditor_);

    insertsSectionLabel_.setText("Inserts", juce::dontSendNotification);
    insertsSectionLabel_.setFont(juce::FontOptions(11.0f));
    addAndMakeVisible(insertsSectionLabel_);

    addInsertButton_.setButtonText("+ Add insert");
    addInsertButton_.onClick = [this] {
        const TrackId t = session_.getActiveTrackId();
        if (t == kInvalidTrackId || !pluginHost_.requestAdd)
        {
            return;
        }
        pluginHost_.requestAdd(t);
    };
    addAndMakeVisible(addInsertButton_);

    insertSlotNameLabel_.setJustificationType(juce::Justification::centredLeft);
    insertSlotNameLabel_.setFont(juce::FontOptions(12.0f));
    insertSlotNameLabel_.setInterceptsMouseClicks(false, false);
    addAndMakeVisible(insertSlotNameLabel_);

    editPluginButton_.setButtonText("Edit");
    editPluginButton_.onClick = [this] {
        const TrackId t = session_.getActiveTrackId();
        if (t == kInvalidTrackId || !pluginHost_.requestEdit)
        {
            return;
        }
        pluginHost_.requestEdit(t);
    };
    addAndMakeVisible(editPluginButton_);

    removePluginButton_.setButtonText("Remove");
    removePluginButton_.onClick = [this] {
        const TrackId t = session_.getActiveTrackId();
        if (t == kInvalidTrackId || !pluginHost_.requestRemove)
        {
            return;
        }
        pluginHost_.requestRemove(t);
        refreshFromSession();
    };
    addAndMakeVisible(removePluginButton_);

    refreshFromSession();
}

void InspectorView::setVolumeEditorTextFromLinearGain(const float linearGain)
{
    channelVolumeDbEditor_.setText(formatLinearGainToValueFieldOnly(linearGain),
                                   juce::dontSendNotification);
}

void InspectorView::updateElidedTrackTitleDisplay()
{
    if (activeTrackPlainName_.isEmpty())
    {
        activeTrackTitleLabel_.setTooltip({});
        activeTrackTitleLabel_.setText({}, juce::dontSendNotification);
        return;
    }

    const float w = static_cast<float>(activeTrackTitleLabel_.getWidth());
    if (w <= 1.f)
    {
        activeTrackTitleLabel_.setTooltip(activeTrackPlainName_);
        activeTrackTitleLabel_.setText(activeTrackPlainName_, juce::dontSendNotification);
        return;
    }

    const juce::Font font = activeTrackTitleLabel_.getFont();
    const float maxW = w - 2.f;
    const juce::String shown = elideTextToFitWidth(activeTrackPlainName_, font, maxW);
    activeTrackTitleLabel_.setTooltip(activeTrackPlainName_);
    activeTrackTitleLabel_.setText(shown, juce::dontSendNotification);
}

void InspectorView::syncInsertsWhenInspectorDisabled()
{
    insertsSectionLabel_.setVisible(true);
    addInsertButton_.setVisible(true);
    addInsertButton_.setEnabled(false);
    insertSlotNameLabel_.setVisible(false);
    editPluginButton_.setVisible(false);
    removePluginButton_.setVisible(false);
}

void InspectorView::syncInsertsNoActiveTrack()
{
    insertsSectionLabel_.setVisible(true);
    addInsertButton_.setVisible(true);
    addInsertButton_.setEnabled(false);
    insertSlotNameLabel_.setVisible(false);
    editPluginButton_.setVisible(false);
    removePluginButton_.setVisible(false);
}

void InspectorView::syncInsertsForActiveTrack(const TrackId active)
{
    if (active == kInvalidTrackId)
    {
        syncInsertsNoActiveTrack();
        return;
    }
    insertsSectionLabel_.setVisible(true);
    const bool hasPlugin = pluginHost_.hasPlugin && pluginHost_.hasPlugin(active);
    juce::String pname;
    if (hasPlugin && pluginHost_.displayName)
    {
        pname = pluginHost_.displayName(active);
    }

    addInsertButton_.setVisible(!hasPlugin);
    addInsertButton_.setEnabled(!hasPlugin && (pluginHost_.requestAdd != nullptr));
    insertSlotNameLabel_.setVisible(hasPlugin);
    editPluginButton_.setVisible(hasPlugin);
    removePluginButton_.setVisible(hasPlugin);
    if (hasPlugin)
    {
        insertSlotNameLabel_.setText("1  " + pname, juce::dontSendNotification);
        editPluginButton_.setEnabled(pluginHost_.requestEdit != nullptr);
        removePluginButton_.setEnabled(pluginHost_.requestRemove != nullptr);
    }
}

void InspectorView::commitVolumeField()
{
    const std::shared_ptr<const SessionSnapshot> snap = session_.loadSessionSnapshotForAudioThread();
    if (snap == nullptr || snap->getNumTracks() <= 0)
        return;

    const TrackId active = session_.getActiveTrackId();
    const int idx = snap->findTrackIndexById(active);
    if (idx < 0)
        return;

    const float snapGain = snap->getTrack(idx).getChannelFaderGain();

    float parsedLinear = snapGain;
    if (!tryParseCommittedText(channelVolumeDbEditor_.getText(), parsedLinear))
    {
        setVolumeEditorTextFromLinearGain(snapGain);
        return;
    }

    session_.setTrackChannelFaderGain(active, parsedLinear);

    const std::shared_ptr<const SessionSnapshot> after = session_.loadSessionSnapshotForAudioThread();
    if (after != nullptr && after->findTrackIndexById(active) >= 0)
    {
        const int ix = after->findTrackIndexById(active);
        setVolumeEditorTextFromLinearGain(after->getTrack(ix).getChannelFaderGain());
    }
    else
    {
        setVolumeEditorTextFromLinearGain(parsedLinear);
    }
}

void InspectorView::refreshFromSession()
{
    const std::shared_ptr<const SessionSnapshot> snap = session_.loadSessionSnapshotForAudioThread();
    if (snap == nullptr || snap->getNumTracks() <= 0)
    {
        setEnabled(false);
        activeTrackPlainName_.clear();
        activeTrackTitleLabel_.setText("—", juce::dontSendNotification);
        activeTrackTitleLabel_.setTooltip({});
        channelVolumeDbEditor_.setText({}, juce::dontSendNotification);
        syncInsertsWhenInspectorDisabled();
        return;
    }
    setEnabled(true);

    const TrackId active = session_.getActiveTrackId();
    const int idx = snap->findTrackIndexById(active);
    if (idx < 0)
    {
        activeTrackPlainName_.clear();
        activeTrackTitleLabel_.setText("(no active track)", juce::dontSendNotification);
        activeTrackTitleLabel_.setTooltip({});
        channelVolumeDbEditor_.setText({}, juce::dontSendNotification);
        syncInsertsNoActiveTrack();
        return;
    }
    const Track& tr = snap->getTrack(idx);
    activeTrackPlainName_ = tr.getName();
    updateElidedTrackTitleDisplay();

    const float snapGain = tr.getChannelFaderGain();
    const bool switchedTrack = (lastShownTrackId_ != active);
    lastShownTrackId_ = active;

    syncInsertsForActiveTrack(active);

    if (channelVolumeDbEditor_.hasKeyboardFocus(false) && !switchedTrack)
        return;

    if (switchedTrack)
    {
        setVolumeEditorTextFromLinearGain(snapGain);
        return;
    }

    const float implied = linearGainImpliedByEditorText(channelVolumeDbEditor_.getText());
    if (implied < 0.f
        || std::fabs((double)implied - (double)snapGain)
               > static_cast<double>(kGainDriftEps))
    {
        setVolumeEditorTextFromLinearGain(snapGain);
    }
}

void InspectorView::resized()
{
    auto area = getLocalBounds().reduced(4);
    sectionTitleLabel_.setBounds(area.removeFromTop(20));
    activeTrackTitleLabel_.setBounds(area.removeFromTop(22));
    area.removeFromTop(4);

    channelVolumeCaptionLabel_.setBounds(area.removeFromTop(18));
    area.removeFromTop(2);
    auto fieldRow = area.removeFromTop(juce::jmax(26, kDbValueFieldHeight));
    const int xStart = fieldRow.getX();
    const int yy = fieldRow.getY();

    channelVolumeDbEditor_.setBounds(xStart, yy, kDbValueFieldWidth, kDbValueFieldHeight);
    channelVolumeDbUnitLabel_.setBounds(xStart + kDbValueFieldWidth + kGapValueToDbSuffix, yy,
                                        kDbUnitLabelWidth, kDbValueFieldHeight);

    area.removeFromTop(10);
    insertsSectionLabel_.setBounds(area.removeFromTop(18));
    area.removeFromTop(4);
    constexpr int kInsertButtonH = 24;
    if (addInsertButton_.isVisible())
    {
        addInsertButton_.setBounds(area.removeFromTop(kInsertButtonH));
    }
    else
    {
        insertSlotNameLabel_.setBounds(area.removeFromTop(20));
        area.removeFromTop(4);
        auto btnRow = area.removeFromTop(kInsertButtonH);
        constexpr int kBtnGap = 4;
        const int half = (btnRow.getWidth() - kBtnGap) / 2;
        editPluginButton_.setBounds(btnRow.removeFromLeft(half));
        btnRow.removeFromLeft(kBtnGap);
        removePluginButton_.setBounds(btnRow);
    }

    updateElidedTrackTitleDisplay();
}

void InspectorView::textEditorReturnKeyPressed(juce::TextEditor& editor)
{
    if (&editor == &channelVolumeDbEditor_)
        commitVolumeField();
}

void InspectorView::textEditorEscapeKeyPressed(juce::TextEditor& editor)
{
    if (&editor != &channelVolumeDbEditor_)
        return;

    const std::shared_ptr<const SessionSnapshot> snap = session_.loadSessionSnapshotForAudioThread();
    if (snap == nullptr || snap->getNumTracks() <= 0)
        return;

    const TrackId active = session_.getActiveTrackId();
    const int idx = snap->findTrackIndexById(active);
    if (idx < 0)
        return;

    setVolumeEditorTextFromLinearGain(snap->getTrack(idx).getChannelFaderGain());
}

void InspectorView::textEditorFocusLost(juce::TextEditor& editor)
{
    if (&editor == &channelVolumeDbEditor_)
        commitVolumeField();
}
