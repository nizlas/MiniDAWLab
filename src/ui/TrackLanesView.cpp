// =============================================================================
// TrackLanesView.cpp  —  one lane view per `Track` (message thread)
// =============================================================================

#include "ui/TrackLanesView.h"

#include "audio/LatencySettingsStore.h"
#include "ui/ClipWaveformView.h"
#include "ui/TimelineLocatorPainter.h"
#include "ui/TimelineRulerView.h"
#include "ui/TimelineViewportModel.h"
#include "ui/TrackHeaderView.h"
#include "ui/TrackColourPalette.h"
#include "domain/Session.h"
#include "domain/SessionSnapshot.h"
#include "domain/Track.h"
#include "instruments/InstrumentTrackController.h"
#include "domain/PlacedClip.h"
#include "transport/Transport.h"
#include "diagnostics/UiPaintLoadCounters.h"
#include "diagnostics/UndoDiagnosticConfig.h"
#include "diagnostics/UndoDiagnosticFileLog.h"

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_core/juce_core.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <optional>
#include <unordered_set>
#include <utility>
#include <vector>

namespace
{
    constexpr double kSppMin = 0.1;
    // Match `ClipWaveformView` playhead refresh so preview drain + repaints stay in the same ballpark.
    constexpr int kRecordingPreviewTimerHz = 20;

    // Shared arrange-lane chrome behind waveform / MIDI lanes (`TrackLanesView::paint`).
    constexpr unsigned int kArrangementLaneBackgroundArgb = 0xff252528u;

    /// Horizontal separator in the track-header column only (lane segment keeps subtle grey below).
    constexpr unsigned int kArrangementHeaderRowSeparatorArgb = 0xff181a1du;

    // Single separator family (vertical drawn in `paintOverChildren` so lane children do not cover it).
    constexpr unsigned int kArrangementSeparatorArgb = 0xff4a4a52u;
    constexpr float kArrangementSeparatorAlphaVertical = 0.62f;
    constexpr float kArrangementSeparatorAlphaHorizontal = 0.58f;

    // Move up to `maxSamples` front-most source samples from `from` into `out` (may split a block).
    void peelPeakBlocksBySampleCount(std::vector<RecordingPreviewPeakBlock>& from,
                                     std::vector<RecordingPreviewPeakBlock>& out,
                                     const std::int64_t maxSamples)
    {
        out.clear();
        if (maxSamples <= 0)
        {
            return;
        }
        std::int64_t taken = 0;
        while (taken < maxSamples && !from.empty())
        {
            RecordingPreviewPeakBlock blk = from.front();
            if (blk.numSourceSamples <= 0)
            {
                from.erase(from.begin());
                continue;
            }
            const std::int64_t need = maxSamples - taken;
            const std::int64_t n = static_cast<std::int64_t>(blk.numSourceSamples);
            if (n <= need)
            {
                out.push_back(blk);
                taken += n;
                from.erase(from.begin());
            }
            else
            {
                RecordingPreviewPeakBlock head = blk;
                head.numSourceSamples = static_cast<int>(need);
                out.push_back(head);
                from.front().numSourceSamples -= static_cast<int>(need);
                taken += need;
            }
        }
    }

    [[nodiscard]] std::int64_t totalPeakBlockSamples(const std::vector<RecordingPreviewPeakBlock>& v)
    {
        std::int64_t s = 0;
        for (const auto& b : v)
        {
            s += static_cast<std::int64_t>(juce::jmax(0, b.numSourceSamples));
        }
        return s;
    }

    void peakBlocksAppendPrefixCopy(const std::vector<RecordingPreviewPeakBlock>& src,
                                    std::vector<RecordingPreviewPeakBlock>& dst,
                                    const std::int64_t prefixSamples)
    {
        dst.clear();
        if (prefixSamples <= 0)
        {
            return;
        }
        std::int64_t taken = 0;
        for (const RecordingPreviewPeakBlock& blk : src)
        {
            if (blk.numSourceSamples <= 0)
            {
                continue;
            }
            const std::int64_t ns = static_cast<std::int64_t>(blk.numSourceSamples);
            const std::int64_t need = prefixSamples - taken;
            if (need <= 0)
            {
                break;
            }
            if (ns <= need)
            {
                dst.push_back(blk);
                taken += ns;
            }
            else
            {
                RecordingPreviewPeakBlock head = blk;
                head.numSourceSamples = static_cast<int>(need);
                dst.push_back(head);
                taken += need;
                break;
            }
        }
    }

    void discardPeakSamplesFromFront(std::vector<RecordingPreviewPeakBlock>& v,
                                     std::int64_t nSamples)
    {
        if (nSamples <= 0)
        {
            return;
        }
        while (nSamples > 0 && !v.empty())
        {
            RecordingPreviewPeakBlock& fb = v.front();
            if (fb.numSourceSamples <= 0)
            {
                v.erase(v.begin());
                continue;
            }
            const auto ns = static_cast<std::int64_t>(fb.numSourceSamples);
            if (nSamples >= ns)
            {
                nSamples -= ns;
                v.erase(v.begin());
                continue;
            }
            fb.numSourceSamples -= static_cast<int>(nSamples);
            nSamples = 0;
            break;
        }
    }

    void peakBlocksApplyPlacementCompensation(std::vector<RecordingPreviewPeakBlock> peaksWork,
                                              const std::int64_t rawSegmentTimelineStart,
                                              const std::int64_t placementOffsetSamples,
                                              std::int64_t& outVisibleStartSample,
                                              std::vector<RecordingPreviewPeakBlock>& outPeaks)
    {
        const std::int64_t wanted = rawSegmentTimelineStart + placementOffsetSamples;
        const std::int64_t trimAtProjectOrigin = wanted < std::int64_t{ 0 } ? -wanted : std::int64_t{ 0 };
        outVisibleStartSample = wanted < std::int64_t{ 0 } ? std::int64_t{ 0 } : wanted;
        discardPeakSamplesFromFront(peaksWork, trimAtProjectOrigin);
        outPeaks = std::move(peaksWork);
    }

    [[nodiscard]] double effectiveDisplaySampleRate(juce::AudioDeviceManager& dm) noexcept
    {
        if (juce::AudioIODevice* d = dm.getCurrentAudioDevice())
        {
            const double r = d->getCurrentSampleRate();
            if (r > 0.0 && std::isfinite(r))
            {
                return r;
            }
        }
        return 48000.0;
    }

    /// Mini-strip clip-interval fill: the grey-blue family of the MIDI note previews / clip
    /// chrome, dim enough to read as an overview, bright enough against the 0xff252528 lane bg.
    constexpr unsigned int kCollapsedStripClipFillArgb = 0xff8e98a8u;
    /// Collapsed run's header-column plate behind the marker (slightly lighter than the lane bg).
    constexpr unsigned int kCollapsedRunHeaderPlateArgb = 0xff2b2d31u;
    /// The shared group-marker colour (same as the header's left-edge member marker).
    constexpr unsigned int kVisualGroupMarkerArgb = 0xff6f8096u;
    /// Marker x inside the header column: right of the 4 px active stripe, left of all content.
    constexpr int kVisualGroupMarkerXPx = TrackHeaderView::kHeaderActiveStripeWidthPx + 1;
    constexpr int kVisualGroupMarkerWidthPx = 2;
    /// Movement past this cancels a pending handle long-press AND the short-click toggle.
    constexpr int kVisualGroupHandleDragCancelPx = 4;

} // namespace

/// The group handle tab: the "inverted golf club" head where the vertical member marker turns
/// into a short tab extending right over the header area at the group's top boundary. Owns the
/// three handle gestures (short click = toggle collapse, long press = inline rename, right-click
/// = menu) and swallows its mouse events so the row underneath — the previous track's name strip
/// or resize band — is never activated through it (spec §3).
class TrackLanesView::VisualGroupHandleView final : public juce::Component,
                                                    public juce::TooltipClient,
                                                    private juce::Timer,
                                                    private juce::TextEditor::Listener
{
public:
    VisualGroupHandleView(TrackLanesView& owner, const int groupId) noexcept
        : owner_(owner), groupId_(groupId)
    {
        setWantsKeyboardFocus(false);
    }

    [[nodiscard]] int getGroupId() const noexcept { return groupId_; }

    /// The full group name — needed when the compact tab (Micro neighbours) truncates it.
    juce::String getTooltip() override { return name_; }

    void setDisplayState(const juce::String& name, const bool collapsed)
    {
        if (name_ == name && collapsed_ == collapsed)
        {
            return;
        }
        name_ = name;
        collapsed_ = collapsed;
        repaint();
    }

    void paint(juce::Graphics& g) override
    {
        const auto b = getLocalBounds();
        if (b.isEmpty())
        {
            return;
        }
        g.setColour(juce::Colour(0xf0343a46));
        g.fillRoundedRectangle(b.toFloat(), 3.0f);
        // The member marker's continuation at the tab's left edge (the club's shaft meets the head).
        g.setColour(juce::Colour(kVisualGroupMarkerArgb));
        g.fillRect(0, 0, kVisualGroupMarkerWidthPx, b.getHeight());
        g.drawRoundedRectangle(b.toFloat().reduced(0.5f), 3.0f, 1.0f);
        // Collapse glyph: a small triangle — down-pointing expanded, right-pointing collapsed.
        const float gy = (float) (b.getHeight() - 8) * 0.5f;
        juce::Path p;
        if (collapsed_)
        {
            p.addTriangle(5.0f, gy, 5.0f, gy + 8.0f, 11.0f, gy + 4.0f);
        }
        else
        {
            p.addTriangle(4.0f, gy + 1.0f, 12.0f, gy + 1.0f, 8.0f, gy + 7.0f);
        }
        g.setColour(juce::Colour(0xffd8dee8));
        g.fillPath(p);
        if (renameEditor_ == nullptr || !renameEditor_->isVisible())
        {
            // Compact tab (12 px, Micro neighbours): a smaller font; the name ellipsizes and the
            // full text is the tooltip.
            g.setFont(juce::Font(juce::FontOptions(b.getHeight() < 14 ? 9.5f : 11.0f)));
            g.drawText(name_,
                       b.withTrimmedLeft(15).withTrimmedRight(3),
                       juce::Justification::centredLeft,
                       true);
        }
    }

    void resized() override
    {
        if (renameEditor_ != nullptr && renameEditor_->isVisible())
        {
            renameEditor_->setBounds(renameEditorBounds());
        }
    }

    void mouseDown(const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu())
        {
            stopTimer();
            pressActive_ = false;
            owner_.showVisualGroupHandleContextMenu(groupId_);
            return;
        }
        if (!e.mods.isLeftButtonDown() || (renameEditor_ != nullptr && renameEditor_->isVisible()))
        {
            return;
        }
        longPressFired_ = false;
        pressMoved_ = false;
        pressActive_ = true;
        startTimer(kVisualGroupHandleLongPressMs);
    }

    void mouseDrag(const juce::MouseEvent& e) override
    {
        if (pressActive_ && !pressMoved_
            && e.getDistanceFromDragStart() > kVisualGroupHandleDragCancelPx)
        {
            // Clear mouse movement cancels BOTH the pending long-press rename and the short-click
            // toggle: a drag off the handle is neither gesture (spec §3).
            pressMoved_ = true;
            stopTimer();
        }
    }

    void mouseUp(const juce::MouseEvent&) override
    {
        stopTimer();
        const bool plainShortClick = pressActive_ && !longPressFired_ && !pressMoved_;
        pressActive_ = false;
        if (plainShortClick)
        {
            // Short click toggles exactly once; a long press (rename began) must NOT also toggle
            // at mouse-up (spec §3).
            owner_.toggleVisualGroupCollapsedFromHandle(groupId_);
        }
    }

    /// Inline rename (long press / context menu / test): Enter commits, Escape cancels. The
    /// editor takes keyboard focus, so app shortcuts cannot swallow the typed text.
    bool beginInlineRename()
    {
        ensureRenameEditor();
        renameEditor_->setText(name_, juce::dontSendNotification);
        renameEditor_->setBounds(renameEditorBounds());
        renameEditor_->setVisible(true);
        renameEditor_->toFront(true);
        renameEditor_->selectAll();
        renameEditor_->grabKeyboardFocus();
        repaint();
        return true;
    }

    [[nodiscard]] bool isRenameEditorOpen() const noexcept
    {
        return renameEditor_ != nullptr && renameEditor_->isVisible();
    }

    /// [Test] The exact commit path of the inline editor with `newName` as its content.
    bool commitRenameWithTextForTest(const juce::String& newName)
    {
        ensureRenameEditor();
        renameEditor_->setText(newName, juce::dontSendNotification);
        renameEditor_->setVisible(true);
        submitRename();
        return true;
    }

private:
    void timerCallback() override
    {
        stopTimer();
        if (!pressActive_ || pressMoved_)
        {
            return;
        }
        // Central long-press threshold reached without clear movement: rename, not toggle.
        longPressFired_ = true;
        beginInlineRename();
    }

    [[nodiscard]] juce::Rectangle<int> renameEditorBounds() const noexcept
    {
        return getLocalBounds().withTrimmedLeft(14).reduced(1);
    }

    void ensureRenameEditor()
    {
        if (renameEditor_ != nullptr)
        {
            return;
        }
        renameEditor_ = std::make_unique<juce::TextEditor>();
        renameEditor_->setFont(juce::Font(juce::FontOptions(11.0f)));
        renameEditor_->setBorder(juce::BorderSize<int>(1));
        renameEditor_->setSelectAllWhenFocused(true);
        renameEditor_->addListener(this);
        addChildComponent(*renameEditor_);
    }

    void closeRenameEditor() noexcept
    {
        if (renameEditor_ != nullptr)
        {
            renameEditor_->setVisible(false);
        }
        repaint();
    }

    void submitRename()
    {
        if (renameEditor_ == nullptr)
        {
            return;
        }
        const juce::String newName = renameEditor_->getText().trim();
        closeRenameEditor();
        if (newName.isNotEmpty() && newName != name_
            && owner_.visualGroupUiHooks_.renameGroup != nullptr)
        {
            owner_.visualGroupUiHooks_.renameGroup(groupId_, newName);
        }
    }

    void textEditorReturnKeyPressed(juce::TextEditor&) override { submitRename(); }
    void textEditorEscapeKeyPressed(juce::TextEditor&) override { closeRenameEditor(); }
    void textEditorFocusLost(juce::TextEditor&) override { closeRenameEditor(); }

    TrackLanesView& owner_;
    const int groupId_;
    juce::String name_;
    bool collapsed_ = false;
    bool pressActive_ = false;
    bool pressMoved_ = false;
    bool longPressFired_ = false;
    std::unique_ptr<juce::TextEditor> renameEditor_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(VisualGroupHandleView)
};

TrackLanesView::TrackLanesView(
    Session& session,
    Transport& transport,
    TimelineViewportModel& timelineViewport,
    juce::AudioDeviceManager& deviceManager,
    RecorderService& recorder,
    LatencySettingsStore& latencySettingsStore,
    AudioWaveformCache& waveformCache)
    : session_(session)
    , transport_(transport)
    , timelineViewport_(timelineViewport)
    , deviceManager_(deviceManager)
    , recorder_(recorder)
    , latencyStore_(latencySettingsStore)
    , waveformCache_(waveformCache)
{
    setOpaque(true);
    // NOT a decorative optimization — do not remove. The transparent `PlayheadOverlay` (sibling
    // above this view) invalidates a narrow full-height stripe ~60×/s during playback, and without
    // buffering every component under that stripe re-runs `paint`: that steady playback repaint tax
    // was the confirmed root cause of the main-window playback-dependent zoom freeze (Follow OFF,
    // stable viewport; see MAIN_WINDOW_ZOOM_UI_FREEZE_FORENSIC_AUDIT.md §20). Buffering caches the
    // rendered subtree (grid + audio lanes + instrument row + headers) so stripes become image
    // blits. A child's own `repaint()` still invalidates the matching buffer region on its way to
    // the peer, so all existing content-change paths keep working — but lane visual changes must
    // now invalidate explicitly; they can no longer rely on overlay stripes exposing stale content.
    setBufferedToImage(true);
    // Middle-drag hand-pan across the whole lanes band, including over child lanes/headers.
    addMouseListener(&middlePanListener_, true);
    addAndMakeVisible(headerColumnResizeHandle_);
    syncTracksFromSession();
    startTimerHz(kRecordingPreviewTimerHz);
}

// ---------------------------------------------------------------------------------------------
// Header column width (shared boundary) + drag handle
// ---------------------------------------------------------------------------------------------
int TrackLanesView::effectiveTrackHeaderColumnWidthPxForTotalWidth(const int totalWidthPx) const noexcept
{
    return clampHeaderColumnWidthForTotalWidth(trackHeaderColumnWidthPx_, totalWidthPx);
}

void TrackLanesView::setTrackHeaderColumnWidthPx(const int widthPx, const bool notifyOwner) noexcept
{
    const int clamped = juce::jlimit(kTrackHeaderColumnMinWidthPx, kTrackHeaderColumnMaxWidthPx, widthPx);
    if (clamped == trackHeaderColumnWidthPx_)
    {
        return;
    }
    trackHeaderColumnWidthPx_ = clamped;
    // Own layout first (headers, lanes, MIDI lanes, handle), then the owner moves the ruler corner,
    // the add-track button and the playhead overlay to the same boundary.
    resized();
    repaint();
    if (notifyOwner && onTrackHeaderColumnWidthChanged_ != nullptr)
    {
        onTrackHeaderColumnWidthChanged_(trackHeaderColumnWidthPx_, false);
    }
}

void TrackLanesView::setOnTrackHeaderColumnWidthChanged(std::function<void(int, bool)> fn) noexcept
{
    onTrackHeaderColumnWidthChanged_ = std::move(fn);
}

void TrackLanesView::notifyTrackHeaderColumnWidthDragEnded() noexcept
{
    if (onTrackHeaderColumnWidthChanged_ != nullptr)
    {
        onTrackHeaderColumnWidthChanged_(trackHeaderColumnWidthPx_, true);
    }
}

juce::Rectangle<int> TrackLanesView::getHeaderColumnResizeHandleBoundsForDiagnostics() const noexcept
{
    return headerColumnResizeHandle_.getBounds();
}

void TrackLanesView::simulateHeaderColumnHandleDragForStabilityTest(const int deltaPx) noexcept
{
    const int anchor = headerColumnWidthPx();
    setTrackHeaderColumnWidthPx(anchor + deltaPx);
    notifyTrackHeaderColumnWidthDragEnded();
}

