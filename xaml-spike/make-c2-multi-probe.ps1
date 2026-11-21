#requires -Version 7.0
# Per-window C2 probe. Derived from the single-window C2 probe.
# The XAML probe is keyed by native handle (never a single global slot),
# attached to WindowState during correlation, and consumed per state.

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$source = Join-Path $PSScriptRoot 'c2-probe.cpp'
$target = Join-Path $PSScriptRoot 'c2-multi-probe.cpp'

$text = [IO.File]::ReadAllText($source)

function Replace-Once([string]$haystack, [string]$old, [string]$new, [string]$label) {
    $old = $old.Replace("`n", "`r`n")
    $new = $new.Replace("`n", "`r`n")
    if (-not $haystack.Contains($old)) { throw "missing $label" }
    return $haystack.Replace($old, $new)
}

$text = Replace-Once $text 'L"esx-c2-probe.log"' 'L"esx-c2-multi.log"' 'log path'

$text = Replace-Once $text @'
class VisualTreeWatcher;

// Phase A: per-window registry.
'@ @'
class VisualTreeWatcher;
struct CallbackProbeResult;

// Phase A: per-window registry.
'@ 'forward declaration'

$text = Replace-Once $text @'
    bool xamlMapped = false;
};
'@ @'
    bool xamlMapped = false;
    DWORD threadId = 0;
    std::shared_ptr<CallbackProbeResult> probe;
    std::atomic<bool> c2Done{false};
    std::atomic<bool> c2Running{false};
};
'@ 'WindowState fields'

$text = Replace-Once $text @'
std::atomic<uint64_t> g_windowGeneration{1};
'@ @'
std::atomic<uint64_t> g_windowGeneration{1};

// Per-window probe slots, keyed by the native XAML handle. No single global
// slot is used to create controls for different windows.
std::mutex g_probeByHandleMutex;
std::unordered_map<unsigned long long, std::shared_ptr<CallbackProbeResult>>
    g_probeByHandle;
'@ 'probe map declaration'

$text = Replace-Once $text @'
CallbackProbeResult g_callbackProbe;
std::atomic<bool> g_callbackProbeReported{false};
'@ @'
CallbackProbeResult g_callbackProbe;
std::atomic<bool> g_callbackProbeReported{false};

std::shared_ptr<CallbackProbeResult> ProbeForHandle(unsigned long long handle) {
    std::lock_guard<std::mutex> guard(g_probeByHandleMutex);
    auto it = g_probeByHandle.find(handle);
    if (it != g_probeByHandle.end()) return it->second;
    auto result = std::make_shared<CallbackProbeResult>();
    g_probeByHandle.emplace(handle, result);
    return result;
}
'@ 'probe helper'

$text = Replace-Once $text @'
            m_probeInProgress.store(false);
        }

        if (element.Handle == g_createdHandle && g_createdHandle != 0) {
'@ @'
            m_probeInProgress.store(false);
        }
        if (!added) {
            std::lock_guard<std::mutex> guard(g_probeByHandleMutex);
            auto it = g_probeByHandle.find(
                static_cast<unsigned long long>(element.Handle));
            if (it != g_probeByHandle.end()) {
                it->second->ready.store(false);
                it->second->handle.store(0);
                g_probeByHandle.erase(it);
            }
        }

        if (element.Handle == g_createdHandle && g_createdHandle != 0) {
'@ 'probe removal'

$text = Replace-Once $text @'
            auto& result = g_callbackProbe;
            if (result.handle.load() !=
'@ @'
            auto probeRef = ProbeForHandle(
                static_cast<unsigned long long>(element.Handle));
            auto& result = *probeRef;
            if (result.handle.load() !=
'@ 'callback probe slot'

$text = Replace-Once $text '!m_probeInProgress.exchange(true)' '!result.inProgress.exchange(true)' 'in-progress guard'

$text = Replace-Once $text @'
                } else {
                    result.chainHr = E_UNEXPECTED;
                    result.ready.store(true);
                }
                m_probeInProgress.store(false);
            }
            return S_OK;
'@ @'
                } else {
                    result.chainHr = E_UNEXPECTED;
                    result.ready.store(true);
                }
                result.inProgress.store(false);
            }
            return S_OK;
'@ 'callback release'

