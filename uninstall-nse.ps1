<#
    Removes the per-user registration and the installed binaries.

    The DLL is usually still mapped by Explorer, so it cannot be deleted until
    the shell restarts; the failure is reported instead of thrown, because the
    registration is already gone at that point.
#>
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'

$guid = '{7B39C2A2-3E1F-4E4C-9E58-2D9A7B1C4F60}'

foreach ($key in @(
        "HKCU:\Software\Classes\CLSID\$guid",
        "HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\Desktop\NameSpace\$guid",
        "HKCU:\Software\EverythingExplorerSearch")) {
    if (Test-Path $key) { Remove-Item -Path $key -Recurse -Force }
}

$root = Join-Path $env:LOCALAPPDATA 'EverythingExplorerSearch'
$nse = Join-Path $root 'nse'
if (Test-Path $nse) {
    try {
        Remove-Item -Path $nse -Recurse -Force
    } catch {
        Write-Warning "Could not delete $nse while Explorer has the DLL loaded. Restart Explorer and delete the folder manually."
    }
}

$request = Join-Path $root 'request.txt'
if (Test-Path $request) { Remove-Item -Path $request -Force }

Write-Host 'Uninstalled. Restart Explorer to unload the DLL.' -ForegroundColor Green
