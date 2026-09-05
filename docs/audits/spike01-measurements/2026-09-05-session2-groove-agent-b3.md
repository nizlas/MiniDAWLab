# SPIKE-01 sanitized measurement report — authoritative plugin-state capture

SPIKE-01 diagnostic output (roadmap slice P0/P1A; canonical steering §9.2 / PID-001).

**Privacy:** this report contains only sizes, SHA-256 hashes, timings, and
notification metadata. No raw plugin-state bytes, presets, or licensing
material are captured to disk by the SPIKE-01 probe.

## Environment

| Field | Value |
|---|---|
| Generated | 2026-09-05T22:19:08.450+02:00 |
| App version | 0.2.0 |
| Plugin | Groove Agent SE |
| Plugin format | VST3 |
| Plugin version | 5.2.20 |
| Plugin identifier | C:\Program Files\Common Files\VST3\Steinberg\Groove Agent SE.vst3\Contents\x86_64-win\Groove Agent SE.vst3 |
| Operator notes |  |

## Capture measurements per phase

| Phase / path | n | min ms | median ms | p95 ms | max ms | blob bytes (min..max) | distinct hashes | byte-stable |
|---|---|---|---|---|---|---|---|---|
| B3 / raw-getStateInformation | 10 | 4.41 | 4.62 | 6.47 | 6.47 | 148591..148791 | 10 | NO |

## Hash inventory (12-hex prefixes; full hashes in raw sample log)

- **B3**: `30b9fa9dcbca…`, `39a9be6a4b88…`, `55635f0b3b2f…`, `727efa080aae…`, `83d20add1bf4…`, `9d5e2dc230a1…`, `ad419609083b…`, `c21110f3f38d…`, `ef9659472e22…`, `f7a7bdf9b8da…`

## Threading

- Captures executed on the message thread: 10
- Captures executed on other threads: 0 (expected: 0)
- Parameter/processor notifications on the message thread: 0
- Parameter/processor notifications on other threads: 0

## Parameter/processor notifications (0)

*None observed while the listener was attached.*

## Raw sample log (10)

| Time | Phase | Path | ms | bytes | editor | transport | thread | sha256 |
|---|---|---|---|---|---|---|---|---|
| 2026-09-05T22:18:54.107+02:00 | B3 | raw-getStateInformation | 6.47 | 148591 | closed | stopped | message | `727efa080aaee7f0dd705b60046ba522b2ec64d5f194778c36d2978cc3624aef` |
| 2026-09-05T22:18:54.120+02:00 | B3 | raw-getStateInformation | 4.55 | 148608 | closed | stopped | message | `83d20add1bf4a517040182307d744d7b71ca83e3c5c851621f19e76c79396212` |
| 2026-09-05T22:18:54.130+02:00 | B3 | raw-getStateInformation | 4.84 | 148635 | closed | stopped | message | `ef9659472e22256b07ebc1fc236a760e1eadd33e778bb8794c18034b61e19580` |
| 2026-09-05T22:18:54.139+02:00 | B3 | raw-getStateInformation | 4.59 | 148672 | closed | stopped | message | `30b9fa9dcbca63cf6fc0f5c721fc2416737f3129b2977854d97c9fd4c1b33144` |
| 2026-09-05T22:18:54.149+02:00 | B3 | raw-getStateInformation | 4.74 | 148689 | closed | stopped | message | `55635f0b3b2fb2e78c36c6f9a3c6ee5fcadf2e0a0b99c13ba8d767799860cb19` |
| 2026-09-05T22:18:54.158+02:00 | B3 | raw-getStateInformation | 4.65 | 148717 | closed | stopped | message | `ad419609083bc437653a3d84fc261e2843c02862f41161c9f219ff0388917176` |
| 2026-09-05T22:18:54.167+02:00 | B3 | raw-getStateInformation | 4.41 | 148736 | closed | stopped | message | `9d5e2dc230a1807d9ecde73bcef4722146cb90a017dc44d1b8dedcc947762ea0` |
| 2026-09-05T22:18:54.176+02:00 | B3 | raw-getStateInformation | 4.73 | 148753 | closed | stopped | message | `39a9be6a4b88118918e8d1e355d6495fa4eae156b3a2b60f96b01410b78e78e3` |
| 2026-09-05T22:18:54.185+02:00 | B3 | raw-getStateInformation | 4.47 | 148769 | closed | stopped | message | `c21110f3f38de2c395204eccb716292da8fceda7cf767d7158c85fa63c0789b7` |
| 2026-09-05T22:18:54.194+02:00 | B3 | raw-getStateInformation | 4.53 | 148791 | closed | stopped | message | `f7a7bdf9b8dab88d3488ccb9d29380695f624671a1f8dd7c2aa6e99005d05a55` |

## Operator notes

*None.*

---
*End of sanitized SPIKE-01 report. Verify before sharing: this file must
contain no base64 blocks and no plugin-state byte dumps.*
