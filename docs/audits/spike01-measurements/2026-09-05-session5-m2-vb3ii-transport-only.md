> # ⚠️ INVALID / INCONCLUSIVE — DO NOT USE FOR CONCLUSIONS
>
> This session (`M2`, "transport-only") targeted trackId 4 (a VB3-II lane whose clip
> carried notes but which encountered no scheduled MIDI at the played transport range) and
> ran **without any MIDI-delivery proof**. Its "one hash across playing/stopped" result was
> misread as "transport/MIDI does not perturb VB3-II state." That interpretation is **retracted**:
> no MIDI/CC was proven to reach the instance, so the run proves nothing about playback effects.
> The raw measurements are preserved here as evidence only. The corrected, delivery-proven
> replacement is **session 9 (`M2V`)**; see audit report §28.3–§28.4 (revised) and §28.10.

# SPIKE-01 sanitized measurement report — authoritative plugin-state capture

SPIKE-01 diagnostic output (roadmap slice P0/P1A; canonical steering §9.2 / PID-001).

**Privacy:** this report contains only sizes, SHA-256 hashes, timings, and
notification metadata. No raw plugin-state bytes, presets, or licensing
material are captured to disk by the SPIKE-01 probe.

## Environment

| Field | Value |
|---|---|
| Generated | 2026-09-05T23:37:51.576+02:00 |
| App version | 0.9.0 |
| Plugin | VB3-II |
| Plugin format | VST3 |
| Plugin version | 2.3.1 |
| Plugin identifier | C:\Program Files\Common Files\VST3\VB3-II.vst3 |
| Operator notes |  |

## Capture measurements per phase

| Phase / path | n | min ms | median ms | p95 ms | max ms | blob bytes (min..max) | distinct hashes | byte-stable |
|---|---|---|---|---|---|---|---|---|
| M2-late / raw-getStateInformation | 40 | 0.42 | 0.49 | 0.88 | 1.16 | 10437..10437 | 1 | yes |
| M2-play / raw-getStateInformation | 40 | 0.44 | 0.52 | 0.94 | 1.15 | 10437..10437 | 1 | yes |
| M2-stop / raw-getStateInformation | 12 | 0.52 | 0.54 | 0.88 | 0.88 | 10437..10437 | 1 | yes |

## Hash inventory (12-hex prefixes; full hashes in raw sample log)

- **M2-late**: `f19a23677ea7…`
- **M2-play**: `f19a23677ea7…`
- **M2-stop**: `f19a23677ea7…`

## Threading

- Captures executed on the message thread: 92
- Captures executed on other threads: 0 (expected: 0)
- Parameter/processor notifications on the message thread: 0
- Parameter/processor notifications on other threads: 0

## Parameter/processor notifications (0)

*None observed while the listener was attached.*

## Raw sample log (92)

