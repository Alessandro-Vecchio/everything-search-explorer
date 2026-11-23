<#
    Registers the Everything Search namespace extension for the current user
    and installs the DLL in
    %LOCALAPPDATA%\EverythingSearchExplorer\nse\<version>\ (the same layout
    the mod uses).

    A DLL already mapped by Explorer cannot be overwritten: if the copy is
    blocked, the script installs into a sibling folder (e.g. <version>-1)
    instead, so rebuilding without bumping the version is safe. Explorer is
    restarted at the end to load the new DLL.

    No administrator rights required.
#>
[CmdletBinding()]
param(
    [switch]$DesktopIcon
)

$ErrorActionPreference = 'Stop'

$guid = '{7B39C2A2-3E1F-4E4C-9E58-2D9A7B1C4F60}'
$root = $PSScriptRoot
$src = Join-Path $root 'dist\bin'
$sourceDll = Join-Path $src 'EverythingSearchNse.dll'
$dllName = 'EverythingSearchNse.dll'

if (-not (Test-Path $sourceDll)) {
    throw "Missing dist\bin\EverythingSearchNse.dll. Run .\build-nse.ps1 first."
}

$version = (Select-String -Path (Join-Path $root 'nse\version.h') `
    -Pattern 'ESX_VERSION_STRING\s+"([^"]+)"').Matches[0].Groups[1].Value

# Copy-Item wraps the Win32 sharing violation, so check the whole exception
# chain instead of relying on a localised error message.
function Test-LockedFileError {
    param([System.Management.Automation.ErrorRecord]$ErrorRecord)
    for ($ex = $ErrorRecord.Exception; $ex; $ex = $ex.InnerException) {
        if ($ex.HResult -eq -2147024864 -or $ex.Message -match 'used by another process') {
            return $true
        }
    }
    return $false
}

$nseRoot = Join-Path $env:LOCALAPPDATA 'EverythingSearchExplorer\nse'
$suffix = 0
$dll = $null
$copyFailure = $null

while (-not $dll) {
    $folderName = if ($suffix -eq 0) { $version } else { "$version-$suffix" }
    $installDir = Join-Path $nseRoot $folderName
    $candidate = Join-Path $installDir $dllName
    New-Item -ItemType Directory -Force -Path $installDir | Out-Null

    # Reusing an identical DLL avoids a needless folder when Explorer has the
    # current one loaded but the binary did not change.
    if ((Test-Path $candidate) -and
        (Get-FileHash $sourceDll).Hash -eq (Get-FileHash $candidate).Hash) {
        $dll = $candidate
        break
    }

    try {
        Copy-Item -Path (Join-Path $src '*') -Destination $installDir -Force -ErrorAction Stop
        $dll = $candidate
    } catch {
        $copyFailure = $_
        if (-not (Test-LockedFileError $_)) { throw }
        $suffix++
        if ($suffix -gt 10) {
            throw "Could not install the NSE DLL: $($copyFailure.Exception.Message)"
        }
    }
}

$base = "HKCU:\Software\Classes\CLSID\$guid"
New-Item -Force -Path $base | Out-Null
Set-ItemProperty -Path $base -Name '(default)' -Value 'Search results'
Set-ItemProperty -Path $base -Name 'LocalizedString' -Value '@shell32.dll,-34133'

New-Item -Force -Path "$base\InprocServer32" | Out-Null
Set-ItemProperty -Path "$base\InprocServer32" -Name '(default)' -Value $dll
Set-ItemProperty -Path "$base\InprocServer32" -Name 'ThreadingModel' -Value 'Apartment'

New-Item -Force -Path "$base\ShellFolder" | Out-Null
Set-ItemProperty -Path "$base\ShellFolder" -Name 'Attributes' -Value 0x20000000 -Type DWord

# Same state the mod writes, so it recognises this installation.
$state = 'HKCU:\Software\EverythingSearchExplorer'
New-Item -Force -Path $state | Out-Null
Set-ItemProperty -Path $state -Name 'Version' -Value $version
Set-ItemProperty -Path $state -Name 'Path' -Value $dll
if ($DesktopIcon) {
    $namespace = "HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\Desktop\NameSpace\$guid"
    New-Item -Force -Path $namespace | Out-Null
    Set-ItemProperty -Path $namespace -Name '(default)' -Value 'Search results'
    Set-ItemProperty -Path $namespace -Name 'LocalizedString' -Value '@shell32.dll,-34133'
}

Write-Host "Namespace extension registered: $dll" -ForegroundColor Green
if ($suffix -gt 0) {
    Write-Host 'The previous DLL was still loaded, so a separate folder was used.' -ForegroundColor Yellow
}
Write-Host "Open it with:  explorer.exe 'shell:::$guid'"

Write-Host 'Restarting Explorer...'
Stop-Process -Name explorer -Force -ErrorAction SilentlyContinue
Start-Sleep -Seconds 2
# Explorer normally restarts itself; make sure the shell comes back.
if (-not (Get-Process -Name explorer -ErrorAction SilentlyContinue)) {
    Start-Process explorer.exe
}

# Old folders can still be held by the previous Explorer process: delete what
# is unlocked now, leave the rest for the next run.
Get-ChildItem $nseRoot -Directory -ErrorAction SilentlyContinue |
    Where-Object { $_.FullName -ne $installDir } |
    Remove-Item -Recurse -Force -ErrorAction SilentlyContinue