$text = Replace-Once $text @'
    if (message == kOwnerProbeMessage && self && lParam) {
        auto* request = reinterpret_cast<OwnerRequest*>(lParam);
        auto& result = g_callbackProbe;
'@ @'
    if (message == kOwnerProbeMessage && self && lParam) {
        auto* request = reinterpret_cast<OwnerRequest*>(lParam);
        auto probeRef = ProbeForHandle(
            static_cast<unsigned long long>(request->native));
        auto& result = *probeRef;
'@ 'owner probe result slot'

$text = Replace-Once $text @'
        state->watcher = watcherRef;
        state->nativeHandle = element.Handle;
'@ @'
        state->watcher = watcherRef;
        state->threadId = g_observedBoxes.find(key)->second.threadId;
        {
            std::lock_guard<std::mutex> probeGuard(g_probeByHandleMutex);
            auto pit = g_probeByHandle.find(
                static_cast<unsigned long long>(element.Handle));
            if (pit != g_probeByHandle.end()) state->probe = pit->second;
        }
        state->nativeHandle = element.Handle;
'@ 'pending probe attach'

$text = Replace-Once $text @'
            state->frame = frame;
            state->pid = pid;
            state->generation = g_windowGeneration.fetch_add(1);
'@ @'
            state->frame = frame;
            state->pid = pid;
            state->threadId = GetWindowThreadProcessId(frame, nullptr);
            state->generation = g_windowGeneration.fetch_add(1);
'@ 'uia thread id'

$text = Replace-Once $text @'
        if (candidates.size() != 1) {
            ++it;  // ambiguous: leave the window untouched
            continue;
        }
'@ @'
        if (candidates.empty()) {
            ++it;
            continue;
        }
        if (candidates.size() > 1) {
            std::vector<std::shared_ptr<WindowState>> threaded;
            for (const auto& candidate : candidates) {
                if (candidate->threadId == pending->threadId) {
                    threaded.push_back(candidate);
                }
            }
            if (threaded.size() == 1) candidates = threaded;
        }
        if (candidates.size() != 1) {
            ++it;  // ambiguous: leave the window untouched
            continue;
        }
'@ 'correlation by thread'

$text = Replace-Once $text @'
        target->watcher = pending->watcher;
        Log("WINDOW correlated frame=%p pid=%lu gen=%llu native=%llu index=%u",
'@ @'
        target->watcher = pending->watcher;
        target->threadId = pending->threadId;
        {
            std::lock_guard<std::mutex> probeGuard(g_probeByHandleMutex);
            auto pit = g_probeByHandle.find(static_cast<unsigned long long>(
                pending->nativeHandle));
            if (pit != g_probeByHandle.end()) target->probe = pit->second;
        }
        Log("WINDOW correlated frame=%p pid=%lu gen=%llu native=%llu index=%u",
'@ 'correlation transfers probe'

$text = Replace-Once $text @'
                if (g_callbackProbe.ready.load()) {
                    RunC2Probe();
                }
'@ @'
                RunC2ForReadyStates();
'@ 'coordinator call'

$startMarker = 'static std::atomic<bool> g_c2ProbeDone{false};'
$endMarker = 'void TryCreateExperimentTextBox() {'
$start = $text.IndexOf($startMarker)
$end = $text.IndexOf($endMarker)
if ($start -lt 0 -or $end -lt 0 -or $end -le $start) { throw 'c2 function markers not found' }
$oldFunction = $text.Substring($start, $end - $start)

$preambleOld = @'
static std::atomic<bool> g_c2ProbeDone{false};

static void RunC2Probe() {
    if (g_c2ProbeDone.exchange(true)) return;
    auto* watcher = g_probeWatcher;
    if (!watcher || !g_callbackProbe.ready.load()) return;

    const InstanceHandle nativeHandle =
        static_cast<InstanceHandle>(g_callbackProbe.handle.load());
    auto state = FindStateForNative(nativeHandle);
    if (!state || !state->frame || !IsWindow(state->frame)) {
        Log("C2 ABORT: no valid WindowState");
        return;
    }
    const HWND frame = state->frame;
    const DWORD pid = state->pid;
    const uint64_t generation = state->generation;
    const InstanceHandle expectedParent = state->parentHandle;
    const unsigned expectedIndex = state->nativeIndex;

    Log("C2 queued frame=%p pid=%lu gen=%llu native=%llu parent=%llu index=%u "
        "tid=%lu",
        frame, pid, static_cast<unsigned long long>(generation),
        static_cast<unsigned long long>(nativeHandle),
        static_cast<unsigned long long>(expectedParent), expectedIndex,
        GetCurrentThreadId());
'@
$preambleNew = @'
static void RunC2ProbeForState(const std::shared_ptr<WindowState>& state) {
    if (!state || !state->watcher) return;
    Log("C2 ENTER state=%p frame=%p done=%d running=%d", state.get(),
        state->frame, state->c2Done.load() ? 1 : 0,
        state->c2Running.load() ? 1 : 0);
    if (state->c2Done.load() || state->c2Running.exchange(true)) return;
    state->c2Done.store(true);
    Log("C2 CLAIMED state=%p frame=%p", state.get(), state->frame);

    auto probe = state->probe;
    if (!probe || !probe->ready.load()) {
        Log("C2 ABORT frame=%p gen=%llu: probe not ready", state->frame,
            static_cast<unsigned long long>(state->generation));
        state->c2Running.store(false);
        return;
    }
    auto* watcher = state->watcher.get();
    const InstanceHandle nativeHandle =
        static_cast<InstanceHandle>(probe->handle.load());
    if (state->nativeHandle && state->nativeHandle != nativeHandle) {
        Log("C2 ABORT frame=%p gen=%llu: native handle mismatch", state->frame,
            static_cast<unsigned long long>(state->generation));
        state->c2Running.store(false);
        return;
    }
    const HWND frame = state->frame;
    const DWORD pid = state->pid;
    const uint64_t generation = state->generation;
    const InstanceHandle expectedParent = state->parentHandle;
    const unsigned expectedIndex = state->nativeIndex;

    Log("C2 queued frame=%p pid=%lu gen=%llu native=%llu parent=%llu index=%u "
        "tid=%lu",
        frame, pid, static_cast<unsigned long long>(generation),
        static_cast<unsigned long long>(nativeHandle),
        static_cast<unsigned long long>(expectedParent), expectedIndex,
        GetCurrentThreadId());
'@

$preambleOld = $preambleOld.Replace("`n", "`r`n")
$preambleNew = $preambleNew.Replace("`n", "`r`n")
if (-not $oldFunction.Contains($preambleOld)) { throw 'c2 preamble not found' }
$newFunction = $oldFunction.Replace($preambleOld, $preambleNew)
$tailOld = @'
    Log("C2 dispatch hr=0x%08X", static_cast<unsigned long>(dispatchHr));
}

'@
$tailNew = @'
    Log("C2 dispatch hr=0x%08X", static_cast<unsigned long>(dispatchHr));
    state->c2Running.store(false);
}

static void RunC2ForReadyStates() {
    std::vector<std::shared_ptr<WindowState>> targets;
    {
        std::lock_guard<std::mutex> guard(g_windowStatesMutex);
        for (const auto& item : g_windowStates) {
            auto state = item.second;
            Log("C2 SCAN state=%p frame=%p gen=%llu watcher=%d probe=%d "
                "ready=%d done=%d running=%d",
                state.get(), state->frame,
                static_cast<unsigned long long>(state->generation),
                state->watcher ? 1 : 0, state->probe ? 1 : 0,
                state->probe && state->probe->ready.load() ? 1 : 0,
                state->c2Done.load() ? 1 : 0, state->c2Running.load() ? 1 : 0);
            if (state->watcher && state->probe && state->probe->ready.load() &&
                !state->c2Done.load() && !state->c2Running.load()) {
                targets.push_back(state);
            }
        }
    }
    for (auto& state : targets) RunC2ProbeForState(state);
}

'@
$tailOld = $tailOld.Replace("`n", "`r`n")
$tailNew = $tailNew.Replace("`n", "`r`n")
if (-not $newFunction.Contains($tailOld)) { throw 'c2 tail not found' }
$newFunction = $newFunction.Replace($tailOld, $tailNew)

$text = $text.Substring(0, $start) + $newFunction + $text.Substring($end)
[IO.File]::WriteAllText($target, $text)
Write-Host "created $target"
