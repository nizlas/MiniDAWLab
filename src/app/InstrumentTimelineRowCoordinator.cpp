#include "app/InstrumentTimelineRowCoordinator.h"

#include "app/InstrumentRuntimeCoordinator.h"
#include "domain/Session.h"
#include "domain/SessionSnapshot.h"
#include "domain/Track.h"
#include "instruments/InstrumentTrackController.h"
#include "plugins/ExperimentalInstrumentHost.h"
#include "transport/Transport.h"
#include "ui/InspectorView.h"
#include "ui/TimelineClipEventChrome.h"
#include "ui/TimelineRulerView.h"
#include "ui/TimelineViewportModel.h"
#include "ui/TrackLanesView.h"

#include <cmath>

namespace
{
/// MIDI runtime clip: same outer chrome sequence as placed audio clips (`ClipWaveformView`); label only inside.
void paintRuntimeMidiClipEventBlock(juce::Graphics& g,
                                    juce::Rectangle<float> eb,
                                    bool selected,
                                    bool activeClip,
                                    const juce::String& clipName)
{
    using namespace mini_daw::timeline_clip_chrome;
    paintEventChromeBody(g, eb, midiLaneEventBodyFill());
    if (selected)
    {
        paintEventChromeSelectionOverlay(g, eb);
    }
    if (activeClip)
    {
        paintEventChromeActiveSelectionOutline(g, eb);
    }
    g.setColour(juce::Colour(0xff242a33));
    g.setFont(11.5f);
    const juce::String label = clipName.trim().isNotEmpty() ? clipName.trim() : juce::String("MIDI");
    g.drawFittedText(
        label,
        clipEventLabelBounds(eb).toNearestInt(),
        juce::Justification::centredLeft,
        1);
}
} // namespace

