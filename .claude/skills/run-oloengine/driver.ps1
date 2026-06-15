#requires -version 5.1
<#
.SYNOPSIS
  Launch and screenshot an OloEngine GUI binary (OloEditor / OloRuntime) on Windows.

  This is the agent-facing harness for the run-oloengine skill. OloEditor is a
  native GLFW + OpenGL 4.6 + ImGui desktop app, so there is no DOM and no
  Playwright/chromium handle. Instead we drive it through the Win32 window
  manager: launch the process with the correct working directory, wait for its
  top-level window to appear, bring it to the foreground, and capture the
  DWM-composited pixels with GDI BitBlt (PrintWindow as a fallback).

.EXAMPLE
  # One-shot: build must already exist. Launch, wait, screenshot, kill.
  pwsh -File driver.ps1 -Action capture

.EXAMPLE
  # Leave it running for interactive poking, then shoot / stop by stored PID.
  pwsh -File driver.ps1 -Action launch
  pwsh -File driver.ps1 -Action shot -Out shots/again.png
  pwsh -File driver.ps1 -Action stop
#>
[CmdletBinding()]
param(
    [ValidateSet('capture', 'launch', 'shot', 'stop', 'attach')]
    [string]$Action = 'capture',

    [ValidateSet('OloEditor', 'OloRuntime')]
    [string]$Target = 'OloEditor',

    [ValidateSet('Debug', 'Release', 'Dist')]
    [string]$Config = 'Debug',

    [string]$Exe,                       # explicit exe path; overrides the templated lookup
    [string]$Out,                       # output PNG path (default: shots/<Target>-<Config>.png)
    [int]$WaitSeconds = 60,             # max wait for the window to appear (GL init can be slow)
    [int]$SettleSeconds = 30,           # render-settle delay; the editor warms up 42 PBR shaders first
    [int]$ProcId = 0,                   # for shot/stop against an already-running process
    [switch]$KeepOpen,                  # capture: leave the app running afterwards
    [int]$McpPort = 0,                  # attach: override the per-worktree MCP port (0 = derive from worktree path)
    [string]$McpName,                   # attach: override the per-worktree MCP server name (default oloeditor-<slug>)
    # 'print' (default) uses PrintWindow -> captures THIS window's own surface even
    # when occluded/not focused. 'screen' BitBlts the desktop at the window rect and
    # only works if OloEditor is genuinely the top-most visible window (it usually is
    # NOT, because a background process cannot steal foreground on Windows).
    [ValidateSet('print', 'screen')]
    [string]$Method = 'print'
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

# --- Win32 plumbing -------------------------------------------------------
if (-not ('WinShot' -as [type])) {
    Add-Type @'
using System;
using System.Text;
using System.Collections.Generic;
using System.Runtime.InteropServices;

public static class WinShot {
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int n);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
    [DllImport("dwmapi.dll")] public static extern int DwmGetWindowAttribute(IntPtr h, int attr, out RECT r, int size);

    public delegate bool EnumProc(IntPtr h, IntPtr l);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }

    // Largest visible top-level window owned by the given process id (0 = none).
    public static IntPtr MainWindowForPid(uint target) {
        IntPtr best = IntPtr.Zero; long bestArea = 0;
        EnumWindows((h, l) => {
            uint pid; GetWindowThreadProcessId(h, out pid);
            if (pid == target && IsWindowVisible(h)) {
                RECT r; GetWindowRect(h, out r);
                long area = (long)(r.Right - r.Left) * (r.Bottom - r.Top);
                if ((r.Right - r.Left) > 200 && (r.Bottom - r.Top) > 200 && area > bestArea) {
                    bestArea = area; best = h;
                }
            }
            return true;
        }, IntPtr.Zero);
        return best;
    }
}
'@
}

$DWMWA_EXTENDED_FRAME_BOUNDS = 9
$SW_RESTORE = 9
$rectSize = [System.Runtime.InteropServices.Marshal]::SizeOf([type]'WinShot+RECT')

