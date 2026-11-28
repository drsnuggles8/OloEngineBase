<#
.SYNOPSIS
    The long lifecycle soak (issue #1796): a fresh-process control, then many
    cycles of the editor and cooked-runtime sequences in one process, on one or
    both backends, with a JSON report per leg.

.DESCRIPTION
    The per-PR smoke is the same test with its defaults (one warm-up cycle, two
    measured cycles) and runs in the ordinary suite. This script is the long
    run, for a renderer, scene-lifecycle or memory change, and before a packaged
    KESTREL build (#1759). docs/testing/lifecycle-soak.md has the coverage
    manifest, what the report holds and who owns the run.

    For each backend it starts two processes of the test binary:

      1. The control: opens A once in a fresh process and writes its settled
         frame (--olo-lifecycle-soak-control-out), one per leg.
      2. The soak: -WarmUp + -Cycles cycles of LifecycleSoak.*Sequence* in one
         process, compared at the end against that fresh process's A
         (--olo-lifecycle-soak-control-in), with the replay data written to
         <OutDir>/soak_<leg>_<backend>.json.

    Then it runs the negative control (LifecycleSoak.MissingActivationResetIsCaught)
    once per backend, so a report from a run whose comparison could not fail is
    never read as green.

    The exit code is non-zero when any process failed. Run it on a quiet GPU:
    the frame times are measured, and a sibling compile is contention.

.PARAMETER Cycles
    Measured cycles after warm-up, 2 to 100000 (default 25): one cannot show growth.

.PARAMETER WarmUp
    Warm-up cycles, not judged for growth (default 2).

.PARAMETER Rhi
    gl, vulkan or both (default both).

.PARAMETER Exe
    The test binary (default build-cached/OloEngine/tests/Debug/OloEngine-Tests.exe).

.PARAMETER OutDir
    Where the controls and reports go (default build-cached/lifecycle-soak). Git-ignored
    build output, never committed.

.EXAMPLE
    pwsh -NoProfile -File scripts/lifecycle-soak.ps1 -Cycles 25 -Rhi both
#>
[CmdletBinding()]
param(
    [ValidateRange(2, 100000)][int]$Cycles = 25,
    [ValidateRange(0, 1000)][int]$WarmUp = 2,
    [ValidateSet('gl', 'vulkan', 'both')][string]$Rhi = 'both',
    [string]$Exe = '',
    [string]$OutDir = ''
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
if (-not $Exe) { $Exe = Join-Path $repo 'build-cached/OloEngine/tests/Debug/OloEngine-Tests.exe' }
if (-not $OutDir) { $OutDir = Join-Path $repo 'build-cached/lifecycle-soak' }
# Relative paths mean the caller's directory; each process runs from the repo
# root, so resolve both before anything changes directory.
$Exe = [System.IO.Path]::GetFullPath($Exe, (Get-Location).Path)
$OutDir = [System.IO.Path]::GetFullPath($OutDir, (Get-Location).Path)
if (-not (Test-Path $Exe)) { throw "No test binary at $Exe; build OloEngine-Tests through build-lock.ps1 first." }
New-Item -ItemType Directory -Force $OutDir | Out-Null

$backends = if ($Rhi -eq 'both') { @('gl', 'vulkan') } else { @($Rhi) }
$failures = [System.Collections.Generic.List[string]]::new()

function Invoke-Soak([string]$label, [string[]]$arguments) {
    $log = Join-Path $OutDir "$label.log"
    Write-Host "[lifecycle-soak] $label : $Exe $($arguments -join ' ')"
    # The test binary runs from the repo root; the fixture enters the editor
    # root (or the staged game directory) itself.
    # A sharding environment inherited from a ctest or CI caller would hand
    # this process an empty shard of the filter, which gtest passes.
    $savedShards = $env:GTEST_TOTAL_SHARDS, $env:GTEST_SHARD_INDEX
    $env:GTEST_TOTAL_SHARDS = $null
    $env:GTEST_SHARD_INDEX = $null
    Push-Location $repo
    try {
        & $Exe @arguments *> $log
        $code = $LASTEXITCODE
    }
    finally {
        Pop-Location
        $env:GTEST_TOTAL_SHARDS, $env:GTEST_SHARD_INDEX = $savedShards
    }
    Get-Content $log | Select-String -Pattern '^\[LifecycleSoak\]|^  (cycle|GPU|process|after restart)|^\[  (FAILED|PASSED|SKIPPED) ' |
        ForEach-Object { Write-Host "    $($_.Line)" }
    if ($code -ne 0) {
        $failures.Add("$label (exit $code, log $log)")
    }
    elseif (Select-String -Path $log -Pattern '\[  SKIPPED \]' -Quiet) {
        $failures.Add("$label skipped (no device?), log $log")
    }
    elseif (-not (Select-String -Path $log -Pattern '^\[  PASSED  \] [1-9][0-9]* tests?\.' -Quiet)) {
        # Exit 0 with nothing run: the filter matched no test.
        $failures.Add("$label ran no test, log $log")
    }
}

foreach ($backend in $backends) {
    $rhiArgs = if ($backend -eq 'vulkan') { @('--olo-rhi=vulkan', '--olo-require-vulkan') } else { @('--olo-require-gpu') }
    $control = Join-Path $OutDir 'control.olosoak'
    $report = Join-Path $OutDir 'soak.json'

    Invoke-Soak "control-$backend" (@('--gtest_filter=LifecycleSoak.*Sequence*',
            "--olo-lifecycle-soak-control-out=$control") + $rhiArgs)
    Invoke-Soak "soak-$backend" (@('--gtest_filter=LifecycleSoak.*Sequence*',
            "--olo-lifecycle-soak-cycles=$Cycles", "--olo-lifecycle-soak-warmup=$WarmUp",
            "--olo-lifecycle-soak-report=$report", "--olo-lifecycle-soak-control-in=$control") + $rhiArgs)
    Invoke-Soak "negative-control-$backend" (@('--gtest_filter=LifecycleSoak.MissingActivationResetIsCaught') + $rhiArgs)
}

Write-Host "[lifecycle-soak] reports and logs: $OutDir"
if ($failures.Count -gt 0) {
    Write-Host "[lifecycle-soak] FAILED:" -ForegroundColor Red
    $failures | ForEach-Object { Write-Host "  $_" -ForegroundColor Red }
    exit 1
}
Write-Host "[lifecycle-soak] every process passed" -ForegroundColor Green
exit 0