bool TrackLanesView::verifyHeaderColumnLayoutForDiagnostics(juce::String& report, juce::String& failReason) const
{
    const int headerW = headerColumnWidthPx();
    const int boundaryX = getLocalBounds().getX() + juce::jmin(headerW, getWidth());
    report << "header column: preference=" << trackHeaderColumnWidthPx_ << " effective=" << headerW
           << " view width=" << getWidth() << " boundaryX=" << boundaryX
           << " handle=" << headerColumnResizeHandle_.getBounds().toString() << "\n";

    const auto checkHeader = [&](const juce::String& kind, const TrackHeaderView& h, const juce::Component* lane) -> bool {
        if (!h.isVisible() || h.getBounds().isEmpty())
        {
            return true;
        }
        const juce::Rectangle<int> hb = h.getBounds();
        const juce::Rectangle<int> local = h.getLocalBounds();
        struct Cell { const char* name; juce::Rectangle<int> r; };
        const Cell cells[] = {
            { "instrument", h.getInstrumentEditorButtonBounds() },
            { "power", h.getPowerButtonBounds() },
            { "mute", h.getMuteButtonBounds() },
            { "monitor", h.getMonitorButtonBounds() },
            { "arm", h.getArmButtonBounds() },
            { "alternatives", h.getAlternativesButtonBounds() },
        };
        juce::String cellText;
        int rightMost = 0;
        for (const Cell& c : cells)
        {
            if (c.r.isEmpty())
            {
                continue;
            }
            cellText << " " << c.name << "=" << c.r.toString();
            rightMost = juce::jmax(rightMost, c.r.getRight());
            if (!local.contains(c.r))
            {
                failReason = kind + " header \"" + h.getName() + "\" (track " + juce::String((juce::int64)h.getTrackId())
                             + "): " + c.name + " cell " + c.r.toString() + " is not fully inside the header "
                             + local.toString() + " at column width " + juce::String(headerW);
                return false;
            }
        }
        report << "  " << kind << " track " << juce::String((juce::int64)h.getTrackId()) << " header=" << hb.toString()
               << " rightmostCellEdge=" << rightMost << " margin=" << (hb.getWidth() - rightMost) << cellText;
        if (lane != nullptr && lane->isVisible())
        {
            report << " lane=" << lane->getBounds().toString();
        }
        report << "\n";
        if (hb.getX() != getLocalBounds().getX() || hb.getWidth() != juce::jmin(headerW, getWidth()))
        {
            failReason = kind + " header (track " + juce::String((juce::int64)h.getTrackId()) + ") spans "
                         + hb.toString() + " but the shared column is " + juce::String(headerW) + " px";
            return false;
        }
        if (lane != nullptr && lane->isVisible() && lane->getX() != boundaryX)
        {
            failReason = kind + " lane (track " + juce::String((juce::int64)h.getTrackId()) + ") starts at x="
                         + juce::String(lane->getX()) + " but the boundary is x=" + juce::String(boundaryX);
            return false;
        }
        return true;
    };

    for (size_t i = 0; i < headers_.size(); ++i)
    {
        const TrackHeaderView* h = headers_[i].get();
        const juce::Component* lane = i < lanes_.size() ? lanes_[i].get() : nullptr;
        if (h != nullptr && !checkHeader("audio", *h, lane))
        {
            return false;
        }
    }
    for (const auto& [tid, att] : instrumentTimelineAttachments_)
    {
        juce::ignoreUnused(tid);
        if (att.header != nullptr && !checkHeader("instrument", *att.header, att.midiLane))
        {
            return false;
        }
    }
    for (const auto& [tid, gh] : groupHeaders_)
    {
        juce::ignoreUnused(tid);
        if (gh != nullptr && !checkHeader("group", *gh, nullptr))
        {
            return false;
        }
    }
    for (const auto& mh : masterHeaders_)
    {
        if (mh != nullptr && !checkHeader("master", *mh, nullptr))
        {
            return false;
        }
    }
    return true;
}

const TrackHeaderView* TrackLanesView::findInstrumentRowHeaderForDiagnostics(const TrackId tid) const noexcept
{
    const auto it = instrumentTimelineAttachments_.find(tid);
    return it != instrumentTimelineAttachments_.end() ? it->second.header : nullptr;
}

bool TrackLanesView::verifyVerticalScrollLayoutForDiagnostics(juce::String& report, juce::String& failReason) const
{
    const VerticalScrollModel m = verticalScrollModel();
    constexpr int gutter = kArrangementTimelineHeaderGutterPx;
    const juce::Rectangle<int> viewport = getLocalBounds().withTrimmedTop(gutter);
    report << "vertical scroll: content=" << m.contentHeightPx << " viewport=" << m.viewportHeightPx
           << " offset=" << m.offsetPx << " max=" << m.maxOffsetPx() << " rows=" << (int) visibleTrackEntries_.size()
           << " fits=" << (m.everythingFits() ? 1 : 0) << "\n";
    if (m.offsetPx < 0 || m.offsetPx > m.maxOffsetPx())
    {
        failReason = "offset " + juce::String(m.offsetPx) + " outside 0.." + juce::String(m.maxOffsetPx());
        return false;
    }
    // Every row sits at `gutter - offset + sum(previous heights)`; header and lane of the SAME row
    // share y and height (clipped to the viewport the same way), and rows are contiguous.
    int expectedY = viewport.getY() - m.offsetPx;
    for (int vi = 0; vi < (int) visibleTrackEntries_.size(); ++vi)
    {
        const VisibleTrackEntry& e = visibleTrackEntries_[(size_t) vi];
        const int rowH = juce::jmax(1, rowHeightForVisibleEntry(vi));
        const juce::Rectangle<int> row(getLocalBounds().getX(), expectedY, getWidth(), rowH);
        const juce::Rectangle<int> visibleRow = row.getIntersection(viewport);
        const juce::Component* header = nullptr;
        const juce::Component* lane = nullptr;
        const char* kind = "audio";
        if (e.kind == VisibleTrackKind::Instrument)
        {
            kind = "instrument";
            const auto it = instrumentTimelineAttachments_.find(e.sessionTrackId);
            if (it != instrumentTimelineAttachments_.end())
            {
                header = it->second.header;
                lane = it->second.midiLane;
            }
        }
        else if (e.kind == VisibleTrackKind::Master)
        {
            kind = "master";
            header = masterHeaders_.empty() ? nullptr : masterHeaders_[0].get();
        }
        else if (e.kind == VisibleTrackKind::Group)
        {
            kind = "group";
            const auto it = groupHeaders_.find(e.sessionTrackId);
            header = it != groupHeaders_.end() ? it->second.get() : nullptr;
        }
        else
        {
            const int si = audioLaneIndexFromTrackId(e.sessionTrackId);
            if (si >= 0 && si < (int) headers_.size() && si < (int) lanes_.size())
            {
                header = headers_[(size_t) si].get();
                lane = lanes_[(size_t) si].get();
            }
        }
        const juce::Rectangle<int> hb = header != nullptr ? header->getBounds() : juce::Rectangle<int>();
        const juce::Rectangle<int> lb = lane != nullptr ? lane->getBounds() : juce::Rectangle<int>();
        report << "  row " << vi << " " << kind << " track " << juce::String((juce::int64) e.sessionTrackId)
               << " h=" << rowH << " expectedY=" << expectedY << " visible=" << visibleRow.toString()
               << " header=" << hb.toString() << (lane != nullptr ? " lane=" + lb.toString() : juce::String()) << "\n";
        if (header != nullptr && header->isVisible())
        {
            // A member of a COLLAPSED visual group keeps its 4 px display row but deliberately
            // gets EMPTY header/lane bounds — no hidden buttons, hit areas or tooltips under the
            // mini strip (groups spec §4). Expected-empty like a scrolled-out row.
            if (isTrackInCollapsedVisualGroup(e.sessionTrackId))
            {
                if (!hb.isEmpty() || !lb.isEmpty())
                {
                    failReason = juce::String(kind) + " row of track " + juce::String((juce::int64) e.sessionTrackId)
                                 + " is in a collapsed group but still has laid-out components (header="
                                 + hb.toString() + " lane=" + lb.toString() + ")";
                    return false;
                }
                expectedY += rowH;
                continue;
            }
            if (visibleRow.isEmpty())
            {
                if (!hb.isEmpty())
                {
                    failReason = juce::String(kind) + " header of track " + juce::String((juce::int64) e.sessionTrackId)
                                 + " is laid out (" + hb.toString() + ") although its row is scrolled out of view";
                    return false;
                }
            }
            else if (hb.getY() != visibleRow.getY() || hb.getHeight() != visibleRow.getHeight())
            {
                failReason = juce::String(kind) + " header of track " + juce::String((juce::int64) e.sessionTrackId)
                             + " spans y=" + juce::String(hb.getY()) + " h=" + juce::String(hb.getHeight())
                             + " but the row is y=" + juce::String(visibleRow.getY()) + " h=" + juce::String(visibleRow.getHeight());
                return false;
            }
            if (lane != nullptr && lane->isVisible() && !visibleRow.isEmpty()
                && (lb.getY() != hb.getY() || lb.getHeight() != hb.getHeight()))
            {
                failReason = juce::String(kind) + " lane of track " + juce::String((juce::int64) e.sessionTrackId)
                             + " (" + lb.toString() + ") is not aligned with its header (" + hb.toString() + ")";
                return false;
            }
        }
        expectedY += rowH;
    }
    if (expectedY - (viewport.getY() - m.offsetPx) != m.contentHeightPx)
    {
        failReason = "row heights sum to " + juce::String(expectedY - (viewport.getY() - m.offsetPx))
                     + " but the model says content=" + juce::String(m.contentHeightPx);
        return false;
    }
    return true;
}

int TrackLanesView::visibleRowIndexForTrackForDiagnostics(const TrackId tid) const noexcept
{
    for (int i = 0; i < (int) visibleTrackEntries_.size(); ++i)
    {
        if (visibleTrackEntries_[(size_t) i].sessionTrackId == tid)
        {
            return i;
        }
    }
    return -1;
}

int TrackLanesView::rowTopOffsetPxForTrackForDiagnostics(const TrackId tid) const noexcept
{
    int y = 0;
    for (int i = 0; i < (int) visibleTrackEntries_.size(); ++i)
    {
        if (visibleTrackEntries_[(size_t) i].sessionTrackId == tid)
        {
            return y;
        }
        y += rowHeightForVisibleEntry(i);
    }
    return -1;
}

bool TrackLanesView::openHeaderContextMenuForStabilityTest(const TrackId tid)
{
    TrackHeaderView* header = nullptr;
    if (const auto it = instrumentTimelineAttachments_.find(tid); it != instrumentTimelineAttachments_.end())
    {
        header = it->second.header;
    }
    if (header == nullptr)
    {
        if (const auto it = groupHeaders_.find(tid); it != groupHeaders_.end())
        {
            header = it->second.get();
        }
    }
    if (header == nullptr)
    {
        const int si = audioLaneIndexFromTrackId(tid);
        if (si >= 0 && si < (int) headers_.size())
        {
            header = headers_[(size_t) si].get();
        }
    }
    if (header == nullptr || !header->isVisible() || header->getBounds().isEmpty())
    {
        return false;
    }
    return header->showContextMenuLikeRightClickForStabilityTest();
}

TrackLanesView::HeaderColumnResizeHandle::HeaderColumnResizeHandle(TrackLanesView& owner) noexcept
    : owner_(owner)
{
    setMouseCursor(juce::MouseCursor::LeftRightResizeCursor);
    setRepaintsOnMouseActivity(true);
    // Opaque to clicks so nothing underneath (header resize band, lane clips) can start a gesture
    // from the same press; the middle-pan listener still sees these events via the parent.
    setInterceptsMouseClicks(true, false);
}

void TrackLanesView::HeaderColumnResizeHandle::mouseDown(const juce::MouseEvent& e)
{
    if (!e.mods.isLeftButtonDown())
    {
        return;
    }
    dragging_ = true;
    anchorWidthPx_ = owner_.headerColumnWidthPx();
}

void TrackLanesView::HeaderColumnResizeHandle::mouseDrag(const juce::MouseEvent& e)
{
    if (!dragging_)
    {
        return;
    }
    owner_.setTrackHeaderColumnWidthPx(anchorWidthPx_ + e.getDistanceFromDragStartX());
}

void TrackLanesView::HeaderColumnResizeHandle::mouseUp(const juce::MouseEvent&)
{
    if (!dragging_)
    {
        return;
    }
    dragging_ = false;
    owner_.notifyTrackHeaderColumnWidthDragEnded();
}

void TrackLanesView::HeaderColumnResizeHandle::paint(juce::Graphics& g)
{
    if (!isMouseOverOrDragging())
    {
        return;
    }
    const auto b = getLocalBounds();
    g.setColour(juce::Colours::white.withAlpha(dragging_ ? 0.45f : 0.22f));
    g.fillRect(b.getCentreX() - 1, b.getY(), 2, b.getHeight());
}

TrackLanesView::~TrackLanesView()
{
    stopTimer();
    clearCycleRecordingPreviewContext();
    // `TrackLanesView`'s JUCE `Component` has no `removeFromParent()`; keep non-owned shell children
    // alive for `TransportControlsContent`. `removeChildComponent` does not delete the child.
    // ORDER DEPENDENCY: the attachment pointers below reference components owned by
    // `InstrumentTimelineRowCoordinator`. This is only safe because `TransportControlsContent`
    // declares `trackLanesView` *after* `instrumentTimelineRowCoordinator_` (members are destroyed
    // in reverse declaration order, so this view dies first while those components are still alive).
    auto detachFromParentIfAny = [](juce::Component* c) noexcept {
        if (c == nullptr)
        {
            return;
        }
        if (auto* const p = c->getParentComponent())
        {
            p->removeChildComponent(c);
        }
    };
    for (auto& kv : instrumentTimelineAttachments_)
    {
        detachFromParentIfAny(kv.second.header);
        detachFromParentIfAny(kv.second.midiLane);
    }
    instrumentTimelineAttachments_.clear();
}

void TrackLanesView::setTrackHeaderPluginHost(TrackHeaderPluginHost host) noexcept
{
    trackHeaderPluginHost_ = std::move(host);
    for (const auto& h : headers_)
    {
        cancelHeaderDragIfSourceIs(h.get());
    }
    headers_.clear();
    lanes_.clear();
    aggregatedSelectedPlacedClip_.reset();
    syncTracksFromSession();
}

void TrackLanesView::timerCallback()
{
    updateRecordingPreviewOverlaysFromRecorder();
}

void TrackLanesView::updateRecordingPreviewOverlaysFromRecorder()
{
    if (!recorder_.isRecording())
    {
        recordingPreviewPeaksAccum_.clear();
        cycleRecordingCompletedPassPeaks_.clear();
        cyclePreviewActive_ = false;
        for (auto& u : lanes_)
        {
            if (u != nullptr)
            {
                u->clearRecordingPreviewOverlay();
            }
        }
        return;
    }

    RecordingPreviewPeakBlock blk;
    while (recorder_.drainNextPreviewBlock(blk))
    {
        recordingPreviewPeaksAccum_.push_back(blk);
    }

    const TrackId recTid = recorder_.getRecordingTrackId();
    const std::int64_t placementOff = latencyStore_.getCurrentRecordingOffsetSamples();

    std::int64_t recStart = recorder_.getRecordingStartSample();
    std::int64_t recLen = recorder_.getRecordedSampleCount();

    const std::uint32_t wrapNow = transport_.readCycleWrapCountForUi();

    // Cycle preview mapping (raw anchors S, L, R). Placement offset mirrors commit math in Main.cpp:
    // wantedPreviewStart = rawSegmentTimelineStart + placementOff; clamp visible start >= 0;
    // discard that many preview source samples before drawing (timeline underflow trim).
    const std::int64_t passLen = cyclePreviewLocR_ - cyclePreviewLocL_;
    const bool cycleRangeUsable = cyclePreviewActive_
                                  && passLen > 0
                                  && cyclePreviewActualStart_ < cyclePreviewLocR_;
    std::int64_t firstSegLen = 0;
    std::vector<std::vector<RecordingPreviewPeakBlock>> compensatedCompletedBehind;

    if (cycleRangeUsable)
    {
        firstSegLen = cyclePreviewLocR_ - cyclePreviewActualStart_;

        while (cyclePreviewLastSeenWrap_ < wrapNow)
        {
            const std::uint32_t alreadyConsumed = cyclePreviewLastSeenWrap_ - cyclePreviewWrapBaseline_;
            const std::int64_t peelLen = (alreadyConsumed == 0u) ? firstSegLen : passLen;
            std::vector<RecordingPreviewPeakBlock> onePass;
            peelPeakBlocksBySampleCount(recordingPreviewPeaksAccum_, onePass, peelLen);
            cycleRecordingCompletedPassPeaks_.push_back(std::move(onePass));
            ++cyclePreviewLastSeenWrap_;
        }

        compensatedCompletedBehind.reserve(cycleRecordingCompletedPassPeaks_.size());
        for (size_t pi = 0; pi < cycleRecordingCompletedPassPeaks_.size(); ++pi)
        {
            const std::int64_t rawAnch
                = (pi == 0) ? cyclePreviewActualStart_ : cyclePreviewLocL_;
            std::vector<RecordingPreviewPeakBlock> work = cycleRecordingCompletedPassPeaks_[pi];
            std::int64_t segVisStartUnused = 0;
            std::vector<RecordingPreviewPeakBlock> comp;
            peakBlocksApplyPlacementCompensation(
                std::move(work), rawAnch, placementOff, segVisStartUnused, comp);
            compensatedCompletedBehind.push_back(std::move(comp));
        }

        const std::uint32_t wraps = (wrapNow >= cyclePreviewWrapBaseline_)
                                    ? (wrapNow - cyclePreviewWrapBaseline_)
                                    : 0u;
        if (wraps == 0u)
        {
            recStart = cyclePreviewActualStart_;
            recLen = juce::jlimit<std::int64_t>(std::int64_t{ 0 }, firstSegLen, recLen);
        }
        else
        {
            const std::int64_t sourceOffset
                = firstSegLen + static_cast<std::int64_t>(wraps - 1u) * passLen;
            const std::int64_t offsetInPass = recLen - sourceOffset;
            recStart = cyclePreviewLocL_;
            recLen = juce::jlimit<std::int64_t>(std::int64_t{ 0 }, passLen, offsetInPass);
        }
    }

    for (auto& u : lanes_)
    {
        if (u == nullptr)
        {
            continue;
        }
        if (u->getTrackId() == recTid)
        {
            if (cycleRangeUsable)
            {
                const std::int64_t wrappedWanted = cyclePreviewLocL_ + placementOff;
                const std::int64_t compensatedLoopAnchorL = (wrappedWanted < 0) ? std::int64_t{ 0 }
                                                                               : wrappedWanted;
                const std::int64_t wrappedPassVisibleLen
                    = (wrappedWanted < 0)
                          ? juce::jmax<std::int64_t>(std::int64_t{ 0 }, passLen + wrappedWanted)
                          : passLen;

                std::int64_t firstSegmentVisLen = 0;
                std::int64_t firstSegmentTimelineStart = 0;
                if (!compensatedCompletedBehind.empty())
                {
                    const std::int64_t seg0wanted
                        = cyclePreviewActualStart_ + placementOff;
                    firstSegmentTimelineStart
                        = (seg0wanted < 0) ? std::int64_t{ 0 } : seg0wanted;
                    firstSegmentVisLen = totalPeakBlockSamples(compensatedCompletedBehind.front());
                }

                std::vector<RecordingPreviewPeakBlock> currentPrefix;
                peakBlocksAppendPrefixCopy(
                    recordingPreviewPeaksAccum_, currentPrefix, recLen);
                std::int64_t currentVisStart = 0;
                std::vector<RecordingPreviewPeakBlock> currentCompensatedPeaks;
                peakBlocksApplyPlacementCompensation(
                    std::move(currentPrefix),
                    recStart,
                    placementOff,
                    currentVisStart,
                    currentCompensatedPeaks);
                const std::int64_t currentVisLen
                    = totalPeakBlockSamples(currentCompensatedPeaks);

                u->setRecordingCyclePassPreviewLayers(
                    compensatedCompletedBehind,
                    firstSegmentTimelineStart,
                    firstSegmentVisLen,
                    compensatedLoopAnchorL,
                    wrappedPassVisibleLen,
                    currentVisStart,
                    currentVisLen,
                    currentCompensatedPeaks);
            }
            else
            {
                std::vector<RecordingPreviewPeakBlock> previewPrefix;
                peakBlocksAppendPrefixCopy(recordingPreviewPeaksAccum_, previewPrefix, recLen);
                std::int64_t visStartSample = 0;
                std::vector<RecordingPreviewPeakBlock> compPeaks;
                peakBlocksApplyPlacementCompensation(
                    std::move(previewPrefix),
                    recStart,
                    placementOff,
                    visStartSample,
                    compPeaks);
                const std::int64_t visLen = totalPeakBlockSamples(compPeaks);
                u->setRecordingPreviewOverlay(visStartSample, visLen, compPeaks);
            }
        }
        else
        {
            u->clearRecordingPreviewOverlay();
        }
    }
}

