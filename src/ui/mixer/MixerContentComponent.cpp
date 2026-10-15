#include "ui/mixer/MixerContentComponent.h"

#include "domain/SessionSnapshot.h"

#include <algorithm>

namespace
{
    using namespace mixer_layout;

    constexpr mixer_layout::Section kToolbarOrder[] = {
        Section::Headers, Section::Routing, Section::PreGain, Section::PreInserts,
        Section::PostInserts, Section::Sends, Section::Faders, Section::Meters
    };
    static_assert(sizeof(kToolbarOrder) / sizeof(kToolbarOrder[0]) == kSectionCount, "toolbar lists every section");

    constexpr int kToolbarHeightPx = 30;
    constexpr int kToolbarPadPx = 4;
    constexpr int kToggleWidthPx = 84;
    constexpr int kMasterColumnGapPx = 6;
    constexpr int kRefreshHz = 10;
    constexpr int kDividerHitHalfHeightPx = 5;
    constexpr juce::uint32 kContentBgArgb = 0xff1b1d20;
    constexpr juce::uint32 kToolbarBgArgb = 0xff26292d;
    constexpr juce::uint32 kToolbarLineArgb = 0xff3a3d42;
    constexpr juce::uint32 kToggleOnArgb = 0xff2a4a5a;
    constexpr juce::uint32 kToggleOffArgb = 0xff3e3e3e;
    constexpr juce::uint32 kMasterDividerArgb = 0xff4a4f57;
    constexpr juce::uint32 kSectionDividerArgb = 0xff6a6f78;
    constexpr juce::uint32 kSectionDividerHotArgb = 0xff9aa3ad;

    void styleToggle(juce::TextButton& b, const bool on)
    {
        b.setColour(juce::TextButton::buttonColourId, juce::Colour(on ? kToggleOnArgb : kToggleOffArgb));
        b.setColour(juce::TextButton::buttonOnColourId, juce::Colour(kToggleOnArgb));
        b.setColour(juce::TextButton::textColourOffId, juce::Colour(on ? 0xfff2f6f9 : 0xff9aa3ad));
        b.setColour(juce::TextButton::textColourOnId, juce::Colour(0xfff2f6f9));
        b.setToggleState(on, juce::dontSendNotification);
    }
} // namespace

// --- MixerIdentityCell ------------------------------------------------------------------------------
// Pinned under a channel. Number is the arrangement index + 1 (the same number the track header
// draws). A collapsed visual group does not change snapshot order, so it does not renumber.

class MixerIdentityCell final : public juce::Component,
                                public juce::SettableTooltipClient
{
public:
    explicit MixerIdentityCell(const TrackId id) : trackId_(id)
    {
        setWantsKeyboardFocus(false);
        setMouseClickGrabsKeyboardFocus(false);
    }

    [[nodiscard]] TrackId trackId() const noexcept { return trackId_; }
    [[nodiscard]] int number() const noexcept { return number_; }
    [[nodiscard]] juce::String tooltipText() const { return name_; }

    void setIdentity(const int number, const juce::String& name, const bool active)
    {
        const bool changed = number_ != number || name_ != name || active_ != active;
        number_ = number;
        name_ = name;
        active_ = active;
        setTooltip(name);
        if (changed)
        {
            repaint();
        }
    }

    std::function<void(TrackId)> onActivate;

    void paint(juce::Graphics& g) override
    {
        using namespace track_strip_glyphs;
        drawHeaderPlate(g, getLocalBounds(), active_);
        auto row = getLocalBounds().reduced(0, 1);
        // 4 px from the channel edge (the active stripe is the same 4 px, so the digits
        // start just after it). The number is only as wide as its own glyphs.
        constexpr int kLeftMarginPx = 4;
        constexpr int kNumberNameGapPx = 4;
        row.removeFromLeft(kLeftMarginPx);
        g.setColour(headerNameColour());
        const juce::Font numberFont(juce::FontOptions(12.0f, juce::Font::bold));
        g.setFont(numberFont);
        if (number_ > 0 && row.getWidth() > 0)
        {
            const juce::String digits(number_);
            const int numberW = juce::jmin(row.getWidth(), juce::GlyphArrangement::getStringWidthInt(numberFont, digits) + 1);
            g.drawText(digits, row.removeFromLeft(numberW), juce::Justification::centredLeft, false);
            row.removeFromLeft(juce::jmin(kNumberNameGapPx, row.getWidth()));
        }
        g.setFont(juce::FontOptions(12.0f));
        g.drawText(name_, row, juce::Justification::centredLeft, true);
    }

    void mouseDown(const juce::MouseEvent& e) override
    {
        if (getLocalBounds().contains(e.getPosition()))
        {
            userClick();
        }
    }

    void userClick()
    {
        if (onActivate != nullptr)
        {
            onActivate(trackId_);
        }
    }

private:
    TrackId trackId_ = kInvalidTrackId;
    int number_ = 0;
    juce::String name_;
    bool active_ = false;
};

