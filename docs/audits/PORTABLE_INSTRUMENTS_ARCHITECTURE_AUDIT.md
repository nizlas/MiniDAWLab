# Portable Instruments and Proxy Rendering — Forensic Architecture Audit

**Date:** 2026-09-02
**Scope:** Documentation-only audit of the current repository state, performed before writing the
canonical technical steering document for the planned Primary / Secondary / Proxy instrument
feature. **No production code, schema, or test was modified for this audit.**

**Evidence discipline.** Every claim is marked:

* **[Verified]** — read directly in the repository (file, type, function cited; line numbers are
  approximate and prefixed `~` where the file may drift).
* **[Inference]** — a conclusion that follows from verified facts but was not itself observed as a
  single code location.
* **[Open]** — an open question that the steering document must resolve; the repository does not
  answer it today.

Planned types (Proxy renderer, fingerprint store, etc.) **do not exist** in the repository and are
never described below as if they did.

---

## 1. Executive forensic verdict

**[Verified]** DAL is substantially *closer* to proxy-readiness than a typical JUCE hobby DAW on the
**state/MIDI side**, and substantially *farther* on the **render-infrastructure side**:

Strong foundations that the proxy feature can stand on today:

1. **One clean audio boundary already exists.** The instrument's raw plugin output is produced in
   exactly one place (`ExperimentalInstrumentHost::audioThread_processBlockAndAddToOutputs`,
   `src/plugins/ExperimentalInstrumentHost.cpp` ~3471–3587) into a host-owned stereo scratch, and is
   consumed in exactly one place (`playback_mix_helpers::renderInstrumentPostStripToStereoScratch`,
   `src/engine/PlaybackMixHelpers.cpp` ~651–703) where Pre-inserts → fader → Post-inserts → pan →
   bus routing are applied *afterwards*. This is precisely the requested "after instrument output,
   before DAL fader/pan/inserts/routing" boundary (§4).
2. **Immutable-snapshot discipline everywhere.** Session state, MIDI render plans, routing topology,
   instrument registry, and insert chains are all published to the audio thread as immutable
   `std::shared_ptr` snapshots via atomic release/acquire (§3). A proxy audio buffer/publication
   would be the sixth instance of an established pattern, not a new invention.
3. **The destination event stream is already reproducible.** Per-source MIDI/CC is pre-baked into
   `InstrumentTrackRenderSnapshot` (notes with effective channel applied, bounded CC streams, chase
   semantics) and merged deterministically (destination first, then `TrackKind::Midi` sources in
   session order). A deterministic capture seam (`MidiDeliveryCaptureSink`) already exists at the
   exact plugin-delivery boundary and is used by stability tests (§5).
4. **Stable asset key.** `TrackId` is a persisted, monotonically allocated `std::uint64_t` that
   survives save/load and is never regenerated (§8) — suitable for keying proxy assets.
5. **Atomic file publication patterns exist** (temp sibling + move for project save; delete + move
   for mixdown), plus a skip-on-missing media policy that never blocks project load (§8).

Missing pieces that must be *built*, because nothing reusable exists:

1. **No background render infrastructure.** The only offline audio path (mixdown) runs *blocking on
   the message thread*, silences the entire realtime engine through a gate, and — critically —
   **re-uses the live `AudioPluginInstance`s** (§6). It is a correct exclusive bounce, and an
   explicitly unsafe template for a concurrent proxy renderer.
2. **No plugin-clone-with-state path in production.** The closest precedent is a temporary
   diagnostic second instance with copied live state (drum-name probe, §7.5) — a proof of concept,
   not a lifecycle.
3. **No staleness signal.** Nothing in the repository can answer "Organ's rendered output is now
   stale because Organ Lower changed" (§5.6). Fingerprinting must be designed from scratch; the
   whole-project dirty flag is explicitly unsuitable and per-controller `revision` counters are
   runtime-only.
4. **No plugin latency compensation, no tail rendering, no cancellation** in any existing offline
   path (§6), and **plugin version is not persisted** in the project descriptor (§10, §11).
5. **No portable-project/collect-media feature, no media GC, no Save As media copy** (§8).

**[Inference]** Net risk profile: the *engine integration* of proxy playback is low-risk (one
substitution point, everything downstream untouched); the *render job + fingerprint* side is where
all the genuinely new architecture lives.

---

## 2. Current architecture map

**[Verified]** Moving parts relevant to the feature, with owners:

```
Message thread                                    Audio thread (device callback)
──────────────                                    ──────────────────────────────
Session ──publishes──> SessionSnapshot ─────────> tracks/clips/mute/fader/pan/MIDI-To (acquire)
  │  (immutable; Track by value; COW with*() )
  │
InstrumentRuntimeCoordinator
  │  owns unordered_map<TrackId, unique_ptr<ExperimentalInstrumentHost>>
  │  owns unordered_map<TrackId, unique_ptr<InstrumentTrackController>>
  │
  ├─ ExperimentalInstrumentHost (one per instrument lane)
  │    atomic<shared_ptr<InstrumentOwner>> activeOwner_
  │      └─ InstrumentOwner { unique_ptr<juce::AudioPluginInstance>, layoutOk }
  │    scratch_ (stereo render buffer)   midiIo_->uiPendingMidi (UI FIFO, CriticalSection)
  │    rtBlockMidi_ (transport MIDI, audio-thread only)
  │    midiCaptureSink_ (deterministic test capture at delivery boundary)
  │
  ├─ InstrumentTrackController (one per Instrument OR Midi lane; hostOrNull for Midi lanes)
  │    clips_: vector<InstrumentMidiClip{ pattern, startSamples, anchor, length }>
  │    publishRenderSnapshot() ──> atomic<shared_ptr<InstrumentTrackRenderSnapshot>>
  │                                  { revision, playbackEnabled, clips[notes], ccStreams }
  │
  ├─ PluginInsertHost — per-track VST3 insert chains (Pre/Post), atomic PluginAudioThreadMap
  │
  └─ PlaybackEngine (juce::AudioIODeviceCallback)
       publishExperimentalInstrumentPlaybackSnapshot()
         ──> atomic<shared_ptr<ExperimentalInstrumentPlaybackSnapshot>>
               { entries[{TrackId, host*, controller*}], midiSources[{TrackId, controller*}] }
       rebuildRoutingPlanFromSession() ──> atomic<shared_ptr<RoutingPlan>>
               { sourceSteps, busSteps, masterBusIndex, bus scratch (co-owned) }
       renderOfflineMixdownBlock()  [message thread, offline-gate protected]

Persistence (src/io/ProjectFile.*): single JSON .dalproj, schema v19
  tracks[] (id, kind, fader, pan, mute/off, routedOutputTrackId, sends, midiChannel, midiTo, inserts[])
  experimentalInstrumentTracks[] (trackId, descriptor, pluginStateBase64, clips[notes, ccPoints])
Media: <ProjectFolder>/Audio/ relative-only; skip-on-missing at load
```

