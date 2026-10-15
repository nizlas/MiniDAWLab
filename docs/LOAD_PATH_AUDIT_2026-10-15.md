# Project load path audit (2026-10-15)

Reviewed revision: `a761c98` on `main`. The only load-path change after that revision is below: a matched VST3 description is reused until the plugin file's modification time changes. Product version stays 1.3.1. Project format stays 31.

Cubase 13 was already running and was left running. Measurements are separate DAL processes on project copies. A new process is not a cold disk cache. The earlier 45 s and 65.4 s figures are not an A/B and are not used as a regression.

Copies only: `%TEMP%\dal-load-audit\repeated-plugins.dalproj` (3× VB3-II, Groove Agent SE, HALion Sonic, AmpliTube 4, Pro-Q 3, DAL Mono Delay, same saved state as the small project, no clips) and `%TEMP%\dal-close-progress\100tracks\`. Originals were not opened.

## Where the time goes

Each insert load does, on the message thread, in this order:

1. `findAllTypesForFile` for the saved path (JUCE 8.0.4 caches the module handle, not the description list).
2. `createPluginInstance`. The VST3 wrapper ignores the sample-rate and block-size arguments. `initialise` includes `synchroniseStates`: one component `getState`, then controller `setComponentState` and a parameter reset. That round trip is the wrapper connecting a separate controller. It is not a second restore of the project blob.
3. One bus setup and `prepareToPlay` (`releaseResources` on a never-activated instance returns immediately, then `setBusesLayout`, `setupProcessing`, bus activate, `setActive`).
4. One `setStateInformation`: component `setState`, controller `setComponentState`, controller `setState`, parameter reset. Processor state and controller state are both required.
5. A second `prepareToPlay` only if the main bus is no longer stereo. It did not run on these projects (`prepareAfterMs=0`).
6. No `getStateInformation` after restore on the load path. The in-place path that reads state back is for a chain that is already live, and project load does not take it.
7. One realtime-map publish for the finished chain. Rebuilding the map does not `prepare` instances that are already loaded. Publish and the empty retire before a new chain measured 0 ms, including on the 50th chain.

Each instrument does one `createPluginInstance`, one bus/`prepareToPlay`, and one `setStateInformation`. `unload` on a fresh host measured 0 ms (the 100 ms sleep runs only when a live owner is replaced). The next instance starts when the previous message-thread call returns. There is no extra sleep or poll between instances. Groove Agent kit state is applied by `setStateInformation`; DAL does not wait further before the next track, and an unfinished instrument is still not published for render before that restore returns.

Generic VST3 resolution re-reads `instrument-catalog-v1.xml` (1.8 KB) per track. Sixteen reads were 20 ms. Not worth a second cache.

## 100-track copy, before the description reuse

Release, Cubase 13 left open. `load: complete elapsedMs=74967`. Decode 327 ms (3 files). Apply model 701 ms. Fifty chains, eighteen AmpliTube 4, sixteen Pro-Q 3, sixteen DAL Mono Delay. Instruments: sixteen Groove Agent SE, sixteen VB3-II, sixteen HALion Sonic. Every AmpliTube state blob was 7259 bytes.

| Plugin | n | describe | create | prepare | setState | prepare again |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| AmpliTube 4 | 18 | 20120 | 19701 | 2561 | 16 | 0 |
| Pro-Q 3 | 16 | 54 | 255 | 0 | 0 | 0 |
| DAL Mono Delay | 16 | 24 | 26 | 21 | 0 | 0 |
| Groove Agent SE | 16 | — | 5042 | 328 | 2748 | 0 |
| VB3-II | 16 | — | 3976 | 2319 | 39 | 0 |
| HALion Sonic | 16 | — | 2793 | 311 | 1981 | 0 |

AmpliTube describe was 1967 ms the first time and 951–1178 ms on each of the other seventeen. Those walks are inside `findAllTypesForFile`, not inside `setState`. Create stayed about one second per further instance after the module was already loaded. Map publish summed to 0 ms. Catalog reads summed to 20 ms.

The logged instrument steps sum to 23.3 s and the insert steps to 43.5 s. About 3.8 s and 0.7 s of those are DAL work around the plugin calls (base64, cache read, logging). About 7 s of the 75.0 s total sits between the step logs (the 4 ms yield, progress painting, and other message-loop work). That remainder was not split further.

## Change

`pickDescriptionForSavedIdentity` keeps a matched `PluginDescription` keyed by path, saved identifier, and file modification time. A different plugin at the same path still misses and is not given the other plugin's state. JUCE's module cache is unchanged.

## Same 100-track copy, after

`load: complete elapsedMs=50209`. Same instance counts. AmpliTube state still 7259 bytes on every instance. Describe was 1404 ms once, then 0 ms seventeen times. Create stayed 19489 ms.

| | Before (first process) | After (later process) |
| --- | ---: | ---: |
| Total | 74967 | 50209 |
| AmpliTube describe | 20120 | 1404 |
| AmpliTube create | 19701 | 19489 |
| AmpliTube prepare | 2561 | 2249 |
| AmpliTube setState | 16 | 17 |

The description reuse accounts for 18.7 s (20120 − 1404). The other ~6 s of the total drop is the later process: instrument create and setState were lower with the modules already loaded, and the cache does not touch those calls.

The repeated-plugin fixture showed the same shape. Before: AmpliTube describe 1955, 945, 945 ms. After: 1953, 0, 0 ms. State bytes stayed 7259.

## What was checked

- Eighteen AmpliTube, sixteen Pro-Q 3, sixteen DAL Mono Delay, sixteen of each instrument, same state byte sizes as before the change.
- Pre/Post, order, and routing are still the saved chain fields. The restore path that assigns them was not changed. No insert came back as unavailable.
- The small-project copy played after load: saved playhead 297216, after 1200 ms `playheadSamples=341760`, intent Playing. Close of that copy and of the three-AmpliTube fixture logged `close: done`.
- The 100-track process was stopped after `load: complete`, so the known AmpliTube teardown crash was not re-run. It did not occur on the loads or on the smaller closes. No crash dump was produced by these runs.
- Progress is still one chain and one instrument per turn. The map publish still happens after that chain's instances exist. Nothing is released during playback; the play check ran only after load complete.

## What remains

The largest remaining cost is one `createPluginInstance` per AmpliTube (about 19.5 s for eighteen instances) plus Groove Agent / HALion / VB3 create, prepare, and setState (about 16–20 s). Those calls are the plugin and the JUCE wrapper on the message thread. A second prepare and a state read-back are not in this path. Overlapping instance creation would move VST3 setup off the message thread, which JUCE 8.0.4 does not allow, and is out of scope here.

## Cubase, manual

No Cubase project was modified, and this audit does not describe Cubase's loader. To compare like with like: a new empty Cubase project, the same instance counts and the same presets (16 Groove Agent SE, 16 HALion Sonic, 16 VB3-II, 18 AmpliTube 4, 16 Pro-Q 3, 16 DAL Mono Delay). Time from open until playback produces the correct sound. On this machine, with Cubase left open, DAL's comparable figure after the description reuse is 50.2 s for the 100-track copy.
