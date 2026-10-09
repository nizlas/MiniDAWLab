# A1/A2 at 48 samples — short performance measurement (2026-10-09)

Follow-up to `PARALLEL_A1A2_LOCAL_VERIFICATION_2026-10-09.md` (which measured at 512 samples).
Same verified candidate binary, same project copy, same passage; only the device buffer differs.
No code change, no new build, no tests. Raw log lines (three runs, 18:12 / 18:14 / 18:16) and the
shutdown-crash classification: `docs/evidence/parallel-a1a2-2026-10-09/stability-run-extract-48samples.log`,
`MiniDAWLab-crash-20261009-181451-pid25832.txt`.

## Setup (verified before measuring)

- Exe: `C:\Users\nicla\development\MiniDAWLab-pr6-a2\build\ninja-release\MiniDAWLab_artefacts\Release\MiniDAWLab.exe`,
  revision `10374e2` (worktree clean), SHA-256 `9849DD84…EEAC6` — unchanged since the 512 report.
  (A1/A2 has since been merged to main as `50951b5`; the measurement uses the verified binary.)
- Project copy: `%TEMP%\dal-perf-100tracks-ab\TSE_pt2_100tracks.dalproj`, byte-identical to the original
  (sha1 `3f585fc5f701…`): 36 audio rows unmuted (guitars included), 48 live instruments
  (16 VB3-II, 16 Groove Agent SE, 16 HALion), 50 inserts (18 AmpliTube 4, 16 Pro-Q 3, 16 DAL Mono Delay),
  cycle on, right locator 1 968 000 samples (41.0 s), 180 BPM 4/4. Original untouched.
- Device as the user set it (nothing changed by me): ASIO Fireface USB, 48 000 Hz, **actual `buf 48`**
  logged at device start in all three runs; engine block = 48 samples → budget 1.000 ms.
  Inputs were active this time (`in: Analog 1 … `), unlike the 512 runs (`in: none`).
- Render pool: 7 workers + callback (8 threads), 84 jobs per block (48 instrument + 36 strip jobs),
  `parallelBlocks = all`, `serialBlocks = 1` (the first block). Cubase closed; no other DAL instance;
  no compiler running. Mixer closed, main window visible.

## Run 1 — passage bars 17–30, profiler on

`--stability-perf-profile <copy> --seconds 19 --warmup 4 --start-seconds 17.333`

| Metric | Value |
|---|---|
| Blocks in window | 19 031 × 48 samples |
| Callback ms min / mean / max | 0.207 / **0.439** / **1.131** |
| Budget share mean / max | **43.9 %** / **113.1 %** |
| nearOverruns (>70 %) / overruns (≥100 %) | 444 / **6** |
| Callback start interval mean / max; late starts (>1.25×) | 1.000 / 1.429 ms; **4** |
| Generation section wall mean / max (dispatch → join) | 0.396 / 1.077 ms |
| Callback idle wait at join mean / max | 0.018 / 0.292 ms |
| Effective parallelism (instrument CPU / section wall) | 4.97 |
| Remainder (DAL work outside plug-ins) mean / max | 0.043 / 0.269 ms |
| Instrument CPU (summed over threads) mean / max | 1.967 / 5.408 ms, 48 calls/block |
| Insert CPU (summed) mean / max | 0.769 / 2.552 ms, 44.5 calls/block |
| Transport | 1 025 135 → 1 938 431 samples = 19.03 s in 19.03 s wall (bar 22.3 → 31.2) |
| Instruments | 48 hosts with live processBlock, 0 proxy-mixed, 0 with neither |
| Output | peak 0.760, overs 0/0, nonFinite 0 |
| Process | 294.5 % of one core (12.3 % of 24), working set 9.0 GB |
| Exit | code 0, no crash dump |

Worst block (1.131 ms): section wall 1.077 + remainder 0.054 — the whole overrun is the parallel
generation section (84 jobs on 8 threads at a 1 ms deadline); callback-side join wait at most 0.29 ms.

## Run 2 — cycle wrap (start 35 s, warm-up 2 s, 15 s window), profiler on

| Metric | Value |
|---|---|
| Blocks in window | 15 015 |
| Callback ms min / mean / max | 0.190 / 0.386 / **11.612** |
| Budget share mean / max | 38.6 % / **1161 %** |
| nearOverruns / overruns | 43 / **7** |
| Start interval mean / max; late starts | 1.001 / **11.626** ms; **8** |
| Worst block | 11.613 ms: insert-plugin summed CPU **74.458** ms, section wall 11.559, instrument CPU 1.906, remainder 0.054 |
| Section wall mean / max | 0.348 / 11.559 ms |
| Transport | 1 778 352 → 530 880 (wrapped at 1 968 000 and continued; 15.02 s in 15.02 s wall) |
| Output | peak 0.579, overs 0/0, nonFinite 0; 48 instruments live |
| Exit | scenario PASS, then the known shutdown crash (see below) |

