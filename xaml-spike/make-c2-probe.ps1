#requires -Version 7.0
# Builds the C1/C2 mutation probe from the read-only service probe source.
# C2 adds our TextBox, verifies it, removes it, verifies restoration.
# The native control is never touched.

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$source = Join-Path $PSScriptRoot 'service-probe.cpp'
$target = Join-Path $PSScriptRoot 'c2-probe.cpp'

$text = [IO.File]::ReadAllText($source)
$text = $text.Replace('L"esx-svc-probe.log"', 'L"esx-c2-probe.log"')

$startMarker = 'static std::atomic<bool> g_serviceProbeDone{false};'
$endMarker = 'void TryCreateExperimentTextBox() {'
$start = $text.IndexOf($startMarker)
$end = $text.IndexOf($endMarker)
if ($start -lt 0 -or $end -lt 0 -or $end -le $start) { throw 'probe markers not found' }

$probe = @'
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

    const HRESULT dispatchHr = watcher->RunOnDispatcher(
        nativeHandle,
        [watcher, nativeHandle, frame, pid, generation, expectedParent,
         expectedIndex, state]() -> HRESULT {
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

            // C1.1/C1.2: re-acquire the native element and verify identity.
            winrt::com_ptr<::IInspectable> nativeRaw{nullptr};
            HRESULT hr = diagnostics->GetIInspectableFromHandle(nativeHandle,
                                                                nativeRaw.put());
            if (FAILED(hr) || !nativeRaw) {
                Log("C2 ABORT: native handle invalid hr=0x%08X",
                    static_cast<unsigned long>(hr));
                return hr;
            }
            winrt::Windows::Foundation::IInspectable nativeInspectable{nullptr};
            winrt::copy_from_abi(nativeInspectable, nativeRaw.get());
            auto element = nativeInspectable.as<
                winrt::Microsoft::UI::Xaml::FrameworkElement>();

            // C1.3: acquire Panel.Children inside this dispatch only.
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
            Log("C2 collection hr=0x%08X handle=%llu tid=%lu",
                static_cast<unsigned long>(collectionHr),
                static_cast<unsigned long long>(collection), tid);
            if (FAILED(collectionHr) || !collection) return collectionHr;

            // C1.6: verify collection and native element before any mutation.
            unsigned beforeSize = 0;
            const HRESULT countHr = service->GetCollectionCount(collection,
                                                                &beforeSize);
            unsigned nativeIndex = 0;
            const HRESULT nativeHr = ServiceFindHandleIndex(
                service, collection, nativeHandle, &nativeIndex);
            Log("C2 before countHr=0x%08X size=%u nativeHr=0x%08X index=%u",
                static_cast<unsigned long>(countHr), beforeSize,
                static_cast<unsigned long>(nativeHr), nativeIndex);
            const std::wstring before = ServiceSnapshot(service, collection);
            Log("C2 snapshot before %ls", before.c_str());
            if (FAILED(countHr) || FAILED(nativeHr) ||
                beforeSize < 1 || nativeIndex >= beforeSize ||
                (expectedIndex != 0 && nativeIndex != expectedIndex)) {
                Log("C2 ABORT: collection or native identity changed");
                return E_ABORT;
            }

            // C1.4/C1.5: create the TextBox and register it in this dispatch.
            auto box = winrt::Microsoft::UI::Xaml::Controls::TextBox();
            box.PlaceholderText(L"Cerca con Everything");
            box.Visibility(winrt::Microsoft::UI::Xaml::Visibility::Visible);
            box.IsEnabled(true);
            box.IsTabStop(true);
            box.IsHitTestVisible(true);
            box.Width(element.Width());
            box.Height(element.Height());
            box.MinWidth(element.MinWidth());
            box.MaxWidth(element.MaxWidth());
            box.MinHeight(element.MinHeight());
            box.MaxHeight(element.MaxHeight());
            box.Margin(element.Margin());
            box.HorizontalAlignment(element.HorizontalAlignment());
            box.VerticalAlignment(element.VerticalAlignment());
            box.FlowDirection(element.FlowDirection());
            winrt::Microsoft::UI::Xaml::Controls::Grid::SetRow(
                box, winrt::Microsoft::UI::Xaml::Controls::Grid::GetRow(element));
            winrt::Microsoft::UI::Xaml::Controls::Grid::SetColumn(
                box, winrt::Microsoft::UI::Xaml::Controls::Grid::GetColumn(element));
            winrt::Microsoft::UI::Xaml::Controls::Grid::SetRowSpan(
                box, winrt::Microsoft::UI::Xaml::Controls::Grid::GetRowSpan(element));
            winrt::Microsoft::UI::Xaml::Controls::Grid::SetColumnSpan(
                box, winrt::Microsoft::UI::Xaml::Controls::Grid::GetColumnSpan(element));

            winrt::com_ptr<::IInspectable> boxRaw{nullptr};
            boxRaw = box.as<::IInspectable>();
            InstanceHandle created = 0;
            const HRESULT regHr = diagnostics->RegisterInstance(boxRaw.get(),
                                                                &created);
            Log("C2 RegisterInstance textbox hr=0x%08X handle=%llu",
                static_cast<unsigned long>(regHr),
                static_cast<unsigned long long>(created));
            if (FAILED(regHr) || !created) return regHr;

            // C2.1: AddChild on the dispatcher, native untouched.
            const unsigned insertAt = nativeIndex + 1;
            const HRESULT addHr = service->AddChild(collection, created, insertAt);
            Log("C2 AddChild hr=0x%08X index=%u tid=%lu",
                static_cast<unsigned long>(addHr), insertAt, tid);
            if (FAILED(addHr)) {
                Log("C2 RESULT FAIL (AddChild)");
                return addHr;
            }

            // C2.2/C2.3: verify count and our element identity.
            unsigned afterSize = 0;
            const HRESULT afterCountHr = service->GetCollectionCount(collection,
                                                                     &afterSize);
            unsigned ourIndex = 0;
            const HRESULT ourHr = ServiceFindHandleIndex(service, collection,
                                                         created, &ourIndex);
            unsigned nativeAfterIndex = 0;
            const HRESULT nativeAfterHr = ServiceFindHandleIndex(
                service, collection, nativeHandle, &nativeAfterIndex);
            Log("C2 verify countHr=0x%08X size=%u ourHr=0x%08X ourIndex=%u "
                "nativeHr=0x%08X nativeIndex=%u",
                static_cast<unsigned long>(afterCountHr), afterSize,
                static_cast<unsigned long>(ourHr), ourIndex,
                static_cast<unsigned long>(nativeAfterHr), nativeAfterIndex);
            const std::wstring after = ServiceSnapshot(service, collection);
            Log("C2 snapshot after %ls", after.c_str());
            const bool addVerified =
                SUCCEEDED(afterCountHr) && afterSize == beforeSize + 1 &&
                SUCCEEDED(ourHr) && ourIndex == insertAt &&
                SUCCEEDED(nativeAfterHr) && nativeAfterIndex == nativeIndex;

            // C2.4: always remove our TextBox to restore the initial state.
            HRESULT removeHr = E_UNEXPECTED;
            if (SUCCEEDED(ourHr)) {
                removeHr = service->RemoveChild(collection, ourIndex);
            }
            Log("C2 RemoveChild hr=0x%08X index=%u tid=%lu",
                static_cast<unsigned long>(removeHr), ourIndex, tid);

            // C2.5: verify the collection is back to its initial state.
            unsigned finalSize = 0;
            const HRESULT finalCountHr = service->GetCollectionCount(collection,
                                                                     &finalSize);
            unsigned nativeFinalIndex = 0;
            const HRESULT nativeFinalHr = ServiceFindHandleIndex(
                service, collection, nativeHandle, &nativeFinalIndex);
            unsigned ourFinalIndex = 0;
            const HRESULT ourFinalHr = ServiceFindHandleIndex(
                service, collection, created, &ourFinalIndex);
            Log("C2 final countHr=0x%08X size=%u nativeHr=0x%08X nativeIndex=%u "
                "ourHr=0x%08X",
                static_cast<unsigned long>(finalCountHr), finalSize,
                static_cast<unsigned long>(nativeFinalHr), nativeFinalIndex,
                static_cast<unsigned long>(ourFinalHr));
            const std::wstring finalSnapshot = ServiceSnapshot(service, collection);
            Log("C2 snapshot final %ls", finalSnapshot.c_str());
            const bool restored =
                SUCCEEDED(finalCountHr) && finalSize == beforeSize &&
                SUCCEEDED(nativeFinalHr) && nativeFinalIndex == nativeIndex &&
                FAILED(ourFinalHr);

            Log("C2 RESULT %s addVerified=%d restored=%d removeHr=0x%08X "
                "frame=%p gen=%llu tid=%lu",
                (addVerified && restored && SUCCEEDED(removeHr)) ? "PASS" : "FAIL",
                addVerified ? 1 : 0, restored ? 1 : 0,
                static_cast<unsigned long>(removeHr), frame,
                static_cast<unsigned long long>(generation), tid);
            return S_OK;
        });
    Log("C2 dispatch hr=0x%08X", static_cast<unsigned long>(dispatchHr));
}

'@

$probe = $probe.Replace("`n", "`r`n")
$text = $text.Substring(0, $start) + $probe + $text.Substring($end)
$text = $text.Replace('RunServiceProbe();', 'RunC2Probe();')
[IO.File]::WriteAllText($target, $text)
Write-Host "created $target"