Key files: `src/domain/Track.h`, `src/domain/Session.h/.cpp`, `src/domain/SessionSnapshot.h/.cpp`,
`src/engine/PlaybackEngine.h/.cpp`, `src/engine/PlaybackMixHelpers.h/.cpp`,
`src/engine/RoutingPlan.h`, `src/engine/RoutingPlanBuilder.cpp`,
`src/plugins/ExperimentalInstrumentHost.h/.cpp`, `src/plugins/PluginInsertHost.h/.cpp`,
`src/instruments/InstrumentTrackController.h/.cpp`, `src/app/InstrumentRuntimeCoordinator.h/.cpp`,
`src/playback/ExperimentalInstrumentPlaybackBridge.h/.cpp`, `src/io/ProjectFile.h/.cpp`,
`src/app/ProjectIoCoordinator.h/.cpp`, `src/app/AudioMixdownExporter.h/.cpp`,
`src/io/InstrumentMidiClipExport.h/.cpp`.

---

## 3. Verified call chains and ownership

### 3.1 Realtime chain, one instrument destination (VB3-II example)

**[Verified]** Per device block, `PlaybackEngine::audioDeviceIOCallbackWithContext`
(`src/engine/PlaybackEngine.cpp` ~504) runs phases documented by the `AudioCallbackPhase` enum
(`PlaybackEngine.h` ~191–209): Begin → RecorderPush → TransportBeginBlock → (OfflineGateSilence) →
LoadSnapshot → InstrumentBeginBlock → MixPrep → CountIn → ClipRender → **TransportMidiSchedule** →
**InstrumentMix** → FinalizeRouting.

**TransportMidiSchedule** (~1359–1461):

1. For each `TrackKind::Instrument` row in session order: its own
   `InstrumentTrackController::audioThread_scheduleTransportMidiForSegment(*ownHost, …)`.
2. Then for each `instrumentSnap->midiSources` entry (session order = deterministic merge order):
   resolve `Track::getMidiDestinationTrackId()` **from the current SessionSnapshot each block**
   (~1416–1422; a deleted/illegal destination silently fails to resolve), then schedule into the
   *destination's* host. A reroute first flushes that source's pending note-offs into the old
   destination — never a broad `allNotesOff` (~1430–1447).

**Event generation** (`src/instruments/InstrumentTrackController.cpp`
`audioThread_scheduleTransportMidiForSegment` ~2571–2721, `audioThread_scheduleCcForSegment`
~2723–2814):

* Reads the acquire-loaded `InstrumentTrackRenderSnapshot`; a `revision` bump forces CC re-chase.
* Ordering contract implemented by emission order (comment ~2658–2664): **pending Note Offs →
  CC (chase + in-segment) → Note Ons**; `juce::MidiBuffer` preserves insertion order at equal
  offsets, so CC at a note's start precedes its Note On.
* CC chase on discontinuity (`forceDiscontinuity || revisionBump || segment gap`): latest event
  strictly before segment start; no invented default before the first point; unchanged values are
  deduplicated per stream (`rtCcLastSentValue_`).
* Note offs: explicit `noteOffAbsSample`, else `gateSamples` fallback (100 ms at the snapshot's
  sample rate, ~2293).

**Delivery + processing** (`ExperimentalInstrumentHost::audioThread_processBlockAndAddToOutputs`,
`src/plugins/ExperimentalInstrumentHost.cpp` ~3471–3587): drains `uiPendingMidi` (UI/audition MIDI,
short `CriticalSection`) first, appends `rtBlockMidi_` (transport MIDI), optionally hands the merged
buffer to `MidiDeliveryCaptureSink` (test seam), then `inst.processBlock(view, blockMidi)` into the
host-owned `scratch_`, and finally adds the first stereo bus to the target with `gain × panLaw`.

**Mixing** (`PlaybackEngine.cpp` `mixKeyedInstrumentLanesIntoOutputsIfAny` ~984–1167 →
`playback_mix_helpers::renderInstrumentPostStripToStereoScratch` ~651–703): skip if
`isTrackOff() || isMuted() || fader <= 0`; with active inserts the instrument renders at
**unity gain / center pan** into the insert scratch, then `InsertStage::Pre` → fader →
`InsertStage::Post` → pan → post-strip stage → `fanPostStripStageToDryAndSends` into the
`RoutingPlan` bus scratch; Group/Master bus strips run in `finalizeRoutingToDevice` (~844–978).

### 3.2 Ownership table

**[Verified]**

| Object | Owner | Lifetime notes |
|---|---|---|
| `juce::AudioPluginInstance` (instrument) | `ExperimentalInstrumentHost::activeOwner_` (`atomic<shared_ptr<InstrumentOwner>>`, `.h` ~250–254) | Swapped atomically on load/unload; audio thread retains via acquire |
| `ExperimentalInstrumentHost`, `InstrumentTrackController` | `InstrumentRuntimeCoordinator` maps keyed by `TrackId` (`src/app/InstrumentRuntimeCoordinator.cpp` ~264–299) | Publish-before-destroy + `waitForAudioCallbackExit` drain on removal (~345–369) |
| Insert `AudioPluginInstance`s | `PluginInsertHost::LiveInsertSlot` | Message-thread mutation; atomic `PluginAudioThreadMap` for RT |
| `SessionSnapshot` | `Session` (atomic shared_ptr) | `Track` held **by value**; copy-on-write `with*()` helpers |
| Proxy-relevant scratch | `ExperimentalInstrumentHost::scratch_` (per host), `PluginInsertHost::scratch_` (shared, sequential), engine bus pool (grow-only, plan co-owns slots) | Stability C4B pattern: never freed while a plan may reference them |

