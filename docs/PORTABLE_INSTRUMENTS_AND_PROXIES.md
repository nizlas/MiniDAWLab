# Portable Instruments and Proxy Rendering — Technical Steering Document

**Status:** `Canonical`
**Date:** 2026-09-06 (revision 5, approved by explicit human review 2026-09-06 of the SPIKE-01B /
SPIKE-01B-M evidence — see below; revision 4 applied the SPIKE-01 amendment approved 2026-09-05;
revision 3 canonicalized through explicit human review 2026-09-04; revision 2 incorporated the
2026-09-03 design review)
**Authority:** normative for all Portable Instruments / Proxy implementation slices.

> **Revision 5 (2026-09-06).** Based on the reviewed SPIKE-01B / SPIKE-01B-M evidence
> (`docs/audits/SPIKE_01_AUTHORITATIVE_PLUGIN_STATE_CAPTURE.md`, measurement verdict
> **PARTIAL PASS**), this revision:
>
> * **corrects revision 4's premature PID-001 resolution** (the original SPIKE-01 PASS
>   over-claimed; the corrected evidence separates the capture layer from the identity layer);
> * **incorporates the reviewed PARTIAL PASS evidence** (byte-stable vs volatile serializers,
>   proven MIDI/CC delivery effects, probe self-perturbation, the observability limit);
> * **locks the hybrid state-identity contract** (§9.4): host-managed revision identity, raw
>   bytes never as general validity identity, byte equality only as positive evidence for
>   byte-stable-classified plugins at host-observable quiescence;
> * **separates snapshot eligibility from concurrent background rendering** (§9.4.4): rendering
>   may run during playback on an isolated instance; the project-start snapshot has its own
>   eligibility boundary;
> * **replaces the earlier short edit debounce with four selectable update modes** (§18.1),
>   including Auto with a fixed five-minute P1 idle delay per destination.
>
> PID-001 is now **Locked (reviewed) — hybrid authoritative-state and host-observed identity
> contract**: resolved by explicitly accepting the plugin API's fundamental observability
> boundary, not by claiming DAL can detect unknowable internal plugin changes. E2/MIDI-learn and
> broader plugin coverage remain documented compatibility limitations, not P1 blockers.

> **This document is DAL's canonical steering authority for Portable Instruments and Proxy
> Rendering.** Implementation prompts MUST reference the applicable roadmap slice and invariant
> IDs. Evidence-gated mechanisms remain intentionally unresolved behind their named spikes:
> Canonical document status does not automatically make an Open or Recommended implementation
> mechanism Locked.

**Evidence base:** the forensic audit
[`docs/audits/PORTABLE_INSTRUMENTS_ARCHITECTURE_AUDIT.md`](audits/PORTABLE_INSTRUMENTS_ARCHITECTURE_AUDIT.md)
(referenced below as **Audit**), cross-checked against production sources where pivotal
(`src/plugins/ExperimentalInstrumentHost.cpp`, `src/engine/PlaybackMixHelpers.cpp`,
`src/engine/PlaybackEngine.cpp`, `src/instruments/InstrumentTrackController.cpp`,
`src/io/ProjectFile.h/.cpp`, `src/app/AudioMixdownExporter.cpp`).

**Classification vocabulary used throughout:**

| Marker | Meaning |
|---|---|
| **Verified** | Demonstrated by repository evidence (Audit citation or direct source read). |
| **Locked** | Locked product decision — already decided and normative. |
| **Recommended** | The canonical preferred direction, but not a Locked invariant; changing it requires documented evidence and review. |
| **Open** | Intentionally unresolved behind a named validation gate or deferred design phase; dependent implementation MUST NOT pass that gate before review. |
| **Proposed** | A type, API, component, folder, or seam that **does not exist yet**. |

**Normative language:** MUST / MUST NOT mark locked requirements and correctness invariants.
SHOULD / SHOULD NOT mark strong recommendations. MAY marks optional or deferred behavior.
No Proposed type or API is ever described as though it already exists.

---

## Evidence-gated decisions and deliberately deferred design

Explicit human review approved this document and its Locked product behavior for canonicalization
on 2026-09-04. The same review approved retaining these controlled implementation gates inside the
Canonical document:

* PID-001 was evidence-gated behind SPIKE-01; revision 4's resolution (2026-09-05) proved
  premature and was corrected by SPIKE-01B/SPIKE-01B-M. **Final status (human review 2026-09-06,
  revision 5): Locked (reviewed) — hybrid authoritative-state and host-observed identity
  contract** (§9.4). Resolved by explicitly accepting the plugin API's observability boundary;
  E2 (MIDI-learn-class silent state) and plugin coverage beyond the two measured remain
  documented compatibility limitations, not P1 blockers.
* PID-005's numeric tail values remain evidence-gated behind SPIKE-02.
* OI-002's bounded playback I/O and sample-rate-adaptation mechanism remains evidence-gated behind
  SPIKE-03.
* PID-009's Secondary persistence shape remains deliberately deferred to P2.

These items do not leave the document itself in Draft status. No product-behavior decision remains
open; full context for every controlled gate lives in the decision register (§21).

| ID | Question | Status after review | Gate |
|---|---|---|---|
| **PID-001** (part) | Authoritative plugin-state capture mechanism: how does DAL observe "the user tweaked VB3-II drawbars" so the proxy goes stale and renders from *current* state? (Audit U2, H2) | **Locked (reviewed) — hybrid authoritative-state and host-observed identity contract, human review 2026-09-06 (revision 5).** Capture layer: fresh checkpoint capture (Resolved on measured evidence). Identity layer: host-managed revision + conservative lifecycle bumps + persisted save-pairing; raw bytes never as general validity identity; byte equality only as positive "unchanged" evidence for byte-stable-classified plugins at host-observable quiescence (§9.4). Resolved by explicitly accepting the observability boundary — not by claiming DAL detects unknowable internal changes. Documented compatibility limitations (non-blocking): E2 (MIDI-learn), plugins beyond the two measured. | SPIKE-01 + SPIKE-01B/01B-M **completed** (final measurement verdict PARTIAL PASS, reviewed). P1C unblocked. |
| **PID-005** (part) | Tail-policy numeric values: silence threshold, continuous-silence window, maximum tail duration. | **Open / evidence-gated.** Structure is Locked (§15.2); −60 dBFS / 1 s / 30 s are experimental starting points only, not canonical defaults. | **SPIKE-02** measurements with VB3-II-class tails. Blocks P1D numeric confirmation. |
| **OI-002** | Bounded proxy playback I/O and sample-rate adaptation: the audio-thread-safe playback-source mechanism (bounded memory, read-ahead vs budgeted preload vs cached conversion, resampler placement). | **Open / evidence-gated.** The *requirements* are Locked (§7.3, §15.3, PI-030/PI-031); only the technical mechanism is open. Unbounded full preload is rejected. | **SPIKE-03** (§22) — blocks P1G. |
| **PID-009** (part) | Secondary persistence/registry shape (second descriptor+state home per instrument track). | **Reviewed and deliberately deferred to P2.** Non-blocking for P1; technically unresolved until P2 design. P1B adds no placeholder schema (§12.4). | P2 design, using then-current architecture evidence. |

Review outcomes incorporated throughout this revision (details in §21):

* **OI-001 → Locked requirement:** cross-sample-rate proxy playback is mandatory; a rate mismatch
  alone never makes a proxy stale or silent (§15.3, PI-030). Only the adaptation mechanism remains
  open (OI-002/SPIKE-03).
* **PID-008 → Locked (first P2 version):** Secondary audition split defined in §17/§21 — no mixing
  of live Secondary audition over Proxy transport playback in the first P2 version.
* **PID-011 → Locked P1 behavior:** `Prepare Portable Project` is a concrete, cancellable P1
  operation with its own slice **P1J** (§22); only nonessential UI/container details remain
  Recommended.
* **PID-002, PID-003, PID-004, PID-006, PID-007, PID-010 → Locked** for required v1 behavior
  (implementation details of not-yet-existing APIs remain Proposed).
* **Incomplete tails never publish:** reaching the tail cap with material output is a diagnosed
  failure, not a Current proxy (§15.2).
* **Stale proxies are never audible in P1:** no explicit stale-playback feature exists (§17, §20).

---

## 1. Document authority, status, and change protocol

1. **Status transition.** `Draft for design review` → `Canonical` was completed by explicit human
   review on 2026-09-04. Only a human reviewer may change the document's authority status.
2. **Canonical precedence.** For Portable Instruments / Proxy work this document takes
   precedence over phase narratives and chat history; `docs/CURRENT_ARCHITECTURE.md` remains the
   authority for how the codebase is wired *today* (this document describes verified anchors plus a
   Proposed target).
3. **Change protocol.** Implementation slices MUST NOT widen this envelope. If an implementation
   discovers that a Locked invariant is wrong or a Recommended choice is infeasible, the slice
   stops and proposes the smallest steering amendment (per `docs/IMPLEMENTATION_GUIDE.md` Steering
   Document Change Rule); the amendment is reviewed by a human before code proceeds.
4. **Stable IDs.** Invariants `PI-###`, decisions `PID-###` (mirroring Audit D1–D11), additional
   open items `OI-###`, high-risk findings `HR-#`, spikes `SPIKE-##`, roadmap slices `P0/P1A…P1J`,
   tests `T-##`, blocking implementation prerequisites `ORD-#`/`TLD-#`. Future edits MUST NOT
   renumber existing IDs.
5. **Slice referencing.** A future implementation prompt references exactly one roadmap slice ID
   (§22) plus the invariants and decisions that slice cites. It never restates or reinterprets the
   whole feature.

## 2. Goals, user promise, and non-goals

### 2.1 User promise (Locked)

A DAL project that uses instrument plugins remains **musically editable and audibly faithful on a
machine where those plugins are not installed**. MIDI stays the visible, editable representation;
a hidden, internally owned audio *proxy* of each instrument destination reproduces the Primary
instrument's authoritative sound through the track's normal mixer path when the Primary plugin
cannot run.

This promise holds **across machines and audio configurations**: a current proxy remains playable
when the current engine/device sample rate differs from the rate it was rendered at, including
when Primary is missing (PI-030, §15.3). And the promise is deliverable: `Prepare Portable
Project` (P1J, §16.6) is part of P1, so a user can actually produce a validated, portable package
— not merely a project that would be portable in principle.

### 2.2 Goals (Locked)

* One visible instrument/MIDI track per instrument destination — proxies add **no** visible
  arrangement track and **no** mixer channel (PI-001).
* Proxy playback is transparent downstream: DAL inserts, fader, pan, sends, buses, master, and
  offline mixdown are shared, unchanged (PI-002, PI-008).
* Deterministic staleness: proxies represent exactly the musical content they were rendered from,
  proven by a canonical fingerprint (PI-018, PI-019); the plugin-state component of validity is
  host-observed per the §9.4 hybrid identity contract (revision 5). Staleness is a
  *musical-content* verdict: an engine sample-rate mismatch alone is a playback-adaptation
  concern, never staleness (PI-030).
* Background rendering that never blocks or corrupts the realtime engine (PI-011, PI-012).
* Proxy playback resource use is bounded: memory and I/O follow an explicit budget (PI-031,
  OI-002) — many long proxies must not require gigabytes of resident decoded audio.
* An explicit, cancellable `Prepare Portable Project` operation validates and packages everything
  a portable project needs (P1J, PID-011).

### 2.3 Non-goals for proxy v1 (Locked)

* No visible/diagnostic waveform view of proxy audio (MAY come later).
* No partial/chunked re-rendering (P4; PI-010).
* No Secondary instrument implementation (P2; PI-005).
* No missing-*effect*-plugin handling and no post-insert track freeze (P3).
* No general Plugin Delay Compensation (deferred engine feature; PI-015).
* No packaging of plugin binaries, licenses, or activation data — ever (PI-026).

## 3. Terminology

