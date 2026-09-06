# SPIKE-01 sanitized measurement report — authoritative plugin-state capture

SPIKE-01 diagnostic output (roadmap slice P0/P1A; canonical steering §9.2 / PID-001).

**Privacy:** this report contains only sizes, SHA-256 hashes, timings, and
notification metadata. No raw plugin-state bytes, presets, or licensing
material are captured to disk by the SPIKE-01 probe.

## Environment

| Field | Value |
|---|---|
| Generated | 2026-09-06T00:08:59.964+02:00 |
| App version | 0.9.0 |
| Plugin | VB3-II |
| Plugin format | VST3 |
| Plugin version | 2.3.1 |
| Plugin identifier | C:\Program Files\Common Files\VST3\VB3-II.vst3 |
| Operator notes |  |

## Capture measurements per phase

| Phase / path | n | min ms | median ms | p95 ms | max ms | blob bytes (min..max) | distinct hashes | byte-stable |
|---|---|---|---|---|---|---|---|---|
| M2-late / raw-getStateInformation | 40 | 0.43 | 0.48 | 0.63 | 1.18 | 10393..10415 | 2 | NO |
| M2-play / raw-getStateInformation | 40 | 0.42 | 0.48 | 0.60 | 1.42 | 10393..10413 | 4 | NO |
| M2-stop / raw-getStateInformation | 12 | 0.52 | 0.55 | 0.91 | 0.91 | 10393..10415 | 2 | NO |

## Hash inventory (12-hex prefixes; full hashes in raw sample log)

- **M2-late**: `55c9edcfc93f…`, `875dc964caa6…`
- **M2-play**: `006605833a09…`, `4511715dd079…`, `875dc964caa6…`, `a7f57a0b5cd8…`
- **M2-stop**: `55c9edcfc93f…`, `875dc964caa6…`

## Threading

- Captures executed on the message thread: 92
- Captures executed on other threads: 0 (expected: 0)
- Parameter/processor notifications on the message thread: 0
- Parameter/processor notifications on other threads: 369

## Parameter/processor notifications (369)

