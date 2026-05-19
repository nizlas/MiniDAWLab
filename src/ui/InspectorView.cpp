#include "ui/InspectorView.h"

#include "domain/Track.h"

#include "domain/Session.h"
#include "domain/SessionRouting.h"
#include "domain/SessionSnapshot.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <algorithm>
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
        const juce::String inf = utf8InfinityChar();
        return t == (juce::String("-") + inf);
    }

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

    constexpr double kSendAmountMinDb = -60.0;
    constexpr double kSendAmountMaxDb = 6.02;
    constexpr float kSendAmountDriftEps = 5.0e-5f;

    [[nodiscard]] juce::String formatSendLinearToDbField(const float amountLinear)
    {
        const float clamped = clampTrackSendAmountLinear(amountLinear);
        if (clamped <= 0.0f)
        {
            return juce::String("-Inf");
        }
        const float db = juce::Decibels::gainToDecibels(
            clamped, static_cast<float>(kSendAmountMinDb));
        const double d = juce::jlimit(kSendAmountMinDb, kSendAmountMaxDb, static_cast<double>(db));
        if (std::fabs(d) <= 0.00005)
        {
            return juce::String("0.00");
        }
        if (d > 0.00005)
        {
            return juce::String("+") + juce::String(d, 2);
        }
        return juce::String(d, 2);
    }

    [[nodiscard]] bool tryParseSendAmountText(const juce::String raw, float& outLinear)
    {
        const juce::String strippedUnit = stripDbUnitSuffix(raw);
        if (strippedUnit.isEmpty())
        {
            return false;
        }
        if (isNegativeInfinityText(strippedUnit))
        {
            outLinear = 0.0f;
            return true;
        }
        const std::string buf = strippedUnit.toStdString();
        char* endPtr = nullptr;
        const double v = std::strtod(buf.c_str(), &endPtr);
        if (endPtr == buf.c_str())
        {
            return false;
        }
        while (endPtr != buf.c_str() + buf.size()
               && std::isspace(static_cast<unsigned char>(*endPtr)))
        {
            ++endPtr;
        }
        if (endPtr != buf.c_str() + buf.size())
        {
            return false;
        }
        double db = v;
        if (db < kSendAmountMinDb)
        {
            db = kSendAmountMinDb;
        }
        if (db > kSendAmountMaxDb)
        {
            db = kSendAmountMaxDb;
        }
        if (db <= kSendAmountMinDb + 1.0e-9)
        {
            outLinear = 0.0f;
            return true;
        }
        outLinear = clampTrackSendAmountLinear(juce::Decibels::decibelsToGain(
            static_cast<float>(db), static_cast<float>(kSendAmountMinDb)));
        return true;
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
} // namespace

[[nodiscard]] static bool parseInsertRowDragDescription(const juce::var& description,
                                                       TrackId& outTid,
                                                       InsertSlotId& outSid,
                                                       InsertStage& outSrcStage)
{
    outTid = kInvalidTrackId;
    outSid = kInvalidInsertSlotId;
    outSrcStage = InsertStage::Post;

    juce::String s = description.toString().trim().unquoted();
    if (!s.startsWithIgnoreCase("insert:"))
    {
        return false;
    }

    s = s.substring(7).trim();

    juce::StringArray parts;
    parts.addTokens(s, ":", "");
    parts.trim();
    parts.removeEmptyStrings();

    if (parts.size() != 3)
    {
        return false;
    }

    const auto tid64 = parts[0].getLargeIntValue();
    const auto sid64 = parts[1].getLargeIntValue();
    if (tid64 <= 0 || sid64 <= 0)
    {
        return false;
    }

    if (parts[2].equalsIgnoreCase("pre"))
    {
        outSrcStage = InsertStage::Pre;
    }
    else if (parts[2].equalsIgnoreCase("post"))
    {
        outSrcStage = InsertStage::Post;
    }
    else
    {
        return false;
    }

    outTid = static_cast<TrackId>(tid64);
    outSid = static_cast<InsertSlotId>(sid64);
    return true;
}

[[nodiscard]] static juce::String insertRowDragDescription(const TrackId tid,
                                                          const InsertSlotId sid,
                                                          const InsertStage st)
{
    const char* const stageStr = st == InsertStage::Pre ? "pre" : "post";
    return juce::String("insert:") + juce::String(static_cast<juce::int64>(tid))
        + ":" + juce::String(static_cast<juce::int64>(sid)) + ":" + stageStr;
}

class InspectorView::StageDropTarget final : public juce::Component,
                                             public juce::DragAndDropTarget
{
public:
    StageDropTarget(InspectorView& owner, InsertStage stage)
        : owner_(owner)
        , stage_(stage)
    {
        setOpaque(false);
    }

    bool isInterestedInDragSource(const SourceDetails& dragSourceDetails) override
    {
        return owner_.isInsertRowDragPayloadAcceptedForActiveTrack(dragSourceDetails.description);
    }

    void itemDragEnter(const SourceDetails& details) override
    {
        owner_.updateInsertDragHoverFromInspectorPoint(
            owner_.getLocalPoint(this, details.localPosition));
    }

    void itemDragMove(const SourceDetails& details) override
    {
        owner_.updateInsertDragHoverFromInspectorPoint(
            owner_.getLocalPoint(this, details.localPosition));
    }

    void itemDragExit(const SourceDetails&) override
    {
        owner_.clearInsertDropHover();
    }

    void itemDropped(const SourceDetails& dragSourceDetails) override
    {
        TrackId tid = kInvalidTrackId;
        InsertSlotId sid = kInvalidInsertSlotId;
        InsertStage src = InsertStage::Post;
        if (!parseInsertRowDragDescription(dragSourceDetails.description, tid, sid, src))
        {
            return;
        }
        owner_.handleInsertDropped(
            tid, sid, src, stage_, owner_.getLocalPoint(this, dragSourceDetails.localPosition));
    }

private:
    InspectorView& owner_;
    InsertStage stage_;
};

