// ==WindhawkMod==
// @id              everything-explorer-search
// @name            Everything Search for Explorer
// @description     Replaces Explorer's native search box in-process with a field backed by Everything.
// @version         0.5.0
// @author          Alessandro
// @include         explorer.exe
// @compilerOptions -lole32 -loleaut32 -lshlwapi -lshell32 -lgdi32 -luuid -ladvapi32 -lcomctl32 -ld2d1 -lruntimeobject -lcoremessaging -luiautomationcore -lversion
// ==/WindhawkMod==

// ==WindhawkModReadme==
// # Everything Search for Explorer
//
// Replaces Explorer's native search field directly in the XAML tree: no overlay
// window and no second search box. Type a query and press Enter to open the
// Everything results folder, scoped to the current folder when it has a real
// filesystem path (virtual folders such as "This PC" search globally).
// `Ctrl+F`, `Ctrl+E` and `F3` focus the field; Backspace edits text.
//
// Requires Everything 1.4+ running non-elevated and the companion
// `EverythingSearchNse.dll` namespace extension installed via `install.ps1`.
// Folder windows must be hosted in separate processes
// (`HKCU\Software\Microsoft\Windows\CurrentVersion\Explorer\Advanced\SeparateProcess
// = 1`): XAML Diagnostics is attached only to `/factory` processes, and the
// native bar is never touched when the window cannot be identified safely.
// ==/WindhawkModReadme==

// ==WindhawkModSettings==
// - maxResults: 0
//   $name: Maximum results
//   $description: 0 returns every match; a positive value sets an explicit limit.
// - scopeToFolder: true
//   $name: Scope to current folder
//   $description: Restrict the search to the folder shown in the current Explorer window.
// ==/WindhawkModSettings==

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cwchar>
#include <commctrl.h>
#include <appmodel.h>
#include <roapi.h>
#include <DispatcherQueue.h>
#include <UIAutomation.h>
#include <xamlOM.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <exdisp.h>
#include <shlwapi.h>
#include <tlhelp32.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <array>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <map>
#include <vector>

#include "winrt/base.h"
#include "winrt/Microsoft.UI.h"
#include "winrt/Microsoft.UI.Content.h"
#include "winrt/Microsoft.UI.Dispatching.h"
#include "winrt/Microsoft.UI.Xaml.h"
#include "winrt/Microsoft.UI.Xaml.Controls.h"
#include "winrt/Microsoft.UI.Xaml.Controls.Primitives.h"
#include "winrt/Microsoft.UI.Xaml.Hosting.h"
#include "winrt/Microsoft.UI.Xaml.Input.h"
#include "winrt/Microsoft.UI.Xaml.Media.h"
#include "winrt/Windows.Foundation.h"
#include "winrt/Windows.Foundation.Collections.h"
#include "winrt/Windows.Graphics.h"
#include "winrt/Windows.System.h"

#include "windhawk_api.h"
#include "../shared/protocol.h"

namespace {

// Phase 1: passive observer only. Collect the structural relation reported by
// OnVisualTreeChange for every FileExplorerSearchBox; never query the tree and
// never modify it. No islands, no hiding, no subclassing.
constexpr bool kObserverOnly = true;
constexpr bool kCallbackProbeOnly = true;
constexpr bool kReplaceNative = true;
constexpr bool kAutoRollback = false;
constexpr bool kEnableTreeWalk = false;
constexpr UINT kServiceCallMessage = WM_APP + 0x59;
constexpr UINT kOwnerInitMessage = WM_APP + 0x5A;
constexpr UINT kOwnerProbeMessage = WM_APP + 0x5B;
constexpr UINT kOwnerAddMessage = WM_APP + 0x5C;
constexpr UINT kOwnerRollbackMessage = WM_APP + 0x5D;
constexpr UINT kOwnerFocusMessage = WM_APP + 0x5E;
constexpr UINT kOwnerBackspaceMessage = WM_APP + 0x5F;
constexpr UINT kOwnerEnumerateMessage = WM_APP + 0x60;
constexpr wchar_t kOwnerWindowClass[] = L"EsxDirectXamlOwnerChannel";
extern InstanceHandle g_createdHandle;
extern winrt::Microsoft::UI::Xaml::Controls::TextBox g_createdTextBox;
class VisualTreeWatcher;
struct CallbackProbeResult;

// Phase A: per-window registry. The existing single-window replacement path is
// left untouched as a fallback; nothing here mutates XAML yet.
void Log(const char* format, ...);
struct AddTxResult;
struct RollbackTxResult;

enum class WindowPhase : int {
    Discovered = 1,
    XamlMapped = 2,
    TextBoxCreated = 3,
    AddPending = 4,
    Replacing = 5,
    Active = 6,
    RestorePending = 7,
    Closing = 8,
    Failed = 9
};

static HWND ActiveTabWindow(HWND frame);
static HWND TabWindowForRect(HWND frame, const RECT& rect);
static HWND TabWindowForThread(DWORD pid, DWORD threadId);

struct WindowState {
    HWND frame = nullptr;
    HWND tab = nullptr;  // per-tab identity (ShellTabWindowClass)
    std::wstring queryText;  // last query submitted from this tab
    DWORD pid = 0;
    uint64_t generation = 0;
    winrt::com_ptr<VisualTreeWatcher> watcher;
    InstanceHandle nativeHandle = 0;
    InstanceHandle parentHandle = 0;
    InstanceHandle collectionHandle = 0;
    unsigned nativeIndex = 0;
    RECT nativeRect{};
    winrt::Microsoft::UI::Xaml::Controls::TextBox textBox{nullptr};
    InstanceHandle textBoxHandle = 0;
    winrt::com_ptr<::IInspectable> originalNative;
    std::wstring origin;
    bool replacementActive = false;
    bool addPosted = false;
    bool rollbackPosted = false;
    ULONGLONG insertTick = 0;
    std::shared_ptr<AddTxResult> addTx;
    std::shared_ptr<RollbackTxResult> rollbackTx;
    std::atomic<WindowPhase> phase{WindowPhase::Discovered};
    bool uiaDiscovered = false;
    bool xamlMapped = false;
    DWORD threadId = 0;
    std::shared_ptr<CallbackProbeResult> probe;
    std::atomic<bool> c2Done{false};
    std::atomic<bool> c2Running{false};
    std::atomic<bool> textHasFocus{false};
    std::atomic<bool> mutating{false};
    std::atomic<bool> ignoreNextEmpty{false};
    std::atomic<bool> closingSearch{false};
    winrt::agile_ref<winrt::Microsoft::UI::Dispatching::DispatcherQueue> dispatcher;

    // Search command-bar button (created together with the replacement box).
    winrt::Microsoft::UI::Xaml::Controls::AppBarButton optionsButton{nullptr};
    InstanceHandle optionsButtonHandle = 0;
    std::atomic<bool> commandBarAdded{false};
    std::atomic<bool> buttonsVisible{false};
    std::atomic<ULONGLONG> lastButtonCheck{0};
};

std::unordered_map<HWND, std::shared_ptr<WindowState>> g_windowStates;
std::unordered_map<InstanceHandle, std::shared_ptr<WindowState>> g_xamlPending;
std::mutex g_windowStatesMutex;
std::atomic<uint64_t> g_windowGeneration{1};

// Explorer has a single XAML search box per window, shared by all its tabs.
// Keep the typed query per tab and swap it when the active tab changes.
std::mutex g_tabTextMutex;
std::unordered_map<HWND, std::wstring> g_tabText;
std::unordered_map<HWND, HWND> g_frameLastTab;

void SaveTabText(HWND tab, const std::wstring& text) {
    std::lock_guard<std::mutex> guard(g_tabTextMutex);
    g_tabText[tab] = text;
}

std::wstring LoadTabText(HWND tab) {
    std::lock_guard<std::mutex> guard(g_tabTextMutex);
    const auto it = g_tabText.find(tab);
    return it != g_tabText.end() ? it->second : std::wstring();
}

// Per-window probe slots, keyed by the native XAML handle. No single global
// slot is used to create controls for different windows.
std::mutex g_probeByHandleMutex;
std::unordered_map<unsigned long long, std::shared_ptr<CallbackProbeResult>>
    g_probeByHandle;

std::shared_ptr<WindowState> FindStateForNative(InstanceHandle handle) {
    std::lock_guard<std::mutex> guard(g_windowStatesMutex);
    for (const auto& item : g_windowStates) {
        if (item.second->nativeHandle == handle) return item.second;
    }
    auto pending = g_xamlPending.find(handle);
    return pending != g_xamlPending.end() ? pending->second : nullptr;
}

struct ThreadFrameLookup {
    DWORD pid = 0;
    DWORD thread = 0;
    std::vector<HWND> frames;
};

void CorrelateWindowStates() {
    std::lock_guard<std::mutex> guard(g_windowStatesMutex);
    for (auto it = g_xamlPending.begin(); it != g_xamlPending.end();) {
        auto pending = it->second;
        // Tabs share frame, thread and screen rectangle. The tab captured when
        // the search box was first observed is the reliable discriminator;
        // the thread lookup is only a fallback for states created elsewhere.
        const HWND tab = pending->tab ? pending->tab
                                      : TabWindowForThread(pending->pid, pending->threadId);
        std::vector<std::shared_ptr<WindowState>> candidates;
        for (const auto& item : g_windowStates) {
            auto state = item.second;
            if (state->pid == pending->pid && state->uiaDiscovered &&
                !state->xamlMapped && (!tab || state->tab == tab)) {
                candidates.push_back(state);
            }
        }
        if (candidates.empty() && tab) {
            const HWND frame = ::GetAncestor(tab, GA_ROOT);
            auto& state = g_windowStates[tab];
            if (!state) {
                state = std::make_shared<WindowState>();
                state->frame = frame;
                state->tab = tab;
                state->pid = pending->pid;
                state->threadId = pending->threadId;
                state->generation = g_windowGeneration.fetch_add(1);
                state->uiaDiscovered = true;
                state->phase = WindowPhase::Discovered;
            }
            if (!state->xamlMapped) candidates.push_back(state);
        }
        if (candidates.empty() && !tab) {
            // No tab could be resolved from the thread: fall back to the old
            // thread/frame heuristic and leave the bar untouched if ambiguous.
            ThreadFrameLookup lookup{pending->pid, pending->threadId, {}};
            EnumWindows(
                [](HWND hwnd, LPARAM parameter) -> BOOL {
                    auto* found = reinterpret_cast<ThreadFrameLookup*>(parameter);
                    DWORD windowPid = 0;
                    const DWORD windowThread =
                        GetWindowThreadProcessId(hwnd, &windowPid);
                    if (windowThread != found->thread || windowPid != found->pid ||
                        !IsWindowVisible(hwnd)) {
                        return TRUE;
                    }
                    wchar_t className[128]{};
                    if (!GetClassNameW(hwnd, className, ARRAYSIZE(className)) ||
                        wcscmp(className, L"CabinetWClass") != 0) {
                        return TRUE;
                    }
                    found->frames.push_back(hwnd);
                    return TRUE;
                },
                reinterpret_cast<LPARAM>(&lookup));
            if (lookup.frames.size() == 1) {
                const HWND frame = lookup.frames.front();
                const HWND frameTab = ActiveTabWindow(frame);
                const HWND key = frameTab ? frameTab : frame;
                auto& state = g_windowStates[key];
                if (!state) {
                    state = std::make_shared<WindowState>();
                    state->frame = frame;
                    state->tab = key;
                    state->pid = pending->pid;
                    state->threadId = pending->threadId;
                    state->generation = g_windowGeneration.fetch_add(1);
                    state->phase = WindowPhase::Discovered;
                }
                if (!state->xamlMapped) candidates.push_back(state);
            }
        }
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
        auto target = candidates.front();
        if (tab) target->tab = tab;
        target->xamlMapped = true;
        target->phase = WindowPhase::XamlMapped;
        target->nativeHandle = pending->nativeHandle;
        target->parentHandle = pending->parentHandle;
        target->nativeIndex = pending->nativeIndex;
        target->watcher = pending->watcher;
        target->threadId = pending->threadId;
        {
            std::lock_guard<std::mutex> probeGuard(g_probeByHandleMutex);
            auto pit = g_probeByHandle.find(static_cast<unsigned long long>(
                pending->nativeHandle));
            if (pit != g_probeByHandle.end()) target->probe = pit->second;
        }
        Log("WINDOW correlated frame=%p tab=%p pid=%lu tid=%lu gen=%llu native=%llu "
            "index=%u source=%s",
            target->frame, static_cast<void*>(target->tab), target->pid,
            pending->threadId, static_cast<unsigned long long>(target->generation),
            static_cast<unsigned long long>(target->nativeHandle),
            target->nativeIndex, target->uiaDiscovered ? "uia" : "thread");
        it = g_xamlPending.erase(it);
    }
}

void LogWindowStates() {
    std::lock_guard<std::mutex> guard(g_windowStatesMutex);
    for (const auto& item : g_windowStates) {
        const auto& state = item.second;
        Log("WINDOW frame=%p pid=%lu gen=%llu phase=%d uia=%d xaml=%d "
            "native=%llu parent=%llu index=%u watcher=%p rect=%ld,%ld %ldx%ld",
            state->frame, state->pid,
            static_cast<unsigned long long>(state->generation),
            static_cast<int>(state->phase.load()), state->uiaDiscovered ? 1 : 0,
            state->xamlMapped ? 1 : 0,
            static_cast<unsigned long long>(state->nativeHandle),
            static_cast<unsigned long long>(state->parentHandle),
            state->nativeIndex, state->watcher.get(), state->nativeRect.left,
            state->nativeRect.top, state->nativeRect.right - state->nativeRect.left,
            state->nativeRect.bottom - state->nativeRect.top);
    }
    for (const auto& item : g_xamlPending) {
        const auto& state = item.second;
        if (state->frame) continue;
        Log("XPENDING native=%llu pid=%lu gen=%llu parent=%llu index=%u "
            "watcher=%p",
            static_cast<unsigned long long>(state->nativeHandle), state->pid,
            static_cast<unsigned long long>(state->generation),
            static_cast<unsigned long long>(state->parentHandle),
            state->nativeIndex, state->watcher.get());
    }
}

struct CallbackProbeResult {
    std::atomic<bool> inProgress{false};
    std::atomic<bool> ready{false};
    std::atomic<unsigned long long> handle{0};
    unsigned long long parent = 0;
    unsigned int childIndex = 0;
    unsigned int threadId = 0;
    unsigned int execThread = 0;
    unsigned int ownerThread = 0;
    bool tidMismatch = false;
    HRESULT chainHr = E_FAIL;
    unsigned int valueCount = 0;
    unsigned int collectionCount = 0;
    unsigned long long firstCollection = 0;
    unsigned int firstCollectionIndex = 0;
    hyper collectionMetadata = 0;
    wchar_t propertyName[128]{};
    wchar_t itemType[128]{};
    wchar_t valueType[128]{};
    HRESULT countHr = E_FAIL;
    unsigned int collectionSize = 0;
    HRESULT elementsHr = E_FAIL;
    unsigned int elementCount = 0;
    struct ElementInfo {
        unsigned int index = 0;
        hyper bits = 0;
        unsigned long long handle = 0;
        wchar_t valueType[64]{};
        wchar_t value[64]{};
    };
    std::array<ElementInfo, 16> elements{};
    struct LayoutProp {
        wchar_t name[64]{};
        wchar_t value[64]{};
        wchar_t declaring[96]{};
        unsigned int chainIndex = 0;
    };
    std::array<LayoutProp, 32> layoutProps{};
    unsigned int layoutCount = 0;
    CallbackProbeResult() {
        handle = 0;
        parent = 0;
        childIndex = 0;
        threadId = 0;
        execThread = 0;
        ownerThread = 0;
        tidMismatch = false;
        chainHr = E_FAIL;
        valueCount = 0;
        collectionCount = 0;
        firstCollection = 0;
        firstCollectionIndex = 0;
    }
};
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
// The reentrant probe must run only in a process that actually owns a folder
// window. The shell process has none and its XAML is busy: probing there is
// the likely cause of the recent crashes during Windhawk injection.
std::atomic<bool> g_probeAllowed{false};
VisualTreeWatcher* g_probeWatcher = nullptr;
winrt::com_ptr<::IInspectable> g_nativeObject;

struct OwnerAddRequest {
    std::shared_ptr<WindowState> state;
    uint64_t generation = 0;
    InstanceHandle collection = 0;
    InstanceHandle native = 0;
    InstanceHandle created = 0;
    unsigned index = 0;
    bool replaceNative = false;
};

struct AddTxResult {
    std::atomic<bool> ready{false};
    HRESULT countHr = E_FAIL;
    unsigned beforeSize = 0;
    HRESULT nativeBeforeHr = E_FAIL;
    unsigned nativeBeforeIndex = 0;
    HRESULT addHr = E_FAIL;
    HRESULT afterCountHr = E_FAIL;
    unsigned afterSize = 0;
    HRESULT ourHr = E_FAIL;
    unsigned ourIndex = 0;
    HRESULT nativeAfterHr = E_FAIL;
    unsigned nativeAfterIndex = 0;
    HRESULT recheckHr = E_FAIL;
    unsigned recheckIndex = 0;
    HRESULT removeHr = E_FAIL;
    HRESULT finalCountHr = E_FAIL;
    unsigned finalSize = 0;
    HRESULT ourGoneHr = E_FAIL;
    HRESULT nativeFinalHr = E_FAIL;
    unsigned nativeFinalIndex = 0;
    HRESULT nativeRemoveFindHr = E_FAIL;
    unsigned nativeRemoveIndex = 0;
    HRESULT nativeRemoveHr = E_FAIL;
    HRESULT afterRemoveCountHr = E_FAIL;
    unsigned afterRemoveSize = 0;
    HRESULT nativeGoneHr = E_FAIL;
    HRESULT ourAfterRemoveHr = E_FAIL;
    unsigned ourAfterRemoveIndex = 0;
};
AddTxResult g_addTx;
std::atomic<bool> g_addTxReported{false};
std::atomic<bool> g_addTxPosted{false};
HANDLE g_addMutex = nullptr;

struct RollbackTxResult {
    std::atomic<bool> ready{false};
    HRESULT findHr = E_FAIL;
    unsigned index = 0;
    HRESULT removeHr = E_FAIL;
    HRESULT countHr = E_FAIL;
    unsigned size = 0;
    HRESULT ourGoneHr = E_FAIL;
    HRESULT nativeHr = E_FAIL;
    unsigned nativeIndex = 0;
    HRESULT restoreHr = E_FAIL;
    unsigned long long restoredHandle = 0;
};
RollbackTxResult g_rollbackTx;
std::atomic<bool> g_rollbackPosted{false};
ULONGLONG g_insertTick = 0;
OwnerAddRequest g_lastAddRequest;
std::atomic<bool> g_rollbackReported{false};
std::atomic<bool> g_replacementActive{false};
std::atomic<bool> g_directTextHasFocus{false};
std::wstring g_origin;
RECT g_nativeRect{0, 0, 0, 0};

bool WriteSearchRequest(const std::wstring& query, const std::wstring& folder, HWND frame);
bool IsResultsFolder(HWND frame);
std::wstring CurrentFolderPath(HWND frame);
void NavigateToResults(HWND frame);
bool GetBrowserForWindow(HWND frame, winrt::com_ptr<IShellBrowser>& browser);
static HWND ActiveTabWindow(HWND frame);

struct ObservedSearchBox;
void RecordObservedBox(uintptr_t watcher, const ParentChildRelation& relation,
                       const VisualElement& element, bool added);

IUIAutomation* g_automation = nullptr;

// Windhawk settings (defaults come from the ==WindhawkModSettings== block).
int g_maxResults = 0;       // 0 = every match
bool g_scopeToFolder = true;

void Log(const char* format, ...) {
    char buffer[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);

    wchar_t dir[MAX_PATH]{};
    if (!GetTempPathW(MAX_PATH, dir)) return;
    const std::wstring path = std::wstring(dir) + L"everything-explorer-search.log";
    // Development diagnostics only: never let the file grow without bound.
    WIN32_FILE_ATTRIBUTE_DATA info{};
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info)) {
        const ULONGLONG size =
            (static_cast<ULONGLONG>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
        if (size > 4ULL * 1024 * 1024) {
            HANDLE reset = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                                       CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (reset != INVALID_HANDLE_VALUE) CloseHandle(reset);
        }
    }
    HANDLE file = CreateFileW(path.c_str(), FILE_APPEND_DATA,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                              OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    char line[1100];
    SYSTEMTIME now{};
    GetLocalTime(&now);
    const int n = snprintf(line, sizeof(line),
                           "%02u:%02u:%02u.%03u [%lu] %s\r\n", now.wHour,
                           now.wMinute, now.wSecond, now.wMilliseconds,
                           GetCurrentThreadId(), buffer);
    if (n > 0) {
        DWORD written = 0;
        WriteFile(file, line, static_cast<DWORD>(n), &written, nullptr);
    }
    CloseHandle(file);
}

HWND FindFrame() {
    struct FrameSearch {
        DWORD processId;
        HWND found;
    } search{GetCurrentProcessId(), nullptr};

    EnumWindows([](HWND hwnd, LPARAM param) -> BOOL {
        auto* search = reinterpret_cast<FrameSearch*>(param);
        DWORD processId = 0;
        GetWindowThreadProcessId(hwnd, &processId);
        if (processId != search->processId) return TRUE;

        wchar_t cls[128]{};
        if (!GetClassNameW(hwnd, cls, ARRAYSIZE(cls))) return TRUE;
        if (wcscmp(cls, L"CabinetWClass") != 0 || !IsWindowVisible(hwnd)) return TRUE;
        search->found = hwnd;
        return FALSE;
    }, reinterpret_cast<LPARAM>(&search));
    return search.found;
}

struct SearchControl {
    HWND hwnd = nullptr;
    RECT rect{};
};

struct ChildWindowSearch {
    HWND frame = nullptr;
    RECT target{};
    HWND exactMatch = nullptr;
    int exactMatchCount = 0;
    int intersectingCount = 0;
};

bool RectsMatch(const RECT& left, const RECT& right) {
    constexpr LONG kRectTolerance = 2;
    return std::abs(left.left - right.left) <= kRectTolerance &&
           std::abs(left.top - right.top) <= kRectTolerance &&
           std::abs(left.right - right.right) <= kRectTolerance &&
           std::abs(left.bottom - right.bottom) <= kRectTolerance;
}

BOOL CALLBACK InspectSearchChild(HWND hwnd, LPARAM parameter) {
    auto* search = reinterpret_cast<ChildWindowSearch*>(parameter);
    if (!IsWindowVisible(hwnd)) return TRUE;

    RECT rect{};
    if (!GetWindowRect(hwnd, &rect)) return TRUE;
    const bool intersects = rect.left < search->target.right &&
                            rect.right > search->target.left &&
                            rect.top < search->target.bottom &&
                            rect.bottom > search->target.top;
    if (!intersects) return TRUE;

    char className[128]{};
    GetClassNameA(hwnd, className, ARRAYSIZE(className));
    const LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    if (search->intersectingCount < 32) {
        Log("intersecting child hwnd=%p class=%s style=0x%p rect=%ld,%ld,%ld,%ld",
            hwnd, className, reinterpret_cast<void*>(style), rect.left, rect.top,
            rect.right, rect.bottom);
    }
    ++search->intersectingCount;

    if (RectsMatch(rect, search->target)) {
        search->exactMatch = hwnd;
        ++search->exactMatchCount;
    }
    return TRUE;
}

bool FindDedicatedChildWindow(HWND frame, const RECT& target, SearchControl* result) {
    ChildWindowSearch search{frame, target};
    EnumChildWindows(frame, InspectSearchChild, reinterpret_cast<LPARAM>(&search));

    Log("search-area child windows: intersecting=%d exact=%d",
        search.intersectingCount, search.exactMatchCount);
    if (search.exactMatchCount != 1 || !IsWindow(search.exactMatch)) return false;
    if (GetAncestor(search.exactMatch, GA_ROOT) != frame) {
        Log("exact child candidate has unexpected root hwnd=%p", search.exactMatch);
        return false;
    }

    result->hwnd = search.exactMatch;
    result->rect = target;
    return true;
}

bool FindNativeSearchControl(HWND frame, SearchControl* result) {
    IUIAutomation* automation = g_automation;
    if (!automation) {
        Log("UIA not warmed up");
        return false;
    }

    IUIAutomationElement* root = nullptr;
    HRESULT hr = automation->ElementFromHandle(frame, &root);
    if (FAILED(hr)) {
        Log("UIA ElementFromHandle hr=0x%08X", hr);
        return false;
    }

    VARIANT automationId{};
    automationId.vt = VT_BSTR;
    automationId.bstrVal = SysAllocString(L"FileExplorerSearchBox");
    IUIAutomationCondition* condition = nullptr;
    hr = automation->CreatePropertyCondition(UIA_AutomationIdPropertyId, automationId,
                                             &condition);
    VariantClear(&automationId);

    IUIAutomationElement* searchElement = nullptr;
    if (SUCCEEDED(hr)) {
        hr = root->FindFirst(TreeScope_Descendants, condition, &searchElement);
    }
    if (SUCCEEDED(hr) && !searchElement) {
        struct ChildSearch {
            IUIAutomation* automation;
            IUIAutomationCondition* condition;
            IUIAutomationElement* element = nullptr;
            HRESULT result = S_OK;
        } childSearch{automation, condition};
        Log("UIA search not found under frame; checking child HWND trees");
        EnumChildWindows(frame, [](HWND child, LPARAM parameter) -> BOOL {
            auto* search = reinterpret_cast<ChildSearch*>(parameter);
            IUIAutomationElement* childRoot = nullptr;
            HRESULT childHr = search->automation->ElementFromHandle(child, &childRoot);
            if (FAILED(childHr) || !childRoot) return TRUE;

            childHr = childRoot->FindFirst(TreeScope_Descendants,
                                           search->condition,
                                           &search->element);
            childRoot->Release();
            if (FAILED(childHr)) {
                search->result = childHr;
                return TRUE;
            }
            return search->element ? FALSE : TRUE;
        }, reinterpret_cast<LPARAM>(&childSearch));
        if (childSearch.element) {
            searchElement = childSearch.element;
        } else {
            hr = childSearch.result;
        }
    }
    if (condition) condition->Release();
    root->Release();
    if (FAILED(hr) || !searchElement) {
        Log("UIA FileExplorerSearchBox lookup hr=0x%08X", hr);
        return false;
    }

    UIA_HWND nativeHwndValue = nullptr;
    RECT bounds{};
    const HRESULT hwndHr = searchElement->get_CurrentNativeWindowHandle(&nativeHwndValue);
    const HRESULT boundsHr = searchElement->get_CurrentBoundingRectangle(&bounds);
    searchElement->Release();
    const HWND nativeHwnd = reinterpret_cast<HWND>(nativeHwndValue);
    Log("native search hwnd=%p hwndHr=0x%08X boundsHr=0x%08X rect=%.1f,%.1f %.1fx%.1f",
        nativeHwnd, hwndHr, boundsHr, static_cast<double>(bounds.left),
        static_cast<double>(bounds.top), static_cast<double>(bounds.right - bounds.left),
        static_cast<double>(bounds.bottom - bounds.top));
    if (FAILED(boundsHr)) return false;
    const RECT automationRect = bounds;
    if (SUCCEEDED(hwndHr) && IsWindow(nativeHwnd) && nativeHwnd != frame) {
        RECT windowRect{};
        if (GetWindowRect(nativeHwnd, &windowRect) &&
            GetAncestor(nativeHwnd, GA_ROOT) == frame &&
            RectsMatch(windowRect, automationRect)) {
            result->hwnd = nativeHwnd;
            result->rect = automationRect;
            return true;
        }
        Log("UIA HWND is not a dedicated search window; trying child HWNDs");
    }

    if (FindDedicatedChildWindow(frame, automationRect, result)) return true;

    result->rect = automationRect;
    result->hwnd = nullptr;
    Log("search box has no dedicated HWND; will target its XAML element");
    return true;
}

DWORD WINAPI CoordinatorThread(LPVOID);

constexpr CLSID kSearchBoxTapClsid = {
    0x6A7E4B31,
    0x9F24,
    0x47D8,
    {0xA6, 0x2D, 0x52, 0x6C, 0x1B, 0x43, 0x89, 0xE0}};

MIDL_INTERFACE("7776037b-cbbe-4de1-ad6f-0dbf1d5817b4")
IXamlDiagnosticsTestHooks : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE UnregisterInstance(InstanceHandle) = 0;
    virtual HRESULT STDMETHODCALLTYPE TryGetDispatcherQueueForObject(
        InstanceHandle object, ::IInspectable** queue) = 0;
};

