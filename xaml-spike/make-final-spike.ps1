#requires -Version 7.0
# Promotes the validated rollback probe into the final spike.cpp source.
# Removes the temporary crash-loop safe mode and noisy debug logs.

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$source = Join-Path $PSScriptRoot 'rollback-probe.cpp'
$target = Join-Path $PSScriptRoot 'spike.cpp'

$text = [IO.File]::ReadAllText($source)

function Replace-Once([string]$haystack, [string]$old, [string]$new, [string]$label) {
    $old = $old.Replace("`n", "`r`n")
    $new = $new.Replace("`n", "`r`n")
    if (-not $haystack.Contains($old)) { throw "missing $label" }
    return $haystack.Replace($old, $new)
}

$text = Replace-Once $text 'L"esx-rollback-probe.log"' 'L"esx-xaml-spike.log"' 'log path'

$text = Replace-Once $text @'
    // A window opened in an existing shell has a taskbar owned by another
    // process. Only a fresh shell starts without one: that is the restart loop
    // the guard must catch. Ordinary folder windows must never be counted.
    const bool shellStarting = FindWindowW(L"Shell_TrayWnd", nullptr) == nullptr;
    const int recentStarts = shellStarting ? RecentStarts() : 0;
    if (shellStarting) RecordStart();
    if (recentStarts >= kLoopStarts) {
        Log("SAFE MODE: %d shell starts within 120 s, spike disabled",
            recentStarts);
        return TRUE;
    }
'@ '' 'safe mode block'

$text = Replace-Once $text @'
    Log("C3 ENTER state=%p frame=%p done=%d running=%d", state.get(),
        state->frame, state->c2Done.load() ? 1 : 0,
        state->c2Running.load() ? 1 : 0);
'@ '' 'enter debug log'

$text = Replace-Once $text @'
    Log("C3 CLAIMED state=%p frame=%p", state.get(), state->frame);
'@ '' 'claimed debug log'

$text = Replace-Once $text @'
            Log("C3 SCAN state=%p frame=%p gen=%llu watcher=%d probe=%d "
                "ready=%d done=%d running=%d",
                state.get(), state->frame,
                static_cast<unsigned long long>(state->generation),
                state->watcher ? 1 : 0, state->probe ? 1 : 0,
                state->probe && state->probe->ready.load() ? 1 : 0,
                state->c2Done.load() ? 1 : 0, state->c2Running.load() ? 1 : 0);
'@ '' 'scan debug log'

[IO.File]::WriteAllText($target, $text)
Write-Host "created $target"