void MixerContentComponent::StripsRow::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colour(kContentBgArgb));
}

// --- DividerOverlay ---------------------------------------------------------------------------------

int MixerContentComponent::DividerOverlay::lineYOf(const Divider& d) const
{
    // Strip y → viewport content y (strips sit at y = 0 inside the row) → overlay y.
    return d.lineY - owner_.stripsViewport_.getViewPositionY() + owner_.stripsViewport_.getY() - getY();
}

int MixerContentComponent::DividerOverlay::dividerAt(const int y) const
{
    const std::vector<Divider> dividers = owner_.currentDividers();
    for (int i = 0; i < static_cast<int>(dividers.size()); ++i)
    {
        if (std::abs(lineYOf(dividers[static_cast<size_t>(i)]) - y) <= kDividerHitHalfHeightPx)
        {
            return i;
        }
    }
    return -1;
}

bool MixerContentComponent::DividerOverlay::hitTest(const int x, const int y)
{
    if (draggingIndex_ >= 0)
    {
        return true; // keep the drag even when the pointer leaves the thin band
    }
    // Never over the viewport's scroll bars.
    const int barW = owner_.stripsViewport_.isVerticalScrollBarShown() ? owner_.stripsViewport_.getScrollBarThickness() : 0;
    const int barH = owner_.stripsViewport_.isHorizontalScrollBarShown() ? owner_.stripsViewport_.getScrollBarThickness() : 0;
    const int stripsRight = owner_.stripsViewport_.getRight() - getX() - barW;
    const int bottom = owner_.stripsViewport_.getBottom() - getY() - barH;
    if (y >= bottom)
    {
        return false;
    }
    if (x > stripsRight && x < owner_.masterViewport_.getX() - getX())
    {
        // The gap in front of Stereo Out is fine; the vertical scroll bar column is not.
        if (x <= stripsRight + barW && barW > 0)
        {
            return false;
        }
    }
    return dividerAt(y) >= 0;
}

void MixerContentComponent::DividerOverlay::paint(juce::Graphics& g)
{
    const std::vector<Divider> dividers = owner_.currentDividers();
    if (dividers.empty())
    {
        return;
    }
    const int barH = owner_.stripsViewport_.isHorizontalScrollBarShown() ? owner_.stripsViewport_.getScrollBarThickness() : 0;
    const int bottom = owner_.stripsViewport_.getBottom() - getY() - barH;
    const int top = owner_.stripsViewport_.getY() - getY();
    const juce::Point<int> mouse = getMouseXYRelative();
    const int hot = draggingIndex_ >= 0 ? draggingIndex_ : (isMouseOver() ? dividerAt(mouse.y) : -1);
    for (int i = 0; i < static_cast<int>(dividers.size()); ++i)
    {
        const int y = lineYOf(dividers[static_cast<size_t>(i)]);
        if (y < top || y >= bottom)
        {
            continue; // scrolled out of the visible strip area
        }
        g.setColour(juce::Colour(i == hot ? kSectionDividerHotArgb : kSectionDividerArgb));
        // One continuous line across the strips, their gaps and the fixed Stereo Out column.
        g.fillRect(0, y, getWidth(), i == hot ? 2 : 1);
    }
}

void MixerContentComponent::DividerOverlay::mouseDown(const juce::MouseEvent& e)
{
    draggingIndex_ = dividerAt(e.getPosition().y);
    if (draggingIndex_ >= 0)
    {
        dragStartY_ = e.getPosition().y;
        owner_.beginDividerDrag(draggingIndex_);
    }
}

