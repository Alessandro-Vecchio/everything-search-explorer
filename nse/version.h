#pragma once

// Single source for the shell component version.
//
// Bump these when the DLL changes; the mod carries the same value in
// ESX_COMPONENT_VERSION and uses it to tell whether an update is installed.
// The same numbers feed nse/version.rc, so Explorer's file properties agree.

#define ESX_VERSION_MAJOR 0
#define ESX_VERSION_MINOR 3
#define ESX_VERSION_PATCH 5
#define ESX_VERSION_BUILD 0

#define ESX_VERSION_STRING "0.3.5"
#define ESX_VERSION_WSTRING L"0.3.5"
