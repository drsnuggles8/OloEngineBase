<#
.SYNOPSIS
  Assert that a Ninja build tree recorded real header dependencies for every object.

.DESCRIPTION
  Issue #858. Ninja stores, per output, the set of headers that output depends on
  (`.ninja_deps`, readable with `ninja -t deps`). An object whose record says
  `#deps 0` has NO header dependencies recorded, so ninja will never rebuild it when
  a header it includes changes. The build stays green; the objects silently drift
  out of sync with the headers they were compiled against.

  That is not hypothetical: with ccache in front of clang-cl and CMake's default
  gcc-style depfile scanning, ccache cannot parse `-clang:-MF<file>`, so it never
  restored a depfile on a cache hit — 699 of 701 records in this repo's cached tree
  carried `#deps 0`. cmake/CompilerCache.cmake now forces `/showIncludes` instead;
  this script is the check that would have caught it, and the one that catches it
  again the next time the cache configuration is tuned.

  A `#deps 0` record IS legitimate for a translation unit that includes nothing but
  system headers, because clang-cl's `/showIncludes` does not report headers reached
  through system search paths (`-imsvc`) — verified: no record in this repo's tree
  names a Visual Studio or Windows Kits path. `tools/OloHeaderTool/main.cpp` is
  exactly that case: 5,000 lines including only `<...>` standard headers.

  So rather than an allowlist by name — which would go stale silently the day that
  file grows a project include — each zero-dep object is traced back to its source
  and cleared only if that source has no quoted `#include "..."` at all. A name-based
  exception would be a blind spot; this is a property that stops holding the moment
  the file changes.

.PARAMETER BuildDir
  The ninja build directory. Defaults to build-cached next to the repository root.

.PARAMETER Config
  Multi-config name whose ninja file to read (build-<Config>.ninja). Defaults to Debug.
  Pass an empty string for a single-config tree (plain build.ninja).

.PARAMETER MaxTrace
  Above this many zero-dep records the failure is systemic, so the per-object tracing
  is skipped and the check fails immediately. Default 25.

.EXAMPLE
  pwsh -File scripts/Check-NinjaHeaderDeps.ps1 -BuildDir build-cached