void MixerContentComponent::DividerOverlay::mouseDrag(const juce::MouseEvent& e)
{
    if (draggingIndex_ >= 0)
    {
        owner_.continueDividerDrag(draggingIndex_, e.getPosition().y - dragStartY_);
    }
}

void MixerContentComponent::DividerOverlay::mouseUp(const juce::MouseEvent&)
{
    if (draggingIndex_ >= 0)
    {
        draggingIndex_ = -1;
        owner_.endDividerDrag();
    }
}

// --- MixerContentComponent --------------------------------------------------------------------------

MixerContentComponent::MixerContentComponent(const MixerStripBindings& bindings, LevelMeterHub* const hub)
    : bindings_(bindings)
    , meterHub_(hub)
{
    setOpaque(true);

    titleLabel_.setText("Mixer", juce::dontSendNotification);
    titleLabel_.setFont(juce::FontOptions(13.0f, juce::Font::bold));
    titleLabel_.setColour(juce::Label::textColourId, juce::Colour(0xfff2f6f9));
    titleLabel_.setInterceptsMouseClicks(false, false);
    addAndMakeVisible(titleLabel_);

    for (const Section s : kToolbarOrder)
    {
        juce::TextButton& b = sectionToggles_[static_cast<size_t>(s)];
        b.setButtonText(sectionName(s));
        b.setClickingTogglesState(false);
        b.setTooltip(s == Section::Headers
                         ? juce::String("Show / hide the channel header (name, type and Power / Mute / Solo / Monitor / Record)")
                         : juce::String("Show / hide the ") + sectionName(s) + " section on every channel");
        styleToggle(b, visibility_.get(s));
        b.onClick = [this, s] {
            SectionVisibility next = visibility_;
            next.set(s, !next.get(s));
            setSectionVisibility(next);
            if (onSectionVisibilityChanged != nullptr)
            {
                onSectionVisibilityChanged(visibility_);
            }
        };
        addAndMakeVisible(b);
    }

    stripsViewport_.setViewedComponent(&stripsRow_, false);
    stripsViewport_.setScrollBarsShown(true, true);
    stripsViewport_.setScrollBarThickness(12);
    stripsViewport_.onVisibleAreaChanged = [this] {
        syncMasterScroll();
        syncIdentityScroll();
        dividerOverlay_.repaint();
    };
    addAndMakeVisible(stripsViewport_);

    masterViewport_.setViewedComponent(&masterHolder_, false);
    masterViewport_.setScrollBarsShown(false, false);
    masterViewport_.setScrollOnDragMode(juce::Viewport::ScrollOnDragMode::never);
    masterViewport_.onVisibleAreaChanged = [this] {
        // Wheel over the Stereo Out strip scrolls the shared stack too.
        if (!syncingScroll_)
        {
            syncingScroll_ = true;
            stripsViewport_.setViewPosition(stripsViewport_.getViewPositionX(), masterViewport_.getViewPositionY());
            syncingScroll_ = false;
        }
    };
    addAndMakeVisible(masterViewport_);

    identityViewport_.setViewedComponent(&identityRow_, false);
    identityViewport_.setScrollBarsShown(false, false);
    identityViewport_.setScrollOnDragMode(juce::Viewport::ScrollOnDragMode::never);
    addAndMakeVisible(identityViewport_);
    addAndMakeVisible(masterIdentityHolder_);

    // The divider layer sits above both viewports (added last = in front).
    addAndMakeVisible(dividerOverlay_);

    startTimerHz(kRefreshHz);
    refreshFromSession();
}

MixerContentComponent::~MixerContentComponent()
{
    stopTimer();
    // Strips unregister from the hub in their destructors; destroy them before the viewports go.
    strips_.clear();
    masterStrip_.reset();
    identityCells_.clear();
    masterIdentity_.reset();
}

int MixerContentComponent::toolbarHeight() const noexcept
{
    return kToolbarHeightPx;
}

