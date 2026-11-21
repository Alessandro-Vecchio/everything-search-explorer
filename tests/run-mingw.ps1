<#
    Builds everything with MinGW (no MSVC required) and runs the out-of-process
    probe against the namespace extension. Intended for CI/dev machines without
    Visual Studio; the official build is build.ps1.
#>
[CmdletBinding()]
param(
    [string]$Query = "*.ps1",
    [string]$Folder = (Get-Location).Path,
    [int]$Max = 50,
    [switch]$Global,
    [switch]$Ipc,
    [switch]$Trace,
    [string]$Gcc = "$env:TEMP\mingw\mingw64\bin\g++.exe"
)

$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$out = Join-Path $root 'build_check'
New-Item -ItemType Directory -Force -Path $out | Out-Null

$defs = @('-DUNICODE', '-D_UNICODE', '-DWIN32_LEAN_AND_MEAN', '-DNOMINMAX',
          '-D_WIN32_WINNT=0x0A00', '-DNTDDI_VERSION=0x0A000000')
if ($Trace) {
    # Every shell entry point appends to %TEMP%\esx-nse.log (see nse/trace.h).
    $defs += '-DESX_TRACE'
    Write-Host 'Trace build: %TEMP%\esx-nse.log will be written by Explorer' -ForegroundColor Yellow
}
$common = @('-std=c++17', '-O1', '-static') + $defs

Write-Host 'Building version resource...'
& (Join-Path (Split-Path $Gcc -Parent) 'windres.exe') '-I' "$root/nse" '-O' 'coff' `
    "$root/nse/version.rc" "$out/version.res"
if ($LASTEXITCODE -ne 0) { throw "windres failed ($LASTEXITCODE)" }

Write-Host 'Building EverythingSearchNse.dll...'
& $Gcc @common '-shared' '-I' "$root/shared" `
    "$root/nse/dllmain.cpp" "$root/nse/pidl.cpp" "$root/nse/bridge.cpp" "$root/nse/folder.cpp" `
    "$out/version.res" "$root/nse/nse.def" '-o' "$out/EverythingSearchNse.dll" `
    '-Wl,--no-insert-timestamp' `
    '-lshell32' '-lshlwapi' '-lole32' '-loleaut32' '-luuid' '-lpropsys' '-lgdi32'
if ($LASTEXITCODE -ne 0) { throw "NSE build failed ($LASTEXITCODE)" }

Write-Host 'Building probe...'
& $Gcc @common '-municode' '-I' "$root/nse" `
    "$root/tests/nse_probe.cpp" '-o' "$out/nse_probe.exe" `
    '-lshell32' '-lshlwapi' '-lole32' '-loleaut32' '-luuid'
if ($LASTEXITCODE -ne 0) { throw "probe build failed ($LASTEXITCODE)" }

Write-Host 'Building IPC probe...'
& $Gcc @common '-municode' '-I' "$root/shared" `
    "$root/tests/ipc_probe.cpp" '-o' "$out/ipc_probe.exe"
if ($LASTEXITCODE -ne 0) { throw "ipc probe build failed ($LASTEXITCODE)" }

Write-Host 'Building live probes...'
& $Gcc @common '-municode' '-I' "$root/nse" "$root/tests/context_menu_probe.cpp" `
    '-o' "$out/context_menu_probe.exe" '-lshell32' '-lshlwapi' '-lole32' '-luuid'
if ($LASTEXITCODE -ne 0) { throw "context menu probe build failed ($LASTEXITCODE)" }
& $Gcc @common '-municode' "$root/tests/navigate_probe.cpp" `
    '-o' "$out/navigate_probe.exe" '-lshell32' '-lshlwapi' '-lole32' '-loleaut32' '-luuid'
if ($LASTEXITCODE -ne 0) { throw "navigate probe build failed ($LASTEXITCODE)" }
& $Gcc @common '-municode' "$root/tests/view_probe.cpp" `
    '-o' "$out/view_probe.exe" '-lshell32' '-lshlwapi' '-lole32' '-loleaut32' '-luuid'
if ($LASTEXITCODE -ne 0) { throw "view probe build failed ($LASTEXITCODE)" }
& $Gcc @common '-municode' "$root/tests/rightclick_probe.cpp" `
    '-o' "$out/rightclick_probe.exe" '-lshell32' '-lshlwapi' '-lole32' '-loleaut32' '-luuid'
if ($LASTEXITCODE -ne 0) { throw "right click probe build failed ($LASTEXITCODE)" }


$dataDir = Join-Path $env:LOCALAPPDATA 'EverythingExplorerSearch'
New-Item -ItemType Directory -Force -Path $dataDir | Out-Null
# bit 0 = scope to folder, bit 1 = include folders.
$flags = if ($Global) { 2 } else { 3 }
$request = @(
    'seq=1',
    "max=$Max",
    "flags=$flags",
    "folder=$Folder",
    "query=$Query"
) -join "`n"
[System.IO.File]::WriteAllText((Join-Path $dataDir 'request.txt'), $request,
                               [System.Text.UTF8Encoding]::new($false))

if ($Ipc) {
    Write-Host 'Running IPC probe (raw reply dump)...' -ForegroundColor Cyan
    $search = if ($Global) { $Query } else { "path:`"$Folder`" $Query" }
    & "$out\ipc_probe.exe" $search
    return
}

Write-Host 'Running probe...' -ForegroundColor Cyan
& "$out\nse_probe.exe"
if ($LASTEXITCODE -ne 0) { throw "NSE probe failed ($LASTEXITCODE)" }
