#requires -Version 7.0
# Per-window input routing on top of the C3 replacement probe.
# Shortcuts and Backspace target the WindowState of the foreground frame and
# are executed on that window's resolved dispatcher.

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$source = Join-Path $PSScriptRoot 'c3-probe.cpp'
$target = Join-Path $PSScriptRoot 'input-probe.cpp'

$text = [IO.File]::ReadAllText($source)

function Replace-Once([string]$haystack, [string]$old, [string]$new, [string]$label) {
    $old = $old.Replace("`n", "`r`n")
    $new = $new.Replace("`n", "`r`n")
    if (-not $haystack.Contains($old)) { throw "missing $label" }
    return $haystack.Replace($old, $new)
}

$text = Replace-Once $text 'L"esx-c3-probe.log"' 'L"esx-input-probe.log"' 'log path'

$text = Replace-Once $text @'
                if (SUCCEEDED(findHr)) {
                    nativeRemoveHr =
                        service->RemoveChild(collection, nativeRemoveIndex);
                }
'@ @'
                if (SUCCEEDED(findHr)) {
                    state->mutating.store(true);
                    nativeRemoveHr =
                        service->RemoveChild(collection, nativeRemoveIndex);
                    state->mutating.store(false);
                }
'@ 'mutating guard around native removal'

$text = Replace-Once $text @'
            for (auto it = g_windowStates.begin(); it != g_windowStates.end();) {
                if (it->second->nativeHandle == element.Handle) {
                    it = g_windowStates.erase(it);
                } else {
                    ++it;
                }
            }
'@ @'
            for (auto it = g_windowStates.begin(); it != g_windowStates.end();) {
                if (it->second->nativeHandle == element.Handle &&
                    !it->second->replacementActive && !it->second->mutating) {
                    it = g_windowStates.erase(it);
                } else {
                    ++it;
                }
            }
'@ 'keep active state on remove'

$text = Replace-Once $text @'
    std::atomic<bool> c2Running{false};
    winrt::agile_ref<winrt::Microsoft::UI::Dispatching::DispatcherQueue> dispatcher;
'@ @'
    std::atomic<bool> c2Running{false};
    std::atomic<bool> textHasFocus{false};
    std::atomic<bool> mutating{false};
    winrt::agile_ref<winrt::Microsoft::UI::Dispatching::DispatcherQueue> dispatcher;
'@ 'focus field'

$handlers = @'
            box.GotFocus([state](auto const&, auto const&) {
                state->textHasFocus.store(true);
                Log("DIRECT focus gained frame=%p tid=%lu", state->frame,
                    GetCurrentThreadId());
            });
            box.LostFocus([state](auto const&, auto const&) {
                state->textHasFocus.store(false);
                Log("DIRECT focus lost frame=%p tid=%lu", state->frame,
                    GetCurrentThreadId());
            });

'@
$text = Replace-Once $text @'
            winrt::com_ptr<::IInspectable> boxRaw{nullptr};
'@ ($handlers + '            winrt::com_ptr<::IInspectable> boxRaw{nullptr};') 'focus handlers'

$helpers = @'
static std::shared_ptr<WindowState> FindActiveStateForFrame(HWND frame) {
    if (!frame) return nullptr;
    std::lock_guard<std::mutex> guard(g_windowStatesMutex);
    for (const auto& item : g_windowStates) {
        const auto& state = item.second;
        if (state->frame == frame && state->replacementActive) return state;
    }
    return nullptr;
}

static bool PostToStateDispatcher(const std::shared_ptr<WindowState>& state,
                                  std::function<void()> action) {
    if (!state) return false;
    try {
        auto dispatcher = state->dispatcher.get();
        if (!dispatcher) return false;
        return dispatcher.TryEnqueue([action = std::move(action)]() mutable {
            try {
                if (action) action();
            } catch (...) {
                Log("DIRECT queued action exception hr=0x%08X",
                    static_cast<unsigned long>(winrt::to_hresult()));
            }
        });
    } catch (...) {
        Log("DIRECT enqueue exception hr=0x%08X",
            static_cast<unsigned long>(winrt::to_hresult()));
        return false;
    }
}

