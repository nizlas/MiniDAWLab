// =============================================================================
// InputRoutingFocusedTests — focused, deterministic tests for per-track input
// selection + software input monitoring (no audio device, no UI, no plugins).
// =============================================================================
//
// Scope (input-selection/monitoring slice only — NOT the monolithic selftest):
//   1. `packedActiveInputPositionForPhysical` — the PRODUCTION physical→packed
//      mapping used by the audio callback for recording and monitoring,
//      including sparse enabled-channel sets.
//   2. `sanitizeTrackInputAssignment` + `SessionSnapshot::withTrackInputAssignment`
//      — domain rules (invalid values repair to the legacy default, never to a
//      different concrete input).
//   3. ProjectFile v23 round-trip — save/load of `inputKind`/`inputChanA`/`inputChanB`,
//      legacy pre-v23 files defaulting to DefaultFirstInput, and preservation of
//      an assignment that is unavailable on this machine.
//   4. `RecorderService` — the PRODUCTION raw capture path: mono and stereo takes
//      (interleaved FIFO → writer thread → 24-bit WAV), channel order, layout.
//   5. `playback_mix_helpers` — the PRODUCTION monitoring strip pass
//      (`renderLiveInputTrackPostStripToStereoScratch`: mono fan-out without
//      doubling, pre-gain placement, mute) and monitored-track clip suppression
//      in `renderAudioTracksClipSummingForSegment`.
//
// These tests drive the same code the app runs; the injected-input fixture here
// is deterministic buffers, clearly distinct from physical ASIO verification.
//
// Run the exe: prints one line per check; exit 0 = all green.
// =============================================================================

#include "domain/AudioClip.h"
#include "domain/PlacedClip.h"
#include "domain/SessionSnapshot.h"
#include "domain/Track.h"
#include "domain/TrackStereoPan.h"
#include "engine/PlaybackMixHelpers.h"
#include "engine/RecorderService.h"
#include "io/MonoWavFileWriter.h"
#include "io/ProjectFile.h"

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>

#include <cmath>
#include <cstdio>
#include <memory>
#include <thread>
#include <vector>

