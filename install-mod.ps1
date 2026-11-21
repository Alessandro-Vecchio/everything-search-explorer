#requires -Version 7.0
<#
.SYNOPSIS
    Builds (if needed) and installs the Everything Search Windhawk mod.

.DESCRIPTION
    Must run elevated (HKLM + C:\ProgramData). The script re-launches itself
    with a UAC prompt when needed.

    Steps:
      1. mod\build-mod.ps1 compiles the .wh.cpp with MSVC x64 (skipped with
         -SkipBuild or when -Dll is passed).
      2. The DLL is copied to C:\ProgramData\Windhawk\Engine\Mods\64 and the
         source to C:\ProgramData\Windhawk\ModsSource (so the Windhawk UI edits
         the same code that is installed).
      3. The mod is registered under
         HKLM\SOFTWARE\Windhawk\Engine\Mods\local@everything-explorer-search
         with its include/exclude taken from the source metadata, default
         settings, and Disabled=0.
      4. Windhawk is restarted so the new DLL is injected into Explorer.

    Explorer is never terminated: exploring a fresh window is enough for the
    new build to load. The shell component (EverythingSearchNse.dll) is a
    separate install: .\install-nse.ps1.

.PARAMETER Dll
    Already compiled mod DLL. Skips compilation.

.PARAMETER SkipBuild
    Reuse the newest dist\windhawk\local@everything-explorer-search_*.dll
    instead of recompiling.

.PARAMETER NoRestart
    Do not restart Windhawk (the mod stays installed but inactive until the
    engine reloads it).
#>
[CmdletBinding()]
param(
    [string]$Dll,
    [switch]$SkipBuild,
    [switch]$NoRestart,
    [string]$windhawkRoot = 'C:\Program Files\Windhawk',
    [string]$engineMods = 'C:\ProgramData\Windhawk\Engine\Mods\64',
    [string]$modsSource = 'C:\ProgramData\Windhawk\ModsSource'
)

$ErrorActionPreference = 'Stop'

$modId        = 'local@everything-explorer-search'
$repoRoot     = $PSScriptRoot
$source       = Join-Path $repoRoot 'mod\everything-explorer-search.wh.cpp'

# --- elevation --------------------------------------------------------------
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$isAdmin = (New-Object Security.Principal.WindowsPrincipal($identity)).IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator)