void MixerContentComponent::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colour(kContentBgArgb));
    g.setColour(juce::Colour(kToolbarBgArgb));
    g.fillRect(0, 0, getWidth(), kToolbarHeightPx);
    g.setColour(juce::Colour(kToolbarLineArgb));
    g.fillRect(0, kToolbarHeightPx - 1, getWidth(), 1);
    // Divider in front of the fixed Stereo Out column.
    const int dividerX = masterViewport_.getX() - kMasterColumnGapPx / 2;
    g.setColour(juce::Colour(kMasterDividerArgb));
    g.fillRect(dividerX, kToolbarHeightPx, 2, juce::jmax(0, getHeight() - kToolbarHeightPx - kIdentityRowHeightPx));
}

void MixerContentComponent::resized()
{
    auto area = getLocalBounds();
    auto toolbar = area.removeFromTop(kToolbarHeightPx).reduced(kToolbarPadPx, kToolbarPadPx);
    titleLabel_.setBounds(toolbar.removeFromLeft(56));
    int remaining = kSectionCount;
    for (const Section s : kToolbarOrder)
    {
        const int w = juce::jmin(kToggleWidthPx, juce::jmax(40, toolbar.getWidth() / juce::jmax(1, remaining)));
        sectionToggles_[static_cast<size_t>(s)].setBounds(toolbar.removeFromLeft(w).reduced(2, 0));
        --remaining;
    }
    area.removeFromBottom(kIdentityRowHeightPx);
    dividerOverlay_.setBounds(area);
    // Stereo Out column: fixed at the right; the rest scrolls. Width follows the section flags.
    const int masterW = channelWidthPx(visibility_);
    auto masterArea = area.removeFromRight(masterW);
    area.removeFromRight(kMasterColumnGapPx);
    masterViewport_.setBounds(masterArea);
    stripsViewport_.setBounds(area);
    layoutStrips();
}

int MixerContentComponent::availableStripHeight() const noexcept
{
    const int viewportW = stripsViewport_.getWidth();
    const int viewportH = stripsViewport_.getHeight();
    const int n = static_cast<int>(strips_.size());
    const int stripW = channelWidthPx(visibility_);
    const int rowW = juce::jmax(0, n * (stripW + kStripGapPx));
    const bool horizontalBar = rowW > viewportW;
    const int barH = horizontalBar ? stripsViewport_.getScrollBarThickness() : 0;
    return juce::jmax(0, viewportH - barH);
}

ComputedLayout MixerContentComponent::currentLayout() const noexcept
{
    return computeLayout(visibility_, heights_, lastStripWidth_, lastStripHeight_);
}

void MixerContentComponent::layoutIdentityBand()
{
    const int stripW = channelWidthPx(visibility_);
    const int y = getHeight() - kIdentityRowHeightPx;
    const int barW = stripsViewport_.isVerticalScrollBarShown() ? stripsViewport_.getScrollBarThickness() : 0;
    identityViewport_.setBounds(stripsViewport_.getX(), y, juce::jmax(0, stripsViewport_.getWidth() - barW), kIdentityRowHeightPx);
    masterIdentityHolder_.setBounds(masterViewport_.getX(), y, stripW, kIdentityRowHeightPx);
}

void MixerContentComponent::syncIdentityScroll()
{
    if (syncingScroll_)
    {
        return;
    }
    syncingScroll_ = true;
    identityViewport_.setViewPosition(stripsViewport_.getViewPositionX(), 0);
    syncingScroll_ = false;
}

MixerIdentityCell* MixerContentComponent::identityFor(const TrackId trackId) const noexcept
{
    for (const auto& c : identityCells_)
    {
        if (c != nullptr && c->trackId() == trackId)
        {
            return c.get();
        }
    }
    if (masterIdentity_ != nullptr && masterIdentity_->trackId() == trackId)
    {
        return masterIdentity_.get();
    }
    return nullptr;
}