namespace
{
int failures = 0;
int checks = 0;

void expect(const bool condition, const char* const label)
{
    ++checks;
    if (condition)
    {
        std::printf("[PASS] %s\n", label);
    }
    else
    {
        ++failures;
        std::printf("[FAIL] %s\n", label);
    }
}

[[nodiscard]] bool nearlyEqual(const float a, const float b, const float tol = 1.0e-5f)
{
    return std::fabs(a - b) <= tol;
}

// ---------------------------------------------------------------------------
// 1. Physical → packed active-channel mapping (sparse enables)
// ---------------------------------------------------------------------------
void testPackedActiveInputMapping()
{
    using playback_mix_helpers::packedActiveInputPositionForPhysical;

    // Dense mask: packed position equals the physical index.
    const std::uint64_t dense = 0b1111ull;
    expect(packedActiveInputPositionForPhysical(dense, 0) == 0
               && packedActiveInputPositionForPhysical(dense, 3) == 3,
           "map: dense enables — packed position equals physical index");

    // Sparse mask (only physical 2 and 5 enabled): callback array has 2 rows.
    const std::uint64_t sparse = (1ull << 2) | (1ull << 5);
    expect(packedActiveInputPositionForPhysical(sparse, 2) == 0,
           "map: sparse — physical 2 is packed position 0");
    expect(packedActiveInputPositionForPhysical(sparse, 5) == 1,
           "map: sparse — physical 5 is packed position 1");
    expect(packedActiveInputPositionForPhysical(sparse, 0) == -1
               && packedActiveInputPositionForPhysical(sparse, 3) == -1,
           "map: sparse — disabled physical channels resolve to -1 (silence, no substitute)");

    // Bounds and empty mask.
    expect(packedActiveInputPositionForPhysical(sparse, -1) == -1
               && packedActiveInputPositionForPhysical(sparse, 64) == -1
               && packedActiveInputPositionForPhysical(0ull, 0) == -1,
           "map: out-of-range physical index or empty mask resolve to -1");
}

// ---------------------------------------------------------------------------
// 2. Domain: sanitize + snapshot copy-on-write
// ---------------------------------------------------------------------------
void testDomainAssignmentRules()
{
    // Valid values pass through unchanged.
    const TrackInputAssignment mono3 = sanitizeTrackInputAssignment(
        { TrackInputKind::Mono, 3, -1 });
    expect(mono3.kind == TrackInputKind::Mono && mono3.physicalChannelA == 3
               && mono3.physicalChannelB == -1,
           "domain: valid mono assignment is preserved");

    const TrackInputAssignment st01 = sanitizeTrackInputAssignment(
        { TrackInputKind::StereoPair, 0, 1 });
    expect(st01.kind == TrackInputKind::StereoPair && st01.physicalChannelA == 0
               && st01.physicalChannelB == 1,
           "domain: valid stereo pair is preserved");

    // Broken values repair to the legacy default — never to a different concrete input.
    expect(sanitizeTrackInputAssignment({ TrackInputKind::Mono, -5, -1 }).kind
               == TrackInputKind::DefaultFirstInput,
           "domain: mono with negative channel repairs to legacy default");
    expect(sanitizeTrackInputAssignment({ TrackInputKind::StereoPair, 4, 4 }).kind
               == TrackInputKind::DefaultFirstInput,
           "domain: stereo pair with identical channels repairs to legacy default");
    expect(sanitizeTrackInputAssignment({ TrackInputKind::None, 9, 9 }).physicalChannelA == -1,
           "domain: None clears stray channel indices");

    // Snapshot copy-on-write applies only to the targeted track.
    std::vector<Track> tracks;
    tracks.emplace_back(TrackId{ 1 }, juce::String("Guitar"), std::vector<PlacedClip>{});
    tracks.emplace_back(TrackId{ 2 }, juce::String("Vocals"), std::vector<PlacedClip>{});
    const auto snap = SessionSnapshot::withTracks(std::move(tracks), 0, 0, 0, ProjectMusicalTime{});
    const auto next = SessionSnapshot::withTrackInputAssignment(
        *snap, TrackId{ 1 }, { TrackInputKind::Mono, 2, -1 });
    expect(next != nullptr
               && next->getTrack(0).getInputAssignment().kind == TrackInputKind::Mono
               && next->getTrack(0).getInputAssignment().physicalChannelA == 2,
           "domain: withTrackInputAssignment sets the targeted track");
    expect(next->getTrack(1).getInputAssignment().kind == TrackInputKind::DefaultFirstInput,
           "domain: withTrackInputAssignment leaves other tracks on the legacy default");
}

// ---------------------------------------------------------------------------
// 3. Persistence: v23 round-trip, legacy default, unavailable preservation
// ---------------------------------------------------------------------------
void testPersistenceRoundTrip()
{
    const juce::File dir = juce::File::getSpecialLocation(juce::File::tempDirectory)
                               .getChildFile("dal-input-routing-tests");
    (void)dir.createDirectory();
    const juce::File f = dir.getChildFile("roundtrip.dalproj");

    ProjectFileV1 out;
    out.deviceSampleRateAtSave = 48000.0;
    out.timelineSampleRate = 48000.0;
    out.tracks.resize(4);
    out.tracks[0].id = TrackId{ 1 };
    out.tracks[0].name = "DefaultTrack";
    out.tracks[1].id = TrackId{ 2 };
    out.tracks[1].name = "MonoTrack";
    out.tracks[1].inputAssignment = { TrackInputKind::Mono, 3, -1 };
    out.tracks[2].id = TrackId{ 3 };
    out.tracks[2].name = "StereoTrack";
    out.tracks[2].inputAssignment = { TrackInputKind::StereoPair, 0, 1 };
    out.tracks[3].id = TrackId{ 4 };
    out.tracks[3].name = "NoneAndUnavailable";
    out.tracks[3].inputAssignment = { TrackInputKind::None, -1, -1 };
    expect(writeProjectFile(f, out).wasOk(), "persist: v23 write succeeds");

    ProjectFileV1 in;
    // Note: the reader repairs a master-less project by appending a Master row, so the loaded
    // track count is our 4 rows + 1 (that repair predates this feature and is not under test).
    expect(readProjectFile(f, in).wasOk() && in.tracks.size() >= 4, "persist: v23 read succeeds");
    expect(in.tracks[0].inputAssignment.kind == TrackInputKind::DefaultFirstInput,
           "persist: absent input keys load as the legacy default");
    expect(in.tracks[1].inputAssignment.kind == TrackInputKind::Mono
               && in.tracks[1].inputAssignment.physicalChannelA == 3,
           "persist: mono assignment round-trips (physical index verbatim)");
    expect(in.tracks[2].inputAssignment.kind == TrackInputKind::StereoPair
               && in.tracks[2].inputAssignment.physicalChannelA == 0
               && in.tracks[2].inputAssignment.physicalChannelB == 1,
           "persist: stereo pair round-trips in order");
    expect(in.tracks[3].inputAssignment.kind == TrackInputKind::None,
           "persist: explicit None round-trips");

    // Unavailable-on-this-machine: a high physical index (nothing local has 200 inputs) must be
    // preserved verbatim so the original device can resolve it again.
    ProjectFileV1 far;
    far.deviceSampleRateAtSave = 48000.0;
    far.timelineSampleRate = 48000.0;
    far.tracks.resize(1);
    far.tracks[0].id = TrackId{ 1 };
    far.tracks[0].name = "OtherMachine";
    far.tracks[0].inputAssignment = { TrackInputKind::Mono, 200, -1 };
    expect(writeProjectFile(f, far).wasOk() && readProjectFile(f, far).wasOk()
               && far.tracks[0].inputAssignment.kind == TrackInputKind::Mono
               && far.tracks[0].inputAssignment.physicalChannelA == 200,
           "persist: unavailable input assignment is preserved verbatim (never substituted)");

    // Legacy pre-v23 file: strip the new keys and lower the version — loads as the default.
    {
        juce::var root;
        expect(juce::JSON::parse(f.loadFileAsString(), root).wasOk()
                   && root.getDynamicObject() != nullptr,
               "persist: legacy fixture parse ok");
        root.getDynamicObject()->setProperty("version", 22);
        if (auto* trs = root.getProperty("tracks", {}).getArray())
        {
            for (auto& tv : *trs)
            {
                if (auto* to = tv.getDynamicObject())
                {
                    to->removeProperty("inputKind");
                    to->removeProperty("inputChanA");
                    to->removeProperty("inputChanB");
                }
            }
        }
        (void)f.replaceWithText(juce::JSON::toString(root, true));
        ProjectFileV1 legacy;
        expect(readProjectFile(f, legacy).wasOk()
                   && legacy.tracks[0].inputAssignment.kind == TrackInputKind::DefaultFirstInput,
               "persist: pre-v23 file loads as legacy default (established source kept)");
    }

    (void)dir.deleteRecursively();
}

// ---------------------------------------------------------------------------
// 4. RecorderService — production raw capture path (mono + stereo takes)
// ---------------------------------------------------------------------------
[[nodiscard]] bool readWavFully(const juce::File& f, juce::AudioBuffer<float>& outBuffer,
                                double& outRate)
{
    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader(fm.createReaderFor(f));
    if (reader == nullptr)
    {
        return false;
    }
    outRate = reader->sampleRate;
    outBuffer.setSize((int)reader->numChannels, (int)reader->lengthInSamples);
    return reader->read(&outBuffer, 0, (int)reader->lengthInSamples, 0, true, true);
}

void testRecorderMonoTake()
{
    const juce::File dir = juce::File::getSpecialLocation(juce::File::tempDirectory)
                               .getChildFile("dal-input-routing-tests");
    (void)dir.createDirectory();
    const juce::File takeFile = dir.getChildFile("mono-take.wav");

    RecorderService rec;
    rec.armForRecording(TrackId{ 7 });

    BeginRecordingRequest req;
    req.takeFile = takeFile;
    req.targetTrackId = TrackId{ 7 };
    req.recordingStartSample = 0;
    req.sampleRate = 48000.0;
    req.numChannels = 1;
    req.inputPhysicalChannelA = 2;
    expect(rec.beginRecording(req), "rec-mono: beginRecording succeeds");
    expect(rec.getRecordingNumChannels() == 1 && rec.getRecordingInputPhysicalChannelA() == 2,
           "rec-mono: audio-thread take parameters expose the selected physical channel");

    // Three blocks of a deterministic ramp; distinct "wrong channel" data proves no leakage by
    // simply never being handed to the recorder (the callback routes per selection).
    constexpr int kBlock = 512;
    std::vector<float> ramp((size_t)kBlock * 3);
    for (size_t i = 0; i < ramp.size(); ++i)
    {
        ramp[i] = -0.9f + 1.8f * (float)i / (float)(ramp.size() - 1);
    }
    for (int b = 0; b < 3; ++b)
    {
        rec.pushInputBlock(ramp.data() + (size_t)b * kBlock, nullptr, kBlock);
    }
    // Writer thread drains asynchronously; give it a moment before finalize.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const RecordedTakeResult r = rec.stopRecordingAndFinalize();
    expect(r.success && r.numChannels == 1 && r.intendedSampleCount == kBlock * 3,
           "rec-mono: finalize reports mono take with intended frame count");

    juce::AudioBuffer<float> wav;
    double rate = 0.0;
    expect(readWavFully(takeFile, wav, rate) && wav.getNumChannels() == 1
               && wav.getNumSamples() == kBlock * 3 && rate == 48000.0,
           "rec-mono: WAV on disk is mono, 48 kHz, exact length");
    bool exact = wav.getNumSamples() == kBlock * 3;
    for (int i = 0; exact && i < wav.getNumSamples(); ++i)
    {
        exact = nearlyEqual(wav.getSample(0, i), ramp[(size_t)i], 2.0e-7f * 2.0f + 1.0e-6f);
    }
    expect(exact, "rec-mono: recorded samples are bit-close to the injected input (raw, dry)");
    (void)dir.deleteRecursively();
}

void testRecorderStereoTake()
{
    const juce::File dir = juce::File::getSpecialLocation(juce::File::tempDirectory)
                               .getChildFile("dal-input-routing-tests");
    (void)dir.createDirectory();
    const juce::File takeFile = dir.getChildFile("stereo-take.wav");

    RecorderService rec;
    rec.armForRecording(TrackId{ 9 });

    BeginRecordingRequest req;
    req.takeFile = takeFile;
    req.targetTrackId = TrackId{ 9 };
    req.recordingStartSample = 0;
    req.sampleRate = 48000.0;
    req.numChannels = 2;
    req.inputPhysicalChannelA = 0;
    req.inputPhysicalChannelB = 1;
    expect(rec.beginRecording(req), "rec-stereo: beginRecording succeeds");

    // Distinct per-channel signals: L = +0.5 constant, R = alternating ±0.25. Channel order in
    // the WAV must match (no swap, no leakage between channels).
    constexpr int kBlock = 480;
    std::vector<float> left((size_t)kBlock, 0.5f);
    std::vector<float> right((size_t)kBlock);
    for (int i = 0; i < kBlock; ++i)
    {
        right[(size_t)i] = (i % 2 == 0) ? 0.25f : -0.25f;
    }
    for (int b = 0; b < 2; ++b)
    {
        rec.pushInputBlock(left.data(), right.data(), kBlock);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const RecordedTakeResult r = rec.stopRecordingAndFinalize();
    expect(r.success && r.numChannels == 2 && r.intendedSampleCount == kBlock * 2,
           "rec-stereo: finalize reports stereo take with intended frame count");

    juce::AudioBuffer<float> wav;
    double rate = 0.0;
    expect(readWavFully(takeFile, wav, rate) && wav.getNumChannels() == 2
               && wav.getNumSamples() == kBlock * 2,
           "rec-stereo: WAV on disk is stereo with exact frame count");
    bool orderOk = wav.getNumChannels() == 2 && wav.getNumSamples() == kBlock * 2;
    for (int i = 0; orderOk && i < wav.getNumSamples(); ++i)
    {
        orderOk = nearlyEqual(wav.getSample(0, i), 0.5f, 1.0e-5f)
                  && nearlyEqual(wav.getSample(1, i), (i % 2 == 0) ? 0.25f : -0.25f, 1.0e-5f);
    }
    expect(orderOk, "rec-stereo: left/right channel order and content are exact (no swap/leak)");

    // Stereo take with a null right pointer records silence on R (unresolved half never
    // substitutes another input).
    const juce::File takeFile2 = dir.getChildFile("stereo-nullR.wav");
    req.takeFile = takeFile2;
    expect(rec.beginRecording(req), "rec-stereo: second take begins");
    rec.pushInputBlock(left.data(), nullptr, kBlock);
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    const RecordedTakeResult r2 = rec.stopRecordingAndFinalize();
    juce::AudioBuffer<float> wav2;
    double rate2 = 0.0;
    bool nullROk = r2.success && readWavFully(takeFile2, wav2, rate2)
                   && wav2.getNumChannels() == 2 && wav2.getNumSamples() == kBlock;
    for (int i = 0; nullROk && i < wav2.getNumSamples(); ++i)
    {
        nullROk = nearlyEqual(wav2.getSample(0, i), 0.5f, 1.0e-5f)
                  && nearlyEqual(wav2.getSample(1, i), 0.0f, 1.0e-6f);
    }
    expect(nullROk, "rec-stereo: unresolved right channel records silence, left unaffected");
    (void)dir.deleteRecursively();
}

// ---------------------------------------------------------------------------
// 5. Monitoring strip pass + clip suppression (production mix helpers)
// ---------------------------------------------------------------------------
[[nodiscard]] std::shared_ptr<const SessionSnapshot> makeSnapshotWithToneClip(
    const TrackId trackId, const float clipValue, const int clipLen)
{
    juce::AudioBuffer<float> pcm(1, clipLen);
    for (int i = 0; i < clipLen; ++i)
    {
        pcm.setSample(0, i, clipValue);
    }
    const auto material = std::make_shared<const AudioClip>(std::move(pcm), 48000.0, "test-tone");
    std::vector<PlacedClip> clips;
    clips.emplace_back(PlacedClipId{ 1 }, material, 0);
    std::vector<Track> tracks;
    tracks.emplace_back(trackId, juce::String("Mon"), std::move(clips));
    return SessionSnapshot::withTracks(std::move(tracks), 0, 0, 0, ProjectMusicalTime{});
}

void testMonitoringStripPass()
{
    using namespace playback_mix_helpers;

    constexpr int kN = 256;
    std::vector<float> in((size_t)kN, 0.5f);
    std::vector<float> stageL((size_t)kN, 0.0f);
    std::vector<float> stageR((size_t)kN, 0.0f);

    std::vector<Track> ts;
    ts.emplace_back(TrackId{ 1 }, juce::String("Mon"), std::vector<PlacedClip>{});
    const Track& tr = ts.back();

    // Mono live input, unity fader, center pan: both sides get the SAME pan-law gain a mono clip
    // gets — one copy per side, no unintended doubling.
    renderLiveInputTrackPostStripToStereoScratch(
        tr, 0, in.data(), nullptr, kN, stageL.data(), stageR.data(), nullptr, nullptr);
    const float expected = 0.5f * trackPanLawGainLeft(0.0f);
    expect(nearlyEqual(stageL[0], expected) && nearlyEqual(stageR[0], expected)
               && nearlyEqual(stageL[kN - 1], expected),
           "strip: mono live input fans to L/R at mono-clip level (no doubling)");

    // Stereo pair keeps channel separation.
    std::vector<float> inR((size_t)kN, -0.25f);
    std::fill(stageL.begin(), stageL.end(), 0.0f);
    std::fill(stageR.begin(), stageR.end(), 0.0f);
    renderLiveInputTrackPostStripToStereoScratch(
        tr, 0, in.data(), inR.data(), kN, stageL.data(), stageR.data(), nullptr, nullptr);
    expect(nearlyEqual(stageL[0], 0.5f * trackPanLawGainLeft(0.0f))
               && nearlyEqual(stageR[0], -0.25f * trackPanLawGainRight(0.0f)),
           "strip: stereo live input keeps left/right separation through the strip");

    // Pre-gain scales the monitored signal (recording stays raw by construction: the recorder
    // capture point is before this pass entirely).
    const Track boosted = tr.withPreGainDb(6.0206f); // ×2 linear
    std::fill(stageL.begin(), stageL.end(), 0.0f);
    std::fill(stageR.begin(), stageR.end(), 0.0f);
    renderLiveInputTrackPostStripToStereoScratch(
        boosted, 0, in.data(), nullptr, kN, stageL.data(), stageR.data(), nullptr, nullptr);
    expect(nearlyEqual(stageL[0], 2.0f * expected, 1.0e-3f),
           "strip: pre-gain applies to the monitored signal");

    // Muted track contributes nothing.
    std::vector<Track> muted;
    muted.emplace_back(TrackId{ 1 }, juce::String("Mon"), std::vector<PlacedClip>{}, 1.0f, false,
                       true);
    std::fill(stageL.begin(), stageL.end(), 0.0f);
    renderLiveInputTrackPostStripToStereoScratch(
        muted.back(), 0, in.data(), nullptr, kN, stageL.data(), stageR.data(), nullptr, nullptr);
    expect(nearlyEqual(stageL[0], 0.0f, 1.0e-9f), "strip: muted track monitors silence");

    // Unresolved input (null pointers) is silence — no substitute source.
    std::fill(stageL.begin(), stageL.end(), 0.0f);
    renderLiveInputTrackPostStripToStereoScratch(
        tr, 0, nullptr, nullptr, kN, stageL.data(), stageR.data(), nullptr, nullptr);
    expect(nearlyEqual(stageL[0], 0.0f, 1.0e-9f),
           "strip: unresolved live input renders silence (never a different channel)");
}

void testMonitoredClipSuppression()
{
    using namespace playback_mix_helpers;

    constexpr int kN = 256;
    const TrackId tid{ 5 };
    const auto snap = makeSnapshotWithToneClip(tid, 0.4f, kN * 4);

    std::vector<float> outL((size_t)kN, 0.0f);
    std::vector<float> outR((size_t)kN, 0.0f);
    float* outs[2] = { outL.data(), outR.data() };

    // Monitor OFF: the clip renders into the outputs.
    renderAudioTracksClipSummingForSegment(
        *snap, 0, kN, 0, 2, outs, nullptr, kInvalidTrackId, kN * 4, -1, nullptr, nullptr);
    expect(std::fabs(outL[0]) > 1.0e-3f,
           "suppress: Monitor OFF — timeline clip is audible in the mix pass");

    // Monitor ON for this track: same call with a monitor snapshot silences the clip.
    LiveInputMonitorSnapshot mon;
    mon.count = 1;
    mon.trackIds[0] = tid;
    std::fill(outL.begin(), outL.end(), 0.0f);
    std::fill(outR.begin(), outR.end(), 0.0f);
    renderAudioTracksClipSummingForSegment(
        *snap, 0, kN, 0, 2, outs, nullptr, kInvalidTrackId, kN * 4, -1, nullptr, &mon);
    expect(nearlyEqual(outL[0], 0.0f, 1.0e-9f) && nearlyEqual(outR[0], 0.0f, 1.0e-9f),
           "suppress: Monitor ON — that track's clips are suppressed in the mix pass");

    // A different monitored track does not affect this one (no cross-track suppression).
    mon.trackIds[0] = TrackId{ 99 };
    std::fill(outL.begin(), outL.end(), 0.0f);
    renderAudioTracksClipSummingForSegment(
        *snap, 0, kN, 0, 2, outs, nullptr, kInvalidTrackId, kN * 4, -1, nullptr, &mon);
    expect(std::fabs(outL[0]) > 1.0e-3f,
           "suppress: monitoring another track leaves this track's clips playing");

    // Offline-mixdown equivalence: the export path passes no monitor snapshot (nullptr), which is
    // exactly the Monitor-OFF render above — exports contain clips regardless of Monitor state.
}

// ---------------------------------------------------------------------------
// 6. Multi-channel WAV writer (cycle takes)
// ---------------------------------------------------------------------------
void testMultiChannelWavWriter()
{
    const juce::File dir = juce::File::getSpecialLocation(juce::File::tempDirectory)
                               .getChildFile("dal-input-routing-tests");
    (void)dir.createDirectory();
    const juce::File f = dir.getChildFile("multi.wav");

    constexpr int kFrames = 300;
    std::vector<float> l((size_t)kFrames, 0.3f);
    std::vector<float> r((size_t)kFrames, -0.6f);
    const float* chans[2] = { l.data(), r.data() };
    expect(MonoWavFileWriter::writeMulti24BitWavSegment(f, chans, 2, kFrames, 48000.0).wasOk(),
           "wav: multi-channel segment write succeeds");

    juce::AudioBuffer<float> wav;
    double rate = 0.0;
    bool ok = readWavFully(f, wav, rate) && wav.getNumChannels() == 2
              && wav.getNumSamples() == kFrames && rate == 48000.0;
    for (int i = 0; ok && i < kFrames; ++i)
    {
        ok = nearlyEqual(wav.getSample(0, i), 0.3f) && nearlyEqual(wav.getSample(1, i), -0.6f);
    }
    expect(ok, "wav: stereo cycle segment content and channel order are exact");
    (void)dir.deleteRecursively();
}

} // namespace

int main()
{
    testPackedActiveInputMapping();
    testDomainAssignmentRules();
    testPersistenceRoundTrip();
    testRecorderMonoTake();
    testRecorderStereoTake();
    testMonitoringStripPass();
    testMonitoredClipSuppression();
    testMultiChannelWavWriter();

    std::printf("\n%d checks, %d failure(s)\n", checks, failures);
    return failures == 0 ? 0 : 1;
}

// ---------------------------------------------------------------------------
// Link seams — PlaybackMixHelpers.cpp references these host entry points, but every test here
// passes null hosts, so the stubs never execute (they exist only to satisfy the linker without
// dragging VST3 hosting into this focused console target).
// ---------------------------------------------------------------------------
#include "plugins/ExperimentalInstrumentHost.h"
#include "plugins/PluginInsertHost.h"

void PluginInsertHost::audioThread_clearScratch(int, int) noexcept {}
float* const* PluginInsertHost::audioThread_getScratchWritePointers() noexcept { return nullptr; }
void PluginInsertHost::audioThread_processChainForTrack(TrackId, InsertStage, int) noexcept {}
bool PluginInsertHost::audioThread_hasActivePluginForTrack(TrackId) const noexcept { return false; }
void ExperimentalInstrumentHost::audioThread_processBlockAndAddToOutputs(
    float* const*, int, int, float, float) noexcept {}