struct InstrumentTimelineRowCoordinator::MidiEventLane final : public juce::Component,
                                                                 private juce::ChangeListener,
                                                                 private juce::Timer
{
    static constexpr bool kLogInstrumentLane = false;
    static constexpr float kClipDragThresholdPx = 3.0f;

    explicit MidiEventLane(InstrumentTimelineRowCoordinator& ownerIn,
                            InstrumentTrackController* ctl,
                            TrackId timelineInstrumentTrackId) noexcept
        : owner_(ownerIn)
        , boundCtl_(ctl)
        , laneTimelineTrackId_(timelineInstrumentTrackId)
    {
        startTimerHz(20);
        if (boundCtl_ != nullptr)
        {
            boundCtl_->addChangeListener(this);
        }
    }

    ~MidiEventLane() override
    {
        stopTimer();
        if (boundCtl_ != nullptr)
        {
            boundCtl_->removeChangeListener(this);
            boundCtl_ = nullptr;
        }
    }

    [[nodiscard]] TrackId laneTimelineTrackId() const noexcept { return laneTimelineTrackId_; }

    void attachControllerIfStillValid(InstrumentTrackController* ctl) noexcept
    {
        if (ctl == boundCtl_)
        {
            return;
        }
        if (boundCtl_ != nullptr)
        {
            boundCtl_->removeChangeListener(this);
        }
        boundCtl_ = ctl;
        if (boundCtl_ != nullptr)
        {
            boundCtl_->addChangeListener(this);
        }
    }

    [[nodiscard]] InstrumentTrackController* fixedControllerNullable() const noexcept { return boundCtl_; }

private:
    void changeListenerCallback(juce::ChangeBroadcaster*) override
    {
        owner_.repaintInstrumentTrackRow();
        owner_.refreshMidiEditorInstrumentUiIfOpen();
    }

    void timerCallback() override
    {
        if (owner_.transport_.readPlaybackIntentForUi() == PlaybackIntent::Playing)
        {
            repaint();
        }
    }

    void paint(juce::Graphics& g) override
    {
        const auto lane = getLocalBounds();

        const juce::Colour laneBg = getLookAndFeel().findColour(juce::ResizableWindow::backgroundColourId).darker(0.2f);
        g.setColour(laneBg);
        g.fillRect(lane);
        g.setColour(laneBg.darker(0.12f));
        g.drawVerticalLine(lane.getX(), (float)lane.getY(), (float)lane.getBottom());

        const auto laneContent = getLaneContentBounds();
        if (laneContent.isEmpty())
        {
            return;
        }

        InstrumentTrackController* const ac = activeControllerNullable();
        if (ac == nullptr)
        {
            return;
        }

        const InstrumentMidiClipId activeClipId = ac->getSelectedClipId();

        for (const auto& up : ac->getClips())
        {
            const auto* c = up.get();
            if (c == nullptr)
            {
                continue;
            }
            const bool sel = ac->isClipSelected(c->id);
            const std::int64_t previewDelta
                = (dragDragging_ && sel) ? dragEffectivePreviewDeltaSamples_ : std::int64_t{ 0 };
            const auto eb = getEventBoundsForClip(*c, laneContent, previewDelta);
            if (eb.isEmpty())
            {
                continue;
            }
            if (kLogInstrumentLane)
            {
                juce::Logger::writeToLog(
                    "instrument-lane: paint clip id=" + juce::String((juce::int64)c->id)
                    + " selected=" + juce::String(sel ? "true" : "false") + " eventBounds=" + eb.toString());
            }

            const bool activeClip = (c->id != 0 && c->id == activeClipId);
            paintRuntimeMidiClipEventBlock(g, eb.toFloat(), sel, activeClip && sel, c->name);
        }
    }

    void mouseDown(const juce::MouseEvent& e) override
    {
        const auto pos = e.getPosition();
        if (kLogInstrumentLane)
        {
            juce::Logger::writeToLog("instrument-lane: mouseDown x=" + juce::String(pos.x) + " y="
                                     + juce::String(pos.y));
        }

        if (!getLocalBounds().contains(pos))
        {
            return;
        }

        InstrumentTrackController* const ac = activeControllerNullable();
        if (ac == nullptr)
        {
            return;
        }

        dragCouldMove_ = false;
        dragDragging_ = false;
        dragEffectivePreviewDeltaSamples_ = 0;

        const bool midiMoveBlocked = owner_.trackLanes_.isInstrumentMidiClipMoveBlocked();

        if (auto* clip = hitTestClipAtEvent(e.position))
        {
            const bool toggleMulti = e.mods.isShiftDown();
            if (toggleMulti)
            {
                const bool thisTrackHasSelection = !ac->getSelectedClipIds().empty();
                if (!thisTrackHasSelection)
                {
                    if (owner_.callbacks_.clearAudioAndOtherInstrumentSelectionsForMidiTrack != nullptr)
                    {
                        owner_.callbacks_.clearAudioAndOtherInstrumentSelectionsForMidiTrack(
                            laneTimelineTrackId_);
                    }
                    ac->setSelectedClipIdsExclusive(clip->id);
                }
                else
                {
                    ac->toggleClipSelection(clip->id);
                }
            }
            else
            {
                if (owner_.callbacks_.clearAudioAndOtherInstrumentSelectionsForMidiTrack != nullptr)
                {
                    owner_.callbacks_.clearAudioAndOtherInstrumentSelectionsForMidiTrack(laneTimelineTrackId_);
                }
                if (ac->isClipSelected(clip->id))
                {
                    // Plain click on an already-selected clip: activate only (preserves multi-selection,
                    // including for double-click's first click).
                    ac->setActiveSelectedClipId(clip->id);
                }
                else
                {
                    ac->setSelectedClipIdsExclusive(clip->id);
                }
            }
            if (kLogInstrumentLane)
            {
                juce::Logger::writeToLog(
                    "instrument-lane: hit clip id=" + juce::String((juce::int64)clip->id) + " selected=true");
            }

            dragMouseDownLocal_ = e.position.toInt();
            dragCouldMove_ = !midiMoveBlocked && ac->isClipSelected(clip->id)
                             && timelineMappingAvailableForClipDrag_();
        }
        else
        {
            if (owner_.callbacks_.clearAllArrangementEventSelections != nullptr)
            {
                owner_.callbacks_.clearAllArrangementEventSelections();
            }
            else
            {
                ac->clearClipSelection();
            }
            if (kLogInstrumentLane)
            {
                juce::Logger::writeToLog("instrument-lane: no hit");
            }
        }

        repaint();
    }

    void mouseDrag(const juce::MouseEvent& e) override
    {
        if (!dragCouldMove_)
        {
            return;
        }

        InstrumentTrackController* const ac = activeControllerNullable();
        if (ac == nullptr || owner_.trackLanes_.isInstrumentMidiClipMoveBlocked())
        {
            return;
        }

        const std::int64_t s0 = laneTimelineSampleAtLocalX(dragMouseDownLocal_);
        const std::int64_t s1 = laneTimelineSampleAtLocalX(e.position.toInt());
        const std::int64_t rawDelta = s1 - s0;
        const std::int64_t effDelta = ac->clampInstrumentMidiClipMoveDeltaForCurrentSelection(rawDelta);

        if (!dragDragging_)
        {
            const float dx = std::abs(e.position.x - (float)dragMouseDownLocal_.x);
            if (dx < kClipDragThresholdPx)
            {
                return;
            }
            dragDragging_ = true;
        }

        dragEffectivePreviewDeltaSamples_ = effDelta;
        repaint();
    }

    void mouseUp(const juce::MouseEvent& e) override
    {
        InstrumentTrackController* const ac = activeControllerNullable();

        if (dragDragging_ && ac != nullptr && !owner_.trackLanes_.isInstrumentMidiClipMoveBlocked())
        {
            const std::int64_t s0 = laneTimelineSampleAtLocalX(dragMouseDownLocal_);
            const std::int64_t s1 = laneTimelineSampleAtLocalX(e.position.toInt());
            const std::int64_t dCommit = ac->clampInstrumentMidiClipMoveDeltaForCurrentSelection(s1 - s0);

            if (dCommit != 0)
            {
                auto execute = owner_.callbacks_.executeUndoableInstrumentEdit;
                if (execute != nullptr)
                {
                    TrackId laneTid = laneTimelineTrackId_;
                    execute(juce::String("Move MIDI clip"), [this, laneTid, dCommit]() mutable -> bool {
                        InstrumentRuntimeCoordinator& rc = owner_.instrumentRuntime_;
                        InstrumentTrackController* ctl = rc.getInstrumentControllerForTrack(laneTid);
                        if (ctl == nullptr || !ctl->hasInstrumentTrack())
                        {
                            return false;
                        }
                        const bool ok = ctl->moveSelectedInstrumentMidiClipsByDeltaSamples(dCommit);
                        if (ok)
                        {
                            owner_.trackLanes_.repaint();
                            owner_.inspector_.refreshFromSession();
                        }
                        return ok;
                    });
                }
                else
                {
                    if (ac->moveSelectedInstrumentMidiClipsByDeltaSamples(dCommit))
                    {
                        owner_.trackLanes_.repaint();
                        owner_.inspector_.refreshFromSession();
                    }
                }
            }
        }

        dragCouldMove_ = false;
        dragDragging_ = false;
        dragEffectivePreviewDeltaSamples_ = 0;
        repaint();
    }

    void mouseDoubleClick(const juce::MouseEvent& e) override
    {
        InstrumentTrackController* const ac = activeControllerNullable();
        if (ac == nullptr)
        {
            return;
        }

        if (auto* clip = hitTestClipAtEvent(e.position))
        {
            if (!ac->isClipSelected(clip->id))
            {
                ac->setSelectedClipIdsExclusive(clip->id);
            }
            else
            {
                ac->setActiveSelectedClipId(clip->id);
            }
            owner_.openMidiEditorForInstrumentClip(laneTimelineTrackId_, clip->id);
            repaint();
        }
    }

    [[nodiscard]] InstrumentTrackController* activeControllerNullable() const noexcept { return boundCtl_; }

    [[nodiscard]] bool timelineMappingAvailableForClipDrag_() const noexcept
    {
        const double spp = owner_.timelineViewport_.getSamplesPerPixel();
        return spp > 0.0 && std::isfinite(spp);
    }

    [[nodiscard]] std::int64_t laneTimelineSampleAtLocalX(juce::Point<int> localPt) const noexcept
    {
        const auto lc = getLaneContentBounds();
        const float relX = (float)(localPt.x - lc.getX());
        TimelineViewportModel& vp = owner_.timelineViewport_;
        const double spp = vp.getSamplesPerPixel();
        const std::int64_t visStart = vp.getVisibleStartSamples();
        if (!timelineMappingAvailableForClipDrag_())
        {
            return visStart;
        }
        return TimelineRulerView::xToSessionSampleClamped(relX, (float)lc.getWidth(), visStart, spp);
    }

    [[nodiscard]] juce::Rectangle<int> getLaneContentBounds() const
    {
        return getLocalBounds().reduced(0, 6);
    }

    [[nodiscard]] juce::Rectangle<int> getEventBoundsForClip(const InstrumentMidiClip& c,
                                                             juce::Rectangle<int> laneContent,
                                                             std::int64_t previewStartDelta) const
    {
        using namespace mini_daw::timeline_clip_chrome;
        const auto band = laneContent.toFloat().reduced(0.0f, kEventVerticalMargin);
        TimelineViewportModel& vp = owner_.timelineViewport_;
        const double spp = vp.getSamplesPerPixel();
        if (spp > 0.0 && std::isfinite(spp) && c.lengthSamples > 0)
        {
            const std::int64_t visStart = vp.getVisibleStartSamples();
            const float originX = band.getX();
            const std::int64_t len = juce::jmax(std::int64_t{ 1 }, c.lengthSamples);
            const std::int64_t anchor
                = juce::jmax(std::int64_t{ 0 }, c.startSamples + previewStartDelta);
            const float x0 = TimelineRulerView::sessionSampleToLocalX(anchor, originX, visStart, spp);
            const float x1 = TimelineRulerView::sessionSampleToLocalX(
                anchor + len, originX, visStart, spp);
            float left = juce::jmin(x0, x1);
            float right = juce::jmax(x0, x1);
            constexpr float minW = 40.0f;
            if (right - left < minW)
            {
                const float mid = 0.5f * (left + right);
                left = mid - minW * 0.5f;
                right = mid + minW * 0.5f;
            }
            left = juce::jlimit(band.getX(), band.getRight(), left);
            right = juce::jlimit(band.getX(), band.getRight(), right);
            if (right <= band.getX() + 0.5f || left >= band.getRight() - 0.5f)
            {
                return {};
            }
            const int y = juce::roundToInt(band.getY());
            const int h = juce::jmax(1, juce::roundToInt(band.getHeight()));
            return { juce::roundToInt(left), y, juce::jmax(1, juce::roundToInt(right - left)), h };
        }

        const int laneCW = juce::jmax(1, juce::roundToInt(band.getWidth()));
        const float s = (float)c.laneStartFractionPermille / 1000.f;
        const float e = (float)c.laneEndFractionPermille / 1000.f;
        const float span = juce::jlimit(0.02f, 1.f, e - s);
        int w = juce::roundToInt((float)laneCW * span);
        w = juce::jmax(40, juce::jmin(w, laneCW));
        const int minX0 = juce::roundToInt(band.getX());
        const int maxX0 = juce::roundToInt(band.getRight()) - w;
        if (maxX0 < minX0)
        {
            return {};
        }
        const int avail = juce::jmax(0, maxX0 - minX0);
        const int x0 = minX0 + (avail > 0 ? juce::roundToInt(s * (float)avail) : 0);
        const int clampedX0 = juce::jlimit(minX0, maxX0, x0);
        const int y = juce::roundToInt(band.getY());
        const int h = juce::jmax(1, juce::roundToInt(band.getHeight()));
        return { clampedX0, y, w, h };
    }

    [[nodiscard]] InstrumentMidiClip* hitTestClipAtEvent(juce::Point<float> pos) const
    {
        const auto laneContent = getLaneContentBounds();
        if (!laneContent.contains(pos.toInt()))
        {
            return nullptr;
        }

        InstrumentTrackController* const ac = activeControllerNullable();
        if (ac == nullptr)
        {
            return nullptr;
        }

        for (const auto& up : ac->getClips())
        {
            auto* c = up.get();
            if (c == nullptr)
            {
                continue;
            }

            if (getEventBoundsForClip(*c, laneContent, 0).contains(pos.toInt()))
            {
                return c;
            }
        }

        return nullptr;
    }

    InstrumentTimelineRowCoordinator& owner_;
    InstrumentTrackController* boundCtl_ = nullptr;
    TrackId laneTimelineTrackId_ = kInvalidTrackId;

    bool dragCouldMove_ = false;
    bool dragDragging_ = false;
    juce::Point<int> dragMouseDownLocal_;
    std::int64_t dragEffectivePreviewDeltaSamples_ = 0;
};