class InspectorView::InsertSlotButton final : public juce::Component,
                                              public juce::DragAndDropTarget
{
public:
    InsertSlotButton(InspectorView& owner,
                     TrackId trackId,
                     InsertSlotId slotId,
                     InsertStage stage,
                     int displayIndex,
                     juce::String displayName)
        : owner_(owner)
        , trackId_(trackId)
        , slotId_(slotId)
        , stage_(stage)
        , displayIndex_(displayIndex)
        , displayName_(std::move(displayName))
    {
        setOpaque(false);
        setHelpText(juce::String(displayIndex_) + "  " + displayName_);
    }

    void paint(juce::Graphics& g) override
    {
        const auto bounds = getLocalBounds().toFloat().reduced(0.5f);
        constexpr float kCorner = 4.f;
        juce::Colour fill = findColour(juce::TextButton::buttonColourId);
        juce::Colour outline = findColour(juce::TextEditor::outlineColourId);
        if (!isEnabled())
        {
            fill = fill.withMultipliedAlpha(0.55f);
        }
        const bool leftDown = isMouseButtonDown() && !pendingContextMenu_;
        if (leftDown)
        {
            fill = fill.brighter(0.12f);
        }
        else if (hovered_)
        {
            fill = fill.brighter(0.06f);
        }
        g.setColour(fill);
        g.fillRoundedRectangle(bounds, kCorner);
        g.setColour(outline.withMultipliedAlpha(hovered_ ? 1.0f : 0.75f));
        g.drawRoundedRectangle(bounds, kCorner, 1.f);

        const juce::Font font(juce::FontOptions(12.0f));
        const juce::String line = juce::String(displayIndex_) + "  " + displayName_;
        const float pad = 8.f;
        const float textL = bounds.getX() + pad;
        const float maxW = juce::jmax(4.f, bounds.getWidth() - 2.f * pad);
        const juce::String elided = elideTextToFitWidth(line, font, maxW);
        g.setColour(findColour(juce::Label::textColourId));
        g.setFont(font);
        g.drawText(elided, juce::Rectangle<float>(textL, bounds.getY(), maxW, bounds.getHeight()),
                   juce::Justification::centredLeft, true);
    }

    void mouseDown(const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu())
        {
            pendingContextMenu_ = true;
            armedForLeftClick_ = false;
            dragLiftIssued_ = false;
            return;
        }
        pendingContextMenu_ = false;
        armedForLeftClick_ = true;
        dragLiftIssued_ = false;
    }

    void mouseDrag(const juce::MouseEvent& e) override
    {
        if (pendingContextMenu_)
        {
            return;
        }
        if (dragLiftIssued_)
        {
            return;
        }
        if (e.getDistanceFromDragStart() < 8)
        {
            return;
        }
        dragLiftIssued_ = true;
        armedForLeftClick_ = false;
        owner_.onInsertSlotDragStarted(stage_);
        if (auto* container = findParentComponentOfClass<juce::DragAndDropContainer>())
        {
            const juce::String desc = insertRowDragDescription(trackId_, slotId_, stage_);
            container->startDragging(juce::var(desc), this, juce::ScaledImage(), true, nullptr);
        }
    }

    void mouseUp(const juce::MouseEvent& e) override
    {
        if (pendingContextMenu_)
        {
            pendingContextMenu_ = false;
            juce::PopupMenu menu;
            menu.addItem(1, "Remove insert");
            const auto screenPt = localPointToGlobal(e.getPosition());
            menu.showMenuAsync(
                juce::PopupMenu::Options{}.withTargetScreenArea(
                    juce::Rectangle<int>(screenPt.x, screenPt.y, 1, 1)),
                [safeThis = juce::Component::SafePointer<InsertSlotButton>(this)](int r) {
                    if (safeThis == nullptr || r != 1)
                    {
                        return;
                    }
                    safeThis->performRemoveFromContextMenu();
                });
            repaint();
            return;
        }
        if (e.mods.isPopupMenu())
        {
            repaint();
            return;
        }
        const bool openEditor
            = armedForLeftClick_ && !dragLiftIssued_ && e.mouseWasClicked() && !e.mods.isPopupMenu();
        armedForLeftClick_ = false;
        repaint();
        if (openEditor)
        {
            owner_.requestEditForSlot(slotId_);
        }
    }

    bool isInterestedInDragSource(const SourceDetails& dragSourceDetails) override
    {
        return owner_.isInsertRowDragPayloadAcceptedForActiveTrack(dragSourceDetails.description);
    }

    void itemDragEnter(const SourceDetails& details) override
    {
        owner_.updateInsertDragHoverFromInspectorPoint(
            owner_.getLocalPoint(this, details.localPosition));
    }

    void itemDragMove(const SourceDetails& details) override
    {
        owner_.updateInsertDragHoverFromInspectorPoint(
            owner_.getLocalPoint(this, details.localPosition));
    }

    void itemDragExit(const SourceDetails&) override
    {
        owner_.clearInsertDropHover();
    }

    void itemDropped(const SourceDetails& dragSourceDetails) override
    {
        TrackId tid = kInvalidTrackId;
        InsertSlotId sid = kInvalidInsertSlotId;
        InsertStage src = InsertStage::Post;
        if (!parseInsertRowDragDescription(dragSourceDetails.description, tid, sid, src))
        {
            return;
        }
        owner_.handleInsertDropped(
            tid, sid, src, stage_, owner_.getLocalPoint(this, dragSourceDetails.localPosition));
    }

    void mouseEnter(const juce::MouseEvent&) override
    {
        hovered_ = true;
        repaint();
    }

    void mouseExit(const juce::MouseEvent&) override
    {
        hovered_ = false;
        repaint();
    }

    /// Called from async context menu handler (SafePointer to `this` only).
    void performRemoveFromContextMenu()
    {
        owner_.requestRemoveForSlot(slotId_);
    }

private:
    InspectorView& owner_;
    TrackId trackId_ = kInvalidTrackId;
    InsertSlotId slotId_ = kInvalidInsertSlotId;
    InsertStage stage_ = InsertStage::Post;
    int displayIndex_ = 1;
    juce::String displayName_;

    bool dragLiftIssued_ = false;
    bool armedForLeftClick_ = false;
    bool pendingContextMenu_ = false;
    bool hovered_ = false;
};

