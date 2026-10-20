<#
.SYNOPSIS
    Print the folder name of every open VS Code / VS Code Insiders window, one per line.

.DESCRIPTION
    `Get-Process ... MainWindowTitle` reports only one window per Electron process, so this
    enumerates every visible top-level window with Win32 EnumWindows instead. VS Code's default
    title is `<editor> - <rootName> - <appName>`; every " - "-separated token of a VS Code title
    is printed, so a worktree is open when its folder leaf name is in the output.

    Assumes `window.title` still contains `${rootName}`. If the user customised it, the output
    will not contain folder names: say so rather than re-opening duplicate windows.

.EXAMPLE
    pwsh -NoProfile -File .claude/skills/resume-worktrees/scripts/open-vscode-folders.ps1
#>
$ErrorActionPreference = 'Stop'

$sig = @'
using System; using System.Collections.Generic; using System.Runtime.InteropServices; using System.Text;
public class OloWin {
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumWindowsProc cb, IntPtr l);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern int GetWindowTextLength(IntPtr h);
  public delegate bool EnumWindowsProc(IntPtr h, IntPtr l);
  public static List<string> Titles() {
    var list = new List<string>();
    EnumWindows((h, l) => {
      if (IsWindowVisible(h)) { int len = GetWindowTextLength(h);
        if (len > 0) { var sb = new StringBuilder(len + 1); GetWindowText(h, sb, sb.Capacity); list.Add(sb.ToString()); } }
      return true; }, IntPtr.Zero);
    return list; } }
'@
if (-not ('OloWin' -as [type])) { Add-Type -TypeDefinition $sig }

[OloWin]::Titles() |
    Where-Object { $_ -match 'Visual Studio Code' } |
    ForEach-Object { $_ -split ' - ' } |
    Where-Object { $_ -and $_ -notmatch 'Visual Studio Code' } |
    Sort-Object -Unique