if (-not $isAdmin) {
    $arguments = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$PSCommandPath`"")
    if ($Dll) { $arguments += @('-Dll', "`"$Dll`"") }
    if ($SkipBuild) { $arguments += '-SkipBuild' }
    if ($NoRestart) { $arguments += '-NoRestart' }
    $arguments += @('-windhawkRoot', "`"$windhawkRoot`"", '-engineMods', "`"$engineMods`"",
                    '-modsSource', "`"$modsSource`"")
    Write-Host 'Requesting administrator rights...'
    $elevated = Start-Process -FilePath (Get-Process -Id $PID).Path -Verb RunAs `
        -ArgumentList $arguments -PassThru -Wait
    exit $elevated.ExitCode
}

if (-not (Test-Path $windhawkRoot)) {
    throw "Windhawk is not installed ($windhawkRoot)."
}
if (-not (Test-Path $source)) {
    throw "Mod source not found: $source"
}

# --- locate the mod DLL ------------------------------------------------------
$distDir = Join-Path $repoRoot 'dist\windhawk'
if (-not $Dll) {
    if (-not $SkipBuild) {
        & (Join-Path $repoRoot 'mod\build-mod.ps1')
    }
    $candidates = Get-ChildItem $distDir -Filter "$modId`_*.dll" -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending
    if (-not $candidates) {
        throw "No compiled mod found in $distDir. Run .\mod\build-mod.ps1 first."
    }
    $Dll = $candidates[0].FullName
}

if (-not (Test-Path $Dll)) { throw "Mod DLL not found: $Dll" }

# The compiled name carries the mod version: <id>_<version>_<build>.dll
$dllName = Split-Path -Leaf $Dll
if ($dllName -match "^$([regex]::Escape($modId))_(?<version>\d+(?:\.\d+)+)_\d+\.dll$") {
    $modVersion = $Matches.version
} else {
    throw "Unexpected mod DLL name: $dllName"
}

# Warn when the mod and the shell extension were built against different
# versions: they are distributed and installed separately.
$componentVersion = (Select-String -Path (Join-Path $repoRoot 'nse\version.h') `
    -Pattern 'ESX_VERSION_STRING\s+"([^"]+)"').Matches[0].Groups[1].Value
if ($componentVersion -ne $modVersion) {
    Write-Warning "Mod version $modVersion does not match nse\version.h ($componentVersion)."
}

# --- install the DLL ---------------------------------------------------------
New-Item -ItemType Directory -Path $engineMods -Force | Out-Null
$target = Join-Path $engineMods $dllName

# Older builds may still be mapped by Explorer. Removing them is best-effort:
# a locked file stays until the engine unloads it, which is harmless.
Get-ChildItem $engineMods -Filter "$modId`_*.dll" -ErrorAction SilentlyContinue |
    Where-Object { $_.Name -ne $dllName } |
    Remove-Item -Force -ErrorAction SilentlyContinue

$needsCopy = $true
if (Test-Path $target) {
    $needsCopy = (Get-FileHash $Dll).Hash -ne (Get-FileHash $target).Hash
}
if ($needsCopy) {
    Copy-Item $Dll $target -Force
}

# The WindowsAppRuntime bootstrapper is not part of the Windhawk engine. The
# XAML replacement needs it next to the mod DLL.
$bootstrapSource = Join-Path $PSScriptRoot 'mod\bin\Microsoft.WindowsAppRuntime.Bootstrap.dll'
if (Test-Path -LiteralPath $bootstrapSource) {
    Copy-Item -LiteralPath $bootstrapSource `
        -Destination (Join-Path $engineMods 'Microsoft.WindowsAppRuntime.Bootstrap.dll') -Force
}

# Keep the source in sync with the installed binary: the Windhawk UI shows and
# edits ModsSource, and it must not be an older build.
Copy-Item $source (Join-Path $modsSource "$modId.wh.cpp") -Force

# --- register the mod --------------------------------------------------------
$sourceText = Get-Content $source -Raw
$include = [regex]::Matches($sourceText, '(?m)^//\s*@include\s+(.+?)\s*$') |
    ForEach-Object { $_.Groups[1].Value.Trim() }
$exclude = [regex]::Matches($sourceText, '(?m)^//\s*@exclude\s+(.+?)\s*$') |
    ForEach-Object { $_.Groups[1].Value.Trim() }
if (-not $include) { throw "No // @include line found in $source" }

$key = "HKLM:\SOFTWARE\Windhawk\Engine\Mods\$modId"
New-Item -Path $key -Force | Out-Null
$now = [int][double]::Parse((Get-Date -UFormat %s), [Globalization.CultureInfo]::InvariantCulture)

Set-ItemProperty -Path $key -Name 'LibraryFileName' -Value $dllName
Set-ItemProperty -Path $key -Name 'Version' -Value $modVersion
Set-ItemProperty -Path $key -Name 'Include' -Value ($include -join '|')
Set-ItemProperty -Path $key -Name 'Exclude' -Value ($exclude -join '|')
Set-ItemProperty -Path $key -Name 'Architecture' -Value 'x86-64'
Set-ItemProperty -Path $key -Name 'SettingsChangeTime' -Value $now -Type DWord
Set-ItemProperty -Path $key -Name 'Disabled' -Value 0 -Type DWord

# Default settings. A hand-registered mod has no Settings key at all and the
# engine reads 0 for missing values, so they must be written once: without
# maxResults the search would return nothing.
$settingsKey = Join-Path $key 'Settings'
New-Item -Path $settingsKey -Force | Out-Null
$currentMax = (Get-ItemProperty -Path $settingsKey -Name 'maxResults' -ErrorAction SilentlyContinue).maxResults
if ($null -eq $currentMax -or $currentMax -eq 500 -or $currentMax -eq 1000) {
    # 0 = every match. 500 and 1000 were earlier defaults, migrate them back.
    Set-ItemProperty -Path $settingsKey -Name 'maxResults' -Value 0 -Type DWord
}
if (-not (Get-ItemProperty -Path $settingsKey -Name 'scopeToFolder' -ErrorAction SilentlyContinue)) {
    Set-ItemProperty -Path $settingsKey -Name 'scopeToFolder' -Value 1 -Type DWord
}

# --- reload ------------------------------------------------------------------
if (-not $NoRestart) {
    Write-Host 'Restarting Windhawk...'
    & (Join-Path $windhawkRoot 'windhawk.exe') -restart -tray-only
    Start-Sleep -Seconds 4
}

$guid = '{7B39C2A2-3E1F-4E4C-9E58-2D9A7B1C4F60}'
$componentKey = "HKCU:\Software\Classes\CLSID\$guid\InprocServer32"

Write-Host ''
Write-Host 'Windhawk mod installed and enabled:' -ForegroundColor Green
Write-Host "  $key"
Write-Host "  $target"
Write-Host ''
Write-Host 'Open any Explorer window and press Ctrl+F (Ctrl+Shift+F for setup).'
if (-not (Test-Path $componentKey)) {
    Write-Host 'The shell component is not registered yet:' -ForegroundColor Yellow
    Write-Host 'download the DLL from the releases page and install it from the panel,'
    Write-Host 'or run .\install-nse.ps1 (development install from dist\bin).'
}
