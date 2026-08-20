<#
.SYNOPSIS
  Cross-worktree build gate. Admits a bounded number of builds across every OloEngine worktree.

.DESCRIPTION
  One clean Debug build peaks at ~47 GiB on this 64 GB host (issue #759) — and CLAUDE.md's
  "never build the msvc and clangcl trees at the same time" rule was previously enforced only
  by each agent checking for live MSBuild/ninja processes at whatever moment it happened to
  look. Two sessions checking simultaneously both see "clear".

  CONCURRENCY IS EARNED, NOT DEFAULT. This began as a hard 1-at-a-time mutex, and that is
  still what an ordinary `build/` tree gets. Measured 2026-08-20 across three worktrees, pure
  serialisation cost 43 minutes of waiting against 58 minutes of building — including one
  build that waited 13.8 minutes to run for 1.1. So a SECOND slot is now granted, but only to
  a build that is genuinely throttleable, and only when every current holder is too:

    * it must target the cached Ninja tree, which carries the cross-tree link semaphore
      (cmake/LinkSemaphore.cmake) and the compiler cache. The Visual Studio generator ignores
      CMAKE_<LANG>_LINKER_LAUNCHER, Ninja job pools and CMAKE_<LANG>_COMPILER_LAUNCHER alike,
      so a `build/` tree has no link bound at all — two of those is exactly the shape that was
      measured leaving this host 4.7 GiB free;
    * free memory must be at least -ConcurrentMinFreeGB;
    * and the per-build lane ceiling is divided among the active builds, so two concurrent
      builds get 6 lanes each rather than 12 — the number originally chosen for a world where
      builds could overlap.

  Every uncertainty answers NO. The cost of a wrong no is a queue wait; the cost of a wrong
  yes is an OOM that loses every session's work. -MaxConcurrent 1 restores the original
  behaviour exactly.

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
  (.claude/hooks/claude-build-lock-guard.py) denies any agent tool call that starts a build without
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

.PARAMETER Jobs
  Build parallelism, decided at ACQUIRE time rather than by the caller.
    0  (default) derive it from measured free memory — see Get-AdaptiveJobs. The value
       REPLACES whatever --parallel N / -jN the command carries, in either direction: it
       lowers a caller's -j6 when memory is tight just as readily as it raises it.
    >0 pin exactly this many jobs.
    -1 do not touch the command at all; run it verbatim.

.PARAMETER Priority
  Sort this ticket ahead of every non-priority waiter. It only changes WHOSE TURN IS NEXT —
  it never preempts a running build, because killing one throws away all its work. FIFO still
  holds within the priority band. One-shot: it applies to this invocation only, and by policy
  the user asks for it.

.PARAMETER MaxConcurrent
  Ceiling on builds running at once, machine-wide. Default 2. A slot past the first is only
  granted under the conditions above, so this is an upper bound rather than a target — most
  builds still run alone. Pass 1 to restore the original hard mutex.

.PARAMETER ConcurrentMinFreeGB
  Refuse a second concurrent build below this much free memory. Default 24, which leaves room
  for the measured two-linker working set (avg 43.4 GiB, max 55.3 of 64).

.EXAMPLE
  pwsh -File build-lock.ps1 -Command 'cmake --build build --target OloEngine-Tests --config Debug --parallel 6'

.EXAMPLE
  # The cached tree: ~3.5x faster warm, and the only kind eligible for a concurrent slot.
  pwsh -File build-lock.ps1 -Command 'cmake --build build-cached --target OloEngine-Tests --config Debug --parallel 6'
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string] $Command,
    [int] $TimeoutMinutes = 180,
    [int] $PollSeconds    = 15,
    [switch] $NoParentWatch,
    [int] $ParentPollSeconds = 5,
    [int] $ParentGracePolls  = 2,
    # 0 = derive from free memory at acquire time (see Get-AdaptiveJobs). A positive
    # value pins it. -1 disables the rewrite entirely and runs $Command verbatim.
    [int] $Jobs = 0,
    # Jump ahead of non-priority tickets. NEVER preempts a running build. One-shot:
    # it applies to this invocation only. Policy: the user asks for it.
    [switch] $Priority,
    # How many builds may run at once, machine-wide. 1 restores the original hard mutex.
    # A slot beyond the first is only ever granted under the conditions in
    # Test-ConcurrencyAdmissible — it is a ceiling, not a promise.
    [int] $MaxConcurrent = 2,
    # A second concurrent build is refused below this much free memory. 24 GiB leaves
    # room for the measured 2-linker working set (avg 43.4 GiB, max 55.3 of 64).
    [int] $ConcurrentMinFreeGB = 24
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

