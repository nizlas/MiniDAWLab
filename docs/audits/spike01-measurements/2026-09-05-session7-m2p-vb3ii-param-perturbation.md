# SPIKE-01 sanitized measurement report — authoritative plugin-state capture

SPIKE-01 diagnostic output (roadmap slice P0/P1A; canonical steering §9.2 / PID-001).

**Privacy:** this report contains only sizes, SHA-256 hashes, timings, and
notification metadata. No raw plugin-state bytes, presets, or licensing
material are captured to disk by the SPIKE-01 probe.

## Environment

| Field | Value |
|---|---|
| Generated | 2026-09-05T23:45:04.816+02:00 |
| App version | 0.9.0 |
| Plugin | VB3-II |
| Plugin format | VST3 |
| Plugin version | 2.3.1 |
| Plugin identifier | C:\Program Files\Common Files\VST3\VB3-II.vst3 |
| Operator notes |  |

## Capture measurements per phase

| Phase / path | n | min ms | median ms | p95 ms | max ms | blob bytes (min..max) | distinct hashes | byte-stable |
|---|---|---|---|---|---|---|---|---|
| M2P-late / raw-getStateInformation | 40 | 0.43 | 0.48 | 0.98 | 1.04 | 10437..10437 | 1 | yes |
| M2P-mid / raw-getStateInformation | 2 | 0.52 | 0.53 | 0.53 | 0.53 | 10437..10437 | 1 | yes |
| M2P-post / raw-getStateInformation | 12 | 0.51 | 0.54 | 1.24 | 1.24 | 10437..10437 | 1 | yes |
| M2P-pre / raw-getStateInformation | 10 | 0.44 | 0.48 | 0.52 | 0.52 | 10437..10437 | 1 | yes |

## Hash inventory (12-hex prefixes; full hashes in raw sample log)

- **M2P-late**: `f19a23677ea7…`
- **M2P-mid**: `482a3aa086a1…`
- **M2P-post**: `f19a23677ea7…`
- **M2P-pre**: `f19a23677ea7…`

## Threading

- Captures executed on the message thread: 64
- Captures executed on other threads: 0 (expected: 0)
- Parameter/processor notifications on the message thread: 4
- Parameter/processor notifications on other threads: 0

## Parameter/processor notifications (4)

| Time | Kind | Param idx | Param name | Value | Thread | Detail |
|---|---|---|---|---|---|---|
| 2026-09-05T23:44:40.295+02:00 | paramChanged | 0 | Volume | 0.25 | message |  |
| 2026-09-05T23:44:40.794+02:00 | paramChanged | 0 | Volume | 0.50 | message |  |
| 2026-09-05T23:44:53.811+02:00 | paramChanged | 0 | Volume | 0.25 | message |  |
| 2026-09-05T23:44:54.309+02:00 | paramChanged | 0 | Volume | 0.50 | message |  |

## Raw sample log (64)