| Term | Definition |
|---|---|
| **Instrument destination** | A `TrackKind::Instrument` track owning one instrument plugin lane. **Verified:** one `ExperimentalInstrumentHost` per instrument track (Audit §3.2). |
| **MIDI source** | A `TrackKind::Midi` track routed via `midiTo` to exactly one instrument destination. **Verified:** strictly one hop, cycles structurally impossible (Audit §5.1). |
| **Primary** | The instrument selected through DAL's existing instrument-track workflow. Its sound is authoritative; its descriptor, state, and preset are preserved (Locked). |
| **Secondary** | An optional compatibility instrument with its own descriptor/state/preset, providing working audio when Primary cannot be used. P2. Never authoritative (Locked). |
| **Proxy** | Hidden audio rendered from Primary, owned by the destination track, representing Primary's authoritative sound when Primary is unavailable (Locked). |
| **Proxy process boundary** | The point after the instrument plugin's synthesis/internal processing and before DAL Pre inserts, fader, Post inserts, pan, group routing, master routing (Locked; Verified seam in §7). |
| **Render fingerprint** | Canonical hash over every input at or before the proxy process boundary that can affect Primary output (Proposed; §11). |
| **Render snapshot** | Immutable capture of everything a render job needs, taken at enqueue (Proposed; §10). |
| **Generation** | One published proxy asset identified by its fingerprint hash; immutable once published, together with the recorded render configuration (sample rate, policies) that produced it (Locked identity model; §16). |
| **Derived playback representation** | A runtime-prepared, engine-rate-compatible view of a generation (e.g. resampled or read-ahead data), built outside the audio thread when the engine rate differs from the generation's recorded render rate. It is never the authoritative asset and is rebuilt on engine-rate change (Proposed mechanism, Locked requirement; §15.3, OI-002). |
| **Stale proxy** | A retained, previously valid proxy whose fingerprint no longer matches current *musical* content (evaluated under the generation's own recorded render configuration — a sample-rate mismatch alone is not staleness). It remains an asset but is **never selected for playback** in P1; no explicit stale-playback feature exists (Locked; §17, §20). |
| **Update mode** | Per-destination proxy maintenance mode: Auto after idle / On Save / Manual / Off (§18.1, revision 5). |
| **Host-observable quiescence** | The boundary at which DAL has sent no MIDI/CC to the instance and received no parameter/dirty/non-parameter notification for the defined debounce window. The only quiescence DAL can establish; never proof of internal plugin quiescence (§9.4.4). |
| **Current (host-observed)** | Defined semantic concept for volatile-plugin currency: no render-relevant change observable by DAL since the accepted snapshot/revision — not an absolute claim about internal plugin activity (§9.4.5). Not required as the permanent main track label. |

## 4. Verified current-architecture anchors

Everything in this section is **Verified** (Audit citation given; pivotal items re-read in source).

### 4.1 The single instrument-output boundary

* Raw plugin output is produced in exactly one place:
  `ExperimentalInstrumentHost::audioThread_processBlockAndAddToOutputs`
  (`src/plugins/ExperimentalInstrumentHost.cpp` ~3471–3587). After the merged per-block MIDI
  (UI FIFO + `rtBlockMidi_`) is optionally handed to the `MidiDeliveryCaptureSink` test seam, the
  instance renders into the host-owned `scratch_`; gain and pan are applied only at the final
  `addFirstStereoBusToDeviceOutputs` add. The scratch content after `inst.processBlock` **is** the
  pre-fader/pre-insert/pre-pan signal (Audit §4.1; re-verified in source).
* That output is consumed in exactly one place:
  `playback_mix_helpers::renderInstrumentPostStripToStereoScratch`
  (`src/engine/PlaybackMixHelpers.cpp` ~651–703), which applies Pre inserts → fader → Post inserts
  → pan → bus fan-out *afterwards*, in both the insert and no-insert branches (Audit §4.1;
  re-verified). Both realtime playback (`PlaybackEngine.cpp` ~1084) and offline mixdown
  (`PlaybackEngine.cpp` ~1970) call this same helper.

### 4.2 Snapshot discipline

Five realtime views already use the identical pattern — immutable object, atomic `shared_ptr`
release-store on the message thread, acquire-load in the callback: `SessionSnapshot`,
`ExperimentalInstrumentPlaybackSnapshot`, per-controller `InstrumentTrackRenderSnapshot`,
`RoutingPlan`, `PluginAudioThreadMap` (Audit §3.3). A published "current proxy + source mode" per
instrument track would be the sixth instance of an established pattern.

### 4.3 Verified MIDI merge structure and unresolved equal-time ordering

The current merge is deterministic in structure but **not** fully deterministic at equal-time
boundaries. The complete current event stream MUST NOT be described as deterministic before ORD-1
lands. Precisely:

* **Verified deterministic destination/source merge order:** per block, the destination's own
  controller schedules first, then each `TrackKind::Midi` source in session order resolves
  `midiTo` from the current `SessionSnapshot` and schedules into the destination's host;
  `juce::MidiBuffer` preserves insertion order at equal offsets (Audit §3.1). Per-segment emission
  is pending Note Offs → CC (chase + in-segment) → note scan, with the re-verified nuance that a
  Note Off belonging to a Note On from the *same* segment is emitted during the note scan, i.e.
  after the CC pass at that offset (§8.3).
* **Verified deterministic CC normalization/insertion:** CC points live in repository-canonical
  normalized order (`normalizePoints`: `std::stable_sort`, duplicates collapsed last-wins), and
  the bake stable-sorts each per-`(controller, channel)` stream with append-order ties — so
  equal-time CC last-wins is deterministic given session track order (§8.3).
* **Unresolved equal-time note/clip ordering (Verified gap, HR-10):** the bake sorts notes by
  `absSample` and clip plans by `startSamples` with **unstable `std::sort` and no tie-break**
  (`InstrumentTrackController.cpp` ~2344–2353), so equal-time note order and equal-start clip
  order are implementation-defined today, and equal-time Note Off/Note On interleaving across
  clips can audibly differ.
* **ORD-1 (Proposed, not yet implemented):** replace both sorts with `std::stable_sort`, making
  stored order the documented equal-key tie-break (§8.3). Stored-order tie-break semantics in this
  document are post-ORD-1 target semantics, not current guarantees.

The stability fixture already builds the Organ Upper/Lower/Pedal shape (Audit §5.5). The detailed
scheduling description in §8.3 is authoritative and consistent with this summary.

### 4.4 Persistence and media

Single JSON `.dalproj`, schema **v19** (`ProjectFileV1::kCurrentVersion = 19`,
`src/io/ProjectFile.h` ~250; re-verified). Additive absent-key defaults are the established
migration seam (v17 `midiChannel`, v18 `midiTo`, v19 `ccPoints`). `TrackId` is a persisted,
monotonic `std::uint64_t`, never regenerated (Audit §8.1). Media is strictly project-relative
under `<ProjectFolder>/Audio/`; missing media is skipped at load with a diagnostic, never blocking
load (Audit §8.3). Atomic publication precedents: temp sibling + `moveFileTo` (project text);
delete + move (mixdown, Windows-aware) (Audit §8.3).

### 4.5 Plugin lifecycle constraints

VST3 only. `createPluginInstance`, `prepareToPlay`/`releaseResources`, bus layout, editor
open/close, `get/setStateInformation`, load/unload are message-thread-only as enforced by DAL
guards; `processBlock` runs on the audio thread in playback and on the *message* thread in mixdown
(exclusive, gated) (Audit §7.3). State blob is captured **at project save** via
`getCurrentInstrumentStateBase64` (host ~2055; called from `InstrumentTrackController.cpp` ~883/~948
— re-verified); instrument-editor close does **not** capture state (Audit §7.1). The drum-name
probe creates a temporary second `AudioPluginInstance` with the live instance's state copied in
(`ExperimentalInstrumentHost.cpp` ~3133–3163 — re-verified): precedent for cloned instantiation and
state restoration, **not** a render architecture (Locked interpretation, Audit §7.5).

### 4.6 What does not exist (Verified-absent)

No background audio rendering, no plugin-clone render path in production, no staleness signal, no
PDC (plugin `getLatencySamples` is only logged, `PluginInsertHost.cpp` ~261 — re-verified), no tail
rendering, no cancellation in offline paths, no persisted plugin version, no
`canDoOfflineProcessing`/`setNonRealtime` usage anywhere (re-verified by search), no
portable-project/collect-media feature, no media GC, no centralized `sourcesForDestination`
(duplicated at ≥6 sites, Audit §5.2), no audio streaming reader (clips fully decoded into memory,
Audit §4.2).

### 4.7 High-risk findings register (Verified)

These audit findings materially shape this document. Each is addressed where cited; none is a
footnote.

| ID | Finding (Audit ref) | Addressed in |
|---|---|---|
| HR-1 | Plugin state is captured too late (save-only) for correct fingerprinting and rendering (Audit H2, U2) | §9 (contract Locked §9.4, revision 5), PID-001, SPIKE-01/01B, slice P0/P1A, tests T-01, T-30 |
| HR-2 | Plugin version is not persisted in the project descriptor (Audit U1, H3) | §9.2, §11 (F1v), §12, PID-001, slice P1B, T-05 |
| HR-3 | Reverse incoming-MIDI enumeration duplicated at ≥6 sites (Audit §5.2, H4) | §8.2, PID-010, slice P1C, T-02 |
| HR-4 | Track order is semantic for same-time CC last-wins (Audit F9, H5) | §8.3, §11 (F9), T-03, T-06 |
| HR-5 | Existing mixdown blocks the message thread, gates the engine, reuses live instances — unsuitable as background-render model (Audit §6, H1) | §14.4, PI-017, PID-004, slices P1D/P1E, T-09/T-10 |
| HR-6 | No PDC policy exists anywhere (Audit H7) | §15.1, PI-014/PI-015, PID-005, T-15 |
| HR-7 | No reusable tail policy exists (Audit §6.1) | §15.2, PI-016, PID-005, T-16 |
| HR-8 | No cancellation/generation mechanism prevents stale publication (Audit §6.1, §7.4) | §13, PI-028, PID-004, slice P1E, T-11/T-12 |
| HR-9 | Sample-rate portability requires an explicit policy (Audit U5, H6) | §15.3 (Locked cross-rate playback, PI-030), OI-002/SPIKE-03, T-17, T-25 |
| HR-10 | Bake sorts for notes and clip plans are unstable `std::sort` without tie-breakers — equal-time delivered order is implementation-defined, and equal-time Off/On interleaving can audibly differ (source re-verified for revisions 2 and 3: `InstrumentTrackController.cpp` ~2344–2353) | §4.3, §8.3 (ORD-1 blocking P1C prerequisite), §11.4, T-03 |
| HR-11 | Sample-domain timeline fields (clip anchors/windows, locators, extent, playhead) are integers referenced to `deviceSampleRateAtSave`, which is re-stamped on every save **without rescaling** the integers; load performs no rescale and the bake uses the current device rate — so cross-rate machines drift and persistence alone cannot pin wall-clock meaning (source re-verified for revisions 2 and 3: `Session.cpp` ~1569–1570, ~1856–1982; `InstrumentTrackController.h` ~337–338; every runtime consumer receives the device rate, `ProjectIoCoordinator.cpp` ~782, `InstrumentRuntimeCoordinator.cpp` ~163) | §10.1 (TLD-1 blocking P1B prerequisite), §11.1 F3/F11, §15.3 |

## 5. Normative invariants

Each invariant is **Locked** unless marked otherwise. §25 maps every invariant to at least one
roadmap slice and test.

### Track and role model

* **PI-001** An instrument destination remains exactly one visible instrument/MIDI track. A proxy
  MUST NOT create another arrangement track and MUST NOT create another mixer channel. MIDI remains
  the visible and editable musical representation; proxy audio is hidden and internally owned by
  the destination track.
* **PI-002** Proxy playback MUST pass through the destination track's normal downstream DAL
  processing (Pre inserts, fader, Post inserts, pan, bus routing, master, offline mixdown). Proxy
  playback replaces only instrument generation.
* **PI-003** Primary is authoritative. Its plugin descriptor, state, and preset MUST remain
  preserved regardless of proxy or Secondary activity. When installed, available, and usable,
  Primary is the normal live playback source.
* **PI-004** Secondary MUST NOT overwrite or redefine Primary, MUST NOT overwrite or redefine the
  Primary proxy, and MUST NOT be presented as sonically identical to Primary.
* **PI-005** Secondary is instantiated only when needed, remains P2, and MUST NOT be pulled into
  the first proxy-foundation slices. Its v1 supports Preserve channels and MAY support a simple
  explicit channel remap (e.g. channels 1, 2, 3 → 1); advanced controller mapping is deferred; CC
  data including CC11 is initially forwarded unchanged.
* **PI-006** A proxy belongs only to Primary rendering, owned by the instrument destination (not
  by individual MIDI sources).
* **PI-007** A stale but valid previous proxy remains retained until a new proxy has been rendered
  and published successfully. A stale proxy MUST NOT be selected for playback in P1 (automatically
  or manually — no explicit stale-playback feature exists); it is retained as an asset only.

### Process boundary

* **PI-008** Proxy v1 represents Primary output after the plugin's synthesis and internal
  processing and **before** DAL Pre inserts, fader, Post inserts, pan, group routing, and master
  routing. Fader, pan, inserts, and downstream routing are therefore excluded from the fingerprint
  and shared at playback. Post-insert freeze is a separate later feature (P3).
* **PI-009** The complete incoming MIDI dependency graph of a destination (its own clips plus
  every routed source's content and channel settings) MUST be represented in the render snapshot
  and fingerprint. A render-relevant change in the destination **or any routed source** makes the
  destination's proxy stale.

### Rendering

* **PI-010** Proxy v1 re-renders the complete instrument destination. Arbitrary partial rendering
  is deferred (held notes/Note Off history, sustain and prior CC state, CC chase, Leslie/chorus/LFO
  phase, release envelopes, reverb/delay tails, internal filters/compressors, round-robin, and
  plugin-internal randomness make partial rendering unsafe without proof). Chunking, preroll,
  overlap, and crossfades MAY be investigated later (P4) but MUST NOT weaken v1 correctness.
* **PI-011** The background renderer MUST NOT process or share the live audio-thread plugin
  instance. Every render job uses a separate, isolated Primary instance restored from snapshotted
  state.
* **PI-012** No render job may block or incorrectly access the audio thread.
* **PI-013** Render jobs MUST support cooperative cancellation/obsolescence checks at safe block
  boundaries, MUST pause during recording, MAY pause during playback if required to avoid glitches,
  and MUST terminate safely on project close and application shutdown.

### Latency and tails

* **PI-014** Proxy v1 preserves the Primary plugin's actual output latency. The renderer MUST NOT
  independently shift rendered audio earlier. The plugin's reported latency MUST be recorded in
  proxy metadata, and the latency-policy version MUST be included in the render fingerprint.
  Switching between live Primary and Proxy MUST NOT introduce a timing jump caused by different
  latency treatment.
* **PI-015** Full Plugin Delay Compensation is deferred to a later, general DAL audio-engine
  feature covering live instruments, proxies, inserts, groups, master routing, and mixdown
  consistently. When the general PDC policy changes, existing proxies MUST either remain correctly
  interpretable through their latency metadata or be deterministically marked stale and
  regenerated. (`LatencySettingsStore` provides device playback/recording offsets only — **not**
  PDC; Verified §4.6.)
* **PI-016** The renderer MUST continue beyond the final MIDI event to capture instrument tails,
  governed by a versioned tail policy with deterministic silence detection, a maximum-tail limit,
  and explicit failure behavior for known truncation (§15.2).

### Architecture reuse

* **PI-017** The existing mixdown path (message-thread blocking, engine-gating, live-instance
  reuse) MUST NOT be copied as the architectural model for background proxy rendering. Existing
  deterministic scheduling and downstream mix helpers MAY be reused only where their contracts are
  demonstrably safe (§14.4). The drum-name probe is precedent for cloned instantiation + state
  restoration only.

### Fingerprinting and state

* **PI-018** The whole-project dirty flag MUST NOT determine proxy validity.
* **PI-019** A canonical render fingerprint MUST contain every input at or before the proxy
  process boundary that can affect Primary output, serialized under the canonical rules of §11.4
  (stable field order, stable collection ordering, deterministic numeric encoding, normalization,
  algorithm/schema version tags). Fingerprints MUST NOT depend on pointer identity,
  unordered-container iteration, display names, locale-sensitive formatting, or incidental JSON
  ordering.
* **PI-020** The current audible Primary state MUST be included in a render snapshot (as far as
  the host can observe — §9.4.5); DAL-observable parameter changes MUST make the proxy stale;
  rendering MUST NOT silently use an older Save-time state; Save, autosave, proxy rendering, and
  plugin-editor close MUST agree about current state (§9; mechanism Locked per §9.4, revision 5).

### Playback source

* **PI-021** Automatic transport source priority is: (1) Primary live when installed, available,
  and usable; (2) current Primary proxy when Primary is unavailable — including when its recorded
  render rate differs from the engine rate (PI-030); (3) Secondary live when no current Primary
  proxy represents the current edited state; (4) an explicit missing/silent state. A stale proxy
  is retained as an asset but is never selected (PI-007). Source changes MUST NOT occur silently.
* **PI-022** DAL MUST expose which source the user is hearing: Primary; Proxy current; Proxy
  rendering; Proxy stale; Secondary; Missing; Render failed.

### Modes, save, media

* **PI-023** Update modes Auto after idle / On Save / Manual / Off behave per §18.1 (revision 5:
  Auto uses a fixed five-minute per-destination idle delay in P1). Normal Save MUST remain
  responsive: capture required current state, save project data, queue render work only per the
  destination's update mode (§18.2 — never a forced render of every stale destination), and MUST
  NOT wait for rendering. Autosave never starts proxy rendering.
* **PI-024** Proxy audio is external binary media (never embedded in project JSON) with
  project-relative storage, stable `TrackId` ownership, render-hash generation identity,
  temp-file creation, validation before publication, and Windows-safe atomic publication. The
  previous valid proxy MUST be retained after a failed render.
* **PI-025** A missing or corrupt proxy MUST NOT prevent the project from loading; it degrades to
  a diagnosed status like missing plugins do today.
* **PI-026** Portable packaging MUST NOT include plugin binaries, licenses, activation data, or
  private licensing material.
* **PI-027** Proxy progress, queue state, and automatic source selection MUST NOT create
  meaningless musical undo entries (§18.3).
* **PI-028** An obsolete render job MUST NOT overwrite or replace newer valid state; publication
  requires a fingerprint-currency check at publish time.
* **PI-029** Product phases are preserved: P1 Primary + hidden proxy + portable packaging (P1J);
  P2 Secondary + simple remap; P3 missing effect plugins + post-insert freeze; P4 verified render
  optimizations / partial rendering. P2–P4 work MUST NOT be pulled into P1.

### Sample-rate adaptation and playback resources

* **PI-030** A current proxy generation MUST remain playable when its recorded render sample rate
  differs from the current engine/device sample rate, including when Primary is missing. A
  sample-rate mismatch by itself MUST NOT mark unchanged musical content stale, MUST NOT force
  silence, MUST NOT require Primary to be installed, and MUST NOT modify the authoritative proxy
  asset. Validity of an existing generation is evaluated under that generation's recorded render
  configuration (§11, §12.3). When rates differ, DAL prepares a derived playback representation
  outside the audio thread (§15.3); an engine-rate change invalidates and rebuilds only that
  derived representation, never the underlying generation. If Primary is available, DAL MAY later
  produce a native-rate generation as a quality refresh, but native re-rendering is never required
  before playback.
* **PI-031** Proxy playback resource use MUST be bounded. No resampling allocation, file I/O, or
  other expensive preparation may occur on the audio thread; memory use follows an explicit
  size/project budget rather than unconditional full decode of every proxy. The concrete
  mechanism (bounded read-ahead, budgeted preload, cached conversion) is evidence-gated by
  SPIKE-03 (OI-002) and blocks P1G.

## 6. Target architecture and ownership

All component and API names in this section are **Proposed** (they do not exist). Thread-affinity
choices follow verified DAL constraints (§4.5). "Owner" means the object that constructs, holds,
and destroys the component.

| # | Responsibility | Proposed component | Owner | Thread affinity |
|---|---|---|---|---|
| R1 | Project/model representation of proxy metadata | v20 additive fields on `experimentalInstrumentTracks[]` (§12) | `Session` / `ProjectFile` | Message |
| R2 | Authoritative plugin-state synchronization | `PrimaryStateAuthority` | `InstrumentRuntimeCoordinator` | Message (capture); audio-safe hints only |
| R3 | Incoming-MIDI reverse-dependency enumeration | `midi_dependency::sourcesForDestination()` | free function over `SessionSnapshot` | Any (pure) |
| R4 | Immutable render-snapshot construction | `ProxyRenderSnapshotBuilder` | `InstrumentProxyCoordinator` | Message |
| R5 | Canonical fingerprint construction | `ProxyFingerprintBuilder` | used by R4 and staleness checks | Any (pure over snapshot) |
| R6 | Isolated plugin render-instance lifecycle | `ProxyRenderInstance` | `ProxyRenderJob` | Construct/restore/prepare/release/destroy: Message; `processBlock`: worker |
| R7 | Render scheduler / queue | `ProxyRenderQueue` (single worker, Recommended) | `InstrumentProxyCoordinator` | Message (control) + one worker thread |
| R8 | Complete-destination renderer | `ProxyDestinationRenderer` | `ProxyRenderJob` | Worker (processing loop) |
| R9 | Proxy asset store and publication | `ProxyAssetStore` | `InstrumentProxyCoordinator` | Message (publication); worker writes temp files |
| R10 | Playback-source selection and rate-adapted proxy reads | per-host atomic source mode + derived playback representation (host-level seam, Locked §7.3; read mechanism gated by SPIKE-03) | `ExperimentalInstrumentHost` (RT view) driven by `InstrumentProxyCoordinator` | Message/background prepares; audio thread acquires |
| R11 | UI/status adaptation | `ProxyStatusModel` (+ existing Inspector/track-header seams, Audit §9) | UI layer | Message |
| R12 | Portable-project validation, collection, and packaging | `PortableProjectValidator` + packaging flow (P1J, PID-011 Locked) | `ProjectIoCoordinator` | Message (control) + queue reuse; never blocks the message thread during progress |

Per-responsibility contract:

* **R1 — Project/model (Proposed).** Mutable state: persisted proxy metadata fields. Inputs:
  publication events, load. Outputs: v20 JSON fields. Failure: absent/invalid metadata degrades to
  "no proxy" (PI-025). Cancellation: n/a. Shutdown: persisted with normal Save.
* **R2 — `PrimaryStateAuthority` (Proposed).** Mutable state: last-captured state blob + capture
  timestamp + the host-managed state revision + stable/volatile classification. Inputs: editor
  lifecycle events, revision-bump sources, capture requests (Save, enqueue), mechanism per the
  Locked §9.4 hybrid contract (PID-001). Outputs: authoritative state blob for snapshots and Save;
  revision/staleness hint. Failure: capture failure → proxy render blocked with diagnosed status, Save
  falls back to last-known blob with diagnostic (never silent). Cancellation: n/a. Shutdown: no
  background work to stop (message-thread only).
* **R3 — dependency enumerator (Proposed).** Pure function `SessionSnapshot × TrackId →
  ordered list of contributing sources` (destination first, then `TrackKind::Midi` rows with
  resolved `midiTo == destination`, in session order — mirroring the verified realtime merge,
  §4.3). No mutable state. Failure: none (total function). Existing duplicated scan sites SHOULD
  migrate to it over time (HR-3); realtime callers MAY keep their loops if migration risks the RT
  path, but fingerprint and snapshot construction MUST use the enumerator.
* **R4 — snapshot builder (Proposed).** Inputs: `SessionSnapshot`, R3 enumeration, tick-domain
  clip/note/CC data, R2 state blob, render policies. Output: immutable `ProxyRenderSnapshot`
  (§10). Failure: refuse to build (and report) if the state blob is unavailable. Cancellation:
  n/a (synchronous, message thread).
* **R5 — fingerprint builder (Proposed).** Pure over `ProxyRenderSnapshot` → canonical bytes →
  hash (§11). No mutable state. Failure: none for well-formed snapshots.
* **R6 — render instance (Proposed).** Mutable state: one `juce::AudioPluginInstance` never shared
  with the live engine (PI-011). Lifecycle: create → `setStateInformation` → `prepareToPlay` on
  the message thread; `processBlock` loop on the worker; `releaseResources` + destruction back on
  the message thread. Do **not** assume arbitrary worker-thread plugin construction is safe; the
  message-thread-only rule is DAL's verified guard (§4.5) and SPIKE-02 validates the division of
  labor. Failure: instantiation/restore failure fails the job (Failed state, previous proxy
  retained). Cancellation: worker loop observes the job's cancellation flag between blocks.
  Shutdown: coordinator cancels jobs, worker joins, instances destroyed on the message thread.
* **R7 — queue (Proposed).** Mutable state: pending/active job list, per-destination latest
  fingerprint. Single worker thread, low priority (Recommended, PID-004). Inputs: stale
  notifications, mode changes, explicit render requests. Outputs: job state transitions, progress
  reports. Failure: worker death is a diagnosed fatal queue state (no silent hang). Cancellation:
  per-job cooperative flags; superseding job cancels the older job for the same destination.
  Shutdown: cancel-all + bounded join (following the recorder/scan patterns, Audit §7.4).
* **R8 — renderer (Proposed).** Inputs: `ProxyRenderSnapshot`, prepared R6 instance, render
  config. Algorithm: §14.2. Outputs: temp WAV + measured metadata (latency, peak), **or** a
  diagnosed tail-limit failure when the max-tail cap is reached with materially non-silent output
  (§15.2 — never published). Failure: any processing exception → Failed. Cancellation: checked
  each block. Shutdown: via R7.
* **R9 — asset store (Proposed).** Mutable state: on-disk generations + a per-destination pointer
  to the most recently published generation (the pointer never implies status: Current vs Stale is
  always a fingerprint verdict, §12.3). Inputs: validated temp files + fingerprint identity.
  Outputs: atomically published generation files, retirement of superseded generations (§16).
  Failure: publication failure never deletes or overwrites the previous published generation — its
  metadata and asset are retained; it remains Current (and playable) only if its fingerprint still
  matches current musical content, otherwise it is retained as Stale and never selected for
  playback in P1 (PI-007, PI-024). Cancellation: obsolete temp files deleted safely. Shutdown:
  temp cleanup on next launch (crash recovery §16.4).
* **R10 — source selection (Proposed).** Mutable state: per-host atomic
  `shared_ptr<ProxyPlaybackView>` (Proposed: immutable {mode, playback-source handle, latency
  metadata}) following the `activeOwner_` swap pattern (§4.2). The view exposes the *derived
  playback representation* for the current engine rate (PI-030); its concrete read mechanism
  (bounded read-ahead vs budgeted preload vs cached conversion) is OI-002/SPIKE-03 scope
  (PI-031). Inputs: proxy status, Primary availability, engine rate. Outputs: the audio thread
  reads engine-rate-compatible proxy samples at the timeline position instead of calling
  `processBlock` (§7.3), with no allocation, file I/O, or resampling work on the audio thread.
  Failure: absent view → silent + Missing status (never crash). Cancellation: preparation work is
  abandonable. Shutdown: engine teardown unchanged (publish-before-destroy discipline, Audit §3.2).
* **R11 — status model (Proposed).** Mutable state: per-track UI status snapshot (PI-022
  vocabulary). Inputs: coordinator events. Outputs: Inspector/track-header status, notifications
  on source change (PI-021). Failure: UI-only. Shutdown: n/a.
* **R12 — portable validation and packaging (Proposed component; behavior Locked, P1J).** Inputs:
  authoritative fingerprints, asset validation, media inventory. Outputs: a validated portable
  package, or an explicit blocker report naming the tracks that prevent packaging. Runs as an
  explicit, cancellable operation with responsive progress; MUST NOT block the message thread and
  MUST NOT run during normal Save (PI-023). Failure/cancellation leaves no partially published
  package (§16.6). Never packages plugin binaries, licenses, or activation data (PI-026).

## 7. Proxy process boundary and signal flow

### 7.1 The boundary (Verified + Locked)

The Locked boundary of PI-008 exists in code today as the host `scratch_` immediately after
`inst.processBlock(view, blockMidi)` inside `audioThread_processBlockAndAddToOutputs`
(§4.1). Downstream consumption is `renderInstrumentPostStripToStereoScratch`, in which the
insert branch renders the instrument at unity gain / center pan before Pre → fader → Post → pan,
and the no-insert branch folds gain/pan into the final add.

### 7.2 Signal flow with proxy substitution (Proposed)

```
                       ┌──────────────── destination track ────────────────┐
MIDI (own + routed) ──► [Primary live: inst.processBlock into scratch_]     │
                       [Proxy: fill scratch_ from published proxy audio     │
                        at the current timeline position]        ◄─ SUBSTITUTION ONLY HERE
                       └──────────────┬────────────────────────────────────┘
                                      ▼   (unchanged, shared, Verified)
                    Pre inserts → fader → Post inserts → pan → bus fan-out
                                      ▼
                            groups → master → device / offline mixdown
```

### 7.3 Substitution contract at the seam (Proposed; seam location Verified)

**Recommended integration level: host-level** — inside
`audioThread_processBlockAndAddToOutputs`, replace only the "clear scratch + `inst.processBlock`"
region with "fill `scratch_` from the published proxy at the current timeline position" when the
selected source is Proxy. Rationale (Audit §4.2): UI/audition MIDI drain, the capture sink, and
diagnostics stay at one boundary, and the offline mixdown path inherits the substitution
automatically because `renderOfflineMixdownBlock` reuses the same helper. The helper-level
alternative remains documented in the Audit and is not preferred.

Contract requirements:

* The substitution MUST leave every downstream stage untouched (PI-002).
* The audio thread acquires an immutable `ProxyPlaybackView` (Proposed) via atomic
  `shared_ptr` exactly like `activeOwner_`; no locks, no allocation on the audio thread (PI-012).
* **Timeline position plumbing (Verified gap):** the mix call currently receives only
  `numSamples`; the seam requires threading the block's timeline start into the instrument-mix
  call or the host (Audit §4.2). This is the one signature-level change the seam needs and belongs
  to slice P1G.
* Proxy read policy (**Locked requirements; mechanism Open — OI-002/SPIKE-03**): playback reads
  MUST be audio-thread-safe (no allocation, file I/O, locks, or resampling on the audio thread)
  and memory-bounded (PI-031). Unconditional full preload of every proxy is **rejected**: at
  48 kHz, stereo 32-bit float audio is ≈23 MB per minute per proxy, so a ten-minute project with
  many unavailable Primaries would demand several gigabytes fully decoded. The fact that no
  streaming reader exists today (§4.6) is a build cost to evaluate, not a license for unbounded
  preload. The likely shape is a bounded hybrid — background read-ahead plus optional full preload
  only below an explicit size/project budget, with rate conversion prepared off the audio thread
  (§15.3) — but the mechanism is selected from SPIKE-03 evidence and blocks P1G.
* When the generation's recorded render rate differs from the engine rate, the view serves the
  derived playback representation (PI-030, §15.3); preparing or rebuilding that representation is
  background/message-thread work and never audio-thread work.
* When Primary is live, behavior is bit-for-bit today's behavior (the substitution branch is not
  taken).

## 8. MIDI dependency and deterministic scheduling contract

### 8.1 Dependency model (Verified)

Strictly one hop: `TrackKind::Midi` row → persisted `midiTo` (`TrackId`) → `TrackKind::Instrument`
row; enforced at every mutation point; cycles structurally impossible (Audit §5.1). Eligibility is
baked at publish time: `playbackEnabled = trackActive_ && powerOn_ && !muted_ && …` (Audit §5.3).
Solo does not exist.

### 8.2 Destination-owned dependency set (Locked, with Recommended mechanism)

A proxy belongs to the instrument destination (PI-006). The **required worked example** (Locked;
matches the verified stability fixture, Audit §5.5):

* Organ owns one VB3-II instance.
* Organ may contain Upper notes on MIDI channel 1.
* Organ Lower routes MIDI To Organ on channel 2.
* Organ Pedal routes MIDI To Organ on channel 3.
* Notes and relevant CC events, including CC11, from all three sources are merged.
* The destination is rendered exactly once through the one Primary instance.
* A render-relevant change in Organ, Organ Lower, **or** Organ Pedal makes Organ's proxy stale.

**Locked (PID-010, reviewed):** one central authoritative enumerator
`midi_dependency::sourcesForDestination(const SessionSnapshot&, TrackId)` (Proposed name, R3)
built in slice P1C, used by snapshot construction and fingerprinting. It MUST reproduce the
verified merge semantics: destination first, then eligible sources in session order.

### 8.3 Deterministic scheduling contract (Verified basis, contract Locked)

The render snapshot MUST reproduce exactly the event stream the live engine would deliver. The
ordering semantics below were **re-verified in source for this revision** (they replace, and are
more precise than, the draft's earlier summary):

**Verified per-source bake order** (`InstrumentTrackController::publishRenderSnapshot`, ~2267–2512):

* Clips are stored in append/load order (`clips_` vector; positional JSON round-trip,
  `ProjectFile.cpp` ~1869–1892) with a stable persisted `InstrumentMidiClipId`. The bake builds
  one plan per non-empty clip, then sorts plans by `startSamples` (~2350–2353).
* Notes inside a clip are stored in vector order (no per-note stable ID exists,
  `ExperimentalMidiPattern.h` ~45–59); the bake emits them in stored order and then sorts by
  `absSample` only (~2344–2347). **Stored vector order is the *intended* semantic tie-break for
  equal-time notes — guaranteed only once ORD-1 lands**, because the current sort is unstable
  (see ORD-1 below); today equal-time order is implementation-defined.
* CC points are kept in a repository-canonical normalized order — `startTick` → `controller` →
  `channel`, duplicates collapsed last-wins (`ExperimentalMidiCcAutomation.h` ~90–157,
  `normalizePoints` with `std::stable_sort`). The bake visits CC clips stable-sorted by
  `startSamples`, appends per-`(controller, channel)` stream events, stable-sorts each stream by
  `absSample` (equal-sample ties keep append order — "last delivered value wins", ~2359–2361,
  ~2426–2435).

**Verified per-segment emission order** (`audioThread_scheduleTransportMidiForSegment`,
~2571–2720): pending/discontinuity **Note Offs** first (~2614–2648), then the **CC pass** (chase +
in-segment, ~2658–2664), then the **note scan** emitting Note Ons *and any same-segment Note Offs*
(~2668–2718). Precisely: a Note Off pending from an earlier segment precedes CC at the same
offset (Off → CC → On), but a Note Off whose Note On occurred **earlier in the same segment** is
emitted during the note scan and therefore lands **after** the CC pass at that offset
(CC → Off → On). The snapshot renderer MUST reproduce this exact emission structure — not an
idealized global Off → CC → On rule.

**Verified merge order across sources:** destination's own content schedules first, then
`TrackKind::Midi` sources in session track order (`InstrumentRuntimeCoordinator.cpp` ~842–864;
`PlaybackEngine.cpp` ~1359–1461); the offline mixdown path shares the same resolution and merge
order (~1862–1920). `juce::MidiBuffer` preserves insertion order at equal offsets and DAL relies
on it (`ExperimentalInstrumentHost.cpp` ~3499–3511). Equal-time CC last-wins therefore depends on
session track order — order is semantic and MUST be captured (F9, HR-4).

**Other verified rules:**

* CC chase: latest event strictly before segment start; no invented default before the first
  point; per-stream dedup (Verified).
* Note Off: explicit `noteOffAbsSample`, else the 100 ms `gateSamples` fallback at the render
  sample rate (Verified, Audit §3.1) — the gate rule inputs are fingerprint field F10.
* Tick→sample conversion uses per-clip `bpm` and TPQ (Verified, Audit §5.4). Notes and CC bake
  from ticks at the **render** sample rate; sample-domain clip anchors/windows convert via the
  recorded timeline reference rate (§10.1) — live-rate baked samples are never reused (HR-9).
* The `MidiDeliveryCaptureSink` seam (Verified) is the reference oracle: tests compare the
  renderer's delivered MIDI against live delivery (T-03, T-04). Note its current assertions cover
  same-offset CC-after-Note-On violations per channel, not full Off/CC/On triples — T-03/T-04
  extend coverage.

**ORD-1 — blocking P1C prerequisite (Verified gap, HR-10).** The two bake sorts above use
**unstable `std::sort` with no tie-breaker** (notes by `absSample` ~2344–2347; clip plans by
`startSamples` ~2350–2353). Equal-key order is thus implementation-defined rather than
semantically guaranteed, and equal-time Note Off/Note On interleaving across clips can audibly
differ (e.g. same-pitch retrigger vs. kill). **Smallest deterministic fix (proposed, not
implemented here):** replace both with `std::stable_sort`, making stored order the documented
tie-break — no musical behavior change for non-equal keys, deterministic behavior for equal keys.
ORD-1 MUST land (as its own reviewed production micro-change) before P1C's fingerprint can claim
to match live delivery; the fingerprint's canonical serialization (§11.4) already encodes stored
order so it is correct once ORD-1 holds.

### 8.4 Mute/enable semantics (Locked + Recommended)

* Source eligibility (source off/mute) gates content and is fingerprinted (F8).
* **Locked (PID-006, reviewed):** destination proxy rendering is independent of the destination's
  current mute/off state — the proxy represents the destination's musical content, not the current
  monitoring choice. Destination mute/off remains a playback-time gate (Verified mix-stage skip)
  and is excluded from the fingerprint.

## 9. Plugin state and identity contract

### 9.1 The problem (Verified, HR-1)

State is captured only at explicit project save; instrument-editor close does not capture
(§4.5). Fingerprints based on the last-saved blob silently miss live tweaks; per-check fresh
`getStateInformation` calls cost message-thread time (Audit H2/U2).

### 9.2 Authoritative state-capture contract (Locked contract; mechanism Locked per §9.4)

The contract (PI-020) requires, whatever the mechanism:

1. The state blob in a render snapshot equals the currently audible Primary state at enqueue time
   (as far as the host can observe — §9.4.5; snapshot eligibility per §9.4.4).
2. A **DAL-observable** change that alters audible output makes the proxy stale (Stale is marked
   immediately on the observed change; only the *start of rendering* is governed by the §18.1
   update mode, e.g. the Auto five-minute idle timer). Changes the plugin never surfaces to the
   host are the accepted E2/observability limitation (§9.4).
3. Rendering can never silently use an older Save-time state.
4. Save, autosave, proxy rendering, and plugin-editor close agree on what "current state" is:
   Save MUST capture fresh state (it already does at save time — Verified); editor close SHOULD
   trigger capture/hint (instrument-editor close currently does not — Verified gap); render
   enqueue MUST capture fresh state.
5. All `get/setStateInformation` calls stay on the message thread (Verified constraint); the audio
   thread is never involved in capture.
6. Undo/dirty: state capture for proxy purposes MUST NOT by itself create undo entries or dirty
   the project (§18.3); the *user's* parameter edit dirties per existing rules.

**Final decision (PID-001, revision 5 — human review 2026-09-06): Locked (reviewed) — hybrid
authoritative-state and host-observed identity contract (§9.4).** Revision 4's resolution
("Resolved, PASS") was premature and is corrected: the SPIKE-01B/SPIKE-01B-M corrective
measurements (final verdict **PARTIAL PASS**) separate the **capture layer** (fresh checkpoint
capture — Resolved on measured evidence) from the **identity layer** (host-managed revision +
lifecycle bumps + save-pairing; bytes never as general identity — now Locked as §9.4). Polling
remains rejected. The decision is resolved by **explicitly accepting the plugin API's fundamental
observability boundary** — if a plugin changes sound-relevant state internally without any
observable parameter change, dirty notification, or other host-visible signal, DAL cannot detect
that change with certainty — not by claiming DAL can detect unknowable internal plugin changes.
See the findings blocks below, §9.4, and the full evidence report.

**SPIKE-01 (named validation spike, blocking P0/P1A gate)** must establish:

* cost and message-thread impact of `getStateInformation` on VB3-II-class plugins;
* whether repeated capture of an unchanged audible state produces **byte-stable** blobs;
* coverage and reliability of host parameter notifications for plugin-GUI-driven changes;
* editor-**open** and editor-**close** behavior (capture opportunities and state visibility);
* behavior for state changes **not** represented by ordinary automatable parameters;
* Save/autosave/enqueue agreement on "current state";
* a safe fallback if state blobs are not byte-stable for identical audible state
  (capture-generation counters plus conservative staleness);
* conservative invalidation behavior that prefers unnecessary re-rendering over silently treating
  changed sound as current.

SPIKE-01 delivers an **evidence report and a proposed steering amendment** for human review; it
MUST NOT implement product behavior. No fingerprint slice (P1C) may complete before SPIKE-01's
outcome is reviewed.

**SPIKE-01 findings (2026-09-05, executed; historical — the original PASS verdict is superseded
by the SPIKE-01B/01B-M PARTIAL PASS below; the measured facts stand except where the later block
corrects them).** Repository evidence + local measurements: 167 message-thread captures, 230
notifications, VB3-II 2.3.1 and Groove Agent SE 5.2.20, Debug build; evidence report
`docs/audits/SPIKE_01_AUTHORITATIVE_PLUGIN_STATE_CAPTURE.md`:

1. DAL contains no plugin parameter/processor listeners anywhere (instruments or inserts);
   live plugin-GUI edits are structurally invisible today and do not dirty the project. This
   dirty gap means tweak-only sessions may never autosave.
2. Save and autosave share one fresh-capture path
   (`buildExperimentalInstrumentProjectBlock` → `getCurrentInstrumentStateBase64`); rule 4 above
   is Verified for autosave and Measured at runtime (raw-vs-Save-path hash MATCH).
3. The instrument editor-close handler (`editorWindowClosing`, message thread,
   staleness-guarded) is a safe hint hook; the insert host already implements an editor-close
   state diff as precedent. Measured: editor open/close alone changes no serialized bytes on
   VB3-II.
4. Capture cost is checkpoint-affordable and message-thread-clean (VB3-II ≈0.5 ms / 10 KB;
   Groove Agent SE ≈5 ms / 148 KB; 100 % message thread, Debug build). Continuous polling stays
   forbidden.
5. VB3-II state blobs are byte-stable at rest in all §9.2 cases including
   save → restart → reload, and revert to baseline bytes when a parameter returns to its
   original value; they are not stable while playback drives the plugin. Groove Agent SE blobs
   are never byte-stable (idle counter/clock churn, zero notifications). Therefore: hash
   equality at quiescent checkpoints is a valid positive "unchanged" proof; hash inequality
   proves nothing and MUST take the conservative stale path (re-render over silent staleness).
6. VST3 parameter notifications (VB3-II) arrive densely and on the message thread for GUI
   edits, and preset changes surface as a full-parameter burst — but no gesture and no
   `audioProcessorChanged` callbacks were ever observed. Listeners are confirmed as hints only;
   non-parameter changes can be completely silent.
7. The authoritative mechanism is measurement-confirmed: fresh capture at checkpoints +
   monotonic generation counter bumped by hints (notifications, editor open/close) + hash
   equality only as an opportunistic short-circuit. Capture failure ⇒ keep previous blob, mark
   proxy stale/unknown.

**Historical status change (revision 4, superseded):** revision 4 marked PID-001's state-capture
mechanism Resolved on the original PASS verdict. That resolution was premature; the final,
reviewed status is in the revision-5 block below.

**SPIKE-01B / SPIKE-01B-M findings (2026-09-05/06, corrective follow-up; measurement verdict
PARTIAL PASS; reviewed and accepted 2026-09-06 — revision 5).** Full evidence: the same report,
§22–§28, including delivery-proven measurements (M2V) and the correction record (§28.0):

1. **One blob cannot serve three roles** (SPIKE01B-F1): authoritative render state, semantic
   validity identity, and volatile/runtime bytes must be separated. Raw blob hashes as general
   validity identity would make every volatile-plugin proxy permanently stale (Groove Agent SE:
   10/10 distinct idle hashes); a bare counter alone leaves silent changes undetected. The hybrid
   contract (§9.4) bounds both failure modes and is the only evaluated policy that does.
2. **Groove Agent SE churns from the first post-restore capture**, and growth correlates with
   capture count, not elapsed time: repeated `getStateInformation` observably perturbs the
   plugin's own next serialized output. The side effect is harmless to the §9.4 validity-identity
   algorithm (volatile bytes are excluded from identity), but **sonic equivalence of the
   successively changing blobs was not measured and MUST NOT be claimed**; capture counts MUST
   remain minimal (diagnostic and production).
3. **Proven MIDI/CC delivery perturbs VB3-II's blob during playback** (M2V: 32 note-ons,
   32 note-offs, 384 CC across three channels delivered to the measured instance; 9/36 in-play
   captures differed from baseline; return to the authored baseline by the first post-stop
   capture in that session). A during-playback capture is never admissible as byte identity
   evidence; snapshot eligibility is defined in §9.4.4.
4. **`host-observable quiescence`** (no host-sent MIDI/CC and no received parameter/dirty/
   non-parameter notification for the debounce window) is the *only* quiescence DAL can
   establish. It does **not** prove internal plugin quiescence; notification silence is never
   treated as proof of plugin rest.
5. **The k-capture classification probe is validated** (k=2 sufficed in every measured burst);
   misclassification risk is asymmetric, not universally harmless (§9.4.3).
6. **E2 stays a documented compatibility limitation:** silent non-parameter authored state
   (MIDI-learn class) is unmeasurable without native-editor automation; DAL's currency claims are
   therefore host-observed, never absolute.

**Status change (approved 2026-09-06, revision 5):** PID-001 → **Locked (reviewed) — hybrid
authoritative-state and host-observed identity contract** (§9.4). Capture layer Resolved;
identity layer locked by accepting the observability boundary explicitly. E2 and broader plugin
coverage remain documented compatibility limitations, non-blocking for P1. P1C's gate is
satisfied.

### 9.3 Plugin identity and version (Verified gap HR-2; Recommended resolution)

The persisted descriptor has no version field (Verified, Audit §7.1/U1). **Recommended:** persist
`PluginDescription::version` at schema v20 (§12); include it in the fingerprint (F1v). A version
change ⇒ fingerprint change ⇒ deterministic staleness. Same-version-different-binary upgrades are
accepted as undetectable in v1 (documented limitation). Alternatives (live-version-only checks,
binary hashing) are noted in PID-001.

### 9.4 Hybrid authoritative-state and host-observed identity contract (Locked, revision 5)

The reviewed SPIKE-01B contract. Evidence authority:
`docs/audits/SPIKE_01_AUTHORITATIVE_PLUGIN_STATE_CAPTURE.md` (§22–§28).

#### 9.4.1 Authoritative render state

* A render snapshot contains the **exact opaque bytes** captured through `getStateInformation` on
  the message thread. No normalization or reinterpretation, ever.
* Those bytes are restored **only into an isolated render instance** (PI-011). The live audio
  instance is never processed by the renderer.
* State-capture cost is suitable for **checkpoints, not continuous polling** (Measured: VB3-II
  ≈0.5 ms / 10 KB, Groove Agent SE ≈5 ms / 148 KB, Debug build).
* **Save-time capture and proxy-render snapshot eligibility are related but not automatically
  equivalent:** Save may capture during playback as today (its blob is restored into the *same*
  instance at load); that precedent does not by itself prove suitability as the initial state of
  a project-start proxy render (§9.4.4).

#### 9.4.2 Semantic validity identity

* **Raw plugin-state bytes MUST NOT be the general proxy-validity identity.**
* The identity is a **monotonic host-managed revision per Primary**, bumped by every
  render-relevant change DAL can observe, including:
  * parameter and processor notifications;
  * non-parameter dirty notifications when the plugin supplies them (`setDirty` →
    `audioProcessorChanged`);
  * plugin editor lifecycle (open/close) per the conservative policy;
  * DAL-mediated preset/program changes;
  * Primary replacement, reload, and state restoration;
  * render-relevant MIDI, CC, and routing edits;
  * applicable project/schema/configuration changes (fingerprint-schema or policy-version bumps).
* **Publication and obsolete-job checks compare the captured revision with the current
  revision — never a newly captured raw blob hash** (PI-028 is evaluated on revisions).
* **Persisted proxy↔saved-state pairing restores load-time validity by construction:** load
  restores the exact saved blob into the instance, so the saved pairing is valid without any byte
  comparison.

#### 9.4.3 Byte-stable and volatile classification

* A small **quiescent k-capture probe** (k = 2–3, milliseconds apart, at host-observable
  quiescence) MAY classify a plugin instance as **byte-stable** or **volatile**. Cached per
  plugin identity+version; re-probed per session.
* **Equal bytes for a byte-stable plugin are positive "unchanged" evidence only** — usable to
  rescue a revision bump (cancel an unnecessary re-render). **Byte inequality never proves a
  semantic sound change** and has no user-facing meaning.
* **Volatile plugins never depend on byte equality** for publication or currency; they publish
  via revision compare like everyone else.
* A stable-classified plugin MAY be **safely demoted to volatile** after repeated qualifying
  at-rest inequality. **Volatile → stable promotion requires a new qualifying probe.**
* Misclassification risk is **asymmetric — neither direction universally harmless**:
  false-volatile loses an optimization or causes extra work (never strengthens a currency claim);
  false-stable can temporarily expose the accepted bounded false-current blind spot.
* **Repeated capture may alter later serialized bytes** (Measured, Groove Agent SE). Capture
  counts MUST remain minimal. **Sonic equivalence of successively changing volatile blobs was
  not measured and MUST NOT be claimed.**

#### 9.4.4 Snapshot eligibility versus concurrent background rendering

* **Background proxy rendering MAY run concurrently with normal transport playback.** It runs
  through an isolated plugin instance; the transport does not need to remain stopped during the
  actual background render.
* **Snapshot capture is a separate operation from rendering.**
* A **project-start render snapshot SHOULD normally be captured at a host-observable-quiescence
  boundary**: no host-sent MIDI/CC to the instance and no received parameter/dirty/non-parameter
  notification for the defined debounce window. **This does not prove internal plugin
  quiescence.**
* **Capture while the destination actively receives MIDI/CC is not proven to provide clean
  project-start initial conditions** (Measured: M2V; no clone/restore/reset/render equivalence
  test was performed for a playback-time blob).
* **P1D MUST validate** the isolated-instance lifecycle:
  `restore → prepare → reset/flush where supported → deterministic MIDI/CC chase → complete
  render from project start`. If that lifecycle cannot remove performance-transient initial
  state for a plugin, snapshot capture MUST wait for an eligible boundary or DAL MUST expose a
  compatibility limitation for that plugin.

#### 9.4.5 Host-observed currency and UI honesty

* For volatile plugins, **"Current" means: no render-relevant change observable by DAL has
  occurred since the accepted snapshot/revision.** It is not an absolute claim about unknowable
  internal plugin activity. `Current (host-observed)` is the **defined semantic concept** for
  this — it is NOT required as the permanent main track label.
* Normal compact UI MAY continue to show **"Proxy current"**. But a status detail, information
  affordance, or tooltip MUST explain, for a volatile plugin, that currency is based on changes
  detected by DAL and that the plugin cannot provide fully verifiable state-change information.
* **Volatile plugins retain normal proxy support** (including Auto mode) and are not restricted
  to Manual-only.
* Currency for *any* plugin is "current as far as the host can observe", never an absolute
  guarantee; DAL never claims to observe internal plugin quiescence.

## 10. Immutable render-snapshot schema (Proposed)

`ProxyRenderSnapshot` (Proposed) is built on the message thread at enqueue, then never mutated.
Draft shape (field names are Proposed; all content derives from Verified structures cited in §11):

```
ProxyRenderSnapshot (immutable, Proposed)
├─ destinationTrackId          : TrackId
├─ pluginIdentity              : { fileOrIdentifier, uniqueId, deprecatedUid, format,
│                                  isInstrument, version }            // F1, F1v
├─ pluginStateBlob             : bytes (authoritative current state)  // F2, via R2
├─ destinationClips[]          : { clipId,
│                                  startSamples, timelineAnchorSamples, lengthSamples
│                                    (sample-domain @ timeline reference rate, §10.1),
│                                  bpm, tpq,
│                                  notes[] {midiNote, velocity, offVelocity, channel,
│                                           startTick, durationTicks}   (tick-domain),
│                                  ccPoints[] {startTick, controller, value, channel,
│                                              interpolationToNext}     (tick-domain,
│                                              normalized order) }   // F3, F4, F5
├─ destinationMidiOutputChannel: int                                  // F6
├─ sources[] (ordered = session order)                                // F7, F9
│    └─ { sourceTrackId, midiOutputChannel, eligibility {off, muted}, // F7, F8
│         clips[] (same shape as destinationClips) }
├─ noteOffGateRule             : { gateMs = 100, renderSampleRate }   // F10
├─ renderConfig                : { sampleRate, blockPolicy,
│                                  timelineReferenceRate }            // F11 (§10.1)
├─ policies                    : { latencyPolicyVersion, tailPolicyVersion,
│                                  renderPolicyVersion, proxyFormatVersion } // F12, F13
├─ spanRule                    : { start = project start (0),
│                                  end = last relevant event + tail } // PID-007 (Locked, reviewed)
└─ fingerprint                 : ProxyFingerprint (computed by R5 from the above)
```

Rules:

* The snapshot stores musical data in its **native persisted domains** (§10.1): notes and CC in
  ticks; clip anchors/windows in samples at the recorded timeline reference rate. It re-bakes to
  samples at the render sample rate using the §10.1 conversion rules (Verified rationale: baked
  samples are live-rate-dependent, Audit §5.4).
* The snapshot is self-sufficient: the render job never reads `Session`, live hosts, or
  controllers after construction (PI-011 isolation, PI-013 safe cancellation).
* Runtime-only data (§11.3) never enters the snapshot.

### 10.1 Timeline domains, reference rate, and conversion (Verified; TLD-1 prerequisite)

Re-verified in source for this revision — the draft's earlier claim that the snapshot re-bakes
purely "from tick-domain data" was **half-true** and is corrected here:

**Genuinely tick-domain (rate-independent as stored):** note `startTick`/`durationTicks`
(`ExperimentalMidiPattern.h` ~55–58), CC `startTick` (`ExperimentalMidiCcAutomation.h` ~59–63),
`ticksPerQuarter`, and tempo values (project `ProjectMusicalTime`, per-clip `pattern.bpm`).

**Sample-domain (integers, no load-time rescale):** MIDI clip `startSamples`,
`timelineAnchorSamples`, `lengthSamples` (`InstrumentTrackController.h` ~38–46); audio clip
placement (`PlacedClip` start/visible length); `arrangementExtentSamples`,
`leftLocatorSamples`/`rightLocatorSamples`, `playheadSamples` (`ProjectFile.h` ~256–263). These
integers were authored at the device rate recorded in the **required** project field
`deviceSampleRateAtSave` (`ProjectFile.h` ~257). File-material fields (`leftTrimSamples`, material
window) index decoded PCM and are not session time.

**Verified current behavior (the hazard, HR-11):** load performs **no rescale** of any
sample-domain field when the device rate differs (`Session.cpp` ~1856–1982;
`setTimelineSampleRate` documents "Does not rescale clips", `InstrumentTrackController.h`
~337–338); tick→sample baking uses the **current** `timelineSampleRate_` (default 48 kHz,
`publishRenderSnapshot` ~2288–2292) via
`samples = round(ticks / tpq · (60 / bpm) · sampleRate)` (`ExperimentalMidiPattern.h` ~101–118);
and **save re-stamps `deviceSampleRateAtSave` to the current device rate without rescaling the
integers** (`Session.cpp` ~1569–1570). Mismatched-rate audio files are skipped entirely at load
(`AudioFileLoader.cpp` ~58–68); offline mixdown runs at the device rate only.

**Conversion contract for the snapshot (Locked):**

* A sample count created at rate A MUST NOT be interpreted unchanged as a sample count at rate B.
* The snapshot records the **timeline reference rate** — the rate under which the session's
  sample-domain fields are currently interpreted at enqueue (`timelineSampleRate_`) — in
  `renderConfig` (F11). Sample-domain anchors/windows convert to the render rate as
  `renderSamples = round(storedSamples · renderRate / timelineReferenceRate)`; ticks bake directly
  at the render rate. Generation validity is evaluated under the generation's recorded
  configuration including this field (PI-030) — a device-rate change alone therefore still never
  produces Stale.
* The fingerprint serializes the sample-domain integers **raw** (they are persisted content,
  stable across save/load and machines); the timeline reference rate lives in F11 (render
  configuration), not in the content fields.

**Timeline reference-rate contract (Locked target; the persisted field and every conversion seam
are Proposed — they do not exist today):**

* The persisted timeline reference rate and the current engine/device sample rate are **separate
  coordinate domains**. They MUST NOT be conflated: one defines what the stored integers *mean*,
  the other defines what the running engine *needs*.
* On v20 load, the persisted timeline reference rate is authoritative for interpreting **all**
  persisted sample-domain timeline fields (MIDI clip anchors/windows, audio-clip placement,
  locators, arrangement extent, playhead).
* Changing the audio device/engine rate MUST NOT overwrite that reference rate and MUST NOT
  reinterpret the stored integers.
* Every boundary that needs engine-rate sample positions MUST convert from the persisted timeline
  domain to the current engine domain
  (`engineSamples = round(storedSamples · engineRate / timelineReferenceRate)`). The proxy
  snapshot conversion above is **one** such boundary. Merely persisting the field fixes nothing by
  itself: **today every runtime timeline consumer interprets the stored integers at the current
  device rate** (Verified: project load passes `device->getCurrentSampleRate()` into
  `setTimelineSampleRate` at every load site — `ProjectIoCoordinator.cpp` ~782 feeding
  ~212/~276/~929/~992; device prepare re-stamps the timeline rate,
  `InstrumentRuntimeCoordinator.cpp` ~163/~624–637; plus edit-coordinator and MIDI-editor call
  sites). These consumers remain unconverted until a slice migrates them.
* Any explicit change of the timeline reference rate is a deliberate migration operation that MUST
  rescale all affected sample-domain fields **atomically** in the same operation. Fingerprints MAY
  change only as part of that defined atomic conversion — never merely because the audio device
  rate changed.
* v19→v20 migration initializes the new field from the stored `deviceSampleRateAtSave`, because it
  is the best available historical reference.
* **Legacy migration limitation (Locked honesty):** migration cannot reconstruct the original
  timing of a v19 project that was previously re-saved at another device rate after its integers
  had already been misinterpreted (the re-stamp destroyed the original reference). v20 pins the
  interpretation from migration onward; it does not repair lost historical information, and this
  document MUST NOT claim otherwise.

**TLD-1 — blocking P1B prerequisite (Verified gap, HR-11).** Because save re-stamps
`deviceSampleRateAtSave` without rescaling, the persisted pair {sample integers, recorded rate}
silently changes meaning when a project is saved on a machine with a different device rate —
current persistence therefore lacks a stable machine-independent reference for sample-domain
placement, which the portable promise (§2.1) requires. **Smallest fix (proposed, not implemented
here):** persist an explicit project `timelineSampleRate` that is initialized once (from
`deviceSampleRateAtSave` on v19→v20 migration) and never silently re-stamped — any future change of
it must rescale the sample-domain integers in the same operation, per the contract above. P1B MUST
include TLD-1 (or an explicitly reviewed equivalent) before P1C fingerprints sample-domain fields;
its completion gate additionally verifies that the persisted domain is actually consumed by the
relevant runtime and proxy-snapshot boundaries and that remaining unconverted sample-domain
consumers are enumerated and assigned to a blocking slice before P1J cross-rate acceptance (§22
P1B, T-21). This prerequisite is recorded here on its own and deliberately **not** concealed
behind the sample-rate playback policy (§15.3).

## 11. Canonical fingerprint specification

### 11.1 Must-fingerprint fields (Verified field inventory, Audit §10.1 — preserved in full)

The Audit's classification is preserved entry-for-entry; F1v/F13 extend it per Locked latency and
format policies. Every field below is a fingerprint input.

| # | Field(s) | Source of truth | Notes |
|---|---|---|---|
| F1 | Primary plugin identity: `fileOrIdentifier`, `uniqueId`, `deprecatedUid`, format, `isInstrument` | `ProjectFileGenericVst3DescriptorV1` / live `lastLoadedPluginDescription_` | Verified |
| F1v | Plugin version (explicitly defined version identity) | `PluginDescription::version`, persisted at v20 (Recommended §9.3) | resolves Audit U1 |
| F2 | Primary plugin **state blob** (current, not last-saved) as snapshot content, with its **identity component represented by the host-managed state revision** (§9.4.2) — raw blob bytes/hashes are never the general validity identity; byte equality serves only as positive "unchanged" rescue for byte-stable-classified plugins at host-observable quiescence (§9.4.3) | authoritative capture via R2 (§9.2, §9.4) | mechanism **Locked** (revision 5, §9.4; Audit U2) |
| F3 | Destination's own MIDI clips: per clip `id`, `startSamples`, `timelineAnchorSamples`, `lengthSamples` (sample-domain raw integers, §10.1), pattern `bpm`, `ticksPerQuarter` | `InstrumentMidiClip` | tempo & tick→sample conversion inputs; sample-domain integers serialize raw — their interpretation rate is F11's timeline reference rate, never re-baked into the content fields; Audit U4: per-clip bpm is fingerprinted (Recommended: confirmed), project-bpm sync edits surface as clip-bpm content changes |
| F4 | Every note: `midiNote`, `velocity`, `offVelocity`, `channel`, `startTick`, `durationTicks` | `TimelineMidiNote` | note timing, length, velocity, channel, Note Off semantics |
| F5 | Every CC point: `startTick`, `controller`, `value`, `channel`, `interpolationToNext` | `MidiCcPoint` | CC ordering/interpolation; chase-relevant state derives from these (§8.3) |
| F6 | Destination `midiOutputChannel` (effective-channel remap baked into events) | `Track` | native vs effective channels |
| F7 | The routed-source **set**: every `TrackKind::Midi` row with `midiDestinationTrackId == dest`, incl. each source's F3–F5 content and its own `midiOutputChannel` | `Track` + source clips | MIDI To destinations; complete dependency graph (PI-009) |
| F8 | Per-source eligibility gating MIDI: source `trackOff` (power), source `muted` | `Track` / controller `playbackEnabled` inputs | mute/enable semantics (source side) |
| F9 | Merge order: session track order of contributing sources | `SessionSnapshot` row order | equal-time CC last-wins depends on it (HR-4); track reorder ⇒ stale |
| F10 | Note-off gate rule inputs: 100 ms `gateSamples` derivation and render sample rate | `publishRenderSnapshot` ~2293 | Verified |
| F11 | Render sample rate, fixed render block policy, and timeline reference rate (§10.1) | render policy (Proposed, §15.3/§15.4) + `timelineSampleRate_` at enqueue | block-size-sensitive plugins; render rate and timeline reference rate are **generation identity** — validity of an existing generation is evaluated under its recorded configuration, never by substituting the current device rate (PI-030) |
| F12 | Tail-policy version and latency-policy version | render policy (Proposed, §15.1/§15.2) | Locked inclusion (PI-014, PI-016) |
| F13 | Proxy format version and render-policy version | render policy (Proposed, §15.5) | Locked inclusion (product decision §11 list) |

### 11.2 Excluded — after the process boundary (Verified, Audit §10.2 — preserved)

`channelFaderGain_`, `stereoPan_`, insert chains (`PluginTrackChain` slots incl. `opaqueState`),
`sends_`, `routedOutputTrackId_` (audio bus), Group/Master strips and their inserts, master fader,
device playback/recording offsets (`LatencySettingsStore`). Destination-side *audio* gates
evaluated at playback time (mix-stage skip on destination mute/off) are excluded per the
Recommended PID-006 resolution of Audit U3 (§8.4).

### 11.3 Runtime-only and irrelevant (Verified, Audit §10.3 — preserved)

UI/audition MIDI (`uiPendingMidi`), preview players, selection, undo stacks (`SessionHistory`),
dirty flags, editor windows, roll viewport fields (persisted but view-only), CC-lane view state,
track names/`activeTrackId`, diagnostics, autosave state, `InstrumentTrackRenderSnapshot::revision`
(process-lifetime counter — usable only as a cheap runtime change hint, never persisted identity),
capture sinks.

### 11.4 Canonical serialization rules (Locked)

The fingerprint is a hash over a canonical byte serialization of the snapshot:

* **Stable field order:** fields serialize in the fixed F-number order above; within structures,
  in documented declaration order tagged by the fingerprint schema version.
* **Stable collection ordering (matches verified live scheduling, §8.3 — not an invented sort):**
  * **Clips** serialize in the bake's plan order: stable sort by `startSamples` over the stored
    clip sequence (equal-start ties keep stored order — the delivery tie-break once ORD-1 holds).
    `InstrumentMidiClipId` serializes as a data field, **not** as an ordering key: live scheduling
    never consults it. Reordering equal-start clips changes delivery and therefore the
    fingerprint; reordering different-start clips does not change either.
  * **Notes** serialize in **stored vector order** within their clip (positional order is
    persisted and round-trips, `ProjectFile.cpp` ~1869–1892). Stored order is the equal-time
    delivery tie-break once ORD-1 holds (§8.3), so it is fingerprint data. Deliberately
    conservative consequence: a stored-order permutation of *different*-time notes changes the
    fingerprint without changing delivery — accepted for v1 (unnecessary re-render, never a
    missed staleness).
  * **CC points** serialize in their repository-canonical normalized order (`startTick` →
    `controller` → `channel`, duplicates already collapsed last-wins by `normalizePoints` —
    Verified §8.3); no additional sort is invented.
  * **Sources** strictly in session order (F9 — order itself is data), destination's own content
    first.
* **Deterministic numeric encoding:** integers as fixed-width little-endian; doubles (bpm) as IEEE
  754 bit patterns; no printf/locale formatting anywhere (PI-019).
* **String and path normalization:** plugin `fileOrIdentifier` normalized to forward slashes;
  identity strings hashed as UTF-8 bytes; display names never included.
* **Version tags:** the serialization begins with {fingerprint algorithm id, fingerprint schema
  version}; any future field change bumps the schema version.
* **Invalidation after change:** a fingerprint schema/algorithm bump deterministically marks all
  existing proxies stale (they were hashed under the old schema); assets remain retained per
  PI-007.
* **Hash algorithm (Recommended):** a fixed 256-bit cryptographic hash (e.g. SHA-256) — collision
  behavior must be negligible because the hash is also the asset generation identity (§16).
* Forbidden inputs: pointer identity, unordered-container iteration order, display names,
  locale-sensitive formatting, incidental JSON key ordering (PI-019).

### 11.5 Unresolved entries from the Audit (Audit §10.4 — preserved with dispositions)

| Audit # | Question | Disposition in this document |
|---|---|---|
| U1 | Plugin version not persisted | Recommended: persist at v20, fingerprint as F1v (§9.3, PID-001) |
| U2 | State-capture cadence/mechanism | **Locked (reviewed, revision 5)** — SPIKE-01 + corrective SPIKE-01B/01B-M completed (final measurement verdict PARTIAL PASS); hybrid authoritative-state and host-observed identity contract locked by human review 2026-09-06 (§9.4, PID-001). Revision 4's "Resolved (PASS)" was premature (historical) |
| U3 | Destination mute/off during render | Locked (reviewed): render independent of mute/off (§8.4, PID-006) |
| U4 | Clip bpm vs project bpm | Recommended: fingerprint per-clip bpm (source of truth for baking); project-bpm sync edits are content changes (F3) |
| U5 | Proxy render sample rate | Locked (reviewed): render rate is generation identity; validity evaluated under the generation's recorded configuration; cross-rate playback required (§15.3, PI-030). Adaptation mechanism **Open** (OI-002/SPIKE-03) |
| U6 | Span rule | Locked (reviewed): project start → last relevant event + tail (§15.6, PID-007) |

## 12. Proxy metadata and draft project-schema design

Everything in this section is **Proposed** (draft for v20; no schema change is made now).

### 12.1 Placement

Additive fields on each `experimentalInstrumentTracks[]` entry, using the established absent-key
default seam (Verified §4.4), with `ProjectFileV1::kCurrentVersion` bumped 19 → 20 in the slice
that introduces them (P1B). Older readers of v20 files are already rejected by version range
checks; v19 files load into v20 code with all proxy fields defaulted to "no proxy".

### 12.2 Draft fields (Proposed)

```jsonc
// inside experimentalInstrumentTracks[i] — DRAFT, names not final
"pluginVersion": "1.4.0",            // F1v (Recommended, §9.3)
"proxy": {                            // absent => no proxy
  "generationId": "sha256:…",        // == fingerprint hash of the published render
  "fingerprintSchemaVersion": 1,
  "relativePath": "InstrumentProxies/track_12_sha256-….wav",
  "sampleRate": 48000,                // recorded RENDER rate — generation identity (PI-030):
                                      // playback at another engine rate adapts, never invalidates
  "lengthSamples": 12345678,          // at the recorded render rate
  "pluginLatencySamples": 256,        // PI-014: recorded, never pre-trimmed; at the render rate
  "latencyPolicyVersion": 1,
  "tailPolicyVersion": 1,
  "renderPolicyVersion": 1,
  "proxyFormatVersion": 1,
  "renderedUtc": "2026-09-03T12:00:00Z"
  // NOTE deliberately absent: no "tailTruncated" field. A render that reaches the tail cap while
  // output remains materially non-silent is an incomplete render and is NEVER published as a
  // generation (§15.2); tail-limit diagnostics live on the failed job, not on published metadata.
},
"proxyUpdateMode": "auto"            // "auto" | "onSave" | "manual" | "off" (§18.1, revision 5)
// Proposed additions per §9.4 (names not final): host-observed identity metadata required across
// load (e.g. the accepted revision pairing for the published generation), and stable/volatile
// classification info only where safe and version-qualified (plugin identity+version-keyed;
// re-probed per session — never trusted blindly across plugin updates).
```

### 12.3 Rules

* Proxy audio itself is never embedded in JSON (PI-024) — the schema stores metadata + relative
  path only.
* Current-vs-stale is **not** persisted as a boolean: it is computed by comparing `generationId`
  against a freshly built expected fingerprint at load and after edits (PI-018; a persisted flag
  would rot). The expected fingerprint is built **under the generation's recorded render
  configuration** (its `sampleRate` and policy versions), never by blindly substituting the
  current device rate (PI-030): the comparison asks "does this generation still represent the
  current musical content, under its own render configuration?" — an engine-rate difference alone
  therefore never produces Stale.
* **Plugin-state component (revision 5, §9.4.2):** the expected fingerprint's state component is
  the host-managed revision / persisted save-pairing — **never a freshly captured raw blob hash**.
  At load, the persisted proxy↔saved-state pairing restores validity by construction; after edits,
  currency follows the revision compare. Freshly captured bytes MAY only *rescue* an unnecessary
  re-render for a byte-stable-classified plugin at host-observable quiescence (positive equality
  evidence, §9.4.3); byte inequality never demotes or proves anything.
* Missing/corrupt referenced file at load ⇒ status degradation, project loads (PI-025).
* Musical undo continues to strip plugin blobs (Verified) and MUST also strip proxy metadata from
  undo comparison where it would create meaningless entries (§18.3).

### 12.4 Secondary naming discipline (PID-009 — reviewed, deliberately deferred to P2)

Human review explicitly accepted that PID-009 does not block P1. P1B MUST NOT add empty Secondary
fields, placeholder objects, or speculative registry structures. The only P1 obligation is
negative: schema naming merely avoids unnecessarily consuming an obvious future namespace (e.g. do
not name an unrelated field `secondaryInstrument`). The actual embedded-block versus role-list
decision is made during P2 design using then-current architecture evidence; it remains technically
unresolved until then.

## 13. Track proxy state and render-job state machines

Two distinct machines (Proposed). The destination's *proxy status* is derived, user-facing; a
*render job's* execution state is internal to the queue.

### 13.1 Destination proxy status (derived)

| Status | Meaning |
|---|---|
| Absent / not requested | No proxy metadata/asset (or mode Off with none existing). |
| Current / published | Published generation's `generationId` equals the expected fingerprint built under that generation's recorded render configuration (§12.3; plugin-state component via the host-managed revision, never a fresh blob hash — §9.4.2). "Current" is host-observed for the plugin-state part: no DAL-observable render-relevant change since the accepted snapshot/revision (§9.4.5; for volatile plugins the UI exposes this explanation). A generation whose recorded render rate differs from the current engine rate is still **Current** — it plays through the derived playback representation (PI-030); the mismatch never demotes the status. |
| Stale | A retained generation exists but the *musical* content no longer matches (PI-007). Never caused by an engine-rate difference alone. |
| Rendering | A job for the current fingerprint is queued/preparing/rendering/finalizing. |
| Failed | The most recent job for the current fingerprint failed (including a diagnosed incomplete render at the tail cap, §15.2). The previous published generation (if any) is retained — never deleted or overwritten — and its own status remains a pure fingerprint verdict: typically Stale (the edit that triggered the job made it non-matching, so it is never selected for playback in P1), but still Current and playable if the fingerprint still matches (e.g. a failed native-rate quality refresh, or the triggering edit was undone). The failure is surfaced as detail alongside that verdict. |

### 13.2 Render-job execution states

`Queued → Preparing → Rendering → Finalizing → Published` with exits to `Cancelled` (user/mode
action), `Obsolete` (fingerprint changed; superseded by a newer job), `Failed` (error). Terminal
states: Published, Cancelled, Obsolete, Failed.

* **Queued:** snapshot + fingerprint captured; waiting for the worker (pause conditions §14.3).
* **Preparing (message thread):** create isolated instance, restore state, `prepareToPlay`.
* **Rendering (worker):** block loop with cancellation/obsolescence checks per block (PI-013).
* **Finalizing:** tail completion, temp-file validation, latency metadata. If the maximum-tail
  limit was reached while output remained materially non-silent, finalizing MUST route the job to
  **Failed** with the diagnosed reason "tail limit reached — render incomplete" (§15.2); the
  truncated temp output MAY be retained for diagnostics but is never a publishable generation.
* **Published (message thread):** currency check (fingerprint still matches) then atomic
  publication (PI-028, §16.3).

### 13.3 Race handling (Locked behaviors)

| Race | Required behavior |
|---|---|
| Edit during rendering | New fingerprint/revision ⇒ running job becomes Obsolete at its next check. In Auto mode the edit restarts the destination's five-minute idle interval before a new job may start; in On Save the next explicit Save queues it; in Manual/Off the track simply shows Stale (§18.1). The obsolete job MUST NOT publish (PI-028; the currency check compares revisions, never a fresh blob hash — §9.4.2). |
| Newer job supersedes older | Older job cancelled cooperatively; only the job whose fingerprint matches current content may publish. |
| Track deletion | Jobs for that destination cancelled; assets retired per cleanup policy (§16.5); engine-side removal follows existing publish-before-destroy + callback-drain discipline (Verified, Audit §3.2). |
| Primary removal (descriptor cleared) | Jobs cancelled; existing generations retained (they still represent the last authoritative sound — status Stale); no new renders until a Primary exists. |
| Routing change (`midiTo` edit) | Fingerprint changes for old and new destinations ⇒ both stale per PI-009. |
| Engine sample-rate change | Per §15.3 (PI-030): the rate change alone never makes a generation Stale — an existing generation remains Current as long as its musical fingerprint still matches under its own recorded render configuration; only its derived playback representation is invalidated and rebuilt off the audio thread. Jobs in flight continue — their generation identity is their own recorded render configuration. When Primary is available, Auto mode MAY queue a native-rate quality refresh; it is never required for playback. |
| Save As / project move | Assets copied/relocated with the project (§16.6) before the new location is considered complete; fingerprints are path-independent so validity is preserved. |
| Project close | Cancel-all, bounded worker join, temp cleanup; never blocks on rendering completion (PI-023 spirit); queued work is re-derivable from fingerprints on reopen. |
| Application shutdown | Same as close plus instance destruction on the message thread (§6 R6/R7). |
| Publication failure | The previous published generation is never deleted or overwritten; its metadata and asset are retained. It remains Current and playable only if its fingerprint still matches current musical content; otherwise it is retained as Stale and never selected for playback in P1 (PI-007). Job Failed; status surfaces per §20 (PI-024). "Current" is never a synonym for "latest file on disk". |
| Application crash | On next launch: temp files swept, metadata validated against on-disk assets; missing/corrupt ⇒ PI-025 degradation (§16.4). |

## 14. Threading and plugin lifecycle

### 14.1 Thread map (Proposed, built on Verified constraints)

| Work | Thread |
|---|---|
| Snapshot build, fingerprint compare, queue control, publication, metadata | Message |
| Plugin create / `setStateInformation` / `prepareToPlay` / `releaseResources` / destroy (render instance) | Message (Verified DAL guard, §4.5) |
| Render `processBlock` loop, temp-file writing, silence detection | One low-priority worker (Recommended, PID-004) |
| Proxy playback reads | Audio thread, lock-free acquire of immutable view (R10) |

**Verified basis for the division:** the mixdown path already proves `processBlock` off the device
thread for an exclusive, prepared instance (message thread today); JUCE permits a non-device
thread when the caller guarantees exclusive prepared access (Audit §7.3 [Inference]). Worker-thread
*construction* is NOT assumed safe; construction stays on the message thread. SPIKE-02 validates
the create-on-message / process-on-worker handoff before P1D relies on it.

### 14.2 Render job algorithm (Locked sequence)

1. Capture an immutable snapshot (§10).
2. Create a separate Primary plugin instance (never the live one — PI-011).
3. Restore the snapshotted Primary state.
4. Prepare the render instance with the selected render configuration (sample rate, block policy;
   offline/non-realtime indication where supported — §15.4).
5. Process the complete destination MIDI event stream (§8.3 contract).
6. Apply the defined latency and tail policies (§15.1, §15.2).
7. Write a temporary proxy asset.
8. Validate the rendered asset (readable, expected length, format).
9. Verify that the destination's current fingerprint still matches the job.
10. Publish only if the job remains fingerprint-current (PI-028).
11. Discard obsolete or failed temporary output safely.

### 14.3 Execution policy (Locked capabilities, Recommended parameters)

The architecture MUST allow: offline/non-realtime plugin indication where supported; controlled
realtime fallback for incompatible plugins; low-priority execution; limited concurrency; render
progress and speed reporting; cooperative cancellation/obsolescence checks at safe block
boundaries; pausing during recording; optional pausing during playback if required to avoid
glitches; safe project close and application shutdown. Faster-than-realtime rendering is
normal-case but **not guaranteed**. **Recommended:** one worker, concurrency 1 (PID-004);
pause-during-playback MAY default ON until SPIKE-02 measures contention — but this is purely a
**resource-contention safeguard**, never a correctness requirement: background rendering with an
already eligible snapshot is architecturally allowed during transport playback (§9.4.4), and the
transport never needs to remain stopped throughout a render.

### 14.4 What is reused vs rejected from existing offline code (Verified basis)

Reused (contracts demonstrably safe): snapshot-capture discipline; the offline block algorithm
shape (segment loop → schedule → process → write) as reference; the `instrumentForceDiscontinuity`
CC-chase contract; the shared CC evaluator (`midi_cc::collectCcEventsInTickRange`); WAV writer
plumbing; temp-sibling publication patterns. Rejected (PI-017, HR-5): live host/insert instance
reuse; the offline render gate; message-thread blocking; shared engine scratch pools; transport
stopping. Generation/staleness precedents to imitate: `Session::loadGeneration_`,
`AsyncCallbackGuard` (Verified, Audit §7.4).

## 15. Latency, tail, sample-rate, block, and render-format policies

### 15.1 Latency (Locked — proxy v1 latency preservation)

* Proxy v1 preserves the Primary plugin's actual output latency so proxy playback matches DAL's
  existing live Primary behavior (which today has no PDC — Verified §4.6).
* The renderer MUST NOT independently shift rendered audio earlier (no latency trim).
* The plugin's reported latency (`getLatencySamples` on the prepared render instance) MUST be
  recorded in proxy metadata (`pluginLatencySamples`).
* `latencyPolicyVersion` MUST be in the fingerprint (F12): a future policy change deterministically
  invalidates or reinterprets proxies (PI-015).
* Switching live Primary ↔ Proxy MUST NOT introduce a timing jump caused by different latency
  treatment (T-15).
* Full PDC is deferred and MUST cover live instruments, proxies, inserts, groups, master routing,
  and mixdown consistently when it arrives. `LatencySettingsStore` is device playback/recording
  offset only and does **not** provide PDC (Verified).

### 15.2 Tail (Locked structure; numeric values Open)

* The renderer MUST continue past the final MIDI event until deterministic silence detection
  triggers or the maximum-tail limit is reached.
* Silence detection (structure Locked): peak amplitude below threshold X for a continuous window
  Y ⇒ tail complete. X, Y and the max-tail cap Z remain **evidence-gated by SPIKE-02** (PID-005).
  −60 dBFS, 1 s, and 30 s MAY be used as experimental starting points; they are not canonical
  defaults.
* **Known tail truncation never becomes Current (Locked, reviewed).** Reaching the maximum-tail
  limit while output remains materially non-silent is a **diagnosed incomplete render**:
  * the incomplete render MUST NOT be published as the Current authoritative proxy;
  * the previous valid generation remains retained (PI-007);
  * the job terminates in a clearly non-current state — `Failed` with reason "tail limit reached —
    render incomplete" (§13.2) — surfaced to the user as a needs-attention status (§19, §20);
  * a later action MAY allow increasing the tail limit and retrying;
  * any future explicit "accept truncated result" feature is deferred and MUST NOT occur silently;
  * the truncated temp/output file MAY be retained for diagnostics where safe, but it is never an
    authoritative playback generation and never enters published metadata (§12.2).
* `tailPolicyVersion` MUST be in the fingerprint (F12); changing X/Y/Z bumps it ⇒ deterministic
  staleness.

### 15.3 Sample rate (Locked policy — OI-001 resolved by review; adaptation mechanism OI-002)

Human review rejected "rate mismatch ⇒ stale/silent". Cross-sample-rate proxy playback is a
**Locked product requirement** (PI-030). The design distinguishes two layers:

1. **The authoritative proxy generation** and the render configuration that produced it.
2. **The derived playback representation** prepared for the current engine sample rate.

Locked contract:

* **Current repository behavior (Verified):** everything renders at the current device rate;
  render snapshots bake ticks→samples at the live rate; mixdown renders at device rate only
  (Audit §5.4, §6.1). No resampler exists in the playback path today (build cost, not policy).
* **Render rate:** v1 renders at the project's current engine sample rate at enqueue time. The
  generation **records its render sample rate** in metadata (§12.2), and the render rate remains
  part of generation identity (fingerprint F11) — Primary output may genuinely depend on sample
  rate. The render configuration also records the timeline reference rate used to interpret
  sample-domain placement fields (§10.1, TLD-1).
* **Validity:** an existing generation is evaluated using **its own recorded render
  configuration**, never by blindly substituting the current device rate into the expected
  fingerprint (§12.3). A rate mismatch alone MUST NOT make unchanged musical content stale, force
  silence, require Primary to be installed, or modify the authoritative asset.
* **Playback at a differing engine rate:** DAL prepares a compatible derived playback
  representation **outside the audio thread** (background/offline preparation is the preferred
  direction). The original generation remains immutable. No resampling allocation, file I/O, or
  expensive preparation occurs on the audio thread (PI-031).
* **Engine-rate change:** invalidates and rebuilds the **derived playback representation** only —
  never automatically the underlying musical proxy. In-flight render jobs continue (§13.3).
* **Native-rate refresh:** if Primary is available, DAL MAY later create a native-rate generation
  as a quality refresh; native re-rendering is never required before playback.
* **Portable case (Locked):** missing Primary plus a differing device rate MUST still produce
  proxy playback — this is precisely the portable-project use case the feature exists for.
* **Mechanism (Open — OI-002):** the concrete resampler placement, caching strategy, and read
  mechanism are evidence-gated by SPIKE-03 and block P1G. No exact resampler API is mandated
  before validation.
* **Required validation:** T-17, T-25 (§23).

### 15.4 Block policy (Recommended)

Fixed render block size (e.g. 512 samples), recorded as part of `renderPolicyVersion`; included in
the fingerprint via F11 because some plugins are block-boundary sensitive. Offline/non-realtime
indication (`setNonRealtime(true)` / VST3 offline processing) is applied where the plugin supports
it; no such usage exists in the repo today (Verified §4.6), so SPIKE-02 validates behavior per
plugin and defines the realtime-fallback trigger.

### 15.5 Render format (Recommended)

Stereo WAV (32-bit float recommended) matching the boundary signal (the host scratch is stereo —
Verified §4.1). `proxyFormatVersion` in metadata + fingerprint (F13) allows future format changes
to invalidate deterministically.

### 15.6 Span rule (Locked, PID-007 reviewed)

Render from project start (sample 0) through the last render-relevant event of the destination's
dependency set, plus tail. Rationale: CC chase and held state make mid-timeline starts unsafe
(PI-010 reasons); rendering from zero guarantees the render instance experiences exactly the event
history the live engine would produce. The span end is part of the snapshot; the span rule version
is folded into `renderPolicyVersion`.

## 16. Media lifecycle and atomic publication

### 16.1 Layout (Locked, PID-003 reviewed)

* Separate project-relative folder `<ProjectFolder>/InstrumentProxies/` (Proposed name), a sibling
  of `Audio/` — the `Audio/` folder has user-clip semantics baked into save validation (Verified,
  Audit §8.4), so proxies MUST NOT live there.
* File naming (Proposed shape): `track_<TrackId>_<fingerprintHash>.wav` — stable `TrackId`
  ownership + content-addressed generation identity (Locked model: immutable content-addressed
  generations, PID-003).
* Generations are immutable: a file, once published, is never rewritten in place.

### 16.2 Why immutable generations (Verified constraint)

Windows file replacement requires delete+move when a reader may hold the old file (Verified, Audit
H10). The engine may be streaming/preloading a proxy at publication time; therefore publication is
**publish-new-name + atomic pointer swap + retire old** (mirroring the C4B scratch-retirement
discipline), never in-place replacement.

### 16.3 Publication sequence (Locked)

1. Worker writes `InstrumentProxies/tmp_<jobId>.wav` (temp name never matches a generation name).
2. Finalize + validate (§14.2 steps 7–8).
3. On the message thread: verify job currency (PI-028), then `moveFileTo` the temp to its final
   generation name (new name ⇒ no collision with any open handle).
4. Publish the new `ProxyPlaybackView` (R10) and update metadata (R1).
5. Retire the superseded generation: remove it only after no acquired view can reference it
   (retirement follows the snapshot-retain discipline of §4.2).
6. Failure at any step: temp deleted (or swept later); the previous published generation is never
   deleted or overwritten — its metadata and asset are retained, and whether it is Current or
   Stale remains a pure fingerprint verdict (§12.3): Current and playable only while it still
   matches current musical content, otherwise a Stale retained asset that is never selected for
   playback in P1 (PI-007, PI-024).

### 16.4 Recovery (Locked behaviors)

* **Missing/corrupt proxy at load:** keep the track row, degrade to Missing/Stale status with a
  diagnostic; never block load (PI-025) — the stronger variant of the existing skip-on-missing
  media policy (Verified §4.4).
* **Crash recovery:** on launch, sweep `tmp_*.wav`; validate metadata against on-disk files;
  orphaned generations (no metadata reference) are removable by cleanup.
* **Cleanup:** keep at most the most recently published generation plus the retained previous
  valid generation per destination (Recommended); orphan sweep at load/save. No general media GC
  exists to reuse (Verified §4.6) — this is new, scoped code.

### 16.5 Track deletion

Assets of a deleted destination become orphans; removed by cleanup after the deletion is committed
(undo of track deletion within the session SHOULD restore metadata; the asset survives until
cleanup, making undo cheap — Recommended).

### 16.6 Save As / project move / `Prepare Portable Project` (Locked, PID-011 reviewed — P1)

Save As MUST copy current (and retained previous) generations into the new project folder before
reporting success — noting that **no Save As media copy exists today** (Verified §4.6), so this is
new behavior in P1H.

**`Prepare Portable Project` is part of P1** (slice P1J): portable packaging is part of the
product promise, not a later convenience. Locked behavior:

* It is an **explicit, cancellable operation**. Its progress UI remains responsive; it MUST NOT
  block the message thread.
* It validates **all required destination proxies** using the authoritative fingerprints and asset
  validation (§11, §16) — never a weaker heuristic.
* A track in mode **Off with a current proxy is acceptable**; a track in mode Off with a stale,
  missing, corrupt, or failed proxy is a **blocker**.
* The operation offers an explicit **one-shot "render required proxies for this package"** action
  that renders what the package needs **without silently changing the track's persisted update
  mode**.
* If Primary is missing or a render cannot succeed, the operation **reports the blocking tracks**
  and does not publish a supposedly complete portable package.
* Package publication occurs **only after all required proxies and media have been validated**.
* **Cancellation or failure leaves no partially published package** (temp staging + atomic
  publication, following §16.3 discipline).
* Plugin binaries, licenses, activation data, and private licensing material are **never**
  included (PI-026).
* The exact package container format (folder copy vs archive) remains a later technical choice
  (Recommended: decide in P1J from packaging evidence); repository evidence does not determine it
  today.

The packaged project MUST be portable across machines and sample rates: a package produced on a
48 kHz machine plays on a 44.1 kHz machine without Primary installed (PI-030; verified end-to-end
in P1J, T-27).

## 17. Playback-source decision table

Automatic transport selection (Locked priority, PI-021). "Current proxy" means published
generation fingerprint == current fingerprint.

| Primary usable | Proxy status | Secondary usable (P2) | Transport source | Exposed status (PI-022) |
|---|---|---|---|---|
| yes | any | any | **Primary live** | Primary |
| no | Current (any recorded render rate — PI-030) | any | **Proxy** (rate-adapted when needed) | Proxy current |
| no | Rendering (none published current) | yes | Secondary live | Proxy rendering (+ Secondary as source) |
| no | Rendering | no | silent | Proxy rendering |
| no | Stale (retained) | yes | **Secondary live** (the stale proxy is never selected; no explicit stale-playback feature exists in P1) | Proxy stale (+ Secondary as source) |
| no | Stale | no | silent (honest Stale status) | Proxy stale |
| no | Failed (no current) | yes | Secondary live | Render failed (+ Secondary) |
| no | Failed | no | silent | Render failed |
| no | Absent | yes | Secondary live | Secondary |
| no | Absent | no | silent | **Missing** (explicit missing/silent state) |

Notes:

* Until P2 ships, every "Secondary usable = yes" row collapses to its "no" sibling (PI-005).
* A **Current** proxy whose recorded render rate differs from the engine rate plays through the
  derived playback representation (PI-030, §15.3) — a rate mismatch never demotes a row to silent.
* **Audition split (Locked for the first P2 version — PID-008 reviewed):** transport playback and
  live audition are separate concerns; audition (UI FIFO) requires a live instrument, and a proxy
  cannot audition arbitrary new notes (Verified constraint, Audit §9). In the first P2 version:
  * when transport is **stopped** and Primary is missing, Secondary MAY provide live audition of
    newly played notes;
  * while a current Proxy is supplying transport playback, DAL MUST NOT mix live Secondary
    audition on top of that Proxy — the UI explains that live audition is unavailable during Proxy
    transport playback (§19);
  * if a musical edit makes the Proxy stale, the source-priority table above may select Secondary
    as the working transport sound, at which point the track clearly shows Secondary as its
    source. This avoids silently combining the authoritative Primary proxy sound with an
    approximate Secondary sound. Proxy v1/P1 contains no Secondary implementation (PI-005).
* Source changes MUST be surfaced, never silent (PI-021); the seam publishes a status event on
  every mode transition (R11).
* Offline mixdown renders through the same selected source automatically at the host-level seam
  (Verified propagation, §7.3; T-22).

## 18. Save, autosave, undo, and dirty semantics

### 18.1 Update modes (Locked, revision 5)

> **Historical (superseded by revision 5):** earlier revisions defined three modes
> (Auto / Paused(Manual) / Off) with a short ~2 s edit debounce before automatic rendering. That
> model is replaced by the four selectable modes below; the short debounce no longer exists —
> automatic rendering never begins a few seconds after an edit.

Four selectable per-destination modes. In **every** mode, staleness detection remains active and
a render-relevant change marks the destination proxy **Stale immediately** (the status verdict is
never delayed by the idle timer — only the start of rendering is).

**Auto after idle (recommended default for portable-collaboration projects; selectable, not
mandatory):**

* A render-relevant change marks the destination proxy Stale immediately **and starts or resets a
  per-destination five-minute idle timer**.
* No render begins until that destination has received **no new render-relevant edit for five
  continuous minutes**. The P1 delay is **fixed at five minutes**; future versions MAY make it
  configurable.
* Activity on unrelated tracks does **not** reset the destination's timer.
* Recording pauses eligibility and render work per the existing scheduler policy (PI-013).
* Playback does not prohibit background rendering once an eligible snapshot exists (§9.4.4).
* A new relevant edit during rendering makes the running job obsolete (PI-028) and starts a new
  five-minute interval.
* Only one queued or running job may exist per destination; multiple destinations becoming
  eligible are serialized through the single P1 render worker (PID-004).
* An explicit Save does **not** force every stale Auto destination to render: it queues only work
  already eligible under the Auto policy and creates no duplicate jobs.
* Autosave **never** starts proxy rendering.

**On Save:**

* Render-relevant edits mark proxies Stale immediately; **no idle timer starts rendering**.
* An explicit user Save queues the latest stale eligible destinations.
* Autosave does not queue rendering.
* Save remains non-blocking (PI-023); duplicate or superseded work is coalesced.

**Manual:**

* Rendering starts only from **Update Proxy**, **Retry**, or **Prepare Portable Project**.
* Staleness detection remains active.

**Off:**

* No normal automatic or manual background proxy maintenance occurs.
* Existing metadata and assets remain safe (they simply age to Stale).
* `Prepare Portable Project` MAY offer an explicit one-shot render without changing the persisted
  mode (§16.6).

**Prepare Portable Project** overrides ordinary update modes after explicit user confirmation: it
captures current eligible state, renders every required stale or missing proxy, waits responsively,
reports blocking failures and cancellations, and never packages a project while pretending
stale/missing proxies are current (§16.6).

* Storage (Recommended): per-destination `proxyUpdateMode` persisted at v20 (§12.2), values
  `"auto" | "onSave" | "manual" | "off"`.

### 18.2 Save and autosave (Locked)

Normal Save MUST: safely capture required current project/plugin state (fresh state capture already
happens at save — Verified §4.5 — and R2 keeps it authoritative); save project data; queue render
work **per the destination's update mode** (§18.1); and MUST NOT wait for rendering (PI-023).
Mode-specific Save behavior:

* **Auto:** Save queues only work already eligible under the Auto idle policy — it does not force
  every stale Auto destination to render and creates no duplicate jobs (no render storm on Save).
* **On Save:** an explicit user Save queues the latest stale eligible destinations (this is the
  mode's trigger).
* **Manual / Off:** Save queues no proxy rendering.

**Autosave never starts proxy rendering in any mode** (Verified mechanism: message-thread timer,
atomic writer). The explicit `Prepare Portable Project` operation (P1J, §16.6) is the only flow
that waits for proxies — and even it waits cancellably with responsive progress, never by blocking
the message thread.

### 18.3 Undo and dirty (Locked classification; see §19 of task → §20 state classes)

* Musical edits (notes, CC, routing, channels, clips) remain undoable and dirty the project —
  unchanged.
* Proxy progress, queue state, job states, and automatic source selection are non-undoable
  background state and MUST NOT create undo entries (PI-027).
* The remaining Auto idle countdown and render progress are runtime-only (§20) and MUST NOT dirty
  the project or create undo steps.
* Proxy metadata updates on publication (Recommended): dirty the project (the file references
  changed and should be saved) but are excluded from musical-undo comparison — consistent with the
  existing undo blob-stripping precedent (Verified §12.3).
* `proxyUpdateMode` changes (Recommended): persisted setting; dirties the project; not part of
  musical undo.
* User parameter edits inside the plugin editor follow existing dirty rules
  (`instrumentOrPluginEditsSinceClean_` — Verified, Audit §5.6); state capture itself adds nothing
  (§9.2 rule 6).

## 19. Required UI contract (no pixel design)

* Every instrument destination exposes its status from the PI-022 vocabulary (Primary / Proxy
  current / Proxy rendering / Proxy stale / Secondary / Missing / Render failed), reachable from
  the track context (existing seams: Inspector per-track controls, track-header rows,
  coordinator/controller runtime status — Verified locations, Audit §9).
* Render progress and speed reporting for active jobs (PI-013 capabilities), plus queue length
  when multiple destinations are pending.
* Explicit "Update Proxy"/"Retry" actions (Manual mode; Retry after failure in any maintaining
  mode) and mode selection Auto after idle / On Save / Manual / Off per destination (§18.1).
* For a volatile-classified plugin (§9.4.3), the compact status MAY read "Proxy current", but a
  status detail, information affordance, or tooltip MUST explain that currency is based on
  changes detected by DAL and that the plugin cannot provide fully verifiable state-change
  information (§9.4.5). Volatile plugins are not restricted to Manual-only.
* Source changes surface as a visible status change, never silently (PI-021).
* Failure states (§20) present a reason (plugin failed to instantiate, tail limit reached —
  render incomplete, publication failed, state capture failed) and the action taken (previous
  proxy retained). The tail-limit reason SHOULD offer the follow-up action of raising the tail
  limit and retrying (§15.2).
* During Proxy transport playback (P2, once Secondary exists) the UI explains that live audition
  is unavailable (§17 audition split).
* `Prepare Portable Project` (P1J) shows cancellable, responsive progress and reports blocking
  tracks explicitly (§16.6); it never reports a package complete when required proxies are not
  current and validated.
* Proxy mode MUST NOT offer a Primary plugin editor when Primary is missing (Verified: missing
  plugin ⇒ no editor, Audit §9).
* No diagnostic waveform view of proxy audio in v1 (§2.3).

## 20. Failure, recovery, and trust model

State classification (Locked, task §16):

| Class | Contents |
|---|---|
| Project-persisted musical state | tracks, clips, notes, CC, routing, channels, descriptors, plugin state blobs, `pluginVersion` |
| Project-persisted proxy state | selected update mode (`proxyUpdateMode`); proxy generation metadata (the v20 `proxy` object: references + policy versions); host-observed identity metadata required across load (accepted revision/save-pairing, §9.4.2); stable/volatile compatibility information **only where safe and version-qualified** (§9.4.3) |
| External proxy media | generation WAV files under `InstrumentProxies/` |
| Runtime engine state | published playback views, source mode, render instances, worker queue |
| Runtime-only proxy state | remaining Auto idle countdown; last-render-relevant-edit timestamps; queued/running job objects; render progress; temporary classification-probe state (§9.4.3) |
| Runtime UI/view state | status model, progress display |
| Derived/cache state | fingerprints (recomputable), Current/Stale/Failed verdicts, host-observed compatibility explanation (§9.4.5), queue eligibility |
| Undoable | musical edits only |
| Non-undoable background state | job states, progress, automatic source selection, idle-timer state (PI-027) |
| Project-dirty effects | musical edits (existing rules); publication metadata updates; mode changes — **never** job progress or the Auto idle countdown (§18.3: countdown and render progress MUST NOT dirty the project or create undo steps) |

Trust rules:

* The proxy system is advisory infrastructure: **no proxy failure may damage musical data**.
  Failures degrade status, never project content.
* Failed render (renderer error, publication failure, tail-limit failure, or an obsolete job) ⇒
  the previous published generation is retained — never deleted or overwritten — and the failure
  is clearly reported (PI-007, PI-024). The retained generation remains Current and playable only
  while its fingerprint still matches current musical content; otherwise it is a Stale retained
  asset and is never selected for playback in P1. "Current" always means fingerprint-current under
  the generation's recorded render configuration — never merely "latest file on disk" or
  "previously published" — and its plugin-state component is host-observed (§9.4.5), never an
  absolute claim about unknowable internal plugin activity.
* Stale is honest: the user always hears Primary, a current proxy (rate-adapted when needed),
  Secondary, or silence — never a stale proxy. A stale proxy is retained as an asset only; P1
  provides no manual "play stale proxy as current" override, and if Primary is missing with no
  current proxy or Secondary, transport is silent with an honest Stale/Missing status
  (PI-007/PI-021/PI-022). A future diagnostic comparison action MAY be designed separately —
  it does not exist in this document's scope.
* Corrupt metadata/assets degrade like missing plugins today: row preserved, silent + status
  (PI-025; Verified precedent §4.6).
* Every failure path (instantiation, restore, prepare, processing exception, tail limit reached,
  validation, publication, capture) has a defined terminal job state (§13.2) and a user-visible
  reason (§19). An incomplete render at the tail cap is a failure path, never a publication path
  (§15.2).

## 21. Complete decision register

All eleven Audit decisions (D1–D11) are preserved as PID-001…PID-011. None is omitted or silently
resolved. "Blocking slice" = the earliest roadmap slice that cannot complete without the decision.

---

**PID-001 (Audit D1) — Fingerprint definition.**
*Question:* Exact field list, hash construction, storage location, and the U1/U2 state-capture
policy. *Audit evidence:* §10 field classification; H2 (state captured too late); U1 (no persisted
plugin version); dirty flag unsuitable (§5.6). *Recommended decision:* field set F1–F13 (§11.1)
with canonical serialization (§11.4); SHA-256; storage = v20 metadata (`generationId`) + computed
staleness (§12.3); persist plugin version at v20 (U1). *Alternatives:* sidecar fingerprint file
(rejected: second source of truth); live-version-only check (rejected: portable projects can't
verify); binary hashing of plugin files (deferred). *Consequences:* schema bump to v20; fingerprint
schema versioning forever. *Required validation:* SPIKE-01 (state blob stability/cost); T-05, T-06.
*Blocking slice:* P1C (fields/serialization), P0/P1A (capture mechanism). *Status:*
**Locked (reviewed) — hybrid authoritative-state and host-observed identity contract** (human
review 2026-09-06, revision 5; contract text §9.4; evidence
`docs/audits/SPIKE_01_AUTHORITATIVE_PLUGIN_STATE_CAPTURE.md`, final measurement verdict PARTIAL
PASS). Capture layer: fresh checkpoint capture on the message thread — Resolved on measured
evidence. Identity layer: monotonic host-managed revision bumped by every DAL-observable
render-relevant change + persisted save-pairing; raw blob bytes/hashes never as general validity
identity; byte equality only as positive "unchanged" rescue for byte-stable-classified plugins at
host-observable quiescence, with asymmetric misclassification risk and demote-only reclassification
without a new probe (§9.4.3). The decision explicitly accepts the plugin API's fundamental
observability boundary — DAL never claims to detect unknowable internal plugin changes.
*Historical note:* revision 4 marked this "Resolved (PASS)" on 2026-09-05; that verdict was
premature and is superseded. Documented compatibility limitations (non-blocking for P1): E2
(MIDI-learn-class silent state), plugins beyond the two measured. Field set and serialization
remain **Recommended**; field-set completeness relative to the boundary follows from Locked
PI-019.

**PID-002 (Audit D2) — Staleness signalling.**
*Question:* Where fingerprints are compared (edit-time vs save-time vs render-enqueue-time) and the
UI contract for "proxy stale". *Audit evidence:* §5.6 (only runtime revision counters +
unsuitable dirty flag exist). *Recommended decision:* cheap runtime hints (controller
`ChangeBroadcaster` / render-snapshot `revision`, R2 state hints) mark *candidate* staleness
immediately; the authoritative verdict is a full fingerprint/revision compare promptly after the
observed change (coalesced, message-thread-affordable) and at enqueue; at load, one full compare
per destination (persisted save-pairing restores plugin-state validity by construction, §9.4.2).
UI shows Stale from the authoritative verdict (PI-022); Stale is marked immediately in every
update mode — the §18.1 Auto five-minute idle timer delays only the start of rendering, never the
staleness verdict. *Alternatives:* full recompute on every edit (rejected: message-thread cost);
save-time only (rejected: violates PI-020). *Consequences:* rendering (not staleness) lags edits
by the Auto idle period — accepted by design (revision 5). *Required validation:* T-06, T-07, T-08. *Blocking slice:* P1C. *Status:*
**Locked (reviewed)** — the signalling model defines required v1 behavior; hint-source
implementation details remain Proposed until the APIs exist.

**PID-003 (Audit D3) — Proxy asset layout.**
*Question:* Folder, naming, format, retention of the previous valid proxy, cleanup policy.
*Audit evidence:* §8.3/§8.4 (Audio/ has clip semantics; two atomic-publication variants; Windows
replace hazard H10; no media GC exists). *Recommended decision:* separate project-relative
`InstrumentProxies/` folder; immutable content-addressed generations
`track_<TrackId>_<hash>.wav`; publish-new-name + retire; keep the most recently published +
retained previous generation; orphan sweep (§16). Retention-on-failure is Locked (PI-007/PI-024):
the retained generation's Current-vs-Stale status is always a fingerprint verdict (§13.1). *Alternatives:* store in
`Audio/` (rejected: save-validation entanglement); mutable single file per track (rejected:
Windows replace hazard + PI-028). *Consequences:* small disk overhead (≤2 generations); new
cleanup code. *Required validation:* T-13, T-14, T-18. *Blocking slice:* P1F. *Status:*
**Locked (reviewed)** — separate project-relative proxy folder and immutable content-addressed
generations are required v1 behavior; concrete file/folder names remain Proposed.

**PID-004 (Audit D4) — Render job model.**
*Question:* Worker count, instance lifecycle, cancellation/generation guards,
faster-than-realtime vs realtime fallback. *Audit evidence:* §7.3 (thread contract; mixdown proves
off-device processBlock), §7.4 (no job queue; loadGeneration_/AsyncCallbackGuard precedents;
don't extend the waveform pool or the mixdown gate), §6.2 (offline flags unused — Verified
re-check). *Recommended decision:* **one** low-priority worker (product-preferred); lifecycle:
message-thread create/restore/prepare/release/destroy + worker processBlock; per-job cancellation
flag + fingerprint currency check at publish (loadGeneration_ pattern); offline indication where
supported with realtime-paced fallback per SPIKE-02 findings. *Alternatives:* worker pool
(deferred until single-worker throughput proves insufficient); message-thread rendering (rejected:
HR-5). *Consequences:* long projects render serially; acceptable for v1. *Required validation:*
SPIKE-02; T-09, T-10, T-11, T-12. *Blocking slice:* P1D/P1E. *Status:* **Locked (reviewed)** —
one low-priority render worker is required v1 behavior; offline-indication details and the
create-on-message/process-on-worker handoff remain evidence-gated by SPIKE-02.

**PID-005 (Audit D5) — Tail and latency policy.**
*Question:* Tail length source, latency trim vs preserve, live-vs-proxy timing parity.
*Audit evidence:* §6.1 (no tails, no PDC anywhere; latency only logged — re-verified), §9
(latency interaction row). *Decision:* **Locked** — proxy v1 preserves plugin latency exactly as
§15.1 (no independent shift; metadata records `getLatencySamples`; latency-policy version
fingerprinted; no live↔proxy timing jump; full PDC explicitly deferred and, when it arrives, must
cover live/proxies/inserts/groups/master/mixdown consistently with deterministic proxy
reinterpretation-or-staleness). Tail structure Locked (§15.2), including the reviewed rule that a
render reaching the maximum-tail limit with materially non-silent output is a diagnosed incomplete
render: it MUST NOT automatically become Current, the job terminates Failed with a tail-limit
reason, the previous generation is retained, the user is informed, and a later retry MAY raise the
limit; any future explicit "accept truncated result" feature is deferred and never silent.
*Open:* tail numeric values (threshold, window, cap) — evidence-gated by SPIKE-02 measurements
(−60 dBFS / 1 s / 30 s are experimental starting points, not canonical defaults). *Alternatives
(latency):* trim-at-render (rejected: creates live/proxy timing divergence today);
trim-at-playback (rejected: that *is* PDC — deferred). *Alternatives (truncation):*
publish-with-warning (rejected by review: silently promotes an incomplete render to authoritative
sound). *Consequences:* proxies inherit today's uncompensated timing — matching live behavior by
design. *Required validation:* T-15, T-16. *Blocking slice:* P1D (policy versions must exist
before first renders). *Status:* **Locked** (latency, tail structure incl. truncation-never-
Current) / **Open** (tail numbers, SPIKE-02-gated).

**PID-006 (Audit D6, U3) — Destination-mute semantics during proxy render.**
*Question:* Does a muted/off destination render "as if audible"? *Audit evidence:* §5.3
(destination mute gates its own MIDI at publish; audio gate at mix stage). *Recommended decision:*
render **independently** of destination mute/off (product-preferred): the proxy represents musical
content, not the monitoring choice; destination mute stays a playback-time gate and is excluded
from the fingerprint (§8.4, §11.2). The snapshot builder MUST therefore bypass the
`playbackEnabled` gate for the destination's own content when building render input.
*Alternatives:* mirror mute (rejected: a muted track would ship a silent/absent proxy in a
portable project). *Consequences:* snapshot construction cannot naively reuse published
`InstrumentTrackRenderSnapshot`s — it re-derives from tick data (already required by §10).
*Required validation:* T-07 (mute changes cause no staleness), T-02. *Blocking slice:* P1C.
*Status:* **Locked (reviewed)** — destination-independent rendering is required v1 behavior.

**PID-007 (Audit D7, U6) — Span rule.**
*Question:* What timeline range does a v1 proxy cover? *Audit evidence:* §6.1 (mixdown renders
locator span only), §10.4 U6. *Recommended decision:* project start → last render-relevant event
of the dependency set + tail (product-preferred; §15.6). *Alternatives:* first-event start with
synthesized chase preroll (deferred P4 optimization; risks CC-chase divergence); locator span
(rejected: proxies are not bounces). *Consequences:* long silent leaders render quickly but occupy
file length — acceptable; MAY be optimized later without fingerprint change if the output is
provably identical. *Required validation:* T-04, T-16. *Blocking slice:* P1C (span is snapshot
data). *Status:* **Locked (reviewed)** — required v1 behavior.

**PID-008 (Audit D8) — Playback-source selection contract.**
*Question:* Host-level vs helper-level seam; timeline-position plumbing; the
Primary/Proxy/Secondary/silent priority table including audition with missing Primary.
*Audit evidence:* §4.2 (two seam candidates; mixdown propagation favors host-level; timeline
position missing at the mix call), §9 (interaction table). *Decision:* priority order **Locked**
(PI-021; table §17). Seam: **Recommended** host-level substitution (§7.3) with timeline plumbing
placed in slice P1G. *Audition split (resolved by review — Locked for the first P2 version):*
when transport is stopped and Primary is missing, Secondary MAY provide live audition of newly
played notes; while a current Proxy supplies transport playback, DAL MUST NOT mix live Secondary
audition on top of that Proxy, and the UI explains that live audition is unavailable during Proxy
transport playback; if a musical edit makes the Proxy stale, the source-priority policy may select
Secondary as the working transport sound with the track clearly shown as using Secondary. Proxy
v1/P1 contains no Secondary implementation. *Alternatives:* helper-level seam (documented, not
preferred — must replicate the gain/pan fold and update mixdown in step); mixing Secondary
audition over Proxy transport (rejected by review: silently combines authoritative and approximate
sounds). *Consequences:* one signature-level change threading timeline position to the host.
*Required validation:* T-19, T-22. *Blocking slice:* P1G. *Status:* **Locked** (priority; audition
split for the first P2 version) / **Recommended** (seam mechanics).

**PID-009 (Audit D9) — Secondary role scope.**
*Question:* Where Secondary's descriptor/state live (second host per track? second
`experimentalInstrumentTracks` entry?), never-sonically-identical presentation, later channel-remap
hook. *Audit evidence:* §3.2 (one-host-per-track registry does not model it). *Locked boundaries:*
Secondary is P2; never overwrites/redefines Primary or the Primary proxy; never presented as
sonically identical; instantiated only when needed; v1 Preserve channels + optional simple remap;
CC (incl. CC11) forwarded unchanged; advanced mapping deferred (PI-004, PI-005). *Reviewed
disposition (human review):* the persistence/registry shape is **deliberately deferred to P2** and
explicitly non-blocking for P1. P1B MUST NOT add empty Secondary fields, placeholder objects, or
speculative registry structures; the only P1 obligation is to avoid unnecessarily consuming an
obvious future namespace (§12.4). The embedded-block versus role-list decision is made during P2
design using then-current architecture evidence. *Consequences:* none for P1 beyond naming
discipline. *Required validation:* none in P1 (P2 acceptance later). *Blocking slice:* none in P1
(P2). *Status:* **Reviewed — deliberately deferred**; technically unresolved until P2.

**PID-010 (Audit D10) — Dependency enumeration centralization.**
*Question:* Centralize `sourcesForDestination()` before building fingerprint collection on top?
*Audit evidence:* §5.2 (≥6 duplicated scan sites; HR-3: drift risk between "what plays" and "what
was fingerprinted"). *Recommended decision:* yes — build the central authoritative enumerator
(product-preferred) in P1C; snapshot/fingerprint construction MUST use it; existing sites migrate
opportunistically, realtime paths only with care (§6 R3). *Alternatives:* a seventh private scan
(rejected: institutionalizes HR-3). *Consequences:* small shared utility + tests; later cleanup
opportunities. *Required validation:* T-02 (enumerator vs live merge equivalence at the capture
sink). *Blocking slice:* P1C. *Status:* **Locked (reviewed)** — one central authoritative
enumerator is required v1 behavior; the function's name/signature remains Proposed.

**PID-011 (Audit D11) — Prepare Portable Project.**
*Question:* Packaging flow, relation to Save (fast, non-blocking), verification that "all proxies
current" before packaging. *Audit evidence:* §8.3 (no portable/collect feature, no Save As media
copy, no media GC). *Decision (resolved by human review — Locked P1 behavior, §16.6):* portable
packaging is part of the product promise and completes in **P1** as slice **P1J**. Locked:
`Prepare Portable Project` is an explicit, cancellable operation with responsive progress that
never blocks the message thread; it validates all required destination proxies via authoritative
fingerprints and asset validation; mode-Off with a current proxy is acceptable while mode-Off with
a stale/missing/corrupt/failed proxy is a blocker; it offers an explicit one-shot
render-for-package action without silently changing the persisted update mode; missing Primary or
unrenderable tracks are reported as blockers and no supposedly complete package is published;
publication happens only after all required proxies and media are validated; cancellation/failure
leaves no partially published package; plugin binaries/licenses/activation data are never included
(PI-026); normal Save never waits for rendering (PI-023). *Recommended (nonessential details):*
progress UI specifics; package container format (folder vs archive — repository evidence does not
determine it; decided in P1J). *Consequences:* new slice P1J after P1I; Save As asset copy lands in
P1H as its prerequisite. *Required validation:* T-23 (validation verdicts), T-27 (packaging
end-to-end incl. cross-machine/sample-rate portability). *Blocking slice:* P1J. *Status:*
**Locked (reviewed)** for behavior; only UI/container details Recommended.

---

**OI-001 — Cross-sample-rate proxy playback (resolved by human review → Locked requirement).**
*Question (as originally posed):* what happens when a project with proxies rendered at rate A is
opened on a machine running at rate B? *Review decision:* the previously recommended
"mismatch ⇒ stale, no resampler in v1" behavior is **rejected**. A current proxy MUST remain
playable across engine-rate differences, including when Primary is missing (PI-030, §15.3):
generation validity is evaluated under the generation's recorded render configuration; playback
uses a derived playback representation prepared outside the audio thread; an engine-rate change
rebuilds only that derived representation; native-rate re-rendering is an optional quality refresh
when Primary is available, never a playback prerequisite. *Remaining openness:* only the technical
implementation mechanism (OI-002/SPIKE-03). *Status:* **Locked** (product requirement).

**OI-002 — Bounded proxy playback I/O and sample-rate adaptation (new, reviewed technical item).**
*Question:* the audio-thread-safe playback-source mechanism satisfying PI-030/PI-031: bounded
memory use; background read-ahead or another bounded mechanism; optional preload only below an
explicit size/project budget; sample-rate conversion outside the audio thread where practical;
transport seek and discontinuity; loop/cycle playback; source switching; engine sample-rate
changes; offline mixdown; missing/corrupt files; multiple simultaneously required proxies; Windows
file-handle and generation-retirement behavior. *Evidence basis:* no streaming reader exists today
(Verified §4.6); at 48 kHz, stereo 32-bit float is ≈23 MB/min/proxy, so unbounded full preload is
rejected (§7.3). *Recommended direction:* a bounded hybrid (read-ahead + budgeted preload +
off-thread conversion), exact mechanism selected from SPIKE-03 evidence. *Required validation:*
**SPIKE-03** (§22), T-25, T-26. *Blocking slice:* P1G (not SPIKE-01/P0-P1A). *Status:* **Open /
evidence-gated** (mechanism only; the requirements are Locked).

---

**Register cross-check:** D1→PID-001, D2→PID-002, D3→PID-003, D4→PID-004, D5→PID-005, D6→PID-006,
D7→PID-007, D8→PID-008, D9→PID-009, D10→PID-010, D11→PID-011 — all eleven present; none silently
resolved. Additional non-audit items: OI-001 (resolved → Locked cross-rate playback requirement,
§15.3) and OI-002 (bounded playback I/O mechanism, Open/SPIKE-03-gated).

## 22. Implementation roadmap

P1 is split into ordered, independently reviewable slices. **No slice combines schema, renderer,
background scheduling, publication, playback fallback, and UI.** Each slice states its completion
gate; test IDs refer to §23. Per `docs/DEVELOPMENT_TEST_POLICY.md`, ordinary slices run the
smallest falsifying tests (Level 1); broader stability scenarios are assigned only at the
integration gates marked below.

### P0/P1A — Blocking validation spikes and architectural prerequisites

* **Goal:** retire the two earliest evidence gaps (SPIKE-01 blocks P1C and everything downstream;
  SPIKE-02 blocks P1D); no production behavior change. (The third spike, SPIKE-03, gates only P1G
  and is defined below.)
* **Included:** **SPIKE-01** (authoritative state capture: `getStateInformation` cost, blob
  stability for identical audible state, parameter-listener coverage, editor-close hooks —
  evidence report + mechanism proposal for human review, §9.2). **SPIKE-02** (isolated render
  instance: message-thread create/restore/prepare + worker processBlock for a dedicated instance;
  `setNonRealtime`/offline-indication behavior; faster-than-realtime feasibility and latency
  reporting with VB3-II-class plugins; contention with live playback). Spike code stays in
  diagnostics/test scaffolding, never in the product path.
* **Excluded:** any schema change, any renderer, any UI.
* **Expected files/components:** diagnostics/selftest scaffolding only (e.g. alongside
  `src/diagnostics/`); spike reports under `docs/audits/` or equivalent.
* **Invariants protected:** PI-011, PI-012, PI-020 (evidence basis).
* **Prerequisite decisions:** none (this slice *produces* decision evidence for PID-001/PID-004/
  PID-005 numbers).
* **Automated tests:** none new in product; spike harness assertions.
* **Plugin-dependent/manual:** VB3-II (or comparable VST3) spike sessions; results recorded.
* **Rollback:** delete scaffolding; zero product risk.
* **Completion gate:** human review of SPIKE-01 outcome selects the state-capture mechanism
  (PID-001 Open item resolved or explicitly re-scoped). **P1C MUST NOT complete before this gate.**
  **Gate status: satisfied** — SPIKE-01 (2026-09-05) plus the corrective SPIKE-01B/SPIKE-01B-M
  follow-up (final measurement verdict PARTIAL PASS) were reviewed 2026-09-06 and locked the §9.4
  hybrid contract (the original 2026-09-05 PASS verdict is historical/superseded). SPIKE-02
  remains open and still gates P1D.

### SPIKE-03 — Proxy playback I/O, memory budget, seeking, and resampling (blocks P1G)

* **Goal:** evidence-select the audio-thread-safe playback-source mechanism required by
  PI-030/PI-031 (OI-002). SPIKE-03 gates **P1G only** — it does not block SPIKE-01/P0/P1A and MAY
  run in parallel with P1B–P1F once render output exists to play with.
* **Must evaluate:** bounded memory use; background read-ahead or another bounded mechanism;
  optional preload only below an explicit size/project budget; sample-rate conversion outside the
  audio thread where practical; transport seek and discontinuity; loop/cycle playback; source
  switching; engine sample-rate changes; offline mixdown; missing/corrupt files; multiple
  simultaneously required proxies; Windows file-handle and generation-retirement behavior.
* **Deliverable:** an evidence report and a proposed mechanism (with budget numbers) for human
  review; spike code stays in diagnostics/test scaffolding, never in the product path.
* **Excluded:** any product behavior change; mandating an exact resampler API before validation.
* **Invariants protected:** PI-012, PI-030, PI-031 (evidence basis).
* **Completion gate:** human review of the SPIKE-03 report resolves OI-002. **P1G MUST NOT
  complete before this gate.**

### P1B — Project model/schema seam with backward-compatible loading

* **Goal:** v19→v20 additive schema for proxy metadata + plugin version; loading stays
  backward-compatible.
* **Included:** v20 fields of §12.2 (defaulted absent), `pluginVersion` persistence, undo
  blob-stripping extension (§12.3), Secondary naming discipline (§12.4 — **no placeholder
  fields**), and **TLD-1** (§10.1): a stable persisted timeline reference rate that is
  authoritative at load, is never re-stamped by device/engine-rate changes, and whose explicit
  change rescales all affected sample-domain fields atomically. No asset I/O, no rendering.
* **Excluded:** fingerprinting, rendering, playback, UI beyond nothing-visible defaults.
* **Expected files/components:** `src/io/ProjectFile.h/.cpp`, `src/domain/Session.*` DTO plumbing,
  `src/instruments/InstrumentTrackController.cpp` (descriptor version capture at save).
* **Invariants protected:** PI-024 (metadata-not-media), PI-025 (absent/invalid ⇒ no proxy),
  PI-029 (no P2 modeling), PI-030 (timeline reference integrity feeds cross-rate validity).
* **Prerequisite decisions:** PID-001 storage (Recommended — confirmed at design review), PID-003
  naming vocabulary (paths stored, folder may not exist yet); TLD-1 approach reviewed (§10.1).
* **Automated tests:** T-05, T-21 (v19 round-trip; v20 defaults; unknown-field tolerance per
  existing migration conventions; timeline-reference-rate migration and domain integrity).
* **Plugin-dependent/manual:** none.
* **Rollback:** revert version bump; v19 files never contained the fields.
* **Completion gate:** load/save round-trip of v19 and v20 fixtures passes, and the TLD-1 gate
  verifies at minimum (§10.1, T-21):
  1. a migrated v19 file receives the stored `deviceSampleRateAtSave` as its initial timeline
     reference rate;
  2. saving a v20 project while the engine runs at another rate neither re-stamps the timeline
     reference nor changes the stored sample-domain integers;
  3. conversions from the persisted timeline domain to engine/render samples preserve wall-clock
     positions;
  4. the persisted timeline domain is actually consumed by the relevant runtime and proxy-snapshot
     boundaries (not merely stored as unused metadata);
  5. any remaining sample-domain consumers outside P1B (§10.1 lists today's device-rate
     interpretation sites) are explicitly enumerated and assigned to a blocking slice before
     P1J's cross-rate acceptance.
  Schema review incl. TLD-1 concludes the slice.

### P1C — Centralized MIDI dependency snapshot and canonical fingerprint

* **Goal:** `midi_dependency::sourcesForDestination()` (R3), `ProxyRenderSnapshotBuilder` (R4),
  `ProxyFingerprintBuilder` (R5), staleness verdicts (PID-002) — all message-thread, no rendering.
* **Included:** snapshot schema §10; fingerprint §11 incl. canonical serialization; state capture
  and host-observed identity per the Locked §9.4 hybrid contract (R2 first version: revision
  counter, bump sources, k-capture classification probe, save-pairing); stale detection plumbing
  (no queue yet — verdicts only; the §18.1 idle timing belongs to P1H).
* **Excluded:** render worker, assets, playback changes, UI beyond diagnostics.
* **Expected files/components:** new `src/instruments/` or `src/app/` units for R2–R5; hooks in
  `InstrumentRuntimeCoordinator`.
* **Invariants protected:** PI-009, PI-018, PI-019, PI-020; PID-006/PID-007 semantics.
* **Prerequisite decisions:** SPIKE-01 gate (P0/P1A); **ORD-1 landed** (§8.3 — separately reviewed
  stable-sort micro-change; the fingerprint cannot claim delivery parity before it); **TLD-1
  landed** (P1B); PID-002/PID-006/PID-007/PID-010 Locked (reviewed).
* **Automated tests:** T-01, T-02, T-03, T-05, T-06, T-07, T-08, T-30 (deterministic selftests;
  Organ Upper/Lower/Pedal fixture reused; T-30 covers the §9.4 revision/classification identity
  behavior with synthetic state providers).
* **Plugin-dependent/manual:** T-01 blob checks against a real VST3.
* **Rollback:** feature is inert (no consumer yet); revert cleanly.
* **Completion gate:** fingerprint repeatability + full stale-trigger matrix green; capture-sink
  ordering parity (incl. equal-time tie-breaks per §8.3) green.

### P1D — Isolated complete-destination renderer in a controlled foreground test

* **Goal:** `ProxyRenderInstance` (R6) + `ProxyDestinationRenderer` (R8) render a snapshot to a
  temp WAV, driven synchronously from a diagnostic/selftest entry point (no background queue).
* **Included:** §14.2 steps 2–8 (foreground); latency metadata recording (§15.1); tail policy
  v1 (§15.2, provisional Open numbers under version tag); span rule (§15.6); render-config
  policies (§15.3–15.5); **the §9.4.4 isolated-instance lifecycle validation** (restore →
  prepare → reset/flush where supported → deterministic MIDI/CC chase → complete render from
  project start), including the fallback obligation: a plugin whose lifecycle cannot remove
  performance-transient initial state gets deferred snapshot capture or an explicit compatibility
  limitation.
* **Excluded:** background thread, publication into the project, playback substitution, UI.
* **Expected files/components:** new render units (likely `src/plugins/` or a new `src/render/`);
  diagnostics hook.
* **Invariants protected:** PI-010, PI-011, PI-014, PI-016, PI-017.
* **Prerequisite decisions:** SPIKE-02 results; PID-004 lifecycle; PID-005 latency (Locked) +
  provisional tail numbers.
* **Automated tests:** T-04 (capture-sink MIDI equivalence), T-09 (live instance untouched),
  T-31 harness parts; Level-1 harness.
* **Plugin-dependent/manual:** T-15 (latency equivalence), T-16 (tail completion), T-31
  (§9.4.4 lifecycle validation) with VB3-II; rendered-file listening check.
* **Rollback:** diagnostic-only entry point; remove hook.
* **Completion gate:** rendered WAV of the Organ fixture is MIDI-equivalent at the capture sink
  and latency metadata matches the prepared instance's report.

### P1E — Background queue, cancellation, and obsolete-job protection

* **Goal:** `ProxyRenderQueue` (R7) runs P1D's renderer on one low-priority worker with
  cooperative cancellation, supersession, pause-during-recording, and safe shutdown.
* **Included:** job state machine §13.2; race rules §13.3 (edit-during-render, supersession,
  close/shutdown); progress reporting plumbing.
* **Excluded:** publication into project metadata (temp output only), playback, UI.
* **Expected files/components:** queue/worker unit (patterned on recorder/scan threads, §14.4);
  coordinator wiring.
* **Invariants protected:** PI-012, PI-013, PI-028 (currency check exists even for temp
  finalization), PI-017.
* **Prerequisite decisions:** PID-004 (Recommended confirmed).
* **Automated tests:** T-10 (audio thread never blocked — stability-invariant style), T-11
  (edit-during-render ⇒ obsolete), T-12 (obsolete publication rejection at the finalize step),
  T-24 (shutdown/track-deletion races), T-29 (single job per destination; serialization;
  rendering during playback). **Integration gate:** relevant stability scenarios
  (Level 2: smoke + delete-loop) because a new thread touches engine-adjacent lifecycles.
* **Plugin-dependent/manual:** long-render cancellation with a real plugin.
* **Rollback:** queue is unconsumed by playback; disable enqueue.
* **Completion gate:** race tests green; stability scenarios clean.

### P1F — Proxy asset storage and atomic publication

* **Goal:** `ProxyAssetStore` (R9): `InstrumentProxies/` layout, publish-new-name + retire,
  metadata update (P1B fields), retention of previous generation, crash-recovery sweep.
* **Included:** §16.1–16.5; PI-028 currency check at publish; load-time validation (PI-025).
* **Excluded:** playback substitution, Save As copy (P1H), portable collection.
* **Expected files/components:** asset-store unit near `src/io/`; `ProjectIoCoordinator` hooks.
* **Invariants protected:** PI-007, PI-024, PI-025, PI-026, PI-028.
* **Prerequisite decisions:** PID-003 (Recommended confirmed).
* **Automated tests:** T-12, T-13 (retention on failure), T-14 (Windows-safe publication), T-18
  (missing/corrupt recovery).
* **Plugin-dependent/manual:** none beyond upstream slices.
* **Rollback:** published assets are inert without the playback slice; folder removable.
* **Completion gate:** publication/retention/recovery tests green on Windows semantics.

### P1G — Proxy playback substitution, rate adaptation, and missing-Primary fallback

* **Goal:** host-level seam substitution (§7.3): timeline-position plumbing, per-host
  `ProxyPlaybackView` publication, the SPIKE-03-selected bounded read/adaptation mechanism
  (PI-030/PI-031), source selection per §17 (without Secondary rows), mixdown propagation.
* **Included:** PI-021 priority for {Primary, Proxy current, Missing}; stale proxies never play;
  cross-rate playback via the derived playback representation (rate mismatch never silences a
  current proxy); bounded read policy per OI-002 resolution; engine-rate-change rebuild of derived
  representations off the audio thread.
* **Excluded:** Secondary (P2), UI beyond existing missing-plugin status parity, modes (P1H).
* **Expected files/components:** `src/plugins/ExperimentalInstrumentHost.*`,
  `src/engine/PlaybackMixHelpers.*` / `PlaybackEngine.cpp` (timeline plumbing only),
  coordinator wiring.
* **Invariants protected:** PI-001, PI-002, PI-003, PI-006, PI-007, PI-008, PI-021, PI-030,
  PI-031.
* **Prerequisite decisions:** PID-008 seam (Recommended confirmed); **OI-002 resolved via the
  SPIKE-03 gate** (blocking).
* **Automated tests:** T-19 (source-selection matrix, non-Secondary rows), T-22 (offline mixdown
  through selected source), T-08 (post-boundary changes don't re-render), T-25 (cross-rate
  playback), T-26 (bounded playback I/O), T-29 (rendering during playback with an eligible
  snapshot — P1G part). **Integration gate:** Level 2 stability (smoke,
  delete-loop, mixdown) — audio-callback path touched.
* **Plugin-dependent/manual:** unload Primary mid-project; verify proxy playback through
  inserts/fader/pan; T-15 timing parity live↔proxy; switch device sample rate with Primary missing
  and confirm continued proxy playback.
* **Rollback:** substitution branch behind the published view — absent view ⇒ exactly today's
  behavior.
* **Completion gate:** matrix + stability green; audible parity check recorded; cross-rate
  playback demonstrated.

### P1H — Auto/Manual/Off behavior, Save/close semantics, and portable validation

* **Goal:** update modes (§18.1), Save/autosave queue-not-wait (§18.2), undo/dirty classification
  (§18.3), Save As asset copy (§16.6), portable-validation verdict logic
  (`PortableProjectValidator` verdicts — consumed by the P1J packaging flow).
* **Included:** the four §18.1 update modes incl. the per-destination five-minute Auto idle timer
  (runtime-only countdown, §20); mode persistence; mode-specific Save queueing (§18.2);
  close/shutdown ordering with the queue.
* **Excluded:** the `Prepare Portable Project` packaging flow itself (P1J), Secondary.
* **Expected files/components:** coordinator, `ProjectIoCoordinator`, autosave timer touchpoints.
* **Invariants protected:** PI-023, PI-027, PI-029.
* **Prerequisite decisions:** none open (modes are Locked).
* **Automated tests:** T-20 (mode-specific Save/autosave behavior incl. no-render-storm and
  no-dirty/no-undo assertions), T-11 (Auto interval restart), T-28 (five-minute idle-timer
  semantics, mock clock), T-29 (P1H parts), T-21 (backward-compat load), T-23 (portable
  validation verdicts), T-24 (close races). Level 2: autosave/recover scenarios.
* **Plugin-dependent/manual:** mode-switch walkthrough across all four modes.
* **Rollback:** modes default Auto; feature flags not used — revert slice.
* **Completion gate:** Save latency unchanged (no waiting), four-mode semantics incl. the fixed
  five-minute Auto idle policy test-proven.

### P1I — Status UI, failure presentation, and end-to-end verification

* **Goal:** PI-022 status vocabulary in Inspector/track-header seams, progress/speed display,
  failure reasons (§19, §20) including the tail-limit needs-attention state, source-change
  surfacing; final end-to-end pass of the playback feature set.
* **Included:** `ProxyStatusModel` (R11); notifications on source change.
* **Excluded:** pixel design mandates; diagnostic waveform view (non-goal); packaging (P1J).
* **Expected files/components:** `src/ui/InspectorView.*`, track-header coordinator, status model
  unit.
* **Invariants protected:** PI-021 (no silent changes), PI-022, PI-027.
* **Prerequisite decisions:** none open.
* **Automated tests:** status-model unit tests; T-19 re-run.
* **Plugin-dependent/manual:** full E2E: edit → stale → render → publish → unload Primary → proxy
  playback → mixdown; failure-path walkthrough incl. a forced tail-limit failure. **Integration
  gate:** full relevant stability matrix per `docs/DEVELOPMENT_TEST_POLICY.md` Level 3 criteria
  (finishing a batch of runtime changes).
* **Rollback:** UI-only revert.
* **Completion gate:** E2E walkthrough recorded; §24 criteria not owned by P1J checked.

### P1J — Prepare Portable Project packaging and portability end-to-end verification

* **Goal:** the Locked `Prepare Portable Project` operation (§16.6, PID-011) as a distinct slice —
  deliberately **not** folded into the already broad P1H or the UI-only P1I.
* **Included:** collection of the project file; referenced normal media; required current proxy
  generations; validation of all of the above via authoritative fingerprints and asset checks;
  cancellable, responsive progress (no message-thread blocking); the explicit one-shot
  render-required-proxies action (no silent persisted-mode change); explicit blocker reporting
  (incl. mode-Off tracks with stale/missing/corrupt/failed proxies and missing-Primary tracks);
  atomic package publication with no partial package on cancel/failure; cross-machine and
  cross-sample-rate portability verification of a produced package (PI-030).
* **Excluded:** plugin binaries/licenses/activation data (PI-026 — never); Secondary (P2); any
  new render architecture (reuses the P1E queue and P1F assets).
* **Expected files/components:** `PortableProjectValidator`/packaging flow near
  `ProjectIoCoordinator` (R12); reuse of §16.3 temp+atomic publication discipline for the package.
* **Invariants protected:** PI-023, PI-024, PI-026, PI-028, PI-030.
* **Prerequisite decisions:** PID-011 (Locked); package container format decided in-slice
  (Recommended detail).
* **Automated tests:** T-23 (verdicts), T-27 (packaging end-to-end: happy path, blockers,
  cancellation atomicity, cross-rate open of the produced package). Level 2: autosave/recover +
  smoke scenarios (project I/O touched).
* **Plugin-dependent/manual:** produce a package on machine/rate A; open on machine/rate B without
  Primary; verify playback and honest statuses.
* **Rollback:** operation-level feature; revert slice.
* **Completion gate:** T-27 green; a real cross-machine (or cross-rate simulated) package
  walkthrough recorded; §24 packaging criteria checked.

### Recommended next implementation slice

*Historical:* SPIKE-01 was the recommended first slice; it and its corrective SPIKE-01B/SPIKE-01B-M
follow-up are now **completed and reviewed** (revision 5; PID-001 Locked, §9.4). The SPIKE-01
diagnostic scaffolding remains in the tree pending the cleanup recorded in the evidence report
(§28.8) and is not removed by this revision. **The recommended next slice is SPIKE-02** (isolated
render instance; gates P1D and PID-005's tail numbers), with ORD-1 and TLD-1/P1B as the earliest
production micro-changes on the P1C path. (SPIKE-03 blocks only P1G and can run later, in parallel
with P1B–P1F.) Implementation of product proxy behavior has **not** started.

## 23. Verification strategy

Tests are split into **deterministic selftests** (no real plugin; use the `MidiDeliveryCaptureSink`
seam, the Organ Upper/Lower/Pedal fixture, and capture-only mode — Verified test seams, §4.1/§8.2),
**plugin-dependent integration tests** (real VST3, VB3-II-class), and **manual checks**. Broad
stability regression runs only at the P1E/P1G/P1I/P1J integration gates (per
`docs/DEVELOPMENT_TEST_POLICY.md`), not per slice.

| ID | Test | Kind | Slice |
|---|---|---|---|
| T-01 | Authoritative plugin-state capture: snapshot blob equals currently audible state; editor-close/Save/enqueue agreement (mechanism per SPIKE-01) | plugin-dependent + deterministic harness parts | P0/P1A, P1C |
| T-02 | Deterministic incoming-source enumeration: R3 output equals live merge set/order (capture-sink comparison), incl. destination-mute independence (PID-006) | deterministic | P1C |
| T-03 | Deterministic equal-time MIDI/CC ordering per §8.3: track-order last-wins; stored-order tie-break for equal-time notes (post-ORD-1); same-segment CC→Off→On vs pending Off→CC→On emission structure; reorder ⇒ fingerprint change | deterministic | P1C |
| T-04 | VB3-II Upper/Lower/Pedal snapshot correctness: renderer's delivered MIDI stream ≡ live delivery at the capture sink for the fixture | deterministic (capture-only) + plugin-dependent audio | P1C, P1D |
| T-05 | Fingerprint repeatability: same content ⇒ identical hash across runs/processes; v19/v20 load parity | deterministic | P1B, P1C |
| T-06 | All required stale triggers: note/CC/channel edits, source add/remove/reroute, source mute/off, track reorder, clip bpm, destination output channel, plugin state change, plugin version change, policy-version bumps. Plus the negative assertions: an engine/device sample-rate change alone triggers **no** staleness (it rebuilds only the derived playback representation — T-17/T-25; a native-rate quality refresh MAY be queued when Primary exists while the generation remains Current), and a timeline-reference-rate migration changes fingerprints only as part of its defined atomic rescale conversion (§10.1), never merely because the device rate changed | deterministic | P1C |
| T-07 | Post-boundary changes cause **no** staleness: fader, pan, inserts, sends, bus routing, master, destination mute/off | deterministic | P1C |
| T-08 | Post-boundary changes cause no re-render at playback level | deterministic | P1G |
| T-09 | Live instance never used by rendering: instrumented proof that render jobs touch only their own instance (live host untouched during render) | deterministic harness + plugin-dependent | P1D |
| T-10 | Audio thread never blocked by render activity (stability-invariant style measurement during renders) | deterministic + stability scenario | P1E |
| T-11 | Edit-during-render ⇒ job obsolete at next boundary; in Auto mode the edit restarts the destination's five-minute idle interval before any new job may start (§18.1) | deterministic | P1E, P1H |
| T-12 | Obsolete job publication rejection: stale-fingerprint job cannot publish (PI-028) | deterministic | P1E, P1F |
| T-13 | Retention of the previous published generation on render/publication failure (PI-007/PI-024), covering **both** status cases: (1) failure while the previous generation still matches current musical content (e.g. a failed native-rate quality refresh, or the triggering edit was undone) ⇒ it remains Current and playable; (2) failure after a render-relevant edit ⇒ it is retained as Stale and never selected for playback in P1 | deterministic | P1F |
| T-14 | Windows-safe publication: publish-new-name with an open handle on the old generation; retire without in-place replace | deterministic (Windows semantics) | P1F |
| T-15 | Latency equivalence: live Primary vs proxy v1 playback timing identical for a latency-reporting plugin (no jump on source switch) | plugin-dependent | P1D, P1G |
| T-16 | Tail completion (silence-detected end); reaching the max-tail cap with materially non-silent output ⇒ job Failed with tail-limit reason, **no publication**, previous generation retained, user-visible needs-attention status (§15.2) | plugin-dependent | P1D |
| T-17 | Sample-rate identity: metadata records the render rate; generation validity is evaluated under the generation's recorded configuration; an engine-rate difference alone never produces Stale or silence (§15.3, PI-030) | deterministic + manual | P1C, P1G |
| T-18 | Missing/corrupt proxy recovery: project loads, status degrades, no crash (PI-025) | deterministic | P1F |
| T-19 | Source-selection matrix of §17 (v1 rows; Secondary rows added in P2) | deterministic | P1G, P1I |
| T-20 | Save and autosave per update mode (§18.1/§18.2): fresh state captured; Save never waits (latency assertion); explicit Save in Auto queues only already-eligible work — **no render storm** and no duplicate jobs; explicit Save in On Save queues the latest stale eligible destinations; Manual/Off queue nothing; **autosave never starts proxy rendering in any mode**; Auto idle countdown and render progress create **no** dirty state and **no** undo steps (PI-027, §18.3) | deterministic | P1H |
| T-21 | Backward-compatible loading and timeline-domain integrity: v19 projects load with proxy fields defaulted and the timeline reference rate initialized from stored `deviceSampleRateAtSave`; v20 round-trip; saving under a different engine rate neither re-stamps the timeline reference nor changes stored sample-domain integers; persisted-domain → engine/render conversions preserve wall-clock positions; the persisted domain is consumed by the relevant runtime/proxy-snapshot boundaries (not stored-but-unused); remaining unconverted sample-domain consumers are enumerated and assigned to a blocking slice before P1J cross-rate acceptance (§10.1) | deterministic | P1B, P1H |
| T-22 | Offline mixdown renders through the selected source (Primary or proxy) with identical downstream processing | deterministic + plugin-dependent | P1G |
| T-23 | Portable-project validation verdicts: current/stale/failed/mode-Off combinations | deterministic | P1H |
| T-24 | Shutdown and track-deletion races: close/shutdown/delete during queued and running jobs; no leaks, no publications after removal | deterministic + stability scenario | P1E, P1H |
| T-25 | Cross-rate proxy playback: a current generation rendered at rate A plays at engine rate B (Primary missing), via a derived playback representation prepared off the audio thread; engine-rate change rebuilds only the derived representation; the authoritative asset is unmodified; timeline alignment correct (PI-030) | deterministic + plugin-dependent/manual | P1G |
| T-26 | Bounded proxy playback I/O (per the SPIKE-03-selected mechanism): memory stays within the explicit budget with many simultaneous long proxies; seek/loop/source-switch discontinuities are glitch-defined and audio-thread-safe; missing/corrupt file mid-playback degrades without crash; generation retirement with open handles is Windows-safe (PI-031) | deterministic + stability scenario | P1G |
| T-27 | `Prepare Portable Project` end-to-end: happy path packages project + media + required current generations only after validation; mode-Off/current acceptable; mode-Off/stale, missing-Primary, failed-render are reported blockers with no package published; cancellation/failure leaves no partial package; the produced package opens and plays on a different rate without Primary (§16.6); **the operation correctly overrides all four §18.1 update modes** (renders required stale/missing proxies in Auto/On Save/Manual/Off after explicit confirmation) without changing any persisted mode | deterministic + manual cross-machine | P1J |
| T-28 | Auto idle-timer semantics (§18.1): the five-minute timer starts on the first render-relevant edit; subsequent relevant edits reset **only that destination's** timer; unrelated-track edits do not reset it; no render starts before five continuous idle minutes; recording pauses eligibility and render work; timer state is runtime-only and survives nothing it shouldn't (no persistence, no dirty) | deterministic (mock clock) | P1H |
| T-29 | Scheduler discipline: at most one queued-or-running job per destination; multiple eligible destinations serialize through the single P1 worker; background rendering proceeds during transport playback once an eligible snapshot exists (§9.4.4) with no audio-thread impact (extends T-10) | deterministic + stability scenario | P1E, P1G, P1H |
| T-30 | Hybrid identity contract (§9.4): publication/obsolete checks compare host-managed revisions and **never a freshly captured raw blob hash** (volatile-classified plugin publishes with churning bytes); byte-equality rescue cancels an unnecessary re-render only for a byte-stable-classified plugin at host-observable quiescence; repeated qualifying at-rest inequality demotes stable → volatile; volatile → stable promotion requires a new qualifying probe; persisted proxy↔saved-state pairing restores load-time validity with no byte comparison | deterministic (synthetic state providers) | P1C |
| T-31 | P1D isolated-instance lifecycle validation (§9.4.4): restore → prepare → reset/flush where supported → deterministic MIDI/CC chase → complete render from project start; renders from equivalent snapshots are deterministic per plugin-class evidence; a plugin whose lifecycle cannot remove performance-transient initial state is deferred to an eligible capture boundary or flagged with a compatibility limitation | plugin-dependent + deterministic harness parts | P1D |

## 24. First-version acceptance criteria

Proxy v1 (P1 complete) is accepted when all of the following hold:

1. A project whose Primary plugins are all missing on the current machine loads, shows honest
   per-track statuses, and plays current proxies through each destination's normal
   inserts/fader/pan/bus/master path — with no added tracks or mixer channels (PI-001/PI-002).
2. Editing any note, CC, channel, routing, or plugin parameter of a destination or its routed
   sources marks that destination's proxy stale; nothing else does (T-06/T-07 matrix).
3. With Primary installed, Auto mode re-renders in the background — after the destination's fixed
   five-minute idle period (§18.1) — without audio-thread impact, including during transport
   playback once an eligible snapshot exists (§9.4.4); an edit mid-render supersedes the job
   without a stale publication and restarts the idle interval (T-10/T-11/T-12/T-28/T-29).
   Explicit Save produces no render storm and autosave never starts rendering (T-20).
4. A failed render retains the previous published generation and reports the failure. The retained
   generation remains playable only when it is still Current; otherwise it remains a Stale
   retained asset and is never selected for playback in P1 (T-13, both cases).
5. Live Primary ↔ proxy switching produces no latency-policy timing jump (T-15).
6. Tails are captured to deterministic silence; a render reaching the maximum-tail limit with
   materially non-silent output terminates Failed with a tail-limit reason, publishes nothing,
   retains the previous generation, and informs the user (T-16).
7. Save latency is unchanged and never waits on rendering (T-20).
8. v19 projects load unchanged; v20 projects with missing/corrupt proxy media load with degraded
   status (T-18/T-21).
9. Offline mixdown of a proxy-playing project is correct through the shared downstream path
   (T-22).
10. The user can always see which source they are hearing, and source changes are surfaced
    (PI-021/PI-022; T-19). A stale proxy is never audible (PI-007).
11. A project whose proxies were rendered at rate A plays them on an engine running at rate B —
    including with Primary missing — via off-audio-thread-prepared playback adaptation; the rate
    mismatch alone never produces Stale or silence (PI-030; T-17/T-25).
12. Proxy playback memory and I/O stay within the explicit budget selected via SPIKE-03, with
    many simultaneous long proxies (PI-031; T-26).
13. `Prepare Portable Project` produces a validated portable package or an explicit blocker
    report; cancellation/failure leaves no partial package; the package plays cross-machine and
    cross-rate without Primary (PID-011; T-27), and it correctly overrides all four update modes
    without changing any persisted mode (T-27).
14. Proxy validity/publication never compares freshly captured raw plugin-state bytes; volatile
    plugins publish through the same revision-based path with full proxy support, and the UI
    exposes the host-observed currency explanation for them (§9.4; T-30).

## 25. Evidence-gated decisions, deferred design, and traceability

### 25.1 Open decisions (authoritative list)

Exactly the items in the top-of-document table, all evidence-gated or deliberately deferred —
no product-behavior decision remains open: **PID-005** (tail numeric values — SPIKE-02-gated),
**OI-002** (bounded playback I/O and rate-adaptation mechanism — SPIKE-03-gated, blocks P1G),
**PID-009** (Secondary persistence shape — reviewed, deliberately deferred to P2, non-blocking
for P1). PID-001 compatibility limitations (non-blocking, documented in §9.2/§9.4): E2
(MIDI-learn-class silent state) and plugins beyond the two measured — these are accepted
observability limitations, not open decisions. Resolved by the 2026-09-03 human review: OI-001
(→ Locked cross-rate playback requirement), PID-008 (→ Locked first-P2 audition split), PID-011
(→ Locked P1 packaging, P1J), and PID-002/003/004/006/007/010 (→ Locked v1 behavior). The
2026-09-05 review's PID-001 resolution ("SPIKE-01 PASS") was premature (historical); the final
status is set by the 2026-09-06 review (revision 5): **PID-001 → Locked (reviewed) — hybrid
authoritative-state and host-observed identity contract** (§9.4; evidence verdict PARTIAL PASS).

### 25.2 Traceability: invariants → slices → tests

| Invariant | Slice(s) | Test(s) |
|---|---|---|
| PI-001, PI-002 | P1G | T-19, T-22 |
| PI-003 | P1B, P1G | T-05, T-19 |
| PI-004, PI-005 | P1B (naming reservation only), P2 | P2 acceptance (deferred); guarded by §22 exclusions |
| PI-006, PI-007 | P1F, P1G | T-13, T-19 |
| PI-008 | P1C, P1G | T-07, T-08 |
| PI-009 | P1C | T-02, T-06 |
| PI-010 | P1D | T-04, T-16 |
| PI-011 | P0/P1A, P1D | T-09, T-31 |
| PI-012, PI-013 | P1E | T-10, T-11, T-24, T-29 |
| PI-014, PI-015 | P1D, P1G | T-15 |
| PI-016 | P1D | T-16 |
| PI-017 | P1D, P1E | T-09, T-10 |
| PI-018, PI-019 | P1C | T-05, T-06, T-07 |
| PI-020 | P0/P1A, P1C | T-01, T-30 |
| PI-021, PI-022 | P1G, P1I | T-19 |
| PI-023 | P1H, P1J | T-20, T-27, T-28 |
| PI-024, PI-025, PI-026 | P1F, P1H, P1J | T-13, T-14, T-18, T-23, T-27 |
| PI-027 | P1H | T-20 (undo/dirty assertions) |
| PI-028 | P1E, P1F | T-12, T-30 |
| PI-029 | all (scope discipline) | slice exclusion lists (§22) |
| PI-030 | P1G, P1J | T-17, T-25, T-27 |
| PI-031 | SPIKE-03, P1G | T-26 |

### 25.3 Quality-gate cross-check (performed for the Canonical revision 3)

* All eleven audit decisions appear in the register (§21 cross-check line), plus OI-001
  (resolved → Locked) and OI-002 (Open, SPIKE-03-gated). ✔
* No current-code section claims fully deterministic equal-time scheduling before ORD-1: §4.3
  presents the verified merge structure together with the unresolved unstable-sort gap, and every
  stored-order tie-break statement is marked as post-ORD-1 target semantics (§4.3, §8.3, §11.4,
  T-03). ✔
* No section still claims that a sample-rate mismatch makes a proxy stale or silent; validity is
  evaluated under the generation's recorded render configuration everywhere (§3, §12.3, §13,
  §15.3, §17), and no engine/device-rate change appears as a proxy-staleness trigger — T-06
  asserts the negative explicitly; §13.3's rate row is adaptation-only. ✔
* No failure path calls a retained stale generation Current or playable: renderer failure,
  publication failure, tail-limit failure, and obsolete jobs all retain the previous published
  generation with its status as a pure fingerprint verdict, and "Current" is used exclusively for
  fingerprint-currency under the generation's recorded render configuration — never for "latest
  file on disk" or "previously published" (R9, §13.1, §13.3, §16.3, §20, criterion 4, T-13). ✔
* TLD-1 distinguishes the persisted timeline coordinate domain from the current engine domain,
  requires conversion at every consuming boundary (the proxy snapshot is only one of them; today's
  device-rate interpretation sites are enumerated), requires atomic rescale on any explicit
  reference-rate change, and documents the v19 legacy migration limitation (§10.1, P1B gate,
  T-21). ✔
* No section still accepts unbounded full proxy preload; playback I/O is budget-bounded and
  SPIKE-03-gated (§7.3, PI-031, OI-002). ✔
* No section still allows a tail-truncated render to publish as Current; the tail-cap path is a
  diagnosed Failed state with the previous generation retained (§12.2, §13.2, §15.2, T-16). ✔
* No section still describes stale-proxy playback as available "explicitly"; stale proxies are
  never audible in P1 (§3, PI-007, §17, §20). ✔
* `Prepare Portable Project` is a Locked P1 operation with its own slice P1J and tests (§16.6,
  §22, T-27); no section still defers it beyond P1 or reduces it to validator-only behavior. ✔
* P1B adds no Secondary placeholder schema; PID-009 is reviewed-deferred (§12.4). ✔
* The canonical serialization's collection ordering mirrors verified live scheduling (§8.3
  re-verification) — clips by (startSamples, stored order), notes in stored order, CC in
  normalized order, sources in session order; no invented sort key remains (§11.4). The
  stored-order equal-key tie-breaks become delivery guarantees only once ORD-1 lands, which is
  exactly why ORD-1 blocks P1C. ✔
