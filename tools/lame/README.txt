Bundled LAME (MP3 encoder) — runtime layout and developer copy
=============================================================

MiniDAWLab / Danielssons Audio Lab encodes MP3 mixdowns by spawning an external LAME executable.
The repository does not ship `lame.exe`; you obtain it yourself (same Windows ABI as the app).

Runtime layout (what the app looks for — beside MiniDAWLab.exe):

  Tools/lame/lame.exe

Example after a local Debug build:

  .../MiniDAWLab_artefacts/Debug/MiniDAWLab.exe
  .../MiniDAWLab_artefacts/Debug/Tools/lame/lame.exe

Developer convenience (CMake post-build)
----------------------------------------

If you keep a local encoder at:

  external_tools/lame/lame.exe

then each successful build of the `MiniDAWLab` target copies it to
`<target output dir>/Tools/lame/lame.exe` (Debug and Release presets alike). Reconfigure is not
required after adding the file; the next build copies it. If the source file is missing, configure
and build still succeed; MP3 export is disabled until `lame.exe` is present beside the executable.

Packaging (Windows)
---------------------

`scripts/package-windows.ps1` stages the Release executable under `dist/` and builds a zip / Inno
installer from that tree. When `external_tools/lame/lame.exe` exists, the script also stages
`Tools/lame/lame.exe`. License placeholders under `licenses/LAME/` are staged as
`licenses/LAME/` when present. If `lame.exe` is absent, packaging still succeeds and MP3 remains
disabled until the user adds the encoder manually.

See ../licenses/LAME/ for license placeholders and notices.
See ../../external_tools/lame/README.txt for the optional dev-only source path.
