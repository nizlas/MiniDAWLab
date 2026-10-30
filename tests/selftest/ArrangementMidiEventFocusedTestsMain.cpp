// =============================================================================
// ArrangementMidiEventFocusedTests — three arrangement-window MIDI event bugs
// =============================================================================
//
// Drives the production MidiEventLane (mouse handlers, paint, undo command). No audio device.
// =============================================================================

#include "app/InstrumentMusicalUndoSnapshot.h"
#include "app/InstrumentRuntimeCoordinator.h"
#include "app/InstrumentTimelineRowCoordinator.h"
#include "app/UndoRedoCoordinator.h"
#include "audio/LatencySettingsStore.h"
#include "diagnostics/StabilityScenarioRunner.h"
#include "domain/ArrangementMusicalSnap.h"
#include "domain/AudioClip.h"
#include "domain/Session.h"
#include "engine/PlaybackEngine.h"
#include "engine/RecorderService.h"
#include "instruments/InstrumentTrackController.h"
#include "io/AudioWaveformCache.h"
#include "plugins/ExperimentalInstrumentHost.h"
#include "plugins/PluginInsertHost.h"
#include "transport/Transport.h"
#include "ui/ClipWaveformView.h"
#include "ui/ForbiddenCursor.h"
#include "ui/InspectorView.h"
#include "ui/TimelineRulerView.h"
#include "ui/TimelineViewportModel.h"
#include "ui/TrackHeaderView.h"
#include "ui/TrackLanesView.h"
#include "ui/UiPlayheadClock.h"
#include "ui/experimental/ExperimentalMidiPattern.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>

namespace
{
int failures = 0;
int checks = 0;

void expect(const bool ok, const char* what)
{
    ++checks;
    if (!ok)
    {
        ++failures;
        std::printf("FAIL: %s\n", what);
    }
}

juce::File tempDir()
{
    const juce::File d = juce::File::getSpecialLocation(juce::File::tempDirectory)
                             .getChildFile("dal-arrangement-midi-event-tests");
    (void)d.createDirectory();
    return d;
}

struct Harness
{
    static constexpr double kSpp = 200.0;
    static constexpr std::int64_t kExtent = 30000000;

    Session session;
    Transport transport;
    TimelineViewportModel viewport;
    juce::AudioDeviceManager deviceManager;
    RecorderService recorder;
    LatencySettingsStore latencyStore { deviceManager, tempDir().getChildFile("latency.xml") };
    AudioWaveformCache waveformCache;
    PlaybackEngine engine;
    PluginInsertHost plugins;
    std::unique_ptr<UndoRedoCoordinator> undo;
    std::unique_ptr<InstrumentRuntimeCoordinator> runtime;
    std::unique_ptr<InspectorView> inspector;
    std::unique_ptr<TrackLanesView> lanes;
    std::unique_ptr<InstrumentTimelineRowCoordinator> rows;
    TrackId trackA = kInvalidTrackId;
    TrackId trackB = kInvalidTrackId;
    bool rowsReady = false;

