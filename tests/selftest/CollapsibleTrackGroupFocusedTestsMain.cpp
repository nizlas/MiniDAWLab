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
#include "ui/TimelineClipEventChrome.h"
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
// §4 + §5: collapsed layout = one block of `collapsedGroupBlockHeightPx(n)` (4 px strips with
// 1 px gaps, centred; at least the readable header minimum), shared scroll model, no hidden hit
// targets
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

    // Three members: strips content 3 x 4 + 2 x 1 = 14 px, block = the readable header minimum
    // (25 px), strips block centred (pad 5 above / 6 below): member display heights 10 / 5 / 10.
    constexpr int kBlock = TrackLanesView::collapsedGroupBlockHeightPx(3);
    constexpr int kPad = TrackLanesView::collapsedGroupStripsTopPadPx(3);
    static_assert(TrackLanesView::collapsedGroupStripsContentHeightPx(3) == 14 && kBlock == 25 && kPad == 5,
                  "3-member collapsed block: 14 px of strips inside the 25 px readable minimum");
    const std::map<TrackId, int> h = f.rowHeights();
    expect(h.at(rows[4]) + h.at(rows[5]) + h.at(rows[6]) == kBlock && h.at(rows[4]) == kPad + 4 + 1
               && h.at(rows[5]) == 4 + 1 && h.at(rows[6]) == 4 + (kBlock - 14 - kPad),
           "collapse: the members' display heights sum to the block height (25) — 4 px strips, 1 px "
           "gaps, centring pads on the first / last member (the Micro minimum does not apply)");
    expect(h.at(rows[0]) == trh::kMediumPresetPx && h.at(rows[1]) == trh::kMediumPresetPx,
           "collapse: rows outside the group keep their heights");
    expect(f.lanes->verticalScrollModel().contentHeightPx == contentExpanded - (64 + 150 + 192) + kBlock,
           "collapse: scrollbar content height shrinks by exactly the members' heights minus the block");
    expect(f.lanes->rowTopOffsetPxForTrackForDiagnostics(rows[5])
                   - f.lanes->rowTopOffsetPxForTrackForDiagnostics(rows[4])
               == kPad + 4 + 1
               && f.lanes->rowTopOffsetPxForTrackForDiagnostics(rows[6])
                          - f.lanes->rowTopOffsetPxForTrackForDiagnostics(rows[5])
                      == 4 + 1,
           "collapse: consecutive strips are exactly 5 px apart (4 px strip + 1 px gap)");
    expect(f.verticalLayoutOk("collapsed"), "collapse: the shared vertical layout model verifies");
    expect(f.session.getActiveTrackId() != kInvalidTrackId,
           "collapse: a valid active track remains (never cleared by collapsing)");

    // No hidden hit targets: points inside the collapsed block hit the lanes view itself (which
    // paints the strips), never a member header, lane, clip or button. The chevron button and the
    // name label at the block's top are the only interactive elements — and they are laid out
    // INSIDE the block (readable header), never over the row above.
    const int headerW = f.lanes->headerColumnWidthPx();
    const int blockTop = f.widgetYTopForTrack(rows[4]);
    const juce::Rectangle<int> button = f.lanes->visualGroupHandleBoundsForTest(*gid);
    const juce::Rectangle<int> label = f.lanes->visualGroupNameLabelBoundsForTest(*gid);
    expect(button == juce::Rectangle<int>(0, blockTop + TrackLanesView::kVisualGroupCollapsedTopPadPx,
                                          TrackLanesView::kVisualGroupButtonWidthPx, TrackLanesView::kVisualGroupButtonHeightPx)
               && label.getX() == TrackLanesView::kVisualGroupButtonWidthPx + TrackLanesView::kVisualGroupButtonToLabelGapPx
               && label.getY() == button.getY() && label.getHeight() == TrackLanesView::kVisualGroupLabelHeightPx
               && label.getRight() == headerW - TrackLanesView::kVisualGroupLabelRightPadPx
               && label.getBottom() <= blockTop + kBlock,
           "collapse: button + name label sit at the top pad INSIDE the collapsed block, the label using the header width");
    bool noHiddenHits = true;
    for (const int y : { blockTop + 1, blockTop + kPad + 2, blockTop + kPad + 7, blockTop + kBlock - 2 })
    {
        for (const int x : { 40, headerW - 30, headerW + 60, headerW + 300 })
        {
            const juce::Point<int> p(x, y);
            if (button.contains(p) || label.contains(p))
            {
                continue;
            }
            juce::Component* const c = f.lanes->getComponentAt(p.x, p.y);
            noHiddenHits = noHiddenHits && (c == f.lanes.get());
        }
    }
    expect(noHiddenHits,
           "collapse: NO hidden child component under the collapsed block (headers, lanes, "
           "buttons and clips are all unreachable)");
    juce::Component* const atLabel = f.lanes->getComponentAt(label.getCentreX(), label.getCentreY());
    expect(atLabel != nullptr && atLabel->getBounds() == label,
           "collapse: the name label is hit at its centre (its own component, nothing beneath)");

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

    // Strip i (member order) occupies 4 rows at blockTop + pad + 5 i; the row between two strips
    // is the 1 px gap (lane background).
    const int blockTop = f.widgetYTopForTrack(rows[1]);
    constexpr int kPad3 = TrackLanesView::collapsedGroupStripsTopPadPx(3);
    const auto stripTopY = [&](const int i) { return blockTop + kPad3 + 5 * i; };
    const auto stripMidY = [&](const TrackId tid) {
        return stripTopY(tid == tA ? 0 : (tid == tB ? 1 : 2)) + 2;
    };
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
        // 4 px strips with 1 px empty separation: all four rows of A's strip are filled, the pad
        // row above it and the gap row below it are not.
        const int xA = widgetXForSample(f, sec(1.3));
        expect(isFill(img, xA, stripTopY(0)) && isFill(img, xA, stripTopY(0) + 3)
                   && !isFill(img, xA, stripTopY(0) - 1) && !isFill(img, xA, stripTopY(0) + 4),
               "strips: a strip is exactly 4 rows tall, with the centring pad above and the 1 px gap below unfilled");
        const int xC = widgetXForSample(f, sec(1.2));
        expect(isFill(img, xC, stripTopY(2)) && isFill(img, xC, stripTopY(2) + 3) && !isFill(img, xC, stripTopY(2) - 1)
                   && !isFill(img, xC, stripTopY(2) + 4),
               "strips: the last strip is 4 rows tall too, gap above, centring pad below");
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
    const int blockTop = f.widgetYTopForTrack(audioTid);
    for (int i = 0; i < 2; ++i)
    {
        const int y = blockTop + TrackLanesView::collapsedGroupStripsTopPadPx(2) + 5 * i + 2;
        for (int x = headerW + 3; x < f.lanes->getWidth() - 2; x += 7)
        {
            anyFill = anyFill || (img.getPixelAt(x, y) == juce::Colour(kStripFillArgb));
        }
    }
    juce::ignoreUnused(busTid);
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
    expect(hCollapsed.at(rows[1]) + hCollapsed.at(rows[2]) + hCollapsed.at(rows[3])
                   == TrackLanesView::collapsedGroupBlockHeightPx(3)
               && hCollapsed.at(rows[1]) < trh::kMinRowHeightPx && hCollapsed.at(rows[2]) < trh::kMinRowHeightPx
               && hCollapsed.at(rows[3]) < trh::kMinRowHeightPx,
           "heights: the preset does NOT change the collapsed display (still the 25 px block)");
    expect(f.lanes->uniformTrackRowHeightPresetStatus() == trh::TrackRowHeightPreset::Small,
           "heights: status reads Small from the normal heights while the group is collapsed");
    f.setCollapsed(*gid, false);
    const std::map<TrackId, int> hExpanded = f.rowHeights();
    expect(hExpanded.at(rows[1]) == trh::kSmallPresetPx
               && hExpanded.at(rows[2]) == trh::kSmallPresetPx
               && hExpanded.at(rows[3]) == trh::kSmallPresetPx,
           "heights: expansion restores the heights the preset gave the HIDDEN members (Small)");
}

