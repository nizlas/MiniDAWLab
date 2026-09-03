# Portable Instruments and Proxy Rendering — Technical Steering Document

**Status:** `Draft for design review`
**Date:** 2026-09-03
**Authority when canonical:** normative for all Portable Instruments / Proxy implementation slices.

> **This document is not yet canonical.** It becomes DAL's canonical steering document for
> Portable Instruments and Proxy Rendering **only after explicit human review** of the open
> decisions listed immediately below. Until then it is a design-review draft: implementation
> prompts MUST NOT treat Recommended or Open items as settled.

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
| **Recommended** | Technically preferred, awaiting human review before becoming Locked. |
| **Open** | Requires a human product or architecture decision. |
| **Proposed** | A type, API, component, folder, or seam that **does not exist yet**. |

**Normative language:** MUST / MUST NOT mark locked requirements and correctness invariants.
SHOULD / SHOULD NOT mark strong recommendations. MAY marks optional or deferred behavior.
No Proposed type or API is ever described as though it already exists.

---

## Decisions requiring human review

These are the only decisions still **Open** after this analysis. Everything else in this document
is Verified, Locked, or Recommended. Full context for each lives in the decision register (§21).

| ID | Question | Options | Agent recommendation |
|---|---|---|---|
| **PID-001** (part) | Authoritative plugin-state capture mechanism: how does DAL observe "the user tweaked VB3-II drawbars" so the proxy goes stale and renders from *current* state? (Audit U2, H2) | (a) capture on instrument-editor close + on render enqueue; (b) periodic message-thread polling of `getStateInformation` with hash compare; (c) JUCE parameter-listener–driven dirty hint + capture at enqueue; (d) combination of (a)+(c) | Run blocking spike **SPIKE-01** (§22 P0/P1A) first; provisional preference is (d): a cheap change *hint* from listeners/editor-close, with the authoritative blob always captured fresh at render-enqueue and at Save. Do not lock any mechanism without spike evidence. |
| **PID-005** (part) | Tail-policy numeric values: silence threshold, required silent duration, maximum tail duration. | e.g. threshold −60 dBFS vs −72 dBFS; silent window 250 ms vs 1 s; max tail 10 s vs 30 s vs 60 s | Keep structure Locked (§15.2) and numbers Open; propose starting values (−60 dBFS, 1 s window, 30 s cap) to be confirmed against SPIKE-02 measurements with VB3-II Leslie/reverb tails. |
| **PID-008** (part) | Live audition when Primary is missing but a current proxy plays transport: may Secondary provide live audition for newly played notes while the proxy supplies transport playback? | (a) yes, Secondary auditions while proxy plays transport; (b) no, audition is silent with a status explanation; (c) defer entirely to P2 | (c): defer to P2 with Secondary itself; v1 shows an explicit "audition unavailable — Primary missing" status. Mixing two sources simultaneously must be provably clear to the user before (a) is allowed. |
| **PID-009** (part) | Secondary persistence/registry shape (second descriptor+state home per instrument track). | (a) second embedded descriptor/state block inside the same `experimentalInstrumentTracks[]` entry; (b) a parallel array; (c) a generalized role list | P2 decision. For P1B only reserve non-colliding schema naming (§12.4). Provisional preference: (a). |
| **PID-011** (part) | `Prepare Portable Project` flow: modal wait vs background task with completion notification; what "all required proxies current" means for tracks in mode Off. | modal blocking dialog; cancellable progress task; export-style wizard | Deferred (post-P1H). Provisional preference: cancellable progress task reusing the render queue, hard-failing on tracks whose proxies cannot become current. |
| **OI-001** | Sample-rate portability of proxy playback: what happens when a project with proxies rendered at rate A is opened on a machine running at rate B? | (a) mark proxies stale, re-render if possible, silent otherwise; (b) offline resample at load; (c) realtime resampling playback | (a) for v1: metadata-checked staleness, no resampler (§15.3). (b)/(c) are quality/complexity trade-offs a human should weigh; (c) is the only option that helps when the Primary plugin is missing on machine B, which is the portable use case — this tension is exactly why the item stays Open. |

---

## 1. Document authority, status, and change protocol

1. **Status ladder.** `Draft for design review` → (explicit human review of §Decisions-requiring-human-review) → `Canonical`. Only a human reviewer moves the status.
2. **Precedence when canonical.** For Portable Instruments / Proxy work this document takes
   precedence over phase narratives and chat history; `docs/CURRENT_ARCHITECTURE.md` remains the
   authority for how the codebase is wired *today* (this document describes verified anchors plus a
   Proposed target).
3. **Change protocol.** Implementation slices MUST NOT widen this envelope. If an implementation
   discovers that a Locked invariant is wrong or a Recommended choice is infeasible, the slice
   stops and proposes the smallest steering amendment (per `docs/IMPLEMENTATION_GUIDE.md` Steering
   Document Change Rule); the amendment is reviewed by a human before code proceeds.
4. **Stable IDs.** Invariants `PI-###`, decisions `PID-###` (mirroring Audit D1–D11), additional
   open items `OI-###`, high-risk findings `HR-#`, spikes `SPIKE-##`, roadmap slices `P0/P1A…P1I`,
   tests `T-##`. Future edits MUST NOT renumber existing IDs.
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

### 2.2 Goals (Locked)

* One visible instrument/MIDI track per instrument destination — proxies add **no** visible
  arrangement track and **no** mixer channel (PI-001).
* Proxy playback is transparent downstream: DAL inserts, fader, pan, sends, buses, master, and
  offline mixdown are shared, unchanged (PI-002, PI-008).
* Deterministic staleness: proxies represent exactly the musical content they were rendered from,
  proven by a canonical fingerprint (PI-018, PI-019).
* Background rendering that never blocks or corrupts the realtime engine (PI-011, PI-012).

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
| **Generation** | One published proxy asset identified by its fingerprint hash; immutable once published (Recommended; §16). |
| **Stale proxy** | A retained, previously valid proxy whose fingerprint no longer matches current content. It remains an asset but MUST NOT silently be presented as the current sound of edited MIDI (Locked). |
| **Update mode** | Per-destination proxy maintenance mode: Auto / Paused(Manual) / Off (§18.1). |

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

### 4.3 Deterministic MIDI merge

Per block: the destination's own controller schedules first, then each `TrackKind::Midi` source in
session order resolves `midiTo` from the current `SessionSnapshot` and schedules into the
destination's host; emission order is pending Note Offs → CC (chase + in-segment) → Note Ons;
`juce::MidiBuffer` preserves insertion order at equal offsets (Audit §3.1). The stability fixture
already builds the Organ Upper/Lower/Pedal shape (Audit §5.5).

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
| HR-1 | Plugin state is captured too late (save-only) for correct fingerprinting and rendering (Audit H2, U2) | §9, PID-001, SPIKE-01, slice P0/P1A, tests T-01 |
| HR-2 | Plugin version is not persisted in the project descriptor (Audit U1, H3) | §9.2, §11 (F1v), §12, PID-001, slice P1B, T-05 |
| HR-3 | Reverse incoming-MIDI enumeration duplicated at ≥6 sites (Audit §5.2, H4) | §8.2, PID-010, slice P1C, T-02 |
| HR-4 | Track order is semantic for same-time CC last-wins (Audit F9, H5) | §8.3, §11 (F9), T-03, T-06 |
| HR-5 | Existing mixdown blocks the message thread, gates the engine, reuses live instances — unsuitable as background-render model (Audit §6, H1) | §14.4, PI-017, PID-004, slices P1D/P1E, T-09/T-10 |
| HR-6 | No PDC policy exists anywhere (Audit H7) | §15.1, PI-014/PI-015, PID-005, T-15 |
| HR-7 | No reusable tail policy exists (Audit §6.1) | §15.2, PI-016, PID-005, T-16 |
| HR-8 | No cancellation/generation mechanism prevents stale publication (Audit §6.1, §7.4) | §13, PI-028, PID-004, slice P1E, T-11/T-12 |
| HR-9 | Sample-rate portability requires an explicit policy (Audit U5, H6) | §15.3, OI-001, T-17 |

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
  and published successfully. A stale proxy MUST NOT silently be presented as the current sound of
  edited MIDI.

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
* **PI-020** The current audible Primary state MUST be included in a render snapshot; parameter
  changes MUST make the proxy stale; rendering MUST NOT silently use an older Save-time state;
  Save, autosave, proxy rendering, and plugin-editor close MUST agree about current state (§9;
  mechanism gated by SPIKE-01).

