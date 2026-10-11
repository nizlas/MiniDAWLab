#include "app/TrackAutomationCommitBatch.h"
#include "domain/AudioClip.h"
#include "domain/PlacedClip.h"
#include "domain/SessionHistory.h"
#include "domain/SessionSnapshot.h"
#include "domain/Track.h"
#include "engine/PlaybackMixHelpers.h"
#include "engine/TrackAutomationRuntime.h"
#include "io/ProjectFile.h"
#include "ui/TrackAutomationLaneView.h"
#include "ui/TrackHeaderView.h"
#include "ui/TrackRowHeightPresets.h"

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <atomic>
#include <cmath>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>

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

TrackAutomationCurve kneeCurve(const float y0, const float y1, const double kneeU, const float kneeY)
{
    TrackAutomationCurve curve;
    curve.anchorSet = true;
    curve.anchor = y0;
    curve.points.push_back({ 0.0, y0 });
    curve.points.push_back({ 1.0, y1 });
    curve.shape.resize(1);
    trackAutomationShapeFromKnee(y0, y1, kneeU, kneeY, curve.shape[0]);
    return curve;
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

void testIndependentShapes()
{
    const TrackAutomationCurve late = kneeCurve(0.0f, 1.0f, 0.8, 0.2f);
    const TrackAutomationCurve early = kneeCurve(0.0f, 1.0f, 0.2, 0.8f);
    expectNear(late.points.front().value, 0.0, 1.0e-6, "late start endpoint");
    expectNear(late.points.back().value, 1.0, 1.0e-6, "late end endpoint");
    expectNear(trackAutomationEvaluate(late, 0.8, 0.0f), 0.2, 0.02, "late curve is about 0.2 at t=0.8");
    expectNear(trackAutomationEvaluate(early, 0.2, 0.0f), 0.8, 0.02, "early curve is about 0.8 at t=0.2");
    expect(trackAutomationEvaluate(late, 0.2, 0.0f) < 0.15f, "late rise is still low at t=0.2");
    expect(trackAutomationEvaluate(early, 0.8, 0.0f) > 0.85f, "early rise is already high at t=0.8");
    float previous = -1.0f;
    for (int i = 0; i <= 20; ++i)
    {
        const double t = static_cast<double>(i) / 20.0;
        const float y = trackAutomationEvaluate(late, t, 0.0f);
        expect(y >= -1.0e-4f && y <= 1.0f + 1.0e-4f, "late curve stays inside the ends");
        expect(y + 1.0e-4f >= previous, "late curve is monotone");
        previous = y;
    }
    expectNear(trackAutomationEvaluate(kneeCurve(0.4f, 0.4f, 0.3, 0.9f), 0.5, 0.0f), 0.4, 1.0e-5, "equal ends stay flat");
    TrackAutomationCurve tiny;
    tiny.anchorSet = true;
    tiny.anchor = 0.0f;
    tiny.points = { { 0.0, 0.0f }, { 1.0e-5, 1.0f } };
    tiny.shape = { kneeCurve(0.0f, 1.0f, 0.8, 0.2f).shape[0] };
    expectNear(trackAutomationEvaluate(tiny, 5.0e-6, 0.0f), 0.5, 1.0e-3, "very short segment is the chord");
}

void testPunchKeepsOutside()
{
    TrackAutomationCurve curve = kneeCurve(0.2f, 0.9f, 0.75, 0.3f);
    curve.points = { { 0.0, 0.2f }, { 1.0, 0.9f }, { 2.0, 0.3f }, { 3.0, 0.95f } };
    curve.shape.resize(3);
    trackAutomationShapeFromKnee(0.2f, 0.9f, 0.8, 0.35f, curve.shape[0]);
    trackAutomationShapeFromKnee(0.9f, 0.3f, 0.25, 0.8f, curve.shape[1]);
    trackAutomationShapeFromKnee(0.3f, 0.95f, 0.7, 0.4f, curve.shape[2]);
    const TrackAutomationCurve before = curve;
    std::vector<TrackAutomationPoint> stroke = { { 1.05, 0.2f }, { 1.4, 0.05f }, { 1.8, 0.15f } };
    trackAutomationPunch(curve, 1.0, 2.0, stroke, kTrackAutomationVolumeId);
    for (const double t : { 0.1, 0.25, 0.5, 0.8, 0.99 })
    {
        expectNear(trackAutomationEvaluate(curve, t, 0.0f), trackAutomationEvaluate(before, t, 0.0f), 2.0e-3, "outside left of punch");
    }
    for (const double t : { 2.0, 2.2, 2.5, 2.8, 3.0 })
    {
        expectNear(trackAutomationEvaluate(curve, t, 0.0f), trackAutomationEvaluate(before, t, 0.0f), 2.0e-3, "outside right of punch");
    }
    expectNear(trackAutomationEvaluate(curve, 1.4, 0.0f), 0.05, 1.0e-3, "recorded dip is kept");
}

void testLegacyQuadratic()
{
    TrackAutomationCurve curve;
    curve.anchorSet = true;
    curve.anchor = 0.0f;
    curve.points = { { 0.0, 0.0f }, { 1.0, 1.0f } };
    curve.shape = { trackAutomationShapeFromLegacyControl(0.0f, 1.0f, 0.0f) };
    expectNear(trackAutomationEvaluate(curve, 0.8, 0.0f), 0.64, 2.0e-3, "v29 slow quadratic is preserved");
}

void testHeaderAndLaneDrag(const juce::File& shotDir)
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
        auto view = std::make_unique<TrackHeaderView>([model] { return model; }, callbacks, kInvalidTrackId, std::nullopt);
        view->setSize(280, height);
        return view;
    };
    const auto small = makeHeader(track_row_heights::kSmallPresetPx);
    savePng(small->createComponentSnapshot(small->getLocalBounds(), false, 1.0f), shotDir, "automation-header-small.png");
    expect(!small->getAutomationReadBounds().isEmpty() && !small->getAutomationWriteBounds().isEmpty(), "small header has R and W");
    expect(small->clickAutomationChevronLikeMouseForStabilityTest() && toggles == 1, "chevron click toggles lanes");

    auto lane = std::make_unique<TrackAutomationLaneView>(kTrackAutomationPanId, TrackId{ 1 });
    lane->setSize(640, TrackAutomationLaneView::kLaneHeightPx);
    lane->setTimeMap(1.0, 0, 100.0);
    TrackAutomationCurve curve;
    curve.anchorSet = true;
    curve.anchor = -1.0f;
    curve.points = { { 0.0, -1.0f }, { 1.0, 1.0f } };
    curve.shape = { trackAutomationLinearShape(-1.0f, 1.0f) };
    lane->setCurve(curve, 0.0f, juce::Colour(0xff3e5a85));
    lane->dragCurveLikeMouseForTest({ lane->secondsToX(0.5), lane->valueToY(0.0f) },
                                    { lane->secondsToX(0.8), lane->valueToY(-0.6f) });
    expect(lane->getPlotBounds().getWidth() > 0, "lane plot is laid out");
}