function Resolve-RepoRoot {
    (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
}

function Resolve-Exe {
    param($repo)
    if ($Exe) { return $Exe }
    $templated = Join-Path $repo "bin\$Config\$Target\$Target.exe"
    if (Test-Path $templated) { return $templated }
    # Fallback: search both output trees for the freshest matching exe.
    $hit = Get-ChildItem -Recurse -Path (Join-Path $repo 'bin'), (Join-Path $repo 'build') `
        -Filter "$Target.exe" -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if ($hit) { return $hit.FullName }
    throw "Could not find $Target.exe. Build it first: cmake --build build --target $Target --config $Config --parallel  (looked for $templated)"
}

# Locate dumpbin.exe from the latest VS install (via vswhere). Returns $null if
# not found — the DLL diagnostic below then degrades to "skipped".
function Find-Dumpbin {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path $vswhere)) { return $null }
    $vs = & $vswhere -latest -property installationPath 2>$null
    if (-not $vs) { return $null }
    $db = Get-ChildItem "$vs\VC\Tools\MSVC\*\bin\Hostx64\x64\dumpbin.exe" -ErrorAction SilentlyContinue |
        Sort-Object FullName -Descending | Select-Object -First 1
    if ($db) { return $db.FullName }
    return $null
}

# Imported DLLs of $exePath that cannot be resolved against the loader search path
# (exe dir, System32/SysWOW64, PATH). API-set stubs (api-ms-win-*, ext-ms-*) are
# skipped — the OS resolves those. This is exactly the "which DLL is missing?"
# answer behind a STATUS_DLL_NOT_FOUND (0xC0000135) early exit.
function Get-UnresolvedDlls {
    param($exePath)
    $dumpbin = Find-Dumpbin
    if (-not $dumpbin) { return $null }   # cannot determine
    $out = & $dumpbin /dependents $exePath 2>$null
    $deps = $out | Select-String -Pattern '^\s+(\S+\.dll)\s*$' |
        ForEach-Object { $_.Matches[0].Groups[1].Value }
    if (-not $deps) { return @() }
    $exeDir = Split-Path $exePath
    $searchDirs = @($exeDir, "$env:SystemRoot\System32", "$env:SystemRoot\SysWOW64") +
        ($env:PATH -split ';' | Where-Object { $_ })
    $missing = @()
    foreach ($d in $deps) {
        if ($d -like 'api-ms-win-*' -or $d -like 'ext-ms-*') { continue }
        $found = $false
        foreach ($dir in $searchDirs) {
            if ($dir -and (Test-Path (Join-Path $dir $d) -ErrorAction SilentlyContinue)) { $found = $true; break }
        }
        if (-not $found) { $missing += $d }
    }
    return $missing
}

function Get-WindowRectFor {
    param($hwnd)
    $r = New-Object WinShot+RECT
    $hr = [WinShot]::DwmGetWindowAttribute($hwnd, $DWMWA_EXTENDED_FRAME_BOUNDS, [ref]$r, $rectSize)
    if ($hr -ne 0) { [WinShot]::GetWindowRect($hwnd, [ref]$r) | Out-Null }
    return $r
}

function Save-Shot {
    param($hwnd, $path)
    [WinShot]::ShowWindow($hwnd, $SW_RESTORE) | Out-Null
    [WinShot]::SetForegroundWindow($hwnd) | Out-Null
    Start-Sleep -Seconds $SettleSeconds

    $r = Get-WindowRectFor $hwnd
    $w = $r.Right - $r.Left; $h = $r.Bottom - $r.Top
    if ($w -le 0 -or $h -le 0) { throw "Window has zero size ($w x $h)" }

    $bmp = New-Object System.Drawing.Bitmap $w, $h
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    if ($Method -eq 'print') {
        $hdc = $g.GetHdc()
        [WinShot]::PrintWindow($hwnd, $hdc, 2) | Out-Null   # PW_RENDERFULLCONTENT
        $g.ReleaseHdc($hdc)
    }
    else {
        $g.CopyFromScreen($r.Left, $r.Top, 0, 0, (New-Object System.Drawing.Size($w, $h)))
    }
    New-Item -ItemType Directory -Force -Path (Split-Path $path) | Out-Null
    $bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)

    # Cheap blank-frame check: sample a 16x16 grid, report mean + spread so the
    # caller knows whether the capture actually has content (a black/uniform
    # frame usually means the session is non-interactive or the window is hidden).
    $sum = 0.0; $sumSq = 0.0; $n = 0
    for ($yy = 0; $yy -lt 16; $yy++) {
        for ($xx = 0; $xx -lt 16; $xx++) {
            $px = $bmp.GetPixel([int]($xx * ($w - 1) / 15), [int]($yy * ($h - 1) / 15))
            $lum = ($px.R + $px.G + $px.B) / 3.0
            $sum += $lum; $sumSq += $lum * $lum; $n++
        }
    }
    $g.Dispose(); $bmp.Dispose()
    $mean = $sum / $n
    $std = [math]::Sqrt([math]::Max(0, ($sumSq / $n) - ($mean * $mean)))
    [pscustomobject]@{ Path = $path; Width = $w; Height = $h; MeanLum = [math]::Round($mean, 1); StdLum = [math]::Round($std, 1) }
}

function Start-Editor {
    param($exePath, $workDir, [bool]$detached)
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $exePath
    $psi.WorkingDirectory = $workDir
    # detached (launch): ShellExecute starts it in its own console so it
    # survives this script exiting. inline (capture): inherit the console so
    # the editor's log streams here, and we kill it ourselves before exiting.
    $psi.UseShellExecute = $detached
    return [System.Diagnostics.Process]::Start($psi)
}

function Wait-Window {
    param($proc, $timeoutSec, $exePath)
    $deadline = (Get-Date).AddSeconds($timeoutSec)
    while ((Get-Date) -lt $deadline) {
        if ($proc.HasExited) {
            $msg = "Process exited early (exit code $($proc.ExitCode)) before a window appeared."
            if ($proc.ExitCode -eq -1073741515) {
                $msg += " Exit code 0xC0000135 = STATUS_DLL_NOT_FOUND: a required DLL is missing from the loader search path."
            }
            if ($exePath) {
                $missing = $null
                $dllCheckError = $null
                try { $missing = Get-UnresolvedDlls $exePath }
                catch { $dllCheckError = $_.Exception.Message }
                if ($dllCheckError) {
                    $msg += "`n(DLL dependency check failed: $dllCheckError)"
                }
                elseif ($null -eq $missing) {
                    $msg += " (dumpbin not found — cannot list imports; install the VC++ toolset to enable this diagnostic.)"
                }
                elseif ($missing.Count -gt 0) {
                    $msg += "`nUnresolved DLL imports: " + ($missing -join ', ')
                }
            }
            $msg += " Check OloEditor/OloEngine.log."
            throw $msg
        }
        $proc.Refresh()
        $h = $proc.MainWindowHandle
        if ($h -ne [IntPtr]::Zero) { return $h }
        $h = [WinShot]::MainWindowForPid([uint32]$proc.Id)
        if ($h -ne [IntPtr]::Zero) { return $h }
        Start-Sleep -Milliseconds 500
    }
    throw "Window for $Target did not appear within $timeoutSec s"
}