void TrackLanesView::setCycleRecordingPreviewContext(
    const bool active,
    const std::int64_t loopLeftSample,
    const std::int64_t loopRightSample,
    const std::int64_t actualRecordingStart,
    const std::uint32_t wrapPassCountBaselineAtRecordingStart) noexcept
{
    cyclePreviewActive_ = active;
    cyclePreviewLocL_ = loopLeftSample;
    cyclePreviewLocR_ = loopRightSample;
    cyclePreviewActualStart_ = actualRecordingStart;
    cyclePreviewWrapBaseline_ = wrapPassCountBaselineAtRecordingStart;
    cyclePreviewLastSeenWrap_ = wrapPassCountBaselineAtRecordingStart;
    recordingPreviewPeaksAccum_.clear();
    cycleRecordingCompletedPassPeaks_.clear();
}

void TrackLanesView::clearCycleRecordingPreviewContext() noexcept
{
    cyclePreviewActive_ = false;
    recordingPreviewPeaksAccum_.clear();
    cycleRecordingCompletedPassPeaks_.clear();
}

void TrackLanesView::syncTracksFromSession()
{
    rebuildChildLanesIfNeeded();
    rebuildMasterHeadersIfNeeded();
    rebuildGroupHeadersIfNeeded();
    resized();
}

void TrackLanesView::setOnDeleteTrackRequested(
    std::function<void(TrackId)> onDeleteTrackRequested) noexcept
{
    onDeleteTrackRequested_ = std::move(onDeleteTrackRequested);
}

void TrackLanesView::requestDeleteTrackForHeaderMenu(const TrackId tid) noexcept
{
    if (onDeleteTrackRequested_ != nullptr)
    {
        onDeleteTrackRequested_(tid);
    }
}

void TrackLanesView::setOnDuplicateTrackRequested(
    std::function<void(TrackId)> onDuplicateTrackRequested) noexcept
{
    onDuplicateTrackRequested_ = std::move(onDuplicateTrackRequested);
}

void TrackLanesView::requestDuplicateTrackForHeaderMenu(const TrackId tid) noexcept
{
    if (tid == kInvalidTrackId || isStructuralTimelineEditBlocked())
    {
        return;
    }
    if (onDuplicateTrackRequested_ != nullptr)
    {
        onDuplicateTrackRequested_(tid);
    }
}

juce::PopupMenu::Item TrackLanesView::makeDuplicateTrackMenuItem(const int itemId, const bool editLocked)
{
    juce::PopupMenu::Item item;
    item.itemID = itemId;
    item.text = editLocked ? juce::String("Duplicate Track (stop playback first)")
                           : juce::String("Duplicate Track");
    item.isEnabled = !editLocked;
    return item;
}

void TrackLanesView::setOnUndoableClipMoveRequested(
    std::function<bool(PlacedClipId, std::int64_t, std::optional<TrackId>)> fn) noexcept
{
    onUndoableClipMoveRequested_ = std::move(fn);
}

void TrackLanesView::setOnUndoableClipTrimRequested(
    std::function<bool(PlacedClipId, ClipTrimEdge, std::int64_t)> fn) noexcept
{
    onUndoableClipTrimRequested_ = std::move(fn);
}

void TrackLanesView::setOnUndoableClipRenameRequested(
    std::function<bool(PlacedClipId, juce::String)> fn) noexcept
{
    onUndoableClipRenameRequested_ = std::move(fn);
}

void TrackLanesView::setActiveEditToolProvider(std::function<EditTool()> fn) noexcept
{
    activeEditToolProvider_ = std::move(fn);
}

void TrackLanesView::setOnUndoableClipSplitRequested(
    std::function<void(PlacedClipId, std::int64_t, bool)> fn) noexcept
{
    onUndoableClipSplitRequested_ = std::move(fn);
}

void TrackLanesView::setOnUndoableRenameTrackRequested(
    std::function<bool(TrackId, juce::String)> fn) noexcept
{
    onUndoableRenameTrackRequested_ = std::move(fn);
}

bool TrackLanesView::invokeUndoableRenameTrackRequested(const TrackId tid,
                                                        const juce::String proposedName) noexcept
{
    if (onUndoableRenameTrackRequested_ == nullptr)
    {
        return false;
    }
    return onUndoableRenameTrackRequested_(tid, proposedName);
}

void TrackLanesView::setArrangementTimelineSnapFunction(std::function<std::int64_t(std::int64_t)> fn) noexcept
{
    arrangementTimelineSnap_ = std::move(fn);
}

void TrackLanesView::setHeaderActiveSuppressProvider(std::function<bool()> fn) noexcept
{
    headerActiveSuppressProvider_ = std::move(fn);
    for (auto& h : headers_)
    {
        if (h != nullptr)
        {
            h->repaint();
        }
    }
}

void TrackLanesView::setOnAudioHeaderActivated(std::function<void()> fn) noexcept
{
    onAudioHeaderActivated_ = std::move(fn);
}

void TrackLanesView::setInputMonitoringHooks(std::function<bool(TrackId)> isMonitored,
                                             std::function<void(TrackId)> toggleMonitor) noexcept
{
    isTrackInputMonitoredFn_ = std::move(isMonitored);
    toggleTrackInputMonitorFn_ = std::move(toggleMonitor);
}

void TrackLanesView::setSoloUiHooks(SoloUiHooks hooks) noexcept
{
    soloUiHooks_ = std::move(hooks);
    for (auto& h : headers_)
    {
        h->repaint();
    }
    for (auto& h : masterHeaders_)
    {
        h->repaint();
    }
    for (auto& [tid, h] : groupHeaders_)
    {
        juce::ignoreUnused(tid);
        h->repaint();
    }
}

void TrackLanesView::setOnAudioClipMouseDownClearForeignSelections(std::function<void()> fn) noexcept
{
    onAudioClipMouseDownClearForeignSelections_ = std::move(fn);
}

void TrackLanesView::setOnAudioTrackImportClipAtPlayhead(std::function<void(TrackId)> fn) noexcept
{
    onAudioTrackImportClipAtPlayhead_ = std::move(fn);
}

void TrackLanesView::setStructuralTimelineEditBlockedPredicate(std::function<bool()> fn) noexcept
{
    structuralTimelineEditBlockedPredicate_ = std::move(fn);
}

void TrackLanesView::setInstrumentMidiClipMoveBlockedPredicate(std::function<bool()> fn) noexcept
{
    instrumentMidiClipMoveBlockedPredicate_ = std::move(fn);
}

void TrackLanesView::setCommittedHeaderDragTrackReorder(
    std::function<void(TrackId, int)> fn) noexcept
{
    committedHeaderDragTrackReorder_ = std::move(fn);
}

void TrackLanesView::syncInstrumentTimelineAttachments(
    const std::vector<InstrumentTimelineAttachment>& rows) noexcept
{
    std::unordered_set<TrackId> keep;
    keep.reserve(rows.size());
    for (const InstrumentTimelineAttachment& r : rows)
    {
        if (r.sessionTrackId != kInvalidTrackId)
        {
            keep.insert(r.sessionTrackId);
        }
    }

    for (auto it = instrumentTimelineAttachments_.begin(); it != instrumentTimelineAttachments_.end();)
    {
        if (keep.count(it->first) == 0)
        {
            InstrumentTimelineAttachment& slot = it->second;
            cancelHeaderDragIfSourceIs(slot.header);
            if (slot.header != nullptr && slot.header->getParentComponent() == this)
            {
                removeChildComponent(slot.header);
            }
            if (slot.midiLane != nullptr && slot.midiLane->getParentComponent() == this)
            {
                removeChildComponent(slot.midiLane);
            }
            it = instrumentTimelineAttachments_.erase(it);
        }
        else
        {
            ++it;
        }
    }

    for (const InstrumentTimelineAttachment& row : rows)
    {
        if (row.sessionTrackId == kInvalidTrackId || row.controller == nullptr || row.header == nullptr
            || row.midiLane == nullptr)
        {
            continue;
        }
        InstrumentTimelineAttachment& dst = instrumentTimelineAttachments_[row.sessionTrackId];
        dst = row;
        if (dst.header->getParentComponent() != this)
        {
            addAndMakeVisible(*dst.header);
        }
        if (dst.midiLane->getParentComponent() != this)
        {
            addAndMakeVisible(*dst.midiLane);
        }
    }

    refreshInstrumentHeaderReorderAttachments();
    rebuildVisibleTrackEntries();
    resized();
}

void TrackLanesView::detachInstrumentTimelineRowForTrack(const TrackId tid) noexcept
{
    const auto it = instrumentTimelineAttachments_.find(tid);
    if (it == instrumentTimelineAttachments_.end())
    {
        return;
    }
    InstrumentTimelineAttachment& slot = it->second;
    cancelHeaderDragIfSourceIs(slot.header);
    if (slot.header != nullptr && slot.header->getParentComponent() == this)
    {
        removeChildComponent(slot.header);
    }
    if (slot.midiLane != nullptr && slot.midiLane->getParentComponent() == this)
    {
        removeChildComponent(slot.midiLane);
    }
    instrumentTimelineAttachments_.erase(it);

    refreshInstrumentHeaderReorderAttachments();
    rebuildVisibleTrackEntries();
    resized();
}

void TrackLanesView::refreshInstrumentHeaderReorderAttachments() noexcept
{
    for (auto& kv : instrumentTimelineAttachments_)
    {
        InstrumentTimelineAttachment& a = kv.second;
        TrackHeaderView* const h = a.header;
        if (h == nullptr)
        {
            continue;
        }

        const bool trioReady = a.controller != nullptr && a.controller->hasInstrumentTrack() && a.midiLane != nullptr
                               && kv.first != kInvalidTrackId && a.sessionTrackId == kv.first;

        if (trioReady && a.controller->getExperimentalInstrumentDomainTrackId() != kv.first)
        {
            juce::Logger::writeToLog(
                "[TrackLanesView] Instrument header drag id "
                + juce::String((juce::int64)kv.first) + " does not match controller domain id "
                + juce::String((juce::int64)a.controller->getExperimentalInstrumentDomainTrackId())
                + " — header reorder disabled until they match.");
            h->setHeaderReorderDrag(std::nullopt, kInvalidTrackId);
            continue;
        }

        const TrackId dragId = trioReady ? kv.first : kInvalidTrackId;

        if (trioReady && dragId != kInvalidTrackId)
        {
            TrackHeaderDragHost dh;
            dh.onHeaderDragBegan
                = [this](const TrackId id, TrackHeaderView* src) { beginHeaderTrackDrag(id, *src); };
            dh.onHeaderDragMoved
                = [this](const TrackId id, const juce::Point<int> p) { updateHeaderTrackDrag(id, p); };
            dh.onHeaderDragEnded = [this](const TrackId id) { endHeaderTrackDrag(id); };
            h->setHeaderReorderDrag(std::move(dh), dragId);
        }
        else
        {
            h->setHeaderReorderDrag(std::nullopt, kInvalidTrackId);
        }
    }
}

void TrackLanesView::rebuildVisibleTrackEntries() noexcept
{
    visibleTrackEntries_.clear();
    const int n = session_.getNumTracks();
    visibleTrackEntries_.reserve((size_t)juce::jmax(0, n));

    for (int i = 0; i < n; ++i)
    {
        const TrackId tid = session_.getTrackIdAtIndex(i);
        if (tid == kInvalidTrackId)
        {
            jassert(false);
            continue;
        }

        if (session_.getTrackKindAtIndex(i) == TrackKind::Audio)
        {
            visibleTrackEntries_.push_back(
                VisibleTrackEntry{ VisibleTrackKind::Audio, tid });
            continue;
        }

        if (session_.getTrackKindAtIndex(i) == TrackKind::Master)
        {
            visibleTrackEntries_.push_back(VisibleTrackEntry{ VisibleTrackKind::Master, tid });
            continue;
        }

        if (session_.getTrackKindAtIndex(i) == TrackKind::Group)
        {
            visibleTrackEntries_.push_back(VisibleTrackEntry{ VisibleTrackKind::Group, tid });
            continue;
        }

        auto itAttach = instrumentTimelineAttachments_.find(tid);
        if (itAttach == instrumentTimelineAttachments_.end())
        {
            juce::Logger::writeToLog(
                juce::String("[TrackLanesView] Snapshot Instrument row id=") + juce::String((juce::int64)tid)
                + " has no instrument UI attachment.");
            continue;
        }

        InstrumentTrackController* const ctl = itAttach->second.controller;
        if (ctl == nullptr || !ctl->hasInstrumentTrack() || itAttach->second.header == nullptr
            || itAttach->second.midiLane == nullptr)
        {
            juce::Logger::writeToLog(juce::String("[TrackLanesView] Instrument attachment incomplete for tid=")
                                     + juce::String((juce::int64)tid) + ".");
            continue;
        }

        if (ctl->getExperimentalInstrumentDomainTrackId() != tid)
        {
            juce::Logger::writeToLog("[TrackLanesView] Snapshot Instrument row id="
                                     + juce::String((juce::int64)tid)
                                     + " mismatches controller domain id="
                                     + juce::String((juce::int64)ctl->getExperimentalInstrumentDomainTrackId())
                                     + ".");
            continue;
        }

        visibleTrackEntries_.push_back(
            VisibleTrackEntry{ VisibleTrackKind::Instrument, tid });
    }

    // Visual track groups + header multi-selection ride on the visible entries: refresh the
    // display cache (membership → 4 px collapsed strips, marker/handle runs) and drop selection
    // ids of tracks that no longer exist.
    rebuildVisualGroupDisplayCache();
    if (!headerMultiSelection_.empty())
    {
        headerMultiSelection_.erase(
            std::remove_if(headerMultiSelection_.begin(),
                           headerMultiSelection_.end(),
                           [this](const TrackId tid)
                           { return visibleRowIndexForTrackForDiagnostics(tid) < 0; }),
            headerMultiSelection_.end());
    }
    if (headerSelectionAnchorTid_ != kInvalidTrackId
        && visibleRowIndexForTrackForDiagnostics(headerSelectionAnchorTid_) < 0)
    {
        headerSelectionAnchorTid_ = kInvalidTrackId;
    }
}

bool TrackLanesView::isInstrumentTimelineRowVisible() const noexcept
{
    for (const VisibleTrackEntry& entry : visibleTrackEntries_)
        if (entry.kind == VisibleTrackKind::Instrument)
            return true;
    return false;
}

bool TrackLanesView::isStructuralTimelineEditBlocked() const noexcept
{
    if (structuralTimelineEditBlockedPredicate_)
    {
        return structuralTimelineEditBlockedPredicate_();
    }
    return recorder_.isRecording();
}

bool TrackLanesView::isInstrumentMidiClipMoveBlocked() const noexcept
{
    if (instrumentMidiClipMoveBlockedPredicate_)
    {
        return instrumentMidiClipMoveBlockedPredicate_();
    }
    return recorder_.isRecording();
}

bool TrackLanesView::isClipEditGestureInProgress() const noexcept
{
    for (const auto& u : lanes_)
    {
        if (u == nullptr)
        {
            continue;
        }
        if (u->isClipMoveGestureInProgress() || u->isClipTrimGestureInProgress())
        {
            return true;
        }
    }
    return false;
}

void TrackLanesView::clearAllPlacedClipSelections() noexcept
{
    aggregatedSelectedPlacedClip_.reset();
    for (auto& u : lanes_)
    {
        if (u != nullptr)
        {
            u->clearSelectionOnly();
        }
    }
}

void TrackLanesView::cancelAllClipGesturesAndTransientUiState() noexcept
{
    if constexpr (undo_diagnostic::kUndoDiag)
    {
        writeUndoDiagnosticLogLine(
            "[UndoDiag] TrackLanesView::cancelAllClipGestures entered laneCount="
            + juce::String(static_cast<int>(lanes_.size())));
    }
    aggregatedSelectedPlacedClip_.reset();
    for (auto& u : lanes_)
    {
        if (u != nullptr)
        {
            u->cancelInteractionStateForSnapshotRestore();
        }
    }
    clearHeaderTrackDragState();
    if constexpr (undo_diagnostic::kUndoDiag)
    {
        writeUndoDiagnosticLogLine("[UndoDiag] TrackLanesView::cancelAllClipGestures done");
    }
}

