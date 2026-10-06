#include "ui/mixer/MixerChannelStrip.h"

#include "domain/SessionSnapshot.h"
#include "ui/TrackChannelOptions.h"
#include "ui/TrackValueFieldText.h"

#include <algorithm>
#include <cmath>

namespace
{
    using namespace mixer_layout;
    using namespace track_strip_glyphs;

    constexpr juce::uint32 kStripBgArgb = 0xff232528;
    constexpr juce::uint32 kHeaderActiveArgb = 0xff2a4a5a;
    constexpr juce::uint32 kHeaderInactiveArgb = 0xff2b2e33;
    constexpr juce::uint32 kSeparatorArgb = 0xff3a3d42;
    constexpr juce::uint32 kCaptionTextArgb = 0xff9aa3ad;
    constexpr juce::uint32 kNameTextArgb = 0xfff5f5f5;
    constexpr juce::uint32 kDimTextArgb = 0xff6f757d;
    constexpr juce::uint32 kActiveStripeArgb = 0xff00bfff; // deepskyblue, like the active header stripe
    constexpr juce::uint32 kInsertUnavailableTextArgb = 0xffe0a040;

    constexpr int kCellSizePx = 20;
    constexpr int kCellGapPx = 3;

    void styleSectionCaption(juce::Label& l, const juce::String& text)
    {
        l.setText(text, juce::dontSendNotification);
        l.setFont(juce::FontOptions(9.5f, juce::Font::bold));
        l.setColour(juce::Label::textColourId, juce::Colour(kCaptionTextArgb));
        l.setJustificationType(juce::Justification::centredLeft);
        l.setInterceptsMouseClicks(false, false);
        l.setMinimumHorizontalScale(0.8f);
    }

    void styleSmallCaption(juce::Label& l, const juce::String& text)
    {
        l.setText(text, juce::dontSendNotification);
        l.setFont(juce::FontOptions(9.5f));
        l.setColour(juce::Label::textColourId, juce::Colour(kCaptionTextArgb));
        l.setJustificationType(juce::Justification::centredLeft);
        l.setInterceptsMouseClicks(false, false);
        l.setMinimumHorizontalScale(0.8f);
    }

    [[nodiscard]] juce::Rectangle<int> squareCellBody(const juce::Rectangle<int> cell) noexcept
    {
        const int side = juce::jmin(cell.getWidth(), cell.getHeight());
        return juce::Rectangle<int>(cell.getX(), cell.getY() + (cell.getHeight() - side) / 2, side, side);
    }

    [[nodiscard]] bool insertRowsEqual(const std::vector<InspectorInsertRow>& a, const std::vector<InspectorInsertRow>& b) noexcept
    {
        if (a.size() != b.size())
        {
            return false;
        }
        for (size_t i = 0; i < a.size(); ++i)
        {
            if (a[i].slotId != b[i].slotId || a[i].stage != b[i].stage || a[i].displayName != b[i].displayName)
            {
                return false;
            }
        }
        return true;
    }
} // namespace

// --- CellButton -------------------------------------------------------------------------------------

void MixerChannelStrip::CellButton::paint(juce::Graphics& g)
{
    StripButtonState s = state_;
    s.hovered = hover_;
    drawStripButton(g, squareCellBody(getLocalBounds()), s, juce::Colour(0xff1b1d20));
}

void MixerChannelStrip::CellButton::mouseUp(const juce::MouseEvent& e)
{
    if (state_.enabled && onClick != nullptr && getLocalBounds().contains(e.getPosition()) && !e.mods.isPopupMenu())
    {
        onClick();
    }
}

// --- InsertRowButton --------------------------------------------------------------------------------

void MixerChannelStrip::InsertRowButton::mouseUp(const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu())
    {
        if (onRightClick != nullptr && getLocalBounds().contains(e.getPosition()))
        {
            onRightClick(e);
        }
        return;
    }
    juce::TextButton::mouseUp(e);
}

// --- MixerChannelStrip ------------------------------------------------------------------------------