// ---------------------------------------------------------------------------------------------
// §1 + §2: group controls (chevron button + separate name label) — placement at the first
// track, in the viewport, scrolled past the top, scrolled out, Micro neighbours, two adjacent
// groups; nothing underneath is reachable; short click toggles, long press on the name renames
// ---------------------------------------------------------------------------------------------
void testHandlePlacementAndGestures()
{
    using TLV = TrackLanesView;
    GroupFixture f(/*extraAudioTracks*/ 10);
    f.lanes->applyTrackRowHeightPreset(trh::TrackRowHeightPreset::Medium);
    constexpr int gutter = TLV::kArrangementTimelineHeaderGutterPx;
    const int headerW = f.lanes->headerColumnWidthPx();
    const int segRight = TrackHeaderView::kHeaderGroupMarginPx
                         + TrackHeaderView::colourSegmentWidthPxForDigits(f.lanes->trackNumberDigitCount());
    const int firstCellX = segRight + TrackHeaderView::kHeaderSegmentToStripGapPx;
    constexpr int labelX = TLV::kVisualGroupButtonWidthPx + TLV::kVisualGroupButtonToLabelGapPx;

    // A group starting at the very FIRST track: button + label sit in the gutter band above the
    // first member (right of the add-track corner), never over the member's title row.
    const std::optional<int> gFirst = f.makeGroup({ 0, 1 }, "First");
    const juce::Rectangle<int> bFirst = f.lanes->visualGroupHandleBoundsForTest(*gFirst);
    const juce::Rectangle<int> lFirst = f.lanes->visualGroupNameLabelBoundsForTest(*gFirst);
    expect(bFirst == juce::Rectangle<int>(TLV::kVisualGroupHandleGutterLeftPx,
                                          gutter - TLV::kVisualGroupGutterBottomGapPx - TLV::kVisualGroupButtonHeightPx,
                                          TLV::kVisualGroupButtonWidthPx, TLV::kVisualGroupButtonHeightPx)
               && lFirst.getX() == bFirst.getX() + labelX && lFirst.getY() == bFirst.getY()
               && lFirst.getHeight() == TLV::kVisualGroupLabelHeightPx
               && lFirst.getRight() == headerW - TLV::kVisualGroupLabelRightPadPx && lFirst.getBottom() < gutter,
           "controls: a group at the FIRST track keeps button + label visible in the gutter band (covers no title row)");

    // A mid-list group with Medium neighbours: the button straddles the boundary on the marker's
    // shaft (7 above / 5 below), the label hangs above the boundary with its bottom edge 2 px
    // below it — inside the previous row's free chrome + resize band, never over a control cell
    // of either row, and using the header width.
    const std::optional<int> gMid = f.makeGroup({ 3, 4, 5 }, "Mid");
    const juce::Rectangle<int> bMid = f.lanes->visualGroupHandleBoundsForTest(*gMid);
    const juce::Rectangle<int> lMid = f.lanes->visualGroupNameLabelBoundsForTest(*gMid);
    const int boundaryY = f.widgetYTopForTrack(f.tidAt(3));
    expect(bMid == juce::Rectangle<int>(0, boundaryY - TLV::kVisualGroupButtonReachAbovePx, TLV::kVisualGroupButtonWidthPx,
                                        TLV::kVisualGroupButtonHeightPx)
               && bMid.getRight() < firstCellX,
           "controls: the mid-list button straddles the group's top boundary on the marker column, left of every cell");
    expect(lMid.getX() == labelX && lMid.getBottom() == boundaryY + TLV::kVisualGroupLabelReachBelowPx
               && lMid.getHeight() == TLV::kVisualGroupLabelHeightPx && lMid.getRight() == headerW - TLV::kVisualGroupLabelRightPadPx
               && lMid.getBottom() <= boundaryY + TrackHeaderView::kHeaderRowTopPadPx
               && lMid.getY() >= f.widgetYTopForTrack(f.tidAt(2)) + TrackHeaderView::kHeaderRowTopPadPx
                                     + 2 * TrackHeaderView::kStripControlCellWidthPx + TrackHeaderView::kHeaderRowGapPx,
           "controls: the name label hangs above the boundary, ending at the first member's top pad, below the previous row's cells");
    expect(TLV::headerFreeBottomPxForRowHeight(trh::kMediumPresetPx) == 45
               && TLV::headerFreeBottomPxForRowHeight(trh::kMiniPresetPx) == 14
               && TLV::headerFreeBottomPxForRowHeight(trh::kSmallPresetPx) == 3
               && TLV::headerFreeBottomPxForRowHeight(trh::kMicroPresetPx) == 0
               && TLV::headerFreeBottomPxForRowHeight(TLV::kCollapsedGroupMemberRowHeightPx) == 0,
           "controls: free chrome under the lowest control row = 45 / 14 / 3 / 0 / 0 px for Medium / Mini / Small / Micro / strip");

    // Scroll until the mid group's top is above the viewport while its rows are still visible:
    // the controls move into the gutter band instead of disappearing (sticky).
    f.lanes->scrollVerticallyToOffsetPx(f.lanes->rowTopOffsetPxForTrackForDiagnostics(f.tidAt(4)));
    const juce::Rectangle<int> bClamped = f.lanes->visualGroupHandleBoundsForTest(*gMid);
    expect(!bClamped.isEmpty() && bClamped.getBottom() == gutter - TLV::kVisualGroupGutterBottomGapPx
               && bClamped.getX() == TLV::kVisualGroupHandleGutterLeftPx
               && !f.lanes->visualGroupNameLabelBoundsForTest(*gMid).isEmpty(),
           "controls: with the group's top scrolled past the viewport top both controls sit in the gutter band");

    // Scroll far past the group: the controls unmap (no floating orphans).
    f.lanes->scrollVerticallyToOffsetPx(f.lanes->verticalScrollModel().maxOffsetPx());
    expect(f.lanes->visualGroupHandleBoundsForTest(*gFirst).isEmpty()
               && f.lanes->visualGroupNameLabelBoundsForTest(*gFirst).isEmpty(),
           "controls: a fully scrolled-out group has NO laid-out button or label");
    f.lanes->scrollVerticallyToOffsetPx(0);

    // Micro neighbours (no free chrome): the button alone, straddling the boundary left of every
    // control cell; the label is hidden (the button's tooltip carries the name).
    {
        f.lanes->applyTrackRowHeightPreset(trh::TrackRowHeightPreset::Micro);
        const juce::Rectangle<int> bMicro = f.lanes->visualGroupHandleBoundsForTest(*gMid);
        const int boundaryMicro = f.widgetYTopForTrack(f.tidAt(3));
        expect(bMicro == juce::Rectangle<int>(0, boundaryMicro - TLV::kVisualGroupButtonReachAbovePx,
                                              TLV::kVisualGroupButtonWidthPx, TLV::kVisualGroupButtonHeightPx)
                   && bMicro.getRight() <= segRight && bMicro.getRight() < firstCellX
                   && bMicro.getBottom() <= boundaryMicro + TrackHeaderView::kHeaderRowTopPadPx + 3,
               "controls (Micro): the button straddles the boundary over the margin + segment zone only (icon rows untouched)");
        expect(f.lanes->visualGroupNameLabelBoundsForTest(*gMid).isEmpty(),
               "controls (Micro): no room for the label between two Micro rows -> hidden (tooltip on the button)");
        // Two adjacent groups in Micro: disjoint buttons at their own boundaries.
        const std::optional<int> gNext = f.makeGroup({ 6, 7 }, "Next");
        const juce::Rectangle<int> bNext = f.lanes->visualGroupHandleBoundsForTest(*gNext);
        expect(gNext.has_value() && !bNext.isEmpty() && !bNext.intersects(bMicro)
                   && bNext.getY() == f.widgetYTopForTrack(f.tidAt(6)) - TLV::kVisualGroupButtonReachAbovePx
                   && bNext.getRight() < firstCellX,
               "controls (Micro): two directly adjacent groups get two disjoint buttons, both left of the control cells");
        juce::Component* const atNext
            = f.lanes->getComponentAt(TLV::kVisualGroupButtonBoxXPx + 4, bNext.getCentreY());
        expect(atNext != nullptr && atNext->getBounds() == bNext,
               "controls (Micro): the button is the component hit inside its box (it swallows its own clicks)");
        // The first member's Power / Mute cells are NOT covered by the controls.
        const juce::Rectangle<int> firstRowCells(firstCellX, boundaryMicro + TrackHeaderView::kHeaderRowTopPadPx,
                                                 3 * TrackHeaderView::kStripControlCellWidthPx, TrackHeaderView::kStripControlCellWidthPx);
        expect(!firstRowCells.intersects(bMicro) && !firstRowCells.intersects(bNext),
               "controls (Micro): neither button overlaps the title cells of the row below");
        // A collapsed group directly ABOVE another group: the next group's button stays inside the
        // collapsed block's free bottom (the readable minimum reserves it), never over its label.
        f.setCollapsed(*gMid, true);
        const juce::Rectangle<int> lMidCollapsed = f.lanes->visualGroupNameLabelBoundsForTest(*gMid);
        const juce::Rectangle<int> bNext2 = f.lanes->visualGroupHandleBoundsForTest(*gNext);
        expect(!lMidCollapsed.isEmpty() && !bNext2.isEmpty() && !bNext2.intersects(lMidCollapsed)
                   && !bNext2.intersects(f.lanes->visualGroupHandleBoundsForTest(*gMid)),
               "controls: a group right below a collapsed group never overlaps the collapsed block's button or label");
        f.setCollapsed(*gMid, false);
        f.lanes->applyTrackRowHeightPreset(trh::TrackRowHeightPreset::Medium);
        f.lanes->scrollVerticallyToOffsetPx(0);
    }

    // The controls never hit-test through to the previous track's header or its resize band.
    const juce::Rectangle<int> bMid2 = f.lanes->visualGroupHandleBoundsForTest(*gMid);
    const juce::Rectangle<int> lMid2 = f.lanes->visualGroupNameLabelBoundsForTest(*gMid);
    juce::Component* const atButton = f.lanes->getComponentAt(TLV::kVisualGroupButtonBoxXPx + 4, bMid2.getCentreY());
    juce::Component* const atLabel = f.lanes->getComponentAt(lMid2.getCentreX(), lMid2.getCentreY());
    expect(atButton != nullptr && atButton->getBounds() == bMid2 && atLabel != nullptr && atLabel->getBounds() == lMid2,
           "controls: hit-testing the button / label areas reaches ONLY those components (never the "
           "previous track or its resize band)");

    // Short click on the button toggles exactly once per click (the exact mouse-up action).
    const TrackId activeBefore = f.session.getActiveTrackId();
    expect(f.lanes->shortClickVisualGroupHandleLikeMouseForTest(*gMid)
               && f.session.findVisualTrackGroupById(*gMid)->collapsed,
           "button: one short click collapses the group");
    expect(f.lanes->shortClickVisualGroupHandleLikeMouseForTest(*gMid)
               && !f.session.findVisualTrackGroupById(*gMid)->collapsed,
           "button: the next short click expands it again (exactly one toggle per click)");
    expect(f.session.getActiveTrackId() == activeBefore,
           "button: collapsing/expanding never changes the active track");

    // Long press on the NAME begins the inline rename WITHOUT toggling; committing renames.
    const bool collapsedBefore = f.session.findVisualTrackGroupById(*gMid)->collapsed;
    expect(f.lanes->beginRenameOnVisualGroupHandleLikeLongPressForTest(*gMid),
           "label: a long press on the name opens the inline rename editor");
    expect(f.session.findVisualTrackGroupById(*gMid)->collapsed == collapsedBefore,
           "label: the long press did NOT also toggle the collapse state");
    expect(f.lanes->commitVisualGroupHandleRenameForTest(*gMid, "Drums")
               && f.session.findVisualTrackGroupById(*gMid)->name == "Drums",
           "label: committing the rename editor renames the group to \"Drums\"");
    expect(f.session.findVisualTrackGroupById(*gMid)->collapsed == collapsedBefore,
           "label: the committed rename still never toggled the group");
}

