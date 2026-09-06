// =============================================================================
// MiniDAWSelftests — deterministic Level-1 unit tests (no audio device, no UI)
// =============================================================================
//
// Phase B.1 focused tests for the pure MIDI-editor logic headers:
//   * `midi_editor_text`   — window title + destination-aware status wording;
//   * octave labels        — Cubase convention via juce::MidiMessage (C-2 … G8);
//   * `midi_channel_diag`  — native/effective channel semantics reused by audition;
//   * `midi_audition`      — audition gesture timing/ownership with a FAKE clock
//                            (no real-time sleeps: `nowMs` is passed in by the test).
//
// Run: MiniDAWSelftests.exe — prints one line per check, exits 0 only when all pass.
// =============================================================================

#include "ui/experimental/ExperimentalMidiPatternPlayer.h"
#include "ui/experimental/MidiEditorAuditionModel.h"
#include "ui/experimental/MidiEditorTitleStatus.h"
#include "ui/experimental/MidiEditorToolbarLayout.h"
#include "ui/experimental/MidiCcLaneViewState.h"
#include "ui/experimental/ExperimentalMidiChannelDiagnostics.h"

#include "domain/SessionSnapshot.h"
#include "domain/TimelineDomain.h"
#include "domain/Track.h"
#include "instruments/InstrumentTrackController.h"
#include "instruments/MidiDependencyEnumeration.h"
#include "instruments/PrimarySemanticRevision.h"
#include "instruments/ProxyFingerprint.h"
#include "instruments/ProxyRenderSnapshot.h"
#include "io/InstrumentMidiClipExport.h"
#include "io/ProjectFile.h"
#include "ui/experimental/ExperimentalMidiCcAutomation.h"

// SPIKE-01 (P0/P1A validation spike) pure diagnostic helpers — see
// docs/audits/SPIKE_01_AUTHORITATIVE_PLUGIN_STATE_CAPTURE.md. Removable with the spike.
#include "diagnostics/Spike01MidiDeliveryCounters.h"
#include "diagnostics/Spike01ReportFormat.h"
#include "diagnostics/Spike01Sha256.h"