The spike is the same one seen at 512 (AmpliTube 4 chains doing extra work on the transport
discontinuity: ~74 ms summed insert CPU across the strip jobs). At 512 the candidate absorbed it
as one 13.1 ms block (1 overrun, 123 %); at 48 the same work costs 11.6 ms against a 1 ms budget,
i.e. about eleven missed periods in a row, which the 11.6 ms maximum start interval and 8 late
starts also show. Away from the wrap the run is quieter than the passage (mean 38.6 %, 43 near-overruns).

## Run 3 — control: passage without profiler (`--profile-off`)

Done because 6 overruns at a 1.13 ms maximum is a thin margin where profiler sampling could matter.

| Metric | Profiler on (run 1) | Profiler off (run 3) |
|---|---|---|
| Blocks | 19 031 | 19 070 |
| Callback mean / max | 0.439 / 1.131 ms | 0.461 / 1.769 ms |
| Budget share mean / max | 43.9 % / 113 % | 46.1 % / 177 % |
| nearOverruns / overruns | 444 / 6 | 356 / 3 |
| Output peak / overs / nonFinite | 0.760 / 0 / 0 | 0.759 / 0 / 0 |
| Exit | 0, no dump | 0, no dump |

The profiler is not what causes the overruns: without it there are still 3 isolated blocks over
budget (one at 1.77 ms) in 19 s, and the mean is the same. Noise between two runs (3 vs 6 overruns)
is of the same size as the effect, so there is no reason to run more.

## Comparison with the candidate at 512 (same passage, same binary)

| | 512 samples (budget 10.67 ms) | 48 samples (budget 1.00 ms) |
|---|---|---|
| Callback mean / max | 2.30 / 4.77 ms | 0.44 / 1.13 ms (control: 0.46 / 1.77) |
| Budget share mean / max | 21.5 % / 44.7 % | **43.9 % / 113 %** (control 46 % / 177 %) |
| Overruns in the passage | 0 of 1 786 blocks | 6 of 19 031 (control 3 of 19 070) |
| Late starts | 0 | 4 |
| Wrap worst block | 13.1 ms = 123 % (1 overrun) | 11.6 ms = 1161 % (7 overruns, 8 late starts) |
| Instruments live / inserts per block | 48 / 44.5 | 48 / 44.5 |

Time per block went down but cost per sample doubled (0.44 ms / 48 ≈ 9.1 µs per sample vs
2.30 ms / 512 ≈ 4.5 µs): the per-block overhead of 84 job dispatches and ~92 plug-in calls is paid
10.7× more often. That is what moves the mean from 21.5 % to 44 % of the budget and leaves the
isolated 1.1–1.8 ms blocks over it.

## Answers

1. **Does DAL actually run at 48 samples?** Yes. The device reported `buf 48` at start in all three
   runs, the engine processed 48-sample blocks with a 1.000 ms budget, the mean callback start
   interval was 1.000 ms, and 19 031 blocks × 48 samples = 19.03 s of transport progress in 19.03 s wall.
2. **Does it keep up during the passage (bars 17–30)?** Mostly, with a thin margin: mean load
   44–46 % of the 1 ms budget (vs 21.5 % at 512), but 3–6 isolated blocks per 19 s exceed the
   budget (max 1.13–1.77 ms) and 4 callback starts were late. Each such block is a single period
   missed by ≤ 0.8 ms. Whether that is audible depends on the RME driver's output buffering
   (reported output latency 88 samples); the user reports clean audio, and the measurement does not
   contradict that, but it does not prove inaudibility either. No overs, no non-finite output, all
   48 instruments live, transport in real time.
3. **What happens at the cycle wrap?** The known AmpliTube chain spike (~74 ms summed insert CPU)
   becomes an 11.6 ms callback — eleven budgets long at 48 samples — with 7 overruns and 8 late
   starts clustered at the wrap. At 512 the same spike fitted in 1.2 budgets. This is an insert
   (plug-in) cost at the transport discontinuity, not generation or DAL overhead (remainder 0.054 ms);
   it is not made worse by A1/A2 (at 512 the baseline's wrap block was 38.5 ms vs the candidate's 13.1).
   Expect an audible dropout at the wrap at 48 samples with this project.

## Shutdown crash (separate from playback)

Run 2 exited with −1073741819 after `RESULT: PASS`: `AmpliTube 4.vpa+0x7675E`, 0xC0000005, last
operation "app shutdown begin" — the same module offset and phase as in the four 512 runs (baseline
and candidate). Runs 1 and 3 exited with code 0 and no dump. It happens during plug-in teardown, after
all playback results were collected, and is pre-existing (not introduced by A1/A2 or by 48 samples).

## Not done

No optimisation, no wider troubleshooting, no merge, no packaging. No device settings were changed.
Listening is the user's; this document reports only what was measured.
