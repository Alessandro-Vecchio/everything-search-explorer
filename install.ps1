<#
    Registers the Everything Search namespace extension for the current user
    and installs the DLL in
    %LOCALAPPDATA%\EverythingExplorerSearch\nse\<version>\ (the same layout
    the mod uses).

    No administrator rights required.
#>
[CmdletBinding()]
param(
    [switch]$DesktopIcon,
    [switch]$RestartExplorer
)

$ErrorActionPreference = 'Stop'

$guid = '{7B39C2A2-3E1F-4E4C-9E58-2D9A7B1C4F60}'
$root = $PSScriptRoot
$src = Join-Path $root 'dist\bin'

if (-not (Test-Path (Join-Path $src 'EverythingSearchNse.dll'))) {
    throw "Missing dist\bin\EverythingSearchNse.dll. Run .\build.ps1 first."
}

$version = (Select-String -Path (Join-Path $root 'nse\version.h') `
    -Pattern 'ESX_VERSION_STRING\s+"([^"]+)"').Matches[0].Groups[1].Value

# Versioned folder, same layout the mod uses: a DLL already mapped by Explorer
# cannot be overwritten in place, so a new build gets a new folder.
$installDir = Join-Path $env:LOCALAPPDATA "EverythingExplorerSearch\nse\$version"
New-Item -ItemType Directory -Force -Path $installDir | Out-Null
Copy-Item -Path (Join-Path $src '*') -Destination $installDir -Force

$dll = Join-Path $installDir 'EverythingSearchNse.dll'

$base = "HKCU:\Software\Classes\CLSID\$guid"
New-Item -Force -Path $base | Out-Null
Set-ItemProperty -Path $base -Name '(default)' -Value 'Everything Search'

New-Item -Force -Path "$base\InprocServer32" | Out-Null
Set-ItemProperty -Path "$base\InprocServer32" -Name '(default)' -Value $dll
Set-ItemProperty -Path "$base\InprocServer32" -Name 'ThreadingModel' -Value 'Apartment'

New-Item -Force -Path "$base\ShellFolder" | Out-Null
Set-ItemProperty -Path "$base\ShellFolder" -Name 'Attributes' -Value 0x20000000 -Type DWord

# Same state the mod writes, so its setup panel sees this install as its own.
$state = 'HKCU:\Software\EverythingExplorerSearch'
New-Item -Force -Path $state | Out-Null
Set-ItemProperty -Path $state -Name 'Version' -Value $version
Set-ItemProperty -Path $state -Name 'Path' -Value $dll
if ($DesktopIcon) {
    $namespace = "HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\Desktop\NameSpace\$guid"
    New-Item -Force -Path $namespace | Out-Null
    Set-ItemProperty -Path $namespace -Name '(default)' -Value 'Everything Search'
}

Write-Host "Namespace extension registered: $dll" -ForegroundColor Green
Write-Host "Open it with:  explorer.exe 'shell:::$guid'"

if ($RestartExplorer) {
    Write-Host 'Restarting Explorer...'
    Stop-Process -Name explorer -Force -ErrorAction SilentlyContinue
} else {
    Write-Host 'Restart Explorer (or use -RestartExplorer) to load the new extension.' -ForegroundColor Yellow
}
