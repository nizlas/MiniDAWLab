// =============================================================================
// CollapsibleTrackGroupFocusedTests — purely visual collapsible track groups (production code)
// =============================================================================
//
// Drives the PRODUCTION `TrackLanesView` (offscreen, real `Session` / `Transport` / viewport /
// vertical-scroll model), the PRODUCTION project save/load path and the REAL `UndoRedoCoordinator`:
//   - header multi-selection (plain click / shift range / right-click policy) and the
//     "Create collapsible group" validation (>= 2 adjacent ungrouped tracks, never Stereo Out);
//   - collapsed mini-view: exactly 4 logical px per member with zero gaps, grey clip-interval
//     strips following the shared zoom/scroll/origin mapping (pixel-sampled), empty rows staying
//     empty (incl. a clean instrument destination), no hidden components under the strips;
//   - ONE shared layout model: scrollbar content height, scroll anchoring + clamping, the
//     production vertical-layout verification with collapsed rows;
//   - normal heights stored separately from the 4 px display (presets while collapsed, expansion
//     restore, Custom status from normal heights, rowHeight never saved as 4);
//   - group handle: placement at the group's top boundary (incl. first track / viewport top),
//     short-click toggle vs long-press rename (the exact actions the handle's mouse code runs);
//   - membership on track changes: Duplicate-after-source, delete + snapshot-restore, visual
//     dissolve below 2 members, the reorder veto (incl. allowed within-group moves);
//   - narrow group-metadata undo (create/rename/ungroup) through the REAL UndoRedoCoordinator,
//     collapse/expand never recording a step, the timeline snapshot never touched;
//   - v27 persistence: round trip, non-displayable groups dropped on save, pre-v27 files -> no
//     groups, malformed/overlapping/non-contiguous metadata degrading safely on load.
// No audio device, no window, no instrument runtime (instrument entry points referenced by the
// linked production sources are satisfied by never-executed link stubs below).
//
// Exit 0 = all checks green.
// =============================================================================

#include "app/UndoRedoCoordinator.h"
#include "domain/Session.h"
#include "domain/SessionSnapshot.h"
#include "domain/Track.h"
#include "domain/VisualTrackGroup.h"
#include "engine/RecorderService.h"
#include "audio/LatencySettingsStore.h"
#include "io/AudioWaveformCache.h"
#include "io/MonoWavFileWriter.h"
#include "io/ProjectFile.h"
#include "plugins/PluginInsertHost.h"
#include "transport/Transport.h"
#include "ui/TimelineRulerView.h"
#include "ui/TimelineViewportModel.h"
#include "ui/TrackLanesView.h"
#include "ui/TrackRowHeightPresets.h"
#include "ui/TrackColourPalette.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <memory>
#include <utility>
#include <vector>

namespace trh = track_row_heights;

namespace
{
int failures = 0;
int checks = 0;

constexpr double kRate = 48000.0;
/// Must match `kCollapsedStripClipFillArgb` in TrackLanesView.cpp (the grey clip-interval fill of
/// the collapsed mini-view; opaque, so snapshot pixels compare exactly).
constexpr juce::uint32 kStripFillArgb = 0xff8e98a8u;

void expect(const bool condition, const juce::String& label)
{
    ++checks;
    std::printf("[%s] %s\n", condition ? "PASS" : "FAIL", label.toRawUTF8());
    std::fflush(stdout);
    if (!condition)
    {
        ++failures;
    }
}

void info(const juce::String& text)
{
    std::printf("       %s\n", text.toRawUTF8());
    std::fflush(stdout);
}

[[nodiscard]] juce::File tempDir()
{
    const juce::File d = juce::File::getSpecialLocation(juce::File::tempDirectory)
                             .getChildFile("dal-track-group-tests");
    (void)d.createDirectory();
    return d;
}

/// Mono 1 s sine WAV for clip-interval fixtures (same writer the recorder uses).
[[nodiscard]] juce::File writeToneWav(const juce::String& name, const double seconds = 1.0)
{
    const juce::File wav = tempDir().getChildFile(name);
    const int n = static_cast<int>(seconds * kRate);
    std::vector<float> pcm(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i)
    {
        pcm[(size_t)i] = 0.4f
                         * static_cast<float>(std::sin(
                             2.0 * juce::MathConstants<double>::pi * 440.0 * i / kRate));
    }
    const float* chans[1] = { pcm.data() };
    const juce::Result r = MonoWavFileWriter::writeMulti24BitWavSegment(wav, chans, 1, n, kRate);
    if (!r.wasOk())
    {
        info("tone WAV write failed: " + r.getErrorMessage());
        return {};
    }
    return wav;
}

/// Offscreen production stack with the visual-group UI hooks wired the way `Main` wires them
/// (session command + display refresh); no undo coordinator unless a test creates one.
struct GroupFixture
{
    Session session;
    Transport transport;
    TimelineViewportModel viewport;
    juce::AudioDeviceManager deviceManager; // never initialised — no device
    RecorderService recorder;
    LatencySettingsStore latencyStore{ deviceManager, tempDir().getChildFile("latency.xml") };
    AudioWaveformCache waveformCache;
    std::unique_ptr<TrackLanesView> lanes;
    int hookDirtyCount = 0;

    explicit GroupFixture(const int extraAudioTracks = 5, const bool withGroupBus = false)
    {
        for (int i = 0; i < extraAudioTracks; ++i)
        {
            session.addTrack();
        }
        if (withGroupBus)
        {
            session.addGroupTrack();
        }
        viewport.setSamplesPerPixelIfUnset(480.0); // 100 px per second at 48 kHz
        lanes = std::make_unique<TrackLanesView>(session, transport, viewport, deviceManager,
                                                 recorder, latencyStore, waveformCache);
        // A parentless offscreen component defaults to invisible, which disables the whole
        // hit-test tree — make it visible so `getComponentAt` behaves like in the running app.
        lanes->setVisible(true);
        lanes->setSize(800, 400);
        lanes->syncTracksFromSession();

        VisualTrackGroupUiHooks hooks;
        hooks.createGroup = [this](juce::String name, std::vector<TrackId> members) {
            if (session.createVisualTrackGroup(std::move(name), std::move(members)).has_value())
            {
                ++hookDirtyCount;
                lanes->refreshVisualTrackGroupsFromSession();
            }
        };
        hooks.renameGroup = [this](const int gid, juce::String n) {
            session.renameVisualTrackGroup(gid, std::move(n));
            ++hookDirtyCount;
            lanes->refreshVisualTrackGroupsFromSession();
        };
        hooks.ungroup = [this](const int gid) {
            session.removeVisualTrackGroup(gid);
            ++hookDirtyCount;
            lanes->refreshVisualTrackGroupsFromSession();
        };
        hooks.setCollapsed = [this](const int gid, const bool c) {
            if (session.setVisualTrackGroupCollapsed(gid, c))
            {
                ++hookDirtyCount;
                lanes->refreshVisualTrackGroupsFromSession();
            }
        };
        lanes->setVisualTrackGroupUiHooks(std::move(hooks));
    }

    [[nodiscard]] TrackId tidAt(const int sessionIndex) const
    {
        return session.getTrackIdAtIndex(sessionIndex);
    }

    [[nodiscard]] std::vector<TrackId> tidsAt(const std::vector<int>& sessionIndices) const
    {
        std::vector<TrackId> out;
        for (const int i : sessionIndices)
        {
            out.push_back(tidAt(i));
        }
        return out;
    }

    /// Create a group straight on the Session (the create hook path minus the name prompt).
    [[nodiscard]] std::optional<int> makeGroup(const std::vector<int>& sessionIndices,
                                               const juce::String& name = {})
    {
        const std::optional<int> id = session.createVisualTrackGroup(name, tidsAt(sessionIndices));
        lanes->refreshVisualTrackGroupsFromSession();
        return id;
    }

    void setCollapsed(const int groupId, const bool collapsed)
    {
        (void)session.setVisualTrackGroupCollapsed(groupId, collapsed);
        lanes->refreshVisualTrackGroupsFromSession();
    }

    [[nodiscard]] std::vector<TrackId> rowTrackIds() const
    {
        std::vector<std::pair<int, TrackId>> rows;
        const int n = session.getNumTracks();
        for (int i = 0; i < n; ++i)
        {
            const TrackId tid = session.getTrackIdAtIndex(i);
            const int ix = lanes->visibleRowIndexForTrackForDiagnostics(tid);
            if (tid != kInvalidTrackId && ix >= 0)
            {
                rows.emplace_back(ix, tid);
            }
        }
        std::sort(rows.begin(), rows.end());
        std::vector<TrackId> out;
        for (const auto& [ix, tid] : rows)
        {
            juce::ignoreUnused(ix);
            out.push_back(tid);
        }
        return out;
    }

    /// Effective DISPLAY height per row from the production layout model (top-offset deltas; the
    /// last row closes against the model's content height).
    [[nodiscard]] std::map<TrackId, int> rowHeights() const
    {
        std::map<TrackId, int> out;
        const std::vector<TrackId> rows = rowTrackIds();
        for (size_t i = 0; i < rows.size(); ++i)
        {
            const int top = lanes->rowTopOffsetPxForTrackForDiagnostics(rows[i]);
            const int bottom = (i + 1 < rows.size())
                                   ? lanes->rowTopOffsetPxForTrackForDiagnostics(rows[i + 1])
                                   : lanes->verticalScrollModel().contentHeightPx;
            out[rows[i]] = bottom - top;
        }
        return out;
    }

    [[nodiscard]] bool verticalLayoutOk(const char* const where) const
    {
        juce::String report;
        juce::String fail;
        const bool ok = lanes->verifyVerticalScrollLayoutForDiagnostics(report, fail);
        if (!ok)
        {
            info(juce::String(where) + " vertical layout FAIL: " + fail);
        }
        return ok;
    }