| Time | Phase | Path | ms | bytes | editor | transport | thread | sha256 |
|---|---|---|---|---|---|---|---|---|
| 2026-09-05T23:37:14.973+02:00 | M2-play | raw-getStateInformation | 0.53 | 10437 | closed | playing | message | `f19a23677ea79fd355bedaab900af8afe956f8f6266026a634f7b935e8d1ba3c` |
| 2026-09-05T23:37:14.979+02:00 | M2-play | raw-getStateInformation | 0.49 | 10437 | closed | playing | message | `f19a23677ea79fd355bedaab900af8afe956f8f6266026a634f7b935e8d1ba3c` |
| 2026-09-05T23:37:14.985+02:00 | M2-play | raw-getStateInformation | 0.51 | 10437 | closed | playing | message | `f19a23677ea79fd355bedaab900af8afe956f8f6266026a634f7b935e8d1ba3c` |
| 2026-09-05T23:37:14.989+02:00 | M2-play | raw-getStateInformation | 0.94 | 10437 | closed | playing | message | `f19a23677ea79fd355bedaab900af8afe956f8f6266026a634f7b935e8d1ba3c` |
| 2026-09-05T23:37:14.993+02:00 | M2-play | raw-getStateInformation | 0.91 | 10437 | closed | playing | message | `f19a23677ea79fd355bedaab900af8afe956f8f6266026a634f7b935e8d1ba3c` |
| 2026-09-05T23:37:14.997+02:00 | M2-play | raw-getStateInformation | 0.66 | 10437 | closed | playing | message | `f19a23677ea79fd355bedaab900af8afe956f8f6266026a634f7b935e8d1ba3c` |
| 2026-09-05T23:37:15.001+02:00 | M2-play | raw-getStateInformation | 0.62 | 10437 | closed | playing | message | `f19a23677ea79fd355bedaab900af8afe956f8f6266026a634f7b935e8d1ba3c` |
| 2026-09-05T23:37:15.004+02:00 | M2-play | raw-getStateInformation | 0.57 | 10437 | closed | playing | message | `f19a23677ea79fd355bedaab900af8afe956f8f6266026a634f7b935e8d1ba3c` |
| 2026-09-05T23:37:15.007+02:00 | M2-play | raw-getStateInformation | 0.57 | 10437 | closed | playing | message | `f19a23677ea79fd355bedaab900af8afe956f8f6266026a634f7b935e8d1ba3c` |
| 2026-09-05T23:37:15.010+02:00 | M2-play | raw-getStateInformation | 0.51 | 10437 | closed | playing | message | `f19a23677ea79fd355bedaab900af8afe956f8f6266026a634f7b935e8d1ba3c` |
| 2026-09-05T23:37:18.977+02:00 | M2-play | raw-getStateInformation | 0.72 | 10437 | closed | playing | message | `f19a23677ea79fd355bedaab900af8afe956f8f6266026a634f7b935e8d1ba3c` |
| 2026-09-05T23:37:18.982+02:00 | M2-play | raw-getStateInformation | 0.66 | 10437 | closed | playing | message | `f19a23677ea79fd355bedaab900af8afe956f8f6266026a634f7b935e8d1ba3c` |
| 2026-09-05T23:37:18.985+02:00 | M2-play | raw-getStateInformation | 0.48 | 10437 | closed | playing | message | `f19a23677ea79fd355bedaab900af8afe956f8f6266026a634f7b935e8d1ba3c` |
| 2026-09-05T23:37:18.988+02:00 | M2-play | raw-getStateInformation | 0.59 | 10437 | closed | playing | message | `f19a23677ea79fd355bedaab900af8afe956f8f6266026a634f7b935e8d1ba3c` |
| 2026-09-05T23:37:18.992+02:00 | M2-play | raw-getStateInformation | 0.46 | 10437 | closed | playing | message | `f19a23677ea79fd355bedaab900af8afe956f8f6266026a634f7b935e8d1ba3c` |
| 2026-09-05T23:37:18.994+02:00 | M2-play | raw-getStateInformation | 0.44 | 10437 | closed | playing | message | `f19a23677ea79fd355bedaab900af8afe956f8f6266026a634f7b935e8d1ba3c` |
| 2026-09-05T23:37:18.997+02:00 | M2-play | raw-getStateInformation | 0.47 | 10437 | closed | playing | message | `f19a23677ea79fd355bedaab900af8afe956f8f6266026a634f7b935e8d1ba3c` |
| 2026-09-05T23:37:19.000+02:00 | M2-play | raw-getStateInformation | 0.45 | 10437 | closed | playing | message | `f19a23677ea79fd355bedaab900af8afe956f8f6266026a634f7b935e8d1ba3c` |
| 2026-09-05T23:37:19.002+02:00 | M2-play | raw-getStateInformation | 0.48 | 10437 | closed | playing | message | `f19a23677ea79fd355bedaab900af8afe956f8f6266026a634f7b935e8d1ba3c` |
| 2026-09-05T23:37:19.005+02:00 | M2-play | raw-getStateInformation | 0.68 | 10437 | closed | playing | message | `f19a23677ea79fd355bedaab900af8afe956f8f6266026a634f7b935e8d1ba3c` |
| 2026-09-05T23:37:19.974+02:00 | M2-stop | raw-getStateInformation | 0.56 | 10437 | closed | stopped | message | `f19a23677ea79fd355bedaab900af8afe956f8f6266026a634f7b935e8d1ba3c` |
| 2026-09-05T23:37:20.126+02:00 | M2-stop | raw-getStateInformation | 0.55 | 10437 | closed | stopped | message | `f19a23677ea79fd355bedaab900af8afe956f8f6266026a634f7b935e8d1ba3c` |
| 2026-09-05T23:37:20.275+02:00 | M2-stop | raw-getStateInformation | 0.54 | 10437 | closed | stopped | message | `f19a23677ea79fd355bedaab900af8afe956f8f6266026a634f7b935e8d1ba3c` |
| 2026-09-05T23:37:20.526+02:00 | M2-stop | raw-getStateInformation | 0.88 | 10437 | closed | stopped | message | `f19a23677ea79fd355bedaab900af8afe956f8f6266026a634f7b935e8d1ba3c` |
| 2026-09-05T23:37:21.028+02:00 | M2-stop | raw-getStateInformation | 0.55 | 10437 | closed | stopped | message | `f19a23677ea79fd355bedaab900af8afe956f8f6266026a634f7b935e8d1ba3c` |
| 2026-09-05T23:37:22.030+02:00 | M2-stop | raw-getStateInformation | 0.54 | 10437 | closed | stopped | message | `f19a23677ea79fd355bedaab900af8afe956f8f6266026a634f7b935e8d1ba3c` |
| 2026-09-05T23:37:25.032+02:00 | M2-late | raw-getStateInformation | 0.55 | 10437 | closed | stopped | message | `f19a23677ea79fd355bedaab900af8afe956f8f6266026a634f7b935e8d1ba3c` |
| 2026-09-05T23:37:25.036+02:00 | M2-late | raw-getStateInformation | 0.64 | 10437 | closed | stopped | message | `f19a23677ea79fd355bedaab900af8afe956f8f6266026a634f7b935e8d1ba3c` |
| 2026-09-05T23:37:25.041+02:00 | M2-late | raw-getStateInformation | 0.56 | 10437 | closed | stopped | message | `f19a23677ea79fd355bedaab900af8afe956f8f6266026a634f7b935e8d1ba3c` |
| 2026-09-05T23:37:25.045+02:00 | M2-late | raw-getStateInformation | 0.48 | 10437 | closed | stopped | message | `f19a23677ea79fd355bedaab900af8afe956f8f6266026a634f7b935e8d1ba3c` |
| 2026-09-05T23:37:25.050+02:00 | M2-late | raw-getStateInformation | 0.50 | 10437 | closed | stopped | message | `f19a23677ea79fd355bedaab900af8afe956f8f6266026a634f7b935e8d1ba3c` |
| 2026-09-05T23:37:25.056+02:00 | M2-late | raw-getStateInformation | 0.47 | 10437 | closed | stopped | message | `f19a23677ea79fd355bedaab900af8afe956f8f6266026a634f7b935e8d1ba3c` |
| 2026-09-05T23:37:25.059+02:00 | M2-late | raw-getStateInformation | 0.50 | 10437 | closed | stopped | message | `f19a23677ea79fd355bedaab900af8afe956f8f6266026a634f7b935e8d1ba3c` |
| 2026-09-05T23:37:25.062+02:00 | M2-late | raw-getStateInformation | 0.46 | 10437 | closed | stopped | message | `f19a23677ea79fd355bedaab900af8afe956f8f6266026a634f7b935e8d1ba3c` |
| 2026-09-05T23:37:25.067+02:00 | M2-late | raw-getStateInformation | 0.50 | 10437 | closed | stopped | message | `f19a23677ea79fd355bedaab900af8afe956f8f6266026a634f7b935e8d1ba3c` |
| 2026-09-05T23:37:25.070+02:00 | M2-late | raw-getStateInformation | 0.45 | 10437 | closed | stopped | message | `f19a23677ea79fd355bedaab900af8afe956f8f6266026a634f7b935e8d1ba3c` |
| 2026-09-05T23:37:30.031+02:00 | M2-late | raw-getStateInformation | 0.55 | 10437 | closed | stopped | message | `f19a23677ea79fd355bedaab900af8afe956f8f6266026a634f7b935e8d1ba3c` |
| 2026-09-05T23:37:30.037+02:00 | M2-late | raw-getStateInformation | 0.55 | 10437 | closed | stopped | message | `f19a23677ea79fd355bedaab900af8afe956f8f6266026a634f7b935e8d1ba3c` |
| 2026-09-05T23:37:30.040+02:00 | M2-late | raw-getStateInformation | 0.47 | 10437 | closed | stopped | message | `f19a23677ea79fd355bedaab900af8afe956f8f6266026a634f7b935e8d1ba3c` |
| 2026-09-05T23:37:30.043+02:00 | M2-late | raw-getStateInformation | 0.88 | 10437 | closed | stopped | message | `f19a23677ea79fd355bedaab900af8afe956f8f6266026a634f7b935e8d1ba3c` |
| 2026-09-05T23:37:30.047+02:00 | M2-late | raw-getStateInformation | 0.59 | 10437 | closed | stopped | message | `f19a23677ea79fd355bedaab900af8afe956f8f6266026a634f7b935e8d1ba3c` |
| 2026-09-05T23:37:30.051+02:00 | M2-late | raw-getStateInformation | 0.56 | 10437 | closed | stopped | message | `f19a23677ea79fd355bedaab900af8afe956f8f6266026a634f7b935e8d1ba3c` |
| 2026-09-05T23:37:30.053+02:00 | M2-late | raw-getStateInformation | 0.48 | 10437 | closed | stopped | message | `f19a23677ea79fd355bedaab900af8afe956f8f6266026a634f7b935e8d1ba3c` |
| 2026-09-05T23:37:30.056+02:00 | M2-late | raw-getStateInformation | 0.49 | 10437 | closed | stopped | message | `f19a23677ea79fd355bedaab900af8afe956f8f6266026a634f7b935e8d1ba3c` |
| 2026-09-05T23:37:30.059+02:00 | M2-late | raw-getStateInformation | 0.47 | 10437 | closed | stopped | message | `f19a23677ea79fd355bedaab900af8afe956f8f6266026a634f7b935e8d1ba3c` |
| 2026-09-05T23:37:30.061+02:00 | M2-late | raw-getStateInformation | 0.42 | 10437 | closed | stopped | message | `f19a23677ea79fd355bedaab900af8afe956f8f6266026a634f7b935e8d1ba3c` |
| 2026-09-05T23:37:36.045+02:00 | M2-play | raw-getStateInformation | 1.15 | 10437 | closed | playing | message | `f19a23677ea79fd355bedaab900af8afe956f8f6266026a634f7b935e8d1ba3c` |
| 2026-09-05T23:37:36.050+02:00 | M2-play | raw-getStateInformation | 0.88 | 10437 | closed | playing | message | `f19a23677ea79fd355bedaab900af8afe956f8f6266026a634f7b935e8d1ba3c` |
| 2026-09-05T23:37:36.054+02:00 | M2-play | raw-getStateInformation | 0.75 | 10437 | closed | playing | message | `f19a23677ea79fd355bedaab900af8afe956f8f6266026a634f7b935e8d1ba3c` |
| 2026-09-05T23:37:36.059+02:00 | M2-play | raw-getStateInformation | 0.69 | 10437 | closed | playing | message | `f19a23677ea79fd355bedaab900af8afe956f8f6266026a634f7b935e8d1ba3c` |
| 2026-09-05T23:37:36.062+02:00 | M2-play | raw-getStateInformation | 1.03 | 10437 | closed | playing | message | `f19a23677ea79fd355bedaab900af8afe956f8f6266026a634f7b935e8d1ba3c` |
| 2026-09-05T23:37:36.066+02:00 | M2-play | raw-getStateInformation | 0.63 | 10437 | closed | playing | message | `f19a23677ea79fd355bedaab900af8afe956f8f6266026a634f7b935e8d1ba3c` |
| 2026-09-05T23:37:36.069+02:00 | M2-play | raw-getStateInformation | 0.49 | 10437 | closed | playing | message | `f19a23677ea79fd355bedaab900af8afe956f8f6266026a634f7b935e8d1ba3c` |
| 2026-09-05T23:37:36.072+02:00 | M2-play | raw-getStateInformation | 0.50 | 10437 | closed | playing | message | `f19a23677ea79fd355bedaab900af8afe956f8f6266026a634f7b935e8d1ba3c` |
| 2026-09-05T23:37:36.075+02:00 | M2-play | raw-getStateInformation | 0.46 | 10437 | closed | playing | message | `f19a23677ea79fd355bedaab900af8afe956f8f6266026a634f7b935e8d1ba3c` |
| 2026-09-05T23:37:36.078+02:00 | M2-play | raw-getStateInformation | 0.47 | 10437 | closed | playing | message | `f19a23677ea79fd355bedaab900af8afe956f8f6266026a634f7b935e8d1ba3c` |
| 2026-09-05T23:37:40.033+02:00 | M2-play | raw-getStateInformation | 0.54 | 10437 | closed | playing | message | `f19a23677ea79fd355bedaab900af8afe956f8f6266026a634f7b935e8d1ba3c` |
| 2026-09-05T23:37:40.037+02:00 | M2-play | raw-getStateInformation | 0.46 | 10437 | closed | playing | message | `f19a23677ea79fd355bedaab900af8afe956f8f6266026a634f7b935e8d1ba3c` |
| 2026-09-05T23:37:40.042+02:00 | M2-play | raw-getStateInformation | 0.48 | 10437 | closed | playing | message | `f19a23677ea79fd355bedaab900af8afe956f8f6266026a634f7b935e8d1ba3c` |
| 2026-09-05T23:37:40.045+02:00 | M2-play | raw-getStateInformation | 0.53 | 10437 | closed | playing | message | `f19a23677ea79fd355bedaab900af8afe956f8f6266026a634f7b935e8d1ba3c` |
| 2026-09-05T23:37:40.048+02:00 | M2-play | raw-getStateInformation | 0.46 | 10437 | closed | playing | message | `f19a23677ea79fd355bedaab900af8afe956f8f6266026a634f7b935e8d1ba3c` |
| 2026-09-05T23:37:40.050+02:00 | M2-play | raw-getStateInformation | 0.44 | 10437 | closed | playing | message | `f19a23677ea79fd355bedaab900af8afe956f8f6266026a634f7b935e8d1ba3c` |
| 2026-09-05T23:37:40.053+02:00 | M2-play | raw-getStateInformation | 0.48 | 10437 | closed | playing | message | `f19a23677ea79fd355bedaab900af8afe956f8f6266026a634f7b935e8d1ba3c` |
| 2026-09-05T23:37:40.055+02:00 | M2-play | raw-getStateInformation | 0.46 | 10437 | closed | playing | message | `f19a23677ea79fd355bedaab900af8afe956f8f6266026a634f7b935e8d1ba3c` |
| 2026-09-05T23:37:40.058+02:00 | M2-play | raw-getStateInformation | 0.59 | 10437 | closed | playing | message | `f19a23677ea79fd355bedaab900af8afe956f8f6266026a634f7b935e8d1ba3c` |
| 2026-09-05T23:37:40.061+02:00 | M2-play | raw-getStateInformation | 0.47 | 10437 | closed | playing | message | `f19a23677ea79fd355bedaab900af8afe956f8f6266026a634f7b935e8d1ba3c` |
| 2026-09-05T23:37:41.040+02:00 | M2-stop | raw-getStateInformation | 0.54 | 10437 | closed | stopped | message | `f19a23677ea79fd355bedaab900af8afe956f8f6266026a634f7b935e8d1ba3c` |
| 2026-09-05T23:37:41.169+02:00 | M2-stop | raw-getStateInformation | 0.56 | 10437 | closed | stopped | message | `f19a23677ea79fd355bedaab900af8afe956f8f6266026a634f7b935e8d1ba3c` |
| 2026-09-05T23:37:41.321+02:00 | M2-stop | raw-getStateInformation | 0.54 | 10437 | closed | stopped | message | `f19a23677ea79fd355bedaab900af8afe956f8f6266026a634f7b935e8d1ba3c` |
| 2026-09-05T23:37:41.572+02:00 | M2-stop | raw-getStateInformation | 0.52 | 10437 | closed | stopped | message | `f19a23677ea79fd355bedaab900af8afe956f8f6266026a634f7b935e8d1ba3c` |
| 2026-09-05T23:37:42.073+02:00 | M2-stop | raw-getStateInformation | 0.52 | 10437 | closed | stopped | message | `f19a23677ea79fd355bedaab900af8afe956f8f6266026a634f7b935e8d1ba3c` |
| 2026-09-05T23:37:43.072+02:00 | M2-stop | raw-getStateInformation | 0.53 | 10437 | closed | stopped | message | `f19a23677ea79fd355bedaab900af8afe956f8f6266026a634f7b935e8d1ba3c` |
| 2026-09-05T23:37:46.077+02:00 | M2-late | raw-getStateInformation | 0.54 | 10437 | closed | stopped | message | `f19a23677ea79fd355bedaab900af8afe956f8f6266026a634f7b935e8d1ba3c` |
| 2026-09-05T23:37:46.083+02:00 | M2-late | raw-getStateInformation | 0.50 | 10437 | closed | stopped | message | `f19a23677ea79fd355bedaab900af8afe956f8f6266026a634f7b935e8d1ba3c` |
| 2026-09-05T23:37:46.087+02:00 | M2-late | raw-getStateInformation | 0.50 | 10437 | closed | stopped | message | `f19a23677ea79fd355bedaab900af8afe956f8f6266026a634f7b935e8d1ba3c` |
| 2026-09-05T23:37:46.092+02:00 | M2-late | raw-getStateInformation | 0.49 | 10437 | closed | stopped | message | `f19a23677ea79fd355bedaab900af8afe956f8f6266026a634f7b935e8d1ba3c` |
| 2026-09-05T23:37:46.095+02:00 | M2-late | raw-getStateInformation | 0.45 | 10437 | closed | stopped | message | `f19a23677ea79fd355bedaab900af8afe956f8f6266026a634f7b935e8d1ba3c` |
| 2026-09-05T23:37:46.100+02:00 | M2-late | raw-getStateInformation | 0.48 | 10437 | closed | stopped | message | `f19a23677ea79fd355bedaab900af8afe956f8f6266026a634f7b935e8d1ba3c` |
| 2026-09-05T23:37:46.103+02:00 | M2-late | raw-getStateInformation | 0.46 | 10437 | closed | stopped | message | `f19a23677ea79fd355bedaab900af8afe956f8f6266026a634f7b935e8d1ba3c` |
| 2026-09-05T23:37:46.106+02:00 | M2-late | raw-getStateInformation | 0.43 | 10437 | closed | stopped | message | `f19a23677ea79fd355bedaab900af8afe956f8f6266026a634f7b935e8d1ba3c` |
| 2026-09-05T23:37:46.110+02:00 | M2-late | raw-getStateInformation | 0.51 | 10437 | closed | stopped | message | `f19a23677ea79fd355bedaab900af8afe956f8f6266026a634f7b935e8d1ba3c` |
| 2026-09-05T23:37:46.113+02:00 | M2-late | raw-getStateInformation | 0.45 | 10437 | closed | stopped | message | `f19a23677ea79fd355bedaab900af8afe956f8f6266026a634f7b935e8d1ba3c` |
| 2026-09-05T23:37:51.077+02:00 | M2-late | raw-getStateInformation | 1.11 | 10437 | closed | stopped | message | `f19a23677ea79fd355bedaab900af8afe956f8f6266026a634f7b935e8d1ba3c` |
| 2026-09-05T23:37:51.082+02:00 | M2-late | raw-getStateInformation | 1.16 | 10437 | closed | stopped | message | `f19a23677ea79fd355bedaab900af8afe956f8f6266026a634f7b935e8d1ba3c` |
| 2026-09-05T23:37:51.086+02:00 | M2-late | raw-getStateInformation | 0.48 | 10437 | closed | stopped | message | `f19a23677ea79fd355bedaab900af8afe956f8f6266026a634f7b935e8d1ba3c` |
| 2026-09-05T23:37:51.089+02:00 | M2-late | raw-getStateInformation | 0.44 | 10437 | closed | stopped | message | `f19a23677ea79fd355bedaab900af8afe956f8f6266026a634f7b935e8d1ba3c` |
| 2026-09-05T23:37:51.095+02:00 | M2-late | raw-getStateInformation | 0.48 | 10437 | closed | stopped | message | `f19a23677ea79fd355bedaab900af8afe956f8f6266026a634f7b935e8d1ba3c` |
| 2026-09-05T23:37:51.098+02:00 | M2-late | raw-getStateInformation | 0.72 | 10437 | closed | stopped | message | `f19a23677ea79fd355bedaab900af8afe956f8f6266026a634f7b935e8d1ba3c` |
| 2026-09-05T23:37:51.101+02:00 | M2-late | raw-getStateInformation | 0.47 | 10437 | closed | stopped | message | `f19a23677ea79fd355bedaab900af8afe956f8f6266026a634f7b935e8d1ba3c` |
| 2026-09-05T23:37:51.104+02:00 | M2-late | raw-getStateInformation | 0.45 | 10437 | closed | stopped | message | `f19a23677ea79fd355bedaab900af8afe956f8f6266026a634f7b935e8d1ba3c` |
| 2026-09-05T23:37:51.107+02:00 | M2-late | raw-getStateInformation | 0.44 | 10437 | closed | stopped | message | `f19a23677ea79fd355bedaab900af8afe956f8f6266026a634f7b935e8d1ba3c` |
| 2026-09-05T23:37:51.109+02:00 | M2-late | raw-getStateInformation | 0.46 | 10437 | closed | stopped | message | `f19a23677ea79fd355bedaab900af8afe956f8f6266026a634f7b935e8d1ba3c` |

## Operator notes

*None.*

---
*End of sanitized SPIKE-01 report. Verify before sharing: this file must
contain no base64 blocks and no plugin-state byte dumps.*
