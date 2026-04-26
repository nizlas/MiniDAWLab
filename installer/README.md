# Windows installer (Inno Setup 6)

The [`MiniDAWLab.iss`](MiniDAWLab.iss) script produces **`dist\MiniDAWLab-<version>-Setup.exe`**. It installs the app under **Program Files**, places a **Start Menu** shortcut, offers an **optional desktop icon**, runs the **Microsoft Visual C++ 2015-2022 Redistributable (x64)** from an embedded `vc_redist.x64.exe` (no online download in the installer), and does **not** register a `.dalproj` file association.

## Prerequisites

- **Inno Setup 6** (includes `ISCC.exe`). Typical path: `C:\Program Files (x86)\Inno Setup 6\ISCC.exe`
- A **staged** tree at `dist\MiniDAWLab-<version>\` and `dist\vendor\vc_redist.x64.exe`, normally produced by:

```powershell
cd <repo>
.\scripts\package-windows.ps1
```

`package-windows.ps1` parses the version from the root **`CMakeLists.txt`**, can run the **Release** build, downloads the redistributable once to `dist\vendor\`, creates the zip, and runs `ISCC` if it is found on PATH or under the default Inno install path.

## Manual compile (without the packaging script)

After staging `dist\MiniDAWLab-0.1.0\` and `dist\vendor\vc_redist.x64.exe` yourself:

```powershell
& "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe" /DAppVersion=0.1.0 "installer\MiniDAWLab.iss"
```

Run from the **repository root** (or use full paths in the script). The output is written to **`dist\`**.

**Note:** The `[Files]` section expects paths relative to the `.iss` file location (`..\dist\...`).
