# SPIKE-01 sanitized measurement report — authoritative plugin-state capture

SPIKE-01 diagnostic output (roadmap slice P0/P1A; canonical steering §9.2 / PID-001).

**Privacy:** this report contains only sizes, SHA-256 hashes, timings, and
notification metadata. No raw plugin-state bytes, presets, or licensing
material are captured to disk by the SPIKE-01 probe.

## Environment

| Field | Value |
|---|---|
| Generated | 2026-09-05T22:17:57.846+02:00 |
| App version | 0.2.0 |
| Plugin | VB3-II |
| Plugin format | VST3 |
| Plugin version | 2.3.1 |
| Plugin identifier | C:\Program Files\Common Files\VST3\VB3-II.vst3 |
| Operator notes |  |

## Capture measurements per phase

| Phase / path | n | min ms | median ms | p95 ms | max ms | blob bytes (min..max) | distinct hashes | byte-stable |
|---|---|---|---|---|---|---|---|---|
| A1 / raw-getStateInformation | 41 | 0.44 | 0.48 | 0.80 | 0.97 | 10433..10453 | 5 | NO |
| A2 / raw-getStateInformation | 10 | 0.44 | 0.47 | 0.77 | 0.77 | 10455..10455 | 1 | yes |
| A3 / raw-getStateInformation | 30 | 0.43 | 0.46 | 0.58 | 0.98 | 10433..10433 | 1 | yes |
| A4 / raw-getStateInformation | 10 | 0.45 | 0.47 | 0.99 | 0.99 | 10433..10433 | 1 | yes |
| A5 / raw-getStateInformation | 1 | 0.56 | 0.56 | 0.56 | 0.56 | 10453..10453 | 1 | yes |
| B2 / raw-getStateInformation | 20 | 0.45 | 0.50 | 0.59 | 0.97 | 10433..10433 | 1 | yes |
| B4 / raw-getStateInformation | 20 | 0.43 | 0.46 | 0.70 | 1.01 | 10433..10433 | 1 | yes |
| B5 / raw-getStateInformation | 11 | 0.46 | 0.50 | 0.84 | 0.84 | 10433..10433 | 1 | yes |
| E1 / raw-getStateInformation | 1 | 0.54 | 0.54 | 0.54 | 0.54 | 10393..10393 | 1 | yes |
| E2 / raw-getStateInformation | 1 | 0.56 | 0.56 | 0.56 | 0.56 | 10393..10393 | 1 | yes |
| F1 / raw-getStateInformation | 1 | 0.56 | 0.56 | 0.56 | 0.56 | 10393..10393 | 1 | yes |
| F1 / save-path-base64 | 1 | 0.55 | 0.55 | 0.55 | 0.55 | 10393..10393 | 1 | yes |

## Hash inventory (12-hex prefixes; full hashes in raw sample log)

- **A1**: `10d39d410df1…`, `a58813037e07…`, `eb1c914bd711…`, `f329ab8b9014…`, `f36254823219…`
- **A2**: `5aa0dbac7e37…`
- **A3**: `a58813037e07…`
- **A4**: `a58813037e07…`
- **A5**: `33a537eff6ec…`
- **B2**: `a58813037e07…`
- **B4**: `a58813037e07…`
- **B5**: `a58813037e07…`
- **E1**: `875dc964caa6…`
- **E2**: `875dc964caa6…`
- **F1**: `875dc964caa6…`

## Threading

- Captures executed on the message thread: 147
- Captures executed on other threads: 0 (expected: 0)
- Parameter/processor notifications on the message thread: 230
- Parameter/processor notifications on other threads: 0

## Parameter/processor notifications (230)

