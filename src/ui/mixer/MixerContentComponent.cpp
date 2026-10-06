#include "ui/mixer/MixerContentComponent.h"

#include "domain/SessionSnapshot.h"

#include <algorithm>

namespace
{
    using namespace mixer_layout;

    constexpr int kToolbarHeightPx = 30;
    constexpr int kToolbarPadPx = 4;
    constexpr int kToggleWidthPx = 84;
    constexpr int kMasterColumnGapPx = 6;
    constexpr int kRefreshHz = 10;
    constexpr juce::uint32 kContentBgArgb = 0xff1b1d20;
    constexpr juce::uint32 kToolbarBgArgb = 0xff26292d;
    constexpr juce::uint32 kToolbarLineArgb = 0xff3a3d42;
    constexpr juce::uint32 kToggleOnArgb = 0xff2a4a5a;
    constexpr juce::uint32 kToggleOffArgb = 0xff3e3e3e;
    constexpr juce::uint32 kMasterDividerArgb = 0xff4a4f57;

    void styleToggle(juce::TextButton& b, const bool on)
    {
        b.setColour(juce::TextButton::buttonColourId, juce::Colour(on ? kToggleOnArgb : kToggleOffArgb));
        b.setColour(juce::TextButton::buttonOnColourId, juce::Colour(kToggleOnArgb));
        b.setColour(juce::TextButton::textColourOffId, juce::Colour(on ? 0xfff2f6f9 : 0xff9aa3ad));
        b.setColour(juce::TextButton::textColourOnId, juce::Colour(0xfff2f6f9));
        b.setToggleState(on, juce::dontSendNotification);
    }
} // namespace

void MixerContentComponent::StripsRow::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colour(kContentBgArgb));
}

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

    for (int i = 0; i < kSectionCount; ++i)
    {
        const Section s = static_cast<Section>(i);
        juce::TextButton& b = sectionToggles_[static_cast<size_t>(i)];
        b.setButtonText(sectionName(s));
        b.setClickingTogglesState(false);
        b.setTooltip(juce::String("Show / hide the ") + sectionName(s) + " section on every channel");
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
    stripsViewport_.onVisibleAreaChanged = [this] { syncMasterScroll(); };
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

    startTimerHz(kRefreshHz);
    refreshFromSession();
}

MixerContentComponent::~MixerContentComponent()
{
    stopTimer();
    // Strips unregister from the hub in their destructors; destroy them before the viewports go.
    strips_.clear();
    masterStrip_.reset();
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
    g.fillRect(dividerX, kToolbarHeightPx, 2, getHeight() - kToolbarHeightPx);
}

void MixerContentComponent::resized()
{
    auto area = getLocalBounds();
    auto toolbar = area.removeFromTop(kToolbarHeightPx).reduced(kToolbarPadPx, kToolbarPadPx);
    titleLabel_.setBounds(toolbar.removeFromLeft(56));
    for (int i = 0; i < kSectionCount; ++i)
    {
        const int w = juce::jmin(kToggleWidthPx, juce::jmax(40, toolbar.getWidth() / (kSectionCount - i)));
        sectionToggles_[static_cast<size_t>(i)].setBounds(toolbar.removeFromLeft(w).reduced(2, 0));
    }
    // Stereo Out column: fixed at the right; the rest scrolls.
    const int masterW = kStripWidthPx;
    auto masterArea = area.removeFromRight(masterW);
    area.removeFromRight(kMasterColumnGapPx);
    masterViewport_.setBounds(masterArea);
    stripsViewport_.setBounds(area);
    layoutStrips();
}

void MixerContentComponent::layoutStrips()
{
    const int viewportW = stripsViewport_.getWidth();
    const int viewportH = stripsViewport_.getHeight();
    // Height available to a strip = viewport height minus the horizontal scrollbar it will show
    // when the strips do not fit horizontally.
    const int n = static_cast<int>(strips_.size());
    const int rowW = juce::jmax(0, n * (kStripWidthPx + kStripGapPx));
    const bool horizontalBar = rowW > viewportW;
    const int barH = horizontalBar ? stripsViewport_.getScrollBarThickness() : 0;
    const int availableH = juce::jmax(0, viewportH - barH);
    const int minH = minimumStripHeight(visibility_);
    const int stripH = juce::jmax(minH, availableH);
    const bool verticalBar = stripH > availableH;
    const int barW = verticalBar ? stripsViewport_.getScrollBarThickness() : 0;
    juce::ignoreUnused(barW);

    const ComputedLayout layout = computeLayout(visibility_, kStripWidthPx, stripH);
    lastStripHeight_ = stripH;

    stripsRow_.setSize(juce::jmax(rowW, 1), stripH);
    int x = 0;
    for (auto& s : strips_)
    {
        s->setBounds(x, 0, kStripWidthPx, stripH);
        s->applyLayout(layout);
        x += kStripWidthPx + kStripGapPx;
    }
    masterHolder_.setSize(kStripWidthPx, stripH);
    if (masterStrip_ != nullptr)
    {
        masterStrip_->setBounds(0, 0, kStripWidthPx, stripH);
        masterStrip_->applyLayout(layout);
    }
    syncMasterScroll();
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
    if (v == visibility_ && sectionToggles_[0].getToggleState() == v.get(Section::Routing))
    {
        return;
    }
    visibility_ = v;
    for (int i = 0; i < kSectionCount; ++i)
    {
        styleToggle(sectionToggles_[static_cast<size_t>(i)], visibility_.get(static_cast<Section>(i)));
    }
    layoutStrips();
    if (meterHub_ != nullptr)
    {
        meterHub_->refreshInterestNow();
    }
    repaint();
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
    TrackId master = snap.findCanonicalMasterTrackId();
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

    if (master != masterTrackId_ || (master != kInvalidTrackId && masterStrip_ == nullptr))
    {
        masterTrackId_ = master;
        masterStrip_.reset();
        if (master != kInvalidTrackId)
        {
            masterStrip_ = std::make_unique<MixerChannelStrip>(master, bindings_, meterHub_);
            masterHolder_.addAndMakeVisible(*masterStrip_);
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
        masterTrackId_ = kInvalidTrackId;
        layoutStrips();
        return;
    }
    syncStripSet(*snap);
    const TrackId active = bindings_.activeTrackId ? bindings_.activeTrackId() : kInvalidTrackId;
    for (auto& s : strips_)
    {
        s->refreshFromSession(*snap, s->trackId() == active);
    }
    if (masterStrip_ != nullptr)
    {
        masterStrip_->refreshFromSession(*snap, masterStrip_->trackId() == active);
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
    report << "  strip height " << lastStripHeight_ << " (minimum " << minimumStripHeight(visibility_) << "), "
           << static_cast<int>(strips_.size()) << " strips + master\n";
    for (int i = 0; i < kSectionCount; ++i)
    {
        report << "  section " << sectionName(static_cast<Section>(i)) << "=" << (visibility_.get(static_cast<Section>(i)) ? "shown" : "hidden");
        if (!sectionToggles_[static_cast<size_t>(i)].getBounds().isEmpty() && sectionToggles_[static_cast<size_t>(i)].getRight() > getWidth())
        {
            return fail("section toggle leaves the toolbar");
        }
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
        if (s->getHeight() != lastStripHeight_ || s->getWidth() != kStripWidthPx)
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
    if (masterStrip_ != nullptr && masterViewport_.getWidth() < kStripWidthPx)
    {
        ok = fail("Stereo Out column narrower than a strip");
    }
    return ok;
}
