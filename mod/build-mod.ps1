<#
    Builds the official XAML-based Everything Search mod with MSVC x64 and
    names the DLL the way install-mod.ps1 expects:
      dist\windhawk\local@everything-explorer-search_<version>_<build>.dll

    The mod uses C++/WinRT and the generated headers in mod\generated, so the
    old clang/mingw build is no longer suitable.

    Visual Studio and the Windhawk engine are discovered at build time: no
    machine-specific version is hardcoded in build-mod.cmd.

    The C++/WinRT projection is not committed: regenerate-winrt.ps1 rebuilds it
    from the installed Windows SDK and NuGet WinMD files when it is missing.
#>
[CmdletBinding()]
param(
    [switch]$Trace,
    [switch]$RegenerateWinrt
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$source = Join-Path $PSScriptRoot 'everything-explorer-search.wh.cpp'
$buildScript = Join-Path $PSScriptRoot 'build-mod.cmd'
$distDir = Join-Path $root 'dist\windhawk'
$rawDll = Join-Path $distDir 'everything-explorer-search.dll'

$match = Select-String -Path $source -Pattern '^//\s*@version\s+(\d+(?:\.\d+)+)'
if (-not $match) { throw "Cannot read // @version from $source" }
$version = $match.Matches[0].Groups[1].Value

# --- Visual Studio ------------------------------------------------------------
$vcCandidates = @(
    'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat'
    'C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat'
    'C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat'
    'C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat'
)
$vcvars = $vcCandidates | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
if (-not $vcvars) {
    throw 'vcvars64.bat not found. Install Visual Studio 2022 with the C++ workload.'
}
$env:ESX_VCVARS64 = $vcvars

# --- Windhawk engine (newest installed version) -------------------------------
$engineRoot = 'C:\Program Files\Windhawk\Engine'
$libs = @()
if (Test-Path -LiteralPath $engineRoot) {
    foreach ($dir in Get-ChildItem -LiteralPath $engineRoot -Directory) {
        $candidate = Join-Path $dir.FullName '64\windhawk.lib'
        if (-not (Test-Path -LiteralPath $candidate)) { continue }
        try { $parsed = [version]($dir.Name -replace '^v', '') } catch { $parsed = [version]'0.0' }
        $libs += [pscustomobject]@{ Path = $candidate; Version = $parsed }
    }
}
$lib = $libs | Sort-Object Version -Descending | Select-Object -First 1
if (-not $lib) {
    throw "windhawk.lib not found under $engineRoot. Install Windhawk first."
}
$env:ESX_WINDHAWK_LIB = $lib.Path

# --- C++/WinRT projection -----------------------------------------------------
$generatedDir = Join-Path $PSScriptRoot 'generated'
$regenerateScript = Join-Path $PSScriptRoot 'regenerate-winrt.ps1'
if ($RegenerateWinrt -or -not (Test-Path -LiteralPath (Join-Path $generatedDir 'winrt\base.h'))) {
    if (-not (Test-Path -LiteralPath $regenerateScript)) {
        throw "C++/WinRT projection is missing and $regenerateScript was not found."
    }
    Write-Host 'Regenerating C++/WinRT projection...'
    & $regenerateScript -OutputDir $generatedDir -Force
    Write-Host 'Projection ready.'
}

& "$env:ComSpec" /d /c "`"$buildScript`""
if ($LASTEXITCODE -ne 0) { throw "Build failed with exit code $LASTEXITCODE" }

New-Item -ItemType Directory -Force -Path $distDir | Out-Null
$buildId = Get-Random -Minimum 100000 -Maximum 999999
$destination = Join-Path $distDir "local@everything-explorer-search_${version}_${buildId}.dll"
Copy-Item $rawDll $destination -Force
Remove-Item $rawDll -Force -ErrorAction SilentlyContinue

if ($Trace) {
    Write-Host 'Note: -Trace is accepted for compatibility; the log goes to %TEMP%\everything-explorer-search.log.' -ForegroundColor Yellow
}
Write-Host "Built $destination"
Write-Host "Using $vcvars and $($lib.Path)"