### 3.3 UI-to-engine publication mechanisms

**[Verified]** All five realtime views use the identical pattern — immutable object, atomic
`shared_ptr` release-store on the message thread, acquire-load + retain in the callback:
`SessionSnapshot`, `ExperimentalInstrumentPlaybackSnapshot`
(`PlaybackEngine::publishExperimentalInstrumentPlaybackSnapshot` ~345), per-controller
`InstrumentTrackRenderSnapshot` (`publishRenderSnapshot` ~2511), `RoutingPlan`
(`rebuildRoutingPlanFromSession` ~2177), `PluginAudioThreadMap` (`PluginInsertHost` ~937–965).
**[Inference]** A published "current proxy audio + mode" per instrument track should follow this
exact pattern; no new synchronization concept is needed.

---

## 4. Candidate proxy process boundary

### 4.1 The boundary, verified in code

**[Verified]** The requested boundary — *after the instrument plugin's own output, before DAL track
fader, pan, inserts and downstream routing* — exists as an identifiable buffer in exactly one
production code path:

```651:703:src/engine/PlaybackMixHelpers.cpp
void renderInstrumentPostStripToStereoScratch(ExperimentalInstrumentHost* host,
                                              const Track& track, ...)
{
    ...
    if (useInsert && effectiveGain > 0.0f)
    {
        pluginHost->audioThread_clearScratch(PluginInsertHost::kInsertChannels, numSamples);
        if (float* const* scratch = pluginHost->audioThread_getScratchWritePointers())
        {
            mixExperimentalInstrumentAfterTracks(
                host, scratch, 2, numSamples, 1.0f, kTrackStereoPanCenter);   // <── RAW INSTRUMENT
            pluginHost->audioThread_processChainForTrack(trackId, InsertStage::Pre, numSamples);
            scaleStereoScratch(scratch, numSamples, effectiveGain);           //     fader
            pluginHost->audioThread_processChainForTrack(trackId, InsertStage::Post, numSamples);
            multiplyStereoScratchLR(...pan law...);                           //     pan
            addStereoScratchToStereoScratch(stageL, stageR, ...);
        }
        return;
    }
    float* const stagePtrs[2] = { stageL, stageR };
    mixExperimentalInstrumentAfterTracks(host, stagePtrs, 2, numSamples,
                                         effectiveGain, track.getStereoPan()); // <── gain/pan folded
}
```

`mixExperimentalInstrumentAfterTracks` (`PlaybackMixHelpers.cpp` ~234–247) is a thin forward to
`host->audioThread_processBlockAndAddToOutputs(...)`, whose internal `scratch_` after
`inst.processBlock(view, blockMidi)` (`ExperimentalInstrumentHost.cpp` ~3553–3558) **is the
pre-fader / pre-insert / pre-pan signal** in both branches — gain and pan are applied only at the
final add (`addFirstStereoBusToDeviceOutputs` with `g × pL / g × pR`, ~3583–3586).

### 4.2 Where proxy playback would substitute

**[Verified — seam location; Inference — the substitution itself]** Two candidate integration
levels, both leaving every downstream stage (Pre/fader/Post/pan/buses/master) untouched:

* **Host-level (narrowest):** inside `audioThread_processBlockAndAddToOutputs`, replace the
  "clear scratch + `inst.processBlock`" region with "fill `scratch_` from prerendered proxy audio at
  the current timeline position". Everything else in the function (MIDI drain/consumption, gain/pan
  add, diagnostics) is unchanged. Advantage: audition/UI MIDI handling and the capture sink remain
  at one boundary; the offline mixdown path (`renderOfflineMixdownBlock` reuses the same helper —
  §6) inherits the substitution automatically.
* **Helper-level:** in `renderInstrumentPostStripToStereoScratch`, branch before
  `mixExperimentalInstrumentAfterTracks` and read proxy samples into the same scratch. Advantage:
  keeps `ExperimentalInstrumentHost` plugin-only; disadvantage: the no-insert branch folds
  gain/pan into the host call, so the substitution must replicate that fold, and the mixdown path
  must be updated in step.

**[Verified]** Timeline position for a proxy read is available in the callback: the MIDI scheduling
path already receives `timelineSegStart`/`audibleRun` per segment, but the *mix* call currently does
not receive timeline coordinates (`renderInstrumentPostStripToStereoScratch` takes only
`numSamples`). **[Open]** Threading timeline position into the instrument-mix call (or the host) is
the one signature-level change the seam requires; the steering document must place it.

**[Verified]** Audio-file streaming precedent: none. Audio clips are fully decoded into memory
(`AudioClip`, `src/domain/AudioClip.h` ~30–50) and read with `getReadPointer` on the audio thread —
there is no streaming reader (`BufferingAudioSource` unused). **[Open]** Proxy files (potentially
long) need a policy: preload like `AudioClip` vs a new realtime-safe streaming reader.

---

## 5. MIDI dependency-graph findings

### 5.1 Model

**[Verified]** Strictly one hop: `TrackKind::Midi` row → `Track::midiDestinationTrackId_`
(`TrackId`, persisted as JSON `midiTo`, v18+) → `TrackKind::Instrument` row. Enforced at every
mutation point: `Session::setTrackMidiDestination` (`Session.cpp` ~487–524),
`SessionSnapshot::withTrackMidiDestination` (~1788–1833, rejects non-Midi source and non-Instrument
destination), load/structural repair `session_routing::repairMidiDestinationsInPlace`
(`SessionRouting.cpp` ~314–338: non-existent / non-Instrument / self → `kInvalidTrackId`).
**Cycles are structurally impossible** for MIDI routing (Midi→Midi and Instrument→Instrument edges
cannot be stored). Chains do not exist.

### 5.2 Enumerating sources for a destination

**[Verified]** There is **no centralized `sourcesForDestination(TrackId)`**. The reverse lookup is
performed as a forward scan (all Midi rows / all `midiSources`, filter by resolved destination) in
at least: `PlaybackEngine.cpp` realtime schedule (~1410–1461), offline mixdown schedule
(~1888–1920), stop-flush (~648–669), `InstrumentRuntimeCoordinator.cpp` snapshot build (~842–864,
publishes *all* Midi controllers without destination filter), `SessionSnapshot::withTrackRemoved`
destination clearing (~749–754), `MidiEditorPresenter.cpp` editor-close-on-delete (~542–554), and
destination-status UI. **[Inference]** Proxy dependency collection would be the next duplicate;
centralizing the enumeration is a prerequisite-quality seam but is a design decision for P1, not
this audit.