# --- slots ------------------------------------------------------------------
# Slot 0 keeps the original name so anything that reads the lock file (tooling, a human
# with `cat`) still finds the same path it always did. Extra slots are siblings.
#
# Each slot is an independent exclusive handle, so the SAFETY argument is unchanged from
# the single-lock design: ownership is the OS handle, a crashed holder releases
# automatically, and nothing outside the owner can release it. What changed is only HOW
# MANY handles exist — and a slot past the first is handed out under strict conditions
# (Test-ConcurrencyAdmissible), because two builds is a memory decision, not a fairness one.
if ($MaxConcurrent -lt 1) { $MaxConcurrent = 1 }
$slotPaths = @($lockPath)
if ($MaxConcurrent -gt 1) {
    foreach ($i in 1..($MaxConcurrent - 1)) {
        $slotPaths += (Join-Path $commonDir.Trim() "olo-build.slot$i.lock")
    }
}

# --- metrics ----------------------------------------------------------------
# One JSONL line per build attempt, in the same shared directory as the lock so
# every worktree appends to ONE file. This exists to answer questions we have
# been guessing at: how long builds actually take, how long we actually wait,
# how often the lock is contended at all, and how much memory is free when a
# build starts (the input any future concurrent-admission policy needs).
#
# Every failure here is swallowed. Instrumentation must never be able to fail a
# build or, worse, prevent a release — so nothing in this section throws, and
# the record is written on a best-effort basis only.
$metricsPath = Join-Path $commonDir.Trim() 'olo-build-metrics.jsonl'

function Write-BuildMetric([hashtable] $Record) {
    try {
        $Record['ts'] = (Get-Date).ToString('o')
        $line  = ($Record | ConvertTo-Json -Compress -Depth 4) + "`n"
        $bytes = [System.Text.Encoding]::UTF8.GetBytes($line)
        # FileShare.Read excludes other WRITERS, so concurrent appenders serialise
        # by retrying rather than interleaving a half-written line.
        for ($attempt = 0; $attempt -lt 5; $attempt++) {
            try {
                $fs = [System.IO.File]::Open($metricsPath, [System.IO.FileMode]::Append,
                                             [System.IO.FileAccess]::Write,
                                             [System.IO.FileShare]::Read)
                try { $fs.Write($bytes, 0, $bytes.Length); $fs.Flush() } finally { $fs.Dispose() }
                return
            } catch { Start-Sleep -Milliseconds 50 }
        }
    } catch { }
}

