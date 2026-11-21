#requires -Version 7.0
# C2 transactions executed on the dispatcher resolved per XAML element.
# Derived from the per-window C2 probe; replaces the cached watcher dispatcher.

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$source = Join-Path $PSScriptRoot 'c2-multi-probe.cpp'
$target = Join-Path $PSScriptRoot 'c2-disp-probe.cpp'

$text = [IO.File]::ReadAllText($source)

function Replace-Once([string]$haystack, [string]$old, [string]$new, [string]$label) {
    $old = $old.Replace("`n", "`r`n")
    $new = $new.Replace("`n", "`r`n")
    if (-not $haystack.Contains($old)) { throw "missing $label" }
    return $haystack.Replace($old, $new)
}

$text = Replace-Once $text 'L"esx-c2-multi.log"' 'L"esx-c2-disp.log"' 'log path'

$text = Replace-Once $text @'
    std::atomic<bool> c2Running{false};
};
'@ @'
    std::atomic<bool> c2Running{false};
    winrt::agile_ref<winrt::Microsoft::UI::Dispatching::DispatcherQueue> dispatcher;
};
'@ 'dispatcher field'

$text = Replace-Once $text @'
    HRESULT RunOnDispatcher(InstanceHandle element,
                            std::function<HRESULT()> action) {
        return RunOnTargetDispatcher(element, std::move(action));
    }
'@ @'
    HRESULT RunOnDispatcher(InstanceHandle element,
                            std::function<HRESULT()> action) {
        return RunOnTargetDispatcher(element, std::move(action));
    }

    // Resolve the dispatcher queue for THIS element every time. The legacy
    // RunOnTargetDispatcher caches one queue per watcher, which is wrong when
    // a process hosts several XAML trees with distinct dispatchers.
    HRESULT RunOnElementDispatcher(
        InstanceHandle element,
        std::function<HRESULT()> action,
        winrt::Microsoft::UI::Dispatching::DispatcherQueue* resolved = nullptr) {
        try {
            auto hooks = m_testHooks.get();
            if (!hooks) return E_NOINTERFACE;
            winrt::com_ptr<::IInspectable> queueObject;
            const HRESULT resolveHr = hooks->TryGetDispatcherQueueForObject(
                element, queueObject.put());
            if (FAILED(resolveHr)) return resolveHr;
            if (!queueObject) return E_UNEXPECTED;
            auto dispatcher =
                queueObject.as<winrt::Microsoft::UI::Dispatching::DispatcherQueue>();
            if (resolved) *resolved = dispatcher;
            Log("CDISP resolved element=%llu abi=%p tid=%lu hasAccess=%d "
                "ownerTid=%lu",
                static_cast<unsigned long long>(element),
                winrt::get_abi(dispatcher), GetCurrentThreadId(),
                dispatcher.HasThreadAccess() ? 1 : 0, m_ownerThreadId);

            if (dispatcher.HasThreadAccess()) return action();

            struct DispatchC2State {
                DispatchC2State()
                    : done(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}
                ~DispatchC2State() {
                    if (done) CloseHandle(done);
                }
                HANDLE done = nullptr;
                std::atomic<HRESULT> result{E_PENDING};
            };
            auto state = std::make_shared<DispatchC2State>();
            if (!state->done) return HRESULT_FROM_WIN32(GetLastError());

            AddRef();
            bool enqueued = false;
            try {
                enqueued = dispatcher.TryEnqueue([this, state, action]() {
                    HRESULT taskHr = E_ABORT;
                    try {
                        taskHr = action();
                    } catch (...) {
                        taskHr = winrt::to_hresult();
                    }
                    state->result = taskHr;
                    SetEvent(state->done);
                    Release();
                });
            } catch (...) {
                Release();
                return winrt::to_hresult();
            }
            if (!enqueued) {
                Release();
                return E_ABORT;
            }
            const DWORD wait = WaitForSingleObject(state->done, 10000);
            return wait == WAIT_OBJECT_0 ? state->result.load()
                                         : HRESULT_FROM_WIN32(WAIT_TIMEOUT);
        } catch (...) {
            return winrt::to_hresult();
        }
    }
'@ 'per-element dispatcher method'

$text = Replace-Once $text @'
    const HRESULT dispatchHr = watcher->RunOnDispatcher(
        nativeHandle,
'@ @'
    winrt::Microsoft::UI::Dispatching::DispatcherQueue resolved{nullptr};
    const HRESULT dispatchHr = watcher->RunOnElementDispatcher(
        nativeHandle,
'@ 'dispatch call'

$text = Replace-Once $text @'
        });
    Log("C2 dispatch hr=0x%08X", static_cast<unsigned long>(dispatchHr));
'@ @'
        },
        &resolved);
    if (resolved) state->dispatcher = winrt::make_agile(resolved);
    Log("C2 dispatch hr=0x%08X", static_cast<unsigned long>(dispatchHr));
'@ 'dispatch call tail'

$text = Replace-Once $text @'
            const DWORD tid = GetCurrentThreadId();
            Log("C2 begin tid=%lu frame=%p pid=%lu gen=%llu native=%llu",
                tid, frame, pid, static_cast<unsigned long long>(generation),
                static_cast<unsigned long long>(nativeHandle));

            auto diagnostics = watcher->OwnerDiagnostics();
            auto* service = watcher->OwnerService();
            if (!diagnostics || !service) {
                Log("C2 ABORT: missing XAML diagnostics/service");
                return E_UNEXPECTED;
            }
'@ @'
            const DWORD tid = GetCurrentThreadId();
            auto diagnostics = watcher->OwnerDiagnostics();
            auto* service = watcher->OwnerService();
            if (!diagnostics || !service) {
                Log("C2 ABORT: missing XAML diagnostics/service");
                return E_UNEXPECTED;
            }
            Log("C2 begin tid=%lu frame=%p pid=%lu gen=%llu native=%llu "
                "service=%p",
                tid, frame, pid, static_cast<unsigned long long>(generation),
                static_cast<unsigned long long>(nativeHandle), service);
'@ 'service identity log'

$text = Replace-Once $text @'
            Log("C2 RESULT %s addVerified=%d restored=%d removeHr=0x%08X "
                "frame=%p gen=%llu tid=%lu",
                (addVerified && restored && SUCCEEDED(removeHr)) ? "PASS" : "FAIL",
                addVerified ? 1 : 0, restored ? 1 : 0,
                static_cast<unsigned long>(removeHr), frame,
                static_cast<unsigned long long>(generation), tid);
'@ @'
            Log("C2 RESULT %s addVerified=%d restored=%d removeHr=0x%08X "
                "frame=%p gen=%llu tid=%lu service=%p collection=%llu",
                (addVerified && restored && SUCCEEDED(removeHr)) ? "PASS" : "FAIL",
                addVerified ? 1 : 0, restored ? 1 : 0,
                static_cast<unsigned long>(removeHr), frame,
                static_cast<unsigned long long>(generation), tid, service,
                static_cast<unsigned long long>(collection));
'@ 'result identity log'

[IO.File]::WriteAllText($target, $text)
Write-Host "created $target"
