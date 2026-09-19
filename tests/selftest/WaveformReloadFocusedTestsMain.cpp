// =============================================================================
// WaveformReloadFocusedTests — waveform display after project save/reload
// =============================================================================
//
// Reproduces the reported regression on the PRODUCTION path (no parallel
// rendering implementation):
//   real `Session::saveProjectToFile` / `loadProjectFromFile` → real
//   `ClipWaveformView::paint` (raster cache + pyramid cache) rendered offscreen,
//   asserting WAVEFORM pixels (not clip body / border / selection chrome).
//
// Mouse input cannot be synthesised without a window/desktop event loop here, so
// the press/release states are exercised through the production event handlers
// (`mouseDown` / `mouseUp` with constructed `juce::MouseEvent`s) on the real
// component. That is reported as a harness limitation, not a live-input test.
//
// Usage: WaveformReloadFocusedTests.exe [pngOutputDir]
// Exit 0 = all checks green.
// =============================================================================

#include "domain/AudioClip.h"
#include "domain/Session.h"
#include "domain/SessionSnapshot.h"
#include "io/AudioWaveformCache.h"
#include "io/MonoWavFileWriter.h"
#include "transport/Transport.h"
#include "ui/ClipWaveformView.h"
#include "ui/TimelineViewportModel.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>
#include <thread>
#include <vector>

namespace
{
int failures = 0;
int checks = 0;
juce::File pngDir;

void expect(const bool condition, const char* const label)
{
    ++checks;
    std::printf("[%s] %s\n", condition ? "PASS" : "FAIL", label);
    if (!condition)
    {
        ++failures;
    }
}

constexpr double kRate = 48000.0;
constexpr int kLaneW = 900;
constexpr int kLaneH = 72;
constexpr int kTakeFrames = (int)(48000.0 * 2.0);

// --- Fixture: a temp project folder with one non-silent recorded-style take -------------------
struct Fixture
{
    juce::File root;
    juce::File projectFile;
    juce::File wavFile;

    explicit Fixture(const juce::String& name)
    {
        root = juce::File::getSpecialLocation(juce::File::tempDirectory)
                   .getChildFile("dal-waveform-reload-tests")
                   .getChildFile(name);
        (void)root.deleteRecursively();
        (void)root.createDirectory();
        (void)root.getChildFile("Audio").createDirectory();
        projectFile = root.getChildFile("fixture.dalproj");
        wavFile = root.getChildFile("Audio").getChildFile("take_fixture.wav");
    }

    ~Fixture() { (void)root.getParentDirectory().deleteRecursively(); }

    /// Loud 220 Hz tone with an envelope — a silent file would make any waveform assertion vacuous.
    void writeTakeWav(const int numFrames = kTakeFrames) const
    {
        std::vector<float> pcm((size_t)numFrames);
        for (int i = 0; i < numFrames; ++i)
        {
            const double t = (double)i / kRate;
            const double env = 0.55 + 0.45 * std::sin(2.0 * juce::MathConstants<double>::pi * 0.75 * t);
            pcm[(size_t)i] = (float)(0.8 * env * std::sin(2.0 * juce::MathConstants<double>::pi * 220.0 * t));
        }
        const float* chans[1] = { pcm.data() };
        const juce::Result r
            = MonoWavFileWriter::writeMulti24BitWavSegment(wavFile, chans, 1, numFrames, kRate);
        jassert(r.wasOk());
        juce::ignoreUnused(r);
    }
};

// --- Offscreen render of the production lane -------------------------------------------------
struct LaneHarness
{
    Session session;
    Transport transport;
    TimelineViewportModel viewport;
    AudioWaveformCache cache;
    std::unique_ptr<ClipWaveformView> lane;

    LaneHarness() { cache.setOnPyramidReady([this](const AudioClip*) { pyramidNotifies++; if (lane != nullptr) { lane->repaint(); } }); }
    ~LaneHarness() { lane.reset(); cache.shutdown(); }

    int pyramidNotifies = 0;

    void createLaneForTrack(const TrackId tid)
    {
        lane = std::make_unique<ClipWaveformView>(session, transport, tid, viewport, cache);
        lane->setSize(kLaneW, kLaneH);
    }

