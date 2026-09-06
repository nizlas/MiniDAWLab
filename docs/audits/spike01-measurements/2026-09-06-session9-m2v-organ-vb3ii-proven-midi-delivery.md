# SPIKE-01 sanitized measurement report — authoritative plugin-state capture

SPIKE-01 diagnostic output (roadmap slice P0/P1A; canonical steering §9.2 / PID-001).

**Privacy:** this report contains only sizes, SHA-256 hashes, timings, and
notification metadata. No raw plugin-state bytes, presets, or licensing
material are captured to disk by the SPIKE-01 probe.

## Environment

| Field | Value |
|---|---|
| Generated | 2026-09-06T00:21:41.368+02:00 |
| App version | 0.9.0 |
| Plugin | VB3-II |
| Plugin format | VST3 |
| Plugin version | 2.3.1 |
| Plugin identifier | C:\Program Files\Common Files\VST3\VB3-II.vst3 |
| Operator notes |  |

## Capture measurements per phase

| Phase / path | n | min ms | median ms | p95 ms | max ms | blob bytes (min..max) | distinct hashes | byte-stable |
|---|---|---|---|---|---|---|---|---|
| M2V-late / raw-getStateInformation | 20 | 0.43 | 0.47 | 1.05 | 1.22 | 10393..10393 | 1 | yes |
| M2V-play / raw-getStateInformation | 36 | 0.50 | 0.54 | 1.03 | 1.17 | 10393..10415 | 9 | NO |
| M2V-pre / raw-getStateInformation | 10 | 0.44 | 0.47 | 0.53 | 0.53 | 10393..10393 | 1 | yes |
| M2V-stop / raw-getStateInformation | 6 | 0.51 | 0.53 | 0.74 | 0.74 | 10393..10393 | 1 | yes |

## Hash inventory (12-hex prefixes; full hashes in raw sample log)

- **M2V-late**: `875dc964caa6…`
- **M2V-play**: `09ba00347f3a…`, `12fd74cbff5b…`, `20ecc24bf680…`, `40ee6a58fae1…`, `875dc964caa6…`, `8879d2d721f2…`, `9079686ddff7…`, `dad4f30b4a51…`, `f5496f18aff6…`
- **M2V-pre**: `875dc964caa6…`
- **M2V-stop**: `875dc964caa6…`

## Threading

- Captures executed on the message thread: 72
- Captures executed on other threads: 0 (expected: 0)
- Parameter/processor notifications on the message thread: 0
- Parameter/processor notifications on other threads: 312

## Parameter/processor notifications (312)