| Time | Phase | Path | ms | bytes | editor | transport | thread | sha256 |
|---|---|---|---|---|---|---|---|---|
| 2026-09-05T23:44:39.291+02:00 | M2P-pre | raw-getStateInformation | 0.52 | 10437 | closed | stopped | message | `f19a23677ea79fd355bedaab900af8afe956f8f6266026a634f7b935e8d1ba3c` |
| 2026-09-05T23:44:39.294+02:00 | M2P-pre | raw-getStateInformation | 0.44 | 10437 | closed | stopped | message | `f19a23677ea79fd355bedaab900af8afe956f8f6266026a634f7b935e8d1ba3c` |
| 2026-09-05T23:44:39.298+02:00 | M2P-pre | raw-getStateInformation | 0.50 | 10437 | closed | stopped | message | `f19a23677ea79fd355bedaab900af8afe956f8f6266026a634f7b935e8d1ba3c` |
| 2026-09-05T23:44:39.300+02:00 | M2P-pre | raw-getStateInformation | 0.46 | 10437 | closed | stopped | message | `f19a23677ea79fd355bedaab900af8afe956f8f6266026a634f7b935e8d1ba3c` |
| 2026-09-05T23:44:39.303+02:00 | M2P-pre | raw-getStateInformation | 0.49 | 10437 | closed | stopped | message | `f19a23677ea79fd355bedaab900af8afe956f8f6266026a634f7b935e8d1ba3c` |
| 2026-09-05T23:44:39.306+02:00 | M2P-pre | raw-getStateInformation | 0.47 | 10437 | closed | stopped | message | `f19a23677ea79fd355bedaab900af8afe956f8f6266026a634f7b935e8d1ba3c` |
| 2026-09-05T23:44:39.309+02:00 | M2P-pre | raw-getStateInformation | 0.45 | 10437 | closed | stopped | message | `f19a23677ea79fd355bedaab900af8afe956f8f6266026a634f7b935e8d1ba3c` |
| 2026-09-05T23:44:39.312+02:00 | M2P-pre | raw-getStateInformation | 0.49 | 10437 | closed | stopped | message | `f19a23677ea79fd355bedaab900af8afe956f8f6266026a634f7b935e8d1ba3c` |
| 2026-09-05T23:44:39.315+02:00 | M2P-pre | raw-getStateInformation | 0.44 | 10437 | closed | stopped | message | `f19a23677ea79fd355bedaab900af8afe956f8f6266026a634f7b935e8d1ba3c` |
| 2026-09-05T23:44:39.317+02:00 | M2P-pre | raw-getStateInformation | 0.48 | 10437 | closed | stopped | message | `f19a23677ea79fd355bedaab900af8afe956f8f6266026a634f7b935e8d1ba3c` |
| 2026-09-05T23:44:40.546+02:00 | M2P-mid | raw-getStateInformation | 0.52 | 10437 | closed | stopped | message | `482a3aa086a1af4d476e0eedbfe7e88959dd6cb5237657f642615211013bf7e9` |
| 2026-09-05T23:44:40.802+02:00 | M2P-post | raw-getStateInformation | 0.51 | 10437 | closed | stopped | message | `f19a23677ea79fd355bedaab900af8afe956f8f6266026a634f7b935e8d1ba3c` |
| 2026-09-05T23:44:40.898+02:00 | M2P-post | raw-getStateInformation | 0.53 | 10437 | closed | stopped | message | `f19a23677ea79fd355bedaab900af8afe956f8f6266026a634f7b935e8d1ba3c` |
| 2026-09-05T23:44:41.047+02:00 | M2P-post | raw-getStateInformation | 0.83 | 10437 | closed | stopped | message | `f19a23677ea79fd355bedaab900af8afe956f8f6266026a634f7b935e8d1ba3c` |
| 2026-09-05T23:44:41.300+02:00 | M2P-post | raw-getStateInformation | 0.54 | 10437 | closed | stopped | message | `f19a23677ea79fd355bedaab900af8afe956f8f6266026a634f7b935e8d1ba3c` |
| 2026-09-05T23:44:41.798+02:00 | M2P-post | raw-getStateInformation | 1.24 | 10437 | closed | stopped | message | `f19a23677ea79fd355bedaab900af8afe956f8f6266026a634f7b935e8d1ba3c` |
| 2026-09-05T23:44:42.800+02:00 | M2P-post | raw-getStateInformation | 0.52 | 10437 | closed | stopped | message | `f19a23677ea79fd355bedaab900af8afe956f8f6266026a634f7b935e8d1ba3c` |
| 2026-09-05T23:44:45.802+02:00 | M2P-late | raw-getStateInformation | 0.54 | 10437 | closed | stopped | message | `f19a23677ea79fd355bedaab900af8afe956f8f6266026a634f7b935e8d1ba3c` |
| 2026-09-05T23:44:45.806+02:00 | M2P-late | raw-getStateInformation | 0.56 | 10437 | closed | stopped | message | `f19a23677ea79fd355bedaab900af8afe956f8f6266026a634f7b935e8d1ba3c` |
| 2026-09-05T23:44:45.810+02:00 | M2P-late | raw-getStateInformation | 1.04 | 10437 | closed | stopped | message | `f19a23677ea79fd355bedaab900af8afe956f8f6266026a634f7b935e8d1ba3c` |
| 2026-09-05T23:44:45.813+02:00 | M2P-late | raw-getStateInformation | 0.52 | 10437 | closed | stopped | message | `f19a23677ea79fd355bedaab900af8afe956f8f6266026a634f7b935e8d1ba3c` |
| 2026-09-05T23:44:45.817+02:00 | M2P-late | raw-getStateInformation | 0.48 | 10437 | closed | stopped | message | `f19a23677ea79fd355bedaab900af8afe956f8f6266026a634f7b935e8d1ba3c` |
| 2026-09-05T23:44:45.819+02:00 | M2P-late | raw-getStateInformation | 0.44 | 10437 | closed | stopped | message | `f19a23677ea79fd355bedaab900af8afe956f8f6266026a634f7b935e8d1ba3c` |
| 2026-09-05T23:44:45.822+02:00 | M2P-late | raw-getStateInformation | 0.43 | 10437 | closed | stopped | message | `f19a23677ea79fd355bedaab900af8afe956f8f6266026a634f7b935e8d1ba3c` |
| 2026-09-05T23:44:45.825+02:00 | M2P-late | raw-getStateInformation | 0.48 | 10437 | closed | stopped | message | `f19a23677ea79fd355bedaab900af8afe956f8f6266026a634f7b935e8d1ba3c` |
| 2026-09-05T23:44:45.829+02:00 | M2P-late | raw-getStateInformation | 0.44 | 10437 | closed | stopped | message | `f19a23677ea79fd355bedaab900af8afe956f8f6266026a634f7b935e8d1ba3c` |
| 2026-09-05T23:44:45.832+02:00 | M2P-late | raw-getStateInformation | 0.45 | 10437 | closed | stopped | message | `f19a23677ea79fd355bedaab900af8afe956f8f6266026a634f7b935e8d1ba3c` |
| 2026-09-05T23:44:50.804+02:00 | M2P-late | raw-getStateInformation | 0.98 | 10437 | closed | stopped | message | `f19a23677ea79fd355bedaab900af8afe956f8f6266026a634f7b935e8d1ba3c` |
| 2026-09-05T23:44:50.810+02:00 | M2P-late | raw-getStateInformation | 0.51 | 10437 | closed | stopped | message | `f19a23677ea79fd355bedaab900af8afe956f8f6266026a634f7b935e8d1ba3c` |
| 2026-09-05T23:44:50.815+02:00 | M2P-late | raw-getStateInformation | 0.47 | 10437 | closed | stopped | message | `f19a23677ea79fd355bedaab900af8afe956f8f6266026a634f7b935e8d1ba3c` |
| 2026-09-05T23:44:50.818+02:00 | M2P-late | raw-getStateInformation | 0.43 | 10437 | closed | stopped | message | `f19a23677ea79fd355bedaab900af8afe956f8f6266026a634f7b935e8d1ba3c` |
| 2026-09-05T23:44:50.821+02:00 | M2P-late | raw-getStateInformation | 1.03 | 10437 | closed | stopped | message | `f19a23677ea79fd355bedaab900af8afe956f8f6266026a634f7b935e8d1ba3c` |
| 2026-09-05T23:44:50.825+02:00 | M2P-late | raw-getStateInformation | 0.50 | 10437 | closed | stopped | message | `f19a23677ea79fd355bedaab900af8afe956f8f6266026a634f7b935e8d1ba3c` |
| 2026-09-05T23:44:50.831+02:00 | M2P-late | raw-getStateInformation | 0.50 | 10437 | closed | stopped | message | `f19a23677ea79fd355bedaab900af8afe956f8f6266026a634f7b935e8d1ba3c` |
| 2026-09-05T23:44:50.836+02:00 | M2P-late | raw-getStateInformation | 0.54 | 10437 | closed | stopped | message | `f19a23677ea79fd355bedaab900af8afe956f8f6266026a634f7b935e8d1ba3c` |
| 2026-09-05T23:44:50.841+02:00 | M2P-late | raw-getStateInformation | 0.47 | 10437 | closed | stopped | message | `f19a23677ea79fd355bedaab900af8afe956f8f6266026a634f7b935e8d1ba3c` |
| 2026-09-05T23:44:50.845+02:00 | M2P-late | raw-getStateInformation | 0.48 | 10437 | closed | stopped | message | `f19a23677ea79fd355bedaab900af8afe956f8f6266026a634f7b935e8d1ba3c` |
| 2026-09-05T23:44:54.060+02:00 | M2P-mid | raw-getStateInformation | 0.53 | 10437 | closed | stopped | message | `482a3aa086a1af4d476e0eedbfe7e88959dd6cb5237657f642615211013bf7e9` |
| 2026-09-05T23:44:54.318+02:00 | M2P-post | raw-getStateInformation | 0.92 | 10437 | closed | stopped | message | `f19a23677ea79fd355bedaab900af8afe956f8f6266026a634f7b935e8d1ba3c` |
| 2026-09-05T23:44:54.410+02:00 | M2P-post | raw-getStateInformation | 0.51 | 10437 | closed | stopped | message | `f19a23677ea79fd355bedaab900af8afe956f8f6266026a634f7b935e8d1ba3c` |
| 2026-09-05T23:44:54.561+02:00 | M2P-post | raw-getStateInformation | 0.54 | 10437 | closed | stopped | message | `f19a23677ea79fd355bedaab900af8afe956f8f6266026a634f7b935e8d1ba3c` |
| 2026-09-05T23:44:54.811+02:00 | M2P-post | raw-getStateInformation | 0.54 | 10437 | closed | stopped | message | `f19a23677ea79fd355bedaab900af8afe956f8f6266026a634f7b935e8d1ba3c` |
| 2026-09-05T23:44:55.312+02:00 | M2P-post | raw-getStateInformation | 0.72 | 10437 | closed | stopped | message | `f19a23677ea79fd355bedaab900af8afe956f8f6266026a634f7b935e8d1ba3c` |
| 2026-09-05T23:44:56.312+02:00 | M2P-post | raw-getStateInformation | 0.53 | 10437 | closed | stopped | message | `f19a23677ea79fd355bedaab900af8afe956f8f6266026a634f7b935e8d1ba3c` |
| 2026-09-05T23:44:59.313+02:00 | M2P-late | raw-getStateInformation | 0.52 | 10437 | closed | stopped | message | `f19a23677ea79fd355bedaab900af8afe956f8f6266026a634f7b935e8d1ba3c` |
| 2026-09-05T23:44:59.319+02:00 | M2P-late | raw-getStateInformation | 0.53 | 10437 | closed | stopped | message | `f19a23677ea79fd355bedaab900af8afe956f8f6266026a634f7b935e8d1ba3c` |
| 2026-09-05T23:44:59.325+02:00 | M2P-late | raw-getStateInformation | 0.51 | 10437 | closed | stopped | message | `f19a23677ea79fd355bedaab900af8afe956f8f6266026a634f7b935e8d1ba3c` |
| 2026-09-05T23:44:59.328+02:00 | M2P-late | raw-getStateInformation | 0.43 | 10437 | closed | stopped | message | `f19a23677ea79fd355bedaab900af8afe956f8f6266026a634f7b935e8d1ba3c` |
| 2026-09-05T23:44:59.330+02:00 | M2P-late | raw-getStateInformation | 0.80 | 10437 | closed | stopped | message | `f19a23677ea79fd355bedaab900af8afe956f8f6266026a634f7b935e8d1ba3c` |
| 2026-09-05T23:44:59.335+02:00 | M2P-late | raw-getStateInformation | 0.47 | 10437 | closed | stopped | message | `f19a23677ea79fd355bedaab900af8afe956f8f6266026a634f7b935e8d1ba3c` |
| 2026-09-05T23:44:59.338+02:00 | M2P-late | raw-getStateInformation | 0.48 | 10437 | closed | stopped | message | `f19a23677ea79fd355bedaab900af8afe956f8f6266026a634f7b935e8d1ba3c` |
| 2026-09-05T23:44:59.343+02:00 | M2P-late | raw-getStateInformation | 0.46 | 10437 | closed | stopped | message | `f19a23677ea79fd355bedaab900af8afe956f8f6266026a634f7b935e8d1ba3c` |
| 2026-09-05T23:44:59.348+02:00 | M2P-late | raw-getStateInformation | 0.45 | 10437 | closed | stopped | message | `f19a23677ea79fd355bedaab900af8afe956f8f6266026a634f7b935e8d1ba3c` |
| 2026-09-05T23:44:59.354+02:00 | M2P-late | raw-getStateInformation | 0.52 | 10437 | closed | stopped | message | `f19a23677ea79fd355bedaab900af8afe956f8f6266026a634f7b935e8d1ba3c` |
| 2026-09-05T23:45:04.312+02:00 | M2P-late | raw-getStateInformation | 0.53 | 10437 | closed | stopped | message | `f19a23677ea79fd355bedaab900af8afe956f8f6266026a634f7b935e8d1ba3c` |
| 2026-09-05T23:45:04.317+02:00 | M2P-late | raw-getStateInformation | 0.52 | 10437 | closed | stopped | message | `f19a23677ea79fd355bedaab900af8afe956f8f6266026a634f7b935e8d1ba3c` |
| 2026-09-05T23:45:04.322+02:00 | M2P-late | raw-getStateInformation | 0.51 | 10437 | closed | stopped | message | `f19a23677ea79fd355bedaab900af8afe956f8f6266026a634f7b935e8d1ba3c` |
| 2026-09-05T23:45:04.326+02:00 | M2P-late | raw-getStateInformation | 0.48 | 10437 | closed | stopped | message | `f19a23677ea79fd355bedaab900af8afe956f8f6266026a634f7b935e8d1ba3c` |
| 2026-09-05T23:45:04.331+02:00 | M2P-late | raw-getStateInformation | 0.48 | 10437 | closed | stopped | message | `f19a23677ea79fd355bedaab900af8afe956f8f6266026a634f7b935e8d1ba3c` |
| 2026-09-05T23:45:04.335+02:00 | M2P-late | raw-getStateInformation | 0.47 | 10437 | closed | stopped | message | `f19a23677ea79fd355bedaab900af8afe956f8f6266026a634f7b935e8d1ba3c` |
| 2026-09-05T23:45:04.339+02:00 | M2P-late | raw-getStateInformation | 0.48 | 10437 | closed | stopped | message | `f19a23677ea79fd355bedaab900af8afe956f8f6266026a634f7b935e8d1ba3c` |
| 2026-09-05T23:45:04.342+02:00 | M2P-late | raw-getStateInformation | 0.49 | 10437 | closed | stopped | message | `f19a23677ea79fd355bedaab900af8afe956f8f6266026a634f7b935e8d1ba3c` |
| 2026-09-05T23:45:04.345+02:00 | M2P-late | raw-getStateInformation | 0.45 | 10437 | closed | stopped | message | `f19a23677ea79fd355bedaab900af8afe956f8f6266026a634f7b935e8d1ba3c` |
| 2026-09-05T23:45:04.348+02:00 | M2P-late | raw-getStateInformation | 0.44 | 10437 | closed | stopped | message | `f19a23677ea79fd355bedaab900af8afe956f8f6266026a634f7b935e8d1ba3c` |

## Operator notes

*None.*

---
*End of sanitized SPIKE-01 report. Verify before sharing: this file must
contain no base64 blocks and no plugin-state byte dumps.*