InstrumentTimelineRowCoordinator::InstrumentTimelineRowCoordinator(
    Session& session,
    Transport& transport,
    TrackLanesView& trackLanesView,
    InspectorView& inspectorView,
    TimelineViewportModel& timelineViewport,
    InstrumentRuntimeCoordinator& instrumentRuntime,
    Callbacks callbacks)
    : session_(session)
    , transport_(transport)
    , trackLanes_(trackLanesView)
    , inspector_(inspectorView)
    , timelineViewport_(timelineViewport)
    , instrumentRuntime_(instrumentRuntime)
    , callbacks_(std::move(callbacks))
{
}

InstrumentTimelineRowCoordinator::~InstrumentTimelineRowCoordinator() = default;

void InstrumentTimelineRowCoordinator::refreshMidiEditorInstrumentUiIfOpen()
{
    if (callbacks_.refreshMidiEditorInstrumentUiIfOpen != nullptr)
    {
        callbacks_.refreshMidiEditorInstrumentUiIfOpen();
    }
}

void InstrumentTimelineRowCoordinator::openMidiEditorForInstrumentClip(const TrackId timelineInstrumentTrackId,
                                                                       const InstrumentMidiClipId clipId)
{
    if (callbacks_.openMidiEditorForInstrumentClip != nullptr)
    {
        callbacks_.openMidiEditorForInstrumentClip(timelineInstrumentTrackId, clipId);
    }
}