// SPIKE-02 (isolated render / tail-policy spike) pure evaluation helpers — see
// docs/audits/SPIKE_02_ISOLATED_RENDER_TAIL_LATENCY.md. Removable with the spike.
#include "instruments/ProxyOfflineSequencer.h"
#include "instruments/ProxyRenderExecutor.h"
#include "instruments/ProxyRenderTypes.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace
{
    int failures = 0;
    int checks = 0;

    void expect(const bool ok, const std::string& what)
    {
        ++checks;
        if (!ok)
        {
            ++failures;
        }
        std::printf("%s %s\n", ok ? "[ ok ]" : "[FAIL]", what.c_str());
    }

    void expectEq(const juce::String& got, const juce::String& want, const std::string& what)
    {
        expect(got == want,
               what + " (got \"" + got.toStdString() + "\", want \"" + want.toStdString() + "\")");
    }

    using midi_audition::AuditionEvent;
    using midi_audition::AuditionScheduler;

    [[nodiscard]] int countOns(const std::vector<AuditionEvent>& evs)
    {
        int n = 0;
        for (const auto& e : evs)
        {
            n += e.noteOn ? 1 : 0;
        }
        return n;
    }
    [[nodiscard]] int countOffs(const std::vector<AuditionEvent>& evs)
    {
        return (int)evs.size() - countOns(evs);
    }

    // --- Tests 1–4: window title + destination-aware status --------------------------------
    void testTitleAndStatus()
    {
        const juce::String title = midi_editor_text::buildWindowTitle("Organ Pedal");
        expect(title.contains("Organ Pedal"), "title contains the track name");
        expect(!title.contains("I2") && !title.contains("Drum hits"),
               "title has no internal 'I2' / '(Drum hits)' labels");
        expectEq(title, juce::String::fromUTF8("MIDI Editor \xe2\x80\x94 Organ Pedal"),
                 "title format is 'MIDI Editor — <track>'");
        expectEq(midi_editor_text::buildWindowTitle({}), "MIDI Editor", "unbound title is generic");

        midi_editor_text::BoundTrackStatus inst;
        inst.isMidiTrack = false;
        inst.instrumentLoaded = true;
        inst.instrumentName = "VB3-II";
        expectEq(midi_editor_text::buildInstrumentStatusLine(inst), "Instrument: VB3-II",
                 "instrument-track status shows its own plugin");
        inst.instrumentLoaded = false;
        inst.instrumentName.clear();
        expectEq(midi_editor_text::buildInstrumentStatusLine(inst), "No instrument loaded",
                 "instrument track without plugin says 'No instrument loaded'");

        midi_editor_text::BoundTrackStatus midi;
        midi.isMidiTrack = true;
        midi.hasDestination = true;
        midi.destinationTrackName = "Organ";
        midi.instrumentLoaded = true;
        midi.instrumentName = "VB3-II";
        expectEq(midi_editor_text::buildInstrumentStatusLine(midi),
                 juce::String::fromUTF8("MIDI To: Organ \xe2\x80\x94 VB3-II"),
                 "MIDI-track status shows its destination plugin");
        midi.hasDestination = false;
        expectEq(midi_editor_text::buildInstrumentStatusLine(midi), "No MIDI destination",
                 "MIDI To: None shows 'No MIDI destination'");
        midi.hasDestination = true;
        midi.instrumentLoaded = false;
        midi.instrumentName.clear();
        expectEq(midi_editor_text::buildInstrumentStatusLine(midi), "Organ: No instrument loaded",
                 "destination without plugin is named (distinct from source without plugin)");
    }

    // --- Tests 5–7: Cubase octave labels ----------------------------------------------------
    void testOctaveLabels()
    {
        const auto label = [](const int pitch) {
            return juce::MidiMessage::getMidiNoteName(pitch, true, true, 3);
        };
        expectEq(label(0), "C-2", "MIDI 0 labels as C-2");
        expectEq(label(12), "C-1", "MIDI 12 labels as C-1");
        expectEq(label(24), "C0", "MIDI 24 labels as C0");
        expectEq(label(36), "C1", "MIDI 36 labels as C1");
        expectEq(label(48), "C2", "MIDI 48 labels as C2");
        expectEq(label(60), "C3", "MIDI 60 labels as C3 (middle C, Cubase convention)");
        expectEq(label(72), "C4", "MIDI 72 labels as C4");
        expectEq(label(84), "C5", "MIDI 84 labels as C5");
        expectEq(label(96), "C6", "MIDI 96 labels as C6");
        expectEq(label(108), "C7", "MIDI 108 labels as C7");
        expectEq(label(120), "C8", "MIDI 120 labels as C8");
        expectEq(label(127), "G8", "MIDI 127 labels as G8");
    }

    // --- Tests 12–13: audition channel semantics --------------------------------------------
    void testChannelSemantics()
    {
        expect(midi_channel_diag::effectiveChannel(5, kTrackMidiOutputChannelAny) == 5,
               "Any (Preserve) auditions on the note's native channel");
        expect(midi_channel_diag::effectiveChannel(5, 3) == 3,
               "fixed output channel auditions on the effective (remapped) channel");
        expect(midi_channel_diag::channelForNewNotes(2) == 2,
               "key-strip audition on a fixed-2 track uses channel 2 (Organ Lower case)");
        expect(midi_channel_diag::channelForNewNotes(3) == 3,
               "key-strip audition on a fixed-3 track uses channel 3 (Organ Pedal case)");
        expect(midi_channel_diag::channelForNewNotes(kTrackMidiOutputChannelAny)
                   == kTrackMidiOutputChannelDrums,
               "key-strip audition on an Any track uses the new-note default channel");
    }

    // --- Tests 14–16, 23: arranged-note audition timing --------------------------------------
    void testArrangedTiming()
    {
        constexpr double kDefault = midi_audition::kArrangedAuditionMs;
        expect(kDefault >= 700.0 && kDefault <= 900.0,
               "default arranged audition duration is in the 700-900 ms band");

        // Quick click: Mouse Up long before the deadline must NOT stop the note early.
        {
            AuditionScheduler s;
            std::vector<AuditionEvent> evs;
            s.beginPreview(2, 60, 100, 64, 0.0, kDefault, evs);
            expect(countOns(evs) == 1 && countOffs(evs) == 0, "quick click: Note On at Mouse Down");
            s.endPreview(2, 60, 100.0); // Mouse Up at 100 ms
            evs.clear();
            s.drainDue(kDefault - 1.0, evs);
            expect(evs.empty(), "quick click: no Note Off before the default deadline");
            s.drainDue(kDefault, evs);
            expect(countOffs(evs) == 1 && !evs.empty() && evs.front().channel == 2,
                   "quick click: Note Off exactly at the default deadline, on the captured channel");
        }
        // Long hold: the note stays active past the deadline until Mouse Up.
        {
            AuditionScheduler s;
            std::vector<AuditionEvent> evs;
            s.beginPreview(1, 48, 90, 40, 0.0, kDefault, evs);
            evs.clear();
            s.drainDue(kDefault * 3.0, evs);
            expect(evs.empty(), "long hold: scheduled deadline never fires while held");
            s.endPreview(1, 48, kDefault * 3.0);
            s.drainDue(kDefault * 3.0, evs);
            expect(countOffs(evs) == 1, "long hold: Note Off immediately at Mouse Up");
            expect(!evs.empty() && evs.front().velocity == 40,
                   "Note Off carries the off-velocity captured at Note On");
        }
        // Arranged note length never enters the model: duration = max(default, hold) only.
        {
            AuditionScheduler s;
            std::vector<AuditionEvent> evs;
            s.beginPreview(1, 60, 100, 64, 0.0, kDefault, evs);
            s.endPreview(1, 60, 10.0);
            evs.clear();
            s.drainDue(1e9, evs);
            expect(countOffs(evs) == 1,
                   "audition duration is max(default, hold); arranged length is irrelevant");
        }
        // Retrigger: an older scheduled Note Off must never cut the newer preview (test 23).
        {
            AuditionScheduler s;
            std::vector<AuditionEvent> evs;
            s.beginPreview(1, 60, 100, 64, 0.0, kDefault, evs);
            s.endPreview(1, 60, 10.0); // off scheduled at kDefault
            evs.clear();
            s.beginPreview(1, 60, 100, 64, 400.0, kDefault, evs);
            expect(countOffs(evs) == 1 && countOns(evs) == 1,
                   "retrigger emits Note Off + Note On to restart the attack");
            evs.clear();
            s.drainDue(kDefault + 1.0, evs);
            expect(evs.empty(), "stale scheduled Note Off cannot cut the newer preview");
            s.endPreview(1, 60, 900.0);
            s.drainDue(400.0 + kDefault - 1.0, evs);
            expect(evs.empty(), "newer preview keeps its own full default duration");
            s.drainDue(400.0 + kDefault, evs);
            expect(countOffs(evs) == 1, "newer preview releases at its own deadline");
        }
    }

    // --- Tests 17–20: key-strip (piano key / drum row) exact timing ---------------------------
    void testKeyStripTiming()
    {
        // Very short click: equally short note (no minimum duration).
        {
            AuditionScheduler s;
            std::vector<AuditionEvent> evs;
            s.beginPreview(2, 36, 77, 23, 0.0, 0.0, evs);
            expect(countOns(evs) == 1 && !evs.empty() && evs.back().velocity == 77,
                   "key strip: Note On at Mouse Down with the editor's current Vel");
            evs.clear();
            s.endPreview(2, 36, 5.0);
            s.drainDue(5.0, evs);
            expect(countOffs(evs) == 1, "key strip: Note Off exactly at Mouse Up (5 ms click)");
            expect(!evs.empty() && evs.front().velocity == 23,
                   "key strip: Note Off uses the editor's current Off value");
        }
        // Long hold: sustained until Mouse Up.
        {
            AuditionScheduler s;
            std::vector<AuditionEvent> evs;
            s.beginPreview(3, 24, 100, 64, 0.0, 0.0, evs);
            evs.clear();
            s.drainDue(5000.0, evs);
            expect(evs.empty(), "key strip: held note keeps sounding (drum-row semantics too)");
            s.endPreview(3, 24, 5000.0);
            s.drainDue(5000.0, evs);
            expect(countOffs(evs) == 1 && !evs.empty() && evs.front().channel == 3,
                   "key strip: Note Off at Mouse Up after a long hold, on the captured channel");
        }
    }

    // --- Tests 21–22, 24: ownership + cleanup -------------------------------------------------
    void testOwnershipAndCleanup()
    {
        // Destination/channel change or editor close: release everything at the captured channel.
        {
            AuditionScheduler s;
            std::vector<AuditionEvent> evs;
            s.beginPreview(2, 60, 100, 64, 0.0, midi_audition::kArrangedAuditionMs, evs);
            s.beginPreview(5, 62, 100, 64, 0.0, 0.0, evs);
            evs.clear();
            s.releaseAllActive(evs);
            expect(countOffs(evs) == 2 && !s.hasActivePreviews(),
                   "teardown releases every active preview (close / reroute / channel change)");
            bool sawCh2 = false;
            bool sawCh5 = false;
            for (const auto& e : evs)
            {
                sawCh2 = sawCh2 || e.channel == 2;
                sawCh5 = sawCh5 || e.channel == 5;
            }
            expect(sawCh2 && sawCh5, "releases return to the channels captured at Note On");
        }
        // Focus loss: only held previews are cut; scheduled quick-click tails survive.
        {
            AuditionScheduler s;
            std::vector<AuditionEvent> evs;
            s.beginPreview(1, 60, 100, 64, 0.0, midi_audition::kArrangedAuditionMs, evs);
            s.endPreview(1, 60, 10.0); // quick-click tail (scheduled safe Note Off)
            s.beginPreview(1, 62, 100, 64, 0.0, 0.0, evs); // held key
            evs.clear();
            s.releaseHeldActive(evs);
            expect(countOffs(evs) == 1 && !evs.empty() && evs.front().pitch == 62,
                   "focus loss releases only the held preview");
            evs.clear();
            s.drainDue(midi_audition::kArrangedAuditionMs, evs);
            expect(countOffs(evs) == 1 && !evs.empty() && evs.front().pitch == 60,
                   "quick-click tail still ends via its scheduled Note Off (not stuck)");
        }
        // Preview offs are keyed: a preview on one pitch/channel can never emit an off for another
        // (model-level guarantee behind 'transport notes are not killed by preview cleanup').
        {
            AuditionScheduler s;
            std::vector<AuditionEvent> evs;
            s.beginPreview(1, 60, 100, 64, 0.0, 0.0, evs);
            evs.clear();
            s.endPreview(1, 61, 10.0); // wrong pitch: ignored
            s.endPreview(2, 60, 10.0); // wrong channel: ignored
            s.drainDue(1e9, evs);
            expect(evs.empty(), "preview Note Off is keyed to its own (channel, pitch) identity");
            s.endPreview(1, 60, 20.0);
            s.drainDue(20.0, evs);
            expect(countOffs(evs) == 1, "the matching key still releases normally");
        }
    }
    // =========================================================================================
    // Stage C — MIDI export honors the source track's effective channel
    // =========================================================================================

    /// One parsed channel-voice note event from the produced SMF bytes.
    struct ParsedNote
    {
        int channel = 0;
        int pitch = -1;
        int velocity = 0;
        std::int64_t tick = -1;
        bool isOn = false;
    };

    /// Builds the SMF, serializes it to real bytes, re-parses those bytes and returns the
    /// note events — tests verify actual event channels in the produced MIDI data, not
    /// helper return values.
    [[nodiscard]] std::vector<ParsedNote> exportAndParseNotes(const InstrumentMidiClip& clip,
                                                              const int sourceTrackChannel)
    {
        juce::MidiFile built;
        const auto r = buildInstrumentMidiClipMidiFile(clip, sourceTrackChannel, 48000.0, built);
        std::vector<ParsedNote> out;
        if (!r.ok)
        {
            return out;
        }
        juce::MemoryOutputStream bytes;
        if (!built.writeTo(bytes, 1))
        {
            return out;
        }
        juce::MemoryInputStream in(bytes.getData(), bytes.getDataSize(), false);
        juce::MidiFile parsed;
        if (!parsed.readFrom(in))
        {
            return out;
        }
        for (int t = 0; t < parsed.getNumTracks(); ++t)
        {
            const juce::MidiMessageSequence* seq = parsed.getTrack(t);
            if (seq == nullptr)
            {
                continue;
            }
            for (int i = 0; i < seq->getNumEvents(); ++i)
            {
                const juce::MidiMessage& m = seq->getEventPointer(i)->message;
                if (m.isNoteOn() || m.isNoteOff())
                {
                    ParsedNote p;
                    p.channel = m.getChannel();
                    p.pitch = m.getNoteNumber();
                    p.velocity = (int)m.getVelocity();
                    p.tick = (std::int64_t)std::llround(m.getTimeStamp());
                    p.isOn = m.isNoteOn();
                    out.push_back(p);
                }
            }
        }
        return out;
    }

    [[nodiscard]] InstrumentMidiClip makeExportTestClip()
    {
        InstrumentMidiClip clip;
        clip.name = "ExportTest";
        clip.lengthSamples = 0; // no sample-window filter: pure tick-domain export
        clip.pattern.bpm = 120.0;
        clip.pattern.ticksPerQuarter = 960;
        const auto note = [](const int pitch, const int ch, const std::int64_t start,
                             const std::int64_t dur, const int vel, const int offVel) {
            TimelineMidiNote n;
            n.midiNote = pitch;
            n.channel = (std::uint8_t)ch;
            n.startTick = start;
            n.durationTicks = dur;
            n.velocity = vel;
            n.offVelocity = offVel;
            return n;
        };
        // Mixed native channels incl. the pre-v17 legacy 10, plus boundary pitches 0 and 127.
        clip.pattern.timelineNotes.push_back(note(0, 2, 0, 480, 100, 64));
        clip.pattern.timelineNotes.push_back(note(60, 5, 960, 240, 90, 40));
        clip.pattern.timelineNotes.push_back(note(127, 10, 1920, 960, 127, 0));
        return clip;
    }

    void testExportEffectiveChannel()
    {
        const InstrumentMidiClip clip = makeExportTestClip();
        const std::vector<int> nativeBefore = { 2, 5, 10 };

        // 1. `Any (Preserve)` exports the mixed native channels unchanged.
        {
            const auto evs = exportAndParseNotes(clip, kTrackMidiOutputChannelAny);
            expect(evs.size() == 6U, "Any (Preserve): 3 notes export as 3 on + 3 off events");
            bool ok = true;
            const auto chForPitch = [](const int pitch) { return pitch == 0 ? 2 : (pitch == 60 ? 5 : 10); };
            for (const auto& e : evs)
            {
                ok = ok && e.channel == chForPitch(e.pitch);
            }
            expect(ok, "Any (Preserve) exports each note's native channel (2/5/10 mixture kept)");
        }
        // 2./3. Fixed channels export the effective channel on every event.
        for (const int fixed : { 1, 3 })
        {
            const auto evs = exportAndParseNotes(clip, fixed);
            bool allFixed = !evs.empty();
            for (const auto& e : evs)
            {
                allFixed = allFixed && e.channel == fixed;
            }
            expect(allFixed,
                   "fixed channel " + std::to_string(fixed)
                       + " exports every Note On/Off on channel " + std::to_string(fixed));
        }
        // 4. Note On and its matching Note Off share one effective channel (pairs by pitch).
        {
            const auto evs = exportAndParseNotes(clip, kTrackMidiOutputChannelAny);
            bool paired = !evs.empty();
            for (const auto& on : evs)
            {
                if (!on.isOn)
                {
                    continue;
                }
                bool foundOff = false;
                for (const auto& off : evs)
                {
                    foundOff = foundOff
                               || (!off.isOn && off.pitch == on.pitch && off.channel == on.channel);
                }
                paired = paired && foundOff;
            }
            expect(paired, "each Note On has a matching Note Off on the same effective channel");
        }
        // 5./6. The exporter honors exactly the given SOURCE track channel — there is no
        // destination input at all, so a routed Midi row can never inherit its destination's
        // channel here. (The UI passes the bound source track's setting, resolved by TrackId.)
        {
            const auto evs = exportAndParseNotes(clip, 7);
            bool all7 = !evs.empty();
            for (const auto& e : evs)
            {
                all7 = all7 && e.channel == 7;
            }
            expect(all7,
                   "exporter uses the given source-track channel (instrument and MIDI-only rows "
                   "share the path; no destination parameter exists)");
        }
        // 7. Export never mutates the stored notes (native channels, pitch, timing, velocities).
        {
            InstrumentMidiClip mutableClip = makeExportTestClip();
            juce::MidiFile built;
            (void)buildInstrumentMidiClipMidiFile(mutableClip, 1, 48000.0, built);
            bool untouched = mutableClip.pattern.timelineNotes.size() == 3U;
            for (size_t i = 0; untouched && i < mutableClip.pattern.timelineNotes.size(); ++i)
            {
                untouched = (int)mutableClip.pattern.timelineNotes[i].channel == nativeBefore[i];
            }
            expect(untouched, "fixed-channel export does not mutate stored native channels");
        }
        // 9./10. Pitch, tick timing, velocity and off-velocity survive exactly, incl. 0 and 127.
        {
            const auto evs = exportAndParseNotes(clip, 1);
            const auto findOn = [&evs](const int pitch) -> const ParsedNote* {
                for (const auto& e : evs)
                {
                    if (e.isOn && e.pitch == pitch)
                    {
                        return &e;
                    }
                }
                return nullptr;
            };
            const auto findOff = [&evs](const int pitch) -> const ParsedNote* {
                for (const auto& e : evs)
                {
                    if (!e.isOn && e.pitch == pitch)
                    {
                        return &e;
                    }
                }
                return nullptr;
            };
            const ParsedNote* on0 = findOn(0);
            const ParsedNote* on127 = findOn(127);
            const ParsedNote* on60 = findOn(60);
            const ParsedNote* off60 = findOff(60);
            expect(on0 != nullptr && on0->tick == 0 && on0->velocity == 100,
                   "pitch 0 exports with exact tick 0 and velocity 100");
            expect(on127 != nullptr && on127->tick == 1920 && on127->velocity == 127,
                   "pitch 127 exports with exact tick 1920 and velocity 127");
            expect(on60 != nullptr && on60->tick == 960 && off60 != nullptr && off60->tick == 1200,
                   "note timing survives exactly (on tick 960, off tick 960+240)");
            expect(off60 != nullptr && off60->velocity == 40,
                   "stored off-velocity is written on the true Note Off event");
        }
        // 11. Non-channel export behavior unchanged: conductor meta + named note track.
        {
            juce::MidiFile built;
            const auto r = buildInstrumentMidiClipMidiFile(clip, 4, 48000.0, built);
            expect(r.ok && r.notesExported == 3, "export result reports 3 notes");
            bool sawTempo = false;
            bool sawTimeSig = false;
            bool sawTrackName = false;
            for (int t = 0; t < built.getNumTracks(); ++t)
            {
                const auto* seq = built.getTrack(t);
                for (int i = 0; seq != nullptr && i < seq->getNumEvents(); ++i)
                {
                    const auto& m = seq->getEventPointer(i)->message;
                    sawTempo = sawTempo || m.isTempoMetaEvent();
                    sawTimeSig = sawTimeSig || m.isTimeSignatureMetaEvent();
                    sawTrackName = sawTrackName || m.isTrackNameEvent();
                }
            }
            expect(sawTempo && sawTimeSig && sawTrackName,
                   "meta events (tempo, time signature, track name) are present and never remapped");
        }
    }
    // =========================================================================================
    // Stage D — MIDI CC automation: model, evaluation, event generation, export, persistence
    // =========================================================================================

    [[nodiscard]] MidiCcPoint ccPt(const std::int64_t tick, const int controller, const int value,
                                   const int channel, const MidiCcInterpolation interp)
    {
        MidiCcPoint p;
        p.startTick = tick;
        p.controller = (std::uint8_t)controller;
        p.value = (std::uint8_t)value;
        p.channel = (std::uint8_t)channel;
        p.interpolationToNext = interp;
        return p;
    }

    void testCcModelValidation()
    {
        // 7. Controller/value boundaries and clamping; 14. duplicate identity resolution.
        std::vector<MidiCcPoint> pts;
        pts.push_back(ccPt(100, 11, 127, 5, MidiCcInterpolation::hold)); // duplicate identity (1st)
        pts.push_back(ccPt(-5, 11, 64, 1, MidiCcInterpolation::hold));   // negative tick: dropped
        pts.push_back(ccPt(0, 11, 0, 1, MidiCcInterpolation::linear));   // boundary value 0
        pts.push_back(ccPt(50, 127, 127, 16, MidiCcInterpolation::hold)); // boundary controller 127
        pts.push_back(ccPt(100, 11, 90, 5, MidiCcInterpolation::linear)); // duplicate identity (2nd)
        const int repaired = midi_cc::normalizePoints(pts);
        expect(repaired >= 2, "normalize reports dropped/duplicate repairs");
        expect(pts.size() == 3U, "negative tick dropped; duplicate identity collapsed to one point");
        expect(pts[0].startTick == 0 && pts[1].startTick == 50 && pts[2].startTick == 100,
               "points sorted canonically by tick");
        expect((int)pts[2].value == 90
                   && pts[2].interpolationToNext == MidiCcInterpolation::linear,
               "duplicate identity resolved deterministically: the LAST point wins");
        expect((int)pts[1].controller == 127 && (int)pts[1].channel == 16
                   && (int)pts[0].value == 0,
               "controller 127 / channel 16 / value 0 boundaries survive unclamped");

        // 8. Native channel persistence through normalize (clamped only when out of range).
        std::vector<MidiCcPoint> ch;
        ch.push_back(ccPt(0, 11, 100, 20, MidiCcInterpolation::hold)); // channel 20 → clamp 16
        (void)midi_cc::normalizePoints(ch);
        expect((int)ch[0].channel == 16, "out-of-range channel clamps to 16 (load repair)");
    }

    void testCcEvaluation()
    {
        std::vector<MidiCcPoint> pts;
        pts.push_back(ccPt(1000, 11, 100, 5, MidiCcInterpolation::hold));
        pts.push_back(ccPt(2000, 11, 20, 5, MidiCcInterpolation::linear));
        pts.push_back(ccPt(3000, 11, 120, 5, MidiCcInterpolation::hold));
        (void)midi_cc::normalizePoints(pts);

        // 12. Before the first point: no value, nothing may be emitted.
        expect(!midi_cc::valueAtTick(pts, 11, 5, 999).has_value(),
               "before the first point no value exists (no invented default)");
        // 11. Exact endpoints.
        expect(midi_cc::valueAtTick(pts, 11, 5, 1000) == std::optional<int>(100),
               "exact value at a point's own tick");
        // 9. Hold keeps the previous value until the next point.
        expect(midi_cc::valueAtTick(pts, 11, 5, 1999) == std::optional<int>(100),
               "Hold retains the previous value until the next point");
        // 10. Linear interpolates (2000→3000 goes 20→120; halfway = 70).
        expect(midi_cc::valueAtTick(pts, 11, 5, 2500) == std::optional<int>(70),
               "Linear interpolates between adjacent points (midpoint)");
        expect(midi_cc::valueAtTick(pts, 11, 5, 3000) == std::optional<int>(120),
               "Linear segment reaches its exact endpoint value");
        // 13. Held value after the final point.
        expect(midi_cc::valueAtTick(pts, 11, 5, 1000000) == std::optional<int>(120),
               "the final point's value holds forever");
        // Other streams are independent.
        expect(!midi_cc::valueAtTick(pts, 7, 5, 2500).has_value()
                   && !midi_cc::valueAtTick(pts, 11, 6, 2500).has_value(),
               "streams are keyed by (controller, channel); other streams see nothing");
    }

    void testCcEventGeneration()
    {
        // Bounded linear ramp: 0 → 127 over a huge span emits at most 128 events, exact endpoints.
        {
            std::vector<MidiCcPoint> pts;
            pts.push_back(ccPt(0, 11, 0, 1, MidiCcInterpolation::linear));
            pts.push_back(ccPt(960000, 11, 127, 1, MidiCcInterpolation::hold));
            (void)midi_cc::normalizePoints(pts);
            std::vector<midi_cc::MidiCcEvent> evs;
            midi_cc::collectCcEventsInTickRange(pts, 11, 1, 0, 960001, std::nullopt, evs);
            expect((int)evs.size() == 128,
                   "monotonic 0->127 ramp emits exactly one event per crossed integer (128), "
                   "independent of segment length (got " + std::to_string(evs.size()) + ")");
            expect(evs.front().tick == 0 && (int)evs.front().value == 0
                       && evs.back().tick <= 960000 && (int)evs.back().value == 127,
                   "ramp endpoints are exact and never overshoot");
            bool ascending = true;
            for (size_t i = 1; i < evs.size(); ++i)
            {
                ascending = ascending && evs[i].tick > evs[i - 1].tick
                            && (int)evs[i].value == (int)evs[i - 1].value + 1;
            }
            expect(ascending, "ramp events are strictly ascending in tick and value");

            // 40 (determinism): a second run yields the identical event list.
            std::vector<midi_cc::MidiCcEvent> evs2;
            midi_cc::collectCcEventsInTickRange(pts, 11, 1, 0, 960001, std::nullopt, evs2);
            bool identical = evs.size() == evs2.size();
            for (size_t i = 0; identical && i < evs.size(); ++i)
            {
                identical = evs[i].tick == evs2[i].tick && evs[i].value == evs2[i].value;
            }
            expect(identical, "linear event generation is deterministic across runs");
        }
        // Hold emits endpoints only; unchanged values are suppressed via lastSentValue.
        {
            std::vector<MidiCcPoint> pts;
            pts.push_back(ccPt(0, 11, 100, 1, MidiCcInterpolation::hold));
            pts.push_back(ccPt(5000, 11, 100, 1, MidiCcInterpolation::hold)); // same value again
            pts.push_back(ccPt(9000, 11, 40, 1, MidiCcInterpolation::hold));
            (void)midi_cc::normalizePoints(pts);
            std::vector<midi_cc::MidiCcEvent> evs;
            midi_cc::collectCcEventsInTickRange(pts, 11, 1, 0, 10000, std::nullopt, evs);
            expect(evs.size() == 2U && (int)evs[0].value == 100 && (int)evs[1].value == 40,
                   "Hold emits value changes only; an unchanged repeat point is not resent");
            // 34-style: a scan starting mid-stream with lastSent already correct emits nothing new.
            std::vector<midi_cc::MidiCcEvent> tail;
            midi_cc::collectCcEventsInTickRange(pts, 11, 1, 1, 8999, 100, tail);
            expect(tail.empty(),
                   "adjacent-range rescans with unchanged chase state do not flood repeats");
        }
    }

    void testExportWithCc()
    {
        InstrumentMidiClip clip = makeExportTestClip();
        // CC11 dip around the tick-960 note: baseline at 0, ramp handled elsewhere; here one
        // point exactly at the note start (tests same-tick ordering) plus a later restore.
        clip.pattern.ccPoints.push_back(ccPt(0, 11, 127, 5, MidiCcInterpolation::hold));
        clip.pattern.ccPoints.push_back(ccPt(960, 11, 90, 5, MidiCcInterpolation::hold));
        clip.pattern.ccPoints.push_back(ccPt(1500, 11, 127, 5, MidiCcInterpolation::hold));
        const auto storedBefore = clip.pattern.ccPoints;

        struct ParsedCc
        {
            int channel = 0;
            int controller = -1;
            int value = -1;
            std::int64_t tick = -1;
            int sequenceIndex = -1;
        };
        const auto exportParsedCc = [](const InstrumentMidiClip& c, const int trackCh,
                                       std::vector<ParsedCc>& outCc, int& noteOn960SeqIndex) {
            juce::MidiFile built;
            (void)buildInstrumentMidiClipMidiFile(c, trackCh, 48000.0, built);
            juce::MemoryOutputStream bytes;
            (void)built.writeTo(bytes, 1);
            juce::MemoryInputStream in(bytes.getData(), bytes.getDataSize(), false);
            juce::MidiFile parsed;
            (void)parsed.readFrom(in);
            outCc.clear();
            noteOn960SeqIndex = -1;
            for (int t = 0; t < parsed.getNumTracks(); ++t)
            {
                const auto* seq = parsed.getTrack(t);
                for (int i = 0; seq != nullptr && i < seq->getNumEvents(); ++i)
                {
                    const auto& m = seq->getEventPointer(i)->message;
                    if (m.isController())
                    {
                        ParsedCc p;
                        p.channel = m.getChannel();
                        p.controller = m.getControllerNumber();
                        p.value = m.getControllerValue();
                        p.tick = (std::int64_t)std::llround(m.getTimeStamp());
                        p.sequenceIndex = i;
                        outCc.push_back(p);
                    }
                    else if (m.isNoteOn() && m.getNoteNumber() == 60
                             && (std::int64_t)std::llround(m.getTimeStamp()) == 960)
                    {
                        noteOn960SeqIndex = i;
                    }
                }
            }
        };

        // 37. Number/value/tick correct; 39. Any (Preserve) exports native CC channel 5.
        {
            std::vector<ParsedCc> cc;
            int noteOnIdx = -1;
            exportParsedCc(clip, kTrackMidiOutputChannelAny, cc, noteOnIdx);
            expect(cc.size() == 3U, "3 CC points export as 3 Control Change events");
            bool exact = cc.size() == 3U;
            if (exact)
            {
                exact = cc[0].tick == 0 && cc[0].value == 127 && cc[1].tick == 960
                        && cc[1].value == 90 && cc[2].tick == 1500 && cc[2].value == 127;
                for (const auto& p : cc)
                {
                    exact = exact && p.controller == 11 && p.channel == 5;
                }
            }
            expect(exact, "CC controller/value/tick are exact; Any (Preserve) keeps native channel 5");
            // 41. Same-tick ordering: the CC at tick 960 precedes the tick-960 Note On.
            expect(noteOnIdx >= 0 && cc.size() == 3U && cc[1].sequenceIndex < noteOnIdx,
                   "CC at a note's start tick is written before that Note On");
        }
        // 38. Fixed channel exports the effective CC channel.
        {
            std::vector<ParsedCc> cc;
            int noteOnIdx = -1;
            exportParsedCc(clip, 2, cc, noteOnIdx);
            bool all2 = !cc.empty();
            for (const auto& p : cc)
            {
                all2 = all2 && p.channel == 2;
            }
            expect(all2, "fixed track channel 2 exports every CC event on channel 2");
        }
        // 42. Export never mutates stored CC data.
        {
            bool same = clip.pattern.ccPoints.size() == storedBefore.size();
            for (size_t i = 0; same && i < storedBefore.size(); ++i)
            {
                const auto& a = clip.pattern.ccPoints[i];
                const auto& b = storedBefore[i];
                same = a.startTick == b.startTick && a.controller == b.controller
                       && a.value == b.value && a.channel == b.channel
                       && a.interpolationToNext == b.interpolationToNext;
            }
            expect(same, "export leaves stored CC points byte-identical (native channel 5 kept)");
        }
    }

    void testProjectV19PersistenceAndMigration()
    {
        const juce::File dir = juce::File::getSpecialLocation(juce::File::tempDirectory)
                                   .getChildFile("MiniDAWSelftests");
        (void)dir.createDirectory();
        const juce::File v19File = dir.getChildFile("cc-roundtrip-v19.dalproj");
        const juce::File v18File = dir.getChildFile("cc-migration-v18.dalproj");

        ProjectFileV1 data;
        data.deviceSampleRateAtSave = 48000.0;
        data.nextTrackId = TrackId{ 4 };
        {
            ProjectFileTrackV1 midiRow;
            midiRow.id = TrackId{ 2 };
            midiRow.name = "MIDI 1";
            midiRow.kind = "midi";
            ProjectFileTrackV1 master;
            master.id = TrackId{ 3 };
            master.name = "Master";
            master.kind = "master";
            data.tracks.push_back(std::move(midiRow));
            data.tracks.push_back(std::move(master));
        }
        {
            ProjectFileExperimentalInstrumentTrackV1 et;
            et.trackId = TrackId{ 2 };
            et.instrumentKind = "MidiContent";
            ProjectFileExperimentalInstrumentClipV1 c;
            c.id = 7;
            c.name = "CC clip";
            ProjectFileExperimentalTimelineNoteV12 n;
            n.midiNote = 60;
            n.channel = 10;
            n.startTick = 960;
            c.timelineNotes.push_back(n);
            ProjectFileExperimentalMidiCcPointV19 p1;
            p1.startTick = 0;
            p1.controller = 11;
            p1.value = 127;
            p1.channel = 5;
            p1.interpolationToNext = 0; // hold
            ProjectFileExperimentalMidiCcPointV19 p2;
            p2.startTick = 1200;
            p2.controller = 11;
            p2.value = 90;
            p2.channel = 5;
            p2.interpolationToNext = 1; // linear
            c.ccPoints.push_back(p1);
            c.ccPoints.push_back(p2);
            et.clips.push_back(std::move(c));
            data.experimentalInstrumentTracks.push_back(std::move(et));
        }

        // 17. v19 serialization round-trip preserves controller/channel/tick/value/interpolation.
        {
            const auto wr = writeProjectFile(v19File, data);
            expect(wr.wasOk(), "v19 project with CC points writes ok");
            ProjectFileV1 back;
            const auto rr = readProjectFile(v19File, back);
            expect(rr.wasOk() && back.version == ProjectFileV1::kCurrentVersion,
                   std::string("v19 project reads back at current version")
                       + (rr.wasOk() ? "" : (std::string(" — ") + rr.getErrorMessage().toStdString())));
            bool ok = back.experimentalInstrumentTracks.size() == 1U
                      && back.experimentalInstrumentTracks[0].clips.size() == 1U;
            if (ok)
            {
                const auto& c = back.experimentalInstrumentTracks[0].clips[0];
                ok = c.ccPoints.size() == 2U && c.ccPoints[0].startTick == 0
                     && c.ccPoints[0].controller == 11 && c.ccPoints[0].value == 127
                     && c.ccPoints[0].channel == 5 && c.ccPoints[0].interpolationToNext == 0
                     && c.ccPoints[1].startTick == 1200 && c.ccPoints[1].value == 90
                     && c.ccPoints[1].interpolationToNext == 1
                     && c.timelineNotes.size() == 1U && c.timelineNotes[0].channel == 10;
            }
            expect(ok, "v19 round-trip preserves CC tick/controller/value/channel/interpolation "
                       "and leaves notes (native ch 10) untouched");
        }

        // 16. v18 → v19 migration: same JSON without ccPoints and version 18 loads cleanly with
        // no CC automation and identical note data.
        {
            juce::String json = v19File.loadFileAsString();
            expect(json.contains("\"ccPoints\""), "v19 JSON actually carries the ccPoints key");
            // Build a faithful v18 file: strip the CC array (cut from the comma that precedes the
            // key through the closing bracket) and rewind the version stamp. `midiRollFollowEnabled`
            // is always written after `ccPoints`, so the array is never the last clip property.
            const int ccPos = json.indexOf("\"ccPoints\"");
            const int cutStart = json.substring(0, ccPos).lastIndexOfChar(',');
            const int arrEnd = json.indexOf(ccPos, "]");
            juce::String v18Json = json.substring(0, cutStart) + json.substring(arrEnd + 1);
            // The writer stamps the current version (20 since P1B); rewind it to an authentic 18.
            v18Json = v18Json.replace("\"version\": " + juce::String(ProjectFileV1::kCurrentVersion),
                                      "\"version\": 18")
                          .replace("\"version\":" + juce::String(ProjectFileV1::kCurrentVersion),
                                   "\"version\":18");
            expect(v18Json.contains("\"version\": 18") && !v18Json.contains("ccPoints"),
                   "synthesized v18 file has no CC data and an authentic version stamp");
            (void)v18File.replaceWithText(v18Json);
            ProjectFileV1 old;
            const auto rr = readProjectFile(v18File, old);
            expect(rr.wasOk() && old.version == 18, "v18 project still loads (no migration error)");
            bool ok = old.experimentalInstrumentTracks.size() == 1U
                      && old.experimentalInstrumentTracks[0].clips.size() == 1U;
            if (ok)
            {
                const auto& c = old.experimentalInstrumentTracks[0].clips[0];
                ok = c.ccPoints.empty() && c.timelineNotes.size() == 1U
                     && c.timelineNotes[0].channel == 10 && c.timelineNotes[0].startTick == 960;
            }
            expect(ok, "v18 loads with empty CC automation and identical notes/channels");
        }
        (void)v19File.deleteFile();
        (void)v18File.deleteFile();
    }

    // --- P1B: schema v20, timeline reference rate (TLD-1), proxy metadata ------------------
    //
    // Steering: docs/PORTABLE_INSTRUMENTS_AND_PROXIES.md §10.1 (TLD-1), §12.2 (proxy metadata),
    // §9.3 (plugin version F1v), §18.1 (update modes); roadmap slice P1B, test T-21/T-05 parts.

    [[nodiscard]] ProjectFileV1 makeV20FixtureProject()
    {
        ProjectFileV1 data;
        data.deviceSampleRateAtSave = 48000.0;
        data.timelineSampleRate = 48000.0;
        data.nextTrackId = TrackId{ 5 };
        ProjectFileTrackV1 instRow;
        instRow.id = TrackId{ 2 };
        instRow.name = "Organ";
        instRow.kind = "instrument";
        ProjectFileTrackV1 master;
        master.id = TrackId{ 3 };
        master.name = "Master";
        master.kind = "master";
        data.tracks.push_back(std::move(instRow));
        data.tracks.push_back(std::move(master));

        ProjectFileExperimentalInstrumentTrackV1 et;
        et.trackId = TrackId{ 2 };
        et.instrumentKind = "GenericVst3";
        et.name = "Organ";
        et.hasGenericVst3Descriptor = true;
        et.genericVst3Descriptor.name = "VB3-II";
        et.genericVst3Descriptor.manufacturerName = "GSi";
        et.genericVst3Descriptor.pluginFormatName = "VST3";
        et.genericVst3Descriptor.fileOrIdentifier = "C:/Plugins/VB3-II.vst3";
        et.genericVst3Descriptor.uniqueId = 0x1234abcd;
        et.genericVst3Descriptor.isInstrument = true;
        et.pluginVersion = "2.3.1";
        et.hasProxy = true;
        et.proxy.generationId = "sha256:0011aabb";
        et.proxy.fingerprintSchemaVersion = 1;
        et.proxy.fingerprintAlgorithmId = 1;
        et.proxy.channels = 2;
        et.proxy.relativePath = "InstrumentProxies/track_2_sha256-0011aabb.wav";
        et.proxy.sampleRate = 48000.0;
        et.proxy.lengthSamples = 480000;
        et.proxy.pluginLatencySamples = 256;
        et.proxy.latencyPolicyVersion = 1;
        et.proxy.tailPolicyVersion = 1;
        et.proxy.renderPolicyVersion = 1;
        et.proxy.proxyFormatVersion = 1;
        et.proxy.renderedUtc = "2026-09-06T12:00:00Z";
        et.proxyUpdateMode = "onSave";
        ProjectFileExperimentalInstrumentClipV1 c;
        c.id = 7;
        c.name = "Organ clip";
        c.startSamples = 96000;
        c.timelineAnchorSamples.emplace(96000);
        c.lengthSamples = 48000;
        ProjectFileExperimentalTimelineNoteV12 n;
        n.midiNote = 60;
        n.channel = 1;
        n.startTick = 0;
        c.timelineNotes.push_back(n);
        et.clips.push_back(std::move(c));
        data.experimentalInstrumentTracks.push_back(std::move(et));
        return data;
    }

    void testProjectV20SchemaRoundTripAndV19Migration()
    {
        const juce::File dir = juce::File::getSpecialLocation(juce::File::tempDirectory)
                                   .getChildFile("MiniDAWSelftests");
        (void)dir.createDirectory();
        const juce::File v20File = dir.getChildFile("p1b-v20.dalproj");
        const juce::File v19File = dir.getChildFile("p1b-v19.dalproj");
        const juce::File malformedFile = dir.getChildFile("p1b-malformed.dalproj");

        // v20 write → read round-trip of every new field.
        const ProjectFileV1 data = makeV20FixtureProject();
        {
            const auto wr = writeProjectFile(v20File, data);
            expect(wr.wasOk(), "p1b: v20 project with proxy metadata writes ok");
            ProjectFileV1 back;
            const auto rr = readProjectFile(v20File, back);
            expect(rr.wasOk() && back.version == 20, "p1b: v20 project reads back as version 20");
            expect(back.timelineSampleRate == 48000.0,
                   "p1b: timeline reference rate round-trips exactly");
            bool ok = back.experimentalInstrumentTracks.size() == 1U;
            if (ok)
            {
                const auto& et = back.experimentalInstrumentTracks[0];
                ok = et.pluginVersion == "2.3.1" && et.hasProxy
                     && et.proxy.generationId == "sha256:0011aabb"
                     && et.proxy.relativePath == "InstrumentProxies/track_2_sha256-0011aabb.wav"
                     && et.proxy.sampleRate == 48000.0 && et.proxy.lengthSamples == 480000
                     && et.proxy.pluginLatencySamples == 256 && et.proxy.fingerprintSchemaVersion == 1
                     && et.proxy.latencyPolicyVersion == 1 && et.proxy.tailPolicyVersion == 1
                     && et.proxy.renderPolicyVersion == 1 && et.proxy.proxyFormatVersion == 1
                     && et.proxy.fingerprintAlgorithmId == 1 && et.proxy.channels == 2
                     && et.proxy.renderedUtc == "2026-09-06T12:00:00Z"
                     && et.proxyUpdateMode == "onSave";
            }
            expect(ok, "p1b: pluginVersion + full proxy metadata + update mode round-trip");
        }

        // P1D preflight keys are optional: a v20 proxy object written before them (or by another
        // tool) loads with the locked defaults (algorithm 1 = SHA-256, 2 = stereo channels).
        {
            const auto wr = writeProjectFile(v20File, data);
            juce::var root;
            const auto pr = juce::JSON::parse(v20File.loadFileAsString(), root);
            expect(wr.wasOk() && pr.wasOk() && root.getDynamicObject() != nullptr,
                   "p1d-preflight: fixture parse ok");
            if (auto* arr = root.getProperty("experimentalInstrumentTracks", {}).getArray())
            {
                for (auto& tv : *arr)
                {
                    if (auto* proxyObj = tv.getProperty("proxy", {}).getDynamicObject())
                    {
                        proxyObj->removeProperty("fingerprintAlgorithmId");
                        proxyObj->removeProperty("channels");
                    }
                }
            }
            (void)v20File.replaceWithText(juce::JSON::toString(root, true));
            ProjectFileV1 back;
            const auto rr = readProjectFile(v20File, back);
            expect(rr.wasOk() && back.experimentalInstrumentTracks.size() == 1U
                       && back.experimentalInstrumentTracks[0].hasProxy
                       && back.experimentalInstrumentTracks[0].proxy.fingerprintAlgorithmId == 1
                       && back.experimentalInstrumentTracks[0].proxy.channels == 2,
                   "p1d-preflight: absent fingerprintAlgorithmId/channels load as defaults");
        }

        // Reference-rate stability: a v20 file whose device rate differs keeps its own reference.
        {
            ProjectFileV1 crossRate = makeV20FixtureProject();
            crossRate.deviceSampleRateAtSave = 44100.0; // device changed; reference did not
            crossRate.timelineSampleRate = 48000.0;
            const auto wr = writeProjectFile(v20File, crossRate);
            ProjectFileV1 back;
            const auto rr = readProjectFile(v20File, back);
            expect(wr.wasOk() && rr.wasOk() && back.timelineSampleRate == 48000.0
                       && back.deviceSampleRateAtSave == 44100.0,
                   "p1b: device-rate change does not re-stamp the persisted timeline reference");
            // Sample-domain integers are untouched by the rate difference.
            expect(back.experimentalInstrumentTracks.size() == 1U
                       && back.experimentalInstrumentTracks[0].clips.size() == 1U
                       && back.experimentalInstrumentTracks[0].clips[0].startSamples == 96000,
                   "p1b: stored sample-domain integers survive a device-rate difference unchanged");
        }

        // v19 migration: strip the v20 keys, rewind the version — the reader initializes the
        // timeline reference from deviceSampleRateAtSave and defaults proxy fields. The surgery
        // works on the parsed var tree (the writer emits single-line JSON), so it is robust
        // against formatting details.
        {
            const auto wr = writeProjectFile(v20File, data);
            expect(wr.wasOk(), "p1b: fixture rewrite for v19 surgery ok");
            juce::var root;
            const auto pr = juce::JSON::parse(v20File.loadFileAsString(), root);
            juce::DynamicObject* rootObj = root.getDynamicObject();
            expect(pr.wasOk() && rootObj != nullptr && root.hasProperty("timelineSampleRate"),
                   "p1b: v20 JSON actually carries the new root key");
            rootObj->setProperty("version", 19);
            rootObj->removeProperty("timelineSampleRate");
            bool sawTrackKeys = false;
            if (const auto* exArr = root.getProperty("experimentalInstrumentTracks", {}).getArray())
            {
                for (const juce::var& tv : *exArr)
                {
                    if (juce::DynamicObject* to = tv.getDynamicObject())
                    {
                        sawTrackKeys = sawTrackKeys
                                       || (tv.hasProperty("proxy") && tv.hasProperty("pluginVersion")
                                           && tv.hasProperty("proxyUpdateMode"));
                        to->removeProperty("proxy");
                        to->removeProperty("pluginVersion");
                        to->removeProperty("proxyUpdateMode");
                    }
                }
            }
            expect(sawTrackKeys, "p1b: v20 JSON actually carries the new per-track keys");
            (void)v19File.replaceWithText(juce::JSON::toString(root, true));
            ProjectFileV1 old;
            const auto rr = readProjectFile(v19File, old);
            expect(rr.wasOk() && old.version == 19,
                   std::string("p1b: synthesized v19 file loads")
                       + (rr.wasOk() ? "" : (std::string(" — ") + rr.getErrorMessage().toStdString())));
            expect(old.timelineSampleRate == old.deviceSampleRateAtSave
                       && old.timelineSampleRate == 48000.0,
                   "p1b: v19 migration initializes the timeline reference from deviceSampleRateAtSave");
            expect(old.experimentalInstrumentTracks.size() == 1U
                       && !old.experimentalInstrumentTracks[0].hasProxy
                       && old.experimentalInstrumentTracks[0].pluginVersion.isEmpty()
                       && old.experimentalInstrumentTracks[0].proxyUpdateMode == "auto",
                   "p1b: v19 loads with no proxy, no plugin version, update mode auto");
        }

        // Malformed proxy metadata / malformed timeline rate never fail the load (PI-025).
        {
            const auto wr = writeProjectFile(v20File, data);
            expect(wr.wasOk(), "p1b: fixture rewrite for malformed surgery ok");
            juce::var root;
            const auto pr = juce::JSON::parse(v20File.loadFileAsString(), root);
            expect(pr.wasOk() && root.getDynamicObject() != nullptr, "p1b: malformed-surgery parse ok");
            root.getDynamicObject()->setProperty("timelineSampleRate", -7.5);
            if (const auto* exArr = root.getProperty("experimentalInstrumentTracks", {}).getArray())
            {
                for (const juce::var& tv : *exArr)
                {
                    if (juce::DynamicObject* to = tv.getDynamicObject())
                    {
                        if (juce::DynamicObject* po = tv.getProperty("proxy", {}).getDynamicObject())
                        {
                            po->setProperty("generationId", juce::String());
                            po->setProperty("sampleRate", "not-a-number");
                        }
                        to->setProperty("proxyUpdateMode", "bogus");
                    }
                }
            }
            (void)malformedFile.replaceWithText(juce::JSON::toString(root, true));
            ProjectFileV1 back;
            const auto rr = readProjectFile(malformedFile, back);
            expect(rr.wasOk(), "p1b: malformed proxy metadata never makes the project unloadable");
            expect(back.experimentalInstrumentTracks.size() == 1U
                       && !back.experimentalInstrumentTracks[0].hasProxy,
                   "p1b: malformed proxy object degrades to no-proxy");
            expect(back.timelineSampleRate == 48000.0,
                   "p1b: malformed timeline rate falls back to deviceSampleRateAtSave");
            expect(back.experimentalInstrumentTracks[0].proxyUpdateMode == "auto",
                   "p1b: unrecognized proxyUpdateMode repairs to auto");
        }

        // Newer versions stay rejected (unchanged contract).
        {
            juce::var root;
            const auto pr = juce::JSON::parse(v20File.loadFileAsString(), root);
            expect(pr.wasOk() && root.getDynamicObject() != nullptr, "p1b: newer-version parse ok");
            root.getDynamicObject()->setProperty("version", 21);
            (void)malformedFile.replaceWithText(juce::JSON::toString(root, true));
            ProjectFileV1 back;
            const auto rr = readProjectFile(malformedFile, back);
            expect(!rr.wasOk(), "p1b: files newer than v20 are still rejected");
        }

        // Musical-undo strip removes the v20 proxy/identity fields.
        {
            ProjectFileExperimentalInstrumentTrackV1 et = makeV20FixtureProject().experimentalInstrumentTracks[0];
            stripExperimentalInstrumentTrackPluginFieldsForUndo(et);
            expect(!et.hasProxy && et.pluginVersion.isEmpty() && et.proxyUpdateMode == "auto",
                   "p1b: undo strip clears proxy metadata, plugin version and update mode");
        }

        (void)v20File.deleteFile();
        (void)v19File.deleteFile();
        (void)malformedFile.deleteFile();
    }

    void testTimelineDomainConversion()
    {
        using timeline_domain::convertSampleCount;
        using timeline_domain::engineToReferenceSamples;
        using timeline_domain::referenceToEngineSamples;

        // Identity: equal rates are bit-exact with no rounding.
        expect(convertSampleCount(123456789, 48000.0, 48000.0) == 123456789,
               "tld: equal-rate conversion is exact identity");
        // Wall-clock preservation: one second stays one second across the domain change.
        expect(referenceToEngineSamples(48000, 48000.0, 44100.0) == 44100,
               "tld: one reference second converts to one engine second (48k->44.1k)");
        expect(referenceToEngineSamples(44100, 44100.0, 48000.0) == 48000,
               "tld: one reference second converts to one engine second (44.1k->48k)");
        // Round-trip stays within one sample (integer rounding bound).
        const std::int64_t original = 96123;
        const std::int64_t there = referenceToEngineSamples(original, 48000.0, 44100.0);
        const std::int64_t backAgain = engineToReferenceSamples(there, 44100.0, 48000.0);
        expect(std::llabs(backAgain - original) <= 1,
               "tld: reference->engine->reference round-trip is within one sample");
        // Invalid rates pass the value through unchanged (callers validate at the boundary).
        expect(convertSampleCount(500, 0.0, 48000.0) == 500
                   && convertSampleCount(500, 48000.0, -1.0) == 500,
               "tld: invalid rates never invent a rescale");
        // Rounding is nearest, not truncation.
        expect(convertSampleCount(1, 48000.0, 44100.0) == 1,
               "tld: sub-sample results round to nearest");
    }

    // --- P1C: MIDI dependency enumeration, proxy snapshot, canonical fingerprint ------------
    //
    // Steering: docs/PORTABLE_INSTRUMENTS_AND_PROXIES.md §8 (dependency + ORD-1), §10 (snapshot),
    // §11 (canonical fingerprint), §15.7 (silent generation); roadmap slice P1C; tests T-01/T-02/
    // T-05/T-06/T-07 parts. Uses the REAL SessionSnapshot/Track domain and the required Organ /
    // Organ Lower / Organ Pedal worked example (steering §8.2).

    /// The Organ fixture: instrument destination (id 1) + two routed MIDI sources (Lower ch 2,
    /// Pedal ch 3) + an unrelated instrument (id 4) with its own source (id 5) + Master (id 6).
    struct ProxyFixture
    {
        std::vector<Track> tracks;
        std::map<TrackId, std::vector<InstrumentMidiClip>> clipsByTrack;
        proxy_snapshot::BuildInputs inputs;

        [[nodiscard]] std::shared_ptr<const SessionSnapshot> session() const
        {
            std::vector<Track> copy = tracks;
            return SessionSnapshot::withTracks(std::move(copy), 0, 0, 0, ProjectMusicalTime{});
        }

        [[nodiscard]] proxy_snapshot::ProxyRenderSnapshot build(const TrackId dest = TrackId{ 1 }) const
        {
            const auto snap = session();
            return proxy_snapshot::buildProxyRenderSnapshot(
                *snap,
                dest,
                [this](const TrackId tid) {
                    std::vector<const InstrumentMidiClip*> out;
                    const auto it = clipsByTrack.find(tid);
                    if (it != clipsByTrack.end())
                    {
                        for (const auto& c : it->second)
                        {
                            out.push_back(&c);
                        }
                    }
                    return out;
                },
                inputs);
        }

        [[nodiscard]] juce::String fp(const TrackId dest = TrackId{ 1 }) const
        {
            return proxy_fingerprint::computeFingerprint(build(dest));
        }
    };

    [[nodiscard]] InstrumentMidiClip makeClip(const std::uint64_t id,
                                              const std::int64_t startSamples,
                                              std::vector<TimelineMidiNote> notes,
                                              std::vector<MidiCcPoint> cc = {})
    {
        InstrumentMidiClip c;
        c.id = id;
        c.name = "clip";
        c.startSamples = startSamples;
        c.timelineAnchorSamples = startSamples;
        c.lengthSamples = 48000;
        c.pattern.bpm = 120.0;
        c.pattern.ticksPerQuarter = 960;
        c.pattern.timelineNotes = std::move(notes);
        c.pattern.ccPoints = std::move(cc);
        return c;
    }

    [[nodiscard]] TimelineMidiNote makeNote(const int pitch,
                                            const std::int64_t startTick,
                                            const std::int64_t durationTicks,
                                            const int channel,
                                            const int velocity = 100)
    {
        TimelineMidiNote n;
        n.midiNote = pitch;
        n.velocity = velocity;
        n.offVelocity = 64;
        n.channel = (std::uint8_t)channel;
        n.startTick = startTick;
        n.durationTicks = durationTicks;
        return n;
    }

    [[nodiscard]] MidiCcPoint makeCc(const std::int64_t startTick, const int value, const int channel)
    {
        MidiCcPoint p;
        p.startTick = startTick;
        p.controller = 11;
        p.value = (std::uint8_t)value;
        p.channel = (std::uint8_t)channel;
        p.interpolationToNext = MidiCcInterpolation::hold;
        return p;
    }

    [[nodiscard]] ProxyFixture makeOrganFixture()
    {
        ProxyFixture f;
        const auto addTrack = [&f](const TrackId id, const char* name, const TrackKind kind,
                                   const int midiChannel, const TrackId midiTo) {
            f.tracks.emplace_back(id, juce::String(name), std::vector<PlacedClip>{}, 1.0f,
                                  /*off*/ false, /*muted*/ false, kind, kTrackStereoPanCenter,
                                  kInvalidTrackId, std::vector<TrackSend>{}, midiChannel, midiTo);
        };
        addTrack(TrackId{ 1 }, "Organ", TrackKind::Instrument, kTrackMidiOutputChannelAny, kInvalidTrackId);
        addTrack(TrackId{ 2 }, "Organ Lower", TrackKind::Midi, 2, TrackId{ 1 });
        addTrack(TrackId{ 3 }, "Organ Pedal", TrackKind::Midi, 3, TrackId{ 1 });
        addTrack(TrackId{ 4 }, "Other Synth", TrackKind::Instrument, kTrackMidiOutputChannelAny, kInvalidTrackId);
        addTrack(TrackId{ 5 }, "Loose Midi", TrackKind::Midi, 4, TrackId{ 4 });
        addTrack(TrackId{ 6 }, "Master", TrackKind::Master, kTrackMidiOutputChannelAny, kInvalidTrackId);

        // Organ Upper (destination-local, channel 1): two equal-time notes (ORD-1 tie-break data)
        // + CC11. Lower/Pedal sources: one note + CC each on their native channels.
        f.clipsByTrack[TrackId{ 1 }] = { makeClip(
            1, 0, { makeNote(60, 0, 960, 1), makeNote(64, 0, 960, 1) }, { makeCc(0, 127, 1) }) };
        f.clipsByTrack[TrackId{ 2 }] = { makeClip(2, 0, { makeNote(48, 480, 960, 2) },
                                                  { makeCc(480, 90, 2) }) };
        f.clipsByTrack[TrackId{ 3 }] = { makeClip(3, 0, { makeNote(36, 960, 1920, 3) },
                                                  { makeCc(960, 70, 3) }) };
        f.clipsByTrack[TrackId{ 5 }] = { makeClip(9, 0, { makeNote(72, 0, 960, 4) }) };

        f.inputs.pluginIdentity.fileOrIdentifier = "C:\\Plugins\\VB3-II.vst3";
        f.inputs.pluginIdentity.uniqueId = 0x1234abcd;
        f.inputs.pluginIdentity.deprecatedUid = 77;
        f.inputs.pluginIdentity.format = "VST3";
        f.inputs.pluginIdentity.isInstrument = true;
        f.inputs.pluginIdentity.version = "2.3.1";
        f.inputs.stateIdentity.primaryStateRevision = 7;
        f.inputs.stateIdentity.pairedWithSavedState = true;
        f.inputs.pluginStateBlob.append("state-bytes", 11);
        f.inputs.renderConfig.renderSampleRate = 48000.0;
        f.inputs.renderConfig.renderBlockSize = 512;
        f.inputs.renderConfig.timelineReferenceRate = 48000.0;
        f.inputs.policies = {};
        f.inputs.instrumentClassifiedHostEventDriven = false;
        return f;
    }

    void testMidiDependencyEnumeration()
    {
        const ProxyFixture f = makeOrganFixture();
        const auto snap = f.session();

        // The Organ worked example: exactly Lower + Pedal, in session order, with channels.
        const auto organ = midi_dependency::sourcesForDestination(*snap, TrackId{ 1 });
        expect(organ.size() == 2U && organ[0].trackId == TrackId{ 2 } && organ[1].trackId == TrackId{ 3 },
               "p1c: Organ enumerates Organ Lower then Organ Pedal in session order");
        expect(organ.size() == 2U && organ[0].midiOutputChannel == 2 && organ[1].midiOutputChannel == 3
                   && !organ[0].trackOff && !organ[0].muted,
               "p1c: enumeration carries source output channels and eligibility flags");

        // The unrelated destination owns only its own source.
        const auto other = midi_dependency::sourcesForDestination(*snap, TrackId{ 4 });
        expect(other.size() == 1U && other[0].trackId == TrackId{ 5 },
               "p1c: unrelated destination enumerates only its own source");

        // Non-instrument and unknown destinations are refused (one-hop model; never partial).
        expect(midi_dependency::sourcesForDestination(*snap, TrackId{ 2 }).empty(),
               "p1c: a Midi row is not a legal destination (one-hop, no transitive chains)");
        expect(midi_dependency::sourcesForDestination(*snap, TrackId{ 99 }).empty(),
               "p1c: unknown destination id enumerates nothing");

        // Cycle policy: cycles are structurally impossible (midiTo only targets Instrument rows);
        // a degenerate self-pointing Midi row must never be followed as its own destination.
        {
            ProxyFixture cyc = makeOrganFixture();
            cyc.tracks[1] = cyc.tracks[1].withMidiDestinationTrackId(TrackId{ 2 }); // self-loop row
            const auto s2 = cyc.session();
            expect(midi_dependency::sourcesForDestination(*s2, TrackId{ 2 }).empty(),
                   "p1c: degenerate self-loop Midi row is refused, never followed");
            expect(midi_dependency::sourcesForDestination(*s2, TrackId{ 1 }).size() == 1U,
                   "p1c: Organ keeps its remaining valid source after the degenerate rewire");
        }

        // Session reorder changes enumeration order (F9 order is data).
        {
            ProxyFixture re = makeOrganFixture();
            std::swap(re.tracks[1], re.tracks[2]); // Pedal now precedes Lower in session order
            const auto s3 = re.session();
            const auto reordered = midi_dependency::sourcesForDestination(*s3, TrackId{ 1 });
            expect(reordered.size() == 2U && reordered[0].trackId == TrackId{ 3 }
                       && reordered[1].trackId == TrackId{ 2 },
                   "p1c: session track reorder changes enumeration order (merge order is semantic)");
        }
    }

    void testProxySnapshotImmutabilityAndDeterminism()
    {
        const ProxyFixture f = makeOrganFixture();

        // Repeated generation: identical canonical bytes and identical hash.
        const auto snapA = f.build();
        const auto snapB = f.build();
        const juce::MemoryBlock bytesA = proxy_fingerprint::serializeCanonicalFingerprintBytes(snapA);
        const juce::MemoryBlock bytesB = proxy_fingerprint::serializeCanonicalFingerprintBytes(snapB);
        expect(bytesA == bytesB && bytesA.getSize() > 0,
               "p1c: repeated snapshot builds serialize to identical canonical bytes");
        const juce::String h1 = proxy_fingerprint::computeFingerprint(snapA);
        const juce::String h2 = proxy_fingerprint::computeFingerprint(snapB);
        expect(h1 == h2 && h1.startsWith("sha256:") && h1.length() == 7 + 64,
               "p1c: repeated fingerprint generation produces the identical sha256 hash");

        // Deep-copy isolation: mutating the live fixture after build never changes an existing
        // snapshot (no references to live containers).
        ProxyFixture mut = makeOrganFixture();
        const auto frozen = mut.build();
        const juce::String frozenFp = proxy_fingerprint::computeFingerprint(frozen);
        mut.clipsByTrack[TrackId{ 1 }][0].pattern.timelineNotes[0].midiNote = 61;
        mut.tracks[0] = mut.tracks[0].withMuted(true);
        expect(proxy_fingerprint::computeFingerprint(frozen) == frozenFp,
               "p1c: an existing snapshot is immune to later live-model edits (deep copy)");
        expect(mut.fp() != frozenFp, "p1c: a rebuilt snapshot naturally sees the edit");

        // Snapshot structure sanity: destination first, sources in session order, contents copied.
        expect(frozen.destinationClips.size() == 1U && frozen.sources.size() == 2U
                   && frozen.sources[0].trackId == TrackId{ 2 } && frozen.sources[1].trackId == TrackId{ 3 }
                   && frozen.sources[0].clips.size() == 1U
                   && frozen.sources[0].clips[0].notes.size() == 1U,
               "p1c: snapshot holds destination content plus both sources with copied clips");
    }

    void testFingerprintRenderRelevantEdits()
    {
        const juce::String base = makeOrganFixture().fp();
        int changedCount = 0;
        const auto expectChanges = [&base, &changedCount](const ProxyFixture& f, const std::string& what) {
            const juce::String h = f.fp();
            changedCount += (h != base) ? 1 : 0;
            expect(h != base, "p1c: fingerprint changes when " + what);
        };

        // Destination-local note edits (F4).
        {
            ProxyFixture f = makeOrganFixture();
            f.clipsByTrack[TrackId{ 1 }][0].pattern.timelineNotes[0].midiNote = 62;
            expectChanges(f, "a destination note pitch changes");
        }
        {
            ProxyFixture f = makeOrganFixture();
            f.clipsByTrack[TrackId{ 1 }][0].pattern.timelineNotes[0].velocity = 40;
            expectChanges(f, "a destination note velocity changes");
        }
        {
            ProxyFixture f = makeOrganFixture();
            f.clipsByTrack[TrackId{ 1 }][0].pattern.timelineNotes[0].durationTicks = 480;
            expectChanges(f, "a destination note duration changes");
        }
        {
            ProxyFixture f = makeOrganFixture();
            f.clipsByTrack[TrackId{ 1 }][0].pattern.timelineNotes[0].channel = 5;
            expectChanges(f, "a destination note NATIVE channel changes");
        }
        // ORD-1 tie-break data: swapping two equal-time stored notes changes delivery and hash.
        {
            ProxyFixture f = makeOrganFixture();
            auto& notes = f.clipsByTrack[TrackId{ 1 }][0].pattern.timelineNotes;
            std::swap(notes[0], notes[1]);
            expectChanges(f, "equal-time stored note order swaps (ORD-1 tie-break is data)");
        }
        // CC edits (F5).
        {
            ProxyFixture f = makeOrganFixture();
            f.clipsByTrack[TrackId{ 1 }][0].pattern.ccPoints[0].value = 10;
            expectChanges(f, "a destination CC value changes");
        }
        {
            ProxyFixture f = makeOrganFixture();
            f.clipsByTrack[TrackId{ 3 }][0].pattern.ccPoints[0].value = 71;
            expectChanges(f, "an Organ Pedal CC value changes (source content, F7)");
        }
        // Clip conversion inputs and placement (F3).
        {
            ProxyFixture f = makeOrganFixture();
            f.clipsByTrack[TrackId{ 1 }][0].pattern.bpm = 90.0;
            expectChanges(f, "the clip bpm changes");
        }
        {
            ProxyFixture f = makeOrganFixture();
            f.clipsByTrack[TrackId{ 1 }][0].pattern.ticksPerQuarter = 480;
            expectChanges(f, "the clip ticksPerQuarter changes");
        }
        {
            ProxyFixture f = makeOrganFixture();
            f.clipsByTrack[TrackId{ 1 }][0].startSamples = 24000;
            f.clipsByTrack[TrackId{ 1 }][0].timelineAnchorSamples = 24000;
            expectChanges(f, "the clip moves on the timeline (sample-domain raw integers)");
        }
        // Source content and eligibility (F7/F8): Organ Lower edits make Organ stale.
        {
            ProxyFixture f = makeOrganFixture();
            f.clipsByTrack[TrackId{ 2 }][0].pattern.timelineNotes[0].midiNote = 50;
            expectChanges(f, "an Organ Lower note changes (routed source content)");
        }
        {
            ProxyFixture f = makeOrganFixture();
            f.tracks[1] = f.tracks[1].withMuted(true);
            expectChanges(f, "a source mute toggles (F8 eligibility)");
        }
        {
            ProxyFixture f = makeOrganFixture();
            f.tracks[2] = f.tracks[2].withTrackOff(true);
            expectChanges(f, "a source power-off toggles (F8 eligibility)");
        }
        // Merge order (F9) and channels (F6 / effective channel inputs).
        {
            ProxyFixture f = makeOrganFixture();
            std::swap(f.tracks[1], f.tracks[2]);
            expectChanges(f, "session source order swaps (F9 merge order)");
        }
        {
            ProxyFixture f = makeOrganFixture();
            f.tracks[0] = f.tracks[0].withMidiOutputChannel(7);
            expectChanges(f, "the destination midiOutputChannel changes (F6)");
        }
        {
            ProxyFixture f = makeOrganFixture();
            f.tracks[1] = f.tracks[1].withMidiOutputChannel(kTrackMidiOutputChannelAny);
            expectChanges(f, "a source effective-channel remap changes (native vs effective)");
        }
        // Plugin identity and version (F1/F1v).
        {
            ProxyFixture f = makeOrganFixture();
            f.inputs.pluginIdentity.version = "2.4.0";
            expectChanges(f, "the plugin version changes (F1v)");
        }
        {
            ProxyFixture f = makeOrganFixture();
            f.inputs.pluginIdentity.uniqueId = 0x55;
            expectChanges(f, "the plugin uniqueId changes (F1)");
        }
        // Hybrid state identity (F2): revision and pairing are identity; blob bytes are not.
        {
            ProxyFixture f = makeOrganFixture();
            f.inputs.stateIdentity.primaryStateRevision = 8;
            expectChanges(f, "the host-managed state revision changes (F2 identity)");
        }
        {
            ProxyFixture f = makeOrganFixture();
            f.inputs.stateIdentity.pairedWithSavedState = false;
            expectChanges(f, "the save-pairing flag changes (F2 identity)");
        }
        // Render configuration and policies (F10–F13).
        {
            ProxyFixture f = makeOrganFixture();
            f.inputs.renderConfig.renderSampleRate = 44100.0;
            expectChanges(f, "the render sample rate changes (F11 generation identity)");
        }
        {
            ProxyFixture f = makeOrganFixture();
            f.inputs.renderConfig.renderBlockSize = 256;
            expectChanges(f, "the render block size changes (F11)");
        }
        {
            ProxyFixture f = makeOrganFixture();
            f.inputs.renderConfig.timelineReferenceRate = 44100.0;
            expectChanges(f, "the timeline reference rate changes (F11)");
        }
        {
            ProxyFixture f = makeOrganFixture();
            f.inputs.policies.tailPolicyVersion = 2;
            expectChanges(f, "the tail-policy version changes (F12)");
        }
        {
            ProxyFixture f = makeOrganFixture();
            f.inputs.policies.proxyFormatVersion = 2;
            expectChanges(f, "the proxy format version changes (F13)");
        }
        expect(changedCount >= 22, "p1c: every render-relevant edit class produced a distinct hash");

        // Blob bytes are NOT identity: same revision + different bytes ⇒ same fingerprint (§9.4).
        {
            ProxyFixture f = makeOrganFixture();
            f.inputs.pluginStateBlob.reset();
            f.inputs.pluginStateBlob.append("other-bytes!", 12);
            expect(f.fp() == base,
                   "p1c: raw state-blob byte changes never alter the fingerprint (revision is identity)");
        }
        // Equal-start clip stored order is data; different-start stored order is not (plan order).
        {
            ProxyFixture f = makeOrganFixture();
            auto& clips = f.clipsByTrack[TrackId{ 1 }];
            clips.push_back(makeClip(11, 0, { makeNote(67, 0, 960, 1) })); // same startSamples
            const juce::String ordered = f.fp();
            std::swap(clips[0], clips[1]);
            expect(f.fp() != ordered,
                   "p1c: equal-start clip stored-order swap changes the fingerprint (delivery tie-break)");
        }
        {
            ProxyFixture f = makeOrganFixture();
            auto& clips = f.clipsByTrack[TrackId{ 1 }];
            clips.push_back(makeClip(11, 96000, { makeNote(67, 0, 960, 1) })); // later start
            const juce::String ordered = f.fp();
            std::swap(clips[0], clips[1]);
            expect(f.fp() == ordered,
                   "p1c: different-start clip stored-order swap does not change the fingerprint (plan order)");
        }
    }

    void testFingerprintExclusions()
    {
        const juce::String base = makeOrganFixture().fp();

        // After the process boundary (§11.2): fader, pan, sends, audio routing, dest mute/off.
        {
            ProxyFixture f = makeOrganFixture();
            f.tracks[0] = f.tracks[0].withChannelFaderGain(0.25f).withStereoPan(-0.7f);
            expect(f.fp() == base, "p1c: destination fader/pan changes never alter the fingerprint");
        }
        {
            ProxyFixture f = makeOrganFixture();
            f.tracks[0] = f.tracks[0].withMuted(true).withTrackOff(true);
            expect(f.fp() == base,
                   "p1c: destination mute/off never alters the fingerprint (PID-006 playback gate)");
        }
        {
            ProxyFixture f = makeOrganFixture();
            f.tracks[0] = f.tracks[0].withRoutedOutputTrackId(TrackId{ 6 });
            expect(f.fp() == base, "p1c: downstream audio routing never alters the fingerprint");
        }
        {
            ProxyFixture f = makeOrganFixture();
            f.tracks[1] = f.tracks[1].withChannelFaderGain(0.1f);
            expect(f.fp() == base, "p1c: a source's audio fader never alters the fingerprint");
        }
        // Display names and view state (§11.3).
        {
            ProxyFixture f = makeOrganFixture();
            f.tracks[0] = f.tracks[0].withName("Renamed Organ");
            expect(f.fp() == base, "p1c: display names never alter the fingerprint");
        }
        {
            ProxyFixture f = makeOrganFixture();
            f.clipsByTrack[TrackId{ 1 }][0].midiRollVisibleStartSamples = 12345;
            f.clipsByTrack[TrackId{ 1 }][0].midiRollSamplesPerPixel = 99.0;
            expect(f.fp() == base, "p1c: piano-roll viewport fields never alter the fingerprint");
        }
        // Unrelated tracks (the Organ example's boundary): edits there never touch Organ.
        {
            ProxyFixture f = makeOrganFixture();
            f.clipsByTrack[TrackId{ 5 }][0].pattern.timelineNotes[0].midiNote = 40;
            f.tracks[3] = f.tracks[3].withMuted(true);
            expect(f.fp() == base, "p1c: unrelated-track edits never alter Organ's fingerprint");
        }
        // And symmetrically: Organ edits never alter the unrelated destination's fingerprint.
        {
            const juce::String otherBase = makeOrganFixture().fp(TrackId{ 4 });
            ProxyFixture f = makeOrganFixture();
            f.clipsByTrack[TrackId{ 1 }][0].pattern.timelineNotes[0].midiNote = 61;
            expect(f.fp(TrackId{ 4 }) == otherBase,
                   "p1c: Organ edits never alter the unrelated destination's fingerprint");
        }
    }

    void testSilentGenerationAndSpan()
    {
        // Populated Organ: events exist, span ends after the Pedal note (tick 960 + 1920 at
        // 120 bpm / tpq 960 → 1.5 s → 72000 reference samples from the clip anchor at 0).
        {
            const auto snap = makeOrganFixture().build();
            expect(snap.spanAndSilence.hasHostScheduledEvents,
                   "p1c: populated destination reports host-scheduled events");
            expect(snap.spanAndSilence.lastRelevantEventReferenceSample == 72000,
                   "p1c: span end lands on the last relevant event (Pedal note end, 72000 ref samples)");
            expect(!snap.spanAndSilence.silentGenerationEligible,
                   "p1c: a populated destination is never silent-generation eligible");
        }
        // A muted source no longer extends the relevant span (not delivered), but remains
        // fingerprinted content.
        {
            ProxyFixture f = makeOrganFixture();
            f.tracks[2] = f.tracks[2].withMuted(true); // Pedal held the latest event
            const auto snap = f.build();
            expect(snap.spanAndSilence.lastRelevantEventReferenceSample < 72000
                       && snap.spanAndSilence.lastRelevantEventReferenceSample > 0,
                   "p1c: muting the last-event source shortens the relevant span");
        }
        // Empty destination: conservative classification — silent fast path stays FORBIDDEN
        // while the instrument's output model is unknown (autonomous generators, §15.7).
        {
            ProxyFixture f = makeOrganFixture();
            f.clipsByTrack.clear();
            const auto snap = f.build();
            expect(!snap.spanAndSilence.hasHostScheduledEvents
                       && snap.spanAndSilence.lastRelevantEventReferenceSample == 0,
                   "p1c: empty destination reports no host-scheduled events and zero span");
            expect(!snap.spanAndSilence.silentGenerationEligible,
                   "p1c: autonomous-output uncertainty forbids the silent fast path by default");
        }
        // Only an explicit host-event-driven classification unlocks the fast path — and only
        // for an event-empty destination.
        {
            ProxyFixture f = makeOrganFixture();
            f.clipsByTrack.clear();
            f.inputs.instrumentClassifiedHostEventDriven = true;
            expect(f.build().spanAndSilence.silentGenerationEligible,
                   "p1c: explicit host-event-driven classification + empty content allows silent generation");
        }
        {
            ProxyFixture f = makeOrganFixture();
            f.inputs.instrumentClassifiedHostEventDriven = true;
            expect(!f.build().spanAndSilence.silentGenerationEligible,
                   "p1c: events present forbid the silent fast path regardless of classification");
        }
        // The eligibility inputs are fingerprinted (span/silence are identity inputs).
        {
            ProxyFixture a = makeOrganFixture();
            a.clipsByTrack.clear();
            ProxyFixture b = makeOrganFixture();
            b.clipsByTrack.clear();
            b.inputs.instrumentClassifiedHostEventDriven = true;
            expect(a.fp() != b.fp(),
                   "p1c: silent-generation classification input changes the fingerprint");
        }
    }

    void testCcNormalizationLastWins()
    {
        // Equal-(tick, controller, channel) duplicates collapse LAST-WINS in the repository-
        // canonical order (steering §8.3; the fingerprint serializes this normalized order).
        std::vector<MidiCcPoint> pts;
        pts.push_back(makeCc(960, 30, 1));
        pts.push_back(makeCc(0, 100, 1));
        pts.push_back(makeCc(960, 80, 1)); // same key as the first — later entry must win
        (void)midi_cc::normalizePoints(pts);
        expect(pts.size() == 2U, "p1c: equal-key CC duplicates collapse to one point");
        expect(pts.size() == 2U && pts[0].startTick == 0 && pts[1].startTick == 960
                   && pts[1].value == 80,
               "p1c: CC normalization is last-wins and orders by startTick");
        // Different channels at the same tick both survive (separate streams).
        std::vector<MidiCcPoint> two;
        two.push_back(makeCc(960, 30, 1));
        two.push_back(makeCc(960, 90, 2));
        (void)midi_cc::normalizePoints(two);
        expect(two.size() == 2U, "p1c: same-tick CC on different channels are distinct streams");
    }

    // --- Audition dispatch integration (Note Off regression fix) ---------------------------
    //
    // These tests drive the REAL `ExperimentalMidiPatternPlayer` — the same scheduling/dispatch
    // bridge the live MIDI editor uses — through its deterministic seam: an injected clock and a
    // MIDI-capture delivery boundary standing in for the destination host's message-thread queue
    // (`enqueueMidiMessageFromMessageThread`). They assert actual dispatched `juce::MidiMessage`s,
    // not `AuditionScheduler` return values. The instrument-track and MIDI-only cases are the
    // same player code path: the editor binds the player to whichever destination host the
    // presenter resolved (own host, or the `MIDI To` destination), so one boundary covers both.

    struct AuditionCapture
    {
        std::vector<juce::MidiMessage> messages;

        [[nodiscard]] int ons() const
        {
            int n = 0;
            for (const auto& m : messages) n += m.isNoteOn() ? 1 : 0;
            return n;
        }
        [[nodiscard]] int offs() const
        {
            int n = 0;
            for (const auto& m : messages) n += m.isNoteOff() ? 1 : 0;
            return n;
        }
        [[nodiscard]] const juce::MidiMessage* lastOff() const
        {
            for (auto it = messages.rbegin(); it != messages.rend(); ++it)
                if (it->isNoteOff()) return &(*it);
            return nullptr;
        }
        void clear() { messages.clear(); }
    };

    struct AuditionRig
    {
        AuditionCapture cap;
        double nowMs = 0.0;
        std::unique_ptr<ExperimentalMidiPatternPlayer> player;

        AuditionRig()
        {
            ExperimentalMidiPatternPlayer::TestSeams seams;
            seams.nowMs = [this] { return nowMs; };
            seams.deliver = [this](const juce::MidiMessage& m) { cap.messages.push_back(m); };
            player = std::make_unique<ExperimentalMidiPatternPlayer>(std::move(seams));
        }

        /// Advance the fake clock and run the same drain the live 4 ms UI timer runs.
        void drainAt(const double t)
        {
            nowMs = t;
            player->timerCallback();
        }
    };

    void testAuditionDispatchIntegration()
    {
        constexpr double kA = midi_audition::kArrangedAuditionMs; // 850

        // 1. Arranged-note quick click (MIDI-only boundary): immediate Note On, no premature
        //    Note Off, exactly one Note Off when 850 ms becomes due.
        {
            AuditionRig r;
            r.player->beginArrangedNotePreview(60, 100, 2, 64);
            expect(r.cap.ons() == 1 && r.cap.offs() == 0
                       && r.cap.messages[0].getChannel() == 2
                       && r.cap.messages[0].getNoteNumber() == 60,
                   "integration: arranged Mouse Down dispatches Note On ch2 p60 immediately");
            r.nowMs = 120.0;
            r.player->endNotePreview(60, 2); // quick click's Mouse Up
            r.drainAt(kA - 1.0);
            expect(r.cap.offs() == 0, "integration: no Note Off dispatched before 850 ms is due");
            r.drainAt(kA);
            expect(r.cap.offs() == 1 && r.cap.lastOff()->getChannel() == 2
                       && r.cap.lastOff()->getNoteNumber() == 60,
                   "integration: exactly one Note Off (ch2 p60) dispatched when 850 ms is due");
            r.drainAt(kA + 5000.0);
            expect(r.cap.offs() == 1, "integration: quick click never dispatches a second off");
        }

        // 2. Arranged note held past 850 ms: nothing at 850 while held; Note Off on Mouse Up.
        //    (3. — the instrument-track boundary is the identical code path and timing.)
        {
            AuditionRig r;
            r.player->beginArrangedNotePreview(64, 90, 5, 40);
            r.drainAt(kA + 200.0);
            expect(r.cap.offs() == 0, "integration: held arranged note has no off at/after 850 ms");
            r.nowMs = 1900.0;
            r.player->endNotePreview(64, 5);
            expect(r.cap.offs() == 1 && r.cap.lastOff()->getChannel() == 5,
                   "integration: held arranged note dispatches Note Off at Mouse Up (max rule)");
        }

        // 4./5. Piano-key / drum-row preview: Note On on Mouse Down, Note Off exactly on
        //       Mouse Up, no 850 ms tail even after long waits.
        {
            AuditionRig r;
            r.player->beginHeldKeyPreview(36, 110, 10, 64); // drum row, ch10
            expect(r.cap.ons() == 1 && r.cap.offs() == 0,
                   "integration: key/drum Mouse Down dispatches only the Note On");
            r.nowMs = 30.0; // a very short click
            r.player->endNotePreview(36, 10);
            expect(r.cap.offs() == 1 && r.cap.lastOff()->getChannel() == 10
                       && r.cap.lastOff()->getNoteNumber() == 36,
                   "integration: key/drum Note Off dispatched immediately on Mouse Up (30 ms note)");
            r.drainAt(5000.0);
            expect(r.cap.offs() == 1, "integration: key/drum click has no 850 ms tail event");
        }

        // 6./7. Channel pairing at the boundary: the player dispatches on the channel the roll
        //       resolved (native for Any (Preserve), effective for fixed) and the Note Off always
        //       matches its Note On channel. Channel resolution itself is covered by the
        //       midi_channel_diag tests.
        {
            AuditionRig r;
            r.player->beginArrangedNotePreview(72, 100, 7, 64); // native ch preserved by roll
            r.nowMs = 10.0;
            r.player->endNotePreview(72, 7);
            r.drainAt(kA + 10.0);
            expect(r.cap.ons() == 1 && r.cap.offs() == 1
                       && r.cap.messages[0].getChannel() == 7 && r.cap.lastOff()->getChannel() == 7,
                   "integration: Note On/Off pair matches on the resolved channel (7)");
        }

        // 8. Channel change during an active preview: teardown releases on the ORIGINAL Note On
        //    channel (the editor calls releaseAllActivePreviews before applying the new channel).
        {
            AuditionRig r;
            r.player->beginArrangedNotePreview(60, 100, 5, 64);
            r.cap.clear();
            r.player->releaseAllActivePreviews();
            expect(r.cap.offs() == 1 && r.cap.lastOff()->getChannel() == 5,
                   "integration: channel-change teardown releases on the captured ch5");
        }

        // 9. Reroute / editor rebuild during an active preview: the presenter destroys the player
        //    bound to the OLD destination; destruction must dispatch the off there.
        {
            AuditionCapture cap;
            double now = 0.0;
            {
                ExperimentalMidiPatternPlayer::TestSeams seams;
                seams.nowMs = [&now] { return now; };
                seams.deliver = [&cap](const juce::MidiMessage& m) { cap.messages.push_back(m); };
                ExperimentalMidiPatternPlayer p(std::move(seams));
                p.beginArrangedNotePreview(67, 100, 3, 64);
            } // reroute: player bound to the old destination is destroyed
            expect(cap.ons() == 1 && cap.offs() == 1 && cap.lastOff()->getChannel() == 3,
                   "integration: destroying the old-destination player releases its note there");
        }

        // 10. Retrigger same pitch/channel: restart attack; the first audition's stale scheduled
        //     off must never cut the newer preview.
        {
            AuditionRig r;
            r.player->beginArrangedNotePreview(60, 100, 1, 64);
            r.nowMs = 100.0;
            r.player->endNotePreview(60, 1); // first off scheduled for t=850
            r.nowMs = 500.0;
            r.player->beginArrangedNotePreview(60, 100, 1, 64); // retrigger
            expect(r.cap.ons() == 2 && r.cap.offs() == 1,
                   "integration: retrigger dispatches immediate off + new on (attack restart)");
            r.drainAt(900.0); // past the first note's stale deadline, retrigger still held
            expect(r.cap.offs() == 1,
                   "integration: stale scheduled off cannot cut the retriggered preview");
            r.nowMs = 1600.0;
            r.player->endNotePreview(60, 1); // due at max(1600, 500+850) = 1600 → immediate
            expect(r.cap.offs() == 2, "integration: retriggered preview releases at its own Mouse Up");
        }

        // 11. Focus loss / editor close release active notes at the boundary.
        {
            AuditionRig r;
            r.player->beginHeldKeyPreview(48, 100, 4, 64);
            r.player->releaseHeldPreviewsForFocusLoss();
            expect(r.cap.offs() == 1 && r.cap.lastOff()->getChannel() == 4,
                   "integration: focus loss dispatches Note Off for the held preview");
        }

        // 12. After a normal Note Off, later focus loss / close dispatches NO duplicate cleanup.
        {
            AuditionRig r;
            r.player->beginArrangedNotePreview(60, 100, 2, 64);
            r.nowMs = 50.0;
            r.player->endNotePreview(60, 2);
            r.drainAt(kA + 1.0);
            expect(r.cap.offs() == 1, "integration: normal off delivered before cleanup test");
            r.cap.clear();
            r.player->releaseHeldPreviewsForFocusLoss();
            r.player->releaseAllActivePreviews();
            expect(r.cap.messages.empty(),
                   "integration: finished preview gets no duplicate cleanup Note Off");
        }

        // 13. Two simultaneous previews with different pitch/channel are independently owned.
        {
            AuditionRig r;
            r.player->beginArrangedNotePreview(60, 100, 1, 64);
            r.player->beginArrangedNotePreview(64, 100, 3, 64);
            r.nowMs = 60.0;
            r.player->endNotePreview(60, 1); // release only the first
            r.drainAt(kA);
            expect(r.cap.offs() == 1 && r.cap.lastOff()->getChannel() == 1
                       && r.cap.lastOff()->getNoteNumber() == 60,
                   "integration: releasing one preview leaves the other sounding");
            r.cap.clear();
            r.player->releaseAllActivePreviews();
            expect(r.cap.offs() == 1 && r.cap.lastOff()->getChannel() == 3
                       && r.cap.lastOff()->getNoteNumber() == 64,
                   "integration: remaining preview is released independently on its own channel");
        }

        // CC chase seam: sendControllerChangeNow dispatches before a following Note On (FIFO).
        {
            AuditionRig r;
            r.player->sendControllerChangeNow(2, 11, 90);
            r.player->beginArrangedNotePreview(60, 100, 2, 64);
            expect(r.cap.messages.size() == 2U && r.cap.messages[0].isController()
                       && r.cap.messages[0].getControllerNumber() == 11
                       && r.cap.messages[1].isNoteOn(),
                   "integration: chased CC11 is dispatched before the audition Note On");
        }
    }

    // --- MIDI-editor toolbar: per-control responsive flow + Fit Drums visibility -----------
    void testToolbarLayoutAndVisibility()
    {
        namespace tb = midi_editor_toolbar;
        using tb::ToolbarItem;

        // The live editor's stable logical order and widths (Fit Drums at index 17, gapBefore on
        // the Snap toggle and the Export button as conceptual-group spacing metadata).
        const auto makeItems = [](const bool fitDrumsVisible) {
            return std::vector<ToolbarItem>{
                { 26, 0, true },  { 34, 0, true },  { 26, 0, true },  { 34, 0, true },
                { 26, 0, true },  { 78, 0, true },  { 58, 0, true },  { 52, 0, true },
                { 72, 0, true },  { 52, 12, true }, { 118, 4, true }, { 52, 0, true },
                { 72, 0, true },  { 40, 0, true },  { 104, 0, true }, { 128, 0, true },
                { 72, 0, true },  { 78, 0, fitDrumsVisible }, { 96, 12, true }, { 64, 0, true },
            };
        };
        const auto items = makeItems(true);
        const auto totalWidth = [](const std::vector<ToolbarItem>& its) {
            int total = 0;
            bool first = true;
            for (const auto& it : its)
            {
                if (!it.visible) continue;
                total += (first ? 0 : it.gapBefore) + it.width;
                first = false;
            }
            return total;
        };
        const int total = totalWidth(items);
        const auto lastVisibleWidthWithGap = items.back().width; // Remap, no gap

        // Structural invariants at a given width: original order kept across rows, every right
        // edge inside the available width, no overlap between controls on a row.
        const auto checkInvariants = [&](const int w, const std::string& tag) {
            const auto p = tb::flowLayout(w, items);
            bool orderOk = true;
            bool insideOk = true;
            bool overlapOk = true;
            int prevRow = 0;
            int prevEnd = -1;
            for (size_t i = 0; i < p.size(); ++i)
            {
                if (!p[i].placed) continue;
                if (p[i].row < prevRow) orderOk = false;              // never moves back up mid-list
                if (p[i].row > prevRow) prevEnd = -1;                  // new row restarts the cursor
                if (p[i].x < prevEnd) overlapOk = false;               // mutual order + no overlap
                if (p[i].x + p[i].width > w) insideOk = false;
                prevRow = p[i].row;
                prevEnd = p[i].x + p[i].width;
            }
            expect(orderOk, "toolbar: " + tag + " keeps the logical order across rows");
            expect(insideOk, "toolbar: " + tag + " keeps every right edge inside the width");
            expect(overlapOk, "toolbar: " + tag + " has no overlapping controls");
        };

        // Wide client: everything on one row.
        {
            const auto p = tb::flowLayout(1800, items);
            expect(tb::rowCountOf(p) == 1, "toolbar: wide width lays out one complete row");
            expect(tb::toolbarHeightForRows(1) == tb::kRowHeightPx,
                   "toolbar: one row keeps the original toolbar height");
            checkInvariants(1800, "wide layout");
        }

        // First wrap boundary: exactly-fits is one row; one pixel narrower moves ONLY the last
        // control down (per-control flow, not a group batch).
        {
            expect(tb::rowCountOf(tb::flowLayout(total, items)) == 1,
                   "toolbar: exact fit stays on one row");
            const auto p = tb::flowLayout(total - 1, items);
            expect(tb::rowCountOf(p) == 2, "toolbar: one pixel narrower uses two rows");
            int movedCount = 0;
            for (const auto& pl : p) movedCount += (pl.placed && pl.row == 1) ? 1 : 0;
            expect(movedCount == 1 && p.back().row == 1 && p.back().x == 0,
                   "toolbar: only the last control moves down at the first wrap boundary");
            checkInvariants(total - 1, "first-wrap layout");
        }

        // Progressively narrower: controls move down individually, one more at each boundary,
        // always from the right-hand end of the logical order.
        {
            const int wOneLess = total - 1;
            const int prevBoundary = total - lastVisibleWidthWithGap - items[18].gapBefore; // before Export fits
            const auto p2 = tb::flowLayout(prevBoundary - 1, items);
            int moved = 0;
            for (const auto& pl : p2) moved += (pl.placed && pl.row == 1) ? 1 : 0;
            expect(tb::rowCountOf(p2) == 2 && moved == 2,
                   "toolbar: a further reduction moves exactly the next control down");
            expect(p2[18].row == 1 && p2[19].row == 1 && p2[18].x == 0 && p2[19].x > p2[18].x,
                   "toolbar: lower-row controls retain their mutual order");
            checkInvariants(prevBoundary - 1, "two-moved layout");
            // Widening reverses the movement (pure function of width — no hysteresis).
            const auto wide = tb::flowLayout(wOneLess, items);
            int movedWide = 0;
            for (const auto& pl : wide) movedWide += (pl.placed && pl.row == 1) ? 1 : 0;
            expect(movedWide == 1, "toolbar: widening returns controls one at a time");
            expect(tb::rowCountOf(tb::flowLayout(total, items)) == 1,
                   "toolbar: full width returns everything to one row");
        }

        // Representative lower-resolution client width: no clipping, invariants hold.
        checkInvariants(1280, "1280 px layout");
        checkInvariants(900, "900 px layout");

        // Hidden Fit Drums consumes neither width nor spacing.
        {
            const auto noFit = makeItems(false);
            expect(totalWidth(noFit) == total - 78,
                   "toolbar: hiding Fit Drums removes exactly its width (no gap left behind)");
            const auto p = tb::flowLayout(total - 40, noFit);
            expect(tb::rowCountOf(p) == 1,
                   "toolbar: the reclaimed width can restore a one-row layout");
        }

        // Two-row minimum width: derived from the visible item widths/spacing; below it the flow
        // would need a third row, at it two rows suffice.
        {
            const int minW = tb::minimumWidthForRows(items, tb::kMaxRows);
            expect(tb::rowCountOf(tb::flowLayout(minW, items)) <= 2,
                   "toolbar: computed minimum width fits two rows");
            expect(minW > 0 && tb::rowCountOf(tb::flowLayout(minW - 1, items)) > 2,
                   "toolbar: one pixel below the computed minimum would need a third row");
            expect(minW <= (total + 1) / 2 + 128,
                   "toolbar: the minimum stays reasonable for low-resolution displays");
            expect(tb::toolbarHeightForRows(2) == 2 * tb::kRowHeightPx,
                   "toolbar: second row doubles the toolbar height (grid moves down)");
        }

        // Remap button: complete rendered label + padding always fits inside the button.
        {
            const int textW = 42; // representative rendered width of "Remap"
            const int w = tb::textButtonPreferredWidth(textW, 56);
            expect(w >= textW + 2 * tb::kTextButtonPadPx,
                   "toolbar: Remap width covers its full label plus padding");
            expect(tb::textButtonPreferredWidth(10, 56) == 56,
                   "toolbar: a very short label still gets the minimum button width");
        }

        // --- Vertical partition: width can never alter vertical geometry -------------------
        // The editor content rectangle (ruler + grid + velocity lane + collapsed CC strip,
        // bottom-anchored internally) is a pure function of editor height and the FINAL toolbar
        // row count — width is not even a parameter, so two one-row widths partition identically
        // and the bottom-anchored lanes (and a fitted drum view) cannot move on width-only
        // resizes.
        {
            const int editorH = 760;
            const int wideW = 1800;  // both widths keep one toolbar row
            const int narrowW = total; // exact fit: still one row, at the wrap boundary
            const int rowsWide = tb::rowCountOf(tb::flowLayout(wideW, items));
            const int rowsNarrow = tb::rowCountOf(tb::flowLayout(narrowW, items));
            expect(rowsWide == 1 && rowsNarrow == 1,
                   "vertical: both test widths keep the toolbar on one row");
            expect(tb::editorContentHeight(editorH, rowsWide)
                       == tb::editorContentHeight(editorH, rowsNarrow),
                   "vertical: two one-row widths yield identical content height (lanes cannot move)");

            // Exact partition, no unexplained remainder, for one and two rows and odd heights.
            for (const int h : { 760, 543, 81 })
            {
                for (const int r : { 1, 2 })
                {
                    expect(tb::toolbarHeightForRows(r) + tb::editorContentHeight(h, r) == h,
                           "vertical: toolbar + content exactly partition height "
                               + std::to_string(h) + " at " + std::to_string(r) + " row(s)");
                }
            }

            // One-row -> two-row transition changes the content height by exactly one row;
            // widening back restores the original geometry.
            const int rows2 = tb::rowCountOf(tb::flowLayout(total - 1, items));
            expect(rows2 == 2, "vertical: crossing the wrap boundary yields two rows");
            expect(tb::editorContentHeight(editorH, 1) - tb::editorContentHeight(editorH, rows2)
                       == tb::kRowHeightPx,
                   "vertical: the transition moves the grid down by exactly one toolbar row");
            expect(tb::editorContentHeight(editorH, tb::rowCountOf(tb::flowLayout(wideW, items)))
                       == tb::editorContentHeight(editorH, 1),
                   "vertical: widening again restores the original content height");

            // Degenerate shrink-wrap heights never go negative (roll keeps a >=1 px budget).
            expect(tb::editorContentHeight(30, 2) == 1,
                   "vertical: tiny editor heights clamp the content budget to 1 px");
        }

        // Fit Drums visibility contract.
        expect(tb::fitDrumsVisible(false, true),
               "fit drums: visible on the explicit drum instrument editor");
        expect(!tb::fitDrumsVisible(false, false),
               "fit drums: hidden on a melodic instrument track");
        expect(!tb::fitDrumsVisible(true, true) && !tb::fitDrumsVisible(true, false),
               "fit drums: hidden on a MIDI-only track regardless of labels");
        expect(tb::fitDrumsEnabled(true, true) && !tb::fitDrumsEnabled(true, false)
                   && !tb::fitDrumsEnabled(false, true),
               "fit drums: enabled only when visible and a useful row range exists");
    }

    // -------------------------------------------------------------------------------------------
    // Collapsible CC lane view state — the pure seam driving the piano roll's expand/collapse.
    // View-only by construction: ViewState carries no CC points, undo hooks or dirty flags.
    // -------------------------------------------------------------------------------------------
    void testCcLaneViewState()
    {
        namespace cl = midi_cc_lane;
        constexpr int kDefaultH = 110;   // ExperimentalPianoRollView::kCcLaneHeight
        constexpr int kRuler = 24;       // representative ruler height
        constexpr int kMinGrid = 3 * 18; // kRowHeight * 3 minimum grid budget
        constexpr int kViewH = 600;
        constexpr int kVelLane = 90;     // representative expanded velocity lane

        // 1. Initial state is collapsed and consumes zero height.
        cl::ViewState s{};
        expect(s.heightPref == 0
                   && cl::effectiveLaneHeight(s.heightPref, kViewH, kRuler, kMinGrid) == 0,
               "cc lane: initial state is collapsed with zero lane height");

        // 2. Clicking the collapsed `CC` control expands the lane at the default height.
        s = cl::reopened(s, kDefaultH);
        expect(s.heightPref == kDefaultH,
               "cc lane: reopening from the initial state uses the default height");
        const int gridCollapsed = kViewH - kRuler - kVelLane;
        const int laneH = cl::effectiveLaneHeight(s.heightPref, kViewH, kRuler, kMinGrid);
        const int gridExpanded = kViewH - kRuler - kVelLane - laneH;

        // 8./9. Note grid shrinks by exactly the lane height on expand — the vertical regions
        // still partition the fixed view height with no remainder or overlap.
        expect(laneH == kDefaultH && gridCollapsed - gridExpanded == laneH,
               "cc lane: expanding takes exactly the lane height from the note grid");
        expect(kRuler + gridExpanded + kVelLane + laneH == kViewH,
               "cc lane: expanded regions exactly fill the view height (no remainder)");

        // Simulate the user resizing the lane, then 3./4. collapsing via the header chevron.
        s.heightPref = 140;
        s = cl::collapsed(s);
        expect(s.heightPref == 0
                   && cl::effectiveLaneHeight(s.heightPref, kViewH, kRuler, kMinGrid) == 0,
               "cc lane: collapse returns to zero lane height");
        expect(kViewH - kRuler - kVelLane == gridCollapsed,
               "cc lane: collapse returns the full height to the note grid exactly");

        // Collapse is idempotent and never forgets the memo.
        const auto again = cl::collapsed(s);
        expect(again.heightPref == 0 && again.expandedMemo == 140,
               "cc lane: collapsing twice is a no-op that keeps the height memo");

        // 7. Reopening within the same editor restores the remembered runtime height,
        // and reopening while already open changes nothing.
        s = cl::reopened(s, kDefaultH);
        expect(s.heightPref == 140, "cc lane: reopen restores the remembered runtime height");
        const auto open2 = cl::reopened(s, kDefaultH);
        expect(open2.heightPref == 140, "cc lane: reopening an open lane is a no-op");

        // 5./6. View state is structurally separate from musical data: toggling the lane must not
        // change a CC point set (same pure model the editor stores in the pattern).
        {
            std::vector<MidiCcPoint> pts;
            MidiCcPoint p;
            p.startTick = 480;
            p.controller = 11;
            p.value = 96;
            p.channel = 2;
            pts.push_back(p);
            const auto before = pts;
            cl::ViewState v{};
            v = cl::reopened(v, kDefaultH);
            v = cl::collapsed(v);
            v = cl::reopened(v, kDefaultH);
            expect(pts.size() == before.size() && pts[0].startTick == before[0].startTick
                       && pts[0].controller == before[0].controller
                       && pts[0].value == before[0].value && pts[0].channel == before[0].channel,
                   "cc lane: expand/collapse leaves CC point data untouched");
        }

        // Clamps: the lane never eats the minimum grid or more than half the view.
        expect(cl::effectiveLaneHeight(10000, kViewH, kRuler, kMinGrid) == kViewH / 2,
               "cc lane: lane height clamps to half the view");
        expect(cl::effectiveLaneHeight(10000, 90, kRuler, kMinGrid) == 90 - kRuler - kMinGrid,
               "cc lane: lane height clamps to preserve the minimum grid area");
        expect(cl::effectiveLaneHeight(10000, 40, kRuler, kMinGrid) == 0
                   && cl::effectiveLaneHeight(-5, kViewH, kRuler, kMinGrid) == 0,
               "cc lane: degenerate view heights and negative prefs clamp to zero");

        // 10. Fit Drums interaction: the drum fit sizes the window from
        // rows*rowHeight + ruler + lanes + ccLaneHeightPref, so with the lane expanded the fitted
        // grid still holds every row; collapsing only grows the grid and can never hide a row.
        constexpr int kRows = 14;
        constexpr int kRowH = 18;
        const int fittedViewH = kRuler + kRows * kRowH + kVelLane + kDefaultH;
        const int fittedLane =
            cl::effectiveLaneHeight(kDefaultH, fittedViewH, kRuler, kMinGrid);
        expect(fittedViewH - kRuler - kVelLane - fittedLane == kRows * kRowH,
               "cc lane: drum fit with an expanded lane keeps every fitted row visible");
        expect(fittedViewH - kRuler - kVelLane
                       - cl::effectiveLaneHeight(0, fittedViewH, kRuler, kMinGrid)
                   >= kRows * kRowH,
               "cc lane: collapsing after a drum fit only grows the note grid");
    }
    // --- SPIKE-01 (P0/P1A): capture-diagnostic helpers must be provably correct and
    // provably sanitized before any real-plugin measurement is trusted. Removable with the spike.
    void testSpike01CaptureDiagnostics()
    {
        // 1) SHA-256 against published FIPS 180-4 test vectors (single- and multi-block).
        expect(spike01::Sha256::hashHex("", 0)
                       == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
               "spike01: sha256(\"\") matches the FIPS vector");
        expect(spike01::Sha256::hashHex("abc", 3)
                       == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
               "spike01: sha256(\"abc\") matches the FIPS vector");
        {
            const std::string longMsg = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
            expect(spike01::Sha256::hashHex(longMsg.data(), longMsg.size())
                           == "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1",
                   "spike01: sha256 multi-block FIPS vector matches");
        }
        {
            // Incremental update() across block boundaries must equal one-shot hashing.
            std::string bulk(200, 'x');
            spike01::Sha256 h;
            h.update(bulk.data(), 70);
            h.update(bulk.data() + 70, 130);
            expect(h.finishHex() == spike01::Sha256::hashHex(bulk.data(), bulk.size()),
                   "spike01: incremental sha256 equals one-shot sha256");
        }

        // 2) Duration statistics (min / median / p95 nearest-rank / max).
        {
            const auto s = spike01::computeDurationStats({ 5.0, 1.0, 3.0 });
            expect(s.count == 3 && s.minMs == 1.0 && s.medianMs == 3.0 && s.maxMs == 5.0,
                   "spike01: stats odd-count min/median/max");
            expect(s.p95Ms == 5.0, "spike01: stats n=3 p95 is the max (nearest rank ceil(2.85)=3)");
        }
        {
            const auto s = spike01::computeDurationStats({ 4.0, 2.0, 8.0, 6.0 });
            expect(s.count == 4 && s.medianMs == 5.0,
                   "spike01: stats even-count median averages the middle pair");
        }
        {
            std::vector<double> twenty;
            for (int i = 1; i <= 20; ++i)
                twenty.push_back((double)i);
            const auto s = spike01::computeDurationStats(twenty);
            expect(s.p95Ms == 19.0, "spike01: stats n=20 p95 nearest-rank picks the 19th value");
            expect(s.minMs == 1.0 && s.maxMs == 20.0 && s.medianMs == 10.5,
                   "spike01: stats n=20 min/median/max");
        }
        expect(spike01::computeDurationStats({}).count == 0, "spike01: stats empty input is safe");

        // 3) Sanitization: the report is built from hash+size only and must never contain
        //    anything derived from raw blob content except its SHA-256 hex.
        {
            const std::string secretBlob = "SECRET-LICENSE-XYZ-0042-PRIVATE-PATH-C:/Users/nn";
            const std::string secretHash =
                spike01::Sha256::hashHex(secretBlob.data(), secretBlob.size());

            spike01::CaptureSample a;
            a.phaseId = "A1";
            a.capturePath = "raw-getStateInformation";
            a.durationMs = 1.25;
            a.blobBytes = (std::uint64_t)secretBlob.size();
            a.sha256Hex = secretHash;
            a.onMessageThread = true;
            a.timestampIso = "2026-09-05T10:00:00";
            spike01::CaptureSample b = a;
            b.phaseId = "A4";
            b.durationMs = 2.5;

            spike01::ParamEvent ev;
            ev.kind = "paramChanged";
            ev.parameterIndex = 7;
            ev.parameterName = "Drawbar 16'";
            ev.newValue = 0.5f;
            ev.onMessageThread = true;
            ev.timestampIso = "2026-09-05T10:00:01";

            spike01::ReportHeader hdr;
            hdr.appVersion = "0.9.0";
            hdr.pluginName = "VB3-II";
            hdr.pluginFormat = "VST3";
            hdr.generatedAtIso = "2026-09-05T10:00:02";

            const std::string md =
                spike01::buildReportMarkdown(hdr, { a, b }, { ev }, { "note-1" });

            expect(md.find("SECRET-LICENSE") == std::string::npos,
                   "spike01: report never contains raw blob content");
            expect(md.find(secretHash) != std::string::npos,
                   "spike01: report contains the full sha256 hex");
            expect(md.find("1.25") != std::string::npos && md.find("2.50") != std::string::npos,
                   "spike01: report contains capture timings");
            expect(md.find("| A1 / raw-getStateInformation |") != std::string::npos,
                   "spike01: report groups stats per phase/path");
            expect(md.find("Drawbar 16'") != std::string::npos,
                   "spike01: report lists parameter notifications");
            expect(md.find("byte-stable") != std::string::npos,
                   "spike01: report states byte-stability per phase");
        }

        // 4) Required phases: the panel's phase list covers the SPIKE-01 measurement matrix.
        {
            const auto& phases = spike01::requiredPhases();
            const char* needed[] = { "A1", "A2", "A3", "A4", "A5", "B2",
                                     "B3", "B4", "B5", "E1", "E2", "F1" };
            bool all = true;
            for (const char* id : needed)
            {
                bool found = false;
                for (const auto& p : phases)
                    found = found || (std::string(p.id) == id);
                all = all && found;
            }
            expect(all, "spike01: required phase list covers A1..A5, B2..B5, E1, E2, F1");
        }
    }

    // SPIKE-01B-M (removable with the spike): the MIDI-delivery counters used to prove that
    // scheduled MIDI/CC reached the measured plugin instance in the M2V session. Synthetic
    // buffers exercise the exact raw-byte classification code used in the measurement and the
    // reconciliation identity reported in the audit (§28.3):
    //   noteOn + noteOff + cc + other == sum(channelHist) + channelless
    void testSpike01MidiDeliveryCounters()
    {
        spike01::MidiDeliveryCounters c;

        // Block 1 (512 samples): note-on ch1, note-on-vel0 ch2 (counts as note-off per JUCE
        // semantics), CC11 ch1, CC123 (All Notes Off — the transport-stop flush) ch16, SysEx.
        {
            juce::MidiBuffer b;
            b.addEvent(juce::MidiMessage::noteOn(1, 60, (juce::uint8) 100), 10);
            b.addEvent(juce::MidiMessage::noteOn(2, 61, (juce::uint8) 0), 20);
            b.addEvent(juce::MidiMessage::controllerEvent(1, 11, 64), 30);
            b.addEvent(juce::MidiMessage::allNotesOff(16), 40);
            const juce::uint8 sysex[] = { 0x01, 0x02, 0x03 };
            b.addEvent(juce::MidiMessage::createSysExMessage(sysex, 3), 50);
            c.countBlock(b, 512);
        }
        // Block 2 (512 samples): empty — blocks advances, blocksWithMidi must not.
        {
            juce::MidiBuffer b;
            c.countBlock(b, 512);
        }
        // Block 3 (512 samples): plain note-off ch3 at offset 5 → absolute position 1029.
        {
            juce::MidiBuffer b;
            b.addEvent(juce::MidiMessage::noteOff(3, 60, (juce::uint8) 0), 5);
            c.countBlock(b, 512);
        }

        expect(c.blocks.load() == 3 && c.blocksWithMidi.load() == 2,
               "spike01b: delivery counters track blocks and blocks-with-midi separately");
        expect(c.noteOn.load() == 1, "spike01b: note-on with velocity>0 counted as noteOn");
        expect(c.noteOff.load() == 2,
               "spike01b: 0x80 note-off and 0x90 velocity-0 both counted as noteOff");
        expect(c.cc.load() == 2 && c.cc11.load() == 1,
               "spike01b: CC counts include CC123 all-notes-off; cc11 counts only controller 11");
        expect(c.other.load() == 1 && c.channelless.load() == 1,
               "spike01b: SysEx counted as other and channelless (excluded from histogram)");
        expect(c.channelHist[0].load() == 2 && c.channelHist[1].load() == 1
                       && c.channelHist[2].load() == 1 && c.channelHist[15].load() == 1,
               "spike01b: channel histogram attributes events to ch1/ch2/ch3/ch16");
        expect(c.firstEventAbs.load() == 10 && c.lastEventAbs.load() == 1024 + 5,
               "spike01b: first/last event positions are absolute across blocks");
        {
            std::uint64_t histSum = 0;
            for (const auto& h : c.channelHist)
                histSum += h.load();
            const std::uint64_t typed
                = c.noteOn.load() + c.noteOff.load() + c.cc.load() + c.other.load();
            expect(typed == histSum + c.channelless.load(),
                   "spike01b: reconciliation identity noteOn+noteOff+cc+other == hist+channelless");
        }
    }

    //==========================================================================
    // P1D — production isolated proxy renderer (ProxyRenderTypes.h,
    // ProxyOfflineSequencer.h, ProxyRenderExecutor.h). The executor's processor
    // seam is a template, so these deterministic tests drive the COMPLETE render
    // loop (scheduling parity, scratch rule, latency preservation, tail policy,
    // WAV write + validation, cancellation, failure paths) with fake processors
    // and no plugin hosting. The isolated-instance lifecycle halves are covered
    // by Debug assertions + the automated real-plugin P1D integration plan.
    //==========================================================================

    /// Minimal snapshot builders (fields are plain data; the P1C builder itself is covered by
    /// the existing snapshot/fingerprint tests above).
    proxy_snapshot::SnapshotClip makeSeqClip(const std::int64_t anchorRef,
                                             const std::int64_t startRef,
                                             const std::int64_t lengthRef,
                                             const double bpm = 120.0,
                                             const int tpq = 960)
    {
        proxy_snapshot::SnapshotClip c;
        c.clipId = 1;
        c.timelineAnchorSamples = anchorRef;
        c.startSamples = startRef;
        c.lengthSamples = lengthRef;
        c.bpm = bpm;
        c.ticksPerQuarter = tpq;
        return c;
    }

    proxy_snapshot::SnapshotNote makeSeqNote(const std::int64_t startTick,
                                             const std::int64_t durationTicks,
                                             const int note,
                                             const int channel)
    {
        proxy_snapshot::SnapshotNote n;
        n.midiNote = note;
        n.velocity = 100;
        n.offVelocity = 64;
        n.channel = channel;
        n.startTick = startTick;
        n.durationTicks = durationTicks;
        return n;
    }

    /// Flattened emission for assertions: absolute sample + raw status/data bytes.
    struct EmittedEvent
    {
        std::int64_t absSample = 0;
        int status = 0; // 0x90 on / 0x80 off / 0xB0 cc (already channel-stripped)
        int channel = 1;
        int d1 = 0;
        int d2 = 0;
    };

    std::vector<EmittedEvent> runSequencer(proxy_render::ProxyOfflineSequencer& seq,
                                           const std::int64_t totalSamples,
                                           const int blockSize,
                                           const bool includeResetPrefix)
    {
        std::vector<EmittedEvent> out;
        juce::MidiBuffer midi;
        bool first = true;
        for (std::int64_t pos = 0; pos < totalSamples; pos += blockSize)
        {
            midi.clear();
            if (first && includeResetPrefix)
            {
                seq.emitResetAndChasePrefix(midi);
            }
            first = false;
            seq.emitBlock(pos, blockSize, midi);
            for (const auto meta : midi)
            {
                if (meta.numBytes >= 3)
                {
                    EmittedEvent e;
                    e.absSample = pos + meta.samplePosition;
                    e.status = meta.data[0] & 0xF0;
                    e.channel = (meta.data[0] & 0x0F) + 1;
                    e.d1 = meta.data[1];
                    e.d2 = meta.data[2];
                    out.push_back(e);
                }
            }
        }
        return out;
    }

    /// Live-parity ordering + domains: merge order (destination first, then eligible sources in
    /// session order), CC-before-NoteOn at equal offsets, ORD-1 stored-order tie-break, Note Off
    /// lifecycle across block boundaries, ineligible-source skip, reset/chase prefix, and §10.1
    /// reference→render conversion at the renderer boundary.
    void testProxyOfflineSequencerLiveParity()
    {
        using proxy_render::ProxyOfflineSequencer;
        const double refRate = 48000.0;

        proxy_snapshot::ProxyRenderSnapshot snap;
        snap.destinationTrackId = TrackId{ 7 };
        snap.destinationMidiOutputChannel = 0; // Any → native channels
        snap.renderConfig.timelineReferenceRate = refRate;
        snap.renderConfig.noteOffGateMs = 100;

        // Destination clip: CC11 at tick 0 + two equal-time notes (stored order 60 then 64,
        // both ch1) + a long note whose Note Off lands in a later block (tick 960 @120bpm/960tpq
        // = 0.5 s = 24000 samples at 48 kHz).
        {
            auto clip = makeSeqClip(0, 0, 4 * 48000);
            clip.notes.push_back(makeSeqNote(0, 960, 60, 1));
            clip.notes.push_back(makeSeqNote(0, 960, 64, 1));
            proxy_snapshot::SnapshotCcPoint cc;
            cc.startTick = 0;
            cc.controller = 11;
            cc.value = 100;
            cc.channel = 1;
            cc.interpolationToNext = 0;
            clip.ccPoints.push_back(cc);
            snap.destinationClips.push_back(std::move(clip));
        }
        // Source A (session order first): native ch5 remapped to its track channel 2.
        {
            proxy_snapshot::SnapshotSource s;
            s.trackId = TrackId{ 8 };
            s.midiOutputChannel = 2;
            auto clip = makeSeqClip(0, 0, 4 * 48000);
            clip.notes.push_back(makeSeqNote(0, 960, 48, 5));
            s.clips.push_back(std::move(clip));
            snap.sources.push_back(std::move(s));
        }
        // Source B: channel 3.
        {
            proxy_snapshot::SnapshotSource s;
            s.trackId = TrackId{ 9 };
            s.midiOutputChannel = 3;
            auto clip = makeSeqClip(0, 0, 4 * 48000);
            clip.notes.push_back(makeSeqNote(0, 960, 36, 3));
            s.clips.push_back(std::move(clip));
            snap.sources.push_back(std::move(s));
        }
        // Ineligible source (muted): must not deliver anything (fingerprint keeps it; PID-006).
        {
            proxy_snapshot::SnapshotSource s;
            s.trackId = TrackId{ 10 };
            s.midiOutputChannel = 4;
            s.muted = true;
            auto clip = makeSeqClip(0, 0, 4 * 48000);
            clip.notes.push_back(makeSeqNote(0, 960, 40, 4));
            s.clips.push_back(std::move(clip));
            snap.sources.push_back(std::move(s));
        }

        ProxyOfflineSequencer seq(snap, refRate);
        expect(seq.hasAnyEvents(), "p1d-seq: baked schedule has events");
        expect(seq.lastEventRenderSample() == 24000,
               "p1d-seq: span end = last Note Off at 0.5s (24000 samples @48k)");

        const auto evs = runSequencer(seq, 48000, 512, true);
        // Reset/chase prefix: channels 1,2,3 used (muted ch4 source skipped) → 12 CC at t=0
        // (64/120/121/123 per channel), BEFORE any musical event.
        bool prefixOk = evs.size() > 12;
        for (int i = 0; i < 12 && prefixOk; ++i)
        {
            prefixOk = evs[(size_t)i].status == 0xB0 && evs[(size_t)i].absSample == 0
                       && (evs[(size_t)i].d1 == 64 || evs[(size_t)i].d1 == 120
                           || evs[(size_t)i].d1 == 121 || evs[(size_t)i].d1 == 123);
        }
        expect(prefixOk, "p1d-seq: reset/flush prefix (CC64/120/121/123) precedes all events");
        bool ch4Seen = false;
        for (const auto& e : evs)
        {
            ch4Seen = ch4Seen || e.channel == 4;
        }
        expect(!ch4Seen, "p1d-seq: muted source delivers nothing (playback gate honored)");

        // Musical events at t=0 directly after the prefix: CC11 ch1 BEFORE the destination
        // Note Ons (Stage D rule), dest notes in STORED order (ORD-1), then source A (ch2,
        // remapped from native 5), then source B (ch3) — the live merge order.
        {
            bool ok = evs.size() >= 17;
            size_t i = 12;
            ok = ok && evs[i].status == 0xB0 && evs[i].channel == 1 && evs[i].d1 == 11
                 && evs[i].d2 == 100 && evs[i].absSample == 0;
            ++i;
            ok = ok && evs[i].status == 0x90 && evs[i].channel == 1 && evs[i].d1 == 60;
            ++i;
            ok = ok && evs[i].status == 0x90 && evs[i].channel == 1 && evs[i].d1 == 64;
            ++i;
            ok = ok && evs[i].status == 0x90 && evs[i].channel == 2 && evs[i].d1 == 48;
            ++i;
            ok = ok && evs[i].status == 0x90 && evs[i].channel == 3 && evs[i].d1 == 36;
            expect(ok, "p1d-seq: CC-before-NoteOn, ORD-1 stored order, dest-then-sources merge");
        }

        // Note Off lifecycle: every Note On gets its true Note Off at exactly 24000 (deferred
        // across ~46 block boundaries via the pending queue).
        {
            int offsAt24000 = 0;
            for (const auto& e : evs)
            {
                if (e.status == 0x80 && e.absSample == 24000)
                {
                    ++offsAt24000;
                }
            }
            expect(offsAt24000 == 4,
                   "p1d-seq: all four Note Offs land at the exact deferred sample (24000)");
        }

        // §10.1 conversion at the renderer boundary: the same snapshot rendered at 96 kHz puts
        // tick 960 (0.5 s) at sample 48000 — reference integers are converted, ticks re-bake.
        {
            ProxyOfflineSequencer seq96(snap, 96000.0);
            expect(seq96.lastEventRenderSample() == 48000,
                   "p1d-seq: reference→render conversion (0.5s = 48000 samples @96k)");
            const auto evs96 = runSequencer(seq96, 96000, 512, false);
            bool sawOnAtZero = false;
            bool sawOffAt48000 = false;
            for (const auto& e : evs96)
            {
                sawOnAtZero = sawOnAtZero || (e.status == 0x90 && e.absSample == 0 && e.d1 == 60);
                sawOffAt48000 = sawOffAt48000
                                || (e.status == 0x80 && e.absSample == 48000 && e.d1 == 60);
            }
            expect(sawOnAtZero && sawOffAt48000,
                   "p1d-seq: events re-bake at the render rate, never the device rate");
        }

        // CC dedup parity: an unchanged chased/repeated value is never re-sent.
        {
            ProxyOfflineSequencer seq2(snap, refRate);
            const auto evs2 = runSequencer(seq2, 48000, 512, false);
            int cc11Count = 0;
            for (const auto& e : evs2)
            {
                if (e.status == 0xB0 && e.d1 == 11)
                {
                    ++cc11Count;
                }
            }
            expect(cc11Count == 1, "p1d-seq: single-point CC stream emits exactly one CC11");
        }
    }

    /// Locked tail policy v1 (−70 dBFS peak / 1.0 s window / 30 s cap; §15.2).
    void testProxyTailDetectorPolicy()
    {
        using proxy_render::ProxyTailDetector;
        const double sr = 48000.0;
        const double loud = proxy_render::dbToLinear(-40.0);
        const double quiet = proxy_render::dbToLinear(-80.0);

        {
            // 0.5 s loud tail, then quiet: completes when the 1.0 s window fills.
            ProxyTailDetector d(sr);
            std::int64_t fed = 0;
            auto verdict = ProxyTailDetector::Verdict::Continue;
            while (verdict == ProxyTailDetector::Verdict::Continue)
            {
                verdict = d.feedBlock(fed < (std::int64_t)(0.5 * sr) ? loud : quiet, 512);
                fed += 512;
            }
            expect(verdict == ProxyTailDetector::Verdict::TailComplete,
                   "p1d-tail: decaying signal completes the tail");
            const double tailSec = (double)d.tailSamplesConsumed() / sr;
            expect(tailSec > 1.45 && tailSec < 1.60,
                   "p1d-tail: tail = material 0.5s + 1.0s continuous silence window");
        }
        {
            // Window restarts after a re-rise above the threshold.
            ProxyTailDetector d(sr);
            std::int64_t fed = 0;
            auto verdict = ProxyTailDetector::Verdict::Continue;
            while (verdict == ProxyTailDetector::Verdict::Continue)
            {
                const double t = (double)fed / sr;
                const bool loudNow = t < 0.5 || (t >= 1.2 && t < 1.3); // burst inside the window
                verdict = d.feedBlock(loudNow ? loud : quiet, 512);
                fed += 512;
            }
            const double tailSec = (double)d.tailSamplesConsumed() / sr;
            expect(verdict == ProxyTailDetector::Verdict::TailComplete && tailSec > 2.25
                       && tailSec < 2.45,
                   "p1d-tail: continuous-window requirement restarts after a re-rise");
        }
        {
            // Material output for the whole 30 s cap ⇒ CapReached (diagnosed incomplete).
            ProxyTailDetector d(sr);
            auto verdict = ProxyTailDetector::Verdict::Continue;
            while (verdict == ProxyTailDetector::Verdict::Continue)
            {
                verdict = d.feedBlock(loud, 4096);
            }
            expect(verdict == ProxyTailDetector::Verdict::CapReached,
                   "p1d-tail: 30s cap with material output = CapReached, never complete");
        }
        {
            // Exactly-at-threshold peak is NOT silence (strict below-threshold contract).
            ProxyTailDetector d(sr);
            auto verdict = ProxyTailDetector::Verdict::Continue;
            while (verdict == ProxyTailDetector::Verdict::Continue)
            {
                verdict = d.feedBlock(proxy_render::dbToLinear(-70.0), 4096);
            }
            expect(verdict == ProxyTailDetector::Verdict::CapReached,
                   "p1d-tail: a floor at exactly -70 dBFS never qualifies as silence");
        }
    }

    //==========================================================================
    // Fake processors for the executor's template seam. Thread affinity note:
    // production calls the executor from ONE dedicated render worker
    // (ProxyForegroundRenderJob); these fakes additionally record the calling
    // thread so the single-caller contract is asserted here too.
    //==========================================================================
    struct FakeProcBase
    {
        int ins = 0;
        int outs = 2;
        int latency = 0;
        std::uint64_t blocks = 0;
        juce::Thread::ThreadID firstCaller = nullptr;
        bool singleCaller = true;
        int maxChannelsSeen = 0;

        int getTotalNumInputChannels() const { return ins; }
        int getTotalNumOutputChannels() const { return outs; }
        int getLatencySamples() const { return latency; }

        void noteBlock(juce::AudioBuffer<float>& b)
        {
            ++blocks;
            maxChannelsSeen = juce::jmax(maxChannelsSeen, b.getNumChannels());
            const auto tid = juce::Thread::getCurrentThreadId();
            if (firstCaller == nullptr)
            {
                firstCaller = tid;
            }
            else if (tid != firstCaller)
            {
                singleCaller = false;
            }
        }
    };

    /// Held-note tone with a fixed 1.5 s release tail (deterministic tail measurement) and
    /// per-channel MIDI bookkeeping.
    struct FakeSustainToneProc : FakeProcBase
    {
        std::int64_t pos = 0;
        int held = 0;
        std::int64_t toneUntil = -1; ///< output tone through this absolute sample
        std::int64_t cc11Seen = 0;

        void processBlock(juce::AudioBuffer<float>& b, juce::MidiBuffer& midi)
        {
            noteBlock(b);
            for (const auto meta : midi)
            {
                const auto* d = meta.data;
                if (meta.numBytes < 3)
                {
                    continue;
                }
                const int status = d[0] & 0xF0;
                if (status == 0x90 && d[2] > 0)
                {
                    ++held;
                    toneUntil = -1;
                }
                else if (status == 0x80 || (status == 0x90 && d[2] == 0))
                {
                    if (--held <= 0 && d[1] != 120 && d[1] != 123)
                    {
                        held = 0;
                        toneUntil = pos + meta.samplePosition + (std::int64_t)(1.5 * 48000.0);
                    }
                }
                else if (status == 0xB0 && d[1] == 11)
                {
                    ++cc11Seen;
                }
            }
            for (int i = 0; i < b.getNumSamples(); ++i)
            {
                const std::int64_t abs = pos + i;
                const float v = (held > 0 || (toneUntil >= 0 && abs <= toneUntil)) ? 0.5f : 0.0f;
                for (int c = 0; c < juce::jmin(2, b.getNumChannels()); ++c)
                {
                    b.setSample(c, i, v);
                }
            }
            pos += b.getNumSamples();
        }
    };

    /// SPIKE-02-equivalent fixed-latency instrument (333 samples): a noteOn at absolute T emits
    /// a unit impulse at T + 333 — leading latency must survive into the WAV untrimmed.
    struct FakeLatency333Proc : FakeProcBase
    {
        static constexpr int kLatency = 333;
        std::int64_t pos = 0;
        std::vector<std::int64_t> impulses;

        FakeLatency333Proc() { latency = kLatency; }

        void processBlock(juce::AudioBuffer<float>& b, juce::MidiBuffer& midi)
        {
            noteBlock(b);
            for (const auto meta : midi)
            {
                const auto* d = meta.data;
                if (meta.numBytes >= 3 && (d[0] & 0xF0) == 0x90 && d[2] > 0)
                {
                    impulses.push_back(pos + meta.samplePosition + kLatency);
                }
            }
            b.clear();
            for (std::size_t k = 0; k < impulses.size();)
            {
                const std::int64_t t = impulses[k];
                if (t < pos + b.getNumSamples())
                {
                    if (t >= pos)
                    {
                        for (int c = 0; c < juce::jmin(2, b.getNumChannels()); ++c)
                        {
                            b.setSample(c, (int)(t - pos), 1.0f);
                        }
                    }
                    impulses.erase(impulses.begin() + (std::ptrdiff_t)k);
                }
                else
                {
                    ++k;
                }
            }
            pos += b.getNumSamples();
        }
    };

    /// Six-output instrument: junk on the extra bus channels, silence on the main pair —
    /// proves the scratch rule (max(2,in,out) channels) and the §5 stereo boundary (junk on
    /// channels 2..5 never reaches the WAV or the tail detector).
    struct FakeMultiOutProc : FakeProcBase
    {
        FakeMultiOutProc() { outs = 6; }
        bool markerWritten = false;

        void processBlock(juce::AudioBuffer<float>& b, juce::MidiBuffer&)
        {
            noteBlock(b);
            b.clear();
            if (b.getNumChannels() >= 6)
            {
                for (int i = 0; i < b.getNumSamples(); ++i)
                {
                    b.setSample(5, i, 0.9f); // junk outside the recorded boundary
                }
            }
            if (!markerWritten && b.getNumChannels() >= 2 && b.getNumSamples() > 0)
            {
                b.setSample(0, 0, 0.25f);
                b.setSample(1, 0, 0.25f);
                markerWritten = true;
            }
        }
    };

    struct FakeNeverSilentProc : FakeProcBase
    {
        void processBlock(juce::AudioBuffer<float>& b, juce::MidiBuffer&)
        {
            noteBlock(b);
            for (int c = 0; c < juce::jmin(2, b.getNumChannels()); ++c)
            {
                for (int i = 0; i < b.getNumSamples(); ++i)
                {
                    b.setSample(c, i, 0.1f); // -20 dBFS forever
                }
            }
        }
    };

    struct FakeSilentProc : FakeProcBase
    {
        void processBlock(juce::AudioBuffer<float>& b, juce::MidiBuffer&)
        {
            noteBlock(b);
            b.clear();
        }
    };

    struct FakeNanProc : FakeProcBase
    {
        void processBlock(juce::AudioBuffer<float>& b, juce::MidiBuffer&)
        {
            noteBlock(b);
            b.clear();
            b.setSample(0, 0, std::numeric_limits<float>::quiet_NaN());
        }
    };

    /// Requests cooperative cancellation from inside processBlock after N blocks (deterministic
    /// single-thread cancellation test).
    struct FakeSelfCancelProc : FakeProcBase
    {
        proxy_render::ProxyRenderCancellationToken token;
        std::uint64_t cancelAfterBlocks = 3;

        void processBlock(juce::AudioBuffer<float>& b, juce::MidiBuffer&)
        {
            noteBlock(b);
            b.clear();
            if (blocks >= cancelAfterBlocks)
            {
                token.requestCancel();
            }
        }
    };

    [[nodiscard]] juce::File p1dTempWav(const juce::String& tag)
    {
        return juce::File::getSpecialLocation(juce::File::tempDirectory)
            .getChildFile("MiniDAWSelftests")
            .getChildFile("p1d-" + tag + "-" + juce::String(juce::Time::currentTimeMillis())
                          + ".wav");
    }

    /// Organ-topology snapshot (dest ch-Any + ch2/ch3 sources + CC11) for executor tests.
    [[nodiscard]] proxy_snapshot::ProxyRenderSnapshot makeExecutorSnapshot(const double refRate)
    {
        proxy_snapshot::ProxyRenderSnapshot snap;
        snap.destinationTrackId = TrackId{ 7 };
        snap.destinationMidiOutputChannel = 0;
        snap.renderConfig.timelineReferenceRate = refRate;
        snap.renderConfig.noteOffGateMs = 100;
        snap.spanAndSilence.hasHostScheduledEvents = true;
        {
            auto clip = makeSeqClip(0, 0, (std::int64_t)(4 * refRate));
            clip.notes.push_back(makeSeqNote(0, 960, 60, 1)); // 0.5 s @120bpm
            proxy_snapshot::SnapshotCcPoint cc;
            cc.startTick = 0;
            cc.controller = 11;
            cc.value = 100;
            cc.channel = 1;
            clip.ccPoints.push_back(cc);
            snap.destinationClips.push_back(std::move(clip));
        }
        {
            proxy_snapshot::SnapshotSource s;
            s.trackId = TrackId{ 8 };
            s.midiOutputChannel = 2;
            auto clip = makeSeqClip(0, 0, (std::int64_t)(4 * refRate));
            clip.notes.push_back(makeSeqNote(0, 960, 48, 5));
            s.clips.push_back(std::move(clip));
            snap.sources.push_back(std::move(s));
        }
        {
            proxy_snapshot::SnapshotSource s;
            s.trackId = TrackId{ 9 };
            s.midiOutputChannel = 3;
            auto clip = makeSeqClip(0, 0, (std::int64_t)(4 * refRate));
            clip.notes.push_back(makeSeqNote(0, 960, 36, 3));
            s.clips.push_back(std::move(clip));
            snap.sources.push_back(std::move(s));
        }
        return snap;
    }

    [[nodiscard]] proxy_render::ProxyRenderExecutionConfig makeExecCfg(const double rate,
                                                                       const juce::String& tag)
    {
        proxy_render::ProxyRenderExecutionConfig cfg;
        cfg.renderSampleRate = rate;
        cfg.blockSize = 512;
        cfg.temporaryWavFile = p1dTempWav(tag);
        cfg.expectedFingerprint = "sha256:test-" + tag;
        cfg.primarySemanticRevision = 42;
        return cfg;
    }

    /// Complete happy path: MIDI merge/channels/CC11 delivery, tail completion, WAV
    /// write + §8 validation, request↔result identity echo, single processBlock caller.
    void testProxyRenderExecutorCompleteRender()
    {
        const double sr = 48000.0;
        const auto snap = makeExecutorSnapshot(sr);
        const auto cfg = makeExecCfg(sr, "complete");
        FakeSustainToneProc proc;
        proxy_render::ProxyRenderCancellationToken token;
        const auto r = proxy_render::renderProxyDestination(proc, snap, cfg, token);

        expect(r.status == proxy_render::ProxyRenderStatus::Succeeded
                   && r.failureReason == proxy_render::ProxyRenderFailureReason::None,
               "p1d-exec: complete destination render succeeds");
        expect(r.expectedFingerprint == cfg.expectedFingerprint
                   && r.primarySemanticRevision == 42,
               "p1d-exec: result echoes the captured request identity (fingerprint + revision)");
        expect(r.midi.noteOnsByChannel[0] == 1 && r.midi.noteOnsByChannel[1] == 1
                   && r.midi.noteOnsByChannel[2] == 1 && r.midi.ccByController[11] == 1,
               "p1d-exec: routed ch1/ch2/ch3 notes and CC11 reach the processor");
        expect(r.midi.noteOffsByChannel[0] >= 1 && r.midi.noteOffsByChannel[1] >= 1
                   && r.midi.noteOffsByChannel[2] >= 1,
               "p1d-exec: Note Off lifecycle delivered for every channel");
        expect(r.spanEndRenderSamples == 24000,
               "p1d-exec: span end = final relevant event in render samples");
        // Tail: 1.5 s tone after the final Note Off + 1.0 s silence window, block-rounded.
        const double tailSec = (double)r.tailLengthSamples / sr;
        expect(r.tailCompleted && tailSec > 2.40 && tailSec < 2.65,
               "p1d-exec: tail rendered to completion (release + silence window)");
        expect(r.renderedLengthSamples > r.spanEndRenderSamples
                   && r.renderedLengthSamples == (std::int64_t)r.blocksProcessed * 512,
               "p1d-exec: asset ends when the accepted tail completes (no zero-padding)");
        expect(r.temporaryWavFile.existsAsFile() && r.wavBytes > 0,
               "p1d-exec: validated temporary WAV exists");
        const auto v = proxy_render::validateTemporaryWav(r.temporaryWavFile, sr, 2,
                                                          r.renderedLengthSamples);
        expect(v.ok && v.isFloat && v.bitsPerSample == 32,
               "p1d-exec: artifact revalidates as 32-bit-float stereo WAV at the render rate");
        expect(proc.singleCaller, "p1d-exec: exactly one thread called processBlock");
        expect(r.allFinite && r.maxPeakLinear > 0.4, "p1d-exec: finite, non-silent output");
        (void)r.temporaryWavFile.deleteFile();
    }

    /// §7 latency preservation: first nonzero sample in the WAV = noteOn + 333 (never trimmed),
    /// and the reported value is recorded in the result.
    void testProxyRenderExecutorLatencyPreservation()
    {
        const double sr = 48000.0;
        auto snap = makeExecutorSnapshot(sr);
        snap.sources.clear(); // single ch1 note at t=0 keeps the impulse position exact
        const auto cfg = makeExecCfg(sr, "latency");
        FakeLatency333Proc proc;
        proxy_render::ProxyRenderCancellationToken token;
        const auto r = proxy_render::renderProxyDestination(proc, snap, cfg, token);
        expect(r.status == proxy_render::ProxyRenderStatus::Succeeded,
               "p1d-latency: render succeeds");
        expect(r.pluginLatencySamplesAtStart == 333 && r.pluginLatencySamplesAtEnd == 333,
               "p1d-latency: reported plugin latency recorded in the result");

        juce::WavAudioFormat fmt;
        std::unique_ptr<juce::AudioFormatReader> reader(
            fmt.createReaderFor(r.temporaryWavFile.createInputStream().release(), true));
        std::int64_t firstNonZero = -1;
        if (reader != nullptr)
        {
            juce::AudioBuffer<float> chunk(2, 4096);
            for (std::int64_t pos = 0; pos < (std::int64_t)reader->lengthInSamples && firstNonZero < 0;)
            {
                const int n = (int)juce::jmin<std::int64_t>(4096, reader->lengthInSamples - pos);
                reader->read(&chunk, 0, n, pos, true, true);
                for (int i = 0; i < n && firstNonZero < 0; ++i)
                {
                    if (std::abs(chunk.getSample(0, i)) > 1.0e-7f)
                    {
                        firstNonZero = pos + i;
                    }
                }
                pos += n;
            }
        }
        expect(firstNonZero == 333,
               "p1d-latency: leading latency preserved in the artifact (impulse at 0 + 333)");
        (void)r.temporaryWavFile.deleteFile();
    }

    /// Scratch sizing + §5 stereo boundary for a >2-output plugin.
    void testProxyRenderExecutorScratchAndStereoBoundary()
    {
        const double sr = 8000.0; // small rate keeps the silent tail fast
        auto snap = makeExecutorSnapshot(sr);
        const auto cfg = makeExecCfg(sr, "multiout");
        FakeMultiOutProc proc;
        proxy_render::ProxyRenderCancellationToken token;
        const auto r = proxy_render::renderProxyDestination(proc, snap, cfg, token);
        expect(proc.maxChannelsSeen == 6,
               "p1d-boundary: scratch spans max(2, totalIn, totalOut) channels (6-out plugin)");
        expect(r.status == proxy_render::ProxyRenderStatus::Succeeded,
               "p1d-boundary: junk on channels 2..5 never reaches the tail detector");
        expect(r.maxPeakLinear <= 0.26,
               "p1d-boundary: recorded boundary peak is the main-pair marker, not the junk");
        const auto v = proxy_render::validateTemporaryWav(r.temporaryWavFile, sr, 2,
                                                          r.renderedLengthSamples);
        expect(v.ok && v.channels == 2,
               "p1d-boundary: artifact is the stereo main pair only (same boundary as live DAL)");
        (void)r.temporaryWavFile.deleteFile();
    }

    /// §15.2 Locked: 30 s cap with material output ⇒ Failed("tail limit"), nothing publishable,
    /// temp artifact cleaned up (not retained unless explicitly diagnostic).
    void testProxyRenderExecutorTailCapFailure()
    {
        const double sr = 8000.0;
        auto snap = makeExecutorSnapshot(sr);
        const auto cfg = makeExecCfg(sr, "tailcap");
        FakeNeverSilentProc proc;
        proxy_render::ProxyRenderCancellationToken token;
        const auto r = proxy_render::renderProxyDestination(proc, snap, cfg, token);
        expect(r.status == proxy_render::ProxyRenderStatus::Failed
                   && r.failureReason == proxy_render::ProxyRenderFailureReason::TailLimitReached,
               "p1d-tailcap: cap with material output = Failed tail-limit, never complete");
        expect(!r.tailCompleted && r.temporaryWavFile == juce::File()
                   && !cfg.temporaryWavFile.existsAsFile(),
               "p1d-tailcap: no publishable result; temp artifact deleted by the RAII guard");
        const double tailSec = (double)r.tailLengthSamples / sr;
        expect(tailSec >= 29.9 && tailSec < 30.3, "p1d-tailcap: cap honored at 30s tail");
    }

    /// §9 cancellation: prompt stop at a block boundary, Cancelled (never Failed), temp cleanup.
    void testProxyRenderExecutorCancellation()
    {
        const double sr = 48000.0;
        const auto snap = makeExecutorSnapshot(sr);
        const auto cfg = makeExecCfg(sr, "cancel");
        FakeSelfCancelProc proc;
        const auto r = proxy_render::renderProxyDestination(proc, snap, cfg, proc.token);
        expect(r.status == proxy_render::ProxyRenderStatus::Cancelled
                   && r.failureReason == proxy_render::ProxyRenderFailureReason::None,
               "p1d-cancel: cooperative cancellation returns Cancelled, not Failed");
        expect(proc.blocks == 3,
               "p1d-cancel: stops at the first block boundary after the request (no 4th block)");
        expect(!cfg.temporaryWavFile.existsAsFile(),
               "p1d-cancel: partial temp artifact cleaned up");
    }

    /// Failure paths: missing WAV target and non-finite plugin output.
    void testProxyRenderExecutorFailurePaths()
    {
        const double sr = 8000.0;
        const auto snap = makeExecutorSnapshot(sr);
        {
            auto cfg = makeExecCfg(sr, "nowav");
            cfg.temporaryWavFile = juce::File(); // no target
            FakeSilentProc proc;
            proxy_render::ProxyRenderCancellationToken token;
            const auto r = proxy_render::renderProxyDestination(proc, snap, cfg, token);
            expect(r.status == proxy_render::ProxyRenderStatus::Failed
                       && r.failureReason == proxy_render::ProxyRenderFailureReason::WavWriteFailed,
                   "p1d-fail: unusable WAV target = WavWriteFailed");
        }
        {
            const auto cfg = makeExecCfg(sr, "nan");
            FakeNanProc proc;
            proxy_render::ProxyRenderCancellationToken token;
            const auto r = proxy_render::renderProxyDestination(proc, snap, cfg, token);
            expect(r.status == proxy_render::ProxyRenderStatus::Failed
                       && r.failureReason == proxy_render::ProxyRenderFailureReason::NonFiniteAudio,
                   "p1d-fail: NaN output = NonFiniteAudio failure");
            expect(!cfg.temporaryWavFile.existsAsFile(),
                   "p1d-fail: failure path cleans the temp artifact up");
        }
        {
            // Invalid render configuration.
            auto cfg = makeExecCfg(sr, "badcfg");
            cfg.renderSampleRate = 0.0;
            FakeSilentProc proc;
            proxy_render::ProxyRenderCancellationToken token;
            const auto r = proxy_render::renderProxyDestination(proc, snap, cfg, token);
            expect(r.status == proxy_render::ProxyRenderStatus::Failed
                       && r.failureReason == proxy_render::ProxyRenderFailureReason::SnapshotInvalid,
                   "p1d-fail: invalid render configuration is rejected");
        }
    }

    /// §6 empty destination: conservative classification decides between the explicit silent
    /// generation (no WAV) and the honest full-span render.
    void testProxyRenderExecutorEmptyDestination()
    {
        const double sr = 8000.0;
        proxy_snapshot::ProxyRenderSnapshot empty;
        empty.destinationTrackId = TrackId{ 7 };
        empty.renderConfig.timelineReferenceRate = sr;
        empty.spanAndSilence.hasHostScheduledEvents = false;

        {
            // Eligible (explicitly host-event-driven): silent generation without a WAV.
            auto snap = empty;
            snap.spanAndSilence.instrumentClassifiedHostEventDriven = true;
            snap.spanAndSilence.silentGenerationEligible = true;
            const auto cfg = makeExecCfg(sr, "silentgen");
            FakeSilentProc proc;
            proxy_render::ProxyRenderCancellationToken token;
            const auto r = proxy_render::renderProxyDestination(proc, snap, cfg, token);
            expect(r.status == proxy_render::ProxyRenderStatus::SucceededSilent
                       && r.temporaryWavFile == juce::File() && proc.blocks == 0
                       && !cfg.temporaryWavFile.existsAsFile(),
                   "p1d-empty: eligible empty destination = silent generation, no WAV, no render");
        }
        {
            // Unclassified + actually silent: the normal safe path renders the silence window.
            const auto cfg = makeExecCfg(sr, "emptysafe");
            FakeSilentProc proc;
            proxy_render::ProxyRenderCancellationToken token;
            const auto r = proxy_render::renderProxyDestination(proc, empty, cfg, token);
            expect(r.status == proxy_render::ProxyRenderStatus::Succeeded && proc.blocks > 0
                       && r.renderedLengthSamples >= (std::int64_t)sr,
                   "p1d-empty: unclassified empty destination takes the normal full render path");
            (void)r.temporaryWavFile.deleteFile();
        }
        {
            // Unclassified + autonomous output: NEVER silently discarded — the render honestly
            // fails at the tail cap (compatibility limitation instead of fake silence).
            const auto cfg = makeExecCfg(sr, "emptyauto");
            FakeNeverSilentProc proc;
            proxy_render::ProxyRenderCancellationToken token;
            const auto r = proxy_render::renderProxyDestination(proc, empty, cfg, token);
            expect(r.status == proxy_render::ProxyRenderStatus::Failed
                       && r.failureReason == proxy_render::ProxyRenderFailureReason::TailLimitReached,
                   "p1d-empty: autonomous generator output is never classified as silence");
        }
    }
} // namespace

