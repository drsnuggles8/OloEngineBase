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
    [ValidateSet('capture', 'launch', 'shot', 'stop')]
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
    param($proc, $timeoutSec)
    $deadline = (Get-Date).AddSeconds($timeoutSec)
    while ((Get-Date) -lt $deadline) {
        if ($proc.HasExited) {
            throw "Process exited early (exit code $($proc.ExitCode)) before a window appeared. Check OloEditor/OloEngine.log."
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

# --- main -----------------------------------------------------------------
$repo = Resolve-RepoRoot
$workDir = Join-Path $repo 'OloEditor'          # editor & runtime resolve assets relative to here
$skillDir = $PSScriptRoot
$pidFile = Join-Path $skillDir 'shots\.pid'
if (-not $Out) { $Out = Join-Path $skillDir "shots\$Target-$Config.png" }

switch ($Action) {
    'launch' {
        $exePath = Resolve-Exe $repo
        $proc = Start-Editor $exePath $workDir $true   # detached: survives this script
        $hwnd = Wait-Window $proc $WaitSeconds
        New-Item -ItemType Directory -Force -Path (Split-Path $pidFile) | Out-Null
        Set-Content -Path $pidFile -Value $proc.Id
        Write-Output "LAUNCHED $Target pid=$($proc.Id) hwnd=$hwnd (left running; use -Action shot / stop)"
    }
    'capture' {
        $exePath = Resolve-Exe $repo
        Write-Output "Launching $exePath (cwd=$workDir)"
        $proc = Start-Editor $exePath $workDir $false  # inline: log streams here, we kill it
        try {
            $hwnd = Wait-Window $proc $WaitSeconds
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
    'stop' {
        if (-not $ProcId) {
            if (Test-Path $pidFile) { $ProcId = [int](Get-Content $pidFile | Select-Object -First 1) }
            else { throw "No -ProcId given and no $pidFile." }
        }
        Stop-Process -Id $ProcId -Force -ErrorAction SilentlyContinue
        Remove-Item $pidFile -ErrorAction SilentlyContinue
        Write-Output "Stopped pid $ProcId"
    }
}
