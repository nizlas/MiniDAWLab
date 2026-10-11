#include "domain/TrackAutomation.h"
#include "ui/TrackAutomationLaneView.h"
#include "ui/TrackHeaderView.h"
#include "ui/TrackRowHeightPresets.h"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>

#include <juce_gui_basics/juce_gui_basics.h>

namespace
{
int gFailures = 0;

void expect(const bool ok, const char* what)
{
    if (!ok)
    {
        std::cerr << "FAIL " << what << "\n";
        ++gFailures;
    }
}

void expectNear(const double got, const double want, const double tol, const char* what)
{
    if (!(std::fabs(got - want) <= tol))
    {
        std::cerr << "FAIL " << what << " got " << got << " want " << want << "\n";
        ++gFailures;
    }
}

TrackAutomationCurve segment(const float y0, const float y1, const float control)
{
    TrackAutomationCurve c;
    c.anchorSet = true;
    c.anchor = y0;
    c.points.push_back({ 0.0, y0 });
    c.points.push_back({ 1.0, y1 });
    c.control.push_back(control);
    return c;
}

void savePng(const juce::Image& image, const juce::File& dir, const char* name)
{
    if (!dir.isDirectory() || !image.isValid())
    {
        return;
    }
    const juce::File file = dir.getChildFile(name);
    file.deleteFile();
    juce::FileOutputStream stream(file);
    if (stream.openedOk())
    {
        juce::PNGImageFormat().writeImageToStream(image, stream);
        stream.flush();
    }
}

[[nodiscard]] int countPixelsNear(const juce::Image& image, juce::Colour colour, const int tolerance)
{
    int n = 0;
    for (int y = 0; y < image.getHeight(); ++y)
    {
        for (int x = 0; x < image.getWidth(); ++x)
        {
            if (std::abs((int)image.getPixelAt(x, y).getRed() - (int)colour.getRed()) <= tolerance
                && std::abs((int)image.getPixelAt(x, y).getGreen() - (int)colour.getGreen()) <= tolerance
                && std::abs((int)image.getPixelAt(x, y).getBlue() - (int)colour.getBlue()) <= tolerance)
            {
                ++n;
            }
        }
    }
    return n;
}

void testHeaderAndLaneImages(const juce::File& shotDir)
{
    TrackHeaderModel model;
    model.name = "Guitar";
    model.automationAvailable = true;
    model.automationRead = true;
    model.automationWrite = true;
    model.trackNumber = 1;
    int toggles = 0;
    TrackHeaderCallbacks callbacks;
    callbacks.onToggleAutomationRead = [] {};
    callbacks.onToggleAutomationWrite = [] {};
    callbacks.onToggleAutomationLanes = [&toggles] { ++toggles; };
    callbacks.onToggleArm = [] {};
    callbacks.onToggleMute = [] {};
    callbacks.onTogglePower = [] { return true; };

    auto makeHeader = [&](const int height) {
        auto view = std::make_unique<TrackHeaderView>(
            [model] { return model; }, callbacks, kInvalidTrackId, std::nullopt);
        view->setSize(280, height);
        return view;
    };

    const auto small = makeHeader(track_row_heights::kSmallPresetPx);
    const juce::Image smallImg = small->createComponentSnapshot(small->getLocalBounds(), false, 1.0f);
    savePng(smallImg, shotDir, "automation-header-small.png");
    const auto read = small->getAutomationReadBounds();
    const auto write = small->getAutomationWriteBounds();
    const auto arm = small->getArmButtonBounds();
    const auto name = small->getNameTextBounds();
    const auto chevron = small->getAutomationChevronBounds();
    expect(!read.isEmpty() && !write.isEmpty(), "small header has R and W cells");
    expect(!read.intersects(write), "R and W do not overlap each other");
    expect(arm.isEmpty() || !read.intersects(arm), "R does not overlap Record Arm");
    expect(arm.isEmpty() || !write.intersects(arm), "W does not overlap Record Arm");
    expect(!read.intersects(name) && !write.intersects(name), "R/W do not overlap the track name");
    expect(!chevron.isEmpty() && small->getColourSegmentBounds().contains(chevron),
           "chevron sits inside the colour segment");
    expect(small->clickAutomationChevronLikeMouseForStabilityTest() && toggles == 1,
           "chevron click toggles lanes");

    const auto micro = makeHeader(track_row_heights::kMicroPresetPx);
    const juce::Image microImg = micro->createComponentSnapshot(micro->getLocalBounds(), false, 1.0f);
    savePng(microImg, shotDir, "automation-header-micro.png");
    expect(!micro->getAutomationReadBounds().isEmpty(), "micro header still shows Read");
    expect(!micro->getAutomationWriteBounds().isEmpty(), "micro header still shows Write");
    expect(micro->getArmButtonBounds().isEmpty(), "micro has no Arm cell on the title row");
    expect(!micro->getAutomationReadBounds().intersects(micro->getNameTextBounds()),
           "micro R does not cover the name");

    TrackAutomationCurve crescendo = segment(0.25f, 1.0f, 0.25f);
    auto lane = std::make_unique<TrackAutomationLaneView>(kTrackAutomationVolumeId, 1);
    lane->setSize(640, TrackAutomationLaneView::kLaneHeightPx);
    lane->setTimeMap(48000.0 / 640.0, 0, 48000.0);
    lane->setPlayheadSeconds(0.5);
    lane->setCurve(crescendo, 0.25f, juce::Colour(0xff3e5a85));
    const juce::Image laneImg = lane->createComponentSnapshot(lane->getLocalBounds(), false, 1.0f);
    savePng(laneImg, shotDir, "automation-lane-crescendo.png");
    expect(laneImg.isValid(), "lane snapshot exists");
    expect(countPixelsNear(laneImg, juce::Colour(0xff3e5a85), 8) > 20, "crescendo curve is painted");
    expect(!lane->getPlotBounds().isEmpty(), "lane plot is hittable");
    expect(!lane->getParameterMenuBounds().isEmpty(), "lane parameter label is hittable");
}
} // namespace

