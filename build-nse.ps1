<#
    Builds EverythingSearchNse.dll into dist\bin.

    Prerequisiti:
      - Visual Studio Build Tools 2022 con "Desktop development with C++"
        (winget install Microsoft.VisualStudio.2022.BuildTools)
      - CMake + Ninja (inclusi nei Build Tools)

    Non serve scaricare l'SDK di Everything: l'estensione parla con Everything
    via IPC (WM_COPYDATA), quindi non linka ne' carica nulla di voidtools.
#>
[CmdletBinding()]
param(
    [string]$Configuration = 'Release'
)

$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $vswhere)) {
    throw @'
Visual Studio Build Tools not found.

Install them with:
    winget install Microsoft.VisualStudio.2022.BuildTools --override "--wait --passive --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
'@
}

$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw "MSVC toolset not found in the Visual Studio installation at: $vs" }

$devcmd = Join-Path $vs 'Common7\Tools\VsDevCmd.bat'
$cmake = (& $vswhere -latest -products * -find '**\cmake.exe' | Select-Object -First 1)
if (-not $cmake) { $cmake = 'cmake' }

$buildDir = Join-Path $root 'build'
$args = @(
    "-S `"$root`" -B `"$buildDir`" -G Ninja -DCMAKE_BUILD_TYPE=$Configuration",
    "--build `"$buildDir`""
)

Write-Host "Configuring + building ($Configuration)..."
cmd /c "`"$devcmd`" -arch=x64 -host_arch=x64 >nul && `"$cmake`" $($args[0]) && `"$cmake`" $($args[1])"
if ($LASTEXITCODE -ne 0) { throw "Build failed (exit code $LASTEXITCODE)" }

Write-Host ''
Write-Host 'Artifacts:' -ForegroundColor Green
Get-ChildItem (Join-Path $root 'dist\bin') | Select-Object Name, Length | Format-Table -AutoSize
