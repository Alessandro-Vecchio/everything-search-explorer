// Raw dump of Everything's QUERY2 reply, used to validate the IPC layout that
// shared/everything_ipc.h assumes. Diagnostics only, not shipped.
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../shared/everything_ipc.h"

namespace {

struct Capture {
    bool done = false;
    std::vector<BYTE> data;
    DWORD dwData = 0;
};

Capture* g_capture = nullptr;

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_COPYDATA) {
        auto* cds = reinterpret_cast<COPYDATASTRUCT*>(lParam);
        if (g_capture && cds) {
            g_capture->dwData = cds->dwData;
            const auto* p = static_cast<const BYTE*>(cds->lpData);
            g_capture->data.assign(p, p + cds->cbData);
            g_capture->done = true;
        }
        return TRUE;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

DWORD EverythingVersion(HWND everything, int part) {
    return static_cast<DWORD>(SendMessageW(everything, WM_USER, part, 0));
}

const wchar_t* PartName(int part) {
    switch (part) {
        case 0: return L"major";
        case 1: return L"minor";
        case 2: return L"revision";
        default: return L"build";
    }
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    const std::wstring search = argc > 1 ? argv[1] : L"";

    HWND everything = FindWindowW(esx::ipc::kWindowClass, nullptr);
    if (!everything) {
        wprintf(L"Everything is not running\n");
        return 1;
    }

    wprintf(L"Everything %u.%u.%u.%u\n", EverythingVersion(everything, 0),
            EverythingVersion(everything, 1), EverythingVersion(everything, 2),
            EverythingVersion(everything, 3));

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"IpcProbeWnd";
    RegisterClassExW(&wc);

    HWND wnd = CreateWindowExW(0, L"IpcProbeWnd", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
                               wc.hInstance, nullptr);
    if (!wnd) {
        wprintf(L"CreateWindow failed: %lu\n", GetLastError());
        return 1;
    }

    Capture capture;
    g_capture = &capture;

    const size_t chars = search.size();
    const size_t bytes = sizeof(esx::ipc::Query2) - sizeof(WCHAR) + (chars + 1) * sizeof(WCHAR);
    std::vector<BYTE> buffer(bytes, 0);
    auto* query = reinterpret_cast<esx::ipc::Query2*>(buffer.data());
    query->reply_hwnd = static_cast<DWORD>(reinterpret_cast<DWORD_PTR>(wnd));
    query->reply_copydata_message = 0x1234;
    query->search_flags = 0;
    query->offset = 0;
    query->max_results = 8;
    query->request_flags = esx::ipc::kRequestName | esx::ipc::kRequestPath |
                           esx::ipc::kRequestSize | esx::ipc::kRequestDateModified |
                           esx::ipc::kRequestAttributes;
    query->sort_type = esx::ipc::kSortNameAscending;
    memcpy(query->search_string, search.c_str(), (chars + 1) * sizeof(WCHAR));

    COPYDATASTRUCT cds{};
    cds.dwData = esx::ipc::kCopyDataQuery2W;
    cds.cbData = static_cast<DWORD>(bytes);
    cds.lpData = buffer.data();

    DWORD_PTR sent = 0;
    const LRESULT ok = SendMessageTimeoutW(everything, WM_COPYDATA, reinterpret_cast<WPARAM>(wnd),
                                           reinterpret_cast<LPARAM>(&cds),
                                           SMTO_ABORTIFHUNG | SMTO_BLOCK, 2000, &sent);
    wprintf(L"send: %lld (sent=%llu) search=\"%ls\"\n", static_cast<long long>(ok),
            static_cast<unsigned long long>(sent), search.c_str());
    if (!ok) return 1;

    for (int i = 0; i < 200 && !capture.done; ++i) {
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 100, QS_ALLINPUT);
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    if (!capture.done) {
        wprintf(L"no reply\n");
        return 1;
    }

    wprintf(L"reply: cbData=%zu dwData=0x%08lX (expected 0x1234)\n", capture.data.size(),
            capture.dwData);

    if (capture.data.size() < sizeof(esx::ipc::List2)) {
        wprintf(L"too small for List2 (%zu bytes)\n", sizeof(esx::ipc::List2));
        return 1;
    }

    esx::ipc::List2 list{};
    memcpy(&list, capture.data.data(), sizeof(list));
    wprintf(L"List2 tot=%lu num=%lu offset=%lu request=0x%08lX sort=%lu (sizeof=%zu)\n",
            list.totitems, list.numitems, list.offset, list.request_flags, list.sort_type,
            sizeof(esx::ipc::List2));

    const auto* items =
        reinterpret_cast<const esx::ipc::Item2*>(capture.data.data() + sizeof(esx::ipc::List2));
    const DWORD count = (list.numitems < 8) ? list.numitems : 8;

    for (DWORD i = 0; i < count; ++i) {
        esx::ipc::Item2 item{};
        memcpy(&item, &items[i], sizeof(item));
        wprintf(L"  item[%lu] flags=0x%08lX data_offset=%lu\n", i, item.flags, item.data_offset);

        const BYTE* base = capture.data.data();
        const DWORD size = static_cast<DWORD>(capture.data.size());
        DWORD pos = item.data_offset;
        if (pos + 8 > size) {
            wprintf(L"    offset out of range\n");
            continue;
        }

        DWORD nameChars = 0;
        memcpy(&nameChars, base + pos, 4);
        pos += 4;
        std::wstring name;
        if (nameChars < 4096 && pos + (nameChars + 1) * 2 <= size) {
            name.assign(reinterpret_cast<const WCHAR*>(base + pos), nameChars);
        }
        pos += (nameChars + 1) * 2;

        DWORD pathChars = 0;
        std::wstring path;
        if (pos + 4 <= size) {
            memcpy(&pathChars, base + pos, 4);
            pos += 4;
            if (pathChars < 32768 && pos + (pathChars + 1) * 2 <= size) {
                path.assign(reinterpret_cast<const WCHAR*>(base + pos), pathChars);
            }
            pos += (pathChars + 1) * 2;
        }

        LARGE_INTEGER itemSize{};
        if (pos + 8 <= size) memcpy(&itemSize, base + pos, 8);
        pos += 8;

        FILETIME modified{};
        if (pos + 8 <= size) memcpy(&modified, base + pos, 8);
        pos += 8;

        DWORD attributes = 0;
        if (pos + 4 <= size) memcpy(&attributes, base + pos, 4);

        wprintf(L"    name=\"%ls\"\n    path=\"%ls\"\n    size=%lld attrs=0x%08lX pos=%lu\n",
                name.c_str(), path.c_str(), static_cast<long long>(itemSize.QuadPart), attributes,
                pos);
    }

    wprintf(L"raw head:");
    for (size_t i = 0; i < 48 && i < capture.data.size(); ++i) wprintf(L" %02X", capture.data[i]);
    wprintf(L"\n");

    DestroyWindow(wnd);
    return 0;
}