InspectorView::InspectorView(Session& session)
    : session_(session)
{
    sectionTitleLabel_.setText("Inspector", juce::dontSendNotification);
    sectionTitleLabel_.setFont(
        juce::Font(juce::FontOptions{}.withHeight(13.5f)).boldened());
    addAndMakeVisible(sectionTitleLabel_);

    activeTrackNameEditor_.setMultiLine(false);
    activeTrackNameEditor_.setReturnKeyStartsNewLine(false);
    activeTrackNameEditor_.setScrollbarsShown(false);
    activeTrackNameEditor_.setJustification(juce::Justification::centredLeft);
    activeTrackNameEditor_.setFont(juce::FontOptions(13.0f));
    activeTrackNameEditor_.setIndents(4, 4);
    activeTrackNameEditor_.setCaretVisible(true);
    activeTrackNameEditor_.addListener(this);
    addAndMakeVisible(activeTrackNameEditor_);

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

    panCaptionLabel_.setText("Pan", juce::dontSendNotification);
    panCaptionLabel_.setFont(juce::FontOptions(11.0f));
    addAndMakeVisible(panCaptionLabel_);

    panField_.setPan(0.f, juce::dontSendNotification);
    panField_.onPanChanged = [this](const float pan) {
        const TrackId active = session_.getActiveTrackId();
        if (active == kInvalidTrackId)
        {
            return;
        }
        session_.setTrackStereoPan(active, pan);
    };
    addAndMakeVisible(panField_);

    outputCaptionLabel_.setText("Output", juce::dontSendNotification);
    outputCaptionLabel_.setFont(juce::FontOptions(11.0f));
    addAndMakeVisible(outputCaptionLabel_);

    outputComboBox_.onChange = [this] {
        if (outputComboGuard_ || routedOutputHandler_ == nullptr)
        {
            return;
        }
        const TrackId active = session_.getActiveTrackId();
        const int pick = outputComboBox_.getSelectedId();
        if (active == kInvalidTrackId || pick <= 0)
        {
            return;
        }
        const size_t ix = static_cast<size_t>(pick - 1);
        if (ix >= outputComboDestIds_.size())
        {
            return;
        }
        routedOutputHandler_(active, outputComboDestIds_[ix]);
    };
    addAndMakeVisible(outputComboBox_);

    insertsSectionLabel_.setText("Inserts", juce::dontSendNotification);
    insertsSectionLabel_.setFont(juce::FontOptions(11.0f));
    addAndMakeVisible(insertsSectionLabel_);

    preStageDrop_ = std::make_unique<StageDropTarget>(*this, InsertStage::Pre);
    addAndMakeVisible(*preStageDrop_);

    preSectionLabel_.setText("Pre", juce::dontSendNotification);
    preSectionLabel_.setFont(juce::Font(juce::FontOptions(12.0f)).boldened());
    preSectionLabel_.setJustificationType(juce::Justification::centredLeft);
    preSectionLabel_.setInterceptsMouseClicks(false, false);
    addAndMakeVisible(preSectionLabel_);

    preEmptyLabel_.setText("(empty)", juce::dontSendNotification);
    preEmptyLabel_.setFont(juce::Font(juce::FontOptions(11.0f)).italicised());
    preEmptyLabel_.setColour(juce::Label::textColourId, juce::Colours::grey);
    preEmptyLabel_.setJustificationType(juce::Justification::centredLeft);
    preEmptyLabel_.setInterceptsMouseClicks(false, false);
    addAndMakeVisible(preEmptyLabel_);

    addPreInsertButton_.setButtonText("+ Add Pre insert");
    addPreInsertButton_.onClick = [this] {
        if (insertDragSourceStage_.has_value())
        {
            return;
        }
        const TrackId t = session_.getActiveTrackId();
        if (t == kInvalidTrackId || !pluginHost_.requestAdd)
        {
            return;
        }
        pluginHost_.requestAdd(t, InsertStage::Pre);
    };
    addAndMakeVisible(addPreInsertButton_);

    postStageDrop_ = std::make_unique<StageDropTarget>(*this, InsertStage::Post);
    addAndMakeVisible(*postStageDrop_);

    postSectionLabel_.setText("Post", juce::dontSendNotification);
    postSectionLabel_.setFont(juce::Font(juce::FontOptions(12.0f)).boldened());
    postSectionLabel_.setJustificationType(juce::Justification::centredLeft);
    postSectionLabel_.setInterceptsMouseClicks(false, false);
    addAndMakeVisible(postSectionLabel_);

    postEmptyLabel_.setText("(empty)", juce::dontSendNotification);
    postEmptyLabel_.setFont(juce::Font(juce::FontOptions(11.0f)).italicised());
    postEmptyLabel_.setColour(juce::Label::textColourId, juce::Colours::grey);
    postEmptyLabel_.setJustificationType(juce::Justification::centredLeft);
    postEmptyLabel_.setInterceptsMouseClicks(false, false);
    addAndMakeVisible(postEmptyLabel_);

    addPostInsertButton_.setButtonText("+ Add Post insert");
    addPostInsertButton_.onClick = [this] {
        if (insertDragSourceStage_.has_value())
        {
            return;
        }
        const TrackId t = session_.getActiveTrackId();
        if (t == kInvalidTrackId || !pluginHost_.requestAdd)
        {
            return;
        }
        pluginHost_.requestAdd(t, InsertStage::Post);
    };
    addAndMakeVisible(addPostInsertButton_);

    sendsSectionLabel_.setText("Sends", juce::dontSendNotification);
    sendsSectionLabel_.setFont(juce::FontOptions(11.0f));
    addAndMakeVisible(sendsSectionLabel_);

    sendsExtraLabel_.setFont(juce::FontOptions(10.0f));
    sendsExtraLabel_.setColour(juce::Label::textColourId, juce::Colours::grey);
    sendsExtraLabel_.setJustificationType(juce::Justification::centredLeft);
    sendsExtraLabel_.setInterceptsMouseClicks(false, false);
    addAndMakeVisible(sendsExtraLabel_);

    for (int row = 0; row < kVisibleSendRows; ++row)
    {
        SendRowUi& ui = sendRows_[row];
        ui.destCombo.setTextWhenNothingSelected("(none)");
        ui.destCombo.onChange = [this, row] {
            SendRowUi& r = sendRows_[row];
            if (r.comboGuard || trackSendDestinationHandler_ == nullptr)
            {
                return;
            }
            const TrackId active = session_.getActiveTrackId();
            if (active == kInvalidTrackId)
            {
                return;
            }
            const std::shared_ptr<const SessionSnapshot> snap
                = session_.loadSessionSnapshotForAudioThread();
            if (snap == nullptr)
            {
                return;
            }
            const int tix = snap->findTrackIndexById(active);
            if (tix < 0)
            {
                return;
            }

            const int pick = r.destCombo.getSelectedId();
            if (pick <= 0)
            {
                return;
            }
            TrackId dest = kInvalidTrackId;
            const size_t ix = static_cast<size_t>(pick - 1);
            if (ix < r.destIds.size())
            {
                dest = r.destIds[ix];
            }
            if (dest == kInvalidTrackId
                && findTrackSendVectorIndexForUiSlot(snap->getTrack(tix).getSends(), row) < 0)
            {
                return;
            }
            trackSendDestinationHandler_(active, row, dest);
        };
        addAndMakeVisible(ui.destCombo);

        ui.amountDbUnitLabel.setText("dB", juce::dontSendNotification);
        ui.amountDbUnitLabel.setFont(juce::FontOptions(12.0f));
        ui.amountDbUnitLabel.setJustificationType(juce::Justification::centredLeft);
        ui.amountDbUnitLabel.setInterceptsMouseClicks(false, false);
        addAndMakeVisible(ui.amountDbUnitLabel);

        ui.amountEditor.setMultiLine(false);
        ui.amountEditor.setReturnKeyStartsNewLine(false);
        ui.amountEditor.setFont(juce::FontOptions(12.0f));
        ui.amountEditor.setJustification(juce::Justification::centred);
        ui.amountEditor.setIndents(0, 4);
        ui.amountEditor.setCaretVisible(true);
        ui.amountEditor.addListener(this);
        addAndMakeVisible(ui.amountEditor);

        ui.enableToggle.setButtonText("On");
        ui.enableToggle.onClick = [this, row] {
            SendRowUi& r = sendRows_[row];
            if (r.enableGuard || trackSendEnabledHandler_ == nullptr)
            {
                return;
            }
            const TrackId active = session_.getActiveTrackId();
            if (active == kInvalidTrackId)
            {
                return;
            }
            const std::shared_ptr<const SessionSnapshot> snap
                = session_.loadSessionSnapshotForAudioThread();
            if (snap == nullptr)
            {
                return;
            }
            const int tix = snap->findTrackIndexById(active);
            if (tix < 0
                || findTrackSendVectorIndexForUiSlot(snap->getTrack(tix).getSends(), row) < 0)
            {
                return;
            }
            trackSendEnabledHandler_(active, row, r.enableToggle.getToggleState());
        };
        addAndMakeVisible(ui.enableToggle);
    }

    refreshFromSession();
}

