// Wire format shared by the mod and the NSE. Keep C++20 constructs out:
// both builds must be able to include this header.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <knownfolders.h>
#include <shlobj.h>

#include <atomic>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace esx {

inline constexpr wchar_t kDataDirName[] = L"EverythingSearchExplorer";
inline constexpr wchar_t kRequestFileName[] = L"request.txt";
inline constexpr wchar_t kWindowRequestPrefix[] = L"request-";
inline constexpr wchar_t kWindowRequestSuffix[] = L".txt";

// Flags stored in QueryRequest::flags.
enum QueryFlags : std::uint32_t {
    ScopeToFolder = 1u << 0,
    IncludeFolders = 1u << 1,
    IncludeSubfolders = 1u << 2,  // path:"...\" instead of parent:"..."
};

struct QueryRequest {
    std::uint32_t seq = 0;
    // 0 means "no limit" (up to the NSE's safety ceiling), not 500.
    std::uint32_t maxResults = 0;
    std::uint32_t flags = ScopeToFolder | IncludeFolders;
    std::wstring folder;
    std::wstring query;
    std::wstring filter;        // Everything terms that narrow the search (AND)
    std::wstring includeFilter; // Everything terms that widen the search (OR)
    bool contentSearch = false;
};

struct QueryResultItem {
    std::wstring path;
    std::uint64_t size = 0;
    std::uint64_t modified = 0;  // combined FILETIME (dwLowDateTime | dwHighDateTime << 32)
    std::uint32_t attributes = 0;
    bool isFolder = false;
};

// ---------------------------------------------------------------- text utils

inline std::string WideToUtf8(std::wstring_view s) {
    if (s.empty()) return {};
    const int len = static_cast<int>(s.size());
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, s.data(), len, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string out(static_cast<size_t>(n), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, s.data(), len, out.data(), n, nullptr, nullptr);
    return out;
}

inline std::wstring Utf8ToWide(std::string_view s) {
    if (s.empty()) return {};
    const int len = static_cast<int>(s.size());
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), len, nullptr, 0);
    if (n <= 0) return {};
    std::wstring out(static_cast<size_t>(n), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.data(), len, out.data(), n);
    return out;
}

// ------------------------------------------------------------- request file

inline std::wstring GetDataDir() {
    PWSTR raw = nullptr;
    std::wstring dir;
    if (SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &raw)) && raw) {
        dir = raw;
        ::CoTaskMemFree(raw);
    }
    if (dir.empty()) return {};
    dir += L'\\';
    dir += kDataDirName;
    ::CreateDirectoryW(dir.c_str(), nullptr);  // ok if it already exists
    return dir;
}

inline std::wstring GetRequestFilePath() {
    const std::wstring dir = GetDataDir();
    if (dir.empty()) return {};
    return dir + L"\\" + kRequestFileName;
}

// Per-window files stop one window's search from being adopted by another.
inline std::wstring GetRequestFilePath(HWND frame) {
    if (!frame) return GetRequestFilePath();
    const std::wstring dir = GetDataDir();
    if (dir.empty()) return {};
    return dir + L"\\" + kWindowRequestPrefix +
           std::to_wstring(reinterpret_cast<std::uintptr_t>(frame)) +
           kWindowRequestSuffix;
}

inline std::string SerializeRequest(const QueryRequest& r) {
    std::string s;
    s += "seq=" + std::to_string(r.seq) + "\n";
    s += "max=" + std::to_string(r.maxResults) + "\n";
    s += "flags=" + std::to_string(r.flags) + "\n";
    s += "folder=" + WideToUtf8(r.folder) + "\n";
    s += "query=" + WideToUtf8(r.query) + "\n";
    s += "filter=" + WideToUtf8(r.filter) + "\n";
    s += "include=" + WideToUtf8(r.includeFilter) + "\n";
    s += "content=" + std::to_string(r.contentSearch ? 1 : 0) + "\n";
    return s;
}

// Monotonic: a repeated seq with otherwise identical fields makes the NSE
// skip the request as unchanged.
inline std::uint32_t NextRequestSeq() {
    static std::atomic<std::uint32_t> counter{
        static_cast<std::uint32_t>(::GetTickCount64())};
    return counter.fetch_add(1, std::memory_order_relaxed);
}

