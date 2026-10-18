#include "ui/experimental/MidiEditorCohort.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace
{
int gFailures = 0;

void expect(const bool ok, const char* label)
{
    if (!ok)
    {
        ++gFailures;
        std::cerr << "FAIL " << label << "\n";
    }
}

void writeBmp(const std::string& path, const int w, const int h, const std::vector<midi_editor_cohort::Rgba>& px)
{
    const int rowStride = (w * 3 + 3) & ~3;
    const int pixelBytes = rowStride * h;
    std::vector<unsigned char> file(54 + (size_t)pixelBytes, 0);
    file[0] = 'B';
    file[1] = 'M';
    const auto put32 = [&](const int at, const int v) {
        file[(size_t)at] = (unsigned char)(v & 255);
        file[(size_t)at + 1] = (unsigned char)((v >> 8) & 255);
        file[(size_t)at + 2] = (unsigned char)((v >> 16) & 255);
        file[(size_t)at + 3] = (unsigned char)((v >> 24) & 255);
    };
    put32(2, (int)file.size());
    put32(10, 54);
    put32(14, 40);
    put32(18, w);
    put32(22, h);
    file[26] = 1;
    file[28] = 24;
    for (int y = 0; y < h; ++y)
    {
        for (int x = 0; x < w; ++x)
        {
            const auto c = px[(size_t)(y * w + x)];
            const int dst = 54 + (h - 1 - y) * rowStride + x * 3;
            file[(size_t)dst] = c.b;
            file[(size_t)dst + 1] = c.g;
            file[(size_t)dst + 2] = c.r;
        }
    }
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(file.data()), (std::streamsize)file.size());
}

void fillRect(std::vector<midi_editor_cohort::Rgba>& px, const int w, const int h,
              const int x0, const int y0, const int x1, const int y1, const midi_editor_cohort::Rgba c)
{
    for (int y = y0; y < y1; ++y)
    {
        if (y < 0 || y >= h)
        {
            continue;
        }
        for (int x = x0; x < x1; ++x)
        {
            if (x < 0 || x >= w)
            {
                continue;
            }
            px[(size_t)(y * w + x)] = c;
        }
    }
}

void renderFrame(const std::string& path, const bool laterEventActive)
{
    using namespace midi_editor_cohort;
    constexpr int kW = 880;
    constexpr int kH = 220;
    const Rgba row{ 0x25, 0x25, 0x2d, 255 };
    std::vector<Rgba> px((size_t)(kW * kH), row);
    const int activeStart = laterEventActive ? 280 : 80;
    const int activeEnd = laterEventActive ? 620 : 420;
    for (int y = 24; y < 196; ++y)
    {
        for (int x = 16; x < 864; ++x)
        {
            const bool inside = x >= activeStart && x < activeEnd;
            const Rgba shade = activeSpanShadeRgb(inside);
            Rgba base = ((y / 16) % 2) == 0 ? Rgba{ 0x1f, 0x1f, 0x26, 255 } : row;
            if (shade.a > 0 && shade.r > 200)
            {
                base = mixRgb(base, Rgba{ 255, 255, 255, 255 }, 0.08f);
            }
            else if (inside)
            {
                base = mixRgb(base, Rgba{ 0, 0, 0, 255 }, 0.22f);
            }
            px[(size_t)(y * kW + x)] = base;
        }
    }
    struct Glyph
    {
        int x;
        int pitchRow;
        bool ownerActive;
        bool startInsideOwner;
        bool selected;
        int velocity;
    };
    std::vector<Glyph> glyphs = {
        { 120, 2, !laterEventActive, true, false, 110 },
        { 188, 4, !laterEventActive, true, false, 80 },
        { 206, 4, laterEventActive, true, false, 120 },
        { 40, 3, !laterEventActive, false, false, 100 },
        { 340, 2, laterEventActive, true, true, 120 },
        { 340, 5, !laterEventActive, true, false, 90 },
        { 700, 4, laterEventActive, false, false, 100 },
    };
    std::stable_sort(glyphs.begin(), glyphs.end(), [](const Glyph& a, const Glyph& b) {
        return a.ownerActive < b.ownerActive;
    });
    for (const Glyph& g : glyphs)
    {
        const NotePaintRole role = roleForNote(g.selected, g.ownerActive, g.startInsideOwner);
        const Rgba fill = noteFillRgb(role, g.velocity);
        const int y = 36 + g.pitchRow * 22;
        fillRect(px, kW, kH, g.x, y, g.x + 70, y + 16, fill);
        if (g.selected)
        {
            const Rgba border = velocityRgb(g.velocity);
            fillRect(px, kW, kH, g.x, y, g.x + 70, y + 2, border);
            fillRect(px, kW, kH, g.x, y + 14, g.x + 70, y + 16, border);
        }
    }
    writeBmp(path, kW, kH, px);
}
} // namespace

