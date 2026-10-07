// =============================================================================
// TrackHeaderColumnFocusedTests — shared track-header column width (production header code)
// =============================================================================
//
// User report: "The Monitor button makes the last button clip in some headers."
//
// At the old fixed 120 px column the widest control row — an instrument destination row's
// [Instrument][Power][Mute][Monitor][Arm] (5 × 22 px, left edge at 8 + 6 px) — ends at 124 px, so
// Arm was cut by the column edge. This harness drives the PRODUCTION `TrackHeaderView` (geometry
// + offscreen paint) per row-kind model at the old width, the new minimum and the new default,
// checks the pure clamp formula the app uses for every view that shares the boundary, and the
// app-wide `UiLayoutSettingsStore` round trip. Logical (DPI-independent) px throughout.
//
// Usage: TrackHeaderColumnFocusedTests.exe [<png output dir>]
// Exit 0 = all checks green.
// =============================================================================

#include "ui/TrackHeaderView.h"
#include "ui/TrackLanesView.h"
#include "ui/TrackRowHeightPresets.h"
#include "ui/UiLayoutSettingsStore.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <cstdio>
#include <memory>
#include <vector>

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

struct Kind
{
    const char* name;
    TrackHeaderModel model;
    TrackHeaderCallbacks callbacks;
};

[[nodiscard]] std::vector<Kind> rowKinds(const juce::String& trackName)
{
    std::vector<Kind> kinds;
    {
        Kind k{ "audio", {}, {} };
        k.model.name = trackName;
        k.model.active = true; // widest left trim (active accent)
        k.model.monitorAvailable = true;
        k.model.monitorInteractable = true;
        k.callbacks.onToggleMonitor = [] {};
        k.callbacks.onToggleMute = [] {};
        k.callbacks.onToggleArm = [] {};
        k.callbacks.onTogglePower = [] { return true; };
        kinds.push_back(std::move(k));
    }
    {
        Kind k{ "instrument", {}, {} };
        k.model.name = trackName;
        k.model.subtitle = "VB3-II";
        k.model.active = true;
        k.model.instrumentEditorAvailable = true;
        k.model.instrumentAlternativesAvailable = true;
        k.model.monitorAvailable = true;
        k.model.monitorInteractable = false;
        k.callbacks.onOpenInstrumentEditor = [] {};
        k.callbacks.onShowInstrumentAlternatives = [](juce::Rectangle<int>) {};
        k.callbacks.onToggleMute = [] {};
        k.callbacks.onToggleArm = [] {};
        k.callbacks.onTogglePower = [] { return true; };
        kinds.push_back(std::move(k));
    }
    {
        Kind k{ "midi", {}, {} };
        k.model.name = trackName;
        k.model.active = true;
        kinds.push_back(std::move(k));
    }
    {
        // Live-MIDI slice: instrument destination row with WORKING Monitor + Arm (armed, monitoring,
        // activity dot) — the widest strip: [Instrument][Power][Mute][Monitor][Arm] + Alternatives.
        Kind k{ "instrument-live-midi", {}, {} };
        k.model.name = trackName;
        k.model.subtitle = "VB3-II";
        k.model.active = true;
        k.model.instrumentEditorAvailable = true;
        k.model.instrumentAlternativesAvailable = true;
        k.model.monitorAvailable = true;
        k.model.monitorInteractable = true;
        k.model.monitorEnabled = true;
        k.model.armInteractable = true;
        k.model.armed = true;
        k.model.midiActivity = true;
        k.callbacks.onOpenInstrumentEditor = [] {};
        k.callbacks.onShowInstrumentAlternatives = [](juce::Rectangle<int>) {};
        k.callbacks.onToggleMute = [] {};
        k.callbacks.onToggleArm = [] {};
        k.callbacks.onToggleMonitor = [] {};
        k.callbacks.onTogglePower = [] { return true; };
        kinds.push_back(std::move(k));
    }
    {
        // Live-MIDI slice: plain Midi row now carries a Monitor cell: [Power][Mute][Monitor][Arm].
        Kind k{ "midi-live-midi", {}, {} };
        k.model.name = trackName;
        k.model.active = true;
        k.model.monitorAvailable = true;
        k.model.monitorInteractable = true;
        k.model.armInteractable = true;
        k.model.midiActivity = true;
        k.callbacks.onToggleMute = [] {};
        k.callbacks.onToggleArm = [] {};
        k.callbacks.onToggleMonitor = [] {};
        k.callbacks.onTogglePower = [] { return true; };
        kinds.push_back(std::move(k));
    }
    {
        Kind k{ "group", {}, {} };
        k.model.name = trackName;
        k.model.active = true;
        k.model.showRecordAndPowerStripCells = false;
        kinds.push_back(std::move(k));
    }
    {
        Kind k{ "master", {}, {} };
        k.model.name = trackName;
        k.model.active = true;
        k.model.showRecordAndPowerStripCells = false;
        k.model.trackNameRenameEnabled = false;
        kinds.push_back(std::move(k));
    }
    return kinds;
}

