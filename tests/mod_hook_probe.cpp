// Checks whether a Windhawk mod actually patched user32!PeekMessageW in a
// running process, by comparing the function prologue with the clean copy in
// this probe.
//
// Usage:
//   mod_hook_probe.exe <pid> [function]
//
// A patched prologue starts with a jump (E9 / FF 25) or with Windhawk's
// trampoline; a clean one looks like a normal function body.
#include <windows.h>
#include <tlhelp32.h>

#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>

namespace {

struct ModuleInfo {
    std::wstring name;
    void* base = nullptr;
};

bool Modules(DWORD pid, std::vector<ModuleInfo>& out) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snapshot == INVALID_HANDLE_VALUE) return false;

    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Module32FirstW(snapshot, &entry)) {
        do {
            out.push_back({entry.szModule, entry.modBaseAddr});
        } while (Module32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return !out.empty();
}

std::wstring OwningModule(void* address) {
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(address), &module)) {
        return {};
    }
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(module, path, MAX_PATH);
    const wchar_t* leaf = wcsrchr(path, L'\\');
    return leaf ? leaf + 1 : path;
}

void HexDump(const char* label, const unsigned char* bytes, size_t count) {
    std::printf("%-22s", label);
    for (size_t i = 0; i < count; ++i) std::printf(" %02X", bytes[i]);
    std::printf("\n");
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        std::printf("usage: mod_hook_probe.exe <pid> [function]\n");
        return 1;
    }
    const DWORD pid = wcstoul(argv[1], nullptr, 10);
    const std::string function = (argc > 2) ? std::string(argv[2], argv[2] + wcslen(argv[2]))
                                            : "PeekMessageW";

    FARPROC local = nullptr;
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32) local = GetProcAddress(user32, function.c_str());
    if (!local) {
        std::printf("cannot resolve %s\n", function.c_str());
        return 1;
    }

    const std::wstring owner = OwningModule(reinterpret_cast<void*>(local));
    std::vector<ModuleInfo> mine, theirs;
    Modules(GetCurrentProcessId(), mine);
    if (!Modules(pid, theirs)) {
        std::printf("cannot enumerate modules of pid %lu (%lu)\n", pid, GetLastError());
        return 1;
    }

    void* theirOwnerBase = nullptr;
    for (const auto& module : theirs) {
        if (_wcsicmp(module.name.c_str(), owner.c_str()) == 0) {
            theirOwnerBase = module.base;
            break;
        }
    }
    if (!theirOwnerBase) {
        std::printf("%ls is not loaded in pid %lu\n", owner.c_str(), pid);
        return 1;
    }

    void* ourOwnerBase = nullptr;
    for (const auto& module : mine) {
        if (_wcsicmp(module.name.c_str(), owner.c_str()) == 0) {
            ourOwnerBase = module.base;
            break;
        }
    }

    const auto offset = reinterpret_cast<unsigned char*>(local) -
                        reinterpret_cast<unsigned char*>(ourOwnerBase);
    auto* remote = reinterpret_cast<unsigned char*>(theirOwnerBase) + offset;

    std::printf("function:  %s!%s\n", owner.empty() ? "?" : "x", function.c_str());
    std::printf("this pid:  %p (%s)\n", local, owner.c_str());
    std::printf("target pid %lu -> %p\n", pid, remote);

    unsigned char theirs_bytes[24]{};
    unsigned char ours_bytes[24]{};
    std::memcpy(ours_bytes, reinterpret_cast<void*>(local), sizeof(ours_bytes));

    HANDLE process = OpenProcess(PROCESS_VM_READ, FALSE, pid);
    if (!process) {
        std::printf("OpenProcess failed: %lu\n", GetLastError());
        return 1;
    }
    SIZE_T read = 0;
    const BOOL ok = ReadProcessMemory(process, remote, theirs_bytes, sizeof(theirs_bytes), &read);
    CloseHandle(process);

    if (!ok) {
        std::printf("ReadProcessMemory failed: %lu\n", GetLastError());
        return 1;
    }

    HexDump("clean (this pid):", ours_bytes, sizeof(ours_bytes));
    HexDump("target:", theirs_bytes, sizeof(theirs_bytes));
    const bool patched = std::memcmp(ours_bytes, theirs_bytes, sizeof(ours_bytes)) != 0;
    std::printf("%s\n", patched ? "PATCHED (hook installed)" : "identical (no hook)");
    return patched ? 0 : 2;
}
