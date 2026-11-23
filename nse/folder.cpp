#include "folder.h"

#include <propvarutil.h>
#include <propsys.h>

#include "bridge.h"

namespace {

// EXPLORERPANE values from ShlGuid.h (not declared by the MinGW headers).
// {d9745868-ca5f-4a76-91cd-f5a129fbb076}
constexpr GUID kPaneCommands = {
    0xd9745868, 0xca5f, 0x4a76, {0x91, 0xcd, 0xf5, 0xa1, 0x29, 0xfb, 0xb0, 0x76}};
// {72e81700-e3ec-4660-bf24-3c3b7b648806}
constexpr GUID kPaneCommandsOrganize = {
    0x72e81700, 0xe3ec, 0x4660, {0xbf, 0x24, 0x3c, 0x3b, 0x7b, 0x64, 0x88, 0x06}};
// {21f7c32d-eeaa-439b-bb51-37b96fd6a943}
constexpr GUID kPaneCommandsView = {
    0x21f7c32d, 0xeeaa, 0x439b, {0xbb, 0x51, 0x37, 0xb9, 0x6f, 0xd6, 0xa9, 0x43}};

// Timing of Explorer's shell-view construction, measured from EnumObjects.
ULONGLONG g_enumEnd = 0;
ULONGLONG g_firstAttribute = 0;
ULONGLONG g_lastAttribute = 0;
DWORD g_attributeCalls = 0;
DWORD g_displayCalls = 0;

HRESULT MakeCompareResult(int cmp) {
    return MAKE_HRESULT(SEVERITY_SUCCESS, 0, static_cast<WORD>(static_cast<SHORT>(cmp)));
}

// The shell calls IShellFolder::EnumObjects with the DefView window as hwnd.
// Its root is the top-level Explorer frame, which is exactly the key the mod
// uses for the per-window request file. This is the only reliable window
// identity available to the enumerating CFolder: the instance that receives
// SFVM_WINDOWCREATED is a different one, so m_viewHwnd is null here.
HWND FrameFromEnumHwnd(HWND hwnd) {
    if (!hwnd) return nullptr;
    // A tabbed Explorer window has one CabinetWClass frame shared by all tabs.
    // The per-tab ShellTabWindowClass is the correct request-file key.
    for (HWND current = hwnd; current; current = ::GetParent(current)) {
        wchar_t className[64]{};
        if (!::GetClassNameW(current, className, ARRAYSIZE(className))) continue;
        if (_wcsicmp(className, L"ShellTabWindowClass") == 0) return current;
    }
    HWND frame = ::GetAncestor(hwnd, GA_ROOT);
    if (!frame || !::IsWindow(frame)) return nullptr;
    wchar_t className[64]{};
    const int len = ::GetClassNameW(frame, className, ARRAYSIZE(className));
    if (len == 0 || _wcsicmp(className, L"CabinetWClass") != 0) return nullptr;
    return frame;
}

std::wstring FileNameOf(const std::wstring& path) {
    const wchar_t* name = ::PathFindFileNameW(path.c_str());
    return (name && *name) ? std::wstring(name) : path;
}

std::wstring FormatBytes(std::uint64_t size) {
    wchar_t buf[64]{};
    ::StrFormatByteSizeW(size, buf, ARRAYSIZE(buf));
    return buf;
}

// The shell can ask IShellFolder2 for details on a CFolder instance that never
// enumerated the results, so the in-memory Item cache may be empty. Visible
// rows are few: reading the real file attributes is cheap and always correct.
bool GetFileInfoForColumn(const std::wstring& path, std::uint64_t& size, FILETIME& modified,
                          bool& isFolder) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!::GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) return false;
    size = (static_cast<std::uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
    modified = data.ftLastWriteTime;
    isFolder = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    return true;
}

bool IsSamePathIgnoringTrailingSlash(const std::wstring& first, const std::wstring& second) {
    std::size_t a = first.size();
    std::size_t b = second.size();
    while (a > 0 && (first[a - 1] == L'\\' || first[a - 1] == L'/')) --a;
    while (b > 0 && (second[b - 1] == L'\\' || second[b - 1] == L'/')) --b;
    if (a == 0 || a != b) return false;
    return ::_wcsnicmp(first.c_str(), second.c_str(), a) == 0;
}

std::wstring ParentDirectoryOf(const std::wstring& path) {
    std::wstring parent = path;
    while (!parent.empty() && (parent.back() == L'\\' || parent.back() == L'/')) {
        parent.pop_back();
    }
    const size_t slash = parent.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return {};
    if (slash == 2 && parent.size() > 2 && parent[1] == L':') return parent.substr(0, 3);
    return parent.substr(0, slash);
}

// Ask the property system for the localized column name, so "Ordina per" and
// the Details headers follow the language of Windows instead of our literals.
std::wstring LocalizedColumnName(REFPROPERTYKEY key, const wchar_t* fallback) {
    ComPtr<IPropertyDescription> description;
    if (SUCCEEDED(::PSGetPropertyDescription(key, IID_PPV_ARGS(description.put())))) {
        PWSTR name = nullptr;
        if (SUCCEEDED(description->GetDisplayName(&name)) && name) {
            const std::wstring text(name);
            ::CoTaskMemFree(name);
            return text;
        }
    }
    return fallback;
}

struct SortValue {
    std::uint64_t size = 0;
    FILETIME modified{};
    bool folder = false;
    bool valid = false;
};

std::mutex g_sortCacheMutex;
std::unordered_map<std::wstring, SortValue> g_sortCache;

SortValue SortValueFor(const std::wstring& path) {
    {
        std::lock_guard<std::mutex> lock(g_sortCacheMutex);
        // Sizes and dates are properties of the file, not of the query, so a
        // new generation does not invalidate them. Just cap the cache.
        if (g_sortCache.size() >= 4096) {
            g_sortCache.clear();
        }
        const auto it = g_sortCache.find(path);
        if (it != g_sortCache.end()) return it->second;
    }
    SortValue value{};
    if (GetFileInfoForColumn(path, value.size, value.modified, value.folder)) {
        value.valid = true;
    }
    {
        std::lock_guard<std::mutex> lock(g_sortCacheMutex);
        g_sortCache[path] = value;
    }
    return value;
}

std::wstring FormatFileTime(const FILETIME& ft) {
    if (ft.dwLowDateTime == 0 && ft.dwHighDateTime == 0) return {};
    FILETIME local = ft;
    ::FileTimeToLocalFileTime(&ft, &local);
    SYSTEMTIME st{};
    if (!::FileTimeToSystemTime(&local, &st)) return {};

    wchar_t date[128]{};
    wchar_t time[128]{};
    ::GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, DATE_SHORTDATE, &st, nullptr, date,
                      ARRAYSIZE(date), nullptr);
    ::GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, TIME_NOSECONDS, &st, nullptr, time,
                      ARRAYSIZE(time));

    std::wstring out = date;
    out += L' ';
    out += time;
    return out;
}

