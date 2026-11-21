#include "nse.h"

#include <propvarutil.h>

#include "bridge.h"
#include "search_actions.h"

namespace {

// Timing of Explorer's shell-view construction, measured from EnumObjects.
ULONGLONG g_enumEnd = 0;
ULONGLONG g_firstAttribute = 0;
ULONGLONG g_lastAttribute = 0;
DWORD g_attributeCalls = 0;
DWORD g_displayCalls = 0;

HRESULT MakeCompareResult(int cmp) {
    return MAKE_HRESULT(SEVERITY_SUCCESS, 0, static_cast<WORD>(static_cast<SHORT>(cmp)));
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

std::wstring CFolder::DisplayNameLocked() const {
    std::wstring name = kFolderFriendlyName;
    if (m_failed) {
        name += L" - error";
    } else if (m_loaded) {
        name += L" - ";
        name += std::to_wstring(m_items.size());
        name += (m_items.size() == 1) ? L" result" : L" results";
    }
    return name;
}

void CFolder::AppendPageLocked(const std::vector<esx::QueryResultItem>& page) {
    m_items.reserve(m_items.size() + page.size());
    for (const auto& r : page) {
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

void CFolder::LoadMoreLocked() {
    if (m_queryComplete) return;
    const DWORD wanted = std::min<DWORD>(bridge::kBatchSize, m_limit - m_nextOffset);
    if (wanted == 0) {
        m_queryComplete = true;
        return;
    }

    std::vector<esx::QueryResultItem> page;
    DWORD raw = 0;
    DWORD total = 0;
    std::string error;
    if (!bridge::QueryPage(m_search, m_nextOffset, m_flags, wanted, page, raw, total,
                           error)) {
        m_failed = true;
        m_queryComplete = true;
        ::OutputDebugStringA(("[esx] " + error + "\n").c_str());
        return;
    }

    AppendPageLocked(page);
    m_nextOffset += raw;
    m_totalCount = total;
    if (raw < wanted || total <= m_nextOffset || m_items.size() >= m_limit) {
        m_queryComplete = true;
    }
}

void CFolder::LoadItemsLocked() {
    const ULONGLONG tStart = ::GetTickCount64();
    esx::QueryRequest request;
    if (!esx::ReadRequestFile(request)) {
        ClearItems();
        m_loaded = false;
        m_failed = false;
        m_seq = 0;
        m_query.clear();
        m_folder.clear();
        return;
    }

    // Everything view refreshes re-enumerate constantly; skip the round trip
    // when the request did not change.
    if (m_loaded && request.seq == m_seq && request.query == m_query &&
        request.folder == m_folder && request.maxResults == m_maxResults &&
        request.flags == m_flags) {
        return;
    }

    ClearItems();
    m_generation.fetch_add(1, std::memory_order_relaxed);
    m_loaded = true;
    m_failed = false;
    m_seq = request.seq;
    m_query = request.query;
    m_folder = request.folder;
    m_maxResults = request.maxResults;
    m_flags = request.flags;

    m_search = bridge::BuildSearchString(request);
    m_limit = (request.maxResults && request.maxResults < bridge::kSafetyCeiling)
                  ? request.maxResults
                  : bridge::kSafetyCeiling;
    m_nextOffset = 0;
    m_totalCount = 0;
    m_queryComplete = false;

    // Only the first page is fetched here. The rest is loaded by
    // EnsureLoaded() as Explorer walks IEnumIDList::Next, so the view can
    // start drawing instead of staying on the old folder.
    if (!bridge::EnsureReady()) {
        m_failed = true;
        m_queryComplete = true;
        return;
    }
    LoadMoreLocked();
    ESX_TRACE_LOG("TIMING LoadItems first page=%llu ms items=%zu total=%u",
                  ::GetTickCount64() - tStart, m_items.size(), m_totalCount);
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
        std::lock_guard<std::mutex> lock(m_mutex);
        LoadMoreLocked();
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
    while (!name.empty() && (name.front() == L' ' || name.front() == L'\\')) name.erase(0, 1);

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
            while (!name.empty() && (name.front() == L'\\' || name.front() == L' ')) name.erase(0, 1);
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

HRESULT STDMETHODCALLTYPE CFolder::EnumObjects(HWND, DWORD grfFlags, IEnumIDList** ppenumIDList) {
    if (!ppenumIDList) return E_POINTER;
    *ppenumIDList = nullptr;

    if (!(grfFlags & (SHCONTF_FOLDERS | SHCONTF_NONFOLDERS))) return S_FALSE;

    bool failed = false;
    size_t firstPage = 0;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        LoadItemsLocked();
        failed = m_failed;
        firstPage = m_items.size();
    }

    ESX_TRACE_LOG("EnumObjects(flags=0x%lX) -> first page %zu paths (failed=%d)",
                  grfFlags, firstPage, failed ? 1 : 0);
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
        std::lock_guard<std::mutex> lock(m_mutex);
        const Item* a = FindItemLocked(path1);
        const Item* b = FindItemLocked(path2);
        if (a && b) {
            int cmp = 0;
            if (column == 1) {
                cmp = (a->size < b->size) ? -1 : (a->size > b->size ? 1 : 0);
            } else {
                cmp = static_cast<int>(::CompareFileTime(&a->modified, &b->modified));
            }
            if (cmp != 0) return MakeCompareResult(cmp);
        }
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

        const bool validate = (mask & SFGAO_VALIDATE) != 0;
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

        HRESULT hr = S_OK;
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

        if (FAILED(hr)) {
            ESX_TRACE_LOG("GetAttributesOf(%ls) failed: 0x%08lX", path.c_str(), hr);
            result = 0;
            break;
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
        ESX_TRACE_LOG("GetDisplayNameOf(root, flags=0x%lX)", uFlags);
        return StrDupToStrRet(DisplayNameLocked(), pName);
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

    // Content view asks for a property key per cell. Creating a shell item and
    // querying the property system for every row is far too slow on large
    // result lists, so serve our four columns from the cached Everything data.
    // Unknown keys still fall back to the real item below.
    const Item* item = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        item = FindItemLocked(path);
    }

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
        if (!item || item->isFolder) return S_FALSE;
        PROPVARIANT prop;
        ::PropVariantInit(&prop);
        return toVariant(::InitPropVariantFromUInt64(static_cast<ULONGLONG>(item->size), &prop), prop);
    }
    if (IsEqualPropertyKey(*pscid, PKEY_DateModified)) {
        if (!item || (!item->modified.dwLowDateTime && !item->modified.dwHighDateTime)) {
            return S_FALSE;
        }
        FILETIME modified = item->modified;
        PROPVARIANT prop;
        ::PropVariantInit(&prop);
        return toVariant(::InitPropVariantFromFileTime(&modified, &prop), prop);
    }
    if (IsEqualPropertyKey(*pscid, PKEY_ItemTypeText)) {
        const std::wstring type = TypeNameOf(path.c_str(), item ? item->attributes : 0);
        if (type.empty()) return S_FALSE;
        PROPVARIANT prop;
        ::PropVariantInit(&prop);
        return toVariant(::InitPropVariantFromString(type.c_str(), &prop), prop);
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
        static const wchar_t* kHeaders[] = {L"Name", L"Size", L"Type", L"Date modified"};
        // The shell enumerates headers by increasing iColumn until it gets an
        // error. S_FALSE is still a success, so it would loop forever: this is
        // the documented E_INVALIDARG-on-out-of-range contract.
        if (iColumn >= ARRAYSIZE(kHeaders)) return E_INVALIDARG;
        psd->fmt = (iColumn == 1) ? LVCFMT_RIGHT : LVCFMT_LEFT;
        psd->cxChar = (iColumn == 1) ? 12 : 20;
        return ::SHStrDupW(kHeaders[iColumn], &psd->str.pOleStr);
    }

    if (!GetPidlPath(pidl, path)) return E_INVALIDARG;

    std::lock_guard<std::mutex> lock(m_mutex);
    const Item* item = FindItemLocked(path);

    psd->fmt = (iColumn == 1) ? LVCFMT_RIGHT : LVCFMT_LEFT;
    psd->cxChar = (iColumn == 1) ? 12 : 20;

    switch (iColumn) {
        case 0:
            return ::SHStrDupW(FileNameOf(path).c_str(), &psd->str.pOleStr);
        case 1: {
            const std::wstring text = item ? FormatBytes(item->size) : std::wstring();
            return ::SHStrDupW(text.c_str(), &psd->str.pOleStr);
        }
        case 2: {
            const std::wstring text = TypeNameOf(path, item ? item->attributes : 0);
            return ::SHStrDupW(text.c_str(), &psd->str.pOleStr);
        }
        case 3: {
            const std::wstring text = item ? FormatFileTime(item->modified) : std::wstring();
            return ::SHStrDupW(text.c_str(), &psd->str.pOleStr);
        }
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
        default:
            return E_INVALIDARG;
    }
}

// ------------------------------------------------------- IShellFolderViewCB

HRESULT STDMETHODCALLTYPE CFolder::MessageSFVCB(UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case SFVM_WINDOWCREATED: {
            // Keep the view HWND to parent the two search dialogs and to find
            // the owning IShellBrowser when one of the commands is invoked.
            m_viewHwnd = reinterpret_cast<HWND>(wParam);
            ESX_TRACE_LOG("SFVM_WINDOWCREATED hwnd=%p", static_cast<void*>(m_viewHwnd));
            return E_NOTIMPL;
        }
        case SFVM_MERGEMENU: {
            // Merge into the real DefView menu instead of replacing it: all of
            // Explorer's own commands (view, sort, refresh, paste...) stay.
            auto* info = reinterpret_cast<LPQCMINFO>(lParam);
            if (!info || !info->hmenu) return E_INVALIDARG;

            m_cmdBase = info->idCmdFirst;
            ::InsertMenuW(info->hmenu, info->indexMenu, MF_BYPOSITION | MF_STRING, m_cmdBase,
                          L"Search options...");
            ::InsertMenuW(info->hmenu, info->indexMenu + 1, MF_BYPOSITION | MF_STRING,
                          m_cmdBase + 1, L"Close search");
            info->indexMenu += 2;
            info->idCmdFirst += 2;
            ESX_TRACE_LOG("SFVM_MERGEMENU base=%u", m_cmdBase);
            return S_OK;
        }
        case SFVM_INVOKECOMMAND: {
            const UINT id = LOWORD(wParam);
            if (id == m_cmdBase) {
                EsxShowSearchOptions(m_viewHwnd ? m_viewHwnd : ::GetForegroundWindow());
                return S_OK;
            }
            if (id == m_cmdBase + 1) {
                EsxCloseSearch(m_viewHwnd ? m_viewHwnd : ::GetForegroundWindow());
                return S_OK;
            }
            return E_NOTIMPL;
        }
        case SFVM_DEFVIEWMODE: {
            // The results are files the user wants to inspect, so the default
            // view is "Content" (thumbnail + type, size and date).
            auto* mode = reinterpret_cast<FOLDERVIEWMODE*>(lParam);
            if (!mode) return E_INVALIDARG;
            *mode = FVM_CONTENT;
            ESX_TRACE_LOG("SFVM_DEFVIEWMODE -> FVM_CONTENT");
            return S_OK;
        }
        case SFVM_DEFITEMCOUNT: {
            auto* count = reinterpret_cast<UINT*>(lParam);
            if (!count) return E_INVALIDARG;
            std::lock_guard<std::mutex> lock(m_mutex);
            LoadItemsLocked();
            *count = static_cast<UINT>(m_items.size());
            return S_OK;
        }
        default:
            // E_NOTIMPL lets DefView do its default processing, which includes
            // re-enumerating on IShellView::Refresh().
            return E_NOTIMPL;
    }
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
    SyncGeneration();

    ULONG fetched = 0;
    while (fetched < celt) {
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
    SyncGeneration();
    ULONG skipped = 0;
    while (skipped < celt) {
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
