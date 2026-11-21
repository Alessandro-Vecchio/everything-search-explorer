// Live check inside a real Explorer window: drives the *view* the same way a
// double click / Enter does (IFolderView2::InvokeVerbOnSelection) and reports
// whether the shell actually started the file.
//
// Usage:
//   view_probe.exe "Everything Search" "open"
#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <exdisp.h>
#include <shlwapi.h>

#include <cstdio>
#include <string>

int wmain(int argc, wchar_t** argv) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    const std::wstring needle = (argc > 1) ? argv[1] : L"Everything Search";

    std::string verb;
    if (argc > 2) {
        const int size = WideCharToMultiByte(CP_ACP, 0, argv[2], -1, nullptr, 0, nullptr, nullptr);
        if (size > 1) {
            verb.resize(static_cast<size_t>(size - 1));
            WideCharToMultiByte(CP_ACP, 0, argv[2], -1, verb.data(), size, nullptr, nullptr);
        }
    }
    const char* verbArg = verb.empty() ? nullptr : verb.c_str();

    IShellWindows* windows = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&windows)))) {
        wprintf(L"CLSID_ShellWindows failed\n");
        return 1;
    }

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
            }
            provider->Release();
        }
        dispatch->Release();
    }
    windows->Release();

    if (!browser) {
        wprintf(L"window '%ls' not found\n", needle.c_str());
        return 1;
    }

    IShellView* view = nullptr;
    if (FAILED(browser->QueryActiveShellView(&view)) || !view) {
        wprintf(L"QueryActiveShellView failed\n");
        browser->Release();
        return 1;
    }

    IFolderView2* folderView = nullptr;
    const HRESULT viewHr = view->QueryInterface(IID_PPV_ARGS(&folderView));
    wprintf(L"IFolderView2: 0x%08X\n", viewHr);

    int itemCount = 0;
    if (folderView) {
        folderView->ItemCount(SVGIO_ALLVIEW, &itemCount);
        wprintf(L"items in view: %d\n", itemCount);

        // Content view is a per-folder preference: read back what the shell kept.
        FOLDERVIEWMODE mode{};
        int iconSize = 0;
        if (SUCCEEDED(folderView->GetViewModeAndIconSize(&mode, &iconSize))) {
            wprintf(L"view mode: %d (8 = FVM_CONTENT), icon size: %d\n", static_cast<int>(mode),
                    iconSize);
        } else {
            wprintf(L"GetViewModeAndIconSize failed\n");
        }

        if (itemCount > 0) {
            const HRESULT selectHr = folderView->SelectItem(0, SVSI_SELECT | SVSI_POSITIONITEM);
            wprintf(L"SelectItem(0): 0x%08X\n", selectHr);

            const HRESULT invokeHr = folderView->InvokeVerbOnSelection(verbArg);
            wprintf(L"InvokeVerbOnSelection(\"%hs\"): 0x%08X\n",
                    verbArg ? verbArg : "<default>", invokeHr);
        }
        folderView->Release();
    }

    view->Release();
    browser->Release();

    Sleep(2000);
    CoUninitialize();
    return 0;
}
