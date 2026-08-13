<#
.SYNOPSIS
  Cross-worktree build mutex. Runs one build at a time across every OloEngine worktree.

.DESCRIPTION
  One clean Debug build peaks at ~47 GiB on this 64 GB host (issue #759), so two concurrent
  builds do not fit — and CLAUDE.md's "never build the msvc and clangcl trees at the same time"
  rule is currently enforced only by each agent checking for live MSBuild/ninja processes at
  whatever moment it happens to look. Two sessions checking simultaneously both see "clear".

  This makes the rule mechanical. The lock file lives in the shared git-common-dir, which every
  worktree resolves to the same path by construction, so it serialises across worktrees without
  any coordination between the sessions.

  Advisory only: a human typing `cmake --build` directly bypasses it. That is fine — the problem
  being solved is unattended agent sessions colliding.

.PARAMETER Command
  The build command to run under the lock. Its exit code is what this script returns.

.PARAMETER TimeoutMinutes
  How long to wait for the lock before giving up. Default 180 (longer than the slowest observed
  local full build, so a legitimate queue never fails).

.PARAMETER StaleMinutes
  A held lock older than this is presumed abandoned and stolen, with a warning. Default 180.

.EXAMPLE
  pwsh -File build-lock.ps1 -Command 'cmake --build build --target OloEngine-Tests --config Debug --parallel 6'
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string] $Command,
    [int] $TimeoutMinutes = 180,
    [int] $StaleMinutes   = 180,
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

function Read-Lock {
    try { Get-Content $lockPath -Raw -ErrorAction Stop | ConvertFrom-Json } catch { $null }
}

function Test-LockStale {
    param($info)
    # Must never throw: this runs inside the wait loop, and an escaping exception
    # would abort the waiter instead of queueing it — i.e. exactly the concurrent
    # build this lock exists to prevent. Anything unparseable is treated as stale.
    try {
        if ($null -eq $info) { return $true }                   # unreadable/corrupt -> steal
        if (-not (Get-Process -Id $info.pid -ErrorAction SilentlyContinue)) {
            Write-Host "[build-lock] holder pid=$($info.pid) is gone — stealing"
            return $true
        }
        # ConvertFrom-Json may hand back a [datetime] already, or the raw string.
        $acquired = $info.acquired
        if ($acquired -isnot [datetime]) {
            $acquired = [datetime]::Parse([string]$acquired,
                            [Globalization.CultureInfo]::InvariantCulture,
                            [Globalization.DateTimeStyles]::RoundtripKind)
        }
        $age = (Get-Date) - $acquired
        if ($age.TotalMinutes -gt $StaleMinutes) {
            Write-Warning "[build-lock] holder pid=$($info.pid) has held the lock for $([int]$age.TotalMinutes)m (> $StaleMinutes) — stealing"
            return $true
        }
        return $false
    } catch {
        Write-Warning "[build-lock] could not evaluate the held lock ($_) — treating as stale"
        return $true
    }
}

function Try-Acquire {
    # CreateNew is atomic: it throws if the file already exists, so two racing
    # sessions cannot both believe they acquired.
    try {
        $fs = [System.IO.File]::Open($lockPath, [System.IO.FileMode]::CreateNew,
                                     [System.IO.FileAccess]::Write, [System.IO.FileShare]::None)
    } catch { return $false }
    try {
        $payload = @{ pid = $me; acquired = (Get-Date).ToString('o'); worktree = $here; command = $Command } |
                   ConvertTo-Json -Compress
        $bytes = [System.Text.Encoding]::UTF8.GetBytes($payload)
        $fs.Write($bytes, 0, $bytes.Length)
    } finally { $fs.Dispose() }
    return $true
}

# --- acquire ----------------------------------------------------------------
$deadline = (Get-Date).AddMinutes($TimeoutMinutes)
$announced = $false
while (-not (Try-Acquire)) {
    $info = Read-Lock
    if (Test-LockStale $info) {
        Remove-Item $lockPath -Force -ErrorAction SilentlyContinue
        continue                                                # retry immediately
    }
    if (-not $announced) {
        Write-Host "[build-lock] waiting — held by pid=$($info.pid) in $($info.worktree)"
        $announced = $true
    }
    if ((Get-Date) -gt $deadline) {
        throw "[build-lock] timed out after ${TimeoutMinutes}m waiting for pid=$($info.pid) ($($info.worktree)). Investigate before overriding."
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
    # Release only if we still own it (a steal may have reassigned it).
    $held = Read-Lock
    if ($null -ne $held -and $held.pid -eq $me) {
        Remove-Item $lockPath -Force -ErrorAction SilentlyContinue
        Write-Host "[build-lock] released (pid=$me)"
    } else {
        Write-Warning "[build-lock] lock was taken from us mid-build (pid=$($held.pid)) — not releasing"
    }
}

# Propagate the BUILD's status, never the release's — see task-loop.md Phase 2.
exit $exit
