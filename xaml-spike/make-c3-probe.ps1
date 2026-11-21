#requires -Version 7.0
# C3: actual replacement on the per-element XAML dispatcher.
# AddChild -> verify -> RemoveChild(native) -> verify, with in-dispatch rollback.

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$source = Join-Path $PSScriptRoot 'c2-disp-probe.cpp'
$target = Join-Path $PSScriptRoot 'c3-probe.cpp'

$text = [IO.File]::ReadAllText($source)

function Replace-Once([string]$haystack, [string]$old, [string]$new, [string]$label) {
    $old = $old.Replace("`n", "`r`n")
    $new = $new.Replace("`n", "`r`n")
    if (-not $haystack.Contains($old)) { throw "missing $label" }
    return $haystack.Replace($old, $new)
}

$text = Replace-Once $text 'L"esx-c2-disp.log"' 'L"esx-c3-probe.log"' 'log path'
$text = Replace-Once $text 'constexpr bool kReplaceNative = false;' 'constexpr bool kReplaceNative = true;' 'replace flag'
$text = Replace-Once $text 'RunC2ForReadyStates();' 'RunC3ForReadyStates();' 'coordinator call'

$startMarker = 'static void RunC2ProbeForState(const std::shared_ptr<WindowState>& state) {'
$endMarker = 'void TryCreateExperimentTextBox() {'
$start = $text.IndexOf($startMarker)
$end = $text.IndexOf($endMarker)
if ($start -lt 0 -or $end -lt 0 -or $end -le $start) { throw 'C2 block markers not found' }
$block = $text.Substring($start, $end - $start)
$block = $block.Replace('C2', 'C3')

$keyDown = @'
            box.KeyDown([box, frame](
                            auto const&,
                            winrt::Microsoft::UI::Xaml::Input::KeyRoutedEventArgs const& args) {
                const int key = static_cast<int>(args.Key());
                if (key == VK_RETURN) {
                    const auto text = box.Text();
                    if (text.empty()) {
                        args.Handled(true);
                        return;
                    }
                    const std::wstring query(text.c_str(), text.size());
                    const bool inResults = IsResultsFolder(frame);
                    const std::wstring scope =
                        inResults ? std::wstring() : CurrentFolderPath(frame);
                    const bool written = WriteSearchRequest(query, scope);
                    Log("C3 Enter query='%ls' scope='%ls' written=%d",
                        query.c_str(), scope.c_str(), written ? 1 : 0);
                    if (written && IsWindow(frame)) NavigateToResults(frame);
                    box.Focus(winrt::Microsoft::UI::Xaml::FocusState::Programmatic);
                    args.Handled(true);
                } else if (key == VK_ESCAPE) {
                    box.Text(L"");
                    args.Handled(true);
                }
            });

'@
$block = Replace-Once $block @'
            winrt::com_ptr<::IInspectable> boxRaw{nullptr};
'@ ($keyDown + '            winrt::com_ptr<::IInspectable> boxRaw{nullptr};') 'key handler'

$tailStart = '            // C3.4: always remove our TextBox to restore the initial state.'
$tailEnd = '            return S_OK;'
$s = $block.IndexOf($tailStart)
$e = $block.IndexOf($tailEnd, $s)
if ($s -lt 0 -or $e -lt 0) { throw 'C3 tail markers not found' }
$e = $e + $tailEnd.Length