# Read OloEngine.log even while the editor still holds it open (shared read).
# The editor truncates this file on every launch, so we snapshot it per run.
function Read-EngineLog {
    param($workDir)
    $src = Join-Path $workDir 'OloEngine.log'
    if (-not (Test-Path $src)) { return $null }
    try {
        $fs = [System.IO.File]::Open($src, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, [System.IO.FileShare]::ReadWrite)
        $sr = New-Object System.IO.StreamReader($fs)
        $text = $sr.ReadToEnd()
        $sr.Close(); $fs.Close()
        return $text
    }
    catch { return $null }
}

# Snapshot OloEngine.log next to the screenshot (foo.png -> foo.log). Returns the path.
function Save-Log {
    param($workDir, $pngPath)
    $text = Read-EngineLog $workDir
    if ($null -eq $text) { return $null }
    $dest = [System.IO.Path]::ChangeExtension($pngPath, '.log')
    New-Item -ItemType Directory -Force -Path (Split-Path $dest) | Out-Null
    Set-Content -Path $dest -Value $text -NoNewline
    return $dest
}

function Write-LogTail {
    param($workDir, $n = 30)
    $text = Read-EngineLog $workDir
    if ($null -eq $text) { Write-Output "(no OloEngine.log found in $workDir)"; return }
    Write-Output "---- last $n lines of $workDir\OloEngine.log ----"
    ($text -split "`r?`n") | Select-Object -Last $n | Write-Output
}

# --- MCP auto-attach (per-worktree isolation, issue #316) -----------------
# Each worktree gets a STABLE, distinct MCP port + discovery file + server name
# derived from its path, so parallel editor sessions never collide on the single
# default port / the single legacy discovery file. The engine honours
# OLO_MCP_PORT and OLO_MCP_DISCOVERY_FILE (see McpServer::DiscoveryFilePath).

# Stable, filesystem/CLI-safe identifier for this worktree: the leaf dir name,
# lowercased with runs of non-alphanumerics collapsed to single dashes.
function Get-WorktreeSlug {
    param($repo)
    $leaf = Split-Path $repo -Leaf
    $slug = ($leaf.ToLowerInvariant() -replace '[^a-z0-9]+', '-').Trim('-')
    if (-not $slug) { $slug = 'oloengine' }
    return $slug
}