MixerChannelStrip::MixerChannelStrip(const TrackId trackId, const MixerStripBindings& bindings, LevelMeterHub* const hub)
    : trackId_(trackId)
    , bindings_(bindings)
    , meterHub_(hub)
{
    setOpaque(true);

    // Header -----------------------------------------------------------------------------------
    nameLabel_.setFont(juce::FontOptions(13.0f, juce::Font::bold));
    nameLabel_.setColour(juce::Label::textColourId, juce::Colour(kNameTextArgb));
    nameLabel_.setJustificationType(juce::Justification::centredLeft);
    nameLabel_.setMinimumHorizontalScale(0.6f);
    nameLabel_.setInterceptsMouseClicks(false, false); // clicks fall through to the strip (activate)
    addAndMakeVisible(nameLabel_);
    kindLabel_.setFont(juce::FontOptions(9.0f, juce::Font::bold));
    kindLabel_.setColour(juce::Label::textColourId, juce::Colour(kCaptionTextArgb));
    kindLabel_.setJustificationType(juce::Justification::centredLeft);
    kindLabel_.setInterceptsMouseClicks(false, false);
    addAndMakeVisible(kindLabel_);

    instrumentEditorCell_.onClick = [this] { if (bindings_.openInstrumentEditor) bindings_.openInstrumentEditor(trackId_); };
    instrumentEditorCell_.setTooltip("Open the instrument's editor");
    powerCell_.onClick = [this] { if (bindings_.togglePower) (void)bindings_.togglePower(trackId_); };
    muteCell_.onClick = [this] { if (bindings_.toggleMute) bindings_.toggleMute(trackId_); };
    monitorCell_.onClick = [this] { if (bindings_.toggleMonitor) bindings_.toggleMonitor(trackId_); };
    armCell_.onClick = [this] { if (bindings_.toggleRecordArm) bindings_.toggleRecordArm(trackId_); };
    alternativesCell_.onClick = [this] {
        if (bindings_.showInstrumentAlternatives)
        {
            bindings_.showInstrumentAlternatives(trackId_, alternativesCell_.getScreenBounds());
        }
    };
    for (CellButton* c : { &instrumentEditorCell_, &powerCell_, &muteCell_, &monitorCell_, &armCell_, &alternativesCell_ })
    {
        addChildComponent(*c);
    }

    // Routing ----------------------------------------------------------------------------------
    styleSectionCaption(routingCaption_, "ROUTING");
    addChildComponent(routingCaption_);
    for (int r = 0; r < kRoutingRowsPerStrip; ++r)
    {
        RoutingRow& row = routing_[static_cast<size_t>(r)];
        styleSmallCaption(row.caption, {});
        addChildComponent(row.caption);
        row.combo.onChange = [this, r] { routingRowChanged(r); };
        addChildComponent(row.combo);
        row.infoLabel.setFont(juce::FontOptions(10.5f));
        row.infoLabel.setColour(juce::Label::textColourId, juce::Colour(kNameTextArgb));
        row.infoLabel.setJustificationType(juce::Justification::centredLeft);
        row.infoLabel.setMinimumHorizontalScale(0.6f);
        row.infoLabel.setInterceptsMouseClicks(false, false);
        addChildComponent(row.infoLabel);
    }

    // Pre-gain ---------------------------------------------------------------------------------
    styleSectionCaption(preGainCaption_, "PRE-GAIN");
    addChildComponent(preGainCaption_);
    preGainEditor_.setJustification(juce::Justification::centredRight);
    preGainEditor_.setSelectAllWhenFocused(true);
    preGainEditor_.setTooltip("Input trim in dB before the Pre inserts and the fader (audio tracks). Return commits; Ctrl/Cmd+click resets to 0.0 dB.");
    preGainEditor_.addListener(this);
    preGainEditor_.addMouseListener(this, false);
    addChildComponent(preGainEditor_);
    styleSmallCaption(preGainUnitLabel_, "dB");
    addChildComponent(preGainUnitLabel_);

    // Inserts ----------------------------------------------------------------------------------
    for (InsertStageUi* stageUi : { &preInserts_, &postInserts_ })
    {
        const InsertStage stage = stageUi == &preInserts_ ? InsertStage::Pre : InsertStage::Post;
        styleSectionCaption(stageUi->caption, stage == InsertStage::Pre ? "PRE INSERTS" : "POST INSERTS");
        addChildComponent(stageUi->caption);
        for (int r = 0; r < kInsertRowsPerStage; ++r)
        {
            auto b = std::make_unique<InsertRowButton>();
            b->setConnectedEdges(0);
            b->onClick = [this, stage, r] {
                InsertStageUi& ui = stage == InsertStage::Pre ? preInserts_ : postInserts_;
                const InsertSlotId sid = ui.rowSlots[static_cast<size_t>(r)];
                if (sid != kInvalidInsertSlotId && bindings_.inserts.requestEdit)
                {
                    bindings_.inserts.requestEdit(trackId_, sid);
                }
            };
            b->onRightClick = [this, stage, r](const juce::MouseEvent& e) { showInsertRowMenu(stage, r, e); };
            addChildComponent(*b);
            stageUi->rows[static_cast<size_t>(r)] = std::move(b);
            stageUi->rowSlots[static_cast<size_t>(r)] = kInvalidInsertSlotId;
        }
        styleSmallCaption(stageUi->moreLabel, {});
        stageUi->moreLabel.setJustificationType(juce::Justification::centred);
        addChildComponent(stageUi->moreLabel);
        stageUi->addButton.setButtonText("+ Add");
        stageUi->addButton.setTooltip(stage == InsertStage::Pre ? "Add a Pre insert (VST3)" : "Add a Post insert (VST3)");
        stageUi->addButton.onClick = [this, stage] {
            if (bindings_.inserts.requestAdd)
            {
                bindings_.inserts.requestAdd(trackId_, stage);
            }
        };
        addChildComponent(stageUi->addButton);
    }

    // Sends ------------------------------------------------------------------------------------
    styleSectionCaption(sendsCaption_, "SENDS");
    addChildComponent(sendsCaption_);
    for (int r = 0; r < kSendRows; ++r)
    {
        SendRow& row = sends_[static_cast<size_t>(r)];
        row.enable.setButtonText({});
        row.enable.setTooltip("Send on / off");
        row.enable.onClick = [this, r] {
            SendRow& s = sends_[static_cast<size_t>(r)];
            if (s.guard || !bindings_.edits.setSendEnabled)
            {
                return;
            }
            bindings_.edits.setSendEnabled(trackId_, r, s.enable.getToggleState());
        };
        addChildComponent(row.enable);
        row.dest.setTooltip("Send destination (a Group bus)");
        row.dest.onChange = [this, r] { sendDestinationChanged(r); };
        addChildComponent(row.dest);
        row.amount.setJustification(juce::Justification::centredRight);
        row.amount.setSelectAllWhenFocused(true);
        row.amount.setTooltip("Send level in dB (0.00 = unity, -Inf = off)");
        row.amount.addListener(this);
        addChildComponent(row.amount);
    }
    styleSmallCaption(sendsExtraLabel_, {});
    addChildComponent(sendsExtraLabel_);

    // Fader / pan / meter ----------------------------------------------------------------------
    pan_.onPanChanged = [this](const float pan) {
        if (!panGuard_ && bindings_.setStereoPan)
        {
            bindings_.setStereoPan(trackId_, pan);
        }
    };
    addChildComponent(pan_);
    fader_.onGainChanged = [this](const float linear) {
        if (!faderGuard_ && bindings_.setChannelFaderGain)
        {
            bindings_.setChannelFaderGain(trackId_, linear);
        }
    };
    addChildComponent(fader_);
    styleSmallCaption(meterCaption_, "Out");
    meterCaption_.setJustificationType(juce::Justification::centred);
    addChildComponent(meterCaption_);
    meter_.setTooltip("Audio output of this channel after inserts, fader and pan (Stereo Out: the final master signal before the audio device). Sample peak in dBFS; click to reset the overload lamp.");
    meter_.onOverloadLatchReset = [this] {
        if (meterHub_ != nullptr)
        {
            meterHub_->acknowledgeOverload(trackId_);
        }
    };
    addChildComponent(meter_);

    if (meterHub_ != nullptr)
    {
        meterHub_->addListener(this);
    }
}

MixerChannelStrip::~MixerChannelStrip()
{
    preGainEditor_.removeMouseListener(this);
    if (meterHub_ != nullptr)
    {
        meterHub_->removeListener(this);
    }
}

// --- layout -----------------------------------------------------------------------------------------

void MixerChannelStrip::applyLayout(const ComputedLayout& layout)
{
    layout_ = layout;
    resized();
}

