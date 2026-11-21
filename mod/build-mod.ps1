#requires -Version 7.0
<#
.SYNOPSIS
    Compiles mod\everything-explorer-search.wh.cpp with the toolchain bundled
    with Windhawk.

.DESCRIPTION
    Reproduces the command line Windhawk's mod editor uses (clang, mingw-w64
    target), so the resulting DLL can be installed and registered by hand.

    Version and extra compiler options are read from the mod source
    ("// @version" and "// @compilerOptions"), so the build cannot drift from
    what the Windhawk UI shows.

    Windhawk itself does not need to be running, only installed.

.PARAMETER Output
    Destination DLL. Defaults to
    dist\windhawk\local@everything-explorer-search_<version>_<build>.dll

.PARAMETER WindhawkRoot
    Windhawk installation directory (contains Compiler\ and Engine\).
#>
[CmdletBinding()]
param(
    [string]$Output,
    [string]$WindhawkRoot = 'C:\Program Files\Windhawk',
    [switch]$Trace
)

$ErrorActionPreference = 'Stop'

$repoRoot    = Split-Path -Parent $PSScriptRoot
$source      = Join-Path $PSScriptRoot 'everything-explorer-search.wh.cpp'
$compilerDir = Join-Path $WindhawkRoot 'Compiler'
$compilerExe = Join-Path $compilerDir 'bin\clang++.exe'

if (-not (Test-Path $compilerExe)) {
    throw "Windhawk compiler not found at $compilerExe. Install or update Windhawk first."
}
if (-not (Test-Path $source)) {
    throw "Mod source not found: $source"
}

# The engine actually in use is the one windhawk.ini points at; fall back to
# the newest versioned directory for portable or older layouts.
$engineDir = $null
$ini = Join-Path $WindhawkRoot 'windhawk.ini'
if (Test-Path $ini) {
    $enginePath = (Select-String -Path $ini -Pattern '^\s*EnginePath\s*=\s*(.+?)\s*$').Matches |
        ForEach-Object { $_.Groups[1].Value } |
        Select-Object -First 1
    if ($enginePath) {
        $candidate = Join-Path $WindhawkRoot $enginePath
        if (Test-Path $candidate) { $engineDir = Get-Item $candidate }
    }
}
if (-not $engineDir) {
    $engineDir = Get-ChildItem (Join-Path $WindhawkRoot 'Engine') -Directory -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -match '^[\d.]+$' } |
        Sort-Object { [version]$_.Name } -Descending |
        Select-Object -First 1
}
if (-not $engineDir) {
    throw "No Windhawk engine directory found under $WindhawkRoot\Engine"
}

$windhawkLib = Join-Path $engineDir.FullName '64\windhawk.lib'
if (-not (Test-Path $windhawkLib)) {
    throw "windhawk.lib not found at $windhawkLib"
}

$modId = 'local@everything-explorer-search'

$sourceText = Get-Content $source -Raw
if ($sourceText -match '(?m)^//\s*@version\s+(\S+)\s*$') {
    $modVersion = $Matches[1]
} else {
    throw "No // @version line found in $source"
}
$modCompilerOptions = @()
if ($sourceText -match '(?m)^//\s*@compilerOptions\s+(.+?)\s*$') {
    $modCompilerOptions = $Matches[1] -split '\s+' | Where-Object { $_ }
}
$buildId = (Get-Random -Minimum 100000 -Maximum 999999)

if (-not $Output) {
    $Output = Join-Path $repoRoot "dist\windhawk\${modId}_${modVersion}_${buildId}.dll"
}
$outputDir = Split-Path -Parent $Output
if (-not (Test-Path $outputDir)) {
    New-Item -ItemType Directory -Path $outputDir -Force | Out-Null
}

$arguments = @(
    '-std=c++23'
    '-O2'
    '-shared'
    '-DUNICODE'
    '-D_UNICODE'
    '-DWINVER=0x0A00'
    '-D_WIN32_IE=0x0A00'
    '-DNTDDI_VERSION=0x0A000008'
    '-D__USE_MINGW_ANSI_STDIO=0'
    '-DWH_MOD'
    "-DWH_MOD_ID=L`"$modId`""
    "-DWH_MOD_VERSION=L`"$modVersion`""
    # Inputs before -x are linked as libraries, inputs after it are compiled
    # as C++ (the same order Windhawk's own build task uses).
    $windhawkLib
    '-x'
    'c++'
    $source
    '-include'
    'windhawk_api.h'
    '-I'
    'include'
    '-target'
    'x86_64-w64-mingw32'
    '-Wl,--export-all-symbols'
    '-o'
    $Output
) + $modCompilerOptions

if ($Trace) {
    # Every hotkey decision appends a line to %TEMP%\esx-mod.log (see ESX_MOD_TRACE
    # in the mod source). Explorer writes it, not this shell.
    $arguments += '-DESX_MOD_TRACE'
    Write-Host 'Trace build: Explorer will append to %TEMP%\esx-mod.log' -ForegroundColor Yellow
}

$psi = [System.Diagnostics.ProcessStartInfo]::new($compilerExe)
$psi.UseShellExecute = $false
$psi.WorkingDirectory = $compilerDir
$psi.RedirectStandardError = $true
$psi.RedirectStandardOutput = $true
foreach ($argument in $arguments) {
    $psi.ArgumentList.Add($argument)
}

$process = [System.Diagnostics.Process]::Start($psi)
$stderrTask = $process.StandardError.ReadToEndAsync()
$stdout = $process.StandardOutput.ReadToEnd()
$stderr = $stderrTask.GetAwaiter().GetResult()
$process.WaitForExit()

if ($stdout) { Write-Host $stdout.TrimEnd() }
if ($stderr) { Write-Host $stderr.TrimEnd() }
if ($process.ExitCode -ne 0) {
    throw "clang++ failed with exit code $($process.ExitCode)"
}

$dll = Get-Item $Output
Write-Host "Mod compiled: $($dll.FullName) ($([math]::Round($dll.Length / 1KB)) KB)" -ForegroundColor Green
