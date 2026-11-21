// Reads Explorer's native search box through ISearchBoxInfo (the documented
// SID_SSearchBoxInfo service) and reports what the active view is.
//
// Usage:
//   searchbox_probe.exe "C:\Users"
//
// The search box is DirectUI-based: there is no child edit control to subclass,
// so this service is the only supported way to read the typed query.
#include <windows.h>
#include <exdisp.h>
#include <shlguid.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shobjidl.h>

#include <cstdio>
#include <string>

namespace {

IShellBrowser* FindBrowser(const std::wstring& needle) {
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
            }
            provider->Release();
        }
        dispatch->Release();
    }
    windows->Release();
    return browser;
}

std::wstring DisplayName(IShellFolder* folder) {
    STRRET strret{};
    if (FAILED(folder->GetDisplayNameOf(nullptr, SHGDN_FORPARSING, &strret))) return {};
    PWSTR raw = nullptr;
    if (FAILED(StrRetToStrW(&strret, nullptr, &raw)) || !raw) return {};
    std::wstring name(raw);
    CoTaskMemFree(raw);
    return name;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const std::wstring needle = (argc > 1) ? argv[1] : L"C:\\";

    IShellBrowser* browser = FindBrowser(needle);
    if (!browser) {
        std::wprintf(L"window '%ls' not found\n", needle.c_str());
        return 1;
    }

    // --- search box ---------------------------------------------------------
    IServiceProvider* provider = nullptr;
    if (SUCCEEDED(browser->QueryInterface(IID_PPV_ARGS(&provider))) && provider) {
        ISearchBoxInfo* search = nullptr;
        const HRESULT hr =
            provider->QueryService(SID_SSearchBoxInfo, IID_PPV_ARGS(&search));
        std::printf("QueryService(SID_SSearchBoxInfo): 0x%08lX\n", hr);
        if (SUCCEEDED(hr) && search) {
            LPWSTR text = nullptr;
            const HRESULT textHr = search->GetText(&text);
            std::printf("  GetText: 0x%08lX", textHr);
            if (SUCCEEDED(textHr) && text) {
                std::wprintf(L" -> \"%ls\"", text);
                CoTaskMemFree(text);
            }
            std::printf("\n");
            search->Release();
        } else if (hr == E_NOINTERFACE) {
            std::printf("  (not available through the browser: try the view's provider)\n");
        }
        provider->Release();
    }

    // --- active view --------------------------------------------------------
    IShellView* view = nullptr;
    if (SUCCEEDED(browser->QueryActiveShellView(&view)) && view) {
        IPersistFolder2* folder = nullptr;
        if (SUCCEEDED(view->QueryInterface(IID_PPV_ARGS(&folder))) && folder) {
            PIDLIST_ABSOLUTE pidl = nullptr;
            if (SUCCEEDED(folder->GetCurFolder(&pidl)) && pidl) {
                PWSTR name = nullptr;
                if (SUCCEEDED(SHGetNameFromIDList(pidl, SIGDN_DESKTOPABSOLUTEPARSING, &name)) && name) {
                    std::wprintf(L"view folder: %ls\n", name);
                    CoTaskMemFree(name);
                }
                const ITEMIDLIST* child = ILFindLastID(pidl);
                std::printf("root clsid-ish bytes:");
                const auto* bytes = reinterpret_cast<const unsigned char*>(pidl);
                for (int i = 0; i < 20; ++i) std::printf(" %02X", bytes[i]);
                std::printf(" (cb=%u, child=%p)\n", pidl->mkid.cb, static_cast<const void*>(child));
                CoTaskMemFree(pidl);
            }
            folder->Release();
        }
        view->Release();
    }

    IWebBrowser2* web = nullptr;
    if (SUCCEEDED(browser->QueryInterface(IID_PPV_ARGS(&web))) && web) {
        BSTR location = nullptr;
        if (SUCCEEDED(web->get_LocationName(&location)) && location) {
            std::wprintf(L"LocationName: %ls\n", location);
            SysFreeString(location);
        }
        BSTR url = nullptr;
        if (SUCCEEDED(web->get_LocationURL(&url)) && url) {
            std::wprintf(L"LocationURL:  %ls\n", url);
            SysFreeString(url);
        }
        web->Release();
    }

    browser->Release();
    CoUninitialize();
    return 0;
}