void TrackLanesView::rebuildChildLanesIfNeeded()
{
    prunePerTrackRowHeightsNotInSession();

    const int n = session_.getNumTracks();
    if (n <= 0)
    {
        for (const auto& h : headers_)
        {
            cancelHeaderDragIfSourceIs(h.get());
        }
        headers_.clear();
        lanes_.clear();
        perTrackRowHeightPx_.clear();
        aggregatedSelectedPlacedClip_.reset();
        rebuildVisibleTrackEntries();
        return;
    }
    std::vector<TrackId> audioTrackIds;
    audioTrackIds.reserve((size_t)n);
    for (int i = 0; i < n; ++i)
    {
        if (session_.getTrackKindAtIndex(i) == TrackKind::Audio)
        {
            const TrackId tid = session_.getTrackIdAtIndex(i);
            if (tid != kInvalidTrackId)
            {
                audioTrackIds.push_back(tid);
            }
        }
    }

    bool need = (((int)lanes_.size() != (int)audioTrackIds.size())
                 || ((int)headers_.size() != (int)audioTrackIds.size()));
    if (!need)
    {
        for (int i = 0; i < (int)audioTrackIds.size(); ++i)
        {
            if (lanes_[(size_t)i]->getTrackId() != audioTrackIds[(size_t)i])
            {
                need = true;
                break;
            }
        }
    }
    if (!need)
    {
        rebuildVisibleTrackEntries();
        refreshInstrumentHeaderReorderAttachments();
        return;
    }
    for (const auto& h : headers_)
    {
        cancelHeaderDragIfSourceIs(h.get());
    }
    headers_.clear();
    lanes_.clear();
    aggregatedSelectedPlacedClip_.reset();

    for (const TrackId tid : audioTrackIds)
    {
        if (tid == kInvalidTrackId)
        {
            jassert(false);
            continue;
        }
        TrackHeaderDragHost dragHost;
        dragHost.onHeaderDragBegan
            = [this](const TrackId id, TrackHeaderView* const src) { beginHeaderTrackDrag(id, *src); };
        dragHost.onHeaderDragMoved
            = [this](const TrackId id, const juce::Point<int> p) { updateHeaderTrackDrag(id, p); };
        dragHost.onHeaderDragEnded
            = [this](const TrackId id) { endHeaderTrackDrag(id); };
        const auto onActive = [this] { repaint(); };
        const auto onArm = [this] {
            for (auto& h : headers_)
            {
                h->repaint();
            }
        };
        auto onDelete = [this](const TrackId id) {
            if (onDeleteTrackRequested_ != nullptr)
            {
                onDeleteTrackRequested_(id);
            }
        };

        TrackHeaderModelProvider modelProvider = [this, tid]() -> TrackHeaderModel {
            TrackHeaderModel m;
            m.subtitle = {};
            const bool sessionSaysActive = (session_.getActiveTrackId() == tid);
            const bool suppressed
                = headerActiveSuppressProvider_ != nullptr && headerActiveSuppressProvider_();
            m.active = sessionSaysActive && !suppressed;
            m.armed = (recorder_.getArmedTrackId() == tid);
            if (const auto snap = session_.loadSessionSnapshotForAudioThread())
            {
                const int idx = snap->findTrackIndexById(tid);
                if (idx >= 0)
                {
                const Track& tr = snap->getTrack(idx);
                m.name = tr.getName();
                m.off = tr.isTrackOff();
                m.muted = tr.isMuted();
                m.trackNameRenameEnabled = (tr.getKind() != TrackKind::Master);
            }
        }
        m.powerInteractable = !isStructuralTimelineEditBlocked();
            m.muteInteractable = true;
            m.armInteractable = true;
            // Monitor (audio rows only — this builder only runs for `TrackKind::Audio`): runtime
            // engine state via the injected hooks; cell omitted when the hooks are not wired.
            m.monitorAvailable = isTrackInputMonitoredFn_ != nullptr
                                 && toggleTrackInputMonitorFn_ != nullptr;
            m.monitorEnabled = m.monitorAvailable && isTrackInputMonitoredFn_(tid);
            m.monitorInteractable = true;
            if (soloUiHooks_.displayState != nullptr)
            {
                const TrackSoloDisplayState st = soloUiHooks_.displayState(tid);
                m.soloAvailable = true;
                m.soloed = st.soloed;
                m.soloSilenced = st.soloSilenced;
                m.muteLockedBySolo = st.muteLocked;
            }
            m.headerMultiSelected = isHeaderMultiSelected(tid);
            m.visualGroupMember = isTrackInDisplayableVisualGroup(tid);
            fillCommonHeaderModelFields(m, tid);
            return m;
        };

        TrackHeaderCallbacks callbacks;
        callbacks.onActivateName = [this, tid, onActive] {
            session_.setActiveTrack(tid);
            if (onAudioHeaderActivated_ != nullptr)
            {
                onAudioHeaderActivated_();
            }
            onActive();
        };
        callbacks.onHeaderSelectionClick = [this, tid](const bool shiftRange) {
            handleHeaderSelectionClick(tid, shiftRange);
        };
        callbacks.onShowColourMenu = [this, tid](TrackHeaderView&, const juce::Rectangle<int> anchor) {
            showTrackColourMenuForTrack(tid, anchor);
        };
        callbacks.onToggleArm = [this, tid, onActive, onArm] {
            if (const auto snap = session_.loadSessionSnapshotForAudioThread())
            {
                const int idx = snap->findTrackIndexById(tid);
                if (idx < 0 || !trackKindAcceptsRecordArm(snap->getTrack(idx).getKind()))
                {
                    return;
                }
            }
            if (recorder_.getArmedTrackId() == tid)
            {
                recorder_.disarm();
            }
            else
            {
                recorder_.armForRecording(tid);
            }
            onArm();
            session_.setActiveTrack(tid);
            if (onAudioHeaderActivated_ != nullptr)
            {
                onAudioHeaderActivated_();
            }
            onActive();
        };
        callbacks.onToggleMonitor = [this, tid] {
            // Runtime control: flips engine monitor state only — no session edit, no undo entry,
            // no dirty flag. Repaint headers so the speaker reflects the new state immediately.
            if (toggleTrackInputMonitorFn_ != nullptr)
            {
                toggleTrackInputMonitorFn_(tid);
            }
            for (auto& h : headers_)
            {
                h->repaint();
            }
        };
        callbacks.onToggleMute = [this, tid, onActive, onArm] {
            bool nowMuted = true;
            if (const auto snap = session_.loadSessionSnapshotForAudioThread())
            {
                const int idx = snap->findTrackIndexById(tid);
                if (idx >= 0)
                {
                    nowMuted = !snap->getTrack(idx).isMuted();
                }
            }
            session_.setTrackMuted(tid, nowMuted);
            onArm();
            session_.setActiveTrack(tid);
            if (onAudioHeaderActivated_ != nullptr)
            {
                onAudioHeaderActivated_();
            }
            onActive();
        };
        callbacks.onToggleSolo = [this, tid, onActive, onArm] {
            if (soloUiHooks_.toggleSolo != nullptr)
            {
                soloUiHooks_.toggleSolo(tid);
            }
            onArm();
            session_.setActiveTrack(tid);
            if (onAudioHeaderActivated_ != nullptr)
            {
                onAudioHeaderActivated_();
            }
            onActive();
        };
        callbacks.onTogglePower = [this, tid, onActive, onArm]() -> bool {
            if (isStructuralTimelineEditBlocked())
            {
                return false;
            }
            bool nowOff = true;
            if (const auto snap = session_.loadSessionSnapshotForAudioThread())
            {
                const int idx = snap->findTrackIndexById(tid);
                if (idx >= 0)
                {
                    nowOff = !snap->getTrack(idx).isTrackOff();
                }
            }
            session_.setTrackOff(tid, nowOff);
            onArm();
            session_.setActiveTrack(tid);
            if (onAudioHeaderActivated_ != nullptr)
            {
                onAudioHeaderActivated_();
            }
            onActive();
            return true;
        };
        callbacks.onShowContextMenu = [this, tid, onActive, onDelete, pluginHost = trackHeaderPluginHost_](
            TrackHeaderView& self, const juce::MouseEvent&) {
            // Right-click selection policy (groups spec §2): inside the multi-selection → keep it
            // so "Create collapsible group…" can target the range; outside → select clicked row.
            applyHeaderRightClickSelectionPolicy(tid);
            session_.setActiveTrack(tid);
            if (onAudioHeaderActivated_ != nullptr)
            {
                onAudioHeaderActivated_();
            }
            onActive();

            juce::PopupMenu menu;
            constexpr int kDeleteTrackMenuId = 1;
            constexpr int kDuplicateTrackMenuId = 2;
            constexpr int kCreateCollapsibleGroupMenuId = 3;
            constexpr int kLoadVst3MenuId = 10;
            constexpr int kPluginEditorMenuId = 11;
            constexpr int kPluginParamsMenuId = 12;
            constexpr int kRemovePluginMenuId = 13;
            constexpr int kImportAudioClipMenuId = 14;

            const bool editLocked = isStructuralTimelineEditBlocked();
            juce::PopupMenu::Item deleteItem;
            deleteItem.itemID = kDeleteTrackMenuId;
            deleteItem.text = "Delete Track";
            deleteItem.isEnabled = !editLocked;
            menu.addItem(deleteItem);
            menu.addItem(makeDuplicateTrackMenuItem(kDuplicateTrackMenuId, editLocked));
            appendCreateCollapsibleGroupMenuItem(menu, kCreateCollapsibleGroupMenuId);

            if (onAudioTrackImportClipAtPlayhead_ != nullptr)
            {
                juce::PopupMenu::Item importAudioItem;
                importAudioItem.itemID = kImportAudioClipMenuId;
                importAudioItem.text = "Import audio clip at playhead...";
                importAudioItem.isEnabled = !editLocked;
                menu.addItem(importAudioItem);
            }

            if (pluginHost.loadVst3 != nullptr)
            {
                juce::PopupMenu::Item loadItem;
                loadItem.itemID = kLoadVst3MenuId;
                loadItem.text = juce::String(juce::CharPointer_UTF8("Load VST3\xe2\x80\xa6"));
                loadItem.isEnabled = !editLocked;
                menu.addItem(loadItem);
            }
            if (pluginHost.openPluginEditor != nullptr)
            {
                juce::PopupMenu::Item edItem;
                edItem.itemID = kPluginEditorMenuId;
                edItem.text = juce::String(juce::CharPointer_UTF8("Plugin editor\xe2\x80\xa6"));
                edItem.isEnabled = !editLocked;
                menu.addItem(edItem);
            }
            if (pluginHost.openPluginParams != nullptr)
            {
                juce::PopupMenu::Item parItem;
                parItem.itemID = kPluginParamsMenuId;
                parItem.text = juce::String(juce::CharPointer_UTF8("Plugin parameters\xe2\x80\xa6"));
                parItem.isEnabled = !editLocked;
                menu.addItem(parItem);
            }
            if (pluginHost.removePlugin != nullptr)
            {
                juce::PopupMenu::Item rmItem;
                rmItem.itemID = kRemovePluginMenuId;
                rmItem.text = "Remove VST3";
                rmItem.isEnabled = !editLocked;
                menu.addItem(rmItem);
            }

            juce::Component::SafePointer<TrackHeaderView> safeThis(&self);
            menu.showMenuAsync(
                juce::PopupMenu::Options().withTargetComponent(&self),
                [safeThis,
                 this,
                 pluginHost,
                 tid,
                 onDelete,
                 kDeleteTrackMenuId,
                 kDuplicateTrackMenuId,
                 kCreateCollapsibleGroupMenuId,
                 kImportAudioClipMenuId,
                 kLoadVst3MenuId,
                 kPluginEditorMenuId,
                 kPluginParamsMenuId,
                 kRemovePluginMenuId](const int result) {
                    if (safeThis == nullptr)
                    {
                        return;
                    }
                    if (result == kDeleteTrackMenuId)
                    {
                        if (isStructuralTimelineEditBlocked())
                        {
                            return;
                        }
                        onDelete(tid);
                        return;
                    }
                    if (result == kDuplicateTrackMenuId)
                    {
                        requestDuplicateTrackForHeaderMenu(tid);
                        return;
                    }
                    if (result == kCreateCollapsibleGroupMenuId)
                    {
                        requestCreateCollapsibleGroupFromSelection();
                        return;
                    }
                    if (result == kImportAudioClipMenuId)
                    {
                        if (isStructuralTimelineEditBlocked())
                        {
                            return;
                        }
                        if (onAudioTrackImportClipAtPlayhead_ != nullptr)
                        {
                            onAudioTrackImportClipAtPlayhead_(tid);
                        }
                        return;
                    }
                    if (isStructuralTimelineEditBlocked())
                    {
                        return;
                    }
                    if (result == kLoadVst3MenuId && pluginHost.loadVst3 != nullptr)
                    {
                        pluginHost.loadVst3(tid);
                    }
                    else if (result == kPluginEditorMenuId && pluginHost.openPluginEditor != nullptr)
                    {
                        pluginHost.openPluginEditor(tid);
                    }
                    else if (result == kPluginParamsMenuId && pluginHost.openPluginParams != nullptr)
                    {
                        pluginHost.openPluginParams(tid);
                    }
                    else if (result == kRemovePluginMenuId && pluginHost.removePlugin != nullptr)
                    {
                        pluginHost.removePlugin(tid);
                    }
                });
        };

        callbacks.onRowHeightDrag = [this, tid](const int startH, const int delta) {
            applyTrackRowHeightDelta(tid, startH, delta);
        };
        callbacks.onRowHeightDragEnd = [this, tid] {
            snapTrackHeaderRowHeightAfterResize(tid, false);
        };
        callbacks.canBeginRenameTrack = [this, tid]() {
            // Metadata rename: same rules as Inspector / TrackLanesEditCoordinator (recording + count-in only).
            if (isInstrumentMidiClipMoveBlocked())
            {
                return false;
            }
            if (const auto snap = session_.loadSessionSnapshotForAudioThread())
            {
                const int idx = snap->findTrackIndexById(tid);
                if (idx >= 0 && snap->getTrack(idx).getKind() == TrackKind::Master)
                {
                    return false;
                }
            }
            return true;
        };
        callbacks.onCommitRenameTrack = [this, tid](const juce::String raw) -> bool {
            if (onUndoableRenameTrackRequested_ == nullptr)
            {
                return false;
            }
            return onUndoableRenameTrackRequested_(tid, raw);
        };

        auto head = std::make_unique<TrackHeaderView>(
            std::move(modelProvider),
            std::move(callbacks),
            tid,
            std::optional<TrackHeaderDragHost>(std::move(dragHost)));
        addAndMakeVisible(*head);
        headers_.push_back(std::move(head));
        ClipWaveformLaneHost host;
        host.onBeginMouseDown = [this](ClipWaveformView& sender) {
            for (auto& u : lanes_)
            {
                if (u.get() != &sender)
                {
                    u->clearSelectionOnly();
                }
            }
            if (onAudioClipMouseDownClearForeignSelections_ != nullptr)
            {
                onAudioClipMouseDownClearForeignSelections_();
            }
        };
        host.findLaneAtScreen = [this](const juce::Point<int> screenPos) -> ClipWaveformView* {
            return findLaneAtScreenPosition(screenPos);
        };
        host.setGhostOnLane
            = [this](ClipWaveformView* target, const std::int64_t start, const std::int64_t len) {
                  setGhostOnLaneImpl(target, start, len);
              };
        host.clearAllGhosts = [this] { clearAllGhostsImpl(); };
        host.onPlacedClipSelectionChanged =
            [this](const TrackId laneId, const std::optional<PlacedClipId> id) {
                onLanePlacedClipSelectionChanged(laneId, id);
            };
        host.commitClipMoveAsUndoable =
            [this](const PlacedClipId id, const std::int64_t start, const std::optional<TrackId> dest) -> bool {
                if (onUndoableClipMoveRequested_ != nullptr)
                {
                    return onUndoableClipMoveRequested_(id, start, dest);
                }
                if (dest.has_value())
                {
                    session_.moveClipToTrack(id, start, *dest);
                }
                else
                {
                    session_.moveClip(id, start);
                }
                return true;
            };
        host.commitClipTrimAsUndoable =
            [this](const PlacedClipId id, const ClipTrimEdge edge, const std::int64_t value) -> bool {
                if (onUndoableClipTrimRequested_ != nullptr)
                {
                    return onUndoableClipTrimRequested_(id, edge, value);
                }
                if (edge == ClipTrimEdge::Left)
                {
                    session_.setClipLeftEdgeTrim(id, value);
                }
                else
                {
                    session_.setClipRightEdgeVisibleLength(id, value);
                }
                return true;
            };
        host.commitClipRenameAsUndoable =
            [this](const PlacedClipId id, juce::String newName) -> bool {
                if (onUndoableClipRenameRequested_ != nullptr)
                {
                    return onUndoableClipRenameRequested_(id, std::move(newName));
                }
                session_.setPlacedClipName(id, std::move(newName));
                return true;
            };
        host.getActiveEditTool = [this]() -> EditTool {
            return activeEditToolProvider_ != nullptr ? activeEditToolProvider_() : EditTool::Pointer;
        };
        host.commitClipSplitAsUndoable =
            [this](const PlacedClipId id, const std::int64_t splitT, const bool wasSel) {
                if (onUndoableClipSplitRequested_ != nullptr)
                {
                    onUndoableClipSplitRequested_(id, splitT, wasSel);
                }
                else
                {
                    (void)session_.splitClip(id, splitT);
                }
            };
        host.snapArrangementTimelineSample = [this](std::int64_t s) -> std::int64_t {
            if (arrangementTimelineSnap_ != nullptr)
            {
                return arrangementTimelineSnap_(s);
            }
            return juce::jmax(std::int64_t{ 0 }, s);
        };
        auto ptr = std::make_unique<ClipWaveformView>(
            session_, transport_, tid, timelineViewport_, waveformCache_, std::move(host));
        addAndMakeVisible(*ptr);
        lanes_.push_back(std::move(ptr));
    }
    rebuildVisibleTrackEntries();
    refreshInstrumentHeaderReorderAttachments();
}

void TrackLanesView::rebuildMasterHeadersIfNeeded()
{
    TrackId masterId = kInvalidTrackId;
    const int n = session_.getNumTracks();
    for (int i = n - 1; i >= 0; --i)
    {
        if (session_.getTrackKindAtIndex(i) == TrackKind::Master)
        {
            masterId = session_.getTrackIdAtIndex(i);
            break;
        }
    }

    TrackId boundHeaderId = kInvalidTrackId;
    if (!masterHeaders_.empty() && masterHeaders_[0] != nullptr)
    {
        boundHeaderId = masterHeaders_[0]->getBoundTrackId();
    }

    const bool needRebuild
        = (masterId == kInvalidTrackId && !masterHeaders_.empty())
          || (masterId != kInvalidTrackId && masterHeaders_.size() != 1U)
          || (masterId != kInvalidTrackId && masterId != boundHeaderId);

    if (!needRebuild)
    {
        return;
    }

    for (const auto& h : masterHeaders_)
    {
        cancelHeaderDragIfSourceIs(h.get());
    }
    masterHeaders_.clear();

    if (masterId == kInvalidTrackId)
    {
        return;
    }

    const TrackId tid = masterId;
    const auto onActive = [this] { repaint(); };

    TrackHeaderModelProvider modelProvider = [this, tid]() -> TrackHeaderModel {
        TrackHeaderModel m;
        m.subtitle = {};
        const bool sessionSaysActive = (session_.getActiveTrackId() == tid);
        const bool suppressed
            = headerActiveSuppressProvider_ != nullptr && headerActiveSuppressProvider_();
        m.active = sessionSaysActive && !suppressed;
        m.armed = false;
        m.armInteractable = false;
        m.showRecordAndPowerStripCells = false;
        m.trackNameRenameEnabled = false;
        m.off = false;
        if (const auto snap = session_.loadSessionSnapshotForAudioThread())
        {
            const int idx = snap->findTrackIndexById(tid);
            if (idx >= 0)
            {
                const Track& tr = snap->getTrack(idx);
                m.name = juce::String(kMasterTrackDisplayName);
                m.muted = tr.isMuted();
            }
        }
        else
        {
            m.name = juce::String(kMasterTrackDisplayName);
        }
        m.powerInteractable = false;
        m.muteInteractable = true;
        // Master: never an S cell (`soloAvailable` stays false), but its M still shows the
        // effective state + lock while solo is active elsewhere (mute edits are globally locked).
        if (soloUiHooks_.displayState != nullptr)
        {
            const TrackSoloDisplayState st = soloUiHooks_.displayState(tid);
            m.soloSilenced = st.soloSilenced;
            m.muteLockedBySolo = st.muteLocked;
        }
        // Stereo Out can be shift-range selected (clear indication) but never grouped — the
        // Create item validates against Master membership, and `visualGroupMember` stays false.
        m.headerMultiSelected = isHeaderMultiSelected(tid);
        fillCommonHeaderModelFields(m, tid);
        return m;
    };

    TrackHeaderCallbacks callbacks;
    callbacks.onActivateName = [this, tid, onActive] {
        session_.setActiveTrack(tid);
        if (onAudioHeaderActivated_ != nullptr)
        {
            onAudioHeaderActivated_();
        }
        onActive();
    };
    callbacks.onHeaderSelectionClick = [this, tid](const bool shiftRange) {
        handleHeaderSelectionClick(tid, shiftRange);
    };
    callbacks.onShowColourMenu = [this, tid](TrackHeaderView&, const juce::Rectangle<int> anchor) {
        showTrackColourMenuForTrack(tid, anchor);
    };
    callbacks.onToggleMute = [this, tid, onActive] {
        bool nowMuted = true;
        if (const auto snap = session_.loadSessionSnapshotForAudioThread())
        {
            const int idx = snap->findTrackIndexById(tid);
            if (idx >= 0)
            {
                nowMuted = !snap->getTrack(idx).isMuted();
            }
        }
        session_.setTrackMuted(tid, nowMuted);
        session_.setActiveTrack(tid);
        if (onAudioHeaderActivated_ != nullptr)
        {
            onAudioHeaderActivated_();
        }
        onActive();
    };
    callbacks.onRowHeightDrag = [this, tid](const int startH, const int delta) {
        applyTrackRowHeightDelta(tid, startH, delta);
    };
    callbacks.onRowHeightDragEnd = [this, tid] { snapTrackHeaderRowHeightAfterResize(tid, false); };
    callbacks.canBeginRenameTrack = nullptr;
    callbacks.onCommitRenameTrack = nullptr;

    auto head = std::make_unique<TrackHeaderView>(
        std::move(modelProvider), std::move(callbacks), tid, std::nullopt);
    addAndMakeVisible(*head);
    masterHeaders_.push_back(std::move(head));
}

