#pragma once

// Single source for the shell component version.
//
// Bump these when the DLL changes; install.ps1 uses the version for the
// per-version install folder and the registry state.
// The same numbers feed nse/version.rc, so Explorer's file properties agree.

#define ESX_VERSION_MAJOR 0
#define ESX_VERSION_MINOR 3
#define ESX_VERSION_PATCH 6
#define ESX_VERSION_BUILD 0

#define ESX_VERSION_STRING "0.3.6"
#define ESX_VERSION_WSTRING L"0.3.6"
