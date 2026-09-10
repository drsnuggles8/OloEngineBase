<#
.SYNOPSIS
  Report which Ninja edges the build actually spent its time on, and how much of that
  time was serialized at the end.

.DESCRIPTION
  Issue #1084. A compiler-cache hit rate is the wrong metric for this build, and the
  measurements say so: `Windows / build` at 99.88 % (2 misses of 1668) finished
  `Build with CMake` in 19.6 min, while the same job at 98.49 % (25 misses of 1661)
  took 30.3. One and a half points of hit rate was worth ten minutes, because the two
  `OloEditor/src/MCP/Mcp*.cpp` objects are ~5.4 min and they run SERIALIZED at the very
  end of the build. WHICH translation units miss decides the tail; how many miss does
  not. `sccache --show-stats` reports only the count.

  `.ninja_log` has what is missing. Ninja appends one line per edge it ran, with a
  start and an end millisecond relative to the start of that build, so every question
  above is arithmetic on a file the build already wrote. This script reads it and
  prints three things: the slowest edges, the last edges to finish, and the duration
  of every edge matching -Watch (the MCP objects by default).

  It is a REPORT, not a gate. Edge timings move with runner variance, and a threshold
  on them would go red for reasons nobody could act on. It writes the same tables to
  $GITHUB_STEP_SUMMARY when that is set, so the numbers land on the run page instead of
  in a 60 MB log.

  It never fails the build. A missing or unreadable `.ninja_log` is reported as a
  workflow warning -- loud and countable -- and the script still exits 0: a report that
  turned a green build red would be strictly worse than the missing report.

.PARAMETER BuildDir
  The ninja build directory holding `.ninja_log`. Defaults to build-cached next to the
  repository root.

.PARAMETER Top
  How many rows each of the two ranked tables carries. Default 12.

.PARAMETER Watch
  Wildcard pattern matched against each edge's output path. Every match is reported
  with its duration regardless of rank. Default `*Mcp*`.

.PARAMETER SlowSeconds
  Duration at or above which a -Watch match is called out in the summary line as a real
  compile rather than a cache hit. Default 30. Stated so the reader can disagree with
  it; the table prints the durations either way.

.EXAMPLE
  pwsh -File scripts/Report-NinjaBuildTail.ps1 -BuildDir build
#>
[CmdletBinding()]
param(
    [string] $BuildDir = (Join-Path (Split-Path -Parent $PSScriptRoot) 'build-cached'),
    [int]    $Top = 12,
    [string] $Watch = '*Mcp*',
    [double] $SlowSeconds = 30
)

# Continue, not Stop: every exit from this script is a report or a warning, never a
# failure, so an unexpected error must not become one through the preference.
$ErrorActionPreference = 'Continue'

$summaryPath = $env:GITHUB_STEP_SUMMARY
$summary = [System.Collections.Generic.List[string]]::new()

function Add-Line {
    param([string] $Text = '')
    Write-Host $Text
    $summary.Add($Text) | Out-Null
}

function Save-Summary {
    if (-not $summaryPath) { return }
    try {
        Add-Content -Path $summaryPath -Value ($summary -join "`n") -Encoding utf8
    } catch {
        Write-Host "::warning::could not write the job summary: $($_.Exception.Message)"
    }
}

$logPath = Join-Path $BuildDir '.ninja_log'
if (-not (Test-Path -LiteralPath $logPath)) {
    # Countable, not silent. The usual causes are a build that never reached ninja and
    # a generator that is not ninja at all; both make every number below meaningless,
    # so say which file was looked for rather than printing an empty table.
    Write-Host "::warning::no .ninja_log at '$logPath' -- no build-tail report for this run."
    exit 0
}

try {
    $lines = [System.IO.File]::ReadAllLines($logPath)
} catch {
    Write-Host "::warning::could not read '$logPath': $($_.Exception.Message)"
    exit 0
}