void TrackLanesView::rebuildGroupHeadersIfNeeded()
{
    std::unordered_set<TrackId> wanted;
    const int n = session_.getNumTracks();
    for (int i = 0; i < n; ++i)
    {
        if (session_.getTrackKindAtIndex(i) == TrackKind::Group)
        {
            const TrackId tid = session_.getTrackIdAtIndex(i);
            if (tid != kInvalidTrackId)
            {
                wanted.insert(tid);
            }
        }
    }

    for (auto it = groupHeaders_.begin(); it != groupHeaders_.end();)
    {
        if (wanted.count(it->first) == 0)
        {
            cancelHeaderDragIfSourceIs(it->second.get());
            removeChildComponent(it->second.get());
            it = groupHeaders_.erase(it);
        }
        else
        {
            ++it;
        }
    }

    for (const TrackId tid : wanted)
    {
        if (groupHeaders_.count(tid) > 0)
        {
            continue;
        }

        const auto onActive = [this] { repaint(); };
        TrackHeaderModelProvider modelProvider = [this, tid]() -> TrackHeaderModel {
            TrackHeaderModel m;
            m.subtitle = {};
            const bool sessionSaysActive = (session_.getActiveTrackId() == tid);
            const bool suppressed
                = headerActiveSuppressProvider_ != nullptr && headerActiveSuppressProvider_();
            m.active = sessionSaysActive && !suppressed;
            m.armed = false;
            m.armInteractable = false;
            m.showRecordAndPowerStripCells = false;
            m.trackNameRenameEnabled = true;
            if (const auto snap = session_.loadSessionSnapshotForAudioThread())
            {
                const int idx = snap->findTrackIndexById(tid);
                if (idx >= 0)
                {
                    const Track& tr = snap->getTrack(idx);
                    m.name = tr.getName();
                    m.muted = tr.isMuted();
                }
            }
            m.muteInteractable = true;
            if (soloUiHooks_.displayState != nullptr)
            {
                const TrackSoloDisplayState st = soloUiHooks_.displayState(tid);
                m.soloAvailable = true; // group rows solo (mute-only chrome becomes [M][S])
                m.soloed = st.soloed;
                m.soloSilenced = st.soloSilenced;
                m.muteLockedBySolo = st.muteLocked;
            }
            m.headerMultiSelected = isHeaderMultiSelected(tid);
            m.visualGroupMember = isTrackInDisplayableVisualGroup(tid);
            fillCommonHeaderModelFields(m, tid);
            return m;
        };

        TrackHeaderCallbacks callbacks;
        callbacks.onActivateName = [this, tid, onActive] {
            session_.setActiveTrack(tid);
            if (onAudioHeaderActivated_ != nullptr)
            {
                onAudioHeaderActivated_();
            }
            onActive();
        };
        callbacks.onHeaderSelectionClick = [this, tid](const bool shiftRange) {
            handleHeaderSelectionClick(tid, shiftRange);
        };
        callbacks.onShowColourMenu = [this, tid](TrackHeaderView&, const juce::Rectangle<int> anchor) {
            showTrackColourMenuForTrack(tid, anchor);
        };
        callbacks.onToggleMute = [this, tid, onActive] {
            bool nowMuted = true;
            if (const auto snap = session_.loadSessionSnapshotForAudioThread())
            {
                const int idx = snap->findTrackIndexById(tid);
                if (idx >= 0)
                {
                    nowMuted = !snap->getTrack(idx).isMuted();
                }
            }
            session_.setTrackMuted(tid, nowMuted);
            session_.setActiveTrack(tid);
            if (onAudioHeaderActivated_ != nullptr)
            {
                onAudioHeaderActivated_();
            }
            onActive();
        };
        callbacks.onToggleSolo = [this, tid, onActive] {
            if (soloUiHooks_.toggleSolo != nullptr)
            {
                soloUiHooks_.toggleSolo(tid);
            }
            session_.setActiveTrack(tid);
            if (onAudioHeaderActivated_ != nullptr)
            {
                onAudioHeaderActivated_();
            }
            onActive();
        };
        callbacks.onRowHeightDrag = [this, tid](const int startH, const int delta) {
            applyTrackRowHeightDelta(tid, startH, delta);
        };
        callbacks.onRowHeightDragEnd = [this, tid] { snapTrackHeaderRowHeightAfterResize(tid, false); };
        callbacks.canBeginRenameTrack = [this, tid]() {
            if (isInstrumentMidiClipMoveBlocked())
            {
                return false;
            }
            return true;
        };
        callbacks.onCommitRenameTrack = [this, tid](const juce::String raw) -> bool {
            if (onUndoableRenameTrackRequested_ == nullptr)
            {
                return false;
            }
            return onUndoableRenameTrackRequested_(tid, raw);
        };
        auto onDelete = [this](const TrackId id) {
            if (onDeleteTrackRequested_ != nullptr)
            {
                onDeleteTrackRequested_(id);
            }
        };
        callbacks.onShowContextMenu = [this, tid, onActive, onDelete](TrackHeaderView& self,
                                                                      const juce::MouseEvent&) {
            applyHeaderRightClickSelectionPolicy(tid);
            session_.setActiveTrack(tid);
            if (onAudioHeaderActivated_ != nullptr)
            {
                onAudioHeaderActivated_();
            }
            onActive();

            juce::PopupMenu menu;
            constexpr int kDeleteTrackMenuId = 1;
            constexpr int kDuplicateTrackMenuId = 2;
            constexpr int kCreateCollapsibleGroupMenuId = 3;
            const bool editLocked = isStructuralTimelineEditBlocked();
            juce::PopupMenu::Item deleteItem;
            deleteItem.itemID = kDeleteTrackMenuId;
            deleteItem.text = "Delete Track";
            deleteItem.isEnabled = !editLocked;
            menu.addItem(deleteItem);
            menu.addItem(makeDuplicateTrackMenuItem(kDuplicateTrackMenuId, editLocked));
            appendCreateCollapsibleGroupMenuItem(menu, kCreateCollapsibleGroupMenuId);

            juce::Component::SafePointer<TrackHeaderView> safeThis(&self);
            menu.showMenuAsync(
                juce::PopupMenu::Options().withTargetComponent(&self),
                [safeThis, this, onDelete, tid, kDeleteTrackMenuId, kDuplicateTrackMenuId,
                 kCreateCollapsibleGroupMenuId](const int result) {
                    if (safeThis == nullptr)
                    {
                        return;
                    }
                    if (result == kDuplicateTrackMenuId)
                    {
                        requestDuplicateTrackForHeaderMenu(tid);
                        return;
                    }
                    if (result == kCreateCollapsibleGroupMenuId)
                    {
                        requestCreateCollapsibleGroupFromSelection();
                        return;
                    }
                    if (result != kDeleteTrackMenuId || isStructuralTimelineEditBlocked())
                    {
                        return;
                    }
                    onDelete(tid);
                });
        };

        auto head = std::make_unique<TrackHeaderView>(
            std::move(modelProvider), std::move(callbacks), tid, std::nullopt);
        addAndMakeVisible(*head);
        groupHeaders_[tid] = std::move(head);
    }
}

ClipWaveformView* TrackLanesView::findLaneAtScreenPosition(const juce::Point<int> screenPos)
{
    const juce::Point<int> local = getLocalPoint(nullptr, screenPos);
    if (!getLocalBounds().contains(local))
    {
        return nullptr;
    }
    for (auto& u : lanes_)
    {
        if (u->getBounds().contains(local))
        {
            return u.get();
        }
    }
    return nullptr;
}

void TrackLanesView::setGhostOnLaneImpl(
    ClipWaveformView* const target,
    const std::int64_t startSample,
    const std::int64_t lengthSamples)
{
    for (auto& u : lanes_)
    {
        if (u.get() == target)
        {
            u->setDragGhost(startSample, lengthSamples);
        }
        else
        {
            u->clearDragGhost();
        }
    }
}

void TrackLanesView::clearAllGhostsImpl()
{
    for (auto& u : lanes_)
    {
        u->clearDragGhost();
    }
}

void TrackLanesView::resized()
{
    rebuildChildLanesIfNeeded();
    rebuildVisibleTrackEntries();

    auto area = getLocalBounds();
    const int vr = static_cast<int>(visibleTrackEntries_.size());

    // Shared boundary handle: full height (gutter included), centred on the header/lane edge. Kept
    // on top of every row child below (rows call `toFront` while laying out, so re-raise at the end).
    {
        const int hx = area.getX() + juce::jmin(headerColumnWidthPx(), area.getWidth());
        const int half = kHeaderColumnResizeHandleWidthPx / 2;
        headerColumnResizeHandle_.setBounds(hx - half, area.getY(), kHeaderColumnResizeHandleWidthPx, area.getHeight());
        headerColumnResizeHandle_.setVisible(area.getWidth() > kTrackHeaderColumnMinWidthPx);
    }
    const juce::ScopeGuard raiseHandleAtExit{ [this] { headerColumnResizeHandle_.toFront(false); } };

    std::unordered_set<TrackId> instrumentVisibleTids;
    for (const VisibleTrackEntry& ve : visibleTrackEntries_)
        if (ve.kind == VisibleTrackKind::Instrument)
            instrumentVisibleTids.insert(ve.sessionTrackId);

    for (auto& kv : instrumentTimelineAttachments_)
    {
        const bool visible = instrumentVisibleTids.count(kv.first) > 0;
        InstrumentTimelineAttachment& a = kv.second;
        if (a.header != nullptr)
        {
            a.header->setVisible(visible && a.controller != nullptr && a.controller->hasInstrumentTrack());
        }
        if (a.midiLane != nullptr)
        {
            a.midiLane->setVisible(visible && a.controller != nullptr && a.controller->hasInstrumentTrack());
        }
    }

    if (area.getHeight() <= 0 || vr <= 0)
    {
        if (vr <= 0)
        {
            verticalScrollOffsetPx_ = 0;
        }
        publishVerticalScrollModelIfChanged();
        return;
    }

    constexpr int gutter = kArrangementTimelineHeaderGutterPx;
    if (area.getHeight() <= gutter)
    {
        publishVerticalScrollModelIfChanged();
        return;
    }

    const auto scrollViewport = area.withTrimmedTop(gutter);
    const int viewportH = scrollViewport.getHeight();
    int contentH = 0;
    for (int vi = 0; vi < vr; ++vi)
    {
        contentH += rowHeightForVisibleEntry(vi);
    }
    verticalScrollOffsetPx_
        = juce::jlimit(0, juce::jmax(0, contentH - viewportH), verticalScrollOffsetPx_);

    const int w = area.getWidth();
    const int leftW = juce::jmin(headerColumnWidthPx(), w);

    int y = scrollViewport.getY() - verticalScrollOffsetPx_;
    for (int vi = 0; vi < vr; ++vi)
    {
        const int rowH = juce::jmax(1, rowHeightForVisibleEntry(vi));
        juce::Rectangle row(area.getX(), y, w, rowH);
        auto visibleRow = row.getIntersection(scrollViewport);
        const VisibleTrackEntry& e = visibleTrackEntries_[(size_t)vi];
        if (isTrackInCollapsedVisualGroup(e.sessionTrackId))
        {
            // Collapsed group member: the 4 px mini strip is painted by this view directly; the
            // row's header/lane components get EMPTY bounds (like scrolled-out rows) so none of
            // their buttons, clips or tooltips can be hit under the strip (groups spec §4).
            visibleRow = {};
        }
        if (e.kind == VisibleTrackKind::Instrument)
        {
            auto itA = instrumentTimelineAttachments_.find(e.sessionTrackId);
            if (itA != instrumentTimelineAttachments_.end() && itA->second.header != nullptr
                && itA->second.midiLane != nullptr)
            {
                if (!visibleRow.isEmpty())
                {
                    auto split = visibleRow;
                    const int hw = juce::jmin(leftW, split.getWidth());
                    itA->second.header->setBounds(split.removeFromLeft(hw));
                    itA->second.midiLane->setBounds(split);
                }
                else
                {
                    itA->second.header->setBounds(0, 0, 0, 0);
                    itA->second.midiLane->setBounds(0, 0, 0, 0);
                }
                itA->second.header->toFront(false);
                itA->second.midiLane->toFront(false);
            }
        }
        else if (e.kind == VisibleTrackKind::Master && !masterHeaders_.empty()
                 && masterHeaders_[0] != nullptr)
        {
            TrackHeaderView& mh = *masterHeaders_[0];
            if (!visibleRow.isEmpty())
            {
                auto split = visibleRow;
                const int hw = juce::jmin(leftW, split.getWidth());
                mh.setBounds(split.removeFromLeft(hw));
            }
            else
            {
                mh.setBounds(0, 0, 0, 0);
            }
            mh.toFront(false);
        }
        else if (e.kind == VisibleTrackKind::Group)
        {
            auto itG = groupHeaders_.find(e.sessionTrackId);
            if (itG != groupHeaders_.end() && itG->second != nullptr)
            {
                TrackHeaderView& gh = *itG->second;
                if (!visibleRow.isEmpty())
                {
                    auto split = visibleRow;
                    const int hw = juce::jmin(leftW, split.getWidth());
                    gh.setBounds(split.removeFromLeft(hw));
                }
                else
                {
                    gh.setBounds(0, 0, 0, 0);
                }
                gh.toFront(false);
            }
        }
        else
        {
            const int si = audioLaneIndexFromTrackId(e.sessionTrackId);
            if (si >= 0 && si < (int)headers_.size() && si < (int)lanes_.size()
                && headers_[(size_t)si] != nullptr && lanes_[(size_t)si] != nullptr)
            {
                if (!visibleRow.isEmpty())
                {
                    auto split = visibleRow;
                    const int hw = juce::jmin(leftW, split.getWidth());
                    headers_[(size_t)si]->setBounds(split.removeFromLeft(hw));
                    lanes_[(size_t)si]->setBounds(split);
                }
                else
                {
                    headers_[(size_t)si]->setBounds(0, 0, 0, 0);
                    lanes_[(size_t)si]->setBounds(0, 0, 0, 0);
                }
            }
        }
        y += rowH;
    }

    const int tw = juce::jmax(0, getWidth() - headerColumnWidthPx());
    if (tw > 0)
    {
        timelineViewport_.clampToExtent((double)tw, session_.getArrangementExtentSamples());
    }
    layoutVisualGroupHandles();
    publishVerticalScrollModelIfChanged();
}

TrackLanesView::VerticalScrollModel TrackLanesView::verticalScrollModel() const noexcept
{
    VerticalScrollModel m;
    m.contentHeightPx = visibleTrackEntries_.empty() ? 0 : totalContentHeightPx();
    m.viewportHeightPx = juce::jmax(0, getHeight() - kArrangementTimelineHeaderGutterPx);
    m.offsetPx = juce::jlimit(0, m.maxOffsetPx(), verticalScrollOffsetPx_);
    return m;
}

void TrackLanesView::scrollVerticallyToOffsetPx(const int offsetPx) noexcept
{
    if (visibleTrackEntries_.empty())
    {
        return;
    }
    const int clamped = juce::jlimit(0, maxVerticalScrollOffsetPx(), offsetPx);
    if (clamped == verticalScrollOffsetPx_)
    {
        return;
    }
    setVerticalScrollOffsetPx(clamped);
}

void TrackLanesView::setOnVerticalScrollModelChanged(std::function<void()> fn) noexcept
{
    onVerticalScrollModelChanged_ = std::move(fn);
}

void TrackLanesView::publishVerticalScrollModelIfChanged() noexcept
{
    const VerticalScrollModel now = verticalScrollModel();
    if (now == lastPublishedVerticalScrollModel_)
    {
        return;
    }
    lastPublishedVerticalScrollModel_ = now;
    if (onVerticalScrollModelChanged_ != nullptr)
    {
        onVerticalScrollModelChanged_();
    }
}

void TrackLanesView::paint(juce::Graphics& g)
{
    MINIDAW_UI_PAINT_COUNT(lanesViewPaints);
    const auto bounds = getLocalBounds();
    if (bounds.isEmpty())
    {
        return;
    }

    const auto laneBg = juce::Colour(kArrangementLaneBackgroundArgb);
    g.fillAll(laneBg);

    constexpr int gutter = kArrangementTimelineHeaderGutterPx;
    const int ay = bounds.getY();
    const int headerW = juce::jmin(headerColumnWidthPx(), bounds.getWidth());
    const int hx = bounds.getX() + headerW;
    const int gutterBottom = ay + gutter;
    const auto laneSepColour
        = juce::Colour(kArrangementSeparatorArgb).withAlpha(kArrangementSeparatorAlphaHorizontal);

    if (headerW > 0 && gutterBottom > ay && gutterBottom < bounds.getBottom())
    {
        g.setColour(laneBg);
        g.fillRect(bounds.getX(), ay, headerW, juce::jmin(gutter, bounds.getHeight()));
    }

    const int tw = juce::jmax(0, bounds.getWidth() - headerW);
    const std::int64_t arrLen = session_.getArrangementExtentSamples();
    const double spp = timelineViewport_.getSamplesPerPixel();
    if (spp > 0.0 && tw > 0 && hx < bounds.getRight()
        && gutterBottom < bounds.getBottom())
    {
        const std::int64_t visStart = timelineViewport_.getVisibleStartSamples();
        const std::int64_t visLen = timelineViewport_.getVisibleLengthSamples((double)tw);
        const float timelineOriginX = (float)hx;
        const auto sampleToX = [&](const std::int64_t s) {
            return TimelineRulerView::sessionSampleToLocalX(s, timelineOriginX, visStart, spp);
        };
        const juce::Rectangle<float> gridBounds(
            (float)hx,
            (float)gutterBottom,
            (float)tw,
            (float)(bounds.getBottom() - gutterBottom));
        // Dirty-region cull (zoom-freeze fix): playhead stripe underpaints repaint this view
        // ~60×/s during playback with a clip region a few px wide — the grid loop then covers
        // only that window instead of every visible grid line.
        const juce::Rectangle<int> dirtyPx = g.getClipBounds();
        const std::int64_t cullGridStart
            = visStart + (std::int64_t)std::llround(((double)dirtyPx.getX() - 2.0 - (double)hx) * spp);
        const std::int64_t cullGridEnd
            = visStart
              + (std::int64_t)std::llround(((double)dirtyPx.getRight() + 2.0 - (double)hx) * spp);
        timeline_locator_paint::paintArrangementMusicalVerticalGrid(
            g,
            gridBounds,
            sampleToX,
            arrLen,
            visStart,
            visLen,
            effectiveDisplaySampleRate(deviceManager_),
            spp,
            session_.getProjectMusicalTime(),
            cullGridStart,
            cullGridEnd);
    }

    const int vr = static_cast<int>(visibleTrackEntries_.size());
    if (vr <= 0)
    {
        return;
    }

    // Collapsed visual groups: paint the 4 px/member mini strips (grey clip-interval overview)
    // before the separators so a group's strips read as one gapless block.
    paintCollapsedGroupContent(g);

    int yLine = ay + gutter - verticalScrollOffsetPx_;
    for (int i = 0; i < vr; ++i)
    {
        yLine += rowHeightForVisibleEntry(i);
        if (yLine <= bounds.getY() || yLine >= bounds.getBottom())
        {
            continue;
        }
        if (suppressSeparatorBelowVisibleIndex(i))
        {
            // Zero separator between two mini strips of the same collapsed group (spec §4).
            continue;
        }

        if (hx < bounds.getRight())
        {
            g.setColour(laneSepColour);
            g.drawHorizontalLine(yLine, (float)hx, (float)bounds.getRight());
        }
    }
}

bool TrackLanesView::trackHeaderModelUsesSubtitle(const TrackId tid) const noexcept
{
    if (tid == kInvalidTrackId)
    {
        return false;
    }
    const auto it = instrumentTimelineAttachments_.find(tid);
    if (it == instrumentTimelineAttachments_.end())
    {
        return false;
    }
    InstrumentTrackController* const ctl = it->second.controller;
    return ctl != nullptr && ctl->getLaneHeaderSubtitle().isNotEmpty();
}

