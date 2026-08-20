<#
  Cross-worktree link throttle. Wraps ONE linker invocation in a global semaphore.

  Linking the full static engine is the memory spike, not compiling. Measured 2026-08-19
  by deliberately running several worktrees' builds concurrently with the build mutex
  bypassed, sampling every 15s:

      1 concurrent linker  -> avg 41.2 GiB in use
      2 concurrent linkers -> avg 43.4 GiB, max 55.3     peak COMPILERS across the run: 2
      3 concurrent linkers -> 59.1 GiB, 4.7 GiB free     (+11.3 GiB inside one 15s sample)

  Three linkers took a 64 GB host to within 4.7 GiB of nothing while the peak compiler
  count was only two -- so the spike is not about compile lanes at all.

  CMake's OLO_LINK_JOBS Ninja job pool already caps this, but it is scoped to ONE BUILD
  TREE. N concurrent trees get up to N x OLO_LINK_JOBS linkers and the pool cannot see
  it. That is the gap this closes: the semaphore is named at the OS level, so every
  worktree, every build tree and every generator contend for the SAME permits.

  WHY N NAMED MUTEXES, not one named semaphore, and not a lock file. A file-based scheme
  has to answer "is the holder still alive?", and every wrong answer is either a stolen
  permit (two linkers that should not overlap) or a leaked one (a permit nobody can ever
  take again). A kernel object avoids that -- but only a MUTEX does, because only a mutex
  has an OWNER.

  A Windows semaphore's count is not owned by anyone: `ReleaseSemaphore` is never
  implicit, so a holder that is killed never gives its permit back. The first version of
  this script used one semaphore and a test appeared to show the permit being reclaimed
  after a hard kill -- but that test had only ONE holder, so killing it dropped the last
  handle, the kernel destroyed the object, and the next process created a fresh one with
  a full count. MEASURED with a second process keeping the object alive, the permit
  leaked: the next linker waited out the entire fail-open timeout. That is not
  hypothetical here, because build-lock.ps1 kills whole build trees (Stop-ProcessTree)
  when it detects an orphaned build, so killed linkers are a normal code path. Each one
  would have burned a permit permanently, silently degrading the throttle to nothing.

  A mutex is owned by the thread that took it. If that thread dies without releasing, the
  next waiter is handed the mutex along with an AbandonedMutexException -- acquisition
  SUCCEEDS and is merely flagged. So N mutexes give N permits with automatic reclamation,
  which is the property a semaphore only appeared to have.

  FAILS OPEN, ALWAYS. If the semaphore cannot be created, or the wait times out, the link
  runs anyway. A throttle that can fail a build is worse than no throttle: unthrottled
  means "slower, maybe tight on memory", while a stuck throttle means "no build in any
  worktree ever completes again".

  NO param() BLOCK ON PURPOSE. Linker command lines are full of tokens PowerShell's
  parameter binder would try to interpret -- `-o`, `/OUT:...`, and a bare `--` (which
  binds as an ambiguous parameter NAME and kills the invocation). A script with no
  param() receives every token verbatim in $args, which is the only reliable way to
  forward an arbitrary command. Configuration therefore comes from the environment:

      OLO_LINK_SEMAPHORE_SLOTS     permits; default 2, <=0 disables the throttle
      OLO_LINK_SEMAPHORE_TIMEOUT   seconds to wait before linking anyway; default 1800

  Used as a CMake linker launcher, not by hand -- see cmake/LinkSemaphore.cmake.
#>

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Continue'

$cmd = @($args)
# Tolerate a leading `--` if some caller inserts one; we never require it.
if ($cmd.Count -gt 0 -and $cmd[0] -eq '--') { $cmd = @($cmd[1..($cmd.Count - 1)]) }
if ($cmd.Count -eq 0) {
    Write-Error "[link-semaphore] no linker command was passed"
    exit 1
}

function Get-IntSetting([string] $Name, [int] $Default) {
    $raw = [Environment]::GetEnvironmentVariable($Name)
    if ([string]::IsNullOrWhiteSpace($raw)) { return $Default }
    $parsed = 0
    if ([int]::TryParse($raw, [ref] $parsed)) { return $parsed }
    return $Default
}

$slots   = Get-IntSetting 'OLO_LINK_SEMAPHORE_SLOTS'   2
$timeout = Get-IntSetting 'OLO_LINK_SEMAPHORE_TIMEOUT' 1800

