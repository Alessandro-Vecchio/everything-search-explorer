#include "folder.h"

#include <cstring>

// A PIDL is a sequence of SHITEMIDs terminated by a zero-length one.
// The folder root is a fixed 8-byte PIDL:  [cb=6]["ES"][0]
// A child (relative) PIDL is:             [cb][UTF-16 absolute path][0]
PIDLIST_ABSOLUTE CreateRootPidl() {
    auto* raw = static_cast<BYTE*>(::CoTaskMemAlloc(kRootItemCb + sizeof(USHORT)));
    if (!raw) return nullptr;

    auto* root = reinterpret_cast<RootItemId*>(raw);
    root->cb = kRootItemCb;
    root->magic[0] = L'E';
    root->magic[1] = L'S';
    *reinterpret_cast<USHORT*>(raw + kRootItemCb) = 0;

    return reinterpret_cast<PIDLIST_ABSOLUTE>(raw);
}

PIDLIST_ABSOLUTE CreateItemPidl(const std::wstring& path) {
    const size_t pathBytes = (path.size() + 1) * sizeof(WCHAR);
    if (path.empty() || pathBytes + sizeof(USHORT) > 0xFFFE) return nullptr;

    const size_t itemCb = sizeof(USHORT) + pathBytes;
    const size_t total = itemCb + sizeof(USHORT);

    auto* raw = static_cast<BYTE*>(::CoTaskMemAlloc(total));
    if (!raw) return nullptr;

    *reinterpret_cast<USHORT*>(raw) = static_cast<USHORT>(itemCb);
    std::memcpy(raw + sizeof(USHORT), path.c_str(), pathBytes);
    *reinterpret_cast<USHORT*>(raw + itemCb) = 0;

    return reinterpret_cast<PIDLIST_ABSOLUTE>(raw);
}

bool IsOurPidl(PCUIDLIST_ABSOLUTE pidl) {
    if (!pidl) return false;
    const auto* root = reinterpret_cast<const RootItemId*>(pidl);
    return root->cb == kRootItemCb && root->magic[0] == L'E' && root->magic[1] == L'S';
}

bool IsRootPidl(PCUIDLIST_ABSOLUTE pidl) {
    if (!IsOurPidl(pidl)) return false;
    const auto* raw = reinterpret_cast<const BYTE*>(pidl);
    return *reinterpret_cast<const USHORT*>(raw + kRootItemCb) == 0;
}

bool GetPidlPath(PCUIDLIST_ABSOLUTE pidl, std::wstring& path) {
    if (!pidl) return false;

    const auto* raw = reinterpret_cast<const BYTE*>(pidl);
    size_t offset = 0;

    // Defensive: some shell paths hand back the absolute form (root + item).
    if (IsOurPidl(pidl)) {
        if (IsRootPidl(pidl)) return false;
        offset = kRootItemCb;
    }

    const auto itemCb = *reinterpret_cast<const USHORT*>(raw + offset);
    if (itemCb <= sizeof(USHORT)) return false;

    const size_t chars = (itemCb - sizeof(USHORT)) / sizeof(WCHAR);
    const auto* str = reinterpret_cast<const WCHAR*>(raw + offset + sizeof(USHORT));
    if (chars == 0 || str[chars - 1] != L'\0') return false;

    const size_t len = ::wcsnlen(str, chars);
    if (len == 0) return false;

    std::wstring candidate(str, len);
    if (!IsAbsolutePath(candidate)) return false;

    path = std::move(candidate);
    return true;
}
