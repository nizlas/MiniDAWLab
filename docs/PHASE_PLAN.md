# Phase plan (current stub)

This document is **only** for **current and near‑term slice planning**.

- It is **not** the source of truth for **how the code is wired today**. For that, read **[`docs/CURRENT_ARCHITECTURE.md`](CURRENT_ARCHITECTURE.md)** first.
- The **Detailed historical phased goals, acceptance nuances, and long steering text** moved to **[`docs/ARCHIVE/PHASE_PLAN_FULL.md`](ARCHIVE/PHASE_PLAN_FULL.md)** (verbatim archive). Quote or open that file when you need exact phase wording.

Use [`docs/IMPLEMENTATION_GUIDE.md`](IMPLEMENTATION_GUIDE.md), [`docs/VALIDATION_CHECKLIST.md`](VALIDATION_CHECKLIST.md), and [`docs/ARCHITECTURE_PRINCIPLES.md`](ARCHITECTURE_PRINCIPLES.md) for workflow, gates, and timeless constraints—this stub does not duplicate them.

---

## Current focus (now)

**Steering / doc slimming and architecture–workflow stabilization** before growing new product surface. Prefer small, reviewable slices with an explicit checklist pass.

---

## Near-term stabilization backlog (ordered-ish)

Suggested next candidates (each its **own slice** unless the user bundles explicitly):

