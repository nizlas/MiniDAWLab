#Requires -Version 5.1
<#
.SYNOPSIS
  Symbolizes MiniDAWLab crash addresses (function + file + line) from a PDB, without any
  extra tooling — uses dbghelp.dll which ships with Windows.

.DESCRIPTION
  Crash reports written by the in-app crash handler (%APPDATA%\MiniDAWLab\crash-dumps\
  MiniDAWLab-crash-*.txt) contain a "module offset" line. This script resolves such
  module-relative offsets against a MiniDAWLab.exe + MiniDAWLab.pdb pair from the SAME build.

  Release symbol pairs are archived by scripts\package-windows.ps1 under:
    dist\symbols\DanielssonsAudioLab-<version>\   (MiniDAWLab.exe + MiniDAWLab.pdb)
  Debug builds keep the pair next to each other under:
    build\ninja-debug\MiniDAWLab_artefacts\Debug\

.EXAMPLE
  # Symbolize straight from a crash report (uses the report's "module offset" line):
  .\scripts\symbolize-crash.ps1 -CrashText "$env:APPDATA\MiniDAWLab\crash-dumps\MiniDAWLab-crash-20260827-193000-pid1234.txt" -Exe .\dist\symbols\DanielssonsAudioLab-0.2.0\MiniDAWLab.exe

.EXAMPLE
  # Symbolize one or more raw offsets (e.g. "Fault offset" from Windows Event Log ID 1000):
  .\scripts\symbolize-crash.ps1 -Offset 0x22c76b -Exe .\build\ninja-debug\MiniDAWLab_artefacts\Debug\MiniDAWLab.exe

.EXAMPLE
  # A .dmp file: prints instructions for full-stack analysis in Visual Studio.
  .\scripts\symbolize-crash.ps1 -Dump "$env:APPDATA\MiniDAWLab\crash-dumps\MiniDAWLab-crash-20260827-193000-pid1234.dmp"
#>
param(
    # Path to a MiniDAWLab-crash-*.txt crash report; its "module offset" line is symbolized.
    [string] $CrashText,

    # One or more module-relative offsets (hex like 0x22c76b, or decimal).
    [string[]] $Offset,

    # Path to the MiniDAWLab.exe whose .pdb sits beside it. Must be the same build that crashed.
    # Defaults to the local Release exe, falling back to the local Debug exe.
    [string] $Exe,

    # Path to a .dmp minidump: prints how to open it with full stacks in Visual Studio.
    [string] $Dump
)

$ErrorActionPreference = 'Stop'
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path

if ($Dump) {
    if (-not (Test-Path -LiteralPath $Dump)) { Write-Error "Dump not found: $Dump" }
    $full = (Resolve-Path -LiteralPath $Dump).Path
    Write-Host "Minidump: $full" -ForegroundColor Cyan
    Write-Host ''
    Write-Host 'Full-stack analysis in Visual Studio:'
    Write-Host '  1. Open Visual Studio -> File -> Open -> File... -> select the .dmp.'
    Write-Host '  2. In the Minidump File Summary page, click "Set symbol paths" and add the'
    Write-Host '     folder holding the matching MiniDAWLab.pdb:'
    Write-Host '       - Release builds: dist\symbols\DanielssonsAudioLab-<version>\'
    Write-Host '       - Debug builds:   build\ninja-debug\MiniDAWLab_artefacts\Debug\'
    Write-Host '  3. Click "Debug with Native Only". The crashing thread opens at the faulting'
    Write-Host '     line with a full call stack (Debug > Windows > Call Stack).'
    Write-Host ''
    Write-Host 'Quick single-frame lookup without Visual Studio: re-run this script with the'
    Write-Host '"module offset" from the adjacent MiniDAWLab-crash-*.txt and -Exe <matching exe>.'
    if (-not $CrashText -and -not $Offset) { exit 0 }
}

# Collect offsets: explicit -Offset values plus any parsed from -CrashText.
$offsets = @()
foreach ($o in ($Offset | Where-Object { $_ })) { $offsets += $o }

if ($CrashText) {
    if (-not (Test-Path -LiteralPath $CrashText)) { Write-Error "Crash report not found: $CrashText" }
    $lines = Get-Content -LiteralPath $CrashText
    $moduleLine = $lines | Where-Object { $_ -match '^faulting module:\s*(.+)$' } | Select-Object -First 1
    if ($moduleLine -match '^faulting module:\s*(.+)$') {
        Write-Host "Faulting module (from report): $($Matches[1].Trim())" -ForegroundColor DarkGray
    }
    $offsetLine = $lines | Where-Object { $_ -match '^module offset:\s*(0x[0-9a-fA-F]+)' } | Select-Object -First 1
    if ($offsetLine -match '^module offset:\s*(0x[0-9a-fA-F]+)') {
        $offsets += $Matches[1]
        Write-Host "Module offset (from report): $($Matches[1])" -ForegroundColor DarkGray
    } else {
        Write-Error "No 'module offset: 0x...' line found in: $CrashText"
    }
}

if ($offsets.Count -eq 0) {
    Write-Error 'Nothing to symbolize. Pass -CrashText <crash .txt>, -Offset <hex offset>, or -Dump <.dmp>.'
}