    Harness()
        : engine(transport, session, nullptr, nullptr, nullptr)
    {
        viewport.setSamplesPerPixelIfUnset(kSpp);
        session.setArrangementExtentSamples(kExtent);
        inspector = std::make_unique<InspectorView>(session);
        lanes = std::make_unique<TrackLanesView>(session, transport, viewport, deviceManager, recorder,
                                                 latencyStore, waveformCache);
        runtime = std::make_unique<InstrumentRuntimeCoordinator>(
            session, engine,
            InstrumentRuntimeCoordinator::Callbacks {
                {},
                [this] {
                    if (rowsReady && rows != nullptr)
                    {
                        rows->syncInstrumentTimelineRowAttachmentToSession();
                    }
                } });

        UndoRedoCoordinator::Callbacks undoCallbacks;
        undoCallbacks.buildSortedInstrumentMusicalUndoSnapshot = [this] {
            const auto snap = session.loadSessionSnapshotForAudioThread();
            if (snap == nullptr)
            {
                return std::vector<ProjectFileExperimentalInstrumentTrackV1> {};
            }
            return mini_daw_app_transport::buildSortedInstrumentMusicalUndoSnapshot(
                *snap,
                mini_daw_app_transport::InstrumentMusicalUndoSnapshotCallbacks {
                    [this](const TrackId tid) { return runtime->getMidiClipControllerForTrack(tid); } });
        };
        undoCallbacks.stableSortInstrumentMusicalUndoVector
            = [](std::vector<ProjectFileExperimentalInstrumentTrackV1>& v) {
                  mini_daw_app_transport::stableSortInstrumentMusicalUndoVector(v);
              };
        undoCallbacks.applyInstrumentMusicalUndoVectorToAllKeyedAndStaging
            = [this](const std::vector<ProjectFileExperimentalInstrumentTrackV1>& tracks) {
                  runtime->forEachInstrumentController(
                      [&](TrackId, InstrumentTrackController& ctl) {
                          ctl.applyExperimentalInstrumentMusicalUndoBlock(tracks);
                      });
              };
        undo = std::make_unique<UndoRedoCoordinator>(session, plugins, std::move(undoCallbacks));

        InstrumentTimelineRowCoordinator::Callbacks rowCallbacks;
        rowCallbacks.executeUndoableInstrumentEdit
            = [this](const juce::String& label, std::function<bool()> mutator) {
                  undo->executeUndoableInstrumentEdit(label, std::move(mutator));
              };
        rowCallbacks.snapArrangementTimelineSample = [this](const std::int64_t s) { return snapped(s); };
        rowCallbacks.clearAudioAndOtherInstrumentSelectionsForMidiTrack = [this](const TrackId keep) {
            lanes->clearAllPlacedClipSelections();
            runtime->forEachInstrumentController([&](const TrackId tid, InstrumentTrackController& ctl) {
                if (tid != keep)
                {
                    ctl.clearClipSelection();
                }
            });
        };
        rowCallbacks.clearAllArrangementEventSelections = [this] {
            lanes->clearAllPlacedClipSelections();
            runtime->forEachInstrumentController(
                [](TrackId, InstrumentTrackController& ctl) { ctl.clearClipSelection(); });
        };

        rows = std::make_unique<InstrumentTimelineRowCoordinator>(
            session, transport, *lanes, *inspector, viewport, *runtime, std::move(rowCallbacks));
        rowsReady = true;
        lanes->setArrangementInteractionPeers(rows.get(), runtime.get());
        lanes->setActiveEditToolProvider([] { return EditTool::Pointer; });
        lanes->setArrangementTimelineSnapFunction([this](const std::int64_t s) { return snapped(s); });

        trackA = session.appendExperimentalInstrumentShellTrack("Drums").value_or(kInvalidTrackId);
        trackB = session.appendExperimentalInstrumentShellTrack("Keys").value_or(kInvalidTrackId);
        auto* ctlA = runtime->getOrCreateInstrumentRuntimeForTrack(trackA).second;
        auto* ctlB = runtime->getOrCreateInstrumentRuntimeForTrack(trackB).second;
        expect(ctlA != nullptr && ctlB != nullptr, "two instrument shells exist");
        if (ctlA != nullptr)
        {
            expect(ctlA->bootstrapHalionSonicShellForSessionTrack(trackA), "HALion shell");
            ctlA->setTimelineSampleRate(48000.0);
        }
        if (ctlB != nullptr)
        {
            expect(ctlB->bootstrapGrooveAgentShellForSessionTrack(trackB), "Groove Agent shell");
            ctlB->setTimelineSampleRate(48000.0);
        }

        lanes->setSize(1400, 640);
        lanes->syncTracksFromSession();
        rows->syncInstrumentTimelineRowAttachmentToSession();
        lanes->resized();
    }

    ~Harness()
    {
        // Lanes view dies first while the coordinator still owns the MIDI lanes.
        lanes.reset();
        rows.reset();
        runtime.reset();
    }

    // Same call as the transport's arrangement snap, at the fixture's fixed rate (no audio device).
    [[nodiscard]] std::int64_t snapped(const std::int64_t s) const noexcept
    {
        return snapSampleToGridIfEnabled(s, session.getArrangementSnapSettings(), session.getProjectMusicalTime(),
                                         48000.0);
    }

    [[nodiscard]] InstrumentTrackController* controller(const TrackId tid) const
    {
        return runtime->getMidiClipControllerForTrack(tid);
    }

    [[nodiscard]] std::vector<juce::Component*> midiLanes() const
    {
        std::vector<juce::Component*> out;
        for (int i = 0; i < lanes->getNumChildComponents(); ++i)
        {
            juce::Component* c = lanes->getChildComponent(i);
            if (c == nullptr || c->getWidth() < 80 || c->getX() < lanes->headerColumnWidthPx() - 2)
            {
                continue;
            }
            if (dynamic_cast<ClipWaveformView*>(c) != nullptr || dynamic_cast<TrackHeaderView*>(c) != nullptr)
            {
                continue;
            }
            out.push_back(c);
        }
        std::sort(out.begin(), out.end(), [](const juce::Component* a, const juce::Component* b) {
            return a->getY() < b->getY();
        });
        return out;
    }

    void showSample(const std::int64_t visibleStart)
    {
        const std::int64_t delta = visibleStart - viewport.getVisibleStartSamples();
        if (delta != 0)
        {
            viewport.panBySamples(delta, (double)juce::jmax(1, lanes->getWidth()), kExtent);
        }
    }

    InstrumentMidiClipId putClip(InstrumentTrackController& ctl,
                                 const std::int64_t start,
                                 const std::int64_t length,
                                 const std::vector<TimelineMidiNote>& notes,
                                 const std::vector<MidiCcPoint>& cc)
    {
        std::vector<InstrumentMidiClipId> existing;
        for (const auto& c : ctl.getClips())
        {
            if (c != nullptr)
            {
                existing.push_back(c->id);
            }
        }
        if (!existing.empty())
        {
            (void)ctl.removeInstrumentMidiClipsByIds(existing);
        }
        const InstrumentMidiClipId id = ctl.createEmptyTimelineMidiClipAtSamples(start);
        InstrumentMidiClip* clip = ctl.getClipById(id);
        if (clip == nullptr)
        {
            return 0;
        }
        clip->startSamples = start;
        clip->timelineAnchorSamples = start;
        clip->lengthSamples = length;
        clip->pattern.bpm = 120.0;
        clip->pattern.ticksPerQuarter = kDefaultExperimentalTicksPerQuarter;
        clip->pattern.timelineNotes = notes;
        for (auto& n : clip->pattern.timelineNotes)
        {
            ensureTimelineNoteUid(n);
        }
        clip->pattern.ccPoints = cc;
        ctl.notifyClipPatternMutated(id);
        ctl.setSelectedClipIdsExclusive(id);
        return id;
    }
};

struct Gesture
{
    juce::Component* lane = nullptr;
    juce::Point<float> down {};
    juce::Time now;
    juce::MouseInputSource src { juce::Desktop::getInstance().getMainMouseSource() };