# `# ninja log vN` header, then one tab-separated record per edge:
#   start_ms  end_ms  output_mtime  output_path  command_hash
# Only edges ninja RAN this build are present, which is the right set: an edge ninja
# skipped as up to date cost nothing. A compiler-cache hit is still a run edge -- the
# launcher is the compiler as far as ninja is concerned -- so cache hits appear here
# with their real, small durations, which is exactly what makes the tail readable.
$edges = foreach ($line in $lines) {
    if ($line.Length -eq 0 -or $line[0] -eq '#') { continue }
    $f = $line.Split("`t")
    if ($f.Count -lt 4) { continue }
    $start = 0L; $end = 0L
    if (-not [long]::TryParse($f[0], [ref] $start)) { continue }
    if (-not [long]::TryParse($f[1], [ref] $end)) { continue }
    [pscustomobject]@{
        Output   = $f[3]
        Seconds  = [math]::Round(($end - $start) / 1000.0, 1)
        EndSec   = [math]::Round($end / 1000.0, 1)
        StartSec = [math]::Round($start / 1000.0, 1)
    }
}

$edges = @($edges)
if ($edges.Count -eq 0) {
    Write-Host "::warning::'$logPath' holds no edge records -- no build-tail report for this run."
    exit 0
}

# Ninja rewrites .ninja_log in place and keeps records from earlier builds in it, so
# on an incremental tree the file spans more than one build. The span below is
# therefore "the widest window these records cover", not necessarily one build's wall
# clock; on CI, where the tree is created and built once, they are the same thing.
$span = ($edges | Measure-Object -Property EndSec -Maximum).Maximum
$busy = ($edges | Measure-Object -Property Seconds -Sum).Sum

Add-Line '## Build tail (`.ninja_log`)'
Add-Line ''
Add-Line ('{0} edges recorded, widest span {1:N1} min, {2:N1} min of edge time.' -f `
    $edges.Count, ($span / 60.0), ($busy / 60.0))
Add-Line ''

Add-Line ('### Slowest {0} edges' -f $Top)
Add-Line ''
Add-Line '| sec | ends at (s) | output |'
Add-Line '|---:|---:|---|'
foreach ($e in ($edges | Sort-Object -Property Seconds -Descending | Select-Object -First $Top)) {
    Add-Line ('| {0:N1} | {1:N1} | `{2}` |' -f $e.Seconds, $e.EndSec, $e.Output)
}
Add-Line ''

# The tail is what a serialized end-of-build costs, and it is only visible in FINISH
# order: an edge can be slow and still free if it ran alongside a thousand others.
Add-Line ('### Last {0} edges to finish' -f $Top)
Add-Line ''
Add-Line '| ends at (s) | sec | output |'
Add-Line '|---:|---:|---|'
foreach ($e in ($edges | Sort-Object -Property EndSec -Descending | Select-Object -First $Top)) {
    Add-Line ('| {0:N1} | {1:N1} | `{2}` |' -f $e.EndSec, $e.Seconds, $e.Output)
}
Add-Line ''

$watched = @($edges | Where-Object { $_.Output -like $Watch } | Sort-Object -Property Seconds -Descending)
Add-Line ('### Watched edges (`{0}`)' -f $Watch)
Add-Line ''
if ($watched.Count -eq 0) {
    Add-Line ('No edge output matched `{0}` in this build.' -f $Watch)
} else {
    $slow = @($watched | Where-Object { $_.Seconds -ge $SlowSeconds })
    Add-Line ('{0} matched; {1} took {2}s or more, totalling {3:N1} min.' -f `
        $watched.Count, $slow.Count, $SlowSeconds, (($slow | Measure-Object -Property Seconds -Sum).Sum / 60.0))
    Add-Line ''
    # Capped at -Top. The pattern matches every MCP object in the tree, tests included,
    # and a 186-row table on the run page is not a report anyone reads. The count line
    # above covers the whole set; the table covers the part that costs anything.
    Add-Line '| sec | ends at (s) | output |'
    Add-Line '|---:|---:|---|'
    foreach ($e in ($watched | Select-Object -First $Top)) {
        Add-Line ('| {0:N1} | {1:N1} | `{2}` |' -f $e.Seconds, $e.EndSec, $e.Output)
    }
    if ($watched.Count -gt $Top) {
        Add-Line ''
        Add-Line ('{0} further matches not listed.' -f ($watched.Count - $Top))
    }
}

Save-Summary
exit 0
