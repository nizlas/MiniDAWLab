# Pre-gain and WAV/MP3 export — investigation, fixes and verification (2026-09-30)

Scope: two user reports on Danielssons Audio Lab 1.1.4 — (1) "pre-gain −24 dB on an AmpliTube
audio track changed nothing", (2) "MP3 export leaves a WAV behind, gets stuck at the end, shows a
white box, maybe crashed". Delivered in **1.1.5**. Everything below distinguishes what was
**reproduced**, what is a **confirmed cause established by code + measurement**, what was
**tested**, and what remains **unconfirmed**.

## 1. Export: reproduced defects and confirmed causes

Evidence used: `%APPDATA%\MiniDAWLab\mixdown-diag.log`, the user's `Mixdown` folder (read only),
the crash-dump folder, JUCE 8.0.4 sources, LAME 3.100, and the new focused test.

| Symptom | Status | Cause |
|---|---|---|
| Export "stuck at the end", never finishes | **Reproduced** (focused test, 15 s bound: LAME never exits) | LAME writes its banner and progress lines to stderr — **6,552 bytes for the user's 40 s file**, more than the 4 KiB anonymous pipe. The exporter captured stderr but never read it while polling `waitForProcessToFinish`, so LAME blocked on a full pipe at the very end of the encode and DAL waited for a 10-minute timeout. The user's log ends at `lame start` on 2026-09-28 23:20:44 with nothing after; the LAME output file was complete on disk. |
| A WAV file appears next to the MP3 | **Reproduced** (user folder still holds `…__dal_mp3_source_….wav` from 09-28 and from 08-25) | The intermediate WAV was created as a *visible sibling in the user's export folder* and deliberately kept on failure/timeout ("kept for debugging"); a killed app left it too. |
| "White box" | **Confirmed by JUCE source + on-screen capture** | JUCE 8's default Direct2D peer only *defers* `WM_PAINT` and draws on its next vblank message, which travels through JUCE's internal message window and never arrives while the blocking export owns the message thread; `performAnyPendingRepaintsNow()` is a **no-op** for Direct2D. The old progress window therefore never painted; with the window's own surface empty, Windows showed a blank/white rectangle. |
| "Maybe the app crashed" | **Unconfirmed — no evidence** | No crash dump exists for 2026-09-28; the newest dumps (2026-09-26) are AmpliTube-module faults during project load and unrelated to export. The log pattern (no timeout line after `lame start`) matches the app being ended by the user while LAME was blocked. |
| WAV export also produces MP3 | **Not a defect** (code + scenario) | The WAV path never starts LAME; the in-app scenario asserts the export folder contains exactly the `.wav`. |

## 2. Export: what changed (1.1.5)

- **`src/app/Mp3LameEncoder.h/.cpp` (new)** — runs LAME with its console pipe **drained continuously on a small helper thread** (`LameConsoleDrain`; producer = pipe reader, consumer = message thread under a `CriticalSection`, bounded 64 KiB tail). LAME can no longer block. The drained text yields LAME's real `( NN%)` for the bar (CBR size estimate before the first status line), the exit code and the error text. Cooperative cancel (kill + wait + delete partial output), 10-minute safety timeout, no output file left on any failure path.
- **`src/app/AudioMixdownExporter.cpp`** — the MP3 pipeline's intermediate WAV now lives in the **system temp folder** (`DAL-mixdown-<hex>.wav`) and both working files are owned by scope guards (`ScopedWorkingFile`) that remove them on every exit path; only the export's own uniquely tagged files are ever deleted. Phases reported as `Rendering... NN%`, `Encoding MP3... NN%`, `Finalizing...`; "complete" only after the destination has been replaced. Cancel is polled between render blocks and during the encode; the destination is never touched on cancel.
- **`src/app/AudioMixdownProgressWindow.h/.cpp` (new, moved out of the dialog TU)** — pins JUCE's **software renderer** for this one window and services **only its own HWND's** queued messages (`PeekMessage(hwnd, …)`): it paints, the Cancel button and Escape work, the window can be dragged, and Windows no longer flags the app as not responding. JUCE's timer/`callAsync` window and every other DAL window stay queued, so the established invariant — no session edit, plugin lifecycle, autosave or device change interleaves with the offline render — is unchanged. `Alt+F4` on the window means Cancel.
- **`src/app/AudioMixdownDialog.cpp`** — the progress window is destroyed before any completion alert is queued (single completion path); a cancel shows an info alert "Export cancelled. No file was written." instead of an error.
- **Lifecycle review (code):** the export is synchronous on the message thread, so project switch / quit / device change *during* an export are impossible by construction (documented in `docs/CURRENT_ARCHITECTURE.md`); the offline gate is RAII and observed released (`gateDepth=0`) after success, error and cancel; no callbacks target the progress window after it is destroyed (nothing holds a pointer to it); no worker or LAME process outlives the call (drain thread joined, LAME killed+awaited on cancel/timeout).

