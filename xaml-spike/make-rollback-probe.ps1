#requires -Version 7.0
# Per-window rollback of C3 replacements on mod unload.
# Derived from the per-window input probe.

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$source = Join-Path $PSScriptRoot 'input-probe.cpp'
$target = Join-Path $PSScriptRoot 'rollback-probe.cpp'

$text = [IO.File]::ReadAllText($source)

function Replace-Once([string]$haystack, [string]$old, [string]$new, [string]$label) {
    $old = $old.Replace("`n", "`r`n")
    $new = $new.Replace("`n", "`r`n")
    if (-not $haystack.Contains($old)) { throw "missing $label" }
    return $haystack.Replace($old, $new)
}

$text = Replace-Once $text 'L"esx-input-probe.log"' 'L"esx-rollback-probe.log"' 'log path'

$rollback = @'
static void RollbackStateOnDispatcher(const std::shared_ptr<WindowState>& state) {
    if (!state || !state->replacementActive || !state->watcher ||
        !state->originalNative || !state->textBoxHandle) {
        return;
    }
    Log("ROLLBACK begin frame=%p gen=%llu tid=%lu", state->frame,
        static_cast<unsigned long long>(state->generation), GetCurrentThreadId());

    winrt::Microsoft::UI::Dispatching::DispatcherQueue resolved{nullptr};
    const HRESULT dispatchHr = state->watcher->RunOnElementDispatcher(
        static_cast<InstanceHandle>(state->textBoxHandle),
        [state]() -> HRESULT {
            auto diagnostics = state->watcher->OwnerDiagnostics();
            auto* service = state->watcher->OwnerService();
            if (!diagnostics || !service) return E_UNEXPECTED;

            winrt::com_ptr<::IInspectable> ourRaw{nullptr};
            HRESULT hr = diagnostics->GetIInspectableFromHandle(
                static_cast<InstanceHandle>(state->textBoxHandle), ourRaw.put());
            if (FAILED(hr) || !ourRaw) {
                Log("ROLLBACK ABORT frame=%p gen=%llu: textbox hr=0x%08X",
                    state->frame,
                    static_cast<unsigned long long>(state->generation),
                    static_cast<unsigned long>(hr));
                return hr;
            }
            winrt::Windows::Foundation::IInspectable ourInspectable{nullptr};
            winrt::copy_from_abi(ourInspectable, ourRaw.get());
            auto ourBox = ourInspectable.as<
                winrt::Microsoft::UI::Xaml::Controls::TextBox>();

            InstanceHandle collection = 0;
            HRESULT collectionHr = E_UNEXPECTED;
            auto parent = winrt::Microsoft::UI::Xaml::Media::VisualTreeHelper::GetParent(
                ourBox);
            if (auto panel =
                    parent.try_as<winrt::Microsoft::UI::Xaml::Controls::Panel>()) {
                auto children = panel.Children();
                winrt::com_ptr<::IInspectable> rawChildren{nullptr};
                rawChildren = children.as<::IInspectable>();
                collectionHr = diagnostics->RegisterInstance(rawChildren.get(),
                                                             &collection);
            }
            if (FAILED(collectionHr) || !collection) return collectionHr;

            unsigned ourIndex = 0;
            const HRESULT ourHr = ServiceFindHandleIndex(
                service, collection,
                static_cast<InstanceHandle>(state->textBoxHandle), &ourIndex);
            HRESULT removeHr = E_UNEXPECTED;
            if (SUCCEEDED(ourHr)) {
                removeHr = service->RemoveChild(collection, ourIndex);
            }
            Log("ROLLBACK remove hr=0x%08X index=%u findHr=0x%08X",
                static_cast<unsigned long>(removeHr), ourIndex,
                static_cast<unsigned long>(ourHr));

            InstanceHandle nativeFresh = 0;
            const HRESULT registerHr = diagnostics->RegisterInstance(
                state->originalNative.get(), &nativeFresh);
            unsigned insertAt = state->nativeIndex;
            unsigned size = 0;
            const HRESULT countHr = service->GetCollectionCount(collection, &size);
            if (SUCCEEDED(countHr) && insertAt > size) insertAt = size;
            HRESULT addHr = E_UNEXPECTED;
            if (SUCCEEDED(registerHr) && nativeFresh) {
                addHr = service->AddChild(collection, nativeFresh, insertAt);
            }
            unsigned nativeIndex = 0;
            const HRESULT nativeHr = ServiceFindHandleIndex(
                service, collection, nativeFresh, &nativeIndex);
            unsigned goneIndex = 0;
            const HRESULT goneHr = ServiceFindHandleIndex(
                service, collection,
                static_cast<InstanceHandle>(state->textBoxHandle), &goneIndex);
            const bool ok = SUCCEEDED(removeHr) && SUCCEEDED(registerHr) &&
                            SUCCEEDED(addHr) && SUCCEEDED(nativeHr) &&
                            FAILED(goneHr);
            if (ok) {
                state->replacementActive = false;
                state->nativeHandle = nativeFresh;
                state->phase = WindowPhase::XamlMapped;
            }
            Log("ROLLBACK RESULT %s ok=%d frame=%p gen=%llu removeHr=0x%08X "
                "registerHr=0x%08X addHr=0x%08X nativeHr=0x%08X goneHr=0x%08X "
                "tid=%lu",
                ok ? "PASS" : "FAIL", ok ? 1 : 0, state->frame,
                static_cast<unsigned long long>(state->generation),
                static_cast<unsigned long>(removeHr),
                static_cast<unsigned long>(registerHr),
                static_cast<unsigned long>(addHr),
                static_cast<unsigned long>(nativeHr),
                static_cast<unsigned long>(goneHr), GetCurrentThreadId());
            return ok ? S_OK : E_FAIL;
        },
        &resolved);
    Log("ROLLBACK dispatch frame=%p gen=%llu hr=0x%08X", state->frame,
        static_cast<unsigned long long>(state->generation),
        static_cast<unsigned long>(dispatchHr));
}

static void RollbackAllReplacements() {
    std::vector<std::shared_ptr<WindowState>> targets;
    {
        std::lock_guard<std::mutex> guard(g_windowStatesMutex);
        for (const auto& item : g_windowStates) {
            if (item.second->replacementActive) targets.push_back(item.second);
        }
    }
    for (auto& state : targets) RollbackStateOnDispatcher(state);
}

'@
$text = Replace-Once $text 'extern "C" void Wh_ModUninit() {' ($rollback + 'extern "C" void Wh_ModUninit() {') 'rollback functions'

$text = Replace-Once $text @'
    if (g_replacementActive.load()) RestoreReplacement(true);
'@ @'
    RollbackAllReplacements();
'@ 'uninit rollback call'

[IO.File]::WriteAllText($target, $text)
Write-Host "created $target"