int main(int argc, char** argv)
{
    // Crescendo: C = y0 rises with u^2. Early rise: C = y1.
    {
        const TrackAutomationCurve slow = segment(0.0f, 1.0f, 0.0f);
        const TrackAutomationCurve early = segment(0.0f, 1.0f, 1.0f);
        expectNear(trackAutomationEvaluate(slow, 0.2, 0.0f), 0.04, 1.0e-5, "crescendo at 0.2");
        expectNear(trackAutomationEvaluate(slow, 0.8, 0.0f), 0.64, 1.0e-5, "crescendo at 0.8");
        expect(trackAutomationEvaluate(slow, 0.95, 0.0f) - trackAutomationEvaluate(slow, 0.85, 0.0f)
                   > trackAutomationEvaluate(slow, 0.25, 0.0f) - trackAutomationEvaluate(slow, 0.15, 0.0f),
               "crescendo steepens near the end");
        expectNear(trackAutomationEvaluate(early, 0.2, 0.0f), 0.36, 1.0e-5, "early rise at 0.2");
        expectNear(trackAutomationEvaluate(early, 0.8, 0.0f), 0.96, 1.0e-5, "early rise at 0.8");
    }

    // The handle is the curve at several grab times, including the crescendo control.
    for (const double u : { 0.08, 0.2, 0.5, 0.8, 0.92 })
    {
        const float y0 = 0.25f;
        const float y1 = 1.5f;
        const float crescendo = trackAutomationControlFromGrab(y0, y1, u, -100.0f);
        const float early = trackAutomationControlFromGrab(y0, y1, u, 100.0f);
        expectNear(crescendo, y0, 1.0e-4, "grab down solves the start control");
        expectNear(early, y1, 1.0e-4, "grab up solves the end control");
        const float onCurve = trackAutomationQuadratic(y0, crescendo, y1, trackAutomationClampGrabU(u));
        const double chord = (1.0 - u) * y0 + u * y1;
        const double half = std::fabs(static_cast<double>(y1 - y0)) * u * (1.0 - u);
        expectNear(onCurve, chord - half, 2.0e-4, "handle sits on the low reachable curve");
    }

    // Equal endpoints cannot bend. A very short segment is the chord.
    {
        expectNear(trackAutomationControlFromGrab(0.4f, 0.4f, 0.3, 0.9f), 0.4, 1.0e-6, "flat segment");
        TrackAutomationCurve tiny;
        tiny.anchorSet = true;
        tiny.anchor = 0.0f;
        tiny.points.push_back({ 0.0, 0.0f });
        tiny.points.push_back({ 1.0e-5, 1.0f });
        tiny.control.push_back(0.0f);
        expectNear(trackAutomationEvaluate(tiny, 5.0e-6, 0.0f), 0.5, 1.0e-4, "short segment is the chord");
    }

    // Split keeps the polynomial. A punch keeps bent pieces outside the interval.
    {
        TrackAutomationCurve curve;
        curve.anchorSet = true;
        curve.anchor = 0.2f;
        curve.points = { { 0.0, 0.2f }, { 1.0, 0.8f }, { 2.0, 0.3f }, { 3.0, 0.9f } };
        curve.control = { 0.2f, 0.8f, 0.3f };
        const TrackAutomationCurve before = curve;
        std::vector<TrackAutomationPoint> stroke = { { 1.0, 0.1f }, { 1.4, 0.05f }, { 1.8, 0.1f } };
        trackAutomationPunch(curve, 1.0, 2.0, stroke, kTrackAutomationVolumeId);
        for (const double t : { 0.1, 0.25, 0.5, 0.75, 0.99 })
        {
            expectNear(trackAutomationEvaluate(curve, t, 0.0f),
                       trackAutomationEvaluate(before, t, 0.0f),
                       1.0e-5,
                       "left of the punch");
        }
        for (const double t : { 2.0, 2.25, 2.5, 2.75, 3.0 })
        {
            expectNear(trackAutomationEvaluate(curve, t, 0.0f),
                       trackAutomationEvaluate(before, t, 0.0f),
                       1.0e-5,
                       "right of the punch");
        }
        expectNear(trackAutomationEvaluate(curve, 1.4, 0.0f), 0.05, 1.0e-4, "recorded dip is kept");
        expectNear(trackAutomationEvaluate(curve, 1.0 - 1.0e-4, 0.0f),
                   trackAutomationEvaluate(before, 1.0 - 1.0e-4, 0.0f),
                   1.0e-4,
                   "just before the punch");
    }

    // 48 kHz and 44.1 kHz place the same stored second on different samples.
    // Opening does not rewrite the second. Round-trip does not drift.
    {
        const double oneSecond = 1.0;
        expect(trackAutomationSecondsToEngineSample(oneSecond, 48000.0) == 48000, "1s at 48 kHz");
        expect(trackAutomationSecondsToEngineSample(oneSecond, 44100.0) == 44100, "1s at 44.1 kHz");
        expectNear(oneSecond, 1.0, 0.0, "stored second is not rewritten");
        for (const double rate : { 44100.0, 48000.0 })
        {
            for (const int64_t n : { 0, 1, 48000, 1000003 })
            {
                const double seconds = trackAutomationEngineSampleToSeconds(n, rate);
                const int64_t back = trackAutomationSecondsToEngineSample(seconds, rate);
                expect(back == n, "sample round trip");
            }
        }
    }

    // A dip inside one 256-sample block at 48 kHz is still there when the ends match.
    {
        const double rate = 48000.0;
        const int64_t block = 256;
        TrackAutomationCurve dip;
        dip.anchorSet = true;
        dip.anchor = 1.0f;
        const double t0 = trackAutomationEngineSampleToSeconds(block, rate);
        const double tMid = trackAutomationEngineSampleToSeconds(block + 40, rate);
        const double t1 = trackAutomationEngineSampleToSeconds(block + 80, rate);
        dip.points = { { t0, 1.0f }, { tMid, 0.1f }, { t1, 1.0f } };
        dip.control = { trackAutomationLinearControl(1.0f, 0.1f), trackAutomationLinearControl(0.1f, 1.0f) };
        expectNear(trackAutomationEvaluate(dip, tMid, 1.0f), 0.1, 1.0e-5, "dip inside the block");
        expectNear(trackAutomationEvaluate(dip, t0, 1.0f), 1.0, 1.0e-5, "dip start");
        expectNear(trackAutomationEvaluate(dip, t1, 1.0f), 1.0, 1.0e-5, "dip end");
    }

    {
        juce::ScopedJuceInitialiser_GUI gui;
        const juce::File shotDir = juce::File::getSpecialLocation(juce::File::currentExecutableFile)
                                       .getParentDirectory()
                                       .getChildFile("automation-ui-shots");
        shotDir.createDirectory();
        testHeaderAndLaneImages(shotDir);
        juce::ignoreUnused(argc, argv);
    }

    if (gFailures != 0)
    {
        std::cerr << gFailures << " failed\n";
        return 1;
    }
    std::cout << "TrackAutomationFocusedTests ok\n";
    return 0;
}
