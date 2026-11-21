// ==WindhawkMod==
// @id              everything-explorer-search
// @name            Everything Search for Explorer
// @description     Overlay Explorer's search box with a custom field for Everything results.
// @version         0.6.2
// @author          Alessandro
// @include         explorer.exe
// @compilerOptions -lole32 -loleaut32 -lshlwapi -lshell32 -lgdi32 -luuid -ladvapi32 -lcomctl32 -ld2d1
// ==/WindhawkMod==

// ==WindhawkModReadme==
// # Everything Search for Explorer
//
// Cover Explorer's native search field with a custom Win32 text field. Click
// the field or press Ctrl+F / Ctrl+E, type a query, and press Enter. The mod
// writes a request for the companion namespace extension and navigates the
// current Explorer window to that virtual folder. There is no separate search
// window. The native search control remains underneath the overlay; this mod
// redirects the usual Explorer search shortcuts to the custom field.
//
// The field uses the standard Windows EDIT control, so normal text editing,
// selection, clipboard paste, Unicode input, and IME handling are provided by
// Windows rather than reimplemented in a custom-painted textbox.
//
// The companion namespace extension is required to display the results as a
// real Explorer folder. Install Everything 1.4.1 or newer, leave it running
// normally (not elevated), then press Ctrl+Shift+F to open the setup panel.
// Choose the matching EverythingSearchNse.dll and install it. The installer
// writes only per-user HKCU registration and copies the DLL under LocalAppData.
//
// If a folder path is available, searches are scoped to that folder by default.
// Set "Scope to current folder" to false for global searches.
// ==/WindhawkModReadme==

// ==WindhawkModSettings==
// - maxResults: 0
//   $name: Maximum results
//   $description: 0 returns every match (up to the component safety ceiling); a positive value sets an explicit limit.
// - scopeToFolder: true
//   $name: Scope to current folder
//   $description: Restrict the search to the folder shown in the current Explorer window.
// ==/WindhawkModSettings==

#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shlguid.h>
#include <shlwapi.h>
#include <shellapi.h>
#include <commctrl.h>
#include <d2d1.h>
#include <d2d1helper.h>

#include <cstring>
#include <new>
#include <string>

// Development tracing (mod\build-mod.ps1 -Trace): append a line to
// %TEMP%\esx-mod.log. Release builds compile every call out.
#ifdef ESX_MOD_TRACE
#include <roapi.h>
#include <winstring.h>
#include <cstdarg>
#include <cstdio>

inline void ModTrace(const char* format, ...) {
    char buffer[900]{};
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer) - 32, format, args);
    va_end(args);

    wchar_t directory[MAX_PATH]{};
    if (!::GetTempPathW(MAX_PATH, directory)) return;
    const std::wstring path = std::wstring(directory) + L"esx-mod.log";

    HANDLE file = ::CreateFileW(path.c_str(), FILE_APPEND_DATA,
                                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;

    char line[1000]{};
    const int length = snprintf(line, sizeof(line), "[%lu] %s\r\n",
                                ::GetCurrentThreadId(), buffer);
    if (length > 0) {
        DWORD written = 0;
        ::WriteFile(file, line, static_cast<DWORD>(length), &written, nullptr);
    }
    ::CloseHandle(file);
}

#define ESX_MOD_LOG(...) ModTrace(__VA_ARGS__)
#else
#define ESX_MOD_LOG(...) ((void)0)
#endif

#define ESX_GUID_STRING L"{7B39C2A2-3E1F-4E4C-9E58-2D9A7B1C4F60}"
#define ESX_PANEL_CLASS L"EverythingSearchPanelWnd"
#define ESX_OVERLAY_CLASS L"EverythingSearchBoxOverlay"
#define ESX_OVERLAY_PROP L"EverythingSearchBoxOverlay.Handle"

// CLSID implemented by the companion namespace extension.
static const GUID kFolderClsid = {
    0x7b39c2a2, 0x3e1f, 0x4e4c, {0x9e, 0x58, 0x2d, 0x9a, 0x7b, 0x1c, 0x4f, 0x60}};

#define ESX_COMPONENT_VERSION L"0.3.5"
#define ESX_DLL_NAME L"EverythingSearchNse.dll"
#define ESX_CLSID_KEY \
    L"Software\\Classes\\CLSID\\{7B39C2A2-3E1F-4E4C-9E58-2D9A7B1C4F60}"
#define ESX_STATE_KEY L"Software\\EverythingExplorerSearch"

// Set this to the project's release page before publishing the mod.
#define ESX_RELEASES_URL \
    L"https://github.com/CHANGE-ME/everything-explorer-search/releases/latest"

namespace {

#ifdef ESX_MOD_TRACE
void ProbeWinRtActivation();
#endif

template <class T>
class ComPtr {
public:
    ComPtr() = default;
    ~ComPtr() { Reset(); }
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;

    T* get() const { return p_; }
    T* operator->() const { return p_; }
    explicit operator bool() const { return p_ != nullptr; }

    T** put() {
        Reset();
        return &p_;
    }

    void Reset() {
        if (p_) {
            p_->Release();
            p_ = nullptr;
        }
    }

    T* Detach() {
        T* p = p_;
        p_ = nullptr;
        return p;
    }

    void Attach(T* p) {
        Reset();
        p_ = p;
    }

private:
    T* p_ = nullptr;
};

int g_maxResults = 0;  // 0 = every match
bool g_scopeToFolder = true;

// ---------------------------------------------------------------- helpers

std::string WideToUtf8(const std::wstring& s) {
    if (s.empty()) return {};
    const int len = static_cast<int>(s.size());
    const int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), len, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), len, out.data(), n, nullptr, nullptr);
    return out;
}

bool WriteRequest(const std::wstring& folder, const std::wstring& query) {
    PWSTR raw = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &raw)) || !raw) return false;

    std::wstring dir = raw;
    CoTaskMemFree(raw);
    dir += L"\\EverythingExplorerSearch";
    if (!CreateDirectoryW(dir.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
        return false;
    }

    const std::wstring path = dir + L"\\request.txt";
    const std::wstring tmp = path + L".tmp";

    FILETIME ft{};
    GetSystemTimeAsFileTime(&ft);
    const unsigned seq = static_cast<unsigned>(
        ((static_cast<unsigned long long>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime) & 0xFFFFFFFFull);

    std::string data;
    data += "seq=" + std::to_string(seq) + "\n";
    data += "max=" + std::to_string(g_maxResults) + "\n";
    // bit 0 = scope to folder, bit 1 = include folders (always on).
    data += "flags=" + std::to_string(((g_scopeToFolder && !folder.empty()) ? 1u : 0u) | 2u) + "\n";
    data += "folder=" + WideToUtf8(folder) + "\n";
    data += "query=" + WideToUtf8(query) + "\n";

    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;

    DWORD written = 0;
    const bool ok = WriteFile(h, data.data(), static_cast<DWORD>(data.size()), &written, nullptr) &&
                    written == data.size();
    CloseHandle(h);
    if (!ok) return false;

    return MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != FALSE;
}

int BrowserScore(HWND window, HWND target) {
    if (!window) return -1;
    const bool same = window == target;
    const bool child = IsChild(target, window);
    if (!same && !child) return -1;

    int score = 1;
    if (IsWindowVisible(window)) score += 2;

    HWND focus = GetFocus();
    if (focus && (focus == window || IsChild(window, focus))) score += 4;
    return score;
}

bool GetBrowserForWindow(HWND target, ComPtr<IShellBrowser>& browser, ComPtr<IWebBrowser2>& web) {
    ComPtr<IShellWindows> windows;
    if (FAILED(CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_ALL, IID_PPV_ARGS(windows.put()))))
        return false;

    long count = 0;
    if (FAILED(windows->get_Count(&count))) return false;

    int bestScore = -1;
    for (long i = 0; i < count; ++i) {
        VARIANT index;
        VariantInit(&index);
        V_VT(&index) = VT_I4;
        V_I4(&index) = i;

        ComPtr<IDispatch> dispatch;
        if (FAILED(windows->Item(index, dispatch.put())) || !dispatch) continue;

        ComPtr<IServiceProvider> provider;
        if (FAILED(dispatch->QueryInterface(IID_PPV_ARGS(provider.put())))) continue;

        ComPtr<IShellBrowser> candidate;
        if (FAILED(provider->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(candidate.put()))))
            continue;

        HWND window = nullptr;
        if (FAILED(candidate->GetWindow(&window)) || !window) continue;

        const int score = BrowserScore(window, target);
        if (score <= bestScore) continue;

        bestScore = score;
        browser.Attach(candidate.Detach());
        web.Reset();
        dispatch->QueryInterface(IID_PPV_ARGS(web.put()));
    }

    return bestScore > 0;
}

std::wstring GetFolderPath(IShellBrowser* browser, IWebBrowser2* web) {
    if (web) {
        BSTR url = nullptr;
        if (SUCCEEDED(web->get_LocationURL(&url)) && url) {
            wchar_t buffer[32768]{};
            DWORD length = ARRAYSIZE(buffer);
            if (SUCCEEDED(PathCreateFromUrlW(url, buffer, &length, 0))) {
                SysFreeString(url);
                return buffer;
            }
            SysFreeString(url);
        }
    }

    if (browser) {
        ComPtr<IShellView> view;
        if (SUCCEEDED(browser->QueryActiveShellView(view.put())) && view) {
            ComPtr<IFolderView> folderView;
            ComPtr<IShellFolder> folder;
            ComPtr<IPersistFolder2> persist;
            if (SUCCEEDED(view->QueryInterface(IID_PPV_ARGS(folderView.put()))) &&
                SUCCEEDED(folderView->GetFolder(IID_PPV_ARGS(folder.put()))) &&
                SUCCEEDED(folder->QueryInterface(IID_PPV_ARGS(persist.put())))) {
                PIDLIST_ABSOLUTE pidl = nullptr;
                if (SUCCEEDED(persist->GetCurFolder(&pidl)) && pidl) {
                    PWSTR path = nullptr;
                    const HRESULT nameHr = SHGetNameFromIDList(pidl, SIGDN_FILESYSPATH, &path);
                    CoTaskMemFree(pidl);
                    if (SUCCEEDED(nameHr) && path) {
                        std::wstring result(path);
                        CoTaskMemFree(path);
                        return result;
                    }
                }
            }
        }
    }
    return {};
}