HANDLE g_tapAttachedEvent = nullptr;
HANDLE g_stopEvent = nullptr;
std::mutex g_lifecycleMutex;
std::atomic_bool g_stopping{false};
constexpr UINT kSubmitXamlSearchMessage = WM_APP + 0x54;
constexpr wchar_t kSearchOverlayProperty[] = L"EverythingSearchBoxOverlay.Handle";

struct PropertyOverride {
    InstanceHandle element = 0;
    unsigned int propertyIndex = 0;
};

struct PropertyChainData {
    unsigned int sourceCount = 0;
    PropertyChainSource* sources = nullptr;
    unsigned int valueCount = 0;
    PropertyChainValue* values = nullptr;

    ~PropertyChainData() {
        if (sources) {
            for (unsigned int i = 0; i < sourceCount; ++i) {
                SysFreeString(sources[i].TargetType);
                SysFreeString(sources[i].Name);
                SysFreeString(sources[i].SrcInfo.FileName);
                SysFreeString(sources[i].SrcInfo.Hash);
            }
        }
        if (values) {
            for (unsigned int i = 0; i < valueCount; ++i) {
                SysFreeString(values[i].Type);
                SysFreeString(values[i].DeclaringType);
                SysFreeString(values[i].ValueType);
                SysFreeString(values[i].ItemType);
                SysFreeString(values[i].Value);
                SysFreeString(values[i].PropertyName);
            }
        }
        CoTaskMemFree(sources);
        CoTaskMemFree(values);
    }

    bool IsSearchBox() const {
        bool hasSearchType = false;
        bool hasSearchName = false;
        for (unsigned int i = 0; i < sourceCount; ++i) {
            if (sources[i].TargetType &&
                wcsstr(sources[i].TargetType, L"AutoSuggestBox")) {
                hasSearchType = true;
                break;
            }
        }
        for (unsigned int i = 0; i < valueCount; ++i) {
            if (values[i].PropertyName && values[i].Value &&
                wcscmp(values[i].PropertyName, L"Name") == 0 &&
                wcscmp(values[i].Value, L"FileExplorerSearchBox") == 0) {
                hasSearchName = true;
                break;
            }
        }
        return hasSearchType && hasSearchName;
    }

    bool IsMicrosoftUIXaml() const {
        for (unsigned int i = 0; i < sourceCount; ++i) {
            if (sources[i].TargetType &&
                wcsncmp(sources[i].TargetType, L"Microsoft.UI.Xaml.", 18) == 0) {
                return true;
            }
        }
        return false;
    }
};

struct OwnerRequest {
    std::shared_ptr<WindowState> state;
    uint64_t generation = 0;
    InstanceHandle parent = 0;
    InstanceHandle native = 0;
    InstanceHandle created = 0;
    unsigned index = 0;
};

class VisualTreeWatcher;
extern std::mutex g_hookOwnersMutex;
extern std::unordered_map<DWORD, VisualTreeWatcher*> g_hookOwners;
LRESULT CALLBACK OwnerGetMessageHook(int code, WPARAM wParam, LPARAM lParam);

class VisualTreeWatcher
    : public winrt::implements<VisualTreeWatcher, IVisualTreeServiceCallback2> {
public:
    struct CollectionProbe;
    void EnsureChannelWindow();
    HRESULT ProbeOnOwnerThread(InstanceHandle parent, unsigned index,
                               InstanceHandle native, CallbackProbeResult* out);
    HRESULT ProbeCollectionViaOwner(InstanceHandle parent, unsigned index,
                                    InstanceHandle native, CollectionProbe* out);

    struct ServiceCall {
        std::function<HRESULT(IVisualTreeService3*)> fn;
        HANDLE done = nullptr;
        HRESULT result = E_FAIL;
    };

    static DWORD WINAPI ServiceExecutorProc(LPVOID parameter) {
        auto* self = static_cast<VisualTreeWatcher*>(parameter);
        Log("WATCHER executor start tid=%lu", GetCurrentThreadId());
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        winrt::com_ptr<IVisualTreeService3> proxy;
        const HRESULT hr = CoGetInterfaceAndReleaseStream(
            self->m_serviceStream.get(), __uuidof(IVisualTreeService3),
            proxy.put_void());
        // CoGetInterfaceAndReleaseStream already releases the stream. Detach
        // without releasing again: doing `m_serviceStream = nullptr` here was a
        // double Release on a freed object and crashed in this thread with an
        // execute AV at a heap address (random, process-dependent).
        self->m_serviceStream.detach();
        self->m_unmarshalHr.store(hr);
        Log("DIAGMARSHAL unmarshal hr=0x%08X thread=%lu",
            static_cast<unsigned long>(hr), GetCurrentThreadId());
        if (SUCCEEDED(hr)) {
            self->m_serviceProxy = proxy;
        }
        SetEvent(self->m_executorReadyEvent);

        MSG message{};
        PeekMessageW(&message, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
        for (;;) {
            if (WaitForSingleObject(self->m_executorStopEvent, 0) == WAIT_OBJECT_0) {
                break;
            }
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                if (message.message == kServiceCallMessage && message.lParam) {
                    auto* call = reinterpret_cast<ServiceCall*>(message.lParam);
                    call->result = call->fn(proxy.get());
                    SetEvent(call->done);
                }
            }
            MsgWaitForMultipleObjectsEx(0, nullptr, 50, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        }
        self->m_serviceProxy = nullptr;
        CoUninitialize();
        Log("WATCHER executor exit tid=%lu", GetCurrentThreadId());
        return 0;
    }

    HRESULT RunOnServiceThread(std::function<HRESULT(IVisualTreeService3*)> fn) {
        if (!m_executorThread || !m_executorThreadId) return E_UNEXPECTED;
        if (WaitForSingleObject(m_executorReadyEvent, 3000) != WAIT_OBJECT_0) {
            return HRESULT_FROM_WIN32(WAIT_TIMEOUT);
        }
        const HRESULT unmarshalHr = m_unmarshalHr.load();
        if (FAILED(unmarshalHr)) return unmarshalHr;

        HANDLE owner = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE,
                                  m_ownerThreadId);
        if (owner) {
            const DWORD ownerState = WaitForSingleObject(owner, 0);
            CloseHandle(owner);
            if (ownerState == WAIT_OBJECT_0) {
                Log("DIAGMARSHAL ABORT: owner thread %lu exited", m_ownerThreadId);
                return E_ABORT;
            }
        }

        ServiceCall call;
        call.done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!call.done) return HRESULT_FROM_WIN32(GetLastError());
        call.fn = std::move(fn);
        if (!PostThreadMessageW(m_executorThreadId, kServiceCallMessage, 0,
                                reinterpret_cast<LPARAM>(&call))) {
            CloseHandle(call.done);
            return HRESULT_FROM_WIN32(GetLastError());
        }
        const DWORD wait = WaitForSingleObject(call.done, 2000);
        CloseHandle(call.done);
        if (wait != WAIT_OBJECT_0) return HRESULT_FROM_WIN32(WAIT_TIMEOUT);
        return call.result;
    }

    explicit VisualTreeWatcher(IUnknown* site) {
        winrt::com_ptr<IUnknown> siteReference;
        siteReference.copy_from(site);
        auto diagnostics = siteReference.as<IXamlDiagnostics>();
        auto service = siteReference.as<IVisualTreeService3>();
        m_diagnostics = winrt::make_agile(diagnostics);
        m_service = winrt::make_agile(service);
        m_serviceOriginal = service;
        m_site = siteReference;
        m_ownerThreadId = GetCurrentThreadId();
        {
            auto ownerQueue =
                winrt::Windows::System::DispatcherQueue::GetForCurrentThread();
            if (ownerQueue) {
                m_ownerQueue = winrt::make_agile(ownerQueue);
                Log("DISPATCHQ owner queue on thread=%lu", m_ownerThreadId);
            } else {
                Log("DISPATCHQ no queue on owner thread=%lu", m_ownerThreadId);
            }
        }
        m_channelReadyEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        // Create the message-only channel here, on the owner thread: no hook
        // is needed. The WndProc will execute queued work after OnVisualTreeChange
        // has returned, avoiding reentrant calls into the XAML service.
        EnsureChannelWindow();
        Log("OWNER channel in constructor hwnd=%p thread=%lu",
            m_channelWindow, m_ownerThreadId);
        APTTYPE apartmentType{};
        APTTYPEQUALIFIER apartmentQualifier{};
        const HRESULT apartmentHr = CoGetApartmentType(&apartmentType, &apartmentQualifier);
        m_marshalHr = CoMarshalInterThreadInterfaceInStream(
            __uuidof(IVisualTreeService3), service.get(), m_serviceStream.put());
        Log("DIAGMARSHAL owner thread=%lu aptHr=0x%08X apt=%d qual=%d marshal=0x%08X",
            m_ownerThreadId, static_cast<unsigned long>(apartmentHr),
            static_cast<int>(apartmentType), static_cast<int>(apartmentQualifier),
            static_cast<unsigned long>(m_marshalHr));
        m_executorReadyEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        m_executorStopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (SUCCEEDED(m_marshalHr) && m_executorReadyEvent && m_executorStopEvent) {
            m_executorThread = CreateThread(nullptr, 0, ServiceExecutorProc, this, 0, nullptr);
            if (m_executorThread) {
                m_executorThreadId = GetThreadId(m_executorThread);
                Log("DIAGMARSHAL executor thread=%lu created", m_executorThreadId);
            } else {
                Log("DIAGMARSHAL executor creation failed err=%lu", GetLastError());
            }
        }
        winrt::com_ptr<IXamlDiagnosticsTestHooks> testHooks;
        const HRESULT testHooksHr = diagnostics->QueryInterface(
            __uuidof(IXamlDiagnosticsTestHooks), testHooks.put_void());
        if (SUCCEEDED(testHooksHr)) {
            m_testHooks = winrt::make_agile(testHooks);
        } else {
            Log("IXamlDiagnosticsTestHooks unavailable hr=0x%08X", testHooksHr);
        }
        m_adviseCompleteEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!m_adviseCompleteEvent) {
            winrt::throw_hresult(HRESULT_FROM_WIN32(GetLastError()));
        }
        m_searchFoundEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!m_searchFoundEvent) {
            const DWORD error = GetLastError();
            CloseHandle(m_adviseCompleteEvent);
            m_adviseCompleteEvent = nullptr;
            winrt::throw_hresult(HRESULT_FROM_WIN32(error));
        }
    }

    ~VisualTreeWatcher() {
        if (m_ownerHook) {
            {
                std::lock_guard<std::mutex> guard(g_hookOwnersMutex);
                auto it = g_hookOwners.find(m_ownerThreadId);
                if (it != g_hookOwners.end() && it->second == this) {
                    g_hookOwners.erase(it);
                }
            }
            UnhookWindowsHookEx(m_ownerHook);
            m_ownerHook = nullptr;
        }
        if (m_channelReadyEvent) CloseHandle(m_channelReadyEvent);
        StopThreads();
        // Only close events that no live thread can still signal/wait on.
        if (m_executorStopped) {
            if (m_executorReadyEvent) CloseHandle(m_executorReadyEvent);
            if (m_executorStopEvent) CloseHandle(m_executorStopEvent);
        }
        if (m_adviseStopped) {
            if (m_adviseCompleteEvent) CloseHandle(m_adviseCompleteEvent);
            if (m_searchFoundEvent) CloseHandle(m_searchFoundEvent);
        }
    }

    HRESULT StartAdvise() {
        AddRef();
        m_adviseThread =
            CreateThread(nullptr, 0, AdviseThreadProc, this, 0, nullptr);
        if (!m_adviseThread) {
            const DWORD error = GetLastError();
            Release();
            return HRESULT_FROM_WIN32(error);
        }
        return S_OK;
    }

    HANDLE AdviseCompleteEvent() const { return m_adviseCompleteEvent; }
    HANDLE SearchFoundEvent() const { return m_searchFoundEvent; }
    HRESULT AdviseResult() const { return m_adviseResult.load(); }
    InstanceHandle SearchElementHandle() const { return m_searchElement.load(); }

    // Phase 2b.1: read-only inspection of the collection that owns the native
    // search box. AddChild needs the COLLECTION handle, not the parent element.
    struct CollectionProbe {
        InstanceHandle collection = 0;
        unsigned index = 0;
        std::wstring propertyName;
        std::wstring itemType;
        bool verified = false;
    };

    static InstanceHandle ParseHandleValue(BSTR value) {
        if (!value) return 0;
        unsigned long long parsed = 0;
        if (swscanf_s(value, L"%llu", &parsed) == 1) {
            return static_cast<InstanceHandle>(parsed);
        }
        if (swscanf_s(value, L"0x%llX", &parsed) == 1) {
            return static_cast<InstanceHandle>(parsed);
        }
        return 0;
    }

    HRESULT ProbeCollection(InstanceHandle parent, unsigned childIndex,
                            InstanceHandle nativeHandle, CollectionProbe* out) {
        return ProbeCollectionViaOwner(parent, childIndex, nativeHandle, out);
    }

    HRESULT AddChildAt(InstanceHandle collection, InstanceHandle child, unsigned index) {
        auto service = m_service.get();
        return service ? service->AddChild(collection, child, index) : E_UNEXPECTED;
    }

    HRESULT FindHandleIndex(InstanceHandle collection, InstanceHandle target,
                            unsigned* index) {
        auto service = m_service.get();
        if (!service || !index) return E_UNEXPECTED;
        unsigned count = 0;
        HRESULT hr = service->GetCollectionCount(collection, &count);
        if (FAILED(hr)) return hr;
        for (unsigned start = 0; start < count; start += 16) {
            unsigned wanted = 16;
            CollectionElementValue* elements = nullptr;
            hr = service->GetCollectionElements(collection, start, &wanted, &elements);
            if (FAILED(hr) || !elements) return hr;
            bool found = false;
            for (unsigned i = 0; i < wanted; ++i) {
                if (ParseHandleValue(elements[i].Value) == target) {
                    *index = elements[i].Index;
                    found = true;
                    break;
                }
            }
            for (unsigned i = 0; i < wanted; ++i) {
                SysFreeString(elements[i].ValueType);
                SysFreeString(elements[i].Value);
            }
            CoTaskMemFree(elements);
            if (found) return S_OK;
        }
        return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    }

    HRESULT RemoveChildAt(InstanceHandle collection, unsigned index) {
        auto service = m_service.get();
        return service ? service->RemoveChild(collection, index) : E_UNEXPECTED;
    }

    HRESULT CollectionSnapshot(InstanceHandle collection, std::wstring* description) {
        auto service = m_service.get();
        if (!service || !description) return E_UNEXPECTED;
        unsigned count = 0;
        HRESULT hr = service->GetCollectionCount(collection, &count);
        if (FAILED(hr)) return hr;
        wchar_t buffer[512]{};
        swprintf_s(buffer, L"count=%u [", count);
        description->assign(buffer);
        unsigned wanted = count < 8 ? count : 8;
        CollectionElementValue* elements = nullptr;
        hr = service->GetCollectionElements(collection, 0, &wanted, &elements);
        if (SUCCEEDED(hr) && elements) {
            for (unsigned i = 0; i < wanted; ++i) {
                swprintf_s(buffer, L"%s%u:", i ? L" " : L"", elements[i].Index);
                description->append(buffer);
                if (elements[i].Value) description->append(elements[i].Value);
            }
            for (unsigned i = 0; i < wanted; ++i) {
                SysFreeString(elements[i].ValueType);
                SysFreeString(elements[i].Value);
            }
            CoTaskMemFree(elements);
        }
        description->append(L"]");
        return S_OK;
    }

    // Phase 2a: register a TextBox with the XAML diagnostics service without
    // inserting it anywhere. No visual tree mutation.
    // The original service interface is valid only on the owner thread; the
    // message-only WndProc runs there and uses this accessor.
    IVisualTreeService3* OwnerService() const { return m_serviceOriginal.get(); }
    winrt::com_ptr<IXamlDiagnostics> OwnerDiagnostics() const { return m_diagnostics.get(); }
    IUnknown* OwnerSite() const { return m_site.get(); }
    HWND ChannelWindow() const { return m_channelWindow; }

    HRESULT RegisterCreatedInstance(
        winrt::Microsoft::UI::Xaml::Controls::TextBox const& box,
        InstanceHandle* handle) {
        auto diagnostics = m_diagnostics.get();
        if (!diagnostics) return E_UNEXPECTED;
        winrt::com_ptr<::IInspectable> instance = box.as<::IInspectable>();
        return diagnostics->RegisterInstance(instance.get(), handle);
    }

    HRESULT HideSearchHandle(InstanceHandle element) {
        std::lock_guard<std::recursive_mutex> guard(m_stateMutex);
        if (!m_active) return E_ABORT;
        return HideSearchElement(element);
    }

    // Preferred path: OnVisualTreeChange already found the element, so no
    // HitTest is needed. HitTest during advise registration crashes XAML.
    HRESULT HideKnownSearchElement() {
        const InstanceHandle element = m_searchElement.load();
        if (!element) return S_FALSE;
        return HideSearchHandle(element);
    }

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
        winrt::Microsoft::UI::Dispatching::DispatcherQueue* resolved = nullptr,
        DWORD timeoutMs = 10000) {
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
            const DWORD wait = WaitForSingleObject(state->done, timeoutMs);
            return wait == WAIT_OBJECT_0 ? state->result.load()
                                         : HRESULT_FROM_WIN32(WAIT_TIMEOUT);
        } catch (...) {
            return winrt::to_hresult();
        }
    }

    // Queue UIElement work on the dispatcher that was resolved for the native
    // search control. The diagnostics owner thread is not assumed to be the
    // XAML element's dispatcher thread.
    bool PostToDispatcher(std::function<void()> action) noexcept {
        try {
            winrt::Microsoft::UI::Dispatching::DispatcherQueue dispatcher{nullptr};
            {
                std::lock_guard<std::recursive_mutex> guard(m_stateMutex);
                if (m_dispatcherQueue) dispatcher = m_dispatcherQueue.get();
            }
            if (!dispatcher) return false;
            return dispatcher.TryEnqueue([action = std::move(action)]() mutable {
                try {
                    if (action) action();
                } catch (...) {
                    Log("queued XAML dispatcher action exception hr=0x%08X",
                        static_cast<unsigned long>(winrt::to_hresult()));
                }
            });
        } catch (...) {
            Log("PostToDispatcher exception hr=0x%08X",
                static_cast<unsigned long>(winrt::to_hresult()));
            return false;
        }
    }

    // Windhawk refuses to unload the mod DLL while any thread still has a frame
    // inside it, and waits up to 80 seconds (200 x 400ms) before giving up.
    // Every thread we create must therefore be joined here, not left to the
    // destructor (which may never run while XAML still holds a reference).
    void StopThreads() noexcept {
        const DWORD self = GetCurrentThreadId();
        m_executorStopped = true;
        m_adviseStopped = true;

        if (m_executorThreadId) {
            PostThreadMessageW(m_executorThreadId, WM_QUIT, 0, 0);
        }
        if (m_executorStopEvent) SetEvent(m_executorStopEvent);
        if (m_executorThread) {
            const DWORD wait = WaitForSingleObject(m_executorThread, 2000);
            Log("WATCHER executor join tid=%lu wait=%lu", m_executorThreadId,
                static_cast<unsigned long>(wait));
            if (wait == WAIT_OBJECT_0) {
                CloseHandle(m_executorThread);
                m_executorThread = nullptr;
                m_executorThreadId = 0;
            } else {
                m_executorStopped = false;
            }
        }

        if (m_adviseThread && GetThreadId(m_adviseThread) != self) {
            const DWORD wait = WaitForSingleObject(m_adviseThread, 2000);
            Log("WATCHER advise join wait=%lu", static_cast<unsigned long>(wait));
            if (wait == WAIT_OBJECT_0) {
                CloseHandle(m_adviseThread);
                m_adviseThread = nullptr;
            } else {
                m_adviseStopped = false;
            }
        }
    }

    void Detach() noexcept {
        // Restore the overrides BEFORE unadvising: after Unadvise the diagnostics
        // connection is gone and ClearProperty fails with 0x8000FFFF, leaving
        // the native search box invisible.
        RestoreOverrides();
        m_active = false;
        m_unadviseRequested = true;
        Unadvise();
        StopThreads();
    }

    // The frame window is gone: its XAML island and the hidden element no longer
    // exist. Calling ClearProperty on those dead handles crashes Explorer, so
    // just drop the state and unadvise the connection.
    void Abandon() noexcept {
        m_active = false;
        m_unadviseRequested = true;
        Unadvise();
        StopThreads();
        {
            std::lock_guard<std::recursive_mutex> guard(m_stateMutex);
            m_overrideCount = 0;
            m_hiddenElements.clear();
        }
    }

    HRESULT FindSearchElement(const RECT& screenRect, HWND frame,
                              InstanceHandle* matchedHandle = nullptr,
                              bool hide = true) {
        auto diagnostics = m_diagnostics.get();
        auto service = m_service.get();
        if (!diagnostics || !service) return E_UNEXPECTED;

        RECT candidates[2]{screenRect, screenRect};
        MapWindowPoints(HWND_DESKTOP, frame,
                        reinterpret_cast<POINT*>(&candidates[1]), 2);
        const UINT dpi = GetDpiForWindow(frame);
        if (dpi) {
            candidates[1].left = MulDiv(candidates[1].left, 96, dpi);
            candidates[1].top = MulDiv(candidates[1].top, 96, dpi);
            candidates[1].right = MulDiv(candidates[1].right, 96, dpi);
            candidates[1].bottom = MulDiv(candidates[1].bottom, 96, dpi);
        }

        for (const RECT& rect : candidates) {
            unsigned int handleCount = 0;
            InstanceHandle* handles = nullptr;
            const HRESULT hitTestHr = diagnostics->HitTest(rect, &handleCount, &handles);
            Log("XAML HitTest hr=0x%08X handles=%u rect=%ld,%ld,%ld,%ld",
                hitTestHr, handleCount, rect.left, rect.top, rect.right, rect.bottom);
            if (FAILED(hitTestHr)) {
                CoTaskMemFree(handles);
                continue;
            }

            bool found = false;
            for (unsigned int i = 0; i < handleCount; ++i) {
                PropertyChainData properties;
                const HRESULT propertiesHr = service->GetPropertyValuesChain(
                    handles[i], &properties.sourceCount, &properties.sources,
                    &properties.valueCount, &properties.values);
                if (FAILED(propertiesHr) || !properties.IsSearchBox()) continue;

                std::lock_guard<std::recursive_mutex> guard(m_stateMutex);
                if (!m_active) {
                    CoTaskMemFree(handles);
                    return E_ABORT;
                }
                Log("XAML HitTest matched FileExplorerSearchBox handle=%llu",
                    static_cast<unsigned long long>(handles[i]));
                m_isMicrosoftUIXaml = properties.IsMicrosoftUIXaml();
                if (matchedHandle) *matchedHandle = handles[i];
                if (!hide) {
                    CoTaskMemFree(handles);
                    return S_OK;
                }
                const HRESULT hideHr = HideSearchElement(handles[i]);
                CoTaskMemFree(handles);
                return hideHr;
            }
            CoTaskMemFree(handles);
        }

        return S_FALSE;
    }