$newTail = @'
            // C3.4: keep the native object alive before removing its handle.
            winrt::com_ptr<::IInspectable> nativeObject{nullptr};
            HRESULT nativeObjectHr = E_UNEXPECTED;
            if (diagnostics) {
                nativeObjectHr = diagnostics->GetIInspectableFromHandle(
                    nativeHandle, nativeObject.put());
            }
            Log("C3 native object hr=0x%08X object=%p",
                static_cast<unsigned long>(nativeObjectHr), nativeObject.get());

            bool replaced = false;
            HRESULT nativeRemoveHr = E_UNEXPECTED;
            if (addVerified && SUCCEEDED(nativeObjectHr) && nativeObject) {
                unsigned nativeRemoveIndex = 0;
                const HRESULT findHr = ServiceFindHandleIndex(
                    service, collection, nativeHandle, &nativeRemoveIndex);
                if (SUCCEEDED(findHr)) {
                    nativeRemoveHr =
                        service->RemoveChild(collection, nativeRemoveIndex);
                }
                Log("C3 native RemoveChild findHr=0x%08X index=%u hr=0x%08X",
                    static_cast<unsigned long>(findHr), nativeRemoveIndex,
                    static_cast<unsigned long>(nativeRemoveHr));

                unsigned afterRemoveSize = 0;
                const HRESULT arCountHr = service->GetCollectionCount(
                    collection, &afterRemoveSize);
                unsigned ourAfterIndex = 0;
                const HRESULT ourAfterHr = ServiceFindHandleIndex(
                    service, collection, created, &ourAfterIndex);
                unsigned nativeGoneIndex = 0;
                const HRESULT nativeGoneHr = ServiceFindHandleIndex(
                    service, collection, nativeHandle, &nativeGoneIndex);
                Log("C3 replaced verify countHr=0x%08X size=%u ourHr=0x%08X "
                    "ourIndex=%u nativeGoneHr=0x%08X",
                    static_cast<unsigned long>(arCountHr), afterRemoveSize,
                    static_cast<unsigned long>(ourAfterHr), ourAfterIndex,
                    static_cast<unsigned long>(nativeGoneHr));
                // The native was at nativeIndex; we inserted at nativeIndex+1.
                // After removing it, our TextBox shifts to nativeIndex.
                replaced = SUCCEEDED(nativeRemoveHr) && SUCCEEDED(arCountHr) &&
                           afterRemoveSize == beforeSize &&
                           SUCCEEDED(ourAfterHr) &&
                           ourAfterIndex == nativeIndex && FAILED(nativeGoneHr);
            }

            bool rolledBack = false;
            if (!replaced && addVerified) {
                unsigned rbOurIndex = 0;
                const HRESULT rbFindHr = ServiceFindHandleIndex(
                    service, collection, created, &rbOurIndex);
                HRESULT rbHr = E_UNEXPECTED;
                if (SUCCEEDED(rbFindHr)) {
                    rbHr = service->RemoveChild(collection, rbOurIndex);
                }
                unsigned rbSize = 0;
                const HRESULT rbCountHr =
                    service->GetCollectionCount(collection, &rbSize);
                unsigned rbNativeIndex = 0;
                const HRESULT rbNativeHr = ServiceFindHandleIndex(
                    service, collection, nativeHandle, &rbNativeIndex);
                unsigned rbGoneIndex = 0;
                const HRESULT rbGoneHr = ServiceFindHandleIndex(
                    service, collection, created, &rbGoneIndex);
                rolledBack = SUCCEEDED(rbFindHr) && SUCCEEDED(rbHr) &&
                             SUCCEEDED(rbCountHr) && rbSize == beforeSize &&
                             SUCCEEDED(rbNativeHr) &&
                             rbNativeIndex == nativeIndex && FAILED(rbGoneHr);
                Log("C3 ROLLBACK findHr=0x%08X hr=0x%08X restored=%d size=%u "
                    "nativeHr=0x%08X ourHr=0x%08X",
                    static_cast<unsigned long>(rbFindHr),
                    static_cast<unsigned long>(rbHr), rolledBack ? 1 : 0, rbSize,
                    static_cast<unsigned long>(rbNativeHr),
                    static_cast<unsigned long>(rbGoneHr));
            }

            if (replaced) {
                state->textBox = box;
                state->textBoxHandle = created;
                state->originalNative = nativeObject;
                state->replacementActive = true;
                state->phase = WindowPhase::Active;
            } else {
                state->phase = WindowPhase::Failed;
            }
            Log("C3 RESULT %s replaced=%d rolledBack=%d addVerified=%d "
                "nativeRemoveHr=0x%08X frame=%p gen=%llu tid=%lu service=%p "
                "collection=%llu",
                replaced ? "PASS" : (rolledBack ? "ROLLBACK" : "FAIL"),
                replaced ? 1 : 0, rolledBack ? 1 : 0, addVerified ? 1 : 0,
                static_cast<unsigned long>(nativeRemoveHr), frame,
                static_cast<unsigned long long>(generation), tid, service,
                static_cast<unsigned long long>(collection));
            return S_OK;
'@
$newTail = $newTail.Replace("`n", "`r`n")
$block = $block.Substring(0, $s) + $newTail + $block.Substring($e)

$text = $text.Substring(0, $start) + $block + $text.Substring($end)
[IO.File]::WriteAllText($target, $text)
Write-Host "created $target"
