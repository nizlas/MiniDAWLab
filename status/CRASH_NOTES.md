# CRASH_NOTES — manual Add clip import (diagnostics)

This file documents a **diagnosis-only** logging pass for repeated **Add clip…** imports. Grep the application log (or JUCE’s default `Logger` output) for the fixed tag **`[CLIMPORT]`**.

## Repro (stress)

1. Run MiniDAWLab (Debug or Release is fine; logs go through `juce::Logger::writeToLog`).
2. Click **Add clip…** many times, picking the same or different short WAV/AIFF files.
3. Optionally repeat with **one or few very long** files to stress RAM.
4. Optionally click **Add clip…** again while the file dialog is still open (second open should be ignored; see `STAGE:ui:ignored_second_add_while_chooser_in_flight`).

## Log tag index (in typical order of one successful import)

| Log line (prefix) | Meaning |
| --- | --- |
| `STAGE:ui:ignored_second_add_while_chooser_in_flight` | A second **Add clip…** was clicked while a chooser was already in flight. |
| `STAGE:ui:chooser_dismissed_cancel_or_no_file` | Picker closed with cancel or no file. |
| `STAGE:ui:fail no_audio_device` | No output device. |
| `STAGE:ui:chooser_ok` | A file was chosen. |
| `STAGE:session:entry` | `Session::addClipFromFileAtPlayhead` started. |
| `STAGE:decode:entry` | `AudioFileLoader` about to allocate the decode buffer. |
| `STAGE:decode:ok` | Decode and `AudioClip` construction finished. |
| `STAGE:decode:fail` (with `OOM` / `std::exception` / `unknown`) | Failure inside loader; session unchanged. |
| `STAGE:session:decode_ok` | Per-clip PCM size after decode (bytes ≈ ch × samples × 4). |
| `STAGE:session:build:begin` | About to run `SessionSnapshot::withClipAddedAsNewestOnTargetTrack`. |
| `STAGE:session:publish:begin` + `clipsAfter` + `totalPcmBytesApprox` | New snapshot is built; about to `atomic_store` it. |
| `STAGE:session:publish:ok` | Publish finished. |
| `STAGE:session:build:fail` (OOM / exception) | Build/publish path failed; **no** half-publish (old snapshot should remain if store was not reached; see “Limitations” below). |
| `STAGE:ui:session_add_failed` | `addClipFromFileAtPlayhead` returned error. |
| `STAGE:ui:sync:begin` / `STAGE:ui:sync:done` | `TrackLanesView::syncTracksFromSession` + repaint. |
| `STAGE:peaks:rebuild:begin` / `rebuild:done` / `rebuild:abort` | `ClipWaveformView::rebuildPeaksIfNeeded` (one line per **lane** when snapshot/width change). |

## How to read “last safe stage”

- Last line **before** a hard crash = strongest hint.
- If the last line is `STAGE:decode:entry` and no `decode:ok` → failure during **file read or buffer / clip construction** in the loader.
- If you see `decode:ok` and `STAGE:session:build:begin` but not `publish:begin` or `publish:ok` → look at `build:fail` or a crash **inside** `withClipAdded...` (see limitations).
- If you see `publish:ok` and `STAGE:ui:sync:done` but problems during paint → `STAGE:peaks:*` on that lane.
- `totalPcmBytesApprox` is the **sum of all** clips in the new snapshot: `sum(channels × samples × 4)`.

## Cumulative memory line

- `STAGE:session:publish:begin` includes **`totalPcmBytesApprox`** after a successful build — use it to see whether repeated imports are approaching process memory limits.

## Limitations (important)

- `SessionSnapshot::withClipAddedAsNewestOnTargetTrack` is marked `noexcept`. If an **allocation** inside that path fails in a way that `std::vector` would throw, a **noexcept** function can invoke **`std::terminate`**, which a C++ `try/catch` will **not** catch. In that case the last log is often `STAGE:session:build:begin` with no `publish:begin`.
- The import path remains **synchronous** on the **message thread** (no background decode in this pass).

## Where logs appear

- Depends on your JUCE `Logger` configuration: attach a `FileLogger`, or run under a debugger and watch the debug console if enabled.

## Manual tests (checklist)

- [ ] Many small files in a row — full chain of `decode:ok` → `publish:ok` → `ui:sync:done` → `peaks:rebuild:done` per add (peaks per lane on change).
- [ ] Few very large files — expect `decode:fail` (OOM) or `session:build:fail` with a **message** instead of silent exit where catchable.
- [ ] Second **Add clip…** with dialog open — `ignored_second_add_while_chooser_in_flight` once, no stuck state.
- [ ] After failure, a normal small-file add still works.
- [ ] Playback, drag, and track add/reorder still behave as before (no intended behaviour change in this pass).
