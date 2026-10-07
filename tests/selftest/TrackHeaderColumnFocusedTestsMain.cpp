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
#include "ui/TrackColourPalette.h"
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
        k.model.typeIcon = track_strip_glyphs::TrackTypeIcon::Audio;
        k.model.colourKey = TrackColourKey::Blue;
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
        k.model.typeIcon = track_strip_glyphs::TrackTypeIcon::Instrument;
        k.model.colourKey = TrackColourKey::Teal;
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
        k.model.typeIcon = track_strip_glyphs::TrackTypeIcon::Midi;
        k.model.colourKey = TrackColourKey::Green;
        k.model.active = true;
        kinds.push_back(std::move(k));
    }
    {
        // Live-MIDI slice: instrument destination row with WORKING Monitor + Arm (armed, monitoring,
        // activity dot) — the widest strip: [Instrument][Power][Mute][Monitor][Arm] + Alternatives.
        Kind k{ "instrument-live-midi", {}, {} };
        k.model.name = trackName;
        k.model.typeIcon = track_strip_glyphs::TrackTypeIcon::Instrument;
        k.model.colourKey = TrackColourKey::Ochre;
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
        k.model.typeIcon = track_strip_glyphs::TrackTypeIcon::Midi;
        k.model.colourKey = TrackColourKey::Orange;
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
        k.model.typeIcon = track_strip_glyphs::TrackTypeIcon::Group;
        k.model.colourKey = TrackColourKey::Purple;
        k.model.active = true;
        k.model.showRecordAndPowerStripCells = false;
        kinds.push_back(std::move(k));
    }
    {
        Kind k{ "master", {}, {} };
        k.model.name = trackName;
        k.model.typeIcon = track_strip_glyphs::TrackTypeIcon::Master;
        k.model.colourKey = TrackColourKey::DefaultGrey;
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
    using H = TrackHeaderView;
    info("constants: cell=" + juce::String(H::kStripControlCellWidthPx) + " px, group margin=" + juce::String(H::kHeaderGroupMarginPx)
         + ", segment(3 digits)=" + juce::String(H::colourSegmentWidthPxForDigits(3)) + ", title cells=" + juce::String(H::kHeaderTitleCellCount)
         + ", name min=" + juce::String(H::kHeaderNameMinWidthPx) + " => min column=" + juce::String(H::kMinimumHeaderColumnWidthPx)
         + " px, default=" + juce::String(H::kDefaultHeaderColumnWidthPx) + " px (logical / DPI-independent)");
    expect(H::colourSegmentWidthPxForDigits(3) == 44 && H::colourSegmentWidthPxForDigits(1) == 44 && H::colourSegmentWidthPxForDigits(4) == 51,
           "limits: the colour segment is 3 + 14 + 3 + digits*7 + 3 px with a 3-digit floor (44 / 51 px)");
    expect(H::kMinimumHeaderColumnWidthPx == 181,
           "limits: minimum column width is 8 + 44 + 3 + 3*22 + 4 + 48 + 8 = 181 px (icon/number segment + three title cells + name room)");
    expect(H::kDefaultHeaderColumnWidthPx == 200, "limits: default column width is minimum + 19 = 200 px");
    expect(TrackLanesView::kTrackHeaderColumnMinWidthPx == H::kMinimumHeaderColumnWidthPx
               && TrackLanesView::kTrackHeaderColumnDefaultWidthPx == H::kDefaultHeaderColumnWidthPx,
           "limits: TrackLanesView shares the header-derived limits (one source of truth)");
    expect(H::kHeaderResizeBandPx == 4 && H::kHeaderRowTopPadPx == 2 && H::kMinimumHeightForSecondRowPx == 53,
           "limits: resize band 4 px, title pad 2 px, second control row from 53 px (fits at Small = 56)");
}

/// Everything a header shows, with its bounds, for overlap / alignment checks.
struct Chrome
{
    juce::Rectangle<int> segment, icon, number, name, secondRow;
    Cells cells;
};