| Item | Notes |
|------|--------|
| **Proxy transport Start/Stop step (open defect from the 1.1.15 DC task)** | A proxy whose asset sits on an instrument offset (VB3-II −0.605) steps 0 ↔ −0.605 at the proxy boundary on Play / Stop / restart; through the user's 10 Hz low-cut this is a ≈ −19 dBFS (unity) pulse the Primary does not produce (its offset persists while stopped). Fix belongs in proxy *transport* behaviour (idle output while stopped — offset continuity), not in the asset or the inserts; a ramp cannot hide it through a 10 Hz high-pass. Measured: `docs/PROXY_TAIL_POLICY_V2_2026-10-05.md` §6, `docs/evidence/proxy-tail-policy-v2-2026-10-05/edges-release/`. |
| **Proxy playback-edge comparison with the Primary playing its last notes** | The edge scenario seeks into the tail, where the Primary plays nothing; a like-for-like Primary tail (play from before the last notes through EOF) and the Primary's own offset onset at the first note were not measured (side finding of the 1.1.15 task). |
| **Mixer follow-ups (side findings of the 1.1.16 / 1.1.17 slices, not implemented)** | (a) Insert reordering in the mixer is a context menu (open / up / down / other stage / remove); the Inspector's drag-and-drop was not duplicated. (b) ~~More than 3 inserts per stage show "+N more (Inspector)"~~ — done in 1.1.17 (scrolling lists). (c) `--stability-live-midi` on the TSE copy fails its header-cell geometry check for the fixture's MIDI rows (pre-existing, project-dependent lane height; the scenario passes on its pre-gain fixture) — not a mixer regression. (d) The 150-track stress test and a real mouse-drag test of a mixer fader remain open. (e) `Window > Mixer` is the only menu item showing a shortcut; the other menus still show none. |
| **Parallel processing of independent audio channels and whole insert chains** | 1.1.18 parallelised only the live-instrument generation stage. Audio-clip rows, their Pre / Post insert chains and the instrument rows' insert chains still run serially on the callback thread; on the 100-track project the 18 AmpliTube 4 instances dominate the callback. Candidates: per-row generation + insert chain as one job (dependency order: rows → Groups → Stereo Out), same pool, same publish-before-destroy and state-capture gates. **Status:** slices **A1** (parallel audio-row strips, one combined batch with instrument generation, serial fan-out in plan order — checkpoint `99ff7f5`) and **A2** (instrument generation + the instrument row's strip combined into ONE job per eligible row — `0b9b6eb`, tests `e5b69c4`; serial exceptions: proxy rows, shared host instances, audition/stopped blocks, bus strips, monitoring pass, overflow fallbacks) are **implemented on branch `cursor/parallel-audio-readahead-plan-da54`** (not merged) with the extended focused suite `AudioStripParallelFocusedTests` (85 checks). Cloud (Linux Debug) verification covers logic/determinism only; local Windows/ASIO verification (listening + perf A/B per the plan's corrected recipe — build-vs-build, NOT `--instrument-workers 0`) remains for both slices. Plan: [`docs/PARALLEL_AUDIO_AND_READAHEAD_PLAN.md`](PARALLEL_AUDIO_AND_READAHEAD_PLAN.md) §3/§5 (Stage A, surveyed at `f11c0fb`). |
| **ASIO-Guard-like pre-processing for paths that need no live response** | Rows that are neither monitored, armed nor receiving live MIDI could be rendered ahead of the callback (one or more blocks of look-ahead) on worker threads, leaving the callback with mixing + live paths only. Needs a clear "live" classification per row (monitor / arm / live MIDI / proxy-backed) and must never add latency to live paths. **Plan (NOT implemented; design draft with unresolved ownership-transition questions):** [`docs/PARALLEL_AUDIO_AND_READAHEAD_PLAN.md`](PARALLEL_AUDIO_AND_READAHEAD_PLAN.md) §4/§5 (Stage B, surveyed at `f11c0fb`). |
| **Track automation, stage 1: Channel Volume and Pan** | **Planned, not implemented.** Arrangement lanes, Touch Read/Write, Inspector and mixer, undo, save/load, realtime and offline. VST3 parameter automation is a later stage. Baseline is `3a6f338` (1.3.1: parallel strips on main; read-ahead on at depth 3 unless saved Off or `--no-readahead`). Plan: [`docs/TRACK_AUTOMATION_PLAN.md`](TRACK_AUTOMATION_PLAN.md). |
| **Consistent Mute / Off behaviour for insert processing** | Today a muted row still processes its inserts (gain 0 after the chain, state kept) while an Off row skips everything; the 1.1.18 perf measurement loaded the muted AmpliTube rows anyway. Decide and document ONE rule (e.g. mute = process but silent; a separate "bypass inserts" affordance), apply it identically in realtime, offline export and proxy render, and keep tails / state predictable. **Note:** the per-kind semantics actually in the code at `f11c0fb` (muted rows *skip* their insert chains; only the instrument *host* keeps processing under mute) are tabulated in [`docs/PARALLEL_AUDIO_AND_READAHEAD_PLAN.md`](PARALLEL_AUDIO_AND_READAHEAD_PLAN.md) §2.7, including where this row's wording drifts from the code — unchanged there; this backlog row still owns the decision. |
| **Staged teardown of the previous project** | Opening a project while a large one is loaded retires all instrument runtimes in ONE unit (`clearExperimentalInstrumentRuntimes`, ~15 s for 64 hosts on the 100-track copy, announced as "Closing previous project…" since 1.1.19). Splitting it per row (publish-before-destroy per host) would keep that phase responsive too. |
| **Diagnostic log gating** | Keep always-on logs from spamming normal users; compile‑time / config gates where appropriate ([`docs/CURRENT_ARCHITECTURE.md`](CURRENT_ARCHITECTURE.md) — diagnostics). |
| **Stale singleton‑era comments** | Align headers/comments with **`TrackId`‑keyed**, multi‑instrument reality; [`docs/CURRENT_ARCHITECTURE.md`](CURRENT_ARCHITECTURE.md). |
| **Dead helper cleanup** | Remove unused helpers naming “primary” / single‑slot-era APIs where safe ([`docs/CURRENT_ARCHITECTURE.md`](CURRENT_ARCHITECTURE.md) — naming debt). |
| **Later: `InstrumentRuntimeRegistry` extraction** | Pull instrument registry / wiring clutter out of **`Main.cpp`** when ready—**preserve behavior** ([`docs/CURRENT_ARCHITECTURE.md`](CURRENT_ARCHITECTURE.md) — composition pointers). |

---

## Explicitly deferred (unless the user resumes them)

- **HALion** and broad third‑party instrument policy — out of scope until steered ([`docs/CURRENT_ARCHITECTURE.md`](CURRENT_ARCHITECTURE.md) — non‑goals).
- **Richer plugin picker / scanner UX** beyond current steering — defer unless phased.
- **Rescan crash hardening** (child‑process failures) — **known risk**, fix **only** when scheduled ([`docs/CURRENT_ARCHITECTURE.md`](CURRENT_ARCHITECTURE.md) — Rescan).

---

## Slice discipline (reminder)

**Do not** mix **feature work**, **cleanup**, and **structural refactor** in one slice unless the user explicitly requests that combo. Keep changes **narrow** and **behavior‑preserving** when the slice is labeled cleanup/refactor.

---

## Links

| Question | Doc |
|---------|-----|
| What does the codebase do today? | [`CURRENT_ARCHITECTURE.md`](CURRENT_ARCHITECTURE.md) |
| Full phase history text | [`ARCHIVE/PHASE_PLAN_FULL.md`](ARCHIVE/PHASE_PLAN_FULL.md) |
| How to implement / escalate | [`IMPLEMENTATION_GUIDE.md`](IMPLEMENTATION_GUIDE.md) |
| How to validate | [`VALIDATION_CHECKLIST.md`](VALIDATION_CHECKLIST.md) |
| Track automation, stage 1 (planned, not implemented) | [`TRACK_AUTOMATION_PLAN.md`](TRACK_AUTOMATION_PLAN.md) |