    /// Seed zoom so the whole arrangement fits the lane width, like the app does at startup.
    void fitViewportToArrangement()
    {
        const std::int64_t ext = juce::jmax((std::int64_t)1, session.getArrangementExtentSamples());
        viewport.setSamplesPerPixelIfUnset((double)ext / (double)kLaneW);
        viewport.clampToExtent((double)kLaneW, ext);
    }

    [[nodiscard]] juce::Image render() const
    {
        return lane->createComponentSnapshot(lane->getLocalBounds(), false, 1.0f);
    }

    /// Pump the message loop so async pyramid-ready notifications and timers are delivered,
    /// exactly as they are for a real user.
    void pumpMessages(const int milliseconds)
    {
        const double until = juce::Time::getMillisecondCounterHiRes() + (double)milliseconds;
        while (juce::Time::getMillisecondCounterHiRes() < until)
        {
            (void)juce::MessageManager::getInstance()->runDispatchLoopUntil(10);
        }
    }
};

// --- Waveform-content metric ------------------------------------------------------------------
// The waveform is drawn as light-blue peak columns around the vertical centre of the clip body.
// A "waveform column" is a pixel column that contains clearly blue-dominant pixels spread
// vertically (peaks), which neither the flat body fill, the border, the selection overlay, nor the
// thin centre placeholder line can produce.
struct WaveformMetrics
{
    int waveformColumns = 0;
    int maxVerticalSpanPx = 0;
};

[[nodiscard]] WaveformMetrics measureWaveform(const juce::Image& img)
{
    WaveformMetrics m;
    if (img.isNull())
    {
        return m;
    }
    for (int x = 0; x < img.getWidth(); ++x)
    {
        int firstY = -1;
        int lastY = -1;
        int count = 0;
        for (int y = 0; y < img.getHeight(); ++y)
        {
            const juce::Colour c = img.getPixelAt(x, y);
            // Peak fill is light blue (JUCE lightblue ~ (173,216,230)) blended over the dark
            // body fill: blue clearly dominant and bright enough to exclude body/border greys.
            const int r = (int)c.getRed();
            const int g = (int)c.getGreen();
            const int b = (int)c.getBlue();
            if (b > 110 && b - r > 40 && g > r)
            {
                ++count;
                if (firstY < 0)
                {
                    firstY = y;
                }
                lastY = y;
            }
        }
        const int span = (firstY >= 0) ? (lastY - firstY + 1) : 0;
        // >3 px of vertical extent rules out the 1 px "pyramid not ready" centre line.
        if (count >= 3 && span > 3)
        {
            ++m.waveformColumns;
            m.maxVerticalSpanPx = juce::jmax(m.maxVerticalSpanPx, span);
        }
    }
    return m;
}

void savePng(const juce::Image& img, const char* const name)
{
    if (pngDir == juce::File{})
    {
        return;
    }
    (void)pngDir.createDirectory();
    const juce::File f = pngDir.getChildFile(name);
    (void)f.deleteFile();
    juce::FileOutputStream os(f);
    if (os.openedOk())
    {
        juce::PNGImageFormat png;
        (void)png.writeImageToStream(img, os);
    }
}

[[nodiscard]] juce::String describe(const WaveformMetrics& m)
{
    return "columns=" + juce::String(m.waveformColumns) + " maxSpanPx=" + juce::String(m.maxVerticalSpanPx);
}

// Build a saved project: one audio track with the take placed at 0, then save it.
[[nodiscard]] bool buildAndSaveFixtureProject(Fixture& fx)
{
    fx.writeTakeWav();

    LaneHarness h;
    const TrackId tid = h.session.getActiveTrackId();
    // Same entry point the recorder commit uses for a finished take.
    const juce::Result added
        = h.session.addRecordedTakeAtSample(fx.wavFile, kRate, 0, tid, kTakeFrames);
    if (!added.wasOk())
    {
        std::printf("       fixture add take failed: %s\n", added.getErrorMessage().toRawUTF8());
        return false;
    }
    const juce::Result saved
        = h.session.saveProjectToFile(h.transport, fx.projectFile, kRate);
    if (!saved.wasOk())
    {
        std::printf("       fixture save failed: %s\n", saved.getErrorMessage().toRawUTF8());
        return false;
    }
    return true;
}

// --- The regression scenario -------------------------------------------------------------------
void testWaveformAfterReload(const char* const tag)
{
    Fixture fx(juce::String("reload-") + tag);
    if (!buildAndSaveFixtureProject(fx))
    {
        expect(false, "reload: fixture project saved");
        return;
    }
    expect(true, "reload: fixture project saved (non-silent take, real save path)");

    LaneHarness h;
    juce::StringArray skipped;
    juce::String info;
    const juce::Result loaded
        = h.session.loadProjectFromFile(h.transport, fx.projectFile, kRate, skipped, info);
    expect(loaded.wasOk(), "reload: real loadProjectFromFile succeeded");

    const auto snap = h.session.loadSessionSnapshotForAudioThread();
    int audioTrackIdx = -1;
    for (int i = 0; snap != nullptr && i < snap->getNumTracks(); ++i)
    {
        if (snap->getTrack(i).getKind() == TrackKind::Audio && snap->getTrack(i).getNumPlacedClips() > 0)
        {
            audioTrackIdx = i;
            break;
        }
    }
    expect(audioTrackIdx >= 0, "reload: loaded project has an audio clip on an audio track");
    if (audioTrackIdx < 0)
    {
        return;
    }
    h.createLaneForTrack(snap->getTrack(audioTrackIdx).getId());
    h.fitViewportToArrangement();

    // (1) First paints + async pyramid build, WITHOUT any mouse interaction.
    (void)h.render();
    h.pumpMessages(900);
    const juce::Image afterLoad = h.render();
    savePng(afterLoad, (juce::String("waveform-after-reload-") + tag + ".png").toRawUTF8());
    const WaveformMetrics mLoad = measureWaveform(afterLoad);
    std::printf("       after reload (no interaction): %s, pyramidNotifies=%d\n",
                describe(mLoad).toRawUTF8(), h.pyramidNotifies);
    expect(mLoad.waveformColumns > 50,
           "reload: waveform is drawn after load WITHOUT clicking, dragging, resizing or playback");

    // (2) Mouse DOWN on the clip body without any movement (production handler).
    const juce::Point<float> onClip((float)(kLaneW / 3), (float)(kLaneH / 2));
    const juce::MouseEvent down(juce::Desktop::getInstance().getMainMouseSource(),
                                onClip,
                                juce::ModifierKeys(juce::ModifierKeys::leftButtonModifier),
                                1.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                                h.lane.get(), h.lane.get(), juce::Time::getCurrentTime(),
                                onClip, juce::Time::getCurrentTime(), 1, false);
    h.lane->mouseDown(down);
    const juce::Image whileDown = h.render();
    savePng(whileDown, (juce::String("waveform-mouse-down-") + tag + ".png").toRawUTF8());
    const WaveformMetrics mDown = measureWaveform(whileDown);
    std::printf("       mouse down (no movement): %s\n", describe(mDown).toRawUTF8());
    expect(mDown.waveformColumns > 50, "reload: waveform stays visible while the mouse is held down");

    // (3) Mouse UP (no movement) — release must not remove the waveform.
    const juce::MouseEvent up(juce::Desktop::getInstance().getMainMouseSource(),
                              onClip,
                              juce::ModifierKeys(),
                              1.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                              h.lane.get(), h.lane.get(), juce::Time::getCurrentTime(),
                              onClip, juce::Time::getCurrentTime(), 1, false);
    h.lane->mouseUp(up);
    h.pumpMessages(400);
    const juce::Image afterUp = h.render();
    savePng(afterUp, (juce::String("waveform-after-release-") + tag + ".png").toRawUTF8());
    const WaveformMetrics mUp = measureWaveform(afterUp);
    std::printf("       after release (selected): %s\n", describe(mUp).toRawUTF8());
    expect(mUp.waveformColumns > 50, "reload: waveform stays visible after mouse release");

    // (4) Deselect (production API used by TrackLanesView when clicking empty space).
    h.lane->clearSelectionOnly();
    h.pumpMessages(300);
    const juce::Image afterDeselect = h.render();
    savePng(afterDeselect, (juce::String("waveform-after-deselect-") + tag + ".png").toRawUTF8());
    const WaveformMetrics mDes = measureWaveform(afterDeselect);
    std::printf("       after deselect: %s\n", describe(mDes).toRawUTF8());
    expect(mDes.waveformColumns > 50, "reload: waveform stays visible after deselection");

    // (5) Zoom + scroll, stopped and while playing.
    const std::int64_t ext = juce::jmax((std::int64_t)1, h.session.getArrangementExtentSamples());
    h.viewport.zoomAroundSample(0.5,
                                (double)(kLaneW / 2),
                                (double)kLaneW,
                                ext,
                                1.0,
                                juce::jmax(1.0, (double)ext / (double)kLaneW));
    h.viewport.panBySamples(ext / 16, (double)kLaneW, ext);
    h.pumpMessages(500);
    const juce::Image zoomed = h.render();
    savePng(zoomed, (juce::String("waveform-zoomed-") + tag + ".png").toRawUTF8());
    const WaveformMetrics mZoom = measureWaveform(zoomed);
    std::printf("       zoomed/scrolled: %s\n", describe(mZoom).toRawUTF8());
    expect(mZoom.waveformColumns > 50, "reload: waveform renders after zoom + scroll (stopped)");

    h.transport.requestPlaybackIntent(PlaybackIntent::Playing);
    h.pumpMessages(300);
    const juce::Image playing = h.render();
    const WaveformMetrics mPlay = measureWaveform(playing);
    std::printf("       during playback intent: %s\n", describe(mPlay).toRawUTF8());
    expect(mPlay.waveformColumns > 50, "reload: waveform renders during playback");
}

} // namespace

