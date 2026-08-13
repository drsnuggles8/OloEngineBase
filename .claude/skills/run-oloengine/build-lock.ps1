<#
.SYNOPSIS
  Cross-worktree build mutex. Runs one build at a time across every OloEngine worktree.

.DESCRIPTION
  One clean Debug build peaks at ~47 GiB on this 64 GB host (issue #759), so two concurrent
  builds do not fit — and CLAUDE.md's "never build the msvc and clangcl trees at the same time"
  rule is currently enforced only by each agent checking for live MSBuild/ninja processes at
  whatever moment it happens to look. Two sessions checking simultaneously both see "clear".

  This makes the rule mechanical. The lock lives in the shared git-common-dir, which every
  worktree resolves to the same path by construction, so it serialises across worktrees without
  any coordination between the sessions.

  OWNERSHIP IS THE OS FILE HANDLE, not the file's existence. The holder keeps an exclusive
  write handle open for the whole build (FileShare.Read, so waiters can still read the metadata
  to report who is building). Acquisition is therefore a single atomic operation: either the
  open succeeds and you own the lock, or it fails and you queue.

  This replaces an earlier read-then-judge-then-delete scheme, which had two races:
    * a waiter could read the lock file during the holder's write window, see empty/partial
      JSON, judge it "stale" and delete a live lock — putting two builds side by side;
    * the check and the Remove-Item were separate steps, so "is it stale?" and "take it" were
      not atomic.
  With a held handle neither exists: nothing outside the owner can release it. A crashed or
  killed holder releases automatically, because Windows closes handles on process exit — so
  there is no dead-PID special case to get wrong. A *hung but alive* holder keeps the lock
  until -TimeoutMinutes expires, which is deliberate: silently stealing from a live build is
  the failure this script exists to prevent, so the waiter reports and stops instead.

  Advisory only: a human typing `cmake --build` directly bypasses it. That is fine — the
  problem being solved is unattended agent sessions colliding.

.PARAMETER Command
  The build command to run under the lock. Its exit code is what this script returns.

.PARAMETER TimeoutMinutes
  How long to wait for the lock before giving up. Default 180 (longer than the slowest observed
  local full build, so a legitimate queue never fails).

.EXAMPLE
  pwsh -File build-lock.ps1 -Command 'cmake --build build --target OloEngine-Tests --config Debug --parallel 6'
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string] $Command,
    [int] $TimeoutMinutes = 180,
    [int] $PollSeconds    = 15
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# --- locate the shared lock -------------------------------------------------
# git-common-dir is the ONE directory every worktree of this repo shares.
$commonDir = (git rev-parse --path-format=absolute --git-common-dir 2>$null)
if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace($commonDir)) {
    throw "not inside a git repository — cannot locate the shared lock directory"
}
$lockPath = Join-Path $commonDir.Trim() 'olo-build.lock'
$me       = $PID
$here     = (Get-Location).Path

# Read the holder's metadata for reporting. Never throws; a null result just means
# "held, but we could not read who by" — it is NEVER grounds to take the lock.
function Read-HolderInfo {
    try {
        $fs = [System.IO.File]::Open($lockPath, [System.IO.FileMode]::Open,
                                     [System.IO.FileAccess]::Read,
                                     [System.IO.FileShare]::ReadWrite)
        try {
            $sr = New-Object System.IO.StreamReader($fs)
            return $sr.ReadToEnd() | ConvertFrom-Json
        } finally { $fs.Dispose() }
    } catch { return $null }
}

# Single atomic acquire: an exclusive write handle. FileShare.Read lets waiters read the
# metadata but NOT take ownership. Returns the open FileStream, or $null if held.
function Get-LockHandle {
    try {
        $fs = [System.IO.File]::Open($lockPath, [System.IO.FileMode]::OpenOrCreate,
                                     [System.IO.FileAccess]::Write,
                                     [System.IO.FileShare]::Read)
    } catch { return $null }
    try {
        $fs.SetLength(0)                                   # discard a previous holder's record
        $payload = @{ pid = $me; acquired = (Get-Date).ToString('o'); worktree = $here; command = $Command } |
                   ConvertTo-Json -Compress
        $bytes = [System.Text.Encoding]::UTF8.GetBytes($payload)
        $fs.Write($bytes, 0, $bytes.Length)
        $fs.Flush()
        return $fs
    } catch {
        $fs.Dispose()
        return $null
    }
}

# --- acquire ----------------------------------------------------------------
$deadline  = (Get-Date).AddMinutes($TimeoutMinutes)
$announced = $false
$lock      = $null

while ($null -eq ($lock = Get-LockHandle)) {
    if (-not $announced) {
        $info = Read-HolderInfo
        if ($null -ne $info) {
            Write-Host "[build-lock] waiting — held by pid=$($info.pid) in $($info.worktree)"
        } else {
            Write-Host "[build-lock] waiting — held (holder metadata unreadable)"
        }
        $announced = $true
    }
    if ((Get-Date) -gt $deadline) {
        $info = Read-HolderInfo
        $who  = if ($null -ne $info) { "pid=$($info.pid) ($($info.worktree))" } else { "an unidentified holder" }
        throw "[build-lock] timed out after ${TimeoutMinutes}m waiting for $who. A build that has held the lock this long is wedged — investigate it rather than overriding."
    }
    Start-Sleep -Seconds $PollSeconds
}

# --- run the build under the lock ------------------------------------------
Write-Host "[build-lock] acquired (pid=$me) -> $Command"
$exit = 1
try {
    & pwsh -NoProfile -Command $Command
    $exit = $LASTEXITCODE
} finally {
    # Releasing IS closing the handle — only the owner can do it, by construction.
    # The file itself is left in place; the next acquirer truncates and rewrites it.
    $lock.Dispose()
    Write-Host "[build-lock] released (pid=$me)"
}

# Propagate the BUILD's status, never the release's — see task-loop.md Phase 2.
exit $exit
