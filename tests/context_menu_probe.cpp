// Compares the context-menu contract of a real file (the shell's own menu)
// with the one our namespace extension exposes for the same file.
//
// The point is the default-action path used by DefView on double click:
// QueryContextMenu(CMF_DEFAULTONLY) must insert a command whose id Explorer can
// invoke, and InvokeCommand must then do the right thing. Guessing that
// contract is what made double click silently do nothing.
#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shlwapi.h>

#include <cstdio>
#include <string>

namespace {

std::wstring VerbOf(IContextMenu* menu, UINT id) {
    wchar_t verb[128]{};
    if (FAILED(menu->GetCommandString(id, GCS_VERBW, nullptr, reinterpret_cast<char*>(verb),
                                      ARRAYSIZE(verb)))) {
        return L"<none>";
    }
    return verb;
}

void Dump(const wchar_t* what, IContextMenu* menu, UINT flags) {
    HMENU popup = CreatePopupMenu();
    const UINT first = 1;
    const int result = menu->QueryContextMenu(popup, 0, first, 0x7FFF, flags);
    wprintf(L"%ls: flags=0x%04X return=%d positions=%d\n", what, flags, result,
            GetMenuItemCount(popup));
    const int positions = GetMenuItemCount(popup);
    for (int pos = 0; pos < positions; ++pos) {
        const UINT id = GetMenuItemID(popup, pos);
        wchar_t text[128]{};
        GetMenuStringW(popup, pos, text, ARRAYSIZE(text), MF_BYPOSITION);
        if (id == static_cast<UINT>(-1)) {
            wprintf(L"    (submenu) '%ls'\n", text);
            continue;
        }
        wprintf(L"    pos=%d id=%u text='%ls' verb='%ls'\n", pos, id, text,
                VerbOf(menu, id).c_str());
    }
    DestroyMenu(popup);
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    const std::wstring path = (argc > 1) ? argv[1] : L"C:\\Windows\\notepad.exe";

    wchar_t folderPath[MAX_PATH]{};
    wcscpy_s(folderPath, path.c_str());
    PathRemoveFileSpecW(folderPath);
    const wchar_t* leaf = PathFindFileNameW(path.c_str());

    IShellFolder* folder = nullptr;
    PIDLIST_ABSOLUTE folderPidl = nullptr;
    if (FAILED(SHParseDisplayName(folderPath, nullptr, &folderPidl, 0, nullptr)) || !folderPidl) {
        wprintf(L"SHParseDisplayName(%ls) failed\n", folderPath);
        return 1;
    }
    if (FAILED(SHBindToObject(nullptr, folderPidl, nullptr, IID_PPV_ARGS(&folder))) || !folder) {
        wprintf(L"SHBindToObject failed\n");
        return 1;
    }

    PIDLIST_RELATIVE child = nullptr;
    ULONG eaten = 0;
    if (FAILED(folder->ParseDisplayName(nullptr, nullptr, const_cast<LPWSTR>(leaf), &eaten,
                                        &child, nullptr))) {
        wprintf(L"ParseDisplayName(%ls) failed\n", leaf);
        return 1;
    }

    wprintf(L"file: %ls\n\n", path.c_str());

    const UINT flagSets[] = {CMF_DEFAULTONLY, CMF_DEFAULTONLY | CMF_EXPLORE, CMF_NORMAL};
    for (UINT flags : flagSets) {
        IContextMenu* menu = nullptr;
        const HRESULT hr = folder->GetUIObjectOf(nullptr, 1,
                                                 const_cast<PCUITEMID_CHILD*>(&child),
                                                 IID_IContextMenu, nullptr,
                                                 reinterpret_cast<void**>(&menu));
        if (FAILED(hr) || !menu) {
            wprintf(L"GetUIObjectOf failed: 0x%08X\n", hr);
            continue;
        }
        Dump(L"real shell", menu, flags);
        menu->Release();
    }

    CoTaskMemFree(child);
    folder->Release();
    CoTaskMemFree(folderPidl);
    CoUninitialize();
    return 0;
}
