// =============================================================================
// TrackDuplicateFocusedTests — focused, deterministic tests for the Duplicate Track domain step
// (`SessionSnapshot::withTrackDuplicated` + `PlacedClip::withId`). No audio device, no UI, no
// plug-ins: the runtime side (own insert instances, instrument runtime, undo integration) is
// covered by the in-app scenario `--stability-duplicate-track`.
//
// Run the exe: prints one line per check; exit 0 = all green.
// =============================================================================

#include "domain/AudioClip.h"
#include "domain/PlacedClip.h"
#include "domain/SessionSnapshot.h"
#include "domain/Track.h"

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>

#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>

namespace
{
int failures = 0;
int checks = 0;

void expect(const bool condition, const char* const label)
{
    ++checks;
    std::printf(condition ? "[PASS] %s\n" : "[FAIL] %s\n", label);
    if (!condition)
    {
        ++failures;
    }
}

[[nodiscard]] std::shared_ptr<const AudioClip> makeMaterial(const int lengthSamples)
{
    juce::AudioBuffer<float> buf(1, lengthSamples);
    buf.clear();
    return std::make_shared<const AudioClip>(std::move(buf), 48000.0, "fixture");
}

/// Fixture: [1:Audio "Gitarr" (2 clips, routed to Group 10, send to 10, pre-gain -6, pan 0.3,
/// muted, off), 10:Group, 20:Instrument, 30:Midi (MIDI To 20), 2:Master].
[[nodiscard]] std::shared_ptr<const SessionSnapshot> makeFixture(std::shared_ptr<const AudioClip> material)
{
    std::vector<Track> tracks;
    {
        std::vector<PlacedClip> clips;
        PlacedClip a(PlacedClipId{100}, material, 1000);
        PlacedClip b(PlacedClipId{101}, material, 50000);
        clips.push_back(b); // newest first (index 0) — the Track's own order convention
        clips.push_back(a);
        Track audio(TrackId{1}, "Gitarr", clips, 0.7f, /*off*/ true, /*muted*/ true, TrackKind::Audio, 0.3f,
                    TrackId{10}, { TrackSend{ TrackId{10}, 0.25f, true, 1 } }, 1, kInvalidTrackId);
        tracks.push_back(audio.withPreGainDb(-6.0f));
    }
    tracks.push_back(Track(TrackId{10}, "Group 1", {}, 1.0f, false, false, TrackKind::Group, 0.0f, TrackId{2}, {}, 1,
                           kInvalidTrackId));
    tracks.push_back(Track(TrackId{20}, "VB3", {}, 0.5f, false, false, TrackKind::Instrument, -0.2f, TrackId{2}, {},
                           1, kInvalidTrackId));
    tracks.push_back(Track(TrackId{30}, "MIDI 1", {}, 1.0f, false, false, TrackKind::Midi, 0.0f, kInvalidTrackId, {},
                           3, TrackId{20}));
    tracks.push_back(Track(TrackId{2}, "Stereo Out", {}, 1.0f, false, false, TrackKind::Master, 0.0f, kInvalidTrackId,
                           {}, 1, kInvalidTrackId));
    return SessionSnapshot::withTracks(std::move(tracks));
}

void testPlacedClipWithId(const std::shared_ptr<const AudioClip>& material)
{
    PlacedClip src(PlacedClipId{7}, material, 1234);
    const PlacedClip copy = src.withId(PlacedClipId{8});
    expect(copy.getId() == PlacedClipId{8} && src.getId() == PlacedClipId{7}, "withId: new id on the copy, source untouched");
    expect(copy.getStartSample() == src.getStartSample() && copy.getMaterial() == src.getMaterial()
               && copy.getEffectiveLengthSamples() == src.getEffectiveLengthSamples()
               && copy.getLeftTrimSamples() == src.getLeftTrimSamples(),
           "withId: placement, trims and SHARED material identical");
}

void testDuplicateAudioTrack(const std::shared_ptr<const AudioClip>& material)
{
    const auto base = makeFixture(material);
    int clipIdsUsed = 0;
    const auto dup = SessionSnapshot::withTrackDuplicated(*base, TrackId{1}, TrackId{99}, "Gitarr - kopia",
                                                         PlacedClipId{500}, clipIdsUsed);
    expect(dup != nullptr && dup->getNumTracks() == base->getNumTracks() + 1, "audio: one more track");
    const int srcIx = dup->findTrackIndexById(TrackId{1});
    const int copyIx = dup->findTrackIndexById(TrackId{99});
    expect(srcIx == 0 && copyIx == 1, "audio: copy directly below the source");
    expect(clipIdsUsed == 2, "audio: two clip ids consumed");
    const Track& src = dup->getTrack(srcIx);
    const Track& copy = dup->getTrack(copyIx);
    expect(copy.getName() == "Gitarr - kopia" && copy.getKind() == TrackKind::Audio, "audio: name + kind");
    expect(copy.getPlacedClips().size() == 2 && copy.getPlacedClips()[0].getId() == PlacedClipId{500}
               && copy.getPlacedClips()[1].getId() == PlacedClipId{501},
           "audio: clips copied in order with the new ids");
    expect(copy.getPlacedClips()[0].getStartSample() == src.getPlacedClips()[0].getStartSample()
               && copy.getPlacedClips()[1].getStartSample() == src.getPlacedClips()[1].getStartSample()
               && copy.getPlacedClips()[0].getMaterial() == src.getPlacedClips()[0].getMaterial(),
           "audio: same placements, material shared (no WAV copies)");
    expect(std::fabs(copy.getChannelFaderGain() - 0.7f) < 1e-6f && std::fabs(copy.getPreGainDb() + 6.0f) < 1e-6f
               && std::fabs(copy.getStereoPan() - 0.3f) < 1e-6f && copy.isMuted() && copy.isTrackOff(),
           "audio: fader, pre-gain, pan, Mute, Off copied");
    expect(copy.getRoutedOutputTrackId() == TrackId{10} && copy.getSends().size() == 1
               && copy.getSends()[0].destTrackId == TrackId{10} && std::fabs(copy.getSends()[0].amountLinear - 0.25f) < 1e-6f
               && copy.getSends()[0].uiSlotIndex == 1,
           "audio: routing + sends copied to the same destinations");
    expect(src.getPlacedClips()[0].getId() == PlacedClipId{101} && src.getName() == "Gitarr",
           "audio: source row unchanged");
    // Nothing points at the copy.
    for (int i = 0; i < dup->getNumTracks(); ++i)
    {
        const Track& t = dup->getTrack(i);
        if (t.getRoutedOutputTrackId() == TrackId{99} || t.getMidiDestinationTrackId() == TrackId{99})
        {
            expect(false, "audio: no track routes to the copy");
            return;
        }
    }
    expect(true, "audio: no track routes to the copy");
}

void testDuplicateMidiAndGroup(const std::shared_ptr<const AudioClip>& material)
{
    const auto base = makeFixture(material);
    int used = 0;
    const auto dupMidi = SessionSnapshot::withTrackDuplicated(*base, TrackId{30}, TrackId{31}, "MIDI 1 - kopia",
                                                             PlacedClipId{500}, used);
    const int ix = dupMidi->findTrackIndexById(TrackId{31});
    expect(ix == dupMidi->findTrackIndexById(TrackId{30}) + 1, "midi: copy directly below the source");
    expect(dupMidi->getTrack(ix).getKind() == TrackKind::Midi && dupMidi->getTrack(ix).getMidiDestinationTrackId() == TrackId{20}
               && dupMidi->getTrack(ix).getMidiOutputChannel() == 3,
           "midi: same MIDI To destination + output channel");
    expect(used == 0, "midi: no clip ids consumed for a row without placed clips");

    const auto dupGroup = SessionSnapshot::withTrackDuplicated(*base, TrackId{10}, TrackId{11}, "Group 1 - kopia",
                                                              PlacedClipId{500}, used);
    const int gx = dupGroup->findTrackIndexById(TrackId{11});
    expect(gx == dupGroup->findTrackIndexById(TrackId{10}) + 1 && dupGroup->getTrack(gx).getKind() == TrackKind::Group
               && dupGroup->getTrack(gx).getRoutedOutputTrackId() == TrackId{2},
           "group: copy below the source with the same output routing");
    const Track& feeding = dupGroup->getTrack(dupGroup->findTrackIndexById(TrackId{1}));
    expect(feeding.getRoutedOutputTrackId() == TrackId{10} && feeding.getSends()[0].destTrackId == TrackId{10},
           "group: the feeding audio row still routes / sends to the ORIGINAL group only");
}

void testRefusals(const std::shared_ptr<const AudioClip>& material)
{
    const auto base = makeFixture(material);
    int used = 0;
    expect(SessionSnapshot::withTrackDuplicated(*base, TrackId{2}, TrackId{3}, "x", PlacedClipId{500}, used) == nullptr,
           "refuse: Stereo Out cannot be duplicated");
    expect(SessionSnapshot::withTrackDuplicated(*base, TrackId{77}, TrackId{3}, "x", PlacedClipId{500}, used) == nullptr,
           "refuse: unknown source id");
    expect(SessionSnapshot::withTrackDuplicated(*base, TrackId{1}, TrackId{10}, "x", PlacedClipId{500}, used) == nullptr,
           "refuse: new id already in use");
    expect(SessionSnapshot::withTrackDuplicated(*base, TrackId{1}, kInvalidTrackId, "x", PlacedClipId{500}, used) == nullptr,
           "refuse: invalid new id");
    expect(used == 0, "refuse: no clip ids consumed on refusal");
    // Master stays last and unique after a duplication.
    const auto dup = SessionSnapshot::withTrackDuplicated(*base, TrackId{20}, TrackId{21}, "VB3 - kopia", PlacedClipId{500}, used);
    int masters = 0;
    for (int i = 0; i < dup->getNumTracks(); ++i)
    {
        masters += dup->getTrack(i).getKind() == TrackKind::Master ? 1 : 0;
    }
    expect(masters == 1 && dup->getTrack(dup->getNumTracks() - 1).getKind() == TrackKind::Master,
           "invariant: exactly one Stereo Out, still last");
}
} // namespace

int main()
{
    const auto material = makeMaterial(96000);
    testPlacedClipWithId(material);
    testDuplicateAudioTrack(material);
    testDuplicateMidiAndGroup(material);
    testRefusals(material);
    std::printf("\n%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