// Resolving "shell:::{GUID}" through SHParseDisplayName intermittently fails
// with ERROR_PATH_NOT_FOUND right after Explorer restarts, while the CLSID is
// registered and CoCreateInstance on it succeeds. Falling back to the
// component itself gives the same root PIDL without going through the shell's
// namespace cache.
PIDLIST_ABSOLUTE ResolveRootPidl() {
    const std::wstring spec = std::wstring(L"shell:::") + ESX_GUID_STRING;

    PIDLIST_ABSOLUTE pidl = nullptr;
    if (SUCCEEDED(SHParseDisplayName(spec.c_str(), nullptr, &pidl, 0, nullptr)) && pidl) return pidl;

    ComPtr<IShellFolder> folder;
    if (SUCCEEDED(CoCreateInstance(kFolderClsid, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(folder.put()))) &&
        folder) {
        ComPtr<IPersistFolder2> persist;
        if (SUCCEEDED(folder->QueryInterface(IID_PPV_ARGS(persist.put()))) && persist) {
            persist->GetCurFolder(&pidl);
        }
    }

    Wh_Log(L"SHParseDisplayName(shell:::%ls) failed, fallback pidl=%p", ESX_GUID_STRING, pidl);
    return pidl;
}

void NavigateWindow(HWND frame) {
    ComPtr<IShellBrowser> browser;
    ComPtr<IWebBrowser2> web;
    if (!GetBrowserForWindow(frame, browser, web) || !browser) return;

    // LocationURL is empty for a namespace extension, so the reliable way to
    // know whether this window already shows our folder is to ask the active
    // view which CLSID it is displaying.
    bool alreadyThere = false;
    ComPtr<IShellView> view;
    if (SUCCEEDED(browser->QueryActiveShellView(view.put())) && view) {
        ComPtr<IFolderView> folderView;
        ComPtr<IShellFolder> folder;
        ComPtr<IPersist> persist;
        CLSID clsid{};
        if (SUCCEEDED(view->QueryInterface(IID_PPV_ARGS(folderView.put()))) &&
            SUCCEEDED(folderView->GetFolder(IID_PPV_ARGS(folder.put()))) &&
            SUCCEEDED(folder->QueryInterface(IID_PPV_ARGS(persist.put()))) &&
            SUCCEEDED(persist->GetClassID(&clsid)) && IsEqualCLSID(clsid, kFolderClsid)) {
            alreadyThere = true;
        }
    }

    if (!alreadyThere && web) {
        BSTR url = nullptr;
        if (SUCCEEDED(web->get_LocationURL(&url)) && url) {
            alreadyThere = wcsstr(url, ESX_GUID_STRING) != nullptr;
            SysFreeString(url);
        }
    }

    ESX_MOD_LOG("NavigateWindow alreadyThere=%d", alreadyThere ? 1 : 0);
    if (alreadyThere) {
        // Refresh re-enumerates through our EnumObjects, which picks up the new
        // request.txt because its sequence number changed.
        if (view) view->Refresh();
        return;
    }

    PIDLIST_ABSOLUTE pidl = ResolveRootPidl();
    if (!pidl) {
        Wh_Log(L"Could not resolve the virtual folder PIDL");
        return;
    }

    const HRESULT browseHr = browser->BrowseObject(pidl, SBSP_ABSOLUTE | SBSP_SAMEBROWSER);
    ESX_MOD_LOG("BrowseObject=0x%08lX view=%p", browseHr, static_cast<void*>(view.get()));
    if (FAILED(browseHr)) {
        // Some views (Home, a tab that cannot host a namespace folder) refuse
        // to navigate in place. Open the folder the way the shell's own
        // "open" verb does instead of silently doing nothing.
        SHELLEXECUTEINFOW info{};
        info.cbSize = sizeof(info);
        info.fMask = SEE_MASK_INVOKEIDLIST | SEE_MASK_IDLIST;
        info.lpVerb = L"open";
        info.lpIDList = pidl;
        info.nShow = SW_SHOWNORMAL;
        const bool launched = ShellExecuteExW(&info) != FALSE;
        if (!launched) {
            Wh_Log(L"BrowseObject=0x%08lX and ShellExecuteEx failed: %lu", browseHr, GetLastError());
        }
    }
    CoTaskMemFree(pidl);
}

// ------------------------------------------------------ shell component

struct ComponentState {
    std::wstring path;
    std::wstring version;
    bool registered = false;
    bool fileExists = false;

    bool Usable() const { return registered && fileExists; }
    bool Current() const { return Usable() && version == ESX_COMPONENT_VERSION; }
};

struct EverythingState {
    bool running = false;
    bool responsive = false;
    std::wstring version;
};

std::wstring RegGetString(HKEY root, const wchar_t* subkey, const wchar_t* name) {
    wchar_t buffer[1024]{};
    DWORD size = sizeof(buffer);
    if (RegGetValueW(root, subkey, name, RRF_RT_REG_SZ, nullptr, buffer, &size) != ERROR_SUCCESS) {
        return {};
    }
    return buffer;
}

bool RegSetString(HKEY root, const wchar_t* subkey, const wchar_t* name,
                  const std::wstring& value) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(root, subkey, 0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr) !=
        ERROR_SUCCESS) {
        return false;
    }
    const DWORD bytes = static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t));
    const bool ok =
        RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                       bytes) == ERROR_SUCCESS;
    RegCloseKey(key);
    return ok;
}

bool RegSetDword(HKEY root, const wchar_t* subkey, const wchar_t* name, DWORD value) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(root, subkey, 0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr) !=
        ERROR_SUCCESS) {
        return false;
    }
    const bool ok = RegSetValueExW(key, name, 0, REG_DWORD,
                                   reinterpret_cast<const BYTE*>(&value), sizeof(value)) ==
                    ERROR_SUCCESS;
    RegCloseKey(key);
    return ok;
}

std::wstring KnownFolder(REFKNOWNFOLDERID id) {
    PWSTR raw = nullptr;
    if (FAILED(SHGetKnownFolderPath(id, 0, nullptr, &raw)) || !raw) return {};
    std::wstring path = raw;
    CoTaskMemFree(raw);
    return path;
}

std::wstring ComponentDir() {
    const std::wstring local = KnownFolder(FOLDERID_LocalAppData);
    if (local.empty()) return {};
    return local + L"\\EverythingExplorerSearch\\nse\\" + ESX_COMPONENT_VERSION;
}

std::wstring ComponentTarget() {
    const std::wstring dir = ComponentDir();
    if (dir.empty()) return {};
    return dir + L"\\" + ESX_DLL_NAME;
}

ComponentState QueryComponent() {
    ComponentState state;
    const std::wstring inproc = std::wstring(ESX_CLSID_KEY) + L"\\InprocServer32";
    state.path = RegGetString(HKEY_CURRENT_USER, inproc.c_str(), nullptr);
    state.version = RegGetString(HKEY_CURRENT_USER, ESX_STATE_KEY, L"Version");
    state.registered = !state.path.empty();
    state.fileExists =
        state.registered && GetFileAttributesW(state.path.c_str()) != INVALID_FILE_ATTRIBUTES;
    return state;
}

bool CopyComponent(const std::wstring& source) {
    const std::wstring target = ComponentTarget();
    if (target.empty()) return false;

    // Only something that loads as a PE image may be registered as a shell
    // extension: a wrong pick must not end up in the CLSID key.
    HMODULE probe = LoadLibraryExW(source.c_str(), nullptr, LOAD_LIBRARY_AS_DATAFILE);
    if (!probe) return false;
    FreeLibrary(probe);

    const int directoryResult = SHCreateDirectoryExW(nullptr, ComponentDir().c_str(), nullptr);
    if (directoryResult != ERROR_SUCCESS && directoryResult != ERROR_ALREADY_EXISTS &&
        directoryResult != ERROR_FILE_EXISTS) {
        return false;
    }
    return CopyFileW(source.c_str(), target.c_str(), FALSE) != FALSE;
}

bool InstallComponent(const std::wstring& source, std::wstring& message) {
    if (source.empty()) {
        message = L"No file selected.";
        return false;
    }
    if (!CopyComponent(source)) {
        message = L"Could not copy the component. If Explorer is still using it, restart Explorer.";
        return false;
    }

    const std::wstring clsid(ESX_CLSID_KEY);
    const std::wstring inproc = clsid + L"\\InprocServer32";
    const std::wstring shellFolder = clsid + L"\\ShellFolder";

    const bool ok =
        RegSetString(HKEY_CURRENT_USER, clsid.c_str(), nullptr, L"Everything Search") &&
        RegSetString(HKEY_CURRENT_USER, inproc.c_str(), nullptr, ComponentTarget()) &&
        RegSetString(HKEY_CURRENT_USER, inproc.c_str(), L"ThreadingModel", L"Apartment") &&
        RegSetDword(HKEY_CURRENT_USER, shellFolder.c_str(), L"Attributes", 0x20000000) &&
        RegSetString(HKEY_CURRENT_USER, ESX_STATE_KEY, L"Version", ESX_COMPONENT_VERSION) &&
        RegSetString(HKEY_CURRENT_USER, ESX_STATE_KEY, L"Path", ComponentTarget());

    if (!ok) {
        // Do not delete the CLSID tree here: it may have contained a working
        // registration from an earlier install. Report the failure explicitly.
        message = L"Registration failed. The existing registration was not deleted; verify the HKCU values.";
        return false;
    }

    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    message = L"Component installed. Press Ctrl+F in an Explorer window.";
    return true;
}

