# SPIKE-01 sanitized measurement report — authoritative plugin-state capture

SPIKE-01 diagnostic output (roadmap slice P0/P1A; canonical steering §9.2 / PID-001).

**Privacy:** this report contains only sizes, SHA-256 hashes, timings, and
notification metadata. No raw plugin-state bytes, presets, or licensing
material are captured to disk by the SPIKE-01 probe.

## Environment

| Field | Value |
|---|---|
| Generated | 2026-09-05T23:36:08.969+02:00 |
| App version | 0.9.0 |
| Plugin | Groove Agent SE |
| Plugin format | VST3 |
| Plugin version | 5.2.20 |
| Plugin identifier | C:\Program Files\Common Files\VST3\Steinberg\Groove Agent SE.vst3\Contents\x86_64-win\Groove Agent SE.vst3 |
| Operator notes |  |

## Capture measurements per phase

| Phase / path | n | min ms | median ms | p95 ms | max ms | blob bytes (min..max) | distinct hashes | byte-stable |
|---|---|---|---|---|---|---|---|---|
| M1X-120s / raw-getStateInformation | 10 | 5.60 | 6.03 | 6.57 | 6.57 | 148788..148959 | 10 | NO |
| M1X-60s / raw-getStateInformation | 10 | 6.86 | 8.03 | 8.52 | 8.52 | 148561..148763 | 10 | NO |

## Hash inventory (12-hex prefixes; full hashes in raw sample log)

- **M1X-120s**: `049dec3b7a41…`, `5125c9f5e08b…`, `521141979c22…`, `532bcbdae2a6…`, `6f7b01b17a86…`, `7028b1e88310…`, `7ec43af081af…`, `8e604e0a006c…`, `a7174abaef2d…`, `a86347d5c497…`
- **M1X-60s**: `07ff9f0e8548…`, `290090ddfd8b…`, `2d509c79e77b…`, `33282e7baa21…`, `386534fe3015…`, `49f0408d79d8…`, `6cb14a54b325…`, `9291c5289a36…`, `c93783fb0354…`, `d8c686789e86…`

## Threading

- Captures executed on the message thread: 20
- Captures executed on other threads: 0 (expected: 0)
- Parameter/processor notifications on the message thread: 0
- Parameter/processor notifications on other threads: 0

## Parameter/processor notifications (0)

*None observed while the listener was attached.*

## Raw sample log (20)

