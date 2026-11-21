#requires -Version 7.0
<#
.SYNOPSIS
    Types a query into Explorer's native search box and reports what the mod
    did, without pressing Enter (live-search smoke test).

.PARAMETER Folder
    Folder to open in a new Explorer window.

.PARAMETER Query
    Text to type. Only ASCII letters/digits are supported (keybd_event VKs).
#>
[CmdletBinding()]
param(
    [string]$Folder = (Get-Location).Path,
    [string]$Query = 'readme',
    [switch]$Enter
)

$ErrorActionPreference = 'Stop'

Add-Type -Namespace ESXTEST -Name W -MemberDefinition @'
public delegate bool EnumProc(IntPtr h, IntPtr p);
[DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr p);
[DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, System.Text.StringBuilder sb, int max);
[DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr h, System.Text.StringBuilder sb, int max);
[DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
[DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
[DllImport("user32.dll")] public static extern bool BringWindowToTop(IntPtr h);
[DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
[DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, IntPtr pid);
[DllImport("kernel32.dll")] public static extern uint GetCurrentThreadId();
[DllImport("user32.dll")] public static extern bool AttachThreadInput(uint a, uint b, bool attach);
[DllImport("user32.dll")] public static extern void SwitchToThisWindow(IntPtr h, bool altTab);
[DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, IntPtr extra);
'@

Start-Process explorer.exe "/n,`"$Folder`""
Start-Sleep -Seconds 3

$rows = New-Object System.Collections.Generic.List[object]
$callback = [ESXTEST.W+EnumProc] {
    param($hwnd, $param)
    $class = New-Object System.Text.StringBuilder 128
    [void][ESXTEST.W]::GetClassName($hwnd, $class, 128)
    if ($class.ToString() -eq 'CabinetWClass') {
        $title = New-Object System.Text.StringBuilder 256
        [void][ESXTEST.W]::GetWindowText($hwnd, $title, 256)
        if ($title.ToString() -like "$Folder*") {
            $rows.Add([pscustomobject]@{ H = $hwnd; T = $title.ToString() })
        }
    }
    return $true
}
[void][ESXTEST.W]::EnumWindows($callback, [IntPtr]::Zero)
if (-not $rows) { throw "No Explorer window for $Folder" }
$frame = [IntPtr](($rows | Select-Object -First 1).H)
Write-Host "frame=$frame"

[void][ESXTEST.W]::ShowWindow($frame, 9)
Start-Sleep -Milliseconds 300
[ESXTEST.W]::SwitchToThisWindow($frame, $true)
Start-Sleep -Milliseconds 250
for ($i = 0; $i -lt 8 -and [ESXTEST.W]::GetForegroundWindow() -ne $frame; $i++) {
    $fg = [ESXTEST.W]::GetForegroundWindow()
    $fgThread = [ESXTEST.W]::GetWindowThreadProcessId($fg, [IntPtr]::Zero)
    $myThread = [ESXTEST.W]::GetCurrentThreadId()
    [void][ESXTEST.W]::AttachThreadInput($myThread, $fgThread, $true)
    [ESXTEST.W]::keybd_event(0x12, 0, 0, [IntPtr]::Zero)   # Alt unlocks the foreground lock
    [ESXTEST.W]::keybd_event(0x12, 0, 2, [IntPtr]::Zero)
    [void][ESXTEST.W]::BringWindowToTop($frame)
    [void][ESXTEST.W]::SetForegroundWindow($frame)
    [void][ESXTEST.W]::AttachThreadInput($myThread, $fgThread, $false)
    Start-Sleep -Milliseconds 500
}
if ([ESXTEST.W]::GetForegroundWindow() -ne $frame) {
    throw "Could not focus Explorer (foreground=$([ESXTEST.W]::GetForegroundWindow()), want=$frame)"
}

# Ctrl+E focuses Explorer's native search box.
[ESXTEST.W]::keybd_event(0x11, 0, 0, [IntPtr]::Zero); Start-Sleep -Milliseconds 120
[ESXTEST.W]::keybd_event(0x45, 0, 0, [IntPtr]::Zero); Start-Sleep -Milliseconds 80
[ESXTEST.W]::keybd_event(0x45, 0, 2, [IntPtr]::Zero); Start-Sleep -Milliseconds 120
[ESXTEST.W]::keybd_event(0x11, 0, 2, [IntPtr]::Zero); Start-Sleep -Milliseconds 400

foreach ($character in $Query.ToCharArray()) {
    $vk = [byte][char]::ToUpper($character)
    [ESXTEST.W]::keybd_event($vk, 0, 0, [IntPtr]::Zero); Start-Sleep -Milliseconds 70
    [ESXTEST.W]::keybd_event($vk, 0, 2, [IntPtr]::Zero); Start-Sleep -Milliseconds 70
}
Start-Sleep -Milliseconds 500
if ($Enter) {
    [ESXTEST.W]::keybd_event(0x0D, 0, 0, [IntPtr]::Zero); Start-Sleep -Milliseconds 80
    [ESXTEST.W]::keybd_event(0x0D, 0, 2, [IntPtr]::Zero)
}
Start-Sleep -Milliseconds 2000

$title = New-Object System.Text.StringBuilder 256
[void][ESXTEST.W]::GetWindowText($frame, $title, 256)
Write-Host "title=$($title.ToString())"
Write-Host '--- request.txt ---'
Get-Content "$env:LOCALAPPDATA\EverythingExplorerSearch\request.txt"
Write-Host '--- search box after navigation ---'
& (Join-Path $PSScriptRoot '..\build_check\focus_probe.exe') $frame.ToInt64() |
    Select-String -Pattern 'GetText|focus='
Write-Host '--- mod trace (last 8 entries) ---'
Get-Content "$env:TEMP\everything-explorer-search.log" -Tail 8 -ErrorAction SilentlyContinue