'@
$text = Replace-Once $text 'LRESULT CALLBACK KeyboardHookProc(int code, WPARAM wParam, LPARAM lParam) {' ($helpers + 'LRESULT CALLBACK KeyboardHookProc(int code, WPARAM wParam, LPARAM lParam) {') 'helpers'

$oldBackspace = @'
        if (wParam == WM_KEYDOWN && key->vkCode == VK_BACK) {
            if (g_replacementActive.load() && g_directTextHasFocus.load() &&
                g_probeWatcher) {
                HWND channel = g_probeWatcher->ChannelWindow();
                if (channel && PostMessageW(channel, kOwnerBackspaceMessage, 0, 0)) {
                    return 1;
                }
            }
'@
$newBackspace = @'
        if (wParam == WM_KEYDOWN && key->vkCode == VK_BACK) {
            auto activeState = FindActiveStateForFrame(frame);
            Log("DIRECT hook BACK frame=%p active=%d focus=%d fg=%p",
                frame, activeState ? 1 : 0,
                activeState && activeState->textHasFocus.load() ? 1 : 0,
                GetForegroundWindow());
            if (activeState && activeState->textHasFocus.load()) {
                const bool queued = PostToStateDispatcher(
                    activeState, [activeState]() {
                        auto box = activeState->textBox;
                        if (!box) return;
                        const auto selected = box.SelectedText();
                        if (!selected.empty()) {
                            box.SelectedText(L"");
                        } else {
                            const auto text = box.Text();
                            std::wstring updated(text.c_str(), text.size());
                            if (!updated.empty()) {
                                updated.pop_back();
                                box.Text(winrt::hstring(updated));
                                box.SelectionStart(
                                    static_cast<int32_t>(updated.size()));
                                box.SelectionLength(0);
                            }
                        }
                    });
                if (queued) {
                    Log("DIRECT backspace frame=%p gen=%llu",
                        activeState->frame,
                        static_cast<unsigned long long>(
                            activeState->generation));
                    return 1;
                }
            }
'@
$text = Replace-Once $text $oldBackspace $newBackspace 'backspace routing'

$oldShortcut = @'
        if (searchShortcut && !alt) {
            HWND frame = GetAncestor(GetForegroundWindow(), GA_ROOT);
            if (g_replacementActive.load() && g_probeWatcher &&
                (!g_experimentFrame || frame == g_experimentFrame)) {
                HWND channel = g_probeWatcher->ChannelWindow();
                if (channel && PostMessageW(channel, kOwnerFocusMessage, 0, 0)) {
                    Log("DIRECT shortcut=%lu queued to owner", key->vkCode);
                    return 1;
                }
            }
'@
$newShortcut = @'
        if (searchShortcut) {
            auto activeState = FindActiveStateForFrame(frame);
            Log("DIRECT hook shortcut key=%lu frame=%p fg=%p active=%d "
                "ctrl=%d alt=%d",
                key->vkCode, frame, GetForegroundWindow(),
                activeState ? 1 : 0, control ? 1 : 0, alt ? 1 : 0);
            if (activeState && !alt) {
                const bool queued = PostToStateDispatcher(
                    activeState, [activeState]() {
                        if (!activeState->textBox) return;
                        const bool ok = activeState->textBox.Focus(
                            winrt::Microsoft::UI::Xaml::FocusState::Programmatic);
                        Log("DIRECT focus frame=%p gen=%llu ok=%d",
                            activeState->frame,
                            static_cast<unsigned long long>(
                                activeState->generation),
                            ok ? 1 : 0);
                    });
                if (queued) {
                    Log("DIRECT shortcut=%lu frame=%p gen=%llu", key->vkCode,
                        activeState->frame,
                        static_cast<unsigned long long>(
                            activeState->generation));
                    return 1;
                }
            }
'@
$text = Replace-Once $text $oldShortcut $newShortcut 'shortcut routing'

[IO.File]::WriteAllText($target, $text)
Write-Host "created $target"
