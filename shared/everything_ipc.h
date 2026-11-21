#pragma once

// Minimal, dependency-free declaration of Everything's IPC "QUERY2" protocol.
//
// Structures and constants mirror voidtools' Everything SDK
// (ipc/everything_ipc.h, requires Everything 1.4.1+). Only the subset this
// project uses is declared here, so the build does not depend on the SDK
// download and no third-party code runs inside explorer.exe.
//
// Protocol shape, from the SDK header:
//   - The request is a copy of Query2 followed by a null-terminated search
//     string, sent with WM_COPYDATA (dwData = kCopyDataQuery2W) to the
//     Everything notification window.
//   - The reply is a List2 followed by `numitems` Item2 entries. Each Item2
//     carries data_offset, relative to the start of the List2, pointing at a
//     packed payload: fields appear in request_flags bit order, with no
//     padding. Strings are a DWORD length in characters followed by
//     (length + 1) WCHARs.
//
// Everything stays the engine: it must be installed and running. This header
// only describes how to ask it.

#include <windows.h>

namespace esx::ipc {

inline constexpr wchar_t kWindowClass[] = L"EVERYTHING_TASKBAR_NOTIFICATION";
inline constexpr DWORD kCopyDataQuery2W = 18;

// search_flags
inline constexpr DWORD kMatchCase = 0x00000001;
inline constexpr DWORD kMatchWholeWord = 0x00000002;
inline constexpr DWORD kMatchPath = 0x00000004;
inline constexpr DWORD kRegex = 0x00000008;

// max_results: ask Everything for every match instead of a fixed number.
inline constexpr DWORD kAllResults = 0xFFFFFFFF;

// item flags
inline constexpr DWORD kItemFolder = 0x00000001;
inline constexpr DWORD kItemDrive = 0x00000002;  // drive or other root, never a usable file

// request_flags
inline constexpr DWORD kRequestName = 0x00000001;
inline constexpr DWORD kRequestPath = 0x00000002;
inline constexpr DWORD kRequestFullPathAndName = 0x00000004;
inline constexpr DWORD kRequestExtension = 0x00000008;
inline constexpr DWORD kRequestSize = 0x00000010;
inline constexpr DWORD kRequestDateCreated = 0x00000020;
inline constexpr DWORD kRequestDateModified = 0x00000040;
inline constexpr DWORD kRequestDateAccessed = 0x00000080;
inline constexpr DWORD kRequestAttributes = 0x00000100;

// sort_type
inline constexpr DWORD kSortNameAscending = 1;

#pragma pack(push, 1)

struct Query2 {
    DWORD reply_hwnd;               // 32-bit on purpose, also on x64
    DWORD reply_copydata_message;   // dwData of the reply we get
    DWORD search_flags;
    DWORD offset;                   // first result to return
    DWORD max_results;              // 0 for none, kAllResults for everything
    DWORD request_flags;
    DWORD sort_type;
    WCHAR search_string[1];         // null terminated, variable length
};

struct Item2 {
    DWORD flags;         // kItemFolder | kItemDrive
    DWORD data_offset;   // from the start of the List2
};

struct List2 {
    DWORD totitems;
    DWORD numitems;
    DWORD offset;
    DWORD request_flags;
    DWORD sort_type;
    // Item2 items[numitems]; payloads follow
};

#pragma pack(pop)

}  // namespace esx::ipc
