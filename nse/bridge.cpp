#include "bridge.h"

#include <algorithm>
#include <cstring>
#include <iterator>
#include <mutex>
#include <new>
#include <string>
#include <vector>

#include "everything_ipc.h"
#include "folder.h"

namespace bridge {
namespace {

// Posted to our own worker window to start a query or to abandon one.
constexpr UINT kQueryMessage = WM_APP + 0x51;
constexpr UINT kCancelMessage = WM_APP + 0x52;
constexpr wchar_t kIpcWindowClass[] = L"EverythingSearchIpcWnd";

// Replies carry this value in COPYDATASTRUCT::dwData, so a late reply from an
// abandoned query cannot be mistaken for the current one.
// High bits identify our replies; the low 20 bits hold a wrapping counter.
constexpr DWORD kReplyBase = 0x45500000;
constexpr DWORD kSendTimeoutMs = 4000;   // Everything hung: give up early
constexpr DWORD kReplyTimeoutMs = 5000;  // large pages need real time
constexpr DWORD kRequestFlags = esx::ipc::kRequestName | esx::ipc::kRequestPath |
                                esx::ipc::kRequestSize | esx::ipc::kRequestDateModified |
                                esx::ipc::kRequestAttributes;

enum class Error {
    None,
    NotRunning,
    SendFailed,
    Timeout,
    BadReply,
    Cancelled,
};

struct Job {
    DWORD replyId = 0;
    std::uint64_t owner = 0;
    std::wstring search;
    DWORD offset = 0;
    DWORD maxResults = 0;
    DWORD flags = 0;
    Error error = Error::None;
    DWORD rawCount = 0;
    DWORD totalCount = 0;
    std::vector<esx::QueryResultItem> items;
    std::vector<BYTE> queryBuffer;  // kept alive while Everything may read it
    bool completed = false;
    HANDLE done = nullptr;
    LONG refs = 1;

