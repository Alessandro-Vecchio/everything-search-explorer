<#
.SYNOPSIS
    Checks the prerequisites for building and installing Everything Search Explorer.

.DESCRIPTION
    Reports every prerequisite with its current value and minimum version. With
    -CheckOnly it never prompts or changes the machine and returns 0 when all
    requirements are met, 1 otherwise. Otherwise, if anything is missing or too
    old, it asks once and installs what it can with winget (plus the NuGet
    bootstrap for the WinMD files).

    Press Enter before the window closes; -NoPause skips that wait for automated
    runs. Build and install are left to the dedicated scripts:
      build-nse.ps1 / install-nse.ps1
      mod/build-mod.ps1 / install-mod.ps1
#>
[CmdletBinding()]
param(
    [switch]$CheckOnly,
    [switch]$NoPause
)

$ErrorActionPreference = 'Stop'
$exitCode = 1
$scriptRoot = $PSScriptRoot

if ($PSVersionTable.PSVersion.Major -lt 7) {
    # "Run with PowerShell" starts Windows PowerShell 5.1. Restart in PowerShell 7
    # and keep the window open only when an interactive run requested it.
    $pwsh = Get-Command pwsh.exe -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $pwsh) {
        $candidates = @(
            (Join-Path $env:ProgramFiles 'PowerShell\7\pwsh.exe'),
            (Join-Path ${env:ProgramFiles(x86)} 'PowerShell\7\pwsh.exe'),
            (Join-Path $env:LOCALAPPDATA 'Microsoft\PowerShell\7\pwsh.exe')
        )
        $pwsh = @($candidates | Where-Object { $_ -and (Test-Path -LiteralPath $_) } | Select-Object -First 1)[0]
    }
    if ($pwsh) {
        $pwshPath = if ($pwsh.Source) { $pwsh.Source } else { $pwsh }
        $arguments = @()
        if (-not $NoPause -and -not $CheckOnly) { $arguments += '-NoExit' }
        $arguments += @('-NoProfile', '-File', $PSCommandPath)
        if ($CheckOnly) { $arguments += '-CheckOnly' }
        if ($NoPause) { $arguments += '-NoPause' }
        & $pwshPath @arguments
        exit $LASTEXITCODE
    }
    Write-Host 'PowerShell 7+ is required. See https://aka.ms/powershell' -ForegroundColor Red
    if (-not $NoPause -and -not $CheckOnly) {
        try { $null = Read-Host 'Press Enter to exit' } catch { }
    }
    exit 1
}

function Invoke-NativeChecked {
    param(
        [string]$FilePath,
        [string[]]$Arguments = @(),
        [switch]$AllowFailure
    )
    $output = @(& $FilePath @Arguments 2>&1 | ForEach-Object { "$_" })
    $code = $LASTEXITCODE
    if ($code -ne 0 -and -not $AllowFailure) {
        throw "$FilePath exited with code $code"
    }
    return [pscustomobject]@{ Output = $output; ExitCode = $code }
}

function Get-ParsedVersion {
    param([string]$Text)
    if ($Text -match '(\d+\.\d+(?:\.\d+)*)') {
        try { return [version]$Matches[1] } catch { }
    }
    return [version]'0.0'
}