private:
    HRESULT RunOnTargetDispatcher(InstanceHandle element,
                                  std::function<HRESULT()> action,
                                  bool allowInactive = false) {
        try {
            winrt::Microsoft::UI::Dispatching::DispatcherQueue dispatcher{nullptr};
            {
                std::lock_guard<std::recursive_mutex> guard(m_stateMutex);
                if (!allowInactive && !m_active.load()) return E_ABORT;
                if (m_dispatcherQueue) dispatcher = m_dispatcherQueue.get();
            }
            if (!dispatcher) {
                auto hooks = m_testHooks.get();
                if (!hooks) return E_NOINTERFACE;

                winrt::com_ptr<::IInspectable> queueObject;
                const HRESULT hr = hooks->TryGetDispatcherQueueForObject(
                    element, queueObject.put());
                if (FAILED(hr)) return hr;
                if (!queueObject) return E_UNEXPECTED;

                dispatcher =
                    queueObject.as<winrt::Microsoft::UI::Dispatching::DispatcherQueue>();
                std::lock_guard<std::recursive_mutex> guard(m_stateMutex);
                if (!m_dispatcherQueue) {
                    m_dispatcherQueue = winrt::make_agile(dispatcher);
                }
            }

            if (dispatcher.HasThreadAccess()) {
                if (!allowInactive && !m_active.load()) return E_ABORT;
                return action();
            }

            struct DispatchState {
                DispatchState() : completed(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}
                ~DispatchState() {
                    if (completed) CloseHandle(completed);
                }

                HANDLE completed = nullptr;
                std::atomic<HRESULT> result{E_PENDING};
            };
            auto state = std::make_shared<DispatchState>();
            if (!state->completed) return HRESULT_FROM_WIN32(GetLastError());

            AddRef();
            bool enqueued = false;
            try {
                enqueued = dispatcher.TryEnqueue([this, state, action,
                                                  allowInactive]() {
                    HRESULT taskHr = E_ABORT;
                    if (allowInactive || m_active.load()) {
                        try {
                            taskHr = action();
                        } catch (...) {
                            taskHr = winrt::to_hresult();
                        }
                    }
                    state->result = taskHr;
                    SetEvent(state->completed);
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

            const DWORD waitResult =
                WaitForSingleObject(state->completed, 2000);
            if (waitResult != WAIT_OBJECT_0) {
                // A stale XAML dispatcher during mod unload can leave the
                // enqueued action unprocessed; never block unload forever.
                return waitResult == WAIT_TIMEOUT
                           ? HRESULT_FROM_WIN32(WAIT_TIMEOUT)
                           : HRESULT_FROM_WIN32(GetLastError());
            }
            return state->result.load();
        } catch (...) {
            return winrt::to_hresult();
        }
    }

    static DWORD WINAPI AdviseThreadProc(LPVOID parameter) noexcept {
        auto* watcher = static_cast<VisualTreeWatcher*>(parameter);
        Log("WATCHER advise start tid=%lu", GetCurrentThreadId());
        HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const bool uninitialize = SUCCEEDED(hr);
        if (SUCCEEDED(hr)) {
            try {
                auto service = watcher->m_service.get();
                hr = service ? service->AdviseVisualTreeChange(watcher) : E_UNEXPECTED;
            } catch (...) {
                hr = winrt::to_hresult();
            }
        }

        watcher->m_adviseResult = hr;
        if (SUCCEEDED(hr)) {
            watcher->m_advised = true;
            watcher->Unadvise();
        }
        SetEvent(watcher->m_adviseCompleteEvent);
        if (FAILED(hr)) Log("AdviseVisualTreeChange hr=0x%08X", hr);
        if (uninitialize) CoUninitialize();
        Log("WATCHER advise exit tid=%lu", GetCurrentThreadId());
        watcher->Release();
        return 0;
    }

    void Unadvise() noexcept {
        if (!m_unadviseRequested.load() || !m_advised.exchange(false)) return;
        HRESULT hr = E_FAIL;
        try {
            auto service = m_service.get();
            hr = service ? service->UnadviseVisualTreeChange(this) : E_UNEXPECTED;
        } catch (...) {
            hr = winrt::to_hresult();
        }
        if (FAILED(hr)) Log("UnadviseVisualTreeChange hr=0x%08X", hr);
    }

    HRESULT SetDiagnosticProperty(InstanceHandle element, PCWSTR propertyName,
                                  PCWSTR typeName, PCWSTR value,
                                  unsigned int* propertyIndexOut) {
        auto service = m_service.get();
        if (!service) return E_UNEXPECTED;

        const std::wstring propertyNameCopy(propertyName);
        const std::wstring typeNameCopy(typeName);
        const std::wstring valueCopy(value);
        auto propertyIndex = std::make_shared<unsigned int>(0);
        auto valueHandle = std::make_shared<InstanceHandle>(0);
        const HRESULT hr = RunOnTargetDispatcher(
            element, [service, element, propertyNameCopy, typeNameCopy, valueCopy,
                      propertyIndex, valueHandle]() {
                HRESULT operationHr = service->GetPropertyIndex(
                    element, propertyNameCopy.c_str(), propertyIndex.get());
                if (FAILED(operationHr)) {
                    Log("GetPropertyIndex(%ls) element=%llu hr=0x%08X",
                        propertyNameCopy.c_str(),
                        static_cast<unsigned long long>(element), operationHr);
                    return operationHr;
                }

                BSTR type = SysAllocString(typeNameCopy.c_str());
                BSTR text = SysAllocString(valueCopy.c_str());
                if (!type || !text) {
                    SysFreeString(type);
                    SysFreeString(text);
                    return E_OUTOFMEMORY;
                }

                operationHr = service->CreateInstance(type, text, valueHandle.get());
                SysFreeString(type);
                SysFreeString(text);
                if (FAILED(operationHr)) {
                    Log("CreateInstance(%ls) hr=0x%08X",
                        typeNameCopy.c_str(), operationHr);
                    return operationHr;
                }

                operationHr = service->SetProperty(
                    element, *valueHandle, *propertyIndex);
                if (FAILED(operationHr)) {
                    Log("SetProperty(%ls) hr=0x%08X",
                        propertyNameCopy.c_str(), operationHr);
                }
                return operationHr;
            });
        if (FAILED(hr)) return hr;

        if (SUCCEEDED(hr)) {
            if (m_overrideCount == m_overrides.size()) {
                const HRESULT clearHr = RunOnTargetDispatcher(
                    element, [service, element, propertyIndex]() {
                        return service->ClearProperty(element, *propertyIndex);
                    });
                if (FAILED(clearHr)) {
                    Log("ClearProperty after override overflow hr=0x%08X",
                        clearHr);
                }
                return E_OUTOFMEMORY;
            }
            m_overrides[m_overrideCount++] =
                PropertyOverride{element, *propertyIndex};
            *propertyIndexOut = *propertyIndex;
        }
        return hr;
    }

    void RestoreOverrides() noexcept {
        auto service = m_service.get();
        if (!service && m_overrideCount) {
            Log("cannot restore XAML search properties: diagnostics service unavailable");
            m_overrideCount = 0;
            m_hiddenElements.clear();
            return;
        }
        while (m_overrideCount) {
            const auto& override = m_overrides[--m_overrideCount];
            const HRESULT hr = RunOnTargetDispatcher(
                override.element, [service, override]() {
                    return service->ClearProperty(override.element,
                                                  override.propertyIndex);
                }, true);
            if (FAILED(hr)) Log("ClearProperty hr=0x%08X", hr);
        }
        m_hiddenElements.clear();
    }

    void RestoreOverridesFor(InstanceHandle element) noexcept {
        auto service = m_service.get();
        if (!service) return;
        for (size_t i = 0; i < m_overrideCount;) {
            const auto current = m_overrides[i];
            if (current.element != element) {
                ++i;
                continue;
            }
            RunOnTargetDispatcher(current.element, [service, current]() {
                return service->ClearProperty(current.element, current.propertyIndex);
            }, true);
            for (size_t j = i + 1; j < m_overrideCount; ++j) {
                m_overrides[j - 1] = m_overrides[j];
            }
            --m_overrideCount;
        }
        m_hiddenElements.erase(
            std::remove(m_hiddenElements.begin(), m_hiddenElements.end(), element),
            m_hiddenElements.end());
    }

    HRESULT HideSearchElement(InstanceHandle element) {
        if (std::find(m_hiddenElements.begin(), m_hiddenElements.end(), element) !=
            m_hiddenElements.end()) {
            return S_OK;
        }
        // Do not restore overrides here: hiding window B must not make window
        // A's native search bar visible again.

        const std::wstring xamlNamespace =
            m_isMicrosoftUIXaml.load() ? L"Microsoft.UI.Xaml."
                                       : L"Windows.UI.Xaml.";
        const std::wstring opacityProperty = xamlNamespace + L"UIElement.Opacity";
        const std::wstring hitTestProperty =
            xamlNamespace + L"UIElement.IsHitTestVisible";
        const std::wstring tabStopProperty =
            xamlNamespace + L"Controls.Control.IsTabStop";
        const std::wstring enabledProperty =
            xamlNamespace + L"Controls.Control.IsEnabled";

        unsigned int propertyIndex = 0;
        HRESULT hr = SetDiagnosticProperty(element, opacityProperty.c_str(),
                                           L"Windows.Foundation.Double", L"0",
                                           &propertyIndex);
        if (FAILED(hr)) return hr;
        hr = SetDiagnosticProperty(element, hitTestProperty.c_str(),
                                   L"Windows.Foundation.Boolean", L"False",
                                   &propertyIndex);
        if (FAILED(hr)) {
            RestoreOverridesFor(element);
            return hr;
        }
        hr = SetDiagnosticProperty(element, tabStopProperty.c_str(),
                                   L"Windows.Foundation.Boolean", L"False",
                                   &propertyIndex);
        if (FAILED(hr)) {
            RestoreOverridesFor(element);
            return hr;
        }
        // Best effort: disabling the element also stops keyboard input being
        // routed to it by Explorer's own accelerators.
        hr = SetDiagnosticProperty(element, enabledProperty.c_str(),
                                   L"Windows.Foundation.Boolean", L"False",
                                   &propertyIndex);
        if (FAILED(hr)) {
            Log("could not disable native search element hr=0x%08X", hr);
            hr = S_OK;
        }

        m_hiddenElements.push_back(element);
        Log("native XAML search hidden through diagnostics");
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnVisualTreeChange(ParentChildRelation relation,
                                                  VisualElement element,
                                                  VisualMutationType mutationType) override try {
        // Removals may arrive without Type/Name: always forward them so the
        // registry can drop the handle and allow a later re-add.
        const bool added = mutationType == Add;

        // Reset the single-probe slot when its element is removed, so a later
        // re-add is a fresh experiment.
        if (!added &&
            g_callbackProbe.handle.load() ==
                static_cast<unsigned long long>(element.Handle)) {
            g_callbackProbe.ready.store(false);
            g_callbackProbe.handle.store(0);
            g_callbackProbeReported.store(false);
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
            Log("OBSERVE add OUR TextBox handle=%llu parent=%llu child=%llu "
                "index=%u thread=%lu type='%ls' name='%ls'",
                static_cast<unsigned long long>(element.Handle),
                static_cast<unsigned long long>(relation.Parent),
                static_cast<unsigned long long>(relation.Child),
                relation.ChildIndex, GetCurrentThreadId(),
                element.Type ? element.Type : L"",
                element.Name ? element.Name : L"");
        }
        if (added &&
            (!element.Type || !element.Name ||
             wcscmp(element.Name, L"FileExplorerSearchBox") != 0 ||
             !wcsstr(element.Type, L"AutoSuggestBox"))) {
            return S_OK;
        }
        // Register the XAML sighting in the per-window registry before the
        // legacy probe block returns early. This is what allows correlation
        // even when the callback runs on a different thread.
        RecordObservedBox(reinterpret_cast<uintptr_t>(this), relation, element,
                          added);

        // Single, strictly conditional read-only probe. No fallback: if the
        // callback is not on the owning thread, nothing is called.
        if (kCallbackProbeOnly && g_probeAllowed.load() && added &&
            m_serviceOriginal && relation.Child == element.Handle) {
            auto probeRef = ProbeForHandle(
                static_cast<unsigned long long>(element.Handle));
            auto& result = *probeRef;
            if (result.handle.load() !=
                    static_cast<unsigned long long>(element.Handle) &&
                !result.inProgress.exchange(true)) {
                result.handle.store(static_cast<unsigned long long>(element.Handle));
                result.parent = relation.Parent;
                result.childIndex = relation.ChildIndex;
                result.threadId = GetCurrentThreadId();
                result.ownerThread = m_ownerThreadId;
                result.tidMismatch = (result.threadId != m_ownerThreadId);
                g_probeWatcher = this;

                // Post the work to the invisible channel and return immediately.
                // The WndProc runs on the owner thread after this callback ends.
                if (!result.tidMismatch && m_channelWindow) {
                    auto* request = new (std::nothrow) OwnerRequest();
                    if (request) {
                        request->parent = relation.Parent;
                        request->native = element.Handle;
                        request->index = relation.ChildIndex;
                        if (PostMessageW(m_channelWindow, kOwnerProbeMessage, 0,
                                         reinterpret_cast<LPARAM>(request))) {
                            Log("OWNER posted request from callback thread=%lu",
                                result.threadId);
                        } else {
                            delete request;
                            result.chainHr = HRESULT_FROM_WIN32(GetLastError());
                            result.ready.store(true);
                        }
                    } else {
                        result.chainHr = E_OUTOFMEMORY;
                        result.ready.store(true);
                    }
                } else {
                    result.chainHr = E_UNEXPECTED;
                    result.ready.store(true);
                }
                result.inProgress.store(false);
            }
            return S_OK;
        }

        // Passive phase: only record what XAML reports. No mutation, no query.
        RecordObservedBox(reinterpret_cast<uintptr_t>(this), relation, element, added);
        if (mutationType != Add || !m_active) return S_OK;
        m_isMicrosoftUIXaml =
            wcsncmp(element.Type, L"Microsoft.UI.Xaml.", 18) == 0;
        m_searchElement = element.Handle;
        {
            std::lock_guard<std::recursive_mutex> guard(m_stateMutex);
            m_searchElements.push_back(element.Handle);
        }
        SetEvent(m_searchFoundEvent);
        Log("search XAML element found, type=%ls handle=%llu", element.Type,
            static_cast<unsigned long long>(element.Handle));
        // Do NOT mutate properties from inside the visual-tree callback:
        // re-entrant changes crash Windows.UI.Xaml / Microsoft.UI.Xaml.
        return S_OK;
    } catch (...) {
        Log("visual-tree callback exception");
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnElementStateChanged(InstanceHandle,
                                                     VisualElementState,
                                                     LPCWSTR) noexcept override {
        return S_OK;
    }

    winrt::agile_ref<IXamlDiagnostics> m_diagnostics;
    winrt::agile_ref<IVisualTreeService3> m_service;
    winrt::agile_ref<IXamlDiagnosticsTestHooks> m_testHooks;
    winrt::com_ptr<IVisualTreeService3> m_serviceOriginal;
    winrt::com_ptr<IUnknown> m_site;
    winrt::agile_ref<winrt::Windows::System::DispatcherQueue> m_ownerQueue;
    std::atomic_bool m_probeInProgress{false};
    HHOOK m_ownerHook = nullptr;
    HWND m_channelWindow = nullptr;
    HANDLE m_channelReadyEvent = nullptr;
    // Dedicated apartment for IVisualTreeService3 calls. The interface belongs
    // to the TAP thread, so it must be marshalled before use elsewhere.
    winrt::com_ptr<IStream> m_serviceStream;
    winrt::com_ptr<IVisualTreeService3> m_serviceProxy;
    HANDLE m_executorThread = nullptr;
    bool m_executorStopped = true;
    bool m_adviseStopped = true;
    DWORD m_executorThreadId = 0;
    DWORD m_ownerThreadId = 0;
    HANDLE m_executorReadyEvent = nullptr;
    HANDLE m_executorStopEvent = nullptr;
    HRESULT m_marshalHr = E_FAIL;
    std::atomic<HRESULT> m_unmarshalHr{E_FAIL};
    std::array<PropertyOverride, 64> m_overrides{};
    std::recursive_mutex m_stateMutex;
    winrt::agile_ref<winrt::Microsoft::UI::Dispatching::DispatcherQueue>
        m_dispatcherQueue;
    size_t m_overrideCount = 0;
    std::vector<InstanceHandle> m_hiddenElements;
    std::atomic_bool m_advised{false};
    std::atomic_bool m_active{true};
    std::atomic_bool m_unadviseRequested{false};
    std::atomic_bool m_isMicrosoftUIXaml{false};
    std::atomic<InstanceHandle> m_searchElement{0};
    std::vector<InstanceHandle> m_searchElements;
    std::atomic<HRESULT> m_adviseResult{E_PENDING};
    HANDLE m_adviseThread = nullptr;
    HANDLE m_adviseCompleteEvent = nullptr;
    HANDLE m_searchFoundEvent = nullptr;
};

std::mutex g_hookOwnersMutex;
std::unordered_map<DWORD, VisualTreeWatcher*> g_hookOwners;

LRESULT CALLBACK OwnerGetMessageHook(int code, WPARAM wParam, LPARAM lParam) {
    if (code == HC_ACTION && wParam == PM_REMOVE) {
        const MSG* message = reinterpret_cast<const MSG*>(lParam);
        if (message && message->message == kOwnerInitMessage) {
            std::lock_guard<std::mutex> guard(g_hookOwnersMutex);
            auto it = g_hookOwners.find(GetCurrentThreadId());
            if (it != g_hookOwners.end()) it->second->EnsureChannelWindow();
        }
    }
    return CallNextHookEx(nullptr, code, wParam, lParam);
}

static HRESULT ServiceFindHandleIndex(IVisualTreeService3* service,
                                      InstanceHandle collection,
                                      InstanceHandle target, unsigned* index) {
    unsigned count = 0;
    HRESULT hr = service->GetCollectionCount(collection, &count);
    if (FAILED(hr)) return hr;
    for (unsigned start = 0; start < count; start += 16) {
        unsigned wanted = 16;
        CollectionElementValue* elements = nullptr;
        hr = service->GetCollectionElements(collection, start, &wanted, &elements);
        if (FAILED(hr) || !elements) return hr;
        bool found = false;
        for (unsigned i = 0; i < wanted; ++i) {
            if (VisualTreeWatcher::ParseHandleValue(elements[i].Value) == target) {
                *index = elements[i].Index;
                found = true;
                break;
            }
        }
        for (unsigned i = 0; i < wanted; ++i) {
            SysFreeString(elements[i].ValueType);
            SysFreeString(elements[i].Value);
        }
        CoTaskMemFree(elements);
        if (found) return S_OK;
    }
    return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
}

static std::wstring ServiceSnapshot(IVisualTreeService3* service,
                                    InstanceHandle collection) {
    unsigned count = 0;
    std::wstring result;
    if (FAILED(service->GetCollectionCount(collection, &count))) return result;
    wchar_t buffer[128]{};
    swprintf_s(buffer, L"count=%u [", count);
    result.assign(buffer);
    unsigned wanted = count < 8 ? count : 8;
    CollectionElementValue* elements = nullptr;
    if (SUCCEEDED(service->GetCollectionElements(collection, 0, &wanted, &elements)) &&
        elements) {
        for (unsigned i = 0; i < wanted; ++i) {
            swprintf_s(buffer, L"%s%u:", i ? L" " : L"", elements[i].Index);
            result.append(buffer);
            result.append(elements[i].ValueType ? elements[i].ValueType : L"?");
        }
        for (unsigned i = 0; i < wanted; ++i) {
            SysFreeString(elements[i].ValueType);
            SysFreeString(elements[i].Value);
        }
        CoTaskMemFree(elements);
    }
    result.append(L"]");
    return result;
}

// Read-only walk of the already-built visual tree. HitTest is never used.
// Returns the number of FileExplorerSearchBox elements fed to the registry.
int EnumerateExistingTree(VisualTreeWatcher* watcher) {
    if (!watcher) return 0;
    auto diagnostics = watcher->OwnerDiagnostics();
    if (!diagnostics) return 0;

    winrt::com_ptr<::IInspectable> root{nullptr};
    HRESULT hr = E_FAIL;
    if (IUnknown* site = watcher->OwnerSite()) {
        hr = site->QueryInterface(__uuidof(::IInspectable),
                                  reinterpret_cast<void**>(root.put()));
        Log("TREEWALK site as IInspectable hr=0x%08X", static_cast<unsigned long>(hr));
    }
    if (FAILED(hr) || !root) hr = diagnostics->GetUiLayer(root.put());
    if (FAILED(hr) || !root) {
        Log("TREEWALK GetUiLayer hr=0x%08X", static_cast<unsigned long>(hr));
        return 0;
    }
    InstanceHandle rootHandle = 0;
    hr = diagnostics->GetHandleFromIInspectable(root.get(), &rootHandle);
    Log("TREEWALK root handle=%llu hr=0x%08X",
        static_cast<unsigned long long>(rootHandle),
        static_cast<unsigned long>(hr));
    if (FAILED(hr) || !rootHandle) return 0;

    // The UI layer root often exposes no diagnostics collection. Walk it with
    // WinRT instead: still read-only, no HitTest and no property mutation.
    winrt::Windows::Foundation::IInspectable rootInspectable{nullptr};
    winrt::copy_from_abi(rootInspectable, root.get());
    auto rootElement =
        rootInspectable.try_as<winrt::Microsoft::UI::Xaml::FrameworkElement>();
    if (rootElement) {
        struct WNode {
            winrt::Microsoft::UI::Xaml::FrameworkElement element{nullptr};
            unsigned depth = 0;
        };
        std::vector<WNode> nodesPending{{rootElement, 0}};
        size_t visitedNodes = 0;
        int foundWinrt = 0;
        while (!nodesPending.empty() && visitedNodes < 4000) {
            WNode current = nodesPending.back();
            nodesPending.pop_back();
            if (current.depth > 12 || !current.element) continue;
            ++visitedNodes;
            int childCount = 0;
            try {
                childCount = winrt::Microsoft::UI::Xaml::Media::VisualTreeHelper::
                    GetChildrenCount(current.element);
            } catch (...) {
                continue;
            }
            for (int i = 0; i < childCount; ++i) {
                winrt::Microsoft::UI::Xaml::FrameworkElement child{nullptr};
                try {
                    child = winrt::Microsoft::UI::Xaml::Media::VisualTreeHelper::
                                GetChild(current.element, i)
                                    .try_as<winrt::Microsoft::UI::Xaml::FrameworkElement>();
                } catch (...) {
                    continue;
                }
                if (!child) continue;
                std::wstring name;
                try {
                    name = child.Name().c_str();
                } catch (...) {
                }
                if (name == L"FileExplorerSearchBox") {
                    winrt::com_ptr<::IInspectable> childRaw{nullptr};
                    winrt::com_ptr<::IInspectable> parentRaw{nullptr};
                    childRaw = child.as<::IInspectable>();
                    parentRaw = current.element.as<::IInspectable>();
                    InstanceHandle childHandle = 0;
                    InstanceHandle parentHandle = 0;
                    diagnostics->GetHandleFromIInspectable(childRaw.get(), &childHandle);
                    diagnostics->GetHandleFromIInspectable(parentRaw.get(), &parentHandle);
                    ParentChildRelation relation{parentHandle, childHandle,
                                                 static_cast<unsigned int>(i)};
                    VisualElement element{};
                    element.Handle = childHandle;
                    element.Name = const_cast<wchar_t*>(L"FileExplorerSearchBox");
                    element.Type = const_cast<wchar_t*>(
                        L"Microsoft.UI.Xaml.Controls.AutoSuggestBox");
                    RecordObservedBox(reinterpret_cast<uintptr_t>(watcher),
                                      relation, element, true);
                    ++foundWinrt;
                }
                nodesPending.push_back({child, current.depth + 1});
            }
        }
        Log("TREEWALK winrt done nodes=%zu found=%d", visitedNodes, foundWinrt);
        return foundWinrt;
    }

    struct Node {
        InstanceHandle handle;
        unsigned depth;
    };
    std::vector<Node> pendingNodes{{rootHandle, 0}};
    std::unordered_set<unsigned long long> visited;
    constexpr unsigned kMaxDepth = 12;
    constexpr size_t kMaxNodes = 4000;
    int found = 0;
    size_t nodes = 0;

    while (!pendingNodes.empty() && nodes < kMaxNodes) {
        Node node = pendingNodes.back();
        pendingNodes.pop_back();
        if (node.depth > kMaxDepth) continue;
        const auto key = static_cast<unsigned long long>(node.handle);
        if (!visited.insert(key).second) continue;
        ++nodes;

        auto service = watcher->OwnerService();
        if (!service) break;
        PropertyChainData data;
        const HRESULT chainHr = service->GetPropertyValuesChain(
            node.handle, &data.sourceCount, &data.sources, &data.valueCount,
            &data.values);
        if (FAILED(chainHr)) continue;

        bool isSearchBox = false;
        bool isAutoSuggest = false;
        for (unsigned int i = 0; i < data.valueCount; ++i) {
            const auto& value = data.values[i];
            if (value.PropertyName && value.Value &&
                wcscmp(value.PropertyName, L"Name") == 0 &&
                wcscmp(value.Value, L"FileExplorerSearchBox") == 0) {
                isSearchBox = true;
            }
            if (value.PropertyName && value.Value &&
                wcscmp(value.PropertyName, L"Name") == 0 &&
                wcsstr(value.Value, L"SearchBox") != nullptr) {
                isSearchBox = true;
            }
            for (unsigned int s = 0; s < data.sourceCount; ++s) {
                if (data.sources[s].TargetType &&
                    wcsstr(data.sources[s].TargetType, L"AutoSuggestBox")) {
                    isAutoSuggest = true;
                }
            }
        }
        if (isSearchBox && isAutoSuggest) {
            ParentChildRelation relation{0, node.handle, 0};
            VisualElement element{};
            element.Handle = node.handle;
            element.Name = const_cast<wchar_t*>(L"FileExplorerSearchBox");
            element.Type = const_cast<wchar_t*>(L"Microsoft.UI.Xaml.Controls.AutoSuggestBox");
            RecordObservedBox(reinterpret_cast<uintptr_t>(watcher), relation,
                              element, true);
            ++found;
        }

        for (unsigned int i = 0; i < data.valueCount; ++i) {
            const auto& value = data.values[i];
            if ((value.MetadataBits &
                 static_cast<hyper>(IsValueCollection)) == 0) continue;
            if ((value.MetadataBits &
                 static_cast<hyper>(IsValueCollectionReadOnly)) != 0) continue;
            const InstanceHandle collection =
                VisualTreeWatcher::ParseHandleValue(value.Value);
            if (!collection) continue;
            unsigned int count = 0;
            if (FAILED(service->GetCollectionCount(collection, &count)) ||
                count == 0 || count > 256) {
                continue;
            }
            unsigned int wanted = count;
            CollectionElementValue* elements = nullptr;
            if (FAILED(service->GetCollectionElements(collection, 0, &wanted,
                                                       &elements)) ||
                !elements) {
                continue;
            }
            for (unsigned int e = 0; e < wanted; ++e) {
                if ((elements[e].MetadataBits &
                     static_cast<hyper>(IsValueHandle)) != 0) {
                    const InstanceHandle child =
                        VisualTreeWatcher::ParseHandleValue(elements[e].Value);
                    if (child) pendingNodes.push_back({child, node.depth + 1});
                }
                SysFreeString(elements[e].ValueType);
                SysFreeString(elements[e].Value);
            }
            CoTaskMemFree(elements);
        }
    }
    Log("TREEWALK done nodes=%zu found=%d", nodes, found);
    return found;
}

LRESULT CALLBACK OwnerChannelProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* self = reinterpret_cast<VisualTreeWatcher*>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == kOwnerProbeMessage && self && lParam) {
        auto* request = reinterpret_cast<OwnerRequest*>(lParam);
        auto probeRef = ProbeForHandle(
            static_cast<unsigned long long>(request->native));
        auto& result = *probeRef;
        result.execThread = GetCurrentThreadId();
        Log("OWNER WndProc executing thread=%lu", result.execThread);
        self->ProbeOnOwnerThread(request->parent, request->index, request->native,
                                 &result);
        result.ready.store(true);
        delete request;
        return 0;
    }
    if (message == kOwnerAddMessage && self && lParam) {
        auto* request = reinterpret_cast<OwnerAddRequest*>(lParam);
        auto state = request->state;
        if (!state || request->generation != state->generation ||
            state->phase.load() != WindowPhase::AddPending) {
            Log("OWNER ADD ABORT: stale request");
            delete request;
            return 0;
        }
        state->phase.store(WindowPhase::Replacing);
        auto& tx = *state->addTx;
        auto* service = self->OwnerService();
        Log("OWNER ADD begin thread=%lu collection=%llu native=%llu "
            "created=%llu index=%u",
            GetCurrentThreadId(),
            static_cast<unsigned long long>(request->collection),
            static_cast<unsigned long long>(request->native),
            static_cast<unsigned long long>(request->created), request->index);
        if (!service) {
            tx.addHr = E_UNEXPECTED;
            tx.ready.store(true);
            state->phase.store(WindowPhase::Failed);
            delete request;
            return 0;
        }

        // 1) Re-verify the collection and the native element before writing.
        tx.countHr = service->GetCollectionCount(request->collection,
                                                  &tx.beforeSize);
        tx.nativeBeforeHr = ServiceFindHandleIndex(
            service, request->collection, request->native, &tx.nativeBeforeIndex);
        Log("OWNER ADD before countHr=0x%08X size=%u nativeHr=0x%08X index=%u",
            static_cast<unsigned long>(tx.countHr), tx.beforeSize,
            static_cast<unsigned long>(tx.nativeBeforeHr), tx.nativeBeforeIndex);
        const std::wstring before = ServiceSnapshot(service, request->collection);
        Log("OWNER ADD snapshot %ls", before.c_str());

        const bool safeToWrite =
            SUCCEEDED(tx.countHr) && tx.beforeSize == 3 &&
            SUCCEEDED(tx.nativeBeforeHr) && tx.nativeBeforeIndex == request->index;
        if (!safeToWrite) {
            Log("OWNER ADD ABORT: collection changed, tree untouched");
            tx.addHr = E_ABORT;
            tx.ready.store(true);
            state->phase.store(WindowPhase::Failed);
            delete request;
            return 0;
        }

        // 2) Insert at native index + 1. The native control is not touched.
        const unsigned insertAt = request->index + 1;
        tx.addHr = service->AddChild(request->collection, request->created, insertAt);
        Log("OWNER ADD AddChild hr=0x%08X index=%u",
            static_cast<unsigned long>(tx.addHr), insertAt);
        if (FAILED(tx.addHr)) state->phase.store(WindowPhase::Failed);

        const std::wstring after = ServiceSnapshot(service, request->collection);
        Log("OWNER ADD after %ls", after.c_str());
        tx.afterCountHr = service->GetCollectionCount(request->collection,
                                                       &tx.afterSize);
        tx.ourHr = ServiceFindHandleIndex(
            service, request->collection, request->created, &tx.ourIndex);
        tx.nativeAfterHr = ServiceFindHandleIndex(
            service, request->collection, request->native, &tx.nativeAfterIndex);
        Log("OWNER ADD verify countHr=0x%08X size=%u ourHr=0x%08X ourIndex=%u "
            "nativeHr=0x%08X nativeIndex=%u",
            static_cast<unsigned long>(tx.afterCountHr), tx.afterSize,
            static_cast<unsigned long>(tx.ourHr), tx.ourIndex,
            static_cast<unsigned long>(tx.nativeAfterHr), tx.nativeAfterIndex);

        // 2b.3: only now remove the native control, after our TextBox is
        // confirmed present at index + 1.
        if (request->replaceNative && SUCCEEDED(tx.addHr) && SUCCEEDED(tx.ourHr)) {
            tx.nativeRemoveFindHr = ServiceFindHandleIndex(
                service, request->collection, request->native,
                &tx.nativeRemoveIndex);
            // Keep the native object alive: its handle becomes invalid as soon
            // as it is removed, so rollback must register it again.
            auto diagnostics = self->OwnerDiagnostics();
            HRESULT objectHr = E_UNEXPECTED;
            if (diagnostics && SUCCEEDED(tx.nativeRemoveFindHr)) {
                objectHr = diagnostics->GetIInspectableFromHandle(
                    request->native, state->originalNative.put());
            }
            Log("OWNER ADD native object hr=0x%08X object=%p",
                static_cast<unsigned long>(objectHr),
                state->originalNative.get());
            if (SUCCEEDED(tx.nativeRemoveFindHr)) {
                tx.nativeRemoveHr = service->RemoveChild(
                    request->collection, tx.nativeRemoveIndex);
            }
            Log("OWNER ADD native RemoveChild findHr=0x%08X index=%u "
                "removeHr=0x%08X",
                static_cast<unsigned long>(tx.nativeRemoveFindHr),
                tx.nativeRemoveIndex, static_cast<unsigned long>(tx.nativeRemoveHr));
            const std::wstring replaced = ServiceSnapshot(service, request->collection);
            Log("OWNER ADD replaced %ls", replaced.c_str());
            tx.afterRemoveCountHr = service->GetCollectionCount(
                request->collection, &tx.afterRemoveSize);
            tx.nativeGoneHr = ServiceFindHandleIndex(
                service, request->collection, request->native, &tx.nativeRemoveIndex);
            tx.ourAfterRemoveHr = ServiceFindHandleIndex(
                service, request->collection, request->created,
                &tx.ourAfterRemoveIndex);
            Log("OWNER ADD replaced verify countHr=0x%08X size=%u "
                "nativeGoneHr=0x%08X ourHr=0x%08X ourIndex=%u",
                static_cast<unsigned long>(tx.afterRemoveCountHr), tx.afterRemoveSize,
                static_cast<unsigned long>(tx.nativeGoneHr),
                static_cast<unsigned long>(tx.ourAfterRemoveHr),
                tx.ourAfterRemoveIndex);
        }

        // The replacement state stays in the tree for a few seconds. Rollback
        // is a separate message so the owner thread is never blocked here.
        if (SUCCEEDED(tx.addHr) && SUCCEEDED(tx.ourHr)) {
            state->insertTick = GetTickCount64();
            state->replacementActive = true;
            g_replacementActive.store(true);
            state->phase.store(WindowPhase::Active);
            Log("OWNER ADD replacement active frame=%p gen=%llu",
                state->frame,
                static_cast<unsigned long long>(state->generation));
        }
        tx.ready.store(true);
        delete request;
        return 0;
    }
    if (message == kOwnerRollbackMessage && self && lParam) {
        auto* request = reinterpret_cast<OwnerAddRequest*>(lParam);
        auto state = request->state;
        auto& rb = *state->rollbackTx;
        state->phase.store(WindowPhase::RestorePending);
        auto* service = self->OwnerService();
        Log("OWNER ROLLBACK begin thread=%lu", GetCurrentThreadId());
        if (!service) {
            rb.removeHr = E_UNEXPECTED;
            rb.ready.store(true);
            delete request;
            return 0;
        }
        if (request->replaceNative) {
            // The old handle is invalid after removal: register the saved
            // object again to obtain a fresh handle, then restore its index.
            InstanceHandle restored = 0;
            auto diagnostics = self->OwnerDiagnostics();
            if (diagnostics && state->originalNative) {
                rb.restoreHr = diagnostics->RegisterInstance(
                    state->originalNative.get(), &restored);
            }
            Log("OWNER ROLLBACK RegisterInstance(native) hr=0x%08X fresh=%llu",
                static_cast<unsigned long>(rb.restoreHr),
                static_cast<unsigned long long>(restored));
            if (SUCCEEDED(rb.restoreHr) && restored) {
                rb.restoredHandle = restored;
                rb.restoreHr = service->AddChild(
                    request->collection, restored, request->index);
            }
            Log("OWNER ROLLBACK restore native hr=0x%08X index=%u",
                static_cast<unsigned long>(rb.restoreHr), request->index);
        }
        rb.findHr = ServiceFindHandleIndex(
            service, request->collection, request->created, &rb.index);
        Log("OWNER ROLLBACK findHr=0x%08X currentIndex=%u",
            static_cast<unsigned long>(rb.findHr), rb.index);
        if (SUCCEEDED(rb.findHr)) {
            rb.removeHr = service->RemoveChild(request->collection, rb.index);
            Log("OWNER ROLLBACK RemoveChild hr=0x%08X index=%u",
                static_cast<unsigned long>(rb.removeHr), rb.index);
        }
        const std::wstring final = ServiceSnapshot(service, request->collection);
        Log("OWNER ROLLBACK final %ls", final.c_str());
        rb.countHr = service->GetCollectionCount(request->collection, &rb.size);
        rb.ourGoneHr = ServiceFindHandleIndex(
            service, request->collection, request->created, &rb.index);
        const InstanceHandle nativeToVerify = rb.restoredHandle
                                                  ? static_cast<InstanceHandle>(rb.restoredHandle)
                                                  : request->native;
        rb.nativeHr = ServiceFindHandleIndex(
            service, request->collection, nativeToVerify, &rb.nativeIndex);
        Log("OWNER ROLLBACK RESULT countHr=0x%08X size=%u ourGoneHr=0x%08X "
            "nativeHr=0x%08X nativeIndex=%u",
            static_cast<unsigned long>(rb.countHr), rb.size,
            static_cast<unsigned long>(rb.ourGoneHr),
            static_cast<unsigned long>(rb.nativeHr), rb.nativeIndex);
        rb.ready.store(true);
        state->replacementActive = false;
        state->rollbackPosted = false;
        state->phase.store(WindowPhase::XamlMapped);
        g_replacementActive.store(false);
        g_directTextHasFocus.store(false);
        delete request;
        return 0;
    }
    if (message == kOwnerEnumerateMessage && self) {
        EnumerateExistingTree(self);
        return 0;
    }
    if (message == kOwnerFocusMessage && self) {
        const bool queued = self->PostToDispatcher([] {
            if (!g_createdTextBox) return;
            const bool focused = g_createdTextBox.Focus(
                winrt::Microsoft::UI::Xaml::FocusState::Programmatic);
            Log("DIRECT FOCUS result=%d dispatcherThread=%lu",
                focused ? 1 : 0, GetCurrentThreadId());
        });
        Log("OWNER FOCUS queuedToXamlDispatcher=%d ownerThread=%lu",
            queued ? 1 : 0, GetCurrentThreadId());
        return 0;
    }
    if (message == kOwnerBackspaceMessage && self) {
        const bool queued = self->PostToDispatcher([] {
            if (!g_createdTextBox) return;
            auto box = g_createdTextBox;
            const auto selected = box.SelectedText();
            if (!selected.empty()) {
                box.SelectedText(L"");
            } else {
                const auto current = box.Text();
                const int32_t caret = box.SelectionStart();
                if (caret > 0 && !current.empty()) {
                    std::wstring updated(current.c_str(), current.size());
                    const size_t eraseAt = static_cast<size_t>(
                        std::min<int32_t>(caret - 1,
                                          static_cast<int32_t>(updated.size() - 1)));
                    updated.erase(eraseAt, 1);
                    box.Text(winrt::hstring(updated));
                    box.SelectionStart(static_cast<int32_t>(eraseAt));
                    box.SelectionLength(0);
                }
            }
        });
        Log("OWNER BACKSPACE queuedToXamlDispatcher=%d", queued ? 1 : 0);
        return 0;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

void VisualTreeWatcher::EnsureChannelWindow() {
    if (m_channelWindow && IsWindow(m_channelWindow)) return;
    // Derive a class name unique to this DLL load. Window classes registered by
    // a previous mod instance persist in the process after unload; reusing the
    // fixed name makes RegisterClassExW fail with ERROR_CLASS_ALREADY_EXISTS
    // and leaves the channel window null, crashing the marshaling executor.
    static wchar_t uniqueClass[128]{};
    if (!uniqueClass[0]) {
        HMODULE module = nullptr;
        GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&OwnerChannelProc), &module);
        swprintf_s(uniqueClass, ARRAYSIZE(uniqueClass), L"%s_%p",
                   kOwnerWindowClass, module);
    }
    static ATOM windowClass = 0;
    if (!windowClass) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = OwnerChannelProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = uniqueClass;
        windowClass = RegisterClassExW(&wc);
        if (!windowClass) Log("OWNER RegisterClass failed err=%lu", GetLastError());
    }
    if (!windowClass) return;

    m_channelWindow = CreateWindowExW(0, uniqueClass, L"", 0, 0, 0, 0, 0,
                                      HWND_MESSAGE, nullptr, nullptr, this);
    if (!m_channelWindow) {
        Log("OWNER channel creation failed err=%lu", GetLastError());
        return;
    }
    SetWindowLongPtrW(m_channelWindow, GWLP_USERDATA,
                      reinterpret_cast<LONG_PTR>(this));
    SetEvent(m_channelReadyEvent);
    Log("OWNER channel created hwnd=%p tid=%lu pid=%lu",
        m_channelWindow, GetCurrentThreadId(), GetCurrentProcessId());
}

HRESULT VisualTreeWatcher::ProbeCollectionViaOwner(
    InstanceHandle, unsigned, InstanceHandle, CollectionProbe*) {
    // Dormant: the hook-based path was removed after it crashed Explorer.
    return E_NOTIMPL;
}

HRESULT VisualTreeWatcher::ProbeOnOwnerThread(
    InstanceHandle parent, unsigned childIndex, InstanceHandle nativeHandle,
    CallbackProbeResult* out) {
    auto service = m_serviceOriginal.get();
    if (!service || !out) return E_UNEXPECTED;
    Log("DIAGPROBE owner thread=%lu pid=%lu service=%p", GetCurrentThreadId(),
        GetCurrentProcessId(), static_cast<void*>(service));

    PropertyChainData data;
    const HRESULT chainHr = service->GetPropertyValuesChain(
        parent, &data.sourceCount, &data.sources, &data.valueCount, &data.values);
    Log("DIAGPROBE GetPropertyValuesChain hr=0x%08X values=%u",
        static_cast<unsigned long>(chainHr), data.valueCount);
    out->chainHr = chainHr;
    if (FAILED(chainHr)) return chainHr;

    out->valueCount = data.valueCount;
    for (unsigned int i = 0; i < data.valueCount; ++i) {
        const auto& value = data.values[i];
        if (value.PropertyName && value.Value &&
            out->layoutCount < out->layoutProps.size()) {
            const wchar_t* name = value.PropertyName;
            const bool layoutName =
                wcsstr(name, L"Column") || wcsstr(name, L"Row") ||
                wcsstr(name, L"Width") || wcsstr(name, L"Height") ||
                wcsstr(name, L"Margin") || wcsstr(name, L"Alignment");
            if (!layoutName) continue;
            auto& saved = out->layoutProps[out->layoutCount++];
            wcsncpy_s(saved.name, name, _TRUNCATE);
            wcsncpy_s(saved.value, value.Value, _TRUNCATE);
            saved.chainIndex = value.PropertyChainIndex;
            if (value.DeclaringType) {
                wcsncpy_s(saved.declaring, value.DeclaringType, _TRUNCATE);
            }
        }
        if ((value.MetadataBits & static_cast<hyper>(IsValueCollection)) == 0) continue;
        if ((value.MetadataBits &
             static_cast<hyper>(IsValueCollectionReadOnly)) != 0) {
            continue;
        }

        const InstanceHandle candidate = ParseHandleValue(value.Value);
        if (!candidate) continue;

        unsigned int count = 0;
        if (FAILED(service->GetCollectionCount(candidate, &count)) ||
            childIndex >= count) {
            continue;
        }
        unsigned int wanted = 1;
        CollectionElementValue* probeElements = nullptr;
        if (FAILED(service->GetCollectionElements(candidate, childIndex,
                                                   &wanted, &probeElements)) ||
            wanted == 0 || !probeElements) {
            continue;
        }
        const bool nativeHere =
            ParseHandleValue(probeElements[0].Value) == nativeHandle;
        for (unsigned int e = 0; e < wanted; ++e) {
            SysFreeString(probeElements[e].ValueType);
            SysFreeString(probeElements[e].Value);
        }
        CoTaskMemFree(probeElements);
        if (!nativeHere) continue;

        ++out->collectionCount;
        if (out->firstCollection != 0) continue;
        out->firstCollection = static_cast<unsigned long long>(candidate);
        out->firstCollectionIndex = i;
        out->collectionMetadata = value.MetadataBits;
        if (value.PropertyName) {
            wcsncpy_s(out->propertyName, value.PropertyName, _TRUNCATE);
        }
        if (value.ItemType) {
            wcsncpy_s(out->itemType, value.ItemType, _TRUNCATE);
        }
        if (value.ValueType) {
            wcsncpy_s(out->valueType, value.ValueType, _TRUNCATE);
        }
    }

    if (out->firstCollection == 0) {
        // Some visual trees expose the owning collection only as the parent
        // reported by OnVisualTreeChange. Verify it the same way.
        unsigned int count = 0;
        unsigned int wanted = 1;
        CollectionElementValue* elements = nullptr;
        const HRESULT fallbackCountHr = service->GetCollectionCount(parent, &count);
        HRESULT fallbackElementsHr = E_FAIL;
        unsigned long long parsed = 0;
        if (SUCCEEDED(fallbackCountHr) && childIndex < count) {
            fallbackElementsHr = service->GetCollectionElements(
                parent, childIndex, &wanted, &elements);
            if (wanted > 0 && elements) {
                parsed = static_cast<unsigned long long>(
                    ParseHandleValue(elements[0].Value));
            }
        }
        Log("DIAGPROBE fallback parent=%llu countHr=0x%08X count=%u "
            "elementsHr=0x%08X wanted=%u parsed=%llu native=%llu",
            static_cast<unsigned long long>(parent),
            static_cast<unsigned long>(fallbackCountHr), count,
            static_cast<unsigned long>(fallbackElementsHr), wanted, parsed,
            static_cast<unsigned long long>(nativeHandle));
        if (SUCCEEDED(fallbackCountHr) && childIndex < count &&
            SUCCEEDED(fallbackElementsHr) && wanted > 0 && elements &&
            parsed == static_cast<unsigned long long>(nativeHandle)) {
            out->firstCollection = static_cast<unsigned long long>(parent);
            out->collectionSize = count;
            out->collectionMetadata = IsValueCollection;
        }
        if (elements) {
            for (unsigned int e = 0; e < wanted; ++e) {
                SysFreeString(elements[e].ValueType);
                SysFreeString(elements[e].Value);
            }
            CoTaskMemFree(elements);
        }
    }

    if (out->firstCollection == 0) return S_FALSE;
    out->countHr = service->GetCollectionCount(
        static_cast<InstanceHandle>(out->firstCollection), &out->collectionSize);
    Log("DIAGPROBE GetCollectionCount hr=0x%08X size=%u",
        static_cast<unsigned long>(out->countHr), out->collectionSize);
    if (FAILED(out->countHr) || out->collectionSize == 0) return out->countHr;

    unsigned int requested = out->collectionSize < 16 ? out->collectionSize : 16;
    CollectionElementValue* elements = nullptr;
    out->elementsHr = service->GetCollectionElements(
        static_cast<InstanceHandle>(out->firstCollection), 0, &requested, &elements);
    Log("DIAGPROBE GetCollectionElements hr=0x%08X count=%u",
        static_cast<unsigned long>(out->elementsHr), requested);
    if (SUCCEEDED(out->elementsHr) && elements) {
        out->elementCount = requested < 16 ? requested : 16;
        for (unsigned int e = 0; e < out->elementCount; ++e) {
            auto& saved = out->elements[e];
            saved.index = elements[e].Index;
            saved.bits = elements[e].MetadataBits;
            if (elements[e].ValueType) {
                wcsncpy_s(saved.valueType, elements[e].ValueType, _TRUNCATE);
            }
            if ((elements[e].MetadataBits & static_cast<hyper>(IsValueHandle)) != 0) {
                saved.handle = static_cast<unsigned long long>(
                    ParseHandleValue(elements[e].Value));
            } else if (elements[e].Value) {
                wcsncpy_s(saved.value, elements[e].Value, _TRUNCATE);
            }
        }
    }
    if (elements) {
        for (unsigned int e = 0; e < requested; ++e) {
            SysFreeString(elements[e].ValueType);
            SysFreeString(elements[e].Value);
        }
        CoTaskMemFree(elements);
    }
    return S_OK;
}

// ------------------------------------------------------------------ watchers
// A single diagnostics connection sees the visual trees of all Explorer
// windows in the process. Keep every watcher, and keep every hidden element:
// restoring the overrides of one window must not bring another one back.
std::mutex g_watchersMutex;
std::vector<winrt::com_ptr<VisualTreeWatcher>> g_watchers;

void RegisterWatcher(winrt::com_ptr<VisualTreeWatcher> watcher) {
    std::lock_guard<std::mutex> guard(g_watchersMutex);
    g_watchers.push_back(std::move(watcher));
}

void UnregisterWatcher(VisualTreeWatcher* watcher) {
    if (!watcher) return;
    std::lock_guard<std::mutex> guard(g_watchersMutex);
    g_watchers.erase(
        std::remove_if(g_watchers.begin(), g_watchers.end(),
                       [watcher](const winrt::com_ptr<VisualTreeWatcher>& item) {
                           return item.get() == watcher;
                       }),
        g_watchers.end());
}

std::vector<winrt::com_ptr<VisualTreeWatcher>> SnapshotWatchers() {
    std::lock_guard<std::mutex> guard(g_watchersMutex);
    return g_watchers;
}

enum class ObservedSource { XamlMapped, UiaDiscovered };

// A XAML handle is only unique together with the process and the diagnostics
// connection that reported it. UIA discoveries carry owner = frame HWND and
// handle = 0, so no InstanceHandle is invented for them.
struct ObservedKey {
    DWORD pid = 0;
    uintptr_t owner = 0;
    InstanceHandle handle = 0;
    bool operator<(const ObservedKey& other) const {
        if (pid != other.pid) return pid < other.pid;
        if (owner != other.owner) return owner < other.owner;
        return handle < other.handle;
    }
};

struct ObservedSearchBox {
    ObservedKey key;
    InstanceHandle parent = 0;
    InstanceHandle child = 0;
    unsigned int childIndex = 0;
    DWORD threadId = 0;
    ULONGLONG tick = 0;
    std::wstring type;
    std::wstring name;
    ObservedSource source = ObservedSource::XamlMapped;
    HWND frame = nullptr;
    RECT rect{};
};

std::mutex g_observedMutex;
std::map<ObservedKey, ObservedSearchBox> g_observedBoxes;

void RecordObservedBox(uintptr_t watcher, const ParentChildRelation& relation,
                       const VisualElement& element, bool added) {
    ObservedKey key{GetCurrentProcessId(), watcher, element.Handle};
    std::lock_guard<std::mutex> guard(g_observedMutex);
    if (!added) {
        const size_t erased = g_observedBoxes.erase(key);
        if (erased) {
            Log("OBSERVE remove pid=%lu watcher=%p handle=%llu total=%zu",
                key.pid, reinterpret_cast<void*>(key.owner),
                static_cast<unsigned long long>(key.handle),
                g_observedBoxes.size());
        }
        {
            std::lock_guard<std::mutex> stateGuard(g_windowStatesMutex);
            g_xamlPending.erase(element.Handle);
            for (auto it = g_windowStates.begin(); it != g_windowStates.end();) {
                if (it->second->nativeHandle == element.Handle &&
                    !it->second->replacementActive && !it->second->mutating) {
                    it = g_windowStates.erase(it);
                } else {
                    ++it;
                }
            }
        }
        return;
    }
    if (g_observedBoxes.count(key)) {
        Log("OBSERVE duplicate ignored pid=%lu watcher=%p handle=%llu",
            key.pid, reinterpret_cast<void*>(key.owner),
            static_cast<unsigned long long>(key.handle));
        return;
    }
    ObservedSearchBox box;
    box.key = key;
    box.parent = relation.Parent;
    box.child = relation.Child;
    box.childIndex = relation.ChildIndex;
    box.threadId = GetCurrentThreadId();
    box.tick = GetTickCount64();
    if (element.Type) box.type.assign(element.Type, wcslen(element.Type));
    if (element.Name) box.name.assign(element.Name, wcslen(element.Name));
    g_observedBoxes.emplace(key, std::move(box));

    winrt::com_ptr<VisualTreeWatcher> watcherRef;
    for (auto& candidate : SnapshotWatchers()) {
        if (reinterpret_cast<uintptr_t>(candidate.get()) == watcher) {
            watcherRef = candidate;
            break;
        }
    }
    {
        std::lock_guard<std::mutex> stateGuard(g_windowStatesMutex);
        auto& state = g_xamlPending[element.Handle];
        if (!state) {
            state = std::make_shared<WindowState>();
            state->pid = key.pid;
            // The tab that owns a newly added search box is the active one:
            // its ShellTabWindowClass is the first visible tab of the frame.
            const HWND foreground = ::GetForegroundWindow();
            state->tab = ActiveTabWindow(foreground);
            state->generation = g_windowGeneration.fetch_add(1);
            state->xamlMapped = true;
            state->phase = WindowPhase::XamlMapped;
        }
        state->watcher = watcherRef;
        state->threadId = g_observedBoxes.find(key)->second.threadId;
        {
            std::lock_guard<std::mutex> probeGuard(g_probeByHandleMutex);
            auto pit = g_probeByHandle.find(
                static_cast<unsigned long long>(element.Handle));
            if (pit != g_probeByHandle.end()) state->probe = pit->second;
        }
        state->nativeHandle = element.Handle;
        state->parentHandle = relation.Parent;
        state->nativeIndex = relation.ChildIndex;
    }

    const auto& saved = g_observedBoxes.find(key)->second;
    Log("OBSERVE add pid=%lu watcher=%p handle=%llu parent=%llu child=%llu "
        "index=%u thread=%lu type='%ls' name='%ls' total=%zu",
        key.pid, reinterpret_cast<void*>(key.owner),
        static_cast<unsigned long long>(key.handle),
        static_cast<unsigned long long>(saved.parent),
        static_cast<unsigned long long>(saved.child), saved.childIndex,
        saved.threadId, saved.type.c_str(), saved.name.c_str(),
        g_observedBoxes.size());
}

// UIA inventory of an already-open frame. It only reports that the control
// exists; it never fabricates XAML handles, parents or collection indices.
void RecordUiaDiscovered(HWND frame, const RECT& rect) {
    DWORD pid = 0;
    GetWindowThreadProcessId(frame, &pid);
    ObservedKey key{pid, reinterpret_cast<uintptr_t>(frame), 0};
    std::lock_guard<std::mutex> guard(g_observedMutex);
    if (g_observedBoxes.count(key)) {
        Log("UIA DISCOVERED duplicate frame=%p pid=%lu", frame, pid);
        return;
    }
    ObservedSearchBox box;
    box.key = key;
    box.source = ObservedSource::UiaDiscovered;
    box.frame = frame;
    box.rect = rect;
    box.threadId = GetWindowThreadProcessId(frame, nullptr);
    box.name = L"FileExplorerSearchBox";
    box.type = L"UIA_DISCOVERED";
    g_observedBoxes.emplace(key, std::move(box));
    {
        std::lock_guard<std::mutex> guard(g_windowStatesMutex);
        const HWND tab = TabWindowForRect(frame, rect);
        const HWND key = tab ? tab : frame;
        auto& state = g_windowStates[key];
        if (!state) {
            state = std::make_shared<WindowState>();
            state->frame = frame;
            state->tab = key;
            state->pid = pid;
            state->threadId = GetWindowThreadProcessId(frame, nullptr);
            state->generation = g_windowGeneration.fetch_add(1);
        }
        state->nativeRect = rect;
        state->uiaDiscovered = true;
        if (!state->xamlMapped) state->phase = WindowPhase::Discovered;
    }
    Log("UIA DISCOVERED frame=%p pid=%lu rect=%ld,%ld %ldx%ld total=%zu",
        frame, pid, rect.left, rect.top, rect.right - rect.left,
        rect.bottom - rect.top, g_observedBoxes.size());
}

void ForgetUiaFrame(HWND frame) {
    DWORD pid = 0;
    GetWindowThreadProcessId(frame, &pid);
    ObservedKey key{pid, reinterpret_cast<uintptr_t>(frame), 0};
    std::lock_guard<std::mutex> guard(g_observedMutex);
    if (g_observedBoxes.erase(key)) {
        Log("UIA DISCOVERED removed frame=%p pid=%lu total=%zu", frame, pid,
            g_observedBoxes.size());
    }
}

// Phase 2a is deliberately limited to ONE verified XAML_MAPPED window. It
// creates a TextBox and registers it with diagnostics, but never inserts it.
constexpr bool kPhase2CreateOnly = true;
winrt::Microsoft::UI::Xaml::Controls::TextBox g_createdTextBox{nullptr};
InstanceHandle g_createdHandle = 0;
DWORD g_experimentPid = 0;
HWND g_experimentFrame = nullptr;
bool g_experimentStarted = false;

constexpr bool kPhase2bAddChild = false;
constexpr wchar_t kExperimentMutexName[] =
    L"Local\\EverythingExplorerSearch_DirectXamlExperiment";
HANDLE g_experimentMutex = nullptr;
winrt::com_ptr<VisualTreeWatcher> g_experimentWatcher;
InstanceHandle g_experimentElement = 0;
InstanceHandle g_experimentParent = 0;
unsigned g_experimentIndex = 0;
bool g_addAttempted = false;

// ------------------------------------------------ search command-bar buttons

static void CloseSearchForFrame(HWND frame, const std::shared_ptr<WindowState>& state) {
    if (state && state->closingSearch.exchange(true)) return;
    std::wstring origin = state ? state->origin : std::wstring();
    if (origin.empty()) {
        esx::QueryRequest request{};
        if (esx::ReadRequestForWindow(frame, request)) origin = request.folder;
    }
    if (origin.empty()) origin = L"shell:MyComputerFolder";

    PIDLIST_ABSOLUTE pidl = nullptr;
    const HRESULT parseHr = SHParseDisplayName(origin.c_str(), nullptr, &pidl, 0, nullptr);
    if (FAILED(parseHr) || !pidl) {
        Log("close search: cannot parse origin='%ls' hr=0x%08X", origin.c_str(), parseHr);
        if (state) state->closingSearch.store(false);
        return;
    }
    winrt::com_ptr<IShellBrowser> browser;
    if (GetBrowserForWindow(frame, browser) && browser) {
        const HRESULT hr = browser->BrowseObject(pidl, SBSP_ABSOLUTE | SBSP_SAMEBROWSER);
        Log("close search frame=%p origin='%ls' hr=0x%08X", frame, origin.c_str(), hr);
    }
    if (state) state->closingSearch.store(false);
    ::CoTaskMemFree(pidl);
}

static winrt::Microsoft::UI::Xaml::DependencyObject FindRootVisual(
    winrt::Microsoft::UI::Xaml::DependencyObject element) {
    auto current = element;
    for (int guard = 0; guard < 64 && current; ++guard) {
        auto parent = winrt::Microsoft::UI::Xaml::Media::VisualTreeHelper::GetParent(current);
        if (!parent) break;
        current = parent;
    }
    return current;
}

template <typename T>
static T FindDescendant(winrt::Microsoft::UI::Xaml::DependencyObject root, int depth = 0) {
    if (!root || depth > 10) return nullptr;
    if (auto typed = root.try_as<T>()) return typed;
    const int count = winrt::Microsoft::UI::Xaml::Media::VisualTreeHelper::GetChildrenCount(root);
    for (int i = 0; i < count; ++i) {
        auto child = winrt::Microsoft::UI::Xaml::Media::VisualTreeHelper::GetChild(root, i);
        if (auto found = FindDescendant<T>(child, depth + 1)) return found;
    }
    return nullptr;
}

template <typename T>
static void CollectDescendants(winrt::Microsoft::UI::Xaml::DependencyObject root,
                               std::vector<T>& out, int depth = 0) {
    if (!root || depth > 10) return;
    if (auto typed = root.try_as<T>()) out.push_back(typed);
    const int count = winrt::Microsoft::UI::Xaml::Media::VisualTreeHelper::GetChildrenCount(root);
    for (int i = 0; i < count; ++i) {
        CollectDescendants<T>(
            winrt::Microsoft::UI::Xaml::Media::VisualTreeHelper::GetChild(root, i), out,
            depth + 1);
    }
}

static HWND RequestKeyForFrame(HWND frame) {
    const HWND tab = ActiveTabWindow(frame);
    return tab ? tab : frame;
}

static bool HasFilterToken(const std::wstring& filter, const wchar_t* token) {
    if (!token || !*token) return false;
    std::size_t pos = 0;
    while (pos < filter.size()) {
        while (pos < filter.size() && filter[pos] == L' ') ++pos;
        const std::size_t start = pos;
        while (pos < filter.size() && filter[pos] != L' ') ++pos;
        if (pos > start && _wcsicmp(filter.substr(start, pos - start).c_str(), token) == 0) {
            return true;
        }
    }
    return false;
}

static std::wstring WithFilterToken(const std::wstring& filter, const wchar_t* token, bool add) {
    std::vector<std::wstring> tokens;
    std::size_t pos = 0;
    while (pos < filter.size()) {
        while (pos < filter.size() && filter[pos] == L' ') ++pos;
        const std::size_t start = pos;
        while (pos < filter.size() && filter[pos] != L' ') ++pos;
        if (pos > start) {
            const std::wstring value = filter.substr(start, pos - start);
            if (!token || !*token || _wcsicmp(value.c_str(), token) != 0) {
                tokens.push_back(value);
            }
        }
    }
    if (add && token && *token) tokens.emplace_back(token);
    std::wstring result;
    for (const auto& value : tokens) {
        if (!result.empty()) result += L' ';
        result += value;
    }
    return result;
}

struct MenuEntry {
    const wchar_t* label;
    const wchar_t* token;  // nullptr means "any"/clear the group
};

static void ApplySearchOptions(HWND frame, DWORD flags, DWORD maxResults,
                               const std::wstring& filter, bool contentSearch,
                               const std::wstring& folderFallback = std::wstring()) {
    // The command-bar button is created per window, but the request file must
    // be keyed by the tab that is active when the option is changed.
    const HWND key = RequestKeyForFrame(frame);
    esx::QueryRequest request{};
    if (!esx::ReadRequestForWindow(key, request)) {
        request.flags = esx::ScopeToFolder | esx::IncludeFolders;
    }
    if (request.folder.empty()) request.folder = folderFallback;
    request.flags = flags;
    request.maxResults = maxResults;
    request.filter = filter;
    request.contentSearch = contentSearch;
    request.seq = ::GetTickCount();
    if (!esx::WriteRequestForWindow(key, request)) {
        Log("options: could not write request for key=%p", key);
        return;
    }

    PIDLIST_ABSOLUTE pidl = nullptr;
    const HRESULT hr = SHParseDisplayName(
        L"shell:::{7B39C2A2-3E1F-4E4C-9E58-2D9A7B1C4F60}", nullptr, &pidl, 0, nullptr);
    if (SUCCEEDED(hr) && pidl) {
        SHChangeNotify(SHCNE_UPDATEDIR, SHCNF_IDLIST | SHCNF_FLUSHNOWAIT, pidl, nullptr);
        ::CoTaskMemFree(pidl);
    }
    Log("options applied frame=%p key=%p flags=0x%lX max=%lu filter='%ls' content=%d",
        frame, key, flags, maxResults, filter.c_str(), contentSearch ? 1 : 0);
}

static HRESULT AddSearchCommandBarButtons(IXamlDiagnostics* diagnostics,
                                          IVisualTreeService3* service,
                                          const std::shared_ptr<WindowState>& state,
                                          HWND frame,
                                          winrt::Microsoft::UI::Xaml::DependencyObject const& element) {
    if (!diagnostics || !service || !state || state->commandBarAdded) return S_FALSE;

    auto root = FindRootVisual(element);
    std::vector<winrt::Microsoft::UI::Xaml::Controls::CommandBar> bars;
    CollectDescendants<winrt::Microsoft::UI::Xaml::Controls::CommandBar>(root, bars);
    if (bars.empty()) {
        Log("search buttons: CommandBar not found");
        return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    }

    // There are two command bars in the window (tab strip and folder commands).
    // Pick the one that contains "Visualizza" so the button lands next to it.
    auto commandBar = bars.back();
    unsigned insertAt = 0;
    for (auto bar : bars) {
        auto commands = bar.PrimaryCommands();
        const unsigned count = commands.Size();
        for (unsigned i = 0; i < count; ++i) {
            auto candidate = commands.GetAt(i);
            if (auto button =
                    candidate.try_as<winrt::Microsoft::UI::Xaml::Controls::AppBarButton>()) {
                const std::wstring label(button.Label().c_str());
                if (label.find(L"Visualizza") != std::wstring::npos ||
                    label.find(L"View") != std::wstring::npos ||
                    label.find(L"Ordina") != std::wstring::npos ||
                    label.find(L"Sort") != std::wstring::npos) {
                    commandBar = bar;
                    insertAt = i + 1;
                    break;
                }
            }
        }
    }

    auto primary = commandBar.PrimaryCommands();
    // Place the button before the separator that follows "Visualizza".
    const unsigned primarySize = primary.Size();
    for (unsigned i = insertAt; i < primarySize; ++i) {
        if (primary.GetAt(i).try_as<winrt::Microsoft::UI::Xaml::Controls::AppBarSeparator>()) {
            insertAt = i;
            break;
        }
    }

    winrt::com_ptr<::IInspectable> rawPrimary = primary.as<::IInspectable>();
    InstanceHandle barCollection = 0;
    HRESULT hr = diagnostics->RegisterInstance(rawPrimary.get(), &barCollection);
    Log("search buttons bar collection hr=0x%08X handle=%llu index=%u",
        static_cast<unsigned long>(hr), static_cast<unsigned long long>(barCollection), insertAt);
    if (FAILED(hr) || !barCollection) return hr;

    esx::QueryRequest request{};
    esx::ReadRequestForWindow(RequestKeyForFrame(frame), request);
    // A search started from a virtual folder has no folder in the request; the
    // per-window state still remembers the folder it originated from.
    const std::wstring originFallback =
        (state && !state->origin.empty()) ? state->origin : std::wstring();
    if (request.folder.empty()) request.folder = originFallback;

    auto options = winrt::Microsoft::UI::Xaml::Controls::AppBarButton();
    options.Label(L"Opzioni di ricerca");
    auto flyout = winrt::Microsoft::UI::Xaml::Controls::MenuFlyout();
    flyout.Placement(
        winrt::Microsoft::UI::Xaml::Controls::Primitives::FlyoutPlacementMode::Bottom);

    auto glyph = [](wchar_t const* text) {
        auto icon = winrt::Microsoft::UI::Xaml::Controls::FontIcon();
        icon.Glyph(text);
        return icon;
    };

    auto allSubfolders = winrt::Microsoft::UI::Xaml::Controls::MenuFlyoutItem();
    allSubfolders.Text(L"Tutte le sottocartelle");
    allSubfolders.Icon(glyph(L"\uE8B7"));
    allSubfolders.Click([frame, originFallback](auto const&, auto const&) {
        esx::QueryRequest current{};
        esx::ReadRequestForWindow(RequestKeyForFrame(frame), current);
        const DWORD flags = current.flags |
                            static_cast<DWORD>(esx::ScopeToFolder) |
                            static_cast<DWORD>(esx::IncludeSubfolders);
        ApplySearchOptions(frame, flags, current.maxResults, current.filter,
                           current.contentSearch, originFallback);
    });
    flyout.Items().Append(allSubfolders);

    auto currentFolder = winrt::Microsoft::UI::Xaml::Controls::MenuFlyoutItem();
    currentFolder.Text(L"Cartella corrente");
    currentFolder.Icon(glyph(L"\uE8B7"));
    currentFolder.IsEnabled(!request.folder.empty());
    currentFolder.Click([frame, originFallback](auto const&, auto const&) {
        esx::QueryRequest current{};
        esx::ReadRequestForWindow(RequestKeyForFrame(frame), current);
        const DWORD flags =
            (current.flags | static_cast<DWORD>(esx::ScopeToFolder)) &
            ~static_cast<DWORD>(esx::IncludeSubfolders);
        ApplySearchOptions(frame, flags, current.maxResults, current.filter,
                           current.contentSearch, originFallback);
    });
    flyout.Items().Append(currentFolder);
    flyout.Items().Append(winrt::Microsoft::UI::Xaml::Controls::MenuFlyoutSeparator());

    auto addSubmenu = [&](wchar_t const* text, wchar_t const* iconGlyph,
                          std::initializer_list<MenuEntry> entries) {
        auto sub = winrt::Microsoft::UI::Xaml::Controls::MenuFlyoutSubItem();
        sub.Text(text);
        sub.Icon(glyph(iconGlyph));
        std::vector<std::wstring> group;
        for (const auto& entry : entries) {
            if (entry.token) group.emplace_back(entry.token);
        }
        for (const auto& entry : entries) {
            auto item = winrt::Microsoft::UI::Xaml::Controls::MenuFlyoutItem();
            item.Text(entry.label);
            const std::wstring selected = entry.token ? entry.token : L"";
            const std::vector<std::wstring> tokens = group;
            item.Click([frame, originFallback, tokens, selected](auto const&, auto const&) {
                esx::QueryRequest current{};
                esx::ReadRequestForWindow(RequestKeyForFrame(frame), current);
                std::wstring filter = current.filter;
                for (const auto& token : tokens) {
                    filter = WithFilterToken(filter, token.c_str(), false);
                }
                if (!selected.empty()) {
                    filter = WithFilterToken(filter, selected.c_str(), true);
                }
                ApplySearchOptions(frame, current.flags, current.maxResults, filter,
                                   current.contentSearch, originFallback);
            });
            sub.Items().Append(item);
        }
        flyout.Items().Append(sub);
    };
    addSubmenu(L"Data ultima modifica", L"\uE787",
               {{L"Qualsiasi ora", nullptr},
                {L"Oggi", L"date-modified:today"},
                {L"Ultimi 7 giorni", L"date-modified:7days"},
                {L"Ultimo mese", L"date-modified:30days"}});
    addSubmenu(L"Tipologia", L"\uE8A5",
               {{L"Tutte le tipologie", nullptr},
                {L"Documenti", L"doc:"},
                {L"Immagini", L"image:"},
                {L"Video", L"video:"},
                {L"Audio", L"audio:"}});
    addSubmenu(L"Dimensioni", L"\uE8B4",
               {{L"Qualsiasi dimensione", nullptr},
                {L"Vuoto (0 byte)", L"size:0"},
                {L"Molto piccolo (0 - 10 KB)", L"size:0..10kb"},
                {L"Piccolo (10 - 100 KB)", L"size:10kb..100kb"}});
    flyout.Items().Append(winrt::Microsoft::UI::Xaml::Controls::MenuFlyoutSeparator());

    auto addToggle = [&](wchar_t const* text, wchar_t const* iconGlyph, const wchar_t* token,
                         bool contentToggle) {
        auto item = winrt::Microsoft::UI::Xaml::Controls::ToggleMenuFlyoutItem();
        item.Text(text);
        item.Icon(glyph(iconGlyph));
        item.IsChecked(contentToggle ? request.contentSearch
                                     : HasFilterToken(request.filter, token));
        item.Click([frame, originFallback, token, contentToggle](
                       auto const& sender, auto const&) {
            auto toggle = sender.as<winrt::Microsoft::UI::Xaml::Controls::ToggleMenuFlyoutItem>();
            esx::QueryRequest current{};
            esx::ReadRequestForWindow(RequestKeyForFrame(frame), current);
            if (contentToggle) {
                ApplySearchOptions(frame, current.flags, current.maxResults, current.filter,
                                   toggle.IsChecked(), originFallback);
            } else {
                const std::wstring filter =
                    WithFilterToken(current.filter, token, toggle.IsChecked());
                ApplySearchOptions(frame, current.flags, current.maxResults, filter,
                                   current.contentSearch, originFallback);
            }
        });
        flyout.Items().Append(item);
    };
    addToggle(L"File di sistema", L"\uE7C0", L"attributes:s", false);
    addToggle(L"Cartelle compresse (ZIP)", L"\uE7B8", L"zip:", false);
    addToggle(L"Contenuto file", L"\uE8A5", nullptr, true);
    flyout.Items().Append(winrt::Microsoft::UI::Xaml::Controls::MenuFlyoutSeparator());

    options.Flyout(flyout);

    winrt::com_ptr<::IInspectable> rawOptions = options.as<::IInspectable>();
    hr = diagnostics->RegisterInstance(rawOptions.get(), &state->optionsButtonHandle);
    if (FAILED(hr) || !state->optionsButtonHandle) {
        Log("search buttons: options register failed hr=0x%08X", static_cast<unsigned long>(hr));
        return FAILED(hr) ? hr : E_UNEXPECTED;
    }
    hr = service->AddChild(barCollection, state->optionsButtonHandle, insertAt);
    if (FAILED(hr)) {
        Log("search buttons: options add failed hr=0x%08X", static_cast<unsigned long>(hr));
        return hr;
    }

    state->optionsButton = options;
    state->commandBarAdded = true;
    state->buttonsVisible = true;
    Log("search buttons added frame=%p", frame);
    return S_OK;
}

static void RunC3ProbeForState(const std::shared_ptr<WindowState>& state) {
    if (!state || !state->watcher) return;

    if (state->c2Done.load() || state->c2Running.exchange(true)) return;
    state->c2Done.store(true);


    auto probe = state->probe;
    if (!probe || !probe->ready.load()) {
        Log("C3 ABORT frame=%p gen=%llu: probe not ready", state->frame,
            static_cast<unsigned long long>(state->generation));
        state->c2Running.store(false);
        return;
    }
    auto* watcher = state->watcher.get();
    const InstanceHandle nativeHandle =
        static_cast<InstanceHandle>(probe->handle.load());
    if (state->nativeHandle && state->nativeHandle != nativeHandle) {
        Log("C3 ABORT frame=%p gen=%llu: native handle mismatch", state->frame,
            static_cast<unsigned long long>(state->generation));
        state->c2Running.store(false);
        return;
    }
    const HWND frame = state->frame;
    const DWORD pid = state->pid;
    const uint64_t generation = state->generation;
    const InstanceHandle expectedParent = state->parentHandle;
    const unsigned expectedIndex = state->nativeIndex;

    Log("C3 queued frame=%p pid=%lu gen=%llu native=%llu parent=%llu index=%u "
        "tid=%lu",
        frame, pid, static_cast<unsigned long long>(generation),
        static_cast<unsigned long long>(nativeHandle),
        static_cast<unsigned long long>(expectedParent), expectedIndex,
        GetCurrentThreadId());

    winrt::Microsoft::UI::Dispatching::DispatcherQueue resolved{nullptr};
    const HRESULT dispatchHr = watcher->RunOnElementDispatcher(
        nativeHandle,
        [watcher, nativeHandle, frame, pid, generation, expectedParent,
         expectedIndex, state]() -> HRESULT {
            const DWORD tid = GetCurrentThreadId();
            auto diagnostics = watcher->OwnerDiagnostics();
            auto* service = watcher->OwnerService();
            if (!diagnostics || !service) {
                Log("C3 ABORT: missing XAML diagnostics/service");
                return E_UNEXPECTED;
            }
            Log("C3 begin tid=%lu frame=%p tab=%p queryText='%ls' pid=%lu gen=%llu "
                "native=%llu service=%p",
                tid, frame, static_cast<void*>(state->tab), state->queryText.c_str(),
                pid, static_cast<unsigned long long>(generation),
                static_cast<unsigned long long>(nativeHandle), service);

            // C1.1/C1.2: re-acquire the native element and verify identity.
            winrt::com_ptr<::IInspectable> nativeRaw{nullptr};
            HRESULT hr = diagnostics->GetIInspectableFromHandle(nativeHandle,
                                                                nativeRaw.put());
            if (FAILED(hr) || !nativeRaw) {
                Log("C3 ABORT: native handle invalid hr=0x%08X",
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
            Log("C3 collection hr=0x%08X handle=%llu tid=%lu",
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
            Log("C3 before countHr=0x%08X size=%u nativeHr=0x%08X index=%u",
                static_cast<unsigned long>(countHr), beforeSize,
                static_cast<unsigned long>(nativeHr), nativeIndex);
            const std::wstring before = ServiceSnapshot(service, collection);
            Log("C3 snapshot before %ls", before.c_str());
            if (FAILED(countHr) || FAILED(nativeHr) ||
                beforeSize < 1 || nativeIndex >= beforeSize ||
                (expectedIndex != 0 && nativeIndex != expectedIndex)) {
                Log("C3 ABORT: collection or native identity changed");
                return E_ABORT;
            }

            // C1.4/C1.5: create the TextBox and register it in this dispatch.
            auto box = winrt::Microsoft::UI::Xaml::Controls::TextBox();
            box.PlaceholderText(L"Cerca con Everything");
            // Restore this tab's last query; a brand-new tab starts empty.
            if (state && !state->queryText.empty()) {
                box.Text(winrt::hstring(state->queryText));
                box.SelectionStart(static_cast<int32_t>(state->queryText.size()));
                box.SelectionLength(0);
            }
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

            box.KeyDown([box, frame, weak = std::weak_ptr<WindowState>(state)](
                            auto const&,
                            winrt::Microsoft::UI::Xaml::Input::KeyRoutedEventArgs const& args) {
                auto state = weak.lock();
                if (!state) {
                    args.Handled(true);
                    return;
                }
                const int key = static_cast<int>(args.Key());
                if (key == VK_RETURN) {
                    const auto text = box.Text();
                    if (text.empty()) {
                        args.Handled(true);
                        return;
                    }
                    const std::wstring query(text.c_str(), text.size());
                    if (state) state->queryText = query;
                    const bool inResults = IsResultsFolder(frame);
                    // Keep the folder the first search started from: a second
                    // search inside the results view has no filesystem path of
                    // its own, so the original scope must be carried over.
                    // This is per-window state, like the request file.
                    std::wstring scope;
                    if (inResults) {
                        scope = state ? state->origin : std::wstring();
                    } else {
                        scope = CurrentFolderPath(frame);
                        if (state && !scope.empty()) state->origin = scope;
                    }
                    const HWND key = ActiveTabWindow(frame);
                    if (key) SaveTabText(key, query);
                    const bool written = WriteSearchRequest(query, scope, key ? key : frame);
                    Log("C3 Enter query='%ls' scope='%ls' key=%p written=%d",
                        query.c_str(), scope.c_str(), static_cast<void*>(key),
                        written ? 1 : 0);
                    if (state) state->closingSearch.store(false);
                    if (written && IsWindow(frame)) NavigateToResults(frame);
                    box.Focus(winrt::Microsoft::UI::Xaml::FocusState::Programmatic);
                    // Focus/Text assignment can reset the caret to position 0.
                    box.SelectionStart(static_cast<int32_t>(text.size()));
                    box.SelectionLength(0);
                    args.Handled(true);
                } else if (key == VK_ESCAPE) {
                    if (IsResultsFolder(frame)) {
                        CloseSearchForFrame(frame, state);
                    } else {
                        state->ignoreNextEmpty.store(true);
                        box.Text(L"");
                    }
                    args.Handled(true);
                }
            });
            box.TextChanged([box, frame, weak = std::weak_ptr<WindowState>(state)](
                                 auto const&, auto const&) {
                auto state = weak.lock();
                if (!state || state->closingSearch.load()) return;
                if (state->ignoreNextEmpty.exchange(false)) return;
                const std::wstring current(box.Text().c_str(), box.Text().size());
                const HWND tab = ActiveTabWindow(frame);
                if (tab) SaveTabText(tab, current);
                // IsResultsFolder() enumerates IShellWindows: only pay for it
                // when the field actually became empty.
                if (current.empty() && IsResultsFolder(frame)) {
                    Log("C3 clear button closes search frame=%p", frame);
                    CloseSearchForFrame(frame, state);
                }
            });
            box.GotFocus([weak = std::weak_ptr<WindowState>(state)](auto const&, auto const&) {
                auto state = weak.lock();
                if (!state) return;
                state->textHasFocus.store(true);
                Log("DIRECT focus gained frame=%p tid=%lu", state->frame,
                    GetCurrentThreadId());
            });
            box.LostFocus([weak = std::weak_ptr<WindowState>(state)](auto const&, auto const&) {
                auto state = weak.lock();
                if (!state) return;
                state->textHasFocus.store(false);
                Log("DIRECT focus lost frame=%p tid=%lu", state->frame,
                    GetCurrentThreadId());
            });
            winrt::com_ptr<::IInspectable> boxRaw{nullptr};
            boxRaw = box.as<::IInspectable>();
            InstanceHandle created = 0;
            const HRESULT regHr = diagnostics->RegisterInstance(boxRaw.get(),
                                                                &created);
            Log("C3 RegisterInstance textbox hr=0x%08X handle=%llu",
                static_cast<unsigned long>(regHr),
                static_cast<unsigned long long>(created));
            if (FAILED(regHr) || !created) return regHr;

            // C3.1: AddChild on the dispatcher, native untouched.
            const unsigned insertAt = nativeIndex + 1;
            const HRESULT addHr = service->AddChild(collection, created, insertAt);
            Log("C3 AddChild hr=0x%08X index=%u tid=%lu",
                static_cast<unsigned long>(addHr), insertAt, tid);
            if (FAILED(addHr)) {
                Log("C3 RESULT FAIL (AddChild)");
                return addHr;
            }

            // Add the search command-bar button on the same XAML dispatcher.
            const HRESULT barHr = AddSearchCommandBarButtons(
                diagnostics.get(), service, state, frame, element);
            Log("C3 search buttons hr=0x%08X", static_cast<unsigned long>(barHr));

            // C3.2/C3.3: verify count and our element identity.
            unsigned afterSize = 0;
            const HRESULT afterCountHr = service->GetCollectionCount(collection,
                                                                     &afterSize);
            unsigned ourIndex = 0;
            const HRESULT ourHr = ServiceFindHandleIndex(service, collection,
                                                         created, &ourIndex);
            unsigned nativeAfterIndex = 0;
            const HRESULT nativeAfterHr = ServiceFindHandleIndex(
                service, collection, nativeHandle, &nativeAfterIndex);
            Log("C3 verify countHr=0x%08X size=%u ourHr=0x%08X ourIndex=%u "
                "nativeHr=0x%08X nativeIndex=%u",
                static_cast<unsigned long>(afterCountHr), afterSize,
                static_cast<unsigned long>(ourHr), ourIndex,
                static_cast<unsigned long>(nativeAfterHr), nativeAfterIndex);
            const std::wstring after = ServiceSnapshot(service, collection);
            Log("C3 snapshot after %ls", after.c_str());
            const bool addVerified =
                SUCCEEDED(afterCountHr) && afterSize == beforeSize + 1 &&
                SUCCEEDED(ourHr) && ourIndex == insertAt &&
                SUCCEEDED(nativeAfterHr) && nativeAfterIndex == nativeIndex;

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
                    state->mutating.store(true);
                    nativeRemoveHr =
                        service->RemoveChild(collection, nativeRemoveIndex);
                    state->mutating.store(false);
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
        },
        &resolved);
    if (resolved) state->dispatcher = winrt::make_agile(resolved);
    Log("C3 dispatch hr=0x%08X", static_cast<unsigned long>(dispatchHr));
    state->c2Running.store(false);
}

static void RunC3ForReadyStates() {
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
    for (auto& state : targets) RunC3ProbeForState(state);
}
void TryCreateExperimentTextBox() {
    if (!g_callbackProbe.ready.load() || g_createdHandle != 0 || !g_probeWatcher) {
        return;
    }
    const unsigned long long handle = g_callbackProbe.handle.load();
    if (!handle) return;
    const InstanceHandle nativeHandle = static_cast<InstanceHandle>(handle);
    auto state = FindStateForNative(nativeHandle);
    const HWND frame = state ? state->frame : g_experimentFrame;
    const HRESULT hr = g_probeWatcher->RunOnDispatcher(
        nativeHandle, [watcher = g_probeWatcher, nativeHandle, frame, state]() {
            auto diagnostics = watcher->OwnerDiagnostics();
            if (!diagnostics) {
                Log("DIRECT layout abort: no XAML diagnostics");
                return E_UNEXPECTED;
            }

            winrt::com_ptr<::IInspectable> nativeAbi{nullptr};
            const HRESULT nativeHr = diagnostics->GetIInspectableFromHandle(
                nativeHandle, nativeAbi.put());
            if (FAILED(nativeHr) || !nativeAbi) {
                Log("DIRECT layout abort: GetIInspectableFromHandle hr=0x%08X",
                    static_cast<unsigned long>(nativeHr));
                return nativeHr;
            }

            winrt::Windows::Foundation::IInspectable nativeInspectable{nullptr};
            winrt::copy_from_abi(nativeInspectable, nativeAbi.get());
            winrt::Microsoft::UI::Xaml::FrameworkElement nativeElement{nullptr};
            try {
                nativeElement = nativeInspectable.as<
                    winrt::Microsoft::UI::Xaml::FrameworkElement>();
            } catch (...) {
                const HRESULT castHr = winrt::to_hresult();
                Log("DIRECT layout abort: native control is not FrameworkElement "
                    "hr=0x%08X", static_cast<unsigned long>(castHr));
                return castHr;
            }

            const int32_t gridRow =
                winrt::Microsoft::UI::Xaml::Controls::Grid::GetRow(nativeElement);
            const int32_t gridColumn =
                winrt::Microsoft::UI::Xaml::Controls::Grid::GetColumn(nativeElement);
            const int32_t gridRowSpan =
                winrt::Microsoft::UI::Xaml::Controls::Grid::GetRowSpan(nativeElement);
            const int32_t gridColumnSpan =
                winrt::Microsoft::UI::Xaml::Controls::Grid::GetColumnSpan(nativeElement);

            auto box = winrt::Microsoft::UI::Xaml::Controls::TextBox();
            box.PlaceholderText(L"Cerca con Everything");
            box.Visibility(winrt::Microsoft::UI::Xaml::Visibility::Visible);
            box.IsEnabled(true);
            box.IsTabStop(true);
            box.IsHitTestVisible(true);

            // Match the native element's layout contract. NaN Width/Height
            // values are preserved, so the parent Grid continues to resize it.
            box.Width(nativeElement.Width());
            box.Height(nativeElement.Height());
            box.MinWidth(nativeElement.MinWidth());
            box.MaxWidth(nativeElement.MaxWidth());
            box.MinHeight(nativeElement.MinHeight());
            box.MaxHeight(nativeElement.MaxHeight());
            box.Margin(nativeElement.Margin());
            box.HorizontalAlignment(nativeElement.HorizontalAlignment());
            box.VerticalAlignment(nativeElement.VerticalAlignment());
            box.FlowDirection(nativeElement.FlowDirection());
            winrt::Microsoft::UI::Xaml::Controls::Grid::SetRow(box, gridRow);
            winrt::Microsoft::UI::Xaml::Controls::Grid::SetColumn(box, gridColumn);
            winrt::Microsoft::UI::Xaml::Controls::Grid::SetRowSpan(box, gridRowSpan);
            winrt::Microsoft::UI::Xaml::Controls::Grid::SetColumnSpan(box, gridColumnSpan);

            Log("DIRECT layout copied from native: row=%d column=%d rowSpan=%d "
                "columnSpan=%d width=%.2f height=%.2f actual=%.2fx%.2f "
                "margin=%.1f,%.1f,%.1f,%.1f dispatcherThread=%lu",
                gridRow, gridColumn, gridRowSpan, gridColumnSpan,
                nativeElement.Width(), nativeElement.Height(),
                nativeElement.ActualWidth(), nativeElement.ActualHeight(),
                nativeElement.Margin().Left, nativeElement.Margin().Top,
                nativeElement.Margin().Right, nativeElement.Margin().Bottom,
                GetCurrentThreadId());

            InstanceHandle collection = 0;
            auto parent = winrt::Microsoft::UI::Xaml::Media::VisualTreeHelper::GetParent(
                nativeElement);
            if (auto panel =
                    parent.try_as<winrt::Microsoft::UI::Xaml::Controls::Panel>()) {
                auto children = panel.Children();
                winrt::com_ptr<::IInspectable> rawChildren{nullptr};
                rawChildren = children.as<::IInspectable>();
                diagnostics->RegisterInstance(rawChildren.get(), &collection);
            }
            Log("DIRECT collection from Panel.Children hr=0x%08X handle=%llu depth=0",
                collection ? S_OK : E_FAIL,
                static_cast<unsigned long long>(collection));

            box.GotFocus([](auto const&, auto const&) {
                g_directTextHasFocus.store(true);
                Log("DIRECT TextBox GotFocus thread=%lu", GetCurrentThreadId());
            });
            box.LostFocus([](auto const&, auto const&) {
                g_directTextHasFocus.store(false);
                Log("DIRECT TextBox LostFocus thread=%lu", GetCurrentThreadId());
            });
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
                    std::wstring scope = inResults ? std::wstring()
                                                   : CurrentFolderPath(frame);
                    const bool written = WriteSearchRequest(query, scope, frame);
                    Log("DIRECT Enter query='%ls' scope='%ls'", query.c_str(),
                        scope.c_str());
                    Log("DIRECT request written=%d", written ? 1 : 0);
                    if (written) NavigateToResults(frame);
                    const bool focused = box.Focus(
                        winrt::Microsoft::UI::Xaml::FocusState::Programmatic);
                    Log("DIRECT focus after navigate=%d", focused ? 1 : 0);
                    args.Handled(true);
                } else if (key == VK_ESCAPE) {
                    box.Text(L"");
                    args.Handled(true);
                }
            });

            InstanceHandle created = 0;
            const HRESULT regHr = watcher->RegisterCreatedInstance(box, &created);
            Log("PHASE2 RegisterInstance hr=0x%08X handle=%llu",
                static_cast<unsigned long>(regHr),
                static_cast<unsigned long long>(created));
            if (SUCCEEDED(regHr)) {
                g_createdTextBox = box;
                g_createdHandle = created;
                if (state) {
                    state->textBox = box;
                    state->textBoxHandle = created;
                    state->phase = WindowPhase::TextBoxCreated;
                    Log("DIRECT TextBox registered state frame=%p gen=%llu "
                        "handle=%llu", state->frame,
                        static_cast<unsigned long long>(state->generation),
                        static_cast<unsigned long long>(created));
                }
            }
            return regHr;
        });
    Log("PHASE2 create dispatch hr=0x%08X", static_cast<unsigned long>(hr));
}

void PostAddTransaction() {
    auto state = FindStateForNative(
        static_cast<InstanceHandle>(g_callbackProbe.handle.load()));
    if (!state || !state->watcher || state->addPosted) return;
    if (!g_callbackProbe.ready.load() ||
        g_callbackProbe.firstCollection == 0 ||
        state->textBoxHandle == 0) {
        Log("OWNER ADD skipped: state not ready");
        return;
    }
    WindowPhase expected = WindowPhase::TextBoxCreated;
    if (!state->phase.compare_exchange_strong(expected, WindowPhase::AddPending)) {
        return;
    }
    HWND channel = state->watcher->ChannelWindow();
    if (!channel || !IsWindow(channel)) {
        state->phase.store(WindowPhase::Failed);
        return;
    }

    auto* request = new (std::nothrow) OwnerAddRequest();
    if (!request) {
        state->phase.store(WindowPhase::Failed);
        return;
    }
    request->state = state;
    request->generation = state->generation;
    request->collection = static_cast<InstanceHandle>(g_callbackProbe.firstCollection);
    state->collectionHandle = request->collection;
    request->native = state->nativeHandle;
    request->created = state->textBoxHandle;
    request->index = state->nativeIndex;
    request->replaceNative = kReplaceNative;
    state->addTx = std::make_shared<AddTxResult>();
    state->rollbackTx = std::make_shared<RollbackTxResult>();
    if (PostMessageW(channel, kOwnerAddMessage, 0,
                     reinterpret_cast<LPARAM>(request))) {
        state->addPosted = true;
        Log("OWNER ADD posted frame=%p gen=%llu collection=%llu native=%llu "
            "created=%llu index=%u",
            state->frame, static_cast<unsigned long long>(state->generation),
            static_cast<unsigned long long>(request->collection),
            static_cast<unsigned long long>(request->native),
            static_cast<unsigned long long>(request->created), request->index);
    } else {
        delete request;
        state->phase.store(WindowPhase::Failed);
        Log("OWNER ADD PostMessage failed err=%lu", GetLastError());
    }
}

void LogAddTxResult() {
    if (!g_addTx.ready.load() || g_addTxReported.exchange(true)) return;
    auto& tx = g_addTx;
    Log("OWNER ADD RESULT countHr=0x%08X before=%u nativeBeforeHr=0x%08X "
        "nativeBefore=%u addHr=0x%08X afterCountHr=0x%08X after=%u "
        "ourHr=0x%08X our=%u nativeAfterHr=0x%08X nativeAfter=%u "
        "recheckHr=0x%08X removeHr=0x%08X finalCountHr=0x%08X final=%u "
        "ourGoneHr=0x%08X nativeFinalHr=0x%08X nativeFinal=%u "
        "nativeRemoveFindHr=0x%08X nativeRemoveIndex=%u "
        "nativeRemoveHr=0x%08X afterRemoveSize=%u nativeGoneHr=0x%08X "
        "ourAfterRemoveHr=0x%08X ourAfterRemoveIndex=%u",
        static_cast<unsigned long>(tx.countHr), tx.beforeSize,
        static_cast<unsigned long>(tx.nativeBeforeHr), tx.nativeBeforeIndex,
        static_cast<unsigned long>(tx.addHr),
        static_cast<unsigned long>(tx.afterCountHr), tx.afterSize,
        static_cast<unsigned long>(tx.ourHr), tx.ourIndex,
        static_cast<unsigned long>(tx.nativeAfterHr), tx.nativeAfterIndex,
        static_cast<unsigned long>(tx.recheckHr),
        static_cast<unsigned long>(tx.removeHr),
        static_cast<unsigned long>(tx.finalCountHr), tx.finalSize,
        static_cast<unsigned long>(tx.ourGoneHr),
        static_cast<unsigned long>(tx.nativeFinalHr), tx.nativeFinalIndex,
        static_cast<unsigned long>(tx.nativeRemoveFindHr), tx.nativeRemoveIndex,
        static_cast<unsigned long>(tx.nativeRemoveHr), tx.afterRemoveSize,
        static_cast<unsigned long>(tx.nativeGoneHr),
        static_cast<unsigned long>(tx.ourAfterRemoveHr),
        tx.ourAfterRemoveIndex);
}

void ReleaseExperimentMutex() {
    if (!g_addMutex) return;
    ReleaseMutex(g_addMutex);
    CloseHandle(g_addMutex);
    g_addMutex = nullptr;
}

void RestoreReplacement(bool synchronous) {
    std::vector<std::shared_ptr<WindowState>> targets;
    {
        std::lock_guard<std::mutex> guard(g_windowStatesMutex);
        for (const auto& item : g_windowStates) {
            auto state = item.second;
            if (state->replacementActive && !state->rollbackPosted) {
                targets.push_back(state);
            }
        }
    }
    for (auto& state : targets) {
        if (!state->watcher) continue;
        HWND channel = state->watcher->ChannelWindow();
        if (!channel || !IsWindow(channel)) {
            Log("OWNER ROLLBACK skipped: channel gone frame=%p", state->frame);
            continue;
        }
        // The C3 path never allocates a rollback transaction.
        if (state->rollbackTx) {
            state->rollbackTx->ready.store(false);
        }
        auto* request = new (std::nothrow) OwnerAddRequest();
        if (!request) continue;
        request->state = state;
        request->generation = state->generation;
        request->collection = state->collectionHandle;
        request->native = state->nativeHandle;
        request->created = state->textBoxHandle;
        request->index = state->nativeIndex;
        request->replaceNative = true;
        bool delivered = false;
        if (synchronous) {
            DWORD_PTR reply = 0;
            delivered = SendMessageTimeoutW(
                            channel, kOwnerRollbackMessage, 0,
                            reinterpret_cast<LPARAM>(request), SMTO_ABORTIFHUNG,
                            3000, &reply) != 0;
        } else {
            delivered = PostMessageW(channel, kOwnerRollbackMessage, 0,
                                     reinterpret_cast<LPARAM>(request)) != FALSE;
        }
        if (delivered) {
            state->rollbackPosted = true;
            Log("OWNER ROLLBACK %s frame=%p gen=%llu",
                synchronous ? "sent synchronously" : "posted", state->frame,
                static_cast<unsigned long long>(state->generation));
        } else {
            delete request;
            Log("OWNER ROLLBACK delivery failed frame=%p err=%lu", state->frame,
                GetLastError());
        }
    }
}

void LogRollbackResult() {
    if (!g_rollbackTx.ready.load() || g_rollbackReported.exchange(true)) return;
    auto& rb = g_rollbackTx;
    Log("OWNER ROLLBACK RESULT findHr=0x%08X index=%u removeHr=0x%08X "
        "countHr=0x%08X size=%u ourGoneHr=0x%08X nativeHr=0x%08X "
        "nativeIndex=%u restoreHr=0x%08X",
        static_cast<unsigned long>(rb.findHr), rb.index,
        static_cast<unsigned long>(rb.removeHr),
        static_cast<unsigned long>(rb.countHr), rb.size,
        static_cast<unsigned long>(rb.ourGoneHr),
        static_cast<unsigned long>(rb.nativeHr), rb.nativeIndex,
        static_cast<unsigned long>(rb.restoreHr));
}

void LogCallbackProbeResult() {
    if (!g_callbackProbe.ready.load() || g_callbackProbeReported.exchange(true)) {
        return;
    }
    auto& result = g_callbackProbe;
    Log("CALLBACKPROBE handle=%llu parent=%llu index=%u tidMismatch=%d "
        "chain=0x%08X values=%u collections=%u firstCollection=%llu "
        "propertyIndex=%u metadata=0x%llX property='%ls' itemType='%ls' "
        "valueType='%ls' countHr=0x%08X size=%u elementsHr=0x%08X elements=%u",
        result.handle.load(), result.parent, result.childIndex,
        result.tidMismatch ? 1 : 0, static_cast<unsigned long>(result.chainHr),
        result.valueCount, result.collectionCount, result.firstCollection,
        result.firstCollectionIndex,
        static_cast<unsigned long long>(result.collectionMetadata),
        result.propertyName, result.itemType, result.valueType,
        static_cast<unsigned long>(result.countHr), result.collectionSize,
        static_cast<unsigned long>(result.elementsHr), result.elementCount);
    for (unsigned int i = 0; i < result.layoutCount; ++i) {
        const auto& prop = result.layoutProps[i];
        Log("CALLBACKPROBE layout %u: %ls = '%ls' chain=%u declaring='%ls'", i,
            prop.name, prop.value, prop.chainIndex, prop.declaring);
    }
    for (unsigned int i = 0; i < result.elementCount; ++i) {
        const auto& element = result.elements[i];
        Log("CALLBACKPROBE element %u: index=%u bits=0x%llX handle=%llu "
            "valueType='%ls' value='%ls'",
            i, element.index, static_cast<unsigned long long>(element.bits),
            element.handle, element.valueType, element.value);
    }
}

void LogProcessInventory() {
    const DWORD pid = GetCurrentProcessId();
    DWORD parentPid = 0;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W entry{};
        entry.dwSize = sizeof(entry);
        if (Process32FirstW(snapshot, &entry)) {
            do {
                if (entry.th32ProcessID == pid) {
                    parentPid = entry.th32ParentProcessID;
                    break;
                }
            } while (Process32NextW(snapshot, &entry));
        }
        CloseHandle(snapshot);
    }
    DWORD trayPid = 0;
    GetWindowThreadProcessId(FindWindowW(L"Shell_TrayWnd", nullptr), &trayPid);
    int frames = 0;
    EnumWindows([](HWND hwnd, LPARAM out) -> BOOL {
        DWORD processId = 0;
        GetWindowThreadProcessId(hwnd, &processId);
        if (processId != GetCurrentProcessId() || !IsWindowVisible(hwnd)) return TRUE;
        wchar_t className[128]{};
        if (GetClassNameW(hwnd, className, ARRAYSIZE(className)) &&
            wcscmp(className, L"CabinetWClass") == 0) {
            ++*reinterpret_cast<int*>(out);
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&frames));

    size_t mapped = 0, discovered = 0;
    {
        std::lock_guard<std::mutex> guard(g_observedMutex);
        for (const auto& item : g_observedBoxes) {
            if (item.second.source == ObservedSource::XamlMapped) ++mapped;
            else ++discovered;
        }
    }
    Log("INVENTORY pid=%lu parent=%lu shell=%d trayPid=%lu frames=%d "
        "watchers=%zu xaml=%zu uia=%zu cmd='%ls'",
        pid, parentPid, trayPid == pid ? 1 : 0, trayPid, frames,
        SnapshotWatchers().size(), mapped, discovered, GetCommandLineW());
}

void TryPhase2CreateOnly() {
    if (!kPhase2CreateOnly || g_experimentStarted) return;

    DWORD chosenPid = 0;
    HWND chosenFrame = nullptr;
    InstanceHandle chosenElement = 0;
    InstanceHandle chosenParent = 0;
    unsigned chosenIndex = 0;
    uintptr_t chosenWatcher = 0;
    {
        std::lock_guard<std::mutex> guard(g_observedMutex);
        for (const auto& item : g_observedBoxes) {
            if (item.second.source != ObservedSource::XamlMapped) continue;
            for (const auto& other : g_observedBoxes) {
                if (other.second.source == ObservedSource::UiaDiscovered &&
                    other.second.key.pid == item.second.key.pid) {
                    chosenPid = item.second.key.pid;
                    chosenFrame = other.second.frame;
                    chosenElement = item.second.key.handle;
                    chosenParent = item.second.parent;
                    chosenIndex = item.second.childIndex;
                    chosenWatcher = item.second.key.owner;
                    break;
                }
            }
            if (chosenFrame) break;
        }
    }
    if (!chosenFrame || !IsWindow(chosenFrame) || !chosenElement || !chosenWatcher) {
        return;
    }

    winrt::com_ptr<VisualTreeWatcher> watcher;
    for (auto& candidate : SnapshotWatchers()) {
        if (reinterpret_cast<uintptr_t>(candidate.get()) == chosenWatcher) {
            watcher = candidate;
            break;
        }
    }
    if (!watcher) return;

    g_experimentStarted = true;
    g_experimentPid = chosenPid;
    g_experimentFrame = chosenFrame;

    const HRESULT dispatchHr = watcher->RunOnDispatcher(
        chosenElement, [watcher, chosenElement]() {
            Log("PHASE2 create start pid=%lu frame=%p element=%llu",
                g_experimentPid, g_experimentFrame,
                static_cast<unsigned long long>(chosenElement));
            auto box = winrt::Microsoft::UI::Xaml::Controls::TextBox();
            box.PlaceholderText(L"Cerca con Everything (prototipo)");
            // The first insertion is invisible and inert: no focus stealing,
            // no keyboard routing, no visual change to Explorer.
            box.Visibility(winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
            box.IsEnabled(false);
            box.IsTabStop(false);
            InstanceHandle handle = 0;
            const HRESULT hr = watcher->RegisterCreatedInstance(box, &handle);
            Log("PHASE2 RegisterInstance hr=0x%08X handle=%llu",
                static_cast<unsigned long>(hr),
                static_cast<unsigned long long>(handle));
            if (SUCCEEDED(hr)) {
                g_createdTextBox = box;
                g_createdHandle = handle;
                Log("PHASE2 textbox registered, tree untouched");
            }
            return hr;
        });
    Log("PHASE2 dispatch hr=0x%08X", static_cast<unsigned long>(dispatchHr));
    if (SUCCEEDED(dispatchHr)) {
        g_experimentWatcher = watcher;
        g_experimentElement = chosenElement;
        g_experimentParent = chosenParent;
        g_experimentIndex = chosenIndex;
    } else {
        g_experimentStarted = false;
    }
}

void TryPhase2bAddChild() {
    if (g_addAttempted) return;
    if (!g_createdHandle || !g_experimentWatcher || !g_experimentParent ||
        !g_experimentElement) {
        return;
    }

    // One experiment for the whole Windows session. WAIT_ABANDONED means the
    // previous owner died mid-mutation: stop without touching anything.
    HANDLE mutex = CreateMutexW(nullptr, FALSE, kExperimentMutexName);
    if (!mutex) {
        Log("PHASE2b ABORT: cannot create experiment mutex err=%lu", GetLastError());
        g_addAttempted = true;
        return;
    }
    const DWORD wait = WaitForSingleObject(mutex, 0);
    if (wait == WAIT_ABANDONED) {
        Log("PHASE2b ABORT: experiment mutex abandoned");
        CloseHandle(mutex);
        g_addAttempted = true;
        return;
    }
    if (wait != WAIT_OBJECT_0) {
        Log("PHASE2b ABORT: another session owns the experiment mutex");
        CloseHandle(mutex);
        g_addAttempted = true;
        return;
    }
    g_experimentMutex = mutex;
    Log("PHASE2b mutex acquired pid=%lu", GetCurrentProcessId());

    VisualTreeWatcher::CollectionProbe probe;
    HRESULT hr = g_experimentWatcher->ProbeCollection(
        g_experimentParent, g_experimentIndex, g_experimentElement, &probe);
    Log("PHASE2b probe hr=0x%08X verified=%d collection=%llu index=%u "
        "property='%ls' itemType='%ls'",
        static_cast<unsigned long>(hr), probe.verified ? 1 : 0,
        static_cast<unsigned long long>(probe.collection), probe.index,
        probe.propertyName.c_str(), probe.itemType.c_str());
    if (FAILED(hr) || !probe.verified) {
        Log("PHASE2b ABORT: collection not verified, tree untouched");
        ReleaseMutex(mutex);
        CloseHandle(mutex);
        g_experimentMutex = nullptr;
        g_addAttempted = true;
        return;
    }

    if (!kPhase2bAddChild) {
        Log("PHASE2b PROBE ONLY: collection verified, NO AddChild in this build");
        ReleaseMutex(mutex);
        CloseHandle(mutex);
        g_experimentMutex = nullptr;
        g_addAttempted = true;
        return;
    }

    std::wstring before;
    hr = g_experimentWatcher->CollectionSnapshot(probe.collection, &before);
    Log("PHASE2b before hr=0x%08X %ls", static_cast<unsigned long>(hr),
        before.c_str());

    const unsigned insertAt = probe.index + 1;
    hr = g_experimentWatcher->AddChildAt(probe.collection, g_createdHandle, insertAt);
    Log("PHASE2b AddChild hr=0x%08X collection=%llu child=%llu index=%u",
        static_cast<unsigned long>(hr),
        static_cast<unsigned long long>(probe.collection),
        static_cast<unsigned long long>(g_createdHandle), insertAt);

    std::wstring after;
    if (SUCCEEDED(hr)) {
        hr = g_experimentWatcher->CollectionSnapshot(probe.collection, &after);
        Log("PHASE2b after hr=0x%08X %ls", static_cast<unsigned long>(hr),
            after.c_str());
    }

    unsigned ourIndex = 0;
    const HRESULT findHr =
        g_experimentWatcher->FindHandleIndex(probe.collection, g_createdHandle, &ourIndex);
    Log("PHASE2b FindHandleIndex(our) hr=0x%08X index=%u",
        static_cast<unsigned long>(findHr), ourIndex);
    unsigned nativeIndex = 0;
    const HRESULT nativeHr = g_experimentWatcher->FindHandleIndex(
        probe.collection, g_experimentElement, &nativeIndex);
    Log("PHASE2b FindHandleIndex(native) hr=0x%08X index=%u",
        static_cast<unsigned long>(nativeHr), nativeIndex);

    if (SUCCEEDED(findHr)) {
        // The index may have shifted after insertion: search again immediately
        // before removing instead of reusing the insertion index.
        unsigned currentIndex = 0;
        const HRESULT recheck = g_experimentWatcher->FindHandleIndex(
            probe.collection, g_createdHandle, &currentIndex);
        if (SUCCEEDED(recheck)) {
            const HRESULT removeHr =
                g_experimentWatcher->RemoveChildAt(probe.collection, currentIndex);
            Log("PHASE2b rollback RemoveChild hr=0x%08X index=%u",
                static_cast<unsigned long>(removeHr), currentIndex);
        } else {
            Log("PHASE2b rollback ABORT: our handle disappeared hr=0x%08X",
                static_cast<unsigned long>(recheck));
        }
    } else {
        Log("PHASE2b rollback skipped: our handle was never inserted");
    }

    std::wstring rolled;
    hr = g_experimentWatcher->CollectionSnapshot(probe.collection, &rolled);
    Log("PHASE2b rolledback hr=0x%08X %ls", static_cast<unsigned long>(hr),
        rolled.c_str());

    ReleaseMutex(mutex);
    CloseHandle(mutex);
    g_experimentMutex = nullptr;
    g_addAttempted = true;
    Log("PHASE2b experiment finished");
}

void StopAllXamlDiagnostics() {
    std::vector<winrt::com_ptr<VisualTreeWatcher>> watchers;
    {
        std::lock_guard<std::mutex> guard(g_watchersMutex);
        watchers.swap(g_watchers);
    }
    Log("STOPALL watchers=%zu", watchers.size());
    for (auto& watcher : watchers) watcher->Detach();
}

class SearchBoxTap
    : public winrt::implements<SearchBoxTap, IObjectWithSite, winrt::non_agile> {
public:
    HRESULT STDMETHODCALLTYPE SetSite(IUnknown* site) override try {
        Log("XAML TAP SetSite site=%p", site);
        if (!site) return S_OK;

        std::lock_guard<std::mutex> lifecycleGuard(g_lifecycleMutex);
        if (g_stopping) return HRESULT_FROM_WIN32(ERROR_CANCELLED);

        auto watcher = winrt::make_self<VisualTreeWatcher>(site);
        RegisterWatcher(watcher);
        const HRESULT hr = watcher->StartAdvise();
        Log("XAML TAP StartAdvise hr=0x%08X", hr);
        if (FAILED(hr)) return hr;
        if (g_tapAttachedEvent) SetEvent(g_tapAttachedEvent);
        return S_OK;
    } catch (...) {
        return winrt::to_hresult();
    }

    HRESULT STDMETHODCALLTYPE GetSite(REFIID interfaceId, void** object) noexcept override {
        if (!object) return E_POINTER;
        *object = nullptr;
        return E_NOINTERFACE;
    }
};

template <typename Class>
struct SimpleFactory
    : winrt::implements<SimpleFactory<Class>, IClassFactory, winrt::non_agile> {
    HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown* outer, REFIID interfaceId,
                                             void** object) override try {
        Log("XAML TAP factory CreateInstance");
        if (!object) return E_POINTER;
        *object = nullptr;
        if (outer) return CLASS_E_NOAGGREGATION;
        return winrt::make<Class>().as(interfaceId, object);
    } catch (...) {
        return winrt::to_hresult();
    }

    HRESULT STDMETHODCALLTYPE LockServer(BOOL) noexcept override { return S_OK; }
};

bool ConnectXamlDiagnostics() {
    HMODULE xamlModule = GetModuleHandleW(L"Microsoft.Internal.FrameworkUdk.dll");
    const wchar_t* connectionPrefix = L"WinUIVisualDiagConnection";
    if (!xamlModule) {
        xamlModule = GetModuleHandleW(L"Windows.UI.Xaml.dll");
        connectionPrefix = L"VisualDiagConnection";
    }
    if (!xamlModule) {
        Log("no XAML diagnostics runtime module loaded");
        return false;
    }

    using InitializeXamlDiagnosticsFn =
        HRESULT(WINAPI*)(LPCWSTR, DWORD, LPCWSTR, LPCWSTR, CLSID, LPCWSTR);
    auto initialize = reinterpret_cast<InitializeXamlDiagnosticsFn>(
        GetProcAddress(xamlModule, "InitializeXamlDiagnosticsEx"));
    if (!initialize) {
        Log("InitializeXamlDiagnosticsEx export missing");
        return false;
    }

    HMODULE currentModule = nullptr;
    if (!GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&CoordinatorThread), &currentModule)) {
        Log("GetModuleHandleExW(spike) error=%lu", GetLastError());
        return false;
    }

    wchar_t modulePath[MAX_PATH]{};
    const DWORD pathLength =
        GetModuleFileNameW(currentModule, modulePath, ARRAYSIZE(modulePath));
    if (!pathLength || pathLength >= ARRAYSIZE(modulePath)) {
        Log("GetModuleFileNameW(spike) error=%lu", GetLastError());
        return false;
    }

    // One process exposes one diagnostics endpoint. Always start from index 1:
    // using an increasing index makes the next window fail with
    // "endpoint not found" when several windows share the process.
    for (unsigned int index = 1; index <= 100; ++index) {
        wchar_t connectionName[128]{};
        swprintf_s(connectionName, L"%s%u", connectionPrefix, index);
        const HRESULT hr = initialize(connectionName, GetCurrentProcessId(), L"",
                                      modulePath, kSearchBoxTapClsid, nullptr);
        if (hr == HRESULT_FROM_WIN32(ERROR_NOT_FOUND)) continue;
        Log("XAML diagnostics connection %u hr=0x%08X", index, hr);
        return SUCCEEDED(hr);
    }

    Log("XAML diagnostics endpoint not found");
    return false;
}

