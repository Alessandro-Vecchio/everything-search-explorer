#include "search_actions.h"

#include <exdisp.h>
#include <shlguid.h>
#include <shobjidl.h>

#include <cstdint>
#include <cwchar>

#include "protocol.h"
#include "trace.h"

namespace {

// Finds the Explorer window that owns a view HWND. This runs in-process in
// explorer.exe, so the same IShellWindows/SID_STopLevelBrowser route the mod
// uses is available here too.
IShellBrowser* FindBrowser(HWND owner) {
    if (!owner) return nullptr;

    IShellWindows* windows = nullptr;
    if (FAILED(::CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_ALL,
                                  IID_PPV_ARGS(&windows))) ||
        !windows) {
        return nullptr;
    }

    long count = 0;
    windows->get_Count(&count);

    IShellBrowser* found = nullptr;
    for (long i = 0; i < count && !found; ++i) {
        VARIANT index;
        VariantInit(&index);
        V_VT(&index) = VT_I4;
        V_I4(&index) = i;

        IDispatch* dispatch = nullptr;
        if (FAILED(windows->Item(index, &dispatch)) || !dispatch) continue;

        IServiceProvider* provider = nullptr;
        if (SUCCEEDED(dispatch->QueryInterface(IID_PPV_ARGS(&provider))) && provider) {
            IShellBrowser* candidate = nullptr;
            if (SUCCEEDED(provider->QueryService(SID_STopLevelBrowser,
                                                 IID_PPV_ARGS(&candidate))) &&
                candidate) {
                HWND window = nullptr;
                if (SUCCEEDED(candidate->GetWindow(&window)) && window) {
                    const HWND root = ::GetAncestor(owner, GA_ROOT);
                    if (window == root || ::IsChild(window, owner) || ::IsChild(root, window)) {
                        found = candidate;
                    }
                }
                if (!found) candidate->Release();
            }
            provider->Release();
        }
        dispatch->Release();
    }
    windows->Release();
    return found;
}

void RefreshView(HWND owner) {
    IShellBrowser* browser = FindBrowser(owner);
    if (!browser) return;

    IShellView* view = nullptr;
    if (SUCCEEDED(browser->QueryActiveShellView(&view)) && view) {
        view->Refresh();
        view->Release();
    }
    browser->Release();
}

// ------------------------------------------------------------ options dialog

constexpr UINT kCheckScope = 100;
constexpr UINT kCheckFolders = 101;
constexpr UINT kLabelMax = 102;
constexpr UINT kEditMax = 103;
constexpr UINT kButtonOk = 104;
constexpr UINT kButtonCancel = 105;

struct OptionsState {
    bool accepted = false;
    bool done = false;
    std::uint32_t flags = 0;
    std::uint32_t maxResults = 0;
};

LRESULT CALLBACK OptionsWndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* state = reinterpret_cast<OptionsState*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (message) {
        case WM_CREATE: {
            state = reinterpret_cast<OptionsState*>(
                reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
            ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));

            HFONT font = reinterpret_cast<HFONT>(::GetStockObject(DEFAULT_GUI_FONT));
            auto control = [&](const wchar_t* className, const wchar_t* text, DWORD style, int x,
                               int y, int width, int height, UINT id) {
                HWND child =
                    ::CreateWindowExW(0, className, text, WS_CHILD | WS_VISIBLE | style, x, y,
                                      width, height, hwnd, reinterpret_cast<HMENU>(INT_PTR(id)),
                                      ::GetModuleHandleW(nullptr), nullptr);
                if (child) ::SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
                return child;
            };

            control(L"Button", L"Scope to the searched folder", BS_AUTOCHECKBOX, 14, 16, 260, 20,
                    kCheckScope);
            control(L"Button", L"Include folders in results", BS_AUTOCHECKBOX, 14, 42, 260, 20,
                    kCheckFolders);
            control(L"Static", L"Maximum results (0 = all):", 0, 14, 76, 200, 20, kLabelMax);
            HWND edit = control(L"Edit", L"", WS_BORDER | ES_AUTOHSCROLL | ES_NUMBER, 210, 74, 70,
                                22, kEditMax);
            if (edit) ::SendMessageW(edit, EM_LIMITTEXT, 8, 0);
            control(L"Button", L"OK", BS_DEFPUSHBUTTON, 104, 114, 80, 26, kButtonOk);
            control(L"Button", L"Cancel", BS_PUSHBUTTON, 194, 114, 80, 26, kButtonCancel);
            return 0;
        }
        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case kButtonOk:
                    // Read the controls before DestroyWindow tears them down.
                    if (state) {
                        state->flags = 0;
                        if (::IsDlgButtonChecked(hwnd, kCheckScope) == BST_CHECKED) {
                            state->flags |= esx::ScopeToFolder;
                        }
                        if (::IsDlgButtonChecked(hwnd, kCheckFolders) == BST_CHECKED) {
                            state->flags |= esx::IncludeFolders;
                        }
                        state->maxResults = ::GetDlgItemInt(hwnd, kEditMax, nullptr, FALSE);
                        state->accepted = true;
                    }
                    ::DestroyWindow(hwnd);
                    return 0;
                case kButtonCancel:
                    ::DestroyWindow(hwnd);
                    return 0;
            }
            break;
        case WM_CLOSE:
            ::DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            if (state) state->done = true;
            return 0;
        default:
            break;
    }
    return ::DefWindowProcW(hwnd, message, wParam, lParam);
}

