<#
.SYNOPSIS
    Loop a GoogleTest binary under a restricted CPU-affinity mask to reproduce
    flaky, core-starvation-sensitive races locally, and capture a stack on the
    first crash or deadlock.

.DESCRIPTION
    Some races only surface on CI and never on a fat developer box: GitHub
    Actions Windows runners have very few usable cores, so the scheduler
    preempts aggressively and widens thread-interleaving windows that a 16- or
    28-core workstation almost never hits. This harness recreates that pressure
    locally by pinning the test process to a small CPU-affinity mask (default
    2 cores) while the engine's FScheduler still spawns its full worker-thread
    fleet (~one per logical core) -> heavy oversubscription and preemption.

    It runs a named gtest binary + --gtest_filter in a loop and stops on the
    first failure, classifying it as:
      * FAIL  - clean gtest assertion failure (exit 1)
      * CRASH - non-zero/SEH exit (e.g. 0xC0000005 access violation)
      * HANG  - the process is alive but produced NO new output for -StallSec
                seconds (a real deadlock; the #281 signature where the Jolt /
                scheduler threads never join). Distinct from merely slow: a
                slow-but-progressing run keeps growing its log and is only cut
                at the absolute -HardTimeoutSec cap.

    Affinity is applied at *process creation* by temporarily setting the
    harness's own ProcessorAffinity (children inherit the parent's mask at
    creation) and is also set explicitly on the child. The harness restores
    its own affinity immediately after each launch, so the host shell is never
    left throttled.

    With -CaptureStack and a discoverable cdb.exe, the first CRASH/HANG is
    re-examined under the debugger:
      * HANG  - cdb attaches to the still-alive process and dumps every
                thread's stack (~*kn) plus a full minidump, then the process
                is killed.
      * CRASH - the crashed process is already gone, so the same iteration is
                re-run a handful of times *under* cdb (sxe av) to re-catch the
                access violation with a live, symbolised stack + minidump.

    Born from issue #281 (flaky CI crash: PhysicsLayerFilteringTest SEH
    0xc0000005 in the multithreaded Jolt step). Reusable for any "only flakes
    on CI" race. See docs/agent-rules/testing-architecture.md.

.PARAMETER TestExe
    Path to the gtest binary. Defaults to the Release OloEngine-Tests build.

.PARAMETER Filter
    --gtest_filter value. Defaults to the #281 test.