    ~Job() {
        if (done) ::CloseHandle(done);
    }
};

SRWLOCK g_lock = SRWLOCK_INIT;       // guards g_pending
SRWLOCK g_initLock = SRWLOCK_INIT;   // guards worker startup
HWND g_wnd = nullptr;                // worker thread's message-only window
HANDLE g_ready = nullptr;
HANDLE g_thread = nullptr;
Job* g_pending = nullptr;
LONG g_nextJobId = 0;
// Everything keeps only one query per reply window. Serialize whole page
// requests so two CFolder instances cannot cancel each other's g_pending job.
std::mutex g_queryMutex;

// --------------------------------------------------------------- utilities

void ReleaseJob(Job* job) {
    if (::InterlockedDecrement(&job->refs) == 0) delete job;
}

std::string DescribeError(Error error) {
    switch (error) {
        case Error::NotRunning:
            return "Everything is not running";
        case Error::SendFailed:
            return "Everything is not responding";
        case Error::Timeout:
            return "Everything did not answer in time";
        case Error::BadReply:
            return "Everything returned an unexpected reply";
        case Error::Cancelled:
            return "search cancelled";
        default:
            return "unknown error";
    }
}

std::vector<std::wstring> SplitTerms(const std::wstring& value) {
    std::vector<std::wstring> terms;
    std::size_t pos = 0;
    while (pos < value.size()) {
        while (pos < value.size() && value[pos] == L' ') ++pos;
        const std::size_t start = pos;
        while (pos < value.size() && value[pos] != L' ') ++pos;
        if (pos > start) terms.push_back(value.substr(start, pos - start));
    }
    return terms;
}

// Everything requires matches to stay inside the folder when scoped. The
// search functions used here (parent:, attributes:, date-modified:, size:)
// and < > grouping need Everything 1.5.
std::wstring BuildSearchStringInternal(const esx::QueryRequest& req) {
    // scope is shared by every alternative so a toggle never widens the
    // search outside the current folder. text adds the plain filename query.
    std::wstring scope;
    if ((req.flags & esx::ScopeToFolder) && !req.folder.empty()) {
        // "Cartella corrente" means direct children only (parent:); "Tutte le
        // sottocartelle" adds IncludeSubfolders and uses recursive path:.
        const bool recursive = (req.flags & esx::IncludeSubfolders) != 0;
        std::wstring folder = req.folder;
        if (recursive) {
            // A trailing backslash makes path: match descendants, not a
            // sibling whose name starts with the same text.
            while (folder.size() > 3 && (folder.back() == L'\\' || folder.back() == L'/')) {
                folder.pop_back();
            }
            if (!folder.empty() && folder.back() != L'\\' && folder.back() != L'/') {
                folder += L'\\';
            }
        }
        const wchar_t* function = recursive ? L"path:" : L"parent:";
        scope = function + std::wstring(L"\"") + folder + L"\"";
    }
    auto append = [](std::wstring& target, const std::wstring& term) {
        if (term.empty()) return;
        if (!target.empty()) target += L' ';
        target += term;
    };
    append(scope, req.filter);
    std::wstring text = scope;
    append(text, req.query);

    // Everything hides system, compressed and content matches by default.
    // A toggle includes that category; without it the category is excluded.
    const auto includes = SplitTerms(req.includeFilter);
    auto isIncluded = [&](const std::wstring& token) {
        for (const auto& item : includes) {
            if (_wcsicmp(item.c_str(), token.c_str()) == 0) return true;
        }
        return false;
    };
    const bool withSystem = isIncluded(L"attributes:s");
    const bool withZip = isIncluded(L"zip:");

    auto withExclusions = [&](std::wstring value) {
        if (!withSystem) append(value, L"!attributes:s");
        if (!withZip) append(value, L"!zip:");
        return value;
    };

    std::wstring base = withExclusions(text);
    if (base.empty() && !(req.contentSearch && !req.query.empty())) return {};

    // Each opted-in category is an alternative that still matches the query.
    std::vector<std::wstring> alternatives;
    if (!base.empty()) alternatives.push_back(L"<" + base + L">");
    for (const auto& token : includes) {
        if (!token.empty() && !text.empty()) {
            alternatives.push_back(L"<" + text + L" " + token + L">");
        }
    }
    if (req.contentSearch && !req.query.empty()) {
        // Keep scope and filters: a content search must not escape into a
        // global search. Drop the plain filename term and group the query so
        // multi-word searches match the content instead of the name.
        std::wstring contentQuery = req.query;
        if (contentQuery.size() < 2 || contentQuery.front() != L'<' ||
            contentQuery.back() != L'>') {
            contentQuery = L"<" + contentQuery + L">";
        }
        std::wstring contentBase = withExclusions(scope);
        append(contentBase, L"content:" + contentQuery);
        alternatives.push_back(L"<" + contentBase + L">");
    }
    std::wstring result;
    for (const auto& alt : alternatives) {
        if (!result.empty()) result += L" | ";
        result += alt;
    }
    return result;
}

// ------------------------------------------------------------ reply parsing

bool ReadString(const BYTE* base, DWORD size, DWORD& pos, std::wstring& out) {
    if (pos > size || size - pos < sizeof(DWORD)) return false;

    DWORD chars = 0;
    std::memcpy(&chars, base + pos, sizeof(chars));
    pos += sizeof(DWORD);

    if (chars > (size - pos) / sizeof(WCHAR)) return false;
    if (size - pos < (static_cast<size_t>(chars) + 1) * sizeof(WCHAR)) return false;

    out.assign(reinterpret_cast<const WCHAR*>(base + pos), chars);
    pos += static_cast<DWORD>((chars + 1) * sizeof(WCHAR));
    return true;
}

template <class T>
bool ReadPod(const BYTE* base, DWORD size, DWORD& pos, T& out) {
    if (pos > size || size - pos < sizeof(T)) return false;
    std::memcpy(&out, base + pos, sizeof(T));
    pos += sizeof(T);
    return true;
}

bool ParseReply(const BYTE* base, DWORD size, DWORD requestFlags, DWORD flags, Job& job) {
    if (!base || size < sizeof(esx::ipc::List2)) return false;

    esx::ipc::List2 list{};
    std::memcpy(&list, base, sizeof(list));
    if (list.request_flags != requestFlags) return false;  // payload layout we did not ask for

    // A valid reply fits numitems complete entries before the payload.
    const DWORD available = (size - sizeof(esx::ipc::List2)) / sizeof(esx::ipc::Item2);
    if (list.numitems > available || list.numitems > list.totitems) return false;

    const DWORD count = list.numitems;
    job.rawCount = count;
    job.totalCount = list.totitems;

    const auto* items = reinterpret_cast<const esx::ipc::Item2*>(base + sizeof(esx::ipc::List2));

    for (DWORD i = 0; i < count; ++i) {
        esx::ipc::Item2 item{};
        std::memcpy(&item, &items[i], sizeof(item));  // packed, avoid alignment traps

        if (item.flags & esx::ipc::kItemDrive) continue;

        const bool isFolder = (item.flags & esx::ipc::kItemFolder) != 0;
        if (isFolder && !(flags & esx::IncludeFolders)) continue;

        // Payload starts after the List2 header and the Item2 array; a smaller
        // value would point inside the array and create spurious rows.
        const DWORD payloadStart =
            sizeof(esx::ipc::List2) + count * sizeof(esx::ipc::Item2);
        if (item.data_offset < payloadStart || item.data_offset >= size) continue;

        // Fields are packed in request_flags bit order, with no padding.
        DWORD pos = item.data_offset;
        std::wstring name;
        std::wstring path;
        LARGE_INTEGER itemSize{};
        FILETIME modified{};
        DWORD attributes = 0;

        bool ok = true;
        if (list.request_flags & esx::ipc::kRequestName) {
            ok = ReadString(base, size, pos, name);
        }
        if (ok && (list.request_flags & esx::ipc::kRequestPath)) {
            ok = ReadString(base, size, pos, path);
        }
        if (ok && (list.request_flags & esx::ipc::kRequestSize)) {
            ok = ReadPod(base, size, pos, itemSize);
        }
        if (ok && (list.request_flags & esx::ipc::kRequestDateModified)) {
            ok = ReadPod(base, size, pos, modified);
        }
        if (ok && (list.request_flags & esx::ipc::kRequestAttributes)) {
            ok = ReadPod(base, size, pos, attributes);
        }
        if (!ok) continue;

        esx::QueryResultItem result;
        // QUERY2 returns NAME and PATH separately, where PATH is the containing
        // directory (unlike the v1 LIST, where path is already complete).
        if (name.empty()) continue;
        result.path = path;
        if (!result.path.empty() && result.path.back() != L'\\' && result.path.back() != L'/') {
            result.path += L'\\';
        }
        result.path += name;

        result.isFolder = isFolder;
        result.size = static_cast<std::uint64_t>(itemSize.QuadPart);
        result.modified = (static_cast<std::uint64_t>(modified.dwHighDateTime) << 32) |
                          modified.dwLowDateTime;
        result.attributes = attributes;
        job.items.push_back(std::move(result));
    }

    return true;
}

// ------------------------------------------------------------ worker thread

void FinishJob(Job* job) {
    job->completed = true;
    if (job->done) ::SetEvent(job->done);

    ::AcquireSRWLockExclusive(&g_lock);
    if (g_pending == job) g_pending = nullptr;
    ::ReleaseSRWLockExclusive(&g_lock);

    ReleaseJob(job);  // the worker's reference
}

bool SendToEverything(Job& job) {
    HWND everything = ::FindWindowW(esx::ipc::kWindowClass, nullptr);
    if (!everything) {
        job.error = Error::NotRunning;
        return false;
    }

    const size_t chars = job.search.size();
    const size_t bytes =
        sizeof(esx::ipc::Query2) - sizeof(WCHAR) + (chars + 1) * sizeof(WCHAR);
    job.queryBuffer.assign(bytes, 0);

    auto* query = reinterpret_cast<esx::ipc::Query2*>(job.queryBuffer.data());
    query->reply_hwnd = static_cast<DWORD>(reinterpret_cast<DWORD_PTR>(g_wnd));
    query->reply_copydata_message = job.replyId;
    query->search_flags = 0;
    query->offset = job.offset;
    query->max_results = job.maxResults;
    query->request_flags = kRequestFlags;
    query->sort_type = esx::ipc::kSortNameAscending;
    std::memcpy(query->search_string, job.search.c_str(), (chars + 1) * sizeof(WCHAR));

    COPYDATASTRUCT cds{};
    cds.dwData = esx::ipc::kCopyDataQuery2W;
    cds.cbData = static_cast<DWORD>(bytes);
    cds.lpData = job.queryBuffer.data();

    DWORD_PTR ignored = 0;
    // ABORTIFHUNG: if Everything's thread does not pump, fail now instead of
    // waiting for it. Everything queues the query and answers asynchronously,
    // so this call is short in the normal case.
    const LRESULT sent = ::SendMessageTimeoutW(everything, WM_COPYDATA,
                                               reinterpret_cast<WPARAM>(g_wnd),
                                               reinterpret_cast<LPARAM>(&cds),
                                               SMTO_ABORTIFHUNG | SMTO_BLOCK, kSendTimeoutMs,
                                               &ignored);
    if (!sent) {
        job.error = Error::SendFailed;
        return false;
    }
    return true;
}

// Everything runs one query per reply window: sending another QUERY2 cancels
// the one in flight. max_results=0 asks for no results, so this is cheap and
// its reply (tagged with a fresh id) is ignored because no job is pending.
void SendCancelToEverything() {
    HWND everything = ::FindWindowW(esx::ipc::kWindowClass, nullptr);
    if (!everything || !g_wnd) return;

    constexpr DWORD bytes = sizeof(esx::ipc::Query2) - sizeof(WCHAR) + sizeof(WCHAR);
    std::vector<BYTE> buffer(bytes, 0);
    auto* query = reinterpret_cast<esx::ipc::Query2*>(buffer.data());
    query->reply_hwnd = static_cast<DWORD>(reinterpret_cast<DWORD_PTR>(g_wnd));
    query->reply_copydata_message =
        kReplyBase | (static_cast<DWORD>(::InterlockedIncrement(&g_nextJobId)) & 0x000FFFFF);
    query->sort_type = esx::ipc::kSortNameAscending;

    COPYDATASTRUCT cds{};
    cds.dwData = esx::ipc::kCopyDataQuery2W;
    cds.cbData = bytes;
    cds.lpData = buffer.data();

    DWORD_PTR ignored = 0;
    ::SendMessageTimeoutW(everything, WM_COPYDATA, reinterpret_cast<WPARAM>(g_wnd),
                          reinterpret_cast<LPARAM>(&cds), SMTO_ABORTIFHUNG | SMTO_BLOCK,
                          kSendTimeoutMs, &ignored);
}

LRESULT CALLBACK IpcWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == kQueryMessage) {
        auto* job = reinterpret_cast<Job*>(lParam);

        Job* stale = nullptr;
        ::AcquireSRWLockExclusive(&g_lock);
        stale = g_pending;
        g_pending = job;
        ::ReleaseSRWLockExclusive(&g_lock);

        // Everything keeps one query per reply window, so a new query cancels
        // the previous one: retire it instead of leaking it.
        if (stale && stale != job) {
            stale->error = Error::Timeout;
            FinishJob(stale);
        }

        if (!SendToEverything(*job)) FinishJob(job);
        return 0;
    }