* The two source-verified gaps are recorded as blocking prerequisites with proposed smallest
  fixes, not concealed: ORD-1 (unstable bake sorts → P1C) and TLD-1 (re-stamped timeline
  reference rate → P1B) (§8.3, §10.1, HR-10/HR-11). ✔
* Every Must-fingerprint field F1–F12 from the Audit appears in §11.1, extended by F1v/F13;
  Excluded, Runtime-only, and Unresolved entries preserved (§11.2/§11.3/§11.5). ✔
* Every locked product decision maps to at least one invariant (PI-001…PI-029 cover §§1–17 of the
  product decisions; see §5 groupings). ✔
* Every invariant maps to at least one slice and test (§25.2). ✔
* Primary/Proxy/Secondary selection contains no contradiction: Secondary never outranks a current
  proxy (§17 rows), never overwrites Primary or the proxy (PI-004), is never authoritative. ✔
* No path reuses the live plugin instance: renderer uses R6 isolated instances only (PI-011,
  T-09); mixdown's live-instance reuse is explicitly rejected as a model (PI-017). ✔
* Proxy audio is never a visible track or mixer channel (PI-001; §17 substitution-only seam). ✔
* Whole-project dirty state is never proxy validity (PI-018; staleness is fingerprint-only,
  §12.3). ✔
