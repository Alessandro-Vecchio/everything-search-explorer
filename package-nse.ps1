<#
    Turns the MSVC build into the single file users download.

    Reads the version from nse/version.h, copies dist\bin\EverythingSearchNse.dll
    to dist\EverythingSearchNse-v<version>.dll and writes a SHA256 next to it.
    Attach that DLL to a GitHub release; keep the EverythingSearchNse* prefix
    so it can be recognised by the installer.
#>
[CmdletBinding()]
param(
    [string]$Configuration = 'Release'
)

$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot

$versionHeader = Join-Path $root 'nse\version.h'
$version = $null
foreach ($line in Get-Content -LiteralPath $versionHeader) {
    if ($line -match '^\s*#define\s+ESX_VERSION_STRING\s+"([^"]+)"') { $version = $Matches[1] }
}
if (-not $version) { throw "Could not read ESX_VERSION_STRING from $versionHeader" }

$source = Join-Path $root "dist\bin\EverythingSearchNse.dll"
if (-not (Test-Path -LiteralPath $source)) {
    throw "Missing $source. Run .\build-nse.ps1 first."
}

$dist = Join-Path $root 'dist'
# Keep only the current versioned DLL; older ones are build clutter.
Get-ChildItem -LiteralPath $dist -Filter 'EverythingSearchNse-v*.dll' -ErrorAction SilentlyContinue |
    Where-Object { $_.Name -ne "EverythingSearchNse-v$version.dll" } |
    Remove-Item -Force

$target = Join-Path $dist "EverythingSearchNse-v$version.dll"
Copy-Item -LiteralPath $source -Destination $target -Force

$hash = (Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash
$size = (Get-Item -LiteralPath $target).Length
$checksums = Join-Path $dist 'SHA256SUMS.txt'
"$hash  $(Split-Path $target -Leaf)" | Set-Content -LiteralPath $checksums -Encoding ascii

Write-Host "Packaged $(Split-Path $target -Leaf)" -ForegroundColor Green
Write-Host "  version: $version"
Write-Host "  size:    $size bytes"
Write-Host "  sha256:  $hash"
Write-Host "  file:    $checksums"
