#requires -version 5.1
<#
.SYNOPSIS
    Run `cmake --build` with a de-duplicated environment block.

.DESCRIPTION
    Some launch environments (certain automation / agent shells, or a VS Code
    window opened from a polluted parent process) hand child processes a
    MALFORMED environment block that contains the same variable more than once
    (e.g. both `SystemDrive` and `SYSTEMDRIVE`). When MSBuild then builds the
    environment to launch CL.exe, .NET's case-insensitive environment dictionary
    throws:

        error MSB6001: Invalid command line switch for "CL.exe".
        System.ArgumentException: Item has already been added.
        Key in dictionary: 'SYSTEMDRIVE'  Key being added: 'SystemDrive'

    This wrapper rebuilds the *current* process environment so each variable
    appears exactly once, then invokes cmake. The cmake/MSBuild children inherit
    the clean block and build normally. On an already-clean environment this is a
    no-op (each variable is removed and re-added with the same value).

.NOTES
    This is a workaround for a broken *session* environment, not a repo problem.
    A VS Code window launched normally (e.g. from the Start menu) has a clean
    environment and does not need this. Safe to keep regardless.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$BuildDir,
    [Parameter(Mandatory)][string]$Target,
    [Parameter(Mandatory)][string]$Config,
    [switch]$Diagnose
)

$ErrorActionPreference = 'Stop'

Add-Type -Namespace Win32 -Name EnvBlk -MemberDefinition @'
[System.Runtime.InteropServices.DllImport("kernel32.dll", CharSet=System.Runtime.InteropServices.CharSet.Unicode)]
public static extern System.IntPtr GetEnvironmentStringsW();
[System.Runtime.InteropServices.DllImport("kernel32.dll")]
public static extern bool FreeEnvironmentStringsW(System.IntPtr lpszEnvironmentBlock);
'@

# Read the raw, double-null-terminated environment block (the managed
# Environment.GetEnvironmentVariables() can itself throw on duplicate keys, so we
# read the block directly).
function Read-RawEnv {
    $ptr = [Win32.EnvBlk]::GetEnvironmentStringsW()
    try {
        $sb = [System.Text.StringBuilder]::new()
        $off = 0
        while ($true) {
            $c = [System.Runtime.InteropServices.Marshal]::ReadInt16($ptr, $off); $off += 2
            if ($c -eq 0) {
                $n = [System.Runtime.InteropServices.Marshal]::ReadInt16($ptr, $off)
                [void]$sb.Append("`n")
                if ($n -eq 0) { break }
            }
            else { [void]$sb.Append([char]$c) }
        }
        return $sb.ToString()
    }
    finally { [void][Win32.EnvBlk]::FreeEnvironmentStringsW($ptr) }
}

# Count how many entries share a name case-insensitively (the corruption signal).
function Get-DupCount {
    $names = @()
    foreach ($line in (Read-RawEnv -split "`n")) {
        if ([string]::IsNullOrEmpty($line)) { continue }
        $eq = $line.IndexOf('=', 1)
        if ($eq -lt 1) { continue }
        $nm = $line.Substring(0, $eq)
        if ($nm.StartsWith('=')) { continue }
        $names += $nm
    }
    ($names.Count - ($names | Sort-Object -Unique -CaseSensitive:$false).Count)
}

if ($Diagnose) { Write-Host "[BuildCleanEnv] duplicate env entries before: $(Get-DupCount)" }

# Collect every (name -> [values]) from the raw block, skipping the special
# "=X:" drive-cwd markers Windows keeps in the block. Names are grouped
# case-insensitively (that's what MSBuild collides on), preserving first-seen order.
$order    = [System.Collections.Generic.List[string]]::new()
$lc2canon = @{}
$byName   = @{}
foreach ($line in (Read-RawEnv -split "`n")) {
    if ([string]::IsNullOrEmpty($line)) { continue }
    $eq = $line.IndexOf('=', 1)
    if ($eq -lt 1) { continue }
    $name = $line.Substring(0, $eq)
    if ($name.StartsWith('=')) { continue }
    $val = $line.Substring($eq + 1)
    $lc = $name.ToLowerInvariant()
    if (-not $lc2canon.ContainsKey($lc)) {
        $lc2canon[$lc] = $name
        $order.Add($lc)
        $byName[$lc] = [System.Collections.Generic.List[string]]::new()
    }
    $byName[$lc].Add($val)
}

# Collapse to one value per name. For PATH-like list variables, UNION every
# fragment (dedup directories, preserve order) so a duplicated/merged block never
# loses a directory the build needs (e.g. the MSYS bash / nasm / gmake dirs the
# ffmpeg external build relies on). For scalar variables, first occurrence wins.
$listVars = @{ 'path' = $true; 'pathext' = $true; 'psmodulepath' = $true }
$clean = [ordered]@{}
foreach ($lc in $order) {
    $name = $lc2canon[$lc]
    $vals = $byName[$lc]
    if ($listVars.ContainsKey($lc) -and $vals.Count -gt 1) {
        $seenDir = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
        $dirs = [System.Collections.Generic.List[string]]::new()
        foreach ($v in $vals) {
            foreach ($d in ($v -split ';')) {
                if ($d -ne '' -and $seenDir.Add($d)) { $dirs.Add($d) }
            }
        }
        $clean[$name] = ($dirs -join ';')
    }
    else {
        $clean[$name] = $vals[0]
    }
}

# Re-assert each variable exactly once. Removing until GetEnvironmentVariable
# returns null collapses any case-variant duplicates; then we add a single value.
foreach ($name in @($clean.Keys)) {
    $val = $clean[$name]
    $guard = 0
    while ($null -ne [Environment]::GetEnvironmentVariable($name) -and $guard -lt 16) {
        [Environment]::SetEnvironmentVariable($name, $null)
        $guard++
    }
    [Environment]::SetEnvironmentVariable($name, $val)
}

if ($Diagnose) { Write-Host "[BuildCleanEnv] duplicate env entries after:  $(Get-DupCount)" }

& cmake --build $BuildDir --target $Target --config $Config --parallel
exit $LASTEXITCODE