    explicit Gesture(juce::Component& laneIn, juce::Point<float> downIn)
        : lane(&laneIn)
        , down(downIn)
        , now(juce::Time::getCurrentTime())
    {
        lane->mouseDown(juce::MouseEvent(src, down, juce::ModifierKeys::leftButtonModifier, 0.f, 0.f, 0.f, 0.f, 0.f,
                                          lane, lane, now, down, now, 1, false));
    }

    void dragTo(juce::Point<float> to) const
    {
        lane->mouseDrag(juce::MouseEvent(src, to, juce::ModifierKeys::leftButtonModifier, 0.f, 0.f, 0.f, 0.f, 0.f,
                                          lane, lane, now, down, now, 1, true));
    }

    void finish(juce::Point<float> to) const
    {
        dragTo(to);
        lane->mouseUp(juce::MouseEvent(src, to, juce::ModifierKeys(), 0.f, 0.f, 0.f, 0.f, 0.f, lane, lane, now, down,
                                       now, 1, false));
    }
};

[[nodiscard]] bool nearBody(const juce::Colour c)
{
    const int dr = std::abs(c.getRed() - 0x34);
    const int dg = std::abs(c.getGreen() - 0x3c);
    const int db = std::abs(c.getBlue() - 0x4d);
    return c.getAlpha() > 200 && dr + dg + db < 48;
}

// Lightblue note bar blended over the opaque event body. The selection stroke is a similar blue,
// but it sits on the event's outer edge, so a real bar is the one with body fill both above and below.
[[nodiscard]] bool isNoteBarAt(const juce::Image& image, const int x, const int y)
{
    if (x < 0 || y < 0 || x >= image.getWidth() || y >= image.getHeight())
    {
        return false;
    }
    const juce::Colour c = image.getPixelAt(x, y);
    if (c.getAlpha() < 250 || c.getBlue() < 160 || c.getGreen() < 150 || c.getBlue() <= c.getRed() + 15)
    {
        return false;
    }
    bool bodyAbove = false;
    bool bodyBelow = false;
    for (int d = 5; d <= 12; ++d)
    {
        if (y - d >= 0 && nearBody(image.getPixelAt(x, y - d)))
        {
            bodyAbove = true;
        }
        if (y + d < image.getHeight() && nearBody(image.getPixelAt(x, y + d)))
        {
            bodyBelow = true;
        }
    }
    return bodyAbove && bodyBelow;
}

[[nodiscard]] int noteMinX(const juce::Image& image)
{
    int found = -1;
    for (int y = 0; y < image.getHeight(); ++y)
    {
        for (int x = 0; x < image.getWidth(); ++x)
        {
            if (isNoteBarAt(image, x, y))
            {
                if (found < 0 || x < found)
                {
                    found = x;
                }
                break;
            }
        }
    }
    return found;
}

[[nodiscard]] int noteYAt(const juce::Image& image, const int x)
{
    if (x < 0)
    {
        return -1;
    }
    for (int y = 0; y < image.getHeight(); ++y)
    {
        if (isNoteBarAt(image, x, y))
        {
            return y;
        }
    }
    return -1;
}

[[nodiscard]] int edgeX(const juce::Image& image, const bool fromLeft)
{
    if (fromLeft)
    {
        for (int x = 0; x < image.getWidth(); ++x)
        {
            for (int y = 8; y < image.getHeight() - 8; ++y)
            {
                if (image.getPixelAt(x, y).getAlpha() > 40)
                {
                    return x;
                }
            }
        }
    }
    else
    {
        for (int x = image.getWidth() - 1; x >= 0; --x)
        {
            for (int y = 8; y < image.getHeight() - 8; ++y)
            {
                if (image.getPixelAt(x, y).getAlpha() > 40)
                {
                    return x;
                }
            }
        }
    }
    return -1;
}

[[nodiscard]] bool columnIsPinnedStroke(const juce::Image& image, const int x, const int inwardX)
{
    if (x < 0 || inwardX < 0 || x >= image.getWidth() || inwardX >= image.getWidth())
    {
        return false;
    }
    int strokeRows = 0;
    int bodyRows = 0;
    for (int y = 14; y < image.getHeight() - 14; y += 3)
    {
        const juce::Colour edge = image.getPixelAt(x, y);
        const juce::Colour inward = image.getPixelAt(inwardX, y);
        if (isNoteBarAt(image, x, y) || isNoteBarAt(image, inwardX, y) || edge.getAlpha() < 40)
        {
            continue;
        }
        if (nearBody(inward) && !nearBody(edge))
        {
            ++strokeRows;
        }
        else if (nearBody(edge))
        {
            ++bodyRows;
        }
    }
    return strokeRows >= 4 && strokeRows > bodyRows;
}

void testClippedDragPaintsFrameWithNotes(Harness& h, juce::Component& lane, InstrumentTrackController& ctl)
{
    TimelineMidiNote note;
    note.midiNote = 36;
    note.velocity = 110;
    note.channel = 1;
    note.durationTicks = 80;

    // Left edge off-screen, right edge on-screen.
    const std::int64_t start = 1000000;
    const std::int64_t length = 180000;
    note.startTick = 4000; // 100000 samples into the clip, inside the visible band
    h.putClip(ctl, start, length, { note }, {});
    h.showSample(1080000);
    lane.repaint();
    const auto restLeft = lane.createComponentSnapshot(lane.getLocalBounds());
    const int restNote = noteMinX(restLeft);
    const int restRight = edgeX(restLeft, false);
    expect(restNote > 20, "left-clipped clip paints a note inside the lane");
    const juce::Point<float> grab { (float)juce::jmax(30, restNote + 8), lane.getHeight() * 0.55f };
    Gesture leftDrag(lane, grab);
    leftDrag.dragTo(grab + juce::Point<float>(36.f, 0.f));
    const auto draggedLeft = lane.createComponentSnapshot(lane.getLocalBounds());
    const int dragNote = noteMinX(draggedLeft);
    const int dragRight = edgeX(draggedLeft, false);
    expect(std::abs((dragNote - restNote) - 36) <= 3, "left-clipped note preview moves with the drag");
    expect(std::abs((dragRight - restRight) - (dragNote - restNote)) <= 3,
           "left-clipped frame edge and notes share the drag delta");
    expect(!columnIsPinnedStroke(draggedLeft, 2, 28),
           "left viewport edge is not a false event border while the true start is off-screen");
    const int oldY = noteYAt(restLeft, restNote);
    if (oldY >= 0 && dragNote > restNote + 12)
    {
        expect(!isNoteBarAt(draggedLeft, restNote, oldY),
               "left-clipped drag does not leave the previous note bar");
    }
    leftDrag.finish(grab + juce::Point<float>(36.f, 0.f));
    h.undo->invokeUndoFromWindowShortcut();

    // Right edge off-screen, left edge on-screen.
    note.startTick = 1600;
    h.putClip(ctl, start, 400000, { note }, {});
    h.showSample(960000);
    const auto restRightClip = lane.createComponentSnapshot(lane.getLocalBounds());
    const int restNoteR = noteMinX(restRightClip);
    const int restLeftEdge = edgeX(restRightClip, true);
    expect(restNoteR > 20 && restLeftEdge > 20, "right-clipped clip paints its on-screen left edge and a note");
    const juce::Point<float> grabR { (float)(restLeftEdge + 40), lane.getHeight() * 0.55f };
    Gesture rightDrag(lane, grabR);
    rightDrag.dragTo(grabR + juce::Point<float>(36.f, 0.f));
    const auto draggedRight = lane.createComponentSnapshot(lane.getLocalBounds());
    const int dragNoteR = noteMinX(draggedRight);
    const int dragLeftEdge = edgeX(draggedRight, true);
    expect(std::abs((dragNoteR - restNoteR) - 36) <= 3, "right-clipped note preview moves with the drag");
    expect(std::abs((dragLeftEdge - restLeftEdge) - (dragNoteR - restNoteR)) <= 3,
           "right-clipped frame edge and notes share the drag delta");
    expect(!columnIsPinnedStroke(draggedRight, draggedRight.getWidth() - 3, draggedRight.getWidth() - 30),
           "right viewport edge is not a false event border while the true end is off-screen");
    rightDrag.finish(grabR + juce::Point<float>(36.f, 0.f));
    h.undo->invokeUndoFromWindowShortcut();
}

void testScissors(Harness& h, juce::Component& lane, InstrumentTrackController& ctl)
{
    const std::int64_t start = 2000000;
    const std::int64_t length = 200000;
    TimelineMidiNote crossing;
    crossing.midiNote = 42;
    crossing.velocity = 100;
    crossing.channel = 2;
    crossing.startTick = 0;
    crossing.durationTicks = 8000;
    TimelineMidiNote after;
    after.midiNote = 48;
    after.velocity = 90;
    after.channel = 2;
    after.startTick = 6000;
    after.durationTicks = 400;
    MidiCcPoint early;
    early.startTick = 1000;
    early.controller = 11;
    early.value = 10;
    early.channel = 2;
    MidiCcPoint late;
    late.startTick = 7000;
    late.controller = 11;
    late.value = 90;
    late.channel = 2;
    MidiCcPoint pedal;
    pedal.startTick = 1000;
    pedal.controller = 64;
    pedal.value = 127;
    pedal.channel = 2;
    const InstrumentMidiClipId original = h.putClip(ctl, start, length, { crossing, after }, { early, late, pedal });
    expect(original != 0, "scissors fixture clip");
    h.showSample(start - 20000);
    h.lanes->setActiveEditToolProvider([] { return EditTool::Split; });

    const float midX = TimelineRulerView::sessionSampleToLocalX(start + length / 2, 0.f,
                                                                 h.viewport.getVisibleStartSamples(), Harness::kSpp);
    const juce::Point<float> click { midX, lane.getHeight() * 0.55f };
    const auto now = juce::Time::getCurrentTime();
    const auto src = juce::Desktop::getInstance().getMainMouseSource();
    lane.mouseDown(juce::MouseEvent(src, click, juce::ModifierKeys::leftButtonModifier, 0.f, 0.f, 0.f, 0.f, 0.f,
                                     &lane, &lane, now, click, now, 1, false));

    expect(ctl.getClips().size() == 2, "scissors splits into two clips");
    expect(h.undo->undoStackSizeForDiagnostics() == 1, "scissors is one undo step");
    const InstrumentMidiClip* left = nullptr;
    const InstrumentMidiClip* right = nullptr;
    for (const auto& c : ctl.getClips())
    {
        if (c == nullptr)
        {
            continue;
        }
        if (left == nullptr || c->startSamples < left->startSamples)
        {
            right = left;
            left = c.get();
        }
        else
        {
            right = c.get();
        }
    }
    expect(left != nullptr && right != nullptr, "both split halves exist");
    if (left == nullptr || right == nullptr)
    {
        h.lanes->setActiveEditToolProvider([] { return EditTool::Pointer; });
        return;
    }
    const std::int64_t splitAt = right->startSamples;
    expect(left->startSamples == start, "left half keeps the original start");
    expect(left->startSamples + left->lengthSamples == splitAt, "halves meet at the split");
    expect(right->startSamples + right->lengthSamples == start + length, "right half keeps the original end");
    expect(left->timelineAnchorSamples == start && right->timelineAnchorSamples == start,
           "both halves keep the original timeline anchor");
    expect(splitAt > start && splitAt < start + length, "split is strictly inside the window");
    expect(left->pattern.timelineNotes.size() == 2 && right->pattern.timelineNotes.size() == 2,
           "notes are copied, not rewritten");
    expect(left->pattern.ccPoints.size() == 3 && right->pattern.ccPoints.size() == 3, "CC points are copied");
    bool sameTicks = true;
    for (size_t i = 0; i < left->pattern.timelineNotes.size(); ++i)
    {
        sameTicks = sameTicks && left->pattern.timelineNotes[i].startTick == right->pattern.timelineNotes[i].startTick
                    && left->pattern.timelineNotes[i].durationTicks == right->pattern.timelineNotes[i].durationTicks
                    && left->pattern.timelineNotes[i].channel == 2;
    }
    expect(sameTicks, "copied notes keep tick positions and channel");

    const auto snap = ctl.loadRenderSnapshotForAudioThread();
    expect(snap != nullptr, "split publishes a render snapshot");
    if (snap != nullptr)
    {
        int crossingNotes = 0;
        bool crossingTruncated = false;
        bool crossingRestarts = false;
        int laterNotes = 0;
        for (const auto& ev : snap->notes)
        {
            if (ev.midiNote == 42)
            {
                ++crossingNotes;
                if (ev.noteOffAbsSample == splitAt)
                {
                    crossingTruncated = true;
                }
                if (ev.absSample >= splitAt)
                {
                    crossingRestarts = true;
                }
            }
            if (ev.midiNote == 48)
            {
                ++laterNotes;
                expect(ev.absSample >= splitAt, "note that starts after the split sounds on the right half");
            }
        }
        expect(crossingNotes == 1 && crossingTruncated && !crossingRestarts,
               "a note that crosses the split sounds only on the left, truncated at the split");
        expect(laterNotes == 1, "a note wholly after the split still sounds");
        bool sawEarly = false;
        bool sawLate = false;
        for (const auto& stream : snap->ccStreams)
        {
            if (stream.controller != 11)
            {
                continue;
            }
            for (const auto& ev : stream.events)
            {
                if (ev.value == 10)
                {
                    sawEarly = true;
                }
                if (ev.value == 90)
                {
                    sawLate = true;
                }
            }
        }
        expect(sawEarly && sawLate, "CC on both sides of the split survives in the baked stream");
    }

    h.undo->invokeUndoFromWindowShortcut();
    expect(ctl.getClips().size() == 1, "undo restores the single clip");
    const InstrumentMidiClip* restored = ctl.getClips().empty() ? nullptr : ctl.getClips().front().get();
    expect(restored != nullptr && restored->startSamples == start && restored->lengthSamples == length
               && restored->pattern.timelineNotes.size() == 2 && restored->pattern.ccPoints.size() == 3,
           "undo restores the original window, notes and CC");
    h.undo->invokeRedoFromWindowShortcut();
    expect(ctl.getClips().size() == 2, "redo splits again");
    h.undo->invokeUndoFromWindowShortcut();
    h.lanes->setActiveEditToolProvider([] { return EditTool::Pointer; });
}

void testRepeatedDrags(Harness& h, juce::Component& laneA, juce::Component& laneB, InstrumentTrackController& ctlA,
                       InstrumentTrackController& ctlB)
{
    const std::int64_t start = 2000000;
    TimelineMidiNote note;
    note.midiNote = 60;
    note.channel = 1;
    note.durationTicks = 200;
    h.putClip(ctlA, start, 100000, { note }, {});
    h.putClip(ctlB, start, 100000, { note }, {});
    // putClip selects on that controller only. Clear B so the first gestures on A are single
    // moves; a click on B later is what makes it the selection, matching the reported sequence.
    ctlB.clearClipSelection();
    h.showSample(start - 250 * (std::int64_t)Harness::kSpp);
    h.lanes->setActiveEditToolProvider([] { return EditTool::Pointer; });

    const auto startOf = [](InstrumentTrackController& ctl) {
        return ctl.getClips().empty() || ctl.getClips().front() == nullptr ? std::int64_t { -1 }
                                                                            : ctl.getClips().front()->startSamples;
    };
    const float bodyX = TimelineRulerView::sessionSampleToLocalX(start + 40000, 0.f,
                                                                  h.viewport.getVisibleStartSamples(), Harness::kSpp);
    const juce::Point<float> body { bodyX, laneA.getHeight() * 0.55f };
    const std::int64_t step = (std::int64_t)std::llround(40.0 * Harness::kSpp);

    {
        Gesture right(laneA, body);
        right.finish(body + juce::Point<float>(40.f, 0.f));
    }
    const std::int64_t afterRight = startOf(ctlA);
    expect(afterRight == start + step, "first drag moves A right");

    const float bodyAfterRight = bodyX + 40.f;
    const juce::Point<float> again { bodyAfterRight, body.y };
    {
        Gesture left(laneA, again);
        const juce::Point<float> landed = again + juce::Point<float>(-40.f, 0.f);
        left.dragTo(landed);
        expect(!(laneA.getMouseCursor() == getForbiddenNoDropMouseCursor()),
               "dragging A left after the right drag is a valid drop");
        expect(h.rows->instrumentMidiLaneHitAtScreen(laneA.localPointToGlobal(landed)) == h.trackA,
               "the lane under the pointer is still A's drop target");
        left.finish(landed);
    }
    expect(startOf(ctlA) == start, "the following left drag of A lands on the original start");

    const juce::Point<float> bodyB { bodyX, laneB.getHeight() * 0.55f };
    {
        Gesture moveB(laneB, bodyB);
        moveB.finish(bodyB + juce::Point<float>(30.f, 0.f));
    }
    expect(startOf(ctlB) == start + (std::int64_t)std::llround(30.0 * Harness::kSpp), "B on the other track moves");

    const juce::Point<float> third { bodyX, body.y };
    {
        Gesture leftAgain(laneA, third);
        const juce::Point<float> landed = third + juce::Point<float>(-40.f, 0.f);
        leftAgain.dragTo(landed);
        expect(!(laneA.getMouseCursor() == getForbiddenNoDropMouseCursor()),
               "A can move left again after B was moved");
        leftAgain.finish(landed);
    }
    expect(startOf(ctlA) == start - step, "the third finished drag of A moves it left");
    expect(startOf(ctlA) > 100000, "the left drag stays far from the project start");
}

[[nodiscard]] float xOf(const Harness& h, const std::int64_t sample)
{
    return TimelineRulerView::sessionSampleToLocalX(sample, 0.f, h.viewport.getVisibleStartSamples(),
                                                    h.viewport.getSamplesPerPixel());
}

void setSnap(Harness& h, const bool enabled, const SnapResolution resolution)
{
    SnapSettings snap;
    snap.enabled = enabled;
    snap.resolution = resolution;
    h.session.setArrangementSnapSettings(snap);
}

// 4/4, 120 BPM, 48 kHz: a bar is 96000 samples, a quarter 24000.
void testSnappedDrags(Harness& h, juce::Component& lane, InstrumentTrackController& ctl)
{
    constexpr std::int64_t kBar = 96000;
    constexpr std::int64_t kQuarter = 24000;
    const auto startOf = [&ctl] { return ctl.getClips().front()->startSamples; };
    const auto dragBy = [&](const std::int64_t start, const std::int64_t samples) {
        TimelineMidiNote note;
        note.midiNote = 60;
        note.channel = 1;
        note.durationTicks = 200;
        h.putClip(ctl, start, 100000, { note }, {});
        h.showSample(start - 250 * (std::int64_t)h.viewport.getSamplesPerPixel());
        const juce::Point<float> body { xOf(h, start + 40000), lane.getHeight() * 0.55f };
        Gesture drag(lane, body);
        drag.finish(body + juce::Point<float>((float)((double)samples / h.viewport.getSamplesPerPixel()), 0.f));
        return startOf();
    };

    setSnap(h, true, SnapResolution::Straight_1_1);
    expect(dragBy(20 * kBar, 30000) == 20 * kBar, "1/1: a short MIDI drag stays on its bar");
    expect(dragBy(20 * kBar, 60000) == 21 * kBar, "1/1: a MIDI drag past the midpoint lands on the next bar");
    setSnap(h, true, SnapResolution::Straight_1_4);
    expect(dragBy(20 * kBar, 30000) == 20 * kBar + kQuarter, "1/4: a MIDI drag lands on a quarter");
    setSnap(h, false, SnapResolution::Straight_1_1);
    const std::int64_t free = dragBy(20 * kBar, 30000);
    expect(free != 20 * kBar && free % kQuarter != 0, "snap off: a MIDI drag is free");

    // The same musical result at another zoom and after a horizontal scroll.
    setSnap(h, true, SnapResolution::Straight_1_1);
    h.viewport.zoomAroundSample(0.5, 600.0, (double)h.lanes->getWidth(), Harness::kExtent, 10.0, 2000.0);
    expect(dragBy(30 * kBar, 60000) == 31 * kBar, "1/1 holds after zoom and scroll");
    h.viewport.zoomAroundSample(2.0, 600.0, (double)h.lanes->getWidth(), Harness::kExtent, 10.0, 2000.0);

    // Off-grid object: its new start lands on the absolute grid, it does not keep its offset.
    setSnap(h, true, SnapResolution::Straight_1_1);
    expect(dragBy(20 * kBar + 7000, 60000) == 21 * kBar, "an off-grid MIDI event lands on the absolute grid");
    setSnap(h, false, SnapResolution::Straight_1_1);
}

// Current time set in the ruler. The clock holds the requested sample until the audio thread commits it.
void testSnappedRulerSeek(Harness& h)
{
    constexpr std::int64_t kBar = 96000;
    constexpr std::int64_t kQuarter = 24000;
    UiPlayheadClock clock;
    TimelineRulerView ruler(h.session, h.transport, h.deviceManager, h.viewport, clock);
    ruler.setSize(1200, 40);
    const juce::MouseInputSource src { juce::Desktop::getInstance().getMainMouseSource() };
    const auto now = juce::Time::getCurrentTime();
    const auto seekAt = [&](const std::int64_t sample) {
        const juce::Point<float> p { xOf(h, sample), 30.f };
        ruler.mouseDown(juce::MouseEvent(src, p, juce::ModifierKeys::leftButtonModifier, 0.f, 0.f, 0.f, 0.f, 0.f,
                                         &ruler, &ruler, now, p, now, 1, false));
        ruler.mouseUp(juce::MouseEvent(src, p, juce::ModifierKeys(), 0.f, 0.f, 0.f, 0.f, 0.f, &ruler, &ruler, now, p,
                                       now, 1, false));
        return (std::int64_t)std::llround(clock.readDisplaySamples(h.transport.readPlayheadSamplesForUi(), false,
                                                                   48000.0));
    };

    h.showSample(19 * kBar);
    setSnap(h, true, SnapResolution::Straight_1_1);
    expect(seekAt(20 * kBar + 30000) == 20 * kBar, "1/1: current time goes back to the nearest bar");
    expect(seekAt(20 * kBar + 60000) == 21 * kBar, "1/1: current time goes on to the nearest bar");
    setSnap(h, true, SnapResolution::Straight_1_4);
    expect(seekAt(20 * kBar + 30000) == 20 * kBar + kQuarter, "1/4: current time lands on a quarter");
    setSnap(h, false, SnapResolution::Straight_1_1);
    const std::int64_t free = seekAt(20 * kBar + 30000);
    expect(free % kQuarter != 0 && std::abs(free - (20 * kBar + 30000)) < 400, "snap off: current time is free");
}

void testSnappedAudioDrags(Harness& h)
{
    constexpr std::int64_t kBar = 96000;
    constexpr std::int64_t kQuarter = 24000;
    h.session.addTrack();
    TrackId audioTrack = kInvalidTrackId;
    const auto first = h.session.loadSessionSnapshotForAudioThread();
    for (int i = 0; i < first->getNumTracks(); ++i)
    {
        if (trackKindAcceptsTimelineAudioClips(first->getTrack(i).getKind()))
        {
            audioTrack = first->getTrack(i).getId();
        }
    }
    juce::AudioBuffer<float> silence(2, 480000);
    silence.clear();
    const auto material = std::make_shared<const AudioClip>(std::move(silence), 48000.0, juce::String("snap-audio"));
    expect(h.session.addPlacedClipFromExistingMaterial(material, 20 * kBar, 0, 100000, audioTrack).wasOk(),
           "an audio event is placed");
    h.lanes->syncTracksFromSession();
    h.lanes->resized();
    h.runtime->forEachInstrumentController(
        [](TrackId, InstrumentTrackController& ctl) { ctl.clearClipSelection(); });

    ClipWaveformView* lane = nullptr;
    for (int i = 0; i < h.lanes->getNumChildComponents(); ++i)
    {
        if (auto* view = dynamic_cast<ClipWaveformView*>(h.lanes->getChildComponent(i));
            view != nullptr && view->getTrackId() == audioTrack)
        {
            lane = view;
        }
    }
    expect(lane != nullptr && lane->getWidth() > 0, "the audio lane is laid out");
    if (lane == nullptr)
    {
        return;
    }
    const auto startOf = [&] {
        const auto snap = h.session.loadSessionSnapshotForAudioThread();
        return snap->getTrack(snap->findTrackIndexById(audioTrack)).getPlacedClip(0).getStartSample();
    };
    const auto dragBy = [&](const std::int64_t samples) {
        const std::int64_t start = startOf();
        h.showSample(start - 250 * (std::int64_t)h.viewport.getSamplesPerPixel());
        const juce::Point<float> body { xOf(h, start + 40000), lane->getHeight() * 0.8f };
        Gesture drag(*lane, body);
        drag.finish(body + juce::Point<float>((float)((double)samples / h.viewport.getSamplesPerPixel()), 0.f));
        return startOf();
    };

    setSnap(h, true, SnapResolution::Straight_1_1);
    expect(dragBy(30000) == 20 * kBar, "1/1: a short audio drag stays on its bar");
    expect(dragBy(60000) == 21 * kBar, "1/1: an audio drag past the midpoint lands on the next bar");
    setSnap(h, true, SnapResolution::Straight_1_4);
    expect(dragBy(30000) == 21 * kBar + kQuarter, "1/4: an audio drag lands on a quarter");
    setSnap(h, false, SnapResolution::Straight_1_1);
    const std::int64_t before = startOf();
    const std::int64_t free = dragBy(30000);
    expect(free != before && free % kQuarter != 0, "snap off: an audio drag is free");

    // Multi-select: the grabbed event lands on the grid, the other one keeps its distance.
    expect(h.session.addPlacedClipFromExistingMaterial(material, startOf() - 3 * kBar + 1234, 0, 100000, audioTrack).wasOk(),
           "a second, off-grid audio event is placed");
    h.lanes->syncTracksFromSession();
    const auto placed = [&] {
        const auto snap = h.session.loadSessionSnapshotForAudioThread();
        const Track& track = snap->getTrack(snap->findTrackIndexById(audioTrack));
        std::vector<std::pair<PlacedClipId, std::int64_t>> out;
        for (int i = 0; i < track.getNumPlacedClips(); ++i)
        {
            out.emplace_back(track.getPlacedClip(i).getId(), track.getPlacedClip(i).getStartSample());
        }
        std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.second < b.second; });
        return out;
    };
    h.lanes->setOnArrangementGroupMoveRequested(
        [&](const ArrangementGroupMovePlan& plan, const std::vector<ArrangementEventOrigin>& origins) {
            std::vector<Session::AudioClipGroupRelocation> moves;
            for (size_t i = 0; i < origins.size() && i < plan.dests.size(); ++i)
            {
                Session::AudioClipGroupRelocation move;
                move.id = origins[i].id.eventId;
                move.newStartSampleOnTimeline = plan.dests[i].destStartSamples;
                move.destTrackId = plan.dests[i].destTrackId;
                moves.push_back(move);
            }
            const bool moved = h.session.tryMoveAudioClipsTogether(moves);
            h.lanes->syncTracksFromSession();
            return moved;
        });
    auto events = placed();
    const std::int64_t grabbedStart = events[1].second;
    const std::int64_t gap = events[1].second - events[0].second;
    h.lanes->setArrangementEventSelection({ ArrangementEventId { ArrangementEventKind::AudioClip, audioTrack, events[0].first },
                                            ArrangementEventId { ArrangementEventKind::AudioClip, audioTrack, events[1].first } });
    setSnap(h, true, SnapResolution::Straight_1_1);
    h.showSample(grabbedStart - 500 * (std::int64_t)h.viewport.getSamplesPerPixel());
    const juce::Point<float> grab { xOf(h, grabbedStart + 40000), lane->getHeight() * 0.8f };
    {
        Gesture drag(*lane, grab);
        drag.finish(grab + juce::Point<float>((float)(60000.0 / h.viewport.getSamplesPerPixel()), 0.f));
    }
    events = placed();
    expect(events[1].second % kBar == 0 && events[1].second > grabbedStart,
           "multi-select: the grabbed event lands on the absolute grid");
    expect(events[1].second - events[0].second == gap, "multi-select: the events keep their distance");
    expect(events[0].second % kBar != 0, "multi-select: the other event is not snapped on its own");
    setSnap(h, false, SnapResolution::Straight_1_1);
}

} // namespace

bool isStabilityTestModeActive() noexcept
{
    return false;
}

int main()
{
    juce::ScopedJuceInitialiser_GUI gui;
    Harness harness;
    const auto lanes = harness.midiLanes();
    expect(lanes.size() >= 2, "two MIDI event lanes are laid out");
    if (lanes.size() < 2 || harness.controller(harness.trackA) == nullptr
        || harness.controller(harness.trackB) == nullptr)
    {
        std::printf("%d checks, %d failure(s)\n", checks, failures + 1);
        return 1;
    }
    testClippedDragPaintsFrameWithNotes(harness, *lanes[0], *harness.controller(harness.trackA));
    testScissors(harness, *lanes[0], *harness.controller(harness.trackA));
    testRepeatedDrags(harness, *lanes[0], *lanes[1], *harness.controller(harness.trackA),
                      *harness.controller(harness.trackB));
    testSnappedDrags(harness, *lanes[0], *harness.controller(harness.trackA));
    testSnappedRulerSeek(harness);
    testSnappedAudioDrags(harness);
    std::printf("%d checks, %d failure(s)\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