std::wstring TypeNameOf(const std::wstring& path, DWORD attributes) {
    SHFILEINFOW sfi{};
    if (::SHGetFileInfoW(path.c_str(), attributes, &sfi, sizeof(sfi),
                         SHGFI_TYPENAME | SHGFI_USEFILEATTRIBUTES)) {
        return sfi.szTypeName;
    }
    return {};
}

HRESULT StrDupToStrRet(const std::wstring& text, STRRET* pName) {
    if (!pName) return E_POINTER;
    pName->uType = STRRET_WSTR;
    return ::SHStrDupW(text.c_str(), &pName->pOleStr);
}

// Everything already gives us FILE_ATTRIBUTE_* for every result. Deriving the
// common SFGAO_* bits from it avoids one SHParseDisplayName per row, which is
// what made a large result list hang Explorer. Anything not derivable is still
// asked of the shell (and cached) in GetAttributesOf.
SFGAOF AttributesFromFileFlags(DWORD fileAttributes) {
    SFGAOF attributes = SFGAO_FILESYSTEM;
    if (fileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
        attributes |= SFGAO_FOLDER | SFGAO_BROWSABLE;
    }
    if (fileAttributes & FILE_ATTRIBUTE_READONLY) attributes |= SFGAO_READONLY;
    if (fileAttributes & FILE_ATTRIBUTE_HIDDEN) attributes |= SFGAO_HIDDEN;
    if (fileAttributes & FILE_ATTRIBUTE_SYSTEM) attributes |= SFGAO_SYSTEM;
    if (fileAttributes & FILE_ATTRIBUTE_COMPRESSED) attributes |= SFGAO_COMPRESSED;
    if (fileAttributes & FILE_ATTRIBUTE_ENCRYPTED) attributes |= SFGAO_ENCRYPTED;
    return attributes;
}

std::wstring LoadMuiString(const wchar_t* resourceId, const wchar_t* fallback) {
    wchar_t buffer[512]{};
    if (::SHLoadIndirectString(resourceId, buffer, static_cast<UINT>(ARRAYSIZE(buffer)), nullptr) == S_OK) {
        if (buffer[0] != L'\0') {
            return buffer;
        }
    }
    return fallback;
}

std::wstring FormatWithFolder(const std::wstring& pattern, const std::wstring& folder) {
    const size_t pos = pattern.find(L"%s");
    if (pos == std::wstring::npos) {
        return pattern + L" " + folder;
    }
    std::wstring result = pattern;
    result.replace(pos, 2, folder);
    return result;
}

}  // namespace

// =========================================================== CFolder / COM

HRESULT CFolder::Create(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;

    ESX_TRACE_LOG("CFolder::Create");

    auto* folder = new (std::nothrow) CFolder();
    if (!folder) return E_OUTOFMEMORY;

    ::InterlockedIncrement(&g_cRefDll);
    folder->m_rootPidl = CreateRootPidl();
    if (!folder->m_rootPidl) {
        folder->Release();
        return E_OUTOFMEMORY;
    }

    const HRESULT hr = folder->QueryInterface(riid, ppv);
    folder->Release();
    return hr;
}

CFolder::~CFolder() {
    ClearItems();
    if (m_rootPidl) ::CoTaskMemFree(m_rootPidl);
    ::InterlockedDecrement(&g_cRefDll);
}

HRESULT STDMETHODCALLTYPE CFolder::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;

    if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IShellFolder) ||
        IsEqualIID(riid, IID_IShellFolder2)) {
        *ppv = static_cast<IShellFolder2*>(this);
    } else if (IsEqualIID(riid, IID_IPersist) || IsEqualIID(riid, IID_IPersistFolder) ||
               IsEqualIID(riid, IID_IPersistFolder2)) {
        *ppv = static_cast<IPersistFolder2*>(this);
    } else if (IsEqualIID(riid, IID_IShellFolderViewCB)) {
        *ppv = static_cast<IShellFolderViewCB*>(this);
    } else if (IsEqualIID(riid, IID_IServiceProvider)) {
        *ppv = static_cast<IServiceProvider*>(this);
    } else if (IsEqualIID(riid, IID_IExplorerPaneVisibility)) {
        *ppv = static_cast<IExplorerPaneVisibility*>(this);
    } else {
        return E_NOINTERFACE;
    }

    AddRef();
    return S_OK;
}

ULONG STDMETHODCALLTYPE CFolder::AddRef() {
    return static_cast<ULONG>(::InterlockedIncrement(&m_ref));
}

ULONG STDMETHODCALLTYPE CFolder::Release() {
    const LONG ref = ::InterlockedDecrement(&m_ref);
    if (ref == 0) delete this;
    return static_cast<ULONG>(ref);
}

// ---------------------------------------------------------------- IPersist

