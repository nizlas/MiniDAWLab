# SPIKE-01 sanitized measurement report — authoritative plugin-state capture

SPIKE-01 diagnostic output (roadmap slice P0/P1A; canonical steering §9.2 / PID-001).

**Privacy:** this report contains only sizes, SHA-256 hashes, timings, and
notification metadata. No raw plugin-state bytes, presets, or licensing
material are captured to disk by the SPIKE-01 probe.

## Environment

| Field | Value |
|---|---|
| Generated | 2026-09-05T23:40:25.105+02:00 |
| App version | 0.9.0 |
| Plugin | Groove Agent SE |
| Plugin format | VST3 |
| Plugin version | 5.2.20 |
| Plugin identifier | C:\Program Files\Common Files\VST3\Steinberg\Groove Agent SE.vst3\Contents\x86_64-win\Groove Agent SE.vst3 |
| Operator notes |  |

## Capture measurements per phase

| Phase / path | n | min ms | median ms | p95 ms | max ms | blob bytes (min..max) | distinct hashes | byte-stable |
|---|---|---|---|---|---|---|---|---|
| M1Y-0s / raw-getStateInformation | 10 | 4.91 | 5.20 | 7.44 | 7.44 | 148601..148797 | 10 | NO |
| M1Y-120s / raw-getStateInformation | 10 | 4.96 | 5.16 | 5.96 | 5.96 | 149267..149401 | 10 | NO |
| M1Y-30s / raw-getStateInformation | 10 | 4.91 | 5.26 | 6.43 | 6.43 | 148821..148991 | 10 | NO |
| M1Y-60s / raw-getStateInformation | 10 | 5.07 | 5.17 | 5.86 | 5.86 | 149100..149252 | 10 | NO |

## Hash inventory (12-hex prefixes; full hashes in raw sample log)

- **M1Y-0s**: `06ec29b652d8…`, `1c2b8a1f3312…`, `225f8363c730…`, `2c267c73182f…`, `35d123f61658…`, `39081faf230b…`, `6ad2be4b7785…`, `cf839a53c1a8…`, `e0ec6dc64027…`, `f51cd7a5ce0a…`
- **M1Y-120s**: `0b76a5eefcf2…`, `1382a50732b2…`, `3e9e63bce75b…`, `5f8e29cea37d…`, `68fefd7e675e…`, `6cbb3d130f57…`, `8a44c41ff40e…`, `95d507b69f82…`, `b280dd3755e2…`, `f07f5286dae7…`
- **M1Y-30s**: `06f4773b4269…`, `3f56ab5d8c95…`, `5334184c4ac8…`, `571b8855e0f7…`, `582f22418997…`, `62778bf5bc03…`, `9c95ba50d4bb…`, `a0bd3d6283e3…`, `b107b595ca13…`, `fd6607ad45f6…`
- **M1Y-60s**: `1108df4eddb3…`, `1619124a0726…`, `201a7cf680d2…`, `418a0a3e5e27…`, `48da457a0363…`, `8c1a3e5af18e…`, `9618e38acf82…`, `9e6c8565906f…`, `bfe527b792b7…`, `cc8d5e5c93d8…`

## Threading

- Captures executed on the message thread: 40
- Captures executed on other threads: 0 (expected: 0)
- Parameter/processor notifications on the message thread: 0
- Parameter/processor notifications on other threads: 0

## Parameter/processor notifications (0)

*None observed while the listener was attached.*

## Raw sample log (40)