* Latency preservation (Locked, v1) and future PDC (deferred) are kept distinct throughout
  (§15.1, PID-005); `LatencySettingsStore` is explicitly not PDC. ✔
* All proposed types/APIs/folders are marked Proposed (§6, §7.3, §10, §12, §16). ✔
* Current-code claims cite the Audit and/or re-verified sources (§4, §7, §8, §14, §16). ✔
* No implementation was performed for this document. ✔

### 25.4 Quality-gate cross-check (performed for revision 5, 2026-09-06)

Contradiction searches performed against the revised text; every operative statement now agrees
with the reviewed SPIKE-01B evidence, and superseded statements are explicitly marked historical:

* No operative statement implies automatic rendering begins a few seconds after an edit, or that
  every edit immediately starts a render: the short ~2 s debounce is marked historical in §18.1;
  Auto rendering starts only after the fixed five-minute per-destination idle period, while
  staleness is still marked immediately (§18.1, PID-002, T-28). ✔
* No statement says Save always renders all stale proxies: in Auto, Save queues only
  already-eligible work with no duplicate jobs (no render storm); On Save queues the latest stale
  eligible destinations; Manual/Off queue nothing (§18.1, §18.2, T-20). ✔
* No statement lets autosave start proxy rendering — autosave never renders, in any mode
  (§18.1, §18.2, T-20). ✔