HRESULT STDMETHODCALLTYPE CFolder::GetClassID(CLSID* pclsid) {
    if (!pclsid) return E_POINTER;
    *pclsid = CLSID_EverythingSearchFolder;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CFolder::Initialize(PCIDLIST_ABSOLUTE pidl) {
    ESX_TRACE_LOG("Initialize(pidl=%p ours=%d root=%d)", pidl, pidl ? IsOurPidl(pidl) : 0,
                  pidl ? IsRootPidl(pidl) : 0);
    if (pidl) {
        PIDLIST_ABSOLUTE clone = ::ILClone(pidl);
        if (!clone) return E_OUTOFMEMORY;
        if (m_rootPidl) ::CoTaskMemFree(m_rootPidl);
        m_rootPidl = clone;
    } else if (!m_rootPidl) {
        m_rootPidl = CreateRootPidl();
        if (!m_rootPidl) return E_OUTOFMEMORY;
    }
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CFolder::GetCurFolder(PIDLIST_ABSOLUTE* ppidl) {
    ESX_TRACE_LOG("GetCurFolder");
    if (!ppidl) return E_POINTER;
    *ppidl = m_rootPidl ? ::ILClone(m_rootPidl) : nullptr;
    return *ppidl ? S_OK : E_OUTOFMEMORY;
}

// ------------------------------------------------------------- item cache

void CFolder::ClearItems() {
    for (auto& item : m_items) {
        if (item.pidl) ::CoTaskMemFree(item.pidl);
    }
    m_items.clear();
    m_index.clear();
}

const CFolder::Item* CFolder::FindItemLocked(const std::wstring& path) const {
    const auto it = m_index.find(path);
    return it == m_index.end() ? nullptr : &m_items[it->second];
}

std::wstring CFolder::DisplayNameLocked(SHGDNF uFlags) const {
    std::wstring folderName;
    if (!m_folder.empty()) {
        folderName = FileNameOf(m_folder);
        if (folderName.empty()) folderName = m_folder;
    } else {
        folderName = LoadMuiString(L"@shell32.dll,-9216", L"This PC");
    }
    const std::wstring pattern = LoadMuiString(L"@shell32.dll,-34132", L"Search results in %s");
    const std::wstring base = FormatWithFolder(pattern, folderName);

    if (uFlags & SHGDN_FORADDRESSBAR) return base;
    if (m_query.empty()) return base;
    return m_query + L" - " + base;
}

void CFolder::AppendPageLocked(const std::vector<esx::QueryResultItem>& page) {
    m_items.reserve(m_items.size() + page.size());
    for (const auto& r : page) {
        // `path:"C:\dir" *` matches the directory itself. The old removal lived
        // only in bridge::Query(), which Explorer never calls: filter here so
        // the lazy enumerator used by the shell does not show the origin folder.
        if ((m_flags & static_cast<DWORD>(esx::ScopeToFolder)) && !m_folder.empty() &&
            IsSamePathIgnoringTrailingSlash(r.path, m_folder)) {
            continue;
        }
        Item item;
        item.path = r.path;
        item.name = FileNameOf(item.path);
        item.size = r.size;
        item.modified.dwLowDateTime = static_cast<DWORD>(r.modified);
        item.modified.dwHighDateTime = static_cast<DWORD>(r.modified >> 32);
        item.attributes = r.attributes;
        item.shellAttributes = AttributesFromFileFlags(static_cast<DWORD>(r.attributes));
        // Everything already gave us the file flags. Report every derived SFGAO
        // bit as known so GetAttributesOf never has to touch the file system for
        // each row; only SFGAO_VALIDATE stays queryable.
        item.shellAttributesKnown = ~static_cast<SFGAOF>(SFGAO_VALIDATE);
        item.isFolder = r.isFolder;
        item.pidl = CreateItemPidl(item.path);
        if (!item.pidl) continue;  // out of memory: keep the rest

        m_index.emplace(item.path, m_items.size());
        m_items.push_back(std::move(item));
    }
}

bool CFolder::LoadMore() {
    // One fetch at a time per folder, and never hold m_mutex during IPC:
    // GetAttributesOf/GetDetailsOf on other shell threads would block.
    std::lock_guard<std::mutex> fetch(m_fetchMutex);

    std::wstring search;
    DWORD offset = 0;
    DWORD flags = 0;
    DWORD wanted = 0;
    std::uint64_t generation = 0;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_queryComplete || m_failed) return false;
        // m_nextOffset is Everything's raw offset and also counts filtered
        // items, so it cannot measure how many results are still missing.
        if (m_items.size() >= m_limit) {
            m_queryComplete = true;
            return false;
        }
        search = m_search;
        offset = m_nextOffset;
        flags = m_flags;
        wanted = std::min<DWORD>(bridge::kBatchSize,
                                 static_cast<DWORD>(m_limit - m_items.size()));
        generation = m_generation.load(std::memory_order_relaxed);
    }

    if (!bridge::EnsureReady()) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_failed = true;
        m_queryComplete = true;
        return false;
    }

    std::vector<esx::QueryResultItem> page;
    DWORD raw = 0;
    DWORD total = 0;
    std::string error;
    if (!bridge::QueryPage(search, offset, flags, wanted, page, raw, total, error)) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_failed = true;
        m_queryComplete = true;
        ::OutputDebugStringA(("[esx] " + error + "\n").c_str());
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (generation != m_generation.load(std::memory_order_relaxed)) {
            return false;  // a new query replaced this page
        }
        AppendPageLocked(page);
        m_nextOffset = offset + raw;
        m_totalCount = total;
        if (raw < wanted || total <= m_nextOffset || m_items.size() >= m_limit) {
            m_queryComplete = true;
        }
    }
    return true;
}

void CFolder::LoadItemsLocked(HWND frameHint) {
    const ULONGLONG tStart = ::GetTickCount64();
    // EnumObjects runs on a different CFolder than the one that received the
    // view callbacks, so prefer the hint captured from its hwnd parameter.
    const HWND frame = frameHint ? frameHint : OwnerFrame();
    esx::QueryRequest request;
    if (!esx::ReadRequestForWindow(frame, request)) {
        ClearItems();
        m_loaded = false;
        m_failed = false;
        m_seq = 0;
        m_query.clear();
        m_folder.clear();
        m_filter.clear();
        m_contentSearch = false;
        return;
    }
    ESX_TRACE_LOG("LoadItems frame=%p seq=%u query='%ls'", static_cast<void*>(frame),
                  request.seq, request.query.c_str());

    // When the query is empty, show an empty list immediately without sending a wildcard search to Everything.
    if (request.query.empty()) {
        ClearItems();
        m_generation.fetch_add(1, std::memory_order_relaxed);
        m_loaded = true;
        m_failed = false;
        m_seq = request.seq;
        m_query.clear();
        m_folder.clear();
        m_filter.clear();
        m_contentSearch = false;
        m_queryComplete = true;
        return;
    }

    // Everything view refreshes re-enumerate constantly; skip the round trip
    // when the request did not change.
    if (m_loaded && request.seq == m_seq && request.query == m_query &&
        request.folder == m_folder && request.maxResults == m_maxResults &&
        request.flags == m_flags && request.filter == m_filter &&
        request.contentSearch == m_contentSearch) {
        return;
    }

    ClearItems();
    m_generation.fetch_add(1, std::memory_order_relaxed);
    m_loaded = true;
    m_failed = false;
    m_seq = request.seq;
    m_query = request.query;
    m_folder = request.folder;
    m_filter = request.filter;
    m_contentSearch = request.contentSearch;
    m_maxResults = request.maxResults;
    m_flags = request.flags;

    m_search = bridge::BuildSearchString(request);
    m_limit = (request.maxResults && request.maxResults < bridge::kSafetyCeiling)
                  ? request.maxResults
                  : bridge::kSafetyCeiling;
    m_nextOffset = 0;
    m_totalCount = 0;
    m_queryComplete = false;

    // The first page is fetched by the caller after releasing m_mutex, via
    // LoadMore(). The rest is loaded by EnsureLoaded() as Explorer walks the
    // enumerator. Keep this method free of IPC.
    ESX_TRACE_LOG("TIMING request prepared=%llu ms", ::GetTickCount64() - tStart);
}