#>
[CmdletBinding()]
param(
    [string] $BuildDir = (Join-Path (Split-Path -Parent $PSScriptRoot) 'build-cached'),
    [string] $Config = 'Debug',
    [int]    $MaxTrace = 25
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot

function Write-Explanation {
    Write-Host ""
    Write-Host "  This is issue #858. The usual cause is a compiler-cache launcher swallowing the"
    Write-Host "  dependency file: ccache/sccache cannot parse clang-cl's '-clang:-MF<file>' spelling,"
    Write-Host "  so a cache hit restores the object without its depfile. cmake/CompilerCache.cmake"
    Write-Host "  forces '/showIncludes' for MSVC-frontend compilers to avoid exactly that - check"
    Write-Host "  that block, and the generated CMakeFiles/rules.ninja ('deps = msvc', not 'deps = gcc')."
    Write-Host "  Background: docs/agent-rules/build-trees-and-windows-asan.md section 6."
    Write-Host ""
    Write-Host "  Until it is fixed, treat this tree as unsafe after ANY header edit: delete the"
    Write-Host "  affected objects (or the build directory) rather than trusting an incremental build."
}

if (-not (Test-Path -LiteralPath $BuildDir)) {
    Write-Host "[deps-check] no build directory at $BuildDir - nothing to check."
    exit 0
}
if (-not (Test-Path -LiteralPath (Join-Path $BuildDir '.ninja_deps'))) {
    Write-Host "[deps-check] $BuildDir has no .ninja_deps yet (nothing built) - nothing to check."
    exit 0
}

$ninjaCmd = Get-Command ninja -ErrorAction SilentlyContinue
if (-not $ninjaCmd) {
    Write-Host "[deps-check] ninja not on PATH - skipping."
    exit 0
}
$ninja = $ninjaCmd.Source

$baseArgs = @('-C', $BuildDir)
if ($Config -and (Test-Path -LiteralPath (Join-Path $BuildDir "build-$Config.ninja"))) {
    $baseArgs += @('-f', "build-$Config.ninja")
}

# `ninja -t deps` prints one "<output>: #deps N, deps mtime ... (VALID|STALE)" header
# per recorded output, followed by the dependency lines.
#
# Check the exit status before reading anything into "no records". A ninja that fails
# (unreadable log, wrong -f, a build directory mid-regeneration) also prints nothing,
# and treating that as "nothing to check, exit 0" would turn every future breakage of
# this script into a silent pass — the same shape as the bug it exists to catch.
$depsOutput = & $ninja @baseArgs -t deps 2>$null
$depsExit   = $LASTEXITCODE
if ($depsExit -ne 0) {
    Write-Host "[deps-check] FAIL - 'ninja -t deps' exited $depsExit in $BuildDir; the tree's dependency records could not be read." -ForegroundColor Red
    Write-Host "             Not treating an unreadable log as a pass. Re-run the build, or check the -Config/-BuildDir arguments."
    exit 1
}

$records = $depsOutput | Select-String -Pattern '^(?<out>\S.*): #deps (?<n>\d+),'
if (-not $records) {
    Write-Host "[deps-check] no dependency records in $BuildDir - nothing to check."
    exit 0
}

$total = $records.Count
$zero  = @($records | Where-Object { $_.Matches[0].Groups['n'].Value -eq '0' } |
              ForEach-Object { $_.Matches[0].Groups['out'].Value })

if ($zero.Count -eq 0) {
    Write-Host "[deps-check] OK - all $total objects have header dependencies recorded."
    exit 0
}

if ($zero.Count -gt $MaxTrace) {
    Write-Host ""
    Write-Host "[deps-check] FAIL - $($zero.Count) of $total objects in $BuildDir have NO header dependencies recorded." -ForegroundColor Red
    Write-Host "             They will NOT rebuild when a header they include changes:" -ForegroundColor Red
    $zero | Select-Object -First 15 | ForEach-Object { Write-Host "               $_" }
    Write-Host "               ... and $($zero.Count - 15) more"
    Write-Explanation
    exit 1
}

# Few enough to explain individually. A zero-dep record is fine only if the source
# genuinely depends on nothing but system headers; anything else lost its dependencies.
#
# A quoted include settles it immediately. An ANGLE include does not: `<Jolt/Jolt.h>`,
# `<MaterialXCore/Document.h>` and `<Alembic/Abc/All.h>` are angle-included here and are
# very much our dependencies, so "no quoted includes" alone would clear a genuinely
# broken record. Resolve each angle include against the roots that hold headers we own
# or vendor; anything found there counts as a dependency the record should have had.
# Only unresolvable angle includes (the standard library, the Windows SDK) are system.
$searchRoots = @(
    (Join-Path $repoRoot 'OloEngine/src'),
    (Join-Path $repoRoot 'OloEngine/vendor'),
    (Join-Path $repoRoot 'OloEditor/src')
) + @(Get-ChildItem -LiteralPath (Join-Path $BuildDir 'vcpkg_installed') -Directory -ErrorAction SilentlyContinue |
        ForEach-Object { Join-Path $_.FullName 'include' }) +
    # The FetchContent trees sit a level deeper (OloEngine/vendor/<toolchain>/<name>-src),
    # which is where an angle-included <imgui.h> actually lives.
    @(Get-ChildItem -Path (Join-Path $repoRoot 'OloEngine/vendor/*/*-src') -Directory -ErrorAction SilentlyContinue |
        ForEach-Object { $_.FullName })
$searchRoots = @($searchRoots | Where-Object { Test-Path -LiteralPath $_ })

# Resolves to a header we own or vendor? Depth-limited on purpose: these roots are large,
# and a full recursive walk per include would cost more than the build step this follows.
function Resolve-OwnedInclude([string] $Name) {
    foreach ($root in $searchRoots) {
        if (Test-Path -LiteralPath (Join-Path $root $Name)) { return $true }
    }
    return $false
}

$benign = @()
$broken = @()
foreach ($out in $zero) {
    $src = $null
    try {
        # `ninja -t query <out>` lists the edge's inputs; the source is the one that
        # looks like a compiland rather than an order-only dependency.
        #
        # `.rc` is in this list because the compiled `.res` outputs are recorded
        # dependency edges too, and CI found them the hard way: with only C/C++
        # extensions here, OloEditor.rc.res and OloRuntime.rc.res traced to nothing and
        # were reported as broken. They are not — both .rc files are a single ICON line
        # with no `#include` at all, so no header dependencies is the correct record.
        # They go through the same include analysis as any other source rather than
        # being waved through as "not a compiland".
        $src = & $ninja @baseArgs -t query $out 2>$null |
            ForEach-Object { $_.Trim() } |
            Where-Object { $_ -match '\.(c|cc|cpp|cxx|m|mm|rc)$' } |
            Select-Object -First 1
    } catch { }

    if ($src) {
        $srcPath = if ([System.IO.Path]::IsPathRooted($src)) { $src } else { Join-Path $BuildDir $src }
        if (Test-Path -LiteralPath $srcPath) {
            if (Select-String -LiteralPath $srcPath -Pattern '^\s*#\s*include\s*"' -Quiet) {
                $broken += "$out  (source $src has quoted includes but no recorded deps)"
                continue
            }
            $owned = @(Select-String -LiteralPath $srcPath -Pattern '^\s*#\s*include\s*<([^>]+)>' |
                ForEach-Object { $_.Matches[0].Groups[1].Value } |
                Where-Object { Resolve-OwnedInclude $_ } |
                Select-Object -Unique -First 3)
            if ($owned.Count -gt 0) {
                $broken += "$out  (source $src includes $($owned -join ', ') but recorded no deps)"
            } else {
                $benign += "$out  (source $src includes only system headers)"
            }
            continue
        }
    }
    $broken += "$out  (could not trace back to a source file)"
}

if ($broken.Count -eq 0) {
    Write-Host "[deps-check] OK - $total objects, $($benign.Count) with no headers, all accounted for:"
    $benign | ForEach-Object { Write-Host "               $_" }
    exit 0
}

Write-Host ""
Write-Host "[deps-check] FAIL - $($broken.Count) of $total objects in $BuildDir lost their header dependencies." -ForegroundColor Red
Write-Host "             They will NOT rebuild when a header they include changes:" -ForegroundColor Red
$broken | ForEach-Object { Write-Host "               $_" }
if ($benign.Count -gt 0) {
    Write-Host "             (plus $($benign.Count) legitimately header-less, not counted)"
}
Write-Explanation
exit 1