| Time | Kind | Param idx | Param name | Value | Thread | Detail |
|---|---|---|---|---|---|---|
| 2026-09-05T22:15:38.298+02:00 | paramChanged | 28 | Drawbar Upper A 16' | 0.01 | message |  |
| 2026-09-05T22:15:38.306+02:00 | paramChanged | 28 | Drawbar Upper A 16' | 0.01 | message |  |
| 2026-09-05T22:15:38.313+02:00 | paramChanged | 28 | Drawbar Upper A 16' | 0.03 | message |  |
| 2026-09-05T22:15:38.318+02:00 | paramChanged | 28 | Drawbar Upper A 16' | 0.04 | message |  |
| 2026-09-05T22:15:38.324+02:00 | paramChanged | 28 | Drawbar Upper A 16' | 0.05 | message |  |
| 2026-09-05T22:15:38.330+02:00 | paramChanged | 28 | Drawbar Upper A 16' | 0.07 | message |  |
| 2026-09-05T22:15:38.333+02:00 | paramChanged | 28 | Drawbar Upper A 16' | 0.07 | message |  |
| 2026-09-05T22:15:38.337+02:00 | paramChanged | 28 | Drawbar Upper A 16' | 0.08 | message |  |
| 2026-09-05T22:15:38.340+02:00 | paramChanged | 28 | Drawbar Upper A 16' | 0.09 | message |  |
| 2026-09-05T22:15:38.346+02:00 | paramChanged | 28 | Drawbar Upper A 16' | 0.12 | message |  |
| 2026-09-05T22:15:38.348+02:00 | paramChanged | 28 | Drawbar Upper A 16' | 0.12 | message |  |
| 2026-09-05T22:15:38.353+02:00 | paramChanged | 28 | Drawbar Upper A 16' | 0.14 | message |  |
| 2026-09-05T22:15:38.358+02:00 | paramChanged | 28 | Drawbar Upper A 16' | 0.15 | message |  |
| 2026-09-05T22:15:38.361+02:00 | paramChanged | 28 | Drawbar Upper A 16' | 0.16 | message |  |
| 2026-09-05T22:15:38.365+02:00 | paramChanged | 28 | Drawbar Upper A 16' | 0.18 | message |  |
| 2026-09-05T22:15:38.368+02:00 | paramChanged | 28 | Drawbar Upper A 16' | 0.19 | message |  |
| 2026-09-05T22:15:38.373+02:00 | paramChanged | 28 | Drawbar Upper A 16' | 0.21 | message |  |
| 2026-09-05T22:15:38.376+02:00 | paramChanged | 28 | Drawbar Upper A 16' | 0.22 | message |  |
| 2026-09-05T22:15:38.382+02:00 | paramChanged | 28 | Drawbar Upper A 16' | 0.24 | message |  |
| 2026-09-05T22:15:38.386+02:00 | paramChanged | 28 | Drawbar Upper A 16' | 0.26 | message |  |
| 2026-09-05T22:15:38.390+02:00 | paramChanged | 28 | Drawbar Upper A 16' | 0.29 | message |  |
| 2026-09-05T22:15:38.395+02:00 | paramChanged | 28 | Drawbar Upper A 16' | 0.29 | message |  |
| 2026-09-05T22:15:38.398+02:00 | paramChanged | 28 | Drawbar Upper A 16' | 0.31 | message |  |
| 2026-09-05T22:15:38.402+02:00 | paramChanged | 28 | Drawbar Upper A 16' | 0.32 | message |  |
| 2026-09-05T22:15:38.409+02:00 | paramChanged | 28 | Drawbar Upper A 16' | 0.34 | message |  |
| 2026-09-05T22:15:38.414+02:00 | paramChanged | 28 | Drawbar Upper A 16' | 0.37 | message |  |
| 2026-09-05T22:15:38.417+02:00 | paramChanged | 28 | Drawbar Upper A 16' | 0.38 | message |  |
| 2026-09-05T22:15:38.421+02:00 | paramChanged | 28 | Drawbar Upper A 16' | 0.39 | message |  |
| 2026-09-05T22:15:38.424+02:00 | paramChanged | 28 | Drawbar Upper A 16' | 0.40 | message |  |
| 2026-09-05T22:15:38.428+02:00 | paramChanged | 28 | Drawbar Upper A 16' | 0.42 | message |  |
| 2026-09-05T22:15:38.435+02:00 | paramChanged | 28 | Drawbar Upper A 16' | 0.45 | message |  |
| 2026-09-05T22:15:38.439+02:00 | paramChanged | 28 | Drawbar Upper A 16' | 0.46 | message |  |
| 2026-09-05T22:15:38.444+02:00 | paramChanged | 28 | Drawbar Upper A 16' | 0.49 | message |  |
| 2026-09-05T22:15:38.449+02:00 | paramChanged | 28 | Drawbar Upper A 16' | 0.51 | message |  |
| 2026-09-05T22:15:38.452+02:00 | paramChanged | 28 | Drawbar Upper A 16' | 0.52 | message |  |
| 2026-09-05T22:15:38.458+02:00 | paramChanged | 28 | Drawbar Upper A 16' | 0.56 | message |  |
| 2026-09-05T22:15:38.463+02:00 | paramChanged | 28 | Drawbar Upper A 16' | 0.58 | message |  |
| 2026-09-05T22:15:38.467+02:00 | paramChanged | 28 | Drawbar Upper A 16' | 0.59 | message |  |
| 2026-09-05T22:15:38.472+02:00 | paramChanged | 28 | Drawbar Upper A 16' | 0.62 | message |  |
| 2026-09-05T22:15:38.477+02:00 | paramChanged | 28 | Drawbar Upper A 16' | 0.63 | message |  |
| 2026-09-05T22:15:38.480+02:00 | paramChanged | 28 | Drawbar Upper A 16' | 0.64 | message |  |
| 2026-09-05T22:15:38.487+02:00 | paramChanged | 28 | Drawbar Upper A 16' | 0.65 | message |  |
| 2026-09-05T22:15:38.492+02:00 | paramChanged | 28 | Drawbar Upper A 16' | 0.66 | message |  |
| 2026-09-05T22:15:38.499+02:00 | paramChanged | 28 | Drawbar Upper A 16' | 0.67 | message |  |
| 2026-09-05T22:15:38.504+02:00 | paramChanged | 28 | Drawbar Upper A 16' | 0.67 | message |  |
| 2026-09-05T22:15:38.508+02:00 | paramChanged | 28 | Drawbar Upper A 16' | 0.68 | message |  |
| 2026-09-05T22:15:38.518+02:00 | paramChanged | 28 | Drawbar Upper A 16' | 0.69 | message |  |
| 2026-09-05T22:15:38.522+02:00 | paramChanged | 28 | Drawbar Upper A 16' | 0.70 | message |  |
| 2026-09-05T22:15:38.527+02:00 | paramChanged | 28 | Drawbar Upper A 16' | 0.70 | message |  |
| 2026-09-05T22:15:38.533+02:00 | paramChanged | 28 | Drawbar Upper A 16' | 0.71 | message |  |
| 2026-09-05T22:15:38.540+02:00 | paramChanged | 28 | Drawbar Upper A 16' | 0.73 | message |  |
| 2026-09-05T22:15:38.543+02:00 | paramChanged | 28 | Drawbar Upper A 16' | 0.73 | message |  |
| 2026-09-05T22:15:38.548+02:00 | paramChanged | 28 | Drawbar Upper A 16' | 0.74 | message |  |
| 2026-09-05T22:15:38.553+02:00 | paramChanged | 28 | Drawbar Upper A 16' | 0.75 | message |  |
| 2026-09-05T22:15:38.556+02:00 | paramChanged | 28 | Drawbar Upper A 16' | 0.75 | message |  |
| 2026-09-05T22:15:38.562+02:00 | paramChanged | 28 | Drawbar Upper A 16' | 0.76 | message |  |
| 2026-09-05T22:16:06.489+02:00 | paramChanged | 28 | Drawbar Upper A 16' | 0.75 | message |  |
| 2026-09-05T22:16:06.498+02:00 | paramChanged | 28 | Drawbar Upper A 16' | 0.73 | message |  |
| 2026-09-05T22:16:06.506+02:00 | paramChanged | 28 | Drawbar Upper A 16' | 0.70 | message |  |
| 2026-09-05T22:16:06.512+02:00 | paramChanged | 28 | Drawbar Upper A 16' | 0.67 | message |  |
| 2026-09-05T22:16:06.520+02:00 | paramChanged | 28 | Drawbar Upper A 16' | 0.63 | message |  |
| 2026-09-05T22:16:06.524+02:00 | paramChanged | 28 | Drawbar Upper A 16' | 0.62 | message |  |
| 2026-09-05T22:16:06.529+02:00 | paramChanged | 28 | Drawbar Upper A 16' | 0.59 | message |  |
| 2026-09-05T22:16:06.535+02:00 | paramChanged | 28 | Drawbar Upper A 16' | 0.54 | message |  |
| 2026-09-05T22:16:06.540+02:00 | paramChanged | 28 | Drawbar Upper A 16' | 0.51 | message |  |
| 2026-09-05T22:16:06.543+02:00 | paramChanged | 28 | Drawbar Upper A 16' | 0.50 | message |  |
| 2026-09-05T22:16:06.550+02:00 | paramChanged | 28 | Drawbar Upper A 16' | 0.45 | message |  |
| 2026-09-05T22:16:06.555+02:00 | paramChanged | 28 | Drawbar Upper A 16' | 0.42 | message |  |
| 2026-09-05T22:16:06.557+02:00 | paramChanged | 28 | Drawbar Upper A 16' | 0.40 | message |  |
| 2026-09-05T22:16:06.562+02:00 | paramChanged | 28 | Drawbar Upper A 16' | 0.37 | message |  |
| 2026-09-05T22:16:06.568+02:00 | paramChanged | 28 | Drawbar Upper A 16' | 0.33 | message |  |
| 2026-09-05T22:16:06.571+02:00 | paramChanged | 28 | Drawbar Upper A 16' | 0.32 | message |  |
| 2026-09-05T22:16:06.575+02:00 | paramChanged | 28 | Drawbar Upper A 16' | 0.29 | message |  |
| 2026-09-05T22:16:06.578+02:00 | paramChanged | 28 | Drawbar Upper A 16' | 0.27 | message |  |
| 2026-09-05T22:16:06.583+02:00 | paramChanged | 28 | Drawbar Upper A 16' | 0.24 | message |  |
| 2026-09-05T22:16:06.586+02:00 | paramChanged | 28 | Drawbar Upper A 16' | 0.21 | message |  |
| 2026-09-05T22:16:06.590+02:00 | paramChanged | 28 | Drawbar Upper A 16' | 0.19 | message |  |
| 2026-09-05T22:16:06.592+02:00 | paramChanged | 28 | Drawbar Upper A 16' | 0.16 | message |  |
| 2026-09-05T22:16:06.599+02:00 | paramChanged | 28 | Drawbar Upper A 16' | 0.12 | message |  |
| 2026-09-05T22:16:06.604+02:00 | paramChanged | 28 | Drawbar Upper A 16' | 0.09 | message |  |
| 2026-09-05T22:16:06.606+02:00 | paramChanged | 28 | Drawbar Upper A 16' | 0.06 | message |  |
| 2026-09-05T22:16:06.611+02:00 | paramChanged | 28 | Drawbar Upper A 16' | 0.03 | message |  |
| 2026-09-05T22:16:06.614+02:00 | paramChanged | 28 | Drawbar Upper A 16' | 0.01 | message |  |
| 2026-09-05T22:16:06.618+02:00 | paramChanged | 28 | Drawbar Upper A 16' | 0.00 | message |  |
| 2026-09-05T22:17:12.917+02:00 | paramChanged | 0 | Volume | 0.50 | message |  |
| 2026-09-05T22:17:12.922+02:00 | paramChanged | 1 | Bass | 0.50 | message |  |
| 2026-09-05T22:17:12.926+02:00 | paramChanged | 2 | Middle | 0.50 | message |  |
| 2026-09-05T22:17:12.930+02:00 | paramChanged | 3 | Middle Frequency | 0.50 | message |  |
| 2026-09-05T22:17:12.933+02:00 | paramChanged | 4 | Treble | 0.50 | message |  |
| 2026-09-05T22:17:12.935+02:00 | paramChanged | 5 | Key Click | 0.50 | message |  |
| 2026-09-05T22:17:12.937+02:00 | paramChanged | 6 | Overdrive | 0.00 | message |  |
| 2026-09-05T22:17:12.939+02:00 | paramChanged | 7 | Reverb | 0.13 | message |  |
| 2026-09-05T22:17:12.941+02:00 | paramChanged | 8 | Drawbar Upper B 16' | 1.00 | message |  |
| 2026-09-05T22:17:12.943+02:00 | paramChanged | 9 | Drawbar Upper B 5-1/3' | 0.41 | message |  |
| 2026-09-05T22:17:12.945+02:00 | paramChanged | 10 | Drawbar Upper B 8' | 1.00 | message |  |
| 2026-09-05T22:17:12.947+02:00 | paramChanged | 11 | Drawbar Upper B 4' | 0.00 | message |  |
| 2026-09-05T22:17:12.950+02:00 | paramChanged | 12 | Drawbar Upper B 2-2/3' | 0.00 | message |  |
| 2026-09-05T22:17:12.952+02:00 | paramChanged | 13 | Drawbar Upper B 2' | 0.00 | message |  |
| 2026-09-05T22:17:12.954+02:00 | paramChanged | 14 | Drawbar Upper B 1-3/5' | 0.00 | message |  |
| 2026-09-05T22:17:12.956+02:00 | paramChanged | 15 | Drawbar Upper B 1-1/3' | 0.00 | message |  |
| 2026-09-05T22:17:12.958+02:00 | paramChanged | 16 | Drawbar Upper B 1' | 0.00 | message |  |
| 2026-09-05T22:17:12.960+02:00 | paramChanged | 17 | Drawbar Lower B 16' | 1.00 | message |  |
| 2026-09-05T22:17:12.962+02:00 | paramChanged | 18 | Drawbar Lower B 5-1/3' | 0.00 | message |  |
| 2026-09-05T22:17:12.964+02:00 | paramChanged | 19 | Drawbar Lower B 8' | 1.00 | message |  |
| 2026-09-05T22:17:12.966+02:00 | paramChanged | 20 | Drawbar Lower B 4' | 0.00 | message |  |
| 2026-09-05T22:17:12.967+02:00 | paramChanged | 21 | Drawbar Lower B 2-2/3' | 0.00 | message |  |
| 2026-09-05T22:17:12.969+02:00 | paramChanged | 22 | Drawbar Lower B 2' | 0.00 | message |  |
| 2026-09-05T22:17:12.971+02:00 | paramChanged | 23 | Drawbar Lower B 1-3/5' | 0.00 | message |  |
| 2026-09-05T22:17:12.974+02:00 | paramChanged | 24 | Drawbar Lower B 1-1/3' | 0.00 | message |  |
| 2026-09-05T22:17:12.976+02:00 | paramChanged | 25 | Drawbar Lower B 1' | 0.00 | message |  |
| 2026-09-05T22:17:12.977+02:00 | paramChanged | 26 | Drawbar Pedal 16' | 0.76 | message |  |
| 2026-09-05T22:17:12.979+02:00 | paramChanged | 27 | Drawbar Pedal 8' | 0.25 | message |  |
| 2026-09-05T22:17:12.981+02:00 | paramChanged | 28 | Drawbar Upper A 16' | 0.56 | message |  |
| 2026-09-05T22:17:12.983+02:00 | paramChanged | 29 | Drawbar Upper A 5-1/3' | 1.00 | message |  |
| 2026-09-05T22:17:12.985+02:00 | paramChanged | 30 | Drawbar Upper A 8' | 0.00 | message |  |
| 2026-09-05T22:17:12.987+02:00 | paramChanged | 31 | Drawbar Upper A 4' | 0.00 | message |  |
| 2026-09-05T22:17:12.988+02:00 | paramChanged | 32 | Drawbar Upper A 2-2/3' | 0.00 | message |  |
| 2026-09-05T22:17:12.990+02:00 | paramChanged | 33 | Drawbar Upper A 2' | 0.00 | message |  |
| 2026-09-05T22:17:12.992+02:00 | paramChanged | 34 | Drawbar Upper A 1-3/5' | 0.00 | message |  |
| 2026-09-05T22:17:12.994+02:00 | paramChanged | 35 | Drawbar Upper A 1-1/3' | 0.00 | message |  |
| 2026-09-05T22:17:12.996+02:00 | paramChanged | 36 | Drawbar Upper A 1' | 0.00 | message |  |
| 2026-09-05T22:17:12.998+02:00 | paramChanged | 37 | Drawbar Lower A 16' | 0.00 | message |  |
| 2026-09-05T22:17:13.000+02:00 | paramChanged | 38 | Drawbar Lower A 5-1/3' | 0.00 | message |  |
| 2026-09-05T22:17:13.002+02:00 | paramChanged | 39 | Drawbar Lower A 8' | 1.00 | message |  |
| 2026-09-05T22:17:13.003+02:00 | paramChanged | 40 | Drawbar Lower A 4' | 0.69 | message |  |
| 2026-09-05T22:17:13.006+02:00 | paramChanged | 41 | Drawbar Lower A 2-2/3' | 0.17 | message |  |
| 2026-09-05T22:17:13.008+02:00 | paramChanged | 42 | Drawbar Lower A 2' | 0.00 | message |  |
| 2026-09-05T22:17:13.010+02:00 | paramChanged | 43 | Drawbar Lower A 1-3/5' | 0.00 | message |  |
| 2026-09-05T22:17:13.011+02:00 | paramChanged | 44 | Drawbar Lower A 1-1/3' | 0.00 | message |  |
| 2026-09-05T22:17:13.013+02:00 | paramChanged | 45 | Drawbar Lower A 1' | 0.00 | message |  |
| 2026-09-05T22:17:13.015+02:00 | paramChanged | 46 | Tab Volume | 1.00 | message |  |
| 2026-09-05T22:17:13.017+02:00 | paramChanged | 47 | Tab Vibrato Upper | 1.00 | message |  |
| 2026-09-05T22:17:13.018+02:00 | paramChanged | 48 | Tab Vibrato Lower | 0.00 | message |  |
| 2026-09-05T22:17:13.021+02:00 | paramChanged | 49 | Tab Percussion | 1.00 | message |  |
| 2026-09-05T22:17:13.023+02:00 | paramChanged | 50 | Tab Percussion Volume | 0.00 | message |  |
| 2026-09-05T22:17:13.025+02:00 | paramChanged | 51 | Tab Percussion Decay | 1.00 | message |  |
| 2026-09-05T22:17:13.027+02:00 | paramChanged | 52 | Tab Percussion Harmonic | 1.00 | message |  |
| 2026-09-05T22:17:13.028+02:00 | paramChanged | 53 | Vibrato/Chorus Selector | 1.00 | message |  |
| 2026-09-05T22:17:13.030+02:00 | paramChanged | 54 | Swell Pedal | 1.00 | message |  |
| 2026-09-05T22:17:13.032+02:00 | paramChanged | 55 | Rotary FX Speed Switch | 0.00 | message |  |
| 2026-09-05T22:17:13.034+02:00 | paramChanged | 57 | Split Mode | 0.00 | message |  |
| 2026-09-05T22:17:13.036+02:00 | paramChanged | 58 | Split Note | 0.33 | message |  |
| 2026-09-05T22:17:13.038+02:00 | paramChanged | 59 | Upper Octave | 0.50 | message |  |
| 2026-09-05T22:17:13.042+02:00 | paramChanged | 60 | Lower Octave | 0.50 | message |  |
| 2026-09-05T22:17:13.044+02:00 | paramChanged | 61 | Upper Drawbar Preset | 1.00 | message |  |
| 2026-09-05T22:17:13.045+02:00 | paramChanged | 62 | Lower Drawbar Preset | 1.00 | message |  |
| 2026-09-05T22:17:13.047+02:00 | paramChanged | 63 | Generator | 0.00 | message |  |
| 2026-09-05T22:17:13.051+02:00 | paramChanged | 64 | Generator Shape | 0.00 | message |  |
| 2026-09-05T22:17:13.055+02:00 | paramChanged | 65 | Resistor Wires | 0.50 | message |  |
| 2026-09-05T22:17:13.057+02:00 | paramChanged | 66 | Leakage | 0.00 | message |  |
| 2026-09-05T22:17:13.059+02:00 | paramChanged | 67 | Crosstalk | 0.50 | message |  |
| 2026-09-05T22:17:13.061+02:00 | paramChanged | 68 | Crosstalk Shape | 0.50 | message |  |
| 2026-09-05T22:17:13.062+02:00 | paramChanged | 69 | Complex Wheels | 1.00 | message |  |
| 2026-09-05T22:17:13.065+02:00 | paramChanged | 70 | Foldback on 16' | 1.00 | message |  |
| 2026-09-05T22:17:13.067+02:00 | paramChanged | 71 | PedalsToLower | 0.00 | message |  |
| 2026-09-05T22:17:13.069+02:00 | paramChanged | 72 | Bass Decay | 0.00 | message |  |
| 2026-09-05T22:17:13.071+02:00 | paramChanged | 73 | Trim DB 16' | 0.50 | message |  |
| 2026-09-05T22:17:13.073+02:00 | paramChanged | 74 | Trim DB 5-1/3' | 0.50 | message |  |
| 2026-09-05T22:17:13.075+02:00 | paramChanged | 75 | Trim DB 8' | 0.50 | message |  |
| 2026-09-05T22:17:13.077+02:00 | paramChanged | 76 | Trim DB 4' | 0.50 | message |  |
| 2026-09-05T22:17:13.079+02:00 | paramChanged | 77 | Trim DB 2-2/3' | 0.50 | message |  |
| 2026-09-05T22:17:13.080+02:00 | paramChanged | 78 | Trim DB 2' | 0.50 | message |  |
| 2026-09-05T22:17:13.082+02:00 | paramChanged | 79 | Trim DB 1-3/5' | 0.50 | message |  |
| 2026-09-05T22:17:13.084+02:00 | paramChanged | 80 | Trim DB 1-1/3' | 0.50 | message |  |
| 2026-09-05T22:17:13.086+02:00 | paramChanged | 81 | Trim DB 1' | 0.50 | message |  |
| 2026-09-05T22:17:13.088+02:00 | paramChanged | 82 | Percussion Level | 0.50 | message |  |
| 2026-09-05T22:17:13.090+02:00 | paramChanged | 83 | Percussion DropOut | 0.50 | message |  |
| 2026-09-05T22:17:13.093+02:00 | paramChanged | 84 | Percussion Sotf Level | 0.50 | message |  |
| 2026-09-05T22:17:13.095+02:00 | paramChanged | 85 | Percussion Norm Level | 0.50 | message |  |
| 2026-09-05T22:17:13.097+02:00 | paramChanged | 86 | Percussion Fast Decay | 0.50 | message |  |
| 2026-09-05T22:17:13.099+02:00 | paramChanged | 87 | Percussion Slow Decay | 0.50 | message |  |
| 2026-09-05T22:17:13.100+02:00 | paramChanged | 88 | Percussion Paradise Mod | 0.00 | message |  |
| 2026-09-05T22:17:13.102+02:00 | paramChanged | 89 | Vibrato Depth | 0.50 | message |  |
| 2026-09-05T22:17:13.104+02:00 | paramChanged | 90 | Vibrato V/C Mix | 0.50 | message |  |
| 2026-09-05T22:17:13.106+02:00 | paramChanged | 91 | Key Click Length | 0.50 | message |  |
| 2026-09-05T22:17:13.108+02:00 | paramChanged | 92 | Key Click Color | 0.50 | message |  |
| 2026-09-05T22:17:13.109+02:00 | paramChanged | 93 | Key Click Release | 0.50 | message |  |
| 2026-09-05T22:17:13.111+02:00 | paramChanged | 94 | Key Click Bounce | 0.50 | message |  |
| 2026-09-05T22:17:13.113+02:00 | paramChanged | 95 | Preamp Bass | 0.50 | message |  |
| 2026-09-05T22:17:13.115+02:00 | paramChanged | 96 | Preamp Treble | 0.50 | message |  |
| 2026-09-05T22:17:13.117+02:00 | paramChanged | 97 | Ring Modulator ON/OFF | 0.00 | message |  |
| 2026-09-05T22:17:13.119+02:00 | paramChanged | 98 | Ring Modulator Rate | 0.00 | message |  |
| 2026-09-05T22:17:13.120+02:00 | paramChanged | 99 | Ring Modulator Depth | 0.50 | message |  |
| 2026-09-05T22:17:13.122+02:00 | paramChanged | 100 | Delay Level | 0.00 | message |  |
| 2026-09-05T22:17:13.124+02:00 | paramChanged | 101 | Delay Time | 0.50 | message |  |
| 2026-09-05T22:17:13.126+02:00 | paramChanged | 102 | Delay Feedback | 0.50 | message |  |
| 2026-09-05T22:17:13.128+02:00 | paramChanged | 103 | Delay Stereo Width | 0.00 | message |  |
| 2026-09-05T22:17:13.129+02:00 | paramChanged | 104 | Spring Reverb Level | 0.00 | message |  |
| 2026-09-05T22:17:13.131+02:00 | paramChanged | 105 | Spring Reverb Timbre | 0.50 | message |  |
| 2026-09-05T22:17:13.133+02:00 | paramChanged | 106 | Spring Reverb Decay | 0.50 | message |  |
| 2026-09-05T22:17:13.135+02:00 | paramChanged | 107 | Spring Reverb Damp | 0.50 | message |  |
| 2026-09-05T22:17:13.137+02:00 | paramChanged | 108 | Rotary FX ON/OFF | 1.00 | message |  |
| 2026-09-05T22:17:13.138+02:00 | paramChanged | 109 | Horn Slow Speed | 0.50 | message |  |
| 2026-09-05T22:17:13.140+02:00 | paramChanged | 110 | Horn Fast Speed | 0.50 | message |  |
| 2026-09-05T22:17:13.142+02:00 | paramChanged | 111 | Bass Slow Speed | 0.50 | message |  |
| 2026-09-05T22:17:13.144+02:00 | paramChanged | 112 | Bass Fast Speed | 0.50 | message |  |
| 2026-09-05T22:17:13.146+02:00 | paramChanged | 113 | Horn Ramp Up | 0.50 | message |  |
| 2026-09-05T22:17:13.148+02:00 | paramChanged | 114 | Horn Ramp Down | 0.50 | message |  |
| 2026-09-05T22:17:13.150+02:00 | paramChanged | 115 | Bass Ramp Up | 0.50 | message |  |
| 2026-09-05T22:17:13.151+02:00 | paramChanged | 116 | Bass Ramp Down | 0.50 | message |  |
| 2026-09-05T22:17:13.153+02:00 | paramChanged | 117 | Rotary FX Balance | 0.50 | message |  |
| 2026-09-05T22:17:13.155+02:00 | paramChanged | 118 | Rotary FX Ambience | 0.50 | message |  |
| 2026-09-05T22:17:13.157+02:00 | paramChanged | 119 | Rotary FX Cabinet | 0.50 | message |  |
| 2026-09-05T22:17:13.159+02:00 | paramChanged | 120 | Rotary FX Mic Angle | 0.50 | message |  |
| 2026-09-05T22:17:13.161+02:00 | paramChanged | 121 | Rotary FX Mic Distance | 0.50 | message |  |
| 2026-09-05T22:17:13.163+02:00 | paramChanged | 122 | Rotary FX Horn EQ | 0.50 | message |  |
| 2026-09-05T22:17:13.165+02:00 | paramChanged | 123 | Rotary FX Mid Reflections | 0.50 | message |  |
| 2026-09-05T22:17:13.168+02:00 | paramChanged | 124 | Rotary FX Doppler Intensity | 0.50 | message |  |
| 2026-09-05T22:17:13.169+02:00 | paramChanged | 125 | Rotary FX Dry Leak | 0.00 | message |  |
| 2026-09-05T22:17:13.171+02:00 | paramChanged | 126 | Rotary FX Bass Port | 0.00 | message |  |
| 2026-09-05T22:17:13.173+02:00 | paramChanged | 127 | Rotary FX Tube Feedback | 0.00 | message |  |
| 2026-09-05T22:17:13.175+02:00 | paramChanged | 128 | Rotary FX Stop Position | 0.00 | message |  |
| 2026-09-05T22:17:13.178+02:00 | paramChanged | 129 | Rotary FX Noises | 0.00 | message |  |
| 2026-09-05T22:17:13.180+02:00 | paramChanged | 130 | Rotary FX Memphis Style | 0.00 | message |  |
| 2026-09-05T22:17:13.182+02:00 | paramChanged | 131 | Rotary FX Front Stop | 1.00 | message |  |
| 2026-09-05T22:17:13.184+02:00 | paramChanged | 132 | Limiter On/Off | 0.00 | message |  |
| 2026-09-05T22:17:13.186+02:00 | paramChanged | 133 | Limiter Input | 0.50 | message |  |
| 2026-09-05T22:17:13.188+02:00 | paramChanged | 134 | Limiter Output | 0.50 | message |  |
| 2026-09-05T22:17:13.189+02:00 | paramChanged | 135 | Limiter Attack | 0.50 | message |  |
| 2026-09-05T22:17:13.191+02:00 | paramChanged | 136 | Limiter Release | 0.50 | message |  |
| 2026-09-05T22:17:13.193+02:00 | paramChanged | 137 | Limiter Ratio | 0.00 | message |  |
| 2026-09-05T22:17:13.195+02:00 | paramChanged | 138 | Digital Reverb Decay | 0.50 | message |  |
| 2026-09-05T22:17:13.197+02:00 | paramChanged | 139 | Digital Reverb Damp | 0.50 | message |  |
| 2026-09-05T22:17:13.199+02:00 | paramChanged | 140 | Digital Reverb Diffusion | 0.50 | message |  |
| 2026-09-05T22:17:13.201+02:00 | paramChanged | 141 | Digital Reverb Pre-Delay | 0.24 | message |  |
| 2026-09-05T22:17:13.203+02:00 | paramChanged | 142 | Digital Reverb Room Size | 0.50 | message |  |
| 2026-09-05T22:17:13.206+02:00 | paramChanged | 143 | Digital Reverb High Shelf | 0.16 | message |  |
| 2026-09-05T22:17:13.207+02:00 | paramChanged | 144 | Digital Reverb Low Shelf | 0.79 | message |  |
| 2026-09-05T22:17:13.209+02:00 | paramChanged | 145 | Use VB3 v.1.4 organ engine | 0.00 | message |  |
| 2026-09-05T22:17:13.211+02:00 | paramChanged | 146 | Amp Selection | 0.00 | message |  |

