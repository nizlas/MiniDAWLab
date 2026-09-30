// =============================================================================
// MixdownPreGainFocusedTests — pre-gain signal chain + MP3/WAV export lifecycle (production code)
// =============================================================================
//
// Two user reports drove this harness:
//   1. "Pre-gain −24 dB on an AmpliTube track changed nothing."
//   2. "MP3 export hangs at the end, leaves a WAV behind, shows a white box."
//
// Everything here runs the PRODUCTION implementations — there is no parallel copy of the mix
// helpers, the session, the project file, the VST3 insert host or the LAME runner:
//   A. Pre-gain domain chain: Track → SessionSnapshot → Session setter → undo/redo → save/reload.
//   B. Pre-gain in the production mix helpers (dry clip path, legacy clip path, monitoring path,
//      live ramp, "applied exactly once" against fader and pan).
//   C. Pre-gain into a REAL VST3 through the production `PluginInsertHost`: DAL Mono Delay at
//      mix 0 % is bit-transparent, so its output IS the signal entering the first insert.
//      Optionally (--amplitube) the real AmpliTube 4 is fed 0 dB vs −24 dB and its output compared.
//   D. LAME: the pre-fix wait pattern reproduced (LAME blocks on its undrained console pipe),
//      then the production `runLameMp3EncodeBlocking` (completes, real percentage, cancel, error,
//      no leftovers) with the produced MP3 decoded back by `lame --decode` and measured.
//   E. The production `AudioMixdownProgressWindow` shown on screen while the message thread is
//      blocked exactly like a real export: captured from the SCREEN (GDI BitBlt), and Cancel
//      delivered as a real OS mouse click (SendInput) — labelled as such in the output.
//
// Monitoring uses deterministic injected buffers through the production strip pass: that is a
// synthetic-input test of the software path, not a hardware/ASIO test.
//
// Usage:
//   MixdownPreGainFocusedTests.exe [--out <evidenceDir>] [--lame <lame.exe>]
//                                  [--delay-vst3 <DALMonoDelay.vst3>] [--amplitube] [--no-ui]
//   MixdownPreGainFocusedTests.exe --make-fixture <dir>    (writes the audio-only pre-gain fixture
//                                                           project used by --stability-pregain)
// Exit 0 = all checks green.
// =============================================================================

#include "app/AudioMixdownProgressWindow.h"
#include "app/Mp3LameEncoder.h"
#include "domain/AudioClip.h"
#include "domain/PlacedClip.h"
#include "domain/Session.h"
#include "domain/SessionHistory.h"
#include "domain/SessionSnapshot.h"
#include "domain/Track.h"
#include "domain/TrackStereoPan.h"
#include "engine/PlaybackMixHelpers.h"
#include "io/MonoWavFileWriter.h"
#include "plugins/PluginInsertHost.h"
#include "transport/Transport.h"

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_gui_basics/juce_gui_basics.h>

#if JUCE_WINDOWS
 #define WIN32_LEAN_AND_MEAN
 #define NOMINMAX
 #include <windows.h>