void InspectorView::setTrackSendHandlers(
    std::function<void(TrackId, int, TrackId)> destination,
    std::function<void(TrackId, int, float)> amount,
    std::function<void(TrackId, int, bool)> enabled) noexcept
{
    trackSendDestinationHandler_ = std::move(destination);
    trackSendAmountHandler_ = std::move(amount);
    trackSendEnabledHandler_ = std::move(enabled);
}

InspectorView::~InspectorView() = default;

void InspectorView::requestEditForSlot(const InsertSlotId slotId)
{
    const TrackId t = session_.getActiveTrackId();
    if (t == kInvalidTrackId || slotId == kInvalidInsertSlotId || !pluginHost_.requestEdit)
    {
        return;
    }
    pluginHost_.requestEdit(t, slotId);
}

void InspectorView::requestRemoveForSlot(const InsertSlotId slotId)
{
    const TrackId t = session_.getActiveTrackId();
    if (t == kInvalidTrackId || slotId == kInvalidInsertSlotId || !pluginHost_.requestRemove)
    {
        return;
    }
    pluginHost_.requestRemove(t, slotId);
    juce::Component::SafePointer<InspectorView> safeSelf(this);
    juce::MessageManager::callAsync([safeSelf] {
        if (safeSelf != nullptr)
        {
            safeSelf->refreshFromSession();
        }
    });
}

void InspectorView::onInsertSlotDragStarted(const InsertStage sourceStage)
{
    insertDragSourceStage_ = sourceStage;
    repaint();
}

void InspectorView::clearInsertSlotDragSession() noexcept
{
    insertDragSourceStage_.reset();
    clearInsertDropHover();
}

void InspectorView::dragOperationEnded(const juce::DragAndDropTarget::SourceDetails&)
{
    clearInsertSlotDragSession();
    repaint();
}

std::optional<InsertStage> InspectorView::stageForLocalPoint(const juce::Point<int> p) const noexcept
{
    if (preInsertBlockBounds_.contains(p))
    {
        return InsertStage::Pre;
    }
    if (postInsertBlockBounds_.contains(p))
    {
        return InsertStage::Post;
    }
    return std::nullopt;
}

bool InspectorView::isInterestedInDragSource(const juce::DragAndDropTarget::SourceDetails& details)
{
    return isInsertRowDragPayloadAcceptedForActiveTrack(details.description);
}

void InspectorView::itemDragEnter(const juce::DragAndDropTarget::SourceDetails& details)
{
    updateInsertDragHoverFromInspectorPoint(details.localPosition);
}

void InspectorView::itemDragMove(const juce::DragAndDropTarget::SourceDetails& details)
{
    updateInsertDragHoverFromInspectorPoint(details.localPosition);
}

void InspectorView::itemDragExit(const juce::DragAndDropTarget::SourceDetails&)
{
    clearInsertDropHover();
}

void InspectorView::itemDropped(const juce::DragAndDropTarget::SourceDetails& details)
{
    TrackId tid = kInvalidTrackId;
    InsertSlotId sid = kInvalidInsertSlotId;
    InsertStage src = InsertStage::Post;
    if (!parseInsertRowDragDescription(details.description, tid, sid, src))
    {
        clearInsertSlotDragSession();
        return;
    }
    const auto targetStage = stageForLocalPoint(details.localPosition);
    if (!targetStage.has_value())
    {
        clearInsertSlotDragSession();
        return;
    }
    handleInsertDropped(tid, sid, src, *targetStage, details.localPosition);
}

bool InspectorView::isSameStageAddButtonArea(const InsertStage st, const juce::Point<int> p) const noexcept
{
    if (st == InsertStage::Pre)
    {
        return addPreInsertButton_.getBounds().contains(p);
    }
    return addPostInsertButton_.getBounds().contains(p);
}

int InspectorView::gapIndexForStageAtLocalPoint(const InsertStage stage,
                                               const juce::Point<int> p) const noexcept
{
    const auto& rows = stage == InsertStage::Pre ? preRowStrips_ : postRowStrips_;
    if (rows.empty())
    {
        return 0;
    }
    for (int i = 0; i < static_cast<int>(rows.size()); ++i)
    {
        if (p.y < rows[static_cast<size_t>(i)]->getBounds().getCentreY())
        {
            return i;
        }
    }
    return static_cast<int>(rows.size());
}

int InspectorView::gapIndexForCrossStageDrop(const InsertStage targetStage,
                                            const juce::Point<int> p) const noexcept
{
    const auto& rows = targetStage == InsertStage::Pre ? preRowStrips_ : postRowStrips_;
    if (isSameStageAddButtonArea(targetStage, p))
    {
        return static_cast<int>(rows.size());
    }
    if (rows.empty())
    {
        return 0;
    }
    return gapIndexForStageAtLocalPoint(targetStage, p);
}

void InspectorView::notifyInsertDropHover(const InsertStage stage, const juce::Point<int> p) noexcept
{
    constexpr int kLineH = 2;
    const auto& rows = stage == InsertStage::Pre ? preRowStrips_ : postRowStrips_;
    const juce::Rectangle<int> addBounds = stage == InsertStage::Pre ? addPreInsertButton_.getBounds()
                                                                      : addPostInsertButton_.getBounds();

    int gap = 0;
    juce::Rectangle<int> line;

    if (isSameStageAddButtonArea(stage, p))
    {
        gap = static_cast<int>(rows.size());
        const int x = rows.empty() ? addBounds.getX() : rows[0]->getX();
        const int w = rows.empty() ? addBounds.getWidth() : juce::jmax(1, rows[0]->getWidth());
        line = juce::Rectangle<int>(x, addBounds.getY() - kLineH / 2, w, kLineH);
    }
    else if (rows.empty())
    {
        gap = 0;
        line = juce::Rectangle<int>(addBounds.getX(), addBounds.getY() - kLineH / 2,
                                    juce::jmax(1, addBounds.getWidth()), kLineH);
    }
    else
    {
        gap = gapIndexForStageAtLocalPoint(stage, p);
        const int x = rows[0]->getX();
        const int w = juce::jmax(1, rows[0]->getWidth());
        if (gap == 0)
        {
            const int y = rows[0]->getY();
            line = juce::Rectangle<int>(x, y - kLineH / 2, w, kLineH);
        }
        else if (gap >= static_cast<int>(rows.size()))
        {
            const int y = rows.back()->getBottom();
            line = juce::Rectangle<int>(x, y - kLineH / 2, w, kLineH);
        }
        else
        {
            const int y = rows[static_cast<size_t>(gap)]->getY();
            line = juce::Rectangle<int>(x, y - kLineH / 2, w, kLineH);
        }
    }

    if (insertDropHoverActive_ && insertDropHoverStage_ == stage && insertDropHoverGapIndex_ == gap
        && insertDropLineBounds_ == line)
    {
        return;
    }
    insertDropHoverActive_ = true;
    insertDropHoverStage_ = stage;
    insertDropHoverGapIndex_ = gap;
    insertDropLineBounds_ = line;
    repaint(preInsertBlockBounds_);
    repaint(postInsertBlockBounds_);
}

