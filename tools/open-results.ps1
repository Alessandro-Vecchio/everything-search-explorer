<#
    Triggers a search without the Windhawk mod: writes the request file and
    opens the virtual folder in a new Explorer window.

    Example:
        .\tools\open-results.ps1 -Query "fattura" -Folder "C:\Users\<utente>\Documenti"
        .\tools\open-results.ps1 -Query "*.ps1" -Global
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory, Position = 0)][string]$Query,
    [string]$Folder = (Get-Location).Path,
    [int]$MaxResults = 0,
    [switch]$Global
)

$ErrorActionPreference = 'Stop'

$guid = '{7B39C2A2-3E1F-4E4C-9E58-2D9A7B1C4F60}'
$dir = Join-Path $env:LOCALAPPDATA 'EverythingExplorerSearch'
New-Item -ItemType Directory -Force -Path $dir | Out-Null

$seq = [DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds() % 4294967296
# bit 0 = scope to folder, bit 1 = include folders.
$flags = if ($Global) { 2 } else { 3 }

$lines = @(
    "seq=$seq",
    "max=$MaxResults",
    "flags=$flags",
    "folder=$Folder",
    "query=$Query"
)
$path = Join-Path $dir 'request.txt'
[System.IO.File]::WriteAllText($path, ($lines -join "`n"), [System.Text.UTF8Encoding]::new($false))

Write-Host "Request written to $path"

# Neither "explorer.exe shell:::{GUID}" nor ShellExecute on the shell::: string
# is reliable (Explorer can fall back to the default folder or fail with
# ERROR_FILE_NOT_FOUND). Parsing the PIDL and invoking it directly always opens
# the namespace extension.
Add-Type -Namespace Esx -Name Shell -MemberDefinition @'
[StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
public struct SHELLEXECUTEINFO {
    public int cbSize;
    public uint fMask;
    public IntPtr hwnd;
    [MarshalAs(UnmanagedType.LPWStr)] public string lpVerb;
    [MarshalAs(UnmanagedType.LPWStr)] public string lpFile;
    [MarshalAs(UnmanagedType.LPWStr)] public string lpParameters;
    [MarshalAs(UnmanagedType.LPWStr)] public string lpDirectory;
    public int nShow;
    public IntPtr hInstApp;
    public IntPtr lpIDList;
    [MarshalAs(UnmanagedType.LPWStr)] public string lpClass;
    public IntPtr hkeyClass;
    public uint dwHotKey;
    public IntPtr hIconOrMonitor;
    public IntPtr hProcess;
}

[DllImport("shell32.dll", CharSet = CharSet.Unicode)]
public static extern int SHParseDisplayName(string name, IntPtr bindCtx, out IntPtr pidl,
                                            uint attributes, out uint eaten);

[DllImport("shell32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
public static extern bool ShellExecuteEx(ref SHELLEXECUTEINFO info);

[DllImport("ole32.dll")]
public static extern int CoInitializeEx(IntPtr reserved, uint flags);
'@

[void][Esx.Shell]::CoInitializeEx([IntPtr]::Zero, 2)

$pidl = [IntPtr]::Zero
$eaten = 0
$hr = [Esx.Shell]::SHParseDisplayName("shell:::$guid", [IntPtr]::Zero, [ref]$pidl, 0, [ref]$eaten)
if ($hr -ne 0 -or $pidl -eq [IntPtr]::Zero) {
    throw ("SHParseDisplayName failed: 0x{0:X8}. Is the namespace extension registered?" -f $hr)
}

# SEE_MASK_INVOKEIDLIST | SEE_MASK_IDLIST
$info = New-Object Esx.Shell+SHELLEXECUTEINFO
$info.cbSize = [System.Runtime.InteropServices.Marshal]::SizeOf($info)
$info.fMask = 0x00000004 -bor 0x00000100
$info.lpVerb = 'open'
$info.lpIDList = $pidl
$info.nShow = 1

if (-not [Esx.Shell]::ShellExecuteEx([ref]$info)) {
    throw ("ShellExecuteEx failed: {0}" -f [System.Runtime.InteropServices.Marshal]::GetLastWin32Error())
}