void testLaneDragResult()
{
    auto lane = std::make_unique<TrackAutomationLaneView>(kTrackAutomationPanId, TrackId{ 1 });
    lane->setSize(800, TrackAutomationLaneView::kLaneHeightPx);
    lane->setTimeMap(1.0, 0, 200.0);
    TrackAutomationCurve curve;
    curve.anchorSet = true;
    curve.anchor = -1.0f;
    curve.points = { { 0.0, -1.0f }, { 1.0, 1.0f } };
    curve.shape = { trackAutomationLinearShape(-1.0f, 1.0f) };
    TrackAutomationCurve committed;
    lane->onCurveCommitted = [&committed](const TrackAutomationCurve& next) { committed = next; };
    lane->setCurve(curve, 0.0f, juce::Colour(0xff3e5a85));
    lane->dragCurveLikeMouseForTest({ lane->secondsToX(0.5), lane->valueToY(0.0f) },
                                    { lane->secondsToX(0.8), lane->valueToY(-0.6f) });
    expect(!committed.points.empty(), "lane drag committed a curve");
    if (committed.points.size() >= 2)
    {
        expectNear(committed.points.front().value, -1.0, 1.0e-3, "drag left the start square");
        expectNear(committed.points.back().value, 1.0, 1.0e-3, "drag left the end square");
        expectNear(trackAutomationEvaluate(committed, 0.8, 0.0f), -0.6, 0.08, "drag moved the value at t=0.8");
        expect(std::fabs(committed.shape[0].kneeU - 0.5f) > 0.05f, "drag also moved the knee in time");
    }
}

[[nodiscard]] std::vector<float> renderLive(const Track& track, const ChannelAutomationClock& clock, const int n, const float input)
{
    std::vector<float> in(static_cast<size_t>(n), input);
    std::vector<float> left(static_cast<size_t>(n), 0.0f);
    std::vector<float> right(static_cast<size_t>(n), 0.0f);
    playback_mix_helpers::renderLiveInputTrackPostStripToStereoScratch(
        track, 0, in.data(), nullptr, n, left.data(), right.data(), nullptr, nullptr, nullptr, &clock);
    return left;
}