#endif

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace
{
int failures = 0;
int checks = 0;
juce::File evidenceDir;

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

[[nodiscard]] bool nearlyEqual(const double a, const double b, const double relTol)
{
    return std::fabs(a - b) <= relTol * std::fabs(b);
}

constexpr double kRate = 48000.0;
const double kMinus24Linear = std::pow(10.0, -24.0 / 20.0); // 0.0630957

[[nodiscard]] juce::File repoRootFromExecutable()
{
    juce::File dir = juce::File::getSpecialLocation(juce::File::currentExecutableFile).getParentDirectory();
    for (int i = 0; i < 8; ++i)
    {
        if (dir.getChildFile("CMakeLists.txt").existsAsFile() && dir.getChildFile("external_tools").isDirectory())
        {
            return dir;
        }
        dir = dir.getParentDirectory();
    }
    return {};
}

[[nodiscard]] double rmsOf(const std::vector<float>& v)
{
    if (v.empty())
    {
        return 0.0;
    }
    double s = 0.0;
    for (const float x : v)
    {
        s += static_cast<double>(x) * static_cast<double>(x);
    }
    return std::sqrt(s / static_cast<double>(v.size()));
}

[[nodiscard]] double peakOf(const std::vector<float>& v)
{
    double p = 0.0;
    for (const float x : v)
    {
        p = std::max(p, std::fabs(static_cast<double>(x)));
    }
    return p;
}

// ---------------------------------------------------------------------------------------------
// A. Domain chain: Track / SessionSnapshot / Session / undo / save-reload
// ---------------------------------------------------------------------------------------------
[[nodiscard]] std::shared_ptr<const SessionSnapshot> makeSnapshotWithConstantClip(
    const TrackId trackId, const float clipValue, const int clipLen, const float faderGain = 1.0f,
    const float pan = 0.0f)
{
    juce::AudioBuffer<float> pcm(1, clipLen);
    for (int i = 0; i < clipLen; ++i)
    {
        pcm.setSample(0, i, clipValue);
    }
    const auto material = std::make_shared<const AudioClip>(std::move(pcm), kRate, "constant");
    std::vector<PlacedClip> clips;
    clips.emplace_back(PlacedClipId{ 1 }, material, 0);
    std::vector<Track> tracks;
    tracks.emplace_back(trackId, juce::String("Guitar"), std::move(clips), faderGain, false, false,
                        TrackKind::Audio, pan);
    return SessionSnapshot::withTracks(std::move(tracks), 0, 0, 0, ProjectMusicalTime{});
}

void testDomainChain()
{
    // Track value semantics: the copy-on-write helper keeps every other field and clamps.
    std::vector<Track> ts;
    ts.emplace_back(TrackId{ 1 }, juce::String("Guitar"), std::vector<PlacedClip>{});
    const Track t24 = ts.back().withPreGainDb(-24.0f);
    expect(std::fabs(t24.getPreGainDb() + 24.0f) < 1.0e-6f, "domain: Track::withPreGainDb stores -24 dB");
    expect(std::fabs(t24.withPreGainDb(-99.0f).getPreGainDb() + 24.0f) < 1.0e-6f
               && std::fabs(t24.withPreGainDb(99.0f).getPreGainDb() - 24.0f) < 1.0e-6f,
           "domain: pre-gain clamps to [-24, +24] dB");
    expect(t24.withChannelFaderGain(0.5f).getPreGainDb() == t24.getPreGainDb()
               && t24.withMuted(true).getPreGainDb() == t24.getPreGainDb()
               && t24.withStereoPan(0.3f).getPreGainDb() == t24.getPreGainDb(),
           "domain: other Track edits (fader, mute, pan) keep the pre-gain");

    // Snapshot copy-on-write targets exactly one row.
    std::vector<Track> two;
    two.emplace_back(TrackId{ 1 }, juce::String("A"), std::vector<PlacedClip>{});
    two.emplace_back(TrackId{ 2 }, juce::String("B"), std::vector<PlacedClip>{});
    const auto snap = SessionSnapshot::withTracks(std::move(two), 0, 0, 0, ProjectMusicalTime{});
    const auto next = SessionSnapshot::withTrackPreGainDb(*snap, TrackId{ 2 }, -24.0f);
    expect(next->getTrack(1).getPreGainDb() == -24.0f && next->getTrack(0).getPreGainDb() == 0.0f,
           "domain: SessionSnapshot::withTrackPreGainDb changes only the addressed track");

    // Session setter publishes for the audio thread; a repeated value is a no-op (no undo noise).
    Session session;
    const TrackId active = session.getActiveTrackId();
    const auto before = session.loadSessionSnapshotForAudioThread();
    expect(session.setTrackPreGainDb(active, -24.0f), "session: setTrackPreGainDb(-24) publishes");
    const auto after = session.loadSessionSnapshotForAudioThread();
    expect(after.get() != before.get()
               && after->getTrack(after->findTrackIndexById(active)).getPreGainDb() == -24.0f,
           "session: published snapshot carries -24 dB (what the audio callback reads)");
    expect(!session.setTrackPreGainDb(active, -24.0f), "session: same value again is a no-op");

    // Undo / redo through the production history + restore path.
    SessionHistory history;
    history.record("Set pre-gain", before, after);
    const auto undo = history.popUndo();
    expect(undo.has_value(), "undo: history pops the pre-gain step");
    if (undo.has_value())
    {
        session.restoreSessionSnapshotForUndo(undo->timelineSnapshot);
        const auto s = session.loadSessionSnapshotForAudioThread();
        expect(s->getTrack(s->findTrackIndexById(active)).getPreGainDb() == 0.0f,
               "undo: restores 0 dB");
    }
    const auto redo = history.popRedo();
    if (redo.has_value())
    {
        session.restoreSessionSnapshotForUndo(redo->timelineSnapshot);
        const auto s = session.loadSessionSnapshotForAudioThread();
        expect(s->getTrack(s->findTrackIndexById(active)).getPreGainDb() == -24.0f,
               "redo: restores -24 dB");
    }
}

/// Writes `Audio/tone.wav` (mono sine) into `root` and returns the file.
[[nodiscard]] juce::File writeToneWav(const juce::File& root, const double seconds, const float amplitude,
                                      const double hz)
{
    (void)root.getChildFile("Audio").createDirectory();
    const juce::File wav = root.getChildFile("Audio").getChildFile("tone.wav");
    const int n = static_cast<int>(seconds * kRate);
    std::vector<float> pcm(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i)
    {
        pcm[(size_t)i] = amplitude * static_cast<float>(std::sin(2.0 * juce::MathConstants<double>::pi * hz * i / kRate));
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

void testSaveReloadRoundTrip()
{
    const juce::File root = juce::File::getSpecialLocation(juce::File::tempDirectory)
                                .getChildFile("dal-pregain-tests").getChildFile("roundtrip");
    (void)root.deleteRecursively();
    (void)root.createDirectory();
    const juce::File wav = writeToneWav(root, 1.0, 0.5f, 1000.0);
    const juce::File proj = root.getChildFile("roundtrip.dalproj");

    {
        Session session;
        Transport transport;
        const TrackId tid = session.getActiveTrackId();
        expect(session.addRecordedTakeAtSample(wav, kRate, 0, tid, static_cast<std::int64_t>(kRate)).wasOk(),
               "save/reload: fixture take added");
        expect(session.setTrackPreGainDb(tid, -24.0f), "save/reload: pre-gain set to -24 dB before save");
        expect(session.saveProjectToFile(transport, proj, kRate).wasOk(), "save/reload: project saved");
        expect(proj.loadFileAsString().contains("preGainDb"), "save/reload: JSON carries the preGainDb key");
    }
    {
        Session session;
        Transport transport;
        juce::StringArray skipped;
        juce::String note;
        expect(session.loadProjectFromFile(transport, proj, kRate, skipped, note).wasOk(),
               "save/reload: project loaded");
        const auto s = session.loadSessionSnapshotForAudioThread();
        bool found = false;
        for (int i = 0; s != nullptr && i < s->getNumTracks(); ++i)
        {
            if (s->getTrack(i).getKind() == TrackKind::Audio && s->getTrack(i).getNumPlacedClips() > 0)
            {
                found = true;
                expect(std::fabs(s->getTrack(i).getPreGainDb() + 24.0f) < 1.0e-5f,
                       "save/reload: reloaded audio track reads -24 dB");
            }
        }
        expect(found, "save/reload: the audio track with the take survived the round trip");
    }
    (void)root.deleteRecursively();
}

// ---------------------------------------------------------------------------------------------
// B. Production mix helpers (dry paths, ramp, exactly-once)
// ---------------------------------------------------------------------------------------------
constexpr int kBlock = 512;

/// Renders one block of the clip path (`renderAudioTrackPostStripToStereoScratch`) for a snapshot.
[[nodiscard]] std::vector<float> renderClipPathBlock(const SessionSnapshot& snap, PluginInsertHost* host,
                                                     playback_mix_helpers::PreGainRampState* ramp,
                                                     const std::int64_t startSample = 0)
{
    std::vector<float> l((size_t)kBlock, 0.0f), r((size_t)kBlock, 0.0f);
    playback_mix_helpers::renderAudioTrackPostStripToStereoScratch(
        snap, startSample, kBlock, 0, l.data(), r.data(), host, kInvalidTrackId, kBlock * 64, 0, ramp);
    return l;
}

/// Renders one block of the monitoring path with an injected (synthetic) input buffer.
[[nodiscard]] std::vector<float> renderMonitorPathBlock(const Track& track, const std::vector<float>& input,
                                                        PluginInsertHost* host,
                                                        playback_mix_helpers::PreGainRampState* ramp)
{
    std::vector<float> l((size_t)kBlock, 0.0f), r((size_t)kBlock, 0.0f);
    playback_mix_helpers::renderLiveInputTrackPostStripToStereoScratch(
        track, 0, input.data(), nullptr, kBlock, l.data(), r.data(), host, ramp);
    return l;
}

void testMixHelpersDryPaths()
{
    const TrackId tid{ 1 };
    const auto snap0 = makeSnapshotWithConstantClip(tid, 0.5f, kBlock * 64);
    const auto snap24 = SessionSnapshot::withTrackPreGainDb(*snap0, tid, -24.0f);

    // Clip path (staged renderer used by the routing plan): ratio of −24 dB to 0 dB output.
    const std::vector<float> out0 = renderClipPathBlock(*snap0, nullptr, nullptr);
    const std::vector<float> out24 = renderClipPathBlock(*snap24, nullptr, nullptr);
    const double ratioClip = peakOf(out24) / peakOf(out0);
    info("clip path: 0 dB peak=" + juce::String(peakOf(out0), 6) + " -24 dB peak=" + juce::String(peakOf(out24), 6)
         + " ratio=" + juce::String(ratioClip, 6) + " expected=" + juce::String(kMinus24Linear, 6));
    expect(nearlyEqual(peakOf(out0), 0.5 * trackPanLawGainLeft(0.0f), 1.0e-5),
           "clip path: 0 dB is transparent (clip × pan law only)");
    expect(nearlyEqual(ratioClip, kMinus24Linear, 1.0e-4), "clip path: -24 dB scales by 0.0631 (dry, staged renderer)");

    // Legacy clip renderer (non-staged): same rule.
    std::vector<float> lo0((size_t)kBlock, 0.0f), ro0((size_t)kBlock, 0.0f);
    std::vector<float> lo24((size_t)kBlock, 0.0f), ro24((size_t)kBlock, 0.0f);
    float* outs0[2] = { lo0.data(), ro0.data() };
    float* outs24[2] = { lo24.data(), ro24.data() };
    playback_mix_helpers::renderAudioTracksClipSummingForSegment(*snap0, 0, kBlock, 0, 2, outs0, nullptr,
                                                                 kInvalidTrackId, kBlock * 64, -1, nullptr, nullptr);
    playback_mix_helpers::renderAudioTracksClipSummingForSegment(*snap24, 0, kBlock, 0, 2, outs24, nullptr,
                                                                 kInvalidTrackId, kBlock * 64, -1, nullptr, nullptr);
    expect(nearlyEqual(peakOf(lo24) / peakOf(lo0), kMinus24Linear, 1.0e-4),
           "clip path: -24 dB scales by 0.0631 (dry, legacy renderer)");

    // Monitoring path with a synthetic input (software strip, NOT a hardware test).
    std::vector<float> input((size_t)kBlock, 0.5f);
    const std::vector<float> mon0 = renderMonitorPathBlock(snap0->getTrack(0), input, nullptr, nullptr);
    const std::vector<float> mon24 = renderMonitorPathBlock(snap24->getTrack(0), input, nullptr, nullptr);
    const double ratioMon = peakOf(mon24) / peakOf(mon0);
    info("monitor path (synthetic input): ratio=" + juce::String(ratioMon, 6));
    expect(nearlyEqual(ratioMon, kMinus24Linear, 1.0e-4), "monitor path: -24 dB scales by 0.0631 (dry, synthetic input)");

    // Exactly once: fader 0.5 and pan hard-left must multiply with pre-gain, never square it.
    const auto snapFader = SessionSnapshot::withTrackPreGainDb(
        *makeSnapshotWithConstantClip(tid, 0.5f, kBlock * 64, 0.5f, -1.0f), tid, -24.0f);
    const std::vector<float> outFader = renderClipPathBlock(*snapFader, nullptr, nullptr);
    const double expectedOnce = 0.5 * kMinus24Linear * 0.5 * trackPanLawGainLeft(-1.0f);
    info("exactly-once: measured=" + juce::String(peakOf(outFader), 6) + " expected=" + juce::String(expectedOnce, 6));
    expect(nearlyEqual(peakOf(outFader), expectedOnce, 1.0e-4),
           "exactly once: clip × pre-gain × fader × pan (pre-gain neither skipped nor doubled)");
}

void testMixHelpersLiveRamp()
{
    const TrackId tid{ 1 };
    const auto snap0 = makeSnapshotWithConstantClip(tid, 0.5f, kBlock * 64);
    const auto snap24 = SessionSnapshot::withTrackPreGainDb(*snap0, tid, -24.0f);
    playback_mix_helpers::PreGainRampState ramp;

    // Unprimed first block after prepare: the saved value applies immediately — no fade-in.
    const std::vector<float> first = renderClipPathBlock(*snap24, nullptr, &ramp);
    expect(nearlyEqual(first.front(), 0.5 * kMinus24Linear * trackPanLawGainLeft(0.0f), 1.0e-4)
               && nearlyEqual(first.back(), first.front(), 1.0e-6),
           "ramp: first block after prepare applies the stored pre-gain directly (no fade-in)");

    // Now a live change 0 dB → −24 dB while "playing": the block in which the change lands must
    // glide monotonically from the old to the new gain (no step), and the next block is settled.
    playback_mix_helpers::PreGainRampState liveRamp;
    (void)renderClipPathBlock(*snap0, nullptr, &liveRamp, 0);
    const std::vector<float> transition = renderClipPathBlock(*snap24, nullptr, &liveRamp, kBlock);
    const std::vector<float> settled = renderClipPathBlock(*snap24, nullptr, &liveRamp, kBlock * 2);
    const double startLevel = 0.5 * trackPanLawGainLeft(0.0f);
    const double endLevel = startLevel * kMinus24Linear;
    bool monotone = true;
    double maxStep = 0.0;
    for (size_t i = 1; i < transition.size(); ++i)
    {
        const double step = static_cast<double>(transition[i - 1]) - static_cast<double>(transition[i]);
        monotone = monotone && step >= -1.0e-7;
        maxStep = std::max(maxStep, std::fabs(step));
    }
    const double perSampleBudget = (startLevel - endLevel) / static_cast<double>(kBlock) * 1.5;
    info("ramp: first=" + juce::String(transition.front(), 6) + " last=" + juce::String(transition.back(), 6)
         + " maxStep=" + juce::String(maxStep, 7) + " budget=" + juce::String(perSampleBudget, 7)
         + " settled=" + juce::String(settled.front(), 6));
    expect(transition.front() > transition.back() && monotone && maxStep <= perSampleBudget,
           "ramp: live 0 → -24 dB glides monotonically within one block (no abrupt gain step)");
    expect(nearlyEqual(transition.front(), startLevel, 0.02) && nearlyEqual(settled.front(), endLevel, 1.0e-4)
               && nearlyEqual(settled.back(), endLevel, 1.0e-4),
           "ramp: starts at the old gain and the next block sits exactly on the new target");
}

// ---------------------------------------------------------------------------------------------
// C. Real VST3 through the production PluginInsertHost
// ---------------------------------------------------------------------------------------------
/// Produces a HOST-format state blob for DAL Mono Delay with Mix 0 % (bit-transparent dry path),
/// Feedback 0 % and both filters off. The blob comes from a real hosted instance whose public
/// parameters were set the way a user would, then exported with `getStateInformation()` — i.e.
/// exactly the bytes a DAL project file stores for an insert, so `PluginInsertHost::importChain`
/// (the project-load / undo path) can restore them. Returns an empty block on failure.
[[nodiscard]] juce::MemoryBlock makeTransparentDelayStateBlob(const juce::File& delayBundle)
{
    juce::AudioPluginFormatManager formats;
    formats.addFormat(new juce::VST3PluginFormat());
    juce::OwnedArray<juce::PluginDescription> found;
    for (int i = 0; i < formats.getNumFormats(); ++i)
    {
        formats.getFormat(i)->findAllTypesForFile(found, delayBundle.getFullPathName());
    }
    if (found.isEmpty())
    {
        return {};
    }
    juce::String err;
    std::unique_ptr<juce::AudioPluginInstance> inst = formats.createPluginInstance(*found[0], kRate, kBlock, err);
    if (inst == nullptr)
    {
        info("state blob: could not instantiate delay: " + err);
        return {};
    }
    const auto setParam = [&inst](const juce::String& name, const float normalised) {
        for (auto* p : inst->getParameters())
        {
            if (p->getName(64) == name)
            {
                p->setValue(normalised);
                return true;
            }
        }
        return false;
    };
    const bool ok = setParam("Mix", 0.0f) && setParam("Feedback", 0.0f) && setParam("Low Cut enabled", 0.0f)
                    && setParam("High Cut enabled", 0.0f);
    if (!ok)
    {
        info("state blob: parameter names not found on the hosted delay");
    }
    juce::MemoryBlock blob;
    inst->getStateInformation(blob);
    return blob;
}

void testPreGainIntoRealInsert(const juce::File& delayBundle)
{
    if (!delayBundle.exists())
    {
        expect(false, "vst3: DAL Mono Delay bundle not found: " + delayBundle.getFullPathName());
        return;
    }
    const TrackId tid{ 1 };
    PluginInsertHost host;
    host.prepareForDevice(kRate, kBlock, 2);
    const juce::Result loaded = host.addInsertFromVst3File(tid, InsertStage::Pre, delayBundle);
    expect(loaded.wasOk(), "vst3: DAL Mono Delay loads as a Pre insert through PluginInsertHost");
    if (loaded.failed())
    {
        info(loaded.getErrorMessage());
        return;
    }
    PluginTrackChain chain = host.exportChain(tid);
    const juce::MemoryBlock transparent = makeTransparentDelayStateBlob(delayBundle);
    expect(chain.slots.size() == 1 && transparent.getSize() > 0, "vst3: host-format state blob (Mix 0 %) produced from a real instance");
    if (chain.slots.size() == 1 && transparent.getSize() > 0)
    {
        chain.slots.front().opaqueState = transparent;
        host.importChain(tid, chain); // production project-load / undo restore path
    }
    expect(host.audioThread_hasActivePluginForTrack(tid), "vst3: insert is stereo-ready (active for the audio path)");

    const auto snap0 = makeSnapshotWithConstantClip(tid, 0.5f, kBlock * 64);
    const auto snap24 = SessionSnapshot::withTrackPreGainDb(*snap0, tid, -24.0f);

    // A few warm-up blocks let the plug-in's parameter smoothing settle on "fully dry".
    for (int i = 0; i < 8; ++i)
    {
        (void)renderClipPathBlock(*snap0, &host, nullptr);
    }
    const std::vector<float> out0 = renderClipPathBlock(*snap0, &host, nullptr);
    const std::vector<float> out24 = renderClipPathBlock(*snap24, &host, nullptr);
    const double ratio = peakOf(out24) / peakOf(out0);
    info("vst3 clip path: 0 dB peak=" + juce::String(peakOf(out0), 6) + " -24 dB peak=" + juce::String(peakOf(out24), 6)
         + " ratio=" + juce::String(ratio, 6));
    expect(nearlyEqual(peakOf(out0), 0.5 * trackPanLawGainLeft(0.0f), 1.0e-3),
           "vst3 clip path: transparent insert passes the 0 dB signal unchanged (plug-in input == output)");
    expect(nearlyEqual(ratio, kMinus24Linear, 1.0e-3),
           "vst3 clip path: the signal ENTERING the first Pre insert is scaled by 0.0631 at -24 dB");

    // Monitoring path with the same live insert (synthetic input).
    std::vector<float> input((size_t)kBlock, 0.5f);
    for (int i = 0; i < 4; ++i)
    {
        (void)renderMonitorPathBlock(snap0->getTrack(0), input, &host, nullptr);
    }
    const std::vector<float> mon0 = renderMonitorPathBlock(snap0->getTrack(0), input, &host, nullptr);
    const std::vector<float> mon24 = renderMonitorPathBlock(snap24->getTrack(0), input, &host, nullptr);
    const double ratioMon = peakOf(mon24) / peakOf(mon0);
    info("vst3 monitor path (synthetic input): ratio=" + juce::String(ratioMon, 6));
    expect(nearlyEqual(ratioMon, kMinus24Linear, 1.0e-3),
           "vst3 monitor path: the live input entering the first insert is scaled by 0.0631 at -24 dB");

    host.removeAllPlugins();
}

void testPreGainIntoAmpliTube(const juce::File& amplitubeVst3)
{
    if (!amplitubeVst3.exists())
    {
        info("AmpliTube VST3 not found at " + amplitubeVst3.getFullPathName() + " — skipped");
        return;
    }
    const TrackId tid{ 2 };
    PluginInsertHost host;
    host.prepareForDevice(kRate, kBlock, 2);
    const juce::Result loaded = host.addInsertFromVst3File(tid, InsertStage::Pre, amplitubeVst3);
    expect(loaded.wasOk(), "amplitube: loads through PluginInsertHost");
    if (loaded.failed())
    {
        info(loaded.getErrorMessage());
        return;
    }
    // Guitar-like test tone; the amp model is whatever AmpliTube loads by default, so only the
    // DIRECTION is asserted: a 16× quieter input must produce a measurably different output.
    juce::AudioBuffer<float> pcm(1, kBlock * 200);
    for (int i = 0; i < pcm.getNumSamples(); ++i)
    {
        pcm.setSample(0, i, 0.3f * static_cast<float>(std::sin(2.0 * juce::MathConstants<double>::pi * 110.0 * i / kRate)));
    }
    const auto material = std::make_shared<const AudioClip>(std::move(pcm), kRate, "guitar-ish");
    std::vector<PlacedClip> clips;
    clips.emplace_back(PlacedClipId{ 1 }, material, 0);
    std::vector<Track> tracks;
    tracks.emplace_back(tid, juce::String("Amp"), std::move(clips));
    const auto snap0 = SessionSnapshot::withTracks(std::move(tracks), 0, 0, 0, ProjectMusicalTime{});
    const auto snap24 = SessionSnapshot::withTrackPreGainDb(*snap0, tid, -24.0f);

    const auto renderSeconds = [&](const SessionSnapshot& snap, const double seconds) {
        std::vector<float> all;
        const int blocks = static_cast<int>(seconds * kRate / kBlock);
        for (int b = 0; b < blocks; ++b)
        {
            std::vector<float> l((size_t)kBlock, 0.0f), r((size_t)kBlock, 0.0f);
            playback_mix_helpers::renderAudioTrackPostStripToStereoScratch(
                snap, static_cast<std::int64_t>(b) * kBlock, kBlock, 0, l.data(), r.data(), &host, kInvalidTrackId,
                kBlock * 200, 0, nullptr);
            if (b >= blocks / 2) // skip the amp's attack / settling half
            {
                all.insert(all.end(), l.begin(), l.end());
            }
        }
        return all;
    };
    const double rms0 = rmsOf(renderSeconds(*snap0, 1.0));
    const double rms24 = rmsOf(renderSeconds(*snap24, 1.0));
    const double ratioDb = rms0 > 0.0 && rms24 > 0.0 ? 20.0 * std::log10(rms24 / rms0) : -999.0;
    info("amplitube: output RMS at 0 dB=" + juce::String(rms0, 6) + " at -24 dB=" + juce::String(rms24, 6)
         + " change=" + juce::String(ratioDb, 2) + " dB (input change was -24 dB)");
    expect(rms0 > 1.0e-4, "amplitube: produces output at 0 dB pre-gain");
    expect(rms24 < rms0 * 0.5, "amplitube: -24 dB pre-gain audibly changes the plug-in output (> 6 dB lower RMS)");
    host.removeAllPlugins();
}

// ---------------------------------------------------------------------------------------------
// D. LAME: pre-fix reproduction, then the production runner
// ---------------------------------------------------------------------------------------------
struct RecordingSink final : public mini_daw_audio_mixdown::MixdownProgressSink
{
    std::vector<juce::String> texts;
    std::vector<double> fractions;
    int cancelAfterUpdates = -1;
    bool cancelFlag = false;
    void setMixdownProgress(const juce::String& statusText, const double fraction01) override
    {
        texts.push_back(statusText);
        fractions.push_back(fraction01);
        if (cancelAfterUpdates >= 0 && static_cast<int>(texts.size()) >= cancelAfterUpdates)
        {
            cancelFlag = true;
        }
    }
    [[nodiscard]] bool isMixdownCancelRequested() const noexcept override { return cancelFlag; }
};

/// Decodes an MP3 with the bundled LAME (`--decode`) and returns the decoded WAV frame count + RMS.
[[nodiscard]] bool decodeMp3WithLame(const juce::File& lame, const juce::File& mp3, juce::int64& framesOut,
                                     double& rmsOut)
{
    const juce::File wav = mp3.getSiblingFile(mp3.getFileNameWithoutExtension() + "-decoded.wav");
    (void)wav.deleteFile();
    juce::ChildProcess p;
    juce::StringArray args { lame.getFullPathName(), "--decode", mp3.getFullPathName(), wav.getFullPathName() };
    if (!p.start(args, juce::ChildProcess::wantStdErr))
    {
        return false;
    }
    (void)p.readAllProcessOutput(); // drains until exit
    if (!wav.existsAsFile())
    {
        return false;
    }
    juce::WavAudioFormat fmt;
    std::unique_ptr<juce::AudioFormatReader> reader(fmt.createReaderFor(wav.createInputStream().release(), true));
    if (reader == nullptr)
    {
        return false;
    }
    framesOut = reader->lengthInSamples;
    juce::AudioBuffer<float> buf(static_cast<int>(reader->numChannels), static_cast<int>(framesOut));
    reader->read(&buf, 0, static_cast<int>(framesOut), 0, true, true);
    std::vector<float> mono(buf.getReadPointer(0), buf.getReadPointer(0) + framesOut);
    rmsOut = rmsOf(mono);
    (void)wav.deleteFile();
    return true;
}

void testLameBeforeFixReproduction(const juce::File& lame, const juce::File& sourceWav)
{
    // The PRE-FIX call pattern of AudioMixdownExporter.cpp: capture stderr, never read it,
    // poll `waitForProcessToFinish` in slices. With a 40 s file LAME prints ~6.5 KB of console
    // output — more than the pipe holds — so it must block. Bounded to 15 s here.
    const juce::File out = sourceWav.getSiblingFile("prefix-pattern.mp3");
    (void)out.deleteFile();
    juce::ChildProcess p;
    juce::StringArray args { lame.getFullPathName(), "-b", "320", sourceWav.getFullPathName(), out.getFullPathName() };
    const double t0 = juce::Time::getMillisecondCounterHiRes();
    expect(p.start(args, juce::ChildProcess::wantStdErr), "lame pre-fix pattern: process starts");
    bool finished = false;
    while (juce::Time::getMillisecondCounterHiRes() - t0 < 15000.0)
    {
        if (p.waitForProcessToFinish(100))
        {
            finished = true;
            break;
        }
    }
    const double elapsed = juce::Time::getMillisecondCounterHiRes() - t0;
    info("lame pre-fix pattern: finished=" + juce::String(finished ? "yes" : "NO (blocked)") + " after "
         + juce::String(elapsed, 0) + " ms; mp3 bytes on disk=" + juce::String(out.existsAsFile() ? out.getSize() : 0));
    expect(!finished, "lame pre-fix pattern REPRODUCED: LAME never exits when its console pipe is not drained (bounded 15 s)");
    // Draining now releases it — the same 1 s encode completes immediately.
    const juce::String drained = p.readAllProcessOutput();
    (void)p.waitForProcessToFinish(5000);
    info("lame pre-fix pattern: after draining " + juce::String(drained.length()) + " console bytes the process exited="
         + juce::String(p.isRunning() ? "no" : "yes"));
    expect(!p.isRunning() && drained.length() > 4096,
           "lame pre-fix pattern: console output exceeds 4 KiB and draining it lets LAME finish");
    (void)out.deleteFile();
}

void testLameProductionRunner(const juce::File& lame, const juce::File& sourceWav, const double sourceSeconds,
                              const double sourceRms)
{
    using namespace mini_daw_audio_mixdown;

    // Pure helpers.
    expect(parseLatestLameProgressPercent("  1600/1667  ( 96%)|    0:00/    0:00| x\r").value_or(-1) == 96,
           "lame parse: reads '( 96%)'");
    expect(parseLatestLameProgressPercent("( 5%) ... (100%)").value_or(-1) == 100, "lame parse: takes the LAST token");
    expect(!parseLatestLameProgressPercent("LAME 3.100 64bits\nEncoding as 48 kHz").has_value(),
           "lame parse: banner without percentage yields nothing");
    expect(expectedCbrMp3SizeBytes(40.0, 320) == 1600000, "lame size estimate: 40 s at 320 kbps = 1,600,000 bytes");

    // Success path.
    const juce::File out = sourceWav.getSiblingFile("runner-ok.mp3");
    Mp3LameEncodeRequest req;
    req.lameExecutable = lame;
    req.inputWav = sourceWav;
    req.outputMp3 = out;
    req.bitrateKbps = 320;
    req.expectedDurationSeconds = sourceSeconds;
    req.timeoutMs = 60000;
    RecordingSink sink;
    Mp3LameEncodeOutcome outcome;
    const double t0 = juce::Time::getMillisecondCounterHiRes();
    const juce::Result r = runLameMp3EncodeBlocking(req, &sink, outcome);
    const double elapsed = juce::Time::getMillisecondCounterHiRes() - t0;
    info("lame runner: result=" + juce::String(r.wasOk() ? "ok" : r.getErrorMessage()) + " exit=" + juce::String(outcome.exitCode)
         + " elapsedMs=" + juce::String(elapsed, 0) + " updates=" + juce::String((int)sink.texts.size())
         + " consoleBytes=" + juce::String(outcome.consoleTail.length()));
    expect(r.wasOk() && outcome.exitCode == 0, "lame runner: the same 40 s encode completes (drained pipe)");
    expect(elapsed < 20000.0, "lame runner: finishes in seconds, not minutes");
    int determinate = 0;
    bool increasing = true;
    double last = -1.0;
    bool sawEncodingText = false;
    for (size_t i = 0; i < sink.fractions.size(); ++i)
    {
        if (sink.fractions[i] >= 0.0)
        {
            ++determinate;
            increasing = increasing && sink.fractions[i] >= last - 1.0e-9;
            last = sink.fractions[i];
        }
        sawEncodingText = sawEncodingText || sink.texts[i].startsWith("Encoding MP3...");
    }
    expect(sawEncodingText && determinate >= 2 && increasing && last >= 0.99,
           "lame runner: reports 'Encoding MP3... NN%' with a real, non-decreasing percentage that reaches 100%");
    expect(outcome.consoleTail.length() > 1000, "lame runner: LAME's console output was captured (drained) for diagnostics");
    juce::int64 decodedFrames = 0;
    double decodedRms = 0.0;
    const bool decoded = decodeMp3WithLame(lame, out, decodedFrames, decodedRms);
    const juce::int64 sourceFrames = static_cast<juce::int64>(sourceSeconds * kRate);
    info("lame runner: decoded frames=" + juce::String(decodedFrames) + " source=" + juce::String(sourceFrames)
         + " decodedRms=" + juce::String(decodedRms, 5) + " sourceRms=" + juce::String(sourceRms, 5));
    expect(decoded && std::llabs(decodedFrames - sourceFrames) <= 2 * 1152,
           "lame runner: MP3 decodes back to the source length (±2 frames of encoder padding)");
    expect(decoded && nearlyEqual(decodedRms, sourceRms, 0.05), "lame runner: decoded RMS matches the source within 5%");
    (void)out.deleteFile();

    // Cancel path: request cancel at the first progress update; LAME must be gone and no file left.
    const juce::File outCancel = sourceWav.getSiblingFile("runner-cancel.mp3");
    RecordingSink cancelSink;
    cancelSink.cancelAfterUpdates = 1;
    Mp3LameEncodeOutcome cancelOutcome;
    req.outputMp3 = outCancel;
    const juce::Result rc = runLameMp3EncodeBlocking(req, &cancelSink, cancelOutcome);
    expect(rc.failed() && cancelOutcome.cancelled && rc.getErrorMessage().startsWith("Export cancelled"),
           "lame runner cancel: reports 'Export cancelled.'");
    expect(!outCancel.existsAsFile(), "lame runner cancel: partial MP3 removed");

    // Error path: garbage input.
    const juce::File garbage = sourceWav.getSiblingFile("garbage.wav");
    (void)garbage.replaceWithText("this is not a wav file at all");
    const juce::File outErr = sourceWav.getSiblingFile("runner-err.mp3");
    req.inputWav = garbage;
    req.outputMp3 = outErr;
    Mp3LameEncodeOutcome errOutcome;
    const juce::Result re = runLameMp3EncodeBlocking(req, nullptr, errOutcome);
    info("lame runner error: " + re.getErrorMessage().replaceCharacter('\n', ' '));
    expect(re.failed() && errOutcome.exitCode != 0 && !errOutcome.cancelled, "lame runner error: non-zero LAME exit is reported as an error");
    expect(!outErr.existsAsFile(), "lame runner error: no output file left behind");
    (void)garbage.deleteFile();

    // Missing encoder.
    req.lameExecutable = lame.getSiblingFile("does-not-exist.exe");
    req.inputWav = sourceWav;
    Mp3LameEncodeOutcome missing;
    expect(runLameMp3EncodeBlocking(req, nullptr, missing).failed() && !missing.processStarted,
           "lame runner: missing encoder fails cleanly without starting anything");
}

// ---------------------------------------------------------------------------------------------
// E. Progress window on screen while the message thread is blocked
// ---------------------------------------------------------------------------------------------
#if JUCE_WINDOWS
[[nodiscard]] juce::Image captureScreenRect(const juce::Rectangle<int> r)
{
    juce::Image img(juce::Image::ARGB, r.getWidth(), r.getHeight(), true);
    HDC screen = ::GetDC(nullptr);
    HDC mem = ::CreateCompatibleDC(screen);
    HBITMAP bmp = ::CreateCompatibleBitmap(screen, r.getWidth(), r.getHeight());
    HGDIOBJ old = ::SelectObject(mem, bmp);
    ::BitBlt(mem, 0, 0, r.getWidth(), r.getHeight(), screen, r.getX(), r.getY(), SRCCOPY | CAPTUREBLT);
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = r.getWidth();
    bi.bmiHeader.biHeight = -r.getHeight();
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    std::vector<std::uint32_t> pixels(static_cast<size_t>(r.getWidth() * r.getHeight()));
    ::GetDIBits(mem, bmp, 0, static_cast<UINT>(r.getHeight()), pixels.data(), &bi, DIB_RGB_COLORS);
    for (int y = 0; y < r.getHeight(); ++y)
    {
        for (int x = 0; x < r.getWidth(); ++x)
        {
            const std::uint32_t p = pixels[(size_t)(y * r.getWidth() + x)];
            img.setPixelAt(x, y, juce::Colour(static_cast<juce::uint8>((p >> 16) & 0xff),
                                              static_cast<juce::uint8>((p >> 8) & 0xff),
                                              static_cast<juce::uint8>(p & 0xff)));
        }
    }
    ::SelectObject(mem, old);
    ::DeleteObject(bmp);
    ::DeleteDC(mem);
    ::ReleaseDC(nullptr, screen);
    return img;
}
#endif

void savePng(const juce::Image& img, const juce::String& name)
{
    if (evidenceDir == juce::File{} || img.isNull())
    {
        return;
    }
    (void)evidenceDir.createDirectory();
    const juce::File f = evidenceDir.getChildFile(name);
    (void)f.deleteFile();
    juce::FileOutputStream os(f);
    if (os.openedOk())
    {
        juce::PNGImageFormat png;
        (void)png.writeImageToStream(img, os);
    }
}

[[nodiscard]] int countPixelsNear(const juce::Image& img, const juce::Colour want, const int tol = 12)
{
    int n = 0;
    for (int y = 0; y < img.getHeight(); ++y)
    {
        for (int x = 0; x < img.getWidth(); ++x)
        {
            const juce::Colour c = img.getPixelAt(x, y);
            if (std::abs((int)c.getRed() - (int)want.getRed()) <= tol && std::abs((int)c.getGreen() - (int)want.getGreen()) <= tol
                && std::abs((int)c.getBlue() - (int)want.getBlue()) <= tol)
            {
                ++n;
            }
        }
    }
    return n;
}

void testProgressWindowOnScreen()
{
    AudioMixdownProgressWindow window;
    const juce::Colour accent(0xff2d9d53);
    const juce::Colour background(0xff2a2a33);

    // Blocking "export": the message loop is NOT running here, exactly like the real export.
    // Drive the phases the exporter reports and capture the screen at each one.
    const auto pumpBlocked = [&window](const int ms) {
        const double until = juce::Time::getMillisecondCounterHiRes() + ms;
        while (juce::Time::getMillisecondCounterHiRes() < until)
        {
            juce::Thread::sleep(20);
            (void)window.isMixdownCancelRequested(); // services only the window's own messages
        }
    };

    window.setMixdownProgress("Rendering... 42%", 0.42);
    pumpBlocked(250);
    // GDI screen capture works in physical pixels; window coordinates seen by this (DPI-virtualised)
    // test process are logical. Ask Windows for the physical rect of our own HWND.
    juce::Rectangle<int> screenRect = window.getScreenBounds();
#if JUCE_WINDOWS
    {
        const HWND own = reinterpret_cast<HWND>(window.getPeer()->getNativeHandle());
        RECT wr{};
        ::GetWindowRect(own, &wr);
        POINT tl{ wr.left, wr.top };
        POINT br{ wr.right, wr.bottom };
        ::LogicalToPhysicalPointForPerMonitorDPI(own, &tl);
        ::LogicalToPhysicalPointForPerMonitorDPI(own, &br);
        screenRect = juce::Rectangle<int>(tl.x, tl.y, br.x - tl.x, br.y - tl.y);
    }
#endif
    info("progress window: renderer=" + window.getPeer()->getAvailableRenderingEngines()[window.getPeer()->getCurrentRenderingEngine()]
         + " logical=" + window.getScreenBounds().toString() + " physical=" + screenRect.toString()
         + " paintCount=" + juce::String(window.getPaintCount()));
    expect(window.getPaintCount() >= 1, "progress window: paint() ran while the message loop was blocked");
#if JUCE_WINDOWS
    {
        // The window's own surface (independent of z-order / composition): what Windows holds for it.
        const HWND own = reinterpret_cast<HWND>(window.getPeer()->getNativeHandle());
        RECT wr{};
        ::GetWindowRect(own, &wr);
        const int w = wr.right - wr.left, h = wr.bottom - wr.top;
        HDC screen = ::GetDC(nullptr);
        HDC mem = ::CreateCompatibleDC(screen);
        HBITMAP bmp = ::CreateCompatibleBitmap(screen, w, h);
        HGDIOBJ old = ::SelectObject(mem, bmp);
        const BOOL printed = ::PrintWindow(own, mem, 2 /* PW_RENDERFULLCONTENT */);
        BITMAPINFO bi = {};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = -h;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        std::vector<std::uint32_t> px(static_cast<size_t>(w * h));
        ::GetDIBits(mem, bmp, 0, static_cast<UINT>(h), px.data(), &bi, DIB_RGB_COLORS);
        juce::Image surface(juce::Image::ARGB, w, h, true);
        for (int y = 0; y < h; ++y)
        {
            for (int x = 0; x < w; ++x)
            {
                const std::uint32_t p = px[(size_t)(y * w + x)];
                surface.setPixelAt(x, y, juce::Colour((juce::uint8)((p >> 16) & 0xff), (juce::uint8)((p >> 8) & 0xff), (juce::uint8)(p & 0xff)));
            }
        }
        ::SelectObject(mem, old);
        ::DeleteObject(bmp);
        ::DeleteDC(mem);
        ::ReleaseDC(nullptr, screen);
        savePng(surface, "progress-rendering-42-window-surface.png");
        info("progress window surface (PrintWindow ok=" + juce::String((int)printed) + "): accent px="
             + juce::String(countPixelsNear(surface, accent)) + " background px=" + juce::String(countPixelsNear(surface, background))
             + " rect=" + juce::String(wr.left) + "," + juce::String(wr.top) + " " + juce::String(w) + "x" + juce::String(h));
    }
    // Context capture of the whole primary display for visual inspection of where the window is.
    savePng(captureScreenRect(juce::Rectangle<int>(0, 0, ::GetSystemMetrics(SM_CXSCREEN), ::GetSystemMetrics(SM_CYSCREEN)))
                .rescaled(1280, 720),
            "progress-rendering-42-fullscreen-context.png");
    const juce::Image shotRender = captureScreenRect(screenRect);
    savePng(shotRender, "progress-rendering-42-screen.png");
    const int accentPx = countPixelsNear(shotRender, accent);
    const int bgPx = countPixelsNear(shotRender, background);
    info("progress on-screen capture: accent px=" + juce::String(accentPx) + " background px=" + juce::String(bgPx)
         + " size=" + juce::String(screenRect.getWidth()) + "x" + juce::String(screenRect.getHeight()));
    expect(accentPx > 500 && bgPx > 5000,
           "progress window: SCREEN capture while blocked shows the painted bar and background (not a white box)");
#endif
    savePng(window.createComponentSnapshot(window.getLocalBounds(), false, 1.0f), "progress-rendering-42-component.png");

    window.setMixdownProgress("Encoding MP3... 80%", 0.80);
    pumpBlocked(150);
#if JUCE_WINDOWS
    const juce::Image shotEncode = captureScreenRect(screenRect);
    savePng(shotEncode, "progress-encoding-80-screen.png");
    expect(countPixelsNear(shotEncode, accent) > countPixelsNear(shotRender, accent),
           "progress window: the bar on screen grows from 42% to 80%");
#endif

    window.setMixdownProgress("Finalizing...", -1.0);
    pumpBlocked(150);
#if JUCE_WINDOWS
    savePng(captureScreenRect(screenRect), "progress-finalizing-screen.png");

    // Cancel via a REAL OS click on the Cancel button while the message loop stays blocked.
    // Cursor and hit-test APIs work in this process's (logical) coordinate space.
    const juce::Rectangle<int> btn = window.getCancelButtonScreenBounds();
    const juce::Point<int> centre = btn.getCentre();
    const HWND own = reinterpret_cast<HWND>(window.getPeer()->getNativeHandle());
    ::SetForegroundWindow(own);
    pumpBlocked(100);
    const HWND under = ::WindowFromPoint(POINT{ centre.x, centre.y });
    if (under != own)
    {
        info("progress window: another window covers the Cancel button — real click skipped, using requestCancel()");
        window.requestCancel();
    }
    else
    {
        // Position the pointer, then inject a real button press/release at that position.
        ::SetCursorPos(centre.x, centre.y);
        INPUT in[2] = {};
        in[0].type = INPUT_MOUSE;
        in[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
        in[1].type = INPUT_MOUSE;
        in[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
        ::SendInput(2, in, sizeof(INPUT));
        const double t0 = juce::Time::getMillisecondCounterHiRes();
        while (!window.isMixdownCancelRequested() && juce::Time::getMillisecondCounterHiRes() - t0 < 3000.0)
        {
            juce::Thread::sleep(20);
        }
        expect(window.wasCancelRequested(),
               "progress window: a REAL OS mouse click on Cancel is processed while the message loop is blocked");
    }
    pumpBlocked(150);
    savePng(captureScreenRect(screenRect), "progress-cancelling-screen.png");
#else
    window.requestCancel();
#endif
    expect(window.isMixdownCancelRequested(), "progress window: cancel request is visible to the exporter's poll");
}

// ---------------------------------------------------------------------------------------------
// Fixture for the in-app --stability-pregain scenario
// ---------------------------------------------------------------------------------------------
int makePreGainFixture(const juce::File& dir)
{
    (void)dir.deleteRecursively();
    if (!dir.createDirectory())
    {
        std::printf("could not create %s\n", dir.getFullPathName().toRawUTF8());
        return 1;
    }
    // 4 s of a steady 1 kHz tone at 0.1 (−20 dBFS — the scenario plays it through the real device),
    // loop range [0.5 s, 3.5 s) so the export never touches the clip edges; cycle armed so the
    // mixdown span resolves without any UI.
    const juce::File wav = writeToneWav(dir, 4.0, 0.1f, 1000.0);
    Session session;
    Transport transport;
    const TrackId tid = session.getActiveTrackId();
    if (session.addRecordedTakeAtSample(wav, kRate, 0, tid, static_cast<std::int64_t>(4.0 * kRate)).failed())
    {
        std::printf("fixture: add take failed\n");
        return 1;
    }
    session.setLeftLocatorAtSample(static_cast<std::int64_t>(0.5 * kRate));
    session.setRightLocatorAtSample(static_cast<std::int64_t>(3.5 * kRate));
    transport.requestCycleEnabled(true);
    const juce::File proj = dir.getChildFile("pregain-fixture.dalproj");
    const juce::Result saved = session.saveProjectToFile(transport, proj, kRate);
    if (saved.failed())
    {
        std::printf("fixture: save failed: %s\n", saved.getErrorMessage().toRawUTF8());
        return 1;
    }
    std::printf("%s\n", proj.getFullPathName().toRawUTF8());
    return 0;
}
} // namespace

int main(int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI juceGui;

    juce::File lame;
    juce::File delayBundle("C:\\Program Files\\Common Files\\VST3\\DALMonoDelay.vst3");
    juce::File amplitube("C:\\Program Files\\Common Files\\VST3\\AmpliTube 4.vst3");
    bool runAmplitube = false;
    bool runUi = true;
    for (int i = 1; i < argc; ++i)
    {
        const juce::String a(argv[i]);
        const auto next = [&]() -> juce::String { return i + 1 < argc ? juce::String(argv[++i]) : juce::String{}; };
        if (a == "--make-fixture")
        {
            return makePreGainFixture(juce::File(next()));
        }
        if (a == "--out") { evidenceDir = juce::File(next()); }
        else if (a == "--lame") { lame = juce::File(next()); }
        else if (a == "--delay-vst3") { delayBundle = juce::File(next()); }
        else if (a == "--amplitube") { runAmplitube = true; }
        else if (a == "--no-ui") { runUi = false; }
    }
    if (lame == juce::File{})
    {
        lame = repoRootFromExecutable().getChildFile("external_tools").getChildFile("lame").getChildFile("lame.exe");
    }

    testDomainChain();
    testSaveReloadRoundTrip();
    testMixHelpersDryPaths();
    testMixHelpersLiveRamp();
    testPreGainIntoRealInsert(delayBundle);
    if (runAmplitube)
    {
        testPreGainIntoAmpliTube(amplitube);
    }

    // LAME work area: a 40 s tone matches the size class of the user's stuck export.
    const juce::File work = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("dal-pregain-tests")
                                .getChildFile("lame");
    (void)work.deleteRecursively();
    (void)work.createDirectory();
    if (lame.existsAsFile())
    {
        constexpr double kSeconds = 40.0;
        const juce::File src = writeToneWav(work, kSeconds, 0.5f, 440.0);
        const double srcRms = 0.5 / std::sqrt(2.0);
        testLameBeforeFixReproduction(lame, src);
        testLameProductionRunner(lame, src, kSeconds, srcRms);
    }
    else
    {
        expect(false, "lame.exe not found (pass --lame): " + lame.getFullPathName());
    }
    (void)work.getParentDirectory().deleteRecursively();

    if (runUi)
    {
        testProgressWindowOnScreen();
    }

    std::printf("\n%d checks, %d failure(s)\n", checks, failures);
    return failures == 0 ? 0 : 1;
}

// ---------------------------------------------------------------------------
// Link seams — Session.cpp / PlaybackMixHelpers.cpp reference these instrument entry points; this
// harness never creates instrument tracks or instrument hosts, so the stubs are never executed.
// ---------------------------------------------------------------------------
#include "instruments/InstrumentTrackController.h"
#include "plugins/ExperimentalInstrumentHost.h"

ProjectFileExperimentalInstrumentTrackV1
InstrumentTrackController::buildExperimentalInstrumentProjectBlock() const
{
    jassertfalse;
    return {};
}
void ExperimentalInstrumentHost::audioThread_processBlockAndAddToOutputs(
    float* const*, int, int, float, float) noexcept {}