// --- Regression 1: the cache must ANNOUNCE readiness ------------------------------------------
// Root cause of the reported bug: the built pyramid was moved into the cache slot and the
// "should I notify?" test then inspected the moved-from pointer, so the message-thread
// `onPyramidReady` callback was never sent. Nothing repainted the lanes, so a raster rasterized
// while the pyramid was still building (the normal case right after a project load) kept being
// blitted without peaks.
void testPyramidReadyNotificationIsSent()
{
    juce::AudioBuffer<float> pcm(1, 24000);
    for (int i = 0; i < pcm.getNumSamples(); ++i)
    {
        pcm.setSample(0, i, 0.7f * std::sin(2.0f * juce::MathConstants<float>::pi * 0.01f * (float)i));
    }
    const auto material
        = std::make_shared<const AudioClip>(std::move(pcm), kRate, "notify-test");

    AudioWaveformCache cache;
    int notifyCount = 0;
    const AudioClip* notifiedKey = nullptr;
    cache.setOnPyramidReady([&](const AudioClip* key) { ++notifyCount; notifiedKey = key; });

    expect(cache.getOrEnqueue(material) == nullptr,
           "notify: first getOrEnqueue returns null and schedules the background build");

    // Pump the message loop like the app does; the worker publishes and posts the notification.
    const double until = juce::Time::getMillisecondCounterHiRes() + 4000.0;
    while (juce::Time::getMillisecondCounterHiRes() < until && notifyCount == 0)
    {
        (void)juce::MessageManager::getInstance()->runDispatchLoopUntil(10);
    }

    expect(cache.isPyramidReady(material.get()), "notify: pyramid becomes ready in the cache");
    expect(notifyCount >= 1, "notify: onPyramidReady IS delivered when a pyramid becomes ready");
    expect(notifiedKey == material.get(), "notify: callback identifies the material that became ready");
    expect(cache.getOrEnqueue(material) != nullptr, "notify: the ready pyramid is returned afterwards");
    cache.shutdown();
}