bool CFolder::GetPathAt(size_t index, std::wstring& path) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (index >= m_items.size()) return false;
    path = m_items[index].path;
    return true;
}

bool CFolder::IsIncluded(size_t index, DWORD flags) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (index >= m_items.size()) return false;
    const bool folder = m_items[index].isFolder;
    if (folder) return (flags & SHCONTF_FOLDERS) != 0;
    return (flags & SHCONTF_NONFOLDERS) != 0;
}

bool CFolder::EnsureLoaded(size_t index) {
    for (;;) {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (index < m_items.size() || m_queryComplete) {
                return index < m_items.size();
            }
        }

        // LoadMore() owns m_fetchMutex and keeps IPC outside m_mutex; the
        // caller only waits for the next page to become available.
        if (!LoadMore()) {
            std::lock_guard<std::mutex> lock(m_mutex);
            return index < m_items.size();
        }
    }
}

// ------------------------------------------------------------ IShellFolder

HRESULT STDMETHODCALLTYPE CFolder::ParseDisplayName(HWND, IBindCtx*, LPWSTR pszDisplayName,
                                                    ULONG* pchEaten, PIDLIST_RELATIVE* ppidl,
                                                    ULONG* pdwAttributes) {
    if (!ppidl) return E_POINTER;
    *ppidl = nullptr;
    if (pchEaten) *pchEaten = 0;

    std::wstring name = pszDisplayName ? pszDisplayName : L"";
    // Strip only spaces here: leading backslashes are meaningful for UNC paths.
    while (!name.empty() && name.front() == L' ') name.erase(0, 1);

    if (name.empty()) {
        *ppidl = CreateRootPidl();
        return *ppidl ? S_OK : E_OUTOFMEMORY;
    }

    // Accept "::{GUID}" and "::{GUID}\C:\path" so the address bar can be used.
    wchar_t guidText[64]{};
    if (::StringFromGUID2(CLSID_EverythingSearchFolder, guidText, ARRAYSIZE(guidText)) > 0) {
        std::wstring prefix = L"::";
        prefix += guidText;
        if (name.size() >= prefix.size() &&
            _wcsnicmp(name.c_str(), prefix.c_str(), prefix.size()) == 0) {
            name.erase(0, prefix.size());
            // Remove exactly the single separator after the CLSID; a UNC path
            // keeps its two leading backslashes intact.
            while (!name.empty() && name.front() == L' ') name.erase(0, 1);
            // `::{GUID}\C:\path` has one separator; a UNC path adds two more,
            // so three leading backslashes mean separator + `\\server`.
            if (name.size() >= 3 && name[0] == L'\\' && name[1] == L'\\' &&
                name[2] == L'\\') {
                name.erase(0, 1);
            } else if (!(name.size() >= 2 && name[0] == L'\\' && name[1] == L'\\')) {
                if (!name.empty() && name.front() == L'\\') name.erase(0, 1);
            }
            while (!name.empty() && name.front() == L' ') name.erase(0, 1);
            if (name.empty()) {
                *ppidl = CreateRootPidl();
                return *ppidl ? S_OK : E_OUTOFMEMORY;
            }
        }
    }

    if (!IsAbsolutePath(name)) return E_INVALIDARG;
    if (pdwAttributes) {
        const DWORD attrs = ::GetFileAttributesW(name.c_str());
        if (attrs != INVALID_FILE_ATTRIBUTES) {
            *pdwAttributes &= (attrs & FILE_ATTRIBUTE_DIRECTORY) ? (SFGAO_FOLDER | SFGAO_FILESYSTEM)
                                                                 : SFGAO_FILESYSTEM;
        }
    }
    *ppidl = CreateItemPidl(name);
    if (!*ppidl) return E_OUTOFMEMORY;
    if (pchEaten) *pchEaten = static_cast<ULONG>(wcslen(pszDisplayName));
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CFolder::EnumObjects(HWND hwnd, DWORD grfFlags,
                                               IEnumIDList** ppenumIDList) {
    if (!ppenumIDList) return E_POINTER;
    *ppenumIDList = nullptr;

    if (!(grfFlags & (SHCONTF_FOLDERS | SHCONTF_NONFOLDERS))) return S_FALSE;

    const HWND frame = FrameFromEnumHwnd(hwnd);

    bool failed = false;
    size_t firstPage = 0;
    bool needFirstPage = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        LoadItemsLocked(frame);
        needFirstPage = m_items.empty() && !m_queryComplete && !m_failed;
    }
    if (needFirstPage) {
        LoadMore();
    }
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        failed = m_failed;
        firstPage = m_items.size();
    }

    ESX_TRACE_LOG("EnumObjects(flags=0x%lX, frame=%p) -> first page %zu paths (failed=%d)",
                  grfFlags, static_cast<void*>(frame), firstPage, failed ? 1 : 0);
    g_enumEnd = ::GetTickCount64();
    g_firstAttribute = 0;
    g_lastAttribute = 0;
    g_attributeCalls = 0;
    g_displayCalls = 0;
    ESX_TRACE_LOG("TIMING EnumObjects returned at=%llu", g_enumEnd);
    return CEnumIDList::Create(this, grfFlags, ppenumIDList);
}

HRESULT STDMETHODCALLTYPE CFolder::BindToObject(PCUIDLIST_RELATIVE, IBindCtx*, REFIID, void**) {
    // v1 is a flat, read-only result list. Folder results open through their
    // default verb (IContextMenu), not through shell navigation.
    return E_NOTIMPL;
}

HRESULT STDMETHODCALLTYPE CFolder::BindToStorage(PCUIDLIST_RELATIVE, IBindCtx*, REFIID, void**) {
    return E_NOTIMPL;
}

