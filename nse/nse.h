#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shlwapi.h>
#include <shellapi.h>
#include <propkey.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "trace.h"
#include "../shared/protocol.h"

// {7B39C2A2-3E1F-4E4C-9E58-2D9A7B1C4F60}
inline constexpr GUID CLSID_EverythingSearchFolder = {
    0x7b39c2a2, 0x3e1f, 0x4e4c, {0x9e, 0x58, 0x2d, 0x9a, 0x7b, 0x1c, 0x4f, 0x60}};

inline constexpr wchar_t kFolderFriendlyName[] = L"Everything Search";

extern HINSTANCE g_hInst;
extern LONG g_cRefDll;

// Avoids differences between SDK/mingw headers around PathIsAbsoluteW.
inline bool IsAbsolutePath(const std::wstring& path) {
    if (path.size() >= 2 && path[1] == L':') {
        const wchar_t c = path[0];
        if ((c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z')) return true;
    }
    return path.size() >= 2 && path[0] == L'\\' && path[1] == L'\\';
}

// ---------------------------------------------------------------- ComPtr

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

    void Attach(T* p) {
        Reset();
        p_ = p;
    }

    T* Detach() {
        T* p = p_;
        p_ = nullptr;
        return p;
    }

    HRESULT As(REFIID riid, void** out) const {
        return p_ ? p_->QueryInterface(riid, out) : E_NOINTERFACE;
    }

private:
    T* p_ = nullptr;
};

// ------------------------------------------------------------- root PIDL

// A PIDL is a sequence of SHITEMIDs terminated by a zero-length one. Our root
// item carries a magic value so we can recognize it. Child items append a
// second item that carries the real filesystem path.
#pragma pack(push, 1)
struct RootItemId {
    USHORT cb;
    WCHAR magic[2];
};
#pragma pack(pop)

inline constexpr USHORT kRootItemCb = sizeof(RootItemId);  // 6

PIDLIST_ABSOLUTE CreateRootPidl();
PIDLIST_ABSOLUTE CreateItemPidl(const std::wstring& path);

// True only when the PIDL is the bare root (root item + terminator).
bool IsRootPidl(PCUIDLIST_ABSOLUTE pidl);
// True when the PIDL starts with our root item (root or child).
bool IsOurPidl(PCUIDLIST_ABSOLUTE pidl);
// Extracts the filesystem path from a child PIDL. False for the root.
bool GetPidlPath(PCUIDLIST_ABSOLUTE pidl, std::wstring& path);

// ------------------------------------------------------------------ folder

class CFolder final : public IShellFolder2,
                      public IPersistFolder2,
                      public IShellFolderViewCB {
public:
    static HRESULT Create(REFIID riid, void** ppv);

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;

    // IPersist
    HRESULT STDMETHODCALLTYPE GetClassID(CLSID* pclsid) override;

    // IPersistFolder / IPersistFolder2
    HRESULT STDMETHODCALLTYPE Initialize(PCIDLIST_ABSOLUTE pidl) override;
    HRESULT STDMETHODCALLTYPE GetCurFolder(PIDLIST_ABSOLUTE* ppidl) override;

    // IShellFolder
    HRESULT STDMETHODCALLTYPE ParseDisplayName(HWND hwnd, IBindCtx* pbc, LPWSTR pszDisplayName,
                                               ULONG* pchEaten, PIDLIST_RELATIVE* ppidl,
                                               ULONG* pdwAttributes) override;
    HRESULT STDMETHODCALLTYPE EnumObjects(HWND hwnd, DWORD grfFlags,
                                          IEnumIDList** ppenumIDList) override;
    HRESULT STDMETHODCALLTYPE BindToObject(PCUIDLIST_RELATIVE pidl, IBindCtx* pbc, REFIID riid,
                                           void** ppv) override;
    HRESULT STDMETHODCALLTYPE BindToStorage(PCUIDLIST_RELATIVE pidl, IBindCtx* pbc, REFIID riid,
                                            void** ppv) override;
    HRESULT STDMETHODCALLTYPE CompareIDs(LPARAM lParam, PCUIDLIST_RELATIVE pidl1,
                                         PCUIDLIST_RELATIVE pidl2) override;
    HRESULT STDMETHODCALLTYPE CreateViewObject(HWND hwndOwner, REFIID riid, void** ppv) override;
    HRESULT STDMETHODCALLTYPE GetAttributesOf(UINT cidl, PCUITEMID_CHILD_ARRAY apidl,
                                              SFGAOF* rgfInOut) override;
    HRESULT STDMETHODCALLTYPE GetUIObjectOf(HWND hwndOwner, UINT cidl,
                                            PCUITEMID_CHILD_ARRAY apidl, REFIID riid,
                                            UINT* rgfReserved, void** ppv) override;
    HRESULT STDMETHODCALLTYPE GetDisplayNameOf(PCUITEMID_CHILD pidl, SHGDNF uFlags,
                                               STRRET* pName) override;
    HRESULT STDMETHODCALLTYPE SetNameOf(HWND hwnd, PCUITEMID_CHILD pidl, LPCWSTR pszName,
                                        SHGDNF uFlags, PITEMID_CHILD* ppidlOut) override;

    // IShellFolder2
    HRESULT STDMETHODCALLTYPE GetDefaultSearchGUID(GUID* pguid) override;
    HRESULT STDMETHODCALLTYPE EnumSearches(IEnumExtraSearch** ppenum) override;
    HRESULT STDMETHODCALLTYPE GetDefaultColumn(DWORD dwReserved, ULONG* pSort,
                                               ULONG* pDisplay) override;
    HRESULT STDMETHODCALLTYPE GetDefaultColumnState(UINT iColumn, SHCOLSTATEF* pcsFlags) override;
    HRESULT STDMETHODCALLTYPE GetDetailsEx(PCUITEMID_CHILD pidl, const SHCOLUMNID* pscid,
                                           VARIANT* pv) override;
    HRESULT STDMETHODCALLTYPE GetDetailsOf(PCUITEMID_CHILD pidl, UINT iColumn,
                                           SHELLDETAILS* psd) override;
    HRESULT STDMETHODCALLTYPE MapColumnToSCID(UINT iColumn, SHCOLUMNID* pscid) override;

    // IShellFolderViewCB
    HRESULT STDMETHODCALLTYPE MessageSFVCB(UINT uMsg, WPARAM wParam, LPARAM lParam) override;

    // Lazy enumeration: the first page is loaded by EnumObjects, the rest is
    // fetched on demand while Explorer walks the enumerator.
    bool GetPathAt(size_t index, std::wstring& path) const;
    bool EnsureLoaded(size_t index);
    bool IsIncluded(size_t index, DWORD flags) const;
    std::uint64_t Generation() const { return m_generation.load(); }

private:
    CFolder() = default;
    ~CFolder();

    struct Item {
        std::wstring path;
        std::wstring name;
        std::uint64_t size = 0;
        FILETIME modified{};
        std::uint32_t attributes = 0;
        bool isFolder = false;
        SFGAOF shellAttributes = 0;
        SFGAOF shellAttributesKnown = 0;
        PIDLIST_ABSOLUTE pidl = nullptr;
    };

    void ClearItems();
    void LoadItemsLocked();
    void LoadMoreLocked();       // m_mutex must be held
    void AppendPageLocked(const std::vector<esx::QueryResultItem>& page);
    const Item* FindItemLocked(const std::wstring& path) const;
    std::wstring DisplayNameLocked() const;

    LONG m_ref = 1;
    PIDLIST_ABSOLUTE m_rootPidl = nullptr;

    mutable std::mutex m_mutex;
    std::atomic<std::uint64_t> m_generation{0};
    std::vector<Item> m_items;
    std::unordered_map<std::wstring, size_t> m_index;
    bool m_loaded = false;
    bool m_failed = false;
    bool m_queryComplete = false;
    std::wstring m_search;
    DWORD m_nextOffset = 0;
    DWORD m_totalCount = 0;
    std::uint32_t m_limit = 0;
    std::uint32_t m_seq = 0;
    std::uint32_t m_maxResults = 0;
    std::uint32_t m_flags = 0;
    std::wstring m_query;
    std::wstring m_folder;

    // Commands injected into the view's background menu (SFVM_MERGEMENU).
    UINT m_cmdBase = 0;
    HWND m_viewHwnd = nullptr;
};

// ------------------------------------------------------------- enumerator

class CEnumIDList final : public IEnumIDList {
public:
    static HRESULT Create(CFolder* folder, DWORD flags, IEnumIDList** ppv);

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;

    HRESULT STDMETHODCALLTYPE Next(ULONG celt, PITEMID_CHILD* rgelt, ULONG* pceltFetched) override;
    HRESULT STDMETHODCALLTYPE Skip(ULONG celt) override;
    HRESULT STDMETHODCALLTYPE Reset() override;
    HRESULT STDMETHODCALLTYPE Clone(IEnumIDList** ppenum) override;

private:
    explicit CEnumIDList(CFolder* folder, DWORD flags)
        : m_folder(folder), m_flags(flags), m_generation(folder->Generation()) {
        m_folder->AddRef();
    }
    ~CEnumIDList() { m_folder->Release(); }

    void SyncGeneration() {
        const std::uint64_t current = m_folder->Generation();
        if (current != m_generation) {
            m_generation = current;
            m_index = 0;
        }
    }

    LONG m_ref = 1;
    CFolder* m_folder = nullptr;
    DWORD m_flags = 0;
    std::uint64_t m_generation = 0;
    size_t m_index = 0;
};