| Time | Kind | Param idx | Param name | Value | Thread | Detail |
|---|---|---|---|---|---|---|
| 2026-09-06T00:08:22.555+02:00 | paramChanged | 54 |  | 0.43 | other |  |
| 2026-09-06T00:08:22.575+02:00 | paramChanged | 54 |  | 0.43 | other |  |
| 2026-09-06T00:08:22.585+02:00 | paramChanged | 54 |  | 0.42 | other |  |
| 2026-09-06T00:08:22.595+02:00 | paramChanged | 54 |  | 0.41 | other |  |
| 2026-09-06T00:08:22.615+02:00 | paramChanged | 54 |  | 0.40 | other |  |
| 2026-09-06T00:08:22.616+02:00 | paramChanged | 54 |  | 0.39 | other |  |
| 2026-09-06T00:08:22.635+02:00 | paramChanged | 54 |  | 0.39 | other |  |
| 2026-09-06T00:08:22.655+02:00 | paramChanged | 54 |  | 0.38 | other |  |
| 2026-09-06T00:08:22.656+02:00 | paramChanged | 54 |  | 0.37 | other |  |
| 2026-09-06T00:08:22.666+02:00 | paramChanged | 54 |  | 0.36 | other |  |
| 2026-09-06T00:08:22.675+02:00 | paramChanged | 54 |  | 0.35 | other |  |
| 2026-09-06T00:08:22.696+02:00 | paramChanged | 54 |  | 0.35 | other |  |
| 2026-09-06T00:08:22.705+02:00 | paramChanged | 54 |  | 0.34 | other |  |
| 2026-09-06T00:08:22.715+02:00 | paramChanged | 54 |  | 0.33 | other |  |
| 2026-09-06T00:08:22.736+02:00 | paramChanged | 54 |  | 0.32 | other |  |
| 2026-09-06T00:08:22.736+02:00 | paramChanged | 54 |  | 0.31 | other |  |
| 2026-09-06T00:08:22.746+02:00 | paramChanged | 54 |  | 0.31 | other |  |
| 2026-09-06T00:08:22.776+02:00 | paramChanged | 54 |  | 0.30 | other |  |
| 2026-09-06T00:08:22.776+02:00 | paramChanged | 54 |  | 0.29 | other |  |
| 2026-09-06T00:08:22.785+02:00 | paramChanged | 54 |  | 0.28 | other |  |
| 2026-09-06T00:08:22.795+02:00 | paramChanged | 54 |  | 0.28 | other |  |
| 2026-09-06T00:08:22.816+02:00 | paramChanged | 54 |  | 0.27 | other |  |
| 2026-09-06T00:08:22.825+02:00 | paramChanged | 54 |  | 0.26 | other |  |
| 2026-09-06T00:08:22.836+02:00 | paramChanged | 54 |  | 0.25 | other |  |
| 2026-09-06T00:08:22.856+02:00 | paramChanged | 54 |  | 0.24 | other |  |
| 2026-09-06T00:08:22.856+02:00 | paramChanged | 54 |  | 0.24 | other |  |
| 2026-09-06T00:08:22.865+02:00 | paramChanged | 54 |  | 0.23 | other |  |
| 2026-09-06T00:08:22.896+02:00 | paramChanged | 54 |  | 0.22 | other |  |
| 2026-09-06T00:08:22.896+02:00 | paramChanged | 54 |  | 0.21 | other |  |
| 2026-09-06T00:08:22.906+02:00 | paramChanged | 54 |  | 0.20 | other |  |
| 2026-09-06T00:08:22.916+02:00 | paramChanged | 54 |  | 0.20 | other |  |
| 2026-09-06T00:08:22.936+02:00 | paramChanged | 54 |  | 0.19 | other |  |
| 2026-09-06T00:08:22.945+02:00 | paramChanged | 54 |  | 0.18 | other |  |
| 2026-09-06T00:08:22.955+02:00 | paramChanged | 54 |  | 0.17 | other |  |
| 2026-09-06T00:08:22.975+02:00 | paramChanged | 54 |  | 0.17 | other |  |
| 2026-09-06T00:08:22.976+02:00 | paramChanged | 54 |  | 0.16 | other |  |
| 2026-09-06T00:08:22.985+02:00 | paramChanged | 54 |  | 0.15 | other |  |
| 2026-09-06T00:08:22.995+02:00 | paramChanged | 54 |  | 0.14 | other |  |
| 2026-09-06T00:08:23.015+02:00 | paramChanged | 54 |  | 0.13 | other |  |
| 2026-09-06T00:08:23.025+02:00 | paramChanged | 54 |  | 0.13 | other |  |
| 2026-09-06T00:08:23.036+02:00 | paramChanged | 54 |  | 0.12 | other |  |
| 2026-09-06T00:08:23.055+02:00 | paramChanged | 54 |  | 0.11 | other |  |
| 2026-09-06T00:08:23.056+02:00 | paramChanged | 54 |  | 0.10 | other |  |
| 2026-09-06T00:08:23.075+02:00 | paramChanged | 54 |  | 0.09 | other |  |
| 2026-09-06T00:08:23.095+02:00 | paramChanged | 54 |  | 0.09 | other |  |
| 2026-09-06T00:08:23.096+02:00 | paramChanged | 54 |  | 0.08 | other |  |
| 2026-09-06T00:08:23.106+02:00 | paramChanged | 54 |  | 0.07 | other |  |
| 2026-09-06T00:08:23.116+02:00 | paramChanged | 54 |  | 0.06 | other |  |
| 2026-09-06T00:08:23.136+02:00 | paramChanged | 54 |  | 0.06 | other |  |
| 2026-09-06T00:08:23.145+02:00 | paramChanged | 54 |  | 0.05 | other |  |
| 2026-09-06T00:08:23.155+02:00 | paramChanged | 54 |  | 0.04 | other |  |
| 2026-09-06T00:08:23.175+02:00 | paramChanged | 54 |  | 0.03 | other |  |
| 2026-09-06T00:08:23.176+02:00 | paramChanged | 54 |  | 0.02 | other |  |
| 2026-09-06T00:08:23.196+02:00 | paramChanged | 54 |  | 0.02 | other |  |
| 2026-09-06T00:08:23.215+02:00 | paramChanged | 54 |  | 0.01 | other |  |
| 2026-09-06T00:08:23.216+02:00 | paramChanged | 54 |  | 0.00 | other |  |
| 2026-09-06T00:08:23.225+02:00 | paramChanged | 54 |  | 0.01 | other |  |
| 2026-09-06T00:08:23.235+02:00 | paramChanged | 54 |  | 0.02 | other |  |
| 2026-09-06T00:08:23.255+02:00 | paramChanged | 54 |  | 0.03 | other |  |
| 2026-09-06T00:08:23.256+02:00 | paramChanged | 54 |  | 0.04 | other |  |
| 2026-09-06T00:08:23.265+02:00 | paramChanged | 54 |  | 0.05 | other |  |
| 2026-09-06T00:08:23.275+02:00 | paramChanged | 54 |  | 0.06 | other |  |
| 2026-09-06T00:08:23.296+02:00 | paramChanged | 54 |  | 0.07 | other |  |
| 2026-09-06T00:08:23.296+02:00 | paramChanged | 54 |  | 0.08 | other |  |
| 2026-09-06T00:08:23.306+02:00 | paramChanged | 54 |  | 0.09 | other |  |
| 2026-09-06T00:08:23.315+02:00 | paramChanged | 54 |  | 0.10 | other |  |
| 2026-09-06T00:08:23.336+02:00 | paramChanged | 54 |  | 0.11 | other |  |
| 2026-09-06T00:08:23.336+02:00 | paramChanged | 54 |  | 0.12 | other |  |
| 2026-09-06T00:08:23.345+02:00 | paramChanged | 54 |  | 0.13 | other |  |
| 2026-09-06T00:08:23.355+02:00 | paramChanged | 54 |  | 0.14 | other |  |
| 2026-09-06T00:08:23.376+02:00 | paramChanged | 54 |  | 0.15 | other |  |
| 2026-09-06T00:08:23.376+02:00 | paramChanged | 54 |  | 0.16 | other |  |
| 2026-09-06T00:08:23.386+02:00 | paramChanged | 54 |  | 0.17 | other |  |
| 2026-09-06T00:08:23.396+02:00 | paramChanged | 54 |  | 0.18 | other |  |
| 2026-09-06T00:08:23.415+02:00 | paramChanged | 54 |  | 0.19 | other |  |
| 2026-09-06T00:08:23.416+02:00 | paramChanged | 54 |  | 0.20 | other |  |
| 2026-09-06T00:08:23.425+02:00 | paramChanged | 54 |  | 0.21 | other |  |
| 2026-09-06T00:08:23.435+02:00 | paramChanged | 54 |  | 0.22 | other |  |
| 2026-09-06T00:08:23.456+02:00 | paramChanged | 54 |  | 0.23 | other |  |
| 2026-09-06T00:08:23.456+02:00 | paramChanged | 54 |  | 0.24 | other |  |
| 2026-09-06T00:08:23.465+02:00 | paramChanged | 54 |  | 0.25 | other |  |
| 2026-09-06T00:08:23.476+02:00 | paramChanged | 54 |  | 0.26 | other |  |
| 2026-09-06T00:08:23.496+02:00 | paramChanged | 54 |  | 0.27 | other |  |
| 2026-09-06T00:08:23.497+02:00 | paramChanged | 54 |  | 0.28 | other |  |
| 2026-09-06T00:08:23.506+02:00 | paramChanged | 54 |  | 0.29 | other |  |
| 2026-09-06T00:08:23.515+02:00 | paramChanged | 54 |  | 0.30 | other |  |
| 2026-09-06T00:08:23.536+02:00 | paramChanged | 54 |  | 0.31 | other |  |
| 2026-09-06T00:08:23.536+02:00 | paramChanged | 54 |  | 0.32 | other |  |
| 2026-09-06T00:08:23.545+02:00 | paramChanged | 54 |  | 0.33 | other |  |
| 2026-09-06T00:08:23.555+02:00 | paramChanged | 54 |  | 0.34 | other |  |
| 2026-09-06T00:08:23.576+02:00 | paramChanged | 54 |  | 0.35 | other |  |
| 2026-09-06T00:08:23.576+02:00 | paramChanged | 54 |  | 0.36 | other |  |
| 2026-09-06T00:08:23.585+02:00 | paramChanged | 54 |  | 0.37 | other |  |
| 2026-09-06T00:08:23.595+02:00 | paramChanged | 54 |  | 0.38 | other |  |
| 2026-09-06T00:08:23.615+02:00 | paramChanged | 54 |  | 0.39 | other |  |
| 2026-09-06T00:08:23.616+02:00 | paramChanged | 54 |  | 0.40 | other |  |
| 2026-09-06T00:08:23.625+02:00 | paramChanged | 54 |  | 0.41 | other |  |
| 2026-09-06T00:08:23.635+02:00 | paramChanged | 54 |  | 0.42 | other |  |
| 2026-09-06T00:08:23.655+02:00 | paramChanged | 54 |  | 0.43 | other |  |
| 2026-09-06T00:08:23.656+02:00 | paramChanged | 54 |  | 0.44 | other |  |
| 2026-09-06T00:08:23.665+02:00 | paramChanged | 54 |  | 0.45 | other |  |
| 2026-09-06T00:08:23.675+02:00 | paramChanged | 54 |  | 0.46 | other |  |
| 2026-09-06T00:08:23.696+02:00 | paramChanged | 54 |  | 0.47 | other |  |
| 2026-09-06T00:08:23.696+02:00 | paramChanged | 54 |  | 0.48 | other |  |
| 2026-09-06T00:08:23.705+02:00 | paramChanged | 54 |  | 0.49 | other |  |
| 2026-09-06T00:08:23.715+02:00 | paramChanged | 54 |  | 0.50 | other |  |
| 2026-09-06T00:08:23.735+02:00 | paramChanged | 54 |  | 0.51 | other |  |
| 2026-09-06T00:08:23.736+02:00 | paramChanged | 54 |  | 0.52 | other |  |
| 2026-09-06T00:08:23.746+02:00 | paramChanged | 54 |  | 0.53 | other |  |
| 2026-09-06T00:08:23.755+02:00 | paramChanged | 54 |  | 0.54 | other |  |
| 2026-09-06T00:08:23.775+02:00 | paramChanged | 54 |  | 0.55 | other |  |
| 2026-09-06T00:08:23.776+02:00 | paramChanged | 54 |  | 0.56 | other |  |
| 2026-09-06T00:08:23.785+02:00 | paramChanged | 54 |  | 0.57 | other |  |
| 2026-09-06T00:08:23.795+02:00 | paramChanged | 54 |  | 0.58 | other |  |
| 2026-09-06T00:08:23.815+02:00 | paramChanged | 54 |  | 0.59 | other |  |
| 2026-09-06T00:08:23.815+02:00 | paramChanged | 54 |  | 0.60 | other |  |
| 2026-09-06T00:08:23.826+02:00 | paramChanged | 54 |  | 0.61 | other |  |
| 2026-09-06T00:08:23.835+02:00 | paramChanged | 54 |  | 0.62 | other |  |
| 2026-09-06T00:08:23.856+02:00 | paramChanged | 54 |  | 0.63 | other |  |
| 2026-09-06T00:08:23.856+02:00 | paramChanged | 54 |  | 0.64 | other |  |
| 2026-09-06T00:08:23.866+02:00 | paramChanged | 54 |  | 0.65 | other |  |
| 2026-09-06T00:08:23.876+02:00 | paramChanged | 54 |  | 0.66 | other |  |
| 2026-09-06T00:08:23.896+02:00 | paramChanged | 54 |  | 0.67 | other |  |
| 2026-09-06T00:08:23.896+02:00 | paramChanged | 54 |  | 0.68 | other |  |
| 2026-09-06T00:08:23.905+02:00 | paramChanged | 54 |  | 0.69 | other |  |
| 2026-09-06T00:08:23.915+02:00 | paramChanged | 54 |  | 0.70 | other |  |
| 2026-09-06T00:08:23.935+02:00 | paramChanged | 54 |  | 0.71 | other |  |
| 2026-09-06T00:08:23.935+02:00 | paramChanged | 54 |  | 0.72 | other |  |
| 2026-09-06T00:08:23.946+02:00 | paramChanged | 54 |  | 0.73 | other |  |
| 2026-09-06T00:08:23.956+02:00 | paramChanged | 54 |  | 0.74 | other |  |
| 2026-09-06T00:08:23.975+02:00 | paramChanged | 54 |  | 0.75 | other |  |
| 2026-09-06T00:08:23.975+02:00 | paramChanged | 54 |  | 0.76 | other |  |
| 2026-09-06T00:08:23.985+02:00 | paramChanged | 54 |  | 0.77 | other |  |
| 2026-09-06T00:08:23.996+02:00 | paramChanged | 54 |  | 0.78 | other |  |
| 2026-09-06T00:08:24.016+02:00 | paramChanged | 54 |  | 0.79 | other |  |
| 2026-09-06T00:08:24.016+02:00 | paramChanged | 54 |  | 0.80 | other |  |
| 2026-09-06T00:08:24.025+02:00 | paramChanged | 54 |  | 0.81 | other |  |
| 2026-09-06T00:08:24.035+02:00 | paramChanged | 54 |  | 0.82 | other |  |
| 2026-09-06T00:08:24.055+02:00 | paramChanged | 54 |  | 0.83 | other |  |
| 2026-09-06T00:08:24.056+02:00 | paramChanged | 54 |  | 0.84 | other |  |
| 2026-09-06T00:08:24.066+02:00 | paramChanged | 54 |  | 0.85 | other |  |
| 2026-09-06T00:08:24.076+02:00 | paramChanged | 54 |  | 0.86 | other |  |
| 2026-09-06T00:08:24.096+02:00 | paramChanged | 54 |  | 0.87 | other |  |
| 2026-09-06T00:08:24.096+02:00 | paramChanged | 54 |  | 0.88 | other |  |
| 2026-09-06T00:08:24.105+02:00 | paramChanged | 54 |  | 0.89 | other |  |
| 2026-09-06T00:08:24.115+02:00 | paramChanged | 54 |  | 0.90 | other |  |
| 2026-09-06T00:08:24.135+02:00 | paramChanged | 54 |  | 0.91 | other |  |
| 2026-09-06T00:08:24.136+02:00 | paramChanged | 54 |  | 0.92 | other |  |
| 2026-09-06T00:08:24.145+02:00 | paramChanged | 54 |  | 0.93 | other |  |
| 2026-09-06T00:08:24.156+02:00 | paramChanged | 54 |  | 0.94 | other |  |
| 2026-09-06T00:08:24.176+02:00 | paramChanged | 54 |  | 0.95 | other |  |
| 2026-09-06T00:08:24.177+02:00 | paramChanged | 54 |  | 0.96 | other |  |
| 2026-09-06T00:08:24.185+02:00 | paramChanged | 54 |  | 0.97 | other |  |
| 2026-09-06T00:08:24.196+02:00 | paramChanged | 54 |  | 0.98 | other |  |
| 2026-09-06T00:08:24.215+02:00 | paramChanged | 54 |  | 0.99 | other |  |
| 2026-09-06T00:08:24.216+02:00 | paramChanged | 54 |  | 1.00 | other |  |
| 2026-09-06T00:08:42.767+02:00 | paramChanged | 54 |  | 0.43 | other |  |
| 2026-09-06T00:08:42.786+02:00 | paramChanged | 54 |  | 0.43 | other |  |
| 2026-09-06T00:08:42.795+02:00 | paramChanged | 54 |  | 0.42 | other |  |
| 2026-09-06T00:08:42.816+02:00 | paramChanged | 54 |  | 0.41 | other |  |
| 2026-09-06T00:08:42.816+02:00 | paramChanged | 54 |  | 0.40 | other |  |
| 2026-09-06T00:08:42.825+02:00 | paramChanged | 54 |  | 0.39 | other |  |
| 2026-09-06T00:08:42.856+02:00 | paramChanged | 54 |  | 0.39 | other |  |
| 2026-09-06T00:08:42.856+02:00 | paramChanged | 54 |  | 0.38 | other |  |
| 2026-09-06T00:08:42.865+02:00 | paramChanged | 54 |  | 0.37 | other |  |
| 2026-09-06T00:08:42.876+02:00 | paramChanged | 54 |  | 0.36 | other |  |
| 2026-09-06T00:08:42.896+02:00 | paramChanged | 54 |  | 0.35 | other |  |
| 2026-09-06T00:08:42.906+02:00 | paramChanged | 54 |  | 0.35 | other |  |
| 2026-09-06T00:08:42.915+02:00 | paramChanged | 54 |  | 0.34 | other |  |
| 2026-09-06T00:08:42.936+02:00 | paramChanged | 54 |  | 0.33 | other |  |
| 2026-09-06T00:08:42.936+02:00 | paramChanged | 54 |  | 0.32 | other |  |
| 2026-09-06T00:08:42.946+02:00 | paramChanged | 54 |  | 0.31 | other |  |
| 2026-09-06T00:08:42.955+02:00 | paramChanged | 54 |  | 0.31 | other |  |
| 2026-09-06T00:08:42.976+02:00 | paramChanged | 54 |  | 0.30 | other |  |
| 2026-09-06T00:08:42.985+02:00 | paramChanged | 54 |  | 0.29 | other |  |
| 2026-09-06T00:08:42.995+02:00 | paramChanged | 54 |  | 0.28 | other |  |
| 2026-09-06T00:08:43.015+02:00 | paramChanged | 54 |  | 0.28 | other |  |
| 2026-09-06T00:08:43.016+02:00 | paramChanged | 54 |  | 0.27 | other |  |
| 2026-09-06T00:08:43.035+02:00 | paramChanged | 54 |  | 0.26 | other |  |
| 2026-09-06T00:08:43.055+02:00 | paramChanged | 54 |  | 0.25 | other |  |
| 2026-09-06T00:08:43.056+02:00 | paramChanged | 54 |  | 0.24 | other |  |
| 2026-09-06T00:08:43.065+02:00 | paramChanged | 54 |  | 0.24 | other |  |
| 2026-09-06T00:08:43.076+02:00 | paramChanged | 54 |  | 0.23 | other |  |
| 2026-09-06T00:08:43.096+02:00 | paramChanged | 54 |  | 0.22 | other |  |
| 2026-09-06T00:08:43.105+02:00 | paramChanged | 54 |  | 0.21 | other |  |
| 2026-09-06T00:08:43.115+02:00 | paramChanged | 54 |  | 0.20 | other |  |
| 2026-09-06T00:08:43.136+02:00 | paramChanged | 54 |  | 0.20 | other |  |
| 2026-09-06T00:08:43.137+02:00 | paramChanged | 54 |  | 0.19 | other |  |
| 2026-09-06T00:08:43.156+02:00 | paramChanged | 54 |  | 0.18 | other |  |
| 2026-09-06T00:08:43.175+02:00 | paramChanged | 54 |  | 0.17 | other |  |
| 2026-09-06T00:08:43.176+02:00 | paramChanged | 54 |  | 0.17 | other |  |
| 2026-09-06T00:08:43.186+02:00 | paramChanged | 54 |  | 0.16 | other |  |
| 2026-09-06T00:08:43.196+02:00 | paramChanged | 54 |  | 0.15 | other |  |
| 2026-09-06T00:08:43.215+02:00 | paramChanged | 54 |  | 0.14 | other |  |
| 2026-09-06T00:08:43.225+02:00 | paramChanged | 54 |  | 0.13 | other |  |
| 2026-09-06T00:08:43.235+02:00 | paramChanged | 54 |  | 0.13 | other |  |
| 2026-09-06T00:08:43.256+02:00 | paramChanged | 54 |  | 0.12 | other |  |
| 2026-09-06T00:08:43.256+02:00 | paramChanged | 54 |  | 0.11 | other |  |
| 2026-09-06T00:08:43.265+02:00 | paramChanged | 54 |  | 0.10 | other |  |
| 2026-09-06T00:08:43.296+02:00 | paramChanged | 54 |  | 0.09 | other |  |
| 2026-09-06T00:08:43.297+02:00 | paramChanged | 54 |  | 0.09 | other |  |
| 2026-09-06T00:08:43.306+02:00 | paramChanged | 54 |  | 0.08 | other |  |
| 2026-09-06T00:08:43.316+02:00 | paramChanged | 54 |  | 0.07 | other |  |
| 2026-09-06T00:08:43.335+02:00 | paramChanged | 54 |  | 0.06 | other |  |
| 2026-09-06T00:08:43.345+02:00 | paramChanged | 54 |  | 0.06 | other |  |
| 2026-09-06T00:08:43.356+02:00 | paramChanged | 54 |  | 0.05 | other |  |
| 2026-09-06T00:08:43.375+02:00 | paramChanged | 54 |  | 0.04 | other |  |
| 2026-09-06T00:08:43.376+02:00 | paramChanged | 54 |  | 0.03 | other |  |
| 2026-09-06T00:08:43.385+02:00 | paramChanged | 54 |  | 0.02 | other |  |
| 2026-09-06T00:08:43.416+02:00 | paramChanged | 54 |  | 0.02 | other |  |
| 2026-09-06T00:08:43.417+02:00 | paramChanged | 54 |  | 0.01 | other |  |
| 2026-09-06T00:08:43.426+02:00 | paramChanged | 54 |  | 0.00 | other |  |
| 2026-09-06T00:08:43.436+02:00 | paramChanged | 54 |  | 0.01 | other |  |
| 2026-09-06T00:08:43.456+02:00 | paramChanged | 54 |  | 0.02 | other |  |
| 2026-09-06T00:08:43.456+02:00 | paramChanged | 54 |  | 0.03 | other |  |
| 2026-09-06T00:08:43.466+02:00 | paramChanged | 54 |  | 0.04 | other |  |
| 2026-09-06T00:08:43.475+02:00 | paramChanged | 54 |  | 0.05 | other |  |
| 2026-09-06T00:08:43.495+02:00 | paramChanged | 54 |  | 0.06 | other |  |
| 2026-09-06T00:08:43.495+02:00 | paramChanged | 54 |  | 0.07 | other |  |
| 2026-09-06T00:08:43.505+02:00 | paramChanged | 54 |  | 0.08 | other |  |
| 2026-09-06T00:08:43.515+02:00 | paramChanged | 54 |  | 0.09 | other |  |
| 2026-09-06T00:08:43.536+02:00 | paramChanged | 54 |  | 0.10 | other |  |
| 2026-09-06T00:08:43.536+02:00 | paramChanged | 54 |  | 0.11 | other |  |
| 2026-09-06T00:08:43.545+02:00 | paramChanged | 54 |  | 0.12 | other |  |
| 2026-09-06T00:08:43.555+02:00 | paramChanged | 54 |  | 0.13 | other |  |
| 2026-09-06T00:08:43.576+02:00 | paramChanged | 54 |  | 0.14 | other |  |
| 2026-09-06T00:08:43.577+02:00 | paramChanged | 54 |  | 0.15 | other |  |
| 2026-09-06T00:08:43.586+02:00 | paramChanged | 54 |  | 0.16 | other |  |
| 2026-09-06T00:08:43.595+02:00 | paramChanged | 54 |  | 0.17 | other |  |
| 2026-09-06T00:08:43.616+02:00 | paramChanged | 54 |  | 0.18 | other |  |
| 2026-09-06T00:08:43.616+02:00 | paramChanged | 54 |  | 0.19 | other |  |
| 2026-09-06T00:08:43.626+02:00 | paramChanged | 54 |  | 0.20 | other |  |
| 2026-09-06T00:08:43.635+02:00 | paramChanged | 54 |  | 0.21 | other |  |
| 2026-09-06T00:08:43.656+02:00 | paramChanged | 54 |  | 0.22 | other |  |
| 2026-09-06T00:08:43.657+02:00 | paramChanged | 54 |  | 0.23 | other |  |
| 2026-09-06T00:08:43.665+02:00 | paramChanged | 54 |  | 0.24 | other |  |
| 2026-09-06T00:08:43.676+02:00 | paramChanged | 54 |  | 0.25 | other |  |
| 2026-09-06T00:08:43.696+02:00 | paramChanged | 54 |  | 0.26 | other |  |
| 2026-09-06T00:08:43.697+02:00 | paramChanged | 54 |  | 0.27 | other |  |
| 2026-09-06T00:08:43.705+02:00 | paramChanged | 54 |  | 0.28 | other |  |
| 2026-09-06T00:08:43.715+02:00 | paramChanged | 54 |  | 0.29 | other |  |
| 2026-09-06T00:08:43.735+02:00 | paramChanged | 54 |  | 0.30 | other |  |
| 2026-09-06T00:08:43.736+02:00 | paramChanged | 54 |  | 0.31 | other |  |
| 2026-09-06T00:08:43.745+02:00 | paramChanged | 54 |  | 0.32 | other |  |
| 2026-09-06T00:08:43.756+02:00 | paramChanged | 54 |  | 0.33 | other |  |
| 2026-09-06T00:08:43.776+02:00 | paramChanged | 54 |  | 0.34 | other |  |
| 2026-09-06T00:08:43.777+02:00 | paramChanged | 54 |  | 0.35 | other |  |
| 2026-09-06T00:08:43.786+02:00 | paramChanged | 54 |  | 0.36 | other |  |
| 2026-09-06T00:08:43.795+02:00 | paramChanged | 54 |  | 0.37 | other |  |
| 2026-09-06T00:08:43.815+02:00 | paramChanged | 54 |  | 0.38 | other |  |
| 2026-09-06T00:08:43.816+02:00 | paramChanged | 54 |  | 0.39 | other |  |
| 2026-09-06T00:08:43.825+02:00 | paramChanged | 54 |  | 0.40 | other |  |
| 2026-09-06T00:08:43.836+02:00 | paramChanged | 54 |  | 0.41 | other |  |
| 2026-09-06T00:08:43.855+02:00 | paramChanged | 54 |  | 0.42 | other |  |
| 2026-09-06T00:08:43.856+02:00 | paramChanged | 54 |  | 0.43 | other |  |
| 2026-09-06T00:08:43.865+02:00 | paramChanged | 54 |  | 0.44 | other |  |
| 2026-09-06T00:08:43.876+02:00 | paramChanged | 54 |  | 0.45 | other |  |
| 2026-09-06T00:08:43.896+02:00 | paramChanged | 54 |  | 0.46 | other |  |
| 2026-09-06T00:08:43.897+02:00 | paramChanged | 54 |  | 0.47 | other |  |
| 2026-09-06T00:08:43.905+02:00 | paramChanged | 54 |  | 0.48 | other |  |
| 2026-09-06T00:08:43.916+02:00 | paramChanged | 54 |  | 0.49 | other |  |
| 2026-09-06T00:08:43.935+02:00 | paramChanged | 54 |  | 0.50 | other |  |
| 2026-09-06T00:08:43.936+02:00 | paramChanged | 54 |  | 0.51 | other |  |
| 2026-09-06T00:08:43.945+02:00 | paramChanged | 54 |  | 0.52 | other |  |
| 2026-09-06T00:08:43.956+02:00 | paramChanged | 54 |  | 0.53 | other |  |
| 2026-09-06T00:08:43.976+02:00 | paramChanged | 54 |  | 0.54 | other |  |
| 2026-09-06T00:08:43.977+02:00 | paramChanged | 54 |  | 0.55 | other |  |
| 2026-09-06T00:08:43.985+02:00 | paramChanged | 54 |  | 0.56 | other |  |
| 2026-09-06T00:08:43.995+02:00 | paramChanged | 54 |  | 0.57 | other |  |
| 2026-09-06T00:08:44.016+02:00 | paramChanged | 54 |  | 0.58 | other |  |
| 2026-09-06T00:08:44.017+02:00 | paramChanged | 54 |  | 0.59 | other |  |
| 2026-09-06T00:08:44.025+02:00 | paramChanged | 54 |  | 0.60 | other |  |
| 2026-09-06T00:08:44.035+02:00 | paramChanged | 54 |  | 0.61 | other |  |
| 2026-09-06T00:08:44.056+02:00 | paramChanged | 54 |  | 0.62 | other |  |
| 2026-09-06T00:08:44.056+02:00 | paramChanged | 54 |  | 0.63 | other |  |
| 2026-09-06T00:08:44.065+02:00 | paramChanged | 54 |  | 0.64 | other |  |
| 2026-09-06T00:08:44.075+02:00 | paramChanged | 54 |  | 0.65 | other |  |
| 2026-09-06T00:08:44.095+02:00 | paramChanged | 54 |  | 0.66 | other |  |
| 2026-09-06T00:08:44.095+02:00 | paramChanged | 54 |  | 0.67 | other |  |
| 2026-09-06T00:08:44.105+02:00 | paramChanged | 54 |  | 0.68 | other |  |
| 2026-09-06T00:08:44.115+02:00 | paramChanged | 54 |  | 0.69 | other |  |
| 2026-09-06T00:08:44.135+02:00 | paramChanged | 54 |  | 0.70 | other |  |
| 2026-09-06T00:08:44.136+02:00 | paramChanged | 54 |  | 0.71 | other |  |
| 2026-09-06T00:08:44.145+02:00 | paramChanged | 54 |  | 0.72 | other |  |
| 2026-09-06T00:08:44.156+02:00 | paramChanged | 54 |  | 0.73 | other |  |
| 2026-09-06T00:08:44.175+02:00 | paramChanged | 54 |  | 0.74 | other |  |
| 2026-09-06T00:08:44.176+02:00 | paramChanged | 54 |  | 0.75 | other |  |
| 2026-09-06T00:08:44.185+02:00 | paramChanged | 54 |  | 0.76 | other |  |
| 2026-09-06T00:08:44.196+02:00 | paramChanged | 54 |  | 0.77 | other |  |
| 2026-09-06T00:08:44.216+02:00 | paramChanged | 54 |  | 0.78 | other |  |
| 2026-09-06T00:08:44.217+02:00 | paramChanged | 54 |  | 0.79 | other |  |
| 2026-09-06T00:08:44.225+02:00 | paramChanged | 54 |  | 0.80 | other |  |
| 2026-09-06T00:08:44.235+02:00 | paramChanged | 54 |  | 0.81 | other |  |
| 2026-09-06T00:08:44.255+02:00 | paramChanged | 54 |  | 0.82 | other |  |
| 2026-09-06T00:08:44.256+02:00 | paramChanged | 54 |  | 0.83 | other |  |
| 2026-09-06T00:08:44.265+02:00 | paramChanged | 54 |  | 0.84 | other |  |
| 2026-09-06T00:08:44.276+02:00 | paramChanged | 54 |  | 0.85 | other |  |
| 2026-09-06T00:08:44.296+02:00 | paramChanged | 54 |  | 0.86 | other |  |
| 2026-09-06T00:08:44.297+02:00 | paramChanged | 54 |  | 0.87 | other |  |
| 2026-09-06T00:08:44.305+02:00 | paramChanged | 54 |  | 0.88 | other |  |
| 2026-09-06T00:08:44.315+02:00 | paramChanged | 54 |  | 0.89 | other |  |
| 2026-09-06T00:08:44.336+02:00 | paramChanged | 54 |  | 0.90 | other |  |
| 2026-09-06T00:08:44.336+02:00 | paramChanged | 54 |  | 0.91 | other |  |
| 2026-09-06T00:08:44.346+02:00 | paramChanged | 54 |  | 0.92 | other |  |
| 2026-09-06T00:08:44.355+02:00 | paramChanged | 54 |  | 0.93 | other |  |
| 2026-09-06T00:08:44.375+02:00 | paramChanged | 54 |  | 0.94 | other |  |
| 2026-09-06T00:08:44.375+02:00 | paramChanged | 54 |  | 0.95 | other |  |
| 2026-09-06T00:08:44.385+02:00 | paramChanged | 54 |  | 0.96 | other |  |
| 2026-09-06T00:08:44.395+02:00 | paramChanged | 54 |  | 0.97 | other |  |
| 2026-09-06T00:08:44.416+02:00 | paramChanged | 54 |  | 0.98 | other |  |
| 2026-09-06T00:08:44.417+02:00 | paramChanged | 54 |  | 0.99 | other |  |
| 2026-09-06T00:08:44.425+02:00 | paramChanged | 54 |  | 1.00 | other |  |
| 2026-09-06T00:08:48.775+02:00 | paramChanged | 54 |  | 0.43 | other |  |
| 2026-09-06T00:08:48.786+02:00 | paramChanged | 54 |  | 0.43 | other |  |
| 2026-09-06T00:08:48.797+02:00 | paramChanged | 54 |  | 0.42 | other |  |
| 2026-09-06T00:08:48.816+02:00 | paramChanged | 54 |  | 0.41 | other |  |
| 2026-09-06T00:08:48.817+02:00 | paramChanged | 54 |  | 0.40 | other |  |
| 2026-09-06T00:08:48.825+02:00 | paramChanged | 54 |  | 0.39 | other |  |
| 2026-09-06T00:08:48.856+02:00 | paramChanged | 54 |  | 0.39 | other |  |
| 2026-09-06T00:08:48.856+02:00 | paramChanged | 54 |  | 0.38 | other |  |
| 2026-09-06T00:08:48.866+02:00 | paramChanged | 54 |  | 0.37 | other |  |
| 2026-09-06T00:08:48.875+02:00 | paramChanged | 54 |  | 0.36 | other |  |
| 2026-09-06T00:08:48.886+02:00 | paramChanged | 54 |  | 0.35 | other |  |
| 2026-09-06T00:08:48.906+02:00 | paramChanged | 54 |  | 0.35 | other |  |
| 2026-09-06T00:08:48.916+02:00 | paramChanged | 54 |  | 0.34 | other |  |
| 2026-09-06T00:08:48.936+02:00 | paramChanged | 54 |  | 0.33 | other |  |
| 2026-09-06T00:08:48.937+02:00 | paramChanged | 54 |  | 0.32 | other |  |
| 2026-09-06T00:08:48.945+02:00 | paramChanged | 54 |  | 0.31 | other |  |
| 2026-09-06T00:08:48.956+02:00 | paramChanged | 54 |  | 0.31 | other |  |
| 2026-09-06T00:08:48.976+02:00 | paramChanged | 54 |  | 0.30 | other |  |
| 2026-09-06T00:08:48.986+02:00 | paramChanged | 54 |  | 0.29 | other |  |
| 2026-09-06T00:08:48.996+02:00 | paramChanged | 54 |  | 0.28 | other |  |
| 2026-09-06T00:08:49.015+02:00 | paramChanged | 54 |  | 0.28 | other |  |
| 2026-09-06T00:08:49.016+02:00 | paramChanged | 54 |  | 0.27 | other |  |
| 2026-09-06T00:08:49.036+02:00 | paramChanged | 54 |  | 0.26 | other |  |
| 2026-09-06T00:08:49.056+02:00 | paramChanged | 54 |  | 0.25 | other |  |
| 2026-09-06T00:08:49.056+02:00 | paramChanged | 54 |  | 0.24 | other |  |
| 2026-09-06T00:08:49.066+02:00 | paramChanged | 54 |  | 0.24 | other |  |
| 2026-09-06T00:08:49.076+02:00 | paramChanged | 54 |  | 0.23 | other |  |
| 2026-09-06T00:08:49.097+02:00 | paramChanged | 54 |  | 0.22 | other |  |
| 2026-09-06T00:08:49.105+02:00 | paramChanged | 54 |  | 0.21 | other |  |
| 2026-09-06T00:08:49.116+02:00 | paramChanged | 54 |  | 0.20 | other |  |
| 2026-09-06T00:08:49.136+02:00 | paramChanged | 54 |  | 0.20 | other |  |
| 2026-09-06T00:08:49.137+02:00 | paramChanged | 54 |  | 0.19 | other |  |
| 2026-09-06T00:08:49.156+02:00 | paramChanged | 54 |  | 0.18 | other |  |
| 2026-09-06T00:08:49.175+02:00 | paramChanged | 54 |  | 0.17 | other |  |
| 2026-09-06T00:08:49.176+02:00 | paramChanged | 54 |  | 0.17 | other |  |
| 2026-09-06T00:08:49.186+02:00 | paramChanged | 54 |  | 0.16 | other |  |
| 2026-09-06T00:08:49.196+02:00 | paramChanged | 54 |  | 0.15 | other |  |
| 2026-09-06T00:08:49.216+02:00 | paramChanged | 54 |  | 0.14 | other |  |
| 2026-09-06T00:08:49.226+02:00 | paramChanged | 54 |  | 0.13 | other |  |
| 2026-09-06T00:08:49.236+02:00 | paramChanged | 54 |  | 0.13 | other |  |
| 2026-09-06T00:08:49.256+02:00 | paramChanged | 54 |  | 0.12 | other |  |
| 2026-09-06T00:08:49.256+02:00 | paramChanged | 54 |  | 0.11 | other |  |
| 2026-09-06T00:08:49.266+02:00 | paramChanged | 54 |  | 0.10 | other |  |
| 2026-09-06T00:08:49.295+02:00 | paramChanged | 54 |  | 0.09 | other |  |
| 2026-09-06T00:08:49.296+02:00 | paramChanged | 54 |  | 0.09 | other |  |
| 2026-09-06T00:08:49.306+02:00 | paramChanged | 54 |  | 0.08 | other |  |
| 2026-09-06T00:08:49.315+02:00 | paramChanged | 54 |  | 0.07 | other |  |
| 2026-09-06T00:08:49.335+02:00 | paramChanged | 54 |  | 0.06 | other |  |
| 2026-09-06T00:08:49.346+02:00 | paramChanged | 54 |  | 0.06 | other |  |
| 2026-09-06T00:08:49.355+02:00 | paramChanged | 54 |  | 0.05 | other |  |
| 2026-09-06T00:08:49.366+02:00 | paramChanged | 54 |  | 0.04 | other |  |
| 2026-09-06T00:08:49.376+02:00 | paramChanged | 54 |  | 0.03 | other |  |
| 2026-09-06T00:08:49.386+02:00 | paramChanged | 54 |  | 0.02 | other |  |
| 2026-09-06T00:08:49.406+02:00 | paramChanged | 54 |  | 0.02 | other |  |
| 2026-09-06T00:08:49.415+02:00 | paramChanged | 54 |  | 0.01 | other |  |
| 2026-09-06T00:08:49.426+02:00 | paramChanged | 54 |  | 0.00 | other |  |
| 2026-09-06T00:08:49.436+02:00 | paramChanged | 54 |  | 0.01 | other |  |