    if (msg == kCancelMessage) {
        const std::uint64_t owner = static_cast<std::uint64_t>(lParam);
        Job* victim = nullptr;
        ::AcquireSRWLockExclusive(&g_lock);
        if (g_pending && g_pending->owner == owner) {
            victim = g_pending;
            g_pending = nullptr;
        }
        ::ReleaseSRWLockExclusive(&g_lock);
        if (victim) {
            victim->error = Error::Cancelled;
            FinishJob(victim);
            SendCancelToEverything();
        }
        return 0;
    }

    if (msg == WM_COPYDATA) {
        auto* cds = reinterpret_cast<COPYDATASTRUCT*>(lParam);
        if (!cds || !cds->lpData || cds->cbData == 0) return FALSE;

        Job* job = nullptr;
        ::AcquireSRWLockShared(&g_lock);
        if (g_pending && g_pending->replyId == cds->dwData) job = g_pending;
        ::ReleaseSRWLockShared(&g_lock);

        if (!job) return TRUE;  // late reply for an abandoned query

        if (!ParseReply(static_cast<const BYTE*>(cds->lpData), cds->cbData, kRequestFlags,
                        job->flags, *job)) {
            job->error = Error::BadReply;
        }
        FinishJob(job);
        return TRUE;
    }

    return ::DefWindowProcW(hwnd, msg, wParam, lParam);
}