### Playback source

* **PI-021** Automatic transport source priority is: (1) Primary live when installed, available,
  and usable; (2) current Primary proxy when Primary is unavailable; (3) Secondary live when no
  current Primary proxy represents the current edited state; (4) an explicit missing/silent state.
  A stale proxy MAY be retained as an asset but MUST NOT silently represent current edited MIDI as
  current. Source changes MUST NOT occur silently.
* **PI-022** DAL MUST expose which source the user is hearing: Primary; Proxy current; Proxy
  rendering; Proxy stale; Secondary; Missing; Render failed.

### Modes, save, media

* **PI-023** Update modes Auto / Paused(Manual) / Off behave per §18.1. Normal Save MUST remain
  responsive: capture required current state, save project data, ensure eligible stale work is
  queued, and MUST NOT wait for rendering.
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
* **PI-029** Product phases are preserved: P1 Primary + hidden proxy; P2 Secondary + simple remap;
  P3 missing effect plugins + post-insert freeze; P4 verified render optimizations / partial
  rendering. P2–P4 work MUST NOT be pulled into P1.

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
| R10 | Playback-source selection | per-host atomic source mode + proxy buffer (host-level seam, Recommended §7.3) | `ExperimentalInstrumentHost` (RT view) driven by `InstrumentProxyCoordinator` | Message publishes; audio thread acquires |
| R11 | UI/status adaptation | `ProxyStatusModel` (+ existing Inspector/track-header seams, Audit §9) | UI layer | Message |
| R12 | Portable-project validation and collection | `PortableProjectValidator` (deferred, PID-011) | `ProjectIoCoordinator` | Message + queue reuse |

Per-responsibility contract:

* **R1 — Project/model (Proposed).** Mutable state: persisted proxy metadata fields. Inputs:
  publication events, load. Outputs: v20 JSON fields. Failure: absent/invalid metadata degrades to
  "no proxy" (PI-025). Cancellation: n/a. Shutdown: persisted with normal Save.
* **R2 — `PrimaryStateAuthority` (Proposed).** Mutable state: last-captured state blob + capture
  timestamp + cheap change hint. Inputs: editor lifecycle events, capture requests (Save, enqueue),
  mechanism per PID-001/SPIKE-01. Outputs: authoritative state blob for snapshots and Save;
  staleness hint. Failure: capture failure → proxy render blocked with diagnosed status, Save
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
  config. Algorithm: §14.2. Outputs: temp WAV + measured metadata (latency, tail truncation flag,
  peak). Failure: any processing exception → Failed. Cancellation: checked each block. Shutdown:
  via R7.
* **R9 — asset store (Proposed).** Mutable state: on-disk generations + per-destination current
  pointer. Inputs: validated temp files + fingerprint identity. Outputs: atomically published
  generation files, retirement of superseded generations (§16). Failure: publication failure keeps
  the previous generation current (PI-024). Cancellation: obsolete temp files deleted safely.
  Shutdown: temp cleanup on next launch (crash recovery §16.4).
* **R10 — source selection (Proposed).** Mutable state: per-host atomic
  `shared_ptr<ProxyPlaybackView>` (Proposed: immutable {mode, buffer, latency metadata}) following
  the `activeOwner_` swap pattern (§4.2). Inputs: proxy status, Primary availability. Outputs: the
  audio thread reads proxy samples at the timeline position instead of calling `processBlock`
  (§7.3). Failure: absent view → silent + Missing status (never crash). Cancellation: n/a.
  Shutdown: engine teardown unchanged (publish-before-destroy discipline, Audit §3.2).
* **R11 — status model (Proposed).** Mutable state: per-track UI status snapshot (PI-022
  vocabulary). Inputs: coordinator events. Outputs: Inspector/track-header status, notifications
  on source change (PI-021). Failure: UI-only. Shutdown: n/a.
