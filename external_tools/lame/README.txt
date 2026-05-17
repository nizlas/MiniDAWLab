Developer-local LAME (optional, not committed)
==============================================

Place your Windows LAME build here:

  external_tools/lame/lame.exe

CMake runs a post-build step that copies this file next to MiniDAWLab.exe:

  <target output dir>/Tools/lame/lame.exe

`scripts/build-windows.ps1` runs the same sync after a successful build so incremental Ninja builds
(still “up to date”, no link step) still pick up a newly added `external_tools/lame/lame.exe`.

Examples after Ninja presets:

  build/ninja-debug/MiniDAWLab_artefacts/Debug/Tools/lame/lame.exe
  build/ninja-release/MiniDAWLab_artefacts/Release/Tools/lame/lame.exe

The application still resolves the encoder only from the folder beside the running executable;
this directory is only the **source** supplied by the developer.

If this file is absent, configure and build succeed; MP3 mixdown stays disabled until you copy
lame.exe manually or restore it here and rebuild.

See also: Tools/lame/README.txt and licenses/LAME/