void MixerChannelStrip::resized()
{
    const ComputedLayout& L = layout_;

    // Header
    {
        auto h = L.header;
        h.removeFromTop(2);
        nameLabel_.setBounds(h.removeFromTop(kHeaderNameRowHeightPx));
        kindLabel_.setBounds(h.removeFromTop(kHeaderKindRowHeightPx));
        auto cells = h.removeFromTop(kHeaderButtonsRowHeightPx);
        // Cells in a fixed order; invisible ones take no space.
        for (CellButton* c : { &instrumentEditorCell_, &powerCell_, &muteCell_, &monitorCell_, &armCell_, &alternativesCell_ })
        {
            if (!c->isVisible())
            {
                continue;
            }
            c->setBounds(cells.removeFromLeft(kCellSizePx).withHeight(kCellSizePx).withY(cells.getY() + (kHeaderButtonsRowHeightPx - kCellSizePx) / 2));
            cells.removeFromLeft(kCellGapPx);
        }
    }

    // Routing
    {
        auto b = L.band(Section::Routing);
        const bool show = !b.isEmpty();
        routingCaption_.setVisible(show);
        if (show)
        {
            routingCaption_.setBounds(b.removeFromTop(kSectionCaptionHeightPx));
        }
        for (int r = 0; r < kRoutingRowsPerStrip; ++r)
        {
            RoutingRow& row = routing_[static_cast<size_t>(r)];
            const bool rowUsed = show && row.role != RoutingRole::None;
            row.caption.setVisible(rowUsed);
            row.combo.setVisible(rowUsed && row.role != RoutingRole::DeviceOutputInfo);
            row.infoLabel.setVisible(rowUsed && row.role == RoutingRole::DeviceOutputInfo);
            if (!show)
            {
                continue;
            }
            auto rowArea = b.removeFromTop(kRoutingRowHeightPx);
            row.caption.setBounds(rowArea.removeFromTop(kRoutingCaptionHeightPx));
            const auto ctl = rowArea.removeFromTop(kRoutingControlHeightPx);
            row.combo.setBounds(ctl);
            row.infoLabel.setBounds(ctl);
        }
    }

    // Pre-gain
    {
        auto b = L.band(Section::PreGain);
        const bool show = !b.isEmpty() && kind_ == TrackKind::Audio;
        preGainCaption_.setVisible(show);
        preGainEditor_.setVisible(show);
        preGainUnitLabel_.setVisible(show);
        if (show)
        {
            preGainCaption_.setBounds(b.removeFromTop(kSectionCaptionHeightPx));
            auto row = b.removeFromTop(kPreGainValueRowHeightPx);
            preGainUnitLabel_.setBounds(row.removeFromRight(22));
            row.removeFromRight(4);
            preGainEditor_.setBounds(row.removeFromRight(juce::jmin(56, row.getWidth())));
        }
    }

    // Inserts
    const bool audioPath = kind_ != TrackKind::Midi;
    for (InsertStageUi* ui : { &preInserts_, &postInserts_ })
    {
        auto b = L.band(ui == &preInserts_ ? Section::PreInserts : Section::PostInserts);
        const bool show = !b.isEmpty() && audioPath;
        ui->caption.setVisible(show);
        ui->addButton.setVisible(show);
        if (!show)
        {
            for (auto& r : ui->rows)
            {
                r->setVisible(false);
            }
            ui->moreLabel.setVisible(false);
            continue;
        }
        ui->caption.setBounds(b.removeFromTop(kSectionCaptionHeightPx));
        const int shownRows = static_cast<int>(ui->lastRows.size());
        const bool overflow = shownRows > kInsertRowsPerStage;
        for (int r = 0; r < kInsertRowsPerStage; ++r)
        {
            auto rowArea = b.removeFromTop(kInsertRowHeightPx).reduced(0, 1);
            const bool lastRowIsMore = overflow && r == kInsertRowsPerStage - 1;
            const bool rowVisible = r < shownRows && !lastRowIsMore;
            ui->rows[static_cast<size_t>(r)]->setVisible(rowVisible);
            ui->rows[static_cast<size_t>(r)]->setBounds(rowArea);
            if (lastRowIsMore)
            {
                ui->moreLabel.setBounds(rowArea);
            }
        }
        ui->moreLabel.setVisible(overflow);
        ui->addButton.setBounds(b.removeFromTop(kInsertRowHeightPx).reduced(0, 1));
    }

    // Sends
    {
        auto b = L.band(Section::Sends);
        const bool show = !b.isEmpty() && audioPath && !isMaster_;
        sendsCaption_.setVisible(show);
        if (show)
        {
            auto cap = b.removeFromTop(kSectionCaptionHeightPx);
            sendsExtraLabel_.setBounds(cap.removeFromRight(juce::jmin(70, cap.getWidth() / 2)));
            sendsCaption_.setBounds(cap);
        }
        sendsExtraLabel_.setVisible(show && sendsExtraLabel_.getText().isNotEmpty());
        for (int r = 0; r < kSendRows; ++r)
        {
            SendRow& row = sends_[static_cast<size_t>(r)];
            row.enable.setVisible(show);
            row.dest.setVisible(show);
            row.amount.setVisible(show);
            if (!show)
            {
                continue;
            }
            auto rowArea = b.removeFromTop(kSendRowHeightPx).reduced(0, 1);
            row.enable.setBounds(rowArea.removeFromLeft(18));
            row.amount.setBounds(rowArea.removeFromRight(44));
            rowArea.removeFromRight(3);
            row.dest.setBounds(rowArea);
        }
    }

    // Lower band: pan + fader | meter
    {
        auto fb = L.band(Section::Faders);
        const bool showFader = !fb.isEmpty() && audioPath;
        pan_.setVisible(showFader);
        fader_.setVisible(showFader);
        if (showFader)
        {
            pan_.setBounds(fb.removeFromTop(kPanRowHeightPx).reduced(2, 1));
            fb.removeFromTop(2);
            // The fader column is centred inside its band (wide scale + travel = 52 px).
            const int faderW = juce::jmin(fb.getWidth(), ChannelFaderComponent::preferredWidth());
            fader_.setBounds(fb.getX() + (fb.getWidth() - faderW) / 2, fb.getY(), faderW, fb.getHeight());
        }
        auto mb = L.band(Section::Meters);
        const bool showMeter = !mb.isEmpty() && audioPath;
        meterCaption_.setVisible(showMeter);
        meter_.setVisible(showMeter);
        if (showMeter)
        {
            meterCaption_.setBounds(mb.removeFromTop(kSectionCaptionHeightPx));
            // Align the meter bars with the fader travel: skip the pan row height on the fader side.
            if (showFader)
            {
                mb.removeFromTop(juce::jmax(0, kPanRowHeightPx + 2 - kSectionCaptionHeightPx));
            }
            const int meterW = juce::jmin(mb.getWidth(), LevelMeterComponent::preferredWidthFor(2, mb.getWidth() >= 44));
            meter_.setShowScale(true);
            meter_.setShowScaleLabels(mb.getWidth() >= 44);
            meter_.setBounds(mb.getX() + (mb.getWidth() - meterW) / 2, mb.getY(), meterW, mb.getHeight());
        }
    }
}

// --- paint / mouse -----------------------------------------------------------------------------------

void MixerChannelStrip::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colour(kStripBgArgb));
    // Header plate (active rows use the arrangement header's active colour and stripe).
    const juce::Rectangle<int> header(0, 0, getWidth(), layout_.header.getBottom() + 2);
    g.setColour(juce::Colour(isActive_ ? kHeaderActiveArgb : kHeaderInactiveArgb));
    g.fillRect(header);
    if (isActive_)
    {
        g.setColour(juce::Colour(kActiveStripeArgb));
        g.fillRect(header.withHeight(3));
    }
    // Section separators.
    g.setColour(juce::Colour(kSeparatorArgb));
    for (int i = 0; i < kSectionCount; ++i)
    {
        const auto b = layout_.bands[static_cast<size_t>(i)];
        const Section s = static_cast<Section>(i);
        if (b.isEmpty() || s == Section::Faders || s == Section::Meters)
        {
            continue;
        }
        g.fillRect(b.getX(), b.getY() - 2, b.getWidth(), 1);
    }
    if (!layout_.lowerBand.isEmpty())
    {
        g.fillRect(layout_.lowerBand.getX(), layout_.lowerBand.getY() - 2, layout_.lowerBand.getWidth(), 1);
    }
    // Right edge.
    g.fillRect(getWidth() - 1, 0, 1, getHeight());
    // Dimmed placeholders for bands this kind does not use (keeps the alignment legible).
    g.setColour(juce::Colour(kDimTextArgb));
    g.setFont(juce::FontOptions(9.5f));
    const auto placeholder = [&g](const juce::Rectangle<int> band, const char* text) {
        if (!band.isEmpty())
        {
            g.drawText(text, band.withHeight(kSectionCaptionHeightPx), juce::Justification::centredLeft);
        }
    };
    if (kind_ != TrackKind::Audio)
    {
        placeholder(layout_.band(Section::PreGain), "PRE-GAIN (audio tracks)");
    }
    if (kind_ == TrackKind::Midi)
    {
        placeholder(layout_.band(Section::PreInserts), "PRE INSERTS (no audio path)");
        placeholder(layout_.band(Section::PostInserts), "POST INSERTS (no audio path)");
        placeholder(layout_.band(Section::Sends), "SENDS (no audio path)");
        placeholder(layout_.lowerBand, "MIDI track: no audio fader / meter");
    }
    else if (isMaster_)
    {
        placeholder(layout_.band(Section::Sends), "SENDS (Stereo Out never sends)");
    }
}