.PARAMETER Iterations
    Number of loop iterations (default 1000). Each iteration is a fresh
    process (faithful to CI's one-shot). Combine with -GtestArgs
    '--gtest_repeat=N' to also pile up in-process stepping density per launch
    (far higher throughput, since process startup dominates single-shot time).

.PARAMETER AffinityMask
    CPU-affinity bitmask. Accepts decimal (3) or hex ('0x3'). Default 3 = the
    two lowest logical cores. Use 1 (0x1) for the harshest single-core squeeze.

.PARAMETER StallSec
    Declare a HANG when the process is alive but its output log has not grown
    for this many seconds (default 25). This is the deadlock detector.

.PARAMETER HardTimeoutSec
    Absolute per-iteration wall-clock cap in seconds (default 600). A run that
    keeps making progress (log growing) is allowed up to this long; only then
    is it killed and recorded as a TIMEOUT.

.PARAMETER StopOnFailure
    Stop the loop at the first failing iteration (default $true).

.PARAMETER Shuffle
    Pass --gtest_shuffle (useful for whole-suite filters).

.PARAMETER GtestArgs
    Extra arguments passed straight through to the test binary (array), e.g.
    '--gtest_repeat=500'.

.PARAMETER LogDir
    Directory for per-iteration logs and captured dumps/stacks. Passing logs
    are deleted to avoid churn; failing iterations' logs + any cdb output and
    .dmp are kept. Default <repo>/build/repro-logs. NOTE: give each concurrent
    harness instance its own -LogDir to avoid iter-NNNNN.log collisions.

.PARAMETER CaptureStack
    On the first CRASH/HANG, use cdb (if found) to capture thread stacks and a
    full minidump. Off by default (fast triage); turn on once you want a stack.

.PARAMETER CdbPath
    Explicit path to cdb.exe. If omitted, the harness searches the Windows SDK
    Debuggers and the WinDbg (MSIX) install locations.

.EXAMPLE
    # Fast triage: 2 cores, in-process density, stop on first deadlock/crash.
    scripts\repro-flaky-test.ps1 -Iterations 200 -GtestArgs '--gtest_repeat=400'

.EXAMPLE
    # Harshest squeeze + capture a stack on the first failure.
    scripts\repro-flaky-test.ps1 -AffinityMask 0x1 -Iterations 400 `
        -GtestArgs '--gtest_repeat=400' -CaptureStack
#>
[CmdletBinding()]
param(
    [string]$TestExe = "build\OloEngine\tests\Release\OloEngine-Tests.exe",
    [string]$Filter = "PhysicsLayerFilteringTest.AlphaPassesThroughBetaAndBothLandOnGround",
    [int]$Iterations = 1000,
    [string]$AffinityMask = "0x3",
    [int]$StallSec = 25,
    [int]$HardTimeoutSec = 600,
    [bool]$StopOnFailure = $true,
    [switch]$Shuffle,
    [string[]]$GtestArgs = @(),
    [string]$LogDir = "build\repro-logs",
    [switch]$CaptureStack,
    [string]$CdbPath = ""
)

$ErrorActionPreference = "Stop"

# --- Resolve repo root (parent of this script's scripts\ dir) and paths ------
$RepoRoot = Split-Path -Parent $PSScriptRoot
function Resolve-RepoPath([string]$p) {
    if ([System.IO.Path]::IsPathRooted($p)) { return $p }
    return (Join-Path $RepoRoot $p)
}
$TestExe = Resolve-RepoPath $TestExe
$LogDir  = Resolve-RepoPath $LogDir

if (-not (Test-Path $TestExe)) {
    Write-Error "Test binary not found: $TestExe`nBuild it first, e.g.:`n  cmake --build build --target OloEngine-Tests --config Release --parallel"
    exit 2
}

# --- Locate cdb.exe if stack capture was requested --------------------------
function Find-Cdb([string]$explicit) {
    if ($explicit -and (Test-Path $explicit)) { return $explicit }
    $candidates = @()
    foreach ($root in @(
        "${env:ProgramFiles(x86)}\Windows Kits\10\Debuggers\x64",
        "${env:ProgramFiles}\Windows Kits\10\Debuggers\x64")) {
        if ($root) { $candidates += (Join-Path $root "cdb.exe") }
    }
    # WinDbg (modern MSIX) ships cdb under its package dir. WindowsApps is
    # ACL-locked against listing, so resolve the install path via Get-AppxPackage.
    $pkg = Get-AppxPackage -Name "Microsoft.WinDbg" -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($pkg -and $pkg.InstallLocation) {
        $candidates += (Join-Path $pkg.InstallLocation "amd64\cdb.exe")
    }
    $cmd = Get-Command cdb.exe -ErrorAction SilentlyContinue
    if ($cmd) { $candidates += $cmd.Source }
    return ($candidates | Where-Object { $_ -and (Test-Path $_) } | Select-Object -First 1)
}

$cdb = ""
if ($CaptureStack) {
    $cdb = Find-Cdb $CdbPath
    if ($cdb) { Write-Host "cdb: $cdb" -ForegroundColor DarkCyan }
    else { Write-Warning "cdb.exe not found; CRASH/HANG will be recorded without a stack." }
}

# --- Parse + validate the affinity mask -------------------------------------
$mask = if ($AffinityMask -match '^0[xX]') { [Convert]::ToInt64($AffinityMask, 16) } else { [Convert]::ToInt64($AffinityMask) }
if ($mask -le 0) { Write-Error "AffinityMask must be a positive bitmask; got '$AffinityMask'."; exit 2 }

$self = Get-Process -Id $PID
$systemMask = [int64]$self.ProcessorAffinity
if (($mask -band $systemMask) -ne $mask) {
    Write-Error ("AffinityMask 0x{0:X} selects CPUs outside this system's available mask 0x{1:X} ({2} logical CPUs). Pick a subset." -f $mask, $systemMask, [Environment]::ProcessorCount)
    exit 2
}
$coreCount = 0; $m = $mask
while ($m -ne 0) { $coreCount += ($m -band 1); $m = $m -shr 1 }

New-Item -ItemType Directory -Force -Path $LogDir | Out-Null

# --- Build the per-iteration argument list ----------------------------------
$baseArgs = @("--gtest_filter=$Filter")
if ($Shuffle) { $baseArgs += "--gtest_shuffle" }
$baseArgs += $GtestArgs

# Point cdb at the build output so it auto-resolves the Release PDBs.
$exeDir = Split-Path -Parent $TestExe
if (-not $env:_NT_SYMBOL_PATH) { $env:_NT_SYMBOL_PATH = $exeDir }

Write-Host ""
Write-Host "=== repro-flaky-test ===" -ForegroundColor Cyan
Write-Host ("  Binary     : {0}" -f $TestExe)
Write-Host ("  Filter     : {0}" -f $Filter)
Write-Host ("  Affinity   : 0x{0:X}  ({1} of {2} logical CPUs)" -f $mask, $coreCount, [Environment]::ProcessorCount)
Write-Host ("  Iterations : {0}" -f $Iterations)
Write-Host ("  Stall/Hard : {0}s no-output -> HANG ; {1}s absolute cap" -f $StallSec, $HardTimeoutSec)
Write-Host ("  Stop on 1st: {0}" -f $StopOnFailure)
Write-Host ("  Extra args : {0}" -f ($baseArgs -join ' '))
Write-Host ("  Capture    : {0}" -f ($(if ($CaptureStack) { if ($cdb) { "cdb -> $cdb" } else { "requested, cdb NOT found" } } else { "off" })))
Write-Host ("  Logs       : {0}" -f $LogDir)
Write-Host ""

# --- Launch one test process under the affinity mask ------------------------
function Start-Pinned([string[]]$procArgs, [string]$outFile) {
    $savedAffinity = $self.ProcessorAffinity
    $p = $null
    try {
        $self.ProcessorAffinity = [IntPtr]$mask
        $p = Start-Process -FilePath $TestExe -ArgumentList $procArgs `
            -NoNewWindow -PassThru -WorkingDirectory $RepoRoot `
            -RedirectStandardOutput $outFile -RedirectStandardError "$outFile.err"
    } finally {
        $self.ProcessorAffinity = $savedAffinity
    }
    try { $p.ProcessorAffinity = [IntPtr]$mask } catch { }
    return $p
}

# --- Write a cdb command-file (avoids all shell nested-quoting issues) -------
function New-CdbScript([string[]]$lines) {
    $f = [System.IO.Path]::GetTempFileName()
    Set-Content -Path $f -Value ($lines -join "`r`n") -Encoding ascii
    return $f
}

# --- Capture all thread stacks of a still-alive (hung) process via cdb -------
# Attaches to the live (deadlocked) process, dumps every thread's stack plus a
# full minidump, then quits (terminating the target).
function Capture-Hang([int]$procId, [string]$stackFile, [string]$dumpFile) {
    if (-not $cdb) { return }
    Write-Host "        capturing hang stacks via cdb (attach $procId) ..." -ForegroundColor Yellow
    $script = New-CdbScript @(
        ".echo ===ALL THREAD STACKS (HANG)===",
        "~*kn",
        ".dump /ma $dumpFile",
        "q")
    try { & $cdb -p $procId -cf $script *> $stackFile } finally { Remove-Item $script -ErrorAction SilentlyContinue }
}

# --- Re-run an iteration under cdb to catch a CRASH (SEH av) with a stack ----
# cdb stops at the initial breakpoint and runs the command file: `g` resumes;
# on an UNHANDLED (2nd-chance) access violation cdb regains control and the
# following commands dump the faulting thread + all threads + a full minidump.
# -G makes cdb quit cleanly when the process exits normally (no av this run).
function Capture-Crash([string[]]$procArgs, [string]$stackFile, [string]$dumpFile, [int]$tries) {
    if (-not $cdb) { return $false }
    # Maximise the chance of re-hitting a rare AV per cdb try: force a high
    # in-process repeat (cdb stops at the av, so a big number just means "keep
    # stepping until it crashes" without running forever once it does).
    $cdbArgs = @($procArgs | Where-Object { $_ -notmatch '^--gtest_repeat=' })
    $cdbArgs += "--gtest_repeat=20000"
    $script = New-CdbScript @(
        "g",
        ".echo ===FAULTING THREAD===",
        "kP",
        ".echo ===ALL THREAD STACKS (CRASH)===",
        "~*kn",
        ".dump /ma $dumpFile",
        "q")
    try {
        for ($t = 1; $t -le $tries; $t++) {
            Write-Host ("        re-running under cdb to catch the crash (try {0}/{1}, repeat=20000) ..." -f $t, $tries) -ForegroundColor Yellow
            $savedAffinity = $self.ProcessorAffinity
            try {
                $self.ProcessorAffinity = [IntPtr]$mask
                & $cdb -G -cf $script $TestExe @cdbArgs *>> $stackFile
            } finally {
                $self.ProcessorAffinity = $savedAffinity
            }
            if (Test-Path $dumpFile) { return $true }
        }
    } finally { Remove-Item $script -ErrorAction SilentlyContinue }
    return $false
}

$failures = 0
$firstFailIter = 0
$startTime = Get-Date

for ($i = 1; $i -le $Iterations; $i++) {
    $iterLog = Join-Path $LogDir ("iter-{0:D5}.log" -f $i)
    $outcome = "PASS"
    $exitCode = 0

    $proc = Start-Pinned $baseArgs $iterLog

    # Poll: distinguish exit / deadlock (stalled output) / slow-but-progressing.
    $iterStart = Get-Date
    $lastSize = 0
    $lastGrow = Get-Date
    while ($true) {
        if ($proc.WaitForExit(2000)) {
            $exitCode = $proc.ExitCode
            if ($exitCode -ne 0) { $outcome = if ($exitCode -eq 1) { "FAIL" } else { "CRASH" } }
            break
        }
        $sz = 0
        try { $sz = (Get-Item $iterLog -ErrorAction SilentlyContinue).Length } catch { }
        $now = Get-Date
        if ($sz -gt $lastSize) { $lastSize = $sz; $lastGrow = $now }
        elseif (($now - $lastGrow).TotalSeconds -ge $StallSec) { $outcome = "HANG"; break }
        if (($now - $iterStart).TotalSeconds -ge $HardTimeoutSec) { $outcome = "TIMEOUT"; break }
    }

    # On a deadlock, grab stacks from the live process BEFORE killing it.
    if ($outcome -eq "HANG" -or $outcome -eq "TIMEOUT") {
        if ($CaptureStack -and $firstFailIter -eq 0) {
            Capture-Hang $proc.Id $iterLog.Replace('.log', '.stacks.txt') $iterLog.Replace('.log', '.dmp')
        }
        try { Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue } catch { }
        $proc.WaitForExit(5000) | Out-Null
    }

    # Merge stderr into the iteration log.
    if (Test-Path "$iterLog.err") {
        Add-Content -Path $iterLog -Value "`n--- stderr ---"
        Get-Content "$iterLog.err" -Raw -ErrorAction SilentlyContinue | Add-Content -Path $iterLog
        Remove-Item "$iterLog.err" -ErrorAction SilentlyContinue
    }

    if ($outcome -eq "PASS") {
        Remove-Item $iterLog -ErrorAction SilentlyContinue
        if ($i % 25 -eq 0 -or $i -eq $Iterations) {
            $elapsed = (Get-Date) - $startTime
            Write-Host ("  [{0,5}/{1}] PASS  ({2:n0}s elapsed, {3} failures so far)" -f $i, $Iterations, $elapsed.TotalSeconds, $failures)
        }
    } else {
        $failures++
        if ($firstFailIter -eq 0) { $firstFailIter = $i }
        $codeStr = switch ($outcome) {
            "CRASH"   { "exit 0x{0:X8}" -f ([uint32]($exitCode -band 0xFFFFFFFF)) }
            "HANG"    { "no output for ${StallSec}s (deadlock)" }
            "TIMEOUT" { "still running at ${HardTimeoutSec}s cap" }
            default   { "exit $exitCode" }
        }
        Write-Host ("  [{0,5}/{1}] {2}  ({3})  -> {4}" -f $i, $Iterations, $outcome, $codeStr, $iterLog) -ForegroundColor Red
        $tail = Get-Content $iterLog -Tail 20 -ErrorAction SilentlyContinue
        if ($tail) { $tail | ForEach-Object { Write-Host "        | $_" -ForegroundColor DarkGray } }

        # For a CRASH, the process is gone; try to re-catch it under cdb.
        if ($outcome -eq "CRASH" -and $CaptureStack -and $firstFailIter -eq $i) {
            $got = Capture-Crash $baseArgs $iterLog.Replace('.log', '.stacks.txt') $iterLog.Replace('.log', '.dmp') 8
            if ($got) { Write-Host ("        crash stack/dump captured: {0}" -f $iterLog.Replace('.log', '.stacks.txt')) -ForegroundColor Green }
            else { Write-Host "        could not re-catch the crash under cdb in the allotted tries." -ForegroundColor Yellow }
        }

        if ($StopOnFailure) { break }
    }
}

$elapsed = (Get-Date) - $startTime
Write-Host ""
Write-Host "=== summary ===" -ForegroundColor Cyan
Write-Host ("  Failures   : {0}" -f $failures)
if ($firstFailIter -ne 0) { Write-Host ("  First fail : iteration {0}" -f $firstFailIter) -ForegroundColor Red }
Write-Host ("  Wall time  : {0:n0}s" -f $elapsed.TotalSeconds)
Write-Host ("  Logs in    : {0}" -f $LogDir)
Write-Host ""

if ($failures -gt 0) { exit 1 } else { Write-Host "All iterations passed." -ForegroundColor Green; exit 0 }
