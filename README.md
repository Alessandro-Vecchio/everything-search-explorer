# Everything Search for Explorer

> **⚠️ WORK IN PROGRESS — NOT A FINISHED PRODUCT.**
> This repository is a construction site published to keep track of experiments
> and collect feedback. The mod manipulates Explorer's XAML visual tree through
> **XAML Diagnostics**, an unsupported API: a Windows update can break it at any
> time. No warranty, no support, no auto-update. Use it at your own risk,
> preferably on a test machine.
>
> This project is **not affiliated with or supported by voidtools**. "Everything"
> is a voidtools trademark; it is used here only as an external search engine.
>
> [Italiano](README.it.md)

Instant file search with [Everything](https://www.voidtools.com/) **inside**
File Explorer windows: the native search box is replaced in Explorer's XAML
visual tree, with no overlay and no extra window. Results are a real folder
served by a COM namespace extension, so icons, context menus, drag & drop and
properties remain Explorer's native ones.

## Components

| Component | Runs in | Role |
|---|---|---|
| `mod/everything-explorer-search.wh.cpp` | Windhawk mod in `explorer.exe` | replaces the XAML box, writes the request, navigates the view |
| `nse/EverythingSearchNse.dll` | in-proc in `explorer.exe` | `IShellFolder` enumerating results as real files |
| Everything | separate user process | index and search engine (prerequisite) |

The NSE **does not link or load `Everything64.dll`**: it speaks the documented
IPC protocol (`QUERY2` over `WM_COPYDATA`) from a dedicated thread with timeouts,
so a slow or missing Everything never blocks Explorer.

```
Ctrl+F / XAML box → Enter
        │
        ├─ mod: writes request-<HWND>.txt  (%LOCALAPPDATA%\EverythingExplorerSearch)
        └─ mod: BrowseObject / Refresh to shell:::{7B39...4F60}
                        │
                 NSE in explorer.exe ──WM_COPYDATA/QUERY2──▶ Everything.exe
                        │
                 results = PIDLs of real files in the Explorer view
```

## Usage

1. In an Explorer window press `Ctrl+F` (or `Ctrl+E` / `F3`) to focus the
   replaced search box.
2. Type a query and press **Enter**: the view switches to the
   `Everything Search` folder with name, type, size and date (folders included).
3. Use the **Search options** button in the command bar to filter.
4. **Back** returns to the starting folder.

The mod keeps separate state per tab: box text, caret and results of one tab are
not adopted by another.

### Search options

Every entry writes Everything modifiers to the request file and applies them to
the next query; they do not appear in the text box.

| Entry | Effect |
|---|---|
| All subfolders | `path:"folder"` (recursive) |
| Current folder | `parent:"folder"` (direct children only) |
| Date modified | `date-modified:today` / `7days` / `30days` |
| Type | `doc:` / `image:` / `video:` / `audio:` |
| Size | `size:0` / `size:0..10kb` / `size:10kb..100kb` |
| System files | `attributes:s` |
| Compressed folders (ZIP) | `zip:` |
| File contents | `content:<query>` (can be very slow) |

`Current folder` is available only when a real folder exists to scope to: it
stays disabled in virtual folders such as "This PC".

## Requirements

- Windows 11 x64 (the only tested environment), Explorer with separate processes:

  ```powershell
  Set-ItemProperty 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\Advanced' SeparateProcess 1 -Type DWord
  ```

  This is required because XAML Diagnostics is available only in `/factory`
  processes.
- Windhawk with the `local@everything-explorer-search` mod enabled.
- PowerShell 7+ to run the scripts (`#requires -Version 7.0`); Windows PowerShell
  5.1 is not enough.
- Everything 1.4.1+ running **non-elevated** (otherwise `explorer.exe` cannot send
  it `WM_COPYDATA`; use the Everything service).
- The namespace extension DLL installed (see below).

## Initial setup (from a clean clone)

1. **Tools** (once):

   ```powershell
   winget install Microsoft.VisualStudio.2022.BuildTools --override "--wait --passive --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
   winget install Windhawk.Windhawk
   winget install voidtools.Everything
   ```

   You also need the **Windows SDK** (for `cppwinrt.exe`) and the WinUI WinMD
   files in the NuGet cache. If they are not there yet, populate them with a
   temporary project:

   ```powershell
   mkdir "$env:TEMP\esx-winmd"; cd "$env:TEMP\esx-winmd"
   dotnet new classlib -f net8.0
   dotnet add package Microsoft.WindowsAppSDK
   dotnet add package Microsoft.Web.WebView2
   dotnet add package Microsoft.Windows.SDK.NET.Ref
   dotnet restore
   ```

   `mod\regenerate-winrt.ps1` finds the paths automatically and generates the
   headers. If a WinMD is missing, the script says which package to install.

2. **Configure Explorer** with `SeparateProcess=1` (command in *Requirements*),
   then restart Explorer.

3. **Build the NSE** (CMake + Ninja, already in the Build Tools):

   ```powershell
   .\build-nse.ps1
   ```

   This produces `dist\bin\EverythingSearchNse.dll`.

4. **Register the NSE**: `.\install-nse.ps1 -RestartExplorer`.

5. **Build the Windhawk mod**: `.\mod\build-mod.ps1`. On the first run it
   generates the C++/WinRT headers by itself and produces
   `dist\windhawk\local@everything-explorer-search_0.5.0_<build>.dll`.

6. **Install the mod**: `.\install-mod.ps1` (asks for admin) or load the DLL from
   Windhawk's panel.

7. **Quick check**: `.\tools\open-results.ps1 -Query "*.ps1" -Global` opens the results
   folder without going through the mod. The end-to-end probe
   `.\tests\run-mingw.ps1` requires MinGW-w64: it uses `g++.exe` from `PATH`,
   falls back to `$env:TEMP\mingw\mingw64\bin\g++.exe`, or accepts `-Gcc <path>`.

## Installation

```powershell
.\build-nse.ps1                    # NSE: builds dist\bin\EverythingSearchNse.dll
.\install-nse.ps1                  # NSE: copies the DLL to %LOCALAPPDATA% and registers the HKCU CLSID
.\mod\build-mod.ps1            # mod: builds the .wh.cpp into dist\windhawk
.\install-mod.ps1              # mod: registers in HKLM, restarts Windhawk (admin)
```

`install-mod.ps1` uses `mod\build-mod.ps1`, which produces
`dist\windhawk\local@everything-explorer-search_0.5.0_<build>.dll`. The NSE DLL
folder is versioned because a DLL already mapped by Explorer cannot be
overwritten. To remove everything: `.\uninstall-mod.ps1` and `.\uninstall-nse.ps1`.

Architecture and protocol details: `docs/DIRECT-XAML-REPLACEMENT.md` and
`docs/DESIGN.md` (the latter is partly historical: for the current state the
README and the code win).

## Build and test

```powershell
.\mod\build-mod.ps1                          # Windhawk mod (MSVC x64)
.\tests\run-mingw.ps1 -Folder C:\Windows -Query '*.exe' -Max 20   # NSE without MSVC + end-to-end probe
```

The C++/WinRT headers are **not in the repo**: `mod\regenerate-winrt.ps1`
recreates them from the Windows SDK and NuGet when missing (~30 MB, 254 files).
The packages `Microsoft.WindowsAppSDK.*`, `Microsoft.Web.WebView2` and
`Microsoft.Windows.SDK.NET.Ref` must be in the NuGet cache; to force
regeneration:

```powershell
.\mod\regenerate-winrt.ps1 -Force
```

The mod log goes to `%TEMP%\everything-explorer-search.log`. To inspect a
problem, run `.\tools\diagnose.ps1`: it decodes every request file, flags broken
combinations (such as a scope bit with an empty folder) and shows the related
log events for the same window. Open problems are tracked in
`docs/KNOWN-ISSUES.md`.

```powershell
.\tools\open-results.ps1 -Query "invoice" -Folder "C:\Users\<user>\Documents"
```

`open-results.ps1` writes the request and opens the virtual folder without the mod:
useful for testing the NSE alone.

## Known limitations / open issues

This is the important part: the project **is not ready**.

- **`Current folder` is not yet reliable**: if the second search starts inside
  the results view, the origin folder can be lost and the modifier becomes
  `parent:""` (no results). See the comment in the code in `ApplySearchOptions`
  / the `KeyDown` handler.
- After a second search with many results, the new list may appear only after
  the full re-enumeration finishes.
- The legacy "Organize" bar can reappear in normal folders of the same window
  after a search.
- The window/tab title stays `Everything Search` instead of
  `<query> - Search Results in ...` (Explorer does not call `GetDisplayNameOf`
  on the root).
- The desktop `Everything Search` icon can reappear intermittently: it looks
  like an icon-cache artifact, not a real namespace folder.
- `SeparateProcess=1` is required; without separate windows the native bar is
  not touched. XAML Diagnostics can conflict with other mods or Windows updates.
- x64 / Windows 11 only. Virtual folders have no filesystem path and are
  therefore searched globally.
- Flat, read-only view: `BindToObject` is not implemented, so folders in the
  results do not navigate into the flat list.
- Every search re-enumerates and loses the selection; there is no live update
  while typing (press Enter). Safety ceiling of ~393,000 raw results
  (2048 × 192 batches).
- Results depend on Everything's index: non-indexed volumes and network shares
  without folder indexing/ETP do not appear.
- No auto-update, no code signing, no support.

## Structure

```
shared/protocol.h        request format (mod ↔ NSE), flags and filters
shared/everything_ipc.h  minimal QUERY2 protocol definitions
nse/                     namespace extension + IPC client
mod/                     Windhawk mod: XAML replacement of the native bar
mod/regenerate-winrt.ps1 generates the C++/WinRT headers (not committed)
tests/                   NSE probes, raw IPC, live menu/view tests
tools/open-results.ps1            test trigger without the mod
tools/diagnose.ps1       correlates the mod log with per-tab request files
docs/DEBUGGING.md        crash-test methodology and lessons learned
docs/KNOWN-ISSUES.md     structured ledger of open problems
```

## License

MIT — see `LICENSE`. Generated C++/WinRT headers and Windhawk headers remain
the property of their respective owners; their notices are in the files.