# --- adaptive parallelism ---------------------------------------------------
# -j6 was chosen when NOTHING serialised builds and two could overlap. The mutex now
# guarantees exclusivity, so the cap and the lock defend the SAME hazard and we pay
# for it twice. Issue #759 measured -j12 at 1.3-1.6x the speed of -j6 for only ~2 GiB
# more peak, because a handful of heavy TUs set the peak, not the lane count.
#
# Still derived from MEASURED free memory rather than a constant: this box also hosts
# gh-runners for another repository, so "the mutex is held" does not mean "the machine
# is idle". Deliberately conservative — it has been OOM-killed once, and the cost of
# guessing high is losing every session's work, while guessing low is a slower build.
function Get-AdaptiveJobs([int] $ActiveBuilds = 1) {
    $free = Get-FreeMemoryGB
    if ($null -eq $free) { return 6 }        # unknown -> the old safe default
    $cpu = [Environment]::ProcessorCount
    # ~2.5 GiB per lane is well above the measured steady state and leaves room for
    # the heavy-TU spikes that actually set the peak.
    $byMemory = [int][math]::Floor(($free - 8) / 2.5)   # keep 8 GiB for everything else
    # The 12-lane ceiling was measured for a build that owns the machine. Split it
    # across concurrent builds, which lands two of them on 6 each -- the number chosen
    # back when builds COULD overlap, and for exactly that reason. Free memory already
    # reflects a build in progress, but only what it has allocated SO FAR, so it cannot
    # anticipate the other build's peak; dividing the ceiling is what covers that.
    #
    # A build already running is NOT re-capped when a second one joins — it keeps the lanes
    # it was granted, so the pair can briefly total more than 12. That is deliberate and it
    # is safe for the reason issue #759 measured: lane count barely moves peak memory (-j12
    # cost only ~2 GiB more than -j6, because a handful of heavy TUs set the peak). What
    # actually drives the peak is LINKING, and that is bounded machine-wide by the
    # semaphore in cmake/LinkSemaphore.cmake, not by this number.
    if ($ActiveBuilds -lt 1) { $ActiveBuilds = 1 }
    $ceiling = [int][math]::Floor(12 / $ActiveBuilds)
    $jobs = [math]::Min([math]::Min($byMemory, $ceiling), $cpu)
    if ($jobs -lt 2) { return 2 }
    return $jobs
}

# Rewrites an explicit --parallel N / -jN so the adaptive value actually takes effect;
# CMAKE_BUILD_PARALLEL_LEVEL alone cannot, because an explicit flag outranks it.
# Returns the command unchanged when it carries no such flag (the env var then applies).
function Set-CommandJobs([string] $Cmd, [int] $N) {
    # ${1} not $1: a bare $1 followed by the digits of $N reads as ONE group number
    # ("$2" + "10" -> $210 -> group 21), so the braces are load-bearing.
    $out = [regex]::Replace($Cmd, '(--parallel)(\s+)(\d+)', "`${1}`${2}$N")
    $out = [regex]::Replace($out, '(?<![\w-])(-j)\s*(\d+)', "`${1}$N")
    return $out
}

# --- who may build concurrently --------------------------------------------
# Concurrency is something a build tree EARNS by being throttleable, and the Visual
# Studio generator is not: it ignores CMAKE_<LANG>_LINKER_LAUNCHER (so the cross-tree
# link semaphore never runs) AND Ninja job pools (so OLO_LINK_JOBS does nothing) AND
# CMAKE_<LANG>_COMPILER_LAUNCHER (so there is no compiler cache either). Two concurrent
# `build/` trees is precisely the measured 3-linker shape that left this host with
# 4.7 GiB free. A cached Ninja tree carries both throttles, so two of those fit.
function Test-CachedTreeCommand([string] $Cmd) {
    if ([string]::IsNullOrWhiteSpace($Cmd)) { return $false }
    return ($Cmd -match '(?i)build-cached') -or ($Cmd -match '(?i)--preset\s+\S*dev-cached')
}

