#requires -Version 7.0
<#
.SYNOPSIS
    Correlates the Windhawk mod log with the per-tab request files.

.DESCRIPTION
    One command to see, for every request-<HWND>.txt, the decoded flags, the
    query/filter fields and the recent log events for that same window. It also
    flags the combinations that are known to break searches (for example the
    scope bit set with an empty folder, which produces parent:"").

.PARAMETER LogLines
    Maximum number of matching log lines to print in the tail (default 60).

.PARAMETER Raw
    Print the raw log tail instead of the filtered/correlated view.

.PARAMETER NoLog
    Only show request files, skip log parsing.
#>
[CmdletBinding()]
param(
    [int]$LogLines = 60,
    [switch]$Raw,
    [switch]$NoLog
)

$ErrorActionPreference = 'Stop'
$requestDir = Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) 'EverythingSearchExplorer'
$logPath = Join-Path $env:TEMP 'everything-search-explorer.log'

function Get-FlagDescription {
    param([int]$Flags)
    $parts = @()
    if ($Flags -band 1) { $parts += 'scope' }
    if ($Flags -band 2) { $parts += 'folders' }
    if ($Flags -band 4) { $parts += 'recursive' }
    if ($parts.Count -eq 0) { $parts += 'none' }
    if (($Flags -band 1) -and ($Flags -band 4)) {
        $scope = 'ALL SUBFOLDERS  (path:...)'
    } elseif ($Flags -band 1) {
        $scope = 'CURRENT FOLDER ONLY  (parent:...)'
    } else {
        $scope = 'GLOBAL'
    }
    return ('0x{0:X} [{1}] -> {2}' -f $Flags, ($parts -join ', '), $scope)
}

function Read-Request {
    param([System.IO.FileInfo]$File)
    $fields = @{}
    foreach ($line in [System.IO.File]::ReadAllLines($File.FullName)) {
        $eq = $line.IndexOf('=')
        if ($eq -le 0) { continue }
        $key = $line.Substring(0, $eq).TrimStart([char]0xFEFF)
        $fields[$key] = $line.Substring($eq + 1)
    }
    $flags = 0
    [void][int]::TryParse($fields['flags'], [ref]$flags)
    $content = 0
    [void][int]::TryParse($fields['content'], [ref]$content)
    return [pscustomobject]@{
        File      = $File.Name
        HwndDec   = if ($File.Name -match 'request-(\d+)\.txt') { $matches[1] } else { $null }
        Seq       = $fields['seq']
        Max       = $fields['max']
        Flags     = $flags
        FlagsText = Get-FlagDescription -Flags $flags
        Folder    = $fields['folder']
        Query     = $fields['query']
        Filter    = $fields['filter']
        Content   = $content
        Modified  = $File.LastWriteTime
    }
}

$patterns = @(
    'C3 Enter', 'writing search request', 'options applied', 'tab switched',
    'current folder', 'BrowseObject', 'LocationURL', 'FAIL', 'ABORT', 'error',
    'NOT IMPLEMENTED', 'no filesystem path'
)
$matchedLines = @()
if (-not $Raw) {
    $allLines = @(Get-Content -LiteralPath $logPath -ErrorAction SilentlyContinue)
    foreach ($line in $allLines) {
        foreach ($p in $patterns) {
            if ($line -match [regex]::Escape($p)) { $matchedLines += $line; break }
        }
    }
}

if ($Raw) {
    Write-Host "=== RAW LOG TAIL: $logPath ==="
    Get-Content -LiteralPath $logPath -Tail $LogLines -ErrorAction SilentlyContinue
    return
}

Write-Host '=== REQUEST FILES ==='
$requests = @()
if (Test-Path -LiteralPath $requestDir) {
    $requests = @(Get-ChildItem -LiteralPath $requestDir -Filter 'request*.txt' -File |
        Sort-Object LastWriteTime | ForEach-Object { Read-Request -File $_ })
}
if ($requests.Count -eq 0) {
    Write-Host '(none)'
} else {
    foreach ($r in $requests) {
        $hex = if ($r.HwndDec) { '{0:X}' -f [int64]$r.HwndDec } else { 'global' }
        Write-Host ''
        Write-Host ("--- {0}   hwnd={1}   modified={2:HH:mm:ss}" -f $r.File, $hex, $r.Modified)
        Write-Host ("    seq={0}  max={1}  flags={2}" -f $r.Seq, $r.Max, $r.FlagsText)
        Write-Host ("    folder='{0}'" -f $r.Folder)
        Write-Host ("    query='{0}'" -f $r.Query)
        Write-Host ("    filter='{0}'  content={1}" -f $r.Filter, $r.Content)

        $problems = @()
        if (($r.Flags -band 1) -and [string]::IsNullOrWhiteSpace($r.Folder)) {
            $problems += 'BROKEN SCOPE: scope bit set but folder is empty -> parent:"" matches nothing'
        }
        if (($r.Flags -band 4) -and -not ($r.Flags -band 1)) {
            $problems += 'recursive bit set without scope: ignored by the NSE'
        }
        if ($r.Content -eq 1 -and [string]::IsNullOrWhiteSpace($r.Query)) {
            $problems += 'content search enabled with empty query'
        }
        foreach ($p in $problems) { Write-Host ("    !! {0}" -f $p) -ForegroundColor Yellow }

        if (-not $NoLog -and $matchedLines.Count -gt 0) {
            $key = $hex
            $events = @($matchedLines | Where-Object {
                $_ -match ('frame=0*' + [regex]::Escape($key)) -or
                $_ -match ('key=0*' + [regex]::Escape($key)) -or
                $_ -match ('request-' + [regex]::Escape($r.HwndDec))
            } | Select-Object -Last 8)
            if ($events.Count -gt 0) {
                Write-Host '    log events:'
                foreach ($e in $events) { Write-Host ("      {0}" -f $e) }
            }
        }
    }
}

if (-not $NoLog) {
    Write-Host ''
    Write-Host ("=== LOG EVENTS (last {0} of {1} matching lines) ===" -f $LogLines, $matchedLines.Count)
    $matchedLines | Select-Object -Last $LogLines | ForEach-Object { Write-Host $_ }
}

