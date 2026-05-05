#include "ui/InspectorView.h"

#include "domain/Session.h"
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

    void itemDragEnter(const SourceDetails&) override
    {
        owner_.notifyInsertStageDropHover(stage_);
    }

    void itemDragMove(const SourceDetails&) override
    {
        owner_.notifyInsertStageDropHover(stage_);
    }

    void itemDragExit(const SourceDetails&) override
    {
        owner_.clearInsertStageDropHover();
    }

    void itemDropped(const SourceDetails& dragSourceDetails) override
    {
        TrackId tid = kInvalidTrackId;
        InsertSlotId sid = kInvalidInsertSlotId;
        InsertStage src = InsertStage::Post;
        if (!parseInsertRowDragDescription(dragSourceDetails.description, tid, sid, src))
        {
            owner_.clearInsertStageDropHover();
            return;
        }
        owner_.handleInsertStageDropped(tid, sid, src, stage_);
    }

private:
    InspectorView& owner_;
    InsertStage stage_;
};

class InspectorView::InsertStageDropSlot final : public juce::Component,
                                               public juce::DragAndDropTarget
{
public:
    InsertStageDropSlot(InspectorView& owner, InsertStage stage)
        : owner_(owner)
        , stage_(stage)
    {
        setOpaque(false);
        setInterceptsMouseClicks(true, false);
    }

    void paint(juce::Graphics& g) override
    {
        const auto bounds = getLocalBounds().toFloat().reduced(0.5f);
        constexpr float kCorner = 4.f;

        const juce::Colour fill = findColour(juce::TextButton::buttonColourId).withAlpha(0.12f);
        g.setColour(fill);
        g.fillRoundedRectangle(bounds, kCorner);

        juce::Path outline;
        outline.addRoundedRectangle(bounds, kCorner);
        const float dashLens[] = { 4.f, 3.f };
        juce::Path dashed;
        juce::PathStrokeType dashStroke(1.f);
        dashStroke.createDashedStroke(dashed, outline, dashLens, 2);
        g.setColour(findColour(juce::TextEditor::outlineColourId).withAlpha(0.55f));
        g.strokePath(dashed, juce::PathStrokeType(1.f));

        const juce::Font font(juce::FontOptions(12.0f));
        const juce::String text = "Drop here";
        const float pad = 8.f;
        const float maxW = juce::jmax(4.f, bounds.getWidth() - 2.f * pad);
        const juce::String elided = elideTextToFitWidth(text, font, maxW);
        g.setColour(findColour(juce::Label::textColourId).withAlpha(0.65f));
        g.setFont(font);
        g.drawText(elided, bounds.reduced(pad, 0.f), juce::Justification::centred, true);
    }

    bool isInterestedInDragSource(const SourceDetails& dragSourceDetails) override
    {
        return owner_.isCrossStageInsertDropAccepted(dragSourceDetails.description, stage_);
    }

    void itemDragEnter(const SourceDetails&) override
    {
        owner_.notifyInsertStageDropHover(stage_);
    }

    void itemDragMove(const SourceDetails&) override
    {
        owner_.notifyInsertStageDropHover(stage_);
    }

    void itemDragExit(const SourceDetails&) override
    {
        owner_.clearInsertStageDropHover();
    }

    void itemDropped(const SourceDetails& dragSourceDetails) override
    {
        TrackId tid = kInvalidTrackId;
        InsertSlotId sid = kInvalidInsertSlotId;
        InsertStage src = InsertStage::Post;
        if (!parseInsertRowDragDescription(dragSourceDetails.description, tid, sid, src))
        {
            owner_.clearInsertStageDropHover();
            return;
        }
        owner_.handleInsertStageDropped(tid, sid, src, stage_);
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

    void itemDragEnter(const SourceDetails&) override
    {
        owner_.notifyInsertStageDropHover(stage_);
    }

    void itemDragMove(const SourceDetails&) override
    {
        owner_.notifyInsertStageDropHover(stage_);
    }

    void itemDragExit(const SourceDetails&) override
    {
        owner_.clearInsertStageDropHover();
    }

    void itemDropped(const SourceDetails& dragSourceDetails) override
    {
        TrackId tid = kInvalidTrackId;
        InsertSlotId sid = kInvalidInsertSlotId;
        InsertStage src = InsertStage::Post;
        if (!parseInsertRowDragDescription(dragSourceDetails.description, tid, sid, src))
        {
            owner_.clearInsertStageDropHover();
            return;
        }
        owner_.handleInsertStageDropped(tid, sid, src, stage_);
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
        const TrackId t = session_.getActiveTrackId();
        if (t == kInvalidTrackId || !pluginHost_.requestAdd)
        {
            return;
        }
        pluginHost_.requestAdd(t, InsertStage::Post);
    };
    addAndMakeVisible(addPostInsertButton_);

    preDropSlot_ = std::make_unique<InsertStageDropSlot>(*this, InsertStage::Pre);
    postDropSlot_ = std::make_unique<InsertStageDropSlot>(*this, InsertStage::Post);
    addAndMakeVisible(*preDropSlot_);
    addAndMakeVisible(*postDropSlot_);
    preDropSlot_->setVisible(false);
    postDropSlot_->setVisible(false);

    refreshFromSession();
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

void InspectorView::applyInsertCrossStageDragChrome() noexcept
{
    if (!insertDragSourceStage_.has_value())
    {
        return;
    }
    if (*insertDragSourceStage_ == InsertStage::Post)
    {
        addPreInsertButton_.setVisible(false);
    }
    else
    {
        addPostInsertButton_.setVisible(false);
    }
    if (preDropSlot_ != nullptr && postDropSlot_ != nullptr)
    {
        const bool showPreGhost = *insertDragSourceStage_ == InsertStage::Post;
        preDropSlot_->setVisible(showPreGhost);
        postDropSlot_->setVisible(!showPreGhost);
        if (showPreGhost)
        {
            preDropSlot_->toFront(false);
        }
        else
        {
            postDropSlot_->toFront(false);
        }
    }
}

void InspectorView::onInsertSlotDragStarted(const InsertStage sourceStage)
{
    insertDragSourceStage_ = sourceStage;
    applyInsertCrossStageDragChrome();
    resized();
    repaint();
}

void InspectorView::clearInsertSlotDragSession() noexcept
{
    insertDragSourceStage_.reset();
    addPreInsertButton_.setVisible(true);
    addPostInsertButton_.setVisible(true);
    if (preDropSlot_ != nullptr)
    {
        preDropSlot_->setVisible(false);
    }
    if (postDropSlot_ != nullptr)
    {
        postDropSlot_->setVisible(false);
    }
    clearInsertStageDropHover();
}

void InspectorView::dragOperationEnded(const juce::DragAndDropTarget::SourceDetails&)
{
    clearInsertSlotDragSession();
    resized();
    repaint();
}

std::optional<InsertStage> InspectorView::stageForLocalPoint(const juce::Point<int> p) const noexcept
{
    if (preDropSlot_ != nullptr && preDropSlot_->isVisible() && preDropSlot_->getBounds().contains(p))
    {
        return InsertStage::Pre;
    }
    if (postDropSlot_ != nullptr && postDropSlot_->isVisible() && postDropSlot_->getBounds().contains(p))
    {
        return InsertStage::Post;
    }
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
    const auto stage = stageForLocalPoint(details.localPosition);
    if (stage.has_value())
    {
        notifyInsertStageDropHover(*stage);
    }
    else
    {
        clearInsertStageDropHover();
    }
}

void InspectorView::itemDragMove(const juce::DragAndDropTarget::SourceDetails& details)
{
    const auto stage = stageForLocalPoint(details.localPosition);
    if (stage.has_value())
    {
        notifyInsertStageDropHover(*stage);
    }
    else
    {
        clearInsertStageDropHover();
    }
}

void InspectorView::itemDragExit(const juce::DragAndDropTarget::SourceDetails&)
{
    clearInsertStageDropHover();
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
    handleInsertStageDropped(tid, sid, src, *targetStage);
}

bool InspectorView::isCrossStageInsertDropAccepted(const juce::var& desc,
                                                   const InsertStage targetStage) const noexcept
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
    if (tid != expected)
    {
        return false;
    }
    return src != targetStage;
}

void InspectorView::notifyInsertStageDropHover(InsertStage stage) noexcept
{
    if (insertDragSourceStage_.has_value() && *insertDragSourceStage_ == stage)
    {
        clearInsertStageDropHover();
        return;
    }
    if (insertStageDropHoverActive_ && insertStageDropHoverStage_ == stage)
    {
        return;
    }
    insertStageDropHoverActive_ = true;
    insertStageDropHoverStage_ = stage;
    repaint(preInsertBlockBounds_);
    repaint(postInsertBlockBounds_);
}

void InspectorView::clearInsertStageDropHover() noexcept
{
    if (!insertStageDropHoverActive_)
    {
        return;
    }
    insertStageDropHoverActive_ = false;
    repaint(preInsertBlockBounds_);
    repaint(postInsertBlockBounds_);
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

void InspectorView::handleInsertStageDropped(const TrackId tid,
                                            const InsertSlotId sid,
                                            const InsertStage sourceStage,
                                            const InsertStage targetStage)
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
        return;
    }
    if (!pluginHost_.requestMoveToStage)
    {
        return;
    }

    pluginHost_.requestMoveToStage(tid, sid, targetStage);

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
    if (!insertStageDropHoverActive_)
    {
        return;
    }
    const juce::Rectangle<int> primary
        = insertStageDropHoverStage_ == InsertStage::Pre ? preInsertBlockBounds_
                                                        : postInsertBlockBounds_;
    if (primary.isEmpty())
    {
        return;
    }
    constexpr float kCorner = 4.f;
    g.setColour(findColour(juce::TextButton::buttonOnColourId).withAlpha(0.07f));
    g.fillRoundedRectangle(primary.toFloat().reduced(1.f), kCorner);
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

    applyInsertCrossStageDragChrome();
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
    if (preDropSlot_ != nullptr && preDropSlot_->isVisible())
    {
        preDropSlot_->setBounds(addPreInsertButton_.getBounds());
        preDropSlot_->toFront(false);
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
    if (postDropSlot_ != nullptr && postDropSlot_->isVisible())
    {
        postDropSlot_->setBounds(addPostInsertButton_.getBounds());
        postDropSlot_->toFront(false);
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