DWORD WINAPI WorkerProc(void*) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = IpcWndProc;
    wc.hInstance = g_hInst;
    wc.lpszClassName = kIpcWindowClass;
    ::RegisterClassExW(&wc);  // ERROR_CLASS_ALREADY_EXISTS is harmless

    HWND wnd = ::CreateWindowExW(0, kIpcWindowClass, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
                                 g_hInst, nullptr);

    ::AcquireSRWLockExclusive(&g_initLock);
    g_wnd = wnd;
    ::ReleaseSRWLockExclusive(&g_initLock);
    if (g_ready) ::SetEvent(g_ready);

    if (!wnd) return 0;

    MSG msg;
    while (::GetMessageW(&msg, nullptr, 0, 0) > 0) {
        ::TranslateMessage(&msg);
        ::DispatchMessageW(&msg);
    }
    return 0;
}

bool EnsureWorker() {
    ::AcquireSRWLockExclusive(&g_initLock);
    if (!g_ready) g_ready = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!g_thread && g_ready) {
        // Pin the module for the lifetime of the worker: COM must not unload a
        // DLL whose window class and thread are still alive in Explorer.
        ::InterlockedIncrement(&g_cRefDll);
        g_thread = ::CreateThread(nullptr, 0, WorkerProc, nullptr, 0, nullptr);
        if (!g_thread) ::InterlockedDecrement(&g_cRefDll);
    }
    HANDLE ready = g_ready;
    ::ReleaseSRWLockExclusive(&g_initLock);

    if (!ready) return false;
    if (::WaitForSingleObject(ready, 3000) != WAIT_OBJECT_0) return false;
    return g_wnd != nullptr;
}