void MixerChannelStrip::mouseDown(const juce::MouseEvent& e)
{
    if (e.eventComponent == &preGainEditor_)
    {
        // Ctrl/Cmd+click on the pre-gain field resets to 0.0 dB (DAL's reset gesture).
        if (e.mods.isCommandDown() && bindings_.edits.setPreGainDb)
        {
            bindings_.edits.setPreGainDb(trackId_, kTrackPreGainDbDefault);
            setPreGainEditorText(kTrackPreGainDbDefault);
        }
        return;
    }
    // Background / name click: make this the active track (same path as a header click).
    if (bindings_.activateTrack)
    {
        bindings_.activateTrack(trackId_);
    }
}

// --- refresh -----------------------------------------------------------------------------------------

juce::String MixerChannelStrip::kindTag(const TrackKind kind) const
{
    switch (kind)
    {
    case TrackKind::Audio: return "AUDIO";
    case TrackKind::Instrument: return "INSTRUMENT";
    case TrackKind::Midi: return "MIDI";
    case TrackKind::Group: return "GROUP";
    case TrackKind::Master: return "STEREO OUT";
    }
    return {};
}

void MixerChannelStrip::configureRoutingRow(const int row, const RoutingRole role, const juce::String& caption)
{
    RoutingRow& r = routing_[static_cast<size_t>(row)];
    r.role = role;
    r.caption.setText(caption, juce::dontSendNotification);
}

void MixerChannelStrip::applyKindLayout(const TrackKind kind)
{
    kind_ = kind;
    isMaster_ = (kind == TrackKind::Master);
    kindApplied_ = true;
    kindLabel_.setText(kindTag(kind), juce::dontSendNotification);
    for (int r = 0; r < kRoutingRowsPerStrip; ++r)
    {
        configureRoutingRow(r, RoutingRole::None, {});
    }
    switch (kind)
    {
    case TrackKind::Audio:
        configureRoutingRow(0, RoutingRole::AudioInput, "Audio Input");
        configureRoutingRow(1, RoutingRole::AudioOutput, "Audio Output");
        break;
    case TrackKind::Instrument:
        configureRoutingRow(0, RoutingRole::MidiInput, "MIDI Input");
        configureRoutingRow(1, RoutingRole::MidiInputChannel, "Input Channel");
        configureRoutingRow(2, RoutingRole::MidiOutputChannel, "MIDI Channel");
        configureRoutingRow(3, RoutingRole::AudioOutput, "Audio Output");
        break;
    case TrackKind::Midi:
        configureRoutingRow(0, RoutingRole::MidiInput, "MIDI Input");
        configureRoutingRow(1, RoutingRole::MidiInputChannel, "Input Channel");
        configureRoutingRow(2, RoutingRole::MidiDestination, "MIDI To");
        configureRoutingRow(3, RoutingRole::MidiOutputChannel, "MIDI Channel");
        break;
    case TrackKind::Group:
        configureRoutingRow(0, RoutingRole::AudioOutput, "Audio Output");
        break;
    case TrackKind::Master:
        configureRoutingRow(0, RoutingRole::DeviceOutputInfo, "Device output");
        break;
    }
    // Base buttons per kind (header strip rules): Group / Master = Mute only; Audio = Power,
    // Mute, Monitor, R; Instrument adds the editor and alternatives cells; Midi = Power, Mute,
    // Monitor, R (MIDI monitor / arm).
    const bool audioRow = kind == TrackKind::Audio;
    const bool instrumentRow = kind == TrackKind::Instrument;
    const bool midiRow = kind == TrackKind::Midi;
    instrumentEditorCell_.setVisible(instrumentRow);
    alternativesCell_.setVisible(instrumentRow);
    powerCell_.setVisible(audioRow || instrumentRow || midiRow);
    muteCell_.setVisible(true);
    monitorCell_.setVisible(audioRow || instrumentRow || midiRow);
    armCell_.setVisible(audioRow || instrumentRow || midiRow);
    meterCaption_.setText(isMaster_ ? "Stereo Out" : "Out", juce::dontSendNotification);
    meter_.clear();
    preInserts_.lastRows.clear();
    preInserts_.populated = false;
    postInserts_.lastRows.clear();
    postInserts_.populated = false;
    resized();
    repaint();
}

void MixerChannelStrip::refreshFromSession(const SessionSnapshot& snap, const bool isActive)
{
    const int idx = snap.findTrackIndexById(trackId_);
    if (idx < 0)
    {
        orphaned_ = true;
        return;
    }
    orphaned_ = false;
    const Track& tr = snap.getTrack(idx);
    if (!kindApplied_ || tr.getKind() != kind_)
    {
        applyKindLayout(tr.getKind());
    }
    if (isActive != isActive_)
    {
        isActive_ = isActive;
        repaint();
    }
    const juce::String name = isMaster_ ? juce::String(kMasterTrackDisplayName) : tr.getName();
    if (nameLabel_.getText() != name)
    {
        nameLabel_.setText(name, juce::dontSendNotification);
        nameLabel_.setTooltip(name);
    }

    refreshBaseButtons(tr);

    for (int r = 0; r < kRoutingRowsPerStrip; ++r)
    {
        populateRoutingRow(r, tr, snap);
    }

    if (kind_ == TrackKind::Audio)
    {
        const float db = tr.getPreGainDb();
        if (!preGainEditor_.hasKeyboardFocus(false) && (lastPreGainDb_ != db || preGainEditor_.getText().isEmpty()))
        {
            setPreGainEditorText(db);
        }
        lastPreGainDb_ = db;
    }

    if (kind_ != TrackKind::Midi && bindings_.inserts.getInsertRows)
    {
        const std::vector<InspectorInsertRow> rows = bindings_.inserts.getInsertRows(trackId_);
        std::vector<InspectorInsertRow> pre, post;
        for (const auto& r : rows)
        {
            (r.stage == InsertStage::Pre ? pre : post).push_back(r);
        }
        refreshInserts(InsertStage::Pre, pre);
        refreshInserts(InsertStage::Post, post);
    }

    if (kind_ != TrackKind::Midi && !isMaster_)
    {
        refreshSends(tr, snap);
    }

    if (kind_ != TrackKind::Midi)
    {
        if (!fader_.isDragging() && !fader_.isValueFieldBeingEdited())
        {
            faderGuard_ = true;
            fader_.setLinearGain(tr.getChannelFaderGain(), juce::dontSendNotification);
            faderGuard_ = false;
        }
        if (!pan_.isMouseButtonDown())
        {
            panGuard_ = true;
            pan_.setPan(tr.getStereoPan(), juce::dontSendNotification);
            panGuard_ = false;
        }
    }
}