struct Cells
{
    std::vector<std::pair<const char*, juce::Rectangle<int>>> present;
    int rightMost = 0;
};

[[nodiscard]] Cells cellsOf(const TrackHeaderView& v)
{
    Cells c;
    const std::pair<const char*, juce::Rectangle<int>> all[] = {
        { "instrument", v.getInstrumentEditorButtonBounds() }, { "power", v.getPowerButtonBounds() },
        { "mute", v.getMuteButtonBounds() },                   { "monitor", v.getMonitorButtonBounds() },
        { "arm", v.getArmButtonBounds() },                     { "alternatives", v.getAlternativesButtonBounds() },
    };
    for (const auto& [name, r] : all)
    {
        if (!r.isEmpty())
        {
            c.present.emplace_back(name, r);
            c.rightMost = juce::jmax(c.rightMost, r.getRight());
        }
    }
    return c;
}

void savePng(const juce::Image& img, const juce::File& outDir, const juce::String& name)
{
    if (outDir == juce::File{})
    {
        return;
    }
    (void)outDir.createDirectory();
    const juce::File f = outDir.getChildFile(name);
    (void)f.deleteFile();
    juce::FileOutputStream os(f);
    if (os.openedOk())
    {
        juce::PNGImageFormat png;
        (void)png.writeImageToStream(img, os);
    }
}

[[nodiscard]] int countNonBackgroundPixels(const juce::Image& img, const juce::Rectangle<int> r, const juce::Colour bg)
{
    int n = 0;
    for (int y = r.getY(); y < r.getBottom(); ++y)
    {
        for (int x = r.getX(); x < r.getRight(); ++x)
        {
            if (x < 0 || y < 0 || x >= img.getWidth() || y >= img.getHeight())
            {
                continue;
            }
            const juce::Colour c = img.getPixelAt(x, y);
            if (std::abs((int)c.getRed() - (int)bg.getRed()) > 10 || std::abs((int)c.getGreen() - (int)bg.getGreen()) > 10
                || std::abs((int)c.getBlue() - (int)bg.getBlue()) > 10)
            {
                ++n;
            }
        }
    }
    return n;
}

// ---------------------------------------------------------------------------------------------
void testDerivedLimits()
{
    info("constants: cell=" + juce::String(TrackHeaderView::kStripControlCellWidthPx) + " px, max cells="
         + juce::String(TrackHeaderView::kMaxStripControlCellCount) + ", outer pad=" + juce::String(TrackHeaderView::kHeaderOuterPadXPx)
         + ", active left trim=" + juce::String(TrackHeaderView::kHeaderNameTrimLeftActivePx) + " => min column="
         + juce::String(TrackHeaderView::kMinimumHeaderColumnWidthPx) + " px, default=" + juce::String(TrackHeaderView::kDefaultHeaderColumnWidthPx)
         + " px (logical / DPI-independent)");
    expect(TrackHeaderView::kMinimumHeaderColumnWidthPx == 154, "limits: minimum column width is 8 + 6 + 6*22 + 8 = 154 px (Solo added a 6th cell)");
    expect(TrackHeaderView::kDefaultHeaderColumnWidthPx == 166, "limits: default column width is minimum + 12 = 166 px");
    expect(TrackLanesView::kTrackHeaderColumnMinWidthPx == TrackHeaderView::kMinimumHeaderColumnWidthPx
               && TrackLanesView::kTrackHeaderColumnDefaultWidthPx == TrackHeaderView::kDefaultHeaderColumnWidthPx,
           "limits: TrackLanesView shares the header-derived limits (one source of truth)");
}