void InstrumentTimelineRowCoordinator::repaintInstrumentTrackRow()
{
    for (auto& kv : instrumentTrackHeadersByTrackId_)
    {
        if (kv.second != nullptr)
        {
            kv.second->repaint();
        }
    }
    for (auto& kv : instrumentMidiEventLanesByTrackId_)
    {
        if (kv.second != nullptr)
        {
            kv.second->repaint();
        }
    }
}

void InstrumentTimelineRowCoordinator::tearDownExperimentalInstrumentTimelineUiForTrack(const TrackId tid) noexcept
{
    if (tid == kInvalidTrackId)
    {
        return;
    }
    instrumentMidiEventLanesByTrackId_.erase(tid);
    instrumentTrackHeadersByTrackId_.erase(tid);
}

void InstrumentTimelineRowCoordinator::clearInstrumentTimelineLanesAndHeaders() noexcept
{
    instrumentMidiEventLanesByTrackId_.clear();
    instrumentTrackHeadersByTrackId_.clear();
}

void InstrumentTimelineRowCoordinator::tickStructuralEditBlockedHeaderStripRepaint(
    const bool structuralTimelineEditBlockedUi) noexcept
{
    if (structuralTimelineEditBlockedUi != lastStructuralTimelineBlockedForHeaderStripUi_)
    {
        lastStructuralTimelineBlockedForHeaderStripUi_ = structuralTimelineEditBlockedUi;
        for (auto& kv : instrumentTrackHeadersByTrackId_)
        {
            if (kv.second != nullptr)
            {
                kv.second->repaint();
            }
        }
        trackLanes_.repaint();
    }
}