void testGestureMatchesReplay()
{
    const TrackId id{ 7 };
    const auto runtime = std::make_unique<TrackAutomationRuntime>();
    TrackAutomationCurve frozen;
    frozen.anchorSet = true;
    frozen.anchor = 1.0f;
    runtime->begin(id, kTrackAutomationVolumeId, true, true, 1.0f, 10.0, 1, frozen, 1.0f);
    const double rate = 48000.0;
    const int blockA = 256;
    const int blockB = 512;
    std::vector<double> times;
    std::vector<float> heard;
    auto runBlock = [&](const int64_t startSample, const int n, const double host0, const double host1, const bool release) {
        if (release)
        {
            runtime->requestRelease(id, kTrackAutomationVolumeId, host1);
        }
        const double start = trackAutomationEngineSampleToSeconds(startSample, rate);
        runtime->audioThread_beginBlock(start, static_cast<double>(n) / rate, host1, false, 0.0);
        ChannelAutomationClock clock;
        runtime->audioThread_fillClock(clock, rate);
        clock.blockStartSample = startSample;
        std::vector<Track> tracks;
        tracks.emplace_back(id, juce::String("A"), std::vector<PlacedClip>{});
        Track track = tracks.back().withAutomation({});
        const std::vector<float> audio = renderLive(track, clock, n, 1.0f);
        for (int i = 0; i < n; i += 8)
        {
            times.push_back(trackAutomationEngineSampleToSeconds(startSample + i, rate));
            heard.push_back(audio[static_cast<size_t>(i)]);
        }
        juce::ignoreUnused(host0);
    };
    runtime->push(id, kTrackAutomationVolumeId, 1.0f, 10.05);
    runtime->push(id, kTrackAutomationVolumeId, 0.25f, 10.20);
    runtime->push(id, kTrackAutomationVolumeId, 0.25f, 10.40);
    runBlock(0, blockA, 10.0, 10.50, false);
    runBlock(blockA, blockB, 10.50, 11.50, false);
    runtime->push(id, kTrackAutomationVolumeId, 0.25f, 11.55);
    runBlock(blockA + blockB, blockA, 11.50, 12.00, true);
    TrackId gotId = kInvalidTrackId;
    int gotParam = 0;
    TrackAutomationCommit commit;
    expect(runtime->takeCommit(gotId, gotParam, commit), "gesture produced a commit");
    expect(commit.eventCount >= 2, "commit has the movement and the release");
    bool releaseKept = false;
    for (int i = 0; i < commit.eventCount; ++i)
    {
        if (std::fabs(commit.events[static_cast<size_t>(i)].timeSeconds - commit.releaseSeconds) <= 1.0e-4
            && std::fabs(commit.events[static_cast<size_t>(i)].value - commit.releaseValue) <= 1.0e-4)
        {
            releaseKept = true;
        }
    }
    expect(releaseKept, "release time keeps the held value");
    TrackAutomationCurve saved;
    saved.anchorSet = true;
    saved.anchor = commit.preTouchValue;
    std::vector<TrackAutomationPoint> recorded;
    for (int i = 0; i < commit.eventCount; ++i)
    {
        recorded.push_back(commit.events[static_cast<size_t>(i)]);
    }
    recorded.push_back({ commit.rampEndSeconds, commit.rampEndValue });
    trackAutomationPunch(saved, commit.touchStartSeconds, commit.rampEndSeconds, recorded, kTrackAutomationVolumeId);
    Track track = Track(id, juce::String("A"), std::vector<PlacedClip>{});
    TrackAutomationState state;
    state.read = true;
    state.volume = saved;
    track = track.withAutomation(state);
    ChannelAutomationClock playback;
    playback.engineRate = rate;
    int mismatches = 0;
    for (size_t i = 0; i < times.size(); ++i)
    {
        const int64_t sample = trackAutomationSecondsToEngineSample(times[i], rate);
        playback.blockStartSample = sample;
        const std::vector<float> again = renderLive(track, playback, 1, 1.0f);
        if (std::fabs(again[0] - heard[i]) > 0.02f)
        {
            ++mismatches;
        }
    }
    expect(mismatches == 0, "replayed strip matches the write pass within 0.02");
    juce::ignoreUnused(blockB);
}

void testHostSpacing()
{
    const auto runtime = std::make_unique<TrackAutomationRuntime>();
    TrackAutomationCurve frozen;
    runtime->begin(TrackId{ 1 }, kTrackAutomationVolumeId, true, false, 0.2f, 0.0, 3, frozen, 1.0f);
    runtime->audioThread_beginBlock(0.0, 0.10, 5.0, false, 0.0);
    runtime->push(TrackId{ 1 }, kTrackAutomationVolumeId, 0.5f, 5.25);
    runtime->push(TrackId{ 1 }, kTrackAutomationVolumeId, 0.8f, 5.75);
    runtime->audioThread_beginBlock(1.0, 0.10, 6.0, false, 0.0);
    TrackId id = kInvalidTrackId;
    int param = 0;
    runtime->requestRelease(TrackId{ 1 }, kTrackAutomationVolumeId, 6.0);
    runtime->audioThread_beginBlock(1.1, 0.10, 7.0, false, 0.0);
    TrackAutomationCommit commit;
    expect(runtime->takeCommit(id, param, commit), "spaced gesture committed");
    expect(commit.eventCount >= 3, "three stamps survived");
    if (commit.eventCount >= 3)
    {
        const double d01 = commit.events[1].timeSeconds - commit.events[0].timeSeconds;
        const double d12 = commit.events[2].timeSeconds - commit.events[1].timeSeconds;
        expect(d01 > 0.01 && d12 > 0.01, "events keep host spacing instead of one-sample steps");
    }
}