### 5.3 Eligibility semantics

**[Verified]** Per-source gating is baked at publish time, not read live per block:
`playbackEnabled = trackActive_ && powerOn_ && !muted_ && (host_ == nullptr ||
host_->acceptsTransportMidi())` (`InstrumentTrackController.cpp` ~2299–2300);
`audioThread_scheduleTransportMidiForSegment` early-returns (after pending offs) when false. Track
solo **does not exist** in the domain/audio path. Record-arm is audio-only. Destination mute/off is
applied at the *audio mix* stage (skip processBlock), not at MIDI scheduling.

### 5.4 Tick→sample and tempo

**[Verified]** Notes/CC are stored in ticks (`TimelineMidiNote::startTick/durationTicks`,
`MidiCcPoint::startTick`; TPQ per clip, default 960) and baked to absolute timeline samples at
publish time using **the clip pattern's `bpm`** and the controller's `timelineSampleRate_`
(`ExperimentalMidiPattern.h` `ticksToRelativeSamples`/`absoluteSampleForTimelineNote` ~101–188;
fallbacks 120 bpm / 48 kHz). Project-level `ProjectMusicalTime` bpm is kept in sync with clip bpm on
load/edit. **[Inference]** For fingerprinting, tick data + bpm + TPQ + clip anchors are the musical
source of truth; baked absolute samples are derived and sample-rate-dependent.

### 5.5 The VB3-II example against the actual implementation

**[Verified]** The stability fixture builds exactly this shape (`MainAppWindow.cpp` ~1607–1664):
Instrument "Organ" with its own clips, Midi rows "Organ Lower" (fixed output channel 2) and
"Organ Pedal" (channel 3), both `midiTo` → Organ. Per block, Organ's controller schedules Upper
notes into Organ's host; then Lower and Pedal (session order) resolve their destination and schedule
into the *same* host; the merged buffer (offs → CC incl. CC11 → ons; sources after destination) is
consumed by one `inst.processBlock`. This matches the product-context requirement that all notes and
CC render together through the one destination instance.

### 5.6 Staleness: what exists, what does not

**[Verified — exists]** Per-source `InstrumentTrackRenderSnapshot::revision` (uint32, bumped on every
republish; runtime-only, resets each run), `juce::ChangeBroadcaster` notifications from controllers,
and the coarse project dirty state (`ProjectIoCoordinator`: clean-snapshot pointer compare **or**
`instrumentOrPluginEditsSinceClean_`, `ProjectIoCoordinator.cpp` ~1107–1127).

**[Verified — does not exist]** Any cross-track mechanism answering "destination X's rendered output
is stale because source Y changed": no dependency version counter on the destination, no
content hash, no render-cache invalidation. **[Inference]** The fingerprint design (§10) must build
this from content, not from existing flags; the per-source revision counters are usable only as a
*runtime* cheap-change hint, never as a persisted identity.

---

## 6. Offline-rendering reuse assessment

**[Verified]** Complete inventory of offline paths: stereo audio mixdown WAV
(`mini_daw_audio_mixdown::exportStereoMixdownWavBlocking`, `src/app/AudioMixdownExporter.cpp`
~181–395), mixdown MP3 (same + external LAME child process, ~420–623), single-clip MIDI export
(`src/io/InstrumentMidiClipExport.cpp`). **No** track freeze, bounce-in-place, stem export, or
offline preview exists.

### 6.1 Mixdown anatomy (the only offline audio renderer)

**[Verified]**

| Property | Behavior |
|---|---|
| Clock | Own sample loop (`while (pos < loopSpan.lengthSamples)` ~344–379), independent of device clock; block size `min(4096, device buffer)`; engine cap `kOfflineMixdownBlockCapSamples = 4096` |
| Plugin instances | **Live** — same `ExperimentalInstrumentHost*` / `PluginInsertHost` processors as realtime playback; no cloning anywhere |
| Engine state | Stops transport if playing (no resume); `ScopedOfflineRenderGate` (`beginOfflineRenderGate`, depth-counted) silences the device callback for the entire render; drains in-flight callback ≤250 ms before starting |
| Thread | **Message thread, blocking**; progress window repaints synchronously without pumping the message loop |
| Snapshots | `SessionSnapshot` + instrument playback snapshot loaded **once** at start; hosts remain live objects |
| Determinism aids | `renderOfflineMixdownBlock(..., instrumentForceDiscontinuity)` re-chases CC on the first block; per-block schedule reuses the exact realtime merge (`PlaybackEngine.cpp` ~1879–1920) |
| Cancellation | None mid-render |
| Tails | None — renders exactly `[L, R)` locator span |
| Latency | Device playback offset only (`LatencySettingsStore`); **no plugin-delay compensation** (`getLatencySamples` is only logged, `PluginInsertHost.cpp` ~261) |
| Publication | Unique temp sibling (`name.__dal_wav_render_<hex>.wav`) → delete destination → `moveFileTo` (`replaceDestinationWithRenderedTemp` ~69–103); failure keeps old file |
| Sample rate | Current device rate only |

### 6.2 Reuse verdict for a background proxy renderer

**[Verified basis, Inference verdicts]**

Reusable as-is or as pattern: `SessionSnapshot`/render-snapshot capture discipline; the offline
block's *algorithm shape* (segment loop → schedule → process → write) as a reference; the
`instrumentForceDiscontinuity` chase contract; temp-sibling + atomic replace publication;
`MixdownWavProbe`/WAV writer plumbing; the shared CC evaluator
(`midi_cc::collectCcEventsInTickRange`, single evaluator for realtime bake and MIDI export); loop
span validation.

Unsafe to reuse for a *concurrent* proxy render: the live host/insert instances (single
`processBlock` consumer assumption; internal DSP state would be corrupted); the offline gate
(silences the whole app — the opposite of "keep playing"); the message-thread blocking model; the
shared engine scratch pools; transport stopping. **The audit confirms the product-context warning:
the existing mixdown path is *not* suitable for background proxy rendering merely because it is
offline.**