HRESULT WaitForApartmentHandles(HANDLE* handles, ULONG count, DWORD timeout,
                                DWORD* selectedHandle) {
    return CoWaitForMultipleHandles(
        COWAIT_DISPATCH_CALLS | COWAIT_DISPATCH_WINDOW_MESSAGES, timeout,
        count, handles, selectedHandle);
}

// Start observing before new Explorer windows are created. Existing windows
// are not replayed by AdviseVisualTreeChange; they are logged by ScanFrames as
// "left untouched" in this phase.
void EnsureDiagnosticsObserver() {
    if (!kObserverOnly) return;
    // Stability test: do not install the XAML Diagnostics TAP in the shell
    // process. UIA inventory and the coordinator stay active there.
    if (wcsstr(GetCommandLineW(), L"/factory") == nullptr) {
        Log("OBSERVER skipped in shell pid=%lu", GetCurrentProcessId());
        return;
    }
    for (int attempt = 0; attempt < 40; ++attempt) {
        if (ConnectXamlDiagnostics()) {
            HANDLE handles[] = {g_tapAttachedEvent, g_stopEvent};
            DWORD index = 0;
            if (SUCCEEDED(WaitForApartmentHandles(handles, 2, 1000, &index)) &&
                index == 0) {
                Log("OBSERVER READY: visual tree notifications active");
                auto watchers = SnapshotWatchers();
                if (kEnableTreeWalk && !watchers.empty() && watchers.back()->ChannelWindow()) {
                    PostMessageW(watchers.back()->ChannelWindow(),
                                 kOwnerEnumerateMessage, 0, 0);
                    Log("TREEWALK queued after observer ready");
                }
                return;
            }
        }
        if (WaitForSingleObject(g_stopEvent, 500) == WAIT_OBJECT_0) return;
    }
    Log("OBSERVER: diagnostics endpoint unavailable");
}

