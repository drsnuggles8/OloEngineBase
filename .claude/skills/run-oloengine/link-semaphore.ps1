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

  WHY A KERNEL SEMAPHORE, not a lock file. A file-based scheme has to answer "is the
  holder still alive?", and every wrong answer is either a stolen permit (two linkers
  that should not overlap) or a leaked one (a permit nobody can ever take again). A
  Windows named semaphore is released by the kernel when the owning process exits,
  however it exits. There is no dead-holder case to get wrong -- the same reasoning that
  makes the build mutex a held file HANDLE rather than a file's existence.

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

$sem      = $null
$acquired = $false

if ($slots -gt 0) {
    try {
        # "Global\" would span logon sessions but needs SeCreateGlobalPrivilege; the hazard
        # is this user's own concurrent worktrees, so the per-session namespace is the right
        # scope and needs no elevation.
        $created = $false
        $sem = New-Object System.Threading.Semaphore($slots, $slots, 'OloEngine.Link.v1', ([ref] $created))
        $acquired = $sem.WaitOne([TimeSpan]::FromSeconds($timeout))
        if (-not $acquired) {
            Write-Host "[link-semaphore] timed out after ${timeout}s waiting for a permit -- linking anyway (fail-open)"
        }
    } catch {
        Write-Host "[link-semaphore] unavailable ($($_.Exception.Message)) -- linking unthrottled"
        $sem = $null
    }
}

$code = 1
try {
    $exe  = $cmd[0]
    $rest = if ($cmd.Count -gt 1) { $cmd[1..($cmd.Count - 1)] } else { @() }
    & $exe @rest
    $code = $LASTEXITCODE
} finally {
    if ($null -ne $sem) {
        # Release ONLY what we actually took. Releasing a permit we never acquired would
        # inflate the count permanently and silently raise the ceiling for everyone.
        if ($acquired) { try { [void] $sem.Release() } catch { } }
        try { $sem.Dispose() } catch { }
    }
}

exit $code