void UninstallComponent() {
    const std::wstring path = RegGetString(HKEY_CURRENT_USER, ESX_STATE_KEY, L"Path");
    RegDeleteTreeW(HKEY_CURRENT_USER, ESX_CLSID_KEY);
    RegDeleteTreeW(HKEY_CURRENT_USER, ESX_STATE_KEY);

    if (!path.empty() && DeleteFileW(path.c_str()) == FALSE) {
        // Explorer may have it mapped: remove it at the next reboot.
        MoveFileExW(path.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
    }

    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
}

EverythingState QueryEverything() {
    EverythingState state;
    HWND window = FindWindowW(L"EVERYTHING_TASKBAR_NOTIFICATION", nullptr);
    if (!window) return state;
    state.running = true;

    auto ask = [window](WPARAM part) -> DWORD {
        DWORD_PTR result = 0;
        if (!SendMessageTimeoutW(window, WM_USER, part, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 500,
                                 &result)) {
            return 0;
        }
        return static_cast<DWORD>(result);
    };

    const DWORD major = ask(0);
    if (major == 0) {
        // Window exists but does not answer: almost always UIPI, because
        // Everything runs elevated and we do not.
        return state;
    }

    state.responsive = true;
    state.version = std::to_wstring(major) + L"." + std::to_wstring(ask(1)) + L"." +
                    std::to_wstring(ask(2)) + L"." + std::to_wstring(ask(3));
    return state;
}

std::wstring NewestDownloadedComponent() {
    const std::wstring dir = KnownFolder(FOLDERID_Downloads);
    if (dir.empty()) return {};

    const std::wstring pattern = dir + L"\\EverythingSearchNse*.dll";
    WIN32_FIND_DATAW data{};
    HANDLE find = FindFirstFileW(pattern.c_str(), &data);
    if (find == INVALID_HANDLE_VALUE) return {};

    std::wstring best;
    FILETIME bestTime{};
    do {
        if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (best.empty() || CompareFileTime(&data.ftLastWriteTime, &bestTime) > 0) {
            bestTime = data.ftLastWriteTime;
            best = dir + L"\\" + data.cFileName;
        }
    } while (FindNextFileW(find, &data));
    FindClose(find);
    return best;
}

std::wstring PickComponentFile(HWND owner) {
    ComPtr<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(dialog.put())))) {
        return {};
    }

    COMDLG_FILTERSPEC filters[] = {{L"Shell extension", L"*.dll"}, {L"All files", L"*.*"}};
    dialog->SetFileTypes(ARRAYSIZE(filters), filters);
    dialog->SetTitle(L"Select EverythingSearchNse.dll");

    const std::wstring downloads = KnownFolder(FOLDERID_Downloads);
    ComPtr<IShellItem> folder;
    if (!downloads.empty() &&
        SUCCEEDED(SHCreateItemFromParsingName(downloads.c_str(), nullptr,
                                              IID_PPV_ARGS(folder.put())))) {
        dialog->SetDefaultFolder(folder.get());
    }

    if (FAILED(dialog->Show(owner))) return {};

    ComPtr<IShellItem> result;
    if (FAILED(dialog->GetResult(result.put()))) return {};

    PWSTR raw = nullptr;
    if (FAILED(result->GetDisplayName(SIGDN_FILESYSPATH, &raw)) || !raw) return {};

    std::wstring path = raw;
    CoTaskMemFree(raw);
    return path;
}

// ---------------------------------------------------------------- panel

enum : int {
    kIdStatusEverything = 100,
    kIdStatusComponent,
    kIdStatusDetail,
    kIdStatusMessage,
    kIdInstall = 200,
    kIdChoose,
    kIdDownload,
    kIdUninstall,
    kIdClose,
};

struct PanelState {
    HWND hwnd = nullptr;
    HWND statusEverything = nullptr;
    HWND statusComponent = nullptr;
    HWND statusDetail = nullptr;
    HWND statusMessage = nullptr;
    HWND installButton = nullptr;
    HWND uninstallButton = nullptr;
};

// Alive while the setup window exists; PanelProc clears it on WM_NCDESTROY.
PanelState* g_panel = nullptr;

std::wstring DescribeEverything(const EverythingState& state) {
    if (!state.running) return L"Everything: not running - start it first";
    if (!state.responsive) {
        return L"Everything: running but unreachable - disable \"Run as administrator\"";
    }
    return L"Everything: running, version " + state.version;
}

std::wstring DescribeComponent(const ComponentState& state) {
    if (!state.registered) return L"Shell component: not installed";
    if (!state.fileExists) return L"Shell component: registered, but the DLL is missing";
    if (!state.Current()) {
        return L"Shell component: version " + state.version + L", expected " +
               ESX_COMPONENT_VERSION;
    }
    return L"Shell component: installed, version " + state.version;
}

void RefreshPanel(PanelState* state) {
    const EverythingState everything = QueryEverything();
    const ComponentState component = QueryComponent();

    SetWindowTextW(state->statusEverything, DescribeEverything(everything).c_str());
    SetWindowTextW(state->statusComponent, DescribeComponent(component).c_str());
    SetWindowTextW(state->statusDetail,
                   component.registered ? component.path.c_str() : L"");

    // Install is pointless once the expected version is in place; Uninstall is
    // pointless before anything is registered.
    if (state->installButton) EnableWindow(state->installButton, !component.Current());
    if (state->uninstallButton) EnableWindow(state->uninstallButton, component.registered);
}

void SetPanelMessage(PanelState* state, const std::wstring& text) {
    SetWindowTextW(state->statusMessage, text.c_str());
}

HWND MakeChild(HWND parent, const wchar_t* cls, const wchar_t* text, int x, int y, int cx, int cy,
               int id, DWORD style) {
    HWND child = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, x, y, cx, cy, parent,
                                 reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                 GetModuleHandleW(nullptr), nullptr);
    if (child) SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
    return child;
}