// ------------------------------------------------------------ search handoff
std::string ToUtf8(const std::wstring& value) {
    if (value.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, value.c_str(),
                                         static_cast<int>(value.size()), nullptr, 0,
                                         nullptr, nullptr);
    if (size <= 0) return {};
    std::string result(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()),
                        result.data(), size, nullptr, nullptr);
    return result;
}

bool WriteSearchRequest(const std::wstring& query, const std::wstring& folder, HWND frame) {
    PWSTR raw = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &raw)) || !raw)
        return false;
    std::wstring directory = raw;
    CoTaskMemFree(raw);
    directory += L"\\EverythingExplorerSearch";
    if (!CreateDirectoryW(directory.c_str(), nullptr) &&
        GetLastError() != ERROR_ALREADY_EXISTS)
        return false;

    const std::wstring globalPath = directory + L"\\request.txt";
    std::wstring windowPath;
    if (frame && IsWindow(frame)) {
        windowPath = directory + L"\\request-" +
                     std::to_wstring(reinterpret_cast<ULONG_PTR>(frame)) + L".txt";
    }

    FILETIME now{};
    GetSystemTimeAsFileTime(&now);
    const unsigned sequence = static_cast<unsigned>(
        ((static_cast<unsigned long long>(now.dwHighDateTime) << 32) |
         now.dwLowDateTime) & 0xffffffffull);

    // bit 0 = scope to the folder the search started from, bit 1 = folders.
    const bool scoped = g_scopeToFolder && !folder.empty();
    const unsigned flags = 2u | (scoped ? 1u : 0u) | (scoped ? 4u : 0u);
    std::string data = "seq=" + std::to_string(sequence) + "\n" +
                       "max=" + std::to_string(g_maxResults) +
                       "\nflags=" + std::to_string(flags) +
                       "\nfolder=" + ToUtf8(scoped ? folder : std::wstring()) +
                       "\nquery=" + ToUtf8(query) + "\n";

    // The global file is kept for compatibility with external tools, but the
    // extension reads the per-window snapshot: otherwise a search in another
    // frame overwrites it and a refresh here adopts that query.
    auto writeOne = [&](const std::wstring& path) -> bool {
        if (path.empty()) return true;
        const std::wstring temporary = path + L".tmp";
        HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr,
                                  CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return false;
        DWORD written = 0;
        const bool ok = WriteFile(file, data.data(), static_cast<DWORD>(data.size()),
                                  &written, nullptr) &&
                        written == data.size();
        CloseHandle(file);
        if (!ok) return false;
        return MoveFileExW(temporary.c_str(), path.c_str(),
                           MOVEFILE_REPLACE_EXISTING) != FALSE;
    };

    const bool global = writeOne(globalPath);
    const bool perWindow = writeOne(windowPath);
    Log("writing search request frame=%p flags=%u folder='%ls' query='%ls' "
        "global=%d perWindow=%d",
        frame, flags, folder.c_str(), query.c_str(), global ? 1 : 0, perWindow ? 1 : 0);
    return global && perWindow;
}

