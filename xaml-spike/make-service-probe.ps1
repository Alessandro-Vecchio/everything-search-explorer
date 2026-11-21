#requires -Version 7.0
# Creates a read-only service-probe source from the restored spike source.
# The probe never calls AddChild/RemoveChild and never installs a replacement.

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$source = Join-Path $PSScriptRoot 'spike.cpp'
$target = Join-Path $PSScriptRoot 'service-probe.cpp'

$text = [IO.File]::ReadAllText($source)

$replacements = @(
    @('L"esx-xaml-spike.log"', 'L"esx-svc-probe.log"'),
    @('constexpr bool kReplaceNative = true;', 'constexpr bool kReplaceNative = false;')
)
foreach ($pair in $replacements) {
    if (-not $text.Contains($pair[0])) { throw "missing: $($pair[0])" }
    $text = $text.Replace($pair[0], $pair[1])
}

$oldCall = (@'
                if (g_callbackProbe.ready.load() && !g_addTxPosted.load()) {
                    TryCreateExperimentTextBox();
                    if (g_createdHandle != 0) PostAddTransaction();
                }
'@).Replace("`n", "`r`n")
$newCall = (@'
                if (g_callbackProbe.ready.load()) {
                    RunServiceProbe();
                }
'@).Replace("`n", "`r`n")
if (-not $text.Contains($oldCall)) { throw 'missing coordinator call block' }
$text = $text.Replace($oldCall, $newCall)

$probe = @'
static std::atomic<bool> g_serviceProbeDone{false};

static void RunServiceProbe() {
    if (g_serviceProbeDone.exchange(true)) return;
    auto* watcher = g_probeWatcher;
    if (!watcher || !g_callbackProbe.ready.load()) return;

    APTTYPE callerApt{};
    APTTYPEQUALIFIER callerQual{};
    const HRESULT callerAptHr = CoGetApartmentType(&callerApt, &callerQual);
    Log("SVCTEST caller tid=%lu aptHr=0x%08X apt=%d qual=%d",
        GetCurrentThreadId(), static_cast<unsigned long>(callerAptHr),
        static_cast<int>(callerApt), static_cast<int>(callerQual));

    const InstanceHandle nativeHandle =
        static_cast<InstanceHandle>(g_callbackProbe.handle.load());
    const HRESULT dispatchHr = watcher->RunOnDispatcher(
        nativeHandle, [watcher, nativeHandle]() -> HRESULT {
            const DWORD dispatchTid = GetCurrentThreadId();
            APTTYPE apt{};
            APTTYPEQUALIFIER qual{};
            const HRESULT aptHr = CoGetApartmentType(&apt, &qual);
            auto diagnostics = watcher->OwnerDiagnostics();
            auto* service = watcher->OwnerService();
            Log("SVCTEST dispatch tid=%lu aptHr=0x%08X apt=%d qual=%d "
                "diag=%d service=%p",
                dispatchTid, static_cast<unsigned long>(aptHr),
                static_cast<int>(apt), static_cast<int>(qual),
                diagnostics ? 1 : 0, service);
            if (!diagnostics || !service) return E_UNEXPECTED;

            winrt::com_ptr<IAgileObject> agile;
            const HRESULT agileHr = service->QueryInterface(
                __uuidof(IAgileObject), agile.put_void());
            Log("SVCTEST IAgileObject hr=0x%08X agile=%d",
                static_cast<unsigned long>(agileHr), agile ? 1 : 0);

            winrt::com_ptr<::IInspectable> nativeRaw{nullptr};
            HRESULT hr = diagnostics->GetIInspectableFromHandle(nativeHandle,
                                                                nativeRaw.put());
            Log("SVCTEST GetIInspectableFromHandle hr=0x%08X obj=%p",
                static_cast<unsigned long>(hr), nativeRaw.get());
            if (FAILED(hr) || !nativeRaw) return hr;

            winrt::Windows::Foundation::IInspectable nativeInspectable{nullptr};
            winrt::copy_from_abi(nativeInspectable, nativeRaw.get());
            auto element = nativeInspectable.as<
                winrt::Microsoft::UI::Xaml::FrameworkElement>();
            Log("SVCTEST element in dispatcher tid=%lu", GetCurrentThreadId());

            InstanceHandle collection = 0;
            HRESULT collHr = E_UNEXPECTED;
            auto parent = winrt::Microsoft::UI::Xaml::Media::VisualTreeHelper::GetParent(
                element);
            if (auto panel =
                    parent.try_as<winrt::Microsoft::UI::Xaml::Controls::Panel>()) {
                auto children = panel.Children();
                winrt::com_ptr<::IInspectable> rawChildren{nullptr};
                rawChildren = children.as<::IInspectable>();
                collHr = diagnostics->RegisterInstance(rawChildren.get(), &collection);
            }
            Log("SVCTEST RegisterInstance collection hr=0x%08X handle=%llu",
                static_cast<unsigned long>(collHr),
                static_cast<unsigned long long>(collection));
            if (FAILED(collHr) || !collection) return collHr;

            unsigned directCount = 0;
            const HRESULT directHr = service->GetCollectionCount(collection,
                                                                  &directCount);
            Log("SVCTEST raw GetCollectionCount hr=0x%08X size=%u tid=%lu",
                static_cast<unsigned long>(directHr), directCount,
                GetCurrentThreadId());

            const HRESULT viaHr = watcher->RunOnServiceThread(
                [collection](IVisualTreeService3* s) -> HRESULT {
                    unsigned count = 0;
                    APTTYPE a{};
                    APTTYPEQUALIFIER q{};
                    const HRESULT aHr = CoGetApartmentType(&a, &q);
                    const HRESULT hr2 = s->GetCollectionCount(collection, &count);
                    Log("SVCTEST marshaled GetCollectionCount hr=0x%08X size=%u "
                        "execTid=%lu aptHr=0x%08X apt=%d qual=%d",
                        static_cast<unsigned long>(hr2), count,
                        GetCurrentThreadId(), static_cast<unsigned long>(aHr),
                        static_cast<int>(a), static_cast<int>(q));
                    return hr2;
                });
            Log("SVCTEST viaServiceThread hr=0x%08X", static_cast<unsigned long>(viaHr));
            return S_OK;
        });
    Log("SVCTEST dispatch hr=0x%08X", static_cast<unsigned long>(dispatchHr));
}

'@

$insertMarker = 'void TryCreateExperimentTextBox() {'
if (-not $text.Contains($insertMarker)) { throw 'missing insert marker' }
$probe = $probe.Replace("`n", "`r`n")
$text = $text.Replace($insertMarker, $probe + $insertMarker)

[IO.File]::WriteAllText($target, $text)
Write-Host "created $target"
