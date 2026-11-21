#pragma once

#include <windows.h>

// Commands added to the results folder's context menu by IShellFolderViewCB.
// They replace the two native search commands that disappear when Explorer
// leaves its own search view ("Search options" and "Close search").
void EsxShowSearchOptions(HWND owner);
void EsxCloseSearch(HWND owner);