void testRatesAndPaths()
{
    for (const double timeline : { 44100.0, 48000.0 })
    {
        const double other = timeline == 48000.0 ? 44100.0 : 48000.0;
        expect(trackAutomationTimelineRate(timeline, other) == timeline, "project rate wins over the device rate");
        const int64_t at = trackAutomationSecondsToEngineSample(1.0, timeline);
        expect(at == static_cast<int64_t>(timeline), "1s is the timeline sample, not the other rate");
        auto lane = std::make_unique<TrackAutomationLaneView>(kTrackAutomationVolumeId, TrackId{ 3 });
        lane->setSize(400, TrackAutomationLaneView::kLaneHeightPx);
        lane->setTimeMap(1.0, at - 20, timeline);
        const float x = lane->secondsToX(1.0);
        const double back = lane->xToSeconds(x);
        expectNear(back, 1.0, 1.5 / timeline, "lane pixel maps back to the same second");

        TrackAutomationCurve dip;
        dip.anchorSet = true;
        dip.anchor = 1.0f;
        const double t0 = trackAutomationEngineSampleToSeconds(at - 30, timeline);
        const double tMid = trackAutomationEngineSampleToSeconds(at, timeline);
        const double t1 = trackAutomationEngineSampleToSeconds(at + 30, timeline);
        dip.points = { { t0, 1.0f }, { tMid, 0.2f }, { t1, 1.0f } };
        dip.shape = { trackAutomationLinearShape(1.0f, 0.2f), trackAutomationLinearShape(0.2f, 1.0f) };
        TrackAutomationState state;
        state.read = true;
        state.volume = dip;
        juce::AudioBuffer<float> pcm(1, 128);
        for (int i = 0; i < 128; ++i)
        {
            pcm.setSample(0, i, 1.0f);
        }
        const auto material = std::make_shared<const AudioClip>(std::move(pcm), timeline, "tone");
        std::vector<PlacedClip> clips;
        clips.emplace_back(PlacedClipId{ 1 }, material, at - 40);
        std::vector<Track> tracks;
        tracks.emplace_back(TrackId{ 3 }, juce::String("T"), std::move(clips));
        tracks[0] = tracks[0].withAutomation(state);
        const auto snap = SessionSnapshot::withTracks(std::move(tracks), 0, 0, 0, ProjectMusicalTime{});
        ChannelAutomationClock clock;
        clock.engineRate = timeline;
        clock.blockStartSample = at - 40;
        constexpr int kN = 80;
        std::vector<float> directL(kN, 0.0f), directR(kN, 0.0f);
        playback_mix_helpers::renderAudioTrackPostStripToStereoScratch(
            *snap, at - 40, kN, 0, directL.data(), directR.data(), nullptr, kInvalidTrackId, at + 80, 0, nullptr, nullptr, &clock);
        std::vector<float> chainL(kN, 0.0f), chainR(kN, 0.0f);
        playback_mix_helpers::AudioStripInsertAccess access;
        playback_mix_helpers::renderAudioTrackPostStripToStereoScratchWithChainAccess(
            *snap, at - 40, kN, 0, chainL.data(), chainR.data(), access, kInvalidTrackId, at + 80, 0, nullptr, nullptr, &clock);
        float* outs[2] = { directL.data(), directR.data() };
        juce::ignoreUnused(outs);
        bool dipAtMarker = false;
        bool samePath = true;
        for (int i = 0; i < kN; ++i)
        {
            if (std::fabs(directL[static_cast<size_t>(i)] - chainL[static_cast<size_t>(i)]) > 1.0e-4f)
            {
                samePath = false;
            }
            if (i == 40 && directL[static_cast<size_t>(i)] < directL[0] * 0.5f)
            {
                dipAtMarker = true;
            }
        }
        std::vector<float> offlineL(kN, 0.0f);
        std::vector<float> offlineR(kN, 0.0f);
        float* offlineOuts[2] = { offlineL.data(), offlineR.data() };
        playback_mix_helpers::renderAudioTracksClipSummingForSegment(
            *snap, at - 40, kN, 0, 2, offlineOuts, nullptr, kInvalidTrackId, at + 80, 0, nullptr, nullptr, nullptr, &clock);
        bool offlineMatches = true;
        for (int i = 0; i < kN; ++i)
        {
            if (std::fabs(directL[static_cast<size_t>(i)] - offlineL[static_cast<size_t>(i)]) > 1.0e-4f)
            {
                offlineMatches = false;
            }
        }
        expect(samePath, "direct strip and read-ahead chain-access strip match");
        expect(offlineMatches, "offline clip summing matches the direct strip");
        expect(dipAtMarker, "dip is heard at the timeline sample the lane uses");
        juce::ignoreUnused(other);
    }
}

void testOverflowAndNextGesture()
{
    const auto runtime = std::make_unique<TrackAutomationRuntime>();
    runtime->setCapacityForTest(4);
    TrackAutomationCurve frozen;
    runtime->begin(TrackId{ 1 }, kTrackAutomationVolumeId, true, false, 0.5f, 0.0, 9, frozen, 1.0f);
    for (int i = 0; i < 12; ++i)
    {
        runtime->push(TrackId{ 1 }, kTrackAutomationVolumeId, 0.1f * static_cast<float>(i), 0.01 * i);
    }
    runtime->audioThread_beginBlock(0.0, 0.05, 1.0, false, 0.0);
    TrackId overflowId = kInvalidTrackId;
    int overflowParam = 0;
    expect(runtime->consumeOverflow(overflowId, overflowParam), "overflow is visible");
    runtime->requestRelease(TrackId{ 1 }, kTrackAutomationVolumeId, 1.0);
    runtime->audioThread_beginBlock(0.05, 0.05, 2.0, false, 0.0);
    TrackAutomationCommit commit;
    TrackId id = kInvalidTrackId;
    int param = 0;
    expect(runtime->takeCommit(id, param, commit), "truncated gesture still commits");
    expect(commit.incomplete, "truncated gesture is not marked complete");
    expect(commit.eventCount > 0, "captured points are kept");
    runtime->begin(TrackId{ 1 }, kTrackAutomationVolumeId, true, false, 0.2f, 3.0, 10, frozen, 1.0f);
    runtime->push(TrackId{ 1 }, kTrackAutomationVolumeId, 0.4f, 3.2);
    runtime->audioThread_beginBlock(1.0, 0.05, 4.0, false, 0.0);
    runtime->requestRelease(TrackId{ 1 }, kTrackAutomationVolumeId, 4.0);
    runtime->audioThread_beginBlock(1.05, 0.05, 5.0, false, 0.0);
    TrackAutomationCommit next;
    expect(runtime->takeCommit(id, param, next), "the next gesture works");
    expect(!next.incomplete, "the next gesture is complete");
}

