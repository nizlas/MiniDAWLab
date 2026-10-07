# Solo + four persistent Solo memories (2026-10-07)

Solo as a temporary LISTENING layer on top of the stored Mute flags, with one temporary solo set
plus four persistent, project-saved Solo memories (buttons 1–4 above the track-header column).
Architecture: `docs/CURRENT_ARCHITECTURE.md` "Solo and Solo memories"; test policy row in
`docs/DEVELOPMENT_TEST_POLICY.md`.

**Development environment note.** This slice was implemented and verified in a Linux cloud
environment (GCC, headless). Everything listed under "Verified here" ran there through the real
production code; everything under "Needs local (Windows) verification" was NOT run and must be
checked on the development machine. No Windows/ASIO build, no real plug-in (VB3-II, AmpliTube,
Groove Agent), no listening test and no OS-level click was performed for this slice.

## 1. Planning output (as required by the implementation guide)

**Scope.** Five explicit solo sets in `Session` (one temporary + four memories, exactly one
current); S buttons in arrangement headers and mixer strips; the memory strip in the main window;
a derived `SoloMuteView` consumed by the realtime callback and the offline mixdown; MIDI
event-level gating for shared instrument destinations; the Mute-command lock; v25 persistence of
the four memories; a narrow solo-memory undo step; proxy interplay through a SecondaryLive
override with an atomic refusal when partial isolation is impossible.

**Out of scope (unchanged model).** No change to stored Mute semantics, Power/Off, routing,
volume/pan/inserts, record-arm/monitor behavior, proxy identity/currency or render scheduling; no
solo on Stereo Out; no per-project persistence of the temporary set or the active memory
selection; no installer or version bump.

**Gaps / risks found and how they were handled.**
* `InstrumentTrackController` bakes `muted_` into its snapshot's `playbackEnabled`, so zeroing the
  strip alone could not un-mute a soloed instrument's MIDI → `playbackEnabledIgnoringMute` + a
  `soloForceAudibleOverridesMute` argument fed by `solo_mute_view::trackForcedAudibleBySolo`;
  gate transitions count as segment discontinuities (pending note-offs flushed, CC re-chased).
* An explicitly soloed but base-muted pure-MIDI lane would still have been silent because MIDI
  lanes were initially left `ForcedSilent` in the derived view → the derivation now marks the
  soloed MIDI lane itself `ForcedAudible` (it renders no audio; the mark feeds the scheduler's
  mute override and keeps the UI from tinting it as solo-silenced). Caught by the focused tests.
* The controller's local `muted_` could diverge from the session when `setTrackMuted` refuses
  under the lock → `InstrumentTrackController::setMuted` checks `isMuteChangeLockedBySolo()`
  itself before flipping (load/undo set the flag through snapshot restore and are unaffected).
* ~40 snapshot-publish sites exist; hooking each for re-derivation was rejected →
  `SoloCoordinator` re-derives on every solo command, after solo-step undo/redo, and on a 10 Hz
  snapshot-pointer compare; a track missing from a stale view defaults to ForcedSilent while solo
  is active (briefly over-silenced, never falsely audible).
* The 6th header cell (S) made the old minimum width clip → limits re-derived from the real
  geometry (154/166), old saved narrower widths clamp UP at layout time, larger ones preserved;
  the mixer strip grew 150 → 174 px.

**Plausible wrong implementations avoided.** Rewriting Mute flags for solo (destroys the user's
mute picture; forbidden by the model); reconstructing mutes on un-solo (loses concurrent
changes); auto-copying the temporary set into a newly selected memory (spec forbids);
suppressing a ForcedSilent instrument's MIDI consumption (mute semantics require the host to keep
consuming, zeroed, so un-solo restores mid-note); a global All Notes Off on suppression (would cut
other audible sources); marking derived pass-through red on the S button (only EXPLICIT membership
is red); locking Mute only in the UI (the spec demands the command path refuses).

**Deferred (safe).** Any solo automation; solo-in-place / AFL-PFL distinctions; persistence of
the temporary set (spec says transient); splitting `SoloCoordinator`'s pointer-watch into
publish-site hooks.

## 2. Behavior rules (the contract)