void InspectorView::clearInsertDropHover() noexcept
{
    if (!insertDropHoverActive_)
    {
        return;
    }
    insertDropHoverActive_ = false;
    repaint(preInsertBlockBounds_);
    repaint(postInsertBlockBounds_);
}

void InspectorView::updateInsertDragHoverFromInspectorPoint(const juce::Point<int> p) noexcept
{
    if (!insertDragSourceStage_.has_value())
    {
        clearInsertDropHover();
        return;
    }

    const auto stOpt = stageForLocalPoint(p);
    if (!stOpt.has_value())
    {
        clearInsertDropHover();
        return;
    }

    if (*stOpt == *insertDragSourceStage_ && isSameStageAddButtonArea(*stOpt, p))
    {
        clearInsertDropHover();
        return;
    }

    notifyInsertDropHover(*stOpt, p);
}

bool InspectorView::isInsertRowDragPayloadAcceptedForActiveTrack(const juce::var& desc) const noexcept
{
    TrackId tid = kInvalidTrackId;
    InsertSlotId sid = kInvalidInsertSlotId;
    InsertStage src = InsertStage::Post;
    if (!parseInsertRowDragDescription(desc, tid, sid, src))
    {
        return false;
    }
    const TrackId expected = (lastShownInsertRowsTrackId_ != kInvalidTrackId)
                                 ? lastShownInsertRowsTrackId_
                                 : session_.getActiveTrackId();
    return tid == expected;
}

void InspectorView::handleInsertDropped(const TrackId tid,
                                       const InsertSlotId sid,
                                       const InsertStage sourceStage,
                                       const InsertStage targetStage,
                                       const juce::Point<int> localPoint)
{
    const TrackId active = session_.getActiveTrackId();
    const TrackId shown = lastShownInsertRowsTrackId_;
    const TrackId expected = (shown != kInvalidTrackId) ? shown : active;

    clearInsertSlotDragSession();

    if (tid == kInvalidTrackId || sid == kInvalidInsertSlotId)
    {
        return;
    }
    if (tid != expected)
    {
        return;
    }

    if (sourceStage == targetStage)
    {
        if (isSameStageAddButtonArea(targetStage, localPoint))
        {
            return;
        }
        if (!pluginHost_.requestReorderInStage)
        {
            return;
        }
        const int gap = gapIndexForStageAtLocalPoint(targetStage, localPoint);
        pluginHost_.requestReorderInStage(tid, sid, gap);
    }
    else
    {
        if (!pluginHost_.requestMoveToStageAtGap)
        {
            return;
        }
        const int gap = gapIndexForCrossStageDrop(targetStage, localPoint);
        pluginHost_.requestMoveToStageAtGap(tid, sid, targetStage, gap);
    }

    juce::Component::SafePointer<InspectorView> safeSelf(this);
    juce::MessageManager::callAsync([safeSelf] {
        if (safeSelf != nullptr)
        {
            safeSelf->refreshFromSession();
        }
    });
}

void InspectorView::paintOverChildren(juce::Graphics& g)
{
    if (insertDropHoverActive_ && !insertDropLineBounds_.isEmpty())
    {
        g.setColour(juce::Colour(0xff40c040));
        g.fillRect(insertDropLineBounds_);
    }
}

void InspectorView::clearInsertRowStrips()
{
    for (auto& p : preRowStrips_)
    {
        removeChildComponent(p.get());
    }
    preRowStrips_.clear();
    for (auto& p : postRowStrips_)
    {
        removeChildComponent(p.get());
    }
    postRowStrips_.clear();
}

void InspectorView::rebuildInsertRowStrips(const TrackId active, const std::vector<InspectorInsertRow>& rows)
{
    clearInsertRowStrips();
    if (active == kInvalidTrackId)
    {
        return;
    }

    int preIndex = 1;
    for (const auto& r : rows)
    {
        if (r.stage != InsertStage::Pre || r.slotId == kInvalidInsertSlotId)
        {
            continue;
        }
        const InsertSlotId sid = r.slotId;
        auto strip = std::make_unique<InsertSlotButton>(*this,
                                                      active,
                                                      sid,
                                                      InsertStage::Pre,
                                                      preIndex++,
                                                      r.displayName);
        addAndMakeVisible(*strip);
        preRowStrips_.push_back(std::move(strip));
    }

    int postIndex = 1;
    for (const auto& r : rows)
    {
        if (r.stage != InsertStage::Post || r.slotId == kInvalidInsertSlotId)
        {
            continue;
        }
        const InsertSlotId sid = r.slotId;
        auto strip = std::make_unique<InsertSlotButton>(*this,
                                                      active,
                                                      sid,
                                                      InsertStage::Post,
                                                      postIndex++,
                                                      r.displayName);
        addAndMakeVisible(*strip);
        postRowStrips_.push_back(std::move(strip));
    }
    resized();
    repaint();
}

void InspectorView::setVolumeEditorTextFromLinearGain(const float linearGain)
{
    channelVolumeDbEditor_.setText(formatLinearGainToValueFieldOnly(linearGain),
                                   juce::dontSendNotification);
}

void InspectorView::syncActiveTrackNameEditorDisplay()
{
    if (inspectorNameEditorGuard_)
    {
        return;
    }
    activeTrackNameEditor_.setTooltip(activeTrackPlainName_.isEmpty() ? juce::String{}
                                                                      : activeTrackPlainName_);
    activeTrackNameEditor_.setText(activeTrackPlainName_, juce::dontSendNotification);
}

void InspectorView::commitActiveTrackNameField()
{
    if (inspectorNameEditorGuard_ || renameTrackHandler_ == nullptr)
    {
        return;
    }
    const TrackId active = session_.getActiveTrackId();
    if (active == kInvalidTrackId)
    {
        return;
    }
    if (const auto snap = session_.loadSessionSnapshotForAudioThread())
    {
        const int idx = snap->findTrackIndexById(active);
        if (idx >= 0 && snap->getTrack(idx).getKind() == TrackKind::Master)
        {
            return;
        }
    }
    const juce::String trimmed = activeTrackNameEditor_.getText().trim();
    if (trimmed.isEmpty())
    {
        inspectorNameEditorGuard_ = true;
        activeTrackNameEditor_.setText(activeTrackPlainName_, juce::dontSendNotification);
        inspectorNameEditorGuard_ = false;
        return;
    }
    if (trimmed == activeTrackPlainName_)
    {
        return;
    }
    inspectorNameEditorGuard_ = true;
    const bool ok = renameTrackHandler_(active, trimmed);
    inspectorNameEditorGuard_ = false;
    if (!ok)
    {
        activeTrackNameEditor_.setText(activeTrackPlainName_, juce::dontSendNotification);
    }
}

void InspectorView::setSendAmountEditorText(const int sendRowIndex, const float amountLinear)
{
    if (sendRowIndex < 0 || sendRowIndex >= kVisibleSendRows)
    {
        return;
    }
    SendRowUi& ui = sendRows_[sendRowIndex];
    ui.amountGuard = true;
    ui.amountEditor.setText(formatSendLinearToDbField(amountLinear), juce::dontSendNotification);
    ui.amountGuard = false;
}