**[Open]** Faster-than-realtime rendering with VST3: no `canDoOfflineProcessing`/non-realtime flag
is read anywhere in the repo today; controlled realtime fallback for incompatible plugins has no
existing hook and must be designed.

---

## 7. Plugin lifecycle and threading constraints

### 7.1 Selection, descriptor, state

**[Verified]** VST3 only (`formatManager_.addFormat(new juce::VST3PluginFormat())`;
`ExperimentalInstrumentHost.cpp` ~1642 and `PluginInsertHost.cpp` ~66; no `KnownPluginList`).
Descriptors: live `lastLoadedPluginDescription_` on the host; persisted
`ProjectFileGenericVst3DescriptorV1` (name, manufacturer, format, category, `fileOrIdentifier`,
`uniqueId`, `deprecatedUid`, `isInstrument` — **no version field**) plus advisory
`pluginBundlePath`, on `ProjectFileExperimentalInstrumentTrackV1` (`ProjectFile.h` ~152–193).
State: Base64 `getStateInformation` blob captured **at project save**
(`getCurrentInstrumentStateBase64`, host ~2055–2084) and restored **before** the instance is
published during load autoload (~2223–2308). Instrument-editor close does **not** capture state
(insert-FX editor close does, with undo diff — `PluginInsertHost::editorWindowClosing` ~1112–1142).

### 7.2 Instance ownership and multi-instance status

**[Verified]** One instance per host, one host per instrument track; multiple tracks may host the
same plugin identity (each its own instance; the sibling-clone helpers create fresh instances
**without** state copy). Production never clones a live instance with copied state for rendering.

### 7.3 Threading contract (as enforced by DAL guards)

**[Verified]**

* Message thread only: `createPluginInstance` (synchronous; guards assert message thread, host
  ~1906–1909/~2093–2096), `prepareToPlay`/`releaseResources` wrappers, bus layout, editor
  open/close, `get/setStateInformation`, load/unload, insert-chain mutation.
* Audio thread only: `processBlock` (via `audioThread_processBlockAndAddToOutputs`), transport MIDI
  scheduling, insert `audioThread_processChainForTrack`, snapshot acquire-loads.
* Cross-thread: UI MIDI FIFO under a short `CriticalSection`.

**[Inference — JUCE capability not exercised by the repo]** JUCE permits calling `processBlock` on a
non-device thread when the caller guarantees exclusive, prepared access (the mixdown path already
proves this on the message thread). A proxy worker could therefore drive `processBlock` on a worker
thread **for its own dedicated instance**, provided construction/state-restore/prepare happen on the
message thread and the instance is never shared with the live engine. This is the safest available
division of labor; the repository contains no counterexample.

### 7.4 Existing thread/job infrastructure

**[Verified]** Inventory: one `juce::ThreadPool` (single thread, waveform pyramids,
`AudioWaveformCache.cpp` ~62, shutdown via `removeAllJobs(true, 60000)`); detached `std::thread`s
for out-of-process VST3 scans / catalog rebuilds (results marshalled back with
`MessageManager::callAsync` + `AsyncCallbackGuard`); a joinable recorder WAV-writer thread; an
optional compile-gated UI-hang watchdog; the OOP scan **child process** (`Vst3ChildProcessScan`,
launched via a worker mode in `Main.cpp` ~106–113). Generation/invalidation precedents:
`Session::loadGeneration_` (stale-guards deferred plugin restores, `Session.cpp` ~1822/~2198),
`AsyncLifetimeOwnerToken`, render-snapshot `revision`.

**[Verified — does not exist]** A general job queue, cancellation tokens as a reusable type, worker
prioritization, or any background *audio* processing.

**[Inference]** The safest integration point for a low-priority proxy render queue is a new,
single-concurrency worker following the recorder/scan patterns (owned by a message-thread
coordinator, `AsyncCallbackGuard`-protected completion, `Session::loadGeneration_`-style stale
checks), *not* an extension of the waveform pool (UI-latency coupled) and *not* the mixdown gate
path.

### 7.5 The closest existing precedent for a cloned render instance

**[Verified]** The Phase C drum-name probe creates a **temporary second `AudioPluginInstance`** of
the loaded plugin and copies the **live** instance's current `getStateInformation` into it before
probing, then destroys it (`ExperimentalInstrumentHost.cpp` ~3133–3163). This is the only
state-copied clone in the repository — a working proof that "separate instance + copied state" is
achievable with the existing host code, though its lifecycle (synchronous, message thread,
diagnostic-scoped) is not a render pipeline.

---

## 8. Media / persistence findings

### 8.1 Identity and schema

**[Verified]** `TrackId` = `std::uint64_t`, `kInvalidTrackId = 0`, monotonic `nextTrackId` persisted
in the project root and re-seeded on load with `max(parsed.nextTrackId, maxSeen + 1)`
(`Session.cpp` ~2073–2078). Ids are never regenerated; **load does not reject duplicate track ids**
(clip ids are checked, track ids are not — `ProjectFile.cpp` ~1813–1838). Schema:
`ProjectFileV1::kCurrentVersion = 19` (`ProjectFile.h` ~246–252); writer refuses non-current;
reader accepts 1…19 with structural migrations (`migrateLegacySinglePluginToInserts`,
`migrateProjectFileMasterTrackPreV14`, `migrateProjectFileExperimentalInstrumentLanePreV13`) plus
additive absent-key defaults for v17 `midiChannel` / v18 `midiTo` / v19 `ccPoints`. The additive
pattern is the established seam a future proxy-metadata field would use (with a bump to v20).

### 8.2 Instrument/MIDI persistence relevant to fingerprinting

**[Verified]** Per instrument/Midi lane under `experimentalInstrumentTracks[]`:
descriptor + `pluginStateBase64` + `clips[]`, each clip carrying `bpm`, `ticksPerQuarter`,
`startSamples`, `lengthSamples`, `timelineAnchorSamples`, notes
(`midiNote/velocity/offVelocity/channel/startTick/durationTicks`) and `ccPoints`
(`startTick/controller/value/channel/interp`), plus persisted-but-view-only roll viewport fields.
Musical undo deliberately strips plugin blobs
(`stripExperimentalInstrumentTrackPluginFieldsForUndo`, `ProjectFile.cpp` ~1844+).

### 8.3 Media layout and atomicity

