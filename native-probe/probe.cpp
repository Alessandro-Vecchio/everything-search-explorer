// ==WindhawkMod==
// @id              local@esx-native-probe
// @name            Everything Search - native probe
// @description     Bisect harness: one subsystem at a time, selected by a stage file.
// @version         0.1.0
// @author          Alessandro
// @include         explorer.exe
// @compilerOptions -lole32 -luser32 -ladvapi32
// ==/WindhawkMod==
//
// Loads inside explorer.exe and does nothing except log, then one subsystem at
// a time according to %TEMP%\esx-native-probe-stage.txt:
//
//   0  log only, no thread                        (tests injection alone)
//   1  + STA coordinator thread with message pump (tests COM apartment)
//   2  + LL keyboard hook                         (tests the global hook)
//   3  + window scan every 16 ms                  (tests EnumWindows/GetWindowText)
//   4  + thread-scoped WH_KEYBOARD on the UI thread (the candidate for Enter)
//   5  thread with message pump but no COM apartment
//   6  stage 2, but started only after the UI has settled (40 s + taskbar/frame)
//   7  thread with no message queue and no COM apartment
//   8  bare thread: no logging, no queue, no COM
//   9  no startup thread at all; worker created by a 60 s timer
//
// Two safety rules keep the desktop shell alive:
//   - the stage defaults to 0 and is read from a file, so a rebuild is never
//     needed to change what runs;
//   - if four or more Explorer processes start within 45 s the probe enters
//     safe mode and does nothing, which breaks a crash loop on the second or
//     third restart.
//
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <objbase.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

constexpr int kMaxStage = 9;
constexpr int kLoopStarts = 3;
constexpr long kLoopWindowSeconds = 120;

bool g_shellProcess = false;
int g_stage = 0;
int g_ownFrames = 0;
int g_scanWindows = 0;
int g_scanTitled = 0;

HANDLE g_stopEvent = nullptr;
HANDLE g_thread = nullptr;
HANDLE g_timer = nullptr;
HHOOK g_keyboardHook = nullptr;
bool g_bareThread = false;

// --------------------------------------------------------------------- paths
std::wstring TempFile(const wchar_t* leaf) {
    wchar_t dir[MAX_PATH]{};
    DWORD len = GetTempPathW(ARRAYSIZE(dir), dir);
    std::wstring path;
    if (len > 0 && len < ARRAYSIZE(dir)) {
        path.assign(dir, len);
    } else {
        path = L"C:\\Windows\\Temp\\";
    }
    if (!path.empty() && path.back() != L'\\') path.push_back(L'\\');
    path += leaf;
    return path;
}

std::wstring LocalAppDataFile(const wchar_t* leaf) {
    wchar_t dir[MAX_PATH * 2]{};
    DWORD len = GetEnvironmentVariableW(L"LOCALAPPDATA", dir, ARRAYSIZE(dir));
    std::wstring path = (len > 0 && len < ARRAYSIZE(dir)) ? std::wstring(dir, len)
                                                         : L"C:\\Temp";
    if (!path.empty() && path.back() != L'\\') path.push_back(L'\\');
    path += L"EverythingExplorerSearch";
    CreateDirectoryW(path.c_str(), nullptr);
    path.push_back(L'\\');
    path += leaf;
    return path;
}

// --------------------------------------------------------------------- log
std::wstring LogPath() {
    static const std::wstring path = TempFile(L"esx-native-probe.log");
    return path;
}

// One WriteFile per line: several Explorer processes log to the same file and
// stdio would interleave partial lines and lose them.
void Log(const char* format, ...) {
    char body[1024]{};
    va_list args;
    va_start(args, format);
    const int bodyLength = vsnprintf(body, sizeof(body), format, args);
    va_end(args);
    if (bodyLength < 0) return;

    SYSTEMTIME now{};
    GetLocalTime(&now);
    char line[1200]{};
    const int total = snprintf(line, sizeof(line), "%02u:%02u:%02u.%03u [%lu:%lu] %s\n",
                               now.wHour, now.wMinute, now.wSecond,
                               now.wMilliseconds, GetCurrentProcessId(),
                               GetCurrentThreadId(), body);
    if (total <= 0) return;

    HANDLE file = CreateFileW(LogPath().c_str(), FILE_APPEND_DATA,
                              FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(file, line, static_cast<DWORD>(total), &written, nullptr);
    CloseHandle(file);
}

ULONGLONG ProcessUptimeSeconds() {
    FILETIME creation{}, exit{}, kernel{}, user{};
    if (!GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user)) {
        return 0;
    }
    ULARGE_INTEGER created{};
    created.LowPart = creation.dwLowDateTime;
    created.HighPart = creation.dwHighDateTime;
    FILETIME nowFileTime{};
    GetSystemTimeAsFileTime(&nowFileTime);
    ULARGE_INTEGER now{};
    now.LowPart = nowFileTime.dwLowDateTime;
    now.HighPart = nowFileTime.dwHighDateTime;
    if (now.QuadPart <= created.QuadPart) return 0;
    return (now.QuadPart - created.QuadPart) / 10000000ULL;
}

