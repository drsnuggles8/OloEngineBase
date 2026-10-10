<#
.SYNOPSIS
  Read-only snapshot of the cross-worktree build lock, as JSON, for the olo-fleet mod.

.DESCRIPTION
  Held-ness is the OS handle, not the file (build-lock.ps1: every slot file outlives its
  holder), so a slot is probed the way build-lock.ps1 probes it: an open that refuses to
  share writing fails while a holder has its write handle open. The probe closes at once.
  The holder record is read with FileShare.ReadWrite, which never disturbs the holder.

  A queue ticket is live when its pid is running and was started when the ticket says
  (pids are recycled on Windows). Everything fails soft: an unreadable file is reported,
  never thrown.
#>
param([Parameter(Mandatory = $true)][string] $CommonDir)

$ErrorActionPreference = 'Stop'

function Read-Shared([string] $Path) {
    try {
        $fs = [System.IO.File]::Open($Path, 'Open', 'Read', 'ReadWrite')
        try { return (New-Object System.IO.StreamReader($fs)).ReadToEnd() } finally { $fs.Dispose() }
    } catch { return $null }
}

# ConvertFrom-Json turns an ISO timestamp into a DateTime; hand it back as ISO, not in the
# locale's format, so the reader can parse it.
function Format-Stamp($Value) {
    if ($Value -is [datetime]) { return $Value.ToString('o') }
    return [string]$Value
}

function Test-Held([string] $Path) {
    try {
        $fs = [System.IO.File]::Open($Path, 'Open', 'Read', 'Read')
        $fs.Dispose()
        return $false
    } catch [System.IO.IOException] {
        return $true
    } catch {
        return $false
    }
}

$slots = @()
$slotFiles = @(Join-Path $CommonDir 'olo-build.lock') +
    @(Get-ChildItem -LiteralPath $CommonDir -Filter 'olo-build.slot*.lock' -ErrorAction SilentlyContinue |
      Where-Object { $_.Name -match '^olo-build\.slot[0-9]+\.lock$' } | Sort-Object Name | ForEach-Object FullName)
foreach ($file in $slotFiles) {
    if (-not (Test-Path -LiteralPath $file)) { continue }
    $held = Test-Held $file
    $record = $null
    if ($held) {
        $text = Read-Shared $file
        if ($text) { try { $record = $text | ConvertFrom-Json } catch { $record = $null } }
    }
    $slots += [ordered]@{
        name     = [System.IO.Path]::GetFileName($file)
        held     = $held
        worktree = if ($record) { [string]$record.worktree } else { $null }
        command  = if ($record) { [string]$record.command } else { $null }
        acquired = if ($record) { Format-Stamp $record.acquired } else { $null }
    }
}

$queue = @()
$queueDir = Join-Path $CommonDir 'olo-build-queue'
if (Test-Path -LiteralPath $queueDir) {
    foreach ($ticket in Get-ChildItem -LiteralPath $queueDir -Filter '*.json' | Sort-Object Name) {
        $t = $null
        try { $t = (Read-Shared $ticket.FullName) | ConvertFrom-Json } catch { continue }
        if (-not $t) { continue }
        $alive = $false
        try {
            $p = Get-Process -Id ([int]$t.pid) -ErrorAction Stop
            # startTicks is the waiter's own StartTime.Ticks; allow two seconds of skew.
            $alive = [math]::Abs($p.StartTime.Ticks - [long]$t.startTicks) -lt 20000000
        } catch { $alive = $false }
        $queue += [ordered]@{
            worktree = [string]$t.worktree
            command  = [string]$t.command
            enqueued = Format-Stamp $t.enqueuedText
            alive    = $alive
        }
    }
}

[ordered]@{ slots = @($slots); queue = @($queue) } | ConvertTo-Json -Depth 4 -Compress