* No statement restricts volatile plugins to Manual-only: volatile plugins retain normal proxy
  support including Auto (§9.4.5, §19). ✔
* No operative statement lets raw blob hashes determine currency or publication: publication and
  obsolete-job checks compare host-managed revisions (§9.4.2, F2 note §11.1, §12.3, §13.3, T-30);
  byte equality survives only as positive "unchanged" rescue for byte-stable-classified plugins at
  host-observable quiescence, and byte inequality proves nothing. ✔
* No statement requires the transport to stop throughout rendering: background rendering MAY run
  concurrently with playback on an isolated instance (§9.4.4, §14.3, T-29); the SPIKE-02-gated
  pause-during-playback default is explicitly a resource-contention safeguard, never a correctness
  requirement (§14.3). ✔
* No statement claims capture during active destination MIDI/CC is proven clean: it is explicitly
  not proven to provide clean project-start initial conditions, and the P1D lifecycle validation
  (restore → prepare → reset/flush → deterministic chase → render from start) is the gate, with a
  compatibility-limitation fallback (§9.4.4, T-31). ✔
* No statement treats host-observable quiescence or notification silence as proof of internal
  plugin rest (§3 terminology, §9.2 findings item 4, §9.4.4, §9.4.5). ✔
* PID-001 is neither wholly Open nor absolutely solved: it is **Locked (reviewed)** by explicitly
  accepting the observability boundary, with E2/MIDI-learn and broader plugin coverage as
  documented, non-blocking compatibility limitations (header, gate table, §9.2, §9.4, §21,
  §25.1). Revision 4's premature "Resolved (PASS)" wording is retained only as explicitly marked
  historical text. ✔
* State classification: update mode and required host-observed identity metadata are
  project-persisted; the Auto countdown, last-edit timestamps, job objects, progress, and probe
  state are runtime-only; Current/Stale/Failed, the host-observed compatibility explanation, and
  queue eligibility are derived; countdown and progress never dirty the project or create undo
  steps (§18.3, §20, T-20, T-28). ✔
* No Proposed API was promoted to Existing by this revision; new §9.4/§18.1 mechanisms remain
  Proposed until repository evidence confirms them. ✔