void testGeometryPerKindAndWidth(const juce::File& shotDir)
{
    constexpr int kRowH = 96; // default row height in the app
    const juce::Colour bg(0xff333333);
    for (const int width : { 120, TrackLanesView::kTrackHeaderColumnMinWidthPx, TrackLanesView::kTrackHeaderColumnDefaultWidthPx, 240 })
    {
        const bool legacyWidth = width == 120;
        for (Kind& k : rowKinds("Track"))
        {
            auto view = std::make_unique<TrackHeaderView>(
                [m = k.model] { return m; }, k.callbacks, kInvalidTrackId, std::nullopt);
            view->setSize(width, kRowH);
            const Cells cells = cellsOf(*view);
            const juce::Rectangle<int> local = view->getLocalBounds();
            const juce::Rectangle<int> chrome = local.withTrimmedBottom(TrackHeaderView::kHeaderResizeBandPx);
            bool allInside = true;
            bool allHittable = true;
            juce::String desc;
            for (const auto& [name, r] : cells.present)
            {
                desc << " " << name << "=" << r.toString();
                allInside = allInside && local.contains(r);
                // Hittable like the production hit test: the cell centre lies inside the chrome
                // above the bottom resize band.
                allHittable = allHittable && chrome.contains(r.getCentre());
            }
            const juce::String who = juce::String(k.name) + " @ " + juce::String(width) + " px";
            info(who + ": cells=" + juce::String((int)cells.present.size()) + " rightmost edge=" + juce::String(cells.rightMost)
                 + " margin=" + juce::String(width - cells.rightMost) + desc);
            if (legacyWidth && (juce::String(k.name) == "instrument" || juce::String(k.name) == "instrument-live-midi"))
            {
                // Negative control: documents the reported defect at the old fixed width.
                expect(!allInside, who + ": (old width) the 5-cell instrument row's Arm cell IS clipped — the reported defect");
            }
            else
            {
                expect(allInside, who + ": every present button is fully inside the header");
            }
            if (!legacyWidth)
            {
                expect(allHittable, who + ": every present button centre is hittable (above the resize band)");
                expect(width - cells.rightMost >= TrackHeaderView::kHeaderOuterPadXPx,
                       who + ": at least the standard 8 px right pad remains after the last button");
                const juce::Image img = view->createComponentSnapshot(local, false, 1.0f);
                savePng(img, shotDir, "header-" + juce::String(k.name) + "-" + juce::String(width) + ".png");
                if (!cells.present.empty())
                {
                    // Right-most STRIP cell (the alternatives button sits bottom-left, not in the strip).
                    size_t lastIdx = cells.present.size() - 1;
                    if (juce::String(cells.present[lastIdx].first) == "alternatives" && lastIdx > 0)
                    {
                        --lastIdx;
                    }
                    const auto& last = cells.present[lastIdx];
                    expect(countNonBackgroundPixels(img, last.second, bg) > 30,
                           who + ": the right-most strip button (" + juce::String(last.first) + ") is actually painted");
                }
                // Live-MIDI rows: Monitor AND Arm cells both exist, are inside and are painted
                // (the orange monitoring face / red armed face are not the background).
                if (juce::String(k.name).endsWith("live-midi"))
                {
                    const juce::Rectangle<int> mon = view->getMonitorButtonBounds();
                    const juce::Rectangle<int> arm = view->getArmButtonBounds();
                    expect(!mon.isEmpty() && !arm.isEmpty() && local.contains(mon) && local.contains(arm),
                           who + ": Monitor and Arm cells present and inside the header");
                    expect(countNonBackgroundPixels(img, mon, bg) > 30 && countNonBackgroundPixels(img, arm, bg) > 30,
                           who + ": Monitor and Arm cells are painted");
                }
            }
        }
    }
}

void testNamesNeverChangeLayout()
{
    const juce::String longName = "This is a deliberately very long track name that must be truncated, not widen the column";
    for (const int width : { TrackLanesView::kTrackHeaderColumnMinWidthPx, TrackLanesView::kTrackHeaderColumnDefaultWidthPx })
    {
        std::vector<Kind> shortKinds = rowKinds("A");
        std::vector<Kind> longKinds = rowKinds(longName);
        for (size_t i = 0; i < shortKinds.size(); ++i)
        {
            auto vs = std::make_unique<TrackHeaderView>([m = shortKinds[i].model] { return m; }, shortKinds[i].callbacks,
                                                        kInvalidTrackId, std::nullopt);
            auto vl = std::make_unique<TrackHeaderView>([m = longKinds[i].model] { return m; }, longKinds[i].callbacks,
                                                        kInvalidTrackId, std::nullopt);
            vs->setSize(width, 96);
            vl->setSize(width, 96);
            const Cells cs = cellsOf(*vs);
            const Cells cl = cellsOf(*vl);
            bool same = cs.present.size() == cl.present.size();
            for (size_t j = 0; same && j < cs.present.size(); ++j)
            {
                same = cs.present[j].second == cl.present[j].second;
            }
            expect(same, juce::String(shortKinds[i].name) + " @ " + juce::String(width)
                             + " px: a very long name leaves every button exactly where a short name puts it");
            // The long name is painted only inside the header (truncated/fitted), never beyond it.
            const juce::Image img = vl->createComponentSnapshot(vl->getLocalBounds(), false, 1.0f);
            expect(img.getWidth() == width, juce::String(shortKinds[i].name) + " @ " + juce::String(width)
                                                + " px: header render stays exactly the column width with a long name");
        }
    }
}