static HWND AncestorWithClass(HWND hwnd, const wchar_t* className) {
    for (HWND current = hwnd; current; current = ::GetParent(current)) {
        wchar_t found[128]{};
        if (!::GetClassNameW(current, found, ARRAYSIZE(found))) continue;
        if (_wcsicmp(found, className) == 0) return current;
    }
    return nullptr;
}

// A tabbed Explorer window shares one top-level CabinetWClass frame. The
// per-tab window is a ShellTabWindowClass child, which is the identity we need
// for both the request file and the IShellBrowser.
// Which tab owns a screen rectangle (the search field). This is unambiguous
// even when the focus is elsewhere or several tabs share the same frame.
static HWND TabWindowForRect(HWND frame, const RECT& rect) {
    struct RectLookup {
        POINT center;
        HWND found;
    } lookup{{(rect.left + rect.right) / 2, (rect.top + rect.bottom) / 2}, nullptr};
    ::EnumChildWindows(
        frame,
        [](HWND hwnd, LPARAM parameter) -> BOOL {
            auto* data = reinterpret_cast<RectLookup*>(parameter);
            wchar_t className[128]{};
            if (!::GetClassNameW(hwnd, className, ARRAYSIZE(className)) ||
                _wcsicmp(className, L"ShellTabWindowClass") != 0) {
                return TRUE;
            }
            RECT windowRect{};
            if (!::GetWindowRect(hwnd, &windowRect) || !::PtInRect(&windowRect, data->center)) {
                return TRUE;
            }
            data->found = hwnd;
            return FALSE;
        },
        reinterpret_cast<LPARAM>(&lookup));
    return lookup.found;
}