# A second slot is admissible only when EVERY occupied slot is throttleable, not just
# ours. A cached tree starting alongside a Visual Studio tree is still the unsafe pair,
# and the newcomer is the only party in a position to notice.
#
# Conservative on every uncertainty: an unreadable holder record, an unknown free-memory
# reading, or our own command not being a cached tree all mean "no". The cost of a wrong
# NO is a queue wait; the cost of a wrong YES is an OOM that loses every session's work.
function Test-ConcurrencyAdmissible([int] $SlotIndex, [ref] $Reason) {
    if ($SlotIndex -eq 0) { return $true }                  # the first slot is always fine

    if (-not (Test-CachedTreeCommand $Command)) {
        $Reason.Value = 'this build does not target the cached (Ninja) tree'
        return $false
    }

    $free = Get-FreeMemoryGB
    if ($null -eq $free) {
        $Reason.Value = 'free memory could not be measured'
        return $false
    }
    if ($free -lt $ConcurrentMinFreeGB) {
        $Reason.Value = "only ${free} GB free (need ${ConcurrentMinFreeGB})"
        return $false
    }

    foreach ($p in $slotPaths) {
        if (-not (Test-Path $p)) { continue }
        $info = Read-HolderInfo $p
        if ($null -eq $info) { continue }                   # not held, or a stale record
        # Held-ness is the handle, not the file; a readable record for a slot we could
        # not open means someone IS in it.
        if (Test-SlotFree $p) { continue }
        if (-not (Test-CachedTreeCommand $info.command)) {
            $Reason.Value = "an uncached tree is already building in $($info.worktree)"
            return $false
        }
    }
    return $true
}

function Get-FreeMemoryGB {
    try {
        # FreePhysicalMemory is in KB; 1MB (1048576) converts KB -> GB.
        return [math]::Round((Get-CimInstance Win32_OperatingSystem -ErrorAction Stop).FreePhysicalMemory / 1MB, 1)
    } catch { return $null }
}

# --- fair queue (tickets) ---------------------------------------------------
# The lock itself is still the exclusive file handle — that is what guarantees
# SAFETY and nothing here weakens it. The queue only decides WHOSE TURN it is to
# attempt the handle, which is a fairness layer on top.
#
# It exists because the previous poll-and-race starved arrivals badly: measured
# on 2026-08-19, two waiters that arrived at 11:33 were still waiting at 13:55
# while one that arrived at 11:39 acquired at 13:19 and built. With a 15s race
# and six contenders, arrival order carried no weight whatsoever.
#
# There is no daemon, so every waiter must independently reach the SAME verdict
# from shared state. The rule is therefore a pure, deterministic function of the
# ticket files: sort by (enqueued, pid), and only the head attempts the handle.
#
# Everything here FAILS OPEN. An unreadable queue, a vanished ticket, a directory
# we cannot create — all fall back to racing for the handle exactly as before.
# A fairness layer must never be able to deadlock the thing it is scheduling.
$queueDir        = Join-Path $commonDir.Trim() 'olo-build-queue'
$myTicket        = $null
$myEnqueuedTicks = 0

function Get-MyStartTicks {
    try { return (Get-Process -Id $PID -ErrorAction Stop).StartTime.Ticks } catch { return $null }
}

# Both machine-read fields are stored as integer TICKS, never as date strings.
# ConvertFrom-Json silently coerces anything ISO-8601-shaped into a [DateTime],
# which broke this twice over: the liveness check compared a string against a
# DateTime (so every ticket, including our own, read as dead and got reaped), and
# the ordering key came back locale-formatted to whole seconds — unsortable and
# tie-prone. Integers survive the round trip untouched. `enqueuedText` is for
# humans reading the file and is never used for logic.
function Add-QueueTicket {
    try {
        if (-not (Test-Path $queueDir)) { New-Item -ItemType Directory -Path $queueDir -Force | Out-Null }
        $now  = Get-Date
        # Name sorts chronologically for humans; ordering is decided by the JSON.
        $path = Join-Path $queueDir ('{0}-{1}.json' -f $now.ToString('yyyyMMddHHmmssfff'), $me)
        $rec  = @{ pid           = $me
                   startTicks    = Get-MyStartTicks
                   enqueuedTicks = $now.Ticks
                   enqueuedText  = $now.ToString('o')
                   worktree      = $here
                   priority      = [bool] $Priority
                   command       = $Command } | ConvertTo-Json -Compress
        Set-Content -LiteralPath $path -Value $rec -Encoding UTF8
        $script:myEnqueuedTicks = $now.Ticks
        return $path
    } catch { return $null }
}

