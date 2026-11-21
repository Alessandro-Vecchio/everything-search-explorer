# Direct XAML Replacement (official mod 0.5)

## Status

Official implementation in `mod/everything-explorer-search.wh.cpp` (Windhawk ID
`local@everything-explorer-search`). Direct replacement of Explorer's native
search control, with no Win32 overlay and no `DesktopWindowXamlSource`.
Verified on `/separate` windows, `Ctrl+N` and windows opened from the taskbar:

- layout copied from the native control (`Grid.Row/Column`, `RowSpan/ColumnSpan`,
  `Margin`, alignments, size constraints);
- correct position to the right of the address bar, adapts to resize;
- typing, `Ctrl+F`/`Ctrl+E`/`F3`, Backspace, Esc and Enter working;
- Enter produces `request.txt` and navigation to the NSE folder with the
  correct scope;
- focus preserved after navigation;
- no dumps in the final tests.

## Architecture

1. **Passive observer** (`kObserverOnly = true`): `OnVisualTreeChange` detects
   `FileExplorerSearchBox` without touching the visual tree. The old
   `CreateSession`/`DesktopWindowXamlSource` path stays disabled.
2. **Async channel**: a message-only window (`HWND_MESSAGE`) is created in the
   watcher's constructor, on the owner thread of `IVisualTreeService3`. The
   callback only does `PostMessage`: no reentrant COM calls.
3. **Transaction in the WndProc**: `GetCollectionCount` → snapshot → `AddChild` →
   verify → `RemoveChild` of the native control → verify, all on the owner
   thread. The collection handle is obtained via `Panel.Children()` +
   `RegisterInstance`, not from `ParentChildRelation::Parent`.
4. **Layout**: read from the native `FrameworkElement` on the XAML dispatcher
   and copied onto our `TextBox` with WinRT setters (not fixed pixel values).
5. **Input**: the `TextBox` has `GotFocus`/`LostFocus`/`KeyDown` handlers. The
   low-level hook forwards only shortcuts to the channel, which queues them on
   the XAML dispatcher.
6. **Lifetime**: `kAutoRollback = false`. Restoration is explicit on
   `Wh_ModUninit`, initialization error or window close:
   `GetIInspectableFromHandle` before removal, `RegisterInstance` on that object
   to get a fresh handle, `AddChild` at the original index.

## Current flag state

- `kObserverOnly = true` — do not enable the old overlay.
- `kReplaceNative = true` — remove the native control after insertion.
- `kAutoRollback = false` — persistent replacement.
- `kCallbackProbeOnly = true` — read-only probe.

## Replacement latency

Correlation and swap must not sit behind the 2 s log timer: the coordinator
runs them on every tick (~16 ms). Correlation also uses the XAML callback
thread: if there is **exactly one** visible `CabinetWClass` window with the same
`PID + thread`, the frame is unambiguous and replacement can proceed without
waiting for UIA `FindFirst(Descendants)` (which can block the coordinator for
hundreds of ms). If ambiguous, it stays on the UIA path and the native bar is
not touched. Windows already mapped via XAML are no longer scanned with UIA.

Measurements (opening a new window, `C:\Users\<user>\Downloads`):

| Phase | Before | After |
| --- | --- | --- |
| native XAML element visible (`OBSERVE add`) | +0.88 s | +0.85 s |
| correlation + start of C3 | +2.70 s | +0.86 s |
| `C3 RESULT PASS` | +2.70 s | +1.04 s |

The initial ~0.85 s is Explorer building its UI: it cannot be eliminated
without an overlay, which is excluded by design.

## Uninitialize

`Wh_ModUninit` explicitly stops and joins the threads created by the mod
(`executor`, `advise`) instead of relying on destructors: Windhawk waits up to
80 s for no stack to contain addresses of the DLL. With explicit joins the
"Uninitializing..." queue drains in a few seconds. Also,
`CoGetInterfaceAndReleaseStream` already releases the stream: the subsequent
`m_serviceStream = nullptr` was a double `Release` and caused an access violation
on the `executor` thread; it now uses `detach()`.

## Known limitations

- `SeparateProcess = 1` is still required: only `/factory` processes expose
  XAML Diagnostics; the shell window is never hooked.
- Virtual folders (e.g. "This PC") have no filesystem path: the scope stays
  empty and the search is global, as in the old overlay version. Real folders
  remain scoped.

## Build note

The MSVC build must export `InternalWhModPtr`: Windhawk looks for it in the DLL
to inject the context used by `Wh_GetIntSetting`. Without that export the mod
crashes in `windhawk.dll+0xFD7F` as soon as it reads a setting. The old clang
build exported it automatically (`--export-all-symbols`).

[Italiano](../README.it.md)