namespace
{
    /// P1D preflight — §9.4.2 host-managed monotonic Primary revision: monotonic from 0, exact
    /// under concurrent bumps (plugin notifications may arrive on any thread, incl. audio).
    void testPrimarySemanticRevisionCounter()
    {
        mini_daw::PrimarySemanticRevision rev;
        expect(rev.current() == 0, "p1d-preflight: revision starts at 0 (nothing observed)");
        expect(rev.bump() == 1 && rev.current() == 1,
               "p1d-preflight: bump returns the new monotonic value");

        constexpr int kThreads = 4;
        constexpr int kBumpsPerThread = 5000;
        std::vector<std::thread> ts;
        ts.reserve(kThreads);
        for (int t = 0; t < kThreads; ++t)
        {
            ts.emplace_back([&rev] {
                for (int i = 0; i < kBumpsPerThread; ++i)
                {
                    (void)rev.bump();
                }
            });
        }
        for (auto& t : ts)
        {
            t.join();
        }
        expect(rev.current() == 1 + (std::uint64_t)kThreads * kBumpsPerThread,
               "p1d-preflight: concurrent bumps lose no revision (atomic counter)");
    }
} // namespace

int main()
{
    testTitleAndStatus();
    testOctaveLabels();
    testChannelSemantics();
    testArrangedTiming();
    testKeyStripTiming();
    testOwnershipAndCleanup();
    testExportEffectiveChannel();
    testCcModelValidation();
    testCcEvaluation();
    testCcEventGeneration();
    testExportWithCc();
    testProjectV19PersistenceAndMigration();
    testProjectV20SchemaRoundTripAndV19Migration();
    testTimelineDomainConversion();
    testMidiDependencyEnumeration();
    testProxySnapshotImmutabilityAndDeterminism();
    testFingerprintRenderRelevantEdits();
    testFingerprintExclusions();
    testSilentGenerationAndSpan();
    testCcNormalizationLastWins();
    testPrimarySemanticRevisionCounter();
    testAuditionDispatchIntegration();
    testToolbarLayoutAndVisibility();
    testCcLaneViewState();
    testSpike01CaptureDiagnostics();
    testSpike01MidiDeliveryCounters();
        testProxyOfflineSequencerLiveParity();
        testProxyTailDetectorPolicy();
        testProxyRenderExecutorCompleteRender();
        testProxyRenderExecutorLatencyPreservation();
        testProxyRenderExecutorScratchAndStereoBoundary();
        testProxyRenderExecutorTailCapFailure();
        testProxyRenderExecutorCancellation();
        testProxyRenderExecutorFailurePaths();
        testProxyRenderExecutorEmptyDestination();

    std::printf("\n%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