function Test-IsAdmin {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    return (New-Object Security.Principal.WindowsPrincipal($identity)).IsInRole(
        [Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Test-ProcessElevated {
    param([int]$ProcessId)
    try {
        Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class EsxTokenCheck {
    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern IntPtr OpenProcess(uint desiredAccess, bool inheritHandle, int processId);
    [DllImport("advapi32.dll", SetLastError = true)]
    public static extern bool OpenProcessToken(IntPtr processHandle, uint desiredAccess, out IntPtr tokenHandle);
    [DllImport("advapi32.dll", SetLastError = true)]
    public static extern bool GetTokenInformation(IntPtr tokenHandle, int infoClass, out int info, int length, out int returned);
    [DllImport("kernel32.dll")]
    public static extern bool CloseHandle(IntPtr handle);
}
'@ -ErrorAction SilentlyContinue
        $process = [EsxTokenCheck]::OpenProcess(0x1000, $false, $ProcessId)
        if ($process -eq [IntPtr]::Zero) { return $null }
        try {
            $token = [IntPtr]::Zero
            if (-not [EsxTokenCheck]::OpenProcessToken($process, 0x0008, [ref]$token)) { return $null }
            try {
                $elevated = 0
                $returned = 0
                if ([EsxTokenCheck]::GetTokenInformation($token, 20, [ref]$elevated, 4, [ref]$returned)) {
                    return [bool]$elevated
                }
                return $null
            } finally {
                [void][EsxTokenCheck]::CloseHandle($token)
            }
        } finally {
            [void][EsxTokenCheck]::CloseHandle($process)
        }
    } catch {
        return $null
    }
}

function Find-CMake {
    $found = Get-Command cmake.exe -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($found) { return $found.Source }
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path -LiteralPath $vswhere) {
        $result = Invoke-NativeChecked -FilePath $vswhere -Arguments @('-latest', '-products', '*', '-find', '**\cmake.exe') -AllowFailure
        if ($result.ExitCode -eq 0 -and $result.Output.Count -gt 0) {
            $fromVs = $result.Output[0]
            if ($fromVs -and (Test-Path -LiteralPath $fromVs)) { return $fromVs }
        }
    }
    $candidates = @(
        (Join-Path $env:ProgramFiles 'CMake\bin\cmake.exe'),
        (Join-Path ${env:ProgramFiles(x86)} 'CMake\bin\cmake.exe'),
        (Join-Path $env:LOCALAPPDATA 'Programs\CMake\bin\cmake.exe')
    )
    $wingetRoot = Join-Path $env:LOCALAPPDATA 'Microsoft\WinGet\Packages\Kitware.CMake*'
    if (Test-Path -Path $wingetRoot) {
        $candidates += @(Get-ChildItem -Path $wingetRoot -Recurse -Filter 'cmake.exe' -ErrorAction SilentlyContinue |
            Sort-Object FullName -Descending | Select-Object -First 1 -ExpandProperty FullName)
    }
    return @($candidates | Where-Object { $_ -and (Test-Path -LiteralPath $_) } | Select-Object -First 1)[0]
}

function Find-CppWinrt {
    $roots = @(
        'C:\Program Files (x86)\Windows Kits\10\bin',
        (Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\bin'),
        (Join-Path $env:ProgramFiles 'Windows Kits\10\bin')
    )
    foreach ($root in (@($roots | Where-Object { $_ -and (Test-Path -LiteralPath $_) } | Select-Object -Unique))) {
        $found = @(Get-ChildItem -LiteralPath $root -Directory |
            Where-Object { $_.Name -match '^\d+\.' } |
            Sort-Object { try { [version]$_.Name } catch { [version]'0.0' } } -Descending |
            ForEach-Object { Join-Path $_.FullName 'x64\cppwinrt.exe' } |
            Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1)[0]
        if ($found) { return $found }
    }
    return $null
}

function Find-PackageDir {
    param([string]$Package, [string]$SubPath)
    $root = Join-Path $env:USERPROFILE ".nuget\packages\$Package"
    if (-not (Test-Path -LiteralPath $root)) { return $null }
    return @(Get-ChildItem -LiteralPath $root -Directory |
        Sort-Object { try { [version]($_.Name -replace '[-+].*$', '') } catch { [version]'0.0' } } -Descending |
        ForEach-Object {
            $target = if ($SubPath) { Join-Path $_.FullName $SubPath } else { $_.FullName }
            if (Test-Path -LiteralPath $target) {
                [pscustomobject]@{ Path = $target; Version = Get-ParsedVersion ($_.Name -replace '[-+].*$', '') }
            }
        } | Where-Object { $_ } | Select-Object -First 1)[0]
}

function Find-WinmdFile {
    param([string]$Path, [string]$File)
    if (-not $Path -or -not (Test-Path -LiteralPath $Path)) { return $null }
    return @(Get-ChildItem -LiteralPath $Path -Recurse -Filter $File -File -ErrorAction SilentlyContinue |
        Sort-Object FullName -Descending | Select-Object -First 1 -ExpandProperty FullName)[0]
}

function Get-WinmdState {
    $ui = Find-PackageDir -Package 'microsoft.windowsappsdk.winui' -SubPath 'metadata'
    $interactive = Find-PackageDir -Package 'microsoft.windowsappsdk.interactiveexperiences' -SubPath 'metadata'
    $foundation = Find-PackageDir -Package 'microsoft.windowsappsdk.foundation' -SubPath 'metadata'
    $webview = Find-PackageDir -Package 'microsoft.web.webview2' -SubPath 'lib'
    $sdkRef = Find-PackageDir -Package 'microsoft.windows.sdk.net.ref' -SubPath 'winmd'
    $needed = @(
        @{ Path = if ($ui) { $ui.Path }; File = 'Microsoft.UI.Xaml.winmd' },
        @{ Path = if ($interactive) { $interactive.Path }; File = 'Microsoft.UI.winmd' },
        @{ Path = if ($webview) { $webview.Path }; File = 'Microsoft.Web.WebView2.Core.winmd' },
        @{ Path = if ($sdkRef) { $sdkRef.Path }; File = 'Windows.Foundation.FoundationContract.winmd' }
    )
    $missing = @($needed | Where-Object { -not (Find-WinmdFile -Path $_.Path -File $_.File) } |
        ForEach-Object { $_.File })
    $foundationOk = $false
    if ($foundation) {
        $foundationOk = @(Get-ChildItem -LiteralPath $foundation.Path -Recurse -Filter '*.winmd' -File -ErrorAction SilentlyContinue).Count -gt 0
    }
    if (-not $foundationOk) { $missing += 'Microsoft.WindowsAppSDK.Foundation metadata' }
    $lowest = @($ui, $interactive, $foundation) | Where-Object { $_ } | Sort-Object Version | Select-Object -First 1
    return [pscustomobject]@{
        OK = ($missing.Count -eq 0)
        Version = if ($lowest) { $lowest.Version } else { [version]'0.0' }
        Missing = $missing -join ', '
    }
}

function Get-EverythingPath {
    $process = Get-Process -Name Everything -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($process -and $process.Path -and (Test-Path -LiteralPath $process.Path)) { return $process.Path }
    $candidates = @(
        'C:\Program Files\Everything\Everything.exe',
        'C:\Program Files (x86)\Everything\Everything.exe',
        (Join-Path $env:LOCALAPPDATA 'Programs\Everything\Everything.exe')
    )
    return @($candidates | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1)[0]
}

function Get-DotnetPath {
    $command = Get-Command dotnet.exe -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($command) { return $command.Source }
    $known = 'C:\Program Files\dotnet\dotnet.exe'
    if (Test-Path -LiteralPath $known) { return $known }
    return $null
}

function Get-DotnetSdkVersion {
    $dotnet = Get-DotnetPath
    if (-not $dotnet) { return $null }
    $result = Invoke-NativeChecked -FilePath $dotnet -Arguments @('--list-sdks') -AllowFailure
    if ($result.ExitCode -ne 0) { return $null }
    $versions = @($result.Output | ForEach-Object {
        if ($_ -match '^(\d+\.\d+(?:\.\d+)*)') { Get-ParsedVersion $Matches[1] }
    })
    return @($versions | Where-Object { $_ } | Sort-Object -Descending | Select-Object -First 1)[0]
}

function Invoke-Checks {
    $checks = @()

    $projectionPath = Join-Path $scriptRoot 'mod\generated\winrt\base.h'
    $projectionPresent = Test-Path -LiteralPath $projectionPath
    $winmd = Get-WinmdState
    $winmdNeeded = (-not $projectionPresent) -and (-not $winmd.OK -or $winmd.Version -lt [version]'1.8')

    $os = Get-CimInstance Win32_OperatingSystem
    $osBuild = [int]$os.BuildNumber
    $systemType = (Get-CimInstance Win32_ComputerSystem).SystemType
    $isX64 = ($systemType -match 'x64|64-bit') -and ($systemType -notmatch 'ARM')
    $checks += [pscustomobject]@{
        Name = 'Windows 11 x64'
        Current = "$($os.Caption) build $osBuild / $systemType"
        Minimum = 'build 22000 / x64'
        OK = ($osBuild -ge 22000 -and $isX64)
        Fix = $null
        Manual = 'Install Windows 11 x64 (build 22000 or newer).'
    }

    $checks += [pscustomobject]@{
        Name = 'PowerShell 7'
        Current = $PSVersionTable.PSVersion.ToString()
        Minimum = '7.0'
        OK = ($PSVersionTable.PSVersion.Major -ge 7)
        Fix = $null
        Manual = 'Install PowerShell 7: https://aka.ms/powershell'
    }

    $winget = Get-Command winget.exe -ErrorAction SilentlyContinue | Select-Object -First 1
    $checks += [pscustomobject]@{
        Name = 'winget'
        Current = if ($winget) { $winget.Source } else { 'not found' }
        Minimum = 'any'
        OK = [bool]$winget
        Fix = $null
        Manual = 'Update Windows 11 or install App Installer from the Microsoft Store.'
    }

    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    $vsVersion = $null
    if (Test-Path -LiteralPath $vswhere) {
        $result = Invoke-NativeChecked -FilePath $vswhere -Arguments @(
            '-latest', '-products', '*', '-requires', 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64',
            '-property', 'installationVersion'
        ) -AllowFailure
        if ($result.ExitCode -eq 0 -and $result.Output.Count -gt 0) {
            $vsVersion = Get-ParsedVersion $result.Output[0]
        }
    }
    $checks += [pscustomobject]@{
        Name = 'Visual Studio Build Tools (C++)'
        Current = if ($vsVersion) { $vsVersion.ToString() } else { 'not found' }
        Minimum = '17.0 (2022)'
        OK = ($vsVersion -and $vsVersion.Major -ge 17)
        Fix = @{ Kind = 'winget'; Id = 'Microsoft.VisualStudio.2022.BuildTools'; Args = @('--override', '"--wait --passive --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"') }
        Manual = ''
    }

    $cmake = Find-CMake
    $cmakeVersion = $null
    if ($cmake) {
        $result = Invoke-NativeChecked -FilePath $cmake -Arguments @('--version') -AllowFailure
        if ($result.ExitCode -eq 0 -and $result.Output.Count -gt 0) {
            $cmakeVersion = Get-ParsedVersion $result.Output[0]
        }
    }
    $checks += [pscustomobject]@{
        Name = 'CMake'
        Current = if ($cmakeVersion) { "$cmakeVersion ($cmake)" } else { 'not found' }
        Minimum = '3.20'
        OK = ($cmakeVersion -and $cmakeVersion -ge [version]'3.20')
        Fix = @{ Kind = 'winget'; Id = 'Kitware.CMake'; Args = @() }
        Manual = ''
    }

    $dotnetVersion = Get-DotnetSdkVersion
    if ($winmdNeeded) {
        $checks += [pscustomobject]@{
            Name = '.NET SDK'
            Current = if ($dotnetVersion) { $dotnetVersion.ToString() } else { 'not found' }
            Minimum = '8.0'
            OK = ($dotnetVersion -and $dotnetVersion -ge [version]'8.0')
            Fix = @{ Kind = 'winget'; Id = 'Microsoft.DotNet.SDK.8'; Args = @() }
            Manual = 'Install the .NET 8 SDK to populate the NuGet cache.'
        }
    } else {
        $checks += [pscustomobject]@{
            Name = '.NET SDK'
            Current = 'not required (no NuGet bootstrap needed)'
            Minimum = 'n/a'
            OK = $true
            Fix = $null
            Manual = ''
        }
    }

    $cppwinrt = Find-CppWinrt
    $sdkVersion = $null
    if ($cppwinrt -match 'bin\\(\d+\.\d+\.\d+\.\d+)\\') { $sdkVersion = Get-ParsedVersion $Matches[1] }
    if ($projectionPresent) {
        $checks += [pscustomobject]@{
            Name = 'Windows SDK / cppwinrt'
            Current = 'not required (WinRT projection already present)'
            Minimum = 'n/a'
            OK = $true
            Fix = $null
            Manual = ''
        }
    } else {
        $checks += [pscustomobject]@{
            Name = 'Windows SDK / cppwinrt'
            Current = if ($cppwinrt) { "$sdkVersion ($cppwinrt)" } else { 'not found' }
            Minimum = 'SDK with cppwinrt.exe'
            OK = [bool]$cppwinrt
            Fix = $null
            Manual = 'Install the Windows SDK with the C++/WinRT tools.'
        }
    }

    if ($projectionPresent) {
        $checks += [pscustomobject]@{
            Name = 'NuGet WinMD files'
            Current = 'not required (WinRT projection already present)'
            Minimum = 'n/a'
            OK = $true
            Fix = $null
            Manual = ''
        }
    } else {
        $checks += [pscustomobject]@{
            Name = 'NuGet WinMD files'
            Current = if ($winmd.Version -gt [version]'0.0') { $winmd.Version.ToString() } else { 'not found' }
            Minimum = 'WindowsAppSDK 1.8 + WebView2 + SDK.NET.Ref'
            OK = ($winmd.OK -and $winmd.Version -ge [version]'1.8')
            Fix = @{ Kind = 'dotnet-winmd' }
            Manual = "Install the .NET 8 SDK and re-run. Missing: $($winmd.Missing)"
        }
    }

    $engineRoot = 'C:\Program Files\Windhawk\Engine'
    $whVersion = $null
    if (Test-Path -LiteralPath $engineRoot) {
        $whVersion = @(Get-ChildItem -LiteralPath $engineRoot -Directory |
            ForEach-Object {
                try { [pscustomobject]@{ Version = Get-ParsedVersion $_.Name; Lib = Join-Path $_.FullName '64\windhawk.lib' } } catch { }
            } | Where-Object { Test-Path -LiteralPath $_.Lib } | Sort-Object Version -Descending | Select-Object -First 1 -ExpandProperty Version)[0]
    }
    $checks += [pscustomobject]@{
        Name = 'Windhawk'
        Current = if ($whVersion) { $whVersion.ToString() } else { 'not found' }
        Minimum = '1.7'
        OK = ($whVersion -and $whVersion -ge [version]'1.7')
        Fix = @{ Kind = 'winget'; Id = 'Windhawk.Windhawk'; Args = @() }
        Manual = ''
    }

    $everythingPath = Get-EverythingPath
    $everythingVersion = $null
    if ($everythingPath -and (Test-Path -LiteralPath $everythingPath)) {
        $everythingVersion = Get-ParsedVersion (Get-Item -LiteralPath $everythingPath).VersionInfo.ProductVersion
    }
    $installedOk = ($everythingVersion -and $everythingVersion -ge [version]'1.5')
    $checks += [pscustomobject]@{
        Name = 'Everything installation'
        Current = if ($everythingVersion) { "$everythingVersion ($everythingPath)" } else { 'not found' }
        Minimum = '1.5'
        OK = [bool]$installedOk
        Fix = if ($installedOk) { $null } else { @{ Kind = 'winget'; Id = 'voidtools.Everything'; Args = @() } }
        Manual = 'Install Everything 1.5 or newer.'
    }

    $everythingProcess = Get-Process -Name Everything -ErrorAction SilentlyContinue | Select-Object -First 1
    $checks += [pscustomobject]@{
        Name = 'Everything running'
        Current = if ($everythingProcess) { "running (pid $($everythingProcess.Id))" } else { 'not running' }
        Minimum = 'running'
        OK = [bool]$everythingProcess
        Fix = $null
        Manual = 'Start Everything after installation.'
    }

    $elevation = $null
    $elevationWarning = $null
    if ($everythingProcess) {
        $elevation = Test-ProcessElevated -ProcessId $everythingProcess.Id
        if ($elevation -eq $true) {
            $elevationManual = 'Everything is running as administrator. Restart it as a normal user.'
        } elseif ($elevation -eq $null) {
            $elevationWarning = 'Elevation could not be verified from this shell. Confirm Everything is not running as administrator.'
        }
    }
    $checks += [pscustomobject]@{
        Name = 'Everything non-elevated'
        Current = if (-not $everythingProcess) { 'not applicable (not running)' } else { "elevated=$elevation" }
        Minimum = 'normal user'
        OK = (-not $everythingProcess -or $elevation -eq $false -or [bool]$elevationWarning)
        Warning = $elevationWarning
        Fix = $null
        Manual = $elevationManual
    }

    $advancedKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\Advanced'
    $separate = (Get-ItemProperty -Path $advancedKey -Name SeparateProcess -ErrorAction SilentlyContinue).SeparateProcess
    $checks += [pscustomobject]@{
        Name = 'Explorer SeparateProcess'
        Current = if ($null -eq $separate) { 'unset' } else { "$separate" }
        Minimum = '1'
        OK = ($separate -eq 1)
        Fix = @{ Kind = 'registry' }
        Manual = 'Set HKCU Explorer Advanced SeparateProcess = 1 and restart Explorer.'
    }

    # winget only matters when a winget-based install is actually pending.
    $needsWinget = @($checks | Where-Object { -not $_.OK -and $_.Fix -and $_.Fix.Kind -eq 'winget' }).Count -gt 0
    if (-not $needsWinget) {
        $wingetCheck = $checks | Where-Object { $_.Name -eq 'winget' }
        if ($wingetCheck) { $wingetCheck.OK = $true }
    }

    return $checks
}

function Show-Checks {
    param($Checks)
    $Checks | Select-Object Name, Current, Minimum, @{
        n = 'State'
        e = { if ($_.Warning) { 'WARN' } elseif ($_.OK) { 'OK' } else { 'MISSING' } }
    } | Format-Table -AutoSize | Out-String | Write-Host
}

function Show-Warnings {
    param($Checks)
    $warnings = @($Checks | Where-Object { $_.Warning })
    if ($warnings.Count -eq 0) { return }
    Write-Host ''
    Write-Host 'Warnings:' -ForegroundColor Yellow
    foreach ($item in $warnings) { Write-Host "  - $($item.Name): $($item.Warning)" -ForegroundColor Yellow }
}

function Install-Missing {
    param($Missing)

    $tempRoot = Join-Path $env:TEMP ('esx-setup-' + [guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Force -Path $tempRoot | Out-Null
    try {
        $wingetItems = @($Missing | Where-Object { $_.Fix -and $_.Fix.Kind -eq 'winget' })
        if ($wingetItems.Count -gt 0) {
            if (-not (Get-Command winget.exe -ErrorAction SilentlyContinue)) {
                Write-Host 'winget is not available; install App Installer first.' -ForegroundColor Yellow
            } else {
                $status = Join-Path $tempRoot 'install.status'
                $batch = Join-Path $tempRoot 'install.cmd'
                $lines = @('@echo off')
                foreach ($item in $wingetItems) {
                    $arguments = @('install', '--id', $item.Fix.Id, '--accept-package-agreements', '--accept-source-agreements') + $item.Fix.Args
                    $lines += 'winget ' + ($arguments -join ' ')
                    $lines += 'echo ' + $item.Fix.Id + '=%ERRORLEVEL% >> "' + $status + '"'
                }
                Set-Content -LiteralPath $batch -Value $lines -Encoding ASCII
                Write-Host 'Running winget installations...' -ForegroundColor Cyan
                if (Test-IsAdmin) {
                    & cmd.exe /c "`"$batch`""
                } else {
                    Write-Host 'Accept the UAC prompt to install the missing tools.' -ForegroundColor Yellow
                    try {
                        $installer = Start-Process -FilePath cmd.exe -Verb RunAs -ArgumentList "/c `"$batch`"" -PassThru -Wait -ErrorAction Stop
                    } catch {
                        Write-Host 'UAC was declined or the elevated shell could not start.' -ForegroundColor Red
                        $installer = $null
                    }
                    if (-not $installer -or $installer.ExitCode -ne 0) {
                        Write-Host 'The elevated installer did not complete.' -ForegroundColor Red
                    }
                }

                if (Test-Path -LiteralPath $status) {
                    $codes = @{}
                    foreach ($line in (Get-Content -LiteralPath $status)) {
                        if ($line -match '^\s*(.+?)=(-?\d+)\s*$') { $codes[$Matches[1].Trim()] = [int]$Matches[2] }
                    }
                    foreach ($item in $wingetItems) {
                        $id = $item.Fix.Id
                        if (-not $codes.ContainsKey($id)) {
                            Write-Host "  $id : no result recorded" -ForegroundColor Yellow
                        } elseif ($codes[$id] -eq 0) {
                            Write-Host "  $id : installed successfully" -ForegroundColor Green
                        } else {
                            Write-Host "  $id : failed (exit code $($codes[$id]))" -ForegroundColor Red
                        }
                    }
                } else {
                    Write-Host 'No installation status was produced. Check the winget results manually.' -ForegroundColor Yellow
                }
            }
        }

        foreach ($item in ($Missing | Where-Object { $_.Fix -and $_.Fix.Kind -eq 'dotnet-winmd' })) {
            $dotnet = Get-DotnetPath
            if (-not $dotnet) {
                Write-Host 'dotnet SDK not found; install .NET 8 SDK, then re-run this script.' -ForegroundColor Yellow
                continue
            }
            $project = Join-Path $tempRoot 'winmd'
            New-Item -ItemType Directory -Force -Path $project | Out-Null
            Push-Location $project
            try {
                Invoke-NativeChecked -FilePath $dotnet -Arguments @('new', 'classlib', '-f', 'net8.0', '--force') | Out-Null
                Invoke-NativeChecked -FilePath $dotnet -Arguments @('add', 'package', 'Microsoft.WindowsAppSDK') | Out-Null
                Invoke-NativeChecked -FilePath $dotnet -Arguments @('add', 'package', 'Microsoft.Web.WebView2') | Out-Null
                Invoke-NativeChecked -FilePath $dotnet -Arguments @('add', 'package', 'Microsoft.Windows.SDK.NET.Ref') | Out-Null
                Invoke-NativeChecked -FilePath $dotnet -Arguments @('restore') | Out-Null
                Write-Host 'NuGet WinMD bootstrap completed.' -ForegroundColor Green
            } catch {
                Write-Host "NuGet WinMD bootstrap failed: $_" -ForegroundColor Red
            } finally {
                Pop-Location
            }
        }

        foreach ($item in ($Missing | Where-Object { $_.Fix -and $_.Fix.Kind -eq 'registry' })) {
            $advancedKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\Advanced'
            if (-not (Test-Path -LiteralPath $advancedKey)) {
                New-Item -Path $advancedKey -Force | Out-Null
            }
            Set-ItemProperty -Path $advancedKey -Name SeparateProcess -Value 1 -Type DWord
            Write-Host 'SeparateProcess set to 1. Restart Explorer to apply it.' -ForegroundColor Yellow
        }
    } finally {
        Remove-Item -LiteralPath $tempRoot -Recurse -Force -ErrorAction SilentlyContinue
    }
}

try {
    $checks = Invoke-Checks
    Show-Checks -Checks $checks
    Show-Warnings -Checks $checks

    $missing = @($checks | Where-Object { -not $_.OK })
    if ($missing.Count -eq 0) {
        Write-Host 'All prerequisites are satisfied. Build and install with the dedicated scripts.' -ForegroundColor Green
        $exitCode = 0
        exit $exitCode
    }

    Write-Host ''
    Write-Host 'Missing or outdated prerequisites:' -ForegroundColor Yellow
    foreach ($item in $missing) { Write-Host "  - $($item.Name): $($item.Current) (minimum: $($item.Minimum))" }

    $auto = @($missing | Where-Object { $_.Fix })
    $manual = @($missing | Where-Object { -not $_.Fix })

    if ($CheckOnly) {
        if ($manual.Count -gt 0) {
            Write-Host 'Manual steps:' -ForegroundColor Yellow
            foreach ($item in $manual) { Write-Host "  - $($item.Name): $($item.Manual)" }
        }
        Write-Host 'Check-only mode: no changes made.' -ForegroundColor Yellow
        exit 1
    }

    if ($auto.Count -eq 0) {
        Write-Host 'No automatic fix is available for these prerequisites:' -ForegroundColor Yellow
        foreach ($item in $manual) { Write-Host "  - $($item.Name): $($item.Manual)" }
        exit 1
    }

    $answer = Read-Host "Install/upgrade $($auto.Count) prerequisite(s) now? [y/N]"
    if ($answer -notmatch '^[yY]') {
        Write-Host 'No changes made. Install the prerequisites before building.' -ForegroundColor Yellow
        exit 1
    }

    Install-Missing -Missing $auto

    Write-Host ''
    Write-Host 'Re-checking after installation...' -ForegroundColor Cyan
    $checks = Invoke-Checks
    Show-Checks -Checks $checks
    Show-Warnings -Checks $checks
    $stillMissing = @($checks | Where-Object { -not $_.OK })
    if ($stillMissing.Count -eq 0) {
        Write-Host 'All prerequisites are satisfied. You may need a new shell for PATH changes.' -ForegroundColor Green
        $exitCode = 0
    } else {
        foreach ($item in $stillMissing) {
            if ($item.Manual) { Write-Host "  $($item.Name): $($item.Manual)" -ForegroundColor Yellow }
        }
    }
    exit $exitCode
} finally {
    if (-not $NoPause -and -not $CheckOnly) {
        Write-Host ''
        try { $null = Read-Host 'Press Enter to exit' } catch { }
    }
}