void MixerContentComponent::layoutStrips()
{
    const int stripW = channelWidthPx(visibility_);
    const int oldStride = lastStripWidth_ + kStripGapPx;
    const int leftIndex = oldStride > 0 ? stripsViewport_.getViewPositionX() / oldStride : 0;
    const int rowW = juce::jmax(0, static_cast<int>(strips_.size()) * (stripW + kStripGapPx));
    const int availableH = availableStripHeight();
    const int minH = minimumStripHeight(visibility_, heights_);
    const int stripH = juce::jmax(minH, availableH);
    const ComputedLayout layout = computeLayout(visibility_, heights_, stripW, stripH);
    lastStripHeight_ = stripH;
    lastStripWidth_ = stripW;

    stripsRow_.setSize(juce::jmax(rowW, 1), stripH);
    identityRow_.setSize(juce::jmax(rowW, 1), kIdentityRowHeightPx);
    int x = 0;
    for (size_t i = 0; i < strips_.size(); ++i)
    {
        strips_[i]->setBounds(x, 0, stripW, stripH);
        strips_[i]->applyLayout(layout);
        if (i < identityCells_.size() && identityCells_[i] != nullptr)
        {
            identityCells_[i]->setBounds(x, 0, stripW, kIdentityRowHeightPx);
        }
        x += stripW + kStripGapPx;
    }
    masterHolder_.setSize(stripW, stripH);
    if (masterStrip_ != nullptr)
    {
        masterStrip_->setBounds(0, 0, stripW, stripH);
        masterStrip_->applyLayout(layout);
    }
    if (masterIdentity_ != nullptr)
    {
        masterIdentity_->setBounds(0, 0, stripW, kIdentityRowHeightPx);
    }
    layoutIdentityBand();
    const int newStride = stripW + kStripGapPx;
    stripsViewport_.setViewPosition(leftIndex * newStride, stripsViewport_.getViewPositionY());
    syncMasterScroll();
    syncIdentityScroll();
    dividerOverlay_.repaint();
}

void MixerContentComponent::syncMasterScroll()
{
    if (syncingScroll_)
    {
        return;
    }
    syncingScroll_ = true;
    masterViewport_.setViewPosition(0, stripsViewport_.getViewPositionY());
    syncingScroll_ = false;
}

void MixerContentComponent::setSectionVisibility(const SectionVisibility& v)
{
    const bool changed = v != visibility_;
    visibility_ = v;
    for (int i = 0; i < kSectionCount; ++i)
    {
        styleToggle(sectionToggles_[static_cast<size_t>(i)], visibility_.get(static_cast<Section>(i)));
    }
    if (!changed)
    {
        return;
    }
    // The master column and the scrolling viewport share the derived width, so a toggle
    // relayouts the whole content, not only the strips inside the old viewport.
    resized();
    refreshFromSession();
    if (meterHub_ != nullptr)
    {
        meterHub_->refreshInterestNow();
    }
    repaint();
}

void MixerContentComponent::setSectionHeights(const SectionHeights& h)
{
    SectionHeights clamped;
    for (int i = 0; i < kSectionCount; ++i)
    {
        const Section s = static_cast<Section>(i);
        clamped.set(s, isUpperSection(s) ? h.get(s) : 0);
    }
    if (clamped == heights_)
    {
        return;
    }
    heights_ = clamped;
    layoutStrips();
}

std::vector<Divider> MixerContentComponent::currentDividers() const
{
    return dividersFor(currentLayout(), visibility_);
}

void MixerContentComponent::beginDividerDrag(const int dividerIndex)
{
    juce::ignoreUnused(dividerIndex);
    // The drag is applied against the heights and dividers of the moment the button went down,
    // so every mouse move is "start + total delta" (no accumulation drift).
    heightsAtDragStart_ = heights_;
    dividersAtDragStart_ = currentDividers();
}

void MixerContentComponent::continueDividerDrag(const int dividerIndex, const int deltaY)
{
    if (dividerIndex < 0 || dividerIndex >= static_cast<int>(dividersAtDragStart_.size()))
    {
        return;
    }
    const SectionHeights next = applyDividerDrag(heightsAtDragStart_, visibility_, dividersAtDragStart_[static_cast<size_t>(dividerIndex)],
                                                 deltaY, availableStripHeight());
    if (next != heights_)
    {
        heights_ = next;
        layoutStrips();
    }
}

void MixerContentComponent::endDividerDrag()
{
    // One report per gesture (the owner writes ui-layout.xml here, never per mouse move).
    if (onSectionHeightsChanged != nullptr && heights_ != heightsAtDragStart_)
    {
        onSectionHeightsChanged(heights_);
    }
}

