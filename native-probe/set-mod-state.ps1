#requires -Version 7.0
param(
    [Parameter(Mandatory=$true)][ValidateSet('local@esx-xaml-spike','local@esx-native-probe')][string]$Mod,
    [Parameter(Mandatory=$true)][int]$Disabled,
    [switch]$NoRestart
)
$ErrorActionPreference = 'Stop'
$key = "HKLM:\SOFTWARE\Windhawk\Engine\Mods\$Mod"
if (-not (Test-Path $key)) { throw "Mod not registered: $Mod" }
Set-ItemProperty -Path $key -Name 'Disabled' -Value $Disabled -Type DWord
if (-not $NoRestart) {
    & 'C:\Program Files\Windhawk\windhawk.exe' -restart -tray-only
    Start-Sleep -Seconds 4
}
"$Mod Disabled=$((Get-ItemProperty $key).Disabled)"
