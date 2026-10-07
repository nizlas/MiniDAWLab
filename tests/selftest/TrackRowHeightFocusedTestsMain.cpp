// =============================================================================
// TrackRowHeightFocusedTests — shared track heights Small / Medium / Large (production code)
// =============================================================================
//
// Drives the PRODUCTION `TrackLanesView` (offscreen, real `Session` / `Transport` /
// vertical-scroll model) and the PRODUCTION project save/load path:
//   - a preset command changes EVERY row (all kinds incl. Group + Stereo Out, incl. rows scrolled
//     out of the viewport) in one gathered layout pass and preserves the top visible track;
//   - individual bottom-edge resize still works per row, min = Small (64), max = 480, and flips
//     the dropdown status to Custom;
//   - new rows get the last chosen preset's height; a duplicated row keeps the SOURCE height;
//   - v26 persistence: root `trackRowHeightPreset` + per-track `rowHeight` survive save/reload,
//     pre-v26 files load as Medium, malformed values degrade safely (clamped / defaulted);
//   - the ONE vertical scroll model stays valid and aligned after mid-list resizes (production
//     `verifyVerticalScrollLayoutForDiagnostics`).
// No audio device, no window, no VST3 hosting (plugin/instrument entry points referenced by
// `Session.cpp` / `TrackLanesView.cpp` are satisfied by never-executed link stubs below).
//
// Exit 0 = all checks green.
// =============================================================================

#include "app/TransportLayoutHelper.h"
#include "domain/Session.h"
#include "engine/RecorderService.h"
#include "audio/LatencySettingsStore.h"
#include "io/AudioWaveformCache.h"
#include "io/ProjectFile.h"
#include "transport/Transport.h"
#include "ui/CollapsibleSideStrip.h"
#include "ui/EditToolIconStrip.h"
#include "ui/SoloMemoryStrip.h"
#include "ui/TimelineRulerView.h"
#include "ui/TimelineViewportModel.h"
#include "ui/TrackLanesView.h"
#include "ui/TrackRowHeightPresets.h"
#include "ui/UiPlayheadClock.h"

#include <juce_gui_basics/juce_gui_basics.h>

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
                             .getChildFile("dal-row-height-tests");
    (void)d.createDirectory();
    return d;
}

/// Offscreen production stack: Session rows (audio + group + Stereo Out) under a real
/// `TrackLanesView` with the real vertical scroll model. No instrument attachments are registered
/// (the shared height model is row-kind agnostic; header-chrome fit per kind is covered by
/// TrackHeaderColumnFocusedTests).
struct LanesFixture
{
    Session session;
    Transport transport;
    TimelineViewportModel viewport;
    juce::AudioDeviceManager deviceManager; // never initialised — no device
    RecorderService recorder;
    LatencySettingsStore latencyStore{ deviceManager, tempDir().getChildFile("latency.xml") };
    AudioWaveformCache waveformCache;
    std::unique_ptr<TrackLanesView> lanes;

    explicit LanesFixture(const int extraAudioTracks = 2, const bool withGroup = true)
    {
        for (int i = 0; i < extraAudioTracks; ++i)
        {
            session.addTrack();
        }
        if (withGroup)
        {
            session.addGroupTrack();
        }
        lanes = std::make_unique<TrackLanesView>(session, transport, viewport, deviceManager,
                                                 recorder, latencyStore, waveformCache);
        lanes->setSize(800, 400);
        lanes->syncTracksFromSession();
    }

    /// All session TrackIds that are laid out as arrangement rows, in visible-row order.
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

    /// Effective row height derived from the production layout model (top-offset deltas; the last
    /// row closes against the model's content height) — no private access.
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

