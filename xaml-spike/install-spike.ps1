#requires -Version 7.0
<#
.SYNOPSIS
    Builds and installs the XAML search-box spike as a Windhawk mod.

.DESCRIPTION
    Requests elevation for the Windhawk engine files and HKLM registration.
    The mod is limited to explorer.exe. A uniquely named DLL is installed so
    Explorer can keep an older, already-mapped build until Windhawk reloads.

.PARAMETER Dll
    Install an already-built spike DLL instead of building it.

.PARAMETER SkipBuild
    Reuse dist\spike\esx-xaml-spike.dll.

.PARAMETER NoRestart
    Install and register the mod without restarting Windhawk.

.PARAMETER Enable
    Activate the experimental mod after installation. By default it is left
    disabled to avoid injecting the unverified XAML Diagnostics hook.
#>
[CmdletBinding()]
param(
    [string]$Dll,
    [switch]$SkipBuild,
    [switch]$NoRestart,
    [switch]$Enable
)

$ErrorActionPreference = 'Stop'

$modId = 'local@esx-xaml-spike'
$modVersion = '0.1.0'
$windhawkRoot = 'C:\Program Files\Windhawk'
$engineMods = 'C:\ProgramData\Windhawk\Engine\Mods\64'
$repoRoot = Split-Path -Parent $PSScriptRoot
$buildScript = Join-Path $PSScriptRoot 'build-spike.cmd'
$distDll = Join-Path $repoRoot 'dist\spike\esx-xaml-spike.dll'

if ($Dll) {
    $Dll = [IO.Path]::GetFullPath($Dll)
}

$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]::new($identity)
$isAdmin = $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) {
    $arguments = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$PSCommandPath`"")
    if ($Dll) { $arguments += @('-Dll', "`"$Dll`"") }
    if ($SkipBuild) { $arguments += '-SkipBuild' }
    if ($NoRestart) { $arguments += '-NoRestart' }
    if ($Enable) { $arguments += '-Enable' }

    Write-Host 'Requesting administrator rights...'
    $elevated = Start-Process -FilePath (Get-Process -Id $PID).Path -Verb RunAs `
        -ArgumentList $arguments -PassThru -Wait
    exit $elevated.ExitCode
}

if (-not (Test-Path $windhawkRoot)) {
    throw "Windhawk is not installed ($windhawkRoot)."
}

if (-not $Dll) {
    if (-not $SkipBuild) {
        if (-not (Test-Path $buildScript)) {
            throw "Build script not found: $buildScript"
        }
        & $env:ComSpec /d /c "`"$buildScript`""
        if ($LASTEXITCODE -ne 0) {
            throw "XAML spike build failed with exit code $LASTEXITCODE."
        }
    }
    $Dll = $distDll
}

if (-not (Test-Path -LiteralPath $Dll -PathType Leaf)) {
    throw "Spike DLL not found: $Dll"
}

$buildId = Get-Date -Format 'yyyyMMddHHmmssfff'
$dllName = "$modId`_$modVersion`_$buildId.dll"
New-Item -ItemType Directory -Path $engineMods -Force | Out-Null
$target = Join-Path $engineMods $dllName
Copy-Item -LiteralPath $Dll -Destination $target

# The bootstrapper is not part of the engine; ship it next to the spike DLL.
$bootstrapSource = Join-Path $repoRoot 'xaml-spike\bin\Microsoft.WindowsAppRuntime.Bootstrap.dll'
if (Test-Path -LiteralPath $bootstrapSource) {
    Copy-Item -LiteralPath $bootstrapSource `
        -Destination (Join-Path $engineMods 'Microsoft.WindowsAppRuntime.Bootstrap.dll') -Force
}

# The probe was only a bisect harness; it must not run alongside the spike.
$probeKey = 'HKLM:\SOFTWARE\Windhawk\Engine\Mods\local@esx-native-probe'
if (Test-Path $probeKey) {
    Set-ItemProperty -Path $probeKey -Name 'Disabled' -Value 1 -Type DWord
    Write-Host 'Disabled the native probe mod.'
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
$disabled = if ($Enable) { 0 } else { 1 }
Set-ItemProperty -Path $key -Name 'Disabled' -Value $disabled -Type DWord

if (-not $NoRestart) {
    Write-Host 'Restarting Windhawk...'
    & (Join-Path $windhawkRoot 'windhawk.exe') -restart -tray-only
    Start-Sleep -Seconds 4
}

Write-Host ''
if ($Enable) {
    Write-Host 'XAML spike installed and enabled:' -ForegroundColor Green
} else {
    Write-Host 'XAML spike installed but left disabled:' -ForegroundColor Yellow
}
Write-Host "  $key"
Write-Host "  $target"
if (-not $Enable) {
    Write-Host 'Use -Enable only for an explicitly requested live test.'
}
if ($NoRestart) {
    Write-Host 'Windhawk was not restarted; reload it to activate the new build.'
}