## Raw sample log (147)

| Time | Phase | Path | ms | bytes | editor | transport | thread | sha256 |
|---|---|---|---|---|---|---|---|---|
| 2026-09-05T22:12:53.363+02:00 | A1 | raw-getStateInformation | 0.58 | 10433 | open | stopped | message | `a58813037e078963f40ecb0d1bc12553ed0262807258c41d74fcd2446eb349cc` |
| 2026-09-05T22:13:13.796+02:00 | A1 | raw-getStateInformation | 0.55 | 10433 | open | stopped | message | `a58813037e078963f40ecb0d1bc12553ed0262807258c41d74fcd2446eb349cc` |
| 2026-09-05T22:13:13.800+02:00 | A1 | raw-getStateInformation | 0.54 | 10433 | open | stopped | message | `a58813037e078963f40ecb0d1bc12553ed0262807258c41d74fcd2446eb349cc` |
| 2026-09-05T22:13:13.804+02:00 | A1 | raw-getStateInformation | 0.97 | 10433 | open | stopped | message | `a58813037e078963f40ecb0d1bc12553ed0262807258c41d74fcd2446eb349cc` |
| 2026-09-05T22:13:13.808+02:00 | A1 | raw-getStateInformation | 0.49 | 10433 | open | stopped | message | `a58813037e078963f40ecb0d1bc12553ed0262807258c41d74fcd2446eb349cc` |
| 2026-09-05T22:13:13.812+02:00 | A1 | raw-getStateInformation | 0.48 | 10433 | open | stopped | message | `a58813037e078963f40ecb0d1bc12553ed0262807258c41d74fcd2446eb349cc` |
| 2026-09-05T22:13:13.815+02:00 | A1 | raw-getStateInformation | 0.46 | 10433 | open | stopped | message | `a58813037e078963f40ecb0d1bc12553ed0262807258c41d74fcd2446eb349cc` |
| 2026-09-05T22:13:13.819+02:00 | A1 | raw-getStateInformation | 0.51 | 10433 | open | stopped | message | `a58813037e078963f40ecb0d1bc12553ed0262807258c41d74fcd2446eb349cc` |
| 2026-09-05T22:13:13.821+02:00 | A1 | raw-getStateInformation | 0.47 | 10433 | open | stopped | message | `a58813037e078963f40ecb0d1bc12553ed0262807258c41d74fcd2446eb349cc` |
| 2026-09-05T22:13:13.825+02:00 | A1 | raw-getStateInformation | 0.46 | 10433 | open | stopped | message | `a58813037e078963f40ecb0d1bc12553ed0262807258c41d74fcd2446eb349cc` |
| 2026-09-05T22:13:13.827+02:00 | A1 | raw-getStateInformation | 0.44 | 10433 | open | stopped | message | `a58813037e078963f40ecb0d1bc12553ed0262807258c41d74fcd2446eb349cc` |
| 2026-09-05T22:13:24.108+02:00 | A1 | raw-getStateInformation | 0.80 | 10433 | closed | stopped | message | `a58813037e078963f40ecb0d1bc12553ed0262807258c41d74fcd2446eb349cc` |
| 2026-09-05T22:13:24.113+02:00 | A1 | raw-getStateInformation | 0.53 | 10433 | closed | stopped | message | `a58813037e078963f40ecb0d1bc12553ed0262807258c41d74fcd2446eb349cc` |
| 2026-09-05T22:13:24.117+02:00 | A1 | raw-getStateInformation | 0.47 | 10433 | closed | stopped | message | `a58813037e078963f40ecb0d1bc12553ed0262807258c41d74fcd2446eb349cc` |
| 2026-09-05T22:13:24.121+02:00 | A1 | raw-getStateInformation | 0.47 | 10433 | closed | stopped | message | `a58813037e078963f40ecb0d1bc12553ed0262807258c41d74fcd2446eb349cc` |
| 2026-09-05T22:13:24.123+02:00 | A1 | raw-getStateInformation | 0.46 | 10433 | closed | stopped | message | `a58813037e078963f40ecb0d1bc12553ed0262807258c41d74fcd2446eb349cc` |
| 2026-09-05T22:13:24.127+02:00 | A1 | raw-getStateInformation | 0.46 | 10433 | closed | stopped | message | `a58813037e078963f40ecb0d1bc12553ed0262807258c41d74fcd2446eb349cc` |
| 2026-09-05T22:13:24.129+02:00 | A1 | raw-getStateInformation | 0.44 | 10433 | closed | stopped | message | `a58813037e078963f40ecb0d1bc12553ed0262807258c41d74fcd2446eb349cc` |
| 2026-09-05T22:13:24.132+02:00 | A1 | raw-getStateInformation | 0.44 | 10433 | closed | stopped | message | `a58813037e078963f40ecb0d1bc12553ed0262807258c41d74fcd2446eb349cc` |
| 2026-09-05T22:13:24.135+02:00 | A1 | raw-getStateInformation | 0.48 | 10433 | closed | stopped | message | `a58813037e078963f40ecb0d1bc12553ed0262807258c41d74fcd2446eb349cc` |
| 2026-09-05T22:13:24.138+02:00 | A1 | raw-getStateInformation | 0.50 | 10433 | closed | stopped | message | `a58813037e078963f40ecb0d1bc12553ed0262807258c41d74fcd2446eb349cc` |
| 2026-09-05T22:13:39.882+02:00 | A1 | raw-getStateInformation | 0.58 | 10433 | open | stopped | message | `a58813037e078963f40ecb0d1bc12553ed0262807258c41d74fcd2446eb349cc` |
| 2026-09-05T22:13:39.887+02:00 | A1 | raw-getStateInformation | 0.68 | 10433 | open | stopped | message | `a58813037e078963f40ecb0d1bc12553ed0262807258c41d74fcd2446eb349cc` |
| 2026-09-05T22:13:39.891+02:00 | A1 | raw-getStateInformation | 0.47 | 10433 | open | stopped | message | `a58813037e078963f40ecb0d1bc12553ed0262807258c41d74fcd2446eb349cc` |
| 2026-09-05T22:13:39.895+02:00 | A1 | raw-getStateInformation | 0.48 | 10433 | open | stopped | message | `a58813037e078963f40ecb0d1bc12553ed0262807258c41d74fcd2446eb349cc` |
| 2026-09-05T22:13:39.898+02:00 | A1 | raw-getStateInformation | 0.46 | 10433 | open | stopped | message | `a58813037e078963f40ecb0d1bc12553ed0262807258c41d74fcd2446eb349cc` |
| 2026-09-05T22:13:39.901+02:00 | A1 | raw-getStateInformation | 0.46 | 10433 | open | stopped | message | `a58813037e078963f40ecb0d1bc12553ed0262807258c41d74fcd2446eb349cc` |
| 2026-09-05T22:13:39.905+02:00 | A1 | raw-getStateInformation | 0.52 | 10433 | open | stopped | message | `a58813037e078963f40ecb0d1bc12553ed0262807258c41d74fcd2446eb349cc` |
| 2026-09-05T22:13:39.908+02:00 | A1 | raw-getStateInformation | 0.49 | 10433 | open | stopped | message | `a58813037e078963f40ecb0d1bc12553ed0262807258c41d74fcd2446eb349cc` |
| 2026-09-05T22:13:39.910+02:00 | A1 | raw-getStateInformation | 0.46 | 10433 | open | stopped | message | `a58813037e078963f40ecb0d1bc12553ed0262807258c41d74fcd2446eb349cc` |
| 2026-09-05T22:13:39.913+02:00 | A1 | raw-getStateInformation | 0.45 | 10433 | open | stopped | message | `a58813037e078963f40ecb0d1bc12553ed0262807258c41d74fcd2446eb349cc` |
| 2026-09-05T22:13:56.525+02:00 | A1 | raw-getStateInformation | 0.57 | 10453 | open | playing | message | `eb1c914bd711f82d76e96c8fbe058f1f2df301b1d96252f7887753d3a4ab0a16` |
| 2026-09-05T22:13:56.530+02:00 | A1 | raw-getStateInformation | 0.97 | 10453 | open | playing | message | `eb1c914bd711f82d76e96c8fbe058f1f2df301b1d96252f7887753d3a4ab0a16` |
| 2026-09-05T22:13:56.535+02:00 | A1 | raw-getStateInformation | 0.48 | 10452 | open | playing | message | `f36254823219e2fbd585bac9a60b3abd82e01b59aef000bca27bb3e1abd0309c` |
| 2026-09-05T22:13:56.539+02:00 | A1 | raw-getStateInformation | 0.48 | 10452 | open | playing | message | `f36254823219e2fbd585bac9a60b3abd82e01b59aef000bca27bb3e1abd0309c` |
| 2026-09-05T22:13:56.542+02:00 | A1 | raw-getStateInformation | 0.45 | 10452 | open | playing | message | `f36254823219e2fbd585bac9a60b3abd82e01b59aef000bca27bb3e1abd0309c` |
| 2026-09-05T22:13:56.544+02:00 | A1 | raw-getStateInformation | 0.46 | 10452 | open | playing | message | `f329ab8b90147bcee4776204b3e54728330d8be4be6a2610bc4255b41bd18a32` |
| 2026-09-05T22:13:56.547+02:00 | A1 | raw-getStateInformation | 0.48 | 10452 | open | playing | message | `f329ab8b90147bcee4776204b3e54728330d8be4be6a2610bc4255b41bd18a32` |
| 2026-09-05T22:13:56.549+02:00 | A1 | raw-getStateInformation | 0.50 | 10452 | open | playing | message | `f329ab8b90147bcee4776204b3e54728330d8be4be6a2610bc4255b41bd18a32` |
| 2026-09-05T22:13:56.552+02:00 | A1 | raw-getStateInformation | 0.46 | 10452 | open | playing | message | `f329ab8b90147bcee4776204b3e54728330d8be4be6a2610bc4255b41bd18a32` |
| 2026-09-05T22:13:56.554+02:00 | A1 | raw-getStateInformation | 0.45 | 10453 | open | playing | message | `10d39d410df1e78473f14f2235b4902f69032cd2f3050d98843b6d517c4fc60a` |
| 2026-09-05T22:14:32.985+02:00 | A2 | raw-getStateInformation | 0.56 | 10455 | open | stopped | message | `5aa0dbac7e371815a5381b4dbc765b4c05f1c440d34b7d8854c96ef613500c2a` |
| 2026-09-05T22:14:32.990+02:00 | A2 | raw-getStateInformation | 0.77 | 10455 | open | stopped | message | `5aa0dbac7e371815a5381b4dbc765b4c05f1c440d34b7d8854c96ef613500c2a` |
| 2026-09-05T22:14:32.995+02:00 | A2 | raw-getStateInformation | 0.47 | 10455 | open | stopped | message | `5aa0dbac7e371815a5381b4dbc765b4c05f1c440d34b7d8854c96ef613500c2a` |
| 2026-09-05T22:14:32.998+02:00 | A2 | raw-getStateInformation | 0.47 | 10455 | open | stopped | message | `5aa0dbac7e371815a5381b4dbc765b4c05f1c440d34b7d8854c96ef613500c2a` |
| 2026-09-05T22:14:33.000+02:00 | A2 | raw-getStateInformation | 0.45 | 10455 | open | stopped | message | `5aa0dbac7e371815a5381b4dbc765b4c05f1c440d34b7d8854c96ef613500c2a` |
| 2026-09-05T22:14:33.002+02:00 | A2 | raw-getStateInformation | 0.49 | 10455 | open | stopped | message | `5aa0dbac7e371815a5381b4dbc765b4c05f1c440d34b7d8854c96ef613500c2a` |
| 2026-09-05T22:14:33.005+02:00 | A2 | raw-getStateInformation | 0.46 | 10455 | open | stopped | message | `5aa0dbac7e371815a5381b4dbc765b4c05f1c440d34b7d8854c96ef613500c2a` |
| 2026-09-05T22:14:33.007+02:00 | A2 | raw-getStateInformation | 0.44 | 10455 | open | stopped | message | `5aa0dbac7e371815a5381b4dbc765b4c05f1c440d34b7d8854c96ef613500c2a` |
| 2026-09-05T22:14:33.009+02:00 | A2 | raw-getStateInformation | 0.44 | 10455 | open | stopped | message | `5aa0dbac7e371815a5381b4dbc765b4c05f1c440d34b7d8854c96ef613500c2a` |
| 2026-09-05T22:14:33.011+02:00 | A2 | raw-getStateInformation | 0.45 | 10455 | open | stopped | message | `5aa0dbac7e371815a5381b4dbc765b4c05f1c440d34b7d8854c96ef613500c2a` |
| 2026-09-05T22:14:39.714+02:00 | A3 | raw-getStateInformation | 0.98 | 10433 | open | playing | message | `a58813037e078963f40ecb0d1bc12553ed0262807258c41d74fcd2446eb349cc` |
| 2026-09-05T22:14:39.720+02:00 | A3 | raw-getStateInformation | 0.51 | 10433 | open | playing | message | `a58813037e078963f40ecb0d1bc12553ed0262807258c41d74fcd2446eb349cc` |
| 2026-09-05T22:14:39.725+02:00 | A3 | raw-getStateInformation | 0.49 | 10433 | open | playing | message | `a58813037e078963f40ecb0d1bc12553ed0262807258c41d74fcd2446eb349cc` |
| 2026-09-05T22:14:39.727+02:00 | A3 | raw-getStateInformation | 0.46 | 10433 | open | playing | message | `a58813037e078963f40ecb0d1bc12553ed0262807258c41d74fcd2446eb349cc` |
| 2026-09-05T22:14:39.730+02:00 | A3 | raw-getStateInformation | 0.46 | 10433 | open | playing | message | `a58813037e078963f40ecb0d1bc12553ed0262807258c41d74fcd2446eb349cc` |
| 2026-09-05T22:14:39.733+02:00 | A3 | raw-getStateInformation | 0.46 | 10433 | open | playing | message | `a58813037e078963f40ecb0d1bc12553ed0262807258c41d74fcd2446eb349cc` |
| 2026-09-05T22:14:39.737+02:00 | A3 | raw-getStateInformation | 0.47 | 10433 | open | playing | message | `a58813037e078963f40ecb0d1bc12553ed0262807258c41d74fcd2446eb349cc` |
| 2026-09-05T22:14:39.740+02:00 | A3 | raw-getStateInformation | 0.48 | 10433 | open | playing | message | `a58813037e078963f40ecb0d1bc12553ed0262807258c41d74fcd2446eb349cc` |
| 2026-09-05T22:14:39.743+02:00 | A3 | raw-getStateInformation | 0.45 | 10433 | open | playing | message | `a58813037e078963f40ecb0d1bc12553ed0262807258c41d74fcd2446eb349cc` |
| 2026-09-05T22:14:39.745+02:00 | A3 | raw-getStateInformation | 0.44 | 10433 | open | playing | message | `a58813037e078963f40ecb0d1bc12553ed0262807258c41d74fcd2446eb349cc` |
| 2026-09-05T22:14:42.231+02:00 | A3 | raw-getStateInformation | 0.58 | 10433 | open | playing | message | `a58813037e078963f40ecb0d1bc12553ed0262807258c41d74fcd2446eb349cc` |
| 2026-09-05T22:14:42.236+02:00 | A3 | raw-getStateInformation | 0.51 | 10433 | open | playing | message | `a58813037e078963f40ecb0d1bc12553ed0262807258c41d74fcd2446eb349cc` |
| 2026-09-05T22:14:42.240+02:00 | A3 | raw-getStateInformation | 0.49 | 10433 | open | playing | message | `a58813037e078963f40ecb0d1bc12553ed0262807258c41d74fcd2446eb349cc` |
| 2026-09-05T22:14:42.245+02:00 | A3 | raw-getStateInformation | 0.49 | 10433 | open | playing | message | `a58813037e078963f40ecb0d1bc12553ed0262807258c41d74fcd2446eb349cc` |
| 2026-09-05T22:14:42.248+02:00 | A3 | raw-getStateInformation | 0.47 | 10433 | open | playing | message | `a58813037e078963f40ecb0d1bc12553ed0262807258c41d74fcd2446eb349cc` |
| 2026-09-05T22:14:42.250+02:00 | A3 | raw-getStateInformation | 0.47 | 10433 | open | playing | message | `a58813037e078963f40ecb0d1bc12553ed0262807258c41d74fcd2446eb349cc` |
| 2026-09-05T22:14:42.253+02:00 | A3 | raw-getStateInformation | 0.46 | 10433 | open | playing | message | `a58813037e078963f40ecb0d1bc12553ed0262807258c41d74fcd2446eb349cc` |
| 2026-09-05T22:14:42.256+02:00 | A3 | raw-getStateInformation | 0.43 | 10433 | open | playing | message | `a58813037e078963f40ecb0d1bc12553ed0262807258c41d74fcd2446eb349cc` |
| 2026-09-05T22:14:42.258+02:00 | A3 | raw-getStateInformation | 0.45 | 10433 | open | playing | message | `a58813037e078963f40ecb0d1bc12553ed0262807258c41d74fcd2446eb349cc` |
| 2026-09-05T22:14:42.261+02:00 | A3 | raw-getStateInformation | 0.46 | 10433 | open | playing | message | `a58813037e078963f40ecb0d1bc12553ed0262807258c41d74fcd2446eb349cc` |
| 2026-09-05T22:14:43.105+02:00 | A3 | raw-getStateInformation | 0.56 | 10433 | open | playing | message | `a58813037e078963f40ecb0d1bc12553ed0262807258c41d74fcd2446eb349cc` |
| 2026-09-05T22:14:43.111+02:00 | A3 | raw-getStateInformation | 0.49 | 10433 | open | playing | message | `a58813037e078963f40ecb0d1bc12553ed0262807258c41d74fcd2446eb349cc` |
| 2026-09-05T22:14:43.115+02:00 | A3 | raw-getStateInformation | 0.44 | 10433 | open | playing | message | `a58813037e078963f40ecb0d1bc12553ed0262807258c41d74fcd2446eb349cc` |
| 2026-09-05T22:14:43.119+02:00 | A3 | raw-getStateInformation | 0.46 | 10433 | open | playing | message | `a58813037e078963f40ecb0d1bc12553ed0262807258c41d74fcd2446eb349cc` |
| 2026-09-05T22:14:43.124+02:00 | A3 | raw-getStateInformation | 0.50 | 10433 | open | playing | message | `a58813037e078963f40ecb0d1bc12553ed0262807258c41d74fcd2446eb349cc` |
| 2026-09-05T22:14:43.126+02:00 | A3 | raw-getStateInformation | 0.44 | 10433 | open | playing | message | `a58813037e078963f40ecb0d1bc12553ed0262807258c41d74fcd2446eb349cc` |
| 2026-09-05T22:14:43.129+02:00 | A3 | raw-getStateInformation | 0.46 | 10433 | open | playing | message | `a58813037e078963f40ecb0d1bc12553ed0262807258c41d74fcd2446eb349cc` |
| 2026-09-05T22:14:43.131+02:00 | A3 | raw-getStateInformation | 0.46 | 10433 | open | playing | message | `a58813037e078963f40ecb0d1bc12553ed0262807258c41d74fcd2446eb349cc` |
| 2026-09-05T22:14:43.134+02:00 | A3 | raw-getStateInformation | 0.44 | 10433 | open | playing | message | `a58813037e078963f40ecb0d1bc12553ed0262807258c41d74fcd2446eb349cc` |
| 2026-09-05T22:14:43.136+02:00 | A3 | raw-getStateInformation | 0.45 | 10433 | open | playing | message | `a58813037e078963f40ecb0d1bc12553ed0262807258c41d74fcd2446eb349cc` |
| 2026-09-05T22:14:57.610+02:00 | B5 | raw-getStateInformation | 0.56 | 10433 | open | stopped | message | `a58813037e078963f40ecb0d1bc12553ed0262807258c41d74fcd2446eb349cc` |
| 2026-09-05T22:14:57.616+02:00 | B5 | raw-getStateInformation | 0.52 | 10433 | open | stopped | message | `a58813037e078963f40ecb0d1bc12553ed0262807258c41d74fcd2446eb349cc` |
| 2026-09-05T22:14:57.621+02:00 | B5 | raw-getStateInformation | 0.52 | 10433 | open | stopped | message | `a58813037e078963f40ecb0d1bc12553ed0262807258c41d74fcd2446eb349cc` |
| 2026-09-05T22:14:57.625+02:00 | B5 | raw-getStateInformation | 0.47 | 10433 | open | stopped | message | `a58813037e078963f40ecb0d1bc12553ed0262807258c41d74fcd2446eb349cc` |
| 2026-09-05T22:14:57.630+02:00 | B5 | raw-getStateInformation | 0.48 | 10433 | open | stopped | message | `a58813037e078963f40ecb0d1bc12553ed0262807258c41d74fcd2446eb349cc` |
| 2026-09-05T22:14:57.635+02:00 | B5 | raw-getStateInformation | 0.66 | 10433 | open | stopped | message | `a58813037e078963f40ecb0d1bc12553ed0262807258c41d74fcd2446eb349cc` |
| 2026-09-05T22:14:57.641+02:00 | B5 | raw-getStateInformation | 0.49 | 10433 | open | stopped | message | `a58813037e078963f40ecb0d1bc12553ed0262807258c41d74fcd2446eb349cc` |
| 2026-09-05T22:14:57.644+02:00 | B5 | raw-getStateInformation | 0.50 | 10433 | open | stopped | message | `a58813037e078963f40ecb0d1bc12553ed0262807258c41d74fcd2446eb349cc` |
| 2026-09-05T22:14:57.646+02:00 | B5 | raw-getStateInformation | 0.47 | 10433 | open | stopped | message | `a58813037e078963f40ecb0d1bc12553ed0262807258c41d74fcd2446eb349cc` |
| 2026-09-05T22:14:57.649+02:00 | B5 | raw-getStateInformation | 0.46 | 10433 | open | stopped | message | `a58813037e078963f40ecb0d1bc12553ed0262807258c41d74fcd2446eb349cc` |
| 2026-09-05T22:15:13.976+02:00 | B5 | raw-getStateInformation | 0.84 | 10433 | open | stopped | message | `a58813037e078963f40ecb0d1bc12553ed0262807258c41d74fcd2446eb349cc` |
| 2026-09-05T22:15:17.064+02:00 | A4 | raw-getStateInformation | 0.99 | 10433 | open | stopped | message | `a58813037e078963f40ecb0d1bc12553ed0262807258c41d74fcd2446eb349cc` |
| 2026-09-05T22:15:17.071+02:00 | A4 | raw-getStateInformation | 0.51 | 10433 | open | stopped | message | `a58813037e078963f40ecb0d1bc12553ed0262807258c41d74fcd2446eb349cc` |
| 2026-09-05T22:15:17.076+02:00 | A4 | raw-getStateInformation | 0.45 | 10433 | open | stopped | message | `a58813037e078963f40ecb0d1bc12553ed0262807258c41d74fcd2446eb349cc` |
| 2026-09-05T22:15:17.081+02:00 | A4 | raw-getStateInformation | 0.46 | 10433 | open | stopped | message | `a58813037e078963f40ecb0d1bc12553ed0262807258c41d74fcd2446eb349cc` |
| 2026-09-05T22:15:17.085+02:00 | A4 | raw-getStateInformation | 0.45 | 10433 | open | stopped | message | `a58813037e078963f40ecb0d1bc12553ed0262807258c41d74fcd2446eb349cc` |
| 2026-09-05T22:15:17.090+02:00 | A4 | raw-getStateInformation | 0.49 | 10433 | open | stopped | message | `a58813037e078963f40ecb0d1bc12553ed0262807258c41d74fcd2446eb349cc` |
| 2026-09-05T22:15:17.095+02:00 | A4 | raw-getStateInformation | 0.46 | 10433 | open | stopped | message | `a58813037e078963f40ecb0d1bc12553ed0262807258c41d74fcd2446eb349cc` |
| 2026-09-05T22:15:17.099+02:00 | A4 | raw-getStateInformation | 0.48 | 10433 | open | stopped | message | `a58813037e078963f40ecb0d1bc12553ed0262807258c41d74fcd2446eb349cc` |
| 2026-09-05T22:15:17.102+02:00 | A4 | raw-getStateInformation | 0.47 | 10433 | open | stopped | message | `a58813037e078963f40ecb0d1bc12553ed0262807258c41d74fcd2446eb349cc` |
| 2026-09-05T22:15:17.107+02:00 | A4 | raw-getStateInformation | 0.45 | 10433 | open | stopped | message | `a58813037e078963f40ecb0d1bc12553ed0262807258c41d74fcd2446eb349cc` |
| 2026-09-05T22:15:51.564+02:00 | A5 | raw-getStateInformation | 0.56 | 10453 | open | stopped | message | `33a537eff6ece8ed514a4f8b67167d86727dc7f5881d7cec642c95faa3965e92` |
| 2026-09-05T22:16:14.012+02:00 | B4 | raw-getStateInformation | 1.01 | 10433 | open | stopped | message | `a58813037e078963f40ecb0d1bc12553ed0262807258c41d74fcd2446eb349cc` |
| 2026-09-05T22:16:14.018+02:00 | B4 | raw-getStateInformation | 0.50 | 10433 | open | stopped | message | `a58813037e078963f40ecb0d1bc12553ed0262807258c41d74fcd2446eb349cc` |
| 2026-09-05T22:16:14.022+02:00 | B4 | raw-getStateInformation | 0.46 | 10433 | open | stopped | message | `a58813037e078963f40ecb0d1bc12553ed0262807258c41d74fcd2446eb349cc` |
| 2026-09-05T22:16:14.025+02:00 | B4 | raw-getStateInformation | 0.44 | 10433 | open | stopped | message | `a58813037e078963f40ecb0d1bc12553ed0262807258c41d74fcd2446eb349cc` |
| 2026-09-05T22:16:14.029+02:00 | B4 | raw-getStateInformation | 0.44 | 10433 | open | stopped | message | `a58813037e078963f40ecb0d1bc12553ed0262807258c41d74fcd2446eb349cc` |
| 2026-09-05T22:16:14.033+02:00 | B4 | raw-getStateInformation | 0.47 | 10433 | open | stopped | message | `a58813037e078963f40ecb0d1bc12553ed0262807258c41d74fcd2446eb349cc` |
| 2026-09-05T22:16:14.035+02:00 | B4 | raw-getStateInformation | 0.45 | 10433 | open | stopped | message | `a58813037e078963f40ecb0d1bc12553ed0262807258c41d74fcd2446eb349cc` |
| 2026-09-05T22:16:14.038+02:00 | B4 | raw-getStateInformation | 0.45 | 10433 | open | stopped | message | `a58813037e078963f40ecb0d1bc12553ed0262807258c41d74fcd2446eb349cc` |
| 2026-09-05T22:16:14.040+02:00 | B4 | raw-getStateInformation | 0.43 | 10433 | open | stopped | message | `a58813037e078963f40ecb0d1bc12553ed0262807258c41d74fcd2446eb349cc` |
| 2026-09-05T22:16:14.044+02:00 | B4 | raw-getStateInformation | 0.47 | 10433 | open | stopped | message | `a58813037e078963f40ecb0d1bc12553ed0262807258c41d74fcd2446eb349cc` |
| 2026-09-05T22:16:24.909+02:00 | B4 | raw-getStateInformation | 0.54 | 10433 | closed | stopped | message | `a58813037e078963f40ecb0d1bc12553ed0262807258c41d74fcd2446eb349cc` |
| 2026-09-05T22:16:24.914+02:00 | B4 | raw-getStateInformation | 0.52 | 10433 | closed | stopped | message | `a58813037e078963f40ecb0d1bc12553ed0262807258c41d74fcd2446eb349cc` |
| 2026-09-05T22:16:24.918+02:00 | B4 | raw-getStateInformation | 0.45 | 10433 | closed | stopped | message | `a58813037e078963f40ecb0d1bc12553ed0262807258c41d74fcd2446eb349cc` |
| 2026-09-05T22:16:24.922+02:00 | B4 | raw-getStateInformation | 0.45 | 10433 | closed | stopped | message | `a58813037e078963f40ecb0d1bc12553ed0262807258c41d74fcd2446eb349cc` |
| 2026-09-05T22:16:24.925+02:00 | B4 | raw-getStateInformation | 0.46 | 10433 | closed | stopped | message | `a58813037e078963f40ecb0d1bc12553ed0262807258c41d74fcd2446eb349cc` |
| 2026-09-05T22:16:24.927+02:00 | B4 | raw-getStateInformation | 0.44 | 10433 | closed | stopped | message | `a58813037e078963f40ecb0d1bc12553ed0262807258c41d74fcd2446eb349cc` |
| 2026-09-05T22:16:24.929+02:00 | B4 | raw-getStateInformation | 0.70 | 10433 | closed | stopped | message | `a58813037e078963f40ecb0d1bc12553ed0262807258c41d74fcd2446eb349cc` |
| 2026-09-05T22:16:24.932+02:00 | B4 | raw-getStateInformation | 0.48 | 10433 | closed | stopped | message | `a58813037e078963f40ecb0d1bc12553ed0262807258c41d74fcd2446eb349cc` |
| 2026-09-05T22:16:24.935+02:00 | B4 | raw-getStateInformation | 0.45 | 10433 | closed | stopped | message | `a58813037e078963f40ecb0d1bc12553ed0262807258c41d74fcd2446eb349cc` |
| 2026-09-05T22:16:24.939+02:00 | B4 | raw-getStateInformation | 0.47 | 10433 | closed | stopped | message | `a58813037e078963f40ecb0d1bc12553ed0262807258c41d74fcd2446eb349cc` |
| 2026-09-05T22:16:56.527+02:00 | B2 | raw-getStateInformation | 0.59 | 10433 | closed | stopped | message | `a58813037e078963f40ecb0d1bc12553ed0262807258c41d74fcd2446eb349cc` |
| 2026-09-05T22:16:56.532+02:00 | B2 | raw-getStateInformation | 0.55 | 10433 | closed | stopped | message | `a58813037e078963f40ecb0d1bc12553ed0262807258c41d74fcd2446eb349cc` |
| 2026-09-05T22:16:56.535+02:00 | B2 | raw-getStateInformation | 0.48 | 10433 | closed | stopped | message | `a58813037e078963f40ecb0d1bc12553ed0262807258c41d74fcd2446eb349cc` |
| 2026-09-05T22:16:56.538+02:00 | B2 | raw-getStateInformation | 0.51 | 10433 | closed | stopped | message | `a58813037e078963f40ecb0d1bc12553ed0262807258c41d74fcd2446eb349cc` |
| 2026-09-05T22:16:56.540+02:00 | B2 | raw-getStateInformation | 0.45 | 10433 | closed | stopped | message | `a58813037e078963f40ecb0d1bc12553ed0262807258c41d74fcd2446eb349cc` |
| 2026-09-05T22:16:56.543+02:00 | B2 | raw-getStateInformation | 0.46 | 10433 | closed | stopped | message | `a58813037e078963f40ecb0d1bc12553ed0262807258c41d74fcd2446eb349cc` |
| 2026-09-05T22:16:56.546+02:00 | B2 | raw-getStateInformation | 0.48 | 10433 | closed | stopped | message | `a58813037e078963f40ecb0d1bc12553ed0262807258c41d74fcd2446eb349cc` |
| 2026-09-05T22:16:56.549+02:00 | B2 | raw-getStateInformation | 0.49 | 10433 | closed | stopped | message | `a58813037e078963f40ecb0d1bc12553ed0262807258c41d74fcd2446eb349cc` |
| 2026-09-05T22:16:56.552+02:00 | B2 | raw-getStateInformation | 0.51 | 10433 | closed | stopped | message | `a58813037e078963f40ecb0d1bc12553ed0262807258c41d74fcd2446eb349cc` |
| 2026-09-05T22:16:56.555+02:00 | B2 | raw-getStateInformation | 0.48 | 10433 | closed | stopped | message | `a58813037e078963f40ecb0d1bc12553ed0262807258c41d74fcd2446eb349cc` |
| 2026-09-05T22:17:03.170+02:00 | B2 | raw-getStateInformation | 0.58 | 10433 | open | stopped | message | `a58813037e078963f40ecb0d1bc12553ed0262807258c41d74fcd2446eb349cc` |
| 2026-09-05T22:17:03.175+02:00 | B2 | raw-getStateInformation | 0.53 | 10433 | open | stopped | message | `a58813037e078963f40ecb0d1bc12553ed0262807258c41d74fcd2446eb349cc` |
| 2026-09-05T22:17:03.180+02:00 | B2 | raw-getStateInformation | 0.49 | 10433 | open | stopped | message | `a58813037e078963f40ecb0d1bc12553ed0262807258c41d74fcd2446eb349cc` |
| 2026-09-05T22:17:03.183+02:00 | B2 | raw-getStateInformation | 0.45 | 10433 | open | stopped | message | `a58813037e078963f40ecb0d1bc12553ed0262807258c41d74fcd2446eb349cc` |
| 2026-09-05T22:17:03.187+02:00 | B2 | raw-getStateInformation | 0.97 | 10433 | open | stopped | message | `a58813037e078963f40ecb0d1bc12553ed0262807258c41d74fcd2446eb349cc` |
| 2026-09-05T22:17:03.191+02:00 | B2 | raw-getStateInformation | 0.54 | 10433 | open | stopped | message | `a58813037e078963f40ecb0d1bc12553ed0262807258c41d74fcd2446eb349cc` |
| 2026-09-05T22:17:03.194+02:00 | B2 | raw-getStateInformation | 0.51 | 10433 | open | stopped | message | `a58813037e078963f40ecb0d1bc12553ed0262807258c41d74fcd2446eb349cc` |
| 2026-09-05T22:17:03.197+02:00 | B2 | raw-getStateInformation | 0.46 | 10433 | open | stopped | message | `a58813037e078963f40ecb0d1bc12553ed0262807258c41d74fcd2446eb349cc` |
| 2026-09-05T22:17:03.199+02:00 | B2 | raw-getStateInformation | 0.45 | 10433 | open | stopped | message | `a58813037e078963f40ecb0d1bc12553ed0262807258c41d74fcd2446eb349cc` |
| 2026-09-05T22:17:03.202+02:00 | B2 | raw-getStateInformation | 0.52 | 10433 | open | stopped | message | `a58813037e078963f40ecb0d1bc12553ed0262807258c41d74fcd2446eb349cc` |
| 2026-09-05T22:17:18.671+02:00 | E1 | raw-getStateInformation | 0.54 | 10393 | open | stopped | message | `875dc964caa630252767e6b26bf8a54ab2d64a2fc93a6112174924e0c3633aee` |
| 2026-09-05T22:17:36.374+02:00 | E2 | raw-getStateInformation | 0.56 | 10393 | open | stopped | message | `875dc964caa630252767e6b26bf8a54ab2d64a2fc93a6112174924e0c3633aee` |
| 2026-09-05T22:17:49.247+02:00 | F1 | raw-getStateInformation | 0.56 | 10393 | open | stopped | message | `875dc964caa630252767e6b26bf8a54ab2d64a2fc93a6112174924e0c3633aee` |
| 2026-09-05T22:17:49.257+02:00 | F1 | save-path-base64 | 0.55 | 10393 | open | stopped | message | `875dc964caa630252767e6b26bf8a54ab2d64a2fc93a6112174924e0c3633aee` |

## Operator notes

*None.*

---
*End of sanitized SPIKE-01 report. Verify before sharing: this file must
contain no base64 blocks and no plugin-state byte dumps.*
