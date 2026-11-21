#requires -Version 7.0
<#
.SYNOPSIS
    Regenerates the C++/WinRT projection used by the Windhawk mod.

.DESCRIPTION
    The mod uses WinUI 3 (Microsoft.UI.Xaml) types, so C++/WinRT needs headers
    generated from Windows metadata. Committing the whole projection costs
    ~110 MB; this script rebuilds only the namespaces the mod actually needs
    (~30 MB) from the WinMD files already installed on the machine.

    It discovers:
      - the newest cppwinrt.exe in the installed Windows SDK;
      - the newest versions of the required NuGet packages in %USERPROFILE%\.nuget.

    Required packages in the NuGet cache (any recent version):
      Microsoft.WindowsAppSDK.WinUI, Microsoft.WindowsAppSDK.Foundation,
      Microsoft.WindowsAppSDK.InteractiveExperiences, Microsoft.Web.WebView2,
      Microsoft.Windows.SDK.NET.Ref

    If they are missing, install the Microsoft.WindowsAppSDK 1.8+ and
    Microsoft.Web.WebView2 NuGet packages in any temporary C# project, then
    re-run this script.

.PARAMETER OutputDir
    Destination of the projection. Defaults to mod\generated.

.PARAMETER Force
    Regenerate even if the destination already contains base.h.
#>
[CmdletBinding()]
param(
    [string]$OutputDir,
    [switch]$Force
)

$ErrorActionPreference = 'Stop'

$scriptRoot = $PSScriptRoot
if (-not $OutputDir) { $OutputDir = Join-Path $scriptRoot 'generated' }

if (-not $Force -and (Test-Path -LiteralPath (Join-Path $OutputDir 'winrt\base.h'))) {
    Write-Host "Projection already present in $OutputDir (use -Force to regenerate)."
    return
}

# --- helpers -----------------------------------------------------------------
function Get-NewestVersionDir {
    param([string]$Package, [string]$SubPath = '')

    $root = Join-Path $env:USERPROFILE ".nuget\packages\$Package"
    if (-not (Test-Path -LiteralPath $root)) { return $null }
    $candidates = @()
    foreach ($dir in Get-ChildItem -LiteralPath $root -Directory) {
        $target = if ($SubPath) { Join-Path $dir.FullName $SubPath } else { $dir.FullName }
        if (-not (Test-Path -LiteralPath $target)) { continue }
        try { $parsed = [version]($dir.Name -replace '[-+].*$', '') } catch { $parsed = [version]'0.0' }
        $candidates += [pscustomobject]@{ Path = $target; Version = $parsed }
    }
    return ($candidates | Sort-Object Version -Descending | Select-Object -First 1).Path
}

function Get-NewestWinmdFolder {
    param([string]$Package, [string]$SubPath)
    $base = Get-NewestVersionDir -Package $Package -SubPath $SubPath
    if (-not $base) { return @() }

    $here = @(Get-ChildItem -LiteralPath $base -Filter '*.winmd' -File -ErrorAction SilentlyContinue |
        ForEach-Object { $_.FullName })
    if ($here.Count -gt 0) { return $here }

    # Some packages (e.g. InteractiveExperiences) store WinMDs under additional
    # version folders. Pick the newest one that actually contains WinMD files.
    $children = @(Get-ChildItem -LiteralPath $base -Directory -ErrorAction SilentlyContinue |
        Sort-Object { try { [version]$_.Name } catch { [version]'0.0' } } -Descending)
    foreach ($child in $children) {
        $found = @(Get-ChildItem -LiteralPath $child.FullName -Filter '*.winmd' -File -ErrorAction SilentlyContinue |
            ForEach-Object { $_.FullName })
        if ($found.Count -gt 0) { return $found }
    }
    return @()
}

# --- 1. cppwinrt.exe from the newest Windows SDK -------------------------------
$sdkBin = 'C:\Program Files (x86)\Windows Kits\10\bin'
$cppwinrt = $null
if (Test-Path -LiteralPath $sdkBin) {
    $cppwinrt = @(Get-ChildItem -LiteralPath $sdkBin -Directory |
        Where-Object { $_.Name -match '^\d+\.' } |
        Sort-Object { try { [version]$_.Name } catch { [version]'0.0' } } -Descending |
        ForEach-Object { Join-Path $_.FullName 'x64\cppwinrt.exe' } |
        Where-Object { Test-Path -LiteralPath $_ })[0]
}
if (-not $cppwinrt) {
    throw "cppwinrt.exe not found under $sdkBin. Install the Windows SDK (C++/WinRT)."
}