# Deterministic per-worktree port in [20000, 59999], stable across launches and
# sessions. .NET's String.GetHashCode is per-process randomized, so we hash the
# path bytes with MD5 ourselves (any stable hash would do — this is not security).
function Get-WorktreePort {
    param($repo)
    $md5 = [System.Security.Cryptography.MD5]::Create()
    try {
        $bytes = [System.Text.Encoding]::UTF8.GetBytes($repo.ToLowerInvariant())
        $hash = $md5.ComputeHash($bytes)
    }
    finally { $md5.Dispose() }
    $val = ([int]$hash[0] -shl 8) -bor [int]$hash[1]   # 0..65535
    return 20000 + ($val % 40000)
}

# Block until the editor writes its discovery JSON (host/port/token/url), parse it,
# and return the object. Retries through partial writes (best-effort ConvertFrom-Json).
function Wait-Discovery {
    param($path, $timeoutSec = 30)
    $deadline = (Get-Date).AddSeconds($timeoutSec)
    while ((Get-Date) -lt $deadline) {
        if (Test-Path $path) {
            try {
                $obj = Get-Content $path -Raw | ConvertFrom-Json
                if ($obj.url -and $obj.token) { return $obj }
            }
            catch { } # file caught mid-write; retry
        }
        Start-Sleep -Milliseconds 300
    }
    throw "MCP discovery file did not appear at $path within $timeoutSec s. Is OLO_MCP_AUTOSTART honoured? Check OloEditor/OloEngine.log for an [MCP] bind error."
}

# Register the running server with Claude Code via `claude mcp add` (idempotent:
# drops any stale same-named registration first). Returns $true on success; if the
# `claude` CLI is unavailable, prints the ready-to-paste command and returns $false.
function Register-Mcp {
    param($name, $url, $token)
    $claude = Get-Command claude -ErrorAction SilentlyContinue
    $manual = "claude mcp add --transport http $name $url --header `"Authorization: Bearer $token`""
    if (-not $claude) {
        Write-Warning "`claude` CLI not found on PATH — register manually:`n  $manual"
        return $false
    }
    & claude mcp remove $name 2>$null | Out-Null   # idempotent
    & claude mcp add --transport http $name $url --header "Authorization: Bearer $token" 2>&1 | Write-Output
    if ($LASTEXITCODE -ne 0) {
        Write-Warning "`claude mcp add` failed (exit $LASTEXITCODE). Register manually:`n  $manual"
        return $false
    }
    return $true
}

# --- main -----------------------------------------------------------------
$repo = Resolve-RepoRoot
$workDir = Join-Path $repo 'OloEditor'          # editor & runtime resolve assets relative to here
$skillDir = $PSScriptRoot
$pidFile = Join-Path $skillDir 'shots\.pid'
$mcpFile = Join-Path $skillDir 'shots\.mcp'   # attach: <name>\n<discoveryPath> for stop cleanup
if (-not $Out) { $Out = Join-Path $skillDir "shots\$Target-$Config.png" }

