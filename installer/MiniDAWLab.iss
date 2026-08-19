; MiniDAWLab — Inno Setup 6. Compile from repository root, or via scripts\package-windows.ps1
;   "ISCC.exe" /DAppVersion=0.2.0 "installer\MiniDAWLab.iss"
; Staged payload: dist\DanielssonsAudioLab-{#AppVersion}\ (created by package-windows.ps1)
;   Includes optional Tools\lame\lame.exe and licenses\LAME\ when package-windows.ps1 staged them.
; Embeds: dist\vendor\vc_redist.x64.exe (official Microsoft x64 redistributable)

#define AppName "Danielssons Audio Lab"
; Default matches CMake project(); override: /DAppVersion=x.y.z
#define AppVersion "0.2.0"

[Setup]
AppId={{2F8C9A1B-0D3E-4F5A-8B2C-9E1D4A6F0B2C}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher=MiniDAWLab
DefaultDirName={autopf}\{#AppName}
DefaultGroupName={#AppName}
OutputDir=..\dist
OutputBaseFilename=DanielssonsAudioLab-{#AppVersion}-Setup
Compression=lzma2/ultra
SolidCompression=yes
WizardStyle=modern
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
DisableProgramGroupPage=no
UninstallDisplayIcon={app}\MiniDAWLab.exe
; .dalproj file association below ([Registry]) — tells Explorer to refresh icons/associations.
ChangesAssociations=yes

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "Create a &desktop icon"; GroupDescription: "Additional icons:"; Flags: unchecked

[Files]
; Release payload and docs (staged by package-windows.ps1)
Source: "..\dist\DanielssonsAudioLab-{#AppVersion}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs
; Microsoft Visual C++ 2015-2022 Redistributable (x64) — run silently before first launch
Source: "..\dist\vendor\vc_redist.x64.exe"; DestDir: "{tmp}"; Flags: deleteafterinstall

[Registry]
; Associate .dalproj with the app: double-click in Explorer launches MiniDAWLab.exe "<path>",
; which loads the project via the command-line open path (see Main.cpp).
Root: HKA; Subkey: "Software\Classes\.dalproj"; ValueType: string; ValueName: ""; ValueData: "DanielssonsAudioLab.Project"; Flags: uninsdeletevalue
Root: HKA; Subkey: "Software\Classes\DanielssonsAudioLab.Project"; ValueType: string; ValueName: ""; ValueData: "Danielssons Audio Lab Project"; Flags: uninsdeletekey
Root: HKA; Subkey: "Software\Classes\DanielssonsAudioLab.Project\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\MiniDAWLab.exe,0"
Root: HKA; Subkey: "Software\Classes\DanielssonsAudioLab.Project\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\MiniDAWLab.exe"" ""%1"""

[Icons]
Name: "{group}\{#AppName}"; Filename: "{app}\MiniDAWLab.exe"
Name: "{commondesktop}\{#AppName}"; Filename: "{app}\MiniDAWLab.exe"; Tasks: desktopicon

[Run]
Filename: "{tmp}\vc_redist.x64.exe"; Parameters: "/install /quiet /norestart"; StatusMsg: "Installing Microsoft Visual C++ 2015-2022 x64 redistributable..."; Flags: waituntilterminated
Filename: "{app}\MiniDAWLab.exe"; Description: "Launch {#AppName}"; Flags: nowait postinstall skipifsilent