LRESULT CALLBACK PanelProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* state = reinterpret_cast<PanelState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (msg) {
        case WM_CREATE: {
            auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            state = reinterpret_cast<PanelState*>(create->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
            state->hwnd = hwnd;

            MakeChild(hwnd, L"STATIC", L"Everything Search for Explorer", 16, 12, 400, 22, 0,
                      SS_LEFT);
            state->statusEverything = MakeChild(hwnd, L"STATIC", L"", 16, 44, 600, 20, kIdStatusEverything, SS_LEFT);
            state->statusComponent = MakeChild(hwnd, L"STATIC", L"", 16, 68, 600, 20, kIdStatusComponent, SS_LEFT);
            state->statusDetail = MakeChild(hwnd, L"STATIC", L"", 16, 90, 600, 32, kIdStatusDetail, SS_LEFT | SS_ENDELLIPSIS);
            state->statusMessage = MakeChild(hwnd, L"STATIC", L"", 16, 128, 600, 36, kIdStatusMessage, SS_LEFT);

            MakeChild(hwnd, L"BUTTON", L"Install...", 16, 210, 140, 32, kIdInstall, BS_PUSHBUTTON);
            MakeChild(hwnd, L"BUTTON", L"Choose file...", 164, 210, 130, 32, kIdChoose, BS_PUSHBUTTON);
            MakeChild(hwnd, L"BUTTON", L"Releases...", 302, 210, 110, 32, kIdDownload, BS_PUSHBUTTON);
            state->uninstallButton =
                MakeChild(hwnd, L"BUTTON", L"Uninstall", 420, 210, 100, 32, kIdUninstall, BS_PUSHBUTTON);
            MakeChild(hwnd, L"BUTTON", L"Close", 528, 210, 80, 32, kIdClose, BS_PUSHBUTTON);
            state->installButton = GetDlgItem(hwnd, kIdInstall);

            RefreshPanel(state);
            return 0;
        }
        case WM_COMMAND: {
            if (!state) break;
            const int id = LOWORD(wParam);

            if (id == kIdInstall || id == kIdChoose) {
                std::wstring source =
                    (id == kIdInstall) ? NewestDownloadedComponent() : std::wstring();
                if (source.empty()) source = PickComponentFile(hwnd);
                if (source.empty()) break;

                std::wstring message;
                InstallComponent(source, message);
                SetPanelMessage(state, message);
                RefreshPanel(state);
                return 0;
            }
            if (id == kIdDownload) {
                if (std::wstring(ESX_RELEASES_URL).find(L"CHANGE-ME") != std::wstring::npos) {
                    SetPanelMessage(state,
                                    L"No release URL is configured. Use Choose file... to select the DLL.");
                    return 0;
                }
                ShellExecuteW(nullptr, L"open", ESX_RELEASES_URL, nullptr, nullptr, SW_SHOWNORMAL);
                SetPanelMessage(state, L"Download the DLL, then click Install.");
                return 0;
            }
            if (id == kIdUninstall) {
                UninstallComponent();
                SetPanelMessage(state, L"Component removed.");
                RefreshPanel(state);
                return 0;
            }
            if (id == kIdClose) {
                DestroyWindow(hwnd);
                return 0;
            }
            break;
        }
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_NCDESTROY:
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            if (g_panel == state) g_panel = nullptr;
            delete state;
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void ShowPanel(HWND frame) {
    if (g_panel) {
        SetForegroundWindow(g_panel->hwnd);
        RefreshPanel(g_panel);
        return;
    }

    RECT frameRect{};
    GetWindowRect(frame ? frame : GetDesktopWindow(), &frameRect);

    constexpr int width = 632;
    constexpr int height = 290;
    int x = frameRect.left + (frameRect.right - frameRect.left - width) / 2;
    int y = frameRect.top + (frameRect.bottom - frameRect.top - height) / 2;

    MONITORINFO monitor{};
    monitor.cbSize = sizeof(monitor);
    if (GetMonitorInfoW(MonitorFromWindow(frame, MONITOR_DEFAULTTONEAREST), &monitor)) {
        if (x < monitor.rcWork.left) x = monitor.rcWork.left + 8;
        if (y < monitor.rcWork.top) y = monitor.rcWork.top + 8;
        if (x + width > monitor.rcWork.right) x = monitor.rcWork.right - width - 8;
        if (y + height > monitor.rcWork.bottom) y = monitor.rcWork.bottom - height - 8;
    }

    auto* state = new PanelState();
    HWND hwnd = CreateWindowExW(
        WS_EX_TOOLWINDOW, ESX_PANEL_CLASS, L"Everything Search - setup",
        WS_POPUP | WS_CAPTION | WS_SYSMENU, x, y, width, height, frame, nullptr,
        GetModuleHandleW(nullptr), state);

    if (!hwnd) {
        delete state;
        return;
    }

    g_panel = state;
    ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd);
}


// ---------------------------------------------- custom search field

bool IsExplorerFrame(HWND hwnd) {
    wchar_t cls[64]{};
    if (!hwnd || !GetClassNameW(hwnd, cls, ARRAYSIZE(cls))) return false;
    return _wcsicmp(cls, L"CabinetWClass") == 0;
}

struct OverlayState {
    HWND frame = nullptr;
    HWND edit = nullptr;
    HFONT font = nullptr;
    HBRUSH editBrush = nullptr;
    std::wstring origin;
    // Last applied client size, used to avoid rebuilding the window region on
    // every layout notification.
    int lastWidth = -1;
    int lastHeight = -1;
    int lastX = 0;
    int lastY = 0;
    // Direct2D chrome. The text itself stays in the native EDIT control so IME,
    // clipboard, selection and accessibility keep working.
    ID2D1HwndRenderTarget* renderTarget = nullptr;
    int renderDpi = 0;
    float focusProgress = 0.0f;
    float focusTarget = 0.0f;
    // Color of the command bar just outside the field, sampled from the screen
    // so the antialiased rounded corners blend with whatever is behind them.
    D2D1_COLOR_F backdrop = D2D1::ColorF(0.95f, 0.95f, 0.95f);
    bool backdropValid = false;
    bool hovered = false;
    float hoverProgress = 0.0f;
    float hoverTarget = 0.0f;
};

constexpr UINT_PTR kFocusTimerId = 1;
constexpr UINT_PTR kHoverTimerId = 2;

// Posted instead of executed inside the PeekMessage/GetMessage hooks. Creating
// windows, moving them or taking focus while user32 is retrieving messages
// re-enters the XAML island of the command bar (Windows.UI.Xaml.dll), which is
// what crashed Explorer. Posted messages run from the frame's own message loop,
// at a point where window operations are safe.
constexpr UINT ESX_MSG_ENSURE_OVERLAY = WM_APP + 0x51;
constexpr UINT ESX_MSG_FOCUS_OVERLAY = WM_APP + 0x52;
constexpr UINT ESX_MSG_SHOW_PANEL = WM_APP + 0x53;
constexpr UINT ESX_MSG_SUBMIT_XAML_SEARCH = WM_APP + 0x54;

// Coalesces layout requests: the frame can emit dozens of WM_WINDOWPOSCHANGED
// while Explorer lays out its XAML island, and one posted layout is enough.
constexpr wchar_t ESX_PENDING_PROP[] = L"EverythingSearchLayoutPending";

// Per-monitor DPI of a window. Resolved dynamically so the mod keeps working
// even if user32 does not export GetDpiForWindow (pre-1607 builds).
int WindowDpi(HWND hwnd) {
    using GetDpiForWindowFn = UINT(WINAPI*)(HWND);
    static GetDpiForWindowFn getDpiForWindow = []() {
        return reinterpret_cast<GetDpiForWindowFn>(
            GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"));
    }();
    if (getDpiForWindow) {
        const UINT dpi = getDpiForWindow(hwnd);
        if (dpi >= 48 && dpi <= 480) return static_cast<int>(dpi);
    }
    return 96;
}

int ScaleDpi(HWND hwnd, int value) {
    return MulDiv(value, WindowDpi(hwnd), 96);
}

// Screen rectangle of the native search field, derived from the frame.
//
// Measured through UI Automation (AutomationId "FileExplorerSearchBox") at
// 100% DPI on window widths from 520 to 1940 px:
//
//   frameW  field width   right margin   top offset   height
//     520        74            19            48        32
//     640       110            19            48        32
//     900       188            19            48        32
//    1075       240            19            48        32
//    1400       338            19            48        32
//    1800       458            19            48        32
//    1940       500            19            48        32
//
// The field is therefore anchored to the frame (not to the XAML island): the
// fixed parts are constant in device-independent pixels and the width is 30%
// of the frame width minus a constant. Anchoring to the island was the bug in
// 0.5.2: at 1075 px it drew a 380 px field over a 240 px one, 9 px off to the
// left and 2 px too low.
RECT SearchBoxRect(HWND frame) {
    RECT frameRect{};
    GetWindowRect(frame, &frameRect);

    const int frameWidth = frameRect.right - frameRect.left;
    const int rightMargin = ScaleDpi(frame, 19);
    const int minimum = ScaleDpi(frame, 74);
    const int height = ScaleDpi(frame, 32);

    int width = MulDiv(frameWidth, 3, 10) - ScaleDpi(frame, 82);
    if (width < minimum) width = minimum;

    RECT rect{};
    rect.right = frameRect.right - rightMargin;
    rect.left = rect.right - width;
    rect.top = frameRect.top + ScaleDpi(frame, 48);
    rect.bottom = rect.top + height;
    return rect;
}

bool IsDarkTheme() {
    DWORD value = 1;
    DWORD size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER,
                     L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &size) ==
        ERROR_SUCCESS) {
        return value == 0;
    }
    return false;
}

void RememberSearchOrigin(HWND frame);
void SubmitOverlaySearch(HWND overlay, bool reactivate = true);
void ActivateOverlay(HWND overlay);

// Gives keyboard focus to the field. A popup that is not the active window
// cannot take focus, and SetForegroundWindow is unreliable when the foreground
// lock is held by another process, so activate the popup on its own thread
// first: it is owned by the frame and stays above it either way.
void ActivateOverlay(HWND overlay) {
    if (!IsWindow(overlay)) return;
    auto* state = reinterpret_cast<OverlayState*>(GetWindowLongPtrW(overlay, GWLP_USERDATA));
    if (!state) return;
    if (GetForegroundWindow() != overlay) SetForegroundWindow(overlay);
    if (GetActiveWindow() != overlay) SetActiveWindow(overlay);
    if (state->edit) SetFocus(state->edit);
}

LRESULT CALLBACK EditSubclassProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam,
                                  UINT_PTR subclassId, DWORD_PTR referenceData) {
    if (message == WM_KEYDOWN) {
        if (wParam == VK_RETURN) {
            if ((lParam & (1LL << 30)) == 0) {
                SubmitOverlaySearch(reinterpret_cast<HWND>(referenceData));
            }
            return 0;
        }
        if (wParam == VK_ESCAPE) {
            SetWindowTextW(hwnd, L"");
            return 0;
        }
    }
    if (message == WM_CHAR && wParam == L'\r') return 0;
    if (message == WM_NCDESTROY) {
        RemoveWindowSubclass(hwnd, EditSubclassProc, subclassId);
    }
    return DefSubclassProc(hwnd, message, wParam, lParam);
}

// ---------------------------------------------- Direct2D field chrome

ID2D1Factory* D2DFactory() {
    static ID2D1Factory* factory = []() -> ID2D1Factory* {
        ID2D1Factory* created = nullptr;
        if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &created))) {
            return nullptr;
        }
        return created;
    }();
    return factory;
}

// System accent color (DWM stores it as 0xAABBGGRR).
D2D1_COLOR_F AccentColor() {
    DWORD value = 0;
    DWORD size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\DWM", L"AccentColor",
                     RRF_RT_REG_DWORD, nullptr, &value, &size) == ERROR_SUCCESS) {
        const float r = static_cast<float>(value & 0xFF) / 255.0f;
        const float g = static_cast<float>((value >> 8) & 0xFF) / 255.0f;
        const float b = static_cast<float>((value >> 16) & 0xFF) / 255.0f;
        return D2D1::ColorF(r, g, b, 1.0f);
    }
    return D2D1::ColorF(0.0f, 0.47f, 0.84f, 1.0f);
}

struct FieldPalette {
    D2D1_COLOR_F fill;
    D2D1_COLOR_F border;
    D2D1_COLOR_F glyph;
    D2D1_COLOR_F accent;
};