[[nodiscard]] Chrome chromeOf(const TrackHeaderView& v)
{
    Chrome c;
    c.segment = v.getColourSegmentBounds();
    c.icon = v.getTypeIconBounds();
    c.number = v.getTrackNumberBounds();
    c.name = v.getNameTextBounds();
    c.secondRow = v.getSecondRowStripBounds();
    c.cells = cellsOf(v);
    const juce::Rectangle<int> solo = v.getSoloButtonBounds();
    if (!solo.isEmpty())
    {
        c.cells.present.emplace_back("solo", solo);
        c.cells.rightMost = juce::jmax(c.cells.rightMost, solo.getRight());
    }
    return c;
}

[[nodiscard]] bool isTitleRowCell(const char* name)
{
    const juce::String n(name);
    return n == "power" || n == "mute" || n == "solo";
}

void testGeometryPerKindAndWidth(const juce::File& shotDir)
{
    namespace trh = track_row_heights;
    const juce::Colour bg(0xff333333);
    for (const int width : { 120, TrackLanesView::kTrackHeaderColumnMinWidthPx, TrackLanesView::kTrackHeaderColumnDefaultWidthPx, 240 })
    {
        const bool legacyWidth = width == 120;
        for (Kind& k : rowKinds("Track"))
        {
            const bool isMaster = juce::String(k.name) == "master";
            if (!isMaster)
            {
                k.model.soloAvailable = true;
                k.callbacks.onToggleSolo = [] {};
            }
            k.model.trackNumber = 123;
            k.model.trackNumberDigits = 3;
            auto view = std::make_unique<TrackHeaderView>(
                [m = k.model] { return m; }, k.callbacks, kInvalidTrackId, std::nullopt);
            view->setSize(width, trh::kMediumPresetPx);
            const Chrome ch = chromeOf(*view);
            const juce::Rectangle<int> local = view->getLocalBounds();
            const juce::Rectangle<int> chrome = local.withTrimmedBottom(TrackHeaderView::kHeaderResizeBandPx);
            bool allInside = true;
            bool allHittable = true;
            bool titleRowAligned = true;
            bool noOverlap = true;
            juce::String desc;
            for (const auto& [name, r] : ch.cells.present)
            {
                desc << " " << name << "=" << r.toString();
                allInside = allInside && local.contains(r);
                allHittable = allHittable && chrome.contains(r.getCentre());
                noOverlap = noOverlap && !r.intersects(ch.segment) && !r.intersects(ch.name);
                if (isTitleRowCell(name))
                {
                    titleRowAligned = titleRowAligned && r.getY() == ch.name.getY() && r.getHeight() == ch.name.getHeight()
                                      && r.getRight() <= ch.name.getX() && r.getX() >= ch.segment.getRight();
                }
                else
                {
                    titleRowAligned = titleRowAligned && r.getY() >= ch.name.getBottom();
                }
            }
            const juce::String who = juce::String(k.name) + " @ " + juce::String(width) + " px";
            info(who + ": cells=" + juce::String((int)ch.cells.present.size()) + " segment=" + ch.segment.toString()
                 + " name=" + ch.name.toString() + desc);
            if (legacyWidth)
            {
                // Negative control: at the pre-compact-header fixed 120 px width the icon / number segment,
                // the three title cells and the name cannot all fit (the name area collapses).
                expect(ch.name.getWidth() < TrackHeaderView::kHeaderNameMinWidthPx,
                       who + ": (old width) no usable name room next to segment + title cells — why the minimum grew");
                continue;
            }
            expect(allInside && !ch.cells.present.empty(), who + ": every present button is fully inside the header");
            expect(allHittable, who + ": every present button centre is hittable (above the resize band)");
            expect(noOverlap, who + ": no button overlaps the colour segment or the name area");
            expect(titleRowAligned, who + ": Power / Mute / Solo share the title row with the name (between segment and name); other cells sit below");
            expect(ch.segment.getX() == TrackHeaderView::kHeaderGroupMarginPx && ch.segment.getY() == 0
                       && ch.segment.getBottom() == local.getBottom() && ch.segment.contains(ch.icon) && ch.segment.contains(ch.number)
                       && ch.icon.getRight() <= ch.number.getX(),
                   who + ": colour segment right of the 8 px group margin, the FULL row height down to the separator (no band gap), icon before number inside it");
            {
                // The segment fill really reaches the bottom row of pixels (the resize band is a hit
                // area only, never a visible strip) and the plate shows no outline hairline.
                const juce::Image probe = view->createComponentSnapshot(local, false, 1.0f);
                const juce::Colour segFill = track_colour_palette::headerSegmentFill(k.model.colourKey);
                bool bottomRowIsSegment = true;
                for (int x = ch.segment.getX() + 1; x < ch.segment.getRight() - 1; ++x)
                {
                    bottomRowIsSegment = bottomRowIsSegment && probe.getPixelAt(x, local.getBottom() - 1) == segFill;
                }
                expect(bottomRowIsSegment, who + ": the segment colour fills the row's bottom pixel row (no gap above the separator)");
            }
            expect(ch.name.getWidth() >= TrackHeaderView::kHeaderNameMinWidthPx && ch.name.getRight() == width - TrackHeaderView::kHeaderOuterPadXPx,
                   who + ": name keeps at least " + juce::String(TrackHeaderView::kHeaderNameMinWidthPx) + " px and the 8 px right pad");
            {
                const bool hasSecondRowCells = !(isMaster || juce::String(k.name) == "group");
                const int titleStripX = view->getPowerButtonBounds().isEmpty() ? view->getMuteButtonBounds().getX()
                                                                               : view->getPowerButtonBounds().getX();
                expect(hasSecondRowCells == !ch.secondRow.isEmpty()
                           && (!hasSecondRowCells
                               || (ch.secondRow.getY() == ch.name.getBottom() + TrackHeaderView::kHeaderRowGapPx
                                   && ch.secondRow.getX() == titleStripX)),
                       who + ": at Medium the second row exists (for kinds with Monitor / Arm / editor) under the title row, starting at the title strip's x");
            }

            const juce::Image img = view->createComponentSnapshot(local, false, 1.0f);
            savePng(img, shotDir, "header-" + juce::String(k.name) + "-" + juce::String(width) + ".png");
            expect(countNonBackgroundPixels(img, ch.icon, bg) > 8, who + ": the type icon is painted");
            expect(countNonBackgroundPixels(img, ch.number, bg) > 20, who + ": the three-digit number is painted");
            if (!ch.cells.present.empty())
            {
                const auto& last = ch.cells.present.back();
                expect(countNonBackgroundPixels(img, last.second, bg) > 30,
                       who + ": the last present button (" + juce::String(last.first) + ") is actually painted");
            }
            // Live-MIDI rows: Monitor AND Arm cells both exist, are inside and are painted
            // (the orange monitoring face / red armed face are not the background).
            if (juce::String(k.name).endsWith("live-midi"))
            {
                const juce::Rectangle<int> mon = view->getMonitorButtonBounds();
                const juce::Rectangle<int> arm = view->getArmButtonBounds();
                expect(!mon.isEmpty() && !arm.isEmpty() && local.contains(mon) && local.contains(arm),
                       who + ": Monitor and Arm cells present and inside the header (second row)");
                expect(countNonBackgroundPixels(img, mon, bg) > 30 && countNonBackgroundPixels(img, arm, bg) > 30,
                       who + ": Monitor and Arm cells are painted");
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
            vs->setSize(width, track_row_heights::kMediumPresetPx);
            vl->setSize(width, track_row_heights::kMediumPresetPx);
            const Cells cs = cellsOf(*vs);
            const Cells cl = cellsOf(*vl);
            bool same = cs.present.size() == cl.present.size();
            for (size_t j = 0; same && j < cs.present.size(); ++j)
            {
                same = cs.present[j].second == cl.present[j].second;
            }
            expect(same && vs->getColourSegmentBounds() == vl->getColourSegmentBounds() && vs->getNameTextBounds() == vl->getNameTextBounds(),
                   juce::String(shortKinds[i].name) + " @ " + juce::String(width)
                       + " px: a very long name leaves every button, the segment and the name area exactly where a short name puts them");
            // The long name is painted only inside the header (ellipsized), never beyond it.
            const juce::Image img = vl->createComponentSnapshot(vl->getLocalBounds(), false, 1.0f);
            expect(img.getWidth() == width, juce::String(shortKinds[i].name) + " @ " + juce::String(width)
                                                + " px: header render stays exactly the column width with a long name");
        }
    }
    // Digit column: a 4-digit project widens the segment for EVERY header (shared column), the
    // name shrinks, the cells move right by exactly one digit advance, nothing overlaps.
    {
        std::vector<Kind> kinds = rowKinds("Track");
        Kind& k = kinds.front();
        k.model.trackNumberDigits = 3;
        auto v3 = std::make_unique<TrackHeaderView>([m = k.model] { return m; }, k.callbacks, kInvalidTrackId, std::nullopt);
        k.model.trackNumberDigits = 4;
        k.model.trackNumber = 1234;
        auto v4 = std::make_unique<TrackHeaderView>([m = k.model] { return m; }, k.callbacks, kInvalidTrackId, std::nullopt);
        v3->setSize(TrackLanesView::kTrackHeaderColumnDefaultWidthPx, track_row_heights::kMediumPresetPx);
        v4->setSize(TrackLanesView::kTrackHeaderColumnDefaultWidthPx, track_row_heights::kMediumPresetPx);
        expect(v4->getColourSegmentBounds().getWidth() == v3->getColourSegmentBounds().getWidth() + TrackHeaderView::kHeaderDigitAdvancePx
                   && v4->getMuteButtonBounds().getX() == v3->getMuteButtonBounds().getX() + TrackHeaderView::kHeaderDigitAdvancePx
                   && v4->getNameTextBounds().getWidth() == v3->getNameTextBounds().getWidth() - TrackHeaderView::kHeaderDigitAdvancePx
                   && !v4->getMuteButtonBounds().intersects(v4->getColourSegmentBounds()),
               "digits: a 4-digit column widens the segment by one digit advance and shifts the cells; no overlap");
    }
}

/// Selected (multi-selected) headers: one flat wash, no outline hairline — the bottom / top pixel
/// rows of the plate equal the interior, the active stripe stays, the segment colour stays.
void testSelectedHeadersHaveNoOutline()
{
    for (const bool active : { false, true })
    {
        Kind k = rowKinds("Selected row")[0]; // audio
        k.model.active = active;
        k.model.headerMultiSelected = true;
        k.model.trackNumber = 7;
        k.model.trackNumberDigits = 3;
        const int w = TrackLanesView::kTrackHeaderColumnDefaultWidthPx;
        const int h = track_row_heights::kMediumPresetPx;
        const TrackHeaderModel model = k.model;
        auto view = std::make_unique<TrackHeaderView>([model] { return model; }, std::move(k.callbacks), kInvalidTrackId, std::nullopt);
        view->setSize(w, h);
        const juce::Image img = view->createComponentSnapshot(view->getLocalBounds(), false, 1.0f);
        const juce::Rectangle<int> name = view->getNameTextBounds();
        const int x = name.getX() + 4; // plate area right of the cells, no text at the plate's edges
        const juce::Colour interior = img.getPixelAt(x, h / 2 + 20);
        expect(img.getPixelAt(x, h - 1) == interior && img.getPixelAt(x, h - 2) == interior && img.getPixelAt(x, 0) == interior
                   && img.getPixelAt(w - 1, h / 2 + 20) == interior,
               juce::String(active ? "active+" : "") + "selected: the wash is one flat colour to the plate's bottom, top and right edges (no hairline)");
        expect(img.getPixelAt(x, h - 1) != juce::Colour(0xff000000) && interior != track_strip_glyphs::headerFillColour(active),
               juce::String(active ? "active+" : "") + "selected: the selection wash is visible (differs from the plain plate) and is not a black line");
        const juce::Colour segFill = track_colour_palette::headerSegmentFill(k.model.colourKey);
        expect(img.getPixelAt(view->getColourSegmentBounds().getX() + 2, h - 2) == segFill,
               juce::String(active ? "active+" : "") + "selected: the colour segment keeps its exact track colour (painted over the wash)");
        if (active)
        {
            expect(img.getPixelAt(1, h / 2) == track_strip_glyphs::headerActiveStripeColour(),
                   "active+selected: the 4 px active-track stripe is still the clear left marker");
        }
    }
}

void testClampFormula()
{
    using V = TrackLanesView;
    constexpr int kMin = V::kTrackHeaderColumnMinWidthPx;     // 181
    constexpr int kDef = V::kTrackHeaderColumnDefaultWidthPx; // 200
    expect(V::clampHeaderColumnWidthForTotalWidth(kDef, 1200) == kDef, "clamp: default fits on a wide view");
    expect(V::clampHeaderColumnWidthForTotalWidth(100, 1200) == kMin, "clamp: below minimum -> minimum (181)");
    expect(V::clampHeaderColumnWidthForTotalWidth(154, 1200) == kMin && V::clampHeaderColumnWidthForTotalWidth(166, 1200) == kMin,
           "clamp: saved pre-compact-header widths (154 minimum / 166 default) are raised to the new minimum (181)");
    expect(V::clampHeaderColumnWidthForTotalWidth(240, 1200) == 240, "clamp: a wider saved preference is preserved when it fits");
    expect(V::clampHeaderColumnWidthForTotalWidth(240, 370) == 210,
           "clamp: narrow view keeps 160 px of lane area (370 - 160 = 210) before honouring the preference");
    expect(V::clampHeaderColumnWidthForTotalWidth(240, 330) == kMin,
           "clamp: when the 160 px lane reservation would push below the minimum, the minimum wins (330 - 160 = 170 < 181)");
    expect(V::clampHeaderColumnWidthForTotalWidth(240, 250) == kMin,
           "clamp: very narrow view never goes below the minimum while the view is at least that wide");
    expect(V::clampHeaderColumnWidthForTotalWidth(240, 100) == 100, "clamp: a view narrower than the minimum gets the whole view");
    expect(V::clampHeaderColumnWidthForTotalWidth(240, 0) == 0, "clamp: zero-width view -> 0");
    expect(V::clampHeaderColumnWidthForTotalWidth(144, 1920) == V::clampHeaderColumnWidthForTotalWidth(144, 1920),
           "clamp: pure function (same inputs, same output) — the ruler/overlay layout can call it independently");
}

// ---------------------------------------------------------------------------------------------
// The row-height grid (Micro + n x 14) and the compact title row: at every preset the title row
// (icon, number, Power / Mute / Solo, name) is fully present and hittable; the second row
// (Monitor / Arm / editor / alternatives) exists from Small on and is structurally absent below
// (empty bounds = no paint spec, no hit scan entry, no tooltip — one gate).
void testRowHeightGridAndCompactChrome(const juce::File& shotDir)
{
    namespace trh = track_row_heights;

    expect(trh::kMicroRowHeightPx == 28 && trh::kRowHeightStepPx == 14,
           "grid: Micro = 2 + 22 + 4 = 28 px, step = Micro / 2 = 14 px");
    expect(trh::kMicroPresetPx == 28 && trh::kMiniPresetPx == 42 && trh::kSmallPresetPx == 56 && trh::kMediumPresetPx == 98
               && trh::kLargePresetPx == 196,
           "grid: Micro 28 (n=0), Mini 42 (n=1), Small 56 (n=2), Medium 98 (n=5), Large 196 (n=12)");
    expect(trh::gridStepsForPreset(trh::TrackRowHeightPreset::Medium) >= trh::gridStepsForPreset(trh::TrackRowHeightPreset::Small) + 2
               && trh::gridStepsForPreset(trh::TrackRowHeightPreset::Large) >= trh::gridStepsForPreset(trh::TrackRowHeightPreset::Medium) + 2,
           "grid: Medium >= Small + 2 steps, Large >= Medium + 2 steps");
    expect(trh::presetMatchingHeightPx(28) == trh::TrackRowHeightPreset::Micro && trh::presetMatchingHeightPx(42) == trh::TrackRowHeightPreset::Mini
               && trh::presetMatchingHeightPx(56) == trh::TrackRowHeightPreset::Small && trh::presetMatchingHeightPx(98) == trh::TrackRowHeightPreset::Medium
               && trh::presetMatchingHeightPx(196) == trh::TrackRowHeightPreset::Large && !trh::presetMatchingHeightPx(70).has_value()
               && !trh::presetMatchingHeightPx(64).has_value() && !trh::presetMatchingHeightPx(96).has_value(),
           "grid: presetMatchingHeightPx is exact; grid heights that are no preset (70) and the old 64 / 96 are Custom");
    expect(trh::snapRowHeightPxToGrid(28) == 28 && trh::snapRowHeightPxToGrid(34) == 28 && trh::snapRowHeightPxToGrid(35) == 42
               && trh::snapRowHeightPxToGrid(49) == 56 && trh::snapRowHeightPxToGrid(96) == 98 && trh::snapRowHeightPxToGrid(10) == 28
               && trh::snapRowHeightPxToGrid(250) == 252 && trh::snapRowHeightPxToGrid(100000) == trh::kRowHeightSafetyMaxPx,
           "grid: snapping rounds to the nearest step (ties up), floors at Micro, allows steps above Large, caps at the safety max");
    expect(trh::clampRowHeightPx(64) == 64 && trh::clampRowHeightPx(10) == 28 && trh::clampRowHeightPx(5000) == trh::kRowHeightSafetyMaxPx
               && trh::kRowHeightSafetyMaxPx == 1120 && trh::isOnRowHeightGrid(trh::kRowHeightSafetyMaxPx),
           "grid: clamping (used by project load) keeps valid off-grid heights like the old 64; the 1120 px safety cap is grid-aligned");
    for (const auto p : { trh::TrackRowHeightPreset::Micro, trh::TrackRowHeightPreset::Mini, trh::TrackRowHeightPreset::Small,
                          trh::TrackRowHeightPreset::Medium, trh::TrackRowHeightPreset::Large })
    {
        expect(trh::presetFromPersistenceKey(trh::persistenceKeyForPreset(p)) == p,
               "grid: persistence key round trip for " + trh::displayNameForPreset(p) + " (\"" + trh::persistenceKeyForPreset(p) + "\")");
    }
    expect(trh::presetFromPersistenceKey("") == trh::TrackRowHeightPreset::Medium
               && trh::presetFromPersistenceKey("huge") == trh::TrackRowHeightPreset::Medium,
           "grid: absent/unknown persistence key repairs to Medium (older projects keep their look)");
    expect(trh::laneEventDetailForHeightPx(28) == trh::LaneEventDetail::Bars && trh::laneEventDetailForHeightPx(42) == trh::LaneEventDetail::Content
               && trh::laneEventDetailForHeightPx(56) == trh::LaneEventDetail::Full && trh::laneEventDetailForHeightPx(31) == trh::LaneEventDetail::Bars
               && trh::laneEventDetailForHeightPx(47) == trh::LaneEventDetail::Content,
           "detail: Micro = Bars, Mini = Compact, Small and up = Full (thresholds 32 / 48 px)");

    const juce::Colour bg(0xff333333);
    for (const int width : { TrackLanesView::kTrackHeaderColumnMinWidthPx, TrackLanesView::kTrackHeaderColumnDefaultWidthPx })
    {
        for (Kind& k : rowKinds("A long enough track name"))
        {
            const bool isMaster = juce::String(k.name) == "master";
            if (!isMaster)
            {
                k.model.soloAvailable = true;
                k.callbacks.onToggleSolo = [] {};
            }
            k.model.trackNumber = 100;
            k.model.trackNumberDigits = 3;
            const bool altAvailable = k.model.instrumentAlternativesAvailable;
            auto view = std::make_unique<TrackHeaderView>(
                [m = k.model] { return m; }, k.callbacks, kInvalidTrackId, std::nullopt);

            for (const auto preset : { trh::TrackRowHeightPreset::Micro, trh::TrackRowHeightPreset::Mini, trh::TrackRowHeightPreset::Small,
                                       trh::TrackRowHeightPreset::Medium })
            {
                const int h = trh::heightPxForPreset(preset);
                view->setSize(width, h);
                const Chrome ch = chromeOf(*view);
                const juce::Rectangle<int> local = view->getLocalBounds();
                const juce::Rectangle<int> chrome = local.withTrimmedBottom(TrackHeaderView::kHeaderResizeBandPx);
                const juce::String who = juce::String(k.name) + " @ " + juce::String(width) + " px, " + trh::displayNameForPreset(preset);
                const bool secondRowFits = h >= TrackHeaderView::kMinimumHeightForSecondRowPx;

                // Title row: icon, number, Power/Mute/Solo (as the kind has them) and the name are inside
                // and hittable at EVERY preset, including Micro.
                bool titleInside = chrome.contains(ch.icon) && chrome.contains(ch.number) && chrome.contains(ch.name);
                bool titleCellsPresent = !view->getMuteButtonBounds().isEmpty()
                                         && (isMaster || juce::String(k.name) == "group" || !view->getPowerButtonBounds().isEmpty())
                                         && (isMaster || !view->getSoloButtonBounds().isEmpty());
                bool secondRowCellsAbsent = view->getMonitorButtonBounds().isEmpty() && view->getArmButtonBounds().isEmpty()
                                            && view->getInstrumentEditorButtonBounds().isEmpty() && view->getAlternativesButtonBounds().isEmpty();
                for (const auto& [name, r] : ch.cells.present)
                {
                    titleInside = titleInside && chrome.contains(r) && chrome.contains(r.getCentre());
                    if (!isTitleRowCell(name))
                    {
                        secondRowCellsAbsent = false;
                    }
                }
                expect(titleInside && titleCellsPresent, who + ": title row (icon, number, Power/Mute/Solo, name) fully inside the chrome and hittable");
                expect(ch.name.getY() == TrackHeaderView::kHeaderRowTopPadPx && ch.name.getHeight() == TrackHeaderView::kStripControlCellWidthPx,
                       who + ": the title row is top-aligned at the 2 px pad with the 22 px cell height (it never jumps between heights)");
                if (secondRowFits)
                {
                    const bool hasAnySecond = (juce::String(k.name) != "group" && !isMaster);
                    expect(hasAnySecond == !ch.secondRow.isEmpty(), who + ": second control row present exactly for kinds that have Monitor / Arm / editor");
                    if (hasAnySecond)
                    {
                        expect(chrome.contains(ch.secondRow) && ch.secondRow.getY() == ch.name.getBottom() + TrackHeaderView::kHeaderRowGapPx,
                               who + ": second row sits under the title row inside the chrome");
                    }
                    if (altAvailable)
                    {
                        expect(!view->getAlternativesButtonBounds().isEmpty() && chrome.contains(view->getAlternativesButtonBounds()),
                               who + ": alternatives cell present on the second row");
                    }
                }
                else
                {
                    expect(ch.secondRow.isEmpty() && secondRowCellsAbsent,
                           who + ": NO second-row cell (Monitor / Arm / editor / alternatives) below Small — empty bounds, nothing hittable");
                }
                // Resize band: the bottom 4 px, below every cell (even in Micro the band never
                // overlaps a title cell).
                bool bandClear = true;
                for (const auto& [name, r] : ch.cells.present)
                {
                    juce::ignoreUnused(name);
                    bandClear = bandClear && r.getBottom() <= chrome.getBottom();
                }
                expect(bandClear, who + ": the 4 px resize band lies below every control cell");

                const juce::Image img = view->createComponentSnapshot(local, false, 1.0f);
                savePng(img, shotDir, "header-" + trh::displayNameForPreset(preset).toLowerCase() + "-" + juce::String(k.name) + "-" + juce::String(width) + ".png");
                expect(countNonBackgroundPixels(img, ch.number, bg) > 20 && countNonBackgroundPixels(img, ch.icon, bg) > 8,
                       who + ": number and type icon are painted");
                expect(countNonBackgroundPixels(img, view->getMuteButtonBounds(), bg) > 30, who + ": the M cell is painted");
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
    testSelectedHeadersHaveNoOutline();
    testRowHeightGridAndCompactChrome(shotDir);
    testUiLayoutStore();

    std::printf("\n%d checks, %d failure(s)\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
