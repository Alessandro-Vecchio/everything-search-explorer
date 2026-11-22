# Security policy

This project injects code into `explorer.exe` and registers a shell extension
for the current user. Treat it as experimental software, not as a system
component.

- **Do not run untrusted binaries.** Released DLLs are not signed. If you
  download a build from a third party, always verify it.
- **Do not use in production environments** or on machines with sensitive data
  without first testing uninstall and recovery (`uninstall-mod.ps1`,
  `uninstall-nse.ps1`).
- **Reports**: open an issue with your Windows version, Everything version,
  mod/DLL version and reproduction steps. Do not include logs containing
  private paths or file names: the mod log goes to
  `%TEMP%\everything-search-explorer.log`.
- The most serious known weaknesses are the use of XAML Diagnostics (an
  unsupported API) and the dependency on Explorer's internal shell contract:
  they are not "security bugs" that can be fixed within the scope of this
  project.

This project is not affiliated with voidtools and receives no support from it.

[README in italiano](README.it.md)