void InspectorView::populateSendDestCombo(const int sendRowIndex,
                                          const TrackId activeTrackId,
                                          const Track& track)
{
    if (sendRowIndex < 0 || sendRowIndex >= kVisibleSendRows)
    {
        return;
    }
    SendRowUi& ui = sendRows_[sendRowIndex];
    ui.comboGuard = true;
    ui.destCombo.clear(juce::dontSendNotification);
    ui.destIds.clear();
    ui.destCombo.addItem("(none)", 1);
    ui.destIds.push_back(kInvalidTrackId);

    const std::shared_ptr<const SessionSnapshot> routeSnap
        = session_.loadSessionSnapshotForAudioThread();
    if (routeSnap != nullptr)
    {
        const std::vector<TrackId> legal
            = session_routing::legalSendDestinations(*routeSnap, activeTrackId);
        TrackId currentDest = kInvalidTrackId;
        const int sendIndex = findTrackSendVectorIndexForUiSlot(track.getSends(), sendRowIndex);
        if (sendIndex >= 0)
        {
            currentDest = track.getSend(sendIndex).destTrackId;
        }
        int selectId = 1;
        for (const TrackId destId : legal)
        {
            juce::String label;
            const int dix = routeSnap->findTrackIndexById(destId);
            if (dix >= 0)
            {
                label = routeSnap->getTrack(dix).getName();
            }
            else
            {
                label = juce::String("Track ") + juce::String((juce::int64)destId);
            }
            const int itemId = static_cast<int>(ui.destIds.size()) + 1;
            ui.destIds.push_back(destId);
            ui.destCombo.addItem(label, itemId);
            if (destId == currentDest)
            {
                selectId = itemId;
            }
        }
        if (sendIndex >= 0 && currentDest != kInvalidTrackId
            && std::find(legal.begin(), legal.end(), currentDest) == legal.end())
        {
            const int dix = routeSnap->findTrackIndexById(currentDest);
            juce::String label = (dix >= 0) ? routeSnap->getTrack(dix).getName()
                                            : juce::String("Track ") + juce::String((juce::int64)currentDest);
            const int itemId = static_cast<int>(ui.destIds.size()) + 1;
            ui.destIds.push_back(currentDest);
            ui.destCombo.addItem(label + " (stored)", itemId);
            selectId = itemId;
        }
        ui.destCombo.setSelectedId(selectId, juce::dontSendNotification);
    }
    ui.comboGuard = false;
}

void InspectorView::commitSendAmountField(const int sendRowIndex)
{
    if (sendRowIndex < 0 || sendRowIndex >= kVisibleSendRows || trackSendAmountHandler_ == nullptr)
    {
        return;
    }
    const std::shared_ptr<const SessionSnapshot> snap = session_.loadSessionSnapshotForAudioThread();
    if (snap == nullptr)
    {
        return;
    }
    const TrackId active = session_.getActiveTrackId();
    const int tix = snap->findTrackIndexById(active);
    if (tix < 0)
    {
        return;
    }
    const int sendIndex = findTrackSendVectorIndexForUiSlot(snap->getTrack(tix).getSends(), sendRowIndex);
    if (sendIndex < 0)
    {
        return;
    }
    const float snapAmount = snap->getTrack(tix).getSend(sendIndex).amountLinear;
    float parsed = snapAmount;
    if (!tryParseSendAmountText(sendRows_[sendRowIndex].amountEditor.getText(), parsed))
    {
        setSendAmountEditorText(sendRowIndex, snapAmount);
        return;
    }
    if (std::fabs((double)(parsed - snapAmount)) <= (double)kSendAmountDriftEps)
    {
        return;
    }
    trackSendAmountHandler_(active, sendRowIndex, parsed);
}

void InspectorView::syncSendsWhenInspectorDisabled()
{
    sendsSectionLabel_.setVisible(false);
    sendsExtraLabel_.setVisible(false);
    for (int row = 0; row < kVisibleSendRows; ++row)
    {
        sendRows_[row].destCombo.setVisible(false);
        sendRows_[row].amountEditor.setVisible(false);
        sendRows_[row].amountDbUnitLabel.setVisible(false);
        sendRows_[row].enableToggle.setVisible(false);
    }
}

void InspectorView::syncSendsNoActiveTrack()
{
    syncSendsWhenInspectorDisabled();
}

void InspectorView::syncSendsForActiveTrack(const TrackId active, const Track& track)
{
    const bool showSends = (track.getKind() != TrackKind::Master);
    sendsSectionLabel_.setVisible(showSends);
    sendsExtraLabel_.setVisible(showSends);
    for (int row = 0; row < kVisibleSendRows; ++row)
    {
        const bool rowVisible = showSends;
        sendRows_[row].destCombo.setVisible(rowVisible);
        sendRows_[row].amountEditor.setVisible(rowVisible);
        sendRows_[row].amountDbUnitLabel.setVisible(rowVisible);
        sendRows_[row].enableToggle.setVisible(rowVisible);
    }
    if (!showSends)
    {
        sendsExtraLabel_.setText({}, juce::dontSendNotification);
        return;
    }

    const int extra = countTrackSendsOutsideInspectorUiSlots(track.getSends());
    if (extra > 0)
    {
        sendsExtraLabel_.setText("(+" + juce::String(extra) + " more sends not shown)",
                                 juce::dontSendNotification);
    }
    else
    {
        sendsExtraLabel_.setText({}, juce::dontSendNotification);
    }

    for (int row = 0; row < kVisibleSendRows; ++row)
    {
        populateSendDestCombo(row, active, track);
        SendRowUi& ui = sendRows_[row];
        const int sendIndex = findTrackSendVectorIndexForUiSlot(track.getSends(), row);
        const bool existingSend = sendIndex >= 0;

        ui.destCombo.setEnabled(true);
        ui.amountEditor.setEnabled(existingSend);
        ui.enableToggle.setEnabled(existingSend);

        ui.enableGuard = true;
        if (existingSend)
        {
            const TrackSend& send = track.getSend(sendIndex);
            ui.enableToggle.setToggleState(send.enabled, juce::dontSendNotification);
            if (!ui.amountEditor.hasKeyboardFocus(false))
            {
                setSendAmountEditorText(row, send.amountLinear);
            }
        }
        else
        {
            ui.enableToggle.setToggleState(false, juce::dontSendNotification);
            if (!ui.amountEditor.hasKeyboardFocus(false))
            {
                setSendAmountEditorText(row, kSendAmountUnityLinear);
            }
        }
        ui.enableGuard = false;
    }
}

void InspectorView::syncInsertsWhenInspectorDisabled()
{
    insertsSectionLabel_.setVisible(true);
    syncSendsWhenInspectorDisabled();
    preSectionLabel_.setVisible(true);
    postSectionLabel_.setVisible(true);
    preEmptyLabel_.setVisible(true);
    postEmptyLabel_.setVisible(true);
    addPreInsertButton_.setVisible(true);
    addPostInsertButton_.setVisible(true);
    addPreInsertButton_.setEnabled(false);
    addPostInsertButton_.setEnabled(false);
    clearInsertRowStrips();
    lastShownInsertRows_.clear();
    lastShownInsertRowsTrackId_ = kInvalidTrackId;
    clearInsertSlotDragSession();
    resized();
}