bool EnsureOptionsClass() {
    static bool registered = false;
    if (registered) return true;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = OptionsWndProc;
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"EverythingSearchOptionsWnd";
    registered = ::RegisterClassExW(&wc) != 0;
    return registered;
}

void CenterOver(HWND hwnd, HWND owner) {
    RECT own{}, target{};
    if (!::GetWindowRect(hwnd, &own)) return;
    if (!owner || !::GetWindowRect(owner, &target)) {
        MONITORINFO monitor{};
        monitor.cbSize = sizeof(monitor);
        if (!::GetMonitorInfoW(::MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &monitor)) {
            return;
        }
        target = monitor.rcWork;
    }
    const int width = own.right - own.left;
    const int height = own.bottom - own.top;
    int x = target.left + (target.right - target.left - width) / 2;
    int y = target.top + (target.bottom - target.top - height) / 2;

    MONITORINFO monitor{};
    monitor.cbSize = sizeof(monitor);
    if (::GetMonitorInfoW(::MonitorFromRect(&target, MONITOR_DEFAULTTONEAREST), &monitor)) {
        if (x < monitor.rcWork.left) x = monitor.rcWork.left;
        if (y < monitor.rcWork.top) y = monitor.rcWork.top;
        if (x + width > monitor.rcWork.right) x = monitor.rcWork.right - width;
        if (y + height > monitor.rcWork.bottom) y = monitor.rcWork.bottom - height;
    }
    ::SetWindowPos(hwnd, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
}

}  // namespace

void EsxShowSearchOptions(HWND owner) {
    ESX_TRACE_LOG("Search options dialog");
    if (!EnsureOptionsClass()) return;

    esx::QueryRequest request;
    if (!esx::ReadRequestFile(request)) {
        // No request yet: start from the same defaults the mod would use.
        request.flags = esx::ScopeToFolder | esx::IncludeFolders;
    }

    OptionsState state;
    HWND dialog = ::CreateWindowExW(
        WS_EX_DLGMODALFRAME, L"EverythingSearchOptionsWnd", L"Search options",
        WS_POPUP | WS_CAPTION | WS_SYSMENU, 0, 0, 292, 162, owner, nullptr,
        ::GetModuleHandleW(nullptr), &state);
    if (!dialog) return;

    ::CheckDlgButton(dialog, kCheckScope,
                     (request.flags & esx::ScopeToFolder) ? BST_CHECKED : BST_UNCHECKED);
    ::CheckDlgButton(dialog, kCheckFolders,
                     (request.flags & esx::IncludeFolders) ? BST_CHECKED : BST_UNCHECKED);
    ::SetDlgItemInt(dialog, kEditMax, request.maxResults, FALSE);
    CenterOver(dialog, owner);
    ::ShowWindow(dialog, SW_SHOW);
    ::SetForegroundWindow(dialog);

    MSG message{};
    while (!state.done && ::GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (!::IsDialogMessageW(dialog, &message)) {
            ::TranslateMessage(&message);
            ::DispatchMessageW(&message);
        }
    }

    if (!state.accepted) return;

    request.flags = state.flags;
    request.maxResults = state.maxResults;
    request.seq = ::GetTickCount();
    ESX_TRACE_LOG("Search options applied: flags=0x%lX max=%lu", request.flags,
                  request.maxResults);

    if (!esx::WriteRequestFile(request)) {
        ESX_TRACE_LOG("Search options: could not write request.txt");
        return;
    }
    RefreshView(owner);
}

void EsxCloseSearch(HWND owner) {
    esx::QueryRequest request;
    if (!esx::ReadRequestFile(request) || request.folder.empty()) {
        ESX_TRACE_LOG("Close search: no source folder to go back to");
        return;
    }

    IShellBrowser* browser = FindBrowser(owner);
    if (!browser) {
        ESX_TRACE_LOG("Close search: no shell browser for owner %p", static_cast<void*>(owner));
        return;
    }

    PIDLIST_ABSOLUTE pidl = nullptr;
    if (SUCCEEDED(::SHParseDisplayName(request.folder.c_str(), nullptr, &pidl, 0, nullptr)) &&
        pidl) {
        ESX_TRACE_LOG("Close search -> %ls", request.folder.c_str());
        browser->BrowseObject(pidl, SBSP_ABSOLUTE | SBSP_SAMEBROWSER);
        ::CoTaskMemFree(pidl);
    }
    browser->Release();
}
