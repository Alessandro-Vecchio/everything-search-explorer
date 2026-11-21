<#
    Builds the official XAML-based Everything Search mod with MSVC x64 and
    names the DLL the way install-mod.ps1 expects:
      dist\windhawk\local@everything-explorer-search_<version>_<build>.dll

    The mod uses C++/WinRT and the generated headers in mod\generated, so the
    old clang/mingw build is no longer suitable.
#>
[CmdletBinding()]
param(
    [switch]$Trace
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

& "$env:ComSpec" /d /c "`"$buildScript`""
if ($LASTEXITCODE -ne 0) { throw "Build failed with exit code $LASTEXITCODE" }

New-Item -ItemType Directory -Force -Path $distDir | Out-Null
$buildId = Get-Random -Minimum 100000 -Maximum 999999
$destination = Join-Path $distDir "local@everything-explorer-search_${version}_${buildId}.dll"
Copy-Item $rawDll $destination -Force
Remove-Item $rawDll -Force -ErrorAction SilentlyContinue

if ($Trace) {
    Write-Host 'Note: -Trace is accepted for compatibility; release logging goes to %TEMP%\esx-xaml-spike.log.' -ForegroundColor Yellow
}
Write-Host "Built $destination"
