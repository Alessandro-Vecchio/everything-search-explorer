#requires -Version 7.0
<#
.SYNOPSIS
    Disables and removes the Everything Search Windhawk mod (asks for
    administrator rights).

.DESCRIPTION
    Removes the registry entry, the compiled DLLs, the copy of the source in
    ModsSource, and restarts Windhawk. The shell component is left untouched:
    use .\uninstall.ps1 for that one.

    The mod DLL is mapped by explorer.exe, so Explorer is restarted (unless
    -NoProcessRestart) to release it.

.PARAMETER NoProcessRestart
    Leave Explorer running. The mod DLL stays on disk until the engine unloads
    it and Explorer restarts.
#>
[CmdletBinding()]
param(
    [switch]$NoProcessRestart
)

$ErrorActionPreference = 'Stop'

$modId        = 'local@everything-explorer-search'
$windhawkRoot = 'C:\Program Files\Windhawk'
$engineMods   = 'C:\ProgramData\Windhawk\Engine\Mods\64'

$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$isAdmin = (New-Object Security.Principal.WindowsPrincipal($identity)).IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator)

if (-not $isAdmin) {
    $arguments = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$PSCommandPath`"")
    if ($NoProcessRestart) { $arguments += '-NoProcessRestart' }
    Write-Host 'Requesting administrator rights...'
    $elevated = Start-Process -FilePath (Get-Process -Id $PID).Path -Verb RunAs `
        -ArgumentList $arguments -PassThru -Wait
    exit $elevated.ExitCode
}

$key = "HKLM:\SOFTWARE\Windhawk\Engine\Mods\$modId"

# Disable first: if removing the DLL fails because it is still mapped, the
# engine must not try to load it again.
if (Test-Path $key) {
    Set-ItemProperty -Path $key -Name 'Disabled' -Value 1 -Type DWord
}

if (-not $NoProcessRestart) {
    Get-Process explorer -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
    Start-Sleep -Seconds 2
}

if (Test-Path $key) {
    Remove-Item $key -Recurse -Force
    Write-Host "Removed $key"
}

Get-ChildItem $engineMods -Filter "$modId`_*.dll" -ErrorAction SilentlyContinue |
    Remove-Item -Force -ErrorAction SilentlyContinue
$bootstrap = Join-Path $engineMods 'Microsoft.WindowsAppRuntime.Bootstrap.dll'
if (Test-Path $bootstrap) { Remove-Item $bootstrap -Force -ErrorAction SilentlyContinue }

$source = "C:\ProgramData\Windhawk\ModsSource\$modId.wh.cpp"
if (Test-Path $source) { Remove-Item $source -Force }

if (Test-Path (Join-Path $windhawkRoot 'windhawk.exe')) {
    & (Join-Path $windhawkRoot 'windhawk.exe') -restart -tray-only
    Start-Sleep -Seconds 2
}

if (-not $NoProcessRestart -and -not (Get-Process explorer -ErrorAction SilentlyContinue)) {
    Start-Process explorer.exe
}

Write-Host 'Windhawk mod removed.' -ForegroundColor Green