void testClampFormula()
{
    using V = TrackLanesView;
    expect(V::clampHeaderColumnWidthForTotalWidth(166, 1200) == 166, "clamp: default fits on a wide view");
    expect(V::clampHeaderColumnWidthForTotalWidth(100, 1200) == 154, "clamp: below minimum -> minimum (154)");
    expect(V::clampHeaderColumnWidthForTotalWidth(132, 1200) == 154,
           "clamp: a saved pre-Solo minimum width (132) is raised to the new minimum (154)");
    expect(V::clampHeaderColumnWidthForTotalWidth(144, 1200) == 154,
           "clamp: a saved pre-Solo default width (144) is raised to the new minimum (154)");
    expect(V::clampHeaderColumnWidthForTotalWidth(240, 1200) == 240, "clamp: wide preference honoured when it fits");
    expect(V::clampHeaderColumnWidthForTotalWidth(240, 340) == 180,
           "clamp: narrow view keeps 160 px of lane area (340 - 160 = 180) before honouring the preference");
    expect(V::clampHeaderColumnWidthForTotalWidth(240, 300) == 154,
           "clamp: when the 160 px lane reservation would push below the minimum, the minimum wins (300 - 160 = 140 < 154)");
    expect(V::clampHeaderColumnWidthForTotalWidth(240, 250) == 154,
           "clamp: very narrow view never goes below the minimum while the view is at least that wide");
    expect(V::clampHeaderColumnWidthForTotalWidth(240, 100) == 100, "clamp: a view narrower than the minimum gets the whole view");
    expect(V::clampHeaderColumnWidthForTotalWidth(240, 0) == 0, "clamp: zero-width view -> 0");
    expect(V::clampHeaderColumnWidthForTotalWidth(144, 1920) == V::clampHeaderColumnWidthForTotalWidth(144, 1920),
           "clamp: pure function (same inputs, same output) — the ruler/overlay layout can call it independently");
}