    /// Widget-space y of a row's top edge at the current scroll offset.
    [[nodiscard]] int widgetYTopForTrack(const TrackId tid) const
    {
        return TrackLanesView::kArrangementTimelineHeaderGutterPx
               + lanes->rowTopOffsetPxForTrackForDiagnostics(tid)
               - lanes->verticalScrollModel().offsetPx;
    }
};

/// x in lanes-widget coordinates for a session sample (the shared ruler/lane mapping).
[[nodiscard]] int widgetXForSample(const GroupFixture& f, const std::int64_t s)
{
    return (int)std::lround(TimelineRulerView::sessionSampleToLocalX(
        s, (float)f.lanes->headerColumnWidthPx(), f.viewport.getVisibleStartSamples(),
        f.viewport.getSamplesPerPixel()));
}

// ---------------------------------------------------------------------------------------------
// §2: header multi-selection + create validation
// ---------------------------------------------------------------------------------------------
void testHeaderSelectionAndCreateValidation()
{
    GroupFixture f(/*extraAudioTracks*/ 5, /*withGroupBus*/ true);
    const std::vector<TrackId> rows = f.rowTrackIds();
    expect(rows.size() >= 8, "fixture: at least 8 arrangement rows (audio + Group bus + Stereo Out)");

    // Plain click: exactly the clicked header, anchor moves there.
    f.lanes->handleHeaderSelectionClick(rows[1], false);
    expect(f.lanes->isHeaderMultiSelected(rows[1]) && !f.lanes->isHeaderMultiSelected(rows[2]),
           "selection: a plain click selects exactly the clicked header");

    // Shift-click: contiguous visible range from the anchor; anchor unchanged for re-spanning.
    f.lanes->handleHeaderSelectionClick(rows[4], true);
    expect(f.lanes->selectedHeaderTrackIdsInVisibleOrder()
               == std::vector<TrackId>({ rows[1], rows[2], rows[3], rows[4] }),
           "selection: shift-click selects the contiguous range anchor..clicked, in visible order");
    f.lanes->handleHeaderSelectionClick(rows[2], true);
    expect(f.lanes->selectedHeaderTrackIdsInVisibleOrder()
               == std::vector<TrackId>({ rows[1], rows[2] }),
           "selection: another shift-click re-spans from the SAME anchor");

    // Selection never changes the active track by itself.
    const TrackId activeBefore = f.session.getActiveTrackId();
    f.lanes->handleHeaderSelectionClick(rows[3], true);
    expect(f.session.getActiveTrackId() == activeBefore,
           "selection: multi-selection never changes the Inspector's active track by itself");

    // Right-click policy: inside the selection keeps it, outside selects the clicked row.
    f.lanes->applyHeaderRightClickSelectionPolicy(rows[2]);
    expect(f.lanes->selectedHeaderTrackIdsInVisibleOrder().size() == 3,
           "right-click inside the selection keeps the whole selection");
    f.lanes->applyHeaderRightClickSelectionPolicy(rows[5]);
    expect(f.lanes->selectedHeaderTrackIdsInVisibleOrder() == std::vector<TrackId>({ rows[5] }),
           "right-click outside the selection selects exactly the clicked track");

    // Create validation: single track / non-adjacent / Stereo Out / overlap all refuse.
    const auto menuEnabled = [&f]() {
        juce::PopupMenu m;
        f.lanes->appendCreateCollapsibleGroupMenuItem(m, 3);
        juce::PopupMenu::MenuItemIterator it(m);
        return it.next() && it.getItem().isEnabled;
    };
    f.lanes->handleHeaderSelectionClick(rows[1], false);
    expect(!f.lanes->canCreateCollapsibleGroupFromCurrentSelection() && !menuEnabled(),
           "create: a single selected track does not qualify (menu item disabled)");

    f.lanes->handleHeaderSelectionClick(rows[1], false);
    f.lanes->handleHeaderSelectionClick(rows[2], true);
    expect(f.lanes->canCreateCollapsibleGroupFromCurrentSelection() && menuEnabled(),
           "create: two adjacent ungrouped tracks qualify (menu item enabled)");

    const TrackId master = rows.back();
    f.lanes->handleHeaderSelectionClick(rows[rows.size() - 2], false);
    f.lanes->handleHeaderSelectionClick(master, true);
    expect(!f.lanes->canCreateCollapsibleGroupFromCurrentSelection(),
           "create: a range including Stereo Out never qualifies");

    expect(!f.session.createVisualTrackGroup("NoPair", { rows[1], rows[3] }).has_value(),
           "create: Session refuses NON-adjacent members (nothing changed)");
    expect(!f.session.createVisualTrackGroup("Solo", { rows[1] }).has_value(),
           "create: Session refuses a single member");
    expect(!f.session.createVisualTrackGroup("WithMaster", { rows[rows.size() - 2], master }).has_value(),
           "create: Session refuses Stereo Out as a member");

    // A valid creation: track order, heights and the published snapshot are untouched.
    const auto snapBefore = f.session.loadSessionSnapshotForAudioThread();
    const std::map<TrackId, int> heightsBefore = f.rowHeights();
    const std::optional<int> gid = f.makeGroup({ 1, 2, 3 }, "");
    expect(gid.has_value(), "create: 3 adjacent audio tracks form a group");
    const VisualTrackGroup* g = gid.has_value() ? f.session.findVisualTrackGroupById(*gid) : nullptr;
    expect(g != nullptr && g->name.isNotEmpty() && g->name.startsWith("Group"),
           "create: empty name falls back to a sensible default (\"Group <n>\")");
    expect(g != nullptr && !g->collapsed, "create: the group is created EXPANDED");
    expect(f.rowTrackIds() == rows, "create: visible track order is unchanged");
    expect(f.rowHeights() == heightsBefore, "create: no member height changed at creation");
    expect(f.session.loadSessionSnapshotForAudioThread() == snapBefore,
           "create: the published session snapshot pointer is IDENTICAL (purely visual metadata)");

    // Overlap with the existing group refuses, both at Session and selection level.
    expect(!f.session.createVisualTrackGroup("Overlap", { rows[3], rows[4] }).has_value(),
           "create: Session refuses members already in another group");
    f.lanes->handleHeaderSelectionClick(rows[3], false);
    f.lanes->handleHeaderSelectionClick(rows[4], true);
    expect(!f.lanes->canCreateCollapsibleGroupFromCurrentSelection(),
           "create: a selection overlapping an existing group does not qualify");

    // Group-bus rows (routing) may be members of a VISUAL group — they are arrangement rows too.
    const int busIndex = f.session.getNumTracks() - 2; // bus sits just above Stereo Out
    expect(f.session.createVisualTrackGroup("WithBus", { f.tidAt(busIndex - 1), f.tidAt(busIndex) })
               .has_value(),
           "create: an adjacent audio + Group-bus pair forms a visual group (only Stereo Out is excluded)");
}

// ---------------------------------------------------------------------------------------------
// §4 + §5: collapsed layout = exactly 4 px per member, zero gaps, shared scroll model, no hidden
// hit targets
// ---------------------------------------------------------------------------------------------
void testCollapsedLayoutExactFourPx()
{
    GroupFixture f(/*extraAudioTracks*/ 5, /*withGroupBus*/ true);
    f.lanes->setSize(800, 700); // tall enough that the mid-list group stays inside the viewport
    const std::vector<TrackId> rows = f.rowTrackIds();
    expect(rows.size() >= 8, "fixture: mixed kinds laid out (audio + Group bus + Stereo Out)");

    // Mixed member kinds with varying heights (spec verification point 2): two audio rows + the
    // Group-bus row (indices 4, 5, 6) at 64 / 150 / 192 px.
    f.lanes->setTrackRowHeightPxForStabilityTest(rows[4], 64);
    f.lanes->setTrackRowHeightPxForStabilityTest(rows[5], 150);
    f.lanes->setTrackRowHeightPxForStabilityTest(rows[6], 192);
    const std::optional<int> gid = f.makeGroup({ 4, 5, 6 }, "Mixed");
    expect(gid.has_value(), "fixture: audio+audio+bus group with 64/150/192 px member heights");

    const int contentExpanded = f.lanes->verticalScrollModel().contentHeightPx;
    f.setCollapsed(*gid, true);

    const std::map<TrackId, int> h = f.rowHeights();
    expect(h.at(rows[4]) == 4 && h.at(rows[5]) == 4 && h.at(rows[6]) == 4,
           "collapse: every member row (incl. the bus) displays at EXACTLY 4 px (the Micro minimum "
           "does not apply)");
    expect(h.at(rows[0]) == trh::kMediumPresetPx && h.at(rows[1]) == trh::kMediumPresetPx,
           "collapse: rows outside the group keep their heights");
    expect(f.lanes->verticalScrollModel().contentHeightPx
               == contentExpanded - (64 + 150 + 192) + 3 * 4,
           "collapse: scrollbar content height shrinks by exactly the members' height delta");
    expect(f.lanes->rowTopOffsetPxForTrackForDiagnostics(rows[5])
                   - f.lanes->rowTopOffsetPxForTrackForDiagnostics(rows[4])
               == 4
               && f.lanes->rowTopOffsetPxForTrackForDiagnostics(rows[6])
                          - f.lanes->rowTopOffsetPxForTrackForDiagnostics(rows[5])
                      == 4,
           "collapse: strips stack with ZERO gap (consecutive tops exactly 4 px apart)");
    expect(f.verticalLayoutOk("collapsed"), "collapse: the shared vertical layout model verifies");
    expect(f.session.getActiveTrackId() != kInvalidTrackId,
           "collapse: a valid active track remains (never cleared by collapsing)");

    // No hidden hit targets: points inside the collapsed strips hit the lanes view itself (which
    // paints the strips), never a member header, lane, clip or button. The handle at the group's
    // top boundary is the only interactive element.
    const int headerW = f.lanes->headerColumnWidthPx();
    const int stripsTop = f.widgetYTopForTrack(rows[4]);
    const juce::Rectangle<int> handle = f.lanes->visualGroupHandleBoundsForTest(*gid);
    expect(!handle.isEmpty(), "collapse: the group handle stays laid out as an overlay");
    bool noHiddenHits = true;
    for (const int y : { stripsTop + 2, stripsTop + 6, stripsTop + 10 })
    {
        for (const int x : { 40, headerW - 30, headerW + 60, headerW + 300 })
        {
            const juce::Point<int> p(x, y);
            if (handle.contains(p))
            {
                continue;
            }
            juce::Component* const c = f.lanes->getComponentAt(p.x, p.y);
            noHiddenHits = noHiddenHits && (c == f.lanes.get());
        }
    }
    expect(noHiddenHits,
           "collapse: NO hidden child component under the 12 px strip block (headers, lanes, "
           "buttons and clips are all unreachable)");

    // Expand: each member returns to its stored normal height.
    f.setCollapsed(*gid, false);
    const std::map<TrackId, int> he = f.rowHeights();
    expect(he.at(rows[4]) == 64 && he.at(rows[5]) == 150 && he.at(rows[6]) == 192,
           "expand: every member returns to its individually stored height (64/150/192)");
    expect(f.lanes->verticalScrollModel().contentHeightPx == contentExpanded,
           "expand: scrollbar content height is exactly the pre-collapse value again");
    expect(f.verticalLayoutOk("expanded"), "expand: the shared vertical layout model verifies");
}

// ---------------------------------------------------------------------------------------------
// §4: the collapsed mini-view's grey clip intervals, pixel-sampled under zoom and scroll
// ---------------------------------------------------------------------------------------------
void testCollapsedMiniViewClipIntervals()
{
    const juce::File wav = writeToneWav("strip-tone.wav");
    expect(wav.existsAsFile(), "fixture: tone WAV written");

    GroupFixture f(/*extraAudioTracks*/ 4);
    const std::vector<TrackId> rows = f.rowTrackIds();
    const TrackId tA = rows[1]; // clip 1.0 s .. 2.0 s (+ a far clip at 10 s to extend the arrangement)
    const TrackId tB = rows[2]; // empty — its strip must stay empty
    const TrackId tC = rows[3]; // clips 0.5..1.5 and 1.0..2.0 — overlap paints as ONE union field
    const auto sec = [](const double s) { return (std::int64_t)std::llround(s * kRate); };
    expect(f.session.addRecordedTakeAtSample(wav, kRate, sec(1.0), tA, sec(1.0)).wasOk()
               && f.session.addRecordedTakeAtSample(wav, kRate, sec(10.0), tA, sec(1.0)).wasOk()
               && f.session.addRecordedTakeAtSample(wav, kRate, sec(0.5), tC, sec(1.0)).wasOk()
               && f.session.addRecordedTakeAtSample(wav, kRate, sec(1.0), tC, sec(1.0)).wasOk(),
           "fixture: audio clips placed (A: 1-2 s and 10-11 s, C: 0.5-1.5 s + 1.0-2.0 s)");
    f.lanes->syncTracksFromSession();

    const std::optional<int> gid = f.makeGroup({ 1, 2, 3 }, "Strips");
    expect(gid.has_value(), "fixture: group over the three clip rows");
    f.setCollapsed(*gid, true);

    const auto stripMidY = [&](const TrackId tid) { return f.widgetYTopForTrack(tid) + 2; };
    const auto isFill = [&](const juce::Image& img, const int x, const int y) {
        return img.getPixelAt(x, y) == juce::Colour(kStripFillArgb);
    };

    {
        const juce::Image img = f.lanes->createComponentSnapshot(f.lanes->getLocalBounds(), false, 1.0f);
        // x positions chosen off the 0.25 s beat-grid multiples so grid lines never interfere.
        expect(isFill(img, widgetXForSample(f, sec(1.3)), stripMidY(tA))
                   && isFill(img, widgetXForSample(f, sec(1.9)), stripMidY(tA)),
               "strips: track A's 1-2 s clip paints the grey interval field");
        expect(!isFill(img, widgetXForSample(f, sec(0.6)), stripMidY(tA))
                   && !isFill(img, widgetXForSample(f, sec(2.3)), stripMidY(tA)),
               "strips: outside track A's clip the strip is NOT filled (actual placement + length)");
        expect(!isFill(img, widgetXForSample(f, sec(0.6)), stripMidY(tB))
                   && !isFill(img, widgetXForSample(f, sec(1.3)), stripMidY(tB))
                   && !isFill(img, widgetXForSample(f, sec(1.9)), stripMidY(tB)),
               "strips: the empty member's strip stays completely empty");
        expect(isFill(img, widgetXForSample(f, sec(0.6)), stripMidY(tC))
                   && isFill(img, widgetXForSample(f, sec(1.2)), stripMidY(tC))
                   && isFill(img, widgetXForSample(f, sec(1.9)), stripMidY(tC)),
               "strips: track C's overlapping clips paint one continuous union field (0.5-2.0 s)");
        expect(!isFill(img, widgetXForSample(f, sec(2.3)), stripMidY(tC)),
               "strips: track C's union field ends where the material ends");
    }

    // Zoom in (the production zoom path), then scroll right: the strips must follow the exact
    // same x mapping as the ruler/lanes (`sessionSampleToLocalX` with the NEW visStart + spp).
    const double laneW = (double)(f.lanes->getWidth() - f.lanes->headerColumnWidthPx());
    const std::int64_t ext = f.session.loadSessionSnapshotForAudioThread()->getArrangementExtentSamples();
    f.viewport.zoomAroundSample(0.25, 0.0, laneW, ext, 1.0, 1.0e6);
    f.viewport.panBySamples(sec(0.5), laneW, ext);
    expect(f.viewport.getVisibleStartSamples() == sec(0.5)
               && f.viewport.getSamplesPerPixel() < 480.0,
           "zoom/scroll: viewport zoomed in and panned to 0.5 s");
    {
        const juce::Image img = f.lanes->createComponentSnapshot(f.lanes->getLocalBounds(), false, 1.0f);
        expect(isFill(img, widgetXForSample(f, sec(1.3)), stripMidY(tA))
                   && !isFill(img, widgetXForSample(f, sec(0.9)), stripMidY(tA)),
               "zoom/scroll: track A's interval follows the shared mapping at the new zoom + scroll");
        expect(isFill(img, widgetXForSample(f, sec(0.7)), stripMidY(tC))
                   && !isFill(img, widgetXForSample(f, sec(2.3)), stripMidY(tC)),
               "zoom/scroll: track C's union field follows the shared mapping too");
    }
}

// ---------------------------------------------------------------------------------------------
// §4: members without their own timeline clips paint an EMPTY strip (clip-less audio + Group
// bus). The instrument-row branch (a clean destination with routed MIDI feeders gets NO
// fabricated events) reads only the row's OWN timeline clips by construction and needs the full
// instrument runtime — covered by the real app, not constructible in this harness.
// ---------------------------------------------------------------------------------------------
void testClipLessMemberStripsStayEmpty()
{
    GroupFixture f(/*extraAudioTracks*/ 2, /*withGroupBus*/ true);
    // Session order: audio, audio, audio, bus, master -> group the last audio + bus pair.
    const std::optional<int> gid = f.makeGroup({ 2, 3 }, "NoEvents");
    expect(gid.has_value(), "fixture: clip-less audio + Group bus grouped");
    const TrackId audioTid = f.tidAt(2);
    const TrackId busTid = f.tidAt(3);
    f.setCollapsed(*gid, true);

    const juce::Image img = f.lanes->createComponentSnapshot(f.lanes->getLocalBounds(), false, 1.0f);
    const int headerW = f.lanes->headerColumnWidthPx();
    bool anyFill = false;
    for (const TrackId tid : { audioTid, busTid })
    {
        const int y = f.widgetYTopForTrack(tid) + 2;
        for (int x = headerW + 3; x < f.lanes->getWidth() - 2; x += 7)
        {
            anyFill = anyFill || (img.getPixelAt(x, y) == juce::Colour(kStripFillArgb));
        }
    }
    expect(!anyFill,
           "strips: clip-less members (audio without clips, Group bus) get NO fabricated interval "
           "fields (their strips stay empty end to end)");
}

// ---------------------------------------------------------------------------------------------
// §5: normal heights vs the 4 px display — presets while collapsed, Custom status, save values
// ---------------------------------------------------------------------------------------------
void testNormalHeightsSeparateFromCollapsedDisplay()
{
    GroupFixture f;
    const std::vector<TrackId> rows = f.rowTrackIds();
    f.lanes->applyTrackRowHeightPreset(trh::TrackRowHeightPreset::Medium);
    f.lanes->setTrackRowHeightPxForStabilityTest(rows[2], 150);
    const std::optional<int> gid = f.makeGroup({ 1, 2, 3 }, "Heights");
    f.setCollapsed(*gid, true);

    // The saved per-track heights are the NORMAL heights — never the 4 px display value.
    bool anyFour = false;
    int storedDragged = 0;
    for (const auto& [tid, px] : f.lanes->allTrackRowHeightsPxForProjectSave())
    {
        anyFour = anyFour || (px == 4);
        if (tid == rows[2])
        {
            storedDragged = px;
        }
    }
    expect(!anyFour && storedDragged == 150,
           "heights: project-save heights are the stored NORMAL heights (150 kept, never 4)");
    expect(!f.lanes->uniformTrackRowHeightPresetStatus().has_value(),
           "heights: Custom status derives from NORMAL heights while collapsed (150 differs)");

    // A preset chosen while the group is collapsed changes the members' NORMAL heights; the
    // display stays 4 px until expansion restores the new preset height.
    f.lanes->applyTrackRowHeightPreset(trh::TrackRowHeightPreset::Small);
    const std::map<TrackId, int> hCollapsed = f.rowHeights();
    expect(hCollapsed.at(rows[1]) == 4 && hCollapsed.at(rows[2]) == 4 && hCollapsed.at(rows[3]) == 4,
           "heights: the preset does NOT change the collapsed 4 px display");
    expect(f.lanes->uniformTrackRowHeightPresetStatus() == trh::TrackRowHeightPreset::Small,
           "heights: status reads Small from the normal heights while the group is collapsed");
    f.setCollapsed(*gid, false);
    const std::map<TrackId, int> hExpanded = f.rowHeights();
    expect(hExpanded.at(rows[1]) == trh::kSmallPresetPx
               && hExpanded.at(rows[2]) == trh::kSmallPresetPx
               && hExpanded.at(rows[3]) == trh::kSmallPresetPx,
           "heights: expansion restores the heights the preset gave the HIDDEN members (64)");
}

// ---------------------------------------------------------------------------------------------
// §3: handle placement (first track, viewport top, scrolled out) + short click vs long press
// ---------------------------------------------------------------------------------------------
void testHandlePlacementAndGestures()
{
    GroupFixture f(/*extraAudioTracks*/ 10);
    f.lanes->applyTrackRowHeightPreset(trh::TrackRowHeightPreset::Medium);
    constexpr int gutter = TrackLanesView::kArrangementTimelineHeaderGutterPx;

    // A group starting at the very FIRST track: the handle stays visible — in the gutter band
    // above the first member (right of the add-track corner), never over the member's title row.
    const std::optional<int> gFirst = f.makeGroup({ 0, 1 }, "First");
    const juce::Rectangle<int> bFirst = f.lanes->visualGroupHandleBoundsForTest(*gFirst);
    expect(!bFirst.isEmpty() && bFirst.getBottom() == gutter
               && bFirst.getHeight() == TrackLanesView::kVisualGroupHandleHeightPx
               && bFirst.getX() == TrackLanesView::kVisualGroupHandleGutterLeftPx
               && bFirst.getWidth() <= TrackLanesView::kVisualGroupHandleMaxWidthPx,
           "handle: a group at the FIRST track keeps its handle visible in the gutter band (covers no title row)");

    // A mid-list group with Medium neighbours: the handle sits fully INSIDE the previous row's
    // free chrome (below its second control row), its bottom edge on the group's top boundary.
    const std::optional<int> gMid = f.makeGroup({ 3, 4, 5 }, "Mid");
    const juce::Rectangle<int> bMid = f.lanes->visualGroupHandleBoundsForTest(*gMid);
    const int boundaryY = f.widgetYTopForTrack(f.tidAt(3));
    expect(!bMid.isEmpty() && bMid.getBottom() == boundaryY
               && bMid.getHeight() == TrackLanesView::kVisualGroupHandleHeightPx
               && bMid.getY() >= f.widgetYTopForTrack(f.tidAt(2)) + TrackHeaderView::kHeaderRowTopPadPx
                                     + 2 * TrackHeaderView::kStripControlCellWidthPx + TrackHeaderView::kHeaderRowGapPx,
           "handle: a mid-list handle (Medium rows) sits inside the previous row's free band, bottom on the boundary, below its cells");
    expect(TrackLanesView::headerFreeBottomPxForRowHeight(trh::kMediumPresetPx) == 45
               && TrackLanesView::headerFreeBottomPxForRowHeight(trh::kMiniPresetPx) == 14
               && TrackLanesView::headerFreeBottomPxForRowHeight(trh::kSmallPresetPx) == 3
               && TrackLanesView::headerFreeBottomPxForRowHeight(trh::kMicroPresetPx) == 0
               && TrackLanesView::headerFreeBottomPxForRowHeight(TrackLanesView::kCollapsedGroupMemberRowHeightPx) == 0,
           "handle: free chrome under the lowest control row = 45 / 14 / 3 / 0 / 0 px for Medium / Mini / Small / Micro / collapsed strip");

    // Scroll until the mid group's top is above the viewport while its rows are still visible:
    // the handle moves into the gutter band instead of disappearing (sticky).
    f.lanes->scrollVerticallyToOffsetPx(f.lanes->rowTopOffsetPxForTrackForDiagnostics(f.tidAt(4)));
    const juce::Rectangle<int> bClamped = f.lanes->visualGroupHandleBoundsForTest(*gMid);
    expect(!bClamped.isEmpty() && bClamped.getBottom() == gutter,
           "handle: with the group's top scrolled past the viewport top the handle sits in the "
           "gutter band and stays accessible");

    // Scroll far past the group: the handle unmaps (no floating orphan).
    f.lanes->scrollVerticallyToOffsetPx(f.lanes->verticalScrollModel().maxOffsetPx());
    expect(f.lanes->visualGroupHandleBoundsForTest(*gFirst).isEmpty(),
           "handle: a fully scrolled-out group has NO laid-out handle");
    f.lanes->scrollVerticallyToOffsetPx(0);

    // Micro neighbours (no free chrome anywhere): the compact 12 px tab, centred on the boundary
    // and confined to the group margin + colour segment — left of every Power / Mute / Solo cell
    // and of the name, overlapping at most the top of the first member's icon / number area.
    {
        f.lanes->applyTrackRowHeightPreset(trh::TrackRowHeightPreset::Micro);
        const juce::Rectangle<int> bMicro = f.lanes->visualGroupHandleBoundsForTest(*gMid);
        const int boundaryMicro = f.widgetYTopForTrack(f.tidAt(3));
        const int segRight = TrackHeaderView::kHeaderGroupMarginPx
                             + TrackHeaderView::colourSegmentWidthPxForDigits(f.lanes->trackNumberDigitCount());
        const int firstCellX = segRight + TrackHeaderView::kHeaderSegmentToStripGapPx;
        expect(!bMicro.isEmpty() && bMicro.getHeight() == TrackLanesView::kVisualGroupHandleCompactHeightPx
                   && bMicro.getCentreY() == boundaryMicro && bMicro.getRight() <= segRight && bMicro.getRight() < firstCellX
                   && bMicro.getBottom() <= boundaryMicro + TrackHeaderView::kHeaderRowTopPadPx + 4,
               "handle (Micro): compact 12 px tab centred on the boundary, confined left of every title cell and the name");
        // Two adjacent groups in Micro: the second group's tab at the shared boundary never
        // overlaps the first group's tab or any control cell.
        const std::optional<int> gNext = f.makeGroup({ 6, 7 }, "Next");
        const juce::Rectangle<int> bNext = f.lanes->visualGroupHandleBoundsForTest(*gNext);
        expect(gNext.has_value() && !bNext.isEmpty() && !bNext.intersects(bMicro)
                   && bNext.getCentreY() == f.widgetYTopForTrack(f.tidAt(6)) && bNext.getRight() < firstCellX,
               "handle (Micro): two directly adjacent groups get two disjoint compact tabs, both left of the control cells");
        // The hidden hit-test of the previous (Micro) row's cells: nothing of the compact tab lies
        // over its title cells (they start at x = firstCellX).
        juce::Component* const atNextTab = f.lanes->getComponentAt(bNext.getCentreX(), bNext.getCentreY());
        expect(atNextTab != nullptr && atNextTab->getBounds() == bNext,
               "handle (Micro): the compact tab is the component hit at its centre (it swallows its own clicks)");
        f.lanes->applyTrackRowHeightPreset(trh::TrackRowHeightPreset::Medium);
        f.lanes->scrollVerticallyToOffsetPx(0);
    }

    // The handle never hit-tests through to the previous track's header or its resize band.
    const juce::Rectangle<int> bMid2 = f.lanes->visualGroupHandleBoundsForTest(*gMid);
    juce::Component* const atHandle
        = f.lanes->getComponentAt(bMid2.getCentreX(), bMid2.getCentreY());
    expect(atHandle != nullptr && atHandle != f.lanes.get()
               && atHandle->getBounds() == bMid2,
           "handle: hit-testing the handle area reaches ONLY the handle component (never the "
           "previous track or its resize band)");

    // Short click toggles exactly once per click (the exact action the mouse-up dispatches).
    const TrackId activeBefore = f.session.getActiveTrackId();
    expect(f.lanes->shortClickVisualGroupHandleLikeMouseForTest(*gMid)
               && f.session.findVisualTrackGroupById(*gMid)->collapsed,
           "handle: one short click collapses the group");
    expect(f.lanes->shortClickVisualGroupHandleLikeMouseForTest(*gMid)
               && !f.session.findVisualTrackGroupById(*gMid)->collapsed,
           "handle: the next short click expands it again (exactly one toggle per click)");
    expect(f.session.getActiveTrackId() == activeBefore,
           "handle: collapsing/expanding never changes the active track");

    // Long press begins the inline rename WITHOUT toggling; committing renames the group.
    const bool collapsedBefore = f.session.findVisualTrackGroupById(*gMid)->collapsed;
    expect(f.lanes->beginRenameOnVisualGroupHandleLikeLongPressForTest(*gMid),
           "handle: a long press opens the inline rename editor");
    expect(f.session.findVisualTrackGroupById(*gMid)->collapsed == collapsedBefore,
           "handle: the long press did NOT also toggle the collapse state");
    expect(f.lanes->commitVisualGroupHandleRenameForTest(*gMid, "Drums")
               && f.session.findVisualTrackGroupById(*gMid)->name == "Drums",
           "handle: committing the rename editor renames the group to \"Drums\"");
    expect(f.session.findVisualTrackGroupById(*gMid)->collapsed == collapsedBefore,
           "handle: the committed rename still never toggled the group");
}

// ---------------------------------------------------------------------------------------------
// §5: the group's top edge anchors on collapse; offsets stay valid (clamped)
// ---------------------------------------------------------------------------------------------
void testScrollAnchoringAndClamp()
{
    GroupFixture f(/*extraAudioTracks*/ 12);
    f.lanes->applyTrackRowHeightPreset(trh::TrackRowHeightPreset::Medium);
    const std::optional<int> gid = f.makeGroup({ 4, 5, 6 }, "Anchor");

    // Scroll the group's first member to the viewport top, collapse: the offset (and with it the
    // group's top edge) must not jump.
    const int groupTopOffset = f.lanes->rowTopOffsetPxForTrackForDiagnostics(f.tidAt(4));
    f.lanes->scrollVerticallyToOffsetPx(groupTopOffset);
    f.setCollapsed(*gid, true);
    expect(f.lanes->verticalScrollModel().offsetPx == groupTopOffset,
           "anchor: collapsing keeps the scroll offset (the group's top edge stays put)");
    expect(f.widgetYTopForTrack(f.tidAt(4)) == TrackLanesView::kArrangementTimelineHeaderGutterPx,
           "anchor: the first member's strip sits exactly at the viewport top after collapsing");
    expect(f.verticalLayoutOk("anchored collapse"), "anchor: layout verifies at the kept offset");
    f.setCollapsed(*gid, false);

    // Collapse while scrolled to the very bottom: the shrunken content clamps the offset validly.
    f.lanes->scrollVerticallyToOffsetPx(f.lanes->verticalScrollModel().maxOffsetPx());
    f.setCollapsed(*gid, true);
    const TrackLanesView::VerticalScrollModel m = f.lanes->verticalScrollModel();
    expect(m.offsetPx >= 0 && m.offsetPx <= m.maxOffsetPx(),
           "clamp: collapsing at the bottom clamps the offset into the new valid range");
    expect(f.verticalLayoutOk("clamped collapse"), "clamp: layout verifies after the clamp");
}

// ---------------------------------------------------------------------------------------------
// §6: membership on track changes — Duplicate, delete + snapshot restore, dissolve, reorder veto
// ---------------------------------------------------------------------------------------------
void testMembershipOnTrackChanges()
{
    GroupFixture f(/*extraAudioTracks*/ 6);
    const TrackId tA = f.tidAt(1);
    const TrackId tB = f.tidAt(2);
    const TrackId tC = f.tidAt(3);
    const std::optional<int> gid = f.makeGroup({ 1, 2, 3 }, "Band");
    expect(gid.has_value(), "fixture: group {A,B,C}");

    // Duplicate a member: the copy lands right after its source INSIDE the group.
    const std::optional<TrackId> dup = f.session.duplicateTrack(tB);
    f.lanes->syncTracksFromSession();
    expect(dup.has_value()
               && f.session.getEffectiveVisualGroupMemberTrackIds(*gid)
                      == std::vector<TrackId>({ tA, tB, *dup, tC }),
           "duplicate: the copy joins the group right after its source (A, B, copy, C)");
    expect(f.session.isVisualTrackGroupDisplayable(*gid),
           "duplicate: the group stays displayable (contiguity preserved by insert-below-source)");

    // Delete a member: membership follows the snapshot; restoring the snapshot (undo) restores
    // membership with no extra bookkeeping (stale ids are tolerated in the stored list).
    const auto snapBeforeDelete = f.session.loadSessionSnapshotForAudioThread();
    f.session.removeTrack(*dup);
    f.lanes->syncTracksFromSession();
    expect(f.session.getEffectiveVisualGroupMemberTrackIds(*gid)
               == std::vector<TrackId>({ tA, tB, tC })
               && f.session.isVisualTrackGroupDisplayable(*gid),
           "delete: the removed member drops out of the effective membership");
    f.session.restoreSessionSnapshotForUndo(snapBeforeDelete);
    f.lanes->syncTracksFromSession();
    expect(f.session.getEffectiveVisualGroupMemberTrackIds(*gid)
               == std::vector<TrackId>({ tA, tB, *dup, tC }),
           "delete undo: restoring the snapshot restores the membership too (stale id tolerated)");
    f.session.removeTrack(*dup);
    f.lanes->syncTracksFromSession();

    // Below 2 effective members the group dissolves VISUALLY (safe fallback) and comes back when
    // the snapshot is restored.
    const auto snapBeforeDissolve = f.session.loadSessionSnapshotForAudioThread();
    f.session.removeTrack(tB);
    f.session.removeTrack(tC);
    f.lanes->syncTracksFromSession();
    expect(!f.session.isVisualTrackGroupDisplayable(*gid),
           "dissolve: with <2 remaining members the group stops displaying (rows render normal)");
    expect(f.verticalLayoutOk("dissolved"), "dissolve: layout verifies with the group dissolved");
    f.session.restoreSessionSnapshotForUndo(snapBeforeDissolve);
    f.lanes->syncTracksFromSession();
    expect(f.session.isVisualTrackGroupDisplayable(*gid)
               && f.session.getEffectiveVisualGroupMemberTrackIds(*gid)
                      == std::vector<TrackId>({ tA, tB, tC }),
           "dissolve undo: the snapshot restore brings the displayable group back");

    // Reorder veto: an outside track may not land inside the group, a member may not leave it.
    const TrackId outsider = f.tidAt(5);
    const auto intoMiddle = f.session.checkTrackMoveAgainstVisualGroups(
        outsider, f.session.getNumTracks() > 2 ? 2 : 1);
    expect(intoMiddle.has_value() && *intoMiddle == "Band",
           "reorder: dropping an outside track INTO the group is refused, naming the group");
    const auto memberOut = f.session.checkTrackMoveAgainstVisualGroups(tB, 5);
    expect(memberOut.has_value(), "reorder: moving a member OUT of the group is refused");

    const std::vector<TrackId> orderBefore = f.rowTrackIds();
    f.session.moveTrack(outsider, 2); // the Session-level defensive no-op behind the UI veto
    f.lanes->syncTracksFromSession();
    expect(f.rowTrackIds() == orderBefore,
           "reorder: Session::moveTrack is a no-op for a refused move (model order never diverges)");

    // A WITHIN-group reorder is allowed and keeps the group intact.
    expect(!f.session.checkTrackMoveAgainstVisualGroups(tA, 2).has_value(),
           "reorder: swapping members WITHIN the group passes the check");
    f.session.moveTrack(tA, 2);
    f.lanes->syncTracksFromSession();
    expect(f.session.getEffectiveVisualGroupMemberTrackIds(*gid)
               == std::vector<TrackId>({ tB, tA, tC })
               && f.session.isVisualTrackGroupDisplayable(*gid),
           "reorder: the within-group move lands (B, A, C) with the group still displayable");
    expect(f.rowTrackIds()[1] == tB && f.rowTrackIds()[2] == tA && f.rowTrackIds()[3] == tC,
           "reorder: the visible row order matches the session order exactly");

    // A move that keeps every group contiguous (outsider to the very end) is allowed.
    expect(!f.session
                .checkTrackMoveAgainstVisualGroups(outsider, f.session.getNumTracks() - 2)
                .has_value(),
           "reorder: a move that keeps the group contiguous passes");
}

// ---------------------------------------------------------------------------------------------
// §7: narrow create/rename/ungroup undo through the REAL UndoRedoCoordinator; collapse/expand is
// a display change that never records a step; the timeline (tracks + clips) is never touched.
// The coordinator's restore path republishes `withLocators(recorded)` (a new snapshot object), so
// timeline integrity across undo/redo is compared by CONTENT — pointer identity is asserted for
// the group commands themselves, which never publish a snapshot at all.
// ---------------------------------------------------------------------------------------------
[[nodiscard]] bool timelinesEqualByContent(const SessionSnapshot& a, const SessionSnapshot& b)
{
    if (a.getNumTracks() != b.getNumTracks())
    {
        return false;
    }
    for (int i = 0; i < a.getNumTracks(); ++i)
    {
        const Track& ta = a.getTrack(i);
        const Track& tb = b.getTrack(i);
        if (ta.getId() != tb.getId() || ta.getPlacedClips().size() != tb.getPlacedClips().size())
        {
            return false;
        }
        for (size_t c = 0; c < ta.getPlacedClips().size(); ++c)
        {
            if (ta.getPlacedClips()[c].getId() != tb.getPlacedClips()[c].getId()
                || ta.getPlacedClips()[c].getMaterial() != tb.getPlacedClips()[c].getMaterial())
            {
                return false;
            }
        }
    }
    return true;
}

void testNarrowUndoThroughRealCoordinator()
{
    const juce::File wav = writeToneWav("undo-take.wav");
    auto f = std::make_unique<GroupFixture>(/*extraAudioTracks*/ 4);
    Session& s = f->session;

    // A committed "take" the narrow group steps must never erase.
    expect(s.addRecordedTakeAtSample(wav, kRate, 0, f->tidAt(1), (std::int64_t)kRate).wasOk(),
           "fixture: committed audio take on a future group member");
    f->lanes->syncTracksFromSession();

    PluginInsertHost pluginHost;
    int dirtyCount = 0;
    int groupRefreshes = 0;
    UndoRedoCoordinator::Callbacks cb;
    cb.markProjectDirty = [&] { ++dirtyCount; };
    cb.refreshVisualTrackGroupsAfterUndoRestore = [&] {
        ++groupRefreshes;
        f->lanes->refreshVisualTrackGroupsFromSession();
    };
    UndoRedoCoordinator undo(s, pluginHost, std::move(cb));

    const auto timelineBefore = s.loadSessionSnapshotForAudioThread();
    const std::vector<TrackId> members = f->tidsAt({ 1, 2 });

    // Create (the exact wiring MainAppWindow uses for the Create hook). The group command itself
    // never publishes a snapshot: the live pointer is IDENTICAL right after the recorded edit.
    undo.executeUndoableVisualTrackGroupsEdit("Create track group", [&] {
        return s.createVisualTrackGroup("Undoable", members).has_value();
    });
    f->lanes->refreshVisualTrackGroupsFromSession();
    expect(s.getVisualTrackGroups().size() == 1 && dirtyCount == 1,
           "undo create: one group exists, the edit marked the project dirty once");
    expect(s.loadSessionSnapshotForAudioThread() == timelineBefore,
           "undo create: the recorded group edit left the snapshot pointer IDENTICAL");
    const int gid = s.getVisualTrackGroups().front().id;

    undo.invokeUndoFromWindowShortcut();
    expect(s.getVisualTrackGroups().empty(), "undo create: Ctrl+Z removes the group metadata");
    expect(groupRefreshes >= 1, "undo create: the group display refresh callback fired");
    expect(timelinesEqualByContent(*s.loadSessionSnapshotForAudioThread(), *timelineBefore),
           "undo create: tracks + the committed take are untouched (timeline equal by content)");
    undo.invokeRedoFromWindowShortcut();
    expect(s.getVisualTrackGroups().size() == 1 && s.findVisualTrackGroupById(gid) != nullptr,
           "redo create: the group (same id) is back");

    // Rename: one narrow step; a no-op rename records nothing.
    const int dirtyBeforeRename = dirtyCount;
    undo.executeUndoableVisualTrackGroupsEdit("Rename track group", [&] {
        s.renameVisualTrackGroup(gid, "Horns");
        return true;
    });
    expect(s.findVisualTrackGroupById(gid)->name == "Horns" && dirtyCount == dirtyBeforeRename + 1,
           "undo rename: the rename landed as one dirty-marking step");
    undo.executeUndoableVisualTrackGroupsEdit("Rename track group", [&] {
        s.renameVisualTrackGroup(gid, "Horns");
        return true;
    });
    expect(dirtyCount == dirtyBeforeRename + 1,
           "undo rename: an identical rename records NO step (list unchanged -> skipped)");
    undo.invokeUndoFromWindowShortcut();
    expect(s.findVisualTrackGroupById(gid)->name == "Undoable",
           "undo rename: Ctrl+Z restores the previous name");
    undo.invokeRedoFromWindowShortcut();
    expect(s.findVisualTrackGroupById(gid)->name == "Horns", "redo rename: the new name is back");

    // Collapse: a dirty-only display change that must NOT create an undo step. Ungroup afterwards
    // records collapsed=true on its BEFORE side, so undoing the ungroup restores the group
    // including the collapsed display it had when ungrouped.
    expect(s.setVisualTrackGroupCollapsed(gid, true), "collapse: session state flips to collapsed");
    f->lanes->refreshVisualTrackGroupsFromSession();
    const auto beforeUngroup = s.loadSessionSnapshotForAudioThread();
    undo.executeUndoableVisualTrackGroupsEdit("Ungroup tracks", [&] {
        s.removeVisualTrackGroup(gid);
        return true;
    });
    f->lanes->refreshVisualTrackGroupsFromSession();
    expect(s.getVisualTrackGroups().empty(), "ungroup: the group metadata is removed");
    expect(s.loadSessionSnapshotForAudioThread() == beforeUngroup,
           "ungroup: tracks and clips untouched (identical snapshot pointer across the command)");
    undo.invokeUndoFromWindowShortcut();
    expect(s.findVisualTrackGroupById(gid) != nullptr
               && s.findVisualTrackGroupById(gid)->name == "Horns"
               && s.findVisualTrackGroupById(gid)->collapsed,
           "ungroup + Ctrl+Z: the group returns with its name AND collapsed display state");

    // The next Ctrl+Z goes straight to the RENAME step: the collapse between rename and ungroup
    // recorded no entry of its own. (The restored before-side is the full group list as recorded
    // at rename time, so the collapsed flag reverts with it - a bounded metadata restore.)
    undo.invokeUndoFromWindowShortcut();
    expect(s.findVisualTrackGroupById(gid) != nullptr
               && s.findVisualTrackGroupById(gid)->name == "Undoable",
           "second Ctrl+Z: undoes the rename directly - the collapse recorded NO undo entry");
    expect(timelinesEqualByContent(*s.loadSessionSnapshotForAudioThread(), *timelineBefore),
           "after all group undo/redo: the timeline content NEVER changed");
}

// ---------------------------------------------------------------------------------------------
// §7: v27 persistence — round trip, normal heights on disk, drops, pre-v27, malformed metadata
// ---------------------------------------------------------------------------------------------
void testPersistenceRoundTripAndOlderProjects()
{
    const juce::File proj = tempDir().getChildFile("groups.dalproj");
    (void)proj.deleteFile();

    std::vector<TrackId> g1Members;
    std::vector<TrackId> g2Members;
    {
        GroupFixture f(/*extraAudioTracks*/ 6);
        g1Members = f.tidsAt({ 1, 2, 3 });
        g2Members = f.tidsAt({ 5, 6 });
        const std::optional<int> g1 = f.makeGroup({ 1, 2, 3 }, "Drums");
        const std::optional<int> g2 = f.makeGroup({ 5, 6 }, "Vocals");
        f.lanes->setTrackRowHeightPxForStabilityTest(g1Members[1], 150);
        f.setCollapsed(*g1, true); // saved collapsed, with the 150 px NORMAL height intact

        // A group that is no longer displayable must be dropped on save.
        const std::optional<int> dead = f.session.createVisualTrackGroup(
            "Dead", { f.tidAt(4), g2Members[0] });
        juce::ignoreUnused(dead);
        expect(g1.has_value() && g2.has_value() && !dead.has_value(),
               "save fixture: two displayable groups; the overlapping third refused at creation");
        f.session.removeTrack(g2Members[1]); // Vocals drops to ONE member -> not displayable
        f.lanes->syncTracksFromSession();
        expect(!f.session.isVisualTrackGroupDisplayable(*g2),
               "save fixture: \"Vocals\" is no longer displayable after the member deletion");

        ProjectFileTrackRowHeightsV1 rh;
        rh.presetKey = trh::persistenceKeyForPreset(f.lanes->lastChosenTrackRowHeightPreset());
        rh.perTrackRowHeightPx = f.lanes->allTrackRowHeightsPxForProjectSave();
        const juce::Result saved = f.session.saveProjectToFile(
            f.transport, proj, kRate, nullptr, {}, false, "1_4", std::nullopt, std::nullopt,
            std::nullopt, rh);
        expect(saved.wasOk(), "save: project written (" + saved.getErrorMessage() + ")");
    }

    // Raw file: v27 with exactly the ONE displayable group; member heights are normal heights.
    ProjectFileV1 parsed;
    expect(readProjectFile(proj, parsed).wasOk(), "read: raw readProjectFile succeeds");
    expect(parsed.version == ProjectFileV1::kCurrentVersion && parsed.version >= 27,
           "read: file version is current (>= 27)");
    expect(parsed.visualTrackGroups.size() == 1 && parsed.visualTrackGroups[0].name == "Drums"
               && parsed.visualTrackGroups[0].collapsed
               && parsed.visualTrackGroups[0].memberTrackIds == g1Members,
           "read: exactly the displayable group is stored (name, collapsed, members in order); "
           "the dissolved \"Vocals\" group was dropped");
    int collapsedMemberStoredPx = 0;
    for (const auto& t : parsed.tracks)
    {
        if (t.id == g1Members[1])
        {
            collapsedMemberStoredPx = t.rowHeightPx;
        }
    }
    expect(collapsedMemberStoredPx == 150,
           "read: a collapsed member's rowHeight on disk is its NORMAL height (150, never 4)");

    // Full reload through the production load path: adoption + collapsed display from the file.
    {
        GroupFixture f(/*extraAudioTracks*/ 0);
        juce::StringArray skipped;
        juce::String note;
        expect(f.session.loadProjectFromFile(f.transport, proj, kRate, skipped, note, nullptr).wasOk(),
               "reload: project loads");
        f.lanes->syncTracksFromSession();
        std::vector<std::pair<TrackId, int>> perTrack;
        for (const auto& t : parsed.tracks)
        {
            perTrack.emplace_back(t.id, t.rowHeightPx);
        }
        f.lanes->applyTrackRowHeightsFromLoadedProject(parsed.trackRowHeightPreset, perTrack);
        f.lanes->refreshVisualTrackGroupsFromSession();

        const auto& groups = f.session.getVisualTrackGroups();
        expect(groups.size() == 1 && groups[0].name == "Drums" && groups[0].collapsed
                   && groups[0].memberTrackIds == g1Members
                   && f.session.isVisualTrackGroupDisplayable(groups[0].id),
               "reload: the group is adopted displayable with name, members and collapsed state");
        const std::map<TrackId, int> h = f.rowHeights();
        expect(h.at(g1Members[0]) == 4 && h.at(g1Members[1]) == 4 && h.at(g1Members[2]) == 4,
               "reload: the collapsed group displays its members at 4 px immediately");
        f.setCollapsed(groups[0].id, false);
        expect(f.rowHeights().at(g1Members[1]) == 150,
               "reload + expand: the member's 150 px NORMAL height survived the round trip");
        expect(f.verticalLayoutOk("after reload"), "reload: layout verifies");
    }

    // Pre-v27 file: same honest construction as the row-height tests — save without groups, then
    // rewrite the version text. Loading yields NO groups.
    {
        const juce::File oldProj = tempDir().getChildFile("pre-v27.dalproj");
        (void)oldProj.deleteFile();
        {
            GroupFixture f;
            expect(f.session.saveProjectToFile(f.transport, oldProj, kRate).wasOk(),
                   "pre-v27: base project written");
        }
        const juce::String text = oldProj.loadFileAsString();
        expect(!text.contains("visualTrackGroups"),
               "pre-v27: a save without groups emits NO visualTrackGroups key");
        const juce::String cur = juce::String(ProjectFileV1::kCurrentVersion);
        const juce::String asV26 = text.replace("\"version\": " + cur, "\"version\": 26")
                                       .replace("\"version\":" + cur, "\"version\":26");
        expect(asV26 != text && oldProj.replaceWithText(asV26), "pre-v27: version rewritten to 26");

        GroupFixture f;
        juce::StringArray skipped;
        juce::String note;
        expect(f.session.loadProjectFromFile(f.transport, oldProj, kRate, skipped, note, nullptr).wasOk(),
               "pre-v27: the v26 file loads");
        f.lanes->syncTracksFromSession();
        f.lanes->refreshVisualTrackGroupsFromSession();
        expect(f.session.getVisualTrackGroups().empty(),
               "pre-v27: an older project simply has no groups");
        expect(f.verticalLayoutOk("pre-v27"), "pre-v27: all rows render as normal tracks");
        (void)oldProj.deleteFile();
    }
    (void)proj.deleteFile();
}

void testMalformedGroupMetadataSafeFallback()
{
    const juce::File badProj = tempDir().getChildFile("malformed-groups.dalproj");
    // Tracks 1..6 audio + master 90. The group list mixes one valid group, every §7 failure mode,
    // and plain garbage. Safe fallback = drop the invalid entries, keep tracks as normal rows,
    // never fail the read, never touch musical data.
    const juce::String json =
        "{ \"version\": 27, \"nextPlacedClipId\": 50, \"nextTrackId\": 99, \"activeTrackId\": 1,"
        " \"playheadSamples\": 0, \"deviceSampleRateAtSave\": 48000.0,"
        " \"tracks\": ["
        "  { \"id\": 1, \"name\": \"T1\", \"kind\": \"audio\" },"
        "  { \"id\": 2, \"name\": \"T2\", \"kind\": \"audio\" },"
        "  { \"id\": 3, \"name\": \"T3\", \"kind\": \"audio\" },"
        "  { \"id\": 4, \"name\": \"T4\", \"kind\": \"audio\" },"
        "  { \"id\": 5, \"name\": \"T5\", \"kind\": \"audio\" },"
        "  { \"id\": 6, \"name\": \"T6\", \"kind\": \"audio\" },"
        "  { \"id\": 90, \"name\": \"Stereo Out\", \"kind\": \"master\" } ],"
        " \"visualTrackGroups\": ["
        "  { \"name\": \"Valid\", \"collapsed\": true, \"memberTrackIds\": [2, 3] },"
        "  { \"name\": \"NonContig\", \"memberTrackIds\": [4, 6] },"
        "  { \"name\": \"Overlap\", \"memberTrackIds\": [3, 4] },"
        "  { \"name\": \"WithMaster\", \"memberTrackIds\": [6, 90] },"
        "  { \"name\": \"Single\", \"memberTrackIds\": [5] },"
        "  { \"name\": \"Ghost\", \"memberTrackIds\": [77, 78] },"
        "  { \"memberTrackIds\": [5, 6], \"collapsed\": \"yes\" },"
        "  \"garbage\","
        "  { \"name\": 12, \"memberTrackIds\": \"zzz\" } ] }";
    expect(badProj.replaceWithText(json), "malformed: handcrafted project written");

    ProjectFileV1 parsed;
    const juce::Result read = readProjectFile(badProj, parsed);
    expect(read.wasOk(),
           "malformed: the reader NEVER fails on bad group metadata (" + read.getErrorMessage() + ")");

    GroupFixture f;
    juce::StringArray skipped;
    juce::String note;
    expect(f.session.loadProjectFromFile(f.transport, badProj, kRate, skipped, note, nullptr).wasOk(),
           "malformed: the full load path succeeds");
    f.lanes->syncTracksFromSession();
    f.lanes->refreshVisualTrackGroupsFromSession();

    const auto& groups = f.session.getVisualTrackGroups();
    expect(groups.size() == 2,
           "malformed: exactly the two salvageable groups are adopted ("
               + juce::String((int)groups.size()) + ")");
    expect(groups.size() == 2 && groups[0].name == "Valid" && groups[0].collapsed
               && groups[0].memberTrackIds == std::vector<TrackId>({ 2, 3 }),
           "malformed: \"Valid\" (2,3) is adopted collapsed");
    expect(groups.size() == 2 && groups[1].memberTrackIds == std::vector<TrackId>({ 5, 6 })
               && groups[1].name.isNotEmpty() && !groups[1].collapsed,
           "malformed: the name-less (5,6) group gets a default name; the bogus collapsed value "
           "reads as expanded");
    expect(f.session.getNumTracks() == 7,
           "malformed: all 7 tracks are present - layout metadata repair NEVER drops tracks");
    expect(f.rowTrackIds().size() == 7 && f.verticalLayoutOk("malformed"),
           "malformed: every track renders (invalid groups fall back to normal visible rows)");
    (void)badProj.deleteFile();
}
// ---------------------------------------------------------------------------------------------
// Track colours (compact-header slice): narrow undo through the real coordinator, immediate lane repaint from the
// cached raster, Duplicate inheritance, v28 persistence, pre-v28 / unknown keys -> Default grey.
// ---------------------------------------------------------------------------------------------
[[nodiscard]] int countExactColour(const juce::Image& img,
                                   const juce::Rectangle<int>& area,
                                   const juce::Colour colour)
{
    int n = 0;
    const juce::Rectangle<int> r = area.getIntersection(img.getBounds());
    for (int y = r.getY(); y < r.getBottom(); ++y)
    {
        for (int x = r.getX(); x < r.getRight(); ++x)
        {
            if (img.getPixelAt(x, y) == colour)
            {
                ++n;
            }
        }
    }
    return n;
}

void testTrackColourNarrowUndoRepaintAndPersistence()
{
    const juce::File wav = writeToneWav("colour-take.wav", 1.0);
    auto f = std::make_unique<GroupFixture>(/*extraAudioTracks*/ 3);
    Session& s = f->session;
    f->lanes->setSize(900, 400);
    const TrackId tA = f->tidAt(0);
    const TrackId tB = f->tidAt(1);
    const TrackId tC = f->tidAt(2); // stays uncoloured (Duplicate below inserts right after B)
    s.setActiveTrack(tA);
    expect(s.addRecordedTakeAtSample(wav, kRate, 0, tB, (std::int64_t)kRate).wasOk(),
           "colour fixture: a committed take on the NON-active track B");
    f->lanes->syncTracksFromSession();
    f->lanes->applyTrackRowHeightPreset(trh::TrackRowHeightPreset::Medium);

    PluginInsertHost pluginHost;
    int dirtyCount = 0;
    int colourRefreshes = 0;
    UndoRedoCoordinator::Callbacks cb;
    cb.markProjectDirty = [&] { ++dirtyCount; };
    cb.refreshTrackColoursAfterUndoRestore = [&] {
        ++colourRefreshes;
        f->lanes->refreshTrackColoursFromSession();
    };
    UndoRedoCoordinator undo(s, pluginHost, std::move(cb));
    const auto timelineBefore = s.loadSessionSnapshotForAudioThread();

    // The lane body of track B's clip, in lanes coordinates: below the waveform band (the tone is
    // painted at full display gain around the lane centre) so the sampled pixels are pure body fill.
    const auto clipBodyProbe = [&] {
        const int top = f->widgetYTopForTrack(tB);
        const int x0 = f->lanes->headerColumnWidthPx() + 10;
        return juce::Rectangle<int>(x0, top + trh::kMediumPresetPx - 22, 60, 12);
    };
    const juce::Image before = f->lanes->createComponentSnapshot(f->lanes->getLocalBounds(), false, 1.0f);
    expect(countExactColour(before, clipBodyProbe(), track_colour_palette::eventBodyFill(TrackColourKey::DefaultGrey)) > 300,
           "colour: before any change the clip body paints in the historic default grey-blue");

    // Command on the right-clicked (non-active) track: narrow step, no snapshot publish.
    undo.executeUndoableTrackColourEdit(tB, TrackColourKey::Red);
    expect(s.getTrackColour(tB) == TrackColourKey::Red && s.getTrackColour(tA) == TrackColourKey::DefaultGrey
               && s.getActiveTrackId() == tA,
           "colour: the edit colours exactly the requested track B (active track A untouched, still active)");
    expect(dirtyCount == 1 && colourRefreshes == 1,
           "colour: one dirty mark + one colour refresh for the edit");
    expect(s.loadSessionSnapshotForAudioThread() == timelineBefore,
           "colour: the colour edit left the timeline snapshot pointer IDENTICAL (no musical data touched)");
    const juce::Image after = f->lanes->createComponentSnapshot(f->lanes->getLocalBounds(), false, 1.0f);
    expect(countExactColour(after, clipBodyProbe(), track_colour_palette::eventBodyFill(TrackColourKey::Red)) > 300
               && countExactColour(after, clipBodyProbe(), track_colour_palette::eventBodyFill(TrackColourKey::DefaultGrey)) == 0,
           "colour: the very next paint shows the red event body (cached raster invalidated immediately)");
    const int headerSeg = countExactColour(after, f->lanes->getLocalBounds().withWidth(f->lanes->headerColumnWidthPx())
                                                      .withY(f->widgetYTopForTrack(tB)).withHeight(trh::kMediumPresetPx),
                                           track_colour_palette::headerSegmentFill(TrackColourKey::Red));
    expect(headerSeg > 200, "colour: track B's header colour segment paints in the red segment fill");

    // Same colour again: no step; undo -> grey; redo -> red. Only the colour moves.
    undo.executeUndoableTrackColourEdit(tB, TrackColourKey::Red);
    expect(dirtyCount == 1, "colour: re-applying the same colour records NO step");
    undo.invokeUndoFromWindowShortcut();
    expect(s.getTrackColour(tB) == TrackColourKey::DefaultGrey && colourRefreshes >= 2,
           "colour undo: Ctrl+Z restores Default grey and refreshes the display");
    expect(timelinesEqualByContent(*s.loadSessionSnapshotForAudioThread(), *timelineBefore),
           "colour undo: the take and tracks are untouched (timeline equal by content)");
    undo.invokeRedoFromWindowShortcut();
    expect(s.getTrackColour(tB) == TrackColourKey::Red, "colour redo: red is back");

    // Duplicate inherits the source colour.
    const std::optional<TrackId> dup = s.duplicateTrack(tB);
    expect(dup.has_value() && s.getTrackColour(*dup) == TrackColourKey::Red,
           "colour: Duplicate inherits the source track's colour");

    // v28 persistence: only non-default colours are written; reload restores them.
    // (A clip-less sibling fixture with the same track ids: project saves require clip audio inside
    // the project's Audio folder, which is not what this colour slice is about.)
    const juce::File proj = tempDir().getChildFile("track-colours.dalproj");
    (void)proj.deleteFile();
    GroupFixture pf(/*extraAudioTracks*/ 3);
    expect(pf.tidAt(0) == tA && pf.tidAt(1) == tB, "colour save fixture: same track ids as the live fixture");
    const std::optional<TrackId> pdup = pf.session.duplicateTrack(tB);
    expect(pdup.has_value() && pf.session.setTrackColour(tA, TrackColourKey::Teal)
               && pf.session.setTrackColour(tB, TrackColourKey::Red) && pf.session.setTrackColour(*pdup, TrackColourKey::Red),
           "colour save fixture: A teal, B red, duplicate red");
    const juce::Result saveRes = pf.session.saveProjectToFile(pf.transport, proj, kRate);
    expect(saveRes.wasOk(), "colour save: project written (" + saveRes.getErrorMessage() + ")");
    ProjectFileV1 parsed;
    expect(readProjectFile(proj, parsed).wasOk() && parsed.version == ProjectFileV1::kCurrentVersion
               && parsed.version >= 28,
           "colour save: the file is the current (>= 28) schema");
    int tealCount = 0, redCount = 0, emptyCount = 0;
    for (const auto& t : parsed.tracks)
    {
        if (t.colourKey == "teal") ++tealCount;
        else if (t.colourKey == "red") ++redCount;
        else if (t.colourKey.isEmpty()) ++emptyCount;
    }
    expect(tealCount == 1 && redCount == 2 && emptyCount == (int)parsed.tracks.size() - 3,
           "colour save: teal x1 (A), red x2 (B + duplicate), every other track has NO colour key");
    {
        GroupFixture g(/*extraAudioTracks*/ 0);
        juce::StringArray skipped;
        juce::String note;
        expect(g.session.loadProjectFromFile(g.transport, proj, kRate, skipped, note, nullptr).wasOk(),
               "colour reload: project loads");
        expect(g.session.getTrackColour(tA) == TrackColourKey::Teal && g.session.getTrackColour(tB) == TrackColourKey::Red
                   && g.session.getTrackColour(*pdup) == TrackColourKey::Red
                   && g.session.getTrackColour(tC) == TrackColourKey::DefaultGrey,
               "colour reload: colours restored per track id; uncoloured tracks stay Default grey");
    }

    // Pre-v28 file (no colour keys, version text rewritten) -> every track Default grey.
    {
        const juce::String text = proj.loadFileAsString();
        const juce::String cur = juce::String(ProjectFileV1::kCurrentVersion);
        juce::String asV27 = text.replace("\"version\": " + cur, "\"version\": 27")
                                 .replace("\"version\":" + cur, "\"version\":27");
        asV27 = asV27.replace("\"colour\": \"teal\",", "").replace("\"colour\": \"red\",", "")
                     .replace("\"colour\":\"teal\",", "").replace("\"colour\":\"red\",", "");
        const juce::File oldProj = tempDir().getChildFile("pre-v28-colours.dalproj");
        expect(asV27 != text && !asV27.contains("\"colour\"") && oldProj.replaceWithText(asV27),
               "pre-v28: version rewritten to 27 and colour keys stripped");
        GroupFixture g(/*extraAudioTracks*/ 0);
        juce::StringArray skipped;
        juce::String note;
        expect(g.session.loadProjectFromFile(g.transport, oldProj, kRate, skipped, note, nullptr).wasOk(),
               "pre-v28: the v27 file loads");
        expect(g.session.getTrackColour(tA) == TrackColourKey::DefaultGrey
                   && g.session.getTrackColour(tB) == TrackColourKey::DefaultGrey,
               "pre-v28: older projects simply show Default grey everywhere");
        (void)oldProj.deleteFile();
    }

    // Unknown / malformed colour values -> Default grey, never a failed read.
    {
        const juce::File badProj = tempDir().getChildFile("bad-colours.dalproj");
        const juce::String json =
            "{ \"version\": 28, \"nextPlacedClipId\": 50, \"nextTrackId\": 99, \"activeTrackId\": 1,"
            " \"playheadSamples\": 0, \"deviceSampleRateAtSave\": 48000.0,"
            " \"tracks\": ["
            "  { \"id\": 1, \"name\": \"T1\", \"kind\": \"audio\", \"colour\": \"neon-pink\" },"
            "  { \"id\": 2, \"name\": \"T2\", \"kind\": \"audio\", \"colour\": 7 },"
            "  { \"id\": 3, \"name\": \"T3\", \"kind\": \"audio\", \"colour\": \"purple\" },"
            "  { \"id\": 90, \"name\": \"Stereo Out\", \"kind\": \"master\" } ] }";
        expect(badProj.replaceWithText(json), "bad colours: handcrafted project written");
        GroupFixture g(/*extraAudioTracks*/ 0);
        juce::StringArray skipped;
        juce::String note;
        expect(g.session.loadProjectFromFile(g.transport, badProj, kRate, skipped, note, nullptr).wasOk(),
               "bad colours: the load NEVER fails on unknown colour values");
        expect(g.session.getTrackColour(1) == TrackColourKey::DefaultGrey
                   && g.session.getTrackColour(2) == TrackColourKey::DefaultGrey
                   && g.session.getTrackColour(3) == TrackColourKey::Purple,
               "bad colours: unknown / non-string values fall back to Default grey; valid keys load");
        (void)badProj.deleteFile();
    }
    (void)proj.deleteFile();
}

// ---------------------------------------------------------------------------------------------
// --render <dir>: reproducible example images for the feature report / PR — the SAME group
// rendered expanded and collapsed, in an 8-track and a 16-track arrangement (offscreen snapshots
// of the production TrackLanesView; the handler-level verification above is what proves behavior,
// these images just show the result).
// ---------------------------------------------------------------------------------------------
void writePng(const juce::Image& img, const juce::File& file)
{
    (void)file.deleteFile();
    juce::FileOutputStream os(file);
    if (os.openedOk())
    {
        juce::PNGImageFormat().writeImageToStream(img, os);
        info("wrote " + file.getFullPathName());
    }
}

void renderGroupExample(const juce::File& outDir,
                        const juce::String& baseName,
                        const int extraAudioTracks,
                        const std::vector<int>& groupIndices,
                        const juce::String& groupName,
                        const trh::TrackRowHeightPreset preset,
                        const int viewHeightPx)
{
    const juce::File wav = writeToneWav("render-tone.wav", 4.0);
    GroupFixture f(extraAudioTracks);
    f.lanes->setSize(1100, viewHeightPx);
    f.lanes->applyTrackRowHeightPreset(preset);

    // Scatter clips so the collapsed strips show distinct interval patterns per member.
    const auto sec = [](const double s) { return (std::int64_t)std::llround(s * kRate); };
    const std::vector<TrackId> rows = f.rowTrackIds();
    for (size_t i = 0; i + 1 < rows.size(); ++i) // skip Stereo Out
    {
        const double start = 0.35 * (double)(i % 7);
        const double len = 1.0 + 0.5 * (double)(i % 4);
        if (i % 5 != 4) // every fifth row stays empty
        {
            (void)f.session.addRecordedTakeAtSample(wav, kRate, sec(start), rows[i], sec(len));
            (void)f.session.addRecordedTakeAtSample(wav, kRate, sec(start + len + 0.8), rows[i],
                                                    sec(juce::jmax(0.5, 2.2 - len)));
        }
    }
    f.lanes->syncTracksFromSession();

    const std::optional<int> gid = f.makeGroup(groupIndices, groupName);
    if (!gid.has_value())
    {
        info("render: group creation failed for " + baseName);
        return;
    }
    writePng(f.lanes->createComponentSnapshot(f.lanes->getLocalBounds(), false, 1.0f),
             outDir.getChildFile(baseName + "-expanded.png"));
    f.setCollapsed(*gid, true);
    writePng(f.lanes->createComponentSnapshot(f.lanes->getLocalBounds(), false, 1.0f),
             outDir.getChildFile(baseName + "-collapsed.png"));
}

/// Compact-header example: several row kinds (audio, MIDI, Group bus, Stereo Out) with
/// track colours, 3-digit-wide numbers, long names, clips, and a collapsed group — rendered once
/// per preset (Micro / Mini / Small / Medium) plus one mixed-heights image.
void renderCompactLayoutExample(const juce::File& outDir)
{
    const juce::File wav = writeToneWav("render-tone.wav", 4.0);
    GroupFixture f(/*extraAudioTracks*/ 7, /*withGroupBus*/ true);
    (void)f.session.addMidiTrack();
    (void)f.session.addMidiTrack();
    f.lanes->syncTracksFromSession();
    f.lanes->setSize(1000, 760);

    const auto sec = [](const double s) { return (std::int64_t)std::llround(s * kRate); };
    const TrackColourKey colours[] = { TrackColourKey::Blue,  TrackColourKey::Teal,
                                       TrackColourKey::Green, TrackColourKey::Ochre,
                                       TrackColourKey::Orange, TrackColourKey::Red,
                                       TrackColourKey::Purple, TrackColourKey::DefaultGrey };
    const int n = f.session.getNumTracks();
    for (int i = 0; i < n; ++i)
    {
        const TrackId tid = f.session.getTrackIdAtIndex(i);
        const TrackKind kind = f.session.getTrackKindAtIndex(i);
        if (kind == TrackKind::Audio)
        {
            f.session.setTrackName(tid, i % 3 == 0 ? "A very long audio track name that must ellipsize"
                                                   : "Audio " + juce::String(i + 1));
            (void)f.session.addRecordedTakeAtSample(wav, kRate, sec(0.3 * (double)(i % 5)), tid,
                                                    sec(1.2 + 0.4 * (double)(i % 3)));
            (void)f.session.addRecordedTakeAtSample(wav, kRate, sec(3.2 + 0.2 * (double)(i % 4)), tid,
                                                    sec(0.9));
        }
        else if (kind == TrackKind::Midi)
        {
            f.session.setTrackName(tid, "MIDI out " + juce::String(i + 1));
        }
        if (kind != TrackKind::Master)
        {
            (void)f.session.setTrackColour(tid, colours[i % 8]);
        }
    }
    const std::vector<TrackId> rows = f.rowTrackIds();
    f.lanes->syncTracksFromSession();
    f.lanes->refreshTrackColoursFromSession();

    // Group of three audio rows (indices 2..4), collapsed in every image.
    const std::optional<int> gid = f.makeGroup({ 2, 3, 4 }, "Drum bus group");
    if (!gid.has_value())
    {
        info("render: compact example group creation failed");
        return;
    }
    f.setCollapsed(*gid, true);

    const std::pair<const char*, trh::TrackRowHeightPreset> presets[]
        = { { "micro", trh::TrackRowHeightPreset::Micro }, { "mini", trh::TrackRowHeightPreset::Mini },
            { "small", trh::TrackRowHeightPreset::Small }, { "medium", trh::TrackRowHeightPreset::Medium } };
    // The lane raster cache rebuilds on a deferred timer after a resize (stale rasters are blitted
    // scaled meanwhile - the zoom-freeze fix); pump the message loop so the images show the final
    // per-height painting, exactly as the app does a few hundred ms after a height change.
    // Offscreen components only paint inside createComponentSnapshot: the first snapshot takes
    // the geometry-stale blit path and arms the deferred rebuild, the pumped loop runs it, and
    // the second snapshot is the one written out.
    const auto settle = [&f] {
        (void)f.lanes->createComponentSnapshot(f.lanes->getLocalBounds(), false, 1.0f);
        juce::MessageManager::getInstance()->runDispatchLoopUntil(700);
    };
    for (const auto& [name, preset] : presets)
    {
        f.lanes->applyTrackRowHeightPreset(preset);
        const int contentH = juce::jmin(760, f.lanes->verticalScrollModel().contentHeightPx
                                                 + TrackLanesView::kArrangementTimelineHeaderGutterPx + 24);
        f.lanes->setSize(1000, contentH);
        settle();
        writePng(f.lanes->createComponentSnapshot(f.lanes->getLocalBounds(), false, 1.0f),
                 outDir.getChildFile("compact-" + juce::String(name) + "-collapsed-group.png"));
    }
    // Mixed heights: the first row at Micro, then Mini, Small, Medium, Large, with the group
    // expanded so the inline / compact handle placement is visible between unequal rows.
    f.setCollapsed(*gid, false);
    f.lanes->applyTrackRowHeightPreset(trh::TrackRowHeightPreset::Medium);
    const int mixed[] = { trh::kMicroPresetPx, trh::kMiniPresetPx, trh::kSmallPresetPx, trh::kMediumPresetPx,
                          trh::kLargePresetPx, trh::kMicroPresetPx, trh::kMicroPresetPx, trh::kSmallPresetPx };
    for (size_t i = 0; i < rows.size() && i < 8; ++i)
    {
        f.lanes->setTrackRowHeightPxForStabilityTest(rows[i], mixed[i]);
    }
    f.lanes->setSize(1000, 760);
    settle();
    writePng(f.lanes->createComponentSnapshot(f.lanes->getLocalBounds(), false, 1.0f),
             outDir.getChildFile("compact-mixed-heights-expanded-group.png"));
}

int runRenderMode(const juce::File& outDir)
{
    (void)outDir.createDirectory();
    renderCompactLayoutExample(outDir);
    // 8 arrangement tracks (7 audio + Stereo Out), Medium heights, 4-member "Drums" group.
    renderGroupExample(outDir, "group-8-tracks", /*extraAudioTracks*/ 6, { 1, 2, 3, 4 }, "Drums",
                       trh::TrackRowHeightPreset::Medium, 820);
    // 16 arrangement tracks (15 audio + Stereo Out), Small heights, 8-member "Percussion" group.
    renderGroupExample(outDir, "group-16-tracks", /*extraAudioTracks*/ 14, { 2, 3, 4, 5, 6, 7, 8, 9 },
                       "Percussion", trh::TrackRowHeightPreset::Small, 1080);
    return 0;
}
} // namespace

