#requires -Version 7.0
<#
.SYNOPSIS
    Installs the per-window C2 probe as a separate Windhawk mod.
    -Enable  : disables the stable spike and enables C2 multi (test mode).
    -Restore : disables C2 multi and re-enables the stable spike.
#>
[CmdletBinding(DefaultParameterSetName = 'Install')]
param(
    [Parameter(ParameterSetName = 'Install')][string]$Dll,
    [Parameter(ParameterSetName = 'Install')][switch]$SkipBuild,
    [Parameter(ParameterSetName = 'Install')][switch]$Enable,
    [Parameter(ParameterSetName = 'Restore')][switch]$Restore,
    [switch]$NoRestart
)

$ErrorActionPreference = 'Stop'
$probeId = 'local@esx-c2-multi'
$spikeId = 'local@esx-xaml-spike'
$modVersion = '0.1.0'
$windhawkRoot = 'C:\Program Files\Windhawk'
$engineMods = 'C:\ProgramData\Windhawk\Engine\Mods\64'
$repoRoot = Split-Path -Parent $PSScriptRoot
$generator = Join-Path $PSScriptRoot 'make-c2-multi-probe.ps1'
$buildScript = Join-Path $PSScriptRoot 'build-c2-multi-probe.cmd'
$probeSource = Join-Path $PSScriptRoot 'c2-multi-probe.cpp'
$distDll = Join-Path $repoRoot 'dist\spike\esx-c2-multi-probe.dll'

$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]::new($identity)
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    $arguments = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$PSCommandPath`"")
    if ($Dll) { $arguments += @('-Dll', "`"$Dll`"") }
    if ($SkipBuild) { $arguments += '-SkipBuild' }
    if ($Enable) { $arguments += '-Enable' }
    if ($Restore) { $arguments += '-Restore' }
    if ($NoRestart) { $arguments += '-NoRestart' }
    Write-Host 'Requesting administrator rights...'
    $elevated = Start-Process -FilePath (Get-Process -Id $PID).Path -Verb RunAs `
        -ArgumentList $arguments -PassThru -Wait
    exit $elevated.ExitCode
}

if (-not (Test-Path $windhawkRoot)) { throw "Windhawk is not installed ($windhawkRoot)." }
$probeKey = "HKLM:\SOFTWARE\Windhawk\Engine\Mods\$probeId"
$spikeKey = "HKLM:\SOFTWARE\Windhawk\Engine\Mods\$spikeId"

if ($Restore) {
    New-Item -Path $probeKey -Force | Out-Null
    Set-ItemProperty -Path $probeKey -Name 'Disabled' -Value 1 -Type DWord
    if (Test-Path $spikeKey) {
        Set-ItemProperty -Path $spikeKey -Name 'Disabled' -Value 0 -Type DWord
        Write-Host "Re-enabled $spikeId (DLL: $((Get-ItemProperty $spikeKey).LibraryFileName))"
    }
    Write-Host "Disabled $probeId"
} else {
    if (-not $SkipBuild) {
        if (-not (Test-Path $probeSource)) {
            & pwsh.exe -NoProfile -ExecutionPolicy Bypass -File $generator
        }
        & $env:ComSpec /d /c "`"$buildScript`""
        if ($LASTEXITCODE -ne 0) { throw 'C2 multi probe build failed.' }
    }
    if ($Dll) { $Dll = [IO.Path]::GetFullPath($Dll) } else { $Dll = $distDll }
    if (-not (Test-Path -LiteralPath $Dll -PathType Leaf)) { throw "Probe DLL not found: $Dll" }

    $buildId = Get-Date -Format 'yyyyMMddHHmmssfff'
    $dllName = "$probeId`_$modVersion`_$buildId.dll"
    New-Item -ItemType Directory -Path $engineMods -Force | Out-Null
    Copy-Item -LiteralPath $Dll -Destination (Join-Path $engineMods $dllName)
    $bootstrapSource = Join-Path $repoRoot 'xaml-spike\bin\Microsoft.WindowsAppRuntime.Bootstrap.dll'
    if (Test-Path -LiteralPath $bootstrapSource) {
        Copy-Item -LiteralPath $bootstrapSource `
            -Destination (Join-Path $engineMods 'Microsoft.WindowsAppRuntime.Bootstrap.dll') -Force
    }

    New-Item -Path $probeKey -Force | Out-Null
    $now = [int][DateTimeOffset]::UtcNow.ToUnixTimeSeconds()
    Set-ItemProperty -Path $probeKey -Name 'LibraryFileName' -Value $dllName
    Set-ItemProperty -Path $probeKey -Name 'Version' -Value $modVersion
    Set-ItemProperty -Path $probeKey -Name 'Include' -Value 'explorer.exe'
    Set-ItemProperty -Path $probeKey -Name 'Exclude' -Value ''
    Set-ItemProperty -Path $probeKey -Name 'Architecture' -Value 'x86-64'
    Set-ItemProperty -Path $probeKey -Name 'SettingsChangeTime' -Value $now -Type DWord
    Set-ItemProperty -Path $probeKey -Name 'Disabled' -Value $(if ($Enable) { 0 } else { 1 }) -Type DWord

    if ($Enable -and (Test-Path $spikeKey)) {
        Set-ItemProperty -Path $spikeKey -Name 'Disabled' -Value 1 -Type DWord
        Write-Host "Temporarily disabled $spikeId (DLL untouched)."
    }
    Write-Host "C2 multi probe installed: $dllName"
}

if (-not $NoRestart) {
    & (Join-Path $windhawkRoot 'windhawk.exe') -restart -tray-only
    Start-Sleep -Seconds 4
}