void testLongDrain()
{
    const auto runtime = std::make_unique<TrackAutomationRuntime>();
    runtime->setCapacityForTest(8);
    TrackAutomationCurve frozen;
    runtime->begin(TrackId{ 2 }, kTrackAutomationVolumeId, true, false, 0.0f, 0.0, 4, frozen, 1.0f);
    int drained = 0;
    for (int block = 0; block < 20; ++block)
    {
        runtime->push(TrackId{ 2 }, kTrackAutomationVolumeId, 0.05f * static_cast<float>(block), static_cast<double>(block) + 0.1);
        runtime->audioThread_beginBlock(static_cast<double>(block), 1.0, static_cast<double>(block + 1), false, 0.0);
        TrackAutomationPoint buf[8];
        drained += runtime->drainBefore(TrackId{ 2 }, kTrackAutomationVolumeId, static_cast<double>(block + 1), buf, 8);
    }
    TrackId overflowId = kInvalidTrackId;
    int overflowParam = 0;
    expect(!runtime->consumeOverflow(overflowId, overflowParam), "a drained gesture does not overflow");
    expect(drained > 8, "drain kept more points than the ring holds");
    juce::ignoreUnused(drained);
}

void testGestureIdentity()
{
    std::vector<TrackAutomationUndoGroup> groups;
    const auto a = std::make_unique<TrackAutomationStroke>();
    a->trackId = TrackId{ 1 };
    a->parameterId = kTrackAutomationVolumeId;
    a->commit.gestureId = 1;
    a->commit.restartStroke = true;
    a->commit.writeEnabled = true;
    a->samples = { { 0.0, 1.0f }, { 0.2, 0.5f } };
    auto b = std::make_unique<TrackAutomationStroke>(*a);
    b->commit.restartStroke = false;
    b->samples = { { 1.0, 0.5f }, { 1.2, 0.2f } };
    trackAutomationCollectStroke(groups, std::move(*a));
    trackAutomationCollectStroke(groups, std::move(*b));
    groups[0].userReleased = true;
    expect(groups.size() == 1, "a seek split stays one gesture");
    expect(trackAutomationGroupReady(groups[0], false), "the closed gesture is one undo");
    auto c = std::make_unique<TrackAutomationStroke>();
    c->commit.gestureId = 2;
    c->commit.restartStroke = false;
    c->commit.writeEnabled = true;
    trackAutomationCollectStroke(groups, std::move(*c));
    expect(groups.size() == 2, "a second drag is a second undo even in the same tick");
}

void testUndoRedoAndDuplicateAndSave()
{
    std::vector<Track> tracks;
    tracks.emplace_back(TrackId{ 4 }, juce::String("Dup"), std::vector<PlacedClip>{});
    auto before = SessionSnapshot::withTracks(std::move(tracks), 0, 0, 0, ProjectMusicalTime{});
    TrackAutomationState late;
    late.read = true;
    late.volume = kneeCurve(0.0f, 1.0f, 0.8, 0.2f);
    auto afterLate = SessionSnapshot::withTrackAutomation(*before, TrackId{ 4 }, late);
    TrackAutomationState early = late;
    early.volume = kneeCurve(0.0f, 1.0f, 0.2, 0.8f);
    auto afterEarly = SessionSnapshot::withTrackAutomation(*afterLate, TrackId{ 4 }, early);
    SessionHistory history;
    history.record("gesture-late", before, afterLate);
    history.record("gesture-early", afterLate, afterEarly);
    const auto undoEarly = history.popUndo();
    expect(undoEarly.has_value()
               && undoEarly->timelineSnapshot->getTrack(0).getAutomation().volume.shape[0].kneeU > 0.5f
               && std::fabs(trackAutomationEvaluate(undoEarly->timelineSnapshot->getTrack(0).getAutomation().volume, 0.8, 0.0f) - 0.2) < 0.02,
           "undo restores the previous gesture");
    const auto undoLate = history.popUndo();
    expect(undoLate.has_value() && undoLate->timelineSnapshot->getTrack(0).getAutomation().volume.points.empty(),
           "second undo restores the manual curve");
    const auto redo = history.popRedo();
    expect(redo.has_value() && !redo->timelineSnapshot->getTrack(0).getAutomation().volume.points.empty(),
           "redo brings the first gesture back");

    int clipsUsed = 0;
    auto duplicated = SessionSnapshot::withTrackDuplicated(*afterLate, TrackId{ 4 }, TrackId{ 6 }, "Copy", PlacedClipId{ 10 }, clipsUsed);
    expect(duplicated != nullptr, "duplicate exists");
    if (duplicated != nullptr)
    {
        const int sourceIndex = duplicated->findTrackIndexById(TrackId{ 4 });
        const int copyIndex = duplicated->findTrackIndexById(TrackId{ 6 });
        expect(sourceIndex >= 0 && copyIndex >= 0, "duplicate keeps both rows");
        const float original = trackAutomationEvaluate(duplicated->getTrack(sourceIndex).getAutomation().volume, 0.8, 0.0f);
        TrackAutomationState edited = duplicated->getTrack(copyIndex).getAutomation();
        edited.volume = kneeCurve(0.0f, 1.0f, 0.2, 0.8f);
        auto changed = SessionSnapshot::withTrackAutomation(*duplicated, TrackId{ 6 }, edited);
        expectNear(trackAutomationEvaluate(changed->getTrack(changed->findTrackIndexById(TrackId{ 4 })).getAutomation().volume, 0.8, 0.0f),
                   original, 1.0e-4, "editing the copy leaves the original shape");
        expect(trackAutomationEvaluate(changed->getTrack(changed->findTrackIndexById(TrackId{ 6 })).getAutomation().volume, 0.2, 0.0f) > 0.7f,
               "the copy has its own shape");
    }

    ProjectFileV1 file;
    file.version = ProjectFileV1::kCurrentVersion;
    file.timelineSampleRate = 48000.0;
    file.deviceSampleRateAtSave = 44100.0;
    ProjectFileTrackV1 track;
    track.id = TrackId{ 4 };
    track.name = "Saved";
    track.kind = "audio";
    track.automation = late;
    track.automation.read = true;
    file.tracks.push_back(track);
    const juce::File dir = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("dal-automation-tests");
    dir.createDirectory();
    const juce::File path = dir.getChildFile("shape.dalab");
    expect(writeProjectFile(path, file).wasOk(), "save writes");
    ProjectFileV1 loaded;
    expect(readProjectFile(path, loaded).wasOk(), "load reads");
    expectNear(trackAutomationEvaluate(loaded.tracks[0].automation.volume, 0.8, 0.0f), 0.2, 0.02, "loaded shape keeps the late knee");
    expect(loaded.tracks[0].automation.read, "Read survives the file");
    const juce::String text = path.loadFileAsString();
    expect(!text.contains("\"write\""), "Write is off after open because the file does not store it");
    expect(std::fabs(loaded.timelineSampleRate - 48000.0) < 1.0, "opening at another device rate does not rewrite the timeline rate");
}

