# Export level / click diagnosis, level meters and the Inspector channel panel — report (2026-10-01, 1.1.8)

User report on 1.1.7: a click at the start and the end of the exported MP3, extra distortion in
the MP3 (worst when many sounds play together), the WAV export also slightly distorted, normal
playback in DAL fine, lowering the Stereo Out channel volume "seemed to help", no visible level /
overload indication. The project's intentional distortion (VB3-II, AmpliTube) was to be preserved.

Everything below was measured on a **copy** of the user's project (`TSE_pt2`, with its `Audio/`
and `InstrumentProxies/`) through the production code paths; the real project, its media and the
running DAL process were not touched. Listening verification was **not** performed (no monitoring
from this environment) — all statements are measurements.

---

## 1. Symptom status

| Symptom | Status |
|---|---|
| Click at the start and end of the MP3 | **Reproduced and explained.** A constant DC offset from the VB3-II plug-in (user's preset) sits in the rendered signal; the file therefore starts and ends on a step (0 → +0.34 → 0 at master 0 dB; in the decoded MP3 a 0.14–0.15 step at the encoder-delay boundary and at the end). Not a DAL rendering fault; the plug-in's behaviour. DAL now shows it (DC tag on the track / Stereo Out meters, DC line in the export result). No fade or DC filter was added (would mask the cause). |
| Extra distortion in the MP3, also WAV slightly distorted | **Reproduced and explained — mix overload, not a rendering bug.** The user's float WAV (exported at Stereo Out 0 dB) peaks at **+1.43 dBFS (L) / +2.59 dBFS (R)** with 340 / 1512 samples above full scale; the DC offset (+0.34) eats a third of the headroom. Any PCM conversion / player clips there; MP3 adds overshoot and codes the clipped waveform badly. Lowering the Stereo Out fader by 5 dB brings the same mix to −4.3 dBFS peak with 0 overs — exactly what the user observed. The level **is** the realtime level (see §2). DAL now shows it (meters, overload latch, export result). |
| Playback in DAL sounds correct | **Verified consistent.** The realtime Stereo Out and the offline render are sample-identical for deterministic content and carry the same gains on the real project (peaks within 0.2–1.4 dB, same −12 dB master response). The DC is inaudible in realtime because it is continuous there (no start/stop step), and the overload only occurs at the hottest passages. |
| Lowering Stereo Out volume helped | **Confirmed as a correct remedy for the overload**, not a workaround for a path difference. |
| No level / overload indication | **Fixed**: Inspector channel panel with track meter + Stereo Out meter, overload latch, DC tag; export result reports peak / overload / DC. |

---

## 2. Realtime vs export — same passage, production paths

### 2.1 Deterministic (ExportLevelFocusedTests, stub device, production `PlaybackEngine`)

Two tone clips (0.6 and 0.7 amplitude) on two tracks, master row present, routing plan built:

| Case | Result |
|---|---|
| Both at 0 dB, centre pan | realtime callback vs `renderOfflineMixdownBlock`: **max |Δ| = 0.0** (sample-identical); peak 1.241 > 1.0 — two 0 dB tones already exceed full scale (overload is real mix content) |
| Master fader −12 dB | offline ratio 0.25119, realtime ratio 0.25119 (exact), still sample-identical |
| Track fader 0.5 + hard-left pan, mute | sample-identical; muted track silent, hard-left pan leaves R silent |

So master fader, track fader, pan and mute are applied once and identically on both paths.

### 2.2 Real project copy (`--stability-export-levels`, DAL 1.1.8 Debug, ASIO device)

Loop 0–40 s at 48 kHz, Stereo Out fader −5.00 dB (user's saved state).

| Measurement (L / R) | Realtime Stereo Out (device output) | Offline float32 WAV | 24-bit WAV | MP3 192 (render stats) |
|---|---|---|---|---|
| Sample peak | −5.53 / −5.71 dBFS | −5.35 / −4.33 dBFS | −5.35 / −4.33 | −5.26 / −4.33 |
| RMS | −19.8 / −19.7 dBFS | −15.1 / −15.1 dBFS | −15.1 / −15.1 | −15.1 / −15.1 |
| DC offset | 0.056 (pass A) / 0.040 (pass B) | 0.161 | 0.162 | 0.161 |
| Overs (|x| > 1) | 0 | 0 | 0 | 0 |
| Master −12 dB | peak −10.8 dB, RMS −7.3 dB (DC-dominated) | peak −12.00, RMS −12.00 dB | — | RMS −12.0 dB |

Realtime pass B (after the exports) and offline B agree to 0.3 dB in peak and 0.0 dB in RMS, i.e.
the two paths are equivalent; the RMS gap in pass A is entirely the DC term (see §3) — pass A ran
right after loading, before VB3-II had played, so its DC was present only ~30 % of the time.

Decoded MP3 vs its float WAV (JUCE MP3 decoder to float, cross-correlation aligned): peak −0.25 dB,
RMS −0.26 dB, no overs, codec error −14.6 dB — LAME does **not** change the level. The user's own
MP3 was 6.8 dB quieter than their WAV because the Stereo Out fader had been lowered between the
two exports (its DC of 0.151 = 0.340 × 0.44 proves the same mix at the lower fader).

Float→PCM: JUCE's writer hard-clips 16/24-bit output at ±1.0; float WAV keeps the overs. The MP3
intermediate is a float32 WAV in the system temp folder (unchanged 1.1.5 pipeline, verified again:
exact result file set, no leftovers, `--stability-mixdown wav/mp3` PASS).

---

## 3. The click: DC offset from VB3-II

* Per-track sweep (post-strip meter, 3 s from the loop start, realtime): **VB3-II row = constant
  0.34042 on both channels** (peak = RMS = DC → pure offset, no audio), Track 8 (HALion + Mono
  Delay) DC 0.0000, Groove Agent and audio rows silent. 0.34042 / 0.5623 (track fader −5 dB) =
  **0.605 raw**.
* Isolated probe (`ExportLevelFocusedTests --probe-vst3-dc`, plain JUCE host, no DAL): VB3-II with
  the project's saved state — silence 0.000 before any note; three held notes dc +0.25; after
  note-off **dc −0.589 … −0.605 (−4.4 dBFS) that never decays**, even after `reset()`. With the
  plug-in's default state: dc 0.000 before/after notes. The offset is therefore a property of the
  user's VB3-II preset once notes have been played.
* Per-second timeline of the export: DC 0.1914 from 0 to 22 s (organ idle), then varying
  0.03–0.19 while the organ plays, back to 0.1914 at the end. First sample 0.19, last 0.19.
* The instrument proxies carry no DC (track 5 proxy dc 0.005; track 3 proxy is all-zero).
* In the user's files: WAV dcHead = dcTail = 0.340 (first sample 0.340 → `maxStepHead 0.34@0`);
  MP3 `maxStepHead 0.15@1105` (encoder-delay boundary) and `maxStepTail 0.16@1921104`.

DAL does not filter DC (it would alter the plug-in's sound and hide the fault). It now flags it:
"DC" tag on the row's meter and on the Stereo Out meter, and a DC-offset line in the export
result that tells the user to find the track whose meter shows DC and reload / change that plug-in's
preset (or insert a high-pass).

---

## 4. What changed in DAL

* `engine/LevelMeterAccumulator.h` — realtime-safe block statistics (peak hold, overs, RMS, DC,
  NaN) folded on the audio thread, drained by the UI; no locks / allocation / logging in the callback.
* `PlaybackEngine` — Stereo Out accumulator (device output after the master strip, every callback
  return path) + one metered-track accumulator (post-strip stage: audio clip segments, Monitor pass,
  instrument host output, group bus strip); separate diagnostics windows for scenarios.
* `AudioMixdownExporter` — `MixdownExportLevelReport` measured on the float block before the writer;
  logged and shown in the completion alert (peak L/R, OVERLOAD count, DC OFFSET, NaN).
* Inspector — `InspectorPanel` (scroll viewport + fixed `ChannelStripPanel`), `ChannelFaderComponent`
  (−∞…+6 dB graded scale, value field, Ctrl/Cmd+click = 0 dB, wheel), `LevelMeterComponent` (0…−60
  dBFS, L/R or mono, 24 dB/s time-based fall, 2 s peak hold, uncapped held peak text, sticky
  overload lamp, DC tag). The old Channel-volume text field is removed from the scroll area; the
  fader drives the same `Session::setTrackChannelFaderGain` (same no-undo policy, linear gain stored;
  −∞ = 0.0, never "Infinity"). Inspector default width 160 px (was 90) so the three columns fit.
* Measuring points documented in `docs/CURRENT_ARCHITECTURE.md`.

Not changed: pre-gain, inserts, sends, pan, routing, offline render order, LAME pipeline, plug-in
reset behaviour (an export starts from the plug-ins' current state — see §6), track headers, mixer.

---

## 5. Verification

| Check | How | Result |
|---|---|---|
| Realtime == offline (fader / pan / mute / master, deterministic) | `ExportLevelFocusedTests` (47 checks) | PASS, max |Δ| = 0 |
| Accumulator: peak survives slow UI, overs per channel, mono, NaN | same | PASS |
| Fader scale −∞ / 0 / +6, graded travel, text parse, round trip through the project file | same | PASS; file contains no Infinity/NaN |
| Real project: realtime vs float / 24-bit / MP3, per-track sweep, −12 dB master both paths | `--stability-export-levels` (Debug, ASIO) | PASS (tables in §2–3) |
| Files: peak, oversampled-peak estimate (4×, not a certified true-peak meter), RMS, overs, edges, MP3 alignment | `ExportLevelFocusedTests --analyze` | see §2–3 |
| Inspector panel per row kind (audio / instrument / MIDI / master), meters show signal while playing, overload latch sets at +6 dB master and resets, typed −6 / +3 / −inf / 12 → 0.501 / 1.413 / 0 / 1.995, reset gesture → 1.0, all controls reachable by scrolling at a 640-px-high window, panel pinned ≥ 150 px, no overlaps, fader text == session gain | `--stability-inspector-panel` (running app, PNG evidence in `docs/evidence/inspector-channel-panel-2026-10-01/`) | PASS |
| Export regression | `--stability-mixdown wav` / `mp3`, `--stability-pregain`, `MixdownPreGainFocusedTests --no-ui` | PASS |
| Other regressions | InputRouting 56, InsertPersistence 121, TrackHeaderColumn 103, WaveformReload 30, Selftests 3220, PluginInsertTempo, `--stability-header-column` | PASS |

Running-app verification: both `--stability-*` scenarios drive the real app (ASIO device, real
plug-ins, real Inspector components); PNGs are component snapshots of the live Inspector column at
150 % display scale. Offscreen / handler-level: `ExportLevelFocusedTests`. Code review only: the
`juce::Viewport` scrollbar styling, the hover highlight of the overload lamp. Not performed:
listening.

---

## 5b. Build / artifacts / commit

| | |
|---|---|
| Version | **1.1.8** (schema 23 unchanged) |
| Commit | `976871c34740a1af72c66ce60b8ce055139e6891` on `main` (+ this hash note) |
| Release exe | `build\ninja-release\MiniDAWLab_artefacts\Release\MiniDAWLab.exe` (FileVersion 1.1.8) |
| Installer / portable | `dist\DanielssonsAudioLab-1.1.8-Setup.exe`, `dist\DanielssonsAudioLab-1.1.8.zip` (symbols `dist\symbols\DanielssonsAudioLab-1.1.8\`) |
| Debug exe **with the changes** | `build\ninja-debug-alt\MiniDAWLab_artefacts\Debug\MiniDAWLab.exe` — the normal Debug location (`build\ninja-debug\...`) was locked by the user's running 1.1.7 Debug instance, which was left running |
| Focused test exe | `build\ninja-debug-alt\ExportLevelFocusedTests_artefacts\Debug\ExportLevelFocusedTests.exe` |

## 6. Remaining uncertainties / not done

* **Listening** was not possible here; the measured chain is fully consistent with the report, but
  the user should re-export with the Stereo Out meter showing no overload and confirm by ear.
* **Export starts from the plug-ins' current state** (no reset / pre-roll): a delay or reverb tail
  from previous playback can appear in the first blocks, and VB3-II's DC state depends on its
  history. A reset before export was *not* added in this slice (JUCE's VST3 `reset()` re-activates
  the component; AmpliTube 4 is already fragile at teardown, and VB3-II's offset survives `reset()`
  anyway). Candidate for a later, separately verified slice.
* **AmpliTube 4 teardown crash** (`AmpliTube 4.vpa` offset `0x7675E`) still intermittent at app
  shutdown / project reload (seen once again during this work); unchanged, tracked in the audit.
* Track 3's instrument proxy file is all zeros (stale proxy); not investigated here.
* The panel fader has no undo step, exactly like the removed text field; adding undo for channel
  volume is a product decision for later.