# Resolve the exe (its .pdb must sit beside it and be from the same build as the crash).
if (-not $Exe) {
    $candidates = @(
        (Join-Path $repoRoot 'build\ninja-release\MiniDAWLab_artefacts\Release\MiniDAWLab.exe'),
        (Join-Path $repoRoot 'build\ninja-debug\MiniDAWLab_artefacts\Debug\MiniDAWLab.exe')
    )
    $Exe = $candidates | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
    if (-not $Exe) { Write-Error 'No -Exe given and no local build found. Pass -Exe <MiniDAWLab.exe with .pdb beside it>.' }
    Write-Host "Using exe: $Exe" -ForegroundColor DarkGray
}
$Exe = (Resolve-Path -LiteralPath $Exe).Path
$pdb = [IO.Path]::ChangeExtension($Exe, '.pdb')
if (-not (Test-Path -LiteralPath $pdb)) {
    Write-Error "No PDB beside the exe: $pdb`nRelease symbol pairs are archived under dist\symbols\DanielssonsAudioLab-<version>\."
}
Write-Warning 'The exe/PDB pair must come from the SAME build as the crash, or results are garbage.'

# dbghelp-based lookup: SymFromAddr + SymGetLineFromAddr64 against a fixed load base.
$cs = @"
using System;
using System.Runtime.InteropServices;
public static class MiniDawSym
{
    [DllImport("dbghelp.dll", SetLastError=true)] static extern bool SymInitialize(IntPtr hProcess, string UserSearchPath, bool fInvadeProcess);
    [DllImport("dbghelp.dll", SetLastError=true)] static extern ulong SymLoadModuleEx(IntPtr hProcess, IntPtr hFile, string ImageName, string ModuleName, ulong BaseOfDll, uint DllSize, IntPtr Data, uint Flags);
    [DllImport("dbghelp.dll", SetLastError=true)] static extern bool SymFromAddr(IntPtr hProcess, ulong Address, out ulong Displacement, IntPtr Symbol);
    [DllImport("dbghelp.dll", SetLastError=true)] static extern bool SymGetLineFromAddr64(IntPtr hProcess, ulong Address, out uint Displacement, ref IMAGEHLP_LINE64 Line);
    [DllImport("dbghelp.dll")] static extern uint SymSetOptions(uint opts);
    [StructLayout(LayoutKind.Sequential)]
    struct IMAGEHLP_LINE64 { public uint SizeOfStruct; public IntPtr Key; public uint LineNumber; public IntPtr FileName; public ulong Address; }

    public static void Lookup(string exePath, ulong[] offsets)
    {
        IntPtr h = new IntPtr(0x4d445753); // arbitrary unique handle for this session
        SymSetOptions(0x00000010 | 0x00000002); // SYMOPT_LOAD_LINES | SYMOPT_UNDNAME
        if (!SymInitialize(h, System.IO.Path.GetDirectoryName(exePath), false))
        { Console.WriteLine("SymInitialize failed (err " + Marshal.GetLastWin32Error() + ")"); return; }
        ulong baseAddr = SymLoadModuleEx(h, IntPtr.Zero, exePath, null, 0x140000000UL, 0, IntPtr.Zero, 0);
        if (baseAddr == 0)
        { Console.WriteLine("SymLoadModuleEx failed (err " + Marshal.GetLastWin32Error() + ") — wrong or missing PDB?"); return; }

        const int maxName = 1024;
        IntPtr sym = Marshal.AllocHGlobal(88 + maxName);
        foreach (ulong off in offsets)
        {
            ulong addr = baseAddr + off;
            Console.WriteLine("offset 0x" + off.ToString("x") + ":");
            Marshal.WriteInt32(sym, 0, 88);            // SYMBOL_INFO.SizeOfStruct
            Marshal.WriteInt32(sym, 80, maxName - 1);  // SYMBOL_INFO.MaxNameLen
            ulong disp;
            if (SymFromAddr(h, addr, out disp, sym))
            {
                string name = Marshal.PtrToStringAnsi(new IntPtr(sym.ToInt64() + 84));
                Console.WriteLine("  function: " + name + " +0x" + disp.ToString("x"));
            }
            else { Console.WriteLine("  function: <not found> (err " + Marshal.GetLastWin32Error() + ")"); }
            var line = new IMAGEHLP_LINE64(); line.SizeOfStruct = (uint)Marshal.SizeOf(typeof(IMAGEHLP_LINE64));
            uint ldisp;
            if (SymGetLineFromAddr64(h, addr, out ldisp, ref line))
            { Console.WriteLine("  source:   " + Marshal.PtrToStringAnsi(line.FileName) + " line " + line.LineNumber); }
            else { Console.WriteLine("  source:   <no line info>"); }
        }
        Marshal.FreeHGlobal(sym);
    }
}
"@
Add-Type -TypeDefinition $cs

$parsed = @()
foreach ($o in $offsets) {
    $s = "$o".Trim()
    if ($s -match '^0x') { $parsed += [Convert]::ToUInt64($s.Substring(2), 16) }
    else { $parsed += [Convert]::ToUInt64($s) }
}

Write-Host ''
[MiniDawSym]::Lookup($Exe, [UInt64[]]$parsed)
