<#
    Builds EverythingSearchNse.dll into dist\bin.

    Prerequisites:
      - Visual Studio Build Tools 2022 with "Desktop development with C++"
        (winget install Microsoft.VisualStudio.2022.BuildTools)
      - CMake (MSI or winget package); the build uses the Visual Studio 2022
        generator already included in the Build Tools.

    The Everything SDK is not needed: the extension talks to Everything over
    IPC (WM_COPYDATA), so it neither links nor loads anything from voidtools.
#>
[CmdletBinding()]
param(
    [string]$Configuration = 'Release',
    [string]$CMakePath,
    [switch]$Trace
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

function Find-Tool {
    param(
        [string]$Name,
        [string]$Override,
        [string[]]$CommonPaths,
        [string]$VsWherePattern
    )

    if ($Override) {
        if (-not (Test-Path -LiteralPath $Override)) {
            throw "$Name not found at explicit path: $Override"
        }
        return (Resolve-Path -LiteralPath $Override).Path
    }

    $fromPath = Get-Command $Name -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($fromPath) { return $fromPath.Source }

    if ($VsWherePattern) {
        $fromVs = & $vswhere -latest -products * -find $VsWherePattern 2>$null |
            Select-Object -First 1
        if ($fromVs -and (Test-Path -LiteralPath $fromVs)) { return $fromVs }
    }

    foreach ($candidate in $CommonPaths) {
        if ($candidate -and (Test-Path -LiteralPath $candidate)) {
            return (Resolve-Path -LiteralPath $candidate).Path
        }
    }

    # winget portable installs live under %LOCALAPPDATA%\Microsoft\WinGet\Packages.
    $wingetRoot = Join-Path $env:LOCALAPPDATA 'Microsoft\WinGet\Packages'
    $fromWinget = Get-ChildItem $wingetRoot -Recurse -Filter $Name -ErrorAction SilentlyContinue |
        Sort-Object FullName | Select-Object -First 1 -ExpandProperty FullName
    if ($fromWinget) { return $fromWinget }

    $names = @($CommonPaths | Where-Object { $_ }) -join ', '
    throw "$Name not found. Install it, add it to PATH, or pass an explicit path. Tried: PATH, VS, $names, winget."
}

$cmakeCommon = @(
    (Join-Path $env:ProgramFiles 'CMake\bin\cmake.exe'),
    (Join-Path ${env:ProgramFiles(x86)} 'CMake\bin\cmake.exe'),
    (Join-Path $env:LOCALAPPDATA 'Programs\CMake\bin\cmake.exe'),
    (Join-Path $env:ProgramData 'chocolatey\bin\cmake.exe'),
    (Join-Path $env:USERPROFILE 'scoop\shims\cmake.exe')
)
$cmake = Find-Tool -Name 'cmake.exe' -Override $CMakePath -CommonPaths $cmakeCommon -VsWherePattern '**\cmake.exe'

Write-Host "Using cmake: $cmake"
Write-Host 'Generator: Visual Studio 17 2022 (x64)'

$buildDir = Join-Path $root 'build'
$traceFlag = if ($Trace) { '-DESX_TRACE=ON' } else { '-DESX_TRACE=OFF' }
$args = @(
    "-S `"$root`" -B `"$buildDir`" -G `"Visual Studio 17 2022`" -A x64 $traceFlag",
    "--build `"$buildDir`" --config $Configuration"
)

Write-Host "Configuring + building ($Configuration)..."
cmd /c "`"$devcmd`" -arch=x64 -host_arch=x64 >nul && `"$cmake`" $($args[0]) && `"$cmake`" $($args[1])"
if ($LASTEXITCODE -ne 0) { throw "Build failed (exit code $LASTEXITCODE)" }

Write-Host ''
Write-Host 'Artifacts:' -ForegroundColor Green
Get-ChildItem (Join-Path $root 'dist\bin') | Select-Object Name, Length | Format-Table -AutoSize
