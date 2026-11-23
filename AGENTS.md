# AGENTS.md

Compact guidance for work in this repository. The README is the user-facing source of truth; this file is for operational details that are easy to miss.

## Environment and setup

- Windows 11 x64 only. Five scripts declare `#requires -Version 7.0` (`install-mod.ps1`, `uninstall-mod.ps1`, `mod/regenerate-winrt.ps1`, `tests/live-search-test.ps1`, `tools/diagnose.ps1`); the others also run on Windows PowerShell 5.1. Use PowerShell 7+ for consistency.
- Explorer must use `SeparateProcess=1` (XAML Diagnostics only works in `/factory` processes). Everything must run **non-elevated** or `WM_COPYDATA` IPC fails.
- The C++/WinRT headers in `mod/generated/` are **not committed** (gitignored). `mod/build-mod.ps1` regenerates them automatically when `winrt/base.h` is missing; force with `.\mod\regenerate-winrt.ps1 -Force`. Regeneration needs the Windows SDK and WinUI/WebView2/SDK.NET.Ref WinMDs in the NuGet cache (see README for the bootstrap commands).
- `mod/build-mod.ps1` discovers `vcvars64.bat` and the newest Windhawk `windhawk.lib`; `build-nse.ps1` discovers MSVC via `vswhere` and CMake (PATH, common folders, winget, or `-CMakePath`) and builds with the Visual Studio 2022 generator. Do not hardcode engine/VS versions.

## Commands worth knowing

```powershell
.\build-nse.ps1                                                  # NSE build (MSVC/CMake) -> dist\bin
.\install-nse.ps1                                                # register HKCU CLSID + copy DLL, restart Explorer
.\mod\build-mod.ps1                                              # mod build (auto-regenerates WinRT headers)
.\install-mod.ps1                                                # self-elevates, builds, registers HKLM, restarts Windhawk
.\install-mod.ps1 -SkipBuild -NoRestart                          # reuse newest DLL, no engine restart
.\tests\run-mingw.ps1 -Folder C:\Windows -Query '*.exe' -Max 20  # NSE + probes without MSVC or Explorer
.\tools\open-results.ps1 -Query "*.ps1" -Global                  # test the NSE without the mod
.\tools\diagnose.ps1                                             # decode request files + correlate the mod log
```

- `tests/run-mingw.ps1` is the only focused executable verification that runs without Explorer/Everything. It finds `g++.exe` on `PATH`, then `$env:TEMP\mingw\mingw64\bin\g++.exe`, or accepts `-Gcc <path>`. It writes a global `request.txt`, so do not run it while debugging a live per-tab search.
- `install-mod.ps1` runs elevated and rebuilds unless `-SkipBuild`; it writes the `Settings` key with `maxResults=0` and `scopeToFolder=1`. Missing `maxResults` means **zero search results**.
- No automated unit suite; verification is build + MinGW probe + manual checks in Explorer.

## Conventions and gotchas

- Windhawk metadata blocks in the `.wh.cpp` (`==WindhawkModReadme==` and `==WindhawkModSettings==`) **must contain `/* ... */` comments**, not `//` lines. With line comments the Windhawk UI silently shows no README and no settings.
- Source `@id` is `everything-search-explorer`; the installed mod key is `local@everything-search-explorer`. Changing that ID creates a new mod and resets its settings.
- `// @version` in `mod/everything-search-explorer.wh.cpp` and `ESX_VERSION_STRING` in `nse/version.h` are independent: the mod and the NSE are versioned and installed separately.
- Search handoff: `%LOCALAPPDATA%\EverythingSearchExplorer\request-<HWND>.txt` per window, plus a global `request.txt` fallback. Flags: bit 0 = scope, bit 1 = include folders, bit 2 = recursive. Scope + recursive → `path:"...\"` (trailing backslash keeps the scope strict); scope without recursive → `parent:"..."`. `max=0` means no limit.
- Mod log: `%TEMP%\everything-search-explorer.log`. `tools/diagnose.ps1` decodes flags and flags broken states such as scope set with an empty folder.
- Public docs are English-first ([README.md](README.md)); keep [README.it.md](README.it.md) aligned. [docs/DESIGN.md](docs/DESIGN.md) is partly historical - prefer README and code. [docs/KNOWN-ISSUES.md](docs/KNOWN-ISSUES.md) is the structured bug ledger; update it when opening/fixing issues.
- `build/`, `dist/`, `build_check/` and `mod/generated/` are ignored build output: never commit them. The mod's intermediate `.obj/.lib/.exp` go to `build/windhawk/` (not the repo root).
- User preference: edit → re-read → verify; build and install only when needed or explicitly requested.
- User preference: code says what it does; comments say why or clarify non-obvious behaviour. Never translate the line. Keep comments short; remove useless, redundant or stale ones. Applies to every code file.
- User preference: UI interaction tests belong to the user. Do not drive Explorer or other apps with UIAutomation/SendKeys to verify behaviour; use logs and code inspection instead, then report what the user should check.
- Never commit or push without explicit user permission for that specific action.