FieldPalette CurrentPalette(float hover) {
    const bool dark = IsDarkTheme();
    FieldPalette palette{};
    // WinUI 3 ControlFillColorDefault, composited over the command bar. The
    // fill stays constant whatever the state: the EDIT child paints itself with
    // the same color, so animating it would show a seam.
    palette.fill = dark ? D2D1::ColorF(43.0f / 255.0f, 43.0f / 255.0f, 43.0f / 255.0f)
                        : D2D1::ColorF(250.0f / 255.0f, 250.0f / 255.0f, 250.0f / 255.0f);
    const float t = hover < 0.0f ? 0.0f : (hover > 1.0f ? 1.0f : hover);
    // ControlStrokeColorDefault -> ControlStrokeColorSecondary on pointer over.
    const float borderAlpha = dark ? 0.10f + 0.06f * t : 0.08f + 0.08f * t;
    palette.border = D2D1::ColorF(dark ? 1.0f : 0.0f, dark ? 1.0f : 0.0f, dark ? 1.0f : 0.0f,
                                  borderAlpha);
    const float glyphAlpha = dark ? 0.55f + 0.15f * t : 0.45f + 0.15f * t;
    palette.glyph = D2D1::ColorF(dark ? 1.0f : 0.0f, dark ? 1.0f : 0.0f, dark ? 1.0f : 0.0f,
                                 glyphAlpha);
    palette.accent = AccentColor();
    return palette;
}

bool EnsureRenderTarget(HWND hwnd, OverlayState& state, int width, int height) {
    ID2D1Factory* factory = D2DFactory();
    if (!factory || width <= 0 || height <= 0) return false;

    const int dpi = WindowDpi(hwnd);
    if (state.renderTarget && state.renderDpi != dpi) {
        state.renderTarget->Release();
        state.renderTarget = nullptr;
    }

    if (state.renderTarget) {
        const D2D1_SIZE_U size = state.renderTarget->GetPixelSize();
        if (static_cast<UINT>(width) != size.width || static_cast<UINT>(height) != size.height) {
            if (FAILED(state.renderTarget->Resize(
                    D2D1::SizeU(static_cast<UINT>(width), static_cast<UINT>(height))))) {
                state.renderTarget->Release();
                state.renderTarget = nullptr;
            }
        }
        return state.renderTarget != nullptr;
    }

    const D2D1_RENDER_TARGET_PROPERTIES properties = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE),
        static_cast<float>(dpi), static_cast<float>(dpi));
    const D2D1_HWND_RENDER_TARGET_PROPERTIES hwndProperties = D2D1::HwndRenderTargetProperties(
        hwnd, D2D1::SizeU(static_cast<UINT>(width), static_cast<UINT>(height)),
        D2D1_PRESENT_OPTIONS_NONE);
    if (FAILED(factory->CreateHwndRenderTarget(properties, hwndProperties, &state.renderTarget))) {
        state.renderTarget = nullptr;
        return false;
    }
    state.renderDpi = dpi;
    return true;
}

// Samples the command bar color just outside the field. The command bar behind
// the search box is a flat fill, so a couple of neighbouring pixels are enough
// to make the rounded corners blend exactly, in any theme or accent color.
void SampleBackdrop(HWND hwnd, OverlayState& state) {
    state.backdropValid = false;
    if (!IsWindowVisible(hwnd)) return;

    RECT rect{};
    if (!GetWindowRect(hwnd, &rect)) return;

    HDC screen = GetDC(nullptr);
    if (!screen) return;

    const POINT points[2] = {
        {rect.left - ScaleDpi(hwnd, 3), (rect.top + rect.bottom) / 2},
        {(rect.left + rect.right) / 2, rect.top - ScaleDpi(hwnd, 3)},
    };

    unsigned red = 0;
    unsigned green = 0;
    unsigned blue = 0;
    unsigned count = 0;
    for (const POINT& point : points) {
        const COLORREF color = GetPixel(screen, point.x, point.y);
        if (color == CLR_INVALID) continue;
        red += GetRValue(color);
        green += GetGValue(color);
        blue += GetBValue(color);
        ++count;
    }
    ReleaseDC(nullptr, screen);
    if (!count) return;

    state.backdrop = D2D1::ColorF(static_cast<float>(red) / (255.0f * count),
                                  static_cast<float>(green) / (255.0f * count),
                                  static_cast<float>(blue) / (255.0f * count), 1.0f);
    state.backdropValid = true;
}

bool PaintOverlayD2D(HWND hwnd, OverlayState& state) {
    RECT client{};
    if (!GetClientRect(hwnd, &client)) return false;
    const int width = client.right - client.left;
    const int height = client.bottom - client.top;
    if (!EnsureRenderTarget(hwnd, state, width, height)) return false;

    ID2D1HwndRenderTarget* target = state.renderTarget;
    const bool focused = state.edit && GetFocus() == state.edit;
    const FieldPalette palette = CurrentPalette(focused ? 0.0f : state.hoverProgress);
    const float corner = 4.0f;

    target->BeginDraw();
    target->SetTransform(D2D1::Matrix3x2F::Identity());
    target->Clear(state.backdrop);

    const D2D1_SIZE_F size = target->GetSize();
    ID2D1SolidColorBrush* brush = nullptr;

    // The field itself is a rounded rect drawn with antialiasing; the corners
    // keep the sampled backdrop color, so the window stays rectangular and
    // there is no aliased window region.
    if (SUCCEEDED(target->CreateSolidColorBrush(palette.fill, &brush))) {
        const D2D1_ROUNDED_RECT shape = D2D1::RoundedRect(
            D2D1::RectF(0.5f, 0.5f, size.width - 0.5f, size.height - 0.5f), corner, corner);
        target->FillRoundedRectangle(shape, brush);
        brush->Release();
    }

    if (SUCCEEDED(target->CreateSolidColorBrush(palette.border, &brush))) {
        const D2D1_ROUNDED_RECT shape = D2D1::RoundedRect(
            D2D1::RectF(0.5f, 0.5f, size.width - 0.5f, size.height - 0.5f), corner, corner);
        target->DrawRoundedRectangle(shape, brush, 1.0f);
        brush->Release();
    }

    // WinUI 3 text fields mark focus with a 2px accent underline, not with a
    // colored outline, so the border stays neutral and this animates in.
    if (state.focusProgress > 0.01f) {
        if (SUCCEEDED(target->CreateSolidColorBrush(palette.accent, &brush))) {
            brush->SetOpacity(state.focusProgress);
            const float y = size.height - 1.5f;
            target->DrawLine(D2D1::Point2F(corner + 1.0f, y),
                             D2D1::Point2F(size.width - corner - 1.0f, y), brush, 2.0f);
            brush->Release();
        }
    }

    // Magnifying glass as vectors, so it stays crisp at any DPI. Geometry
    // matches the Fluent "Search" glyph used by the native field: a 16x16 icon
    // starting at x = 12, with the text at x = 36.
    if (SUCCEEDED(target->CreateSolidColorBrush(palette.glyph, &brush))) {
        const float centerY = size.height / 2.0f - 0.5f;
        target->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(18.0f, centerY), 4.9f, 4.9f),
                            brush, 1.35f);
        target->DrawLine(D2D1::Point2F(21.6f, centerY + 3.6f),
                         D2D1::Point2F(24.6f, centerY + 6.6f), brush, 1.35f);
        brush->Release();
    }

    const HRESULT result = target->EndDraw();
    if (result == D2DERR_RECREATE_TARGET) {
        state.renderTarget->Release();
        state.renderTarget = nullptr;
        state.renderDpi = 0;
    }
    return SUCCEEDED(result);
}

// Minimal GDI fallback for the rare case where Direct2D is unavailable.
void PaintOverlayGdi(HDC dc, HWND hwnd, OverlayState& state) {
    RECT client{};
    GetClientRect(hwnd, &client);
    const bool dark = IsDarkTheme();
    const COLORREF fillColor = dark ? RGB(43, 43, 43) : RGB(250, 250, 250);
    HBRUSH fill = CreateSolidBrush(fillColor);
    FillRect(dc, &client, fill);
    DeleteObject(fill);

    const int corner = ScaleDpi(hwnd, 8);
    HPEN border = CreatePen(PS_SOLID, 1, dark ? RGB(64, 64, 64) : RGB(226, 226, 226));
    HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    HGDIOBJ oldPen = SelectObject(dc, border);
    RoundRect(dc, client.left, client.top, client.right - 1, client.bottom - 1, corner, corner);
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(border);

    const int iconX = ScaleDpi(hwnd, 12);
    const int centerY = (client.bottom - client.top) / 2 - 1;
    HPEN glyph = CreatePen(PS_SOLID, 1, dark ? RGB(175, 175, 175) : RGB(120, 120, 120));
    HGDIOBJ oldGlyph = SelectObject(dc, glyph);
    Ellipse(dc, iconX, centerY - 5, iconX + 8, centerY + 3);
    MoveToEx(dc, iconX + 7, centerY + 2, nullptr);
    LineTo(dc, iconX + 12, centerY + 7);
    SelectObject(dc, oldGlyph);
    DeleteObject(glyph);
}

// WinUI 3 uses Segoe UI Variable Text at 14px for the search field. GDI cannot
// synthesize the variable axis, but the named family is registered on Windows
// 11; fall back to Segoe UI on older builds or if the mapper refuses it.
HFONT CreateFieldFont(HWND hwnd, int dipSize) {
    const int height = -ScaleDpi(hwnd, dipSize);
    const wchar_t* faces[] = {L"Segoe UI Variable Text", L"Segoe UI"};
    for (const wchar_t* face : faces) {
        HFONT font = CreateFontW(height, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                 OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                 DEFAULT_PITCH | FF_DONTCARE, face);
        if (!font) continue;

        HDC dc = GetDC(hwnd);
        if (!dc) return font;
        HGDIOBJ old = SelectObject(dc, font);
        wchar_t actual[64]{};
        GetTextFaceW(dc, ARRAYSIZE(actual), actual);
        SelectObject(dc, old);
        ReleaseDC(hwnd, dc);
        if (_wcsicmp(actual, face) == 0) return font;
        DeleteObject(font);
    }
    return nullptr;
}

