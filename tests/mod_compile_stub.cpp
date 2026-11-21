// Compile-only shim for the Windhawk mod.
//
// Windhawk injects the Wh_* API and compiles the .wh.cpp inside its own
// toolchain, so a normal build cannot check the mod. This stub declares the
// three API entry points the mod uses and then includes the mod source, which
// lets MinGW validate syntax and types on machines without Windhawk:
//
//   g++ -c -DUNICODE -D_UNICODE ... tests/mod_compile_stub.cpp
//
// Nothing is linked and no window is created: it is a type check, not a test.
#include <windows.h>

#include <cstdarg>
#include <cstdio>

int Wh_GetIntSetting(const wchar_t* name) {
    (void)name;
    return 0;
}

void Wh_Log(const wchar_t* format, ...) {
    (void)format;
}

BOOL Wh_SetFunctionHook(void* target, void* hook, void** original) {
    (void)target;
    (void)hook;
    if (original) *original = nullptr;
    return TRUE;
}

#include "../mod/everything-explorer-search.wh.cpp"
