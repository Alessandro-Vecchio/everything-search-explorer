# Debugging notes

A short record of a real misdiagnosis, kept because the methodology generalises
to any Explorer crash investigation in this project.

## The episode

During the XAML spike, an Explorer crash-loop was initially blamed on our code.
It was actually caused by the third-party Windhawk mod
`taskbar-fluent-media-player` 1.7.0. Disabling all mods except that one
reproduced the loop; disabling it while keeping the others active stopped it.

Signature: `0xC0000005` reading address 0, always in
`Windows.UI.Xaml.dll+0x2E4126`, no frames from our code in the stack, and the
same crash with our DLL absent from the process.

Ruled out: the XAML island, XAML Diagnostics and hiding the native box, COM
apartment, message pump, `WH_KEYBOARD_LL` hook, `CoreMessaging.dll` import and
the WinAppSDK bootstrap.

## Rules for future crash tests

1. Isolate **one mod at a time**. Disabling only your own mod is not a valid
   control if other mods remain active.
2. Never treat a dump as proof of guilt just because our DLL was mapped: always
   check a control dump with our code absent.
3. Prefer `/separate` windows or `CloseMainWindow` over forced kills during
   testing. A forced kill reloads every mod and can trigger the loop by itself.

## Current state

The temporary startup kill-switch described in the original investigation is
**no longer present** in the mod. The fixes that did survive are the explicit
stop/join of the mod's threads in `Wh_ModUninit` and the `detach()` instead of a
double `Release` on the COM stream; see
[docs/DIRECT-XAML-REPLACEMENT.md](DIRECT-XAML-REPLACEMENT.md) → *Uninitialize*.
