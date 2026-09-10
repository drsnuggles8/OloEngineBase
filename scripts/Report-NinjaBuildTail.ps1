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

# `# ninja log vN` header, then one tab-separated record per OUTPUT:
#   start_ms  end_ms  output_mtime  output_path  command_hash
# Only edges ninja RAN this build are present, which is the right set: an edge ninja
# skipped as up to date cost nothing. A compiler-cache hit is still a run edge -- the
# launcher is the compiler as far as ninja is concerned -- so cache hits appear here
# with their real, small durations, which is exactly what makes the tail readable.
#
# ONE RECORD PER OUTPUT IS NOT ONE RECORD PER EDGE, and this tree has multi-output
# edges: gtest_discover_tests' POST_BUILD step writes the `*_tests.cmake` files beside
# `OloEngine-Tests.exe`, so that one 109 s edge appends FOUR identical records. Summed
# naively it is counted four times, and it took four of the five rows in the
# last-to-finish table on the first local run of this script -- crowding out the very
# tail the table exists to show. Group on (start, end, command hash), which is what an
# edge is.
$records = foreach ($line in $lines) {
    if ($line.Length -eq 0 -or $line[0] -eq '#') { continue }
    $f = $line.Split("`t")
    if ($f.Count -lt 4) { continue }
    $start = 0L; $end = 0L
    if (-not [long]::TryParse($f[0], [ref] $start)) { continue }
    if (-not [long]::TryParse($f[1], [ref] $end)) { continue }
    # No command hash recorded -> fall back to the output path, which is unique per
    # record. That makes the key unique too, so records are never merged on a guess:
    # the failure direction is "reports an edge twice", not "silently drops one".
    $hash = if ($f.Count -ge 5 -and $f[4]) { $f[4] } else { $f[3] }
    [pscustomobject]@{
        Output  = $f[3]
        StartMs = $start
        EndMs   = $end
        Key     = '{0}:{1}:{2}' -f $start, $end, $hash
    }
}

$records = @($records)
if ($records.Count -eq 0) {
    Write-Host "::warning::'$logPath' holds no edge records -- no build-tail report for this run."
    exit 0
}

# MILLISECONDS ARE KEPT RAW HERE AND ROUNDED ONLY WHEN FORMATTED. Rounding each record
# to a tenth before summing puts up to 0.05 s of error into every one of ~1700 rows --
# over a minute on the total, on a figure whose whole job is to be compared between
# runs.
$edges = foreach ($g in ($records | Group-Object -Property Key)) {
    $first = $g.Group[0]
    [pscustomobject]@{
        Output  = $first.Output
        Outputs = @($g.Group | ForEach-Object { $_.Output })
        Extra   = $g.Count - 1
        DurMs   = $first.EndMs - $first.StartMs
        EndMs   = $first.EndMs
    }
}

$edges = @($edges)

# Ninja rewrites .ninja_log in place and keeps records from earlier builds in it, so
# on an incremental tree the file spans more than one build. The span below is
# therefore "the widest window these records cover", not necessarily one build's wall
# clock; on CI, where the tree is created and built once, they are the same thing.
$span = ($edges | Measure-Object -Property EndMs -Maximum).Maximum / 1000.0
$busy = ($edges | Measure-Object -Property DurMs -Sum).Sum / 1000.0

# `+N more` rather than N rows: a multi-output edge is ONE unit of build time, and the
# other outputs are worth knowing about without letting them fill the table.
function Format-Output {
    param($Edge, [string] $Name)
    $label = if ($Name) { $Name } else { $Edge.Output }
    if ($Edge.Extra -gt 0) { '`{0}` (+{1} more)' -f $label, $Edge.Extra } else { '`{0}`' -f $label }
}

Add-Line '## Build tail (`.ninja_log`)'
Add-Line ''
Add-Line ('{0} edges recorded ({1} output records), widest span {2:N1} min, {3:N1} min of edge time.' -f `
    $edges.Count, $records.Count, ($span / 60.0), ($busy / 60.0))
Add-Line ''

Add-Line ('### Slowest {0} edges' -f $Top)
Add-Line ''
Add-Line '| sec | ends at (s) | output |'
Add-Line '|---:|---:|---|'
foreach ($e in ($edges | Sort-Object -Property DurMs -Descending | Select-Object -First $Top)) {
    Add-Line ('| {0:N1} | {1:N1} | {2} |' -f ($e.DurMs / 1000.0), ($e.EndMs / 1000.0), (Format-Output $e))
}
Add-Line ''

# The tail is what a serialized end-of-build costs, and it is only visible in FINISH
# order: an edge can be slow and still free if it ran alongside a thousand others.
Add-Line ('### Last {0} edges to finish' -f $Top)
Add-Line ''
Add-Line '| ends at (s) | sec | output |'
Add-Line '|---:|---:|---|'
foreach ($e in ($edges | Sort-Object -Property EndMs -Descending | Select-Object -First $Top)) {
    Add-Line ('| {0:N1} | {1:N1} | {2} |' -f ($e.EndMs / 1000.0), ($e.DurMs / 1000.0), (Format-Output $e))
}
Add-Line ''

# ANY output of the edge matching counts, and the matching one is what gets shown --
# an edge whose first-recorded output is a `.cmake` stamp can still be the MCP compile
# the pattern is looking for.
$watched = @(
    $edges |
        ForEach-Object {
            $hit = @($_.Outputs | Where-Object { $_ -like $Watch })[0]
            if ($null -ne $hit) { $_ | Add-Member -NotePropertyName Match -NotePropertyValue $hit -Force -PassThru }
        } |
        Sort-Object -Property DurMs -Descending
)
Add-Line ('### Watched edges (`{0}`)' -f $Watch)
Add-Line ''
if ($watched.Count -eq 0) {
    Add-Line ('No edge output matched `{0}` in this build.' -f $Watch)
} else {
    $slowMs = $SlowSeconds * 1000.0
    $slow = @($watched | Where-Object { $_.DurMs -ge $slowMs })
    Add-Line ('{0} matched; {1} took {2}s or more, totalling {3:N1} min.' -f `
        $watched.Count, $slow.Count, $SlowSeconds, (($slow | Measure-Object -Property DurMs -Sum).Sum / 60000.0))
    Add-Line ''
    # Capped at -Top. The pattern matches every MCP object in the tree, tests included,
    # and a 186-row table on the run page is not a report anyone reads. The count line
    # above covers the whole set; the table covers the part that costs anything.
    Add-Line '| sec | ends at (s) | output |'
    Add-Line '|---:|---:|---|'
    foreach ($e in ($watched | Select-Object -First $Top)) {
        Add-Line ('| {0:N1} | {1:N1} | {2} |' -f ($e.DurMs / 1000.0), ($e.EndMs / 1000.0), (Format-Output $e $e.Match))
    }
    if ($watched.Count -gt $Top) {
        Add-Line ''
        Add-Line ('{0} further matches not listed.' -f ($watched.Count - $Top))
    }
}

Save-Summary
exit 0