| Time | Kind | Param idx | Param name | Value | Thread | Detail |
|---|---|---|---|---|---|---|
| 2026-09-06T00:21:22.600+02:00 | paramChanged | 54 |  | 0.43 | other |  |
| 2026-09-06T00:21:22.620+02:00 | paramChanged | 54 |  | 0.43 | other |  |
| 2026-09-06T00:21:22.630+02:00 | paramChanged | 54 |  | 0.42 | other |  |
| 2026-09-06T00:21:22.640+02:00 | paramChanged | 54 |  | 0.41 | other |  |
| 2026-09-06T00:21:22.649+02:00 | paramChanged | 54 |  | 0.40 | other |  |
| 2026-09-06T00:21:22.660+02:00 | paramChanged | 54 |  | 0.39 | other |  |
| 2026-09-06T00:21:22.680+02:00 | paramChanged | 54 |  | 0.39 | other |  |
| 2026-09-06T00:21:22.690+02:00 | paramChanged | 54 |  | 0.38 | other |  |
| 2026-09-06T00:21:22.700+02:00 | paramChanged | 54 |  | 0.37 | other |  |
| 2026-09-06T00:21:22.709+02:00 | paramChanged | 54 |  | 0.36 | other |  |
| 2026-09-06T00:21:22.719+02:00 | paramChanged | 54 |  | 0.35 | other |  |
| 2026-09-06T00:21:22.740+02:00 | paramChanged | 54 |  | 0.35 | other |  |
| 2026-09-06T00:21:22.750+02:00 | paramChanged | 54 |  | 0.34 | other |  |
| 2026-09-06T00:21:22.760+02:00 | paramChanged | 54 |  | 0.33 | other |  |
| 2026-09-06T00:21:22.770+02:00 | paramChanged | 54 |  | 0.32 | other |  |
| 2026-09-06T00:21:22.780+02:00 | paramChanged | 54 |  | 0.31 | other |  |
| 2026-09-06T00:21:22.789+02:00 | paramChanged | 54 |  | 0.31 | other |  |
| 2026-09-06T00:21:22.809+02:00 | paramChanged | 54 |  | 0.30 | other |  |
| 2026-09-06T00:21:22.820+02:00 | paramChanged | 54 |  | 0.29 | other |  |
| 2026-09-06T00:21:22.829+02:00 | paramChanged | 54 |  | 0.28 | other |  |
| 2026-09-06T00:21:22.840+02:00 | paramChanged | 54 |  | 0.28 | other |  |
| 2026-09-06T00:21:22.850+02:00 | paramChanged | 54 |  | 0.27 | other |  |
| 2026-09-06T00:21:22.870+02:00 | paramChanged | 54 |  | 0.26 | other |  |
| 2026-09-06T00:21:22.880+02:00 | paramChanged | 54 |  | 0.25 | other |  |
| 2026-09-06T00:21:22.890+02:00 | paramChanged | 54 |  | 0.24 | other |  |
| 2026-09-06T00:21:22.899+02:00 | paramChanged | 54 |  | 0.24 | other |  |
| 2026-09-06T00:21:22.909+02:00 | paramChanged | 54 |  | 0.23 | other |  |
| 2026-09-06T00:21:22.930+02:00 | paramChanged | 54 |  | 0.22 | other |  |
| 2026-09-06T00:21:22.939+02:00 | paramChanged | 54 |  | 0.21 | other |  |
| 2026-09-06T00:21:22.950+02:00 | paramChanged | 54 |  | 0.20 | other |  |
| 2026-09-06T00:21:22.959+02:00 | paramChanged | 54 |  | 0.20 | other |  |
| 2026-09-06T00:21:22.969+02:00 | paramChanged | 54 |  | 0.19 | other |  |
| 2026-09-06T00:21:22.989+02:00 | paramChanged | 54 |  | 0.18 | other |  |
| 2026-09-06T00:21:23.000+02:00 | paramChanged | 54 |  | 0.17 | other |  |
| 2026-09-06T00:21:23.010+02:00 | paramChanged | 54 |  | 0.17 | other |  |
| 2026-09-06T00:21:23.020+02:00 | paramChanged | 54 |  | 0.16 | other |  |
| 2026-09-06T00:21:23.029+02:00 | paramChanged | 54 |  | 0.15 | other |  |
| 2026-09-06T00:21:23.040+02:00 | paramChanged | 54 |  | 0.14 | other |  |
| 2026-09-06T00:21:23.060+02:00 | paramChanged | 54 |  | 0.13 | other |  |
| 2026-09-06T00:21:23.070+02:00 | paramChanged | 54 |  | 0.13 | other |  |
| 2026-09-06T00:21:23.079+02:00 | paramChanged | 54 |  | 0.12 | other |  |
| 2026-09-06T00:21:23.090+02:00 | paramChanged | 54 |  | 0.11 | other |  |
| 2026-09-06T00:21:23.099+02:00 | paramChanged | 54 |  | 0.10 | other |  |
| 2026-09-06T00:21:23.119+02:00 | paramChanged | 54 |  | 0.09 | other |  |
| 2026-09-06T00:21:23.129+02:00 | paramChanged | 54 |  | 0.09 | other |  |
| 2026-09-06T00:21:23.140+02:00 | paramChanged | 54 |  | 0.08 | other |  |
| 2026-09-06T00:21:23.149+02:00 | paramChanged | 54 |  | 0.07 | other |  |
| 2026-09-06T00:21:23.160+02:00 | paramChanged | 54 |  | 0.06 | other |  |
| 2026-09-06T00:21:23.180+02:00 | paramChanged | 54 |  | 0.06 | other |  |
| 2026-09-06T00:21:23.190+02:00 | paramChanged | 54 |  | 0.05 | other |  |
| 2026-09-06T00:21:23.199+02:00 | paramChanged | 54 |  | 0.04 | other |  |
| 2026-09-06T00:21:23.209+02:00 | paramChanged | 54 |  | 0.03 | other |  |
| 2026-09-06T00:21:23.219+02:00 | paramChanged | 54 |  | 0.02 | other |  |
| 2026-09-06T00:21:23.239+02:00 | paramChanged | 54 |  | 0.02 | other |  |
| 2026-09-06T00:21:23.249+02:00 | paramChanged | 54 |  | 0.01 | other |  |
| 2026-09-06T00:21:23.259+02:00 | paramChanged | 54 |  | 0.00 | other |  |
| 2026-09-06T00:21:23.270+02:00 | paramChanged | 54 |  | 0.01 | other |  |
| 2026-09-06T00:21:23.280+02:00 | paramChanged | 54 |  | 0.02 | other |  |
| 2026-09-06T00:21:23.290+02:00 | paramChanged | 54 |  | 0.03 | other |  |
| 2026-09-06T00:21:23.300+02:00 | paramChanged | 54 |  | 0.04 | other |  |
| 2026-09-06T00:21:23.309+02:00 | paramChanged | 54 |  | 0.05 | other |  |
| 2026-09-06T00:21:23.320+02:00 | paramChanged | 54 |  | 0.06 | other |  |
| 2026-09-06T00:21:23.330+02:00 | paramChanged | 54 |  | 0.07 | other |  |
| 2026-09-06T00:21:23.340+02:00 | paramChanged | 54 |  | 0.08 | other |  |
| 2026-09-06T00:21:23.349+02:00 | paramChanged | 54 |  | 0.09 | other |  |
| 2026-09-06T00:21:23.360+02:00 | paramChanged | 54 |  | 0.10 | other |  |
| 2026-09-06T00:21:23.370+02:00 | paramChanged | 54 |  | 0.11 | other |  |
| 2026-09-06T00:21:23.380+02:00 | paramChanged | 54 |  | 0.12 | other |  |
| 2026-09-06T00:21:23.390+02:00 | paramChanged | 54 |  | 0.13 | other |  |
| 2026-09-06T00:21:23.399+02:00 | paramChanged | 54 |  | 0.14 | other |  |
| 2026-09-06T00:21:23.410+02:00 | paramChanged | 54 |  | 0.15 | other |  |
| 2026-09-06T00:21:23.420+02:00 | paramChanged | 54 |  | 0.16 | other |  |
| 2026-09-06T00:21:23.429+02:00 | paramChanged | 54 |  | 0.17 | other |  |
| 2026-09-06T00:21:23.439+02:00 | paramChanged | 54 |  | 0.18 | other |  |
| 2026-09-06T00:21:23.449+02:00 | paramChanged | 54 |  | 0.19 | other |  |
| 2026-09-06T00:21:23.461+02:00 | paramChanged | 54 |  | 0.20 | other |  |
| 2026-09-06T00:21:23.470+02:00 | paramChanged | 54 |  | 0.21 | other |  |
| 2026-09-06T00:21:23.480+02:00 | paramChanged | 54 |  | 0.22 | other |  |
| 2026-09-06T00:21:23.490+02:00 | paramChanged | 54 |  | 0.23 | other |  |
| 2026-09-06T00:21:23.500+02:00 | paramChanged | 54 |  | 0.24 | other |  |
| 2026-09-06T00:21:23.510+02:00 | paramChanged | 54 |  | 0.25 | other |  |
| 2026-09-06T00:21:23.519+02:00 | paramChanged | 54 |  | 0.26 | other |  |
| 2026-09-06T00:21:23.529+02:00 | paramChanged | 54 |  | 0.27 | other |  |
| 2026-09-06T00:21:23.541+02:00 | paramChanged | 54 |  | 0.28 | other |  |
| 2026-09-06T00:21:23.550+02:00 | paramChanged | 54 |  | 0.29 | other |  |
| 2026-09-06T00:21:23.559+02:00 | paramChanged | 54 |  | 0.30 | other |  |
| 2026-09-06T00:21:23.569+02:00 | paramChanged | 54 |  | 0.31 | other |  |
| 2026-09-06T00:21:23.579+02:00 | paramChanged | 54 |  | 0.32 | other |  |
| 2026-09-06T00:21:23.590+02:00 | paramChanged | 54 |  | 0.33 | other |  |
| 2026-09-06T00:21:23.600+02:00 | paramChanged | 54 |  | 0.34 | other |  |
| 2026-09-06T00:21:23.610+02:00 | paramChanged | 54 |  | 0.35 | other |  |
| 2026-09-06T00:21:23.620+02:00 | paramChanged | 54 |  | 0.36 | other |  |
| 2026-09-06T00:21:23.629+02:00 | paramChanged | 54 |  | 0.37 | other |  |
| 2026-09-06T00:21:23.640+02:00 | paramChanged | 54 |  | 0.38 | other |  |
| 2026-09-06T00:21:23.650+02:00 | paramChanged | 54 |  | 0.39 | other |  |
| 2026-09-06T00:21:23.660+02:00 | paramChanged | 54 |  | 0.40 | other |  |
| 2026-09-06T00:21:23.670+02:00 | paramChanged | 54 |  | 0.41 | other |  |
| 2026-09-06T00:21:23.680+02:00 | paramChanged | 54 |  | 0.42 | other |  |
| 2026-09-06T00:21:23.690+02:00 | paramChanged | 54 |  | 0.43 | other |  |
| 2026-09-06T00:21:23.700+02:00 | paramChanged | 54 |  | 0.44 | other |  |
| 2026-09-06T00:21:23.710+02:00 | paramChanged | 54 |  | 0.45 | other |  |
| 2026-09-06T00:21:23.720+02:00 | paramChanged | 54 |  | 0.46 | other |  |
| 2026-09-06T00:21:23.731+02:00 | paramChanged | 54 |  | 0.47 | other |  |
| 2026-09-06T00:21:23.741+02:00 | paramChanged | 54 |  | 0.48 | other |  |
| 2026-09-06T00:21:23.749+02:00 | paramChanged | 54 |  | 0.49 | other |  |
| 2026-09-06T00:21:23.759+02:00 | paramChanged | 54 |  | 0.50 | other |  |
| 2026-09-06T00:21:23.770+02:00 | paramChanged | 54 |  | 0.51 | other |  |
| 2026-09-06T00:21:23.780+02:00 | paramChanged | 54 |  | 0.52 | other |  |
| 2026-09-06T00:21:23.790+02:00 | paramChanged | 54 |  | 0.53 | other |  |
| 2026-09-06T00:21:23.799+02:00 | paramChanged | 54 |  | 0.54 | other |  |
| 2026-09-06T00:21:23.810+02:00 | paramChanged | 54 |  | 0.55 | other |  |
| 2026-09-06T00:21:23.820+02:00 | paramChanged | 54 |  | 0.56 | other |  |
| 2026-09-06T00:21:23.831+02:00 | paramChanged | 54 |  | 0.57 | other |  |
| 2026-09-06T00:21:23.839+02:00 | paramChanged | 54 |  | 0.58 | other |  |
| 2026-09-06T00:21:23.850+02:00 | paramChanged | 54 |  | 0.59 | other |  |
| 2026-09-06T00:21:23.859+02:00 | paramChanged | 54 |  | 0.60 | other |  |
| 2026-09-06T00:21:23.870+02:00 | paramChanged | 54 |  | 0.61 | other |  |
| 2026-09-06T00:21:23.880+02:00 | paramChanged | 54 |  | 0.62 | other |  |
| 2026-09-06T00:21:23.890+02:00 | paramChanged | 54 |  | 0.63 | other |  |
| 2026-09-06T00:21:23.900+02:00 | paramChanged | 54 |  | 0.64 | other |  |
| 2026-09-06T00:21:23.909+02:00 | paramChanged | 54 |  | 0.65 | other |  |
| 2026-09-06T00:21:23.919+02:00 | paramChanged | 54 |  | 0.66 | other |  |
| 2026-09-06T00:21:23.930+02:00 | paramChanged | 54 |  | 0.67 | other |  |
| 2026-09-06T00:21:23.940+02:00 | paramChanged | 54 |  | 0.68 | other |  |
| 2026-09-06T00:21:23.949+02:00 | paramChanged | 54 |  | 0.69 | other |  |
| 2026-09-06T00:21:23.960+02:00 | paramChanged | 54 |  | 0.70 | other |  |
| 2026-09-06T00:21:23.970+02:00 | paramChanged | 54 |  | 0.71 | other |  |
| 2026-09-06T00:21:23.980+02:00 | paramChanged | 54 |  | 0.72 | other |  |
| 2026-09-06T00:21:23.989+02:00 | paramChanged | 54 |  | 0.73 | other |  |
| 2026-09-06T00:21:24.000+02:00 | paramChanged | 54 |  | 0.74 | other |  |
| 2026-09-06T00:21:24.010+02:00 | paramChanged | 54 |  | 0.75 | other |  |
| 2026-09-06T00:21:24.021+02:00 | paramChanged | 54 |  | 0.76 | other |  |
| 2026-09-06T00:21:24.029+02:00 | paramChanged | 54 |  | 0.77 | other |  |
| 2026-09-06T00:21:24.039+02:00 | paramChanged | 54 |  | 0.78 | other |  |
| 2026-09-06T00:21:24.050+02:00 | paramChanged | 54 |  | 0.79 | other |  |
| 2026-09-06T00:21:24.059+02:00 | paramChanged | 54 |  | 0.80 | other |  |
| 2026-09-06T00:21:24.069+02:00 | paramChanged | 54 |  | 0.81 | other |  |
| 2026-09-06T00:21:24.080+02:00 | paramChanged | 54 |  | 0.82 | other |  |
| 2026-09-06T00:21:24.089+02:00 | paramChanged | 54 |  | 0.83 | other |  |
| 2026-09-06T00:21:24.099+02:00 | paramChanged | 54 |  | 0.84 | other |  |
| 2026-09-06T00:21:24.110+02:00 | paramChanged | 54 |  | 0.85 | other |  |
| 2026-09-06T00:21:24.121+02:00 | paramChanged | 54 |  | 0.86 | other |  |
| 2026-09-06T00:21:24.130+02:00 | paramChanged | 54 |  | 0.87 | other |  |
| 2026-09-06T00:21:24.139+02:00 | paramChanged | 54 |  | 0.88 | other |  |
| 2026-09-06T00:21:24.149+02:00 | paramChanged | 54 |  | 0.89 | other |  |
| 2026-09-06T00:21:24.159+02:00 | paramChanged | 54 |  | 0.90 | other |  |
| 2026-09-06T00:21:24.170+02:00 | paramChanged | 54 |  | 0.91 | other |  |
| 2026-09-06T00:21:24.180+02:00 | paramChanged | 54 |  | 0.92 | other |  |
| 2026-09-06T00:21:24.190+02:00 | paramChanged | 54 |  | 0.93 | other |  |
| 2026-09-06T00:21:24.199+02:00 | paramChanged | 54 |  | 0.94 | other |  |
| 2026-09-06T00:21:24.210+02:00 | paramChanged | 54 |  | 0.95 | other |  |
| 2026-09-06T00:21:24.221+02:00 | paramChanged | 54 |  | 0.96 | other |  |
| 2026-09-06T00:21:24.231+02:00 | paramChanged | 54 |  | 0.97 | other |  |
| 2026-09-06T00:21:24.241+02:00 | paramChanged | 54 |  | 0.98 | other |  |
| 2026-09-06T00:21:24.250+02:00 | paramChanged | 54 |  | 0.99 | other |  |
| 2026-09-06T00:21:24.259+02:00 | paramChanged | 54 |  | 1.00 | other |  |
| 2026-09-06T00:21:28.600+02:00 | paramChanged | 54 |  | 0.43 | other |  |
| 2026-09-06T00:21:28.619+02:00 | paramChanged | 54 |  | 0.43 | other |  |
| 2026-09-06T00:21:28.629+02:00 | paramChanged | 54 |  | 0.42 | other |  |
| 2026-09-06T00:21:28.639+02:00 | paramChanged | 54 |  | 0.41 | other |  |
| 2026-09-06T00:21:28.650+02:00 | paramChanged | 54 |  | 0.40 | other |  |
| 2026-09-06T00:21:28.660+02:00 | paramChanged | 54 |  | 0.39 | other |  |
| 2026-09-06T00:21:28.680+02:00 | paramChanged | 54 |  | 0.39 | other |  |
| 2026-09-06T00:21:28.689+02:00 | paramChanged | 54 |  | 0.38 | other |  |
| 2026-09-06T00:21:28.700+02:00 | paramChanged | 54 |  | 0.37 | other |  |
| 2026-09-06T00:21:28.710+02:00 | paramChanged | 54 |  | 0.36 | other |  |
| 2026-09-06T00:21:28.720+02:00 | paramChanged | 54 |  | 0.35 | other |  |
| 2026-09-06T00:21:28.740+02:00 | paramChanged | 54 |  | 0.35 | other |  |
| 2026-09-06T00:21:28.749+02:00 | paramChanged | 54 |  | 0.34 | other |  |
| 2026-09-06T00:21:28.759+02:00 | paramChanged | 54 |  | 0.33 | other |  |
| 2026-09-06T00:21:28.770+02:00 | paramChanged | 54 |  | 0.32 | other |  |
| 2026-09-06T00:21:28.779+02:00 | paramChanged | 54 |  | 0.31 | other |  |
| 2026-09-06T00:21:28.790+02:00 | paramChanged | 54 |  | 0.31 | other |  |
| 2026-09-06T00:21:28.809+02:00 | paramChanged | 54 |  | 0.30 | other |  |
| 2026-09-06T00:21:28.820+02:00 | paramChanged | 54 |  | 0.29 | other |  |
| 2026-09-06T00:21:28.830+02:00 | paramChanged | 54 |  | 0.28 | other |  |
| 2026-09-06T00:21:28.840+02:00 | paramChanged | 54 |  | 0.28 | other |  |
| 2026-09-06T00:21:28.849+02:00 | paramChanged | 54 |  | 0.27 | other |  |
| 2026-09-06T00:21:28.870+02:00 | paramChanged | 54 |  | 0.26 | other |  |
| 2026-09-06T00:21:28.879+02:00 | paramChanged | 54 |  | 0.25 | other |  |
| 2026-09-06T00:21:28.890+02:00 | paramChanged | 54 |  | 0.24 | other |  |
| 2026-09-06T00:21:28.900+02:00 | paramChanged | 54 |  | 0.24 | other |  |
| 2026-09-06T00:21:28.910+02:00 | paramChanged | 54 |  | 0.23 | other |  |
| 2026-09-06T00:21:28.930+02:00 | paramChanged | 54 |  | 0.22 | other |  |
| 2026-09-06T00:21:28.939+02:00 | paramChanged | 54 |  | 0.21 | other |  |
| 2026-09-06T00:21:28.949+02:00 | paramChanged | 54 |  | 0.20 | other |  |
| 2026-09-06T00:21:28.959+02:00 | paramChanged | 54 |  | 0.20 | other |  |
| 2026-09-06T00:21:28.970+02:00 | paramChanged | 54 |  | 0.19 | other |  |
| 2026-09-06T00:21:28.989+02:00 | paramChanged | 54 |  | 0.18 | other |  |
| 2026-09-06T00:21:28.999+02:00 | paramChanged | 54 |  | 0.17 | other |  |
| 2026-09-06T00:21:29.009+02:00 | paramChanged | 54 |  | 0.17 | other |  |
| 2026-09-06T00:21:29.020+02:00 | paramChanged | 54 |  | 0.16 | other |  |
| 2026-09-06T00:21:29.030+02:00 | paramChanged | 54 |  | 0.15 | other |  |
| 2026-09-06T00:21:29.039+02:00 | paramChanged | 54 |  | 0.14 | other |  |
| 2026-09-06T00:21:29.060+02:00 | paramChanged | 54 |  | 0.13 | other |  |
| 2026-09-06T00:21:29.070+02:00 | paramChanged | 54 |  | 0.13 | other |  |
| 2026-09-06T00:21:29.079+02:00 | paramChanged | 54 |  | 0.12 | other |  |
| 2026-09-06T00:21:29.089+02:00 | paramChanged | 54 |  | 0.11 | other |  |
| 2026-09-06T00:21:29.099+02:00 | paramChanged | 54 |  | 0.10 | other |  |
| 2026-09-06T00:21:29.119+02:00 | paramChanged | 54 |  | 0.09 | other |  |
| 2026-09-06T00:21:29.129+02:00 | paramChanged | 54 |  | 0.09 | other |  |
| 2026-09-06T00:21:29.139+02:00 | paramChanged | 54 |  | 0.08 | other |  |
| 2026-09-06T00:21:29.149+02:00 | paramChanged | 54 |  | 0.07 | other |  |
| 2026-09-06T00:21:29.160+02:00 | paramChanged | 54 |  | 0.06 | other |  |
| 2026-09-06T00:21:29.179+02:00 | paramChanged | 54 |  | 0.06 | other |  |
| 2026-09-06T00:21:29.190+02:00 | paramChanged | 54 |  | 0.05 | other |  |
| 2026-09-06T00:21:29.200+02:00 | paramChanged | 54 |  | 0.04 | other |  |
| 2026-09-06T00:21:29.210+02:00 | paramChanged | 54 |  | 0.03 | other |  |
| 2026-09-06T00:21:29.219+02:00 | paramChanged | 54 |  | 0.02 | other |  |
| 2026-09-06T00:21:29.239+02:00 | paramChanged | 54 |  | 0.02 | other |  |
| 2026-09-06T00:21:29.250+02:00 | paramChanged | 54 |  | 0.01 | other |  |
| 2026-09-06T00:21:29.259+02:00 | paramChanged | 54 |  | 0.00 | other |  |
| 2026-09-06T00:21:29.270+02:00 | paramChanged | 54 |  | 0.01 | other |  |
| 2026-09-06T00:21:29.279+02:00 | paramChanged | 54 |  | 0.02 | other |  |
| 2026-09-06T00:21:29.290+02:00 | paramChanged | 54 |  | 0.03 | other |  |
| 2026-09-06T00:21:29.300+02:00 | paramChanged | 54 |  | 0.04 | other |  |
| 2026-09-06T00:21:29.309+02:00 | paramChanged | 54 |  | 0.05 | other |  |
| 2026-09-06T00:21:29.320+02:00 | paramChanged | 54 |  | 0.06 | other |  |
| 2026-09-06T00:21:29.329+02:00 | paramChanged | 54 |  | 0.07 | other |  |
| 2026-09-06T00:21:29.340+02:00 | paramChanged | 54 |  | 0.08 | other |  |
| 2026-09-06T00:21:29.349+02:00 | paramChanged | 54 |  | 0.09 | other |  |
| 2026-09-06T00:21:29.360+02:00 | paramChanged | 54 |  | 0.10 | other |  |
| 2026-09-06T00:21:29.370+02:00 | paramChanged | 54 |  | 0.11 | other |  |
| 2026-09-06T00:21:29.379+02:00 | paramChanged | 54 |  | 0.12 | other |  |
| 2026-09-06T00:21:29.389+02:00 | paramChanged | 54 |  | 0.13 | other |  |
| 2026-09-06T00:21:29.399+02:00 | paramChanged | 54 |  | 0.14 | other |  |
| 2026-09-06T00:21:29.410+02:00 | paramChanged | 54 |  | 0.15 | other |  |
| 2026-09-06T00:21:29.420+02:00 | paramChanged | 54 |  | 0.16 | other |  |
| 2026-09-06T00:21:29.430+02:00 | paramChanged | 54 |  | 0.17 | other |  |
| 2026-09-06T00:21:29.439+02:00 | paramChanged | 54 |  | 0.18 | other |  |
| 2026-09-06T00:21:29.449+02:00 | paramChanged | 54 |  | 0.19 | other |  |
| 2026-09-06T00:21:29.459+02:00 | paramChanged | 54 |  | 0.20 | other |  |
| 2026-09-06T00:21:29.470+02:00 | paramChanged | 54 |  | 0.21 | other |  |
| 2026-09-06T00:21:29.480+02:00 | paramChanged | 54 |  | 0.22 | other |  |
| 2026-09-06T00:21:29.490+02:00 | paramChanged | 54 |  | 0.23 | other |  |
| 2026-09-06T00:21:29.500+02:00 | paramChanged | 54 |  | 0.24 | other |  |
| 2026-09-06T00:21:29.510+02:00 | paramChanged | 54 |  | 0.25 | other |  |
| 2026-09-06T00:21:29.520+02:00 | paramChanged | 54 |  | 0.26 | other |  |
| 2026-09-06T00:21:29.530+02:00 | paramChanged | 54 |  | 0.27 | other |  |
| 2026-09-06T00:21:29.540+02:00 | paramChanged | 54 |  | 0.28 | other |  |
| 2026-09-06T00:21:29.551+02:00 | paramChanged | 54 |  | 0.29 | other |  |
| 2026-09-06T00:21:29.560+02:00 | paramChanged | 54 |  | 0.30 | other |  |
| 2026-09-06T00:21:29.570+02:00 | paramChanged | 54 |  | 0.31 | other |  |
| 2026-09-06T00:21:29.579+02:00 | paramChanged | 54 |  | 0.32 | other |  |
| 2026-09-06T00:21:29.589+02:00 | paramChanged | 54 |  | 0.33 | other |  |
| 2026-09-06T00:21:29.599+02:00 | paramChanged | 54 |  | 0.34 | other |  |
| 2026-09-06T00:21:29.610+02:00 | paramChanged | 54 |  | 0.35 | other |  |
| 2026-09-06T00:21:29.619+02:00 | paramChanged | 54 |  | 0.36 | other |  |
| 2026-09-06T00:21:29.629+02:00 | paramChanged | 54 |  | 0.37 | other |  |
| 2026-09-06T00:21:29.640+02:00 | paramChanged | 54 |  | 0.38 | other |  |
| 2026-09-06T00:21:29.649+02:00 | paramChanged | 54 |  | 0.39 | other |  |
| 2026-09-06T00:21:29.660+02:00 | paramChanged | 54 |  | 0.40 | other |  |
| 2026-09-06T00:21:29.670+02:00 | paramChanged | 54 |  | 0.41 | other |  |
| 2026-09-06T00:21:29.680+02:00 | paramChanged | 54 |  | 0.42 | other |  |
| 2026-09-06T00:21:29.690+02:00 | paramChanged | 54 |  | 0.43 | other |  |
| 2026-09-06T00:21:29.701+02:00 | paramChanged | 54 |  | 0.44 | other |  |
| 2026-09-06T00:21:29.710+02:00 | paramChanged | 54 |  | 0.45 | other |  |
| 2026-09-06T00:21:29.720+02:00 | paramChanged | 54 |  | 0.46 | other |  |
| 2026-09-06T00:21:29.730+02:00 | paramChanged | 54 |  | 0.47 | other |  |
| 2026-09-06T00:21:29.740+02:00 | paramChanged | 54 |  | 0.48 | other |  |
| 2026-09-06T00:21:29.750+02:00 | paramChanged | 54 |  | 0.49 | other |  |
| 2026-09-06T00:21:29.760+02:00 | paramChanged | 54 |  | 0.50 | other |  |
| 2026-09-06T00:21:29.770+02:00 | paramChanged | 54 |  | 0.51 | other |  |
| 2026-09-06T00:21:29.779+02:00 | paramChanged | 54 |  | 0.52 | other |  |
| 2026-09-06T00:21:29.790+02:00 | paramChanged | 54 |  | 0.53 | other |  |
| 2026-09-06T00:21:29.800+02:00 | paramChanged | 54 |  | 0.54 | other |  |
| 2026-09-06T00:21:29.811+02:00 | paramChanged | 54 |  | 0.55 | other |  |
| 2026-09-06T00:21:29.820+02:00 | paramChanged | 54 |  | 0.56 | other |  |
| 2026-09-06T00:21:29.829+02:00 | paramChanged | 54 |  | 0.57 | other |  |
| 2026-09-06T00:21:29.839+02:00 | paramChanged | 54 |  | 0.58 | other |  |
| 2026-09-06T00:21:29.850+02:00 | paramChanged | 54 |  | 0.59 | other |  |
| 2026-09-06T00:21:29.860+02:00 | paramChanged | 54 |  | 0.60 | other |  |
| 2026-09-06T00:21:29.870+02:00 | paramChanged | 54 |  | 0.61 | other |  |
| 2026-09-06T00:21:29.880+02:00 | paramChanged | 54 |  | 0.62 | other |  |
| 2026-09-06T00:21:29.890+02:00 | paramChanged | 54 |  | 0.63 | other |  |
| 2026-09-06T00:21:29.899+02:00 | paramChanged | 54 |  | 0.64 | other |  |
| 2026-09-06T00:21:29.910+02:00 | paramChanged | 54 |  | 0.65 | other |  |
| 2026-09-06T00:21:29.920+02:00 | paramChanged | 54 |  | 0.66 | other |  |
| 2026-09-06T00:21:29.930+02:00 | paramChanged | 54 |  | 0.67 | other |  |
| 2026-09-06T00:21:29.939+02:00 | paramChanged | 54 |  | 0.68 | other |  |
| 2026-09-06T00:21:29.950+02:00 | paramChanged | 54 |  | 0.69 | other |  |
| 2026-09-06T00:21:29.959+02:00 | paramChanged | 54 |  | 0.70 | other |  |
| 2026-09-06T00:21:29.969+02:00 | paramChanged | 54 |  | 0.71 | other |  |
| 2026-09-06T00:21:29.979+02:00 | paramChanged | 54 |  | 0.72 | other |  |
| 2026-09-06T00:21:29.990+02:00 | paramChanged | 54 |  | 0.73 | other |  |
| 2026-09-06T00:21:30.000+02:00 | paramChanged | 54 |  | 0.74 | other |  |
| 2026-09-06T00:21:30.011+02:00 | paramChanged | 54 |  | 0.75 | other |  |
| 2026-09-06T00:21:30.020+02:00 | paramChanged | 54 |  | 0.76 | other |  |
| 2026-09-06T00:21:30.029+02:00 | paramChanged | 54 |  | 0.77 | other |  |
| 2026-09-06T00:21:30.039+02:00 | paramChanged | 54 |  | 0.78 | other |  |
| 2026-09-06T00:21:30.049+02:00 | paramChanged | 54 |  | 0.79 | other |  |
| 2026-09-06T00:21:30.059+02:00 | paramChanged | 54 |  | 0.80 | other |  |
| 2026-09-06T00:21:30.070+02:00 | paramChanged | 54 |  | 0.81 | other |  |
| 2026-09-06T00:21:30.079+02:00 | paramChanged | 54 |  | 0.82 | other |  |
| 2026-09-06T00:21:30.089+02:00 | paramChanged | 54 |  | 0.83 | other |  |
| 2026-09-06T00:21:30.100+02:00 | paramChanged | 54 |  | 0.84 | other |  |
| 2026-09-06T00:21:30.109+02:00 | paramChanged | 54 |  | 0.85 | other |  |
| 2026-09-06T00:21:30.119+02:00 | paramChanged | 54 |  | 0.86 | other |  |
| 2026-09-06T00:21:30.129+02:00 | paramChanged | 54 |  | 0.87 | other |  |
| 2026-09-06T00:21:30.140+02:00 | paramChanged | 54 |  | 0.88 | other |  |
| 2026-09-06T00:21:30.150+02:00 | paramChanged | 54 |  | 0.89 | other |  |
| 2026-09-06T00:21:30.160+02:00 | paramChanged | 54 |  | 0.90 | other |  |
| 2026-09-06T00:21:30.170+02:00 | paramChanged | 54 |  | 0.91 | other |  |
| 2026-09-06T00:21:30.180+02:00 | paramChanged | 54 |  | 0.92 | other |  |
| 2026-09-06T00:21:30.190+02:00 | paramChanged | 54 |  | 0.93 | other |  |
| 2026-09-06T00:21:30.199+02:00 | paramChanged | 54 |  | 0.94 | other |  |
| 2026-09-06T00:21:30.209+02:00 | paramChanged | 54 |  | 0.95 | other |  |
| 2026-09-06T00:21:30.220+02:00 | paramChanged | 54 |  | 0.96 | other |  |
| 2026-09-06T00:21:30.230+02:00 | paramChanged | 54 |  | 0.97 | other |  |
| 2026-09-06T00:21:30.241+02:00 | paramChanged | 54 |  | 0.98 | other |  |
| 2026-09-06T00:21:30.250+02:00 | paramChanged | 54 |  | 0.99 | other |  |
| 2026-09-06T00:21:30.260+02:00 | paramChanged | 54 |  | 1.00 | other |  |