HRESULT STDMETHODCALLTYPE CFolder::CompareIDs(LPARAM lParam, PCUIDLIST_RELATIVE pidl1,
                                              PCUIDLIST_RELATIVE pidl2) {
    std::wstring path1;
    std::wstring path2;
    GetPidlPath(pidl1, path1);
    GetPidlPath(pidl2, path2);

    const UINT column = LOWORD(lParam);
    if (column == 1 || column == 3) {
        SortValue first;
        SortValue second;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            const Item* a = FindItemLocked(path1);
            const Item* b = FindItemLocked(path2);
            if (a) {
                first.size = a->size;
                first.modified = a->modified;
                first.folder = a->isFolder;
                first.valid = true;
            }
            if (b) {
                second.size = b->size;
                second.modified = b->modified;
                second.folder = b->isFolder;
                second.valid = true;
            }
        }
        // The sorting instance may have an empty result cache; use the cached
        // file attributes (or read them once) so these columns really sort.
        if (!first.valid) first = SortValueFor(path1);
        if (!second.valid) second = SortValueFor(path2);

        int cmp = 0;
        if (column == 1) {
            if (first.folder != second.folder) {
                cmp = first.folder ? -1 : 1;
            } else {
                cmp = (first.size < second.size) ? -1 : (first.size > second.size ? 1 : 0);
            }
        } else {
            cmp = static_cast<int>(::CompareFileTime(&first.modified, &second.modified));
        }
        if (cmp != 0) return MakeCompareResult(cmp);
    }

    return MakeCompareResult(
        ::StrCmpLogicalW(FileNameOf(path1).c_str(), FileNameOf(path2).c_str()));
}

HRESULT STDMETHODCALLTYPE CFolder::CreateViewObject(HWND, REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;

    if (!IsEqualIID(riid, IID_IShellView)) return E_NOINTERFACE;

    SFV_CREATE create{};
    create.cbSize = sizeof(create);
    create.pshf = static_cast<IShellFolder*>(this);
    create.psvOuter = nullptr;
    create.psfvcb = static_cast<IShellFolderViewCB*>(this);

    const HRESULT hr = ::SHCreateShellFolderView(&create, reinterpret_cast<IShellView**>(ppv));
    ESX_TRACE_LOG("CreateViewObject(IShellView) -> 0x%08lX", hr);
    return hr;
}

HRESULT STDMETHODCALLTYPE CFolder::GetAttributesOf(UINT cidl, PCUITEMID_CHILD_ARRAY apidl,
                                                   SFGAOF* rgfInOut) {
    if (!rgfInOut) return E_POINTER;
    const SFGAOF mask = *rgfInOut;

    if (cidl == 0 || !apidl) {
        ESX_TRACE_LOG("GetAttributesOf(root, mask=0x%lX)", mask);
        *rgfInOut = mask & (SFGAO_FOLDER | SFGAO_BROWSABLE | SFGAO_HASSUBFOLDER);
        return S_OK;
    }

    ESX_TRACE_LOG("GetAttributesOf(%u, mask=0x%lX)", cidl, mask);
    const ULONGLONG now = ::GetTickCount64();
    if (g_enumEnd && !g_firstAttribute) {
        g_firstAttribute = now;
        ESX_TRACE_LOG("TIMING first attribute after EnumObjects=%llu ms", now - g_enumEnd);
    }
    if (g_attributeCalls && g_lastAttribute && now - g_lastAttribute > 300) {
        ESX_TRACE_LOG("TIMING attribute burst ended span=%llu ms calls=%u idle=%llu",
                      g_lastAttribute - g_firstAttribute, g_attributeCalls,
                      now - g_lastAttribute);
        g_attributeCalls = 0;
        g_firstAttribute = 0;
    }
    ++g_attributeCalls;
    g_lastAttribute = now;
    SFGAOF result = mask;
    for (UINT i = 0; i < cidl; ++i) {
        std::wstring path;
        if (!GetPidlPath(apidl[i], path)) {
            result = 0;
            break;
        }

        bool cacheable = false;
        SFGAOF queryMask = mask;
        SFGAOF attributes = 0;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            const auto itemIt = m_index.find(path);
            if (itemIt != m_index.end()) {
                const Item& cached = m_items[itemIt->second];
                cacheable = true;

                const SFGAOF derived = AttributesFromFileFlags(cached.attributes) & mask;
                attributes |= derived;
                queryMask &= ~derived;

                const SFGAOF fromCache = cached.shellAttributesKnown & queryMask;
                attributes |= cached.shellAttributes & fromCache;
                queryMask &= ~fromCache;
            }
        }

        SFGAOF queried = 0;

        if (queryMask != 0) {
            // SHParseDisplayName here was the dominant cost: Explorer asks for
            // attributes thousands of times and each call built a PIDL and
            // consulted shell handlers. GetFileAttributesW is a cheap syscall,
            // and everything else is reported as known-false so Explorer never
            // asks again for the same item.
            const SFGAOF requested = queryMask;
            DWORD fileAttributes = ::GetFileAttributesW(path.c_str());
            SFGAOF fromFileSystem = 0;
            if (fileAttributes != INVALID_FILE_ATTRIBUTES) {
                fromFileSystem = AttributesFromFileFlags(fileAttributes) & requested;
            }
            queried = fromFileSystem;
            attributes |= queried;

            if (cacheable) {
                std::lock_guard<std::mutex> lock(m_mutex);
                const auto itemIt = m_index.find(path);
                if (itemIt != m_index.end()) {
                    Item& cached = m_items[itemIt->second];
                    cached.shellAttributes =
                        (cached.shellAttributes & ~requested) | queried;
                    cached.shellAttributesKnown |= requested & ~SFGAO_VALIDATE;
                }
            }
        }

        result &= attributes;
    }

    *rgfInOut = result;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CFolder::GetUIObjectOf(HWND, UINT cidl,
                                                 PCUITEMID_CHILD_ARRAY apidl, REFIID riid,
                                                 UINT*, void** ppv) {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    if (cidl == 0 || !apidl) return E_INVALIDARG;

    ESX_TRACE_LOG("GetUIObjectOf(cidl=%u)", cidl);

    // Items are real filesystem entries, so every heavy lifting operation is
    // delegated to the shell. No custom verbs, no custom drag source.
    std::vector<PIDLIST_ABSOLUTE> absolute(cidl, nullptr);
    std::vector<std::wstring> paths(cidl);
    for (UINT i = 0; i < cidl; ++i) {
        if (!GetPidlPath(apidl[i], paths[i])) return E_INVALIDARG;
        if (FAILED(::SHParseDisplayName(paths[i].c_str(), nullptr, &absolute[i], 0, nullptr))) {
            return E_FAIL;
        }
    }

    struct Cleanup {
        std::vector<PIDLIST_ABSOLUTE>& list;
        ~Cleanup() {
            for (auto* p : list) {
                if (p) ::CoTaskMemFree(p);
            }
        }
    } cleanup{absolute};

    if (IsEqualIID(riid, IID_IContextMenu) || IsEqualIID(riid, IID_IContextMenu2) ||
        IsEqualIID(riid, IID_IContextMenu3)) {
        ComPtr<IShellItemArray> items;
        HRESULT hr = ::SHCreateShellItemArrayFromIDLists(
            cidl, const_cast<PCIDLIST_ABSOLUTE_ARRAY>(absolute.data()), items.put());
        if (FAILED(hr) || !items) return FAILED(hr) ? hr : E_FAIL;

        ComPtr<IContextMenu> menu;
        hr = items->BindToHandler(nullptr, BHID_SFUIObject, IID_PPV_ARGS(menu.put()));
        if (FAILED(hr) || !menu) return FAILED(hr) ? hr : E_FAIL;

        hr = menu->QueryInterface(riid, ppv);
        ESX_TRACE_LOG("GetUIObjectOf(context menu) -> 0x%08lX", hr);
        return hr;
    }

    if (IsEqualIID(riid, IID_IDataObject)) {
        // A shell item array produces the same data object Explorer uses for
        // real files (CF_HDROP, Shell ID List Array, ...), including when the
        // selection spans different folders.
        ComPtr<IShellItemArray> array;
        if (SUCCEEDED(::SHCreateShellItemArrayFromIDLists(
                cidl, const_cast<PCIDLIST_ABSOLUTE_ARRAY>(absolute.data()), array.put())) &&
            array) {
            const HRESULT hr = array->BindToHandler(nullptr, BHID_DataObject, riid, ppv);
            ESX_TRACE_LOG("GetUIObjectOf(data object) -> 0x%08lX", hr);
            if (SUCCEEDED(hr)) return hr;
        }

        if (cidl == 1) {
            ComPtr<IShellItem> item;
            if (SUCCEEDED(::SHCreateItemFromIDList(absolute[0], IID_PPV_ARGS(item.put())))) {
                return item->BindToHandler(nullptr, BHID_DataObject, riid, ppv);
            }
        }
        return E_FAIL;
    }

    if (cidl == 1) {
        HRESULT hr = ::SHCreateItemFromIDList(absolute[0], riid, ppv);
        if (SUCCEEDED(hr)) return hr;

        ComPtr<IShellItem> item;
        if (SUCCEEDED(::SHCreateItemFromIDList(absolute[0], IID_PPV_ARGS(item.put())))) {
            return item->BindToHandler(nullptr, BHID_SFUIObject, riid, ppv);
        }
    }

    return E_NOINTERFACE;
}

