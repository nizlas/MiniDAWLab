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

#include "instruments/InstrumentTrackController.h"
#include "io/InstrumentMidiClipExport.h"
#include "io/ProjectFile.h"
#include "ui/experimental/ExperimentalMidiCcAutomation.h"

// SPIKE-01 (P0/P1A validation spike) pure diagnostic helpers — see
// docs/audits/SPIKE_01_AUTHORITATIVE_PLUGIN_STATE_CAPTURE.md. Removable with the spike.
#include "diagnostics/Spike01ReportFormat.h"
#include "diagnostics/Spike01Sha256.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <cstdio>
#include <memory>
#include <string>
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
            v18Json = v18Json.replace("\"version\": 19", "\"version\": 18")
                          .replace("\"version\":19", "\"version\":18");
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
    testAuditionDispatchIntegration();
    testToolbarLayoutAndVisibility();
    testCcLaneViewState();
    testSpike01CaptureDiagnostics();

    std::printf("\n%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