function Remove-QueueTicket {
    if ($null -ne $script:myTicket) {
        try { Remove-Item -LiteralPath $script:myTicket -Force -ErrorAction SilentlyContinue } catch { }
        $script:myTicket = $null
    }
}

# Live tickets ahead of us, or $null when the queue cannot be trusted (caller
# then races, i.e. old behaviour). Reaps tickets whose owner is gone — pinned by
# (pid, StartTime) because Windows recycles pids and a recycled owner would keep
# a dead ticket at the head forever, blocking every real waiter.
function Get-QueueAhead {
    $files = $null
    try { $files = @(Get-ChildItem -LiteralPath $queueDir -Filter '*.json' -File -ErrorAction Stop) }
    catch { return $null }

    $live = @()
    foreach ($f in $files) {
        $rec = $null
        # An unreadable ticket is skipped but NOT reaped: it is most likely being
        # written right now, and deleting it would drop a legitimate waiter.
        try { $rec = Get-Content -LiteralPath $f.FullName -Raw -ErrorAction Stop | ConvertFrom-Json } catch { continue }
        if ($null -eq $rec) { continue }

        $alive = $true
        try {
            $p = Get-Process -Id ([int] $rec.pid) -ErrorAction Stop
            if ($null -ne $rec.startTicks) {
                # Unreadable StartTime is not proof of death — treat as alive.
                try { $alive = ($p.StartTime.Ticks -eq [long] $rec.startTicks) } catch { $alive = $true }
            }
        } catch { $alive = $false }

        if (-not $alive) {
            try { Remove-Item -LiteralPath $f.FullName -Force -ErrorAction SilentlyContinue } catch { }
            continue
        }
        $prio = $false
        try { if ($null -ne $rec.priority) { $prio = [bool] $rec.priority } } catch { }
        $live += [pscustomobject]@{ EnqueuedTicks = [long] $rec.enqueuedTicks
                                    Pid           = [int] $rec.pid
                                    Worktree      = [string] $rec.worktree
                                    # Sort key: 0 sorts before 1, so priority first.
                                    PrioKey       = $(if ($prio) { 0 } else { 1 }) }
    }

    # Priority first, then arrival. FIFO still holds WITHIN each band, so an override
    # jumps the queue without turning the rest of it back into a lottery.
    $sorted = @($live | Sort-Object PrioKey, EnqueuedTicks, Pid)
    for ($i = 0; $i -lt $sorted.Count; $i++) {
        if ($sorted[$i].Pid -eq $me) { return $i }
    }
    return $null   # our own ticket is gone — fail open rather than wait forever
}

