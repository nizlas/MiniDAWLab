# VST3 insert tempo transfer — verification (2026-09-28)

## Scope and root cause

DAL’s `PluginInsertHost` prepared and called each `juce::AudioPluginInstance`, but never
assigned an `AudioPlayHead`. Therefore `AudioProcessor::getPlayHead()` returned null inside
DAL Mono Delay. Its documented fallback was correctly used: 120 BPM.

JUCE 8.0.4 maps an assigned playhead’s `PositionInfo::getBpm()` to VST3
`ProcessContext::tempo` and `kTempoValid` in
`juce_audio_processors_headless/format_types/juce_VST3PluginFormatImpl.h`. The fault was
upstream of the plug-in and no DAL Mono Delay source, VST3 identity, parameter ID or saved
state was changed.

## Change

`PluginInsertHost` now owns one stable `AudioPlayHead` for the lifetime of all of its insert
instances. `PlaybackEngine` updates the already-allocated playhead context immediately before
each insert-processing phase:

- audio clip Pre/Post chains receive the current timeline segment;
- live input monitoring receives the current project BPM even with stopped transport;
- instrument and group/master insert chains receive their callback-block position;
- offline rendering receives its export timeline position and project BPM, not the live
  `Transport` playhead.

The context exposes only facts DAL knows: sample position/time, BPM, meter, PPQ/bar position,
playing/recording state and valid loop points. JUCE then emits the corresponding standard VST3
process-context flags. The object is never replaced while a processor can use it; live callback
and offline rendering are already mutually exclusive through the existing offline gate.

## Automated production-host evidence

Built and ran the focused test using DAL’s production `PluginInsertHost` and the built
`DALMonoDelay.vst3` bundle:

```powershell
build\ninja-debug\PluginInsertTempoFocusedTests_artefacts\Debug\PluginInsertTempoFocusedTests.exe `
  C:\Users\nicla\development\DALMonoDelay\Builds\VisualStudio2022\x64\Release\VST3\DALMonoDelay.vst3
```

Results at 48 kHz, using the plug-in’s public defaults (Sync, 1/4 triplet, 49% wet):

- stopped input-processing context, 180 BPM: echo peak at sample **10,667**
  (≈ **222.23 ms**, expected 222.22 ms);
- playing context changed to 120 BPM without reload: echo peak at sample **16,000**
  (≈ **333.33 ms**, expected 333.33 ms).

Before the fix, the host supplied no playhead, so both cases use DAL Mono Delay’s 120 BPM
fallback: 16,000 samples. The stopped 180-BPM assertion therefore fails on the pre-fix code.

Debug app build and focused test build both succeeded. ASIO remained enabled during CMake
configuration.

## Remaining verification gaps

- The focused regression validates DAL’s real insert host and the shipped VST3 bundle, but it
  does not drive the rendered DAL GUI or a hardware input. The stopped case is synthetic input
  monitoring at the production host boundary, not a hardware-monitoring claim.
- The bundle’s default 1/4-triplet setting measured the required 180-BPM triplet case. The
  180-BPM straight-quarter (≈333.33 ms) requires a manual parameter interaction in DAL; this
  focused host test deliberately does not add a plug-in-specific parameter-control backdoor.
- Offline position/BPM propagation, seek/loop context and Pre/Post/group/master code paths are
  covered by the common context wiring review, but have not yet been measured with a separate
  rendered impulse after this change.
- Cubase has not been exercised. Its standards compliance is supported by the existing separate
  JUCE VST3-host test, not by a Cubase session; it remains a manual verification.
