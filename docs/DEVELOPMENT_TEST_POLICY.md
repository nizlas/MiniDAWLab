# Development Test Policy — tiered testing for MiniDAWLab (DAL)

Purpose: pick the **smallest test that can falsify the change**. The full stability matrix and
release certification are deliberate gates, not per-edit checks. Running the full matrix after
every small implementation slice is explicitly **not wanted** — it is slow and adds no signal for
low-risk changes.

Related documents:

- `docs/STABILITY_TESTING.md` — what each stability scenario does, logs, and flags.
- `docs/RELEASE_CERTIFICATION.md` — the release gate (Level 4).
- `docs/VALIDATION_CHECKLIST.md` — general slice validation (code review side).

---

## Test levels

### Level 0 — Build check

Use for: documentation-only changes, comments, or tiny isolated code changes with no runtime path
impact.

```powershell
.\scripts\build-windows.ps1 -Config Debug
```

Expected: build succeeds, no new relevant compile warnings/errors.

### Level 1 — Targeted feature check

Use for: normal feature/bug/polish work. This is the **default level** for implementation slices.

Run:

1. Debug build.
2. The smallest manual or automated test that exercises the changed feature.
3. Inspect the relevant diagnostic log if applicable (`%APPDATA%\MiniDAWLab\*.log`).

Examples:

- **Window bounds change:** Debug build; move/resize main window, save, reload, verify restore.
  If MIDI editor bounds changed: open MIDI editor, resize, save/reload, verify. No mixdown or
  matrix required.
- **MIDI note editing change:** Debug build; manual test in the MIDI editor; targeted MIDI
  stability scenario only if one exists. No mixdown/export tests unless touched.
- **Mixdown change:** Debug build; `--stability-mixdown` wav (and mp3 if LAME is available);
  manual GUI check if the dialog changed. No delete-loop unless routing/audio callback changed.
- **Autosave change:** Debug build; `--stability-autosave` and `--stability-recover-autosave`.
  No mixdown unless the project save/export path changed.

### Level 2 — Relevant stability scenario(s)

Use when: a change touches **one** high-risk subsystem. Run only the scenarios that cover it:

- **Project load/save:** `--stability-load-loop` (and open/save/close paths); autosave/recover if
  save state was touched.
- **Track delete/undo/redo:** `--stability-delete-loop`; optionally `--stability-smoke`.
- **Routing/audio callback/scratch buffers/mix graph:** `--stability-smoke`,
  `--stability-delete-loop`; mixdown wav/mp3 if rendering is affected.
- **Export/mixdown:** `--stability-mixdown` wav/mp3 only.

Low iteration counts (2–3) are fine for a targeted regression check; use 5+ only when hunting
intermittent problems.

### Level 3 — Full stability matrix

Run **only** when:

- explicitly requested by Niclas,
- finishing a batch of runtime changes before commit,
- the change touches multiple core subsystems,
- a fix follows a crash/use-after-free/race,
- before packaging a tester build,
- after routing/session/audio-callback/plugin-lifetime changes,
- after stability tooling/invariants themselves changed.

```powershell
.\scripts\stability-matrix.ps1 -Project "<reference.dalproj>" -Iterations 5 -IncludeMixdown -IncludeAutosave
```

Do **not** run Level 3 automatically for every small UI change.

### Level 4 — Release certification

Run **only** when preparing a tester/release build. See `docs/RELEASE_CERTIFICATION.md`.

```powershell
.\scripts\certify-release.ps1 -Project "<reference.dalproj>" -Iterations 5
# Optional: -IncludeAsan -IncludePageHeap
```

---

## Subsystem-to-test mapping

| Changed area | Required normal tests | Optional stronger tests |
|---|---|---|
| Docs/scripts only | Level 0 build (or none if no code is compiled) | — |
| Main window / UI bounds | Debug build + manual bounds save/reload | load-loop, 3 iterations |
| MIDI editor UI only | Debug build + manual check in editor | — |
| MIDI note model/editing | Debug build + manual note edit test | MIDI-specific scenario if implemented; full matrix only if note model/persistence changed broadly |
| Project save/load schema | Debug build + load-loop (2–3) + manual save/reload of a real project | autosave/recover; full matrix before commit batch |
| Autosave/recovery | Debug build + `--stability-autosave` + `--stability-recover-autosave` | full matrix before release |
| Mixdown/export | Debug build + `--stability-mixdown` wav/mp3 + manual dialog click | full matrix before release |
| Routing/master/groups/sends | Debug build + smoke + delete-loop | mixdown wav/mp3; full matrix |
| Instrument/plugin hosting | Debug build + smoke + manual plugin load/edit | delete-loop; ASan delete-loop |
| Track delete/undo/redo | Debug build + delete-loop | smoke; full matrix |
| Stability tooling/invariants | Debug build + the scenarios the tooling change affects | full matrix (tooling changes gate everything else) |
| Installer/package | Release build + install + launch + open reference project | Level 4 certification |

---

## Known accepted visual tradeoffs (do not file as regressions)

- **Arrangement waveforms are approximate during active zoom/pan.** Audio lanes blit the previous
  wave raster scaled while the viewport is moving, so waveforms may look blurry/soft and a rapid
  zoom-out can briefly show a clipped waveform. Each lane rebuilds once after ~200 ms of viewport
  idle and then renders correctly. This is a deliberate part of the main-window playback/zoom
  responsiveness fix (see `docs/CURRENT_ARCHITECTURE.md`, current-time/playhead rendering).
  Report as a bug only if the waveform is still wrong **after** the viewport has been idle, or if
  chrome/labels/selection are missing. Higher-fidelity rendering during the gesture is future
  polish, not a correctness blocker.

---

## Default test discipline for implementation agents

- **Do not run the full stability matrix** unless the task explicitly asks for it or the change is
  high-risk per Level 3 criteria above.
- **Always report which test level was chosen and why.**
- Prefer the smallest test that can falsify the change.
- If a small targeted test fails, **stop and report** — do not mask it by running larger suites.
- If the change touches multiple high-risk subsystems, ask or explicitly justify escalating to
  Level 3.
- Every implementation report must include:
  - tests run,
  - tests intentionally **not** run,
  - the reason,
  - whether a full matrix run is recommended later (e.g. "before the next tester build").
