#pragma once

// Development tracing for the namespace extension.
//
// Explorer gives almost no diagnostics: when a folder fails to open there is no
// error anywhere. Compiling with -DESX_TRACE makes every shell entry point
// append a line to %TEMP%\esx-nse.log, which is how the view-creation path gets
// diagnosed. Release builds compile all of it out.

#ifdef ESX_TRACE

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>

inline void EsxTrace(const char* format, ...) {
    char buffer[1024]{};
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer) - 32, format, args);
    va_end(args);

    wchar_t dir[MAX_PATH]{};
    if (!::GetTempPathW(MAX_PATH, dir)) return;
    const std::wstring path = std::wstring(dir) + L"esx-nse.log";

    HANDLE file = ::CreateFileW(path.c_str(), FILE_APPEND_DATA,
                                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;

    char line[1100]{};
    const int len = snprintf(line, sizeof(line), "[%lu @%llu] %s\r\n",
                             ::GetCurrentThreadId(),
                             static_cast<unsigned long long>(::GetTickCount64()), buffer);
    if (len > 0) {
        DWORD written = 0;
        ::WriteFile(file, line, static_cast<DWORD>(len), &written, nullptr);
    }
    ::CloseHandle(file);
}

// Thread id helps tell the shell's view thread from the caller's.
#define ESX_TRACE_LOG(...) EsxTrace(__VA_ARGS__)

#else

#define ESX_TRACE_LOG(...) ((void)0)

#endif