int TrackLanesView::minimumRowHeightPxForTrackHeader(const TrackId tid) const noexcept
{
    juce::ignoreUnused(tid);
    // The minimum individual height equals the shared Micro preset for EVERY row kind: one title
    // row (type icon, number, Power / Mute / Solo, name) + the resize band
    // (`ui/TrackRowHeightPresets.h`). Collapsed-group 4 px strips are a separate display mechanism.
    return track_row_heights::kMinRowHeightPx;
}

int TrackLanesView::rowHeightForTrack(const TrackId tid) const noexcept
{
    namespace trh = track_row_heights;
    if (tid == kInvalidTrackId)
    {
        return trh::clampRowHeightPx(defaultRowHeightPx_);
    }
    auto it = perTrackRowHeightPx_.find(tid);
    const int h = (it != perTrackRowHeightPx_.end()) ? it->second : defaultRowHeightPx_;
    return trh::clampRowHeightPx(h);
}

int TrackLanesView::rowHeightForVisibleEntry(const int visibleIndex) const noexcept
{
    if (visibleIndex < 0 || visibleIndex >= static_cast<int>(visibleTrackEntries_.size()))
    {
        return 0;
    }
    // DISPLAY height: a member of a collapsed visual group shows as a 4 px mini strip. The
    // STORED normal height (`rowHeightForTrack`, used by save / preset status / duplicate /
    // resize) is deliberately untouched — collapse never rewrites a row height.
    const TrackId tid = visibleTrackEntries_[(size_t)visibleIndex].sessionTrackId;
    if (isTrackInCollapsedVisualGroup(tid))
    {
        return kCollapsedGroupMemberRowHeightPx;
    }
    return rowHeightForTrack(tid);
}

int TrackLanesView::visibleRowPixelHeight(const int visibleIndex) const noexcept
{
    return rowHeightForVisibleEntry(visibleIndex);
}

int TrackLanesView::totalContentHeightPx() const noexcept
{
    int sum = 0;
    const int vr = static_cast<int>(visibleTrackEntries_.size());
    for (int i = 0; i < vr; ++i)
    {
        sum += rowHeightForVisibleEntry(i);
    }
    return sum;
}

void TrackLanesView::applyTrackRowHeightDelta(const TrackId tid,
                                              const int startHeightPx,
                                              const int deltaPx) noexcept
{
    if (tid == kInvalidTrackId)
    {
        return;
    }
    // Snap during the drag from the ORIGINAL height + the total pointer movement: a pure function
    // of the pointer position, so the row cannot flutter between two grid heights at a boundary.
    setTrackRowHeightPx(tid, track_row_heights::snapRowHeightPxToGrid(startHeightPx + deltaPx));
}

void TrackLanesView::setTrackRowHeightPx(const TrackId tid, const int heightPx) noexcept
{
    if (tid == kInvalidTrackId)
    {
        return;
    }
    const int before = rowHeightForTrack(tid);
    const int lo = minimumRowHeightPxForTrackHeader(tid);
    const int nh = juce::jlimit(lo, track_row_heights::kRowHeightSafetyMaxPx, heightPx);
    if (nh == defaultRowHeightPx_)
    {
        perTrackRowHeightPx_.erase(tid);
    }
    else
    {
        perTrackRowHeightPx_[tid] = nh;
    }
    resized();
    repaint();
    if (nh != before)
    {
        // Every caller of this setter is a user-origin edit (drag / stability click-path); the
        // project-load apply uses `applyTrackRowHeightsFromLoadedProject` and never lands here.
        notifyTrackRowHeightsChanged(true);
    }
}

void TrackLanesView::notifyTrackRowHeightsChanged(const bool byUserEdit) noexcept
{
    if (onTrackRowHeightsChanged_)
    {
        onTrackRowHeightsChanged_(byUserEdit);
    }
}

TrackId TrackLanesView::topVisibleTrackIdForCurrentOffset() const noexcept
{
    int acc = 0;
    for (int vi = 0; vi < (int) visibleTrackEntries_.size(); ++vi)
    {
        const int h = rowHeightForVisibleEntry(vi); // display height (4 px collapsed strips)
        if (verticalScrollOffsetPx_ < acc + h)
        {
            return visibleTrackEntries_[(size_t) vi].sessionTrackId;
        }
        acc += h;
    }
    return visibleTrackEntries_.empty() ? kInvalidTrackId
                                        : visibleTrackEntries_.back().sessionTrackId;
}

void TrackLanesView::applyTrackRowHeightPreset(
    const track_row_heights::TrackRowHeightPreset preset) noexcept
{
    namespace trh = track_row_heights;
    // Capture the topmost visible track with the OLD heights before anything changes.
    const TrackId keepTopTid = topVisibleTrackIdForCurrentOffset();
    lastChosenRowHeightPreset_ = preset;
    defaultRowHeightPx_ = trh::heightPxForPreset(preset);
    perTrackRowHeightPx_.clear();
    // ONE gathered layout pass for every row (incl. scrolled-out ones) — `resized()` lays the
    // whole stack out from the shared height model; no per-track relayouts.
    resized();
    if (keepTopTid != kInvalidTrackId)
    {
        // Preserve the previously topmost visible track; the same clamp path as the wheel keeps
        // the offset valid when the content shrank below the viewport.
        scrollVerticallyToOffsetPx(rowTopOffsetPxForTrackForDiagnostics(keepTopTid));
    }
    repaint();
    notifyTrackRowHeightsChanged(true);
}

std::optional<track_row_heights::TrackRowHeightPreset>
TrackLanesView::uniformTrackRowHeightPresetStatus() const noexcept
{
    namespace trh = track_row_heights;
    if (visibleTrackEntries_.empty())
    {
        return trh::presetMatchingHeightPx(trh::clampRowHeightPx(defaultRowHeightPx_));
    }
    const int first = rowHeightForTrack(visibleTrackEntries_.front().sessionTrackId);
    for (const auto& e : visibleTrackEntries_)
    {
        if (rowHeightForTrack(e.sessionTrackId) != first)
        {
            return std::nullopt;
        }
    }
    return trh::presetMatchingHeightPx(first);
}

std::vector<std::pair<TrackId, int>> TrackLanesView::allTrackRowHeightsPxForProjectSave() const
{
    std::vector<std::pair<TrackId, int>> out;
    const int n = session_.getNumTracks();
    out.reserve(static_cast<std::size_t>(juce::jmax(0, n)));
    for (int i = 0; i < n; ++i)
    {
        const TrackId tid = session_.getTrackIdAtIndex(i);
        if (tid != kInvalidTrackId)
        {
            out.emplace_back(tid, rowHeightForTrack(tid));
        }
    }
    return out;
}

void TrackLanesView::applyTrackRowHeightsFromLoadedProject(
    const juce::String& presetKey, const std::vector<std::pair<TrackId, int>>& perTrackPx) noexcept
{
    namespace trh = track_row_heights;
    lastChosenRowHeightPreset_ = trh::presetFromPersistenceKey(presetKey);
    defaultRowHeightPx_ = trh::heightPxForPreset(lastChosenRowHeightPreset_);
    perTrackRowHeightPx_.clear();
    for (const auto& [tid, px] : perTrackPx)
    {
        if (tid == kInvalidTrackId || px <= 0)
        {
            continue; // absent / malformed height: the row keeps the project default.
        }
        // Clamp to [Micro, safety max] but NEVER re-snap: a valid older off-grid height (e.g. the
        // pre-grid 64 / 96 / 192) stays as saved and reports "Custom" until the user resizes or
        // picks a preset.
        const int clamped = trh::clampRowHeightPx(px);
        if (clamped != defaultRowHeightPx_)
        {
            perTrackRowHeightPx_[tid] = clamped;
        }
    }
    prunePerTrackRowHeightsNotInSession();
    resized();
    repaint();
    notifyTrackRowHeightsChanged(false);
}

void TrackLanesView::copyRowHeightForDuplicatedTrack(const TrackId sourceTid,
                                                     const TrackId newTid) noexcept
{
    if (sourceTid == kInvalidTrackId || newTid == kInvalidTrackId || sourceTid == newTid)
    {
        return;
    }
    const int h = rowHeightForTrack(sourceTid);
    if (h == defaultRowHeightPx_)
    {
        perTrackRowHeightPx_.erase(newTid);
    }
    else
    {
        perTrackRowHeightPx_[newTid] = h;
    }
    // The caller follows with `syncTracksFromSession()` + layout; duplication marks the project
    // dirty through its own undoable step, so no user-edit notification is needed here.
}

void TrackLanesView::snapTrackHeaderRowHeightAfterResize(const TrackId tid,
                                                          const bool headerHasSubtitle) noexcept
{
    if (tid == kInvalidTrackId)
    {
        return;
    }
    juce::ignoreUnused(headerHasSubtitle);
    const int h = rowHeightForTrack(tid);
    const int snapped = track_row_heights::snapRowHeightPxToGrid(h);
    if (snapped != h)
    {
        setTrackRowHeightPx(tid, snapped);
    }
}

// --------------------------------------------------------- header numbers / icons / colours

int TrackLanesView::trackNumberForTrack(const TrackId tid) const noexcept
{
    if (tid == kInvalidTrackId)
    {
        return 0;
    }
    const std::shared_ptr<const SessionSnapshot> snap = session_.loadSessionSnapshotForAudioThread();
    if (snap == nullptr)
    {
        return 0;
    }
    const int idx = snap->findTrackIndexById(tid);
    return idx >= 0 ? idx + 1 : 0;
}

int TrackLanesView::trackNumberDigitCount() const noexcept
{
    int digits = 1;
    for (int n = juce::jmax(1, session_.getNumTracks()); n >= 10; n /= 10)
    {
        ++digits;
    }
    return juce::jmax(TrackHeaderView::kHeaderMinNumberDigits, digits);
}

track_strip_glyphs::TrackTypeIcon TrackLanesView::typeIconForTrackKind(const TrackKind kind) noexcept
{
    using track_strip_glyphs::TrackTypeIcon;
    switch (kind)
    {
    case TrackKind::Instrument:
        return TrackTypeIcon::Instrument;
    case TrackKind::Midi:
        return TrackTypeIcon::Midi;
    case TrackKind::Group:
        return TrackTypeIcon::Group;
    case TrackKind::Master:
        return TrackTypeIcon::Master;
    case TrackKind::Audio:
    default:
        return TrackTypeIcon::Audio;
    }
}

void TrackLanesView::fillCommonHeaderModelFields(TrackHeaderModel& m, const TrackId tid) const noexcept
{
    m.trackNumber = trackNumberForTrack(tid);
    m.trackNumberDigits = trackNumberDigitCount();
    m.colourKey = session_.getTrackColour(tid);
    if (const auto snap = session_.loadSessionSnapshotForAudioThread())
    {
        const int idx = snap->findTrackIndexById(tid);
        if (idx >= 0)
        {
            m.typeIcon = typeIconForTrackKind(snap->getTrack(idx).getKind());
        }
    }
}

void TrackLanesView::showTrackColourMenuForTrack(const TrackId tid, const juce::Rectangle<int> segmentScreenBounds)
{
    if (tid == kInvalidTrackId || onTrackColourRequested_ == nullptr)
    {
        return;
    }
    const TrackColourKey current = session_.getTrackColour(tid);
    juce::PopupMenu menu;
    for (int i = 0; i < kTrackColourKeyCount; ++i)
    {
        const TrackColourKey key = trackColourKeyFromIndex(i);
        juce::PopupMenu::Item item(track_colour_palette::displayName(key));
        item.itemID = kTrackColourMenuBaseId + i;
        item.isTicked = (key == current);
        // Swatch: a small filled square in the exact segment colour, drawn as the item icon.
        auto swatch = std::make_unique<juce::DrawableRectangle>();
        swatch->setRectangle(juce::Parallelogram<float>(juce::Rectangle<float>(0.0f, 0.0f, 14.0f, 14.0f)));
        swatch->setCornerSize(juce::Point<float>(2.0f, 2.0f));
        swatch->setFill(juce::FillType(track_colour_palette::menuSwatch(key)));
        swatch->setStrokeFill(juce::FillType(juce::Colour(0x80000000)));
        swatch->setStrokeThickness(1.0f);
        item.image = std::move(swatch);
        menu.addItem(std::move(item));
    }
    juce::Component::SafePointer<TrackLanesView> safeThis(this);
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetScreenArea(segmentScreenBounds),
                       [safeThis, tid](const int result) {
                           if (safeThis == nullptr || result < kTrackColourMenuBaseId
                               || result >= kTrackColourMenuBaseId + kTrackColourKeyCount
                               || safeThis->onTrackColourRequested_ == nullptr)
                           {
                               return;
                           }
                           // The command targets the right-clicked track only — never the header
                           // multi-selection, never the active track.
                           safeThis->onTrackColourRequested_(tid, trackColourKeyFromIndex(result - kTrackColourMenuBaseId));
                       });
}

void TrackLanesView::refreshTrackColoursFromSession() noexcept
{
    // Headers and lanes read `Session::getTrackColour` when they paint; a full repaint of this
    // (buffered) view invalidates every child, and the audio lanes rebuild their wave raster on
    // the next paint because the raster remembers the colour it was built with.
    for (auto& h : headers_)
    {
        if (h != nullptr)
        {
            h->repaint();
        }
    }
    for (auto& l : lanes_)
    {
        if (l != nullptr)
        {
            l->repaint();
        }
    }
    for (auto& kv : groupHeaders_)
    {
        if (kv.second != nullptr)
        {
            kv.second->repaint();
        }
    }
    for (auto& mh : masterHeaders_)
    {
        if (mh != nullptr)
        {
            mh->repaint();
        }
    }
    for (auto& kv : instrumentTimelineAttachments_)
    {
        if (kv.second.header != nullptr)
        {
            kv.second.header->repaint();
        }
        if (kv.second.midiLane != nullptr)
        {
            kv.second.midiLane->repaint();
        }
    }
    repaint();
}