inline bool ParseRequest(std::string_view text, QueryRequest& out) {
    QueryRequest r;
    std::size_t pos = 0;
    // Tolerate a UTF-8 BOM (Windows PowerShell's Set-Content adds one).
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
        static_cast<unsigned char>(text[1]) == 0xBB && static_cast<unsigned char>(text[2]) == 0xBF) {
        pos = 3;
    }
    bool sawQuery = false;
    while (pos < text.size()) {
        std::size_t eol = text.find('\n', pos);
        if (eol == std::string_view::npos) eol = text.size();
        std::string_view line = text.substr(pos, eol - pos);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        const std::size_t eq = line.find('=');
        if (eq != std::string_view::npos) {
            const std::string_view key = line.substr(0, eq);
            const std::string_view value = line.substr(eq + 1);
            if (key == "seq") r.seq = static_cast<std::uint32_t>(std::strtoul(std::string(value).c_str(), nullptr, 10));
            else if (key == "max") r.maxResults = static_cast<std::uint32_t>(std::strtoul(std::string(value).c_str(), nullptr, 10));
            else if (key == "flags") r.flags = static_cast<std::uint32_t>(std::strtoul(std::string(value).c_str(), nullptr, 10));
            else if (key == "folder") r.folder = Utf8ToWide(value);
            else if (key == "query") { r.query = Utf8ToWide(value); sawQuery = true; }
            else if (key == "filter") r.filter = Utf8ToWide(value);
            else if (key == "include") r.includeFilter = Utf8ToWide(value);
            else if (key == "content") r.contentSearch = std::strtoul(std::string(value).c_str(), nullptr, 10) != 0;
        }
        pos = eol + 1;
    }
    // 0 is a valid value (unlimited) and must survive the round trip.
    out = std::move(r);
    return sawQuery;
}

inline bool ReadRequestFileFrom(const std::wstring& path, QueryRequest& out) {
    if (path.empty()) return false;
    HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ,
                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD size = ::GetFileSize(h, nullptr);
    if (size == INVALID_FILE_SIZE || size > 64 * 1024) {
        ::CloseHandle(h);
        return false;
    }
    std::string data(size, '\0');
    DWORD read = 0;
    const bool ok = size == 0 || (::ReadFile(h, data.data(), size, &read, nullptr) && read == size);
    ::CloseHandle(h);
    return ok && ParseRequest(data, out);
}

inline bool WriteRequestFileTo(const std::wstring& path, const QueryRequest& r) {
    if (path.empty()) return false;
    // A unique temp per writer avoids collisions between concurrent searches.
    static std::atomic<unsigned long long> counter{0};
    const std::wstring tmp = path + L"." + std::to_wstring(::GetCurrentProcessId()) + L"." +
                             std::to_wstring(::GetCurrentThreadId()) + L"." +
                             std::to_wstring(counter.fetch_add(1, std::memory_order_relaxed)) +
                             L".tmp";
    HANDLE h = ::CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    const std::string data = SerializeRequest(r);
    DWORD written = 0;
    const bool ok = ::WriteFile(h, data.data(), static_cast<DWORD>(data.size()), &written, nullptr) &&
                    written == data.size();
    ::CloseHandle(h);
    if (!ok) return false;
    return ::MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != FALSE;
}

inline bool ReadRequestFile(QueryRequest& out) {
    return ReadRequestFileFrom(GetRequestFilePath(), out);
}

// The per-window file wins; the global one is kept only as a fallback for
// windows opened before per-window handoff existed (or via external tools).
inline bool ReadRequestForWindow(HWND frame, QueryRequest& out) {
    if (frame && ReadRequestFileFrom(GetRequestFilePath(frame), out)) return true;
    return ReadRequestFile(out);
}

inline bool WriteRequestFile(const QueryRequest& r) {
    return WriteRequestFileTo(GetRequestFilePath(), r);
}

// Write the per-window snapshot and the global file for tools. The extension
// only reads the global file when no per-window file exists.
inline bool WriteRequestForWindow(HWND frame, const QueryRequest& r) {
    const bool global = WriteRequestFile(r);
    if (!frame) return global;
    const bool perWindow = WriteRequestFileTo(GetRequestFilePath(frame), r);
    return perWindow;
}

}  // namespace esx