HRESULT STDMETHODCALLTYPE CFolder::GetDisplayNameOf(PCUITEMID_CHILD pidl, SHGDNF uFlags,
                                                    STRRET* pName) {
    if (!pName) return E_POINTER;

    if (g_enumEnd && (++g_displayCalls % 500) == 0) {
        ESX_TRACE_LOG("TIMING GetDisplayNameOf calls=%u at=%llu ms after EnumObjects",
                      g_displayCalls, ::GetTickCount64() - g_enumEnd);
    }
    std::wstring path;
    if (!pidl || IsRootPidl(pidl) || !GetPidlPath(pidl, path)) {
        if (uFlags & SHGDN_FORPARSING) {
            wchar_t guidText[64]{};
            ::StringFromGUID2(CLSID_EverythingSearchFolder, guidText, ARRAYSIZE(guidText));
            ESX_TRACE_LOG("GetDisplayNameOf(root, flags=0x%lX)", uFlags);
            return StrDupToStrRet(std::wstring(L"::") + guidText, pName);
        }
        std::lock_guard<std::mutex> lock(m_mutex);
        const std::wstring name = DisplayNameLocked(uFlags);
        ESX_TRACE_LOG("GetDisplayNameOf(root, flags=0x%lX) -> '%ls'", uFlags, name.c_str());
        return StrDupToStrRet(name, pName);
    }

    ESX_TRACE_LOG("GetDisplayNameOf(%ls, flags=0x%lX)", path.c_str(), uFlags);
    if (uFlags & (SHGDN_FORPARSING | SHGDN_FORADDRESSBAR)) {
        return StrDupToStrRet(path, pName);
    }
    return StrDupToStrRet(FileNameOf(path), pName);
}

HRESULT STDMETHODCALLTYPE CFolder::SetNameOf(HWND, PCUITEMID_CHILD, LPCWSTR, SHGDNF,
                                             PITEMID_CHILD*) {
    return E_NOTIMPL;  // read-only view
}

// ----------------------------------------------------------- IShellFolder2

HRESULT STDMETHODCALLTYPE CFolder::GetDefaultSearchGUID(GUID*) {
    return E_NOTIMPL;
}

HRESULT STDMETHODCALLTYPE CFolder::EnumSearches(IEnumExtraSearch**) {
    return E_NOTIMPL;
}