# True when a NEWER live ticket exists for this same worktree running the SAME command:
# that session has re-queued an identical build, so ours is a stale snapshot of a tree
# that has already moved on. Building it wastes the slot and produces a binary nobody
# wants. The newest request wins; we stand down.
#
# The command must match too. Matching on worktree ALONE was the first cut and it is
# wrong: a session legitimately queues different targets back to back (OloEngine-Tests
# then OloEditor), and superseding on worktree alone silently cancelled the first one.
# Only an identical re-queue is genuinely redundant.
#
# Cooperative by construction — one process cannot make another exit, so the superseded
# waiter notices this itself on its next poll. Fails CLOSED (returns $false): if the
# queue cannot be read we keep waiting rather than abandoning a legitimate build.
function Test-Superseded {
    # Standing down means exit 0 — the caller believes the build SUCCEEDED. So every
    # ambiguity in here must resolve to "not superseded"; a wrongly-skipped build is
    # far worse than a redundant one.
    #
    # First: if we never got a ticket, Add-QueueTicket returned $null and left
    # $myEnqueuedTicks at 0. Every real ticket's enqueuedTicks exceeds 0, so the
    # "newer than us" test below would call ANY live sibling a successor and we would
    # silently exit 0 without building. With no timestamp of our own, never stand down.
    if ($script:myEnqueuedTicks -le 0) { return $false }

    try { $files = @(Get-ChildItem -LiteralPath $queueDir -Filter '*.json' -File -ErrorAction Stop) }
    catch { return $false }

    foreach ($f in $files) {
        $rec = $null
        try { $rec = Get-Content -LiteralPath $f.FullName -Raw -ErrorAction Stop | ConvertFrom-Json } catch { continue }
        if ($null -eq $rec -or [int] $rec.pid -eq $me) { continue }
        if ([string] $rec.worktree -ne $here) { continue }
        if ([string] $rec.command -ne $Command) { continue }
        if ([long] $rec.enqueuedTicks -le $myEnqueuedTicks) { continue }

        # Liveness pinned by (pid, startTicks), as in Get-QueueAhead. Pid alone is not
        # enough — Windows recycles them, so a dead successor whose pid was reused would
        # read as alive and we would stand down for a build that will never run. Note
        # this fails the OPPOSITE way to Get-QueueAhead's reaper: there, an unreadable
        # StartTime means "assume alive" so a healthy ticket is never reaped; here it
        # means "not a confirmed successor" so a real build is never skipped.
        $successorAlive = $false
        try {
            $p = Get-Process -Id ([int] $rec.pid) -ErrorAction Stop
            if ($null -eq $rec.startTicks) {
                $successorAlive = $true          # older ticket format: pid match is all we have
            } else {
                try { $successorAlive = ($p.StartTime.Ticks -eq [long] $rec.startTicks) } catch { $successorAlive = $false }
            }
        } catch { $successorAlive = $false }

        if ($successorAlive) { return $true }
    }
    return $false
}

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
function Read-HolderInfo([string] $Path = $lockPath) {
    try {
        $fs = [System.IO.File]::Open($Path, [System.IO.FileMode]::Open,
                                     [System.IO.FileAccess]::Read,
                                     [System.IO.FileShare]::ReadWrite)
        try {
            $sr = New-Object System.IO.StreamReader($fs)
            return $sr.ReadToEnd() | ConvertFrom-Json
        } finally { $fs.Dispose() }
    } catch { return $null }
}

# Is this slot free? Probe by attempting the same exclusive open the real acquire uses,
# then immediately closing it. A file's EXISTENCE says nothing — every slot file outlives
# its holder — so the handle is the only honest answer.
#
# Inherently a snapshot: someone can take the slot a microsecond later. That is fine for
# its one caller (an admission heuristic); it is never used to decide ownership, which
# remains the single atomic open in Get-SlotHandle.
function Test-SlotFree([string] $Path) {
    try {
        $fs = [System.IO.File]::Open($Path, [System.IO.FileMode]::OpenOrCreate,
                                     [System.IO.FileAccess]::Write,
                                     [System.IO.FileShare]::Read)
        $fs.Dispose()
        return $true
    } catch { return $false }
}