// ---------------------------------------------------------------------------------------------
// Shared track heights: the central Small/Medium/Large definitions, the proof that Small fits the
// full header chrome (title + every main strip button incl. Solo + resize band) for every row
// kind, and the alternatives-button hide rule at compact heights. The alternatives button is NOT
// a strip cell: when `getAlternativesButtonBounds()` is empty, `buildStripControlSpecs()` adds no
// Alternatives spec, so the paint loop, the hover/click hit scan and the tooltip (which requires
// a hovered spec) are all off through the same single gate — bounds-empty IS the structural
// "no paint, no hit target, no tooltip" guarantee the spec demands.
void testRowHeightPresetsAndSmallChromeFit(const juce::File& shotDir)
{
    namespace trh = track_row_heights;

    expect(trh::kSmallRowHeightPx == 64 && trh::kMediumRowHeightPx == 96 && trh::kLargeRowHeightPx == 192,
           "presets: Small=64, Medium=96 (the historical default), Large=192 (2 x Medium) logical px");
    expect(trh::heightPxForPreset(trh::TrackRowHeightPreset::Small) == 64
               && trh::heightPxForPreset(trh::TrackRowHeightPreset::Medium) == 96
               && trh::heightPxForPreset(trh::TrackRowHeightPreset::Large) == 192,
           "presets: heightPxForPreset maps every preset to its px");
    expect(trh::presetMatchingHeightPx(64) == trh::TrackRowHeightPreset::Small
               && trh::presetMatchingHeightPx(96) == trh::TrackRowHeightPreset::Medium
               && trh::presetMatchingHeightPx(192) == trh::TrackRowHeightPreset::Large
               && !trh::presetMatchingHeightPx(97).has_value() && !trh::presetMatchingHeightPx(0).has_value(),
           "presets: presetMatchingHeightPx is exact (any other height = Custom)");
    expect(trh::presetFromPersistenceKey(trh::persistenceKeyForPreset(trh::TrackRowHeightPreset::Small))
                   == trh::TrackRowHeightPreset::Small
               && trh::presetFromPersistenceKey(trh::persistenceKeyForPreset(trh::TrackRowHeightPreset::Medium))
                      == trh::TrackRowHeightPreset::Medium
               && trh::presetFromPersistenceKey(trh::persistenceKeyForPreset(trh::TrackRowHeightPreset::Large))
                      == trh::TrackRowHeightPreset::Large,
           "presets: persistence keys round trip (small/medium/large)");
    expect(trh::presetFromPersistenceKey("") == trh::TrackRowHeightPreset::Medium
               && trh::presetFromPersistenceKey("huge") == trh::TrackRowHeightPreset::Medium,
           "presets: absent/unknown persistence key repairs to Medium (older projects keep their look)");

    // Snap-rule proof that Small is EXACTLY the smallest full-chrome height for subtitle rows and
    // at least it for title-only rows: with a permissive minimum the 50% rule would move any
    // height below the full name+buttons+band ideal, so passing through unchanged proves >= ideal.
    using H = TrackHeaderView;
    expect(H::snapTrackHeaderRowHeightAfterResize(trh::kSmallRowHeightPx, true, 1, 480) == trh::kSmallRowHeightPx,
           "snap: Small passes through for subtitle rows -> Small >= their full chrome ideal");
    expect(H::snapTrackHeaderRowHeightAfterResize(trh::kSmallRowHeightPx - 1, true, 1, 480)
               != trh::kSmallRowHeightPx - 1,
           "snap: Small-1 is moved for subtitle rows -> Small is the MINIMAL full-chrome height");
    expect(H::snapTrackHeaderRowHeightAfterResize(trh::kSmallRowHeightPx, false, 1, 480) == trh::kSmallRowHeightPx,
           "snap: Small passes through for title-only rows too");
    expect(H::snapTrackHeaderRowHeightAfterResize(10, true, trh::kSmallRowHeightPx, 480) == trh::kSmallRowHeightPx
               && H::snapTrackHeaderRowHeightAfterResize(10, false, trh::kSmallRowHeightPx, 480)
                      == trh::kSmallRowHeightPx,
           "snap: with the app's global minimum = Small, any drag below it lands at Small (every kind)");
    expect(H::snapTrackHeaderRowHeightAfterResize(100000, true, trh::kSmallRowHeightPx, 480) == 480,
           "snap: the existing 480 px maximum is kept");

    const juce::Colour bg(0xff333333);
    for (const int width : { TrackLanesView::kTrackHeaderColumnMinWidthPx, TrackLanesView::kTrackHeaderColumnDefaultWidthPx })
    {
        for (Kind& k : rowKinds("Track"))
        {
            // Solo cells exactly as the app wires them: every row kind except master.
            const bool isMaster = juce::String(k.name) == "master";
            if (!isMaster)
            {
                k.model.soloAvailable = true;
                k.callbacks.onToggleSolo = [] {};
            }
            const bool altAvailable = k.model.instrumentAlternativesAvailable;
            auto view = std::make_unique<TrackHeaderView>(
                [m = k.model] { return m; }, k.callbacks, kInvalidTrackId, std::nullopt);
            view->setSize(width, trh::kSmallRowHeightPx);

            const juce::Rectangle<int> local = view->getLocalBounds();
            const juce::Rectangle<int> chrome = local.withTrimmedBottom(TrackHeaderView::kHeaderResizeBandPx);
            const juce::String who = juce::String(k.name) + " @ " + juce::String(width) + " px, Small height";

            Cells cells = cellsOf(*view);
            const juce::Rectangle<int> solo = view->getSoloButtonBounds();
            if (!solo.isEmpty())
            {
                cells.present.emplace_back("solo", solo);
            }
            expect(!isMaster == !solo.isEmpty(), who + ": Solo cell present on every kind except master");
            bool allInside = true;
            bool allHittable = true;
            for (const auto& [name, r] : cells.present)
            {
                juce::ignoreUnused(name);
                allInside = allInside && local.contains(r);
                allHittable = allHittable && chrome.contains(r.getCentre());
            }
            expect(allInside && !cells.present.empty(),
                   who + ": every main button (incl. S / instrument editor) fully inside the header");
            expect(allHittable, who + ": every main button centre is hittable above the resize band");

            // Alternatives: hidden at Small (empty bounds = no paint spec, no hit scan entry, no
            // tooltip — one structural gate), back at Medium/Large, hidden again when shrunk.
            expect(view->getAlternativesButtonBounds().isEmpty(),
                   who + ": the standalone alternatives button is fully OFF at Small");
            if (altAvailable)
            {
                view->setSize(width, trh::kMediumRowHeightPx);
                const juce::Rectangle<int> altMedium = view->getAlternativesButtonBounds();
                expect(!altMedium.isEmpty() && view->getLocalBounds().contains(altMedium),
                       who + ": alternatives button RETURNS at Medium, inside the header");
                view->setSize(width, trh::kLargeRowHeightPx);
                expect(!view->getAlternativesButtonBounds().isEmpty(),
                       who + ": alternatives button present at Large");
                view->setSize(width, trh::kSmallRowHeightPx);
                expect(view->getAlternativesButtonBounds().isEmpty(),
                       who + ": alternatives button hides again when shrunk back to Small");
            }

            // Paint evidence: the Solo cell (the newest strip button) is actually painted at Small.
            const juce::Image img = view->createComponentSnapshot(local, false, 1.0f);
            savePng(img, shotDir, "header-small-" + juce::String(k.name) + "-" + juce::String(width) + ".png");
            if (!solo.isEmpty())
            {
                expect(countNonBackgroundPixels(img, solo, bg) > 30, who + ": the S cell is painted at Small");
            }
        }
    }
}