void MixerChannelStrip::refreshBaseButtons(const Track& tr)
{
    const auto setState = [](CellButton& c, const bool enabled, const bool active) {
        if (c.state_.enabled != enabled || c.state_.active != active)
        {
            c.state_.enabled = enabled;
            c.state_.active = active;
            c.repaint();
        }
    };
    const bool powerInteractable = bindings_.isPowerInteractable ? bindings_.isPowerInteractable() : true;
    setState(powerCell_, powerInteractable, !tr.isTrackOff());
    powerCell_.setTooltip(tr.isTrackOff() ? "Track is OFF (click to switch on)" : "Track is ON (click to switch off; not while playing or recording)");
    setState(muteCell_, true, tr.isMuted());
    muteCell_.setTooltip(tr.isMuted() ? "Muted (click to unmute)" : "Mute");
    const bool monitorAvail = bindings_.monitorAvailable ? bindings_.monitorAvailable(trackId_) : false;
    setState(monitorCell_, monitorAvail, monitorAvail && bindings_.isMonitorOn && bindings_.isMonitorOn(trackId_));
    monitorCell_.setTooltip(kind_ == TrackKind::Audio ? "Input monitoring" : "Live MIDI monitoring");
    const bool armAvail = bindings_.armAvailable ? bindings_.armAvailable(trackId_) : false;
    setState(armCell_, armAvail, armAvail && bindings_.isRecordArmed && bindings_.isRecordArmed(trackId_));
    armCell_.setTooltip("Record-arm");
    if (kind_ == TrackKind::Instrument)
    {
        const bool editorAvail = bindings_.instrumentEditorAvailable ? bindings_.instrumentEditorAvailable(trackId_) : false;
        setState(instrumentEditorCell_, editorAvail, false);
        const bool altAvail = bindings_.instrumentAlternativesAvailable ? bindings_.instrumentAlternativesAvailable(trackId_) : false;
        setState(alternativesCell_, altAvail, false);
        alternativesCell_.setTooltip("Instrument alternatives (Primary / Secondary / proxy)");
    }
}

void MixerChannelStrip::populateRoutingRow(const int rowIndex, const Track& tr, const SessionSnapshot& snap)
{
    using namespace track_channel_options;
    RoutingRow& row = routing_[static_cast<size_t>(rowIndex)];
    if (row.role == RoutingRole::None)
    {
        return;
    }
    if (row.role == RoutingRole::DeviceOutputInfo)
    {
        const juce::String text = bindings_.deviceOutputDescription ? bindings_.deviceOutputDescription() : juce::String("(no audio device)");
        if (row.infoLabel.getText() != text)
        {
            row.infoLabel.setText(text, juce::dontSendNotification);
            row.infoLabel.setTooltip(text);
        }
        return;
    }
    // Rebuilding a combo while its popup is open would close it under the user's pointer.
    if (row.combo.isPopupActive())
    {
        return;
    }
    row.guard = true;
    switch (row.role)
    {
    case RoutingRole::AudioInput: {
        const InspectorAudioInputDeviceSnapshot dev = bindings_.audioInputDeviceSnapshot ? bindings_.audioInputDeviceSnapshot() : InspectorAudioInputDeviceSnapshot{};
        const auto list = audioInputOptions(tr, dev);
        row.inputValues = list.values;
        applyToCombo(row.combo, list);
        break;
    }
    case RoutingRole::AudioOutput: {
        const auto list = audioOutputOptions(snap, tr);
        row.trackIdValues = list.values;
        applyToCombo(row.combo, list);
        break;
    }
    case RoutingRole::MidiInput: {
        const InspectorMidiInputSnapshot midi = bindings_.midiInputSnapshot ? bindings_.midiInputSnapshot(trackId_) : InspectorMidiInputSnapshot{};
        const auto list = midiInputDeviceOptions(tr, midi);
        row.midiInputValues = list.values;
        applyToCombo(row.combo, list);
        row.combo.setTooltip(midi.statusLine.isNotEmpty() ? midi.statusLine : juce::String("Which MIDI device plays this track live (Monitor) and is recorded (R)."));
        break;
    }
    case RoutingRole::MidiInputChannel: {
        const auto list = midiInputChannelFilterOptions(tr);
        row.intValues = list.values;
        applyToCombo(row.combo, list);
        row.combo.setEnabled(tr.getMidiInputAssignment().mode != TrackMidiInputMode::None);
        break;
    }
    case RoutingRole::MidiOutputChannel: {
        const auto list = midiOutputChannelOptions(tr);
        row.intValues = list.values;
        applyToCombo(row.combo, list);
        break;
    }
    case RoutingRole::MidiDestination: {
        const auto list = midiDestinationOptions(snap, tr);
        row.trackIdValues = list.values;
        applyToCombo(row.combo, list);
        break;
    }
    case RoutingRole::None:
    case RoutingRole::DeviceOutputInfo:
        break;
    }
    row.guard = false;
}

void MixerChannelStrip::routingRowChanged(const int rowIndex)
{
    RoutingRow& row = routing_[static_cast<size_t>(rowIndex)];
    if (row.guard)
    {
        return;
    }
    const int pick = row.combo.getSelectedId();
    if (pick <= 0)
    {
        return;
    }
    const size_t ix = static_cast<size_t>(pick - 1);
    const std::shared_ptr<const SessionSnapshot> snap = bindings_.loadSnapshot ? bindings_.loadSnapshot() : nullptr;
    const int tix = snap != nullptr ? snap->findTrackIndexById(trackId_) : -1;
    switch (row.role)
    {
    case RoutingRole::AudioInput:
        if (ix < row.inputValues.size() && bindings_.edits.setAudioInput)
        {
            bindings_.edits.setAudioInput(trackId_, row.inputValues[ix]);
        }
        break;
    case RoutingRole::AudioOutput:
        if (ix < row.trackIdValues.size() && bindings_.edits.setRoutedOutput)
        {
            bindings_.edits.setRoutedOutput(trackId_, row.trackIdValues[ix]);
        }
        break;
    case RoutingRole::MidiInput:
        if (ix < row.midiInputValues.size() && bindings_.edits.setMidiInput)
        {
            TrackMidiInputAssignment next = row.midiInputValues[ix];
            // Only the device changes here; the row keeps its channel filter (Inspector rule).
            if (tix >= 0)
            {
                next.channelFilter = snap->getTrack(tix).getMidiInputAssignment().channelFilter;
            }
            bindings_.edits.setMidiInput(trackId_, next);
        }
        break;
    case RoutingRole::MidiInputChannel:
        if (ix < row.intValues.size() && bindings_.edits.setMidiInput && tix >= 0)
        {
            TrackMidiInputAssignment next = snap->getTrack(tix).getMidiInputAssignment();
            next.channelFilter = row.intValues[ix];
            bindings_.edits.setMidiInput(trackId_, next);
        }
        break;
    case RoutingRole::MidiOutputChannel:
        if (ix < row.intValues.size() && bindings_.edits.setMidiOutputChannel)
        {
            bindings_.edits.setMidiOutputChannel(trackId_, row.intValues[ix]);
        }
        break;
    case RoutingRole::MidiDestination:
        if (ix < row.trackIdValues.size() && bindings_.edits.setMidiDestination)
        {
            bindings_.edits.setMidiDestination(trackId_, row.trackIdValues[ix]);
        }
        break;
    case RoutingRole::None:
    case RoutingRole::DeviceOutputInfo:
        break;
    }
}

