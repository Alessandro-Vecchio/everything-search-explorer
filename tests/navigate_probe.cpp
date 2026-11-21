// Repeats what the mod does when it navigates an existing Explorer window to
// the virtual folder, but from our own process. Used to reproduce/compare the
// "default page + second window" behaviour without touching Windhawk.
//
// Usage:
//   navigate_probe.exe <window-substring> [--navigate2] [--new]
#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <exdisp.h>
#include <shlwapi.h>

#include <cstdio>
#include <string>

namespace {

constexpr GUID kFolderClsid = {0x7b39c2a2, 0x3e1f, 0x4e4c,
                               {0x9e, 0x58, 0x2d, 0x9a, 0x7b, 0x1c, 0x4f, 0x60}};
constexpr wchar_t kGuidText[] = L"{7B39C2A2-3E1F-4E4C-9E58-2D9A7B1C4F60}";

int WindowCount() {
    IShellWindows* windows = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&windows))))
        return -1;
    long count = 0;
    windows->get_Count(&count);
    windows->Release();
    return static_cast<int>(count);
}

void DumpWindows(const wchar_t* tag) {
    IShellWindows* windows = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&windows))))
        return;
    long count = 0;
    windows->get_Count(&count);
    wprintf(L"%ls: %ld window(s)\n", tag, count);
    for (long i = 0; i < count; ++i) {
        VARIANT index;
        VariantInit(&index);
        V_VT(&index) = VT_I4;
        V_I4(&index) = i;
        IDispatch* dispatch = nullptr;
        if (FAILED(windows->Item(index, &dispatch)) || !dispatch) continue;
        IWebBrowser2* web = nullptr;
        if (SUCCEEDED(dispatch->QueryInterface(IID_PPV_ARGS(&web))) && web) {
            BSTR location = nullptr;
            BSTR url = nullptr;
            web->get_LocationName(&location);
            web->get_LocationURL(&url);
            wprintf(L"    [%ld] location=%ls url=%ls\n", i, location ? location : L"",
                    url ? url : L"");
            SysFreeString(location);
            SysFreeString(url);
            web->Release();
        }
        dispatch->Release();
    }
    windows->Release();
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    const std::wstring needle = (argc > 1) ? argv[1] : L"Windows";
    bool useNavigate2 = false;
    bool newWindow = false;
    for (int i = 2; i < argc; ++i) {
        if (wcscmp(argv[i], L"--navigate2") == 0) useNavigate2 = true;
        if (wcscmp(argv[i], L"--new") == 0) newWindow = true;
    }

    DumpWindows(L"before");

    IShellWindows* windows = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&windows)))) {
        wprintf(L"CLSID_ShellWindows failed\n");
        return 1;
    }

    long count = 0;
    windows->get_Count(&count);

    IShellBrowser* browser = nullptr;
    IWebBrowser2* web = nullptr;
    HWND target = nullptr;

    for (long i = 0; i < count && !browser; ++i) {
        VARIANT index;
        VariantInit(&index);
        V_VT(&index) = VT_I4;
        V_I4(&index) = i;

        IDispatch* dispatch = nullptr;
        if (FAILED(windows->Item(index, &dispatch)) || !dispatch) continue;

        IWebBrowser2* candidateWeb = nullptr;
        if (SUCCEEDED(dispatch->QueryInterface(IID_PPV_ARGS(&candidateWeb))) && candidateWeb) {
            BSTR location = nullptr;
            candidateWeb->get_LocationName(&location);
            const bool match = location && wcsstr(location, needle.c_str()) != nullptr;
            SysFreeString(location);
            if (!match) {
                candidateWeb->Release();
                dispatch->Release();
                continue;
            }
        }

        IServiceProvider* provider = nullptr;
        if (SUCCEEDED(dispatch->QueryInterface(IID_PPV_ARGS(&provider))) && provider) {
            IShellBrowser* candidate = nullptr;
            if (SUCCEEDED(provider->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(&candidate))) &&
                candidate) {
                HWND window = nullptr;
                candidate->GetWindow(&window);
                if (window) {
                    browser = candidate;
                    web = candidateWeb;
                    target = window;
                } else {
                    candidate->Release();
                }
            }
            provider->Release();
        }
        dispatch->Release();
    }

    windows->Release();

    if (!browser) {
        wprintf(L"no matching window for '%ls'\n", needle.c_str());
        return 1;
    }
    wprintf(L"target hwnd=%p\n", target);

    PIDLIST_ABSOLUTE pidl = nullptr;
    HRESULT parseHr = SHParseDisplayName((std::wstring(L"shell:::") + kGuidText).c_str(), nullptr,
                                         &pidl, 0, nullptr);
    wprintf(L"SHParseDisplayName(shell:::GUID) = 0x%08X pidl=%p\n", parseHr, pidl);
    if (FAILED(parseHr) || !pidl) {
        IShellFolder* folder = nullptr;
        if (SUCCEEDED(CoCreateInstance(kFolderClsid, nullptr, CLSCTX_INPROC_SERVER,
                                       IID_PPV_ARGS(&folder))) &&
            folder) {
            IPersistFolder2* persist = nullptr;
            if (SUCCEEDED(folder->QueryInterface(IID_PPV_ARGS(&persist))) && persist) {
                persist->GetCurFolder(&pidl);
                persist->Release();
            }
            folder->Release();
            wprintf(L"fallback GetCurFolder pidl=%p\n", pidl);
        }
    }

    if (useNavigate2) {
        VARIANT url;
        VariantInit(&url);
        V_VT(&url) = VT_BSTR;
        V_BSTR(&url) = SysAllocString((std::wstring(L"shell:::") + kGuidText).c_str());
        VARIANT empty;
        VariantInit(&empty);
        const HRESULT hr = web->Navigate2(&url, &empty, &empty, &empty, &empty);
        wprintf(L"Navigate2 -> 0x%08X\n", hr);
        VariantClear(&url);
    } else if (pidl) {
        UINT flags = newWindow ? SBSP_ABSOLUTE | SBSP_NEWBROWSER : SBSP_ABSOLUTE | SBSP_SAMEBROWSER;
        const HRESULT hr = browser->BrowseObject(pidl, flags);
        wprintf(L"BrowseObject(flags=0x%X) -> 0x%08X\n", flags, hr);
    }

    if (pidl) CoTaskMemFree(pidl);
    if (web) web->Release();
    if (browser) browser->Release();

    Sleep(2500);
    DumpWindows(L"after");
    CoUninitialize();
    return 0;
}