## 3. Pre-gain: measured on every production path — no defect found in the signal chain

All measurements ran the **production** code (no parallel implementation). Expected ratio for
−24 dB: 10^(−24/20) = **0.06310**.

| Path | Harness | Result |
|---|---|---|
| Track → SessionSnapshot → Session setter → published snapshot | focused test | −24 stored, published, no-op detection, other edits keep it |
| Undo / redo (`SessionHistory` + `restoreSessionSnapshotForUndo`) | focused test | 0 → −24 → 0 → −24 restored exactly |
| Save / reload (`Session::saveProjectToFile` / `loadProjectFromFile`, v23 JSON) | focused test **and** in-app scenario | −24 survives; JSON carries `preGainDb` |
| Dry clip path (staged renderer), legacy clip renderer, monitoring strip (synthetic input) | focused test | ratio **0.063096** on all three |
| "Exactly once" with fader 0.5 and pan hard-left | focused test | measured 0.015774 = clip × pre-gain × fader × pan |
| Live change 0 → −24 while rendering | focused test | monotone glide within one block (max step 0.00091 ≤ budget), next block exactly on target; first block after prepare applies the stored value directly |
| **Real VST3 through `PluginInsertHost`** (DAL Mono Delay, Mix 0 % = bit-transparent, state restored through the production `importChain` path) — clip path and monitoring path | focused test | plug-in input == output at 0 dB; ratio **0.063096** ⇒ the signal *entering the first Pre insert* is scaled |
| **Real AmpliTube 4** through `PluginInsertHost` (110 Hz test tone, default preset) | focused test `--amplitube` | output RMS 0.329 → 0.0296 (**−20.9 dB** for a −24 dB input change; the amp compresses ~3 dB). Measured, **not listened to**. |
| **Realtime engine via the audio device** (fixture tone, `--stability-pregain`, value set **while playing**) | in-app scenario | device-output peak 0.10000 → 0.00631, ratio **0.06310** |
| **Offline mixdown** (`--stability-pregain`) | in-app scenario | WAV RMS 0.070711 → 0.004462, ratio **0.06310** (applied exactly once) |
| Recording capture | design + existing InputRoutingFocusedTests | the recorder tap is before the strip; monitoring gain never reaches the take |

**Conclusion:** the reported "no effect" could not be reproduced on any driven path, including a
real AmpliTube instance. One code path *does* produce exactly that user experience and was
confirmed by reading: every **undoable** session edit — pre-gain included — is **refused while a
take is recording or the count-in runs** (`TrackLanesEditCoordinator`), and the refusal was
**silent**: the Inspector field snapped back to the old value without explanation (fader and pan,
which are direct non-undoable controls, keep working). Whether the user was recording at the time
is **unconfirmed**. 1.1.5 does not change that policy (an undo step recorded mid-take could later
restore a snapshot without the take) but makes the refusal visible: a bubble on the pre-gain field
says "Pre-gain can't be changed while recording or during count-in."

## 3b. Follow-up 2026-10-01: the user's project, through the real Inspector (status: not a software defect)

The user confirmed the −24 dB edit was made **during playback of recorded guitar, with no
recording or count-in**, so §3's refusal path is not the explanation. Investigation continued on a
temporary copy of the user's project (`TSE_pt2.dalproj`, saved AmpliTube 4 state included; the
original and its autosave were only read). Running exe at the time: the 1.1.4 Debug build
(`build\ninja-debug\…\MiniDAWLab.exe`, built 2026-09-30 20:01; installed Release 1.1.3) — same
pre-gain signal code as `main`.

