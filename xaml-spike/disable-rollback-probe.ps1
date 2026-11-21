#requires -Version 7.0
$probeId = 'local@esx-rollback-probe'
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]::new($identity)
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    $arguments = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$PSCommandPath`"")
    $elevated = Start-Process -FilePath (Get-Process -Id $PID).Path -Verb RunAs `
        -ArgumentList $arguments -PassThru -Wait
    exit $elevated.ExitCode
}
$key = "HKLM:\SOFTWARE\Windhawk\Engine\Mods\$probeId"
if (Test-Path $key) {
    Set-ItemProperty -Path $key -Name 'Disabled' -Value 1 -Type DWord
    Write-Host "Disabled $probeId (DLL untouched)"
}
& 'C:\Program Files\Windhawk\windhawk.exe' -restart -tray-only