**[Verified]** Single JSON `.dalproj`; media strictly project-relative under
`<ProjectFolder>/Audio/` with forward slashes; absolute/`..` paths are rejected and Save **fails
hard** if a clip's source is outside the target `Audio/` folder. Missing/corrupt media at load →
clip skipped with a collected diagnostic, project still loads (no placeholder object). Atomic
publication: project text via temp sibling + `moveFileTo` (`writeProjectTextAtomically`,
`ProjectFile.cpp` ~70–109); mixdown via delete-destination + `moveFileTo` with a Windows
"file may be open in another program" error path (`AudioMixdownExporter.cpp` ~69–103). Autosave
writes `<stem>_autosave.dalproj` through the same atomic writer on a message-thread timer.
**[Verified — does not exist]** Portable-project/collect/consolidate feature, media GC ("Clean
Unused Media" is only a future mention in help text), Save As media copy.

### 8.4 Proxy asset layout evaluation (no schema selected)

**[Inference from verified constraints]** A TrackId + render-fingerprint-hash layout fits the
repository's conventions: TrackId is stable and persisted; the project folder already defines a
media sibling (`Audio/`); temp-then-rename publication exists in two variants. Constraints the
steering document must respect: (a) the `Audio/` folder currently has *user-clip* semantics baked
into save validation — a **separate sibling folder** (e.g. a proxies directory) avoids entangling
proxy assets with the clip-source rules; (b) Windows replace requires the delete+move variant when a
reader may hold the old file — the engine must therefore never stream a proxy file that publication
may replace in place (publish-new-name + atomic pointer swap in the session, retire old, matches the
C4B scratch-retirement discipline); (c) missing proxy at load must degrade like missing media
(diagnostic + silent placeholder), never block load. The repository does not make one naming scheme
clearly preferable; final layout remains **[Open]**.

---

## 9. Playback-source-selection seam

**[Verified anchor points; Inference on the mechanism]** The narrowest seam where an instrument
track could select between *Primary live / current Proxy / Secondary live / missing-silent* is the
per-track instrument mix decision — concretely, either inside
`ExperimentalInstrumentHost::audioThread_processBlockAndAddToOutputs` (per-host atomic "source
mode" + proxy buffer pointer, following the `activeOwner_` swap pattern) or at the
`renderInstrumentPostStripToStereoScratch` call level. Everything downstream (inserts, fader, pan,
sends, buses, master) is source-agnostic today, which is exactly what the product context requires.

Interaction audit at that seam:

| Concern | Current behavior (Verified) | Interaction (Inference / Open) |
|---|---|---|
| Mute / off | Checked before instrument mix (`PlaybackEngine.cpp` ~1040–1056); also gates the destination's *own* MIDI at publish (`playbackEnabled`) | Proxy playback naturally inherits the same skip; no change |
| Solo | Does not exist | N/A |
| Audition / editor MIDI | UI FIFO consumed at the host boundary; requires a live instrument to make sound | **[Open]** With Primary missing: audition via Secondary, or silent with status; proxy cannot audition arbitrary notes |
| Transport playback | Substitution at the seam is transparent | Needs timeline position at the mix call (§4.2) |
| Offline mixdown | `renderOfflineMixdownBlock` reuses the same helpers | Host-level substitution propagates automatically; helper-level requires parallel edit |
| Plugin editor windows | Owned by host; missing plugin → no editor | Proxy mode must not offer an editor for Primary |
| Latency | No PDC anywhere | **[Open]** A proxy plays at 0 plugin latency; a latent Primary is *not* compensated today, so switching sources can shift audible timing — policy needed |
| Meters | No track meters exist | N/A |
| Missing plugin today | Track name suffix `(plugin not loaded)` (`ProjectIoCoordinator.cpp` ~147–169), silent lane, MIDI preserved | The Proxy role replaces "silent" as the authoritative sound |

**UI extension seams for future status / "Instrument alternatives" controls [Verified locations
only]:** the Inspector already hosts per-track routing controls with live rebuild
(`InspectorView.cpp` MIDI To combo ~1714–1746); track-header rows are built by
`InstrumentTimelineRowCoordinator`; runtime (non-persisted) per-track status has an established home
in the coordinator/controller layer (`InstrumentRuntimeCoordinator` maps, controller
`ChangeBroadcaster`). No UI design is proposed here.

---

## 10. Render-fingerprint field classification

Basis: verified data structures only (`Track`, `InstrumentMidiClip`, `ExperimentalMidiPattern`,
`TimelineMidiNote`, `MidiCcPoint`, `ProjectFileExperimentalInstrumentTrackV1`,
`ProjectFileGenericVst3DescriptorV1`, `PluginTrackChain`, `InstrumentTrackRenderSnapshot`,
`ProjectMusicalTime`, engine constants). The whole-project dirty flag is **not** part of any class
below (explicitly unsuitable — it conflates unrelated edits; `ProjectIoCoordinator` ~1107–1127).

### 10.1 Must be fingerprinted (affects the pre-fader/pre-insert Primary output)

**[Verified fields; Inference that the set is complete for the stated boundary]**

| # | Field(s) | Source of truth |
|---|---|---|
| F1 | Primary plugin identity: `fileOrIdentifier`, `uniqueId`, `deprecatedUid`, format, `isInstrument` | `ProjectFileGenericVst3DescriptorV1` / live `lastLoadedPluginDescription_` |
| F2 | Primary plugin **state blob** (current, not last-saved — see H2) | `getStateInformation` via `getCurrentInstrumentStateBase64` |
| F3 | Destination's own MIDI clips: per clip `startSamples`, `timelineAnchorSamples`, `lengthSamples`, pattern `bpm`, `ticksPerQuarter` | `InstrumentMidiClip` |
| F4 | Every note: `midiNote`, `velocity`, `offVelocity`, `channel`, `startTick`, `durationTicks` | `TimelineMidiNote` |
| F5 | Every CC point: `startTick`, `controller`, `value`, `channel`, `interpolationToNext` | `MidiCcPoint` |
| F6 | Destination `midiOutputChannel` (effective-channel remap baked into events) | `Track` |
| F7 | The routed-source **set**: every `TrackKind::Midi` row with `midiDestinationTrackId == dest`, including each source's F3–F5 content and its own `midiOutputChannel` | `Track` + source clips |
| F8 | Per-source eligibility that gates MIDI: source `trackOff` (power), source `muted` (`playbackEnabled` inputs) | `Track` / controller |
| F9 | Merge order: session track order of the contributing sources (same-offset last-wins for CC) | `SessionSnapshot` row order |
| F10 | Note-off gate rule inputs: the 100 ms `gateSamples` derivation and render sample rate (notes with `noteOffAbsSample == 0`) | `publishRenderSnapshot` ~2293 |
| F11 | Render sample rate and fixed render block size chosen for the proxy (block-size-sensitive plugins) | render policy (new) |
| F12 | Tail policy value and latency-trim policy value, once defined | render policy (new) |

### 10.2 Excluded — after the process boundary

**[Verified as post-boundary in §4]** `channelFaderGain_`, `stereoPan_`, insert chains
(`PluginTrackChain` slots incl. `opaqueState`), `sends_`, `routedOutputTrackId_` (audio bus),
Group/Master strips and their inserts, master fader, device playback/recording offsets
(`LatencySettingsStore`). Also destination-side *audio* gates evaluated at playback time
(mix-stage skip on destination mute/off) — but see D6 in §12 for the destination-mute nuance on the
MIDI side.

### 10.3 Runtime-only and irrelevant

**[Verified as runtime/view state]** UI/audition MIDI (`uiPendingMidi`), preview players, selection,
undo stacks (`SessionHistory`), dirty flags, editor windows, roll viewport fields (persisted but
view-only: `midiRoll*`, lane fractions), `MidiEditorWorkspaceUiState`, CC-lane view state, track
names/`activeTrackId`, diagnostics (`rtDiag_*`, load window), autosave state,
`InstrumentTrackRenderSnapshot::revision` (process-lifetime counter — usable as a cheap runtime
change hint, never as persisted identity), capture sinks.

### 10.4 Unresolved pending a design decision

**[Open]**

| # | Question |
|---|---|
| U1 | Plugin **version** is not persisted (descriptor DTO has no version field). Fingerprint by live `PluginDescription::version`? Persist it at v20? Treat version change as automatic staleness? |
| U2 | State-capture cadence: `getStateInformation` is message-thread and potentially expensive; instrument-editor close does not capture. When is F2 sampled — save, editor close (new), render enqueue? |
| U3 | Destination mute/off: `playbackEnabled` gates the destination's *own* MIDI when muted, but the proxy should presumably render "as if audible". Render ignores mute (recommended for a sound-reference) or mirrors it? |
| U4 | Clip `bpm` vs project `ProjectMusicalTime` bpm: fingerprint per-clip bpm (source of truth for baking) and treat project-bpm sync edits as content changes — confirm. |
| U5 | Proxy render SR: device SR at render time (mixdown precedent) vs fixed canonical SR + resample on playback. Affects whether device switches invalidate proxies. |
| U6 | Loop/cycle state: mixdown renders the locator span; a proxy presumably renders the full arrangement extent of the destination + sources — define the span rule and include it in the fingerprint. |

---

## 11. Architectural hazards and hidden coupling

1. **[Verified]** **Live-instance sharing in the only offline path.** Mixdown mutates the same
   plugin instances as playback behind an all-silencing gate. Copying any part of that pattern into
   a background renderer corrupts live DSP state (§6).
2. **[Verified]** **Plugin state is only captured at explicit save; instrument-editor close does
   not capture.** A user tweaking VB3-II drawbars leaves no observable event in DAL. Fingerprints
   based on the last-saved blob will *silently miss* live tweaks; fingerprints based on fresh
   `getStateInformation` calls pay a message-thread cost per check (H/U2).
3. **[Verified]** **No plugin version in the persisted descriptor** (§10 U1) — "same identifier,
   different DSP" upgrades are invisible to a naive persisted fingerprint.
4. **[Verified]** **Reverse MIDI enumeration is duplicated** at ≥6 sites (§5.2); adding proxy
   dependency collection without centralizing invites drift between "what plays" and "what was
   fingerprinted".
5. **[Verified]** **Session order is semantic.** Source merge order (and therefore same-offset CC
   last-wins) depends on track-list order; a track *reorder* changes the audible stream and must
   invalidate proxies — easy to miss because no note/CC data changed (F9).
6. **[Verified]** **Baked sample domain.** Render snapshots bake ticks→samples at the live device
   rate; a proxy rendered at a different rate cannot reuse published events and must re-bake from
   ticks (§5.4, U5).
7. **[Verified]** **No PDC anywhere.** Introducing latency-compensated proxies would make proxy
   playback *differ* from live Primary playback timing unless the policy is explicit (§9).
8. **[Verified]** **`rtBlockMidi_`/host scratch single-consumer assumptions** — a second reader
   (render worker touching a live host) violates the documented threading contract.
9. **[Verified]** **TrackId duplicate ids are not rejected at load** — a corrupted project could
   alias proxy assets keyed by TrackId (§8.1).
10. **[Verified]** **Windows file replacement** requires the delete+move variant when a destination
    exists and readers may hold it; the engine must never hold an open handle to a proxy file being
    replaced (publish-new-name + retire pattern instead, §8.4).
11. **[Verified]** **Missing-media policy is skip-at-load with a note** — proxies need the stronger
    "keep the row, silent placeholder + status" behavior that instrument lanes already exhibit for
    missing plugins.
12. **[Inference]** **Message-thread cost concentration.** Plugin instantiation, state restore and
    `prepareToPlay` are message-thread-only; a proxy queue that (re)creates instances frequently can
    produce UI hitches — batching/reuse policy belongs in the design.

---

## 12. Open decisions that must be resolved before P1

D1. **Fingerprint definition** — exact field list from §10.1, hash construction, where it is stored
    (sidecar vs project v20 field), and the U1/U2 state-capture policy.
D2. **Staleness signalling** — content-hash comparison points (edit-time vs save-time vs
    render-enqueue-time) and the UI contract for "proxy stale".
D3. **Proxy asset layout** — folder name, file naming (`TrackId` + hash), formats (U5), retention
    of previous valid proxy on failed render (product requirement) and cleanup policy in the
    absence of any media-GC precedent.
D4. **Render job model** — single worker vs limited pool; instance lifecycle (message-thread
    construct/restore/prepare → worker processBlock — §7.3); cancellation + generation guards
    (`loadGeneration_` pattern); faster-than-realtime vs realtime fallback detection (no
    `canDoOfflineProcessing` usage exists today).
D5. **Tail and latency policy** — tail length source (fixed, plugin-reported, user), latency trim
    of `getLatencySamples` at render vs playback, and the live-vs-proxy timing parity rule (§9).
D6. **Destination-mute semantics during proxy render** (U3).
D7. **Span rule** — what timeline range a v1 full-destination proxy covers (U6).
D8. **Playback-source selection contract** — host-level vs helper-level seam (§4.2), the
    timeline-position plumbing, and the Primary/Proxy/Secondary/silent priority table including
    audition behavior with missing Primary.
D9. **Secondary role scope** — descriptor/state home (a second host per track? a second
    `experimentalInstrumentTracks` entry?), never-sonically-identical presentation, and the later
    channel-remap hook. The current one-host-per-track registry (§3.2) does not yet model it.
D10. **Dependency enumeration** — whether to centralize `sourcesForDestination()` before building
    fingerprint collection on top (§5.2, hazard 4).
D11. **Prepare Portable Project** — packaging flow, its relationship to Save (must stay fast and
    non-blocking), and how "all proxies current" is verified before packaging.

---

## 13. Recommended order for the canonical steering document

Rationale: settle contracts that everything else composes against first; UI last.

1. **Process boundary + playback-source selection contract** (D8) — the seam in §4/§9, timeline
   plumbing, priority table. Everything else references this vocabulary.
2. **Dependency set + render fingerprint** (D1, D2, D10, F-table §10) — including the Organ
   Upper/Lower/Pedal worked example as the normative test case.
3. **Render job model** (D4, D5, D6, D7) — instance lifecycle, threading, cancellation,
   generations, tails/latency, fallback for offline-incompatible plugins.
4. **Media layout + atomic publication + retention** (D3) — including Windows replace semantics and
   missing-proxy degradation.
5. **Persistence/schema** — v20 additions (if any), migration notes, what stays runtime-only.
6. **Roles model: Primary / Secondary / Proxy** (D9) — registry shape, status vocabulary shared by
   engine and UI.
7. **Prepare Portable Project** (D11) — composition of 2–6.
8. **Test strategy** — deterministic proxy-parity tests at the `MidiDeliveryCaptureSink` boundary +
   rendered-file comparison, following the existing selftest/stability seam conventions.

---

## 14. Audit meta-report

**Files inspected (union of direct reads and delegated exploration):**

* `src/domain/`: `Track.h`, `Track.cpp`, `Session.h`, `Session.cpp`, `SessionSnapshot.h`,
  `SessionSnapshot.cpp`, `SessionRouting.h`, `SessionRouting.cpp`, `SessionHistory.h/.cpp`,
  `AudioClip.h`, `PlacedClip.h`, `ProjectMusicalTime.h`, `MixdownWavProbe.h`,
  `AudioMixdownProjectSettings.h/.cpp`
* `src/engine/`: `PlaybackEngine.h`, `PlaybackEngine.cpp`, `PlaybackMixHelpers.h/.cpp`,
  `RoutingPlan.h`, `RoutingPlanBuilder.h/.cpp`, `RecorderService.h/.cpp`
* `src/plugins/`: `ExperimentalInstrumentHost.h/.cpp`, `PluginInsertHost.h/.cpp`,
  `PluginTrackSlot.h`, `InsertSlotId.h`, `PluginDiscovery.h/.cpp`, `InstrumentCatalog.h/.cpp`,
  `PluginEditorWindows.h/.cpp`, `Vst3ChildProcessScan.h/.cpp`
* `src/instruments/`: `InstrumentTrackController.h`, `InstrumentTrackController.cpp`
* `src/playback/`: `ExperimentalInstrumentPlaybackBridge.h/.cpp`
* `src/app/`: `InstrumentRuntimeCoordinator.h/.cpp`, `AddInstrumentTrackCoordinator.h/.cpp`,
  `ProjectIoCoordinator.h/.cpp`, `AudioMixdownExporter.h/.cpp`, `AudioMixdownDialog.cpp`,
  `MainAppWindow.cpp`, `MainAppDialogs.h/.cpp`, `MainMenuModel.h/.cpp`, `MidiEditorPresenter.cpp`,
  `TrackLanesEditCoordinator.cpp`, `InstrumentTimelineRowCoordinator.cpp`,
  `UndoRedoCoordinator.h/.cpp`, `Vst3PluginPickerCoordinator.cpp`, `AudioClipImportCoordinator.cpp`
* `src/io/`: `ProjectFile.h`, `ProjectFile.cpp`, `InstrumentMidiClipExport.h/.cpp`,
  `ProjectAudioImport.h/.cpp`, `AudioWaveformCache.h/.cpp`, `AudioFileLoader.h/.cpp`
* `src/ui/`: `InspectorView.h/.cpp`, `TrackLanesView.cpp`;
  `src/ui/experimental/`: `ExperimentalMidiPattern.h`, `ExperimentalMidiCcAutomation.h`,
  `ExperimentalMidiEditorWindow.h/.cpp`, `ExperimentalMidiChannelDiagnostics.h`,
  `MidiEditorTitleStatus.h`, `ExperimentalMidiPatternPlayer.h`,
  `ExperimentalMidiPatternPlayerHostBinding.cpp`, `ExperimentalPianoRollView.cpp`,
  `MidiCcLaneViewState.h`
* `src/audio/`: `LatencySettingsStore.h/.cpp`; `src/transport/`: `Transport.h/.cpp`;
  `src/util/AsyncLifetimeToken.h`; `src/diagnostics/`: `StabilityScenarioRunner.h/.cpp`,
  `StabilityInvariants.cpp`, `StabilityDiagnosticLog.cpp`, `UiHangWatchdogDiag.h`; `src/Main.cpp`
* `docs/CURRENT_ARCHITECTURE.md` (contract cross-checks), `tools/lame/README.txt`

**Files changed:** exactly one — this document
(`docs/audits/PORTABLE_INSTRUMENTS_ARCHITECTURE_AUDIT.md`, new).

**Tests / builds run for this audit:** none. This is a documentation-only task; no build, selftest,
stability scenario, or certification run was required or performed as part of the audit.

**Confirmation:** no production code, header, test, schema, or project file was modified. No code
was committed or pushed.