void MixerChannelStrip::refreshInserts(const InsertStage stage, const std::vector<InspectorInsertRow>& rows)
{
    InsertStageUi& ui = stage == InsertStage::Pre ? preInserts_ : postInserts_;
    if (ui.populated && insertRowsEqual(ui.lastRows, rows))
    {
        return;
    }
    ui.populated = true;
    ui.lastRows = rows;
    const int n = static_cast<int>(rows.size());
    const bool overflow = n > kInsertRowsPerStage;
    for (int r = 0; r < kInsertRowsPerStage; ++r)
    {
        InsertRowButton& b = *ui.rows[static_cast<size_t>(r)];
        const bool lastRowIsMore = overflow && r == kInsertRowsPerStage - 1;
        if (r < n && !lastRowIsMore)
        {
            const InspectorInsertRow& row = rows[static_cast<size_t>(r)];
            ui.rowSlots[static_cast<size_t>(r)] = row.slotId;
            b.setButtonText(row.displayName);
            b.setTooltip(row.displayName + " — click: open editor; right-click: move / stage / remove");
            const bool unavailable = row.displayName.endsWithIgnoreCase("(unavailable)");
            b.setColour(juce::TextButton::textColourOffId, juce::Colour(unavailable ? kInsertUnavailableTextArgb : kNameTextArgb));
        }
        else
        {
            ui.rowSlots[static_cast<size_t>(r)] = kInvalidInsertSlotId;
            b.setButtonText({});
        }
    }
    ui.moreLabel.setText(overflow ? "+" + juce::String(n - (kInsertRowsPerStage - 1)) + " more (Inspector)" : juce::String(), juce::dontSendNotification);
    resized();
}

void MixerChannelStrip::showInsertRowMenu(const InsertStage stage, const int rowIndex, const juce::MouseEvent&)
{
    InsertStageUi& ui = stage == InsertStage::Pre ? preInserts_ : postInserts_;
    const InsertSlotId sid = ui.rowSlots[static_cast<size_t>(rowIndex)];
    if (sid == kInvalidInsertSlotId)
    {
        return;
    }
    const int countInStage = static_cast<int>(ui.lastRows.size());
    juce::PopupMenu menu;
    menu.addItem(1, "Open editor", bindings_.inserts.requestEdit != nullptr);
    menu.addSeparator();
    menu.addItem(2, "Move up", bindings_.inserts.requestReorderInStage != nullptr && rowIndex > 0);
    menu.addItem(3, "Move down", bindings_.inserts.requestReorderInStage != nullptr && rowIndex + 1 < countInStage);
    menu.addItem(4, stage == InsertStage::Pre ? "Move to Post (end)" : "Move to Pre (end)", bindings_.inserts.requestMoveToStageAtGap != nullptr);
    menu.addSeparator();
    menu.addItem(5, "Remove insert", bindings_.inserts.requestRemove != nullptr);
    juce::Component::SafePointer<MixerChannelStrip> self(this);
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(ui.rows[static_cast<size_t>(rowIndex)].get()),
                       [self, stage, rowIndex](const int result) {
                           if (self != nullptr && result > 0)
                           {
                               (void)self->performInsertRowAction(stage, rowIndex, result);
                           }
                       });
}

bool MixerChannelStrip::performInsertRowAction(const InsertStage stage, const int rowIndex, const int actionId)
{
    if (rowIndex < 0 || rowIndex >= kInsertRowsPerStage)
    {
        return false;
    }
    InsertStageUi& ui = stage == InsertStage::Pre ? preInserts_ : postInserts_;
    const InsertSlotId sid = ui.rowSlots[static_cast<size_t>(rowIndex)];
    if (sid == kInvalidInsertSlotId)
    {
        return false;
    }
    const int otherStageCount = static_cast<int>((stage == InsertStage::Pre ? postInserts_ : preInserts_).lastRows.size());
    const TrackId tid = trackId_;
    switch (actionId)
    {
    case 1:
        if (bindings_.inserts.requestEdit) { bindings_.inserts.requestEdit(tid, sid); return true; }
        return false;
    case 2:
        // Gap index before removal: moving up one row = the gap above the previous row.
        if (bindings_.inserts.requestReorderInStage && rowIndex > 0) { bindings_.inserts.requestReorderInStage(tid, sid, rowIndex - 1); return true; }
        return false;
    case 3:
        // Gap below the next row (gap indices count rows before removal).
        if (bindings_.inserts.requestReorderInStage) { bindings_.inserts.requestReorderInStage(tid, sid, rowIndex + 2); return true; }
        return false;
    case 4:
        if (bindings_.inserts.requestMoveToStageAtGap)
        {
            bindings_.inserts.requestMoveToStageAtGap(tid, sid, stage == InsertStage::Pre ? InsertStage::Post : InsertStage::Pre, otherStageCount);
            return true;
        }
        return false;
    case 5:
        if (bindings_.inserts.requestRemove) { bindings_.inserts.requestRemove(tid, sid); return true; }
        return false;
    default:
        return false;
    }
}