void InstrumentTimelineRowCoordinator::syncInstrumentTimelineRowAttachmentToSession() noexcept
{
    std::vector<InstrumentTimelineAttachment> rows;
    const std::shared_ptr<const SessionSnapshot> snap = session_.loadSessionSnapshotForAudioThread();
    if (snap != nullptr)
    {
        for (int ti = 0; ti < snap->getNumTracks(); ++ti)
        {
            const Track& tr = snap->getTrack(ti);
            if (tr.getKind() != TrackKind::Instrument)
            {
                continue;
            }
            const TrackId laneTid = tr.getId();
            InstrumentTrackController* ctl = instrumentRuntime_.getInstrumentControllerForTrack(laneTid);
            if (ctl == nullptr || !ctl->hasInstrumentTrack()
                || ctl->getExperimentalInstrumentDomainTrackId() != laneTid)
            {
                continue;
            }
            ensureInstrumentTimelineHeaderAndLaneForTrack(laneTid);
            auto itLane = instrumentMidiEventLanesByTrackId_.find(laneTid);
            auto itHdr = instrumentTrackHeadersByTrackId_.find(laneTid);
            if (itLane == instrumentMidiEventLanesByTrackId_.end() || itLane->second == nullptr
                || itHdr == instrumentTrackHeadersByTrackId_.end() || itHdr->second == nullptr)
            {
                continue;
            }
            itLane->second->attachControllerIfStillValid(ctl);
            rows.push_back(
                InstrumentTimelineAttachment{ laneTid, ctl, itHdr->second.get(), itLane->second.get() });
        }
    }
    trackLanes_.syncInstrumentTimelineAttachments(rows);
}