switch ($Action) {
    'launch' {
        $exePath = Resolve-Exe $repo
        $proc = Start-Editor $exePath $workDir $true   # detached: survives this script
        $hwnd = Wait-Window $proc $WaitSeconds $exePath
        New-Item -ItemType Directory -Force -Path (Split-Path $pidFile) | Out-Null
        Set-Content -Path $pidFile -Value $proc.Id
        Write-Output "LAUNCHED $Target pid=$($proc.Id) hwnd=$hwnd (left running; use -Action shot / stop)"
    }
    'capture' {
        $exePath = Resolve-Exe $repo
        Write-Output "Launching $exePath (cwd=$workDir)"
        $proc = Start-Editor $exePath $workDir $false  # inline: log streams here, we kill it
        try {
            $hwnd = Wait-Window $proc $WaitSeconds $exePath
            $info = Save-Shot $hwnd $Out
            $info | Format-List | Out-String | Write-Output
            if ($info.StdLum -lt 3) {
                Write-Warning "Capture looks nearly uniform (StdLum=$($info.StdLum)). The session may be non-interactive/RDP-disconnected, or the window is occluded. Try -Method print."
            }
        }
        catch {
            Write-Warning "Capture failed: $($_.Exception.Message)"
            Write-LogTail $workDir 30
            throw
        }
        finally {
            if (-not $KeepOpen) {
                if (-not $proc.HasExited) { $proc.Kill(); $proc.WaitForExit(3000) | Out-Null }
                $log = Save-Log $workDir $Out
                if ($log) { Write-Output "Log snapshot: $log" }
                Write-Output "Stopped $Target (pid=$($proc.Id))"
            }
            else {
                New-Item -ItemType Directory -Force -Path (Split-Path $pidFile) | Out-Null
                Set-Content -Path $pidFile -Value $proc.Id
                $log = Save-Log $workDir $Out
                if ($log) { Write-Output "Log snapshot (live): $log" }
                Write-Output "Left $Target running (pid=$($proc.Id))"
            }
        }
    }
    'shot' {
        if (-not $ProcId) {
            if (Test-Path $pidFile) { $ProcId = [int](Get-Content $pidFile | Select-Object -First 1) }
            else { throw "No -ProcId given and no $pidFile. Launch with -Action launch first." }
        }
        $hwnd = [WinShot]::MainWindowForPid([uint32]$ProcId)
        if ($hwnd -eq [IntPtr]::Zero) { throw "No visible window for pid $ProcId" }
        $info = Save-Shot $hwnd $Out
        $info | Format-List | Out-String | Write-Output
        $log = Save-Log $workDir $Out
        if ($log) { Write-Output "Log snapshot (live): $log" }
    }
    'attach' {
        # Launch the editor detached with its MCP diagnostics server auto-started on
        # this worktree's dedicated port + discovery file, then register it with
        # Claude Code so the olo_* tools become available in the session.
        $exePath = Resolve-Exe $repo
        $slug = Get-WorktreeSlug $repo
        $port = if ($McpPort -gt 0) { $McpPort } else { Get-WorktreePort $repo }
        $name = if ($McpName) { $McpName } else { "oloeditor-$slug" }
        $discoveryPath = Join-Path $env:TEMP "oloengine-mcp-$slug.json"

        # Drop any stale discovery file so Wait-Discovery only returns this run's.
        Remove-Item $discoveryPath -ErrorAction SilentlyContinue

        # The editor reads these on init (EditorLayer) + when writing the discovery
        # file (McpServer::DiscoveryFilePath). A detached ShellExecute launch inherits
        # this process's environment, so set them here and clean up afterwards.
        $env:OLO_MCP_AUTOSTART = '1'
        $env:OLO_MCP_PORT = "$port"
        $env:OLO_MCP_DISCOVERY_FILE = $discoveryPath
        try {
            $proc = Start-Editor $exePath $workDir $true   # detached: survives this script
            $hwnd = Wait-Window $proc $WaitSeconds $exePath
        }
        finally {
            Remove-Item Env:OLO_MCP_AUTOSTART, Env:OLO_MCP_PORT, Env:OLO_MCP_DISCOVERY_FILE -ErrorAction SilentlyContinue
        }

        New-Item -ItemType Directory -Force -Path (Split-Path $pidFile) | Out-Null
        Set-Content -Path $pidFile -Value $proc.Id
        Write-Output "LAUNCHED $Target pid=$($proc.Id) hwnd=$hwnd (MCP port $port)"

        # The window appears early, but MCP autostart runs at the END of editor init
        # (after project load: asset scan, quest DB, save games + the ~10-25s shader
        # warmup), so give the discovery file a generous window beyond -WaitSeconds.
        $discovery = Wait-Discovery $discoveryPath ([Math]::Max(120, $WaitSeconds))
        Write-Output "MCP discovery: $discoveryPath -> $($discovery.url)"
        $registered = Register-Mcp $name $discovery.url $discovery.token
        Set-Content -Path $mcpFile -Value @($name, $discoveryPath)
        if ($registered) {
            Write-Output "ATTACHED MCP server '$name' at $($discovery.url). The olo_* tools are registered for Claude Code (a session reconnect may be needed to surface them). Use -Action stop to kill the editor and deregister."
        }
    }
    'stop' {
        if (-not $ProcId) {
            if (Test-Path $pidFile) { $ProcId = [int](Get-Content $pidFile | Select-Object -First 1) }
            else { throw "No -ProcId given and no $pidFile." }
        }
        Stop-Process -Id $ProcId -Force -ErrorAction SilentlyContinue
        Remove-Item $pidFile -ErrorAction SilentlyContinue
        Write-Output "Stopped pid $ProcId"

        # Tear down an MCP registration left by `attach` (best-effort).
        if (Test-Path $mcpFile) {
            $lines = @(Get-Content $mcpFile)
            $mcpName = $lines[0]
            $disc = if ($lines.Count -gt 1) { $lines[1] } else { $null }
            if ($mcpName -and (Get-Command claude -ErrorAction SilentlyContinue)) {
                & claude mcp remove $mcpName 2>$null | Out-Null
                Write-Output "Deregistered MCP server '$mcpName'"
            }
            if ($disc) { Remove-Item $disc -ErrorAction SilentlyContinue }
            Remove-Item $mcpFile -ErrorAction SilentlyContinue
        }
    }
}