    [[nodiscard]] bool allRowsAt(const int px) const
    {
        for (const auto& [tid, h] : rowHeights())
        {
            juce::ignoreUnused(tid);
            if (h != px)
            {
                return false;
            }
        }
        return !rowHeights().empty();
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
};

// ---------------------------------------------------------------------------------------------
void testPresetChangesEveryRowIncludingOffViewport()
{
    LanesFixture f(/*extraAudioTracks*/ 5, /*withGroup*/ true);
    const int rowCount = (int)f.rowTrackIds().size();
    info("rows laid out: " + juce::String(rowCount) + " (audio + group + Stereo Out)");
    expect(rowCount >= 7, "fixture: at least 7 arrangement rows (incl. Group and Stereo Out)");

    // Small viewport: most rows are scrolled out — the preset must still change them all.
    f.lanes->setSize(800, 150);
    f.lanes->applyTrackRowHeightPreset(trh::TrackRowHeightPreset::Small);
    expect(f.allRowsAt(trh::kSmallRowHeightPx),
           "preset Small: EVERY row (incl. off-viewport, Group, Stereo Out) is exactly 64 px");
    expect(f.lanes->verticalScrollModel().contentHeightPx == rowCount * trh::kSmallRowHeightPx,
           "preset Small: content height = rows x 64 (one gathered layout)");
    expect(f.lanes->uniformTrackRowHeightPresetStatus() == trh::TrackRowHeightPreset::Small,
           "preset Small: dropdown status reports Small");

    f.lanes->applyTrackRowHeightPreset(trh::TrackRowHeightPreset::Large);
    expect(f.allRowsAt(trh::kLargeRowHeightPx), "preset Large: every row is exactly 192 px");
    expect(f.lanes->uniformTrackRowHeightPresetStatus() == trh::TrackRowHeightPreset::Large,
           "preset Large: dropdown status reports Large");

    f.lanes->applyTrackRowHeightPreset(trh::TrackRowHeightPreset::Medium);
    expect(f.allRowsAt(trh::kMediumRowHeightPx), "preset Medium: every row back at 96 px");
    expect(f.verticalLayoutOk("after presets"), "vertical scroll layout verifies after preset changes");
}

void testIndividualResizeAfterPresetIsCustomAndClamped()
{
    LanesFixture f;
    f.lanes->applyTrackRowHeightPreset(trh::TrackRowHeightPreset::Medium);
    const std::vector<TrackId> rows = f.rowTrackIds();
    const TrackId mid = rows[rows.size() / 2];

    // The same setter the drag path ends in (clamped to [Small, max]).
    f.lanes->setTrackRowHeightPxForStabilityTest(mid, 150);
    expect(f.rowHeights()[mid] == 150, "drag: one row resized to 150 px changes ONLY that row");
    int othersAtDefault = 0;
    for (const auto& [tid, h] : f.rowHeights())
    {
        othersAtDefault += (tid != mid && h == trh::kMediumRowHeightPx) ? 1 : 0;
    }
    expect(othersAtDefault == (int)rows.size() - 1, "drag: every other row stays at the preset height");
    expect(!f.lanes->uniformTrackRowHeightPresetStatus().has_value(),
           "drag after preset: dropdown status is Custom (no preset selected)");
    expect(f.lanes->lastChosenTrackRowHeightPreset() == trh::TrackRowHeightPreset::Medium,
           "drag after preset: the project default preset is NOT changed by an individual drag");

    f.lanes->setTrackRowHeightPxForStabilityTest(mid, 10);
    expect(f.rowHeights()[mid] == trh::kSmallRowHeightPx,
           "drag: below-minimum request clamps to the Small preset height (64)");
    f.lanes->setTrackRowHeightPxForStabilityTest(mid, 100000);
    expect(f.rowHeights()[mid] == 480, "drag: the existing 480 px maximum is kept");
    expect(f.verticalLayoutOk("after mid-list resize"),
           "vertical scroll layout verifies after a mid-list resize (headers/lanes aligned)");

    // A uniform height that is EXACTLY a preset reads as that preset again (status from heights).
    for (const TrackId tid : rows)
    {
        f.lanes->setTrackRowHeightPxForStabilityTest(tid, trh::kSmallRowHeightPx);
    }
    expect(f.lanes->uniformTrackRowHeightPresetStatus() == trh::TrackRowHeightPreset::Small,
           "status: all rows dragged to exactly 64 px reads as Small (derived from heights)");
}

void testNewTrackGetsLastChosenPresetAndDuplicateKeepsSourceHeight()
{
    LanesFixture f;
    f.lanes->applyTrackRowHeightPreset(trh::TrackRowHeightPreset::Large);

    // An individual drag must NOT change the default for new tracks.
    const TrackId firstRow = f.rowTrackIds().front();
    f.lanes->setTrackRowHeightPxForStabilityTest(firstRow, 100);

    f.session.addTrack();
    f.lanes->syncTracksFromSession();
    f.lanes->setSize(800, 400); // relayout like the app does after a track add
    const std::vector<TrackId> rows = f.rowTrackIds();
    // The new audio row is the newest id among laid-out rows.
    TrackId newest = kInvalidTrackId;
    for (const TrackId tid : rows)
    {
        newest = juce::jmax(newest, tid);
    }
    expect(f.rowHeights()[newest] == trh::kLargeRowHeightPx,
           "new track: created at the LAST CHOSEN preset height (192) even after an individual drag");

    // Duplicate keeps the SOURCE height (the lanes-side primitive the duplicate flow calls).
    const std::optional<TrackId> dup = f.session.duplicateTrack(firstRow);
    expect(dup.has_value(), "fixture: Session::duplicateTrack created a copy");
    if (dup.has_value())
    {
        f.lanes->copyRowHeightForDuplicatedTrack(firstRow, *dup);
        f.lanes->syncTracksFromSession();
        f.lanes->setSize(800, 401);
        expect(f.rowHeights()[*dup] == 100,
               "duplicate: the copy keeps the source row's height (100), not the project default");
        expect(f.lanes->lastChosenTrackRowHeightPreset() == trh::TrackRowHeightPreset::Large,
               "duplicate: the project default preset is never changed by duplication");
    }
}

void testTopVisibleTrackPreservedAndScrollClamped()
{
    LanesFixture f(/*extraAudioTracks*/ 8);
    f.lanes->setSize(800, 240);
    f.lanes->applyTrackRowHeightPreset(trh::TrackRowHeightPreset::Medium);

    // Scroll so a mid-list row is the top visible track.
    const std::vector<TrackId> rows = f.rowTrackIds();
    const TrackId anchor = rows[4];
    f.lanes->scrollVerticallyToOffsetPx(f.lanes->rowTopOffsetPxForTrackForDiagnostics(anchor));
    expect(f.lanes->verticalScrollModel().offsetPx
               == f.lanes->rowTopOffsetPxForTrackForDiagnostics(anchor),
           "scroll: anchor row scrolled to the top of the viewport");

    f.lanes->applyTrackRowHeightPreset(trh::TrackRowHeightPreset::Large);
    expect(f.lanes->verticalScrollModel().offsetPx
               == f.lanes->rowTopOffsetPxForTrackForDiagnostics(anchor),
           "preset: the previously topmost visible track stays the top track after the change");
    expect(f.verticalLayoutOk("after preset at offset"), "vertical layout verifies at the preserved offset");

    // Shrinking the content below the viewport clamps the offset to a valid position.
    f.lanes->setSize(800, 2000);
    f.lanes->applyTrackRowHeightPreset(trh::TrackRowHeightPreset::Small);
    const TrackLanesView::VerticalScrollModel m = f.lanes->verticalScrollModel();
    expect(m.offsetPx >= 0 && m.offsetPx <= m.maxOffsetPx(),
           "preset: offset clamped valid when the content shrinks below the viewport");
    expect(f.verticalLayoutOk("after shrink"), "vertical layout verifies after content < viewport");
}

void testUserEditVsLoadNotification()
{
    LanesFixture f;
    std::vector<bool> notifications;
    f.lanes->setOnTrackRowHeightsChanged([&notifications](const bool byUserEdit) {
        notifications.push_back(byUserEdit);
    });

    f.lanes->applyTrackRowHeightPreset(trh::TrackRowHeightPreset::Small);
    expect(!notifications.empty() && notifications.back(),
           "dirty: a preset command notifies as a USER edit (marks the project dirty)");

    f.lanes->setTrackRowHeightPxForStabilityTest(f.rowTrackIds().front(), 120);
    expect(notifications.size() >= 2 && notifications.back(),
           "dirty: an individual resize notifies as a USER edit");

    const size_t before = notifications.size();
    f.lanes->applyTrackRowHeightsFromLoadedProject("medium", {});
    expect(notifications.size() == before + 1 && !notifications.back(),
           "dirty: the project-load apply notifies as NOT a user edit (never marks dirty)");
}

void testProjectPersistenceRoundTrip()
{
    const juce::File dir = tempDir();
    const juce::File proj = dir.getChildFile("row-heights.dalproj");
    (void)proj.deleteFile();
    constexpr double kRate = 48000.0;

    TrackId draggedTid = kInvalidTrackId;
    std::vector<std::pair<TrackId, int>> savedHeights;
    {
        LanesFixture f;
        f.lanes->applyTrackRowHeightPreset(trh::TrackRowHeightPreset::Small);
        draggedTid = f.rowTrackIds()[1];
        f.lanes->setTrackRowHeightPxForStabilityTest(draggedTid, 300); // mixed heights
        savedHeights = f.lanes->allTrackRowHeightsPxForProjectSave();

        ProjectFileTrackRowHeightsV1 rh;
        rh.presetKey = trh::persistenceKeyForPreset(f.lanes->lastChosenTrackRowHeightPreset());
        rh.perTrackRowHeightPx = savedHeights;
        const juce::Result saved = f.session.saveProjectToFile(
            f.transport, proj, kRate, nullptr, {}, false, "1_4", std::nullopt, std::nullopt,
            std::nullopt, rh);
        expect(saved.wasOk(), "save: project with mixed row heights written (" + saved.getErrorMessage() + ")");
    }

    // Raw file level: v26, the preset key and the per-track heights are really in the JSON.
    ProjectFileV1 parsed;
    const juce::Result read = readProjectFile(proj, parsed);
    expect(read.wasOk(), "read: raw readProjectFile succeeds");
    expect(parsed.version == ProjectFileV1::kCurrentVersion && parsed.version >= 26,
           "read: file version is current (>= 26)");
    expect(parsed.trackRowHeightPreset == "small", "read: root trackRowHeightPreset == \"small\"");
    int withHeight = 0;
    int draggedPx = 0;
    for (const auto& t : parsed.tracks)
    {
        withHeight += (t.rowHeightPx > 0) ? 1 : 0;
        if (t.id == draggedTid)
        {
            draggedPx = t.rowHeightPx;
        }
    }
    expect(withHeight == (int)savedHeights.size(),
           "read: every arrangement row carries its actual rowHeight (" + juce::String(withHeight) + ")");
    expect(draggedPx == 300, "read: the dragged row's 300 px is stored exactly");

    // Reload into a FRESH fixture through the production load + apply path.
    {
        LanesFixture f(/*extraAudioTracks*/ 0, /*withGroup*/ false);
        juce::StringArray skipped;
        juce::String note;
        const juce::Result loaded
            = f.session.loadProjectFromFile(f.transport, proj, kRate, skipped, note, nullptr);
        expect(loaded.wasOk(), "reload: project loads (" + loaded.getErrorMessage() + ")");
        f.lanes->syncTracksFromSession();
        f.lanes->setSize(800, 400);
        std::vector<std::pair<TrackId, int>> perTrack;
        for (const auto& t : parsed.tracks)
        {
            perTrack.emplace_back(t.id, t.rowHeightPx);
        }
        f.lanes->applyTrackRowHeightsFromLoadedProject(parsed.trackRowHeightPreset, perTrack);

        expect(f.lanes->lastChosenTrackRowHeightPreset() == trh::TrackRowHeightPreset::Small,
               "reload: the saved preset (Small) is the default again");
        expect(f.rowHeights()[draggedTid] == 300, "reload: the dragged row is 300 px again");
        int othersAtSmall = 0;
        int others = 0;
        for (const auto& [tid, h] : f.rowHeights())
        {
            if (tid != draggedTid)
            {
                ++others;
                othersAtSmall += (h == trh::kSmallRowHeightPx) ? 1 : 0;
            }
        }
        expect(others > 0 && othersAtSmall == others, "reload: every other row is back at Small (64)");
        expect(!f.lanes->uniformTrackRowHeightPresetStatus().has_value(),
               "reload: mixed heights -> dropdown status Custom");

        // New track after reload: the restored preset is the default height.
        f.session.addTrack();
        f.lanes->syncTracksFromSession();
        f.lanes->setSize(800, 401);
        TrackId newest = kInvalidTrackId;
        for (const TrackId tid : f.rowTrackIds())
        {
            newest = juce::jmax(newest, tid);
        }
        expect(f.rowHeights()[newest] == trh::kSmallRowHeightPx,
               "reload: a NEW track uses the restored preset default (64)");
    }
    (void)proj.deleteFile();
}

void testOlderProjectsAndMalformedValues()
{
    const juce::File dir = tempDir();

    // Pre-v26 shape: save WITHOUT row-height data (both keys omitted), then rewrite the version
    // number to 25 in the JSON text — a genuine older file (the writer itself only emits the
    // current version, so the text edit is the honest way to make one).
    {
        const juce::File oldProj = dir.getChildFile("pre-v26.dalproj");
        (void)oldProj.deleteFile();
        {
            LanesFixture f;
            const juce::Result saved = f.session.saveProjectToFile(f.transport, oldProj, 48000.0);
            expect(saved.wasOk(), "pre-v26: base project written (no row-height data passed)");
        }
        const juce::String text = oldProj.loadFileAsString();
        expect(!text.contains("rowHeight") && !text.contains("trackRowHeightPreset"),
               "pre-v26: a save without row-height data emits NO row-height keys");
        const juce::String currentVersionText = juce::String(ProjectFileV1::kCurrentVersion);
        const juce::String asV25
            = text.replace("\"version\": " + currentVersionText, "\"version\": 25")
                  .replace("\"version\":" + currentVersionText, "\"version\":25");
        expect(asV25 != text && oldProj.replaceWithText(asV25), "pre-v26: version rewritten to 25");

        ProjectFileV1 reread;
        expect(readProjectFile(oldProj, reread).wasOk(), "pre-v26: v25 file reads");
        expect(reread.version == 25 && reread.trackRowHeightPreset.isEmpty(),
               "pre-v26: version 25, no preset key");

        LanesFixture f;
        std::vector<std::pair<TrackId, int>> perTrack;
        for (const auto& t : reread.tracks)
        {
            perTrack.emplace_back(t.id, t.rowHeightPx);
        }
        f.lanes->applyTrackRowHeightsFromLoadedProject(reread.trackRowHeightPreset, perTrack);
        expect(f.lanes->lastChosenTrackRowHeightPreset() == trh::TrackRowHeightPreset::Medium,
               "pre-v26: missing preset key -> Medium default");
        expect(f.allRowsAt(trh::kMediumRowHeightPx),
               "pre-v26: all rows at the historical Medium default (96) - older projects keep their look");
        (void)oldProj.deleteFile();
    }

    // Malformed values straight in the JSON: negative, non-numeric, absurd, fractional heights
    // and an unknown preset key — reader + apply degrade safely, never a failure.
    {
        const juce::File badProj = dir.getChildFile("malformed-row-heights.dalproj");
        const juce::String json =
            "{ \"version\": 26, \"nextPlacedClipId\": 50, \"nextTrackId\": 99, \"activeTrackId\": 1,"
            " \"playheadSamples\": 0, \"deviceSampleRateAtSave\": 48000.0,"
            " \"trackRowHeightPreset\": \"huge\","
            " \"tracks\": ["
            "  { \"id\": 1, \"name\": \"T1\", \"kind\": \"audio\", \"rowHeight\": -5 },"
            "  { \"id\": 2, \"name\": \"T2\", \"kind\": \"audio\", \"rowHeight\": \"abc\" },"
            "  { \"id\": 3, \"name\": \"T3\", \"kind\": \"audio\", \"rowHeight\": 1000000 },"
            "  { \"id\": 4, \"name\": \"T4\", \"kind\": \"audio\", \"rowHeight\": 72.4 },"
            "  { \"id\": 90, \"name\": \"Stereo Out\", \"kind\": \"master\" } ] }";
        expect(badProj.replaceWithText(json), "malformed: handcrafted project written");
        ProjectFileV1 bad;
        const juce::Result read = readProjectFile(badProj, bad);
        expect(read.wasOk(), "malformed: reader never fails on bad row-height values ("
                                 + read.getErrorMessage() + ")");
        int byId[5] = { -1, -1, -1, -1, -1 };
        for (const auto& t : bad.tracks)
        {
            if (t.id >= 1 && t.id <= 4)
            {
                byId[t.id] = t.rowHeightPx;
            }
        }
        expect(byId[1] == 0 && byId[2] == 0 && byId[3] == 0,
               "malformed: negative / non-numeric / absurd heights read as absent (0)");
        expect(byId[4] == 72, "malformed: a fractional height reads rounded (72)");
        expect(bad.trackRowHeightPreset == "huge"
                   && trh::presetFromPersistenceKey(bad.trackRowHeightPreset)
                          == trh::TrackRowHeightPreset::Medium,
               "malformed: unknown preset key repairs to Medium at apply level");

        // Apply-level clamping of out-of-range heights that DID parse.
        LanesFixture f;
        const std::vector<TrackId> rows = f.rowTrackIds();
        f.lanes->applyTrackRowHeightsFromLoadedProject(
            "medium", { { rows[0], 10 }, { rows[1], 5000 } });
        expect(f.rowHeights()[rows[0]] == trh::kSmallRowHeightPx,
               "malformed: a below-minimum saved height clamps to Small (64) on load");
        expect(f.rowHeights()[rows[1]] == 480, "malformed: an absurd saved height clamps to the 480 max");
        (void)badProj.deleteFile();
    }
}
// The PRODUCTION toolbar layout: the height dropdown sits between the Solo memory strip and the
// Pointer/Split (scissors) tool strip, same row height, and never overlaps either — including a
// narrow window and a widened header column (the tool strip's left clamp extends past the combo).
void testToolbarLayoutDropdownNeverOverlaps()
{
    struct DummySideStripHost final : collapsible_side_strip::Host
    {
        int width = 0;
        [[nodiscard]] int getSideStripWidth() const noexcept override { return width; }
        void setSideStripWidth(const int w) noexcept override { width = w; }
        [[nodiscard]] int getSideStripMaxWidth() const noexcept override { return 400; }
        [[nodiscard]] int getSideStripDefaultWidth() const noexcept override { return 260; }
        void sideStripLayoutChanged() override {}
    };

    LanesFixture f;
    UiPlayheadClock clock;
    TimelineRulerView ruler(f.session, f.transport, f.deviceManager, f.viewport, clock);
    juce::Component owner;
    juce::Component inspector;
    juce::MenuBarComponent menuBar;
    juce::TextButton plusButton;
    EditToolIconStrip toolStrip;
    juce::Label bpmLabel;
    juce::TextEditor bpmEditor;
    juce::ComboBox timeSigCombo, formatCombo, snapResCombo;
    juce::ToggleButton snapToggle;
    juce::TextButton followToggle;
    juce::Label countInLabel, keyDiagLabel;
    DummySideStripHost host;
    collapsible_side_strip::ResizeSplitter splitter(host);
    collapsible_side_strip::CollapsedKnob knob(host);
    SoloMemoryStrip soloStrip;
    juce::ComboBox heightCombo;
    int inspectorWidth = 200;

    const auto layoutAt = [&](const int w, const int h) {
        owner.setSize(w, h);
        mini_daw_app_transport::applyTransportControlsLayout(mini_daw_app_transport::TransportLayoutRefs{
            owner, ruler, *f.lanes, inspector, menuBar, plusButton, toolStrip, bpmLabel, bpmEditor,
            timeSigCombo, formatCombo, snapToggle, snapResCombo, followToggle, countInLabel,
            keyDiagLabel, nullptr, inspectorWidth, splitter, knob, nullptr, nullptr, &soloStrip,
            &heightCombo });
    };

    for (const int width : { 1600, 1100, 900 })
    {
        layoutAt(width, 700);
        const juce::String who = "toolbar @ " + juce::String(width) + " px";
        expect(!heightCombo.getBounds().isEmpty() && heightCombo.isVisible(),
               who + ": height dropdown is laid out and visible");
        expect(soloStrip.getBounds().isEmpty() || heightCombo.getX() >= soloStrip.getRight() + 1,
               who + ": dropdown starts right of the Solo memory strip (no overlap)");
        expect(toolStrip.getX() >= heightCombo.getRight() + 1,
               who + ": the Pointer/Split (scissors) tool group starts right of the dropdown (no overlap)");
        expect(!heightCombo.getBounds().intersects(soloStrip.getBounds())
                   && !heightCombo.getBounds().intersects(toolStrip.getBounds()),
               who + ": dropdown intersects neither the Solo memories nor the tool group");
        expect(heightCombo.getHeight() == toolStrip.getHeight()
                   && heightCombo.getY() == toolStrip.getY(),
               who + ": dropdown shares the tool row's height and baseline");
    }

    // A widened header column widens the Solo strip — the dropdown must keep following it.
    f.lanes->setTrackHeaderColumnWidthPx(240, false);
    layoutAt(1100, 700);
    expect(heightCombo.getX() >= soloStrip.getRight() + 1
               && toolStrip.getX() >= heightCombo.getRight() + 1,
           "toolbar, wide header column: Solo strip < dropdown < tool group still hold");
}
} // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI juceGui;

    testPresetChangesEveryRowIncludingOffViewport();
    testIndividualResizeAfterPresetIsCustomAndClamped();
    testNewTrackGetsLastChosenPresetAndDuplicateKeepsSourceHeight();
    testTopVisibleTrackPreservedAndScrollClamped();
    testUserEditVsLoadNotification();
    testProjectPersistenceRoundTrip();
    testOlderProjectsAndMalformedValues();
    testToolbarLayoutDropdownNeverOverlaps();

    std::printf("\n%d checks, %d failure(s)\n", checks, failures);
    return failures == 0 ? 0 : 1;
}

// ---------------------------------------------------------------------------
// Link seams — `Session.cpp` / `TrackLanesView.cpp` reference these plugin/instrument entry
// points, but this harness passes a null `PluginInsertHost` and registers no instrument timeline
// attachments, so the stubs are never executed (they keep VST3 hosting and the full instrument
// controller out of this console target).
// ---------------------------------------------------------------------------
#include "instruments/InstrumentTrackController.h"
#include "plugins/PluginInsertHost.h"

PluginTrackChain PluginInsertHost::exportChain(TrackId) const { jassertfalse; return {}; }
void PluginInsertHost::importChain(TrackId, const PluginTrackChain&) { jassertfalse; }
void PluginInsertHost::removeAllPlugins() noexcept { jassertfalse; }
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
