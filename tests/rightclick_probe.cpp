// Reproduces what a user does on a result: right-click the first item of the
// virtual folder and report whether a shell context menu appeared, whether the
// window survived, and what the extension logged.
//
// Usage:
//   rightclick_probe.exe "Everything Search"
#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <exdisp.h>
#include <shlwapi.h>

#include <cstdio>
#include <string>

namespace {

IShellBrowser* FindBrowser(const std::wstring& needle, HWND* frame) {
    IShellWindows* windows = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&windows))))
        return nullptr;

    long count = 0;
    windows->get_Count(&count);

    IShellBrowser* browser = nullptr;
    for (long i = 0; i < count && !browser; ++i) {
        VARIANT index;
        VariantInit(&index);
        V_VT(&index) = VT_I4;
        V_I4(&index) = i;

        IDispatch* dispatch = nullptr;
        if (FAILED(windows->Item(index, &dispatch)) || !dispatch) continue;

        IWebBrowser2* web = nullptr;
        if (SUCCEEDED(dispatch->QueryInterface(IID_PPV_ARGS(&web))) && web) {
            BSTR location = nullptr;
            web->get_LocationName(&location);
            const bool match = location && wcsstr(location, needle.c_str()) != nullptr;
            SysFreeString(location);
            web->Release();
            if (!match) {
                dispatch->Release();
                continue;
            }
        }

        IServiceProvider* provider = nullptr;
        if (SUCCEEDED(dispatch->QueryInterface(IID_PPV_ARGS(&provider))) && provider) {
            IShellBrowser* candidate = nullptr;
            if (SUCCEEDED(provider->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(&candidate))) &&
                candidate) {
                browser = candidate;
                candidate->GetWindow(frame);
            }
            provider->Release();
        }
        dispatch->Release();
    }
    windows->Release();
    return browser;
}

bool MenuVisible() { return FindWindowW(L"#32768", nullptr) != nullptr; }

}  // namespace

int wmain(int argc, wchar_t** argv) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const std::wstring needle = (argc > 1) ? argv[1] : L"Everything Search";

    HWND frame = nullptr;
    IShellBrowser* browser = FindBrowser(needle, &frame);
    if (!browser) {
        wprintf(L"window '%ls' not found\n", needle.c_str());
        return 1;
    }

    IShellView* view = nullptr;
    if (FAILED(browser->QueryActiveShellView(&view)) || !view) {
        wprintf(L"QueryActiveShellView failed\n");
        return 1;
    }

    HWND viewWindow = nullptr;
    view->GetWindow(&viewWindow);
    wprintf(L"frame=%p view=%p\n", frame, viewWindow);

    IFolderView* folderView = nullptr;
    const HRESULT viewQi = view->QueryInterface(IID_PPV_ARGS(&folderView));
    wprintf(L"IFolderView: 0x%08X\n", viewQi);

    POINT point{};
    bool positioned = false;
    if (folderView) {
        IEnumIDList* items = nullptr;
        const HRESULT itemsHr = folderView->Items(SVGIO_ALLVIEW, IID_PPV_ARGS(&items));
        wprintf(L"Items: 0x%08X\n", itemsHr);
        if (SUCCEEDED(itemsHr) && items) {
            ITEMID_CHILD* pidl = nullptr;
            ULONG fetched = 0;
            const HRESULT nextHr = items->Next(1, &pidl, &fetched);
            wprintf(L"Next: 0x%08X fetched=%lu pidl=%p\n", nextHr, fetched, pidl);
            if (nextHr == S_OK && pidl) {
                const HRESULT posHr = folderView->GetItemPosition(pidl, &point);
                wprintf(L"GetItemPosition: 0x%08X -> (%ld, %ld)\n", posHr, point.x, point.y);
                positioned = SUCCEEDED(posHr);
                CoTaskMemFree(pidl);
            }
            items->Release();
        }
    }

    if (!positioned) {
        wprintf(L"could not locate the first item\n");
        return 1;
    }
    ClientToScreen(viewWindow, &point);
    wprintf(L"first item at screen (%ld, %ld)\n", point.x, point.y);
    wprintf(L"menu before: %ls\n", MenuVisible() ? L"visible" : L"none");

    SetForegroundWindow(frame);
    Sleep(400);
    SetCursorPos(point.x, point.y);
    Sleep(200);
    mouse_event(MOUSEEVENTF_RIGHTDOWN, 0, 0, 0, 0);
    Sleep(80);
    mouse_event(MOUSEEVENTF_RIGHTUP, 0, 0, 0, 0);
    Sleep(1500);

    wprintf(L"menu after: %ls\n", MenuVisible() ? L"VISIBLE" : L"NOT SHOWN");
    wprintf(L"explorer responding: %ls\n",
            IsHungAppWindow(frame) ? L"no (hung)" : L"yes");

    // Dismiss whatever appeared, without clicking anything.
    keybd_event(VK_ESCAPE, 0, 0, 0);
    keybd_event(VK_ESCAPE, 0, KEYEVENTF_KEYUP, 0);
    Sleep(300);
    wprintf(L"menu after Esc: %ls\n", MenuVisible() ? L"still visible" : L"closed");

    if (folderView) folderView->Release();
    view->Release();
    browser->Release();
    CoUninitialize();
    return 0;
}