1. **Solo never writes Mute.** Stored `Track::isMuted()` flags are never rewritten, project save
   always writes the stored flags, un-solo instantly restores the base Mute picture.
2. **Five separate sets.** One temporary set + four memories; exactly one is current (no memory
   selected → temporary). The S buttons always show/edit the CURRENT set. Switching never copies
   content; the temporary set survives memory trips untouched.
3. **Active = at least one existing soloed track.** A selected but empty (or fully stale) memory
   restricts nothing and does not lock Mute.
4. **Mute is locked while solo is active — in the command path.** `Session::setTrackMuted` (and
   the instrument controller's mirror) refuses; project load and undo restore are unaffected.
   The M cell shows the effective state with a small padlock: normal mute, solo-silenced
   (distinct dimmed tint), or passed-through; clicks are inert while locked.
5. **Audibility is derived from the actual routing model.** Explicit solos are heard even if
   base-muted; every needed downstream bus (main-out chain + enabled sends + their chains) passes
   signal even if base-muted; unrelated sources are zeroed at their OWN strips (so shared buses
   carry no foreign signal); Group solo includes upstream feeders (incl. send paths,
   transitively) WITH their stored Mute ("hear what the group plays"); Off always wins.
6. **MIDI.** Soloing a MIDI lane opens its destination instrument as a carrier: the destination's
   own clips and other MIDI sources into it are suppressed at event level (flush pending
   note-offs, then skip — no hanging notes, no global All Notes Off). Soloing the instrument
   includes its own clips AND all routed sources. A ForcedSilent instrument keeps consuming its
   MIDI with zeroed output (mute semantics).
7. **Red = explicit only.** Derived pass-through (group feeders, carriers) never marks an S red.
8. **Engine discipline.** The view is derived off the audio thread and published behind an atomic
   shared_ptr; the callback acquires one view per block and does only allocation-free binary
   searches; works with serial and parallel instrument processing; offline mixdown follows the
   same published view; recording input is never discarded because monitoring is solo-silenced.
9. **Persistence.** Only the four memories are saved (v25 root `soloMemories`, omitted when all
   empty). On open: memory buttons off, temporary set empty, memories intact. Pre-v25 → four
   empty memories; malformed/duplicate/stale ids degrade silently and never produce a
   false-active solo.
10. **Undo.** A content change of an ACTIVE MEMORY is one narrow undo step (dirty), carrying only
    that memory's before/after sets — undoing can never revert clips/tracks/takes and targets
    the intended memory even after switching. Temporary-set edits and memory switches are not
    undoable and not dirty.
11. **Tracks.** Deleting a track removes it from the EFFECTIVE set (no ghost solo); undoing the
    delete restores membership (the stored id survives); Duplicate Track never auto-adds the
    copy to any solo set.
12. **Proxies.** Whole-destination solo may keep playing the proxy. Partial isolation (one source
    of a proxied destination) requires a usable Secondary live path; when none exists the solo
    toggle is refused atomically with an explanation. Temporary solo never changes stored proxy
    identity/currency and never schedules a render. **Limitation:** on a machine where neither
    the Primary nor a Secondary of a proxied destination can load, a partial isolation inside
    that destination is simply not available (the refusal explains it); whole-destination solo
    still works through the proxy.

## 3. Where things live

| Concern | Files |
|---|---|
| Solo sets, current-set rule, Mute lock, persistence apply | `src/domain/Session.h/.cpp` (solo section) |
| Derived view + audio-thread helpers | `src/engine/SoloMuteView.h/.cpp` |
| Engine publish + strip/MIDI consumption (realtime + offline) | `src/engine/PlaybackEngine.h/.cpp`, `src/engine/PlaybackMixHelpers.h/.cpp` |
| Instrument MIDI gate (suppression + mute override) | `src/instruments/InstrumentTrackController.h/.cpp` |
| App commands, memory semantics, republish, proxy feasibility | `src/app/SoloCoordinator.h/.cpp` |
| Proxy SecondaryLive override | `src/instruments/ProxyPlaybackCoordinator.h` |
| Narrow undo step | `src/domain/SessionHistory.h/.cpp`, `src/app/UndoRedoCoordinator.h/.cpp` |
| v25 `soloMemories` | `src/io/ProjectFile.h/.cpp` |
| UI seam + S cells + locked M + memory strip | `src/ui/SoloUiHooks.h`, `src/ui/TrackHeaderView.h/.cpp`, `src/ui/TrackStripButtonGlyphs.h`, `src/ui/SoloMemoryStrip.h`, `src/ui/TrackLanesView.h/.cpp`, `src/app/InstrumentTimelineRowCoordinator.h/.cpp`, `src/ui/mixer/*`, `src/app/TransportLayoutHelper.h/.cpp`, `src/app/MainAppWindow.cpp` |

## 4. Verified here (Linux cloud, Debug, GCC)

* Full build of ALL targets (app + every focused test executable). Three pre-existing
  Linux/GCC portability errors were fixed on the way (`juce::int64` ambiguity in
  `Spike01StateCapturePanel`, GCC bug 88165 in `FollowAutoscrollGovernor.h`, a Windows-only
  helper called unconditionally in `Vst3ChildProcessScan.cpp`).
* `SoloFocusedTests` **104 / 0** — derivation closures (downstream + sends, group feeders incl.
  send paths and transitivity, two MIDI sources into one instrument, Off rules, stale ids),
  Session command semantics incl. the Mute lock, five-set separation + memory semantics, v25
  round trip through the production save/load + pre-v25 + malformed files, narrow undo through
  the real `UndoRedoCoordinator` (incl. targeting after switching and timeline-content safety),
  delete/restore/Duplicate membership, S-cell and memory-strip geometry offscreen.
* `ExportLevelFocusedTests` **60 / 0** — NEW solo case: soloing t1 renders bit-identically to
  base-muting t2 in BOTH the realtime callback and the offline mixdown; a base-muted soloed
  track is heard while its stored flag stays set; `setTrackMuted` refused under the lock;
  un-solo restores the stored picture bit-exactly.
* `TrackHeaderColumnFocusedTests` **152 / 0** (154/166 limits, clamp-up of old saved widths),
  `MixerFocusedTests` **107 / 0** (174 px strip with the S cell), plus unchanged-green
  `InputRouting` 56, `TrackDuplicate` 23, `InstrumentParallel` 39 (serial-vs-parallel
  bit-identity with the solo-aware scheduling signature), `WaveformReload` 30,
  `LiveMidiRecording` 110.
* `MiniDAWSelftests` 3478 / 2 — the two failures (`mc-txn` replace-failure pair) are
  PRE-EXISTING environment effects: the test makes the target file read-only and expects the
  atomic replace to fail, which holds on Windows (`MoveFileEx`) but not on Linux (`rename()`
  permission sits on the directory). The exit-time `ShutdownDetector` leak assertion is also
  pre-existing (a `juce::Timer` started by `ExperimentalMidiPatternPlayer` tests without a JUCE
  GUI shutdown in that harness). Untouched by this slice.

## 5. Needs local (Windows) verification — short list

1. Build the Windows/ASIO configuration (not attempted here).
2. Listen: solo a few tracks during playback — multiple solos together, un-solo restoring mutes,
   no clicks on toggle during playback and during recording; a recording made while
   solo-silenced still captures its material.
3. Real instrument + MIDI: two MIDI tracks into one VB3-II/instrument — solo one MIDI track
   (other source + instrument's own clips silent, no hanging notes), solo the instrument (all
   sources + own clips sound).
4. Proxy: whole-destination solo plays the proxy; partial isolation flips to SecondaryLive when
   a Secondary exists; without one, the refusal dialog appears and nothing half-toggles; stored
   proxy status unchanged afterwards.
5. UI by eye: S placement next to M in headers and mixer, red only on explicit solos, locked-M
   padlock + solo-silenced tint, memory strip 1–4 aligned above the header column at minimum and
   wide widths; old saved header widths from a pre-solo build open clamped up, wider ones kept.
6. Mixdown/export of a project while a solo is active follows the audible picture; proxy renders
   still render full material.
7. `--stability-live-midi`, `--stability-mixer`, `--stability-proxy-recording` scenario runs
   (Windows-only paths: real devices, plug-ins, screen capture).