## Raw sample log (92)

| Time | Phase | Path | ms | bytes | editor | transport | thread | sha256 |
|---|---|---|---|---|---|---|---|---|
| 2026-09-06T00:08:23.430+02:00 | M2-play | raw-getStateInformation | 0.53 | 10412 | closed | playing | message | `4511715dd0796376e8809d9904157998fbfc8865141cc2dbeebab749c15f363a` |
| 2026-09-06T00:08:23.433+02:00 | M2-play | raw-getStateInformation | 0.55 | 10412 | closed | playing | message | `4511715dd0796376e8809d9904157998fbfc8865141cc2dbeebab749c15f363a` |
| 2026-09-06T00:08:23.436+02:00 | M2-play | raw-getStateInformation | 0.47 | 10413 | closed | playing | message | `006605833a0925eeb81cdff5fb79b72f68122959d42c483ab97ebe9526bd17b8` |
| 2026-09-06T00:08:23.439+02:00 | M2-play | raw-getStateInformation | 0.49 | 10413 | closed | playing | message | `006605833a0925eeb81cdff5fb79b72f68122959d42c483ab97ebe9526bd17b8` |
| 2026-09-06T00:08:23.442+02:00 | M2-play | raw-getStateInformation | 0.47 | 10413 | closed | playing | message | `006605833a0925eeb81cdff5fb79b72f68122959d42c483ab97ebe9526bd17b8` |
| 2026-09-06T00:08:23.445+02:00 | M2-play | raw-getStateInformation | 0.46 | 10413 | closed | playing | message | `006605833a0925eeb81cdff5fb79b72f68122959d42c483ab97ebe9526bd17b8` |
| 2026-09-06T00:08:23.448+02:00 | M2-play | raw-getStateInformation | 0.47 | 10413 | closed | playing | message | `006605833a0925eeb81cdff5fb79b72f68122959d42c483ab97ebe9526bd17b8` |
| 2026-09-06T00:08:23.451+02:00 | M2-play | raw-getStateInformation | 0.48 | 10413 | closed | playing | message | `006605833a0925eeb81cdff5fb79b72f68122959d42c483ab97ebe9526bd17b8` |
| 2026-09-06T00:08:23.454+02:00 | M2-play | raw-getStateInformation | 0.48 | 10413 | closed | playing | message | `006605833a0925eeb81cdff5fb79b72f68122959d42c483ab97ebe9526bd17b8` |
| 2026-09-06T00:08:23.456+02:00 | M2-play | raw-getStateInformation | 0.47 | 10413 | closed | playing | message | `a7f57a0b5cd8524a3b85306169a87cb219e8dc08955bfda3329fd9548e6bd6e6` |
| 2026-09-06T00:08:27.426+02:00 | M2-play | raw-getStateInformation | 0.55 | 10393 | closed | playing | message | `875dc964caa630252767e6b26bf8a54ab2d64a2fc93a6112174924e0c3633aee` |
| 2026-09-06T00:08:27.431+02:00 | M2-play | raw-getStateInformation | 0.54 | 10393 | closed | playing | message | `875dc964caa630252767e6b26bf8a54ab2d64a2fc93a6112174924e0c3633aee` |
| 2026-09-06T00:08:27.435+02:00 | M2-play | raw-getStateInformation | 1.42 | 10393 | closed | playing | message | `875dc964caa630252767e6b26bf8a54ab2d64a2fc93a6112174924e0c3633aee` |
| 2026-09-06T00:08:27.440+02:00 | M2-play | raw-getStateInformation | 0.55 | 10393 | closed | playing | message | `875dc964caa630252767e6b26bf8a54ab2d64a2fc93a6112174924e0c3633aee` |
| 2026-09-06T00:08:27.444+02:00 | M2-play | raw-getStateInformation | 0.48 | 10393 | closed | playing | message | `875dc964caa630252767e6b26bf8a54ab2d64a2fc93a6112174924e0c3633aee` |
| 2026-09-06T00:08:27.446+02:00 | M2-play | raw-getStateInformation | 0.45 | 10393 | closed | playing | message | `875dc964caa630252767e6b26bf8a54ab2d64a2fc93a6112174924e0c3633aee` |
| 2026-09-06T00:08:27.449+02:00 | M2-play | raw-getStateInformation | 0.65 | 10393 | closed | playing | message | `875dc964caa630252767e6b26bf8a54ab2d64a2fc93a6112174924e0c3633aee` |
| 2026-09-06T00:08:27.454+02:00 | M2-play | raw-getStateInformation | 0.46 | 10393 | closed | playing | message | `875dc964caa630252767e6b26bf8a54ab2d64a2fc93a6112174924e0c3633aee` |
| 2026-09-06T00:08:27.458+02:00 | M2-play | raw-getStateInformation | 0.52 | 10393 | closed | playing | message | `875dc964caa630252767e6b26bf8a54ab2d64a2fc93a6112174924e0c3633aee` |
| 2026-09-06T00:08:27.463+02:00 | M2-play | raw-getStateInformation | 0.44 | 10393 | closed | playing | message | `875dc964caa630252767e6b26bf8a54ab2d64a2fc93a6112174924e0c3633aee` |
| 2026-09-06T00:08:28.438+02:00 | M2-stop | raw-getStateInformation | 0.53 | 10393 | closed | stopped | message | `875dc964caa630252767e6b26bf8a54ab2d64a2fc93a6112174924e0c3633aee` |
| 2026-09-06T00:08:28.578+02:00 | M2-stop | raw-getStateInformation | 0.52 | 10393 | closed | stopped | message | `875dc964caa630252767e6b26bf8a54ab2d64a2fc93a6112174924e0c3633aee` |
| 2026-09-06T00:08:28.680+02:00 | M2-stop | raw-getStateInformation | 0.80 | 10393 | closed | stopped | message | `875dc964caa630252767e6b26bf8a54ab2d64a2fc93a6112174924e0c3633aee` |
| 2026-09-06T00:08:28.931+02:00 | M2-stop | raw-getStateInformation | 0.56 | 10393 | closed | stopped | message | `875dc964caa630252767e6b26bf8a54ab2d64a2fc93a6112174924e0c3633aee` |
| 2026-09-06T00:08:29.431+02:00 | M2-stop | raw-getStateInformation | 0.54 | 10393 | closed | stopped | message | `875dc964caa630252767e6b26bf8a54ab2d64a2fc93a6112174924e0c3633aee` |
| 2026-09-06T00:08:30.431+02:00 | M2-stop | raw-getStateInformation | 0.54 | 10393 | closed | stopped | message | `875dc964caa630252767e6b26bf8a54ab2d64a2fc93a6112174924e0c3633aee` |
| 2026-09-06T00:08:33.434+02:00 | M2-late | raw-getStateInformation | 0.54 | 10393 | closed | stopped | message | `875dc964caa630252767e6b26bf8a54ab2d64a2fc93a6112174924e0c3633aee` |
| 2026-09-06T00:08:33.438+02:00 | M2-late | raw-getStateInformation | 0.52 | 10393 | closed | stopped | message | `875dc964caa630252767e6b26bf8a54ab2d64a2fc93a6112174924e0c3633aee` |
| 2026-09-06T00:08:33.444+02:00 | M2-late | raw-getStateInformation | 1.11 | 10393 | closed | stopped | message | `875dc964caa630252767e6b26bf8a54ab2d64a2fc93a6112174924e0c3633aee` |
| 2026-09-06T00:08:33.450+02:00 | M2-late | raw-getStateInformation | 0.50 | 10393 | closed | stopped | message | `875dc964caa630252767e6b26bf8a54ab2d64a2fc93a6112174924e0c3633aee` |
| 2026-09-06T00:08:33.453+02:00 | M2-late | raw-getStateInformation | 0.43 | 10393 | closed | stopped | message | `875dc964caa630252767e6b26bf8a54ab2d64a2fc93a6112174924e0c3633aee` |
| 2026-09-06T00:08:33.456+02:00 | M2-late | raw-getStateInformation | 0.60 | 10393 | closed | stopped | message | `875dc964caa630252767e6b26bf8a54ab2d64a2fc93a6112174924e0c3633aee` |
| 2026-09-06T00:08:33.460+02:00 | M2-late | raw-getStateInformation | 0.47 | 10393 | closed | stopped | message | `875dc964caa630252767e6b26bf8a54ab2d64a2fc93a6112174924e0c3633aee` |
| 2026-09-06T00:08:33.463+02:00 | M2-late | raw-getStateInformation | 0.50 | 10393 | closed | stopped | message | `875dc964caa630252767e6b26bf8a54ab2d64a2fc93a6112174924e0c3633aee` |
| 2026-09-06T00:08:33.467+02:00 | M2-late | raw-getStateInformation | 0.48 | 10393 | closed | stopped | message | `875dc964caa630252767e6b26bf8a54ab2d64a2fc93a6112174924e0c3633aee` |
| 2026-09-06T00:08:33.469+02:00 | M2-late | raw-getStateInformation | 0.63 | 10393 | closed | stopped | message | `875dc964caa630252767e6b26bf8a54ab2d64a2fc93a6112174924e0c3633aee` |
| 2026-09-06T00:08:38.433+02:00 | M2-late | raw-getStateInformation | 1.18 | 10393 | closed | stopped | message | `875dc964caa630252767e6b26bf8a54ab2d64a2fc93a6112174924e0c3633aee` |
| 2026-09-06T00:08:38.439+02:00 | M2-late | raw-getStateInformation | 0.52 | 10393 | closed | stopped | message | `875dc964caa630252767e6b26bf8a54ab2d64a2fc93a6112174924e0c3633aee` |
| 2026-09-06T00:08:38.442+02:00 | M2-late | raw-getStateInformation | 0.47 | 10393 | closed | stopped | message | `875dc964caa630252767e6b26bf8a54ab2d64a2fc93a6112174924e0c3633aee` |
| 2026-09-06T00:08:38.445+02:00 | M2-late | raw-getStateInformation | 0.45 | 10393 | closed | stopped | message | `875dc964caa630252767e6b26bf8a54ab2d64a2fc93a6112174924e0c3633aee` |
| 2026-09-06T00:08:38.448+02:00 | M2-late | raw-getStateInformation | 0.46 | 10393 | closed | stopped | message | `875dc964caa630252767e6b26bf8a54ab2d64a2fc93a6112174924e0c3633aee` |
| 2026-09-06T00:08:38.451+02:00 | M2-late | raw-getStateInformation | 0.44 | 10393 | closed | stopped | message | `875dc964caa630252767e6b26bf8a54ab2d64a2fc93a6112174924e0c3633aee` |
| 2026-09-06T00:08:38.453+02:00 | M2-late | raw-getStateInformation | 0.44 | 10393 | closed | stopped | message | `875dc964caa630252767e6b26bf8a54ab2d64a2fc93a6112174924e0c3633aee` |
| 2026-09-06T00:08:38.456+02:00 | M2-late | raw-getStateInformation | 0.46 | 10393 | closed | stopped | message | `875dc964caa630252767e6b26bf8a54ab2d64a2fc93a6112174924e0c3633aee` |
| 2026-09-06T00:08:38.458+02:00 | M2-late | raw-getStateInformation | 0.45 | 10393 | closed | stopped | message | `875dc964caa630252767e6b26bf8a54ab2d64a2fc93a6112174924e0c3633aee` |
| 2026-09-06T00:08:38.462+02:00 | M2-late | raw-getStateInformation | 0.49 | 10393 | closed | stopped | message | `875dc964caa630252767e6b26bf8a54ab2d64a2fc93a6112174924e0c3633aee` |
| 2026-09-06T00:08:44.439+02:00 | M2-play | raw-getStateInformation | 0.54 | 10393 | closed | playing | message | `875dc964caa630252767e6b26bf8a54ab2d64a2fc93a6112174924e0c3633aee` |
| 2026-09-06T00:08:44.442+02:00 | M2-play | raw-getStateInformation | 0.47 | 10393 | closed | playing | message | `875dc964caa630252767e6b26bf8a54ab2d64a2fc93a6112174924e0c3633aee` |
| 2026-09-06T00:08:44.445+02:00 | M2-play | raw-getStateInformation | 0.44 | 10393 | closed | playing | message | `875dc964caa630252767e6b26bf8a54ab2d64a2fc93a6112174924e0c3633aee` |
| 2026-09-06T00:08:44.448+02:00 | M2-play | raw-getStateInformation | 0.48 | 10393 | closed | playing | message | `875dc964caa630252767e6b26bf8a54ab2d64a2fc93a6112174924e0c3633aee` |
| 2026-09-06T00:08:44.450+02:00 | M2-play | raw-getStateInformation | 0.44 | 10393 | closed | playing | message | `875dc964caa630252767e6b26bf8a54ab2d64a2fc93a6112174924e0c3633aee` |
| 2026-09-06T00:08:44.453+02:00 | M2-play | raw-getStateInformation | 0.48 | 10393 | closed | playing | message | `875dc964caa630252767e6b26bf8a54ab2d64a2fc93a6112174924e0c3633aee` |
| 2026-09-06T00:08:44.455+02:00 | M2-play | raw-getStateInformation | 0.43 | 10393 | closed | playing | message | `875dc964caa630252767e6b26bf8a54ab2d64a2fc93a6112174924e0c3633aee` |
| 2026-09-06T00:08:44.458+02:00 | M2-play | raw-getStateInformation | 0.42 | 10393 | closed | playing | message | `875dc964caa630252767e6b26bf8a54ab2d64a2fc93a6112174924e0c3633aee` |
| 2026-09-06T00:08:44.461+02:00 | M2-play | raw-getStateInformation | 0.45 | 10393 | closed | playing | message | `875dc964caa630252767e6b26bf8a54ab2d64a2fc93a6112174924e0c3633aee` |
| 2026-09-06T00:08:44.464+02:00 | M2-play | raw-getStateInformation | 0.47 | 10393 | closed | playing | message | `875dc964caa630252767e6b26bf8a54ab2d64a2fc93a6112174924e0c3633aee` |
| 2026-09-06T00:08:48.437+02:00 | M2-play | raw-getStateInformation | 0.53 | 10393 | closed | playing | message | `875dc964caa630252767e6b26bf8a54ab2d64a2fc93a6112174924e0c3633aee` |
| 2026-09-06T00:08:48.443+02:00 | M2-play | raw-getStateInformation | 0.49 | 10393 | closed | playing | message | `875dc964caa630252767e6b26bf8a54ab2d64a2fc93a6112174924e0c3633aee` |
| 2026-09-06T00:08:48.446+02:00 | M2-play | raw-getStateInformation | 0.46 | 10393 | closed | playing | message | `875dc964caa630252767e6b26bf8a54ab2d64a2fc93a6112174924e0c3633aee` |
| 2026-09-06T00:08:48.449+02:00 | M2-play | raw-getStateInformation | 0.45 | 10393 | closed | playing | message | `875dc964caa630252767e6b26bf8a54ab2d64a2fc93a6112174924e0c3633aee` |
| 2026-09-06T00:08:48.452+02:00 | M2-play | raw-getStateInformation | 0.58 | 10393 | closed | playing | message | `875dc964caa630252767e6b26bf8a54ab2d64a2fc93a6112174924e0c3633aee` |
| 2026-09-06T00:08:48.455+02:00 | M2-play | raw-getStateInformation | 0.44 | 10393 | closed | playing | message | `875dc964caa630252767e6b26bf8a54ab2d64a2fc93a6112174924e0c3633aee` |
| 2026-09-06T00:08:48.458+02:00 | M2-play | raw-getStateInformation | 0.49 | 10393 | closed | playing | message | `875dc964caa630252767e6b26bf8a54ab2d64a2fc93a6112174924e0c3633aee` |
| 2026-09-06T00:08:48.461+02:00 | M2-play | raw-getStateInformation | 0.46 | 10393 | closed | playing | message | `875dc964caa630252767e6b26bf8a54ab2d64a2fc93a6112174924e0c3633aee` |
| 2026-09-06T00:08:48.463+02:00 | M2-play | raw-getStateInformation | 0.44 | 10393 | closed | playing | message | `875dc964caa630252767e6b26bf8a54ab2d64a2fc93a6112174924e0c3633aee` |
| 2026-09-06T00:08:48.467+02:00 | M2-play | raw-getStateInformation | 0.60 | 10393 | closed | playing | message | `875dc964caa630252767e6b26bf8a54ab2d64a2fc93a6112174924e0c3633aee` |
| 2026-09-06T00:08:49.443+02:00 | M2-stop | raw-getStateInformation | 0.53 | 10415 | closed | stopped | message | `55c9edcfc93fd3049e48621a1b3fa9881fcb750b7ed4390a1e11f6975a87feac` |
| 2026-09-06T00:08:49.554+02:00 | M2-stop | raw-getStateInformation | 0.53 | 10415 | closed | stopped | message | `55c9edcfc93fd3049e48621a1b3fa9881fcb750b7ed4390a1e11f6975a87feac` |
| 2026-09-06T00:08:49.705+02:00 | M2-stop | raw-getStateInformation | 0.56 | 10415 | closed | stopped | message | `55c9edcfc93fd3049e48621a1b3fa9881fcb750b7ed4390a1e11f6975a87feac` |
| 2026-09-06T00:08:49.959+02:00 | M2-stop | raw-getStateInformation | 0.70 | 10415 | closed | stopped | message | `55c9edcfc93fd3049e48621a1b3fa9881fcb750b7ed4390a1e11f6975a87feac` |
| 2026-09-06T00:08:50.459+02:00 | M2-stop | raw-getStateInformation | 0.89 | 10415 | closed | stopped | message | `55c9edcfc93fd3049e48621a1b3fa9881fcb750b7ed4390a1e11f6975a87feac` |
| 2026-09-06T00:08:51.460+02:00 | M2-stop | raw-getStateInformation | 0.91 | 10415 | closed | stopped | message | `55c9edcfc93fd3049e48621a1b3fa9881fcb750b7ed4390a1e11f6975a87feac` |
| 2026-09-06T00:08:54.462+02:00 | M2-late | raw-getStateInformation | 0.54 | 10415 | closed | stopped | message | `55c9edcfc93fd3049e48621a1b3fa9881fcb750b7ed4390a1e11f6975a87feac` |
| 2026-09-06T00:08:54.467+02:00 | M2-late | raw-getStateInformation | 0.51 | 10415 | closed | stopped | message | `55c9edcfc93fd3049e48621a1b3fa9881fcb750b7ed4390a1e11f6975a87feac` |
| 2026-09-06T00:08:54.469+02:00 | M2-late | raw-getStateInformation | 0.44 | 10415 | closed | stopped | message | `55c9edcfc93fd3049e48621a1b3fa9881fcb750b7ed4390a1e11f6975a87feac` |
| 2026-09-06T00:08:54.473+02:00 | M2-late | raw-getStateInformation | 0.50 | 10415 | closed | stopped | message | `55c9edcfc93fd3049e48621a1b3fa9881fcb750b7ed4390a1e11f6975a87feac` |
| 2026-09-06T00:08:54.476+02:00 | M2-late | raw-getStateInformation | 0.45 | 10415 | closed | stopped | message | `55c9edcfc93fd3049e48621a1b3fa9881fcb750b7ed4390a1e11f6975a87feac` |
| 2026-09-06T00:08:54.479+02:00 | M2-late | raw-getStateInformation | 0.47 | 10415 | closed | stopped | message | `55c9edcfc93fd3049e48621a1b3fa9881fcb750b7ed4390a1e11f6975a87feac` |
| 2026-09-06T00:08:54.482+02:00 | M2-late | raw-getStateInformation | 0.43 | 10415 | closed | stopped | message | `55c9edcfc93fd3049e48621a1b3fa9881fcb750b7ed4390a1e11f6975a87feac` |
| 2026-09-06T00:08:54.484+02:00 | M2-late | raw-getStateInformation | 0.60 | 10415 | closed | stopped | message | `55c9edcfc93fd3049e48621a1b3fa9881fcb750b7ed4390a1e11f6975a87feac` |
| 2026-09-06T00:08:54.487+02:00 | M2-late | raw-getStateInformation | 0.48 | 10415 | closed | stopped | message | `55c9edcfc93fd3049e48621a1b3fa9881fcb750b7ed4390a1e11f6975a87feac` |
| 2026-09-06T00:08:54.490+02:00 | M2-late | raw-getStateInformation | 0.48 | 10415 | closed | stopped | message | `55c9edcfc93fd3049e48621a1b3fa9881fcb750b7ed4390a1e11f6975a87feac` |
| 2026-09-06T00:08:59.462+02:00 | M2-late | raw-getStateInformation | 0.54 | 10415 | closed | stopped | message | `55c9edcfc93fd3049e48621a1b3fa9881fcb750b7ed4390a1e11f6975a87feac` |
| 2026-09-06T00:08:59.468+02:00 | M2-late | raw-getStateInformation | 0.57 | 10415 | closed | stopped | message | `55c9edcfc93fd3049e48621a1b3fa9881fcb750b7ed4390a1e11f6975a87feac` |
| 2026-09-06T00:08:59.471+02:00 | M2-late | raw-getStateInformation | 0.50 | 10415 | closed | stopped | message | `55c9edcfc93fd3049e48621a1b3fa9881fcb750b7ed4390a1e11f6975a87feac` |
| 2026-09-06T00:08:59.474+02:00 | M2-late | raw-getStateInformation | 0.58 | 10415 | closed | stopped | message | `55c9edcfc93fd3049e48621a1b3fa9881fcb750b7ed4390a1e11f6975a87feac` |
| 2026-09-06T00:08:59.478+02:00 | M2-late | raw-getStateInformation | 0.49 | 10415 | closed | stopped | message | `55c9edcfc93fd3049e48621a1b3fa9881fcb750b7ed4390a1e11f6975a87feac` |
| 2026-09-06T00:08:59.481+02:00 | M2-late | raw-getStateInformation | 0.44 | 10415 | closed | stopped | message | `55c9edcfc93fd3049e48621a1b3fa9881fcb750b7ed4390a1e11f6975a87feac` |
| 2026-09-06T00:08:59.483+02:00 | M2-late | raw-getStateInformation | 0.47 | 10415 | closed | stopped | message | `55c9edcfc93fd3049e48621a1b3fa9881fcb750b7ed4390a1e11f6975a87feac` |
| 2026-09-06T00:08:59.486+02:00 | M2-late | raw-getStateInformation | 0.44 | 10415 | closed | stopped | message | `55c9edcfc93fd3049e48621a1b3fa9881fcb750b7ed4390a1e11f6975a87feac` |
| 2026-09-06T00:08:59.488+02:00 | M2-late | raw-getStateInformation | 0.43 | 10415 | closed | stopped | message | `55c9edcfc93fd3049e48621a1b3fa9881fcb750b7ed4390a1e11f6975a87feac` |
| 2026-09-06T00:08:59.491+02:00 | M2-late | raw-getStateInformation | 0.48 | 10415 | closed | stopped | message | `55c9edcfc93fd3049e48621a1b3fa9881fcb750b7ed4390a1e11f6975a87feac` |

## Operator notes

*None.*

---
*End of sanitized SPIKE-01 report. Verify before sharing: this file must
contain no base64 blocks and no plugin-state byte dumps.*
