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

  PARENT WATCH. That last guarantee has a hole: a build whose *launching session* dies keeps
  running, keeps the lock, and keeps eating ~47 GiB, with nobody left who will ever stop it.
  That happened here — a build outlived the editor window that started it and held the lock
  ~45 minutes past the point its work was merged. So the build runs as a child process while
  this script polls the process that launched us; if that process is gone for -ParentGracePolls
  consecutive polls, the whole build tree is killed and the lock released. The launcher is
  pinned by (pid, StartTime), not pid alone, because Windows recycles pids and a recycled
  parent would read as alive forever. Every ambiguous reading (no parent recorded, StartTime
  unreadable) fails OPEN — a spurious kill of a live build is worse than a missed orphan.
  A stronger version of this would put the child in a Windows Job Object with
  KILL_ON_JOB_CLOSE, which would also cover *this* script being killed; that needs P/Invoke
  and has not been worth it yet.

  Bypassing the lock is no longer merely discouraged: a PreToolUse hook in .claude/settings.json
  (scripts/claude-build-lock-guard.py) denies any agent tool call that starts a build without
  going through this script. A human typing `cmake --build` in their own terminal still
  bypasses it, which is fine — the problem being solved is unattended agent sessions colliding.

.PARAMETER Command
  The build command to run under the lock. Its exit code is what this script returns.

.PARAMETER TimeoutMinutes
  How long to wait for the lock before giving up. Default 180 (longer than the slowest observed
  local full build, so a legitimate queue never fails).

.PARAMETER NoParentWatch
  Disable the orphan cleanup described above and let the build outlive its launcher. For a build
  deliberately detached from the session that started it.

.PARAMETER ParentGracePolls
  Consecutive polls the launching process must be absent before the build is killed. Default 2,
  so ~10s at the default -ParentPollSeconds — long enough that a momentary read failure does not
  kill a healthy build.

.EXAMPLE
  pwsh -File build-lock.ps1 -Command 'cmake --build build --target OloEngine-Tests --config Debug --parallel 6'
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string] $Command,
    [int] $TimeoutMinutes = 180,
    [int] $PollSeconds    = 15,
    [switch] $NoParentWatch,
    [int] $ParentPollSeconds = 5,
    [int] $ParentGracePolls  = 2
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

# --- parent watch -----------------------------------------------------------
# Identity of the process that launched us, so an orphaned build can be cleaned
# up. Captured as (pid, StartTime): Windows recycles pids, and a recycled parent
# would read as alive forever. $null disables the watch — every reading we cannot
# trust fails OPEN, because killing a healthy build is worse than missing an
# orphaned one.
function Get-ParentIdentity {
    try {
        $self = Get-CimInstance Win32_Process -Filter "ProcessId=$PID" -ErrorAction Stop
        $parentPid = [int] $self.ParentProcessId
        if ($parentPid -le 0) { return $null }
        $parent = Get-Process -Id $parentPid -ErrorAction Stop
        return [pscustomobject]@{ ProcessId = $parentPid; StartTime = $parent.StartTime }
    } catch {
        return $null
    }
}

function Test-ParentAlive([object] $Identity) {
    if ($null -eq $Identity) { return $true }
    $parent = $null
    try { $parent = Get-Process -Id $Identity.ProcessId -ErrorAction Stop } catch { return $false }
    # StartTime can throw on a process we may not query. Unreadable is not proof
    # of death, so treat it as alive.
    try { return $parent.StartTime -eq $Identity.StartTime } catch { return $true }
}

# Depth-first, so a killed node cannot spawn another child on the way out. Each
# level is a FILTERED CIM query: enumerating every process on this box takes
# minutes when a build is running.
function Stop-ProcessTree([int] $RootPid) {
    if ($RootPid -le 0) { return }
    # Both failures below are EXPECTED during a teardown race — a compiler that
    # exits on its own between the enumeration and the kill is the normal case, not
    # an error. They are reported rather than swallowed so a kill that fails for a
    # real reason (access denied on an elevated child) leaves a trace instead of a
    # silently surviving process.
    $children = @()
    try { $children = @(Get-CimInstance Win32_Process -Filter "ParentProcessId=$RootPid" -ErrorAction Stop) }
    catch { Write-Verbose "[build-lock] could not enumerate children of ${RootPid}: $($_.Exception.Message)" }
    foreach ($child in $children) { Stop-ProcessTree ([int] $child.ProcessId) }
    try { Stop-Process -Id $RootPid -Force -ErrorAction Stop }
    catch { Write-Verbose "[build-lock] could not stop ${RootPid} (already gone?): $($_.Exception.Message)" }
}

$parentIdentity = if ($NoParentWatch) { $null } else { Get-ParentIdentity }

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
        $payload = @{ pid       = $me
                      parentPid = if ($null -ne $parentIdentity) { $parentIdentity.ProcessId } else { 0 }
                      acquired  = (Get-Date).ToString('o')
                      worktree  = $here
                      command   = $Command } | ConvertTo-Json -Compress
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

# The build runs as a CHILD process rather than inline, so the parent watch gets
# to look between waits. $Command reaches it through a temp script file: it
# routinely carries quotes, semicolons and `&&`, and re-quoting that into an
# argument string is exactly how quoting bugs get introduced.
#
# Consequence worth knowing: the child inherits our stdout/stderr HANDLES, so
# OS-level redirection (`... > build.log 2>&1`) still captures the build output,
# but an in-process PowerShell pipeline (`... | Tee-Object`) no longer sees it.
$runner     = [System.Diagnostics.Process]::GetCurrentProcess().MainModule.FileName
$scriptFile = Join-Path ([System.IO.Path]::GetTempPath()) "olo-build-lock-$me.ps1"
Set-Content -LiteralPath $scriptFile -Encoding UTF8 -Value @"
`$global:LASTEXITCODE = 0
$Command
exit `$LASTEXITCODE
"@

$exit     = 1
$orphaned = $false
try {
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName         = $runner
    $psi.Arguments        = "-NoProfile -File `"$scriptFile`""
    $psi.WorkingDirectory = $here
    $psi.UseShellExecute  = $false          # inherit this process's console handles
    $child = [System.Diagnostics.Process]::Start($psi)

    $missedPolls = 0
    while (-not $child.WaitForExit($ParentPollSeconds * 1000)) {
        if (Test-ParentAlive $parentIdentity) { $missedPolls = 0; continue }
        $missedPolls++
        if ($missedPolls -lt $ParentGracePolls) { continue }

        Write-Host "[build-lock] the process that launched this build (pid=$($parentIdentity.ProcessId)) is gone — killing the orphaned build tree and releasing the lock"
        $orphaned = $true
        Stop-ProcessTree $child.Id
        [void] $child.WaitForExit(30000)
        break
    }
    # An orphaned build did not succeed, whatever the kill happened to report.
    if (-not $orphaned -and $child.HasExited) { $exit = $child.ExitCode }
} finally {
    # Releasing IS closing the handle — only the owner can do it, by construction.
    # The file itself is left in place; the next acquirer truncates and rewrites it.
    $lock.Dispose()
    Write-Host "[build-lock] released (pid=$me)"
    Remove-Item -LiteralPath $scriptFile -Force -ErrorAction SilentlyContinue
}

# Propagate the BUILD's status, never the release's — see task-loop.md Phase 2.
exit $exit
