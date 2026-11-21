#requires -Version 7.0
# Read-only dispatcher investigation. Derived from the per-window C2 probe.
# Resolves the DispatcherQueue per XAML element instead of using the single
# cached queue on the watcher. No AddChild/RemoveChild, no native removal.

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$source = Join-Path $PSScriptRoot 'c2-multi-probe.cpp'
$target = Join-Path $PSScriptRoot 'disp-probe.cpp'

$text = [IO.File]::ReadAllText($source)

function Replace-Once([string]$haystack, [string]$old, [string]$new, [string]$label) {
    $old = $old.Replace("`n", "`r`n")
    $new = $new.Replace("`n", "`r`n")
    if (-not $haystack.Contains($old)) { throw "missing $label" }
    return $haystack.Replace($old, $new)
}

$text = Replace-Once $text 'L"esx-c2-multi.log"' 'L"esx-disp-probe.log"' 'log path'

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
            Log("DISP resolved element=%llu abi=%p tid=%lu hasAccess=%d "
                "ownerTid=%lu",
                static_cast<unsigned long long>(element),
                winrt::get_abi(dispatcher), GetCurrentThreadId(),
                dispatcher.HasThreadAccess() ? 1 : 0, m_ownerThreadId);

            if (dispatcher.HasThreadAccess()) return action();

            struct DispatchProbeState {
                DispatchProbeState()
                    : done(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}
                ~DispatchProbeState() {
                    if (done) CloseHandle(done);
                }
                HANDLE done = nullptr;
                std::atomic<HRESULT> result{E_PENDING};
            };
            auto state = std::make_shared<DispatchProbeState>();
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

$text = Replace-Once $text '                RunC2ForReadyStates();' '                RunDispatcherProbes();' 'coordinator call'

$startMarker = 'static void RunC2ProbeForState(const std::shared_ptr<WindowState>& state) {'
$endMarker = 'void TryCreateExperimentTextBox() {'
$start = $text.IndexOf($startMarker)
$end = $text.IndexOf($endMarker)
if ($start -lt 0 -or $end -lt 0 -or $end -le $start) { throw 'c2 block markers not found' }