* **R12 — portable validation (Proposed, deferred).** Waits until required proxies are current or
  reports blocking failure/cancellation (PID-011). MUST NOT run during normal Save (PI-023).

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
* Proxy read policy: **Recommended** — v1 preloads the proxy fully into memory following the
  `AudioClip` precedent (no streaming reader exists; §4.6). A realtime-safe streaming reader is a
  P4 candidate. Memory implications are accepted for v1 (typical mono/stereo WAV of one
  destination's arrangement span).
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

**Recommended (PID-010):** one central authoritative enumerator
`midi_dependency::sourcesForDestination(const SessionSnapshot&, TrackId)` (Proposed, R3) built in
slice P1C, used by snapshot construction and fingerprinting. It MUST reproduce the verified merge
semantics: destination first, then eligible sources in session order.

### 8.3 Deterministic scheduling contract (Verified basis, contract Locked)

The render snapshot MUST reproduce, from tick-domain data, exactly the event stream the live
engine would deliver:

* Emission order per segment: pending Note Offs → CC (chase, then in-segment) → Note Ons
  (Verified, Audit §3.1).
* Equal-time CC last-wins depends on session track order — order is semantic and MUST be captured
  (F9, HR-4).
* CC chase: latest event strictly before segment start; no invented default before the first
  point; per-stream dedup (Verified).
* Note Off: explicit `noteOffAbsSample`, else the 100 ms `gateSamples` fallback at the render
  sample rate (Verified, Audit §3.1) — the gate rule inputs are fingerprint field F10.
* Tick→sample conversion uses per-clip `bpm` and TPQ (Verified, Audit §5.4). Snapshots MUST bake
  from ticks at the **render** sample rate, never reuse live-rate baked samples (HR-9).
* The `MidiDeliveryCaptureSink` seam (Verified) is the reference oracle: tests compare the
  renderer's delivered MIDI against live delivery (T-03, T-04).

### 8.4 Mute/enable semantics (Locked + Recommended)

* Source eligibility (source off/mute) gates content and is fingerprinted (F8).
* **Recommended (PID-006):** destination proxy rendering is independent of the destination's
  current mute/off state — the proxy represents the destination's musical content, not the current
  monitoring choice. Destination mute/off remains a playback-time gate (Verified mix-stage skip)
  and is excluded from the fingerprint.

## 9. Plugin state and identity contract

### 9.1 The problem (Verified, HR-1)

State is captured only at explicit project save; instrument-editor close does not capture
(§4.5). Fingerprints based on the last-saved blob silently miss live tweaks; per-check fresh
`getStateInformation` calls cost message-thread time (Audit H2/U2).

### 9.2 Authoritative state-capture contract (Locked contract; mechanism Open)

The contract (PI-020) requires, whatever the mechanism:

1. The state blob in a render snapshot equals the currently audible Primary state at enqueue time.
2. A parameter change that alters audible output eventually makes the proxy stale (bounded
   detection delay is acceptable in Auto mode; §18.1 debounce).
3. Rendering can never silently use an older Save-time state.
4. Save, autosave, proxy rendering, and plugin-editor close agree on what "current state" is:
   Save MUST capture fresh state (it already does at save time — Verified); editor close SHOULD
   trigger capture/hint (instrument-editor close currently does not — Verified gap); render
   enqueue MUST capture fresh state.
5. All `get/setStateInformation` calls stay on the message thread (Verified constraint); the audio
   thread is never involved in capture.
6. Undo/dirty: state capture for proxy purposes MUST NOT by itself create undo entries or dirty
   the project (§18.3); the *user's* parameter edit dirties per existing rules.

**Open (PID-001 part) + blocking gate:** the detection mechanism (polling vs editor-close capture
vs parameter listeners vs combination) is NOT selected here — the repository provides no evidence
about listener reliability or `getStateInformation` cost for VB3-II-class plugins. **SPIKE-01
(named validation spike, blocking P0/P1A gate):** measure `getStateInformation` cost and blob
stability (same state ⇒ same bytes?), test parameter-listener coverage for GUI-driven changes, and
verify editor-close hooks; report evidence and select the mechanism for human confirmation. No
fingerprint slice (P1C) may complete before SPIKE-01's outcome is reviewed. If blobs are not
byte-stable for identical audible state, the fingerprint design of §11 falls back to
capture-generation counters plus conservative staleness (documented in the spike report).

### 9.3 Plugin identity and version (Verified gap HR-2; Recommended resolution)

The persisted descriptor has no version field (Verified, Audit §7.1/U1). **Recommended:** persist
`PluginDescription::version` at schema v20 (§12); include it in the fingerprint (F1v). A version
change ⇒ fingerprint change ⇒ deterministic staleness. Same-version-different-binary upgrades are
accepted as undetectable in v1 (documented limitation). Alternatives (live-version-only checks,
binary hashing) are noted in PID-001.

## 10. Immutable render-snapshot schema (Proposed)

`ProxyRenderSnapshot` (Proposed) is built on the message thread at enqueue, then never mutated.
Draft shape (field names are Proposed; all content derives from Verified structures cited in §11):

```
ProxyRenderSnapshot (immutable, Proposed)
├─ destinationTrackId          : TrackId
├─ pluginIdentity              : { fileOrIdentifier, uniqueId, deprecatedUid, format,
│                                  isInstrument, version }            // F1, F1v
├─ pluginStateBlob             : bytes (authoritative current state)  // F2, via R2
├─ destinationClips[]          : { startSamples→ticks-domain anchor data, bpm, tpq,
│                                  notes[] {midiNote, velocity, offVelocity, channel,
│                                           startTick, durationTicks},
│                                  ccPoints[] {startTick, controller, value, channel,
│                                              interpolationToNext} } // F3, F4, F5
├─ destinationMidiOutputChannel: int                                  // F6
├─ sources[] (ordered = session order)                                // F7, F9
│    └─ { sourceTrackId, midiOutputChannel, eligibility {off, muted}, // F7, F8
│         clips[] (same shape as destinationClips) }
├─ noteOffGateRule             : { gateMs = 100, renderSampleRate }   // F10
├─ renderConfig                : { sampleRate, blockPolicy }          // F11
├─ policies                    : { latencyPolicyVersion, tailPolicyVersion,
│                                  renderPolicyVersion, proxyFormatVersion } // F12, F13
├─ spanRule                    : { start = project start (0),
│                                  end = last relevant event + tail } // PID-007 (Recommended)
└─ fingerprint                 : ProxyFingerprint (computed by R5 from the above)
```

Rules:

* The snapshot stores **tick-domain** musical data plus bpm/TPQ/anchors and re-bakes to samples at
  the render sample rate (Verified rationale: baked samples are live-rate-dependent, Audit §5.4).
* The snapshot is self-sufficient: the render job never reads `Session`, live hosts, or
  controllers after construction (PI-011 isolation, PI-013 safe cancellation).
* Runtime-only data (§11.3) never enters the snapshot.

## 11. Canonical fingerprint specification

### 11.1 Must-fingerprint fields (Verified field inventory, Audit §10.1 — preserved in full)

The Audit's classification is preserved entry-for-entry; F1v/F13 extend it per Locked latency and
format policies. Every field below is a fingerprint input.

| # | Field(s) | Source of truth | Notes |
|---|---|---|---|
| F1 | Primary plugin identity: `fileOrIdentifier`, `uniqueId`, `deprecatedUid`, format, `isInstrument` | `ProjectFileGenericVst3DescriptorV1` / live `lastLoadedPluginDescription_` | Verified |
| F1v | Plugin version (explicitly defined version identity) | `PluginDescription::version`, persisted at v20 (Recommended §9.3) | resolves Audit U1 |
| F2 | Primary plugin **state blob** (current, not last-saved) | authoritative capture via R2 (§9.2) | mechanism gated by SPIKE-01 (Audit U2) |
| F3 | Destination's own MIDI clips: per clip `startSamples`, `timelineAnchorSamples`, `lengthSamples`, pattern `bpm`, `ticksPerQuarter` | `InstrumentMidiClip` | tempo & tick→sample conversion inputs; Audit U4: per-clip bpm is fingerprinted (Recommended: confirmed), project-bpm sync edits surface as clip-bpm content changes |
| F4 | Every note: `midiNote`, `velocity`, `offVelocity`, `channel`, `startTick`, `durationTicks` | `TimelineMidiNote` | note timing, length, velocity, channel, Note Off semantics |
| F5 | Every CC point: `startTick`, `controller`, `value`, `channel`, `interpolationToNext` | `MidiCcPoint` | CC ordering/interpolation; chase-relevant state derives from these (§8.3) |
| F6 | Destination `midiOutputChannel` (effective-channel remap baked into events) | `Track` | native vs effective channels |
| F7 | The routed-source **set**: every `TrackKind::Midi` row with `midiDestinationTrackId == dest`, incl. each source's F3–F5 content and its own `midiOutputChannel` | `Track` + source clips | MIDI To destinations; complete dependency graph (PI-009) |
| F8 | Per-source eligibility gating MIDI: source `trackOff` (power), source `muted` | `Track` / controller `playbackEnabled` inputs | mute/enable semantics (source side) |
| F9 | Merge order: session track order of contributing sources | `SessionSnapshot` row order | equal-time CC last-wins depends on it (HR-4); track reorder ⇒ stale |
| F10 | Note-off gate rule inputs: 100 ms `gateSamples` derivation and render sample rate | `publishRenderSnapshot` ~2293 | Verified |
| F11 | Render sample rate and fixed render block policy | render policy (Proposed, §15.3/§15.4) | block-size-sensitive plugins |
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
* **Stable collection ordering:** destination clips by (timeline anchor, clip id); notes by
  (startTick, midiNote, channel, declaration index); CC points by (startTick, controller, channel,
  declaration index); sources strictly in session order (F9 — order itself is data).
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
| U2 | State-capture cadence/mechanism | **Open** — SPIKE-01, blocking gate (§9.2, PID-001) |
| U3 | Destination mute/off during render | Recommended: render independent of mute/off (§8.4, PID-006) |
| U4 | Clip bpm vs project bpm | Recommended: fingerprint per-clip bpm (source of truth for baking); project-bpm sync edits are content changes (F3) |
| U5 | Proxy render sample rate | Recommended + partially **Open** (§15.3, OI-001) |
| U6 | Span rule | Recommended: project start → last relevant event + tail (§15.6, PID-007) |

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
  "sampleRate": 48000,
  "lengthSamples": 12345678,
  "pluginLatencySamples": 256,        // PI-014: recorded, never pre-trimmed
  "latencyPolicyVersion": 1,
  "tailPolicyVersion": 1,
  "renderPolicyVersion": 1,
  "proxyFormatVersion": 1,
  "renderedUtc": "2026-09-03T12:00:00Z",
  "tailTruncated": false              // §15.2 failure marker
},
"proxyUpdateMode": "auto"            // "auto" | "paused" | "off" (§18.1)
```

### 12.3 Rules

* Proxy audio itself is never embedded in JSON (PI-024) — the schema stores metadata + relative
  path only.
* Current-vs-stale is **not** persisted as a boolean: it is computed by comparing `generationId`
  against a freshly built fingerprint at load and after edits (PI-018; a persisted flag would rot).
* Missing/corrupt referenced file at load ⇒ status degradation, project loads (PI-025).
* Musical undo continues to strip plugin blobs (Verified) and MUST also strip proxy metadata from
  undo comparison where it would create meaningless entries (§18.3).

### 12.4 Secondary reservation (Open, PID-009)

P1B MUST NOT model Secondary but SHOULD reserve non-colliding naming (e.g. keep the top-level entry
open for a future `secondaryInstrument` object) so the P2 schema change remains additive.

## 13. Track proxy state and render-job state machines

Two distinct machines (Proposed). The destination's *proxy status* is derived, user-facing; a
*render job's* execution state is internal to the queue.

### 13.1 Destination proxy status (derived)

| Status | Meaning |
|---|---|
| Absent / not requested | No proxy metadata/asset (or mode Off with none existing). |
| Current / published | Published generation's `generationId` equals the current fingerprint. |
| Stale | A retained generation exists but fingerprints differ (PI-007). |
| Rendering | A job for the current fingerprint is queued/preparing/rendering/finalizing. |
| Failed | The most recent job for the current fingerprint failed; previous generation (if any) is retained and reported as Stale+Failed detail. |

### 13.2 Render-job execution states

`Queued → Preparing → Rendering → Finalizing → Published` with exits to `Cancelled` (user/mode
action), `Obsolete` (fingerprint changed; superseded by a newer job), `Failed` (error). Terminal
states: Published, Cancelled, Obsolete, Failed.

* **Queued:** snapshot + fingerprint captured; waiting for the worker (pause conditions §14.3).
* **Preparing (message thread):** create isolated instance, restore state, `prepareToPlay`.
* **Rendering (worker):** block loop with cancellation/obsolescence checks per block (PI-013).
* **Finalizing:** tail completion, temp-file validation, latency/tail metadata.
* **Published (message thread):** currency check (fingerprint still matches) then atomic
  publication (PI-028, §16.3).

### 13.3 Race handling (Locked behaviors)

| Race | Required behavior |
|---|---|
| Edit during rendering | New fingerprint ⇒ running job becomes Obsolete at its next check; a new job is queued (Auto) or the track shows Stale (Paused). The obsolete job MUST NOT publish (PI-028). |
| Newer job supersedes older | Older job cancelled cooperatively; only the job whose fingerprint matches current content may publish. |
| Track deletion | Jobs for that destination cancelled; assets retired per cleanup policy (§16.5); engine-side removal follows existing publish-before-destroy + callback-drain discipline (Verified, Audit §3.2). |
| Primary removal (descriptor cleared) | Jobs cancelled; existing generations retained (they still represent the last authoritative sound — status Stale); no new renders until a Primary exists. |
| Routing change (`midiTo` edit) | Fingerprint changes for old and new destinations ⇒ both stale per PI-009. |
| Sample-rate change | Per §15.3: fingerprint change ⇒ stale; jobs in flight become Obsolete. |
| Save As / project move | Assets copied/relocated with the project (§16.6) before the new location is considered complete; fingerprints are path-independent so validity is preserved. |
| Project close | Cancel-all, bounded worker join, temp cleanup; never blocks on rendering completion (PI-023 spirit); queued work is re-derivable from fingerprints on reopen. |
| Application shutdown | Same as close plus instance destruction on the message thread (§6 R6/R7). |
| Publication failure | Previous generation retained and remains current-on-disk; job Failed; status surfaces per §20 (PI-024). |
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
10. Publish only if the job remains current (PI-028).
11. Discard obsolete or failed temporary output safely.

### 14.3 Execution policy (Locked capabilities, Recommended parameters)

The architecture MUST allow: offline/non-realtime plugin indication where supported; controlled
realtime fallback for incompatible plugins; low-priority execution; limited concurrency; render
progress and speed reporting; cooperative cancellation/obsolescence checks at safe block
boundaries; pausing during recording; optional pausing during playback if required to avoid
glitches; safe project close and application shutdown. Faster-than-realtime rendering is
normal-case but **not guaranteed**. **Recommended:** one worker, concurrency 1 (PID-004);
pause-during-playback defaults ON until SPIKE-02 measures contention.

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
  Y ⇒ tail complete. X, Y and the max-tail cap Z are **Open** (PID-005; starting values proposed
  in the top table).
* Hitting Z without silence ⇒ the asset is marked `tailTruncated: true`; behavior: publish with a
  visible warning status (Recommended) rather than fail — an audible-but-truncated proxy is more
  useful than none; the warning MUST be surfaced (§20).
* `tailPolicyVersion` MUST be in the fingerprint (F12); changing X/Y/Z bumps it ⇒ deterministic
  staleness.

### 15.3 Sample rate (Recommended policy; portability behavior Open — OI-001, HR-9)

* **Current repository behavior (Verified):** everything renders at the current device rate;
  render snapshots bake ticks→samples at the live rate; mixdown renders at device rate only
  (Audit §5.4, §6.1).
* **Recommended v1 render rate:** the project's current engine sample rate at enqueue time,
  recorded in metadata and fingerprinted (F11). Rationale: no resampler exists in the playback
  path, and rendering at the playback rate makes proxy reads sample-exact.
* **Playback resampling:** NOT required in v1 (Recommended); a proxy whose `sampleRate` differs
  from the engine rate is treated as stale.
* **Staleness on rate change:** device/engine sample-rate change ⇒ fingerprint change ⇒ stale ⇒
  re-render when Primary is available (Auto).
* **Portable project on another machine at a different rate:** **Open (OI-001)** — if Primary is
  missing there, re-rendering is impossible and option (a) yields silence; realtime resampling (c)
  is the only rescue but adds an engine feature. Human decision required; v1 ships (a) unless
  review chooses otherwise.
* **Required validation:** T-17 (§23).

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

### 15.6 Span rule (Recommended, PID-007)

Render from project start (sample 0) through the last render-relevant event of the destination's
dependency set, plus tail. Rationale: CC chase and held state make mid-timeline starts unsafe
(PI-010 reasons); rendering from zero guarantees the render instance experiences exactly the event
history the live engine would produce. The span end is part of the snapshot; the span rule version
is folded into `renderPolicyVersion`.

## 16. Media lifecycle and atomic publication

### 16.1 Layout (Recommended, PID-003)

* Separate project-relative folder `<ProjectFolder>/InstrumentProxies/` (Proposed name), a sibling
  of `Audio/` — the `Audio/` folder has user-clip semantics baked into save validation (Verified,
  Audit §8.4), so proxies MUST NOT live there.
* File naming (Proposed): `track_<TrackId>_<fingerprintHash>.wav` — stable `TrackId` ownership +
  content-addressed generation identity (Recommended: immutable generations, PID-003).
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
6. Failure at any step: temp deleted (or swept later), previous generation remains current
   (PI-024).

### 16.4 Recovery (Locked behaviors)

* **Missing/corrupt proxy at load:** keep the track row, degrade to Missing/Stale status with a
  diagnostic; never block load (PI-025) — the stronger variant of the existing skip-on-missing
  media policy (Verified §4.4).
* **Crash recovery:** on launch, sweep `tmp_*.wav`; validate metadata against on-disk files;
  orphaned generations (no metadata reference) are removable by cleanup.
* **Cleanup:** keep at most the current generation plus the retained previous valid generation per
  destination (Recommended); orphan sweep at load/save. No general media GC exists to reuse
  (Verified §4.6) — this is new, scoped code.

### 16.5 Track deletion

Assets of a deleted destination become orphans; removed by cleanup after the deletion is committed
(undo of track deletion within the session SHOULD restore metadata; the asset survives until
cleanup, making undo cheap — Recommended).

### 16.6 Save As / project move / portable collection

Save As MUST copy current (and retained previous) generations into the new project folder before
reporting success — noting that **no Save As media copy exists today** (Verified §4.6), so this is
new behavior in P1H. Portable collection (PID-011, deferred) validates "all required proxies
current" through the same fingerprint machinery and never packages plugin binaries/licenses
(PI-026).

## 17. Playback-source decision table

Automatic transport selection (Locked priority, PI-021). "Current proxy" means published
generation fingerprint == current fingerprint.

| Primary usable | Proxy status | Secondary usable (P2) | Transport source | Exposed status (PI-022) |
|---|---|---|---|---|
| yes | any | any | **Primary live** | Primary |
| no | Current | any | **Proxy** | Proxy current |
| no | Rendering (none published current) | yes | Secondary live | Proxy rendering (+ Secondary as source) |
| no | Rendering | no | silent | Proxy rendering |
| no | Stale (retained) | yes | **Secondary live** (stale proxy MUST NOT play silently as current) | Proxy stale (+ Secondary as source) |
| no | Stale | no | silent | Proxy stale |
| no | Failed (no current) | yes | Secondary live | Render failed (+ Secondary) |
| no | Failed | no | silent | Render failed |
| no | Absent | yes | Secondary live | Secondary |
| no | Absent | no | silent | **Missing** (explicit missing/silent state) |

Notes:

* Until P2 ships, every "Secondary usable = yes" row collapses to its "no" sibling (PI-005).
* Transport playback and live audition are separate concerns: audition (UI FIFO) requires a live
  instrument; a proxy cannot audition arbitrary new notes (Verified constraint, Audit §9). Whether
  Secondary may provide audition while a current proxy plays transport is **Open** (PID-008 part;
  top table).
* Source changes MUST be surfaced, never silent (PI-021); the seam publishes a status event on
  every mode transition (R11).
* Offline mixdown renders through the same selected source automatically at the host-level seam
  (Verified propagation, §7.3; T-22).

## 18. Save, autosave, undo, and dirty semantics

### 18.1 Update modes (Locked)

* **Auto (default — portable collaboration):** mark stale immediately after a render-relevant
  change; debounce repeated edits (Recommended default: ~2 s quiet window, value tunable); queue
  rendering when eligible (Primary available, not recording, mode allows).
* **Paused/Manual:** continue accurate stale detection; do not start automatic renders; allow
  explicit "Render now".
* **Off:** do not create or update proxies automatically; preserve existing metadata and assets
  safely (they simply age to Stale).
* Storage (Recommended): per-destination `proxyUpdateMode` persisted at v20 (§12.2).

### 18.2 Save and autosave (Locked)

Normal Save MUST: safely capture required current project/plugin state (fresh state capture already
happens at save — Verified §4.5 — and R2 keeps it authoritative); save project data; ensure
eligible stale work is queued; and MUST NOT wait for rendering (PI-023). Autosave (Verified:
message-thread timer, atomic writer) behaves identically and MUST NOT trigger renders by itself
beyond queueing already-eligible work. A future `Prepare Portable Project` operation is the only
flow that waits for proxies (PID-011, deferred).

### 18.3 Undo and dirty (Locked classification; see §19 of task → §20 state classes)

* Musical edits (notes, CC, routing, channels, clips) remain undoable and dirty the project —
  unchanged.
* Proxy progress, queue state, job states, and automatic source selection are non-undoable
  background state and MUST NOT create undo entries (PI-027).
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
* Explicit "Render now" (Paused mode) and mode selection Auto/Paused/Off per destination.
* Source changes surface as a visible status change, never silently (PI-021).
* Failure states (§20) present a reason (plugin failed to instantiate, tail truncated,
  publication failed, state capture failed) and the action taken (previous proxy retained).
* Proxy mode MUST NOT offer a Primary plugin editor when Primary is missing (Verified: missing
  plugin ⇒ no editor, Audit §9).
* No diagnostic waveform view of proxy audio in v1 (§2.3).

## 20. Failure, recovery, and trust model

State classification (Locked, task §16):

| Class | Contents |
|---|---|
| Project-persisted musical state | tracks, clips, notes, CC, routing, channels, descriptors, plugin state blobs, `pluginVersion`, `proxyUpdateMode` |
| External proxy media | generation WAV files under `InstrumentProxies/` |
| Persisted proxy metadata | the v20 `proxy` object (references + policy versions) |
| Runtime engine state | published playback views, source mode, render instances, worker queue |
| Runtime UI/view state | status model, progress display |
| Derived/cache state | fingerprints (recomputable), staleness verdicts |
| Undoable | musical edits only |
| Non-undoable background state | job states, progress, automatic source selection (PI-027) |
| Project-dirty effects | musical edits (existing rules); publication metadata updates; mode changes — never job progress |

Trust rules:

* The proxy system is advisory infrastructure: **no proxy failure may damage musical data**.
  Failures degrade status, never project content.
* Failed render ⇒ previous valid proxy retained and clearly reported (PI-007, PI-024).
* Stale is honest: the user always knows they are hearing Primary, a current proxy, a stale proxy
  (only ever explicitly), Secondary, or silence (PI-021/PI-022).
* Corrupt metadata/assets degrade like missing plugins today: row preserved, silent + status
  (PI-025; Verified precedent §4.6).
* Every failure path (instantiation, restore, prepare, processing exception, validation,
  publication, capture) has a defined terminal job state (§13.2) and a user-visible reason (§19).

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
*Blocking slice:* P1C (fields/serialization), P0/P1A (capture mechanism). *Status:* **Open** for
the state-capture mechanism (blocking, human review after SPIKE-01); field set and serialization
**Recommended**; field-set completeness relative to the boundary follows from Locked PI-019.

**PID-002 (Audit D2) — Staleness signalling.**
*Question:* Where fingerprints are compared (edit-time vs save-time vs render-enqueue-time) and the
UI contract for "proxy stale". *Audit evidence:* §5.6 (only runtime revision counters +
unsuitable dirty flag exist). *Recommended decision:* cheap runtime hints (controller
`ChangeBroadcaster` / render-snapshot `revision`, R2 state hints) mark *candidate* staleness
immediately; the authoritative verdict is a full fingerprint compare at debounce expiry and at
enqueue; at load, one full compare per destination. UI shows Stale from the authoritative verdict
(PI-022). *Alternatives:* full recompute on every edit (rejected: message-thread cost); save-time
only (rejected: violates PI-020). *Consequences:* staleness may lag edits by the debounce window in
Auto mode — accepted. *Required validation:* T-06, T-07, T-08. *Blocking slice:* P1C. *Status:*
**Recommended**.

**PID-003 (Audit D3) — Proxy asset layout.**
*Question:* Folder, naming, format, retention of the previous valid proxy, cleanup policy.
*Audit evidence:* §8.3/§8.4 (Audio/ has clip semantics; two atomic-publication variants; Windows
replace hazard H10; no media GC exists). *Recommended decision:* separate project-relative
`InstrumentProxies/` folder; immutable content-addressed generations
`track_<TrackId>_<hash>.wav`; publish-new-name + retire; keep current + previous generation;
orphan sweep (§16). Retention-on-failure is Locked (PI-007/PI-024). *Alternatives:* store in
`Audio/` (rejected: save-validation entanglement); mutable single file per track (rejected:
Windows replace hazard + PI-028). *Consequences:* small disk overhead (≤2 generations); new
cleanup code. *Required validation:* T-13, T-14, T-18. *Blocking slice:* P1F. *Status:*
**Recommended** (both preferences confirmed as product direction).

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
SPIKE-02; T-09, T-10, T-11, T-12. *Blocking slice:* P1D/P1E. *Status:* **Recommended** (worker
count and lifecycle); offline-indication details resolved by SPIKE-02 evidence, not human taste.

**PID-005 (Audit D5) — Tail and latency policy.**
*Question:* Tail length source, latency trim vs preserve, live-vs-proxy timing parity.
*Audit evidence:* §6.1 (no tails, no PDC anywhere; latency only logged — re-verified), §9
(latency interaction row). *Decision:* **Locked** — proxy v1 preserves plugin latency exactly as
§15.1 (no independent shift; metadata records `getLatencySamples`; latency-policy version
fingerprinted; no live↔proxy timing jump; full PDC explicitly deferred and, when it arrives, must
cover live/proxies/inserts/groups/master/mixdown consistently with deterministic proxy
reinterpretation-or-staleness). Tail structure Locked (§15.2). *Open:* tail numeric values
(threshold, window, cap) — human review with SPIKE-02 measurements. *Alternatives (latency):*
trim-at-render (rejected: creates live/proxy timing divergence today); trim-at-playback (rejected:
that *is* PDC — deferred). *Consequences:* proxies inherit today's uncompensated timing — matching
live behavior by design. *Required validation:* T-15, T-16. *Blocking slice:* P1D (policy versions
must exist before first renders). *Status:* **Locked** (latency, tail structure) / **Open** (tail
numbers).

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
*Status:* **Recommended**.

**PID-007 (Audit D7, U6) — Span rule.**
*Question:* What timeline range does a v1 proxy cover? *Audit evidence:* §6.1 (mixdown renders
locator span only), §10.4 U6. *Recommended decision:* project start → last render-relevant event
of the dependency set + tail (product-preferred; §15.6). *Alternatives:* first-event start with
synthesized chase preroll (deferred P4 optimization; risks CC-chase divergence); locator span
(rejected: proxies are not bounces). *Consequences:* long silent leaders render quickly but occupy
file length — acceptable; MAY be optimized later without fingerprint change if the output is
provably identical. *Required validation:* T-04, T-16. *Blocking slice:* P1C (span is snapshot
data). *Status:* **Recommended**.

**PID-008 (Audit D8) — Playback-source selection contract.**
*Question:* Host-level vs helper-level seam; timeline-position plumbing; the
Primary/Proxy/Secondary/silent priority table including audition with missing Primary.
*Audit evidence:* §4.2 (two seam candidates; mixdown propagation favors host-level; timeline
position missing at the mix call), §9 (interaction table). *Decision:* priority order **Locked**
(PI-021; table §17). Seam: **Recommended** host-level substitution (§7.3) with timeline plumbing
placed in slice P1G. *Open:* Secondary live audition while a current proxy plays transport (top
table; recommendation: defer to P2). *Alternatives:* helper-level seam (documented, not preferred
— must replicate the gain/pan fold and update mixdown in step). *Consequences:* one signature-level
change threading timeline position to the host. *Required validation:* T-19, T-22. *Blocking
slice:* P1G. *Status:* **Locked** (priority) / **Recommended** (seam) / **Open** (audition
behavior).

**PID-009 (Audit D9) — Secondary role scope.**
*Question:* Where Secondary's descriptor/state live (second host per track? second
`experimentalInstrumentTracks` entry?), never-sonically-identical presentation, later channel-remap
hook. *Audit evidence:* §3.2 (one-host-per-track registry does not model it). *Locked boundaries:*
Secondary is P2; never overwrites/redefines Primary or the Primary proxy; never presented as
sonically identical; instantiated only when needed; v1 Preserve channels + optional simple remap;
CC (incl. CC11) forwarded unchanged; advanced mapping deferred (PI-004, PI-005). *Recommended:*
P1B reserves non-colliding schema naming only (§12.4). *Open:* the persistence/registry shape
(top table; provisional preference: embedded second descriptor/state block per entry).
*Consequences:* none for P1 beyond naming discipline. *Required validation:* none in P1 (P2
acceptance later). *Blocking slice:* none in P1 (P2). *Status:* **Open** (shape; explicitly
non-blocking for P1).

**PID-010 (Audit D10) — Dependency enumeration centralization.**
*Question:* Centralize `sourcesForDestination()` before building fingerprint collection on top?
*Audit evidence:* §5.2 (≥6 duplicated scan sites; HR-3: drift risk between "what plays" and "what
was fingerprinted"). *Recommended decision:* yes — build the central authoritative enumerator
(product-preferred) in P1C; snapshot/fingerprint construction MUST use it; existing sites migrate
opportunistically, realtime paths only with care (§6 R3). *Alternatives:* a seventh private scan
(rejected: institutionalizes HR-3). *Consequences:* small shared utility + tests; later cleanup
opportunities. *Required validation:* T-02 (enumerator vs live merge equivalence at the capture
sink). *Blocking slice:* P1C. *Status:* **Recommended**.

**PID-011 (Audit D11) — Prepare Portable Project.**
*Question:* Packaging flow, relation to Save (fast, non-blocking), verification that "all proxies
current" before packaging. *Audit evidence:* §8.3 (no portable/collect feature, no Save As media
copy, no media GC). *Locked:* normal Save never waits for rendering (PI-023); packaging never
includes plugin binaries/licenses (PI-026); the explicit portable operation waits until required
proxies are current or reports a blocking failure/cancellation. *Recommended:* implement after
P1H, reusing the render queue and fingerprint verdicts; Save As asset copy lands in P1H as its
prerequisite. *Open:* the concrete flow (modal vs background task; treatment of mode-Off tracks) —
top table. *Consequences:* none for P1A–P1G. *Required validation:* T-23 (portable validation
logic), P1H tests. *Blocking slice:* none before P1H. *Status:* **Open** (flow), deferred by
design.

---

**Register cross-check:** D1→PID-001, D2→PID-002, D3→PID-003, D4→PID-004, D5→PID-005, D6→PID-006,
D7→PID-007, D8→PID-008, D9→PID-009, D10→PID-010, D11→PID-011 — all eleven present; none silently
resolved. Additional non-audit open item: OI-001 (sample-rate portability playback, §15.3).

## 22. Implementation roadmap

P1 is split into ordered, independently reviewable slices. **No slice combines schema, renderer,
background scheduling, publication, playback fallback, and UI.** Each slice states its completion
gate; test IDs refer to §23. Per `docs/DEVELOPMENT_TEST_POLICY.md`, ordinary slices run the
smallest falsifying tests (Level 1); broader stability scenarios are assigned only at the
integration gates marked below.

### P0/P1A — Blocking validation spikes and architectural prerequisites

* **Goal:** retire the two evidence gaps that block all later slices; no production behavior
  change.
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

### P1B — Project model/schema seam with backward-compatible loading

* **Goal:** v19→v20 additive schema for proxy metadata + plugin version; loading stays
  backward-compatible.
* **Included:** v20 fields of §12.2 (defaulted absent), `pluginVersion` persistence, undo
  blob-stripping extension (§12.3), Secondary naming reservation (§12.4). No asset I/O, no
  rendering.
* **Excluded:** fingerprinting, rendering, playback, UI beyond nothing-visible defaults.
* **Expected files/components:** `src/io/ProjectFile.h/.cpp`, `src/domain/Session.*` DTO plumbing,
  `src/instruments/InstrumentTrackController.cpp` (descriptor version capture at save).
* **Invariants protected:** PI-024 (metadata-not-media), PI-025 (absent/invalid ⇒ no proxy),
  PI-029 (no P2 modeling).
* **Prerequisite decisions:** PID-001 storage (Recommended — confirmed at design review), PID-003
  naming vocabulary (paths stored, folder may not exist yet).
* **Automated tests:** T-05, T-21 (v19 round-trip; v20 defaults; unknown-field tolerance per
  existing migration conventions).
* **Plugin-dependent/manual:** none.
* **Rollback:** revert version bump; v19 files never contained the fields.
* **Completion gate:** load/save round-trip of v19 and v20 fixtures passes; schema review.

### P1C — Centralized MIDI dependency snapshot and canonical fingerprint

* **Goal:** `midi_dependency::sourcesForDestination()` (R3), `ProxyRenderSnapshotBuilder` (R4),
  `ProxyFingerprintBuilder` (R5), staleness verdicts (PID-002) — all message-thread, no rendering.
* **Included:** snapshot schema §10; fingerprint §11 incl. canonical serialization; state capture
  per the SPIKE-01-selected mechanism (R2 first version); stale detection + debounce plumbing
  (no queue yet — verdicts only).
* **Excluded:** render worker, assets, playback changes, UI beyond diagnostics.
* **Expected files/components:** new `src/instruments/` or `src/app/` units for R2–R5; hooks in
  `InstrumentRuntimeCoordinator`.
* **Invariants protected:** PI-009, PI-018, PI-019, PI-020; PID-006/PID-007 semantics.
* **Prerequisite decisions:** SPIKE-01 gate (P0/P1A); PID-002/PID-006/PID-007/PID-010 confirmed at
  design review.
* **Automated tests:** T-01, T-02, T-03, T-05, T-06, T-07, T-08 (deterministic selftests; Organ
  Upper/Lower/Pedal fixture reused).
* **Plugin-dependent/manual:** T-01 blob checks against a real VST3.
* **Rollback:** feature is inert (no consumer yet); revert cleanly.
* **Completion gate:** fingerprint repeatability + full stale-trigger matrix green.

### P1D — Isolated complete-destination renderer in a controlled foreground test

* **Goal:** `ProxyRenderInstance` (R6) + `ProxyDestinationRenderer` (R8) render a snapshot to a
  temp WAV, driven synchronously from a diagnostic/selftest entry point (no background queue).
* **Included:** §14.2 steps 2–8 (foreground); latency metadata recording (§15.1); tail policy
  v1 (§15.2, provisional Open numbers under version tag); span rule (§15.6); render-config
  policies (§15.3–15.5).
* **Excluded:** background thread, publication into the project, playback substitution, UI.
* **Expected files/components:** new render units (likely `src/plugins/` or a new `src/render/`);
  diagnostics hook.
* **Invariants protected:** PI-010, PI-011, PI-014, PI-016, PI-017.
* **Prerequisite decisions:** SPIKE-02 results; PID-004 lifecycle; PID-005 latency (Locked) +
  provisional tail numbers.
* **Automated tests:** T-04 (capture-sink MIDI equivalence), T-09 (live instance untouched);
  Level-1 harness.
* **Plugin-dependent/manual:** T-15 (latency equivalence), T-16 (tail completion) with VB3-II;
  rendered-file listening check.
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
  T-24 (shutdown/track-deletion races). **Integration gate:** relevant stability scenarios
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

### P1G — Proxy playback substitution and missing-Primary fallback

* **Goal:** host-level seam substitution (§7.3): timeline-position plumbing, per-host
  `ProxyPlaybackView` publication, source selection per §17 (without Secondary rows), mixdown
  propagation.
* **Included:** PI-021 priority for {Primary, Proxy current, Missing}; stale proxies never play as
  current; preload read policy.
* **Excluded:** Secondary (P2), UI beyond existing missing-plugin status parity, modes (P1H).
* **Expected files/components:** `src/plugins/ExperimentalInstrumentHost.*`,
  `src/engine/PlaybackMixHelpers.*` / `PlaybackEngine.cpp` (timeline plumbing only),
  coordinator wiring.
* **Invariants protected:** PI-001, PI-002, PI-003, PI-006, PI-007, PI-008, PI-021.
* **Prerequisite decisions:** PID-008 seam (Recommended confirmed).
* **Automated tests:** T-19 (source-selection matrix, non-Secondary rows), T-22 (offline mixdown
  through selected source), T-08 (post-boundary changes don't re-render). **Integration gate:**
  Level 2 stability (smoke, delete-loop, mixdown) — audio-callback path touched.
* **Plugin-dependent/manual:** unload Primary mid-project; verify proxy playback through
  inserts/fader/pan; T-15 timing parity live↔proxy.
* **Rollback:** substitution branch behind the published view — absent view ⇒ exactly today's
  behavior.
* **Completion gate:** matrix + stability green; audible parity check recorded.

### P1H — Auto/Manual/Off behavior, Save/close semantics, and portable validation

* **Goal:** update modes (§18.1), Save/autosave queue-not-wait (§18.2), undo/dirty classification
  (§18.3), Save As asset copy (§16.6), portable-validation logic (`PortableProjectValidator`
  verdict only — flow UI deferred per PID-011).
* **Included:** debounce; mode persistence; close/shutdown ordering with the queue.
* **Excluded:** `Prepare Portable Project` UX flow (Open), Secondary.
* **Expected files/components:** coordinator, `ProjectIoCoordinator`, autosave timer touchpoints.
* **Invariants protected:** PI-023, PI-027, PI-029.
* **Prerequisite decisions:** none open (modes are Locked).
* **Automated tests:** T-20 (Save/autosave behavior), T-21 (backward-compat load), T-23 (portable
  validation verdicts), T-24 (close races). Level 2: autosave/recover scenarios.
* **Plugin-dependent/manual:** mode-switch walkthrough.
* **Rollback:** modes default Auto; feature flags not used — revert slice.
* **Completion gate:** Save latency unchanged (no waiting), mode semantics test-proven.

### P1I — Status UI, failure presentation, and end-to-end verification

* **Goal:** PI-022 status vocabulary in Inspector/track-header seams, progress/speed display,
  failure reasons (§19, §20), source-change surfacing; final end-to-end pass.
* **Included:** `ProxyStatusModel` (R11); notifications on source change.
* **Excluded:** pixel design mandates; diagnostic waveform view (non-goal).
* **Expected files/components:** `src/ui/InspectorView.*`, track-header coordinator, status model
  unit.
* **Invariants protected:** PI-021 (no silent changes), PI-022, PI-027.
* **Prerequisite decisions:** none open.
* **Automated tests:** status-model unit tests; T-19 re-run.
* **Plugin-dependent/manual:** full E2E: edit → stale → render → publish → unload Primary → proxy
  playback → mixdown; failure-path walkthrough. **Integration gate:** full relevant stability
  matrix per `docs/DEVELOPMENT_TEST_POLICY.md` Level 3 criteria (finishing a batch of runtime
  changes).
* **Rollback:** UI-only revert.
* **Completion gate:** E2E walkthrough recorded; first-version acceptance criteria (§24) checked.

### Recommended first implementation slice

**P0/P1A — specifically SPIKE-01 (authoritative state capture).** It is the single blocking gate
(PID-001 Open item) on which fingerprint correctness (P1C) and everything downstream depend, it
changes no production behavior, and its evidence lets the human review lock the largest remaining
architectural unknown. Per the task boundary, its implementation prompt is **not** written here
and implementation has **not** started.

## 23. Verification strategy

Tests are split into **deterministic selftests** (no real plugin; use the `MidiDeliveryCaptureSink`
seam, the Organ Upper/Lower/Pedal fixture, and capture-only mode — Verified test seams, §4.1/§8.2),
**plugin-dependent integration tests** (real VST3, VB3-II-class), and **manual checks**. Broad
stability regression runs only at the P1E/P1G/P1I integration gates (per
`docs/DEVELOPMENT_TEST_POLICY.md`), not per slice.

| ID | Test | Kind | Slice |
|---|---|---|---|
| T-01 | Authoritative plugin-state capture: snapshot blob equals currently audible state; editor-close/Save/enqueue agreement (mechanism per SPIKE-01) | plugin-dependent + deterministic harness parts | P0/P1A, P1C |
| T-02 | Deterministic incoming-source enumeration: R3 output equals live merge set/order (capture-sink comparison), incl. destination-mute independence (PID-006) | deterministic | P1C |
| T-03 | Deterministic equal-time MIDI/CC ordering incl. track-order last-wins; reorder ⇒ fingerprint change | deterministic | P1C |
| T-04 | VB3-II Upper/Lower/Pedal snapshot correctness: renderer's delivered MIDI stream ≡ live delivery at the capture sink for the fixture | deterministic (capture-only) + plugin-dependent audio | P1C, P1D |
| T-05 | Fingerprint repeatability: same content ⇒ identical hash across runs/processes; v19/v20 load parity | deterministic | P1B, P1C |
| T-06 | All required stale triggers: note/CC/channel edits, source add/remove/reroute, source mute/off, track reorder, clip bpm, destination output channel, plugin state change, plugin version change, sample-rate change, policy-version bumps | deterministic | P1C |
| T-07 | Post-boundary changes cause **no** staleness: fader, pan, inserts, sends, bus routing, master, destination mute/off | deterministic | P1C |
| T-08 | Post-boundary changes cause no re-render at playback level | deterministic | P1G |
| T-09 | Live instance never used by rendering: instrumented proof that render jobs touch only their own instance (live host untouched during render) | deterministic harness + plugin-dependent | P1D |
| T-10 | Audio thread never blocked by render activity (stability-invariant style measurement during renders) | deterministic + stability scenario | P1E |
| T-11 | Edit-during-render ⇒ job obsolete at next boundary; new job queued (Auto) | deterministic | P1E |
| T-12 | Obsolete job publication rejection: stale-fingerprint job cannot publish (PI-028) | deterministic | P1E, P1F |
| T-13 | Retention of previous proxy on render/publication failure (PI-007/PI-024) | deterministic | P1F |
| T-14 | Windows-safe publication: publish-new-name with an open handle on the old generation; retire without in-place replace | deterministic (Windows semantics) | P1F |
| T-15 | Latency equivalence: live Primary vs proxy v1 playback timing identical for a latency-reporting plugin (no jump on source switch) | plugin-dependent | P1D, P1G |
| T-16 | Tail completion (silence-detected end) and truncation failure marking at the max-tail cap | plugin-dependent | P1D |
| T-17 | Sample-rate behavior: rate change ⇒ stale; metadata records rate; mismatch never plays silently-wrong (per §15.3 policy) | deterministic + manual | P1C, P1G |
| T-18 | Missing/corrupt proxy recovery: project loads, status degrades, no crash (PI-025) | deterministic | P1F |
| T-19 | Source-selection matrix of §17 (v1 rows; Secondary rows added in P2) | deterministic | P1G, P1I |
| T-20 | Save and autosave: fresh state captured, eligible work queued, Save never waits (latency assertion) | deterministic | P1H |
| T-21 | Backward-compatible loading: v19 projects load with proxy fields defaulted; v20 round-trip | deterministic | P1B, P1H |
| T-22 | Offline mixdown renders through the selected source (Primary or proxy) with identical downstream processing | deterministic + plugin-dependent | P1G |
| T-23 | Portable-project validation verdicts: current/stale/failed/mode-Off combinations | deterministic | P1H |
| T-24 | Shutdown and track-deletion races: close/shutdown/delete during queued and running jobs; no leaks, no publications after removal | deterministic + stability scenario | P1E, P1H |

## 24. First-version acceptance criteria

Proxy v1 (P1 complete) is accepted when all of the following hold:

1. A project whose Primary plugins are all missing on the current machine loads, shows honest
   per-track statuses, and plays current proxies through each destination's normal
   inserts/fader/pan/bus/master path — with no added tracks or mixer channels (PI-001/PI-002).
2. Editing any note, CC, channel, routing, or plugin parameter of a destination or its routed
   sources marks that destination's proxy stale; nothing else does (T-06/T-07 matrix).
3. With Primary installed, Auto mode re-renders in the background without audio-thread impact,
   and an edit mid-render supersedes the job without a stale publication (T-10/T-11/T-12).
4. A failed render leaves the previous proxy playable and reports the failure (T-13).
5. Live Primary ↔ proxy switching produces no latency-policy timing jump (T-15).
6. Tails are captured to silence or explicitly marked truncated (T-16).
7. Save latency is unchanged and never waits on rendering (T-20).
8. v19 projects load unchanged; v20 projects with missing/corrupt proxy media load with degraded
   status (T-18/T-21).
9. Offline mixdown of a proxy-playing project is correct through the shared downstream path
   (T-22).
10. The user can always see which source they are hearing, and source changes are surfaced
    (PI-021/PI-022; T-19).

## 25. Decisions requiring human review (register view) and traceability

### 25.1 Open decisions (authoritative list)

Exactly the items in the top-of-document table: **PID-001** (state-capture mechanism — blocking,
SPIKE-01), **PID-005** (tail numeric values), **PID-008** (audition with missing Primary),
**PID-009** (Secondary persistence shape — P2, non-blocking), **PID-011** (portable flow —
deferred), **OI-001** (sample-rate portability playback behavior). All other register entries are
Locked or Recommended and need only confirmation, not new decisions.

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
| PI-011 | P0/P1A, P1D | T-09 |
| PI-012, PI-013 | P1E | T-10, T-11, T-24 |
| PI-014, PI-015 | P1D, P1G | T-15 |
| PI-016 | P1D | T-16 |
| PI-017 | P1D, P1E | T-09, T-10 |
| PI-018, PI-019 | P1C | T-05, T-06, T-07 |
| PI-020 | P0/P1A, P1C | T-01 |
| PI-021, PI-022 | P1G, P1I | T-19 |
| PI-023 | P1H | T-20 |
| PI-024, PI-025, PI-026 | P1F, P1H | T-13, T-14, T-18, T-23 |
| PI-027 | P1H | T-20 (undo/dirty assertions) |
| PI-028 | P1E, P1F | T-12 |
| PI-029 | all (scope discipline) | slice exclusion lists (§22) |

### 25.3 Quality-gate cross-check (performed for this draft)

* All eleven audit decisions appear in the register (§21 cross-check line). ✔
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