# Single atomic acquire: an exclusive write handle. FileShare.Read lets waiters read the
# metadata but NOT take ownership. Returns the open FileStream, or $null if held.
function Get-SlotHandle([string] $Path) {
    try {
        $fs = [System.IO.File]::Open($Path, [System.IO.FileMode]::OpenOrCreate,
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
$waitStart = Get-Date
# Who we were first blocked behind, for reconstructing contention chains offline.
$blockedByPid      = $null
$blockedByWorktree = $null
$lastPos           = -1
# Which slot we ended up in (0 = the sole slot under the original mutex behaviour).
$slotIndex          = 0
# Announce a concurrency refusal ONCE, not on every 15s poll.
$concurrencyRefused = $false

# Take a ticket BEFORE the first attempt, so arrival order is recorded even if we
# get the lock immediately.
$myTicket          = Add-QueueTicket
$queueAheadAtStart = Get-QueueAhead

try {
    while ($null -eq $lock) {
        $ahead = Get-QueueAhead
        # The head of the queue attempts slot 0; the next $MaxConcurrent-1 waiters may
        # attempt a further slot. $null = the queue could not be trusted, so we race
        # exactly as the original implementation did.
        if ($null -eq $ahead -or $ahead -lt $MaxConcurrent) {
            for ($slot = 0; $slot -lt $slotPaths.Count; $slot++) {
                # Ask permission BEFORE opening the handle. Doing it the other way round
                # would mean holding a slot we then have to hand back, and a slot that is
                # taken-then-released churns every waiter's view of the queue.
                $reason = ''
                if (-not (Test-ConcurrencyAdmissible $slot ([ref] $reason))) {
                    if ($slot -gt 0 -and -not $concurrencyRefused) {
                        Write-Host "[build-lock] not starting a 2nd concurrent build — $reason"
                        $concurrencyRefused = $true
                    }
                    break        # slots are ordered; if slot N is inadmissible so is N+1
                }
                $lock = Get-SlotHandle $slotPaths[$slot]
                if ($null -ne $lock) { $slotIndex = $slot; break }
            }
            if ($null -ne $lock) { break }
        }

        # Re-announce whenever our position changes, so a waiting session can see
        # the queue draining instead of staring at one unchanging line. Knowing the
        # wait is bounded is most of the value of this queue.
        if (-not $announced -or ($null -ne $ahead -and $ahead -ne $lastPos)) {
            $info = Read-HolderInfo
            $posText = if ($null -eq $ahead) { 'position unknown (racing)' }
                       elseif ($ahead -eq 0)  { 'next in line' }
                       else                   { "$ahead ahead of us" }
            if ($null -ne $info) {
                Write-Host "[build-lock] waiting — $posText; held by pid=$($info.pid) in $($info.worktree)"
                if (-not $announced) { try { $blockedByPid = $info.pid; $blockedByWorktree = $info.worktree } catch { } }
            } else {
                Write-Host "[build-lock] waiting — $posText; held (holder metadata unreadable)"
            }
            $announced = $true
            if ($null -ne $ahead) { $lastPos = $ahead }
        }

        # Stand down if this worktree has queued a newer request (see Test-Superseded).
        if (Test-Superseded) {
            Write-Host "[build-lock] superseded — a newer build was queued for $here; standing down so the slot is not spent on a stale tree"
            Write-BuildMetric @{ event    = 'superseded'
                                 pid      = $me
                                 worktree = $here
                                 command  = $Command
                                 wait_s   = [math]::Round(((Get-Date) - $waitStart).TotalSeconds, 1) }
            Remove-QueueTicket
            exit 0
        }

        if ((Get-Date) -gt $deadline) {
            $info = Read-HolderInfo
            $who  = if ($null -ne $info) { "pid=$($info.pid) ($($info.worktree))" } else { "an unidentified holder" }
            Write-BuildMetric @{ event      = 'timeout'
                                 pid        = $me
                                 worktree   = $here
                                 command    = $Command
                                 wait_s     = [math]::Round(((Get-Date) - $waitStart).TotalSeconds, 1)
                                 queue_ahead_at_start = $queueAheadAtStart
                                 queue_ahead_at_end   = $ahead
                                 blocked_by_pid      = $blockedByPid
                                 blocked_by_worktree = $blockedByWorktree }
            throw "[build-lock] timed out after ${TimeoutMinutes}m waiting for $who. A build that has held the lock this long is wedged — investigate it rather than overriding."
        }
        Start-Sleep -Seconds $PollSeconds
    }
} finally {
    # Our turn is over the moment we hold the handle (or give up) — a ticket left
    # behind would stall every waiter until the reaper noticed we were gone.
    Remove-QueueTicket
}

$waitSeconds  = [math]::Round(((Get-Date) - $waitStart).TotalSeconds, 1)
$freeGbAtHold = Get-FreeMemoryGB
$holdStart    = Get-Date

# Parallelism is decided HERE, not by the caller: only at acquire time do we know what
# the machine actually looks like. -1 opts out entirely and runs $Command verbatim.
#
# How many builds are live right now, us included — a slot we cannot open is one somebody
# else is in. Recomputed here rather than reused from admission, because we may have sat
# in the queue for a while since then.
$activeBuilds = 1
foreach ($p in $slotPaths) {
    if ($p -eq $slotPaths[$slotIndex]) { continue }        # our own, already counted
    if ((Test-Path $p) -and -not (Test-SlotFree $p)) { $activeBuilds++ }
}

$effectiveCommand = $Command
$effectiveJobs    = $null
if ($Jobs -ge 0) {
    $effectiveJobs = if ($Jobs -gt 0) { $Jobs } else { Get-AdaptiveJobs $activeBuilds }
    $effectiveCommand = Set-CommandJobs $Command $effectiveJobs
    if ($effectiveCommand -ne $Command) {
        Write-Host "[build-lock] parallelism: -j$effectiveJobs (free ${freeGbAtHold} GB); rewrote the caller's flag"
    } else {
        Write-Host "[build-lock] parallelism: -j$effectiveJobs (free ${freeGbAtHold} GB) via CMAKE_BUILD_PARALLEL_LEVEL"
    }
    # Covers commands that carry no explicit flag. An explicit flag outranks it, which
    # is why the rewrite above exists as well.
    $env:CMAKE_BUILD_PARALLEL_LEVEL = "$effectiveJobs"
}

# --- run the build under the lock ------------------------------------------
$slotText = if ($slotPaths.Count -gt 1) { " slot=$slotIndex of $($slotPaths.Count), $activeBuilds building" } else { '' }
Write-Host "[build-lock] acquired (pid=$me)$slotText -> $Command"

# The compiler cache only pays off if you actually build the tree it lives in, and the
# default path of least resistance is the one that does not. Measured 2026-08-20: three
# worktrees ran 58 minutes of builds between them at a 0% hit rate, every command aimed
# at `build/`, while ccache sat at exactly the call count from the previous day. The
# Visual Studio generator ignores CMAKE_<LANG>_COMPILER_LAUNCHER, so a `build/` tree can
# never cache however the tools are configured -- it is not a misconfiguration to fix,
# it is the wrong tree. A nudge rather than a rewrite: switching trees mid-task costs one
# cold build, so it is the caller's call to make, not ours to force.
if (-not (Test-CachedTreeCommand $Command)) {
    Write-Host "[build-lock] note: this targets an uncached tree — no compiler cache (the VS generator ignores launchers), and it can never take a 2nd concurrent slot."
    Write-Host "[build-lock]       the cached tree is ~3.5x faster warm (12m04s -> 3m23s measured): cmake --preset dev-cached, then build 'build-cached'."
}

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
$effectiveCommand
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
    # Written AFTER the release so a slow/failed metrics write can never delay it.
    Write-BuildMetric @{ event    = 'build'
                         pid      = $me
                         worktree = $here
                         command  = $Command
                         wait_s   = $waitSeconds
                         hold_s   = [math]::Round(((Get-Date) - $holdStart).TotalSeconds, 1)
                         exit     = $exit
                         orphaned = $orphaned
                         contended           = $announced
                         queue_ahead_at_start = $queueAheadAtStart
                         jobs                = $effectiveJobs
                         priority            = [bool] $Priority
                         slot                = $slotIndex
                         active_builds       = $activeBuilds
                         cached_tree         = (Test-CachedTreeCommand $Command)
                         free_gb_at_acquire  = $freeGbAtHold
                         free_gb_at_release  = (Get-FreeMemoryGB)
                         blocked_by_pid      = $blockedByPid
                         blocked_by_worktree = $blockedByWorktree }
}

# Propagate the BUILD's status, never the release's — see task-loop.md Phase 2.
exit $exit