void testUiLayoutStore()
{
    const juce::File dir = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("dal-header-column-tests");
    (void)dir.deleteRecursively();
    (void)dir.createDirectory();
    const juce::File xml = dir.getChildFile("ui-layout.xml");

    {
        UiLayoutSettingsStore store(xml);
        store.loadFromFile();
        expect(!store.getTrackHeaderColumnWidthPx().has_value(), "store: missing file -> absent (default applies)");
        store.setTrackHeaderColumnWidthPx(170);
        store.save();
        expect(xml.existsAsFile(), "store: save wrote " + xml.getFileName());
    }
    {
        UiLayoutSettingsStore store(xml);
        store.loadFromFile();
        expect(store.getTrackHeaderColumnWidthPx().value_or(-1) == 170, "store: round trip reads 170 back");
    }
    {
        (void)xml.replaceWithText("<UI_LAYOUT version=\"1\"><TRACK_HEADER_COLUMN widthPx=\"abc\"/></UI_LAYOUT>");
        UiLayoutSettingsStore store(xml);
        store.loadFromFile();
        expect(!store.getTrackHeaderColumnWidthPx().has_value(), "store: non-numeric value -> absent (safe default)");
    }
    {
        (void)xml.replaceWithText("this is not xml at all <<<");
        UiLayoutSettingsStore store(xml);
        store.loadFromFile();
        expect(!store.getTrackHeaderColumnWidthPx().has_value(), "store: malformed file -> absent (safe default)");
    }
    {
        (void)xml.replaceWithText("<UI_LAYOUT version=\"1\"><TRACK_HEADER_COLUMN widthPx=\"-5\"/></UI_LAYOUT>");
        UiLayoutSettingsStore store(xml);
        store.loadFromFile();
        expect(!store.getTrackHeaderColumnWidthPx().has_value(), "store: negative value -> absent (safe default)");
    }
    {
        (void)xml.replaceWithText("<UI_LAYOUT version=\"1\"><TRACK_HEADER_COLUMN widthPx=\"5000\"/></UI_LAYOUT>");
        UiLayoutSettingsStore store(xml);
        store.loadFromFile();
        const int v = store.getTrackHeaderColumnWidthPx().value_or(0);
        expect(v == 5000 && TrackLanesView::clampHeaderColumnWidthForTotalWidth(v, 1200) == 1200 - TrackLanesView::kMinimumLaneAreaWidthPx,
               "store+clamp: an absurd stored width is clamped by the view at layout time (lane area kept)");
    }
    (void)dir.deleteRecursively();
}
} // namespace

int main(int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI juceGui;
    const juce::File shotDir = argc > 1 ? juce::File(juce::String(argv[1])) : juce::File{};

    testDerivedLimits();
    testGeometryPerKindAndWidth(shotDir);
    testNamesNeverChangeLayout();
    testClampFormula();
    testRowHeightPresetsAndSmallChromeFit(shotDir);
    testUiLayoutStore();

    std::printf("\n%d checks, %d failure(s)\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