// --- Regression 2: peak-less raster must not survive narrow (stripe) repaints ------------------
// After a reload the first paints happen while the pyramid is still building, so the cached raster
// has no peaks. Narrow repaints (playhead stripe, small dirty regions after a click) take the fast
// path that blits that raster and returns — without the fix they blit the peak-less raster
// indefinitely, which is exactly "the waveform vanishes again after releasing the mouse".
void testNarrowRepaintsRecoverWaveform()
{
    Fixture fx("narrow");
    if (!buildAndSaveFixtureProject(fx))
    {
        expect(false, "narrow: fixture project saved");
        return;
    }

    LaneHarness h;
    juce::StringArray skipped;
    juce::String info;
    const juce::Result loaded
        = h.session.loadProjectFromFile(h.transport, fx.projectFile, kRate, skipped, info);
    expect(loaded.wasOk(), "narrow: project loaded");

    const auto snap = h.session.loadSessionSnapshotForAudioThread();
    TrackId tid = kInvalidTrackId;
    for (int i = 0; snap != nullptr && i < snap->getNumTracks(); ++i)
    {
        if (snap->getTrack(i).getKind() == TrackKind::Audio
            && snap->getTrack(i).getNumPlacedClips() > 0)
        {
            tid = snap->getTrack(i).getId();
            break;
        }
    }
    if (tid == kInvalidTrackId)
    {
        expect(false, "narrow: loaded project has an audio clip");
        return;
    }
    h.createLaneForTrack(tid);
    // Zoom so the take spans the lane (a narrow strip then lands on clip material).
    h.viewport.setSamplesPerPixelIfUnset((double)kTakeFrames / (double)kLaneW);

    // First full paint: the app's post-load state — raster rasterized while the background pyramid
    // build is still in flight.
    savePng(h.render(), "narrow-first-paint.png");

    // Let the build finish (and the readiness notification land) with NO full repaint after it.
    h.pumpMessages(1200);

    // From here on, only narrow-region paints — the playhead-stripe style fast path.
    const juce::Rectangle<int> strip(kLaneW / 2, 0, 40, kLaneH);
    juce::Image narrow;
    for (int attempt = 0; attempt < 12; ++attempt)
    {
        narrow = h.lane->createComponentSnapshot(strip, false, 1.0f);
        if (measureWaveform(narrow).waveformColumns > 5)
        {
            break;
        }
        h.pumpMessages(150);
    }
    savePng(narrow, "narrow-stripe-paint.png");
    const WaveformMetrics mNarrow = measureWaveform(narrow);
    std::printf("       narrow stripe paints: %s\n", describe(mNarrow).toRawUTF8());
    expect(mNarrow.waveformColumns > 5,
           "narrow: waveform recovers through narrow (stripe) repaints alone");
}