HRESULT STDMETHODCALLTYPE CFolder::GetDefaultColumn(DWORD, ULONG* pSort, ULONG* pDisplay) {
    if (pSort) *pSort = 0;
    if (pDisplay) *pDisplay = 0;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CFolder::GetDefaultColumnState(UINT iColumn, SHCOLSTATEF* pcsFlags) {
    if (!pcsFlags) return E_POINTER;
    switch (iColumn) {
        case 0:
            *pcsFlags = SHCOLSTATE_TYPE_STR | SHCOLSTATE_ONBYDEFAULT;
            return S_OK;
        case 1:
            *pcsFlags = SHCOLSTATE_TYPE_INT | SHCOLSTATE_ONBYDEFAULT | SHCOLSTATE_SLOW;
            return S_OK;
        case 2:
            *pcsFlags = SHCOLSTATE_TYPE_STR | SHCOLSTATE_ONBYDEFAULT;
            return S_OK;
        case 3:
            *pcsFlags = SHCOLSTATE_TYPE_DATE | SHCOLSTATE_ONBYDEFAULT;
            return S_OK;
        case 4:
            *pcsFlags = SHCOLSTATE_TYPE_STR | SHCOLSTATE_ONBYDEFAULT;
            return S_OK;
        default:
            return E_INVALIDARG;
    }
}

HRESULT STDMETHODCALLTYPE CFolder::GetDetailsEx(PCUITEMID_CHILD pidl, const SHCOLUMNID* pscid,
                                                VARIANT* pv) {
    if (!pv) return E_POINTER;
    ::VariantInit(pv);
    if (!pidl || !pscid) return E_INVALIDARG;

    std::wstring path;
    if (!GetPidlPath(pidl, path)) return E_INVALIDARG;

    // The details may be requested on a CFolder instance that never loaded the
    // results, so fall back to the real file attributes when the cache is empty.
    bool cached = false;
    bool folder = false;
    DWORD attributes = 0;
    std::uint64_t size = 0;
    FILETIME modified{};
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const Item* item = FindItemLocked(path);
        if (item) {
            cached = true;
            folder = item->isFolder;
            attributes = item->attributes;
            size = item->size;
            modified = item->modified;
        }
    }
    if (!cached) GetFileInfoForColumn(path, size, modified, folder);

    auto toVariant = [pv](HRESULT hr, PROPVARIANT& prop) -> HRESULT {
        if (FAILED(hr)) return hr;
        const HRESULT converted = ::PropVariantToVariant(&prop, pv);
        ::PropVariantClear(&prop);
        return converted;
    };

    if (IsEqualPropertyKey(*pscid, PKEY_ItemNameDisplay)) {
        PROPVARIANT prop;
        ::PropVariantInit(&prop);
        return toVariant(::InitPropVariantFromString(FileNameOf(path).c_str(), &prop), prop);
    }
    if (IsEqualPropertyKey(*pscid, PKEY_Size)) {
        if (folder) return S_FALSE;
        PROPVARIANT prop;
        ::PropVariantInit(&prop);
        return toVariant(::InitPropVariantFromUInt64(static_cast<ULONGLONG>(size), &prop), prop);
    }
    if (IsEqualPropertyKey(*pscid, PKEY_DateModified)) {
        if (!modified.dwLowDateTime && !modified.dwHighDateTime) return S_FALSE;
        PROPVARIANT prop;
        ::PropVariantInit(&prop);
        return toVariant(::InitPropVariantFromFileTime(&modified, &prop), prop);
    }
    if (IsEqualPropertyKey(*pscid, PKEY_ItemPathDisplay)) {
        PROPVARIANT prop;
        ::PropVariantInit(&prop);
        return toVariant(
            ::InitPropVariantFromString(ParentDirectoryOf(path).c_str(), &prop), prop);
    }
    if (IsEqualPropertyKey(*pscid, PKEY_ItemTypeText)) {
        std::wstring text = TypeNameOf(path.c_str(), attributes);
        if (text.empty()) return S_FALSE;
        PROPVARIANT prop;
        ::PropVariantInit(&prop);
        return toVariant(::InitPropVariantFromString(text.c_str(), &prop), prop);
    }

    PIDLIST_ABSOLUTE absolute = nullptr;
    if (FAILED(::SHParseDisplayName(path.c_str(), nullptr, &absolute, 0, nullptr)) || !absolute) {
        return E_FAIL;
    }
    ComPtr<IShellItem2> shellItem;
    const HRESULT hr = ::SHCreateItemFromIDList(absolute, IID_PPV_ARGS(shellItem.put()));
    ::CoTaskMemFree(absolute);
    if (FAILED(hr) || !shellItem) return E_FAIL;

    // GetProperty speaks PROPVARIANT; the shell contract speaks VARIANT.
    PROPVARIANT property;
    ::PropVariantInit(&property);
    const HRESULT getHr = shellItem->GetProperty(*pscid, &property);
    if (SUCCEEDED(getHr)) {
        const HRESULT copyHr = ::PropVariantToVariant(&property, pv);
        ::PropVariantClear(&property);
        return copyHr;
    }
    ::PropVariantClear(&property);
    return getHr;
}

HRESULT STDMETHODCALLTYPE CFolder::GetDetailsOf(PCUITEMID_CHILD pidl, UINT iColumn,
                                                SHELLDETAILS* psd) {
    if (!psd) return E_POINTER;
    psd->str.uType = STRRET_WSTR;
    psd->str.pOleStr = nullptr;

    ESX_TRACE_LOG("GetDetailsOf(column=%u pidl=%p)", iColumn, pidl);

    std::wstring path;
    if (!pidl) {
        // The shell enumerates headers by increasing iColumn until it gets an
        // error. S_FALSE is still a success, so it would loop forever: this is
        // the documented E_INVALIDARG-on-out-of-range contract.
        if (iColumn > 4) return E_INVALIDARG;
        const wchar_t* fallback = L"";
        switch (iColumn) {
            case 0: fallback = L"Name"; break;
            case 1: fallback = L"Size"; break;
            case 2: fallback = L"Type"; break;
            case 3: fallback = L"Date modified"; break;
            case 4: fallback = L"Folder path"; break;
        }
        const std::wstring name = [iColumn, fallback]() -> std::wstring {
            switch (iColumn) {
                case 0: return LocalizedColumnName(PKEY_ItemNameDisplay, fallback);
                case 1: return LocalizedColumnName(PKEY_Size, fallback);
                case 2: return LocalizedColumnName(PKEY_ItemTypeText, fallback);
                case 3: return LocalizedColumnName(PKEY_DateModified, fallback);
                default: return LocalizedColumnName(PKEY_ItemPathDisplay, fallback);
            }
        }();
        psd->fmt = (iColumn == 1) ? LVCFMT_RIGHT : LVCFMT_LEFT;
        psd->cxChar = (iColumn == 1) ? 12 : 20;
        return ::SHStrDupW(name.c_str(), &psd->str.pOleStr);
    }

    if (!GetPidlPath(pidl, path)) return E_INVALIDARG;

    DWORD cachedAttributes = 0;
    std::uint64_t cachedSize = 0;
    FILETIME cachedModified{};
    bool cached = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const Item* item = FindItemLocked(path);
        if (item) {
            cachedAttributes = item->attributes;
            cachedSize = item->size;
            cachedModified = item->modified;
            cached = true;
        }
    }

    psd->fmt = (iColumn == 1) ? LVCFMT_RIGHT : LVCFMT_LEFT;
    psd->cxChar = (iColumn == 1) ? 12 : 20;

    switch (iColumn) {
        case 0:
            return ::SHStrDupW(FileNameOf(path).c_str(), &psd->str.pOleStr);
        case 1: {
            std::uint64_t size = cached ? cachedSize : 0;
            FILETIME modified{};
            bool folder = false;
            if (!cached && GetFileInfoForColumn(path, size, modified, folder)) {
                // Folder sizes stay blank, like in Explorer.
                if (folder) size = 0;
            }
            const std::wstring text = size ? FormatBytes(size) : std::wstring();
            return ::SHStrDupW(text.c_str(), &psd->str.pOleStr);
        }
        case 2: {
            const std::wstring text = TypeNameOf(path, cachedAttributes);
            return ::SHStrDupW(text.c_str(), &psd->str.pOleStr);
        }
        case 3: {
            FILETIME modified = cached ? cachedModified : FILETIME{};
            bool folder = false;
            std::uint64_t size = 0;
            if (!cached) GetFileInfoForColumn(path, size, modified, folder);
            const std::wstring text = FormatFileTime(modified);
            return ::SHStrDupW(text.c_str(), &psd->str.pOleStr);
        }
        case 4:
            return ::SHStrDupW(ParentDirectoryOf(path).c_str(), &psd->str.pOleStr);
        default:
            return E_INVALIDARG;
    }
}