void MixerChannelStrip::refreshSends(const Track& tr, const SessionSnapshot& snap)
{
    using namespace track_channel_options;
    const int extra = countTrackSendsOutsideInspectorUiSlots(tr.getSends());
    const juce::String extraText = extra > 0 ? "+" + juce::String(extra) + " more" : juce::String();
    if (sendsExtraLabel_.getText() != extraText)
    {
        sendsExtraLabel_.setText(extraText, juce::dontSendNotification);
        sendsExtraLabel_.setTooltip(extra > 0 ? juce::String(extra) + " stored send(s) outside the four slots are kept and keep processing" : juce::String());
        sendsExtraLabel_.setVisible(sendsCaption_.isVisible() && extra > 0);
    }
    for (int r = 0; r < kSendRows; ++r)
    {
        SendRow& row = sends_[static_cast<size_t>(r)];
        row.guard = true;
        if (!row.dest.isPopupActive())
        {
            const auto list = sendDestinationOptions(snap, tr, r);
            row.destIds = list.values;
            applyToCombo(row.dest, list);
        }
        const int sendIndex = findTrackSendVectorIndexForUiSlot(tr.getSends(), r);
        const bool existing = sendIndex >= 0;
        row.amount.setEnabled(existing);
        row.enable.setEnabled(existing);
        if (existing)
        {
            const TrackSend& s = tr.getSend(sendIndex);
            row.enable.setToggleState(s.enabled, juce::dontSendNotification);
            if (!row.amount.hasKeyboardFocus(false))
            {
                const juce::String want = track_value_text::formatSendLinearToDbField(s.amountLinear);
                if (row.amount.getText() != want)
                {
                    row.amount.setText(want, juce::dontSendNotification);
                }
            }
        }
        else
        {
            row.enable.setToggleState(false, juce::dontSendNotification);
            if (!row.amount.hasKeyboardFocus(false))
            {
                const juce::String want = track_value_text::formatSendLinearToDbField(kSendAmountUnityLinear);
                if (row.amount.getText() != want)
                {
                    row.amount.setText(want, juce::dontSendNotification);
                }
            }
        }
        row.guard = false;
    }
}

void MixerChannelStrip::sendDestinationChanged(const int rowIndex)
{
    SendRow& row = sends_[static_cast<size_t>(rowIndex)];
    if (row.guard || !bindings_.edits.setSendDestination)
    {
        return;
    }
    const int pick = row.dest.getSelectedId();
    if (pick <= 0 || static_cast<size_t>(pick - 1) >= row.destIds.size())
    {
        return;
    }
    bindings_.edits.setSendDestination(trackId_, rowIndex, row.destIds[static_cast<size_t>(pick - 1)]);
}

void MixerChannelStrip::commitSendAmount(const int rowIndex)
{
    SendRow& row = sends_[static_cast<size_t>(rowIndex)];
    if (!bindings_.edits.setSendAmount || !bindings_.loadSnapshot)
    {
        return;
    }
    const std::shared_ptr<const SessionSnapshot> snap = bindings_.loadSnapshot();
    const int tix = snap != nullptr ? snap->findTrackIndexById(trackId_) : -1;
    if (tix < 0)
    {
        return;
    }
    const Track& tr = snap->getTrack(tix);
    const int sendIndex = findTrackSendVectorIndexForUiSlot(tr.getSends(), rowIndex);
    if (sendIndex < 0)
    {
        return;
    }
    const float stored = tr.getSend(sendIndex).amountLinear;
    float parsed = stored;
    if (!track_value_text::tryParseSendAmountText(row.amount.getText(), parsed))
    {
        row.amount.setText(track_value_text::formatSendLinearToDbField(stored), juce::dontSendNotification);
        return;
    }
    if (std::fabs(static_cast<double>(parsed - stored)) <= static_cast<double>(track_value_text::kSendAmountDriftEps))
    {
        row.amount.setText(track_value_text::formatSendLinearToDbField(stored), juce::dontSendNotification);
        return;
    }
    bindings_.edits.setSendAmount(trackId_, rowIndex, parsed);
    // Canonical text at once (a refused edit — recording — is re-synced by the poll).
    row.amount.setText(track_value_text::formatSendLinearToDbField(parsed), juce::dontSendNotification);
}

void MixerChannelStrip::setPreGainEditorText(const float db)
{
    preGainEditor_.setText(track_value_text::formatPreGainDbToValueFieldOnly(db), juce::dontSendNotification);
}

void MixerChannelStrip::commitPreGain()
{
    float parsed = 0.0f;
    if (!track_value_text::tryParsePreGainDbText(preGainEditor_.getText(), parsed))
    {
        setPreGainEditorText(lastPreGainDb_);
        return;
    }
    if (bindings_.edits.setPreGainDb)
    {
        bindings_.edits.setPreGainDb(trackId_, parsed);
    }
    // Show the canonical text at once; the refusal case (recording) is re-synced by the poll.
    setPreGainEditorText(parsed);
}

void MixerChannelStrip::textEditorReturnKeyPressed(juce::TextEditor& editor)
{
    if (&editor == &preGainEditor_)
    {
        commitPreGain();
        editor.giveAwayKeyboardFocus();
        return;
    }
    for (int r = 0; r < kSendRows; ++r)
    {
        if (&editor == &sends_[static_cast<size_t>(r)].amount)
        {
            commitSendAmount(r);
            editor.giveAwayKeyboardFocus();
            return;
        }
    }
}

void MixerChannelStrip::textEditorEscapeKeyPressed(juce::TextEditor& editor)
{
    if (&editor == &preGainEditor_)
    {
        setPreGainEditorText(lastPreGainDb_);
    }
    editor.giveAwayKeyboardFocus();
}

void MixerChannelStrip::textEditorFocusLost(juce::TextEditor& editor)
{
    if (&editor == &preGainEditor_)
    {
        commitPreGain();
        return;
    }
    for (int r = 0; r < kSendRows; ++r)
    {
        if (&editor == &sends_[static_cast<size_t>(r)].amount)
        {
            commitSendAmount(r);
            return;
        }
    }
}

// --- meters -------------------------------------------------------------------------------------------

void MixerChannelStrip::collectMeterInterest(std::vector<TrackId>& out)
{
    // Hidden strips (window hidden, kind without audio) cost nothing on the audio thread.
    if (meter_.isVisible() && isShowing() && !isMaster_ && kind_ != TrackKind::Midi)
    {
        out.push_back(trackId_);
    }
}

void MixerChannelStrip::meterWindowArrived(const TrackId trackId, const level_meter::Reading& reading, const double nowSeconds)
{
    if (trackId == trackId_ && meter_.isVisible())
    {
        meter_.pushReading(reading, nowSeconds);
    }
}

void MixerChannelStrip::meterOverloadAcknowledged(const TrackId trackId)
{
    if (trackId == trackId_)
    {
        meter_.resetOverloadLatch(/*notifyOwner*/ false); // came from the hub: never echo back
    }
}

void MixerChannelStrip::meterTick(const double nowSeconds)
{
    if (meter_.isVisible() && isShowing())
    {
        meter_.tick(nowSeconds);
    }
}

// --- test / scenario surfaces ------------------------------------------------------------------------

juce::String MixerChannelStrip::routingCaption(const int row) const
{
    return row >= 0 && row < kRoutingRowsPerStrip ? routing_[static_cast<size_t>(row)].caption.getText() : juce::String();
}

juce::String MixerChannelStrip::routingText(const int row) const
{
    if (row < 0 || row >= kRoutingRowsPerStrip)
    {
        return {};
    }
    const RoutingRow& r = routing_[static_cast<size_t>(row)];
    return r.role == RoutingRole::DeviceOutputInfo ? r.infoLabel.getText() : r.combo.getText();
}

bool MixerChannelStrip::routingRowVisible(const int row) const
{
    return row >= 0 && row < kRoutingRowsPerStrip && routing_[static_cast<size_t>(row)].caption.isVisible();
}

