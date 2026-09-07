<#
.SYNOPSIS
    Stage every non-system DLL a Windows binary imports next to that binary.

.DESCRIPTION
    Issue #1083 moved the two Windows test runs into shard jobs that receive the
    test tree as an artifact and install no toolchain. That works only if the
    artifact carries everything OloEngine-Tests.exe needs AT LOAD TIME, and the
    list is not obvious: CMake stages the FFmpeg and Steam DLLs beside the exe, so
    those travel for free -- but `shaderc_sharedd.dll` is a STATIC PE IMPORT
    resolved from the Vulkan SDK's bin directory on PATH. A shard without it dies
    in the OS loader with 0xC0000135 before main(), which reads as every case
    failing for no stated reason.

    A hand-written list of DLLs to copy would have missed exactly that one, and
    would rot the next time a dependency moves. So the list is READ FROM THE
    BINARY instead, with `dumpbin /dependents` (on PATH after the MSVC dev
    environment step every Windows job here already runs).

    THE RULE: a DLL that resolves under %SystemRoot% belongs to the runner image
    and is left alone; anything else is copied. An import that resolves NOWHERE
    fails this script -- if the exe cannot load on the machine that built it, a
    shard has no chance, and the failure should be named here rather than
    discovered as a loader error somewhere else.

.PARAMETER Exe
    The binary to inspect. Its own directory is searched first, exactly as the
    Windows loader does.

.PARAMETER Destination
    Where to copy the DLLs. Normally the exe's own directory.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string] $Exe,
    [Parameter(Mandatory = $true)][string] $Destination
)

$ErrorActionPreference = 'Stop'

if (-not (Test-Path -LiteralPath $Exe)) { throw "no such binary: $Exe" }
$exeDir = Split-Path -Parent (Resolve-Path -LiteralPath $Exe)

if (-not (Get-Command dumpbin -ErrorAction SilentlyContinue)) {
    throw "dumpbin is not on PATH. Run this after the MSVC dev environment step."
}

# `dumpbin /dependents` prints one indented DLL name per line inside an
# "Image has the following dependencies:" block. Matching the names directly is
# simpler and just as precise: nothing else in that output looks like `*.dll`.
$out = & dumpbin /nologo /dependents $Exe 2>&1
if ($LASTEXITCODE -ne 0) { throw "dumpbin failed on ${Exe}:`n$out" }

$imports = $out |
    Select-String -Pattern '^\s{4}(\S+\.dll)\s*$' |
    ForEach-Object { $_.Matches[0].Groups[1].Value } |
    Sort-Object -Unique

if (-not $imports) { throw "dumpbin reported no imports for $Exe -- that cannot be right" }

$system = [Environment]::GetFolderPath('Windows')
$copied = @()
$missing = @()

foreach ($name in $imports) {
    # An api-ms-win-* / ext-ms-* name is an API SET, not a file: the loader resolves
    # it through the OS's api-set schema to whatever system DLL currently implements
    # it. Skipped BEFORE resolution, not after -- stray physical copies of these do
    # exist on a developer box (one turned up under the Windows Performance Toolkit
    # while testing this), and shipping one beside the exe would shadow the
    # redirection with a stub from some other SDK's install.
    if ($name -like 'api-ms-*' -or $name -like 'ext-ms-*') { continue }

    # The loader looks in the exe's own directory first; anything already there
    # (CMake stages FFmpeg and steam_api64 there) needs no help.
    if (Test-Path -LiteralPath (Join-Path $exeDir $name)) { continue }

    $resolved = (Get-Command $name -CommandType Application -ErrorAction SilentlyContinue |
                 Select-Object -First 1).Source
    if (-not $resolved) {
        $missing += $name
        continue
    }
    if ($resolved.StartsWith($system, [StringComparison]::OrdinalIgnoreCase)) { continue }

    Copy-Item -LiteralPath $resolved -Destination $Destination -Force
    $copied += "$name  <- $resolved"
}

if ($missing.Count -gt 0) {
    Write-Host "::error::$Exe imports DLLs that resolve nowhere on this runner: $($missing -join ', '). It would not load here either."
    exit 1
}

if ($copied.Count -eq 0) {
    Write-Host "no extra imported DLLs to stage; everything resolves from $exeDir or the system"
} else {
    Write-Host "staged $($copied.Count) imported DLL(s) into ${Destination}:"
    $copied | ForEach-Object { Write-Host "  $_" }
}