# --- 2. WinMD inputs ----------------------------------------------------------
$winui      = Get-NewestWinmdFolder -Package 'microsoft.windowsappsdk.winui' -SubPath 'metadata'
$msui       = Get-NewestWinmdFolder -Package 'microsoft.windowsappsdk.interactiveexperiences' -SubPath 'metadata'
$foundation = Get-NewestWinmdFolder -Package 'microsoft.windowsappsdk.foundation' -SubPath 'metadata'
$webview2   = Get-NewestWinmdFolder -Package 'microsoft.web.webview2' -SubPath 'lib'
$sdkNetRef  = Get-NewestVersionDir -Package 'microsoft.windows.sdk.net.ref' -SubPath 'winmd'

$required = @{
    'Microsoft.UI.Xaml.winmd'                = ($winui | Where-Object { $_ -match 'Microsoft\.UI\.Xaml\.winmd$' } | Select-Object -First 1)
    'Microsoft.UI.Text.winmd'                = ($winui | Where-Object { $_ -match 'Microsoft\.UI\.Text\.winmd$' } | Select-Object -First 1)
    'Microsoft.UI.winmd'                     = ($msui | Where-Object { $_ -match '\\Microsoft\.UI\.winmd$' } | Select-Object -First 1)
    'Microsoft.Web.WebView2.Core.winmd'      = ($webview2 | Where-Object { $_ -match 'Microsoft\.Web\.WebView2\.Core\.winmd$' } | Select-Object -First 1)
    'Windows.Foundation.FoundationContract.winmd' = if ($sdkNetRef) { Join-Path $sdkNetRef 'Windows.Foundation.FoundationContract.winmd' } else { $null }
}
$missing = @($required.GetEnumerator() | Where-Object { -not $_.Value -or -not (Test-Path -LiteralPath $_.Value) } | ForEach-Object { $_.Key })
if ($missing.Count -gt 0 -or $foundation.Count -eq 0) {
    throw "Missing WinMD inputs: $($missing -join ', ')$(if ($foundation.Count -eq 0) { ', Microsoft.WindowsAppSDK.Foundation metadata' }). See the script header for required NuGet packages."
}

# Inputs: only these produce headers. Everything else is a reference so that
# cppwinrt can resolve cross-namespace types without generating them.
$inputs = @($required.Values) + @($foundation)

$references = @($inputs) + @($msui) +
    @(Get-NewestWinmdFolder -Package 'microsoft.windowsappsdk.ai' -SubPath 'metadata') +
    @(Get-NewestWinmdFolder -Package 'microsoft.windowsappsdk.ml' -SubPath 'metadata') +
    @(Get-NewestWinmdFolder -Package 'microsoft.windowsappsdk.widgets' -SubPath 'metadata')
$sdkWinmd = 'C:\Program Files (x86)\Windows Kits\10\UnionMetadata\Windows.winmd'
if (Test-Path -LiteralPath $sdkWinmd) { $references += $sdkWinmd }
$sdkVersioned = Get-ChildItem 'C:\Program Files (x86)\Windows Kits\10\UnionMetadata' -Directory -ErrorAction SilentlyContinue |
    Where-Object { $_.Name -match '^\d+\.' } |
    Sort-Object { try { [version]$_.Name } catch { [version]'0.0' } } -Descending |
    ForEach-Object { Join-Path $_.FullName 'Windows.winmd' } |
    Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
if ($sdkVersioned) { $references += $sdkVersioned }
$references = @($references | Where-Object { $_ -and (Test-Path -LiteralPath $_) } | Select-Object -Unique)

# --- 3. generate --------------------------------------------------------------
New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null
$args = @('-base', '-output', $OutputDir)
foreach ($i in $inputs) { $args += @('-input', $i) }
foreach ($r in $references) { $args += @('-reference', $r) }

Write-Host "cppwinrt: $cppwinrt"
Write-Host "inputs:   $($inputs.Count) WinMD files"
Write-Host "refs:     $($references.Count) WinMD files"
& $cppwinrt @args
if ($LASTEXITCODE -ne 0) { throw "cppwinrt failed with exit code $LASTEXITCODE" }

# --- 4. sanity check ----------------------------------------------------------
$needed = @(
    'base.h', 'Microsoft.UI.h', 'Microsoft.UI.Content.h', 'Microsoft.UI.Dispatching.h',
    'Microsoft.UI.Xaml.h', 'Microsoft.UI.Xaml.Controls.h',
    'Microsoft.UI.Xaml.Controls.Primitives.h', 'Microsoft.UI.Xaml.Hosting.h',
    'Microsoft.UI.Xaml.Input.h', 'Microsoft.UI.Xaml.Media.h',
    'Windows.Foundation.h', 'Windows.Foundation.Collections.h'
)
$missingHeaders = @($needed | Where-Object { -not (Test-Path -LiteralPath (Join-Path $OutputDir "winrt\$_")) })
if ($missingHeaders.Count -gt 0) {
    throw "Generation incomplete. Missing: $($missingHeaders -join ', ')"
}
$count = (Get-ChildItem -LiteralPath $OutputDir -Recurse -File | Measure-Object).Count
Write-Host "Generated $count files in $OutputDir"
