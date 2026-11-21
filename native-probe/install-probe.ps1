#requires -Version 7.0
<#
.SYNOPSIS
    Builds and installs the native probe Windhawk mod.

.DESCRIPTION
    The probe is a bisect harness. It is always installed enabled, but the
    stage file decides what it actually does:

        %TEMP%\esx-native-probe-stage.txt   0..3

    The XAML spike is disabled at the same time, so a new Explorer process
    never loads the crashing build.
#>
[CmdletBinding()]
param(
    [switch]$SkipBuild,
    [switch]$NoRestart
)

$ErrorActionPreference = 'Stop'

$modId = 'local@esx-native-probe'
$modVersion = '0.1.0'
$windhawkRoot = 'C:\Program Files\Windhawk'
$engineMods = 'C:\ProgramData\Windhawk\Engine\Mods\64'
$repoRoot = Split-Path -Parent $PSScriptRoot
$buildScript = Join-Path $PSScriptRoot 'build-probe.cmd'
$distDll = Join-Path $repoRoot 'dist\probe\esx-native-probe.dll'

$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]::new($identity)
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    $arguments = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$PSCommandPath`"")
    if ($SkipBuild) { $arguments += '-SkipBuild' }
    if ($NoRestart) { $arguments += '-NoRestart' }
    Write-Host 'Requesting administrator rights...'
    $elevated = Start-Process -FilePath (Get-Process -Id $PID).Path -Verb RunAs `
        -ArgumentList $arguments -PassThru -Wait
    exit $elevated.ExitCode
}

if (-not (Test-Path $windhawkRoot)) { throw "Windhawk is not installed ($windhawkRoot)." }

if (-not $SkipBuild) {
    & $env:ComSpec /d /c "`"$buildScript`""
    if ($LASTEXITCODE -ne 0) { throw "Probe build failed with exit code $LASTEXITCODE." }
}
if (-not (Test-Path -LiteralPath $distDll -PathType Leaf)) {
    throw "Probe DLL not found: $distDll"
}

$buildId = Get-Date -Format 'yyyyMMddHHmmssfff'
$dllName = "$modId`_$modVersion`_$buildId.dll"
New-Item -ItemType Directory -Path $engineMods -Force | Out-Null
$target = Join-Path $engineMods $dllName
Copy-Item -LiteralPath $distDll -Destination $target

# The XAML spike crashed Explorer on startup. It must never be loaded again.
$spikeKey = 'HKLM:\SOFTWARE\Windhawk\Engine\Mods\local@esx-xaml-spike'
if (Test-Path $spikeKey) {
    Set-ItemProperty -Path $spikeKey -Name 'Disabled' -Value 1 -Type DWord
    Write-Host 'Disabled the XAML spike mod.'
}

$key = "HKLM:\SOFTWARE\Windhawk\Engine\Mods\$modId"
New-Item -Path $key -Force | Out-Null
$now = [int][DateTimeOffset]::UtcNow.ToUnixTimeSeconds()

Set-ItemProperty -Path $key -Name 'LibraryFileName' -Value $dllName
Set-ItemProperty -Path $key -Name 'Version' -Value $modVersion
Set-ItemProperty -Path $key -Name 'Include' -Value 'explorer.exe'
Set-ItemProperty -Path $key -Name 'Exclude' -Value ''
Set-ItemProperty -Path $key -Name 'Architecture' -Value 'x86-64'
Set-ItemProperty -Path $key -Name 'SettingsChangeTime' -Value $now -Type DWord
Set-ItemProperty -Path $key -Name 'Disabled' -Value 0 -Type DWord

# Old probe builds stay mapped by running Explorer processes; removing them is
# best effort and a locked file is harmless.
Get-ChildItem $engineMods -Filter "$modId`_*.dll" -ErrorAction SilentlyContinue |
    Where-Object { $_.Name -ne $dllName } |
    Remove-Item -Force -ErrorAction SilentlyContinue

if (-not $NoRestart) {
    Write-Host 'Restarting Windhawk...'
    & (Join-Path $windhawkRoot 'windhawk.exe') -restart -tray-only
    Start-Sleep -Seconds 4
}

Write-Host ''
Write-Host 'Native probe installed and enabled:' -ForegroundColor Green
Write-Host "  $key"
Write-Host "  $target"
Write-Host "  stage file: $env:TEMP\esx-native-probe-stage.txt"
Write-Host "  log:        $env:TEMP\esx-native-probe.log"