**Session evidence (read-only):** the saved project (2026-09-30 20:27) has the AmpliTube track
(id 4, `Pre:AmpliTube 4`) **muted**, pre-gain 0. Every autosave of the live session on 2026-10-01
(latest 19:03, the project dirty and autosaving every 2 min) has track 4 **muted** and
**`preGainDb = -24.0`** — i.e. the Inspector commit *did* land, on the right track, in the live
session the user was looking at. The other audio track (id 1, an mp3 soundbite) is muted too;
the audible rows are the instrument tracks (VB3-II, Track 3, Track 8).

**Reproduction (`MiniDAWLab.exe --stability-pregain-inspector <copy>`, 1.1.6 alt-Debug):** the
track is activated like a header click, "-24" + Return is typed into the **real Inspector field**
through `TextEditor::keyPressed` (the production listener → handler → undoable Session edit
follows asynchronously, as for a user), and a new bounded `PluginInsertHost` level tap reports the
signal **entering the first insert** and **leaving the last one** for that track while playing.

| | Inspector shows / committed | AmpliTube chain blocks | level before first insert | after AmpliTube | device peak |
|---|---|---|---|---|---|
| Phase A — project state (track 4 **muted**), 0 dB, playing | `0.0` / 0.00 on track 4 | **0** | — | — | 0.236 (instruments) |
| Phase A — typed `-24` + Return while playing | `-24.0` / **−24.00 on track 4**, track 1 unchanged | **0** | — | — | 0.949 (instruments) |
| Phase B — track 4 unmuted like the header, typed `0`, same passage from 0 | `0.0` / 0.00 | 578 | RMS 0.019778 (peak 0.183) | RMS 0.193 (peak 0.792) | 0.920 |
| Phase B — typed `-24` + Return while playing, same passage from 0 | `-24.0` / −24.00 | 572 | RMS **0.001227 → ratio 0.06205** (expected 0.06310) | RMS 0.109 (**−4.96 dB** vs 0 dB) | 0.654 |

**Where the chain does and does not break:** displayed value, committed value and target track
agree at every step; the published snapshot carries the value; the realtime path applies it live
(ratio 0.062 after typing while playing). Two facts explain the "no change" the user heard:

1. In the user's session state the AmpliTube track is **muted**: a muted audio track renders
   nothing — its insert chain is not processed at all (0 blocks) — so no pre-gain value can be
   heard from it. Whatever guitar was audible then did not come from track 4 through AmpliTube.
2. Even unmuted, the user's saved AmpliTube preset compresses a **−24 dB input change into a
   ≈ −5 dB output change** (RMS 0.193 → 0.109). The input *is* 16× quieter; the amp model's gain
   staging hides most of it.