# WaitAny caps the handle count -- a limit the old single-semaphore design did not have,
# so it must be handled rather than inherited. Past the cap the wait THROWS and we fall
# through to "unavailable", i.e. every link runs unthrottled: a nonsense-high value would
# silently disable the throttle rather than loosen it.
#
# 63, not 64: the documented limit is 64, but on an STA thread it is 64 minus one reserved
# handle, and PowerShell hosts do run STA. Measured -- clamping to 64 still threw
# "must be less than or equal to 63". Any value near this is already far past useful; the
# measured safe ceiling on this host is 2.
if ($slots -gt 63) {
    Write-Host "[link-semaphore] OLO_LINK_SEMAPHORE_SLOTS=$slots exceeds the wait-handle limit -- clamping to 63"
    $slots = 63
}

$mutexes  = @()
$acquired = -1          # index of the permit we hold; -1 = none

if ($slots -gt 0) {
    try {
        # "Global\" would span logon sessions but needs SeCreateGlobalPrivilege; the hazard
        # is this user's own concurrent worktrees, so the per-session namespace is the right
        # scope and needs no elevation.
        for ($i = 0; $i -lt $slots; $i++) {
            $created = $false
            $mutexes += New-Object System.Threading.Mutex($false, "OloEngine.Link.v2.$i", ([ref] $created))
        }
        # WaitAny takes the FIRST free permit rather than queueing on a particular one, so
        # permits stay interchangeable and no linker waits behind a specific slot.
        try {
            $acquired = [System.Threading.WaitHandle]::WaitAny($mutexes, [TimeSpan]::FromSeconds($timeout))
        } catch [System.Threading.AbandonedMutexException] {
            # The previous owner died without releasing. THE MUTEX IS OURS -- an abandoned
            # wait is a successful acquisition that also reports the abandonment. This is
            # the whole reason for mutexes over a semaphore; treat it as a normal acquire.
            $acquired = $_.Exception.MutexIndex
            if ($acquired -lt 0) {
                # Documented to carry the index for a WaitAny, but if it ever does not we
                # cannot know which mutex we hold, and releasing the wrong one would be
                # worse than not throttling. Fail open.
                Write-Host "[link-semaphore] a permit was abandoned but its index is unknown -- linking unthrottled"
            } else {
                Write-Host "[link-semaphore] reclaimed permit $acquired from a process that died holding it"
            }
        }
        if ($acquired -eq [System.Threading.WaitHandle]::WaitTimeout) {
            $acquired = -1
            Write-Host "[link-semaphore] timed out after ${timeout}s waiting for a permit -- linking anyway (fail-open)"
        }
    } catch {
        Write-Host "[link-semaphore] unavailable ($($_.Exception.Message)) -- linking unthrottled"
        $acquired = -1
    }
}

$code = 1
try {
    $exe  = $cmd[0]
    $rest = if ($cmd.Count -gt 1) { $cmd[1..($cmd.Count - 1)] } else { @() }
    # Distinguishing "the linker ran and returned N" from "the linker never started" needs
    # a sentinel: on a launch failure $LASTEXITCODE keeps whatever it held before, which
    # can easily be 0 -- reporting SUCCESS for a link that never happened. Clearing it
    # first makes "still null" mean "no native command ran".
    $global:LASTEXITCODE = $null
    & $exe @rest
    $ranOk = $?
    if ($null -eq $LASTEXITCODE) {
        # Nothing native executed. $? is then the only signal we have.
        $code = if ($ranOk) { 0 } else { 1 }
    } else {
        # A real exit code, propagated verbatim -- NOT collapsed via $?, which is merely
        # $false for any non-zero status and would turn a 42 into a 1.
        $code = $LASTEXITCODE
    }
} catch {
    # `&` on a missing executable throws rather than setting an exit code.
    Write-Host "[link-semaphore] failed to launch '$($cmd[0])': $($_.Exception.Message)"
    $code = 1
} finally {
    if ($acquired -ge 0) {
        # Release ONLY the permit we actually took, and only from the thread that took it
        # (a mutex may not be released by another thread). Releasing one we never held
        # throws, and releasing the wrong index would hand a permit to nobody.
        try { $mutexes[$acquired].ReleaseMutex() } catch { }
    }
    foreach ($m in $mutexes) { try { $m.Dispose() } catch { } }
}

exit $code