// ------------------------------------------------------------- query driver

bool RunOnePage(const std::wstring& search, DWORD offset, DWORD flags, DWORD maxResults,
                std::vector<esx::QueryResultItem>& out, DWORD& rawCount, DWORD& totalCount,
                std::string& error, std::uint64_t owner) {
    std::lock_guard<std::mutex> queryLock(g_queryMutex);
    auto* job = new (std::nothrow) Job();
    if (!job) {
        error = "out of memory";
        return false;
    }

    job->done = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!job->done) {
        delete job;
        error = "could not create the completion event";
        return false;
    }

    job->replyId =
        kReplyBase | (static_cast<DWORD>(::InterlockedIncrement(&g_nextJobId)) & 0x000FFFFF);
    job->owner = owner;
    job->search = search;
    job->offset = offset;
    job->maxResults = maxResults;
    job->flags = flags;
    ::InterlockedIncrement(&job->refs);  // the worker owns this reference

    if (!::PostMessageW(g_wnd, kQueryMessage, 0, reinterpret_cast<LPARAM>(job))) {
        ReleaseJob(job);
        ReleaseJob(job);
        error = "could not reach the IPC thread";
        return false;
    }

    const DWORD wait = ::WaitForSingleObject(job->done, kReplyTimeoutMs);
    bool ok = false;
    if (wait == WAIT_OBJECT_0 && job->completed && job->error == Error::None) {
        out = std::move(job->items);
        rawCount = job->rawCount;
        totalCount = job->totalCount;
        ok = true;
    } else {
        // Only read job state when the worker signalled completion.
        error = DescribeError(wait == WAIT_OBJECT_0 ? job->error : Error::Timeout);
    }

    ReleaseJob(job);  // the caller's reference
    return ok;
}

}  // namespace

bool EnsureReady() {
    if (!::FindWindowW(esx::ipc::kWindowClass, nullptr)) return false;
    return EnsureWorker();
}

std::wstring BuildSearchString(const esx::QueryRequest& req) {
    return BuildSearchStringInternal(req);
}