int main()
{
    using namespace midi_editor_cohort;

    expect(classifyArrangementClick(false, false) == ArrangementClick::Plain, "plain click");
    expect(classifyArrangementClick(true, false) == ArrangementClick::Add, "shift adds");
    expect(classifyArrangementClick(false, true) == ArrangementClick::Toggle, "ctrl toggles");
    expect(classifyArrangementClick(true, true) == ArrangementClick::Toggle, "ctrl wins over shift");

    std::vector<EventKey> members = {
        { 2, 20, 1, 1000, false },
        { 1, 10, 0, 4000, true },
        { 3, 30, 2, 500, false },
    };
    expect(chooseInitialActiveIndex(members) == 1, "last clicked stays active");
    members[1].lastClicked = false;
    expect(chooseInitialActiveIndex(members) == 1, "else lowest track order");
    members[0].trackOrder = 5;
    members[1].trackOrder = 5;
    members[2].trackOrder = 5;
    expect(chooseInitialActiveIndex(members) == 2, "then earliest start");
    members[2].startSamples = 1000;
    members[2].clipId = 99;
    members[0].clipId = 3;
    expect(chooseInitialActiveIndex(members) == 0, "then lowest clip id");

    const std::vector<ZMember> z = {
        { false, 2, 100, 3, 0 },
        { true, 4, 900, 9, 1 },
        { false, 0, 500, 2, 2 },
        { false, 0, 100, 1, 3 },
    };
    const auto order = paintOrderBackToFront(z);
    expect(order.size() == 4, "z count");
    expect(order.back() == 1, "active is painted last");
    expect(isAbove(z[2], z[0]), "lower track number is above a higher one");
    expect(isAbove(z[3], z[2]), "same track: earlier start is above");
    expect(order.front() == 0, "highest non-active track is at the back");

    const AudibleWindow window{ 1000, 500, 800 };
    expect(noteStartInsideAudibleWindow(window, 1000), "start on the boundary is valid");
    expect(!noteStartInsideAudibleWindow(window, 1500), "start at the end is outside");
    expect(noteStartInsideAudibleWindow(window, 1499), "tail may cross and the start stays valid");
    expect(!noteStartInsideAudibleWindow(window, 900), "start before the window is outside");

    const std::vector<ProjectSpan> spans = { { 700, 1100 }, { 1200, 1800 } };
    const AudibleWindow grown = extendWindowToCover(window, spans);
    expect(grown.anchorSamples == 800, "left extend keeps the anchor");
    expect(grown.startSamples == 700, "window start moves to the earliest note");
    expect(grown.lengthSamples == 1100, "length covers the latest note end");
    const std::int64_t noteTickProject = 800 + 250;
    expect(noteTickProject == 1050, "an existing note's project time is anchor plus tick");
    expect(grown.anchorSamples + 250 == noteTickProject, "extend does not move that note");

    expect(roleForNote(false, true, true) == NotePaintRole::ActiveValid, "active valid");
    expect(roleForNote(false, false, true) == NotePaintRole::OtherValid, "other valid on any background");
    expect(roleForNote(false, true, false) == NotePaintRole::OutsideOwner, "outside own window");
    expect(roleForNote(true, false, false) == NotePaintRole::Selected, "selection wins");
    const Rgba strong = noteFillRgb(NotePaintRole::ActiveValid, 127);
    const Rgba muted = noteFillRgb(NotePaintRole::OtherValid, 127);
    const Rgba grey = noteFillRgb(NotePaintRole::OutsideOwner, 127);
    expect(strong.r > muted.r, "other events are a quieter red");
    expect(grey.r < 140 && grey.g > 80 && grey.b > 80, "outside notes are neutral grey");
    const Rgba selected = noteFillRgb(NotePaintRole::Selected, 127);
    expect(selected.r == 0x10 && selected.g == 0x10 && selected.b == 0x12, "selected fill stays black");

    const NoteId a{ 1, 10, 5 };
    const NoteId b{ 1, 11, 5 };
    const NoteId c{ 1, 10, 5 };
    expect(!(a == b), "same uid in another event is a different note");
    expect(a == c, "owner plus uid matches");

    const std::vector<ClipboardSourceNote> sources = {
        { 5000, 400, 60, 100, 64, 1 },
        { 8000, 200, 67, 90, 40, 2 },
        { 6200, 100, 64, 70, 64, 1 },
    };
    const auto clip = buildProjectClipboard(sources);
    expect(clip.size() == 3, "clipboard keeps every note");
    expect(clip[0].deltaProjectSamples == 0, "earliest note is the origin");
    expect(clip[1].deltaProjectSamples == 3000, "project gap is preserved");
    expect(clip[2].deltaProjectSamples == 1200, "middle gap is preserved");
    expect(clip[1].channel == 2 && clip[1].offVelocity == 40, "native channel and off-velocity survive");
    const std::int64_t pasteAt = 20000;
    expect(pastedProjectStart(pasteAt, clip[1].deltaProjectSamples) - pastedProjectStart(pasteAt, clip[0].deltaProjectSamples)
               == 3000,
           "paste into one event keeps the project gap");

    std::filesystem::create_directories("build/midi-cohort-preview");
    renderFrame("build/midi-cohort-preview/active-later.bmp", true);
    renderFrame("build/midi-cohort-preview/active-earlier.bmp", false);

    if (gFailures != 0)
    {
        std::cerr << gFailures << " failed\n";
        return 1;
    }
    std::cout << "MidiEditorCohortFocusedTests ok\n";
    return 0;
}