void InspectorView::syncInsertsNoActiveTrack()
{
    insertsSectionLabel_.setVisible(true);
    preSectionLabel_.setVisible(true);
    postSectionLabel_.setVisible(true);
    preEmptyLabel_.setVisible(true);
    postEmptyLabel_.setVisible(true);
    addPreInsertButton_.setVisible(true);
    addPostInsertButton_.setVisible(true);
    addPreInsertButton_.setEnabled(false);
    addPostInsertButton_.setEnabled(false);
    clearInsertRowStrips();
    lastShownInsertRows_.clear();
    lastShownInsertRowsTrackId_ = kInvalidTrackId;
    clearInsertSlotDragSession();
    resized();
}

void InspectorView::syncInsertsForActiveTrack(const TrackId active)
{
    if (active == kInvalidTrackId)
    {
        syncInsertsNoActiveTrack();
        return;
    }
    insertsSectionLabel_.setVisible(true);
    preSectionLabel_.setVisible(true);
    postSectionLabel_.setVisible(true);
    addPreInsertButton_.setVisible(true);
    addPostInsertButton_.setVisible(true);

    const bool canAdd
        = pluginHost_.requestAdd != nullptr && isEnabled();
    addPreInsertButton_.setEnabled(canAdd);
    addPostInsertButton_.setEnabled(canAdd);

    std::vector<InspectorInsertRow> rows;
    if (pluginHost_.getInsertRows)
    {
        rows = pluginHost_.getInsertRows(active);
    }
    const int preCount = static_cast<int>(
        std::count_if(rows.begin(), rows.end(), [](const InspectorInsertRow& r) {
            return r.stage == InsertStage::Pre && r.slotId != kInvalidInsertSlotId;
        }));
    const int postCount = static_cast<int>(
        std::count_if(rows.begin(), rows.end(), [](const InspectorInsertRow& r) {
            return r.stage == InsertStage::Post && r.slotId != kInvalidInsertSlotId;
        }));

    preEmptyLabel_.setVisible(preCount == 0);
    postEmptyLabel_.setVisible(postCount == 0);

    const bool sameTrack = (active == lastShownInsertRowsTrackId_);
    const bool sameRows = sameTrack && lastShownInsertRows_.size() == rows.size()
        && std::equal(rows.begin(), rows.end(), lastShownInsertRows_.begin(),
                      [](const InspectorInsertRow& a, const InspectorInsertRow& b) {
                          return a.slotId == b.slotId && a.stage == b.stage
                                 && a.displayName == b.displayName;
                      });

    if (!sameRows)
    {
        rebuildInsertRowStrips(active, rows);
        lastShownInsertRows_ = rows;
        lastShownInsertRowsTrackId_ = active;
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
        inspectorNameEditorGuard_ = true;
        activeTrackNameEditor_.setText("—", juce::dontSendNotification);
        activeTrackNameEditor_.setTooltip({});
        inspectorNameEditorGuard_ = false;
        channelVolumeDbEditor_.setText({}, juce::dontSendNotification);
        panField_.setPan(0.f, juce::dontSendNotification);
        syncInsertsWhenInspectorDisabled();
        syncSendsWhenInspectorDisabled();
        return;
    }
    setEnabled(true);

    const TrackId active = session_.getActiveTrackId();
    const int idx = snap->findTrackIndexById(active);
    if (idx < 0)
    {
        activeTrackPlainName_.clear();
        inspectorNameEditorGuard_ = true;
        activeTrackNameEditor_.setText("(no active track)", juce::dontSendNotification);
        activeTrackNameEditor_.setTooltip({});
        inspectorNameEditorGuard_ = false;
        channelVolumeDbEditor_.setText({}, juce::dontSendNotification);
        panField_.setPan(0.f, juce::dontSendNotification);
        outputCaptionLabel_.setVisible(false);
        outputComboBox_.setVisible(false);
        outputComboBox_.clear(juce::dontSendNotification);
        outputComboDestIds_.clear();
        syncInsertsNoActiveTrack();
        syncSendsNoActiveTrack();
        return;
    }
    const Track& tr = snap->getTrack(idx);
    const bool masterLane = (tr.getKind() == TrackKind::Master);
    activeTrackPlainName_ = masterLane ? juce::String(kMasterTrackDisplayName) : tr.getName();
    activeTrackNameEditor_.setReadOnly(masterLane);
    activeTrackNameEditor_.setCaretVisible(!masterLane);
    activeTrackNameEditor_.setEnabled(true);
    activeTrackNameEditor_.setInterceptsMouseClicks(!masterLane, !masterLane);

    const float snapGain = tr.getChannelFaderGain();
    const bool switchedTrack = (lastShownTrackId_ != active);
    lastShownTrackId_ = active;

    syncInsertsForActiveTrack(active);
    syncSendsForActiveTrack(active, tr);

    if (!panField_.isMouseButtonDown())
    {
        panField_.setPan(tr.getStereoPan(), juce::dontSendNotification);
    }

    const bool showOutputRouting = (tr.getKind() != TrackKind::Master);
    outputCaptionLabel_.setVisible(showOutputRouting);
    outputComboBox_.setVisible(showOutputRouting);
    if (showOutputRouting)
    {
        outputComboGuard_ = true;
        outputComboBox_.clear(juce::dontSendNotification);
        outputComboDestIds_.clear();
        const std::shared_ptr<const SessionSnapshot> routeSnap
            = session_.loadSessionSnapshotForAudioThread();
        if (routeSnap != nullptr)
        {
            const std::vector<TrackId> legal
                = session_routing::legalOutputDestinations(*routeSnap, active);
            int selectId = 0;
            const TrackId currentOut = tr.getRoutedOutputTrackId();
            for (size_t li = 0; li < legal.size(); ++li)
            {
                const TrackId destId = legal[li];
                outputComboDestIds_.push_back(destId);
                juce::String label;
                const int dix = routeSnap->findTrackIndexById(destId);
                if (dix >= 0 && routeSnap->getTrack(dix).getKind() == TrackKind::Master)
                {
                    label = juce::String(kMasterTrackDisplayName);
                }
                else if (dix >= 0)
                {
                    label = routeSnap->getTrack(dix).getName();
                }
                else
                {
                    label = juce::String("Track ") + juce::String((juce::int64)destId);
                }
                const int itemId = static_cast<int>(li) + 1;
                outputComboBox_.addItem(label, itemId);
                if (destId == currentOut)
                {
                    selectId = itemId;
                }
            }
            if (selectId > 0)
            {
                outputComboBox_.setSelectedId(selectId, juce::dontSendNotification);
            }
        }
        outputComboGuard_ = false;
    }

    const bool nameFocused = activeTrackNameEditor_.hasKeyboardFocus(false);
    if (!nameFocused || switchedTrack)
    {
        syncActiveTrackNameEditorDisplay();
    }

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
    activeTrackNameEditor_.setBounds(area.removeFromTop(22));
    area.removeFromTop(4);

    channelVolumeCaptionLabel_.setBounds(area.removeFromTop(18));
    area.removeFromTop(2);
    auto fieldRow = area.removeFromTop(juce::jmax(26, kDbValueFieldHeight));
    const int xStart = fieldRow.getX();
    const int yy = fieldRow.getY();

    channelVolumeDbEditor_.setBounds(xStart, yy, kDbValueFieldWidth, kDbValueFieldHeight);
    channelVolumeDbUnitLabel_.setBounds(xStart + kDbValueFieldWidth + kGapValueToDbSuffix, yy,
                                        kDbUnitLabelWidth, kDbValueFieldHeight);

    area.removeFromTop(8);
    panCaptionLabel_.setBounds(area.removeFromTop(18));
    area.removeFromTop(2);
    {
        auto panRow = area.removeFromTop(36);
        panField_.setBounds(panRow);
        panField_.toFront(false);
    }

    area.removeFromTop(8);
    outputCaptionLabel_.setBounds(area.removeFromTop(18));
    area.removeFromTop(2);
    outputComboBox_.setBounds(area.removeFromTop(24));

    area.removeFromTop(10);
    insertsSectionLabel_.setBounds(area.removeFromTop(18));
    area.removeFromTop(4);

    constexpr int kRowH = 24;
    constexpr int kSubHdrH = 18;
    constexpr int kEmptyH = 16;
    constexpr int kGapSmall = 2;
    constexpr int kGapSection = 8;

    const int preBlockTop = area.getY();
    preSectionLabel_.setBounds(area.removeFromTop(kSubHdrH));
    area.removeFromTop(kGapSmall);
    for (auto& strip : preRowStrips_)
    {
        strip->setBounds(area.removeFromTop(kRowH));
    }
    if (preEmptyLabel_.isVisible())
    {
        preEmptyLabel_.setBounds(area.removeFromTop(kEmptyH));
    }
    area.removeFromTop(kGapSmall);
    addPreInsertButton_.setBounds(area.removeFromTop(kRowH));

    preInsertBlockBounds_
        = juce::Rectangle<int>(area.getX(), preBlockTop, area.getWidth(), area.getY() - preBlockTop);
    if (preStageDrop_ != nullptr)
    {
        preStageDrop_->setBounds(preInsertBlockBounds_);
    }

    area.removeFromTop(kGapSection);

    const int postBlockTop = area.getY();
    postSectionLabel_.setBounds(area.removeFromTop(kSubHdrH));
    area.removeFromTop(kGapSmall);
    for (auto& strip : postRowStrips_)
    {
        strip->setBounds(area.removeFromTop(kRowH));
    }
    if (postEmptyLabel_.isVisible())
    {
        postEmptyLabel_.setBounds(area.removeFromTop(kEmptyH));
    }
    area.removeFromTop(kGapSmall);
    addPostInsertButton_.setBounds(area.removeFromTop(kRowH));

    postInsertBlockBounds_
        = juce::Rectangle<int>(area.getX(), postBlockTop, area.getWidth(), area.getY() - postBlockTop);
    if (postStageDrop_ != nullptr)
    {
        postStageDrop_->setBounds(postInsertBlockBounds_);
    }

    area.removeFromTop(10);
    sendsSectionLabel_.setBounds(area.removeFromTop(18));
    area.removeFromTop(2);
    if (sendsExtraLabel_.isVisible() && sendsExtraLabel_.getText().isNotEmpty())
    {
        sendsExtraLabel_.setBounds(area.removeFromTop(14));
        area.removeFromTop(2);
    }
    constexpr int kSendRowH = 24;
    constexpr int kSendEnableW = 36;
    for (int row = 0; row < kVisibleSendRows; ++row)
    {
        if (!sendRows_[row].destCombo.isVisible())
        {
            continue;
        }
        auto rowArea = area.removeFromTop(kSendRowH);
        sendRows_[row].enableToggle.setBounds(rowArea.removeFromLeft(kSendEnableW).reduced(0, 2));
        rowArea.removeFromLeft(4);
        sendRows_[row].amountDbUnitLabel.setBounds(rowArea.removeFromRight(kDbUnitLabelWidth));
        rowArea.removeFromRight(kGapValueToDbSuffix);
        sendRows_[row].amountEditor.setBounds(rowArea.removeFromRight(kDbValueFieldWidth));
        rowArea.removeFromRight(6);
        sendRows_[row].destCombo.setBounds(rowArea);
        area.removeFromTop(2);
    }

    syncActiveTrackNameEditorDisplay();
}