[[nodiscard]] bool waitStage(const std::atomic<int>& stage, const int want)
{
    for (int i = 0; i < 100000000; ++i)
    {
        if (stage.load(std::memory_order_acquire) >= want)
        {
            return true;
        }
        std::this_thread::yield();
    }
    return false;
}

struct HandoverSync
{
    std::atomic<int> stage{ 0 };
    TrackAutomationRuntime* runtime = nullptr;
    bool tookDuringClose = false;
    float valueDuringClose = -1.0f;
    bool beganDuringRetire = false;
    int drained = 0;
};

void onBeforeClosePublish(void* context)
{
    auto* sync = static_cast<HandoverSync*>(context);
    sync->stage.store(1, std::memory_order_release);
    (void)waitStage(sync->stage, 2);
}

void onDuringRetire(void* context)
{
    auto* sync = static_cast<HandoverSync*>(context);
    sync->stage.store(1, std::memory_order_release);
    (void)waitStage(sync->stage, 2);
}

void onDuringDrain(void* context)
{
    auto* sync = static_cast<HandoverSync*>(context);
    sync->stage.store(1, std::memory_order_release);
    (void)waitStage(sync->stage, 2);
}

void testHandoverThreads()
{
    auto runtime = std::make_unique<TrackAutomationRuntime>();
    TrackAutomationCurve frozen;
    frozen.anchorSet = true;
    frozen.anchor = 1.0f;
    HandoverSync sync;
    sync.runtime = runtime.get();
    TrackAutomationHandoverProbe probe;
    probe.context = &sync;
    probe.beforeClosePublish = &onBeforeClosePublish;
    runtime->setHandoverProbeForTest(&probe);

    runtime->begin(TrackId{ 1 }, kTrackAutomationVolumeId, true, false, 1.0f, 10.0, 4, frozen, 1.0f);
    runtime->push(TrackId{ 1 }, kTrackAutomationVolumeId, 0.25f, 10.2);
    runtime->requestRelease(TrackId{ 1 }, kTrackAutomationVolumeId, 10.4);
    std::thread message([&] {
        expect(waitStage(sync.stage, 1), "close window opened");
        TrackId id = kInvalidTrackId;
        int param = 0;
        TrackAutomationCommit early;
        sync.tookDuringClose = runtime->takeCommit(id, param, early);
        sync.valueDuringClose = early.releaseValue;
        sync.stage.store(2, std::memory_order_release);
    });
    runtime->audioThread_beginBlock(0.0, 0.10, 10.5, false, 0.0);
    message.join();
    expect(!sync.tookDuringClose, "a close is not visible before its payload is published");
    TrackId id = kInvalidTrackId;
    int param = 0;
    TrackAutomationCommit commit;
    expect(runtime->takeCommit(id, param, commit), "the published close is taken once");
    expectNear(commit.releaseValue, 0.25, 1.0e-4, "the taken close carries the release value");
    expect(commit.gestureId == 4, "the taken close keeps the gesture id");
    TrackAutomationCommit again;
    expect(!runtime->takeCommit(id, param, again), "the same close is not committed twice");
    runtime->setHandoverProbeForTest(nullptr);

    auto reuse = std::make_unique<TrackAutomationRuntime>();
    for (int i = 0; i < 7; ++i)
    {
        reuse->begin(TrackId{ static_cast<TrackId>(i + 1) }, kTrackAutomationVolumeId, true, false, 0.5f, 1.0, 20, frozen, 0.5f);
    }
    reuse->begin(TrackId{ 8 }, kTrackAutomationVolumeId, true, false, 0.5f, 1.0, 21, frozen, 0.5f);
    reuse->requestRelease(TrackId{ 8 }, kTrackAutomationVolumeId, 1.4);
    reuse->audioThread_beginBlock(0.0, 0.10, 1.5, false, 0.0);
    TrackAutomationCommit dumped;
    expect(reuse->takeCommit(id, param, dumped), "the slot being retired has already handed off its close");
    HandoverSync retireSync;
    retireSync.runtime = reuse.get();
    TrackAutomationHandoverProbe retireProbe;
    retireProbe.context = &retireSync;
    retireProbe.duringRetire = &onDuringRetire;
    reuse->setHandoverProbeForTest(&retireProbe);
    std::thread nextGesture([&] {
        expect(waitStage(retireSync.stage, 1), "retire window opened");
        reuse->begin(TrackId{ 9 }, kTrackAutomationVolumeId, true, false, 0.7f, 3.0, 22, frozen, 0.7f);
        retireSync.beganDuringRetire = reuse->isCapturing(TrackId{ 9 }, kTrackAutomationVolumeId);
        retireSync.stage.store(2, std::memory_order_release);
    });
    reuse->messageThread_snapshotReplaced();
    nextGesture.join();
    expect(!retireSync.beganDuringRetire, "the next gesture cannot claim a slot that is still being retired");
    reuse->setHandoverProbeForTest(nullptr);
    reuse->begin(TrackId{ 9 }, kTrackAutomationVolumeId, true, false, 0.7f, 3.0, 22, frozen, 0.7f);
    expect(reuse->isCapturing(TrackId{ 9 }, kTrackAutomationVolumeId), "the slot accepts the next gesture after it is free");
    ChannelAutomationClock clock;
    reuse->audioThread_fillClock(clock, 48000.0);
    const ChannelAutomationGestureLane* lane = clock.find(TrackId{ 9 }, kTrackAutomationVolumeId);
    expect(lane != nullptr && std::fabs(lane->holdValue - 0.7f) < 1.0e-4f, "retirement does not wipe the next gesture");

    auto drainRuntime = std::make_unique<TrackAutomationRuntime>();
    drainRuntime->setCapacityForTest(4);
    HandoverSync drainSync;
    drainSync.runtime = drainRuntime.get();
    TrackAutomationHandoverProbe drainProbe;
    drainProbe.context = &drainSync;
    drainProbe.duringDrain = &onDuringDrain;
    drainRuntime->setHandoverProbeForTest(&drainProbe);
    drainRuntime->begin(TrackId{ 3 }, kTrackAutomationVolumeId, true, false, 0.1f, 0.0, 30, frozen, 0.1f);
    for (int i = 0; i < 4; ++i)
    {
        drainRuntime->push(TrackId{ 3 }, kTrackAutomationVolumeId, 0.2f * static_cast<float>(i + 1), 0.2 * (i + 1));
    }
    drainRuntime->audioThread_beginBlock(0.0, 0.10, 1.0, false, 0.0);
    TrackAutomationPoint copied[4] = {};
    std::thread producer([&] {
        expect(waitStage(drainSync.stage, 1), "drain window opened");
        drainRuntime->push(TrackId{ 3 }, kTrackAutomationVolumeId, 9.0f, 1.2);
        drainRuntime->audioThread_beginBlock(1.0, 0.10, 2.0, false, 0.0);
        drainSync.stage.store(2, std::memory_order_release);
    });
    const int n = drainRuntime->drainBefore(TrackId{ 3 }, kTrackAutomationVolumeId, 10.0, copied, 4);
    producer.join();
    expect(n == 3, "drain keeps a left endpoint and does not drop the copied points");
    expectNear(copied[0].value, 0.1, 1.0e-4, "the first drained point survived the concurrent produce");
    expectNear(copied[2].value, 0.4, 1.0e-4, "the last copied point was not overwritten");
    drainRuntime->setHandoverProbeForTest(nullptr);
    drainRuntime->push(TrackId{ 3 }, kTrackAutomationVolumeId, 8.0f, 2.2);
    drainRuntime->requestRelease(TrackId{ 3 }, kTrackAutomationVolumeId, 2.4);
    drainRuntime->audioThread_beginBlock(2.0, 0.10, 3.0, false, 0.0);
    TrackAutomationCommit drainedCommit;
    expect(drainRuntime->takeCommit(id, param, drainedCommit), "the drained gesture still commits");
    bool sawNine = false;
    bool sawEight = false;
    bool sawKept = false;
    bool sawDrainedAgain = false;
    for (int i = 0; i < drainedCommit.eventCount; ++i)
    {
        const float v = drainedCommit.events[static_cast<size_t>(i)].value;
        sawNine = sawNine || std::fabs(v - 9.0f) < 1.0e-3f;
        sawEight = sawEight || std::fabs(v - 8.0f) < 1.0e-3f;
        sawKept = sawKept || std::fabs(v - 0.6f) < 1.0e-3f;
        sawDrainedAgain = sawDrainedAgain || std::fabs(v - 0.1f) < 1.0e-3f;
    }
    expect(sawKept, "the endpoint still in the ring survived the full-buffer produce");
    expect(!sawNine, "a produce into a full ring does not overwrite an undrained point");
    expect(!sawDrainedAgain, "points already drained are not committed again");
    expect(sawEight, "a point produced after the drain was published is kept");
    juce::ignoreUnused(drainSync);
}