bool MixerContentComponent::dragDividerForTest(const int dividerIndex, const int deltaY)
{
    if (dividerIndex < 0 || dividerIndex >= static_cast<int>(currentDividers().size()))
    {
        return false;
    }
    beginDividerDrag(dividerIndex);
    continueDividerDrag(dividerIndex, deltaY);
    endDividerDrag();
    return true;
}

void MixerContentComponent::clickSectionToggleForTest(const Section s)
{
    // `Button::triggerClick` is asynchronous (posted message); run the same handler now.
    juce::TextButton& b = sectionToggles_[static_cast<size_t>(static_cast<int>(s))];
    if (b.onClick != nullptr)
    {
        b.onClick();
    }
}

void MixerContentComponent::timerCallback()
{
    if (!isShowing())
    {
        return; // hidden window: no repaints, no snapshot walks
    }
    refreshFromSession();
}

void MixerContentComponent::syncStripSet(const SessionSnapshot& snap)
{
    // Target order: every non-Master row in arrangement order; the Master row is the fixed strip.
    std::vector<TrackId> wanted;
    const TrackId master = snap.findCanonicalMasterTrackId();
    for (int i = 0; i < snap.getNumTracks(); ++i)
    {
        const Track& t = snap.getTrack(i);
        if (t.getKind() == TrackKind::Master)
        {
            continue;
        }
        wanted.push_back(t.getId());
    }
    bool changed = false;
    // Remove strips whose rows left.
    for (auto it = strips_.begin(); it != strips_.end();)
    {
        if (std::find(wanted.begin(), wanted.end(), (*it)->trackId()) == wanted.end())
        {
            it = strips_.erase(it);
            changed = true;
        }
        else
        {
            ++it;
        }
    }
    // Add missing strips, then put everything in arrangement order (stable reorder).
    for (const TrackId id : wanted)
    {
        if (stripForTrack(id) == nullptr)
        {
            auto s = std::make_unique<MixerChannelStrip>(id, bindings_, meterHub_);
            stripsRow_.addAndMakeVisible(*s);
            strips_.push_back(std::move(s));
            changed = true;
        }
    }
    std::vector<TrackId> currentOrder;
    currentOrder.reserve(strips_.size());
    for (const auto& s : strips_)
    {
        currentOrder.push_back(s->trackId());
    }
    if (currentOrder != wanted)
    {
        std::vector<std::unique_ptr<MixerChannelStrip>> ordered;
        ordered.reserve(wanted.size());
        for (const TrackId id : wanted)
        {
            for (auto& s : strips_)
            {
                if (s != nullptr && s->trackId() == id)
                {
                    ordered.push_back(std::move(s));
                    break;
                }
            }
        }
        strips_ = std::move(ordered);
        changed = true;
    }

    {
        std::vector<std::unique_ptr<MixerIdentityCell>> orderedIds;
        orderedIds.reserve(strips_.size());
        for (const auto& s : strips_)
        {
            std::unique_ptr<MixerIdentityCell> cell;
            for (auto& c : identityCells_)
            {
                if (c != nullptr && c->trackId() == s->trackId())
                {
                    cell = std::move(c);
                    break;
                }
            }
            if (cell == nullptr)
            {
                cell = std::make_unique<MixerIdentityCell>(s->trackId());
                cell->onActivate = [this](const TrackId id) {
                    if (bindings_.activateTrack)
                    {
                        bindings_.activateTrack(id);
                    }
                };
                identityRow_.addAndMakeVisible(*cell);
                changed = true;
            }
            orderedIds.push_back(std::move(cell));
        }
        if (identityCells_.size() != orderedIds.size())
        {
            changed = true;
        }
        identityCells_ = std::move(orderedIds);
    }

    if (master != masterTrackId_ || (master != kInvalidTrackId && masterStrip_ == nullptr))
    {
        masterTrackId_ = master;
        masterStrip_.reset();
        if (master != kInvalidTrackId)
        {
            masterStrip_ = std::make_unique<MixerChannelStrip>(master, bindings_, meterHub_);
            masterHolder_.addAndMakeVisible(*masterStrip_);
            masterIdentity_ = std::make_unique<MixerIdentityCell>(master);
            masterIdentity_->onActivate = [this](const TrackId id) {
                if (bindings_.activateTrack)
                {
                    bindings_.activateTrack(id);
                }
            };
            masterIdentityHolder_.addAndMakeVisible(*masterIdentity_);
        }
        else
        {
            masterIdentity_.reset();
        }
        changed = true;
    }
    if (changed)
    {
        layoutStrips();
        if (meterHub_ != nullptr)
        {
            meterHub_->refreshInterestNow();
        }
    }
}