HRESULT STDMETHODCALLTYPE CFolder::MapColumnToSCID(UINT iColumn, SHCOLUMNID* pscid) {
    if (!pscid) return E_POINTER;
    switch (iColumn) {
        case 0:
            *pscid = PKEY_ItemNameDisplay;
            return S_OK;
        case 1:
            *pscid = PKEY_Size;
            return S_OK;
        case 2:
            *pscid = PKEY_ItemTypeText;
            return S_OK;
        case 3:
            *pscid = PKEY_DateModified;
            return S_OK;
        case 4:
            *pscid = PKEY_ItemPathDisplay;
            return S_OK;
        default:
            return E_INVALIDARG;
    }
}

// ------------------------------------------------------- IShellFolderViewCB

HRESULT STDMETHODCALLTYPE CFolder::MessageSFVCB(UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case SFVM_WINDOWCREATED: {
            m_viewHwnd = reinterpret_cast<HWND>(wParam);
            ESX_TRACE_LOG("SFVM_WINDOWCREATED hwnd=%p", static_cast<void*>(m_viewHwnd));
            return E_NOTIMPL;
        }
        case SFVM_DEFVIEWMODE: {
            auto* mode = reinterpret_cast<FOLDERVIEWMODE*>(lParam);
            if (!mode) return E_INVALIDARG;
            *mode = FVM_DETAILS;
            ESX_TRACE_LOG("SFVM_DEFVIEWMODE -> FVM_DETAILS");
            return S_OK;
        }
        case SFVM_DEFITEMCOUNT: {
            auto* count = reinterpret_cast<UINT*>(lParam);
            if (!count) return E_INVALIDARG;
            bool needFirstPage = false;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                LoadItemsLocked();
                needFirstPage = m_items.empty() && !m_queryComplete && !m_failed;
            }
            if (needFirstPage) {
                LoadMore();
            }
            std::lock_guard<std::mutex> lock(m_mutex);
            *count = static_cast<UINT>(m_items.size());
            return S_OK;
        }
        default:
            // E_NOTIMPL lets DefView do its default processing, which includes
            // re-enumerating on IShellView::Refresh().
            return E_NOTIMPL;
    }
}

HRESULT STDMETHODCALLTYPE CFolder::QueryService(REFGUID guidService, REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    if (IsEqualGUID(guidService, SID_ExplorerPaneVisibility) &&
        IsEqualIID(riid, IID_IExplorerPaneVisibility)) {
        ESX_TRACE_LOG("QueryService(ExplorerPaneVisibility)");
        return QueryInterface(IID_IExplorerPaneVisibility, ppv);
    }
    return E_NOINTERFACE;
}

HRESULT STDMETHODCALLTYPE CFolder::GetPaneState(REFEXPLORERPANE ep, EXPLORERPANESTATE* peps) {
    if (!peps) return E_POINTER;
    *peps = EPS_DONTCARE;
    if (IsEqualGUID(ep, kPaneCommands) || IsEqualGUID(ep, kPaneCommandsOrganize) ||
        IsEqualGUID(ep, kPaneCommandsView)) {
        ESX_TRACE_LOG("GetPaneState this=%p ep=%08lX -> off", static_cast<void*>(this),
                      static_cast<unsigned long>(ep.Data1));
        *peps = EPS_DEFAULT_OFF | EPS_FORCE;
    }
    return S_OK;
}

// ============================================================ CEnumIDList

HRESULT CEnumIDList::Create(CFolder* folder, DWORD flags, IEnumIDList** ppv) {
    if (!ppv || !folder) return E_POINTER;
    *ppv = nullptr;

    auto* enumerator = new (std::nothrow) CEnumIDList(folder, flags);
    if (!enumerator) return E_OUTOFMEMORY;

    ::InterlockedIncrement(&g_cRefDll);
    *ppv = enumerator;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CEnumIDList::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    if (!IsEqualIID(riid, IID_IUnknown) && !IsEqualIID(riid, IID_IEnumIDList)) {
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    *ppv = static_cast<IEnumIDList*>(this);
    AddRef();
    return S_OK;
}

ULONG STDMETHODCALLTYPE CEnumIDList::AddRef() {
    return static_cast<ULONG>(::InterlockedIncrement(&m_ref));
}

ULONG STDMETHODCALLTYPE CEnumIDList::Release() {
    const LONG ref = ::InterlockedDecrement(&m_ref);
    if (ref == 0) {
        ::InterlockedDecrement(&g_cRefDll);
        delete this;
    }
    return static_cast<ULONG>(ref);
}

HRESULT STDMETHODCALLTYPE CEnumIDList::Next(ULONG celt, PITEMID_CHILD* rgelt,
                                            ULONG* pceltFetched) {
    if (!rgelt) return E_POINTER;
    if (pceltFetched) *pceltFetched = 0;
    if (m_folder->Generation() != m_generation) return S_FALSE;

    ULONG fetched = 0;
    while (fetched < celt) {
        if (m_folder->Generation() != m_generation) break;
        if (!m_folder->EnsureLoaded(m_index)) break;
        std::wstring path;
        if (!m_folder->GetPathAt(m_index, path)) break;
        if (!m_folder->IsIncluded(m_index, m_flags)) {
            ++m_index;
            continue;
        }
        PIDLIST_ABSOLUTE pidl = CreateItemPidl(path);
        if (!pidl) break;
        rgelt[fetched++] = pidl;
        ++m_index;
    }

    if (pceltFetched) *pceltFetched = fetched;
    return fetched == celt ? S_OK : S_FALSE;
}

HRESULT STDMETHODCALLTYPE CEnumIDList::Skip(ULONG celt) {
    if (m_folder->Generation() != m_generation) return S_FALSE;
    ULONG skipped = 0;
    while (skipped < celt) {
        if (m_folder->Generation() != m_generation) return S_FALSE;
        if (!m_folder->EnsureLoaded(m_index)) return S_FALSE;
        std::wstring path;
        if (!m_folder->GetPathAt(m_index, path)) return S_FALSE;
        if (m_folder->IsIncluded(m_index, m_flags)) ++skipped;
        ++m_index;
    }
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CEnumIDList::Reset() {
    m_index = 0;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE CEnumIDList::Clone(IEnumIDList** ppenum) {
    if (!ppenum) return E_POINTER;
    *ppenum = nullptr;

    auto* clone = new (std::nothrow) CEnumIDList(m_folder, m_flags);
    if (!clone) return E_OUTOFMEMORY;
    clone->m_index = m_index;

    ::InterlockedIncrement(&g_cRefDll);
    *ppenum = clone;
    return S_OK;
}
