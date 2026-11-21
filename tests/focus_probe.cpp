// Reports what Explorer exposes about its search box and where the keyboard
// focus actually is, without needing to run inside explorer.exe.
//
// Usage:
//   focus_probe.exe [frame-hwnd]
//
// Prints:
//   - IShellBrowser::GetControlWindow() for every FCW id (documented and not)
//   - GetGUIThreadInfo(hwndFocus) of the window's thread, with the class chain
//
// This is how the mod decides whether Enter belongs to the search box or to
// the address bar: both are DirectUI, so their HWND is the only cheap clue.
#include <windows.h>
#include <exdisp.h>
#include <shlguid.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shobjidl.h>

#include <cstdio>
#include <string>

namespace {

std::wstring ClassOf(HWND hwnd) {
    wchar_t buffer[256]{};
    GetClassNameW(hwnd, buffer, ARRAYSIZE(buffer));
    return buffer;
}

std::wstring TextOf(HWND hwnd) {
    wchar_t buffer[512]{};
    GetWindowTextW(hwnd, buffer, ARRAYSIZE(buffer));
    return buffer;
}

HWND FirstCabinet() {
    struct Finder {
        static BOOL CALLBACK Proc(HWND hwnd, LPARAM param) {
            if (ClassOf(hwnd) == L"CabinetWClass") {
                *reinterpret_cast<HWND*>(param) = hwnd;
                return FALSE;
            }
            return TRUE;
        }
    };
    HWND found = nullptr;
    EnumWindows(Finder::Proc, reinterpret_cast<LPARAM>(&found));
    return found;
}

IShellBrowser* BrowserFor(HWND frame) {
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

        IServiceProvider* provider = nullptr;
        if (SUCCEEDED(dispatch->QueryInterface(IID_PPV_ARGS(&provider))) && provider) {
            IShellBrowser* candidate = nullptr;
            if (SUCCEEDED(provider->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(&candidate))) &&
                candidate) {
                HWND window = nullptr;
                if (SUCCEEDED(candidate->GetWindow(&window)) && window &&
                    (window == frame || IsChild(frame, window))) {
                    browser = candidate;
                } else {
                    candidate->Release();
                }
            }
            provider->Release();
        }
        dispatch->Release();
    }
    windows->Release();
    return browser;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    HWND frame = (argc > 1) ? reinterpret_cast<HWND>(wcstoull(argv[1], nullptr, 0)) : FirstCabinet();
    if (!frame) {
        std::printf("no CabinetWClass window\n");
        return 1;
    }
    std::wprintf(L"frame %p class=%ls title=\"%ls\"\n", static_cast<void*>(frame),
                 ClassOf(frame).c_str(), TextOf(frame).c_str());

    // --- control windows ---------------------------------------------------
    IShellBrowser* browser = BrowserFor(frame);
    if (!browser) {
        std::printf("IShellBrowser not found for this frame\n");
    } else {
        const struct {
            UINT id;
            const char* name;
        } ids[] = {
            {FCW_STATUS, "FCW_STATUS"},         {FCW_TOOLBAR, "FCW_TOOLBAR"},
            {FCW_TREE, "FCW_TREE"},             {FCW_INTERNETBAR, "FCW_INTERNETBAR"},
            {FCW_PROGRESS, "FCW_PROGRESS"},     {0x0B, "id 0x0B"},
            {0x0C, "id 0x0C"},                  {0x0D, "id 0x0D (search?)"},
            {0x0E, "id 0x0E"},                  {0x0F, "id 0x0F"},
        };
        for (const auto& entry : ids) {
            HWND control = nullptr;
            const HRESULT hr = browser->GetControlWindow(entry.id, &control);
            std::printf("  GetControlWindow(%-16s = 0x%02X) hr=0x%08lX hwnd=%p", entry.name, entry.id,
                        hr, static_cast<void*>(control));
            if (control) {
                std::wprintf(L" class=%ls text=\"%ls\" visible=%d", ClassOf(control).c_str(),
                             TextOf(control).c_str(), IsWindowVisible(control) ? 1 : 0);
            }
            std::printf("\n");
        }

        // --- search box service --------------------------------------------
        IServiceProvider* provider = nullptr;
        if (SUCCEEDED(browser->QueryInterface(IID_PPV_ARGS(&provider))) && provider) {
            ISearchBoxInfo* search = nullptr;
            const HRESULT hr = provider->QueryService(SID_SSearchBoxInfo, IID_PPV_ARGS(&search));
            std::printf("  QueryService(SID_SSearchBoxInfo) hr=0x%08lX ptr=%p\n", hr,
                        static_cast<void*>(search));
            if (SUCCEEDED(hr) && search) {
                LPWSTR text = nullptr;
                const HRESULT textHr = search->GetText(&text);
                std::printf("    GetText hr=0x%08lX", textHr);
                if (text) {
                    std::wprintf(L" text=\"%ls\"", text);
                    CoTaskMemFree(text);
                }
                std::printf("\n");
                search->Release();
            }
            provider->Release();
        }
        // --- location ------------------------------------------------------
        IWebBrowser2* web = nullptr;
        if (SUCCEEDED(browser->QueryInterface(IID_PPV_ARGS(&web))) && web) {
            BSTR url = nullptr;
            if (SUCCEEDED(web->get_LocationURL(&url)) && url) {
                std::wprintf(L"  LocationURL:  %ls\n", url);
                SysFreeString(url);
            }
            BSTR name = nullptr;
            if (SUCCEEDED(web->get_LocationName(&name)) && name) {
                std::wprintf(L"  LocationName: %ls\n", name);
                SysFreeString(name);
            }
            web->Release();
        }

        browser->Release();
    }
    // --- focus -------------------------------------------------------------
    const DWORD thread = GetWindowThreadProcessId(frame, nullptr);
    GUITHREADINFO info{};
    info.cbSize = sizeof(info);
    if (GetGUIThreadInfo(thread, &info)) {
        std::printf("focus=%p active=%p capture=%p\n", static_cast<void*>(info.hwndFocus),
                    static_cast<void*>(info.hwndActive), static_cast<void*>(info.hwndCapture));
        HWND current = info.hwndFocus;
        for (int i = 0; i < 10 && current; ++i) {
            std::wprintf(L"  [%d] %p %ls \"%ls\"\n", i, static_cast<void*>(current),
                         ClassOf(current).c_str(), TextOf(current).c_str());
            current = GetParent(current);
        }
    } else {
        std::printf("GetGUIThreadInfo failed: %lu\n", GetLastError());
    }

    CoUninitialize();
    return 0;
}