void TrackLanesView::prunePerTrackRowHeightsNotInSession() noexcept
{
    std::unordered_set<TrackId> alive;
    const int n = session_.getNumTracks();
    for (int i = 0; i < n; ++i)
    {
        const TrackId tid = session_.getTrackIdAtIndex(i);
        if (tid != kInvalidTrackId)
        {
            alive.insert(tid);
        }
    }

    for (auto it = perTrackRowHeightPx_.begin(); it != perTrackRowHeightPx_.end();)
    {
        if (alive.count(it->first) == 0)
        {
            it = perTrackRowHeightPx_.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

int TrackLanesView::audioLaneIndexFromTrackId(const TrackId tid) const noexcept
{
    if (tid == kInvalidTrackId)
    {
        return -1;
    }
    int audioIx = -1;
    const int n = session_.getNumTracks();
    for (int i = 0; i < n; ++i)
    {
        if (session_.getTrackKindAtIndex(i) != TrackKind::Audio)
        {
            continue;
        }
        ++audioIx;
        if (session_.getTrackIdAtIndex(i) == tid)
        {
            return audioIx;
        }
    }
    return -1;
}

int TrackLanesView::maxVerticalScrollOffsetPx() const noexcept
{
    const auto area = getLocalBounds();
    const int vh = juce::jmax(0, area.getHeight() - kArrangementTimelineHeaderGutterPx);
    return juce::jmax(0, totalContentHeightPx() - vh);
}

void TrackLanesView::setVerticalScrollOffsetPx(const int newOffset) noexcept
{
    verticalScrollOffsetPx_ = newOffset;
    resized();
}

int TrackLanesView::findVisibleRowIndexForDragSource(const TrackId movedId) const noexcept
{
    for (int i = 0; i < static_cast<int>(visibleTrackEntries_.size()); ++i)
    {
        if (visibleTrackEntries_[(size_t)i].sessionTrackId == movedId)
        {
            return i;
        }
    }
    return -1;
}



void TrackLanesView::beginHeaderTrackDrag(const TrackId movedId, TrackHeaderView& sourceView)
{
    headerTrackDragActive_ = true;
    headerTrackDragId_ = movedId;
    headerTrackDragSourceView_ = &sourceView;
    headerTrackDragInsertGapK_ = -1;
    headerTrackDragNoopLineY_ = -1;
    headerTrackDragInvalidArea_ = true;
    headerTrackDragNoop_ = true;
}

void TrackLanesView::updateHeaderTrackDrag(const TrackId movedId, const juce::Point<int> screenPos)
{
    if (!headerTrackDragActive_ || movedId != headerTrackDragId_)
    {
        return;
    }
    const juce::Point<int> local = getLocalPoint(nullptr, screenPos);
    if (!getLocalBounds().contains(local) || local.x >= headerColumnWidthPx())
    {
        headerTrackDragInvalidArea_ = true;
        headerTrackDragInsertGapK_ = -1;
        headerTrackDragNoopLineY_ = -1;
        headerTrackDragNoop_ = true;
        if (headerTrackDragSourceView_ != nullptr)
        {
            headerTrackDragSourceView_->setSourceForbiddenForHeaderDrag();
        }
        repaint();
        return;
    }

    headerTrackDragInvalidArea_ = false;
    if (headerTrackDragSourceView_ != nullptr)
    {
        headerTrackDragSourceView_->restoreSourceCursorAfterHeaderDrag();
    }

    const int vr = static_cast<int>(visibleTrackEntries_.size());
    if (vr <= 0)
    {
        return;
    }

    {
        const std::shared_ptr<const SessionSnapshot> snap = session_.loadSessionSnapshotForAudioThread();
        if (snap == nullptr || snap->findTrackIndexById(movedId) < 0)
        {
            headerTrackDragInvalidArea_ = true;
            headerTrackDragInsertGapK_ = -1;
            headerTrackDragNoopLineY_ = -1;
            headerTrackDragNoop_ = true;
            if (headerTrackDragSourceView_ != nullptr)
            {
                headerTrackDragSourceView_->setSourceForbiddenForHeaderDrag();
            }
            repaint();
            return;
        }
    }

    const int sv = findVisibleRowIndexForDragSource(movedId);
    if (sv < 0)
    {
        headerTrackDragInvalidArea_ = true;
        headerTrackDragInsertGapK_ = -1;
        headerTrackDragNoopLineY_ = -1;
        headerTrackDragNoop_ = true;
        if (headerTrackDragSourceView_ != nullptr)
        {
            headerTrackDragSourceView_->setSourceForbiddenForHeaderDrag();
        }
        repaint();
        return;
    }

    const int ay = getLocalBounds().getY();
    std::vector<int> gapY((size_t)vr + 1u);
    int accY = ay + kArrangementTimelineHeaderGutterPx - verticalScrollOffsetPx_;
    for (int k = 0; k <= vr; ++k)
    {
        gapY[(size_t)k] = accY;
        if (k < vr)
        {
            accY += rowHeightForVisibleEntry(k);
        }
    }

    int bestK = 0;
    int bestAbs = 0x7fffffff;
    for (int k = 0; k <= vr; ++k)
    {
        const int d = local.y - gapY[(size_t)k];
        const int a = d < 0 ? -d : d;
        if (a < bestAbs)
        {
            bestAbs = a;
            bestK = k;
        }
    }

    const int destVis = bestK <= sv ? bestK : (bestK - 1);
    // Visual track groups (spec §6): a reorder that would split a displayable group's contiguity
    // (or drop an outside track into its middle) is refused — shown as the red no-op line while
    // dragging; the drop explains with the group's name (`endHeaderTrackDrag`).
    const bool violatesGroup
        = destVis != sv && session_.checkTrackMoveAgainstVisualGroups(movedId, destVis).has_value();
    const bool noop = (destVis == sv) || violatesGroup;
    headerTrackDragNoop_ = noop;
    if (noop)
    {
        // Red: line tracks pointer (valid header column only). Green uses snapped gaps above.
        headerTrackDragInsertGapK_ = -1;
        const int h = getHeight();
        headerTrackDragNoopLineY_ = (h > 0) ? juce::jlimit(0, h - 1, local.y) : 0;
    }
    else
    {
        headerTrackDragNoopLineY_ = -1;
        headerTrackDragInsertGapK_ = bestK;
    }
    repaint();
}

void TrackLanesView::endHeaderTrackDrag(const TrackId movedId)
{
    if (movedId != headerTrackDragId_ || !headerTrackDragActive_)
    {
        return;
    }
    TrackHeaderView* const src = headerTrackDragSourceView_;
    if (src != nullptr)
    {
        src->restoreSourceCursorAfterHeaderDrag();
    }

    const int sv = findVisibleRowIndexForDragSource(movedId);
    const bool commit = (!headerTrackDragInvalidArea_) && !headerTrackDragNoop_
        && (headerTrackDragInsertGapK_ >= 0) && (sv >= 0);
    if (commit)
    {
        const int k = headerTrackDragInsertGapK_;
        const int destSessionIndex = (k <= sv) ? k : (k - 1);
        // Visual track groups (spec §6): refuse clearly instead of splitting a group. The drag
        // already showed this as a red no-op line; this is the final guard at the drop (the
        // session-side `moveTrack` veto backs it up for every other call site).
        if (const std::optional<juce::String> violatedGroupName
            = session_.checkTrackMoveAgainstVisualGroups(movedId, destSessionIndex);
            violatedGroupName.has_value())
        {
            juce::AlertWindow::showMessageBoxAsync(
                juce::AlertWindow::InfoIcon,
                "Move track",
                "This move would split the collapsible group \"" + *violatedGroupName
                    + "\". Ungroup it first (right-click the group handle) to move the track here.");
        }
        else if (committedHeaderDragTrackReorder_ != nullptr)
        {
            committedHeaderDragTrackReorder_(movedId, destSessionIndex);
        }
        const int tw = juce::jmax(0, getWidth() - headerColumnWidthPx());
        if (tw > 0)
        {
            timelineViewport_.clampToExtent((double)tw, session_.getArrangementExtentSamples());
        }
    }

    clearHeaderTrackDragState();
    repaint();
}

void TrackLanesView::cancelHeaderDragIfSourceIs(const TrackHeaderView* const header) noexcept
{
    if (header != nullptr && headerTrackDragSourceView_ == header)
    {
        clearHeaderTrackDragState();
        repaint();
    }
}

void TrackLanesView::clearHeaderTrackDragState() noexcept
{
    headerTrackDragActive_ = false;
    headerTrackDragId_ = kInvalidTrackId;
    headerTrackDragSourceView_ = nullptr;
    headerTrackDragInsertGapK_ = -1;
    headerTrackDragNoopLineY_ = -1;
    headerTrackDragInvalidArea_ = true;
    headerTrackDragNoop_ = true;
}

void TrackLanesView::paintHeaderColumnHorizontalRowSeparators(juce::Graphics& g) const noexcept
{
    const auto bounds = getLocalBounds();
    if (bounds.isEmpty())
    {
        return;
    }

    const int headerW = juce::jmin(headerColumnWidthPx(), bounds.getWidth());
    const int hx = bounds.getX() + headerW;
    if (hx <= bounds.getX())
    {
        return;
    }

    const int ay = bounds.getY();
    constexpr int gutter = kArrangementTimelineHeaderGutterPx;
    const int gutterBottom = ay + gutter;

    g.setColour(juce::Colour(kArrangementHeaderRowSeparatorArgb));
    if (gutterBottom > ay && gutterBottom < bounds.getBottom())
    {
        g.drawHorizontalLine(gutterBottom, (float)bounds.getX(), (float)hx);
    }

    const int vr = static_cast<int>(visibleTrackEntries_.size());
    if (vr <= 0)
    {
        return;
    }

    int yLine = ay + gutter - verticalScrollOffsetPx_;
    for (int i = 0; i < vr; ++i)
    {
        yLine += rowHeightForVisibleEntry(i);
        if (yLine <= bounds.getY() || yLine >= bounds.getBottom())
        {
            continue;
        }
        if (suppressSeparatorBelowVisibleIndex(i))
        {
            continue; // gapless mini strips of one collapsed group (spec §4)
        }

        g.drawHorizontalLine(yLine, (float)bounds.getX(), (float)hx);
    }
}

int TrackLanesView::yForVisibleInsertGapK(const int k) const noexcept
{
    const int vr = static_cast<int>(visibleTrackEntries_.size());
    if (k < 0 || k > vr)
    {
        return 0;
    }
    const int ay = getLocalBounds().getY();
    int y = ay + kArrangementTimelineHeaderGutterPx - verticalScrollOffsetPx_;
    for (int i = 0; i < k; ++i)
    {
        y += rowHeightForVisibleEntry(i);
    }
    return y;
}

void TrackLanesView::paintOverChildren(juce::Graphics& g)
{
    const auto bounds = getLocalBounds();
    if (!bounds.isEmpty())
    {
        const int headerW = juce::jmin(headerColumnWidthPx(), bounds.getWidth());
        if (headerW > 0 && headerW < bounds.getWidth())
        {
            const float vx = (float)(bounds.getX() + headerW) - 0.5f;
            g.setColour(
                juce::Colour(kArrangementSeparatorArgb).withAlpha(kArrangementSeparatorAlphaVertical));
            g.drawLine(vx, (float)bounds.getY(), vx, (float)bounds.getBottom(), 1.0f);

            constexpr int gutter = kArrangementTimelineHeaderGutterPx;
            const int gutterBottom = bounds.getY() + gutter;
            const int hx = bounds.getX() + headerW;
            if (hx < bounds.getRight() && gutterBottom > bounds.getY()
                && gutterBottom < bounds.getBottom())
            {
                g.setColour(juce::Colour(kArrangementSeparatorArgb)
                                .withAlpha(kArrangementSeparatorAlphaHorizontal));
                g.drawHorizontalLine(gutterBottom, (float)hx, (float)bounds.getRight());
            }
        }

        paintHeaderColumnHorizontalRowSeparators(g);
    }

    if (!headerTrackDragActive_ || headerTrackDragInvalidArea_)
    {
        return;
    }
    const int h = getHeight();
    if (h <= 0)
    {
        return;
    }
    int yy = 0;
    if (headerTrackDragNoop_)
    {
        if (headerTrackDragNoopLineY_ < 0)
        {
            return;
        }
        g.setColour(juce::Colour(0xffc04040));
        yy = juce::jlimit(0, juce::jmax(0, h - 2), headerTrackDragNoopLineY_ - 1);
    }
    else
    {
        if (headerTrackDragInsertGapK_ < 0)
        {
            return;
        }
        g.setColour(juce::Colour(0xff40c040));
        const int y = yForVisibleInsertGapK(headerTrackDragInsertGapK_);
        yy = juce::jlimit(0, juce::jmax(0, h - 2), y - 1);
    }
    const int lineW = juce::jmin(headerColumnWidthPx(), getWidth());
    g.fillRect(0, yy, lineW, 2);
}

void TrackLanesView::MiddlePanMouseListener::mouseDown(const juce::MouseEvent& e)
{
    if (!e.mods.isMiddleButtonDown())
    {
        return;
    }
    owner_.beginMiddlePan(e.getEventRelativeTo(&owner_).position.x);
}

void TrackLanesView::MiddlePanMouseListener::mouseDrag(const juce::MouseEvent& e)
{
    if (!owner_.middlePanActive_ || !e.mods.isMiddleButtonDown())
    {
        return;
    }
    owner_.updateMiddlePan(e.getEventRelativeTo(&owner_).position.x);
}

void TrackLanesView::MiddlePanMouseListener::mouseUp(const juce::MouseEvent& e)
{
    juce::ignoreUnused(e);
    owner_.endMiddlePan();
}

void TrackLanesView::beginMiddlePan(const float xInLanes) noexcept
{
    for (const auto& lane : lanes_)
    {
        if (lane != nullptr && lane->isTimelineEditGestureInProgress())
        {
            return;
        }
    }
    middlePanActive_ = true;
    middlePanLastX_ = xInLanes;
}

void TrackLanesView::updateMiddlePan(const float xInLanes) noexcept
{
    const std::int64_t arr = session_.getArrangementExtentSamples();
    if (arr <= 0)
    {
        return;
    }
    const double spp = timelineViewport_.getSamplesPerPixel();
    if (spp <= 0.0)
    {
        return;
    }
    const int tw = juce::jmax(0, getWidth() - headerColumnWidthPx());
    if (tw <= 0)
    {
        return;
    }
    const float dx = xInLanes - middlePanLastX_;
    // Grab-style pan: content follows the mouse, so dragging right shows earlier time.
    const std::int64_t step = -(std::int64_t)std::llround((double)dx * spp);
    if (step == 0)
    {
        // Keep the anchor so sub-pixel movement accumulates instead of getting lost at deep zoom-in.
        return;
    }
    middlePanLastX_ = xInLanes;
    // No direct repaint: the viewport listener repaints ruler+lanes via a coalesced flush
    // (one dirty-marking per message batch — repaint-storm fix, see CoalescedRepaintFlusher).
    timelineViewport_.panBySamples(step, (double)tw, arr);
}

void TrackLanesView::endMiddlePan() noexcept
{
    middlePanActive_ = false;
}

void TrackLanesView::mouseWheelMove(
    const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    for (const auto& lane : lanes_)
    {
        if (lane != nullptr && lane->isTimelineEditGestureInProgress())
        {
            return;
        }
    }
    const double d = (wheel.isReversed ? -wheel.deltaY : wheel.deltaY);
    if (d == 0.0)
    {
        return;
    }
    if (e.mods.isCtrlDown())
    {
        const std::int64_t arr = session_.getArrangementExtentSamples();
        if (arr <= 0)
        {
            return;
        }
        const double spp = timelineViewport_.getSamplesPerPixel();
        if (spp <= 0.0)
        {
            return;
        }
        const int headerW = headerColumnWidthPx();
        if (e.position.x < (float)headerW)
        {
            return;
        }
        const int tw = juce::jmax(0, getWidth() - headerW);
        if (tw <= 0)
        {
            return;
        }
        const double w = (double)tw;
        const double x = (double)e.position.x - (double)headerW;
        const double factor = std::pow(0.85, d);
        const double sppMax
            = juce::jmax(1.0, (double)juce::jmax(std::int64_t{1}, arr) / w);
        // No direct repaint: coalesced via the viewport listener (repaint-storm fix).
        timelineViewport_.zoomAroundSample(factor, x, w, arr, kSppMin, sppMax);
        return;
    }
    if (e.mods.isShiftDown())
    {
        const std::int64_t arr = session_.getArrangementExtentSamples();
        if (arr <= 0)
        {
            return;
        }
        const double spp = timelineViewport_.getSamplesPerPixel();
        if (spp <= 0.0)
        {
            return;
        }
        const int twPan = juce::jmax(0, getWidth() - headerColumnWidthPx());
        if (twPan <= 0)
        {
            return;
        }
        const double wPan = (double)twPan;
        const double panNotchPx = juce::jmax(1.0, wPan / 8.0);
        const double panD = -d;
        const std::int64_t step = (panD > 0.0) ? (std::int64_t)std::llround(panNotchPx * spp)
                                                : -((std::int64_t)std::llround(panNotchPx * spp));
        if (step == 0)
        {
            return;
        }
        // No direct repaint: coalesced via the viewport listener (repaint-storm fix).
        timelineViewport_.panBySamples(step, wPan, arr);
        return;
    }

    if (visibleTrackEntries_.empty())
    {
        return;
    }
    const int deltaPx = (int)std::llround(-d * (double)defaultRowHeightPx_ * 0.5);
    if (deltaPx == 0)
    {
        return;
    }
    setVerticalScrollOffsetPx(verticalScrollOffsetPx_ + deltaPx);
}

void TrackLanesView::notifyPlacedClipRemoved(const TrackId trackId, const PlacedClipId clipId) noexcept
{
    if (aggregatedSelectedPlacedClip_.has_value()
        && aggregatedSelectedPlacedClip_->first == trackId
        && aggregatedSelectedPlacedClip_->second == clipId)
    {
        aggregatedSelectedPlacedClip_.reset();
    }
    for (auto& u : lanes_)
    {
        if (u != nullptr && u->getTrackId() == trackId)
        {
            u->clearSelectionOnly();
            break;
        }
    }
}

void TrackLanesView::onLanePlacedClipSelectionChanged(const TrackId laneTrackId,
                                                      const std::optional<PlacedClipId> id) noexcept
{
    if (id.has_value())
    {
        aggregatedSelectedPlacedClip_ = std::pair<TrackId, PlacedClipId>(laneTrackId, *id);
    }
    else if (aggregatedSelectedPlacedClip_.has_value()
             && aggregatedSelectedPlacedClip_->first == laneTrackId)
    {
        aggregatedSelectedPlacedClip_.reset();
    }
}

std::optional<std::pair<TrackId, PlacedClipId>> TrackLanesView::getAggregatedSelectedClip()
    const noexcept
{
    return aggregatedSelectedPlacedClip_;
}

std::optional<std::pair<TrackId, std::vector<InstrumentMidiClipId>>>
TrackLanesView::getAggregatedSelectedInstrumentMidiClipSelection() const noexcept
{
    const auto pickForTrack = [this](const TrackId tid)
        -> std::optional<std::pair<TrackId, std::vector<InstrumentMidiClipId>>> {
        if (tid == kInvalidTrackId)
        {
            return std::nullopt;
        }
        const auto it = instrumentTimelineAttachments_.find(tid);
        if (it == instrumentTimelineAttachments_.end() || it->second.controller == nullptr)
        {
            return std::nullopt;
        }
        const auto& sel = it->second.controller->getSelectedClipIds();
        if (sel.empty())
        {
            return std::nullopt;
        }
        return std::pair<TrackId, std::vector<InstrumentMidiClipId>>(tid,
                                                                     std::vector<InstrumentMidiClipId>(
                                                                         sel.begin(),
                                                                         sel.end()));
    };

    const TrackId active = session_.getActiveTrackId();
    if (auto fromActive = pickForTrack(active))
    {
        return fromActive;
    }
    for (const auto& kv : instrumentTimelineAttachments_)
    {
        if (auto r = pickForTrack(kv.first))
        {
            return r;
        }
    }
    return std::nullopt;
}

void TrackLanesView::selectFrontPlacedClipOnTrack(const TrackId tid) noexcept
{
    if (tid == kInvalidTrackId)
    {
        return;
    }
    const std::shared_ptr<const SessionSnapshot> snap = session_.loadSessionSnapshotForAudioThread();
    if (snap == nullptr)
    {
        return;
    }
    const int tIdx = snap->findTrackIndexById(tid);
    if (tIdx < 0)
    {
        return;
    }
    const Track& tr = snap->getTrack(tIdx);
    if (tr.getNumPlacedClips() <= 0)
    {
        return;
    }
    const PlacedClipId pid = tr.getPlacedClip(0).getId();
    for (auto& u : lanes_)
    {
        if (u != nullptr && u->getTrackId() != tid)
        {
            u->clearSelectionOnly();
        }
    }
    for (auto& u : lanes_)
    {
        if (u != nullptr && u->getTrackId() == tid)
        {
            u->applyExternalPlacedClipSelection(pid);
            break;
        }
    }
}

void TrackLanesView::selectPlacedClipOnTrack(const TrackId tid, const PlacedClipId clipId) noexcept
{
    if (tid == kInvalidTrackId || clipId == kInvalidPlacedClipId)
    {
        return;
    }
    const std::shared_ptr<const SessionSnapshot> snap = session_.loadSessionSnapshotForAudioThread();
    if (snap == nullptr)
    {
        return;
    }
    const int tIdx = snap->findTrackIndexById(tid);
    if (tIdx < 0)
    {
        return;
    }
    const Track& tr = snap->getTrack(tIdx);
    bool found = false;
    for (int i = 0; i < tr.getNumPlacedClips(); ++i)
    {
        if (tr.getPlacedClip(i).getId() == clipId)
        {
            found = true;
            break;
        }
    }
    if (!found)
    {
        return;
    }
    for (auto& u : lanes_)
    {
        if (u != nullptr && u->getTrackId() != tid)
        {
            u->clearSelectionOnly();
        }
    }
    for (auto& u : lanes_)
    {
        if (u != nullptr && u->getTrackId() == tid)
        {
            u->applyExternalPlacedClipSelection(clipId);
            break;
        }
    }
}

// =============================================================================
// Visual track groups (collapsible, purely visual) + header multi-selection
// =============================================================================

void TrackLanesView::setVisualTrackGroupUiHooks(VisualTrackGroupUiHooks hooks) noexcept
{
    visualGroupUiHooks_ = std::move(hooks);
}

void TrackLanesView::refreshVisualTrackGroupsFromSession() noexcept
{
    rebuildVisibleTrackEntries(); // also rebuilds the group display cache + handles
    resized();
    repaint();
}

void TrackLanesView::rebuildVisualGroupDisplayCache()
{
    visualGroupMembershipByTrackId_.clear();
    visualGroupDisplayRuns_.clear();
    for (const VisualTrackGroup& g : session_.getVisualTrackGroups())
    {
        if (!session_.isVisualTrackGroupDisplayable(g.id))
        {
            continue; // Safe fallback: members render as normal tracks.
        }
        const std::vector<TrackId> members = session_.getEffectiveVisualGroupMemberTrackIds(g.id);
        int firstVi = -1;
        int lastVi = -1;
        bool allVisible = true;
        for (const TrackId tid : members)
        {
            const int vi = visibleRowIndexForTrackForDiagnostics(tid);
            if (vi < 0)
            {
                allVisible = false; // e.g. an instrument row without a UI attachment yet
                break;
            }
            firstVi = (firstVi < 0) ? vi : juce::jmin(firstVi, vi);
            lastVi = juce::jmax(lastVi, vi);
        }
        // The run must map to one contiguous block of VISIBLE rows too; otherwise skip it for
        // this layout pass (no marker/handle/collapse) without touching any data.
        if (!allVisible || firstVi < 0 || lastVi - firstVi + 1 != (int) members.size())
        {
            continue;
        }
        VisualGroupDisplayRun run;
        run.groupId = g.id;
        run.name = g.name;
        run.collapsed = g.collapsed;
        run.firstVisibleIndex = firstVi;
        run.lastVisibleIndex = lastVi;
        visualGroupDisplayRuns_.push_back(std::move(run));
        for (const TrackId tid : members)
        {
            visualGroupMembershipByTrackId_[tid] = VisualGroupMembershipCacheEntry{ g.id, g.collapsed };
        }
    }
    rebuildVisualGroupHandles();
}

void TrackLanesView::rebuildVisualGroupHandles()
{
    // Drop handles of groups that are no longer displayable…
    for (auto it = visualGroupHandles_.begin(); it != visualGroupHandles_.end();)
    {
        if (findVisualGroupRun(it->first) == nullptr)
        {
            it = visualGroupHandles_.erase(it);
        }
        else
        {
            ++it;
        }
    }
    // …and ensure one per current run, with up-to-date name/collapse display state.
    for (const VisualGroupDisplayRun& run : visualGroupDisplayRuns_)
    {
        auto it = visualGroupHandles_.find(run.groupId);
        if (it == visualGroupHandles_.end())
        {
            auto handle = std::make_unique<VisualGroupHandleView>(*this, run.groupId);
            addAndMakeVisible(*handle);
            it = visualGroupHandles_.emplace(run.groupId, std::move(handle)).first;
        }
        it->second->setDisplayState(run.name, run.collapsed);
    }
}

int TrackLanesView::yTopForVisibleIndex(const int vi) const noexcept
{
    int y = getLocalBounds().getY() + kArrangementTimelineHeaderGutterPx - verticalScrollOffsetPx_;
    for (int i = 0; i < vi && i < (int) visibleTrackEntries_.size(); ++i)
    {
        y += rowHeightForVisibleEntry(i);
    }
    return y;
}

int TrackLanesView::headerFreeBottomPxForRowHeight(const int rowDisplayHeightPx) noexcept
{
    // Chrome below the lowest control row of a header of this DISPLAY height: 0 for a collapsed
    // 4 px strip and for Micro (title row + band only), 14 for Mini, 3 for Small, 45 for Medium.
    if (rowDisplayHeightPx < track_row_heights::kMinRowHeightPx)
    {
        return 0;
    }
    int controlsBottom = TrackHeaderView::kHeaderRowTopPadPx + TrackHeaderView::kStripControlCellWidthPx;
    if (rowDisplayHeightPx >= TrackHeaderView::kMinimumHeightForSecondRowPx)
    {
        controlsBottom += TrackHeaderView::kHeaderRowGapPx + TrackHeaderView::kStripControlCellWidthPx;
    }
    return juce::jmax(0, rowDisplayHeightPx - TrackHeaderView::kHeaderResizeBandPx - controlsBottom);
}

void TrackLanesView::layoutVisualGroupHandles() noexcept
{
    const int gutterBottom = getLocalBounds().getY() + kArrangementTimelineHeaderGutterPx;
    const int headerW = headerColumnWidthPx();
    for (const VisualGroupDisplayRun& run : visualGroupDisplayRuns_)
    {
        const auto it = visualGroupHandles_.find(run.groupId);
        if (it == visualGroupHandles_.end() || it->second == nullptr)
        {
            continue;
        }
        VisualGroupHandleView& handle = *it->second;
        const int topY = yTopForVisibleIndex(run.firstVisibleIndex);
        const int bottomY = yTopForVisibleIndex(run.lastVisibleIndex)
                            + rowHeightForVisibleEntry(run.lastVisibleIndex);
        if (bottomY <= gutterBottom || topY >= getHeight() || getHeight() <= gutterBottom)
        {
            handle.setBounds(0, 0, 0, 0); // group fully scrolled out
            continue;
        }
        // The tab stays anchored at the group's top boundary but must never cover a name, a
        // number or a Power / Mute / Solo cell of either adjacent row (spec §6). Three placements:
        //   (a) boundary at or above the viewport top (first track, or scrolled past): the tab
        //       sits in the timeline-gutter band right of the add-track corner (sticky);
        //   (b) the previous row has free chrome under its lowest control row (Mini, Medium,
        //       Large …): the tab sits fully inside that free band, bottom edge on the boundary;
        //   (c) compact neighbours (Micro, Small, collapsed strips): a 12 px tab centred on the
        //       boundary and confined to the group margin + colour-segment zone — left of every
        //       control cell and of the name; the full name is the tab's tooltip.
        int tabH = kVisualGroupHandleHeightPx;
        int tabTop = 0;
        int x = kVisualGroupMarkerXPx;
        int w = 0;
        const int fullW = juce::jmin(kVisualGroupHandleMaxWidthPx, juce::jmax(0, headerW - x - 6));
        if (topY <= gutterBottom)
        {
            x = kVisualGroupHandleGutterLeftPx;
            w = juce::jmin(kVisualGroupHandleMaxWidthPx, juce::jmax(0, headerW - x - 6));
            tabTop = gutterBottom - tabH;
        }
        else
        {
            const int prevVi = run.firstVisibleIndex - 1;
            const int free = prevVi >= 0 ? headerFreeBottomPxForRowHeight(rowHeightForVisibleEntry(prevVi)) : 0;
            if (free >= kVisualGroupHandleMinInlineHeightPx)
            {
                tabH = juce::jmin(kVisualGroupHandleHeightPx, free);
                tabTop = topY - tabH;
                w = fullW;
            }
            else
            {
                tabH = kVisualGroupHandleCompactHeightPx;
                tabTop = topY - tabH / 2;
                const int segRight = TrackHeaderView::kHeaderGroupMarginPx
                                     + TrackHeaderView::colourSegmentWidthPxForDigits(trackNumberDigitCount());
                w = juce::jmax(0, juce::jmin(segRight, headerW - 2) - x);
            }
        }
        handle.setBounds(x, tabTop, w, tabH);
        handle.toFront(false);
    }
}

const TrackLanesView::VisualGroupDisplayRun*
TrackLanesView::findVisualGroupRun(const int groupId) const noexcept
{
    for (const VisualGroupDisplayRun& run : visualGroupDisplayRuns_)
    {
        if (run.groupId == groupId)
        {
            return &run;
        }
    }
    return nullptr;
}

bool TrackLanesView::isTrackInDisplayableVisualGroup(const TrackId tid) const noexcept
{
    return visualGroupMembershipByTrackId_.find(tid) != visualGroupMembershipByTrackId_.end();
}

bool TrackLanesView::isTrackInCollapsedVisualGroup(const TrackId tid) const noexcept
{
    const auto it = visualGroupMembershipByTrackId_.find(tid);
    return it != visualGroupMembershipByTrackId_.end() && it->second.collapsed;
}

bool TrackLanesView::suppressSeparatorBelowVisibleIndex(const int vi) const noexcept
{
    if (vi < 0 || vi + 1 >= (int) visibleTrackEntries_.size())
    {
        return false;
    }
    const auto a = visualGroupMembershipByTrackId_.find(visibleTrackEntries_[(size_t) vi].sessionTrackId);
    const auto b
        = visualGroupMembershipByTrackId_.find(visibleTrackEntries_[(size_t) vi + 1].sessionTrackId);
    return a != visualGroupMembershipByTrackId_.end() && b != visualGroupMembershipByTrackId_.end()
           && a->second.groupId == b->second.groupId && a->second.collapsed && b->second.collapsed;
}

void TrackLanesView::paintCollapsedGroupContent(juce::Graphics& g) const
{
    const auto bounds = getLocalBounds();
    constexpr int gutter = kArrangementTimelineHeaderGutterPx;
    const int headerW = juce::jmin(headerColumnWidthPx(), bounds.getWidth());
    const int hx = bounds.getX() + headerW;
    const int tw = juce::jmax(0, bounds.getWidth() - headerW);
    const double spp = timelineViewport_.getSamplesPerPixel();
    const std::int64_t visStart = timelineViewport_.getVisibleStartSamples();
    const std::shared_ptr<const SessionSnapshot> snap = session_.loadSessionSnapshotForAudioThread();
    const juce::Rectangle<int> viewport = bounds.withTrimmedTop(gutter);

    for (const VisualGroupDisplayRun& run : visualGroupDisplayRuns_)
    {
        if (!run.collapsed)
        {
            continue;
        }
        const int runTop = yTopForVisibleIndex(run.firstVisibleIndex);
        const int runBottom = yTopForVisibleIndex(run.lastVisibleIndex)
                              + rowHeightForVisibleEntry(run.lastVisibleIndex);
        const juce::Rectangle<int> runRect(bounds.getX(), runTop, bounds.getWidth(), runBottom - runTop);
        const juce::Rectangle<int> runVisible = runRect.getIntersection(viewport);
        if (runVisible.isEmpty())
        {
            continue;
        }
        juce::Graphics::ScopedSaveState const gs(g);
        g.reduceClipRegion(runVisible);
        // Header column: a quiet plate + the group marker — no title row, no buttons; the handle
        // tab (a separate child component) is the only interactive element of a collapsed group.
        if (headerW > 0)
        {
            g.setColour(juce::Colour(kCollapsedRunHeaderPlateArgb));
            g.fillRect(bounds.getX(), runTop, headerW, runBottom - runTop);
            g.setColour(juce::Colour(kVisualGroupMarkerArgb));
            g.fillRect(bounds.getX() + kVisualGroupMarkerXPx,
                       runTop,
                       kVisualGroupMarkerWidthPx,
                       runBottom - runTop);
        }
        // Lane area: one 4 px strip per member, zero gap, grey clip-interval fields that follow
        // the exact same zoom / scroll / origin mapping as the full lanes. Purely painted — no
        // components, so there is nothing to hit, hover, drag, or tooltip (spec §4).
        if (tw <= 0 || spp <= 0.0 || snap == nullptr)
        {
            continue;
        }
        const float originX = (float) hx;
        const auto sampleToX = [&](const std::int64_t s) {
            return TimelineRulerView::sessionSampleToLocalX(s, originX, visStart, spp);
        };
        g.setColour(juce::Colour(kCollapsedStripClipFillArgb));
        int y = runTop;
        for (int vi = run.firstVisibleIndex; vi <= run.lastVisibleIndex; ++vi)
        {
            const int stripH = rowHeightForVisibleEntry(vi);
            const VisibleTrackEntry& e = visibleTrackEntries_[(size_t) vi];
            const auto fillInterval = [&](const std::int64_t startS, const std::int64_t lenS) {
                if (lenS <= 0)
                {
                    return;
                }
                const float x1 = juce::jmax((float) hx, sampleToX(startS));
                const float x2 = juce::jmin((float) bounds.getRight(), sampleToX(startS + lenS));
                if (x2 > x1)
                {
                    g.fillRect(x1, (float) y, x2 - x1, (float) stripH);
                }
            };
            if (e.kind == VisibleTrackKind::Audio)
            {
                const int ti = snap->findTrackIndexById(e.sessionTrackId);
                if (ti >= 0)
                {
                    for (const PlacedClip& c : snap->getTrack(ti).getPlacedClips())
                    {
                        fillInterval(c.getStartSample(), c.getEffectiveLengthSamples());
                    }
                }
            }
            else if (e.kind == VisibleTrackKind::Instrument)
            {
                // Instrument destination AND Midi rows: this row's OWN timeline MIDI clips only.
                // A clean destination without own clips gets NO fabricated events from routed
                // MIDI tracks (spec §4).
                const auto itA = instrumentTimelineAttachments_.find(e.sessionTrackId);
                if (itA != instrumentTimelineAttachments_.end() && itA->second.controller != nullptr)
                {
                    for (const auto& clip : itA->second.controller->getClips())
                    {
                        if (clip != nullptr)
                        {
                            fillInterval(clip->startSamples, clip->lengthSamples);
                        }
                    }
                }
            }
            // Group / Master rows have no timeline clips: their strip stays empty.
            y += stripH;
        }
    }
}

// --------------------------------------------------------- header multi-selection

void TrackLanesView::handleHeaderSelectionClick(const TrackId tid, const bool shiftRange) noexcept
{
    const int vi = visibleRowIndexForTrackForDiagnostics(tid);
    if (vi < 0)
    {
        return;
    }
    const int anchorVi = headerSelectionAnchorTid_ != kInvalidTrackId
                             ? visibleRowIndexForTrackForDiagnostics(headerSelectionAnchorTid_)
                             : -1;
    headerMultiSelection_.clear();
    if (!shiftRange || anchorVi < 0)
    {
        headerMultiSelection_.push_back(tid);
        headerSelectionAnchorTid_ = tid;
    }
    else
    {
        // Contiguous range from the anchor (anchor itself unchanged, so another shift-click can
        // re-span from the same anchor).
        const int lo = juce::jmin(anchorVi, vi);
        const int hi = juce::jmax(anchorVi, vi);
        for (int i = lo; i <= hi; ++i)
        {
            headerMultiSelection_.push_back(visibleTrackEntries_[(size_t) i].sessionTrackId);
        }
    }
    repaint();
}

bool TrackLanesView::isHeaderMultiSelected(const TrackId tid) const noexcept
{
    return std::find(headerMultiSelection_.begin(), headerMultiSelection_.end(), tid)
           != headerMultiSelection_.end();
}

void TrackLanesView::clearHeaderMultiSelection() noexcept
{
    if (headerMultiSelection_.empty() && headerSelectionAnchorTid_ == kInvalidTrackId)
    {
        return;
    }
    headerMultiSelection_.clear();
    headerSelectionAnchorTid_ = kInvalidTrackId;
    repaint();
}

void TrackLanesView::applyHeaderRightClickSelectionPolicy(const TrackId clickedTid) noexcept
{
    // Right-click INSIDE the selection keeps it (so "Create collapsible group…" can target the
    // range); outside it selects the clicked track first — never a stale cross-row menu (spec §2).
    if (!isHeaderMultiSelected(clickedTid))
    {
        handleHeaderSelectionClick(clickedTid, false);
    }
}

std::vector<TrackId> TrackLanesView::selectedHeaderTrackIdsInVisibleOrder() const
{
    std::vector<std::pair<int, TrackId>> ordered;
    ordered.reserve(headerMultiSelection_.size());
    for (const TrackId tid : headerMultiSelection_)
    {
        const int vi = visibleRowIndexForTrackForDiagnostics(tid);
        if (vi >= 0)
        {
            ordered.emplace_back(vi, tid);
        }
    }
    std::sort(ordered.begin(), ordered.end());
    std::vector<TrackId> out;
    out.reserve(ordered.size());
    for (const auto& [vi, tid] : ordered)
    {
        juce::ignoreUnused(vi);
        out.push_back(tid);
    }
    return out;
}

bool TrackLanesView::canCreateCollapsibleGroupFromCurrentSelection() const
{
    const std::vector<TrackId> sel = selectedHeaderTrackIdsInVisibleOrder();
    if (sel.size() < 2)
    {
        return false;
    }
    const std::shared_ptr<const SessionSnapshot> snap = session_.loadSessionSnapshotForAudioThread();
    if (snap == nullptr)
    {
        return false;
    }
    int prevIdx = -2;
    for (const TrackId tid : sel)
    {
        const int idx = snap->findTrackIndexById(tid);
        if (idx < 0 || snap->getTrack(idx).getKind() == TrackKind::Master
            || session_.findVisualTrackGroupIdContainingTrack(tid).has_value())
        {
            return false; // No Stereo Out, no overlap with an existing group.
        }
        if (prevIdx >= -1 && idx != prevIdx + 1)
        {
            return false; // Only adjacent tracks.
        }
        prevIdx = idx;
    }
    return true;
}

void TrackLanesView::appendCreateCollapsibleGroupMenuItem(juce::PopupMenu& menu, const int itemId)
{
    juce::PopupMenu::Item item(juce::String("Create collapsible group") + juce::String::fromUTF8("\xe2\x80\xa6"));
    item.itemID = itemId;
    item.isEnabled = canCreateCollapsibleGroupFromCurrentSelection();
    menu.addItem(item);
}

void TrackLanesView::requestCreateCollapsibleGroupFromSelection()
{
    if (!canCreateCollapsibleGroupFromCurrentSelection() || visualGroupUiHooks_.createGroup == nullptr)
    {
        return;
    }
    const std::vector<TrackId> members = selectedHeaderTrackIdsInVisibleOrder();
    int displayableCount = 0;
    for (const VisualTrackGroup& g : session_.getVisualTrackGroups())
    {
        displayableCount += session_.isVisualTrackGroupDisplayable(g.id) ? 1 : 0;
    }
    const juce::String defaultName = "Group " + juce::String(displayableCount + 1);
    // Async name prompt (Enter = create, Escape = cancel); the group is created expanded with
    // every member height untouched — `Session::createVisualTrackGroup` via the Create hook.
    auto* aw = new juce::AlertWindow("Create collapsible group",
                                     "Group name:",
                                     juce::MessageBoxIconType::QuestionIcon,
                                     this);
    aw->addTextEditor("name", defaultName);
    aw->addButton("Create", 1, juce::KeyPress(juce::KeyPress::returnKey));
    aw->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
    juce::Component::SafePointer<TrackLanesView> safeThis(this);
    aw->enterModalState(
        true,
        juce::ModalCallbackFunction::create([safeThis, aw, members](const int result) {
            if (result != 1 || safeThis == nullptr
                || safeThis->visualGroupUiHooks_.createGroup == nullptr)
            {
                return;
            }
            juce::String name = aw->getTextEditorContents("name").trim();
            safeThis->visualGroupUiHooks_.createGroup(std::move(name), members);
        }),
        true);
}

// --------------------------------------------------------- handle actions + tests

void TrackLanesView::toggleVisualGroupCollapsedFromHandle(const int groupId)
{
    const VisualTrackGroup* const g = session_.findVisualTrackGroupById(groupId);
    if (g == nullptr || visualGroupUiHooks_.setCollapsed == nullptr)
    {
        return;
    }
    visualGroupUiHooks_.setCollapsed(groupId, !g->collapsed);
}

void TrackLanesView::beginVisualGroupRenameFromHandle(const int groupId)
{
    const auto it = visualGroupHandles_.find(groupId);
    if (it != visualGroupHandles_.end() && it->second != nullptr
        && !it->second->getBounds().isEmpty())
    {
        it->second->beginInlineRename();
    }
}

void TrackLanesView::showVisualGroupHandleContextMenu(const int groupId)
{
    const VisualTrackGroup* const g = session_.findVisualTrackGroupById(groupId);
    const auto itHandle = visualGroupHandles_.find(groupId);
    if (g == nullptr || itHandle == visualGroupHandles_.end() || itHandle->second == nullptr)
    {
        return;
    }
    enum
    {
        kToggleCollapse = 1,
        kRename = 2,
        kUngroup = 3,
    };
    juce::PopupMenu menu;
    menu.addItem(kToggleCollapse, g->collapsed ? "Expand group" : "Collapse group");
    menu.addItem(kRename, juce::String("Rename group") + juce::String::fromUTF8("\xe2\x80\xa6"));
    // Ungroup removes ONLY the visual grouping — tracks and clips are never deleted (spec §3).
    menu.addItem(kUngroup, "Ungroup");
    juce::Component::SafePointer<TrackLanesView> safeThis(this);
    menu.showMenuAsync(
        juce::PopupMenu::Options().withTargetComponent(itHandle->second.get()),
        [safeThis, groupId](const int result) {
            if (safeThis == nullptr || result == 0)
            {
                return;
            }
            if (result == kToggleCollapse)
            {
                safeThis->toggleVisualGroupCollapsedFromHandle(groupId);
            }
            else if (result == kRename)
            {
                safeThis->beginVisualGroupRenameFromHandle(groupId);
            }
            else if (result == kUngroup && safeThis->visualGroupUiHooks_.ungroup != nullptr)
            {
                safeThis->visualGroupUiHooks_.ungroup(groupId);
            }
        });
}

juce::Rectangle<int> TrackLanesView::visualGroupHandleBoundsForTest(const int groupId) const noexcept
{
    const auto it = visualGroupHandles_.find(groupId);
    return it != visualGroupHandles_.end() && it->second != nullptr ? it->second->getBounds()
                                                                    : juce::Rectangle<int>();
}

bool TrackLanesView::shortClickVisualGroupHandleLikeMouseForTest(const int groupId)
{
    const auto it = visualGroupHandles_.find(groupId);
    if (it == visualGroupHandles_.end() || it->second == nullptr || it->second->getBounds().isEmpty())
    {
        return false;
    }
    // The exact short-click action the handle's mouse-up dispatches.
    toggleVisualGroupCollapsedFromHandle(groupId);
    return true;
}

bool TrackLanesView::beginRenameOnVisualGroupHandleLikeLongPressForTest(const int groupId)
{
    const auto it = visualGroupHandles_.find(groupId);
    if (it == visualGroupHandles_.end() || it->second == nullptr || it->second->getBounds().isEmpty())
    {
        return false;
    }
    return it->second->beginInlineRename();
}

bool TrackLanesView::commitVisualGroupHandleRenameForTest(const int groupId,
                                                          const juce::String& newName)
{
    const auto it = visualGroupHandles_.find(groupId);
    if (it == visualGroupHandles_.end() || it->second == nullptr)
    {
        return false;
    }
    return it->second->commitRenameWithTextForTest(newName);
}