// Places the EDIT control inside the field: vertically centered, with the
// magnifier on the left and a strip at the bottom kept free so the Direct2D
// layer can draw the WinUI focus underline there.
void PlaceEdit(HWND hwnd, HWND edit, int width, int height) {
    if (!IsWindow(edit)) return;

    const int left = ScaleDpi(hwnd, 34);
    int right = width - ScaleDpi(hwnd, 8);
    if (right - left < ScaleDpi(hwnd, 16)) right = left + ScaleDpi(hwnd, 16);

    int textHeight = ScaleDpi(hwnd, 22);
    const int available = height - ScaleDpi(hwnd, 5);
    if (textHeight > available) textHeight = available;
    if (textHeight < 1) textHeight = 1;

    int top = (height - textHeight) / 2;
    if (top < 0) top = 0;

    MoveWindow(edit, left, top, right - left, textHeight, TRUE);
}

LRESULT CALLBACK OverlayProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* state = reinterpret_cast<OverlayState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (message) {
        case WM_NCCREATE: {
            auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            state = static_cast<OverlayState*>(create->lpCreateParams);
            state->frame = create->hwndParent;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
            return TRUE;
        }
        case WM_CREATE: {
            state->font = CreateFieldFont(hwnd, 14);
            state->editBrush = CreateSolidBrush(IsDarkTheme() ? RGB(43, 43, 43)
                                                               : RGB(250, 250, 250));
            RECT client{};
            GetClientRect(hwnd, &client);
            state->edit = CreateWindowExW(
                0, L"EDIT", L"",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | ES_NOHIDESEL,
                0, 0, 1, 1, hwnd, reinterpret_cast<HMENU>(1),
                GetModuleHandleW(nullptr), nullptr);
            if (!state->edit) return -1;
            PlaceEdit(hwnd, state->edit, client.right - client.left, client.bottom - client.top);
            SendMessageW(state->edit, EM_LIMITTEXT, 1024, 0);
            SendMessageW(state->edit, EM_SETCUEBANNER, TRUE,
                         reinterpret_cast<LPARAM>(L"Cerca con Everything"));
            SendMessageW(state->edit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
                         MAKELONG(2, 4));
            SendMessageW(state->edit, WM_SETFONT,
                         reinterpret_cast<WPARAM>(state->font), TRUE);
            if (!SetWindowSubclass(state->edit, EditSubclassProc, 1,
                                   reinterpret_cast<DWORD_PTR>(hwnd))) {
                return -1;
            }
            return 0;
        }
        case WM_SIZE: {
            if (state && state->edit) {
                PlaceEdit(hwnd, state->edit, LOWORD(lParam), HIWORD(lParam));
            }
            return 0;
        }
        case WM_SHOWWINDOW:
            if (state && wParam) {
                state->backdropValid = false;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        case WM_THEMECHANGED:
        case WM_SETTINGCHANGE:
            if (state) {
                state->backdropValid = false;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        case WM_SETFOCUS:
            if (state && state->edit) SetFocus(state->edit);
            return 0;
        case ESX_MSG_SUBMIT_XAML_SEARCH:
            SubmitOverlaySearch(hwnd, false);
            return 0;
        case WM_COMMAND:
            if (HIWORD(wParam) == EN_CHANGE || HIWORD(wParam) == EN_SETFOCUS ||
                HIWORD(wParam) == EN_KILLFOCUS) {
                InvalidateRect(hwnd, nullptr, FALSE);
                if (state) {
                    if (HIWORD(wParam) == EN_SETFOCUS) {
                        state->focusTarget = 1.0f;
                        SetTimer(hwnd, kFocusTimerId, 15, nullptr);
                        RememberSearchOrigin(state->frame);
                    } else if (HIWORD(wParam) == EN_KILLFOCUS) {
                        state->focusTarget = 0.0f;
                        SetTimer(hwnd, kFocusTimerId, 15, nullptr);
                    }
                }
            }
            return 0;
        case WM_TIMER:
            if (state && (wParam == kFocusTimerId || wParam == kHoverTimerId)) {
                auto step = [](float& value, float target) {
                    const float delta = target - value;
                    if (delta > -0.05f && delta < 0.05f) {
                        value = target;
                        return true;
                    }
                    value += delta * 0.35f;
                    return false;
                };
                bool done = true;
                if (wParam == kFocusTimerId) {
                    done = step(state->focusProgress, state->focusTarget) && done;
                    if (state->focusProgress == state->focusTarget) {
                        KillTimer(hwnd, kFocusTimerId);
                    }
                } else {
                    done = step(state->hoverProgress, state->hoverTarget) && done;
                    if (state->hoverProgress == state->hoverTarget) {
                        KillTimer(hwnd, kHoverTimerId);
                    }
                }
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            break;
        case WM_MOUSEMOVE: {
            if (state) {
                if (!state->hovered) {
                    state->hovered = true;
                    TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, hwnd, 0};
                    TrackMouseEvent(&track);
                    state->hoverTarget = 1.0f;
                    SetTimer(hwnd, kHoverTimerId, 15, nullptr);
                }
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            if (state) {
                state->hovered = false;
                state->hoverTarget = 0.0f;
                SetTimer(hwnd, kHoverTimerId, 15, nullptr);
            }
            return 0;
        case WM_CTLCOLOREDIT:
            if (state && state->editBrush) {
                const bool dark = IsDarkTheme();
                const bool placeholder = state->edit && GetWindowTextLengthW(state->edit) == 0 &&
                                         GetFocus() != state->edit;
                // The cue banner is rendered with the control's text color, so a
                // dimmer color here gives the placeholder its WinUI
                // TextFillColorSecondary look without drawing it twice.
                if (placeholder) {
                    SetTextColor(reinterpret_cast<HDC>(wParam),
                                 dark ? RGB(178, 178, 178) : RGB(93, 93, 93));
                } else {
                    SetTextColor(reinterpret_cast<HDC>(wParam),
                                 dark ? RGB(240, 240, 240) : RGB(20, 20, 20));
                }
                SetBkColor(reinterpret_cast<HDC>(wParam),
                           dark ? RGB(43, 43, 43) : RGB(250, 250, 250));
                return reinterpret_cast<LRESULT>(state->editBrush);
            }
            break;
        case WM_LBUTTONDOWN:
            ActivateOverlay(hwnd);
            return 0;
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            HDC dc = BeginPaint(hwnd, &paint);
            if (state && !state->backdropValid) SampleBackdrop(hwnd, *state);
            if (state && !PaintOverlayD2D(hwnd, *state)) {
                // Direct2D unavailable (remote session, driver reset): keep the
                // field usable with the GDI approximation.
                PaintOverlayGdi(dc, hwnd, *state);
            }
            EndPaint(hwnd, &paint);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_MOUSEACTIVATE:
            // The field is an owned popup: activating it keeps the Explorer
            // frame above it and in the taskbar as the same window.
            return MA_ACTIVATE;
        case WM_CLOSE:
            // Alt+F4 on the field must close the Explorer window, not just the
            // overlay.
            if (state && IsWindow(state->frame)) {
                PostMessageW(state->frame, WM_CLOSE, 0, 0);
                return 0;
            }
            DestroyWindow(hwnd);
            return 0;
        case WM_NCDESTROY: {
            if (state) {
                if (state->frame && IsWindow(state->frame) &&
                    GetPropW(state->frame, ESX_OVERLAY_PROP) == hwnd) {
                    RemovePropW(state->frame, ESX_OVERLAY_PROP);
                }
                if (state->font) DeleteObject(state->font);
                if (state->editBrush) DeleteObject(state->editBrush);
                if (state->renderTarget) state->renderTarget->Release();
                KillTimer(hwnd, kFocusTimerId);
                SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
                delete state;
            }
            return DefWindowProcW(hwnd, message, wParam, lParam);
        }
        default:
            break;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

void RememberSearchOrigin(HWND frame) {
    if (!IsWindow(frame)) return;
    HWND overlay = reinterpret_cast<HWND>(GetPropW(frame, ESX_OVERLAY_PROP));
    if (!IsWindow(overlay)) return;
    auto* state = reinterpret_cast<OverlayState*>(GetWindowLongPtrW(overlay, GWLP_USERDATA));
    if (!state) return;

    ComPtr<IShellBrowser> browser;
    ComPtr<IWebBrowser2> web;
    if (GetBrowserForWindow(frame, browser, web)) {
        const std::wstring path = GetFolderPath(browser.get(), web.get());
        if (!path.empty()) state->origin = path;
    }
}

void SubmitOverlaySearch(HWND overlay, bool reactivate) {
    auto* state = reinterpret_cast<OverlayState*>(GetWindowLongPtrW(overlay, GWLP_USERDATA));
    if (!state || !state->edit || !IsWindow(state->frame)) return;

    const int length = GetWindowTextLengthW(state->edit);
    if (length <= 0 || length > 1024) return;
    std::wstring query(static_cast<size_t>(length) + 1, L'\0');
    GetWindowTextW(state->edit, query.data(), length + 1);
    query.resize(static_cast<size_t>(length));

    const size_t first = query.find_first_not_of(L" \t\r\n");
    if (first == std::wstring::npos) return;
    const size_t last = query.find_last_not_of(L" \t\r\n");
    query = query.substr(first, last - first + 1);
    if (query.find_first_of(L"\r\n") != std::wstring::npos) return;

    const HWND frame = state->frame;
    if (state->origin.empty()) {
        ComPtr<IShellBrowser> browser;
        ComPtr<IWebBrowser2> web;
        if (GetBrowserForWindow(frame, browser, web)) {
            const std::wstring path = GetFolderPath(browser.get(), web.get());
            if (!path.empty()) state->origin = path;
        }
    }

    if (!WriteRequest(state->origin, query)) {
        MessageBoxW(frame, L"Impossibile scrivere la richiesta di ricerca.",
                    L"Everything Search", MB_ICONERROR | MB_OK);
        return;
    }

    NavigateWindow(frame);
    if (reactivate) {
        ActivateOverlay(overlay);
    }
}

// Re-applies the native search field geometry to the overlay. Called from the
// frame subclass on every layout notification, so the field tracks resizes,
// DPI changes and show/hide synchronously instead of waiting for a poll.
void LayoutOverlay(HWND frame) {
    if (!IsExplorerFrame(frame) || !IsWindowVisible(frame)) return;

    HWND overlay = reinterpret_cast<HWND>(GetPropW(frame, ESX_OVERLAY_PROP));
    if (!IsWindow(overlay)) return;

    const RECT screen = SearchBoxRect(frame);
    const int width = screen.right - screen.left;
    const int height = screen.bottom - screen.top;
    if (width < ScaleDpi(frame, 40) || height < ScaleDpi(frame, 16)) return;

    SetWindowPos(overlay, nullptr, screen.left, screen.top, width, height,
                 SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);

    auto* state = reinterpret_cast<OverlayState*>(GetWindowLongPtrW(overlay, GWLP_USERDATA));
    if (state && state->lastWidth == width && state->lastHeight == height &&
        state->lastX == screen.left && state->lastY == screen.top) {
        return;
    }

    if (state) {
        state->lastWidth = width;
        state->lastHeight = height;
        state->lastX = screen.left;
        state->lastY = screen.top;
        // The backdrop is sampled in WM_PAINT, never from a layout notification:
        // GetPixel on the desktop DC flushes DWM and must not run while the
        // frame is in the middle of its own layout.
        state->backdropValid = false;
    }
    InvalidateRect(overlay, nullptr, FALSE);
}

// The overlay is a child of CabinetWClass, so frame layout messages are the
// reliable signal that the native field moved or changed size.
BOOL CALLBACK EnsureOverlayForFrame(HWND hwnd, LPARAM);
void ActivateOverlay(HWND overlay);

// Never touch windows from inside the frame's layout notifications: Explorer is
// still inside its XAML island's layout pass there, and moving an owned window
// re-enters it. Post one coalesced request and do the work from the message
// loop instead.
void RequestOverlayLayout(HWND frame) {
    if (!frame || GetPropW(frame, ESX_PENDING_PROP)) return;
    SetPropW(frame, ESX_PENDING_PROP, reinterpret_cast<HANDLE>(1));
    if (!PostMessageW(frame, ESX_MSG_ENSURE_OVERLAY, 0, 0)) {
        RemovePropW(frame, ESX_PENDING_PROP);
    }
}

LRESULT CALLBACK FrameSubclassProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam,
                                   UINT_PTR subclassId, DWORD_PTR) {
    switch (message) {
        case WM_SIZE:
        case WM_WINDOWPOSCHANGED:
        case WM_DPICHANGED:
        case WM_SHOWWINDOW:
            RequestOverlayLayout(hwnd);
            break;
        case ESX_MSG_ENSURE_OVERLAY:
            RemovePropW(hwnd, ESX_PENDING_PROP);
            EnsureOverlayForFrame(hwnd, 0);
            return 0;
        case ESX_MSG_FOCUS_OVERLAY: {
            RemovePropW(hwnd, ESX_PENDING_PROP);
            EnsureOverlayForFrame(hwnd, 0);
            HWND overlay = reinterpret_cast<HWND>(GetPropW(hwnd, ESX_OVERLAY_PROP));
            if (IsWindow(overlay)) {
                ActivateOverlay(overlay);
                if (wParam) {
                    auto* state = reinterpret_cast<OverlayState*>(
                        GetWindowLongPtrW(overlay, GWLP_USERDATA));
                    if (state && state->edit) {
                        SendMessageW(state->edit, EM_SETSEL, 0, -1);
                    }
                }
            }
            return 0;
        }
        case ESX_MSG_SHOW_PANEL:
            ShowPanel(hwnd);
            return 0;
        case WM_NCDESTROY:
            RemovePropW(hwnd, ESX_PENDING_PROP);
            RemoveWindowSubclass(hwnd, FrameSubclassProc, subclassId);
            break;
        default:
            break;
    }
    return DefSubclassProc(hwnd, message, wParam, lParam);
}

BOOL CALLBACK EnsureOverlayForFrame(HWND hwnd, LPARAM) {
    if (!IsWindowVisible(hwnd) || !IsExplorerFrame(hwnd)) return TRUE;
    // Windows must be created on the thread that owns the frame.
    if (GetWindowThreadProcessId(hwnd, nullptr) != GetCurrentThreadId()) return TRUE;

    if (IsWindow(reinterpret_cast<HWND>(GetPropW(hwnd, ESX_OVERLAY_PROP)))) {
        LayoutOverlay(hwnd);
        return TRUE;
    }

    const RECT screen = SearchBoxRect(hwnd);
    const int width = screen.right - screen.left;
    const int height = screen.bottom - screen.top;
    if (width < ScaleDpi(hwnd, 40) || height < ScaleDpi(hwnd, 16)) return TRUE;

    auto* state = new (std::nothrow) OverlayState();
    if (!state) return TRUE;
    state->frame = hwnd;
    // The field must be a top-level popup owned by the frame, not a child:
    // the command bar is a WinUI 3 XAML island and DWM composites its content
    // above any child HWND, whatever the child z-order says. Only a separate
    // top-level window is composited above the island.
    HWND overlay = CreateWindowExW(
        WS_EX_TOOLWINDOW, ESX_OVERLAY_CLASS, L"",
        WS_POPUP | WS_CLIPCHILDREN,
        screen.left, screen.top, width, height, hwnd, nullptr,
        GetModuleHandleW(nullptr), state);
    if (!overlay) {
        // A failing WM_CREATE has already destroyed the window, which frees
        // the state in WM_NCDESTROY.
        return TRUE;
    }
    if (!SetPropW(hwnd, ESX_OVERLAY_PROP, overlay)) {
        DestroyWindow(overlay);
        return TRUE;
    }

    SetWindowSubclass(hwnd, FrameSubclassProc, 2, 0);
    RememberSearchOrigin(hwnd);
    LayoutOverlay(hwnd);
    return TRUE;
}

BOOL CALLBACK PostOverlayRequest(HWND hwnd, LPARAM) {
    if (!IsWindowVisible(hwnd) || !IsExplorerFrame(hwnd)) return TRUE;
    // Windows must be created on the thread that owns the frame.
    if (GetWindowThreadProcessId(hwnd, nullptr) != GetCurrentThreadId()) return TRUE;
#ifdef ESX_MOD_TRACE
    // This callback runs on the frame's UI thread, which is the only thread that
    // can create a XAML island for it.
    static thread_local bool probed = false;
    if (!probed) {
        probed = true;
        ProbeWinRtActivation();
    }
#endif
    // Bootstrap: the deferred layout message is handled by the subclass, so the
    // subclass must exist before anything is posted. SetWindowSubclass only
    // swaps a window procedure pointer, it does not touch DWM or XAML.
    SetWindowSubclass(hwnd, FrameSubclassProc, 2, 0);
    RequestOverlayLayout(hwnd);
    return TRUE;
}

thread_local ULONGLONG g_lastOverlayScan = 0;
void MaintainOverlays() {
    const ULONGLONG now = GetTickCount64();
    if (now - g_lastOverlayScan < 700) return;
    g_lastOverlayScan = now;
    // Only process windows owned by this message-pump thread; this avoids
    // creating child HWNDs from a different Explorer UI thread. The actual work
    // happens when the posted message is dispatched, never inside this hook.
    EnumThreadWindows(GetCurrentThreadId(), PostOverlayRequest, 0);
}

// ------------------------------------------------------------------ input hooks

BOOL(WINAPI* PeekMessageW_Original)(LPMSG, HWND, UINT, UINT, UINT) = nullptr;
BOOL(WINAPI* GetMessageW_Original)(LPMSG, HWND, UINT, UINT) = nullptr;

// Resolves the Explorer frame for a message target. The search field is an
// owned popup now, so messages coming from it resolve to the popup itself.
HWND ExplorerFrameFor(HWND hwnd) {
    HWND root = hwnd ? GetAncestor(hwnd, GA_ROOT) : nullptr;
    if (!root) return nullptr;
    wchar_t className[64]{};
    if (GetClassNameW(root, className, ARRAYSIZE(className)) &&
        _wcsicmp(className, ESX_OVERLAY_CLASS) == 0) {
        auto* state = reinterpret_cast<OverlayState*>(GetWindowLongPtrW(root, GWLP_USERDATA));
        if (state && IsWindow(state->frame)) return state->frame;
    }
    return root;
}

BOOL HandleRetrievedMessage(LPMSG msg, BOOL result, bool removed) {
    MaintainOverlays();
    // PeekMessageW may return FALSE without initializing MSG. Never inspect it
    // unless a message was actually retrieved and removed from the queue.
    if (!msg || result <= 0 || !removed || msg->message != WM_KEYDOWN) return result;

    HWND root = ExplorerFrameFor(msg->hwnd);
    if (!IsExplorerFrame(root)) return result;

    // GetAsyncKeyState, not GetKeyState: this hook inspects messages while they
    // are being retrieved, before the thread has dispatched them, so the
    // thread's synchronized key state may not include the just-pressed
    // modifier yet.
    const bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    const bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    const bool alt = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
    ESX_MOD_LOG("key vk=0x%X ctrl=%d shift=%d alt=%d target=%p root=%p", 
                static_cast<unsigned>(msg->wParam), ctrl ? 1 : 0, shift ? 1 : 0, alt ? 1 : 0,
                static_cast<void*>(msg->hwnd), static_cast<void*>(root));

    if (msg->wParam == 'F' && ctrl && shift && !alt) {
        msg->message = WM_NULL;
        PostMessageW(root, ESX_MSG_SHOW_PANEL, 0, 0);
        return result;
    }

    const bool ctrlF = msg->wParam == 'F' && ctrl && !shift && !alt;
    const bool ctrlE = msg->wParam == 'E' && ctrl && !shift && !alt;
    const bool f3 = msg->wParam == VK_F3 && !ctrl && !shift && !alt;
    if (ctrlF || ctrlE || f3) {
        msg->message = WM_NULL;
        // Never create windows or move focus here: post to the frame and let
        // its message loop do the work. The subclass must exist to receive the
        // message, and installing it is a pure pointer swap.
        SetWindowSubclass(root, FrameSubclassProc, 2, 0);
        PostMessageW(root, ESX_MSG_FOCUS_OVERLAY, ctrlE ? 1 : 0, 0);
    }
    return result;
}

BOOL WINAPI PeekMessageW_Hook(LPMSG msg, HWND hwnd, UINT filterMin, UINT filterMax,
                              UINT removeMsg) {
    const BOOL result = PeekMessageW_Original(msg, hwnd, filterMin, filterMax, removeMsg);
    return HandleRetrievedMessage(msg, result, (removeMsg & PM_REMOVE) != 0);
}

BOOL WINAPI GetMessageW_Hook(LPMSG msg, HWND hwnd, UINT filterMin, UINT filterMax) {
    const BOOL result = GetMessageW_Original(msg, hwnd, filterMin, filterMax);
    return HandleRetrievedMessage(msg, result, true);
}

#ifdef ESX_MOD_TRACE
// Temporary probe: Explorer already hosts a WinUI 3 content island, so the
// Windows App SDK package graph is active in this process. If WinRT activation
// of Microsoft.UI.Xaml resolves here, we can create our own XAML island inside
// CabinetWClass instead of painting a GDI imitation over the native field.
void ProbeWinRtActivation() {
    using RoGetActivationFactoryFn = HRESULT(WINAPI*)(HSTRING, REFIID, void**);
    using WindowsCreateStringFn = HRESULT(WINAPI*)(LPCWSTR, UINT32, HSTRING*);
    using WindowsDeleteStringFn = HRESULT(WINAPI*)(HSTRING);
    static const GUID kIidActivationFactory = {
        0x00000035, 0x0000, 0x0000, {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};

    HMODULE combase = GetModuleHandleW(L"combase.dll");
    if (!combase) {
        ESX_MOD_LOG("probe: combase.dll not loaded");
        return;
    }
    auto getFactory = reinterpret_cast<RoGetActivationFactoryFn>(
        GetProcAddress(combase, "RoGetActivationFactory"));
    auto createString = reinterpret_cast<WindowsCreateStringFn>(
        GetProcAddress(combase, "WindowsCreateString"));
    auto deleteString = reinterpret_cast<WindowsDeleteStringFn>(
        GetProcAddress(combase, "WindowsDeleteString"));
    if (!getFactory || !createString || !deleteString) {
        ESX_MOD_LOG("probe: WinRT string/activation entry points missing");
        return;
    }

    APTTYPE apartmentType = APTTYPE_CURRENT;
    APTTYPEQUALIFIER qualifier = APTTYPEQUALIFIER_NONE;
    const HRESULT apartment = CoGetApartmentType(&apartmentType, &qualifier);
    ESX_MOD_LOG("probe apartment hr=0x%08X type=%d qualifier=%d", apartment,
                static_cast<int>(apartmentType), static_cast<int>(qualifier));

    const wchar_t* classes[] = {
        L"Microsoft.UI.Xaml.Application",
        L"Microsoft.UI.Xaml.Controls.TextBox",
        L"Microsoft.UI.Xaml.Controls.AutoSuggestBox",
        L"Microsoft.UI.Xaml.Hosting.DesktopWindowXamlSource",
        L"Microsoft.UI.Content.DesktopChildSiteBridge",
        L"Windows.UI.Xaml.Controls.TextBox",
        L"Windows.UI.Xaml.Hosting.DesktopWindowXamlSource",
    };
    for (const wchar_t* name : classes) {
        HSTRING id = nullptr;
        if (FAILED(createString(name, static_cast<UINT32>(wcslen(name)), &id))) {
            ESX_MOD_LOG("probe %ls: HSTRING failed", name);
            continue;
        }
        IInspectable* factory = nullptr;
        const HRESULT hr = getFactory(id, kIidActivationFactory,
                                      reinterpret_cast<void**>(&factory));
        ESX_MOD_LOG("probe %ls -> hr=0x%08X factory=%p", name, hr, factory);
        if (factory) factory->Release();
        deleteString(id);
    }
}
#endif

}  // namespace

// ------------------------------------------------------------------- init

void LoadSettings() {
    g_maxResults = Wh_GetIntSetting(L"maxResults");
    if (g_maxResults < 0) g_maxResults = 0;
    g_scopeToFolder = Wh_GetIntSetting(L"scopeToFolder") != 0;
}

BOOL Wh_ModInit() {
    LoadSettings();
    ESX_MOD_LOG("Wh_ModInit pid=%lu maxResults=%d scopeToFolder=%d", GetCurrentProcessId(),
                g_maxResults, g_scopeToFolder ? 1 : 0);
#ifdef ESX_MOD_TRACE
    ProbeWinRtActivation();
#endif

    WNDCLASSEXW panelClass{};
    panelClass.cbSize = sizeof(panelClass);
    panelClass.hInstance = GetModuleHandleW(nullptr);
    panelClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    panelClass.lpfnWndProc = PanelProc;
    panelClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    panelClass.lpszClassName = ESX_PANEL_CLASS;
    if (!RegisterClassExW(&panelClass)) {
        Wh_Log(L"RegisterClassExW(panel) failed: %lu", GetLastError());
        return FALSE;
    }

    WNDCLASSEXW overlayClass{};
    overlayClass.cbSize = sizeof(overlayClass);
    overlayClass.hInstance = GetModuleHandleW(nullptr);
    overlayClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    overlayClass.lpfnWndProc = OverlayProc;
    overlayClass.lpszClassName = ESX_OVERLAY_CLASS;
    if (!RegisterClassExW(&overlayClass)) {
        Wh_Log(L"RegisterClassExW(search overlay) failed: %lu", GetLastError());
        UnregisterClassW(ESX_PANEL_CLASS, GetModuleHandleW(nullptr));
        return FALSE;
    }

    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (!user32) {
        UnregisterClassW(ESX_OVERLAY_CLASS, GetModuleHandleW(nullptr));
        UnregisterClassW(ESX_PANEL_CLASS, GetModuleHandleW(nullptr));
        return FALSE;
    }

    void* peekMessage = reinterpret_cast<void*>(GetProcAddress(user32, "PeekMessageW"));
    if (!peekMessage ||
        !Wh_SetFunctionHook(peekMessage, reinterpret_cast<void*>(PeekMessageW_Hook),
                            reinterpret_cast<void**>(&PeekMessageW_Original))) {
        Wh_Log(L"Failed to hook PeekMessageW");
        UnregisterClassW(ESX_OVERLAY_CLASS, GetModuleHandleW(nullptr));
        UnregisterClassW(ESX_PANEL_CLASS, GetModuleHandleW(nullptr));
        return FALSE;
    }

    void* getMessage = reinterpret_cast<void*>(GetProcAddress(user32, "GetMessageW"));
    if (getMessage &&
        !Wh_SetFunctionHook(getMessage, reinterpret_cast<void*>(GetMessageW_Hook),
                            reinterpret_cast<void**>(&GetMessageW_Original))) {
        Wh_Log(L"GetMessageW hook unavailable; PeekMessageW hook remains active");
    }

    Wh_Log(L"Everything Search for Explorer 0.5.0 loaded");
    return TRUE;
}

BOOL CALLBACK CloseOverlayForFrame(HWND hwnd, LPARAM) {
    DWORD processId = 0;
    GetWindowThreadProcessId(hwnd, &processId);
    if (processId != GetCurrentProcessId() || !IsExplorerFrame(hwnd)) return TRUE;
    HWND overlay = reinterpret_cast<HWND>(GetPropW(hwnd, ESX_OVERLAY_PROP));
    if (IsWindow(overlay)) {
        DWORD_PTR ignored = 0;
        SendMessageTimeoutW(overlay, WM_CLOSE, 0, 0,
                            SMTO_ABORTIFHUNG | SMTO_BLOCK, 750, &ignored);
    }
    return TRUE;
}

void Wh_ModUninit() {
    EnumWindows(CloseOverlayForFrame, 0);
    if (g_panel && IsWindow(g_panel->hwnd)) {
        DWORD_PTR ignored = 0;
        SendMessageTimeoutW(g_panel->hwnd, WM_CLOSE, 0, 0,
                            SMTO_ABORTIFHUNG | SMTO_BLOCK, 750, &ignored);
    }

    UnregisterClassW(ESX_OVERLAY_CLASS, GetModuleHandleW(nullptr));
    UnregisterClassW(ESX_PANEL_CLASS, GetModuleHandleW(nullptr));
    g_lastOverlayScan = 0;
}

void Wh_ModSettingsChanged() {
    LoadSettings();
}
