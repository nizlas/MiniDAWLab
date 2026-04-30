# MiniDAWLab
Detta projekt är i första hand ett hobby- och lärprojekt för att förstå audio engines, DAW-arkitektur, routing, playback, MIDI, GUI och pluginrelaterad infrastruktur bättre.

## Build and run (Windows)

### Toolchain

You need:

1. **Visual Studio 2022 or later** (Community is fine) or **Build Tools**, with the **Desktop development with C++** workload so MSVC and the Windows SDK are installed.
2. **CMake** 3.22 or newer. Example: `winget install Kitware.CMake`
3. **Ninja** — optional if you use Visual Studio’s environment: **Developer PowerShell for Visual Studio** (or the script below) puts Visual Studio’s bundled `ninja.exe` on `PATH`. Otherwise install Ninja, e.g. `winget install Ninja-build.Ninja`, and open a fresh terminal.

The first configure downloads **JUCE** with CMake `FetchContent` (internet required).

### Option A — Recommended: helper script (any PowerShell)

From the repository root, MSVC and Ninja are set up via `VsDevCmd.bat`, then CMake presets are used:

```powershell
.\scripts\build-windows.ps1
```

Release build:

```powershell
.\scripts\build-windows.ps1 -Config Release
```

Re-configure only or build only:

```powershell
.\scripts\build-windows.ps1 -ConfigureOnly
.\scripts\build-windows.ps1 -BuildOnly
```

### Option B — Developer PowerShell + CMake presets

Open **Developer PowerShell for Visual Studio** (x64), `cd` to the repo, then:

```powershell
cmake --preset windows-ninja-debug
cmake --build --preset windows-ninja-debug
```

### Option C — Manual CMake (Ninja + MSVC on PATH)

Use **x64 Native Tools Command Prompt for VS** or run `VsDevCmd.bat` so `cl.exe` and Ninja are available. From the repo root:

```powershell
cmake -B build/ninja-debug -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build/ninja-debug
```

### Run the app

JUCE places the executable under the build tree, for example:

- After **presets** or **Option C** with `build/ninja-debug`:  
  `.\build\ninja-debug\MiniDAWLab_artefacts\Debug\MiniDAWLab.exe`
- After **Release** preset:  
  `.\build\ninja-release\MiniDAWLab_artefacts\Release\MiniDAWLab.exe`

### Windows packaging (Release zip + optional Inno Setup)

From the repository root, after a Release build (or let the script build for you):

```powershell
.\scripts\package-windows.ps1
```

This reads the version from `CMakeLists.txt` (`project(MiniDAWLab VERSION …)`), stages `dist\DanielssonsAudioLab-<version>\` (Release `MiniDAWLab.exe` plus bundled docs), ensures `dist\vendor\vc_redist.x64.exe` exists (downloaded once from Microsoft), writes `dist\DanielssonsAudioLab-<version>.zip`, and **if** [Inno Setup 6](https://jrsoftware.org/isinfo.php)’s `ISCC.exe` is on `PATH` or in the default install location, compiles `installer\MiniDAWLab.iss` to **`dist\DanielssonsAudioLab-<version>-Setup.exe`**.

- Skip the build step (only if `build\ninja-release\…\Release\MiniDAWLab.exe` already exists): `.\scripts\package-windows.ps1 -SkipBuild`
- Override the version string (advanced): `.\scripts\package-windows.ps1 -Version 0.2.0`

Details and hand compilation: [installer/README.md](installer/README.md).

### Notes

- The top-level `project()` enables **C and CXX** because JUCE brings in C sources; this avoids CMake configure errors with recent CMake versions.
- If `cmake` is not found after installing with winget, open a **new** terminal or use `& "$env:ProgramFiles\CMake\bin\cmake.exe"`.