// Each tab owns its own XAML dispatcher thread. Matching the thread reported
// by the diagnostics callback to the ShellTabWindowClass window identifies the
// tab unambiguously, even when several tabs have identical screen rectangles.
static HWND TabWindowForThread(DWORD pid, DWORD threadId) {
    struct ThreadLookup {
        DWORD pid;
        DWORD thread;
        HWND found;
    } lookup{pid, threadId, nullptr};

    ::EnumWindows(
        [](HWND top, LPARAM parameter) -> BOOL {
            auto* data = reinterpret_cast<ThreadLookup*>(parameter);
            DWORD windowPid = 0;
            ::GetWindowThreadProcessId(top, &windowPid);
            if (windowPid != data->pid) return TRUE;
            ::EnumChildWindows(
                top,
                [](HWND child, LPARAM inner) -> BOOL {
                    auto* found = reinterpret_cast<ThreadLookup*>(inner);
                    wchar_t className[128]{};
                    if (!::GetClassNameW(child, className, ARRAYSIZE(className)) ||
                        _wcsicmp(className, L"ShellTabWindowClass") != 0) {
                        return TRUE;
                    }
                    DWORD childPid = 0;
                    const DWORD childThread = ::GetWindowThreadProcessId(child, &childPid);
                    if (childThread == found->thread && childPid == found->pid) {
                        found->found = child;
                        return FALSE;
                    }
                    return TRUE;
                },
                parameter);
            return data->found ? FALSE : TRUE;
        },
        reinterpret_cast<LPARAM>(&lookup));
    return lookup.found;
}

static HWND ActiveTabWindow(HWND frame) {
    HWND focus = ::GetFocus();
    HWND tab = AncestorWithClass(focus, L"ShellTabWindowClass");
    if (tab && (tab == frame || ::IsChild(frame, tab))) return tab;

    // The keyboard hook runs on the coordinator thread, so GetFocus() is not
    // our focus. Ask the foreground thread for its real focus HWND instead.
    HWND foreground = ::GetForegroundWindow();
    if (foreground) {
        const DWORD foregroundThread = ::GetWindowThreadProcessId(foreground, nullptr);
        GUITHREADINFO info{};
        info.cbSize = sizeof(info);
        if (::GetGUIThreadInfo(foregroundThread, &info)) {
            tab = AncestorWithClass(info.hwndFocus, L"ShellTabWindowClass");
            if (tab && (tab == frame || ::IsChild(frame, tab))) return tab;
        }
    }

    tab = AncestorWithClass(foreground, L"ShellTabWindowClass");
    if (tab && (tab == frame || ::IsChild(frame, tab))) return tab;

    struct FirstTab {
        HWND frame;
        HWND found;
    } search{frame, nullptr};
    ::EnumChildWindows(
        frame,
        [](HWND hwnd, LPARAM parameter) -> BOOL {
            auto* data = reinterpret_cast<FirstTab*>(parameter);
            wchar_t className[128]{};
            if (!::GetClassNameW(hwnd, className, ARRAYSIZE(className)) ||
                _wcsicmp(className, L"ShellTabWindowClass") != 0 || !::IsWindowVisible(hwnd)) {
                return TRUE;
            }
            data->found = hwnd;
            return FALSE;
        },
        reinterpret_cast<LPARAM>(&search));
    return search.found;
}

bool GetBrowserForWindow(HWND frame, winrt::com_ptr<IShellBrowser>& browser) {
    const HWND targetTab = ActiveTabWindow(frame);
    winrt::com_ptr<IShellWindows> windows;
    if (FAILED(CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_ALL,
                                IID_PPV_ARGS(windows.put()))))
        return false;
    long count = 0;
    if (FAILED(windows->get_Count(&count))) return false;
    winrt::com_ptr<IShellBrowser> fallback;
    for (long i = 0; i < count; ++i) {
        VARIANT index;
        VariantInit(&index);
        V_VT(&index) = VT_I4;
        V_I4(&index) = i;
        winrt::com_ptr<IDispatch> dispatch;
        if (FAILED(windows->Item(index, dispatch.put())) || !dispatch) continue;
        winrt::com_ptr<IServiceProvider> provider;
        if (FAILED(dispatch->QueryInterface(IID_PPV_ARGS(provider.put())))) continue;
        winrt::com_ptr<IShellBrowser> candidate;
        if (FAILED(provider->QueryService(SID_STopLevelBrowser,
                                         IID_PPV_ARGS(candidate.put()))))
            continue;
        HWND window = nullptr;
        if (FAILED(candidate->GetWindow(&window)) || !window) continue;

        // The status bar belongs to the browser's tab, while GetWindow() always
        // returns the shared top-level frame.
        HWND status = nullptr;
        candidate->GetControlWindow(FCW_STATUS, &status);
        const HWND tab = AncestorWithClass(status, L"ShellTabWindowClass");
        if (targetTab && tab == targetTab) {
            browser = candidate;
            return true;
        }
        if (!fallback && (window == frame || ::IsChild(frame, window))) {
            fallback = candidate;
        }
    }
    if (fallback) {
        browser = fallback;
        return true;
    }
    return false;
}

// The LocationURL lives on the IWebBrowser2 exposed by the IShellWindows item,
// not on IShellBrowser: the latter does not implement IWebBrowser2 in current
// Explorer builds, which is why the old fallback always returned an empty path.
std::wstring LocationUrlPathForWindow(HWND frame) {
    winrt::com_ptr<IShellWindows> windows;
    if (FAILED(CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_ALL,
                                IID_PPV_ARGS(windows.put())))) {
        Log("LocationURL: no IShellWindows");
        return {};
    }
    long count = 0;
    if (FAILED(windows->get_Count(&count))) return {};
    for (long i = 0; i < count; ++i) {
        VARIANT index;
        VariantInit(&index);
        V_VT(&index) = VT_I4;
        V_I4(&index) = i;
        winrt::com_ptr<IDispatch> dispatch;
        if (FAILED(windows->Item(index, dispatch.put())) || !dispatch) continue;

        winrt::com_ptr<IServiceProvider> provider;
        if (SUCCEEDED(dispatch->QueryInterface(IID_PPV_ARGS(provider.put())))) {
            winrt::com_ptr<IShellBrowser> candidate;
            if (SUCCEEDED(provider->QueryService(SID_STopLevelBrowser,
                                                 IID_PPV_ARGS(candidate.put()))) &&
                candidate) {
                HWND window = nullptr;
                if (SUCCEEDED(candidate->GetWindow(&window)) && window &&
                    window != frame && !IsChild(frame, window)) {
                    continue;
                }
            }
        }

        winrt::com_ptr<IWebBrowser2> web;
        const HRESULT webHr = dispatch->QueryInterface(IID_PPV_ARGS(web.put()));
        if (FAILED(webHr) || !web) continue;
        BSTR url = nullptr;
        const HRESULT urlHr = web->get_LocationURL(&url);
        Log("LocationURL: item=%ld webHr=0x%08X urlHr=0x%08X url='%ls'", i,
            static_cast<unsigned long>(webHr), static_cast<unsigned long>(urlHr),
            url ? static_cast<const wchar_t*>(url) : L"");
        if (SUCCEEDED(urlHr) && url) {
            wchar_t buffer[32768]{};
            DWORD length = ARRAYSIZE(buffer);
            if (SUCCEEDED(PathCreateFromUrlW(url, buffer, &length, 0))) {
                const std::wstring path(buffer, length);
                SysFreeString(url);
                return path;
            }
            SysFreeString(url);
        }
    }
    return {};
}

const GUID kResultsFolderClsid = {
    0x7b39c2a2, 0x3e1f, 0x4e4c, {0x9e, 0x58, 0x2d, 0x9a, 0x7b, 0x1c, 0x4f, 0x60}};

bool IsResultsFolder(HWND frame) {
    winrt::com_ptr<IShellBrowser> browser;
    if (!GetBrowserForWindow(frame, browser) || !browser) return false;
    winrt::com_ptr<IShellView> view;
    if (FAILED(browser->QueryActiveShellView(view.put())) || !view) return false;
    winrt::com_ptr<IFolderView> folderView;
    winrt::com_ptr<IShellFolder> folder;
    winrt::com_ptr<IPersist> persist;
    if (FAILED(view->QueryInterface(IID_PPV_ARGS(folderView.put()))) ||
        FAILED(folderView->GetFolder(IID_PPV_ARGS(folder.put()))) ||
        FAILED(folder->QueryInterface(IID_PPV_ARGS(persist.put()))))
        return false;
    CLSID clsid{};
    return SUCCEEDED(persist->GetClassID(&clsid)) &&
           IsEqualCLSID(clsid, kResultsFolderClsid);
}

std::wstring CurrentFolderPath(HWND frame) {
    winrt::com_ptr<IShellBrowser> browser;
    const bool browserFound = GetBrowserForWindow(frame, browser) && browser;
    Log("current folder browser found=%d frame=%p", browserFound ? 1 : 0, frame);
    if (!browserFound) return {};
    std::wstring path = LocationUrlPathForWindow(frame);
    // Virtual folders can expose a parsing name such as ":::{GUID}" through
    // LocationURL. Everything's path: modifier cannot match those, so treat
    // them as "no scope" (same behaviour as the overlay mod).
    if (path.rfind(L"::", 0) == 0 || path.rfind(L"shell:", 0) == 0) {
        Log("current folder virtual LocationURL='%ls'; no scope", path.c_str());
        path.clear();
    }

    // LocationURL is empty for some views (and for shell: folders); ask the
    // active shell view instead.
    if (path.empty()) {
        winrt::com_ptr<IShellView> view;
        const HRESULT viewHr = browser->QueryActiveShellView(view.put());
        Log("current folder QueryActiveShellView hr=0x%08X", viewHr);
        if (SUCCEEDED(viewHr) && view) {
            winrt::com_ptr<IFolderView> folderView;
            winrt::com_ptr<IShellFolder> folder;
            winrt::com_ptr<IPersistFolder2> persist;
            const HRESULT h1 = view->QueryInterface(IID_PPV_ARGS(folderView.put()));
            HRESULT h2 = E_FAIL, h3 = E_FAIL, h4 = E_FAIL, h5 = E_FAIL;
            if (SUCCEEDED(h1)) h2 = folderView->GetFolder(IID_PPV_ARGS(folder.put()));
            if (SUCCEEDED(h2)) h3 = folder->QueryInterface(IID_PPV_ARGS(persist.put()));
            Log("current folder view chain hr=%08X/%08X/%08X", h1, h2, h3);
            if (SUCCEEDED(h3)) {
                PIDLIST_ABSOLUTE pidl = nullptr;
                const HRESULT pidlHr = persist->GetCurFolder(&pidl);
                Log("current folder GetCurFolder hr=0x%08X pidl=%p",
                    static_cast<unsigned long>(pidlHr), static_cast<void*>(pidl));
                if (SUCCEEDED(pidlHr) && pidl) {
                    PWSTR name = nullptr;
                    const HRESULT hr = SHGetNameFromIDList(pidl, SIGDN_FILESYSPATH, &name);
                    Log("current folder SHGetNameFromIDList hr=0x%08X name='%ls'",
                        static_cast<unsigned long>(hr), name ? name : L"");
                    if (SUCCEEDED(hr) && name) {
                        path.assign(name);
                        CoTaskMemFree(name);
                    } else {
                        // "This PC" and other virtual folders have no filesystem
                        // path. SIGDN_DESKTOPABSOLUTEPARSING would return
                        // ":::{GUID}", which Everything cannot use as a path:
                        // keep the scope empty and search globally, as the
                        // overlay mod does.
                        Log("current folder has no filesystem path; no scope");
                    }
                    CoTaskMemFree(pidl);
                }
            }
        }
    }
    Log("current folder for frame=%p: '%ls'", frame, path.c_str());
    return path;
}

void NavigateToResults(HWND frame) {
    const bool wasResults = IsResultsFolder(frame);
    PIDLIST_ABSOLUTE pidl = nullptr;
    const HRESULT parseHr = SHParseDisplayName(
        L"shell:::{7B39C2A2-3E1F-4E4C-9E58-2D9A7B1C4F60}", nullptr, &pidl, 0, nullptr);
    if (FAILED(parseHr) || !pidl) {
        Log("SHParseDisplayName(search folder) hr=0x%08X", parseHr);
        return;
    }

    winrt::com_ptr<IShellBrowser> browser;
    if (GetBrowserForWindow(frame, browser) && browser) {
        const HRESULT hr = browser->BrowseObject(pidl, SBSP_ABSOLUTE | SBSP_SAMEBROWSER);
        Log("BrowseObject hr=0x%08X wasResults=%d", hr, wasResults ? 1 : 0);
        if (FAILED(hr)) {
            SHELLEXECUTEINFOW info{};
            info.cbSize = sizeof(info);
            info.fMask = SEE_MASK_INVOKEIDLIST | SEE_MASK_IDLIST;
            info.lpVerb = L"open";
            info.lpIDList = pidl;
            info.nShow = SW_SHOWNORMAL;
            ShellExecuteExW(&info);
        } else if (wasResults) {
            // Browsing to the same CLSID PIDL is a no-op for Explorer, so a new
            // search issued from inside the results view would keep showing the
            // previous query. Ask the shell to update this folder; Explorer
            // re-enumerates and the extension reads this window's request.
            SHChangeNotify(SHCNE_UPDATEDIR, SHCNF_IDLIST | SHCNF_FLUSHNOWAIT, pidl, nullptr);
            Log("results change notification sent");
        }
    }
    CoTaskMemFree(pidl);
}

// ------------------------------------------------------------- per-window UI
constexpr UINT kSubmitQueryMessage = WM_APP + 0x55;
constexpr UINT kLayoutMessage = WM_APP + 0x56;
constexpr UINT kFocusMessage = WM_APP + 0x57;
constexpr wchar_t kLayoutPendingProperty[] = L"EsxSearchLayoutPending";
constexpr UINT_PTR kLayoutSubclassId = 0x5150;

