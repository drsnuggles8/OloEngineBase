---
description: Re-open VS Code Insiders windows for every incomplete OloEngine worktree not already open (path-derived)
argument-hint: <branch-slug, optional — omit to resume ALL incomplete worktrees>
---
Re-open the editor windows for in-progress OloEngine worktrees — e.g. after a reboot
closed every VS Code Insiders window. Opens one window per worktree that is **still
incomplete** AND **not already open**. Opening windows is non-destructive, so just do it
(after showing the plan) — no confirmation needed.

## 0. Locate the base repo (derive it, don't assume a path)
    git rev-parse --path-format=absolute --git-common-dir
The parent of that .git directory is the BASE REPO; call it $BASE. Run git with
`git -C $BASE ...`. Then refresh so merge/ancestor checks are accurate:
    git -C $BASE fetch --prune origin

## 1. Enumerate worktrees and classify each
    git -C $BASE worktree list --porcelain
Skip the MAIN worktree (the entry whose path == $BASE, branch master) — that's the base
repo, not worktree work. For every OTHER worktree, classify with the SAME gate
`/cleanup-worktree` uses:
  - **completed** = ALL of: branch merged into master
    (`git -C $BASE merge-base --is-ancestor <branch> origin/master`, exit 0)
    AND working tree clean (`git -C <wtPath> status --porcelain` empty)
    AND nothing unpushed (`git -C $BASE rev-list --count origin/master..<branch>` == 0).
  - **incomplete (RESUME candidate)** = anything that fails that gate: uncommitted changes,
    unmerged commits, or an open PR.
**Note the same trap as cleanup:** a worktree whose branch HEAD is an ancestor of master
but whose working tree is DIRTY is live WIP (the feature was never committed) → it counts
as **incomplete** and SHOULD be resumed. Completed worktrees are just awaiting cleanup —
skip them (mention them so I can run `/cleanup-worktree`).
If a slug ("$1") was given, restrict the candidate set to feature/$1 only.

## 2. Detect which worktrees are ALREADY open
`Get-Process … MainWindowTitle` is unreliable for Electron (it reports only one window).
Enumerate ALL top-level windows via Win32 EnumWindows and match each worktree's folder
leaf name against the window-title tokens (VS Code's default title is
`<editor> - <rootName> - <appName>`, separator " - ", so `<rootName>` = the folder leaf
is an exact token). This catches both `code` ("Visual Studio Code") and `code-insiders`
("Visual Studio Code - Insiders"):
```powershell
$sig = @'
using System; using System.Collections.Generic; using System.Runtime.InteropServices; using System.Text;
public class Win {
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumWindowsProc cb, IntPtr l);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern int GetWindowTextLength(IntPtr h);
  public delegate bool EnumWindowsProc(IntPtr h, IntPtr l);
  public static List<string> Titles() {
    var list = new List<string>();
    EnumWindows((h,l) => {
      if (IsWindowVisible(h)) { int len = GetWindowTextLength(h);
        if (len > 0) { var sb = new StringBuilder(len+1); GetWindowText(h, sb, sb.Capacity); list.Add(sb.ToString()); } }
      return true; }, IntPtr.Zero);
    return list; } }
'@
Add-Type -TypeDefinition $sig
$openTitles = [Win]::Titles() | Where-Object { $_ -match 'Visual Studio Code' }
# A worktree at <wtPath> is OPEN if its leaf name is a " - "-delimited token of any title:
function Test-WorktreeOpen($wtPath, $titles) {
  $leaf = Split-Path $wtPath -Leaf
  foreach ($t in $titles) { if (($t -split ' - ') -contains $leaf) { return $true } }
  return $false
}
```
Assumption: the user hasn't customized `window.title` to drop `${rootName}`. If detection
looks wrong, say so rather than reopening a duplicate.

## 3. Compute the set to open and SHOW it
to_open = incomplete worktrees that are NOT already open. Print a short table: each
worktree → status (incomplete-dirty / incomplete-unmerged / incomplete-openPR / completed)
and open? (yes/no), then the final to_open list. If to_open is empty, report that
everything incomplete is already open (or everything is completed) and stop.

## 4. Open each in its own new window
The user runs VS Code Insiders; `-n` forces a NEW window so existing windows survive:
    code-insiders -n "<wtPath>"
Fall back to `code -n "<wtPath>"` if `code-insiders` isn't on PATH. Open them one per
worktree in to_open.

## 5. Report
List what was opened, what was skipped because already-open, and what was skipped because
completed (suggest `/cleanup-worktree` for those). If any opened worktree has a
`HANDOVER.md` at its root (left by `/start-work`), mention it and hand over the kickoff
prompt so the user can prime that window's Claude:
    Read ./HANDOVER.md and continue the task it describes.