int main(int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI juceGui;
    if (argc >= 3 && juce::String(argv[1]) == "--render")
    {
        return runRenderMode(juce::File(juce::String(argv[2])));
    }
    info("CollapsibleTrackGroupFocusedTests - purely visual collapsible track groups (spec slice)");

    testHeaderSelectionAndCreateValidation();
    testCollapsedLayoutExactFourPx();
    testCollapsedMiniViewClipIntervals();
    testClipLessMemberStripsStayEmpty();
    testNormalHeightsSeparateFromCollapsedDisplay();
    testHandlePlacementAndGestures();
    testScrollAnchoringAndClamp();
    testMembershipOnTrackChanges();
    testNarrowUndoThroughRealCoordinator();
    testPersistenceRoundTripAndOlderProjects();
    testMalformedGroupMetadataSafeFallback();
    testTrackColourNarrowUndoRepaintAndPersistence();

    std::printf("\n%d checks, %d failure(s)\n", checks, failures);
    return failures == 0 ? 0 : 1;
}

// ---------------------------------------------------------------------------
// Link seams — the linked production sources reference these instrument entry points, but this
// harness registers no instrument timeline attachments and creates only instrument SHELL rows,
// so the stubs are never executed.
// ---------------------------------------------------------------------------
#include "instruments/InstrumentTrackController.h"
#include "plugins/ExperimentalInstrumentHost.h"

ProjectFileExperimentalInstrumentTrackV1
InstrumentTrackController::buildExperimentalInstrumentProjectBlock() const
{
    jassertfalse;
    return {};
}
juce::String InstrumentTrackController::getLaneHeaderSubtitle() const
{
    jassertfalse;
    return {};
}
void ExperimentalInstrumentHost::audioThread_processBlockAndAddToOutputs(
    float* const*, int, int, float, float) noexcept {}

// UndoRedoCoordinator calls the stability-invariants check after each undo/redo. The production
// implementation pulls in the whole PlaybackEngine; this harness registers no invariant
// providers, in which state the real function trivially passes too.
#include "diagnostics/StabilityInvariants.h"
namespace stability_invariants
{
bool runRegisteredStabilityInvariantsCheck(const juce::String&) { return true; }
} // namespace stability_invariants
