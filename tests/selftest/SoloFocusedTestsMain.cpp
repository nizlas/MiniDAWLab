// =============================================================================
// SoloFocusedTests — focused, deterministic tests for the Solo listening layer (spec §1–§8)
// =============================================================================
//
// Covers, with PRODUCTION code only (no audio device, no window, no VST3 hosting):
//   • `Session` solo commands: five separate sets (temporary + four memories), the current-set
//     rule, the Mute lock in the COMMAND path, Master/stale refusals, no copying on switches.
//   • `solo_mute_view::deriveSoloMuteView`: downstream closure (main out + enabled sends), Group
//     upstream feeder closure passing stored Mute, MIDI carrier/suppression sets, Off rules,
//     stale ids never producing a false-active view, and the audio-thread helpers.
//   • Persistence: v25 `soloMemories` round trip through the production save/load entry points,
//     stored Mute flags saved while Solo is active, pre-v25 files → four empty memories,
//     malformed `soloMemories` JSON degrading safely (never a read failure, never false-active).
//   • Narrow solo-memory undo through the REAL `UndoRedoCoordinator`: one step per S edit,
//     targeting the intended memory even after switching, never touching the timeline snapshot.
//   • Track delete / snapshot restore / Duplicate Track membership rules.
//   • UI geometry: the S cell in the production `TrackHeaderView` (present on Audio / Instrument /
//     Group models, absent on Master) and the `SoloMemoryStrip` fitting at the minimum header
//     column width.
//
// Run the exe: prints one line per check; exit 0 = all green.
// =============================================================================

#include "app/UndoRedoCoordinator.h"
#include "domain/AudioClip.h"
#include "domain/PlacedClip.h"
#include "domain/Session.h"
#include "domain/SessionSnapshot.h"
#include "domain/Track.h"
#include "engine/SoloMuteView.h"
#include "io/ProjectFile.h"
#include "plugins/PluginInsertHost.h"
#include "transport/Transport.h"
#include "ui/SoloMemoryStrip.h"
#include "ui/TrackHeaderView.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <algorithm>
#include <cstdio>
#include <memory>
#include <optional>
#include <vector>