## Raw sample log (72)

| Time | Phase | Path | ms | bytes | editor | transport | thread | sha256 |
|---|---|---|---|---|---|---|---|---|
| 2026-09-06T00:21:19.762+02:00 | M2V-pre | raw-getStateInformation | 0.53 | 10393 | closed | stopped | message | `875dc964caa630252767e6b26bf8a54ab2d64a2fc93a6112174924e0c3633aee` |
| 2026-09-06T00:21:19.768+02:00 | M2V-pre | raw-getStateInformation | 0.52 | 10393 | closed | stopped | message | `875dc964caa630252767e6b26bf8a54ab2d64a2fc93a6112174924e0c3633aee` |
| 2026-09-06T00:21:19.774+02:00 | M2V-pre | raw-getStateInformation | 0.50 | 10393 | closed | stopped | message | `875dc964caa630252767e6b26bf8a54ab2d64a2fc93a6112174924e0c3633aee` |
| 2026-09-06T00:21:19.778+02:00 | M2V-pre | raw-getStateInformation | 0.48 | 10393 | closed | stopped | message | `875dc964caa630252767e6b26bf8a54ab2d64a2fc93a6112174924e0c3633aee` |
| 2026-09-06T00:21:19.783+02:00 | M2V-pre | raw-getStateInformation | 0.47 | 10393 | closed | stopped | message | `875dc964caa630252767e6b26bf8a54ab2d64a2fc93a6112174924e0c3633aee` |
| 2026-09-06T00:21:19.786+02:00 | M2V-pre | raw-getStateInformation | 0.44 | 10393 | closed | stopped | message | `875dc964caa630252767e6b26bf8a54ab2d64a2fc93a6112174924e0c3633aee` |
| 2026-09-06T00:21:19.790+02:00 | M2V-pre | raw-getStateInformation | 0.47 | 10393 | closed | stopped | message | `875dc964caa630252767e6b26bf8a54ab2d64a2fc93a6112174924e0c3633aee` |
| 2026-09-06T00:21:19.793+02:00 | M2V-pre | raw-getStateInformation | 0.46 | 10393 | closed | stopped | message | `875dc964caa630252767e6b26bf8a54ab2d64a2fc93a6112174924e0c3633aee` |
| 2026-09-06T00:21:19.796+02:00 | M2V-pre | raw-getStateInformation | 0.46 | 10393 | closed | stopped | message | `875dc964caa630252767e6b26bf8a54ab2d64a2fc93a6112174924e0c3633aee` |
| 2026-09-06T00:21:19.800+02:00 | M2V-pre | raw-getStateInformation | 0.48 | 10393 | closed | stopped | message | `875dc964caa630252767e6b26bf8a54ab2d64a2fc93a6112174924e0c3633aee` |
| 2026-09-06T00:21:20.519+02:00 | M2V-play | raw-getStateInformation | 0.52 | 10393 | closed | playing | message | `875dc964caa630252767e6b26bf8a54ab2d64a2fc93a6112174924e0c3633aee` |
| 2026-09-06T00:21:20.768+02:00 | M2V-play | raw-getStateInformation | 0.64 | 10393 | closed | playing | message | `875dc964caa630252767e6b26bf8a54ab2d64a2fc93a6112174924e0c3633aee` |
| 2026-09-06T00:21:21.026+02:00 | M2V-play | raw-getStateInformation | 1.17 | 10393 | closed | playing | message | `875dc964caa630252767e6b26bf8a54ab2d64a2fc93a6112174924e0c3633aee` |
| 2026-09-06T00:21:21.266+02:00 | M2V-play | raw-getStateInformation | 0.55 | 10393 | closed | playing | message | `875dc964caa630252767e6b26bf8a54ab2d64a2fc93a6112174924e0c3633aee` |
| 2026-09-06T00:21:21.520+02:00 | M2V-play | raw-getStateInformation | 0.51 | 10393 | closed | playing | message | `875dc964caa630252767e6b26bf8a54ab2d64a2fc93a6112174924e0c3633aee` |
| 2026-09-06T00:21:21.776+02:00 | M2V-play | raw-getStateInformation | 0.53 | 10393 | closed | playing | message | `875dc964caa630252767e6b26bf8a54ab2d64a2fc93a6112174924e0c3633aee` |
| 2026-09-06T00:21:22.021+02:00 | M2V-play | raw-getStateInformation | 0.52 | 10393 | closed | playing | message | `875dc964caa630252767e6b26bf8a54ab2d64a2fc93a6112174924e0c3633aee` |
| 2026-09-06T00:21:22.272+02:00 | M2V-play | raw-getStateInformation | 0.56 | 10393 | closed | playing | message | `875dc964caa630252767e6b26bf8a54ab2d64a2fc93a6112174924e0c3633aee` |
| 2026-09-06T00:21:22.527+02:00 | M2V-play | raw-getStateInformation | 1.03 | 10393 | closed | playing | message | `875dc964caa630252767e6b26bf8a54ab2d64a2fc93a6112174924e0c3633aee` |
| 2026-09-06T00:21:22.794+02:00 | M2V-play | raw-getStateInformation | 0.54 | 10413 | closed | playing | message | `dad4f30b4a51d22056249cdb1b4676713003074a20dabc52457474343cb27f7d` |
| 2026-09-06T00:21:23.033+02:00 | M2V-play | raw-getStateInformation | 0.65 | 10413 | closed | playing | message | `9079686ddff7c5e5fe3738d97f52e7771478c0f0da516942b1d9fbf037553e31` |
| 2026-09-06T00:21:23.286+02:00 | M2V-play | raw-getStateInformation | 0.53 | 10415 | closed | playing | message | `20ecc24bf680e0e6646d9d95375e269921ff63eae79f8e8eb22b954bb10df9e0` |
| 2026-09-06T00:21:23.533+02:00 | M2V-play | raw-getStateInformation | 0.52 | 10413 | closed | playing | message | `f5496f18aff62733fe829e383a3df0e00bb40eb0ad742cb4cd8777c3835ab4e6` |
| 2026-09-06T00:21:23.784+02:00 | M2V-play | raw-getStateInformation | 0.54 | 10413 | closed | playing | message | `8879d2d721f227ebb99b0b94f05c4a5e88eccc91fe837d484d18e7e5892bf1a3` |
| 2026-09-06T00:21:24.042+02:00 | M2V-play | raw-getStateInformation | 0.54 | 10413 | closed | playing | message | `40ee6a58fae1ff1c1b8c4590c9d4aa2e19fd22def0b5d2af302e655f1790db7a` |
| 2026-09-06T00:21:24.285+02:00 | M2V-play | raw-getStateInformation | 0.53 | 10393 | closed | playing | message | `875dc964caa630252767e6b26bf8a54ab2d64a2fc93a6112174924e0c3633aee` |
| 2026-09-06T00:21:24.540+02:00 | M2V-play | raw-getStateInformation | 0.52 | 10393 | closed | playing | message | `875dc964caa630252767e6b26bf8a54ab2d64a2fc93a6112174924e0c3633aee` |
| 2026-09-06T00:21:24.793+02:00 | M2V-play | raw-getStateInformation | 0.56 | 10393 | closed | playing | message | `875dc964caa630252767e6b26bf8a54ab2d64a2fc93a6112174924e0c3633aee` |
| 2026-09-06T00:21:25.041+02:00 | M2V-play | raw-getStateInformation | 0.52 | 10393 | closed | playing | message | `875dc964caa630252767e6b26bf8a54ab2d64a2fc93a6112174924e0c3633aee` |
| 2026-09-06T00:21:25.300+02:00 | M2V-play | raw-getStateInformation | 0.55 | 10393 | closed | playing | message | `875dc964caa630252767e6b26bf8a54ab2d64a2fc93a6112174924e0c3633aee` |
| 2026-09-06T00:21:25.544+02:00 | M2V-play | raw-getStateInformation | 0.80 | 10393 | closed | playing | message | `875dc964caa630252767e6b26bf8a54ab2d64a2fc93a6112174924e0c3633aee` |
| 2026-09-06T00:21:25.795+02:00 | M2V-play | raw-getStateInformation | 0.54 | 10393 | closed | playing | message | `875dc964caa630252767e6b26bf8a54ab2d64a2fc93a6112174924e0c3633aee` |
| 2026-09-06T00:21:26.046+02:00 | M2V-play | raw-getStateInformation | 0.55 | 10393 | closed | playing | message | `875dc964caa630252767e6b26bf8a54ab2d64a2fc93a6112174924e0c3633aee` |
| 2026-09-06T00:21:26.299+02:00 | M2V-play | raw-getStateInformation | 0.55 | 10393 | closed | playing | message | `875dc964caa630252767e6b26bf8a54ab2d64a2fc93a6112174924e0c3633aee` |
| 2026-09-06T00:21:26.551+02:00 | M2V-play | raw-getStateInformation | 0.52 | 10393 | closed | playing | message | `875dc964caa630252767e6b26bf8a54ab2d64a2fc93a6112174924e0c3633aee` |
| 2026-09-06T00:21:26.800+02:00 | M2V-play | raw-getStateInformation | 0.53 | 10393 | closed | playing | message | `875dc964caa630252767e6b26bf8a54ab2d64a2fc93a6112174924e0c3633aee` |
| 2026-09-06T00:21:27.051+02:00 | M2V-play | raw-getStateInformation | 0.53 | 10393 | closed | playing | message | `875dc964caa630252767e6b26bf8a54ab2d64a2fc93a6112174924e0c3633aee` |
| 2026-09-06T00:21:27.302+02:00 | M2V-play | raw-getStateInformation | 0.67 | 10393 | closed | playing | message | `875dc964caa630252767e6b26bf8a54ab2d64a2fc93a6112174924e0c3633aee` |
| 2026-09-06T00:21:27.552+02:00 | M2V-play | raw-getStateInformation | 0.54 | 10393 | closed | playing | message | `875dc964caa630252767e6b26bf8a54ab2d64a2fc93a6112174924e0c3633aee` |
| 2026-09-06T00:21:27.806+02:00 | M2V-play | raw-getStateInformation | 0.55 | 10393 | closed | playing | message | `875dc964caa630252767e6b26bf8a54ab2d64a2fc93a6112174924e0c3633aee` |
| 2026-09-06T00:21:28.056+02:00 | M2V-play | raw-getStateInformation | 0.54 | 10393 | closed | playing | message | `875dc964caa630252767e6b26bf8a54ab2d64a2fc93a6112174924e0c3633aee` |
| 2026-09-06T00:21:28.306+02:00 | M2V-play | raw-getStateInformation | 0.55 | 10393 | closed | playing | message | `875dc964caa630252767e6b26bf8a54ab2d64a2fc93a6112174924e0c3633aee` |
| 2026-09-06T00:21:28.556+02:00 | M2V-play | raw-getStateInformation | 0.54 | 10393 | closed | playing | message | `875dc964caa630252767e6b26bf8a54ab2d64a2fc93a6112174924e0c3633aee` |
| 2026-09-06T00:21:28.804+02:00 | M2V-play | raw-getStateInformation | 0.65 | 10413 | closed | playing | message | `dad4f30b4a51d22056249cdb1b4676713003074a20dabc52457474343cb27f7d` |
| 2026-09-06T00:21:29.063+02:00 | M2V-play | raw-getStateInformation | 0.50 | 10413 | closed | playing | message | `12fd74cbff5bc9cb7d15acc08cbc097a36f1127cfa9472d3dbc313ee73b88839` |
| 2026-09-06T00:21:29.314+02:00 | M2V-play | raw-getStateInformation | 0.52 | 10415 | closed | playing | message | `09ba00347f3a6b7877b39e6ab1b37d7d724476d13e73a2e7c64ede0c9088c9ce` |
| 2026-09-06T00:21:30.334+02:00 | M2V-stop | raw-getStateInformation | 0.54 | 10393 | closed | stopped | message | `875dc964caa630252767e6b26bf8a54ab2d64a2fc93a6112174924e0c3633aee` |
| 2026-09-06T00:21:30.452+02:00 | M2V-stop | raw-getStateInformation | 0.53 | 10393 | closed | stopped | message | `875dc964caa630252767e6b26bf8a54ab2d64a2fc93a6112174924e0c3633aee` |
| 2026-09-06T00:21:30.602+02:00 | M2V-stop | raw-getStateInformation | 0.51 | 10393 | closed | stopped | message | `875dc964caa630252767e6b26bf8a54ab2d64a2fc93a6112174924e0c3633aee` |
| 2026-09-06T00:21:30.853+02:00 | M2V-stop | raw-getStateInformation | 0.55 | 10393 | closed | stopped | message | `875dc964caa630252767e6b26bf8a54ab2d64a2fc93a6112174924e0c3633aee` |
| 2026-09-06T00:21:31.354+02:00 | M2V-stop | raw-getStateInformation | 0.52 | 10393 | closed | stopped | message | `875dc964caa630252767e6b26bf8a54ab2d64a2fc93a6112174924e0c3633aee` |
| 2026-09-06T00:21:32.356+02:00 | M2V-stop | raw-getStateInformation | 0.74 | 10393 | closed | stopped | message | `875dc964caa630252767e6b26bf8a54ab2d64a2fc93a6112174924e0c3633aee` |
| 2026-09-06T00:21:35.359+02:00 | M2V-late | raw-getStateInformation | 0.54 | 10393 | closed | stopped | message | `875dc964caa630252767e6b26bf8a54ab2d64a2fc93a6112174924e0c3633aee` |
| 2026-09-06T00:21:35.363+02:00 | M2V-late | raw-getStateInformation | 0.48 | 10393 | closed | stopped | message | `875dc964caa630252767e6b26bf8a54ab2d64a2fc93a6112174924e0c3633aee` |
| 2026-09-06T00:21:35.366+02:00 | M2V-late | raw-getStateInformation | 0.67 | 10393 | closed | stopped | message | `875dc964caa630252767e6b26bf8a54ab2d64a2fc93a6112174924e0c3633aee` |
| 2026-09-06T00:21:35.370+02:00 | M2V-late | raw-getStateInformation | 0.46 | 10393 | closed | stopped | message | `875dc964caa630252767e6b26bf8a54ab2d64a2fc93a6112174924e0c3633aee` |
| 2026-09-06T00:21:35.373+02:00 | M2V-late | raw-getStateInformation | 0.43 | 10393 | closed | stopped | message | `875dc964caa630252767e6b26bf8a54ab2d64a2fc93a6112174924e0c3633aee` |
| 2026-09-06T00:21:35.375+02:00 | M2V-late | raw-getStateInformation | 0.45 | 10393 | closed | stopped | message | `875dc964caa630252767e6b26bf8a54ab2d64a2fc93a6112174924e0c3633aee` |
| 2026-09-06T00:21:35.378+02:00 | M2V-late | raw-getStateInformation | 0.43 | 10393 | closed | stopped | message | `875dc964caa630252767e6b26bf8a54ab2d64a2fc93a6112174924e0c3633aee` |
| 2026-09-06T00:21:35.380+02:00 | M2V-late | raw-getStateInformation | 0.47 | 10393 | closed | stopped | message | `875dc964caa630252767e6b26bf8a54ab2d64a2fc93a6112174924e0c3633aee` |
| 2026-09-06T00:21:35.383+02:00 | M2V-late | raw-getStateInformation | 0.47 | 10393 | closed | stopped | message | `875dc964caa630252767e6b26bf8a54ab2d64a2fc93a6112174924e0c3633aee` |
| 2026-09-06T00:21:35.386+02:00 | M2V-late | raw-getStateInformation | 0.43 | 10393 | closed | stopped | message | `875dc964caa630252767e6b26bf8a54ab2d64a2fc93a6112174924e0c3633aee` |
| 2026-09-06T00:21:40.360+02:00 | M2V-late | raw-getStateInformation | 0.54 | 10393 | closed | stopped | message | `875dc964caa630252767e6b26bf8a54ab2d64a2fc93a6112174924e0c3633aee` |
| 2026-09-06T00:21:40.365+02:00 | M2V-late | raw-getStateInformation | 1.05 | 10393 | closed | stopped | message | `875dc964caa630252767e6b26bf8a54ab2d64a2fc93a6112174924e0c3633aee` |
| 2026-09-06T00:21:40.372+02:00 | M2V-late | raw-getStateInformation | 0.49 | 10393 | closed | stopped | message | `875dc964caa630252767e6b26bf8a54ab2d64a2fc93a6112174924e0c3633aee` |
| 2026-09-06T00:21:40.376+02:00 | M2V-late | raw-getStateInformation | 0.46 | 10393 | closed | stopped | message | `875dc964caa630252767e6b26bf8a54ab2d64a2fc93a6112174924e0c3633aee` |
| 2026-09-06T00:21:40.381+02:00 | M2V-late | raw-getStateInformation | 0.50 | 10393 | closed | stopped | message | `875dc964caa630252767e6b26bf8a54ab2d64a2fc93a6112174924e0c3633aee` |
| 2026-09-06T00:21:40.384+02:00 | M2V-late | raw-getStateInformation | 0.45 | 10393 | closed | stopped | message | `875dc964caa630252767e6b26bf8a54ab2d64a2fc93a6112174924e0c3633aee` |
| 2026-09-06T00:21:40.386+02:00 | M2V-late | raw-getStateInformation | 0.46 | 10393 | closed | stopped | message | `875dc964caa630252767e6b26bf8a54ab2d64a2fc93a6112174924e0c3633aee` |
| 2026-09-06T00:21:40.389+02:00 | M2V-late | raw-getStateInformation | 0.44 | 10393 | closed | stopped | message | `875dc964caa630252767e6b26bf8a54ab2d64a2fc93a6112174924e0c3633aee` |
| 2026-09-06T00:21:40.394+02:00 | M2V-late | raw-getStateInformation | 0.50 | 10393 | closed | stopped | message | `875dc964caa630252767e6b26bf8a54ab2d64a2fc93a6112174924e0c3633aee` |
| 2026-09-06T00:21:40.399+02:00 | M2V-late | raw-getStateInformation | 1.22 | 10393 | closed | stopped | message | `875dc964caa630252767e6b26bf8a54ab2d64a2fc93a6112174924e0c3633aee` |