/// `--make-fixture <dir>`: writes a standalone temp project (take WAV under `Audio/`) that the real
/// app can open, so live-app checks never touch the user's projects. Prints the project path.
[[nodiscard]] int makeStandaloneFixture(const juce::File& dir)
{
    (void)dir.createDirectory();
    (void)dir.getChildFile("Audio").createDirectory();
    const juce::File wav = dir.getChildFile("Audio").getChildFile("take_fixture.wav");
    const juce::File proj = dir.getChildFile("fixture.dalproj");

    std::vector<float> pcm((size_t)kTakeFrames);
    for (int i = 0; i < kTakeFrames; ++i)
    {
        const double t = (double)i / kRate;
        const double env = 0.55 + 0.45 * std::sin(2.0 * juce::MathConstants<double>::pi * 0.75 * t);
        pcm[(size_t)i]
            = (float)(0.8 * env * std::sin(2.0 * juce::MathConstants<double>::pi * 220.0 * t));
    }
    const float* chans[1] = { pcm.data() };
    if (!MonoWavFileWriter::writeMulti24BitWavSegment(wav, chans, 1, kTakeFrames, kRate).wasOk())
    {
        std::printf("fixture wav write failed\n");
        return 1;
    }

    LaneHarness h;
    const juce::Result added
        = h.session.addRecordedTakeAtSample(wav, kRate, 0, h.session.getActiveTrackId(), kTakeFrames);
    if (!added.wasOk())
    {
        std::printf("fixture take add failed: %s\n", added.getErrorMessage().toRawUTF8());
        return 1;
    }
    const juce::Result saved = h.session.saveProjectToFile(h.transport, proj, kRate);
    if (!saved.wasOk())
    {
        std::printf("fixture save failed: %s\n", saved.getErrorMessage().toRawUTF8());
        return 1;
    }
    std::printf("%s\n", proj.getFullPathName().toRawUTF8());
    return 0;
}

int main(int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI juceGui;
    if (argc > 2 && juce::String(argv[1]) == "--make-fixture")
    {
        return makeStandaloneFixture(juce::File(juce::String(argv[2])));
    }
    if (argc > 1)
    {
        pngDir = juce::File(juce::String(argv[1]));
    }

    testPyramidReadyNotificationIsSent();
    testNarrowRepaintsRecoverWaveform();
    testWaveformAfterReload("first");
    // Second, independent reload in a fresh process-local cache: waveform generation with no
    // pre-existing pyramid for that material.
    testWaveformAfterReload("second");

    std::printf("\n%d checks, %d failure(s)\n", checks, failures);
    return failures == 0 ? 0 : 1;
}

// ---------------------------------------------------------------------------
// Link seams — `Session.cpp` references these plugin/instrument entry points, but this harness
// always passes a null `PluginInsertHost` and loads projects without instrument tracks, so the
// stubs are never executed (they only keep VST3 hosting out of this console target).
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