// ---------------------------------------------------------------------------------------------
// §3: collapsed blocks for 2 / 8 / 16 members — strips content vs readable minimum, same height
// in header + timeline (one layout model), label readable, restored on expand, through scroll
// ---------------------------------------------------------------------------------------------
void testCollapsedBlockHeightsForGroupSizes()
{
    using TLV = TrackLanesView;
    static_assert(TLV::collapsedGroupStripsContentHeightPx(2) == 9 && TLV::collapsedGroupBlockHeightPx(2) == TLV::kCollapsedGroupHeaderMinHeightPx
                      && TLV::collapsedGroupStripsContentHeightPx(8) == 39 && TLV::collapsedGroupBlockHeightPx(8) == 39
                      && TLV::collapsedGroupStripsContentHeightPx(16) == 79 && TLV::collapsedGroupBlockHeightPx(16) == 79
                      && TLV::kCollapsedGroupHeaderMinHeightPx == 25,
                  "block heights: 2 members -> the 25 px readable minimum; 8 -> 39; 16 -> 79 (4 n + (n - 1))");
    GroupFixture f(/*extraAudioTracks*/ 30);
    f.lanes->setSize(900, 1500); // every block starts inside the viewport before the scroll leg
    f.lanes->applyTrackRowHeightPreset(trh::TrackRowHeightPreset::Mini);
    const std::vector<TrackId> rows = f.rowTrackIds();
    expect(rows.size() >= 31, "fixture: 30 audio rows + Stereo Out");

    struct Case { int first; int count; const char* name; };
    for (const Case c : { Case{ 1, 2, "Two" }, Case{ 5, 8, "Eight" }, Case{ 14, 16, "Sixteen" } })
    {
        std::vector<int> idx;
        for (int k = 0; k < c.count; ++k)
        {
            idx.push_back(c.first + k);
        }
        const std::optional<int> gid = f.makeGroup(idx, c.name);
        expect(gid.has_value(), juce::String("fixture: group \"") + c.name + "\" created");
        const int contentBefore = f.lanes->verticalScrollModel().contentHeightPx;
        f.setCollapsed(*gid, true);
        const int block = TLV::collapsedGroupBlockHeightPx(c.count);
        int sum = 0;
        const std::map<TrackId, int> h = f.rowHeights();
        for (const int k : idx)
        {
            sum += h.at(f.tidAt(k));
        }
        expect(sum == block && f.lanes->verticalScrollModel().contentHeightPx == contentBefore - c.count * trh::kMiniPresetPx + block,
               juce::String(c.name) + ": members' display heights sum to the block height and the content height follows");
        const int blockTop = f.widgetYTopForTrack(f.tidAt(c.first));
        const juce::Rectangle<int> label = f.lanes->visualGroupNameLabelBoundsForTest(*gid);
        const juce::Rectangle<int> button = f.lanes->visualGroupHandleBoundsForTest(*gid);
        expect(button.getY() == blockTop + TLV::kVisualGroupCollapsedTopPadPx && label.getY() == button.getY()
                   && label.getBottom() <= blockTop + block && !label.isEmpty(),
               juce::String(c.name) + ": the name label is laid out readable inside the collapsed block");
        expect(f.verticalLayoutOk(c.name), juce::String(c.name) + ": the shared vertical layout verifies while collapsed");
        // Scroll the block to the viewport top and back: the block keeps its height (header and
        // timeline share the one model).
        f.lanes->scrollVerticallyToOffsetPx(f.lanes->rowTopOffsetPxForTrackForDiagnostics(f.tidAt(c.first)));
        int sumScrolled = 0;
        const std::map<TrackId, int> hs = f.rowHeights();
        for (const int k : idx)
        {
            sumScrolled += hs.at(f.tidAt(k));
        }
        expect(sumScrolled == block && f.verticalLayoutOk("scrolled"), juce::String(c.name) + ": block height unchanged under scroll");
        f.lanes->scrollVerticallyToOffsetPx(0);
        f.setCollapsed(*gid, false);
        bool restored = true;
        const std::map<TrackId, int> he = f.rowHeights();
        for (const int k : idx)
        {
            restored = restored && he.at(f.tidAt(k)) == trh::kMiniPresetPx;
        }
        expect(restored && f.lanes->verticalScrollModel().contentHeightPx == contentBefore,
               juce::String(c.name) + ": expanding restores every member's normal height and the content height");
    }
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
        expect(h.at(g1Members[0]) + h.at(g1Members[1]) + h.at(g1Members[2]) == TrackLanesView::collapsedGroupBlockHeightPx(3)
                   && h.at(g1Members[0]) < trh::kMinRowHeightPx && h.at(g1Members[1]) < trh::kMinRowHeightPx
                   && h.at(g1Members[2]) < trh::kMinRowHeightPx,
               "reload: the collapsed group displays as the collapsed block (25 px for 3 members) immediately");
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
// §4 + §5 (audio side): Mini lanes show the real waveform (no label), Micro lanes a thin bar;
// the content returns after a resize through the deferred raster rebuild without any click or
// zoom; the event's outer top / bottom edges sit at the shared 4 px vertical margin.
// ---------------------------------------------------------------------------------------------
void testMiniLaneShowsWaveformAndSharedEventMargin()
{
    const juce::File wav = writeToneWav("mini-tone.wav", 2.0);
    GroupFixture f(/*extraAudioTracks*/ 2);
    f.lanes->setSize(900, 400);
    const TrackId tid = f.tidAt(0);
    expect(f.session.addRecordedTakeAtSample(wav, kRate, 0, tid, (std::int64_t)kRate).wasOk(),
           "mini fixture: a 1 s tone clip at 0 s");
    f.lanes->syncTracksFromSession();

    // Offscreen components paint only inside createComponentSnapshot: the first snapshot after a
    // height change takes the stale-blit path and arms the deferred rebuild (exactly what the app
    // does); pumping the message loop runs it, the next snapshot shows the final painting.
    const auto settle = [&f] {
        (void) f.lanes->createComponentSnapshot(f.lanes->getLocalBounds(), false, 1.0f);
        juce::MessageManager::getInstance()->runDispatchLoopUntil(700);
        return f.lanes->createComponentSnapshot(f.lanes->getLocalBounds(), false, 1.0f);
    };
    const juce::Colour body = track_colour_palette::eventBodyFill(TrackColourKey::DefaultGrey);
    const juce::Colour laneBg(0xff252528);
    const auto countWaveformPixels = [&](const juce::Image& img, const juce::Rectangle<int>& r) {
        // Waveform peaks are painted light (lightblue family) — neither the body fill, the border
        // nor the lane background. Count clearly light pixels.
        int n = 0;
        for (int y = r.getY(); y < r.getBottom(); ++y)
        {
            for (int x = r.getX(); x < r.getRight(); ++x)
            {
                const juce::Colour c = img.getPixelAt(x, y);
                if (c.getBrightness() > 0.75f) // border 0xff7a8aa0 is ~0.63, waveform peaks ~0.86
                {
                    ++n;
                }
            }
        }
        return n;
    };
    const int x0 = f.lanes->headerColumnWidthPx() + 10;
    const int clipW = 70;

    f.lanes->applyTrackRowHeightPreset(trh::TrackRowHeightPreset::Mini);
    {
        const juce::Image img = settle();
        const int top = f.widgetYTopForTrack(tid);
        const juce::Rectangle<int> clip(x0, top, clipW, trh::kMiniPresetPx);
        expect(countWaveformPixels(img, clip) > 200,
               "Mini: the audio lane paints the real waveform inside the event (content before label)");
        // Shared vertical margin: the event body starts 4 px below the lane top and ends 4 px
        // above its bottom (`kEventVerticalMargin`), the same rule the MIDI lanes use.
        constexpr int m = (int) mini_daw::timeline_clip_chrome::kEventVerticalMargin;
        // (2 px outside the body: the 1 px border stroke straddles the edge and blends the first row.)
        const juce::Colour atTopMargin = img.getPixelAt(x0 + 20, top + m - 2);
        const juce::Colour atBodyTop = img.getPixelAt(x0 + 20, top + m + 1);
        const juce::Colour atBodyBottom = img.getPixelAt(x0 + 20, top + trh::kMiniPresetPx - m - 2);
        const juce::Colour atBottomMargin = img.getPixelAt(x0 + 20, top + trh::kMiniPresetPx - m + 1);
        expect(atTopMargin == laneBg && atBottomMargin == laneBg && atBodyTop != laneBg && atBodyBottom != laneBg,
               "Mini: the event's outer edges sit exactly at the shared 4 px vertical margin (lane background outside, event inside)");
        // No name label at Mini: the top-left label strip area shows body / waveform only (no
        // white text pixels in the first rows of the body right of the corner).
        int whiteText = 0;
        for (int y = top + m + 2; y < top + m + 10; ++y)
        {
            for (int x = x0 + 6; x < x0 + 60; ++x)
            {
                const juce::Colour c = img.getPixelAt(x, y);
                if (c.getRed() > 220 && c.getGreen() > 220 && c.getBlue() > 220)
                {
                    ++whiteText;
                }
            }
        }
        expect(whiteText == 0, "Mini: the event name label is hidden (content has priority)");
    }
    f.lanes->applyTrackRowHeightPreset(trh::TrackRowHeightPreset::Micro);
    {
        const juce::Image img = settle();
        const int top = f.widgetYTopForTrack(tid);
        const juce::Rectangle<int> clip(x0, top, clipW, trh::kMicroPresetPx);
        expect(countWaveformPixels(img, clip) == 0, "Micro: no waveform — a thin bar shows the time extent only");
        int bodyRows = 0;
        for (int y = top; y < top + trh::kMicroPresetPx; ++y)
        {
            if (img.getPixelAt(x0 + 20, y) == body)
            {
                ++bodyRows;
            }
        }
        expect(bodyRows >= trh::kLaneBarFieldHeightPx - 2 && bodyRows <= trh::kLaneBarFieldHeightPx,
               "Micro: the bar is the 8 px thin field (body fill rows)");
    }
    f.lanes->applyTrackRowHeightPreset(trh::TrackRowHeightPreset::Small);
    {
        const juce::Image img = settle();
        const int top = f.widgetYTopForTrack(tid);
        const juce::Rectangle<int> clip(x0, top, clipW, trh::kSmallPresetPx);
        expect(countWaveformPixels(img, clip) > 200, "Small: waveform content is back after the resize (deferred rebuild, no click / zoom)");
    }
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
    // Let the waveform pyramids finish and the deferred raster rebuild run (see the compact
    // example) so the images show the final lane painting.
    const auto settle = [&f] {
        (void) f.lanes->createComponentSnapshot(f.lanes->getLocalBounds(), false, 1.0f);
        juce::MessageManager::getInstance()->runDispatchLoopUntil(700);
    };
    settle();
    writePng(f.lanes->createComponentSnapshot(f.lanes->getLocalBounds(), false, 1.0f),
             outDir.getChildFile(baseName + "-expanded.png"));
    f.setCollapsed(*gid, true);
    settle();
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
    testCollapsedBlockHeightsForGroupSizes();
    testScrollAnchoringAndClamp();
    testMembershipOnTrackChanges();
    testNarrowUndoThroughRealCoordinator();
    testPersistenceRoundTripAndOlderProjects();
    testMalformedGroupMetadataSafeFallback();
    testTrackColourNarrowUndoRepaintAndPersistence();
    testMiniLaneShowsWaveformAndSharedEventMargin();

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