// --------------------------------------------------------------------- stage
int ReadStage() {
    const std::wstring path = TempFile(L"esx-native-probe-stage.txt");
    FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"rb") != 0 || !file) return 0;
    char text[32]{};
    const size_t read = fread(text, 1, sizeof(text) - 1, file);
    fclose(file);
    text[read] = '\0';
    int stage = atoi(text);
    if (stage < 0) stage = 0;
    if (stage > kMaxStage) stage = kMaxStage;
    return stage;
}

// --------------------------------------------------------------------- marker
// Crash-loop guard. A restart loop means several Explorer processes start in a
// few seconds; the count works regardless of the process role, which matters
// because the shell has no taskbar window yet when the mod is initialised.
int RecentStartCount() {
    FILE* file = nullptr;
    if (_wfopen_s(&file, LocalAppDataFile(L"probe-starts.txt").c_str(), L"rb") != 0 ||
        !file) {
        return 0;
    }
    const ULONGLONG now = GetTickCount64();
    int count = 0;
    char line[64]{};
    while (fgets(line, sizeof(line), file)) {
        unsigned long pid = 0;
        unsigned long long tick = 0;
        if (sscanf_s(line, "%lu %llu", &pid, &tick) == 2 && tick <= now &&
            now - tick < static_cast<ULONGLONG>(kLoopWindowSeconds) * 1000ULL) {
            ++count;
        }
    }
    fclose(file);
    return count;
}

void RecordStart() {
    const std::wstring path = LocalAppDataFile(L"probe-starts.txt");

    // Keep the file bounded: read the tail, rewrite only the last lines.
    std::string keep;
    {
        FILE* file = nullptr;
        if (_wfopen_s(&file, path.c_str(), L"rb") == 0 && file) {
            std::string all;
            char buffer[256]{};
            while (fgets(buffer, sizeof(buffer), file)) all += buffer;
            fclose(file);
            size_t start = 0;
            int newlines = 0;
            for (size_t i = all.size(); i > 0; --i) {
                if (all[i - 1] == '\n' && ++newlines > 20) {
                    start = i;
                    break;
                }
            }
            keep = all.substr(start);
        }
    }

    FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"wb") != 0 || !file) return;
    fputs(keep.c_str(), file);
    fprintf(file, "%lu %llu\n", GetCurrentProcessId(), GetTickCount64());
    fclose(file);
}

// --------------------------------------------------------------------- process
bool OwnsTaskbar() {
    HWND tray = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (!tray) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(tray, &pid);
    return pid == GetCurrentProcessId();
}

BOOL CALLBACK CountFrameProc(HWND hwnd, LPARAM param) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != GetCurrentProcessId()) return TRUE;
    wchar_t className[64]{};
    if (!GetClassNameW(hwnd, className, ARRAYSIZE(className))) return TRUE;
    if (wcscmp(className, L"CabinetWClass") == 0) {
        ++*reinterpret_cast<int*>(param);
    }
    return TRUE;
}

int CountOwnFrameWindows() {
    int count = 0;
    EnumWindows(CountFrameProc, reinterpret_cast<LPARAM>(&count));
    return count;
}

// --------------------------------------------------------------------- scan
BOOL CALLBACK ScanProc(HWND hwnd, LPARAM) {
    ++g_scanWindows;
    if (GetWindowTextLengthW(hwnd) > 0) ++g_scanTitled;
    return TRUE;
}

void ScanWindows() {
    g_scanWindows = 0;
    g_scanTitled = 0;
    EnumWindows(ScanProc, 0);
}

// --------------------------------------------------------------------- hook
// Thread that owns the window receiving the keys: the taskbar in the shell
// process, or the frame window in a separate one.
struct FindUiThreadContext {
    DWORD pid;
    DWORD tid;
};

BOOL CALLBACK FindUiThreadProc(HWND hwnd, LPARAM param) {
    auto* context = reinterpret_cast<FindUiThreadContext*>(param);
    DWORD pid = 0;
    if (GetWindowThreadProcessId(hwnd, &pid) == 0 || pid != context->pid) return TRUE;
    wchar_t className[64]{};
    if (!GetClassNameW(hwnd, className, ARRAYSIZE(className))) return TRUE;
    if (wcscmp(className, L"CabinetWClass") != 0 &&
        wcscmp(className, L"Shell_TrayWnd") != 0) {
        return TRUE;
    }
    context->tid = GetWindowThreadProcessId(hwnd, nullptr);
    return FALSE;
}