void MixerContentComponent::refreshFromSession()
{
    const std::shared_ptr<const SessionSnapshot> snap = bindings_.loadSnapshot ? bindings_.loadSnapshot() : nullptr;
    if (snap == nullptr)
    {
        strips_.clear();
        masterStrip_.reset();
        identityCells_.clear();
        masterIdentity_.reset();
        masterTrackId_ = kInvalidTrackId;
        layoutStrips();
        return;
    }
    syncStripSet(*snap);
    const TrackId active = bindings_.activeTrackId ? bindings_.activeTrackId() : kInvalidTrackId;
    const auto publishIdentity = [&](MixerIdentityCell* cell, const TrackId id) {
        if (cell == nullptr)
        {
            return;
        }
        const int idx = snap->findTrackIndexById(id);
        if (idx < 0)
        {
            return;
        }
        const Track& tr = snap->getTrack(idx);
        const juce::String name = tr.getKind() == TrackKind::Master ? juce::String(kMasterTrackDisplayName) : tr.getName();
        cell->setIdentity(idx + 1, name, id == active);
    };
    for (auto& s : strips_)
    {
        s->refreshFromSession(*snap, s->trackId() == active);
        publishIdentity(identityFor(s->trackId()), s->trackId());
    }
    if (masterStrip_ != nullptr)
    {
        masterStrip_->refreshFromSession(*snap, masterStrip_->trackId() == active);
        publishIdentity(masterIdentity_.get(), masterStrip_->trackId());
    }
}

MixerChannelStrip* MixerContentComponent::stripForTrack(const TrackId trackId) const noexcept
{
    for (const auto& s : strips_)
    {
        if (s != nullptr && s->trackId() == trackId)
        {
            return s.get();
        }
    }
    if (masterStrip_ != nullptr && masterStrip_->trackId() == trackId)
    {
        return masterStrip_.get();
    }
    return nullptr;
}

std::vector<TrackId> MixerContentComponent::stripOrder() const
{
    std::vector<TrackId> out;
    for (const auto& s : strips_)
    {
        out.push_back(s->trackId());
    }
    return out;
}

juce::Rectangle<int> MixerContentComponent::masterStripScreenBounds() const
{
    return masterStrip_ != nullptr ? masterStrip_->getScreenBounds() : juce::Rectangle<int>();
}