$probe = @'
static void RunDispatcherProbeForState(const std::shared_ptr<WindowState>& state) {
    if (!state || !state->watcher) return;
    if (state->c2Done.load() || state->c2Running.exchange(true)) return;
    state->c2Done.store(true);

    auto probe = state->probe;
    if (!probe || !probe->ready.load()) {
        Log("DISP ABORT frame=%p gen=%llu: probe not ready", state->frame,
            static_cast<unsigned long long>(state->generation));
        state->c2Running.store(false);
        return;
    }
    const InstanceHandle nativeHandle =
        static_cast<InstanceHandle>(probe->handle.load());
    const DWORD callbackTid = probe->threadId;
    Log("DISP queued frame=%p gen=%llu pid=%lu native=%llu callbackTid=%lu",
        state->frame, static_cast<unsigned long long>(state->generation),
        state->pid, static_cast<unsigned long long>(nativeHandle), callbackTid);

    winrt::Microsoft::UI::Dispatching::DispatcherQueue resolved{nullptr};
    const HRESULT dispatchHr = state->watcher->RunOnElementDispatcher(
        nativeHandle,
        [state, nativeHandle, callbackTid]() -> HRESULT {
            auto diagnostics = state->watcher->OwnerDiagnostics();
            auto* service = state->watcher->OwnerService();
            if (!diagnostics || !service) {
                Log("DISP ABORT frame=%p gen=%llu: no service", state->frame,
                    static_cast<unsigned long long>(state->generation));
                return E_UNEXPECTED;
            }
            const DWORD dispatchTid = GetCurrentThreadId();
            Log("DISP begin frame=%p gen=%llu dispatchTid=%lu service=%p",
                state->frame, static_cast<unsigned long long>(state->generation),
                dispatchTid, service);

            winrt::com_ptr<::IInspectable> nativeRaw{nullptr};
            HRESULT hr = diagnostics->GetIInspectableFromHandle(nativeHandle,
                                                                nativeRaw.put());
            if (FAILED(hr) || !nativeRaw) {
                Log("DISP ABORT frame=%p gen=%llu: native hr=0x%08X",
                    state->frame,
                    static_cast<unsigned long long>(state->generation),
                    static_cast<unsigned long>(hr));
                return hr;
            }
            winrt::Windows::Foundation::IInspectable nativeInspectable{nullptr};
            winrt::copy_from_abi(nativeInspectable, nativeRaw.get());
            auto element = nativeInspectable.as<
                winrt::Microsoft::UI::Xaml::FrameworkElement>();

            auto root = element;
            for (int i = 0; i < 64; ++i) {
                auto parent =
                    winrt::Microsoft::UI::Xaml::Media::VisualTreeHelper::GetParent(root);
                if (!parent) break;
                auto parentElement =
                    parent.try_as<winrt::Microsoft::UI::Xaml::FrameworkElement>();
                if (!parentElement) break;
                root = parentElement;
            }
            InstanceHandle rootHandle = 0;
            winrt::com_ptr<::IInspectable> rootRaw{nullptr};
            rootRaw = root.as<::IInspectable>();
            const HRESULT rootHr = diagnostics->GetHandleFromIInspectable(
                rootRaw.get(), &rootHandle);

            InstanceHandle collection = 0;
            HRESULT collectionHr = E_UNEXPECTED;
            auto parent = winrt::Microsoft::UI::Xaml::Media::VisualTreeHelper::GetParent(
                element);
            if (auto panel =
                    parent.try_as<winrt::Microsoft::UI::Xaml::Controls::Panel>()) {
                auto children = panel.Children();
                winrt::com_ptr<::IInspectable> rawChildren{nullptr};
                rawChildren = children.as<::IInspectable>();
                collectionHr = diagnostics->RegisterInstance(rawChildren.get(),
                                                             &collection);
            }
            unsigned count = 0;
            HRESULT countHr = E_UNEXPECTED;
            if (SUCCEEDED(collectionHr) && collection) {
                countHr = service->GetCollectionCount(collection, &count);
            }
            Log("DISP RESULT frame=%p gen=%llu root=%llu rootHr=0x%08X "
                "service=%p collection=%llu collHr=0x%08X countHr=0x%08X "
                "count=%u dispatchTid=%lu callbackTid=%lu",
                state->frame,
                static_cast<unsigned long long>(state->generation),
                static_cast<unsigned long long>(rootHandle),
                static_cast<unsigned long>(rootHr), service,
                static_cast<unsigned long long>(collection),
                static_cast<unsigned long>(collectionHr),
                static_cast<unsigned long>(countHr), count, dispatchTid,
                callbackTid);
            return S_OK;
        },
        &resolved);
    if (resolved) {
        state->dispatcher = winrt::make_agile(resolved);
    }
    Log("DISP dispatch frame=%p gen=%llu hr=0x%08X", state->frame,
        static_cast<unsigned long long>(state->generation),
        static_cast<unsigned long>(dispatchHr));
    state->c2Running.store(false);
}

static void RunDispatcherProbes() {
    std::vector<std::shared_ptr<WindowState>> targets;
    {
        std::lock_guard<std::mutex> guard(g_windowStatesMutex);
        for (const auto& item : g_windowStates) {
            auto state = item.second;
            if (state->watcher && state->probe && state->probe->ready.load() &&
                !state->c2Done.load() && !state->c2Running.load()) {
                targets.push_back(state);
            }
        }
    }
    for (auto& state : targets) RunDispatcherProbeForState(state);
}

'@
$probe = $probe.Replace("`n", "`r`n")
$text = $text.Substring(0, $start) + $probe + $text.Substring($end)
[IO.File]::WriteAllText($target, $text)
Write-Host "created $target"