DWORD FindUiThread() {
    FindUiThreadContext context{GetCurrentProcessId(), 0};
    EnumWindows(FindUiThreadProc, reinterpret_cast<LPARAM>(&context));
    return context.tid;
}

LRESULT CALLBACK KeyboardProc(int code, WPARAM wParam, LPARAM lParam) {
    if (g_stage >= 4) {
        // Thread-scoped WH_KEYBOARD: lParam is not a KBDLLHOOKSTRUCT pointer.
        if (code == HC_ACTION && (wParam == VK_RETURN || wParam == VK_F3)) {
            Log("thread key vk=%lu", static_cast<unsigned long>(wParam));
        }
        return CallNextHookEx(nullptr, code, wParam, lParam);
    }
    if (code == HC_ACTION && (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN)) {
        const auto* info = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lParam);
        if (info && (info->vkCode == VK_RETURN || info->vkCode == VK_F3 ||
                     info->vkCode == 'F')) {
            Log("key vk=%lu ctrl=%d", info->vkCode,
                (GetAsyncKeyState(VK_CONTROL) & 0x8000) ? 1 : 0);
        }
    }
    return CallNextHookEx(nullptr, code, wParam, lParam);
}

// --------------------------------------------------------------------- worker
DWORD WINAPI Coordinator(LPVOID);

// Stage 8: a thread that does nothing at all, not even logging. It exists only
// to separate "an extra thread" from "what the thread does".
DWORD WINAPI BareThread(LPVOID) {
    for (;;) {
        if (WaitForSingleObject(g_stopEvent, 1000) == WAIT_OBJECT_0) break;
    }
    return 0;
}

// Stage 9: create the worker only once the shell has settled. The timer is a
// thread-pool timer, so no thread is created during Explorer's startup.
VOID CALLBACK SettleTimerProc(PVOID, BOOLEAN) {
    Log("settle timer fired uptime=%llus", ProcessUptimeSeconds());
    if (!g_thread) {
        g_thread = CreateThread(nullptr, 0, Coordinator, nullptr, 0, nullptr);
        Log("settle timer created worker=%p", static_cast<void*>(g_thread));
    }
}

DWORD WINAPI Coordinator(LPVOID) {
    Log("coordinator start stage=%d", g_stage);

    if (g_stage == 6) {
        Log("deferred start: waiting for the UI to settle");
        for (;;) {
            if (WaitForSingleObject(g_stopEvent, 500) == WAIT_OBJECT_0) {
                Log("deferred start aborted");
                return 0;
            }
            if (ProcessUptimeSeconds() >= 40 &&
                (OwnsTaskbar() || CountOwnFrameWindows() > 0)) {
                break;
            }
        }
        Log("deferred start: proceeding uptime=%llus", ProcessUptimeSeconds());
    }

    const bool useCom = (g_stage != 5 && g_stage != 7);
    if (useCom) {
        const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        Log("CoInitializeEx hr=0x%08lX", static_cast<unsigned long>(hr));
    } else if (g_stage == 5) {
        Log("stage 5: no COM apartment");
    }

    const bool useQueue = (g_stage != 7);
    MSG message{};
    if (useQueue) {
        PeekMessageW(&message, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
    } else {
        Log("stage 7: no message queue");
    }

    if (g_stage == 2 || g_stage == 6) {
        g_keyboardHook = SetWindowsHookExW(WH_KEYBOARD_LL, KeyboardProc, nullptr, 0);
        Log("LL keyboard hook=%p err=%lu", static_cast<void*>(g_keyboardHook),
            GetLastError());
    } else if (g_stage >= 4) {
        Log("thread hook: waiting for the owning window");
    }

    ULONGLONG lastScan = 0;
    ULONGLONG lastReport = 0;
    ULONGLONG lastHookTry = 0;
    bool loggedHookWait = false;
    ULONGLONG lastBeat = GetTickCount64();
    Log("beat stage=%d uptime=%llus", g_stage, ProcessUptimeSeconds());

    for (;;) {
        if (useQueue) {
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                if (message.message == WM_QUIT) goto done;
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            if (WaitForSingleObject(g_stopEvent, 0) == WAIT_OBJECT_0) break;
        } else {
            if (WaitForSingleObject(g_stopEvent, 200) == WAIT_OBJECT_0) break;
        }

        const ULONGLONG now = GetTickCount64();
        if (now - lastBeat >= 5000) {
            lastBeat = now;
            Log("beat stage=%d uptime=%llus", g_stage, ProcessUptimeSeconds());
        }

        if (g_stage == 4 && !g_keyboardHook && now - lastHookTry >= 500) {
            lastHookTry = now;
            const DWORD uiThread = FindUiThread();
            if (uiThread) {
                g_keyboardHook =
                    SetWindowsHookExW(WH_KEYBOARD, KeyboardProc, nullptr, uiThread);
                Log("thread keyboard hook=%p tid=%lu err=%lu",
                    static_cast<void*>(g_keyboardHook), uiThread, GetLastError());
            } else if (!loggedHookWait) {
                loggedHookWait = true;
                Log("thread hook: no owning window yet");
            }
        }

        if (g_stage == 3) {
            if (now - lastScan >= 16) {
                lastScan = now;
                ScanWindows();
            }
            if (now - lastReport >= 1000) {
                lastReport = now;
                Log("scan windows=%d titled=%d", g_scanWindows, g_scanTitled);
            }
        }

        if (useQueue) {
            MsgWaitForMultipleObjectsEx(0, nullptr, 16, QS_ALLINPUT,
                                        MWMO_INPUTAVAILABLE);
        }
    }

done:
    if (g_keyboardHook) {
        UnhookWindowsHookEx(g_keyboardHook);
        g_keyboardHook = nullptr;
        Log("keyboard hook removed");
    }
    if (useCom) CoUninitialize();
    Log("coordinator exit");
    return 0;
}

}  // namespace