void InstrumentTimelineRowCoordinator::ensureInstrumentTimelineHeaderAndLaneForTrack(const TrackId tid)
{
    if (tid == kInvalidTrackId)
    {
        return;
    }
    InstrumentTrackController* ctl = instrumentRuntime_.getInstrumentControllerForTrack(tid);
    if (ctl == nullptr || !ctl->hasInstrumentTrack())
    {
        return;
    }

    auto itLaneExisting = instrumentMidiEventLanesByTrackId_.find(tid);
    if (itLaneExisting == instrumentMidiEventLanesByTrackId_.end())
    {
        auto lane = std::make_unique<MidiEventLane>(*this, ctl, tid);
        instrumentMidiEventLanesByTrackId_.emplace(tid, std::move(lane));
    }
    else if (itLaneExisting->second != nullptr)
    {
        itLaneExisting->second->attachControllerIfStillValid(ctl);
    }

    auto itHdr = instrumentTrackHeadersByTrackId_.find(tid);
    if (itHdr != instrumentTrackHeadersByTrackId_.end() && itHdr->second != nullptr)
    {
        return;
    }

    const TrackId laneTid = tid;

    TrackHeaderModelProvider modelProvider = [this, ctl, laneTid]() -> TrackHeaderModel {
        TrackHeaderModel m;
        ExperimentalInstrumentHost* mh = instrumentRuntime_.getInstrumentHostForTrack(laneTid);
        m.subtitle = ctl->getLaneHeaderSubtitle();
        m.active = ctl->isActive();
        m.armed = false;
        m.muted = ctl->isMuted();
        m.off = !ctl->isPowerOn();
        m.powerInteractable = !trackLanes_.isStructuralTimelineEditBlocked();
        m.muteInteractable = true;
        m.armInteractable = false;
        if (const auto sn = session_.loadSessionSnapshotForAudioThread())
        {
            const int idx = sn->findTrackIndexById(laneTid);
            if (idx >= 0)
            {
                m.name = sn->getTrack(idx).getName();
            }
            else
            {
                m.name = ctl->getLaneHeaderTitle();
            }
        }
        else
        {
            m.name = ctl->getLaneHeaderTitle();
        }
        m.instrumentEditorAvailable = mh != nullptr && mh->hasInstrument();
        return m;
    };

    auto repaintExtras = [this] {
        repaintInstrumentTrackRow();
        trackLanes_.repaint();
        inspector_.refreshFromSession();
    };

    TrackHeaderCallbacks callbacks;
    callbacks.onActivateName = [this, ctl, laneTid, repaintExtras] {
        instrumentRuntime_.deactivateAllKeyedAndStagingControllers();
        session_.setActiveTrack(laneTid);
        ctl->setActive(true);
        repaintExtras();
    };
    callbacks.onToggleMute = [ctl, laneTid, this, repaintExtras] {
        ctl->setMuted(!ctl->isMuted());
        session_.setActiveTrack(laneTid);
        repaintExtras();
    };
    callbacks.onTogglePower = [ctl, laneTid, this, repaintExtras]() -> bool {
        if (trackLanes_.isStructuralTimelineEditBlocked())
        {
            return false;
        }
        ctl->setPowerOn(!ctl->isPowerOn());
        session_.setActiveTrack(laneTid);
        repaintExtras();
        return true;
    };
    callbacks.onToggleArm = [] {};
    callbacks.onOpenInstrumentEditor = [this, laneTid] {
        if (ExperimentalInstrumentHost* h = instrumentRuntime_.getInstrumentHostForTrack(laneTid))
        {
            h->openNativeEditor();
        }
    };
    callbacks.onShowContextMenu = [this, laneTid, repaintExtras](TrackHeaderView& self, const juce::MouseEvent&) {
        session_.setActiveTrack(laneTid);
        instrumentRuntime_.setKeyedInstrumentControllersActiveExclusive(laneTid);
        repaintExtras();

        juce::PopupMenu menu;
        constexpr int kDeleteTrackMenuId = 1;
        constexpr int kImportMidiFileMenuId = 3;
        constexpr int kRescanDescriptionsMenuId = 2;
        const bool editLocked = trackLanes_.isStructuralTimelineEditBlocked();
        juce::PopupMenu::Item deleteItem;
        deleteItem.itemID = kDeleteTrackMenuId;
        deleteItem.text = "Delete Track";
        deleteItem.isEnabled = !editLocked;
        menu.addItem(deleteItem);
        juce::PopupMenu::Item importMidiItem;
        importMidiItem.itemID = kImportMidiFileMenuId;
        importMidiItem.text = "Import MIDI file...";
        importMidiItem.isEnabled = !editLocked;
        menu.addItem(importMidiItem);
        menu.addSeparator();
        juce::PopupMenu::Item rescanItem;
        rescanItem.itemID = kRescanDescriptionsMenuId;
        rescanItem.text = "Rescan plugin description (out-of-process)…";
        rescanItem.isEnabled = !editLocked;
        menu.addItem(rescanItem);

        juce::Component::SafePointer<TrackLanesView> safeLanes(&trackLanes_);
        auto rescan = callbacks_.runExperimentalInstrumentPluginDescriptionRescanForTrack;
        auto runMidiImport = callbacks_.runInstrumentMidiFileImportForTrack;
        menu.showMenuAsync(
            juce::PopupMenu::Options().withTargetComponent(&self),
            [safeLanes,
             laneTid,
             kDeleteTrackMenuId,
             kImportMidiFileMenuId,
             kRescanDescriptionsMenuId,
             rescan,
             runMidiImport](const int result) {
                if (safeLanes == nullptr || result == 0)
                {
                    return;
                }
                if (result == kDeleteTrackMenuId)
                {
                    safeLanes->requestDeleteTrackForHeaderMenu(laneTid);
                    return;
                }
                if (result == kImportMidiFileMenuId)
                {
                    if (runMidiImport != nullptr)
                    {
                        runMidiImport(laneTid);
                    }
                    return;
                }
                if (result == kRescanDescriptionsMenuId)
                {
                    if (rescan != nullptr)
                    {
                        rescan(laneTid);
                    }
                }
            });
    };

    auto hdr = std::make_unique<TrackHeaderView>(
        std::move(modelProvider), std::move(callbacks), kInvalidTrackId, std::nullopt);
    instrumentTrackHeadersByTrackId_[tid] = std::move(hdr);
}