void InspectorView::textEditorReturnKeyPressed(juce::TextEditor& editor)
{
    if (&editor == &channelVolumeDbEditor_)
    {
        commitVolumeField();
    }
    else if (&editor == &activeTrackNameEditor_)
    {
        commitActiveTrackNameField();
    }
    else
    {
        for (int row = 0; row < kVisibleSendRows; ++row)
        {
            if (&editor == &sendRows_[row].amountEditor)
            {
                commitSendAmountField(row);
                break;
            }
        }
    }
}

void InspectorView::textEditorEscapeKeyPressed(juce::TextEditor& editor)
{
    if (&editor == &activeTrackNameEditor_)
    {
        inspectorNameEditorGuard_ = true;
        activeTrackNameEditor_.setText(activeTrackPlainName_, juce::dontSendNotification);
        inspectorNameEditorGuard_ = false;
        return;
    }

    if (&editor == &channelVolumeDbEditor_)
    {
        const std::shared_ptr<const SessionSnapshot> snap = session_.loadSessionSnapshotForAudioThread();
        if (snap == nullptr || snap->getNumTracks() <= 0)
        {
            return;
        }
        const TrackId active = session_.getActiveTrackId();
        const int idx = snap->findTrackIndexById(active);
        if (idx < 0)
        {
            return;
        }
        setVolumeEditorTextFromLinearGain(snap->getTrack(idx).getChannelFaderGain());
        return;
    }

    for (int row = 0; row < kVisibleSendRows; ++row)
    {
        if (&editor == &sendRows_[row].amountEditor)
        {
            const std::shared_ptr<const SessionSnapshot> snap
                = session_.loadSessionSnapshotForAudioThread();
            if (snap == nullptr)
            {
                return;
            }
            const TrackId active = session_.getActiveTrackId();
            const int idx = snap->findTrackIndexById(active);
            const int sendIndex
                = findTrackSendVectorIndexForUiSlot(snap->getTrack(idx).getSends(), row);
            if (idx < 0 || sendIndex < 0)
            {
                return;
            }
            setSendAmountEditorText(row, snap->getTrack(idx).getSend(sendIndex).amountLinear);
            return;
        }
    }
}

void InspectorView::textEditorFocusLost(juce::TextEditor& editor)
{
    if (&editor == &channelVolumeDbEditor_)
    {
        commitVolumeField();
    }
    else if (&editor == &activeTrackNameEditor_)
    {
        commitActiveTrackNameField();
    }
    else
    {
        for (int row = 0; row < kVisibleSendRows; ++row)
        {
            if (&editor == &sendRows_[row].amountEditor)
            {
                commitSendAmountField(row);
                break;
            }
        }
    }
}