namespace
{
int failures = 0;
int checks = 0;

void expect(const bool condition, const juce::String& label)
{
    ++checks;
    std::printf(condition ? "[PASS] %s\n" : "[FAIL] %s\n", label.toRawUTF8());
    if (!condition)
    {
        ++failures;
    }
}

void info(const juce::String& s)
{
    std::printf("[info] %s\n", s.toRawUTF8());
}

// ---------------------------------------------------------------------------------------------
// Pure derive tests on direct snapshot fixtures (ids chosen to be stable and readable).
//   1  A1 Audio  → Group 10, send → FX 11
//   2  A2 Audio  → Master,  send → Group 10, base-MUTED
//   10 G  Group  → Master
//   11 FX Group  → Master
//   20 I  Instrument → Master
//   30 M1 Midi → instrument 20
//   31 M2 Midi → instrument 20
//   90 Master
// ---------------------------------------------------------------------------------------------
struct FixtureTweaks
{
    bool a1Muted = false;
    bool a1Off = false;
    bool a1RoutesViaFx = false; ///< A1 → FX and FX → G (feeder transitivity variant).
    bool m1Off = false;
    bool instrumentOff = false;
};

[[nodiscard]] std::shared_ptr<const SessionSnapshot> makeRoutingFixture(const FixtureTweaks t = {})
{
    std::vector<Track> tracks;
    tracks.push_back(Track(TrackId{1}, "A1", {}, 1.0f, t.a1Off, t.a1Muted, TrackKind::Audio, 0.0f,
                           t.a1RoutesViaFx ? TrackId{11} : TrackId{10},
                           { TrackSend{ TrackId{11}, 0.25f, true, 0 } }, 1, kInvalidTrackId));
    tracks.push_back(Track(TrackId{2}, "A2", {}, 1.0f, false, /*muted*/ true, TrackKind::Audio, 0.0f,
                           TrackId{90}, { TrackSend{ TrackId{10}, 0.3f, true, 0 } }, 1, kInvalidTrackId));
    tracks.push_back(Track(TrackId{10}, "G", {}, 1.0f, false, false, TrackKind::Group, 0.0f, TrackId{90},
                           {}, 1, kInvalidTrackId));
    tracks.push_back(Track(TrackId{11}, "FX", {}, 1.0f, false, false, TrackKind::Group, 0.0f,
                           t.a1RoutesViaFx ? TrackId{10} : TrackId{90}, {}, 1, kInvalidTrackId));
    tracks.push_back(Track(TrackId{20}, "I", {}, 1.0f, t.instrumentOff, false, TrackKind::Instrument, 0.0f,
                           TrackId{90}, {}, 1, kInvalidTrackId));
    tracks.push_back(Track(TrackId{30}, "M1", {}, 1.0f, t.m1Off, false, TrackKind::Midi, 0.0f,
                           kInvalidTrackId, {}, 1, TrackId{20}));
    tracks.push_back(Track(TrackId{31}, "M2", {}, 1.0f, false, false, TrackKind::Midi, 0.0f,
                           kInvalidTrackId, {}, 1, TrackId{20}));
    tracks.push_back(Track(TrackId{90}, "Stereo Out", {}, 1.0f, false, false, TrackKind::Master, 0.0f,
                           kInvalidTrackId, {}, 1, kInvalidTrackId));
    return SessionSnapshot::withTracks(std::move(tracks));
}

[[nodiscard]] SoloTrackAudioDecision decisionOf(const SoloMuteView& v, const TrackId id)
{
    for (const auto& [tid, d] : v.audioDecisions)
    {
        if (tid == id)
        {
            return d;
        }
    }
    return SoloTrackAudioDecision::ForcedSilent;
}

[[nodiscard]] const Track& trackOf(const SessionSnapshot& snap, const TrackId id)
{
    return snap.getTrack(snap.findTrackIndexById(id));
}

void testDeriveActivation()
{
    const auto snap = makeRoutingFixture();
    using namespace solo_mute_view;
    expect(!deriveSoloMuteView(*snap, {})->soloActive, "derive: empty set -> inactive view");
    expect(!deriveSoloMuteView(*snap, { TrackId{999} })->soloActive,
           "derive: a fully stale set (deleted track id) never produces a false-active solo");
    expect(!deriveSoloMuteView(*snap, { TrackId{90} })->soloActive,
           "derive: the Master row is ignored as an explicit member (no S on Stereo Out)");
    const auto mixed = deriveSoloMuteView(*snap, { TrackId{999}, TrackId{1} });
    expect(mixed->soloActive && decisionOf(*mixed, TrackId{1}) == SoloTrackAudioDecision::ForcedAudible,
           "derive: stale ids inside a set are skipped, live ones still solo");
}

void testDeriveAudioClosure()
{
    using namespace solo_mute_view;
    const auto snap = makeRoutingFixture();
    const auto v = deriveSoloMuteView(*snap, { TrackId{1} });
    expect(v->soloActive, "derive audio: solo A1 -> view active");
    expect(decisionOf(*v, TrackId{1}) == SoloTrackAudioDecision::ForcedAudible, "derive audio: A1 ForcedAudible");
    expect(decisionOf(*v, TrackId{10}) == SoloTrackAudioDecision::ForcedAudible,
           "derive audio: A1's main-out Group passes signal (needed bus)");
    expect(decisionOf(*v, TrackId{11}) == SoloTrackAudioDecision::ForcedAudible,
           "derive audio: A1's enabled send destination passes signal");
    expect(decisionOf(*v, TrackId{90}) == SoloTrackAudioDecision::ForcedAudible,
           "derive audio: Stereo Out passes signal");
    expect(decisionOf(*v, TrackId{2}) == SoloTrackAudioDecision::ForcedSilent,
           "derive audio: the unrelated source A2 is silenced at its own strip (no leakage)");
    expect(decisionOf(*v, TrackId{20}) == SoloTrackAudioDecision::ForcedSilent,
           "derive audio: the unrelated Instrument strip is silenced");
    expect(effectiveTrackMuted(v.get(), trackOf(*snap, TrackId{2})),
           "derive audio: effectiveTrackMuted silences A2");
    expect(!effectiveTrackMuted(v.get(), trackOf(*snap, TrackId{1})), "derive audio: A1 audible");

    // Explicit solo overrides the stored Mute flag — without rewriting it.
    const auto snapMuted = makeRoutingFixture({ .a1Muted = true });
    const auto vm = deriveSoloMuteView(*snapMuted, { TrackId{1} });
    expect(!effectiveTrackMuted(vm.get(), trackOf(*snapMuted, TrackId{1})),
           "derive audio: a base-MUTED but explicitly soloed track is heard (stored flag untouched)");
    expect(trackForcedAudibleBySolo(vm.get(), TrackId{1}) && !trackForcedAudibleBySolo(vm.get(), TrackId{2}),
           "derive audio: trackForcedAudibleBySolo singles out the forced-audible rows");

    // Several solos sound together.
    const auto v2 = deriveSoloMuteView(*snap, { TrackId{1}, TrackId{2} });
    expect(decisionOf(*v2, TrackId{1}) == SoloTrackAudioDecision::ForcedAudible
               && decisionOf(*v2, TrackId{2}) == SoloTrackAudioDecision::ForcedAudible
               && !effectiveTrackMuted(v2.get(), trackOf(*snap, TrackId{2})),
           "derive audio: multiple explicit solos are audible together (base-muted A2 included)");

    // Off always wins: an Off row is never force-opened by solo.
    const auto snapOff = makeRoutingFixture({ .a1Off = true });
    const auto vo = deriveSoloMuteView(*snapOff, { TrackId{1} });
    expect(vo->soloActive && decisionOf(*vo, TrackId{1}) == SoloTrackAudioDecision::ForcedSilent,
           "derive audio: soloing an OFF row never turns it on (Off is always respected)");
}

void testDeriveGroupClosure()
{
    using namespace solo_mute_view;
    const auto snap = makeRoutingFixture();
    const auto v = deriveSoloMuteView(*snap, { TrackId{10} });
    expect(decisionOf(*v, TrackId{10}) == SoloTrackAudioDecision::ForcedAudible
               && decisionOf(*v, TrackId{90}) == SoloTrackAudioDecision::ForcedAudible,
           "derive group: the soloed Group and its downstream chain pass signal");
    expect(decisionOf(*v, TrackId{1}) == SoloTrackAudioDecision::PassStoredMute,
           "derive group: a main-out feeder keeps its stored Mute behavior");
    expect(decisionOf(*v, TrackId{2}) == SoloTrackAudioDecision::PassStoredMute,
           "derive group: a SEND-path feeder keeps its stored Mute behavior too");
    expect(effectiveTrackMuted(v.get(), trackOf(*snap, TrackId{2})),
           "derive group: a base-muted feeder stays silent — group solo means 'hear what the group plays'");
    expect(!effectiveTrackMuted(v.get(), trackOf(*snap, TrackId{1})),
           "derive group: an unmuted feeder is heard");
    expect(decisionOf(*v, TrackId{11}) == SoloTrackAudioDecision::ForcedSilent,
           "derive group: a feeder's send to an UNRELATED bus stays silent — group solo opens only "
           "the group's own picture, never side paths");

    // Transitivity: A1 → FX → G means soloing G includes BOTH hops upstream.
    const auto chain = makeRoutingFixture({ .a1RoutesViaFx = true });
    const auto vc = deriveSoloMuteView(*chain, { TrackId{10} });
    expect(decisionOf(*vc, TrackId{11}) == SoloTrackAudioDecision::PassStoredMute
               && decisionOf(*vc, TrackId{1}) == SoloTrackAudioDecision::PassStoredMute,
           "derive group: upstream feeder closure is transitive across intermediate buses");

    // An Off feeder blocks the upstream path at that row.
    const auto offFeeder = makeRoutingFixture({ .a1Off = true });
    const auto vof = deriveSoloMuteView(*offFeeder, { TrackId{10} });
    expect(decisionOf(*vof, TrackId{1}) == SoloTrackAudioDecision::ForcedSilent,
           "derive group: an OFF feeder is not opened by the upstream closure");
}

void testDeriveMidi()
{
    using namespace solo_mute_view;
    const auto snap = makeRoutingFixture();

    // Solo the MIDI lane M1: destination instrument opens as a CARRIER only.
    const auto v = deriveSoloMuteView(*snap, { TrackId{30} });
    expect(decisionOf(*v, TrackId{20}) == SoloTrackAudioDecision::ForcedAudible
               && decisionOf(*v, TrackId{90}) == SoloTrackAudioDecision::ForcedAudible,
           "derive midi: soloing a MIDI lane opens its destination instrument strip + downstream");
    expect(decisionOf(*v, TrackId{30}) == SoloTrackAudioDecision::ForcedAudible
               && trackForcedAudibleBySolo(v.get(), TrackId{30}),
           "derive midi: the soloed MIDI lane itself is forced audible (a base-muted lane still delivers)");
    expect(transportClipsSuppressedBySolo(v.get(), TrackId{20}),
           "derive midi: the carrier instrument's OWN timeline clips are suppressed (no leakage)");
    expect(routedMidiSourceSuppressedBySolo(v.get(), TrackId{31}),
           "derive midi: the OTHER MIDI source into the same instrument is suppressed");
    expect(!routedMidiSourceSuppressedBySolo(v.get(), TrackId{30}),
           "derive midi: the soloed MIDI source itself is NOT suppressed");

    // Solo the Instrument I: its own clips AND its routed MIDI sources all sound.
    const auto vi = deriveSoloMuteView(*snap, { TrackId{20} });
    expect(decisionOf(*vi, TrackId{20}) == SoloTrackAudioDecision::ForcedAudible
               && !transportClipsSuppressedBySolo(vi.get(), TrackId{20})
               && !routedMidiSourceSuppressedBySolo(vi.get(), TrackId{30})
               && !routedMidiSourceSuppressedBySolo(vi.get(), TrackId{31}),
           "derive midi: instrument solo includes its own clips and every routed MIDI source");

    // Off rules: an OFF MIDI lane is never opened; an OFF destination never passes signal.
    const auto m1Off = makeRoutingFixture({ .m1Off = true });
    const auto vmo = deriveSoloMuteView(*m1Off, { TrackId{30} });
    expect(vmo->soloActive && decisionOf(*vmo, TrackId{30}) == SoloTrackAudioDecision::ForcedSilent
               && decisionOf(*vmo, TrackId{20}) == SoloTrackAudioDecision::ForcedSilent,
           "derive midi: an OFF soloed MIDI lane opens nothing (Off is always respected)");
    const auto instOff = makeRoutingFixture({ .instrumentOff = true });
    const auto vio = deriveSoloMuteView(*instOff, { TrackId{30} });
    expect(decisionOf(*vio, TrackId{20}) == SoloTrackAudioDecision::ForcedSilent
               && !routedMidiSourceSuppressedBySolo(vio.get(), TrackId{31}),
           "derive midi: an OFF destination stays silent and needs no event-level gating");
}

// ---------------------------------------------------------------------------------------------
// Session command semantics (production commands, no snapshot poking).
// ---------------------------------------------------------------------------------------------
struct SessionFixture
{
    Session session;
    TrackId a1 = kInvalidTrackId;
    TrackId a2 = kInvalidTrackId;
    TrackId group = kInvalidTrackId;
    TrackId inst = kInvalidTrackId;
    TrackId m1 = kInvalidTrackId;
    TrackId m2 = kInvalidTrackId;
    TrackId master = kInvalidTrackId;
};

[[nodiscard]] std::unique_ptr<SessionFixture> makeSessionFixture()
{
    auto f = std::make_unique<SessionFixture>();
    Session& s = f->session;
    f->a1 = s.getActiveTrackId();
    s.addTrack();
    f->a2 = s.getActiveTrackId();
    s.addGroupTrack();
    for (int i = 0; i < s.getNumTracks(); ++i)
    {
        if (s.getTrackKindAtIndex(i) == TrackKind::Group)
        {
            f->group = s.getTrackIdAtIndex(i);
        }
    }
    f->inst = s.appendExperimentalInstrumentShellTrack("Inst").value_or(kInvalidTrackId);
    f->m1 = s.addMidiTrack().value_or(kInvalidTrackId);
    f->m2 = s.addMidiTrack().value_or(kInvalidTrackId);
    (void)s.setTrackMidiDestination(f->m1, f->inst);
    (void)s.setTrackMidiDestination(f->m2, f->inst);
    f->master = s.findCanonicalMasterTrackId();
    return f;
}

[[nodiscard]] bool storedMuted(Session& s, const TrackId id)
{
    const auto snap = s.loadSessionSnapshotForAudioThread();
    const int ix = snap->findTrackIndexById(id);
    return ix >= 0 && snap->getTrack(ix).isMuted();
}

void testSessionSoloCommands()
{
    auto f = makeSessionFixture();
    Session& s = f->session;
    expect(f->a1 != kInvalidTrackId && f->a2 != kInvalidTrackId && f->group != kInvalidTrackId
               && f->inst != kInvalidTrackId && f->m1 != kInvalidTrackId && f->m2 != kInvalidTrackId
               && f->master != kInvalidTrackId,
           "session: fixture rows created (audio x2, group, instrument, midi x2, master)");

    // Base mute BEFORE solo.
    s.setTrackMuted(f->a2, true);
    expect(storedMuted(s, f->a2), "session: A2 base-muted before solo");
    expect(!s.isSoloActive() && !s.isMuteChangeLockedBySolo(), "session: no solo -> no lock");

    expect(s.toggleTrackInCurrentSoloSet(f->a1), "session: S on A1 changes the temporary set");
    expect(s.isSoloActive() && s.isMuteChangeLockedBySolo(), "session: solo active -> Mute locked");
    expect(s.isTrackInCurrentSoloSet(f->a1) && !s.isTrackInCurrentSoloSet(f->a2),
           "session: explicit membership reads back (A1 yes, A2 no)");

    // The COMMAND path refuses Mute changes while locked — not just the button state.
    s.setTrackMuted(f->a1, true);
    s.setTrackMuted(f->a2, false);
    expect(!storedMuted(s, f->a1) && storedMuted(s, f->a2),
           "session: setTrackMuted REFUSES both directions while Solo is active (stored flags intact)");

    // Master and unknown ids are refused as members.
    expect(!s.toggleTrackInCurrentSoloSet(f->master), "session: no S membership for Stereo Out");
    expect(!s.toggleTrackInCurrentSoloSet(TrackId{987654}), "session: unknown id refused");

    // Un-solo instantly restores the normal Mute behavior.
    expect(s.toggleTrackInCurrentSoloSet(f->a1), "session: S off on A1");
    expect(!s.isSoloActive() && !s.isMuteChangeLockedBySolo(), "session: empty set -> solo inactive, lock released");
    s.setTrackMuted(f->a2, false);
    expect(!storedMuted(s, f->a2), "session: Mute edits work again after un-solo");
    s.setTrackMuted(f->a2, true); // leave A2 muted for the next assertions

    // Deleted member: effective set drops it (no ghost solo), restore brings it back.
    expect(s.toggleTrackInCurrentSoloSet(f->a2), "session: S on A2 (muted track may be explicitly soloed)");
    const auto preDelete = s.loadSessionSnapshotForAudioThread();
    s.removeTrack(f->a2);
    expect(s.getEffectiveSoloedTrackIds().empty() && !s.isSoloActive(),
           "session: deleting the only soloed track clears the EFFECTIVE set (no ghost solo, lock released)");
    s.restoreSessionSnapshotForUndo(preDelete);
    expect(s.isSoloActive() && s.getEffectiveSoloedTrackIds() == std::vector<TrackId>{ f->a2 },
           "session: restoring the pre-delete snapshot restores membership (stored id survived)");

    // Duplicate Track never auto-adds the copy to any solo set.
    const std::optional<TrackId> dup = s.duplicateTrack(f->a2);
    expect(dup.has_value() && !s.isTrackInCurrentSoloSet(*dup) && s.isTrackInCurrentSoloSet(f->a2),
           "session: Duplicate Track copies the row but NOT its solo membership");
    expect(s.toggleTrackInCurrentSoloSet(f->a2), "session: cleanup un-solo");
}

void testMemorySemantics()
{
    auto f = makeSessionFixture();
    Session& s = f->session;

    expect(s.getActiveSoloMemoryIndex() == -1, "memories: temporary set current by default");
    expect(s.toggleTrackInCurrentSoloSet(f->a1), "memories: temp set edited (A1)");

    s.setActiveSoloMemoryIndex(0);
    expect(s.getActiveSoloMemoryIndex() == 0, "memories: memory 1 selected");
    expect(s.getCurrentSoloSetTrackIds().empty() && !s.isSoloActive() && !s.isMuteChangeLockedBySolo(),
           "memories: an empty active memory restricts NOTHING (normal playback, Mute unlocked)");

    expect(s.toggleTrackInCurrentSoloSet(f->a2), "memories: S edits write the ACTIVE memory directly");
    expect(s.getSoloMemoryTrackIds(0) == std::vector<TrackId>{ f->a2 },
           "memories: memory 1 content is exactly {A2} (no auto-copy from the temporary set)");

    s.setActiveSoloMemoryIndex(1); // direct switch memory 1 -> memory 2
    expect(s.getActiveSoloMemoryIndex() == 1 && s.getCurrentSoloSetTrackIds().empty(),
           "memories: switching to memory 2 shows ITS (empty) content — nothing is copied");
    expect(s.toggleTrackInCurrentSoloSet(f->a1) && s.toggleTrackInCurrentSoloSet(f->group),
           "memories: memory 2 edited (A1 + Group)");
    expect(s.getSoloMemoryTrackIds(0) == std::vector<TrackId>{ f->a2 },
           "memories: memory 1 untouched by edits of memory 2");

    s.setActiveSoloMemoryIndex(-1); // active memory off -> back to the temporary set
    expect(s.getActiveSoloMemoryIndex() == -1 && s.getCurrentSoloSetTrackIds() == std::vector<TrackId>{ f->a1 },
           "memories: back to the temporary set — its content survived the whole trip");

    // Sanitizing setter + range guards.
    s.setSoloMemoryTrackIds(2, { f->a1, f->a1, kInvalidTrackId, f->a2 });
    expect(s.getSoloMemoryTrackIds(2) == (std::vector<TrackId>{ f->a1, f->a2 }),
           "memories: setSoloMemoryTrackIds deduplicates and drops invalid ids");
    s.setSoloMemoryTrackIds(7, { f->a1 });
    s.setActiveSoloMemoryIndex(9);
    expect(s.getActiveSoloMemoryIndex() == -1 && s.getSoloMemoryTrackIds(7).empty(),
           "memories: out-of-range memory index is a no-op everywhere");
}

// ---------------------------------------------------------------------------------------------
// Persistence (production save/load; JSON manipulated only to emulate old/broken files).
// ---------------------------------------------------------------------------------------------
void testPersistence()
{
    const juce::File dir = juce::File::getSpecialLocation(juce::File::tempDirectory)
                               .getChildFile("dal-solo-focused-tests");
    (void)dir.deleteRecursively();
    (void)dir.createDirectory();
    const juce::File proj = dir.getChildFile("Solo.dalproj");

    std::array<std::vector<TrackId>, 4> savedMemories;
    TrackId a1 = kInvalidTrackId, a2 = kInvalidTrackId;
    {
        auto f = makeSessionFixture();
        Session& s = f->session;
        Transport transport;
        a1 = f->a1;
        a2 = f->a2;
        s.setTrackMuted(f->a2, true); // base mute BEFORE solo
        s.setSoloMemoryTrackIds(0, { f->a1 });
        s.setSoloMemoryTrackIds(1, { f->a2, f->group });
        s.setSoloMemoryTrackIds(3, { f->m1 });
        for (int m = 0; m < 4; ++m)
        {
            savedMemories[(size_t)m] = s.getSoloMemoryTrackIds(m);
        }
        expect(s.toggleTrackInCurrentSoloSet(f->a1) && s.isSoloActive(),
               "persist: temporary solo ACTIVE at save time");
        expect(s.saveProjectToFile(transport, proj, 48000.0).wasOk(), "persist: project saved (v25)");
    }

    // Raw file: v25 with the four memories; the temporary set / selection is NOT in the model.
    {
        ProjectFileV1 data;
        expect(readProjectFile(proj, data).wasOk(), "persist: file reads back");
        expect(data.version == 25, "persist: writer version is 25");
        bool memoriesMatch = true;
        for (int m = 0; m < 4; ++m)
        {
            memoriesMatch = memoriesMatch && data.soloMemories[(size_t)m] == savedMemories[(size_t)m];
        }
        expect(memoriesMatch, "persist: all four memories round trip in the file model");
    }

    // Full production load into a fresh Session.
    {
        Session s2;
        Transport transport;
        juce::StringArray skipped;
        juce::String note;
        expect(s2.loadProjectFromFile(transport, proj, 48000.0, skipped, note).wasOk(),
               "persist: production load succeeds");
        bool memoriesMatch = true;
        for (int m = 0; m < 4; ++m)
        {
            memoriesMatch = memoriesMatch && s2.getSoloMemoryTrackIds(m) == savedMemories[(size_t)m];
        }
        expect(memoriesMatch, "persist: all four memories restored into the Session");
        expect(s2.getActiveSoloMemoryIndex() == -1 && s2.getCurrentSoloSetTrackIds().empty()
                   && !s2.isSoloActive(),
               "persist: memory buttons OFF and temporary set EMPTY after load (transient state reset)");
        expect(storedMuted(s2, a2) && !storedMuted(s2, a1),
               "persist: STORED Mute flags were saved, not the solo-derived audibility");
    }

    // Pre-v25 file (no `soloMemories` key): four empty memories, never an error.
    {
        juce::var root = juce::JSON::parse(proj.loadFileAsString());
        if (auto* obj = root.getDynamicObject())
        {
            obj->setProperty("version", 24);
            obj->removeProperty("soloMemories");
        }
        const juce::File old = dir.getChildFile("Old.dalproj");
        (void)old.replaceWithText(juce::JSON::toString(root));
        ProjectFileV1 data;
        expect(readProjectFile(old, data).wasOk(), "persist: pre-v25 file reads");
        expect(std::all_of(data.soloMemories.begin(), data.soloMemories.end(),
                           [](const auto& m) { return m.empty(); }),
               "persist: pre-v25 -> four empty memories");
    }

    // Malformed `soloMemories`: degrade safely — no read failure, no false-active solo.
    {
        juce::var root = juce::JSON::parse(proj.loadFileAsString());
        juce::Array<juce::var> mem0; // duplicates + garbage + a stale id
        mem0.add((int)a1);
        mem0.add((int)a1);
        mem0.add("notAnId");
        mem0.add(-5);
        mem0.add(987654);
        juce::Array<juce::var> memories;
        memories.add(mem0);
        memories.add("notAnArray");
        for (int i = 0; i < 6; ++i)
        {
            memories.add(juce::Array<juce::var>{}); // more slots than exist: extras ignored
        }
        if (auto* obj = root.getDynamicObject())
        {
            obj->setProperty("soloMemories", memories);
        }
        const juce::File bad = dir.getChildFile("Bad.dalproj");
        (void)bad.replaceWithText(juce::JSON::toString(root));
        ProjectFileV1 data;
        expect(readProjectFile(bad, data).wasOk(), "persist: malformed soloMemories never fails the read");
        expect(data.soloMemories[0] == (std::vector<TrackId>{ a1, TrackId{987654} })
                   && data.soloMemories[1].empty(),
               "persist: duplicates / non-numeric / non-positive entries dropped at read time");

        Session s3;
        Transport transport;
        juce::StringArray skipped;
        juce::String note;
        expect(s3.loadProjectFromFile(transport, bad, 48000.0, skipped, note).wasOk(),
               "persist: malformed file still loads");
        s3.setActiveSoloMemoryIndex(0);
        expect(s3.getEffectiveSoloedTrackIds() == std::vector<TrackId>{ a1 },
               "persist: the stale id is invisible in the EFFECTIVE set (no ghost solo)");

        const juce::File onlyStale = dir.getChildFile("Stale.dalproj");
        juce::Array<juce::var> staleMem;
        staleMem.add(987654);
        juce::Array<juce::var> staleMemories;
        staleMemories.add(staleMem);
        if (auto* obj = root.getDynamicObject())
        {
            obj->setProperty("soloMemories", staleMemories);
        }
        (void)onlyStale.replaceWithText(juce::JSON::toString(root));
        Session s4;
        juce::StringArray sk2;
        juce::String n2;
        expect(s4.loadProjectFromFile(transport, onlyStale, 48000.0, sk2, n2).wasOk(),
               "persist: stale-only memory loads");
        s4.setActiveSoloMemoryIndex(0);
        expect(!s4.isSoloActive() && !s4.isMuteChangeLockedBySolo(),
               "persist: a memory of ONLY stale ids never becomes falsely active");
    }

    (void)dir.deleteRecursively();
}

// ---------------------------------------------------------------------------------------------
// Narrow solo-memory undo through the REAL UndoRedoCoordinator.
// ---------------------------------------------------------------------------------------------
[[nodiscard]] bool timelinesEqualByContent(const SessionSnapshot& a, const SessionSnapshot& b)
{
    if (a.getNumTracks() != b.getNumTracks())
    {
        return false;
    }
    for (int i = 0; i < a.getNumTracks(); ++i)
    {
        const Track& ta = a.getTrack(i);
        const Track& tb = b.getTrack(i);
        if (ta.getId() != tb.getId() || ta.getPlacedClips().size() != tb.getPlacedClips().size())
        {
            return false;
        }
        for (size_t c = 0; c < ta.getPlacedClips().size(); ++c)
        {
            if (ta.getPlacedClips()[c].getId() != tb.getPlacedClips()[c].getId()
                || ta.getPlacedClips()[c].getMaterial() != tb.getPlacedClips()[c].getMaterial())
            {
                return false;
            }
        }
    }
    return true;
}

void testNarrowSoloMemoryUndo()
{
    auto f = makeSessionFixture();
    Session& s = f->session;
    PluginInsertHost pluginHost;
    int dirtyCount = 0;
    int soloRefreshCount = 0;
    UndoRedoCoordinator::Callbacks cb;
    cb.markProjectDirty = [&] { ++dirtyCount; };
    cb.refreshSoloStateAfterUndoRestore = [&] { ++soloRefreshCount; };
    UndoRedoCoordinator undo(s, pluginHost, std::move(cb));

    // A committed "take" that the narrow solo steps must never erase.
    {
        juce::AudioBuffer<float> buf(1, 4800);
        buf.clear();
        expect(s.addPlacedClipFromExistingMaterial(
                    std::make_shared<const AudioClip>(std::move(buf), 48000.0, "take"), 0, 0, 4800, f->a1)
                   .wasOk(),
               "undo: fixture take placed on A1");
    }
    const auto timelineBefore = s.loadSessionSnapshotForAudioThread();

    // Temporary-set edits are NOT undoable and NOT dirty (callers never wrap them).
    expect(s.toggleTrackInCurrentSoloSet(f->a1) && undo.undoStackSizeForDiagnostics() == 0 && dirtyCount == 0,
           "undo: a temporary-set S edit records no step and no dirty mark");
    expect(s.toggleTrackInCurrentSoloSet(f->a1), "undo: temp cleanup");

    // Edit memory 1 twice (two steps), then memory 2 once.
    s.setActiveSoloMemoryIndex(0);
    undo.executeUndoableSoloMemoryEdit("Solo memory 1", 0,
                                       [&] { return s.toggleTrackInCurrentSoloSet(f->a1); });
    expect(undo.undoStackSizeForDiagnostics() == 1 && dirtyCount == 1,
           "undo: one narrow step recorded + dirty after the first memory edit");
    undo.executeUndoableSoloMemoryEdit("Solo memory 1", 0,
                                       [&] { return s.toggleTrackInCurrentSoloSet(f->a2); });
    s.setActiveSoloMemoryIndex(1);
    undo.executeUndoableSoloMemoryEdit("Solo memory 2", 1,
                                       [&] { return s.toggleTrackInCurrentSoloSet(f->group); });
    expect(undo.undoStackSizeForDiagnostics() == 3, "undo: three steps for three memory edits");
    expect(s.getSoloMemoryTrackIds(0) == (std::vector<TrackId>{ f->a1, f->a2 })
               && s.getSoloMemoryTrackIds(1) == std::vector<TrackId>{ f->group },
           "undo: memory contents as edited");

    // A refused mutator records nothing.
    undo.executeUndoableSoloMemoryEdit("Solo memory 2", 1, [&] { return false; });
    expect(undo.undoStackSizeForDiagnostics() == 3, "undo: a refused edit records no step");

    // Undo targets the INTENDED memory even after switching the active one.
    s.setActiveSoloMemoryIndex(-1);
    undo.invokeUndoFromWindowShortcut(); // step 3: memory 2 edit
    expect(s.getSoloMemoryTrackIds(1).empty() && s.getSoloMemoryTrackIds(0).size() == 2
               && s.getActiveSoloMemoryIndex() == -1,
           "undo: the last step restores MEMORY 2 even though the temporary set is active now");
    expect(soloRefreshCount >= 1, "undo: the app refresh hook ran after the solo step");
    undo.invokeUndoFromWindowShortcut(); // step 2: memory 1 second edit
    expect(s.getSoloMemoryTrackIds(0) == std::vector<TrackId>{ f->a1 },
           "undo: second undo rolls memory 1 back by exactly one edit");
    undo.invokeRedoFromWindowShortcut();
    expect(s.getSoloMemoryTrackIds(0) == (std::vector<TrackId>{ f->a1, f->a2 }),
           "undo: redo reapplies exactly that edit");

    // Narrow means narrow: the timeline content (tracks, clips, their material) is untouched by
    // recording and by undoing/redoing solo-memory steps — the take on A1 is still there.
    const auto timelineAfter = s.loadSessionSnapshotForAudioThread();
    expect(timelinesEqualByContent(*timelineBefore, *timelineAfter),
           "undo: solo-memory steps never change the timeline content (the take on A1 survives)");
    expect(trackOf(*timelineAfter, f->a1).getPlacedClips().size() == 1,
           "undo: the committed take is still placed after undo + redo of solo steps");
}

// ---------------------------------------------------------------------------------------------
// UI geometry: the S cell in the production header + the memory strip at minimum width.
// ---------------------------------------------------------------------------------------------
void testUiGeometry()
{
    // Header: S present on a full (audio-style) row and on a mute-only Group row; never on Master.
    TrackHeaderModel audio;
    audio.name = "A1";
    audio.soloAvailable = true;
    audio.soloed = true;
    TrackHeaderModel group;
    group.name = "G";
    group.showRecordAndPowerStripCells = false;
    group.soloAvailable = true;
    TrackHeaderModel master;
    master.name = "Stereo Out";
    master.showRecordAndPowerStripCells = false;
    master.trackNameRenameEnabled = false; // soloAvailable stays false

    int soloClicks = 0;
    TrackHeaderCallbacks cbs;
    cbs.onToggleMute = [] {};
    cbs.onToggleSolo = [&] { ++soloClicks; };

    for (const int width : { TrackHeaderView::kMinimumHeaderColumnWidthPx,
                             TrackHeaderView::kDefaultHeaderColumnWidthPx })
    {
        TrackHeaderView av([m = audio] { return m; }, cbs, kInvalidTrackId, std::nullopt);
        av.setSize(width, 96);
        const juce::Rectangle<int> sb = av.getSoloButtonBounds();
        expect(!sb.isEmpty() && av.getLocalBounds().contains(sb),
               "ui: audio-row S cell fully inside the header at " + juce::String(width) + " px");
        expect(!sb.intersects(av.getMuteButtonBounds()) && !sb.intersects(av.getArmButtonBounds()),
               "ui: S cell overlaps no other strip cell at " + juce::String(width) + " px");

        TrackHeaderView gv([m = group] { return m; }, cbs, kInvalidTrackId, std::nullopt);
        gv.setSize(width, 96);
        expect(!gv.getSoloButtonBounds().isEmpty() && gv.getLocalBounds().contains(gv.getSoloButtonBounds()),
               "ui: Group row has an S cell inside the header at " + juce::String(width) + " px");

        TrackHeaderView mv([m = master] { return m; }, cbs, kInvalidTrackId, std::nullopt);
        mv.setSize(width, 96);
        expect(mv.getSoloButtonBounds().isEmpty(),
               "ui: NO S cell on Stereo Out / Master at " + juce::String(width) + " px");
    }

    // The S cell actually dispatches the solo callback.
    {
        TrackHeaderView av([m = audio] { return m; }, cbs, kInvalidTrackId, std::nullopt);
        av.setSize(TrackHeaderView::kDefaultHeaderColumnWidthPx, 96);
        av.clickSoloCellLikeMouseForStabilityTest();
        expect(soloClicks == 1, "ui: clicking the S cell invokes onToggleSolo");
    }

    // Memory strip: fits (all four full cells) at the minimum header column width.
    SoloMemoryStrip strip;
    int lastClick = -1;
    strip.activeMemoryIndexProvider = [] { return 1; };
    strip.onMemoryButtonClick = [&](const int i) { lastClick = i; };
    strip.setSize(TrackHeaderView::kMinimumHeaderColumnWidthPx, 26);
    bool allInside = true;
    bool fullSize = true;
    for (int i = 0; i < SoloMemoryStrip::kMemoryCount; ++i)
    {
        const juce::Rectangle<int> b = strip.buttonBounds(i);
        allInside = allInside && !b.isEmpty() && strip.getLocalBounds().contains(b);
        fullSize = fullSize && b.getWidth() == SoloMemoryStrip::kButtonSidePx;
        for (int j = 0; j < i; ++j)
        {
            allInside = allInside && !b.intersects(strip.buttonBounds(j));
        }
    }
    expect(allInside && fullSize,
           "ui: all four memory buttons are full-size, disjoint and inside the strip at the minimum "
           "header width (" + juce::String(TrackHeaderView::kMinimumHeaderColumnWidthPx) + " px)");
    expect(SoloMemoryStrip::kPreferredWidthPx <= TrackHeaderView::kMinimumHeaderColumnWidthPx,
           "ui: strip preferred width (caption + 4 cells) fits the minimum header column");

    // Painting with an active memory differs from painting with none (the '2' face turns red).
    const juce::Image withActive = strip.createComponentSnapshot(strip.getLocalBounds(), false, 1.0f);
    strip.activeMemoryIndexProvider = [] { return -1; };
    const juce::Image noActive = strip.createComponentSnapshot(strip.getLocalBounds(), false, 1.0f);
    bool differs = false;
    for (int y = 0; y < withActive.getHeight() && !differs; ++y)
    {
        for (int x = 0; x < withActive.getWidth() && !differs; ++x)
        {
            differs = withActive.getPixelAt(x, y) != noActive.getPixelAt(x, y);
        }
    }
    expect(differs, "ui: the active memory button is painted differently from an inactive one");
}

} // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI gui;
    info("SoloFocusedTests — Solo listening layer + four persistent memories (spec slice)");
    testDeriveActivation();
    testDeriveAudioClosure();
    testDeriveGroupClosure();
    testDeriveMidi();
    testSessionSoloCommands();
    testMemorySemantics();
    testPersistence();
    testNarrowSoloMemoryUndo();
    testUiGeometry();
    std::printf("\n%d checks, %d failure(s)\n", checks, failures);
    return failures == 0 ? 0 : 1;
}

// ---------------------------------------------------------------------------
// Link seams — Session.cpp references these instrument entry points; this harness creates an
// instrument SHELL row without any instrument controller/host, so the stubs are never executed.
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

// UndoRedoCoordinator calls the stability-invariants check after each undo/redo. The production
// implementation (StabilityInvariants.cpp) pulls in the whole PlaybackEngine; this harness
// registers no invariant providers, in which state the real function trivially passes too.
#include "diagnostics/StabilityInvariants.h"
namespace stability_invariants
{
bool runRegisteredStabilityInvariantsCheck(const juce::String&) { return true; }
} // namespace stability_invariants