| Time | Phase | Path | ms | bytes | editor | transport | thread | sha256 |
|---|---|---|---|---|---|---|---|---|
| 2026-09-05T23:35:08.851+02:00 | M1X-60s | raw-getStateInformation | 7.54 | 148561 | closed | stopped | message | `33282e7baa2196bb111966440442534828ae2d89eeedceec9d783fc39cf31fef` |
| 2026-09-05T23:35:08.864+02:00 | M1X-60s | raw-getStateInformation | 8.46 | 148587 | closed | stopped | message | `290090ddfd8bf53a2a5957535dbeb7ed05b8d802d11e99ba842c074e2a05728a` |
| 2026-09-05T23:35:08.879+02:00 | M1X-60s | raw-getStateInformation | 7.89 | 148615 | closed | stopped | message | `386534fe3015ec21a6f548aa060ddb163f3327cc1f5740a1c89b0a4c2b684bb3` |
| 2026-09-05T23:35:08.893+02:00 | M1X-60s | raw-getStateInformation | 8.13 | 148647 | closed | stopped | message | `07ff9f0e85483b8003672e35ae563cb5506b16be97aada40c906a57e9df35454` |
| 2026-09-05T23:35:08.910+02:00 | M1X-60s | raw-getStateInformation | 6.86 | 148663 | closed | stopped | message | `d8c686789e863f1b47ce619d02a5e3b1ce75add61074b7e68ea8011db36991c7` |
| 2026-09-05T23:35:08.923+02:00 | M1X-60s | raw-getStateInformation | 8.47 | 148692 | closed | stopped | message | `6cb14a54b32512b735d124f6cd22bac8e8ea6c5b9c5899e5e6fe7bff2953c110` |
| 2026-09-05T23:35:08.940+02:00 | M1X-60s | raw-getStateInformation | 7.94 | 148707 | closed | stopped | message | `9291c5289a362217d6494c4dd924a72ebbde8347062cdf793e809487bf9b5c5c` |
| 2026-09-05T23:35:08.955+02:00 | M1X-60s | raw-getStateInformation | 8.22 | 148723 | closed | stopped | message | `49f0408d79d8506e607c26bdf6d2736587125c78a6c2e59b852c026dd0394ee5` |
| 2026-09-05T23:35:08.970+02:00 | M1X-60s | raw-getStateInformation | 8.52 | 148743 | closed | stopped | message | `2d509c79e77b4670100dddc7848dc98648c4e46d41d69a13af834809b6159cdc` |
| 2026-09-05T23:35:08.986+02:00 | M1X-60s | raw-getStateInformation | 7.15 | 148763 | closed | stopped | message | `c93783fb0354ba367ce11d046d3e87636aa74f40ab879fc35a5f06620a3c5fe9` |
| 2026-09-05T23:36:08.848+02:00 | M1X-120s | raw-getStateInformation | 5.99 | 148788 | closed | stopped | message | `7028b1e8831059ef9efc27d7c91ddf73811392ec58ace72387a82e6ba90d2536` |
| 2026-09-05T23:36:08.861+02:00 | M1X-120s | raw-getStateInformation | 6.54 | 148800 | closed | stopped | message | `532bcbdae2a65f3481aa3bff050bb997bd0c5e8e2ffca631259f2759da91b7cb` |
| 2026-09-05T23:36:08.873+02:00 | M1X-120s | raw-getStateInformation | 6.40 | 148819 | closed | stopped | message | `a86347d5c4975dbcde4b4b60dde34fa2966c4973d5f75de5292500e468312dc1` |
| 2026-09-05T23:36:08.885+02:00 | M1X-120s | raw-getStateInformation | 5.82 | 148840 | closed | stopped | message | `049dec3b7a41ec23658b591dd0e5852224469ae0f8c6666eb0a03fea486d4d87` |
| 2026-09-05T23:36:08.897+02:00 | M1X-120s | raw-getStateInformation | 6.57 | 148863 | closed | stopped | message | `a7174abaef2d0b42a11d528f6aca478c8c0e04d10931ece4238e28e2f062baec` |
| 2026-09-05T23:36:08.909+02:00 | M1X-120s | raw-getStateInformation | 5.86 | 148880 | closed | stopped | message | `7ec43af081afc7d19ddd43df15ffdc54ac1cbd5bf8629c4e3368571eab852e45` |
| 2026-09-05T23:36:08.921+02:00 | M1X-120s | raw-getStateInformation | 5.60 | 148897 | closed | stopped | message | `8e604e0a006ca1e2ad59550c85811739f1c9c52e56d100e21760b2408fe066f1` |
| 2026-09-05T23:36:08.931+02:00 | M1X-120s | raw-getStateInformation | 6.04 | 148915 | closed | stopped | message | `5125c9f5e08bc2a846be95cc83598d4bfd5d04561cb3c3eed39513110fe64418` |
| 2026-09-05T23:36:08.942+02:00 | M1X-120s | raw-getStateInformation | 6.02 | 148940 | closed | stopped | message | `6f7b01b17a86f902e577f71f11147fded1416714419d26abbc16245aa860e844` |
| 2026-09-05T23:36:08.954+02:00 | M1X-120s | raw-getStateInformation | 6.28 | 148959 | closed | stopped | message | `521141979c2238266dcf7142d4c59f2450bb67909e6490a3c0338994940ac78d` |

## Operator notes

*None.*

---
*End of sanitized SPIKE-01 report. Verify before sharing: this file must
contain no base64 blocks and no plugin-state byte dumps.*