bool QueryPage(const std::wstring& search, DWORD offset, DWORD flags, DWORD wanted,
               std::vector<esx::QueryResultItem>& out, DWORD& rawCount,
               DWORD& totalCount, std::string& error, std::uint64_t owner) {
    if (!EnsureReady()) {
        error = DescribeError(Error::NotRunning);
        return false;
    }
    const ULONGLONG started = ::GetTickCount64();
    const bool ok =
        RunOnePage(search, offset, flags, wanted, out, rawCount, totalCount, error, owner);
    ESX_TRACE_LOG("TIMING QueryPage offset=%u raw=%u total=%u ms=%llu", offset, rawCount,
                  totalCount, ::GetTickCount64() - started);
    return ok;
}

bool Query(const esx::QueryRequest& req, std::vector<esx::QueryResultItem>& out,
           std::string& error) {
    out.clear();

    const ULONGLONG tStart = ::GetTickCount64();
    if (!::FindWindowW(esx::ipc::kWindowClass, nullptr)) {
        error = DescribeError(Error::NotRunning);
        return false;
    }
    if (!EnsureWorker()) {
        error = "IPC thread not available";
        return false;
    }
    const ULONGLONG tReady = ::GetTickCount64();
    ESX_TRACE_LOG("TIMING Query init=%llu ms", tReady - tStart);

    const std::wstring search = BuildSearchStringInternal(req);
    // 0 = no user limit; kSafetyCeiling only keeps a runaway query (e.g. "*"
    // on C:\) from filling all RAM. Anything below it is returned in full.
    const DWORD limit = req.maxResults && req.maxResults < kSafetyCeiling
                            ? req.maxResults
                            : kSafetyCeiling;

    // Page through the result list: Everything truncates before we can drop
    // drives, so a drive-heavy scope would otherwise return nothing.
    DWORD offset = 0;
    for (DWORD batch = 0; batch < kMaxBatches && out.size() < limit; ++batch) {
        const DWORD wanted =
        std::min<DWORD>(bridge::kBatchSize, limit - static_cast<DWORD>(out.size()));

        std::vector<esx::QueryResultItem> page;
        DWORD raw = 0;
        DWORD total = 0;
        const ULONGLONG tPage = ::GetTickCount64();
        if (!RunOnePage(search, offset, req.flags, wanted, page, raw, total, error, 0)) {
            return false;
        }
        ESX_TRACE_LOG("TIMING Query page=%u raw=%u total=%u ms=%llu", batch, raw, total,
                      ::GetTickCount64() - tPage);

        out.insert(out.end(), std::make_move_iterator(page.begin()),
                   std::make_move_iterator(page.end()));

        if (raw < wanted) break;
        if (total <= offset + raw) break;
        offset += raw;
    }

    if (out.size() > limit) out.resize(limit);

    // Defensive: the scoped search should already exclude the origin folder,
    // but showing the folder we came from is never useful.
    if (req.flags & esx::ScopeToFolder && !req.folder.empty()) {
        std::wstring scoped = req.folder;
        while (scoped.size() > 3 && (scoped.back() == L'\\' || scoped.back() == L'/')) {
            scoped.pop_back();
        }
        out.erase(std::remove_if(out.begin(), out.end(),
                                 [&scoped](const esx::QueryResultItem& item) {
                                     std::wstring path = item.path;
                                     while (path.size() > 3 &&
                                            (path.back() == L'\\' || path.back() == L'/')) {
                                         path.pop_back();
                                     }
                                     return _wcsicmp(path.c_str(), scoped.c_str()) == 0;
                                 }),
                  out.end());
    }
    ESX_TRACE_LOG("TIMING Query total=%llu ms results=%zu limit=%u",
                  ::GetTickCount64() - tStart, out.size(), limit);
    return true;
}

void CancelQuery(std::uint64_t owner) {
    if (!owner) return;
    ::AcquireSRWLockShared(&g_initLock);
    HWND wnd = g_wnd;
    ::ReleaseSRWLockShared(&g_initLock);
    if (wnd) ::PostMessageW(wnd, kCancelMessage, 0, static_cast<LPARAM>(owner));
}

}  // namespace bridge