void testObserveAndReadAheadHold()
{
    const auto runtime = std::make_unique<TrackAutomationRuntime>();
    TrackAutomationCurve frozen;
    frozen.anchorSet = true;
    frozen.anchor = 1.0f;
    runtime->begin(TrackId{ 8 }, kTrackAutomationVolumeId, true, true, 0.4f, 0.0, 11, frozen, 1.0f);
    expect(runtime->adoptionHeld(TrackId{ 8 }), "a gesture holds read-ahead");
    runtime->audioThread_beginBlock(0.0, 0.05, 1.0, false, 0.0);
    runtime->push(TrackId{ 8 }, kTrackAutomationVolumeId, 0.4f, 1.2);
    runtime->audioThread_beginBlock(0.05, 0.05, 2.0, false, 0.0);
    runtime->requestRelease(TrackId{ 8 }, kTrackAutomationVolumeId, 2.4);
    runtime->audioThread_beginBlock(0.10, 0.05, 3.0, false, 0.0);
    runtime->audioThread_observe(TrackId{ 8 }, true, 0, 0.10);
    expect(runtime->adoptionHeld(TrackId{ 8 }), "a snapshot without the published revision does not release the hold");
    TrackId id = kInvalidTrackId;
    int param = 0;
    TrackAutomationCommit commit;
    expect(runtime->takeCommit(id, param, commit), "observe test took the commit");
    runtime->noteRequiredRevision(id, param, commit.revision, 6);
    runtime->audioThread_observe(id, true, 6, 0.05);
    expect(runtime->adoptionHeld(id), "the ramp has not finished");
    runtime->audioThread_observe(id, true, 6, commit.rampEndSeconds + 0.001);
    expect(!runtime->adoptionHeld(id), "hold clears only after the snapshot revision and the ramp");

    const auto rejected = std::make_unique<TrackAutomationRuntime>();
    rejected->begin(TrackId{ 9 }, kTrackAutomationVolumeId, true, true, 0.2f, 0.0, 12, frozen, 1.0f);
    rejected->audioThread_beginBlock(0.0, 0.02, 1.0, false, 0.0);
    rejected->requestRelease(TrackId{ 9 }, kTrackAutomationVolumeId, 1.5);
    rejected->audioThread_beginBlock(0.02, 0.02, 2.0, false, 0.0);
    TrackAutomationCommit failed;
    expect(rejected->takeCommit(id, param, failed), "failed path has a commit");
    rejected->noteRejected(id, param, failed.revision);
    rejected->audioThread_observe(id, true, 1, failed.rampEndSeconds + 0.001);
    expect(!rejected->adoptionHeld(id), "a rejected commit clears the hold without a false revision");
    rejected->begin(TrackId{ 9 }, kTrackAutomationVolumeId, true, false, 0.3f, 3.0, 13, frozen, 1.0f);
    expect(rejected->isCapturing(TrackId{ 9 }, kTrackAutomationVolumeId), "the track can record again");
}
} // namespace