## MIDI delivery counters (M2V sink, transcribed from the auto-run session log)

Sink installed at the instrument-process boundary of destination trackId 7 ("Organ", VB3-II)
before the pre-captures; final summary logged after stop, before the sink was cleared:

```
blocks=2210 blocksWithMidi=327 noteOn=32 noteOff=32 cc=384 cc11=368 other=0
firstEventAbs=88000 lastEventAbs=555360
channels[ch1=401 ch2=13 ch3=21 ch4=1 ch5=1 ch6=1 ch7=1 ch8=1 ch9=1 ch10=1
         ch11=1 ch12=1 ch13=1 ch14=1 ch15=1 ch16=1]
cycleWraps=1  (boundary block counter advanced 187 -> 2447 across the run)
```

**Reconciliation — all 448 messages accounted for:**

```
by type:     32 noteOn + 32 noteOff + 384 CC + 0 other           = 448
by channel:  401 + 13 + 21 + 13×1 (ch4…ch16)                     = 448

ch1    = 16 noteOn + 16 noteOff + 368 CC11 + 1 CC123             = 401
ch2    =  6 noteOn +  6 noteOff            + 1 CC123             =  13
ch3    = 10 noteOn + 10 noteOff            + 1 CC123             =  21
ch4–16 = 1 CC123 each                                            =  13
```

The 16 CC123 (All Notes Off) events are the transport-stop flush that
`InstrumentTrackController::audioThread_flushTransportMidi` sends on every channel 1–16
(`src/instruments/InstrumentTrackController.cpp:2538–2541`). They are controller events, so they
are included in the CC total (384 = 368 CC11 + 16 CC123). Summing only ch1–ch3 (435) omits the
ch4–16 flush and is not a discrepancy. Note counts: (8 + 3 + 5) arranged notes × 2 cycle laps
= 32 note-ons, each with its note-off.

## Operator notes

*None.*

---
*End of sanitized SPIKE-01 report. Verify before sharing: this file must
contain no base64 blocks and no plugin-state byte dumps.*