struct Session {
    HWND frame = nullptr;
    winrt::com_ptr<VisualTreeWatcher> watcher;
    InstanceHandle searchHandle = 0;
    winrt::Microsoft::UI::Xaml::Hosting::DesktopWindowXamlSource source{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::TextBox box{nullptr};
    winrt::Microsoft::UI::Content::DesktopChildSiteBridge bridge{nullptr};
    std::wstring origin;  // folder the first search was launched from
    std::atomic_bool typedFocus{false};
    RECT field{};
    bool closing = false;
};

// All map operations happen on the coordinator thread.
std::unordered_map<HWND, std::shared_ptr<Session>> g_sessions;
std::unordered_map<HWND, winrt::com_ptr<VisualTreeWatcher>> g_frameWatchers;
std::unordered_map<HWND, InstanceHandle> g_frameElements;
std::unordered_map<HWND, ULONGLONG> g_lastFailure;
// The crash that used to be attributed to touching XAML at startup was
// reproduced with no XAML code at all: it came from killing explorer.exe with
// Stop-Process -Force in the test harness. Attach as soon as a frame exists.
constexpr ULONGLONG kStartupSettleMs = 0;
ULONGLONG g_processStartMs = 0;
DWORD g_coordinatorThreadId = 0;
HANDLE g_thread = nullptr;
HHOOK g_keyboardHook = nullptr;

// UI Automation costs ~700 ms the first time it is created in a process. Do
// that at startup, before any folder window exists, so the per-frame lookup
// is fast and the native search bar is not visible while we wait for it.
void EnsureAutomation() {
    if (g_automation) return;
    const ULONGLONG start = GetTickCount64();
    const HRESULT hr = CoCreateInstance(CLSID_CUIAutomation, nullptr,
                                        CLSCTX_INPROC_SERVER,
                                        IID_PPV_ARGS(&g_automation));
    Log("UIA warm-up hr=0x%08X ms=%llu", static_cast<unsigned long>(hr),
        GetTickCount64() - start);
}

// Ctrl+F / Ctrl+E / F3 must focus our field, never Explorer's hidden native
// search box. The hook runs on the coordinator thread, which owns g_sessions.
static std::shared_ptr<WindowState> FindActiveStateForFrame(HWND frame) {
    if (!frame) return nullptr;
    const HWND tab = ActiveTabWindow(frame);
    std::lock_guard<std::mutex> guard(g_windowStatesMutex);
    for (const auto& item : g_windowStates) {
        const auto& state = item.second;
        if (state->replacementActive &&
            ((tab && state->tab == tab) || (!tab && state->frame == frame))) {
            return state;
        }
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
static void DeleteBeforeCaret(winrt::Microsoft::UI::Xaml::Controls::TextBox const& box) {
    const auto selected = box.SelectedText();
    if (!selected.empty()) {
        box.SelectedText(L"");
        return;
    }
    const int32_t caret = box.SelectionStart();
    if (caret <= 0) return;

    std::wstring text(box.Text().c_str());
    size_t pos = static_cast<size_t>(caret);
    if (pos > text.size()) pos = text.size();
    // Do not split a UTF-16 surrogate pair when deleting the previous glyph.
    if (pos >= 2 && text[pos - 1] >= 0xD800 && text[pos - 1] <= 0xDBFF &&
        text[pos - 2] >= 0xDC00 && text[pos - 2] <= 0xDFFF) {
        --pos;
    }
    if (pos == 0) return;
    text.erase(pos - 1, 1);
    box.Text(winrt::hstring(text));
    box.SelectionStart(static_cast<int32_t>(pos - 1));
    box.SelectionLength(0);
}

LRESULT CALLBACK KeyboardHookProc(int code, WPARAM wParam, LPARAM lParam) {
    if (code == HC_ACTION && (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN)) {
        auto* key = reinterpret_cast<KBDLLHOOKSTRUCT*>(lParam);
        HWND frame = GetAncestor(GetForegroundWindow(), GA_ROOT);

        // Backspace is a shell accelerator in CabinetWClass ("go back"). It is
        // consumed before the XAML TextBox sees it, so args.Handled() cannot
        // stop it. Suppress it here and perform the edit on the XAML dispatcher.
        if (wParam == WM_KEYDOWN && key->vkCode == VK_BACK) {
            auto activeState = FindActiveStateForFrame(frame);
            Log("DIRECT hook BACK frame=%p active=%d focus=%d fg=%p",
                frame, activeState ? 1 : 0,
                activeState && activeState->textHasFocus.load() ? 1 : 0,
                GetForegroundWindow());
            if (activeState && activeState->textHasFocus.load()) {
                const bool queued = PostToStateDispatcher(
                    activeState, [activeState]() {
                        if (activeState->textBox) {
                            DeleteBeforeCaret(activeState->textBox);
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
            auto it = g_sessions.find(frame);
            if (it != g_sessions.end() && !it->second->closing &&
                it->second->typedFocus.load() && it->second->watcher &&
                it->second->searchHandle) {
                auto session = it->second;
                session->watcher->RunOnDispatcher(session->searchHandle, [session]() {
                    DeleteBeforeCaret(session->box);
                    return S_OK;
                });
                Log("Backspace consumed by search field frame=%p", frame);
                return 1;
            }
        }

        const bool control = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
        const bool alt = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
        const bool searchShortcut =
            (control && (key->vkCode == 'F' || key->vkCode == 'E')) ||
            (!control && key->vkCode == VK_F3);
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
            auto it = g_sessions.find(frame);
            if (it != g_sessions.end() && !it->second->closing &&
                it->second->watcher && it->second->searchHandle) {
                auto session = it->second;
                session->watcher->RunOnDispatcher(session->searchHandle, [session]() {
                    const bool ok = session->box.Focus(
                        winrt::Microsoft::UI::Xaml::FocusState::Programmatic);
                    Log("shortcut focus ok=%d focusHwnd=%p", ok ? 1 : 0, GetFocus());
                    return S_OK;
                });
                Log("search shortcut intercepted for frame=%p", frame);
                return 1;
            }
        }
    }
    return CallNextHookEx(g_keyboardHook, code, wParam, lParam);
}

RECT FieldRect(HWND frame) {
    RECT frameRect{};
    GetWindowRect(frame, &frameRect);
    const int dpi = GetDpiForWindow(frame);
    const double scale = dpi ? dpi / 96.0 : 1.0;
    const int width = std::max(static_cast<int>(0.3 * (frameRect.right - frameRect.left) -
                                                82 * scale + 0.5),
                               static_cast<int>(74 * scale + 0.5));
    const int x = frameRect.right - static_cast<int>(19 * scale + 0.5) - width;
    const int y = frameRect.top + static_cast<int>(48 * scale + 0.5);
    const int height = static_cast<int>(32 * scale + 0.5);
    return RECT{x, y, x + width, y + height};
}

// Update on the frame's own UI thread, immediately after its layout notification
// has been dispatched. Doing the work inside WM_WINDOWPOSCHANGED re-enters the
// XAML island; posting to the same window keeps it synchronous with the drag
// without that re-entrancy.
LRESULT CALLBACK FrameSubclassProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam,
                                   UINT_PTR subclassId, DWORD_PTR referenceData) {
    switch (message) {
        case WM_SIZE:
        case WM_WINDOWPOSCHANGED:
        case WM_DPICHANGED:
            if (!GetPropW(hwnd, kLayoutPendingProperty)) {
                SetPropW(hwnd, kLayoutPendingProperty, reinterpret_cast<HANDLE>(1));
                PostMessageW(hwnd, kLayoutMessage, 0, 0);
            }
            break;
        case WM_KEYDOWN:
            Log("frame WM_KEYDOWN vk=%d focus=%p", wParam, GetFocus());
            break;
        case kLayoutMessage: {
            RemovePropW(hwnd, kLayoutPendingProperty);
            auto* session = reinterpret_cast<Session*>(referenceData);
            if (session && session->bridge && IsWindow(hwnd) && !IsIconic(hwnd)) {
                const RECT field = FieldRect(hwnd);
                RECT local = field;
                MapWindowPoints(HWND_DESKTOP, hwnd, reinterpret_cast<POINT*>(&local), 2);
                session->bridge.MoveAndResize(winrt::Windows::Graphics::RectInt32{
                    local.left, local.top, local.right - local.left,
                    local.bottom - local.top});
                session->field = field;
            }
            return 0;
        }
        case WM_NCDESTROY:
            RemovePropW(hwnd, kLayoutPendingProperty);
            RemoveWindowSubclass(hwnd, FrameSubclassProc, subclassId);
            break;
        default:
            break;
    }
    return DefSubclassProc(hwnd, message, wParam, lParam);
}

winrt::com_ptr<VisualTreeWatcher> FindWatcherForFrame(HWND frame, const RECT& field,
                                                       bool allowHitTest = true,
                                                       InstanceHandle* outElement = nullptr) {
    auto tryWatchers = [field, frame, allowHitTest, outElement](bool knownOnly) -> winrt::com_ptr<VisualTreeWatcher> {
        for (auto& watcher : SnapshotWatchers()) {
            const HRESULT hr = knownOnly ? watcher->HideKnownSearchElement()
                                         : (allowHitTest ? watcher->FindSearchElement(field, frame, outElement)
                                                        : HRESULT_FROM_WIN32(ERROR_NOT_FOUND));
            if (SUCCEEDED(hr)) return watcher;
        }
        return nullptr;
    };

    if (auto watcher = tryWatchers(true)) return watcher;

    ResetEvent(g_tapAttachedEvent);
    if (!ConnectXamlDiagnostics()) return nullptr;
    HANDLE handles[] = {g_tapAttachedEvent, g_stopEvent};
    DWORD index = 0;
    const HRESULT hr = WaitForApartmentHandles(handles, 2, 3000, &index);
    if (FAILED(hr) || index == 1) return nullptr;

    for (auto& watcher : SnapshotWatchers()) {
        HANDLE advise[] = {watcher->AdviseCompleteEvent(), g_stopEvent};
        DWORD adviseIndex = 0;
        const HRESULT adviseHr = WaitForApartmentHandles(advise, 2, 5000, &adviseIndex);
        if (FAILED(adviseHr)) continue;
        if (!watcher->SearchElementHandle() && !allowHitTest) {
            for (int spin = 0; spin < 60 && IsWindow(frame); ++spin) {
                if (watcher->SearchElementHandle()) break;
                Sleep(5);
            }
        }
        if (watcher->SearchElementHandle()) {
            if (outElement) *outElement = watcher->SearchElementHandle();
            if (SUCCEEDED(watcher->HideKnownSearchElement())) return watcher;
        }
    }
    return allowHitTest ? tryWatchers(false) : nullptr;
}

// Each hidden element lives in its own XAML tree, so the island must be created
// on the dispatcher of the element that belongs to this frame. Resolve it by
// hit-testing the search box rectangle; the visual tree callback has already
// hidden the element, so do not hide it again here.
winrt::com_ptr<VisualTreeWatcher> ResolveFrameElement(HWND frame, const RECT& field,
                                                      InstanceHandle* matched) {
    for (auto& watcher : SnapshotWatchers()) {
        InstanceHandle candidate = 0;
        if (SUCCEEDED(watcher->FindSearchElement(field, frame, &candidate, false)) &&
            candidate) {
            *matched = candidate;
            return watcher;
        }
    }
    return nullptr;
}

// The WinAppSDK bootstrapper must NOT run while Explorer is still starting: it
// can select a newer XAML runtime (3.3) than the one Explorer already loaded
// (3.2) and the two crash each other. Wait until UIA proves the native search
// box exists, then initialize on the already-loaded runtime.
std::once_flag g_xamlRuntimeOnce;
HRESULT g_xamlRuntimeResult = E_FAIL;
PDISPATCHERQUEUECONTROLLER g_dispatcherController = nullptr;

void LogLoadedXamlRuntime(const char* phase) {
    HMODULE module = GetModuleHandleW(L"Microsoft.UI.Xaml.dll");
    if (!module) {
        Log("XAML runtime [%s]: not loaded", phase);
        return;
    }
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(module, path, ARRAYSIZE(path));
    std::string version = "?";
    const DWORD size = GetFileVersionInfoSizeW(path, nullptr);
    if (size) {
        std::vector<BYTE> buffer(size);
        if (GetFileVersionInfoW(path, 0, size, buffer.data())) {
            VS_FIXEDFILEINFO* info = nullptr;
            UINT length = 0;
            if (VerQueryValueW(buffer.data(), L"\\",
                               reinterpret_cast<void**>(&info), &length) && info) {
                char text[64];
                snprintf(text, sizeof(text), "%u.%u.%u.%u",
                         HIWORD(info->dwFileVersionMS), LOWORD(info->dwFileVersionMS),
                         HIWORD(info->dwFileVersionLS), LOWORD(info->dwFileVersionLS));
                version = text;
            }
        }
    }
    Log("XAML runtime [%s]: version=%s path=%s", phase, version.c_str(),
        ToUtf8(path).c_str());
}

HRESULT ProbeActivation() {
    try {
        winrt::get_activation_factory<
            winrt::Microsoft::UI::Xaml::Hosting::DesktopWindowXamlSource>();
        Log("activation DesktopWindowXamlSource: ok");
        return S_OK;
    } catch (...) {
        const HRESULT hr = winrt::to_hresult();
        Log("activation DesktopWindowXamlSource hr=0x%08X", hr);
        return hr;
    }
}

// WinRT activation factories are not registered for the in-box CBS copy of
// Microsoft.UI.Xaml.dll that Explorer loaded, so get_activation_factory fails
// with 0x80070490. MddBootstrapInitialize would fix that, but it loads a
// second, newer XAML runtime and crashes Explorer at startup. Instead we
// register the factories straight from the DLL that is already in the
// process: no bootstrap, no second runtime.
using GetXamlActivationFactoryFn = HRESULT(WINAPI*)(HSTRING, IActivationFactory**);
GetXamlActivationFactoryFn g_getXamlActivationFactory = nullptr;
RO_REGISTRATION_COOKIE g_xamlFactoryCookie = nullptr;

HRESULT CALLBACK XamlActivationFactoryCallback(HSTRING classId,
                                               IActivationFactory** factory) {
    if (!g_getXamlActivationFactory || !factory) return E_FAIL;
    *factory = nullptr;
    return g_getXamlActivationFactory(classId, factory);
}

HRESULT RegisterLoadedXamlFactories() {
    HMODULE xaml = ::GetModuleHandleW(L"Microsoft.UI.Xaml.dll");
    if (!xaml) return HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND);

    g_getXamlActivationFactory = reinterpret_cast<GetXamlActivationFactoryFn>(
        ::GetProcAddress(xaml, "DllGetActivationFactory"));
    if (!g_getXamlActivationFactory) return HRESULT_FROM_WIN32(::GetLastError());

    winrt::hstring classNames[] = {
        L"Microsoft.UI.Xaml.Hosting.DesktopWindowXamlSource",
        L"Microsoft.UI.Xaml.Controls.TextBox",
    };
    HSTRING ids[] = {reinterpret_cast<HSTRING>(winrt::get_abi(classNames[0])),
                     reinterpret_cast<HSTRING>(winrt::get_abi(classNames[1]))};
    PFNGETACTIVATIONFACTORY callbacks[] = {&XamlActivationFactoryCallback,
                                           &XamlActivationFactoryCallback};

    const HRESULT hr = ::RoRegisterActivationFactories(
        ids, callbacks, static_cast<UINT32>(ARRAYSIZE(ids)), &g_xamlFactoryCookie);
    Log("RoRegisterActivationFactories hr=0x%08X classes=%u", hr,
        static_cast<unsigned>(ARRAYSIZE(ids)));
    return hr;
}

void UnregisterLoadedXamlFactories() {
    if (g_xamlFactoryCookie) {
        ::RoRevokeActivationFactories(g_xamlFactoryCookie);
        g_xamlFactoryCookie = nullptr;
    }
    g_getXamlActivationFactory = nullptr;
}

// Loading WinAppSDK 3.3 next to Explorer's in-box XAML crashes Explorer while
// its XAML island is still being built. The caller only invokes this after the
// first search shortcut, i.e. on a warm process, which is the stable case.
HRESULT EnsureXamlRuntime() {
    std::call_once(g_xamlRuntimeOnce, []() {
        LogLoadedXamlRuntime("before activation");
        g_xamlRuntimeResult = ProbeActivation();
        if (FAILED(g_xamlRuntimeResult)) {
            Log("activation before registration failed hr=0x%08X",
                g_xamlRuntimeResult);
            const HRESULT registrationHr = RegisterLoadedXamlFactories();
            if (SUCCEEDED(registrationHr)) {
                g_xamlRuntimeResult = ProbeActivation();
                Log("activation after registration hr=0x%08X",
                    g_xamlRuntimeResult);
            } else {
                g_xamlRuntimeResult = registrationHr;
                // The in-box CBS runtime cannot activate our controls. Now
                // that Explorer has finished starting, load the WinAppSDK
                // runtime from the MSIX package.
                using BootstrapInitFn =
                    HRESULT(WINAPI*)(UINT32, PCWSTR, PACKAGE_VERSION);
                // The bootstrap DLL is installed next to this mod DLL, which
                // may live outside the default Windhawk directory.
                wchar_t bootstrapPath[MAX_PATH]{};
                HMODULE self = nullptr;
                if (::GetModuleHandleExW(
                        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                        reinterpret_cast<LPCWSTR>(&EnsureXamlRuntime), &self) &&
                    self &&
                    ::GetModuleFileNameW(self, bootstrapPath, ARRAYSIZE(bootstrapPath))) {
                    wchar_t* slash = std::wcsrchr(bootstrapPath, L'\\');
                    if (slash) {
                        const size_t remaining =
                            ARRAYSIZE(bootstrapPath) - static_cast<size_t>(slash + 1 - bootstrapPath);
                        ::wcscpy_s(slash + 1, remaining,
                                   L"Microsoft.WindowsAppRuntime.Bootstrap.dll");
                    } else {
                        bootstrapPath[0] = L'\0';
                    }
                }
                HMODULE bootstrap =
                    bootstrapPath[0] ? ::LoadLibraryW(bootstrapPath) : nullptr;
                Log("bootstrap module=%p", bootstrap);
                if (bootstrap) {
                    auto bootstrapInit = reinterpret_cast<BootstrapInitFn>(
                        ::GetProcAddress(bootstrap, "MddBootstrapInitialize"));
                    if (bootstrapInit) {
                        PACKAGE_VERSION minimum{};
                        minimum.Major = 2;
                        minimum.Minor = 5;
                        minimum.Build = 1;
                        g_xamlRuntimeResult =
                            bootstrapInit(0x00020000, nullptr, minimum);
                        Log("MddBootstrapInitialize hr=0x%08X", g_xamlRuntimeResult);
                    } else {
                        g_xamlRuntimeResult = E_NOINTERFACE;
                        Log("MddBootstrapInitialize export missing");
                    }
                } else {
                    g_xamlRuntimeResult = HRESULT_FROM_WIN32(::GetLastError());
                    Log("bootstrap load failed error=%lu", ::GetLastError());
                }
                if (SUCCEEDED(g_xamlRuntimeResult)) {
                    g_xamlRuntimeResult = ProbeActivation();
                    Log("activation after bootstrap hr=0x%08X", g_xamlRuntimeResult);
                }
            }
        }
        LogLoadedXamlRuntime("after activation");
        if (FAILED(g_xamlRuntimeResult)) {
            Log("XAML runtime unavailable hr=0x%08X", g_xamlRuntimeResult);
            return;
        }

        DispatcherQueueOptions options{sizeof(options), DQTYPE_THREAD_CURRENT,
                                       DQTAT_COM_STA};
        const HRESULT dispatcherHr =
            CreateDispatcherQueueController(options, &g_dispatcherController);
        Log("dispatcher hr=0x%08X", dispatcherHr);
        if (FAILED(dispatcherHr)) {
            g_xamlRuntimeResult = dispatcherHr;
        }
    });
    return g_xamlRuntimeResult;
}

HRESULT CreateSession(HWND frame) {
    if (kObserverOnly) {
        static std::unordered_set<HWND> loggedFrames;
        if (loggedFrames.insert(frame).second) {
            Log("OBSERVER ONLY: frame=%p left untouched", frame);
        }
        return E_NOTIMPL;
    }
    // UIA is warmed up at startup, so this lookup is fast once the frame
    // exists. Hide the native box before measuring its geometry.
    InstanceHandle matchedElement = 0;
    auto watcher = FindWatcherForFrame(frame, RECT{}, false, &matchedElement);
    Log("early hide attempt for frame=%p result=%d", frame, watcher ? 1 : 0);

    SearchControl nativeSearch;
    bool found = false;
    for (int attempt = 0; attempt < 20 && IsWindow(frame); ++attempt) {
        if (FindNativeSearchControl(frame, &nativeSearch)) {
            found = true;
            break;
        }
        Sleep(250);
    }
    if (!found) {
        Log("native search control not ready for frame=%p", frame);
        return E_FAIL;
    }
    const RECT field = nativeSearch.rect;
    if (!watcher) watcher = FindWatcherForFrame(frame, field, true, &matchedElement);
    if (!watcher) {
        Log("no XAML watcher could hide the search box for frame=%p", frame);
        return E_FAIL;
    }
    if (!matchedElement) matchedElement = watcher->SearchElementHandle();

    auto session = std::make_shared<Session>();
    session->frame = frame;
    session->watcher = watcher;
    session->searchHandle = matchedElement;
    session->field = field;

    const HRESULT hr = watcher->RunOnDispatcher(session->searchHandle, [session, frame, field]() {
        Log("constructing source on Explorer XAML dispatcher");
        session->source =
            winrt::Microsoft::UI::Xaml::Hosting::DesktopWindowXamlSource();
        winrt::Microsoft::UI::WindowId parent{
            static_cast<uint64_t>(reinterpret_cast<uintptr_t>(frame))};
        session->source.Initialize(parent);
        Log("source initialized for frame=%p", frame);

        session->box = winrt::Microsoft::UI::Xaml::Controls::TextBox();
        session->box.PlaceholderText(L"Cerca con Everything");
        session->box.MaxLength(1024);
        session->box.VerticalContentAlignment(
            winrt::Microsoft::UI::Xaml::VerticalAlignment::Center);
        session->box.GotFocus([weak = std::weak_ptr<Session>(session)](auto const&, auto const&) {
            auto session = weak.lock();
            if (!session) return;
            session->typedFocus = true;
            Log("search field got focus frame=%p", session->frame);
        });
        session->box.LostFocus([weak = std::weak_ptr<Session>(session)](auto const&, auto const&) {
            auto session = weak.lock();
            if (session) session->typedFocus = false;
        });
        session->box.KeyDown(
            [frame, weak = std::weak_ptr<Session>(session)](
                winrt::Windows::Foundation::IInspectable const& sender,
                winrt::Microsoft::UI::Xaml::Input::KeyRoutedEventArgs const& args) {
                auto session = weak.lock();
                if (!session) {
                    args.Handled(true);
                    return;
                }
                const int key = static_cast<int>(args.Key());
                // The XAML island forwards unhandled keys to Explorer's command
                // bar, where Backspace means "go back". Our TextBox owns every
                // keystroke while it is focused.
                args.Handled(true);
                auto box = sender.as<winrt::Microsoft::UI::Xaml::Controls::TextBox>();
                if (key == VK_ESCAPE) {
                    box.Text(L"");
                    args.Handled(true);
                    return;
                }
                if (key != VK_RETURN) {
                    Log("XAML key=%d text='%ls' focusHwnd=%p", key, box.Text().c_str(),
                        GetFocus());
                    return;
                }

                const auto query = box.Text();
                Log("XAML Enter on frame=%p query='%ls'", frame, query.c_str());
                if (query.empty()) {
                    args.Handled(true);
                    return;
                }

                // Prefer the main mod's overlay when it is active; otherwise
                // submit directly from this spike. Do the work right here on
                // Explorer's XAML dispatcher: PostThreadMessage messages were
                // consumed by CoWaitForMultipleHandles before the coordinator
                // could see them.
                HWND overlay = reinterpret_cast<HWND>(
                    GetPropW(frame, L"EverythingSearchBoxOverlay.Handle"));
                HWND edit = IsWindow(overlay)
                                ? FindWindowExW(overlay, nullptr, L"EDIT", nullptr)
                                : nullptr;
                if (IsWindow(edit)) {
                    SetWindowTextW(edit, query.c_str());
                    PostMessageW(overlay, WM_APP + 0x54, 0, 0);
                    Log("XAML query forwarded to overlay");
                } else {
                    const ULONGLONG tStart = GetTickCount64();
                    const bool inResults = IsResultsFolder(frame);
                    const std::wstring current = CurrentFolderPath(frame);
                    const ULONGLONG tScope = GetTickCount64();
                    std::wstring scope;
                    if (inResults) {
                        scope = session->origin;
                    } else if (!current.empty()) {
                        scope = current;
                        session->origin = current;
                    }
                    Log("search scope results=%d current='%ls' origin='%ls'",
                        inResults ? 1 : 0, current.c_str(), scope.c_str());
                    const bool written =
                        WriteSearchRequest(std::wstring(query.c_str()), scope, frame);
                    const ULONGLONG tWrite = GetTickCount64();
                    Log("XAML request written=%d", written ? 1 : 0);
                    if (written) NavigateToResults(frame);
                    Log("TIMING submit scope=%llu write=%llu nav=%llu total=%llu",
                        tScope - tStart, tWrite - tScope,
                        GetTickCount64() - tWrite, GetTickCount64() - tStart);
                    // Keep the field active so the query can be edited and
                    // submitted again inside the same result folder.
                    box.Focus(winrt::Microsoft::UI::Xaml::FocusState::Programmatic);
                }
                args.Handled(true);
            });
        session->source.Content(session->box);

        RECT local = field;
        MapWindowPoints(HWND_DESKTOP, frame, reinterpret_cast<POINT*>(&local), 2);
        session->bridge = session->source.SiteBridge();
        session->bridge.MoveAndResize(winrt::Windows::Graphics::RectInt32{
            local.left, local.top, local.right - local.left, local.bottom - local.top});
        session->bridge.MoveInZOrderAtTop();
        session->bridge.Show();
        Log("replacement shown (no automatic focus) for frame=%p", frame);
        return S_OK;
    });

    if (FAILED(hr)) {
        Log("replacement creation failed for frame=%p hr=0x%08X", frame, hr);
        watcher->Detach();
        UnregisterWatcher(watcher.get());
        return hr;
    }

    DWORD_PTR existingReference = 0;
    if (!GetWindowSubclass(frame, FrameSubclassProc, kLayoutSubclassId,
                           &existingReference)) {
        SetWindowSubclass(frame, FrameSubclassProc, kLayoutSubclassId,
                          reinterpret_cast<DWORD_PTR>(session.get()));
        Log("frame subclass installed for frame=%p", frame);
    }
    g_sessions[frame] = session;
    return S_OK;
}

void CloseSession(const std::shared_ptr<Session>& session, bool frameGone = false) {
    if (session->closing) return;
    session->closing = true;
    Log("closing session for frame=%p gone=%d", session->frame, frameGone ? 1 : 0);
    if (frameGone || !IsWindow(session->frame)) {
        // The frame was destroyed: its XAML island, the dispatcher and the
        // hidden element are gone. Do not call back into XAML.
        session->bridge = nullptr;
        session->box = nullptr;
        session->source = nullptr;
        if (session->watcher) session->watcher->Abandon();
        return;
    }
    RemoveWindowSubclass(session->frame, FrameSubclassProc, kLayoutSubclassId);
    RemovePropW(session->frame, kLayoutPendingProperty);
    try {
        if (session->source && session->watcher && session->searchHandle) {
            auto source = session->source;
            session->watcher->RunOnDispatcher(session->searchHandle, [source]() {
                source.Content(nullptr);
                source.Close();
                return S_OK;
            });
        }
    } catch (...) {
        Log("closing XAML island failed hr=0x%08X", winrt::to_hresult());
    }
    session->bridge = nullptr;
    session->box = nullptr;
    session->source = nullptr;
    if (session->watcher) {
        session->watcher->Detach();
        UnregisterWatcher(session->watcher.get());
    }
}

void UpdateGeometry() {
    for (auto& entry : g_sessions) {
        auto& session = entry.second;
        if (session->closing || !IsWindow(session->frame) || IsIconic(session->frame)) continue;
        const RECT field = FieldRect(session->frame);
        if (RectsMatch(field, session->field)) continue;
        if (!session->bridge || !session->watcher) continue;

        RECT local = field;
        MapWindowPoints(HWND_DESKTOP, session->frame, reinterpret_cast<POINT*>(&local), 2);
        const HRESULT hr = session->watcher->RunOnDispatcher(
            session->searchHandle,
            [bridge = session->bridge, local]() {
                bridge.MoveAndResize(winrt::Windows::Graphics::RectInt32{
                    local.left, local.top, local.right - local.left,
                    local.bottom - local.top});
                return S_OK;
            });
        if (SUCCEEDED(hr)) {
            session->field = field;
        }
    }
}

void ScanFrames() {
    static bool loggedDeferral = false;
    const ULONGLONG uptime = ::GetTickCount64() - g_processStartMs;
    if (uptime < kStartupSettleMs) {
        if (!loggedDeferral) {
            Log("deferring all XAML work: uptime=%llu ms (need %llu)", uptime,
                static_cast<ULONGLONG>(kStartupSettleMs));
            loggedDeferral = true;
        }
        return;
    }
    std::vector<HWND> frames;
    EnumWindows([](HWND hwnd, LPARAM parameter) -> BOOL {
        DWORD processId = 0;
        GetWindowThreadProcessId(hwnd, &processId);
        if (processId != GetCurrentProcessId() || !IsWindowVisible(hwnd)) return TRUE;
        wchar_t className[128]{};
        if (!GetClassNameW(hwnd, className, ARRAYSIZE(className))) return TRUE;
        if (wcscmp(className, L"CabinetWClass") != 0) return TRUE;
        reinterpret_cast<std::vector<HWND>*>(parameter)->push_back(hwnd);
        return TRUE;
    }, reinterpret_cast<LPARAM>(&frames));

    if (g_replacementActive.load()) {
        if (!g_experimentFrame && !frames.empty()) {
            g_experimentFrame = frames.front();
        }
        if (g_experimentFrame && !IsWindow(g_experimentFrame)) {
            Log("OWNER ROLLBACK: frame %p gone", g_experimentFrame);
            RestoreReplacement(true);
            g_experimentFrame = nullptr;
        }
    }

    static std::unordered_set<HWND> inventoriedFrames;

    for (HWND frame : frames) {
        if (kObserverOnly) {
            if (!inventoriedFrames.count(frame)) {
                // A frame already mapped from its XAML dispatcher thread does
                // not need the UIA sweep: that FindFirst call can block the
                // coordinator for hundreds of ms and is only a fallback.
                {
                    bool mapped = false;
                    {
                        std::lock_guard<std::mutex> guard(g_windowStatesMutex);
                        for (const auto& item : g_windowStates) {
                            if (item.second->frame == frame && item.second->xamlMapped) {
                                mapped = true;
                                break;
                            }
                        }
                    }
                    if (mapped) {
                        inventoriedFrames.insert(frame);
                        continue;
                    }
                }
                SearchControl control;
                if (FindNativeSearchControl(frame, &control)) {
                    g_nativeRect = control.rect;
                    RecordUiaDiscovered(frame, control.rect);
                    inventoriedFrames.insert(frame);
                }
            }
            continue;
        }
        if (g_sessions.count(frame)) continue;
        const ULONGLONG lastFailure = g_lastFailure[frame];
        if (lastFailure && GetTickCount64() - lastFailure < 250) continue;
        Log("new frame=%p, creating replacement", frame);
        if (FAILED(CreateSession(frame))) g_lastFailure[frame] = GetTickCount64();
        else g_lastFailure.erase(frame);
    }
    for (auto it = g_sessions.begin(); it != g_sessions.end();) {
        HWND frame = it->first;
        const bool present = std::find(frames.begin(), frames.end(), frame) != frames.end();
        if (!present) {
            CloseSession(it->second, true);
            it = g_sessions.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = inventoriedFrames.begin(); it != inventoriedFrames.end();) {
        if (std::find(frames.begin(), frames.end(), *it) == frames.end()) {
            ForgetUiaFrame(*it);
            it = inventoriedFrames.erase(it);
        } else {
            ++it;
        }
    }
}

DWORD WINAPI CoordinatorThread(LPVOID) {
    Log("coordinator thread start");
    MSG queueMessage{};
    PeekMessageW(&queueMessage, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
    g_coordinatorThreadId = GetCurrentThreadId();

    struct DiagnosticsCleanup {
        ~DiagnosticsCleanup() { StopAllXamlDiagnostics(); }
    } diagnosticsCleanup;

    try {
        winrt::init_apartment(winrt::apartment_type::single_threaded);
        Log("apartment ok");
        EnsureAutomation();
        // Install the keyboard hook first: EnsureDiagnosticsObserver() can retry
        // for up to a minute, and Ctrl+F must work immediately at startup.
        g_keyboardHook = SetWindowsHookExW(WH_KEYBOARD_LL, KeyboardHookProc, nullptr, 0);
        Log("keyboard hook=%p", g_keyboardHook);
        EnsureDiagnosticsObserver();
        // XAML runtime bootstrap and dispatcher creation are deferred to
        // CreateSession, which runs as soon as the native search box exists.
        // Scan immediately: a visible native bar before the replacement is
        // the delay we are trying to remove.

        ULONGLONG lastInventory = 0;
        ULONGLONG lastPhase2Attempt = 0;

        for (;;) {
            HANDLE stopHandles[] = {g_stopEvent};
            DWORD stopIndex = 0;
            const HRESULT waitHr = WaitForApartmentHandles(stopHandles, 1, 16, &stopIndex);

            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                if (message.message == WM_QUIT) {
                    Log("coordinator WM_QUIT");
                    goto shutdown;
                }
                if (message.hwnd == nullptr && message.message == kSubmitQueryMessage) {
                    auto* query = reinterpret_cast<std::wstring*>(message.lParam);
                    if (query) {
                        Log("submitting query from XAML: %ls", query->c_str());
                        WriteSearchRequest(*query, {},
                                           reinterpret_cast<HWND>(message.wParam));
                        NavigateToResults(reinterpret_cast<HWND>(message.wParam));
                        delete query;
                    }
                } else {
                    DispatchMessageW(&message);
                }
            }
            (void)waitHr;  // CoWaitForMultipleHandles may return RPC_E_DISCONNECTED
                           // while COM is torn down; poll the stop event instead.
            if (g_stopping.load() ||
                WaitForSingleObject(g_stopEvent, 0) == WAIT_OBJECT_0) {
                Log("coordinator stop requested");
                break;
            }

            // Correlate and swap BEFORE the UIA sweep: FindFirst(Descendants)
            // in ScanFrames can block this thread for hundreds of ms, which
            // would otherwise delay the replacement. The 2 s timer below is
            // only for verbose logging.
            CorrelateWindowStates();
            RunC3ForReadyStates();

            ScanFrames();
            UpdateGeometry();

            const ULONGLONG now = GetTickCount64();

            // Explorer shares one search box between the tabs of a window.
            // When the active tab changes, swap the text with the value saved
            // for that tab (empty for a tab that never searched).
            std::vector<std::shared_ptr<WindowState>> textStates;
            {
                std::lock_guard<std::mutex> guard(g_windowStatesMutex);
                for (const auto& item : g_windowStates) {
                    if (item.second->textBox) textStates.push_back(item.second);
                }
            }
            for (const auto& state : textStates) {
                if (!::IsWindow(state->frame)) continue;
                const HWND tab = ActiveTabWindow(state->frame);
                if (!tab) continue;
                HWND previous = nullptr;
                {
                    std::lock_guard<std::mutex> guard(g_tabTextMutex);
                    const auto it = g_frameLastTab.find(state->frame);
                    if (it != g_frameLastTab.end()) previous = it->second;
                    g_frameLastTab[state->frame] = tab;
                }
                if (!previous || previous == tab) continue;
                {
                    // FindActiveStateForFrame() matches state->tab: keep it in
                    // sync with the tab we just switched to.
                    std::lock_guard<std::mutex> guard(g_windowStatesMutex);
                    state->tab = tab;
                }
                const std::wstring text = LoadTabText(tab);
                PostToStateDispatcher(state, [state, text, tab]() {
                    if (!state->textBox) return;
                    state->ignoreNextEmpty.store(true);
                    state->textBox.Text(winrt::hstring(text));
                    state->textBox.SelectionStart(static_cast<int32_t>(text.size()));
                    state->textBox.SelectionLength(0);
                    Log("tab switched to=%p restored query='%ls'", tab, text.c_str());
                });
            }

            // Keep the search buttons in sync with the folder the window shows.
            // Snapshot under the map lock: XAML callbacks on other threads can
            // insert/erase WindowState objects while this loop runs.
            std::vector<std::shared_ptr<WindowState>> buttonStates;
            {
                std::lock_guard<std::mutex> guard(g_windowStatesMutex);
                buttonStates.reserve(g_windowStates.size());
                for (const auto& item : g_windowStates) {
                    if (item.second->commandBarAdded.load()) {
                        buttonStates.push_back(item.second);
                    }
                }
            }
            for (const auto& state : buttonStates) {
                const ULONGLONG last = state->lastButtonCheck.load();
                if (now - last < 750) continue;
                state->lastButtonCheck.store(now);
                const bool visible = IsResultsFolder(state->frame);
                if (visible == state->buttonsVisible.load()) continue;
                state->buttonsVisible.store(visible);
                PostToStateDispatcher(state, [state, visible]() {
                    const auto value = visible
                                           ? winrt::Microsoft::UI::Xaml::Visibility::Visible
                                           : winrt::Microsoft::UI::Xaml::Visibility::Collapsed;
                    if (state->optionsButton) state->optionsButton.Visibility(value);
                });
            }

            if (now - lastInventory >= 10000) {
                lastInventory = now;
                LogProcessInventory();
            }
            if (now - lastPhase2Attempt >= 2000) {
                lastPhase2Attempt = now;
                LogCallbackProbeResult();
                LogWindowStates();
                LogAddTxResult();
                if (kAutoRollback) RestoreReplacement(false);
                LogRollbackResult();
            }
        }
    shutdown:
        if (g_keyboardHook) {
            UnhookWindowsHookEx(g_keyboardHook);
            g_keyboardHook = nullptr;
        }
        UnregisterLoadedXamlFactories();
        for (auto& entry : g_sessions) CloseSession(entry.second);
        g_sessions.clear();
    } catch (winrt::hresult_error const& error) {
        Log("hresult_error 0x%08X: %ls", error.code().value, error.message().c_str());
    } catch (std::exception const& error) {
        Log("std::exception: %s", error.what());
    } catch (...) {
        Log("unknown exception");
    }
    Log("coordinator thread exit");
    return 0;
}

}  // namespace

// ------------------------------------------------------------------- exports
STDAPI DllGetClassObject(REFCLSID classId, REFIID interfaceId, LPVOID FAR* object) {
    if (!object) return E_POINTER;
    *object = nullptr;
    if (!IsEqualCLSID(classId, kSearchBoxTapClsid)) return CLASS_E_CLASSNOTAVAILABLE;
    try {
        return winrt::make<SimpleFactory<SearchBoxTap>>().as(interfaceId, object);
    } catch (...) {
        return winrt::to_hresult();
    }
}

STDAPI DllCanUnloadNow(void) {
    const bool locked = winrt::get_module_lock() != 0;
    Log("DllCanUnloadNow locked=%d count=%lu", locked ? 1 : 0,
        static_cast<unsigned long>(winrt::get_module_lock()));
    return locked ? S_FALSE : S_OK;
}

void ReadSettings() {
    const int maxResults = Wh_GetIntSetting(L"maxResults");
    g_maxResults = maxResults < 0 ? 0 : maxResults;
    g_scopeToFolder = Wh_GetIntSetting(L"scopeToFolder") != 0;
}

extern "C" BOOL Wh_ModInit() {
    Log("Wh_ModInit pid=%lu", GetCurrentProcessId());
    ReadSettings();
    // The observer must run in every Explorer process, including the shell:
    // normal folder windows are hosted there. Earlier crashes in the shell were
    // caused by the WH_GETMESSAGE hook and by another Windhawk mod, both now
    // removed/disabled. No XAML mutation happens in this phase.
    g_probeAllowed.store(true);
    Log("PROBE enabled pid=%lu cmd='%ls'", GetCurrentProcessId(),
        GetCommandLineW());

    g_processStartMs = ::GetTickCount64();
    g_stopping = false;
    g_tapAttachedEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!g_tapAttachedEvent || !g_stopEvent) {
        Log("event creation failed error=%lu", GetLastError());
        return FALSE;
    }
    g_thread = CreateThread(nullptr, 0, CoordinatorThread, nullptr, 0, nullptr);
    return g_thread != nullptr;
}

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
        &resolved, 2000);
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
extern "C" void Wh_ModUninit() {
    Log("Wh_ModUninit");
    // Never leave Explorer without its native search bar.
    RollbackAllReplacements();
    {
        std::lock_guard<std::mutex> guard(g_lifecycleMutex);
        g_stopping = true;
        if (g_stopEvent) SetEvent(g_stopEvent);
    }
    bool coordinatorStopped = true;
    if (g_thread) {
        // Bound the join: a hung XAML dispatcher must never block mod unload
        // indefinitely (Windhawk shows "Uninitializing..." forever).
        DWORD wait = WaitForSingleObject(g_thread, 3000);
        if (wait == WAIT_TIMEOUT) {
            Log("Wh_ModUninit: coordinator join timed out, waiting 5s more");
            wait = WaitForSingleObject(g_thread, 5000);
        }
        coordinatorStopped = wait == WAIT_OBJECT_0;
        Log("Wh_ModUninit: coordinator join result=%lu stopped=%d",
            static_cast<unsigned long>(wait), coordinatorStopped ? 1 : 0);
        if (coordinatorStopped) {
            CloseHandle(g_thread);
            g_thread = nullptr;
        }
    }
    // If the coordinator did not get to its own cleanup, stop every watcher
    // thread here: Windhawk waits up to 80s for mod code to leave thread
    // stacks before unloading the DLL.
    StopAllXamlDiagnostics();
    // Closing events while the coordinator is still alive would leave it
    // waiting on invalid handles; keep them open in that case.
    if (coordinatorStopped) {
        if (g_stopEvent) { CloseHandle(g_stopEvent); g_stopEvent = nullptr; }
        if (g_tapAttachedEvent) {
            CloseHandle(g_tapAttachedEvent);
            g_tapAttachedEvent = nullptr;
        }
    }
}

extern "C" void Wh_ModSettingsChanged() {
    ReadSettings();
    Log("Wh_ModSettingsChanged maxResults=%d scopeToFolder=%d", g_maxResults,
        g_scopeToFolder ? 1 : 0);
}