bool MixerContentComponent::verifyLayout(juce::String& report, juce::String& failReason) const
{
    const auto fail = [&failReason](const juce::String& why) {
        if (failReason.isEmpty())
        {
            failReason = why;
        }
        return false;
    };
    report << "  mixer content " << getLocalBounds().toString() << " toolbar h=" << kToolbarHeightPx
           << " strips viewport " << stripsViewport_.getBounds().toString() << " master viewport "
           << masterViewport_.getBounds().toString() << "\n";
    report << "  strip height " << lastStripHeight_ << " (minimum " << minimumStripHeight(visibility_, heights_) << "), "
           << static_cast<int>(strips_.size()) << " strips + master; heights";
    for (int i = 0; i < kSectionCount; ++i)
    {
        const Section s = static_cast<Section>(i);
        if (isUpperSection(s))
        {
            report << " " << sectionKey(s) << "=" << heights_.get(s);
        }
    }
    report << "\n";
    for (int i = 0; i < kSectionCount; ++i)
    {
        report << "  section " << sectionName(static_cast<Section>(i)) << "=" << (visibility_.get(static_cast<Section>(i)) ? "shown" : "hidden");
        if (!sectionToggles_[static_cast<size_t>(i)].getBounds().isEmpty() && sectionToggles_[static_cast<size_t>(i)].getRight() > getWidth())
        {
            return fail("section toggle leaves the toolbar");
        }
    }
    report << "\n";
    const std::vector<Divider> dividers = currentDividers();
    report << "  dividers:";
    for (const Divider& d : dividers)
    {
        report << " " << sectionKey(d.above) << "|" << (d.below == Section::Count ? "lower" : sectionKey(d.below)) << "@" << d.lineY;
    }
    report << "\n";
    bool ok = true;
    if (masterViewport_.getRight() > getWidth() || masterViewport_.getX() < stripsViewport_.getRight())
    {
        ok = fail("master column overlaps the strips viewport");
    }
    if (masterStrip_ == nullptr)
    {
        ok = fail("no Stereo Out strip");
    }
    // Every strip shares the same bands: compare each strip's header / lower band with the master's.
    const juce::Rectangle<int> refHeader = masterStrip_ != nullptr ? masterStrip_->headerBounds() : juce::Rectangle<int>();
    const juce::Rectangle<int> refLower = masterStrip_ != nullptr ? masterStrip_->lowerBandBounds() : juce::Rectangle<int>();
    for (const auto& s : strips_)
    {
        if (s->headerBounds() != refHeader || s->lowerBandBounds() != refLower)
        {
            report << "    " << s->nameText() << ": bands differ from Stereo Out (header " << s->headerBounds().toString()
                   << " vs " << refHeader.toString() << ", lower " << s->lowerBandBounds().toString() << " vs " << refLower.toString() << ")\n";
            ok = fail("strip bands are not aligned");
        }
        if (s->getHeight() != lastStripHeight_ || s->getWidth() != channelWidthPx(visibility_))
        {
            ok = fail("strip size differs");
        }
        if (!s->verifyChildrenInsideBands(report))
        {
            ok = fail("a control leaves its band in " + s->nameText());
        }
    }
    if (masterStrip_ != nullptr && !masterStrip_->verifyChildrenInsideBands(report))
    {
        ok = fail("a control leaves its band in Stereo Out");
    }
    // The fixed strip must be fully visible regardless of the horizontal scroll position.
    if (masterStrip_ != nullptr && masterViewport_.getWidth() < channelWidthPx(visibility_))
    {
        ok = fail("Stereo Out column narrower than a strip");
    }
    // Dividers: one per gap between consecutive visible bands, each inside the strip, none in a gap
    // that no longer exists (a hidden section leaves no empty band).
    const ComputedLayout layout = currentLayout();
    int visibleUpper = 0;
    for (int i = 0; i < kSectionCount; ++i)
    {
        const Section s = static_cast<Section>(i);
        visibleUpper += (isUpperSection(s) && visibility_.get(s)) ? 1 : 0;
    }
    const int expectedDividers = juce::jmax(0, visibleUpper - 1) + ((visibleUpper > 0 && !layout.lowerBand.isEmpty()) ? 1 : 0);
    if (static_cast<int>(dividers.size()) != expectedDividers)
    {
        ok = fail("divider count " + juce::String((int)dividers.size()) + " != expected " + juce::String(expectedDividers));
    }
    for (const Divider& d : dividers)
    {
        if (d.lineY <= 0 || d.lineY >= lastStripHeight_)
        {
            ok = fail("a divider lies outside the strip");
        }
    }
    return ok;
}

int MixerContentComponent::identityNumberForTest(const TrackId trackId) const noexcept
{
    const MixerIdentityCell* const cell = identityFor(trackId);
    return cell != nullptr ? cell->number() : 0;
}

juce::String MixerContentComponent::identityTooltipForTest(const TrackId trackId) const
{
    const MixerIdentityCell* const cell = identityFor(trackId);
    return cell != nullptr ? cell->tooltipText() : juce::String();
}

int MixerContentComponent::identityCellWidthForTest(const TrackId trackId) const noexcept
{
    const MixerIdentityCell* const cell = identityFor(trackId);
    return cell != nullptr ? cell->getWidth() : 0;
}

bool MixerContentComponent::clickIdentityForTest(const TrackId trackId)
{
    MixerIdentityCell* const cell = identityFor(trackId);
    if (cell == nullptr)
    {
        return false;
    }
    cell->userClick();
    return true;
}

bool MixerContentComponent::headerControlsVisibleForTest(const TrackId trackId) const noexcept
{
    const MixerChannelStrip* const strip = stripForTrack(trackId);
    return strip != nullptr && strip->headerChromeVisible();
}