int main()
{
    auto step = [](const char* name, auto&& fn)
    {
        std::cout << name << std::endl;
        fn();
    };
    step("shapes", testIndependentShapes);
    step("punch", testPunchKeepsOutside);
    step("legacy", testLegacyQuadratic);
    step("spacing", testHostSpacing);
    step("gesture", testGestureMatchesReplay);
    step("rates", testRatesAndPaths);
    step("overflow", testOverflowAndNextGesture);
    step("drain", testLongDrain);
    step("identity", testGestureIdentity);
    step("undo", testUndoRedoAndDuplicateAndSave);
    step("observe", testObserveAndReadAheadHold);
    step("handover", testHandoverThreads);
    {
        juce::ScopedJuceInitialiser_GUI gui;
        const juce::File shotDir = juce::File::getSpecialLocation(juce::File::currentExecutableFile)
                                       .getParentDirectory()
                                       .getChildFile("automation-ui-shots");
        shotDir.createDirectory();
        testHeaderAndLaneDrag(shotDir);
        testLaneDragResult();
    }
    if (gFailures != 0)
    {
        std::cerr << gFailures << " failed\n";
        return 1;
    }
    std::cout << "TrackAutomationFocusedTests ok\n";
    return 0;
}

#include "plugins/ExperimentalInstrumentHost.h"
#include "plugins/PluginInsertHost.h"

void PluginInsertHost::audioThread_clearScratch(int, int) noexcept {}
float* const* PluginInsertHost::audioThread_getScratchWritePointers() noexcept { return nullptr; }
void PluginInsertHost::audioThread_processChainForTrack(TrackId, InsertStage, int) noexcept {}
bool PluginInsertHost::audioThread_hasActivePluginForTrack(TrackId) const noexcept { return false; }
const PluginAudioThreadMap::Entry* PluginInsertHost::audioThread_findEntry(const PluginAudioThreadMap&, TrackId) noexcept
{
    return nullptr;
}
void PluginInsertHost::audioThread_processEntryChain(const PluginAudioThreadMap::Entry&,
                                                     InsertStage,
                                                     int,
                                                     float* const*,
                                                     int,
                                                     juce::MidiBuffer&,
                                                     int) noexcept
{
}
void ExperimentalInstrumentHost::audioThread_processBlockAndAddToOutputs(float* const*, int, int, float, float) noexcept {}