bool MixerChannelStrip::chooseRoutingByText(const int row, const juce::String& itemText)
{
    if (row < 0 || row >= kRoutingRowsPerStrip)
    {
        return false;
    }
    juce::ComboBox& combo = routing_[static_cast<size_t>(row)].combo;
    for (int i = 0; i < combo.getNumItems(); ++i)
    {
        if (combo.getItemText(i) == itemText)
        {
            combo.setSelectedId(combo.getItemId(i), juce::sendNotificationSync);
            return true;
        }
    }
    return false;
}

void MixerChannelStrip::commitPreGainText(const juce::String& text)
{
    preGainEditor_.setText(text, juce::dontSendNotification);
    commitPreGain();
}

int MixerChannelStrip::visibleInsertRowCount(const InsertStage stage) const
{
    const InsertStageUi& ui = stage == InsertStage::Pre ? preInserts_ : postInserts_;
    int n = 0;
    for (const auto& r : ui.rows)
    {
        n += r->isVisible() ? 1 : 0;
    }
    return n;
}

juce::String MixerChannelStrip::insertRowText(const InsertStage stage, const int row) const
{
    const InsertStageUi& ui = stage == InsertStage::Pre ? preInserts_ : postInserts_;
    return row >= 0 && row < kInsertRowsPerStage ? ui.rows[static_cast<size_t>(row)]->getButtonText() : juce::String();
}

bool MixerChannelStrip::sendRowVisible(const int row) const
{
    return row >= 0 && row < kSendRows && sends_[static_cast<size_t>(row)].dest.isVisible();
}

juce::String MixerChannelStrip::sendDestinationText(const int row) const
{
    return row >= 0 && row < kSendRows ? sends_[static_cast<size_t>(row)].dest.getText() : juce::String();
}

juce::String MixerChannelStrip::sendAmountText(const int row) const
{
    return row >= 0 && row < kSendRows ? sends_[static_cast<size_t>(row)].amount.getText() : juce::String();
}

bool MixerChannelStrip::sendEnabled(const int row) const
{
    return row >= 0 && row < kSendRows && sends_[static_cast<size_t>(row)].enable.getToggleState();
}

bool MixerChannelStrip::chooseSendDestinationByText(const int row, const juce::String& itemText)
{
    if (row < 0 || row >= kSendRows)
    {
        return false;
    }
    juce::ComboBox& combo = sends_[static_cast<size_t>(row)].dest;
    for (int i = 0; i < combo.getNumItems(); ++i)
    {
        if (combo.getItemText(i) == itemText)
        {
            combo.setSelectedId(combo.getItemId(i), juce::sendNotificationSync);
            return true;
        }
    }
    return false;
}

void MixerChannelStrip::commitSendAmountText(const int row, const juce::String& text)
{
    if (row < 0 || row >= kSendRows)
    {
        return;
    }
    sends_[static_cast<size_t>(row)].amount.setText(text, juce::dontSendNotification);
    commitSendAmount(row);
}

void MixerChannelStrip::clickBaseButtonForTest(const StripButtonKind kind)
{
    for (CellButton* c : { &instrumentEditorCell_, &powerCell_, &muteCell_, &monitorCell_, &armCell_, &alternativesCell_ })
    {
        if (c->state_.kind == kind && c->isVisible() && c->state_.enabled && c->onClick != nullptr)
        {
            c->onClick();
        }
    }
}

bool MixerChannelStrip::baseButtonVisible(const StripButtonKind kind) const
{
    for (const CellButton* c : { &instrumentEditorCell_, &powerCell_, &muteCell_, &monitorCell_, &armCell_, &alternativesCell_ })
    {
        if (c->state_.kind == kind)
        {
            return c->isVisible();
        }
    }
    return false;
}

bool MixerChannelStrip::baseButtonActive(const StripButtonKind kind) const
{
    for (const CellButton* c : { &instrumentEditorCell_, &powerCell_, &muteCell_, &monitorCell_, &armCell_, &alternativesCell_ })
    {
        if (c->state_.kind == kind)
        {
            return c->state_.active;
        }
    }
    return false;
}

bool MixerChannelStrip::verifyChildrenInsideBands(juce::String& report) const
{
    const juce::Rectangle<int> me = getLocalBounds();
    bool ok = true;
    const auto check = [&](const juce::Component& c, const char* what, const juce::Rectangle<int> band) {
        if (!c.isVisible())
        {
            return;
        }
        const auto b = c.getBounds();
        if (!me.contains(b))
        {
            report << "    " << nameLabel_.getText() << ": " << what << " " << b.toString() << " leaves the strip " << me.toString() << "\n";
            ok = false;
        }
        if (!band.isEmpty() && !band.expanded(2, 2).contains(b))
        {
            report << "    " << nameLabel_.getText() << ": " << what << " " << b.toString() << " leaves its band " << band.toString() << "\n";
            ok = false;
        }
        if (b.getWidth() <= 0 || b.getHeight() <= 0)
        {
            report << "    " << nameLabel_.getText() << ": " << what << " has no size\n";
            ok = false;
        }
    };
    check(nameLabel_, "name", layout_.header);
    check(kindLabel_, "kind", layout_.header);
    for (const CellButton* c : { &instrumentEditorCell_, &powerCell_, &muteCell_, &monitorCell_, &armCell_, &alternativesCell_ })
    {
        check(*c, "base button", layout_.header);
    }
    for (int r = 0; r < kRoutingRowsPerStrip; ++r)
    {
        check(routing_[static_cast<size_t>(r)].caption, "routing caption", layout_.band(Section::Routing));
        check(routing_[static_cast<size_t>(r)].combo, "routing selector", layout_.band(Section::Routing));
        check(routing_[static_cast<size_t>(r)].infoLabel, "routing info", layout_.band(Section::Routing));
    }
    check(preGainEditor_, "pre-gain field", layout_.band(Section::PreGain));
    for (const InsertStageUi* ui : { &preInserts_, &postInserts_ })
    {
        const auto band = layout_.band(ui == &preInserts_ ? Section::PreInserts : Section::PostInserts);
        for (const auto& r : ui->rows)
        {
            check(*r, "insert row", band);
        }
        check(ui->addButton, "insert add", band);
        check(ui->moreLabel, "insert more", band);
    }
    for (int r = 0; r < kSendRows; ++r)
    {
        check(sends_[static_cast<size_t>(r)].enable, "send enable", layout_.band(Section::Sends));
        check(sends_[static_cast<size_t>(r)].dest, "send destination", layout_.band(Section::Sends));
        check(sends_[static_cast<size_t>(r)].amount, "send amount", layout_.band(Section::Sends));
    }
    check(pan_, "pan", layout_.band(Section::Faders));
    check(fader_, "fader", layout_.band(Section::Faders));
    check(meter_, "meter", layout_.band(Section::Meters));
    if (fader_.isVisible() && fader_.getHeight() < 60)
    {
        report << "    " << nameLabel_.getText() << ": fader too short (" << fader_.getHeight() << " px)\n";
        ok = false;
    }
    return ok;
}
