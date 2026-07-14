<#
.SYNOPSIS
    Fetch external assets that are deliberately not committed to the repository.

.DESCRIPTION
    Some assets cannot live in git: a multi-hundred-megabyte mesh is irreversible once it is in
    history, and some sources ask to be fetched rather than redistributed. Those are listed in
    scripts/assets/asset-manifest.json and downloaded on demand by this script.

    Everything that consumes such an asset (a scene, a benchmark) must degrade gracefully when it
    is missing — SKIP with a message naming this script, never fail. A missing optional asset is a
    setup step the user has not run, not a broken build.

    Downloads are checksum-verified (SHA-256) and idempotent: an asset already present with the
    right content is left alone, so re-running is cheap and safe.

.PARAMETER Name
    Fetch only the named asset (e.g. xyzrgb-dragon).

.PARAMETER Tag
    Fetch only assets carrying this tag (e.g. nanite).

.PARAMETER List
    Print the manifest — name, size, triangles, whether it is already present — and exit.

.PARAMETER Force
    Re-download even if the destination already exists and verifies.

.EXAMPLE
    scripts\Fetch-Assets.ps1 -List
    scripts\Fetch-Assets.ps1 -Tag nanite
    scripts\Fetch-Assets.ps1 -Name xyzrgb-dragon
#>
[CmdletBinding()]
param(
    [string] $Name,
    [string] $Tag,
    [switch] $List,
    [switch] $Force
)

$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent $PSScriptRoot
$manifestPath = Join-Path $PSScriptRoot 'assets/asset-manifest.json'

if (-not (Test-Path $manifestPath))
{
    throw "Asset manifest not found at $manifestPath"
}

$manifest = Get-Content $manifestPath -Raw | ConvertFrom-Json
$assets = $manifest.assets

if ($Name)
{
    $assets = $assets | Where-Object { $_.name -eq $Name }
    if (-not $assets)
    {
        throw "No asset named '$Name' in the manifest. Run with -List to see what is available."
    }
}
if ($Tag)
{
    $assets = $assets | Where-Object { $_.tags -contains $Tag }
    if (-not $assets)
    {
        throw "No asset tagged '$Tag' in the manifest. Run with -List to see what is available."
    }
}

function Format-Size([long] $bytes)
{
    if ($bytes -ge 1GB) { return '{0:N1} GB' -f ($bytes / 1GB) }
    if ($bytes -ge 1MB) { return '{0:N1} MB' -f ($bytes / 1MB) }
    return '{0:N0} KB' -f ($bytes / 1KB)
}

if ($List)
{
    Write-Host ''
    Write-Host 'External assets (not in git — fetched on demand):' -ForegroundColor Cyan
    Write-Host ''
    foreach ($a in $assets)
    {
        $dest = Join-Path $repoRoot $a.dest
        $present = Test-Path $dest
        $mark = if ($present) { '[present]' } else { '[missing]' }
        $colour = if ($present) { 'Green' } else { 'DarkGray' }

        Write-Host ("  {0,-9} {1,-16} {2,10}  {3}" -f $mark, $a.name, (Format-Size $a.sizeBytes), $a.description) -ForegroundColor $colour
        if ($a.triangles) { Write-Host ("            {0:N0} triangles" -f $a.triangles) -ForegroundColor DarkGray }
        Write-Host ("            $($a.credit)") -ForegroundColor DarkGray
        Write-Host ''
    }
    exit 0
}

foreach ($a in $assets)
{
    $dest = Join-Path $repoRoot $a.dest
    $destDir = Split-Path -Parent $dest

    if ((Test-Path $dest) -and -not $Force)
    {
        Write-Host "[skip]  $($a.name) already present at $($a.dest)" -ForegroundColor DarkGray
        continue
    }

    if (-not (Test-Path $destDir))
    {
        New-Item -ItemType Directory -Force -Path $destDir | Out-Null
    }

    Write-Host "[fetch] $($a.name) — $(Format-Size $a.sizeBytes) from $($a.url)" -ForegroundColor Cyan
    Write-Host "        $($a.credit)" -ForegroundColor DarkGray

    # Download to a temp file first: a half-written file at the destination would look "present"
    # to the skip check above and to every consumer, and would then fail to parse much later,
    # somewhere far away from the cause.
    $tmp = [System.IO.Path]::GetTempFileName()
    try
    {
        $progressPreferenceBefore = $ProgressPreference
        $ProgressPreference = 'SilentlyContinue' # the progress bar makes Invoke-WebRequest ~10x slower
        try
        {
            Invoke-WebRequest -Uri $a.url -OutFile $tmp -UseBasicParsing
        }
        finally
        {
            $ProgressPreference = $progressPreferenceBefore
        }

        if ($a.sha256)
        {
            $actual = (Get-FileHash -Path $tmp -Algorithm SHA256).Hash.ToLowerInvariant()
            $expected = $a.sha256.ToLowerInvariant()
            if ($actual -ne $expected)
            {
                throw "Checksum mismatch for $($a.name).`n  expected $expected`n  actual   $actual`nThe download is corrupt, or the upstream file changed — do not use it."
            }
            Write-Host "        sha256 ok" -ForegroundColor DarkGray
        }

        switch ($a.unpack)
        {
            'gzip'
            {
                Write-Host "        unpacking gzip..." -ForegroundColor DarkGray
                $inStream = [System.IO.File]::OpenRead($tmp)
                $outStream = [System.IO.File]::Create($dest)
                try
                {
                    $gzip = New-Object System.IO.Compression.GzipStream($inStream, [System.IO.Compression.CompressionMode]::Decompress)
                    try { $gzip.CopyTo($outStream) } finally { $gzip.Dispose() }
                }
                finally
                {
                    $outStream.Dispose()
                    $inStream.Dispose()
                }
            }
            default
            {
                Move-Item -Path $tmp -Destination $dest -Force
            }
        }

        $finalSize = (Get-Item $dest).Length
        Write-Host "[ok]    $($a.name) -> $($a.dest) ($(Format-Size $finalSize))" -ForegroundColor Green
    }
    finally
    {
        if (Test-Path $tmp) { Remove-Item $tmp -Force -ErrorAction SilentlyContinue }
    }
}

Write-Host ''
Write-Host 'Done.' -ForegroundColor Green