Status for the original observation: **not reproduced as a software defect**; explained by the
session state (mute) and the preset's compression. Conditions exercised: the user's project copy
with its saved AmpliTube state; 1.1.6 alt-Debug build; default audio output of this machine (the
user's ASIO device was held by the running DAL); typed input through the Inspector editor's key
path (OS keyboard events were not injected); Track 4 muted as saved, then unmuted like the header;
0 → −24 dB typed while the transport played. Not exercised: the user's own running 1.1.4 process
and its live mute state at the moment of the observation (only the 2-minute autosaves are visible),
and listening.

Recommendation for the user: check the Mute button on the AmpliTube track (header strip) before
judging pre-gain, and compare with the fader at −24 dB — with this preset the audible difference
of a −24 dB *input* change is a ≈5 dB level drop plus less drive, not a 24 dB drop.

**Side observation (outside this task's scope, recorded for the paused stability audit):** the
same scenario run on the **Release** 1.1.6 binary passed and then crashed during application
shutdown inside `AmpliTube 4.vpa` at module offset `0x7675E` — the identical module and offset
as the 2026-09-26 dumps (`crash-dumps\MiniDAWLab-crash-20261001-192759-pid44620.*`, breadcrumb
"app shutdown begin"). The Debug run of the same scenario exited cleanly. This is the known
"AmpliTube teardown — cause undetermined" item of `docs/audits/STABILITY_AUDIT_2026-09-26.md`,
now with a deterministic reproduction recipe (load this project copy, play, quit, Release build).

## 4. Tests run (Level 1–2 per `docs/DEVELOPMENT_TEST_POLICY.md`)

```powershell
# Focused, production-code harness (57 checks, 0 failures; --amplitube optional)
build\ninja-debug\MixdownPreGainFocusedTests_artefacts\Debug\MixdownPreGainFocusedTests.exe --out <evidenceDir> --amplitube
# Audio-only fixture for the in-app scenarios
MixdownPreGainFocusedTests.exe --make-fixture %TEMP%\dal-pregain-fixture
# In-app scenarios (real Session / PlaybackEngine / device / exporter), all PASS
MiniDAWLab.exe --stability-mixdown %TEMP%\dal-pregain-fixture\pregain-fixture.dalproj --format wav
MiniDAWLab.exe --stability-mixdown %TEMP%\dal-pregain-fixture\pregain-fixture.dalproj --format mp3
MiniDAWLab.exe --stability-pregain %TEMP%\dal-pregain-fixture\pregain-fixture.dalproj
# 2026-10-01: user flow through the real Inspector on a copy of a real project (§3b), PASS
MiniDAWLab.exe --stability-pregain-inspector <copy-of-project>.dalproj
# BPM fix preserved
PluginInsertTempoFocusedTests.exe "C:\Program Files\Common Files\VST3\DALMonoDelay.vst3"
```

What the export scenarios assert: exporter result ok; the export folder contains **exactly** the
result file; no `DAL-mixdown-*` working file left in the system temp folder; overwrite replaces a
sentinel; a **controlled write failure** (invalid file name → WAV writer open error / LAME
"Can't init outfile") is reported as an error and leaves **no** file; afterwards playback runs with
audible output and an advancing callback and the offline gate depth is 0; the runtime invariant
battery runs after every step. What the focused LAME tests assert: the pre-fix wait pattern
**blocks** (15 s bound, 958,464 of 1,602,240 bytes written), draining 6,594 console bytes releases
it; the production runner finishes the same 40 s encode in **~165 ms** with a real, non-decreasing
percentage reaching 100 %; the MP3 decodes back (`lame --decode`) to exactly 1,920,000 frames with
RMS 0.35357 vs source 0.35355; cancel removes the partial file; a garbage input fails cleanly with
LAME's message; a missing encoder fails without starting anything.

Progress window evidence (real window, message loop blocked exactly like an export, captured from
the **screen** with GDI and cancelled with a **real OS click** via `SendInput`):
`docs/evidence/mixdown-progress-2026-09-30/progress-rendering-42-screen.png`,
`progress-encoding-80-screen.png`, `progress-cancelling-screen.png`.

Not run: the full stability matrix (`scripts\stability-matrix.ps1`); the change touches the
export and Inspector paths only, and the mixdown scenarios are the policy's Level-2 mapping for it.

## 5. Remaining uncertainties and limits

- The user's pre-gain observation was not reproduced; the recording/count-in refusal is a confirmed
  silent path but unconfirmed as *their* cause. No listening test was performed (AmpliTube's output
  change is a measurement).
- The **dialog → progress window → alert** flow inside the running app was not driven by UI
  automation (the stability hook bypasses the dialog). The progress window class itself was
  verified on screen; the exporter/gate/file lifecycle was verified in-app.
- Cancel during the *render* phase was verified through the production cancel poll in the focused
  LAME/window tests and by code review of `exportStereoMixdownWavBlocking`; an in-app render-phase
  cancel was not driven (the fixture renders in ~100 ms).
- Two orphan working WAVs from earlier failed exports remain in the user's
  `C:\Users\nicla\Music\TSE_pt2_260827\Mixdown` folder (`home made mono delay.__dal_mp3_source_….wav`,
  `TSE_pt2.__dal_mp3_source_….wav`) plus one `…__dal_mp3_encode_….mp3`; 1.1.5 deliberately never
  sweeps foreign files, so these are left for the user to delete.
- Windows only; verified at 150 % display scaling on this machine.
