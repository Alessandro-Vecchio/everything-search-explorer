// Out-of-process probe for the namespace extension.
//
// Loads EverythingSearchNse.dll directly (no registry needed), enumerates the
// virtual folder and prints what Explorer would see. This is how we test the
// whole chain (NSE -> IPC -> Everything) without touching Explorer.
#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shlwapi.h>

#include <cstdio>
#include <string>
#include <vector>

#include "../nse/folder.h"

namespace {

std::wstring ModuleDir() {
    wchar_t path[MAX_PATH];
    const DWORD len = GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring dir(path, len);
    const size_t slash = dir.find_last_of(L"\\/");
    if (slash != std::wstring::npos) dir.resize(slash + 1);
    return dir;
}

std::wstring StrRetToWString(STRRET& str) {
    wchar_t buffer[2048]{};
    if (FAILED(StrRetToBufW(&str, nullptr, buffer, ARRAYSIZE(buffer)))) return L"<strret failed>";
    return buffer;
}

}  // namespace

int wmain() {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool probeFailed = false;

    const std::wstring dir = ModuleDir();
    const std::wstring dllPath = dir + L"EverythingSearchNse.dll";

    HMODULE module = LoadLibraryW(dllPath.c_str());
    if (!module) {
        wprintf(L"LoadLibrary failed: %lu\n", GetLastError());
        return 1;
    }

    auto getVersion = reinterpret_cast<const wchar_t* (*)()>(
        GetProcAddress(module, "EsNseVersion"));
    if (getVersion) {
        wprintf(L"dll version: %ls\n", getVersion());
    } else {
        wprintf(L"EsNseVersion not exported\n");
    }

    auto getClassObject = reinterpret_cast<HRESULT(WINAPI*)(REFCLSID, REFIID, void**)>(
        GetProcAddress(module, "DllGetClassObject"));
    if (!getClassObject) {
        wprintf(L"DllGetClassObject not exported\n");
        return 1;
    }

    IClassFactory* factory = nullptr;
    HRESULT hr = getClassObject(CLSID_EverythingSearchFolder, IID_IClassFactory,
                                reinterpret_cast<void**>(&factory));
    if (FAILED(hr)) {
        wprintf(L"DllGetClassObject failed: 0x%08X\n", hr);
        return 1;
    }

    IShellFolder* folder = nullptr;
    hr = factory->CreateInstance(nullptr, IID_IShellFolder, reinterpret_cast<void**>(&folder));
    factory->Release();
    if (FAILED(hr) || !folder) {
        wprintf(L"CreateInstance failed: 0x%08X\n", hr);
        return 1;
    }

    IShellFolder2* folder2 = nullptr;
    folder->QueryInterface(IID_IShellFolder2, reinterpret_cast<void**>(&folder2));

    STRRET rootName{};
    if (SUCCEEDED(folder->GetDisplayNameOf(nullptr, SHGDN_NORMAL, &rootName))) {
        wprintf(L"folder: %ls\n", StrRetToWString(rootName).c_str());
    }

    IEnumIDList* enumerator = nullptr;
    hr = folder->EnumObjects(nullptr, SHCONTF_FOLDERS | SHCONTF_NONFOLDERS, &enumerator);
    if (FAILED(hr) || !enumerator) {
        wprintf(L"EnumObjects failed: 0x%08X\n", hr);
        folder->Release();
        return 1;
    }

    ULONG fetched = 0;
    LPITEMIDLIST pidl = nullptr;
    int index = 0;

    while (enumerator->Next(1, &pidl, &fetched) == S_OK && pidl) {
        STRRET name{};
        folder->GetDisplayNameOf(pidl, SHGDN_NORMAL, &name);
        const std::wstring display = StrRetToWString(name);

        STRRET full{};
        folder->GetDisplayNameOf(pidl, SHGDN_FORPARSING, &full);
        const std::wstring path = StrRetToWString(full);

        SFGAOF attributes = SFGAO_FILESYSTEM | SFGAO_CANCOPY | SFGAO_HASPROPSHEET;
        const HRESULT attributesHr =
            folder->GetAttributesOf(1, const_cast<PCUITEMID_CHILD*>(&pidl), &attributes);
        if (FAILED(attributesHr) || !(attributes & SFGAO_FILESYSTEM)) probeFailed = true;

        SFGAOF cachedAttributes = SFGAO_FILESYSTEM | SFGAO_CANCOPY | SFGAO_HASPROPSHEET;
        const HRESULT cachedAttributesHr =
            folder->GetAttributesOf(1, const_cast<PCUITEMID_CHILD*>(&pidl), &cachedAttributes);
        if (FAILED(cachedAttributesHr) || cachedAttributes != attributes) probeFailed = true;

        SHELLDETAILS size{};
        if (folder2) folder2->GetDetailsOf(pidl, 1, &size);
        SHELLDETAILS modified{};
        if (folder2) folder2->GetDetailsOf(pidl, 3, &modified);

        if (folder2) {
            // Content view reads property keys through GetDetailsEx.
            VARIANT typeText{};
            VARIANT sizeValue{};
            const HRESULT typeHr =
                folder2->GetDetailsEx(pidl, &PKEY_ItemTypeText, &typeText);
            const HRESULT sizeHr = folder2->GetDetailsEx(pidl, &PKEY_Size, &sizeValue);
            wprintf(L"    GetDetailsEx: type=0x%08X ('%ls') size=0x%08X (%llu)\n", typeHr,
                    typeHr == S_OK ? typeText.bstrVal : L"", sizeHr,
                    sizeHr == S_OK ? sizeValue.ullVal : 0);
            VariantClear(&typeText);
            VariantClear(&sizeValue);
        }

        wprintf(L"[%d] %ls\n    path: %ls\n    attrs: 0x%08X  size: %ls  modified: %ls\n", index,
                display.c_str(), path.c_str(), attributes, StrRetToWString(size.str).c_str(),
                StrRetToWString(modified.str).c_str());

        if (index == 0) {
            IContextMenu* menu = nullptr;
            const HRESULT menuHr = folder->GetUIObjectOf(
                nullptr, 1, const_cast<PCUITEMID_CHILD*>(&pidl), IID_IContextMenu, nullptr,
                reinterpret_cast<void**>(&menu));
            wprintf(L"    IContextMenu: 0x%08X\n", menuHr);
            if (FAILED(menuHr) || !menu) probeFailed = true;

            IContextMenu3* menu3 = nullptr;
            const HRESULT menu3Hr = folder->GetUIObjectOf(
                nullptr, 1, const_cast<PCUITEMID_CHILD*>(&pidl), IID_IContextMenu3, nullptr,
                reinterpret_cast<void**>(&menu3));
            wprintf(L"    IContextMenu3: 0x%08X\n", menu3Hr);
            if (FAILED(menu3Hr) || !menu3) probeFailed = true;
            if (menu3) menu3->Release();

            // Some native handlers return the full menu even for
            // CMF_DEFAULTONLY; verify a default action is still available.
            if (menu) {
                HMENU popup = CreatePopupMenu();
                const UINT first = 1;
                const int count = menu->QueryContextMenu(popup, 0, first, 0x7FFF,
                                                        CMF_DEFAULTONLY | CMF_EXPLORE);
                wprintf(L"    default menu items: %d\n", count);
                const int positions = GetMenuItemCount(popup);
                bool sawDefaultVerb = false;
                for (int pos = 0; pos < positions; ++pos) {
                    const UINT id = GetMenuItemID(popup, pos);
                    wchar_t text[128]{};
                    GetMenuStringW(popup, pos, text, ARRAYSIZE(text), MF_BYPOSITION);
                    if (id == static_cast<UINT>(-1)) {
                        wprintf(L"      (submenu) '%ls'\n", text);
                        continue;
                    }
                    wchar_t verb[128]{};
                    menu->GetCommandString(id, GCS_VERBW, nullptr,
                                           reinterpret_cast<char*>(verb), ARRAYSIZE(verb));
                    if (verb[0] != L'\0') sawDefaultVerb = true;
                    wprintf(L"      id=%u text='%ls' verb='%ls'\n", id, text, verb);
                }
                wprintf(L"    has default verb: %s\n", sawDefaultVerb ? L"yes" : L"no");
                if (!sawDefaultVerb || positions <= 0) probeFailed = true;
                DestroyMenu(popup);

                // Right click path: the full menu must still be the real file
                // menu (extensions included), not an empty one.
                popup = CreatePopupMenu();
                const int fullCount = menu->QueryContextMenu(popup, 0, first, 0x7FFF, CMF_EXPLORE);
                wprintf(L"    full menu items: %d\n", fullCount);
                if (FAILED(fullCount) || GetMenuItemCount(popup) <= 0) probeFailed = true;
                DestroyMenu(popup);
                menu->Release();
            }

            IDataObject* data = nullptr;
            const HRESULT dataHr = folder->GetUIObjectOf(
                nullptr, 1, const_cast<PCUITEMID_CHILD*>(&pidl), IID_IDataObject, nullptr,
                reinterpret_cast<void**>(&data));
            wprintf(L"    IDataObject:  0x%08X\n", dataHr);
            if (FAILED(dataHr) || !data) probeFailed = true;
            if (data) {
                FORMATETC hdropEtc{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
                FORMATETC idListEtc{
                    static_cast<CLIPFORMAT>(RegisterClipboardFormatW(L"Shell IDList Array")),
                    nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
                const HRESULT hdropHr = data->QueryGetData(&hdropEtc);
                const HRESULT shellIdHr = data->QueryGetData(&idListEtc);
                wprintf(L"    CF_HDROP: 0x%08X   ShellIDList: 0x%08X\n", hdropHr, shellIdHr);
                if (FAILED(hdropHr) || FAILED(shellIdHr)) probeFailed = true;

                STGMEDIUM medium{};
                if (SUCCEEDED(data->GetData(&hdropEtc, &medium)) && medium.hGlobal) {
                    auto* drop = static_cast<DROPFILES*>(GlobalLock(medium.hGlobal));
                    if (drop) {
                        const wchar_t* files = reinterpret_cast<const wchar_t*>(
                            reinterpret_cast<const BYTE*>(drop) + drop->pFiles);
                        wprintf(L"    dropped: fWide=%d count=%u first='%ls'\n", drop->fWide,
                                DragQueryFileW(reinterpret_cast<HDROP>(drop), 0xFFFFFFFF, nullptr, 0),
                                drop->fWide ? files : L"<ansi>");
                        GlobalUnlock(medium.hGlobal);
                    }
                    ReleaseStgMedium(&medium);
                }
                data->Release();
            }
        }

        CoTaskMemFree(pidl);
        pidl = nullptr;
        ++index;
    }

    enumerator->Release();
    if (folder2) folder2->Release();
    folder->Release();

    wprintf(L"total: %d\n", index);
    CoUninitialize();
    return probeFailed ? 1 : 0;
}