| Time | Phase | Path | ms | bytes | editor | transport | thread | sha256 |
|---|---|---|---|---|---|---|---|---|
| 2026-09-05T23:38:24.983+02:00 | M1Y-0s | raw-getStateInformation | 5.37 | 148601 | closed | stopped | message | `39081faf230bbafefa34ac7b53f34c9096cb78dbfa497da134ff269a92901e04` |
| 2026-09-05T23:38:24.995+02:00 | M1Y-0s | raw-getStateInformation | 5.09 | 148609 | closed | stopped | message | `225f8363c7307aa62c3d015a44dd57823c54b607b733a02fb24420bf53733c3e` |
| 2026-09-05T23:38:25.008+02:00 | M1Y-0s | raw-getStateInformation | 5.03 | 148648 | closed | stopped | message | `1c2b8a1f33120a9e902a99ff40fe2c519250047f151e760051edfb3b5c48d69a` |
| 2026-09-05T23:38:25.020+02:00 | M1Y-0s | raw-getStateInformation | 5.18 | 148671 | closed | stopped | message | `e0ec6dc64027fab769bd9f63d46225382336aad8001db95e01e5fb7dde4cceac` |
| 2026-09-05T23:38:25.033+02:00 | M1Y-0s | raw-getStateInformation | 5.50 | 148699 | closed | stopped | message | `cf839a53c1a8bbf51c33ccb0561c046d4797fa491c2852b5449961e99637e177` |
| 2026-09-05T23:38:25.046+02:00 | M1Y-0s | raw-getStateInformation | 5.21 | 148725 | closed | stopped | message | `35d123f61658e36106c87724e8e83ed82c8cc522c8ea8dd9f85cdf6e956f1252` |
| 2026-09-05T23:38:25.059+02:00 | M1Y-0s | raw-getStateInformation | 5.30 | 148744 | closed | stopped | message | `2c267c73182f35d24cd8cb97ec48644eb15eb344b93c6bef874e478e7e1233bd` |
| 2026-09-05T23:38:25.070+02:00 | M1Y-0s | raw-getStateInformation | 4.91 | 148760 | closed | stopped | message | `f51cd7a5ce0a9ca5a0cb17284b2fb6075b5754ef1f8b2ed129037710541bccc5` |
| 2026-09-05T23:38:25.080+02:00 | M1Y-0s | raw-getStateInformation | 5.11 | 148783 | closed | stopped | message | `6ad2be4b77851026ce3f1fbc456c276d68551965c8b663e16660a797b430ed0e` |
| 2026-09-05T23:38:25.092+02:00 | M1Y-0s | raw-getStateInformation | 7.44 | 148797 | closed | stopped | message | `06ec29b652d84306ce239b19be1024e63cf29d6cf0740ee34ae0c9e103410db4` |
| 2026-09-05T23:38:54.979+02:00 | M1Y-30s | raw-getStateInformation | 5.03 | 148821 | closed | stopped | message | `b107b595ca139f377144de73b64c83c22ff5722fc73989d983d0e8c962b3f6c4` |
| 2026-09-05T23:38:54.992+02:00 | M1Y-30s | raw-getStateInformation | 5.08 | 148831 | closed | stopped | message | `06f4773b4269f19417b55f3cd46f6247642bb0d7900c60687b48cd9d5f2452e5` |
| 2026-09-05T23:38:55.004+02:00 | M1Y-30s | raw-getStateInformation | 6.43 | 148849 | closed | stopped | message | `62778bf5bc0319fbab45de8581058e15ca2f0112d88cd519f04b707d763136cb` |
| 2026-09-05T23:38:55.015+02:00 | M1Y-30s | raw-getStateInformation | 4.91 | 148872 | closed | stopped | message | `fd6607ad45f6f176bb29acb3749837cd80c0fc754d7eb01d0be4098a4a0d9928` |
| 2026-09-05T23:38:55.028+02:00 | M1Y-30s | raw-getStateInformation | 5.45 | 148900 | closed | stopped | message | `3f56ab5d8c95589027e056d245d4a0cad57772facc2eee0894ca1d548ef05719` |
| 2026-09-05T23:38:55.039+02:00 | M1Y-30s | raw-getStateInformation | 5.24 | 148916 | closed | stopped | message | `5334184c4ac8ad1f062f8458811eb89296e8af1bf82b3d60bd1897822404febf` |
| 2026-09-05T23:38:55.052+02:00 | M1Y-30s | raw-getStateInformation | 5.46 | 148935 | closed | stopped | message | `571b8855e0f72b3440dbe1993fa983eea6a027dfbce32273f736b45b0b3d6096` |
| 2026-09-05T23:38:55.063+02:00 | M1Y-30s | raw-getStateInformation | 5.12 | 148951 | closed | stopped | message | `a0bd3d6283e35edbe4cb2941203b9d628173f168ab55f1c2f1f2ccd7345dc72e` |
| 2026-09-05T23:38:55.074+02:00 | M1Y-30s | raw-getStateInformation | 5.28 | 148973 | closed | stopped | message | `9c95ba50d4bbdf303fa67fa63cc5b83be18a6916ab674368ff0ec0ef51897e7f` |
| 2026-09-05T23:38:55.085+02:00 | M1Y-30s | raw-getStateInformation | 5.78 | 148991 | closed | stopped | message | `582f224189975f1ebf9aedadde360a16d3c73b88fa3ad80da98952cf9755fc4e` |
| 2026-09-05T23:39:24.979+02:00 | M1Y-60s | raw-getStateInformation | 5.18 | 149100 | closed | stopped | message | `201a7cf680d2140da82920290d76fd9c4129aa9966a7d6f4420b9a7cfeeca3c2` |
| 2026-09-05T23:39:24.992+02:00 | M1Y-60s | raw-getStateInformation | 5.53 | 149121 | closed | stopped | message | `48da457a03631f22f547d574d6992535aa789bfa26ccb26b9a89d332846a3b46` |
| 2026-09-05T23:39:25.003+02:00 | M1Y-60s | raw-getStateInformation | 5.13 | 149143 | closed | stopped | message | `418a0a3e5e2758dc70313aae6a77b83f5218618e8e75d1cd4e36396210ce76f0` |
| 2026-09-05T23:39:25.015+02:00 | M1Y-60s | raw-getStateInformation | 5.07 | 149163 | closed | stopped | message | `9618e38acf82eae8dcc8b2f3af9ce5c7e89f60a35c0058713acb852cc0bba640` |
| 2026-09-05T23:39:25.025+02:00 | M1Y-60s | raw-getStateInformation | 5.11 | 149184 | closed | stopped | message | `8c1a3e5af18e7b885397241d0aead6353c1efbd9eb8c870454797d543b834056` |
| 2026-09-05T23:39:25.036+02:00 | M1Y-60s | raw-getStateInformation | 5.16 | 149193 | closed | stopped | message | `1619124a0726d3a8c2bb8f9dd4a4a997f8d303d62c53631299e8bf35d13d4a44` |
| 2026-09-05T23:39:25.046+02:00 | M1Y-60s | raw-getStateInformation | 5.09 | 149208 | closed | stopped | message | `cc8d5e5c93d87253d53244b3fe9d561390a91d07756695f2eee74d0952d271e5` |
| 2026-09-05T23:39:25.057+02:00 | M1Y-60s | raw-getStateInformation | 5.51 | 149224 | closed | stopped | message | `9e6c8565906f38ec39ad594f1c5f0e25d818e52396a168be4a4b20a0fb9d8a7f` |
| 2026-09-05T23:39:25.070+02:00 | M1Y-60s | raw-getStateInformation | 5.86 | 149240 | closed | stopped | message | `1108df4eddb32f3aa14fc9f1e5d3447cb9d29ca0cff3ceff4a2ea92b19d9c71d` |
| 2026-09-05T23:39:25.081+02:00 | M1Y-60s | raw-getStateInformation | 5.24 | 149252 | closed | stopped | message | `bfe527b792b7b419eb94b8af29d96d9e22bda4fbc2aeb6f10d35061438f34d0b` |
| 2026-09-05T23:40:24.981+02:00 | M1Y-120s | raw-getStateInformation | 5.16 | 149267 | closed | stopped | message | `0b76a5eefcf2f66e9b32f4a86bcb059af5506a4cfc753cfe1b8f8995b26e49db` |
| 2026-09-05T23:40:24.993+02:00 | M1Y-120s | raw-getStateInformation | 5.15 | 149288 | closed | stopped | message | `95d507b69f82e3bde776bf003030953a39a6372da4037ba2d34264c00be113b1` |
| 2026-09-05T23:40:25.004+02:00 | M1Y-120s | raw-getStateInformation | 5.03 | 149304 | closed | stopped | message | `3e9e63bce75b1f83cb7b8be475f363669d2381c42ce8fd34a566c0e2e4a1954f` |
| 2026-09-05T23:40:25.014+02:00 | M1Y-120s | raw-getStateInformation | 5.86 | 149317 | closed | stopped | message | `b280dd3755e2dd865eacffc4b095b64d097e390e84c8f8c5923b2efbc0894836` |
| 2026-09-05T23:40:25.027+02:00 | M1Y-120s | raw-getStateInformation | 5.96 | 149336 | closed | stopped | message | `1382a50732b2d88f5f5028bbeadab3e11efa63c842091fafd8a0047a6603fcb7` |
| 2026-09-05T23:40:25.038+02:00 | M1Y-120s | raw-getStateInformation | 5.15 | 149345 | closed | stopped | message | `5f8e29cea37d10de880b6cd21d74f42be90ad0f25d55af99c514acfc6c8ee7d8` |
| 2026-09-05T23:40:25.051+02:00 | M1Y-120s | raw-getStateInformation | 5.09 | 149367 | closed | stopped | message | `6cbb3d130f570c368757966176ac1b7a9727c97fbd49dff34cba567177bc5b32` |
| 2026-09-05T23:40:25.063+02:00 | M1Y-120s | raw-getStateInformation | 5.25 | 149379 | closed | stopped | message | `68fefd7e675ebd85136124fba16a8566df6b86120e2c819821d9022df5d51c05` |
| 2026-09-05T23:40:25.074+02:00 | M1Y-120s | raw-getStateInformation | 5.26 | 149387 | closed | stopped | message | `8a44c41ff40ece5f3874efbbab76871b6b469150eca5832553b83a5dfffb69cc` |
| 2026-09-05T23:40:25.087+02:00 | M1Y-120s | raw-getStateInformation | 4.96 | 149401 | closed | stopped | message | `f07f5286dae74d8c5311b6771e2370fd7b4100e3ce64fc4800c10ef251fd2c7a` |

## Operator notes

*None.*

---
*End of sanitized SPIKE-01 report. Verify before sharing: this file must
contain no base64 blocks and no plugin-state byte dumps.*