// --------------------------------------------------------------------- exports
STDAPI DllGetClassObject(REFCLSID, REFIID, LPVOID FAR* object) {
    if (object) *object = nullptr;
    return CLASS_E_CLASSNOTAVAILABLE;
}

STDAPI DllCanUnloadNow(void) {
    return S_OK;
}

extern "C" BOOL Wh_ModInit() {
    g_shellProcess = OwnsTaskbar();
    g_stage = ReadStage();
    g_ownFrames = CountOwnFrameWindows();

    wchar_t exe[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe, ARRAYSIZE(exe));

    const int recentStarts = RecentStartCount();
    const bool safeMode = recentStarts >= kLoopStarts;

    Log("init pid=%lu shell=%d frames=%d stage=%d recentStarts=%d safe=%d uptime=%llus exe=%ls",
        GetCurrentProcessId(), g_shellProcess ? 1 : 0, g_ownFrames, g_stage,
        recentStarts, safeMode ? 1 : 0, ProcessUptimeSeconds(), exe);

    RecordStart();

    if (safeMode) {
        Log("SAFE MODE: %d Explorer starts within %ld s, probe disabled",
            recentStarts, kLoopWindowSeconds);
        return TRUE;
    }

    if (g_stage <= 0) {
        Log("stage 0: log only, no thread");
        return TRUE;
    }

    g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!g_stopEvent) {
        Log("event creation failed err=%lu", GetLastError());
        return FALSE;
    }
    if (g_stage == 8) {
        Log("stage 8: creating bare thread");
        g_thread = CreateThread(nullptr, 0, BareThread, nullptr, 0, nullptr);
        if (!g_thread) {
            Log("bare thread creation failed err=%lu", GetLastError());
            return FALSE;
        }
        return TRUE;
    }

    if (g_stage == 9) {
        Log("stage 9: arming 60 s timer, no startup thread");
        if (!CreateTimerQueueTimer(&g_timer, nullptr, SettleTimerProc, nullptr,
                                   60000, 0, WT_EXECUTEONLYONCE)) {
            Log("timer creation failed err=%lu", GetLastError());
            return FALSE;
        }
        return TRUE;
    }

    g_thread = CreateThread(nullptr, 0, Coordinator, nullptr, 0, nullptr);
    if (!g_thread) {
        Log("thread creation failed err=%lu", GetLastError());
        CloseHandle(g_stopEvent);
        g_stopEvent = nullptr;
        return FALSE;
    }
    return TRUE;
}

extern "C" void Wh_ModUninit() {
    Log("Wh_ModUninit");
    if (g_timer) {
        DeleteTimerQueueTimer(nullptr, g_timer, INVALID_HANDLE_VALUE);
        g_timer = nullptr;
    }
    if (g_stopEvent) SetEvent(g_stopEvent);
    if (g_thread) {
        WaitForSingleObject(g_thread, INFINITE);
        CloseHandle(g_thread);
        g_thread = nullptr;
    }
    if (g_stopEvent) {
        CloseHandle(g_stopEvent);
        g_stopEvent = nullptr;
    }
}
