# =============================================================================
# run-perf-battery.ps1 — measurement driver for the perf stress scene battery.
#
# Launches ONE OloEditor instance (default -Config Dist) and drives it through
# every generated scene in Assets/Scenes/PerfStress/manifest.json (see
# OloEngine/tests/scripts/generate_perf_scenes.py) over MCP, switching scenes
# and Play state with the consented-write scene-control tools (#316 Part 5)
# instead of relaunching the editor per scene:
#
#   1. launch once, with OLO_MCP_AUTOSTART=1 + OLO_MCP_ALLOW_WRITES=1 (the
#      session-level write-consent opt-in — off by default; needed because
#      olo_scene_open/olo_scene_play/olo_scene_stop are ProjectWrite tools) +
#      OLO_EDITOR_AUTOSAVE_RECOVERY=original (pre-answers the recovery modal a
#      headless session could never click, in case a stray .auto file exists),
#   2. per scene: olo_scene_open (timed — this is the scene-load-time metric),
#      then olo_scene_play if the scene needs Play mode (physics/scripts/
#      animation don't tick in Edit mode; olo_scene_open stops Play first, so
#      no explicit olo_scene_stop is needed between scenes),
#   3. over MCP JSON-RPC: olo_viewport_set_size 1920x1080 (window is NOT
#      maximized — the override fights a maximized panel, known issue), poses
#      the editor camera for edit-mode scenes, waits -SettleSeconds, then
#      records olo_perf_snapshot xN, olo_perf_pass_timings xN (averaged),
#      olo_perf_frame_history, olo_memory_report, a screenshot, a workload
#      probe, and shader/script error probes,
#   4. after the last scene, stops Play mode and kills the one editor process,
#      and writes results.md (markdown table) + results.json (raw samples) +
#      per-scene PNGs/log-deltas into -OutDir.
#
# A scene that fails to open/play (e.g. missing file, no primary camera) is
# recorded as an error row and the run continues — no relaunch needed, unlike
# the old per-scene-process design.
#
# Usage (repo root):
#   pwsh -File scripts/perf/run-perf-battery.ps1                       # all scenes
#   pwsh -File scripts/perf/run-perf-battery.ps1 -Scenes physics_pile_10000,lights_many_512
#   pwsh -File scripts/perf/run-perf-battery.ps1 -Config Release -SettleSeconds 30
#
# Prereqs: scenes generated (generate_perf_scenes.py --scene all), OloEditor
# built in the chosen config (with the OLO_MCP_ALLOW_WRITES hook —
# EditorLayer.cpp, merged alongside #316 Part 5), interactive desktop session
# (GL context).
# =============================================================================
[CmdletBinding()]
param(
    [string[]]$Scenes,                    # manifest keys; default = all in manifest
    [ValidateSet('Debug', 'Release', 'Dist')]
    [string]$Config = 'Dist',
    [int]$SettleSeconds = 20,             # steady-state wait before sampling
    [int]$Samples = 3,                    # perf_snapshot / pass_timings repeats
    [int]$StartupTimeoutSeconds = 120,    # one-time: launch -> MCP discovery file
    [int]$SceneOpenTimeoutSeconds = 600,  # per-scene: 50k-entity scenes can load for minutes
    [switch]$SkipBaseline,                # baseline = PinkCube.olo reference row
    [string]$OutDir
)

$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$workDir = Join-Path $repo 'OloEditor'
$manifestPath = Join-Path $workDir 'SandboxProject\Assets\Scenes\PerfStress\manifest.json'
$exePath = Join-Path $repo "bin\$Config\OloEditor\OloEditor.exe"

if (-not (Test-Path $exePath)) { throw "Not built: $exePath (cmake --build build --target OloEditor --config $Config --parallel)" }
if (-not (Test-Path $manifestPath)) { throw "No manifest at $manifestPath. Run generate_perf_scenes.py --scene all first." }

if (-not $OutDir) {
    $stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
    $OutDir = Join-Path $env:TEMP "olo-perf-battery\$stamp"
}
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

$manifest = Get-Content $manifestPath -Raw | ConvertFrom-Json
$allKeys = @($manifest.PSObject.Properties.Name | Sort-Object)
if (-not $Scenes) { $Scenes = $allKeys }

# Baseline row: a near-empty stock scene, same measurement path, for reference.
$jobs = @()
if (-not $SkipBaseline) {
    $jobs += [pscustomobject]@{ Key = 'baseline_pinkcube'; File = '../PinkCube.olo'; Entities = 2; NeedsPlay = $false; Camera = $null; ProbeId = $null; Description = 'baseline: stock PinkCube.olo' }
}
foreach ($k in $Scenes) {
    $m = $manifest.$k
    if (-not $m) { throw "Scene '$k' not in manifest. Known: $($allKeys -join ', ')" }
    $jobs += [pscustomobject]@{ Key = $k; File = $m.file; Entities = $m.entities; NeedsPlay = $m.needsPlayMode; Camera = $m.camera; ProbeId = $m.probeEntityId; Description = $m.description }
}

# --- MCP JSON-RPC plumbing (pattern: run-oloengine mcp-smoke-test.ps1) -------
$script:nextId = 0
$script:rpcUrl = $null
$script:rpcHeaders = $null

function Invoke-Rpc([string]$method, $params, [int]$timeoutSec = 60) {
    $script:nextId++
    $body = @{ jsonrpc = '2.0'; id = $script:nextId; method = $method }
    if ($null -ne $params) { $body.params = $params }
    $json = $body | ConvertTo-Json -Depth 10
    $resp = Invoke-WebRequest -Uri $script:rpcUrl -Method Post -Headers $script:rpcHeaders -Body $json -TimeoutSec $timeoutSec
    $sid = $resp.Headers['Mcp-Session-Id']
    if ($sid) {
        if ($sid -is [System.Array]) { $sid = $sid[0] }
        $script:rpcHeaders['Mcp-Session-Id'] = [string]$sid
    }
    return ($resp.Content | ConvertFrom-Json)
}

function Invoke-Tool([string]$name, $arguments, [int]$timeoutSec = 60) {
    $r = Invoke-Rpc 'tools/call' @{ name = $name; arguments = $arguments } $timeoutSec
    if ($r.result.isError) {
        Write-Warning "$name -> isError: $($r.result.content[0].text)"
        return $null
    }
    return $r
}

function Get-ToolJson([string]$name, $arguments, [int]$timeoutSec = 60) {
    $r = Invoke-Tool $name $arguments $timeoutSec
    if (-not $r) { return $null }
    try { return $r.result.content[0].text | ConvertFrom-Json } catch { return $null }
}

function Wait-Discovery([string]$path, [int]$timeoutSec, $proc) {
    $deadline = (Get-Date).AddSeconds($timeoutSec)
    while ((Get-Date) -lt $deadline) {
        if ($proc.HasExited) { throw "Editor exited early (code $($proc.ExitCode)) — check OloEditor/OloEngine.log" }
        if (Test-Path $path) {
            try {
                $obj = Get-Content $path -Raw | ConvertFrom-Json
                if ($obj.url -and $obj.token) { return $obj }
            } catch { }
        }
        Start-Sleep -Milliseconds 250
    }
    throw "MCP discovery did not appear within $timeoutSec s ($path)"
}

function Read-EngineLog {
    $src = Join-Path $workDir 'OloEngine.log'
    if (-not (Test-Path $src)) { return $null }
    try {
        $fs = [System.IO.File]::Open($src, 'Open', 'Read', 'ReadWrite')
        $sr = New-Object System.IO.StreamReader($fs)
        $text = $sr.ReadToEnd(); $sr.Close(); $fs.Close()
        return $text
    } catch { return $null }
}

# The editor is now a single long-lived process for the whole battery, so
# OloEngine.log keeps growing across scenes (it is truncated only once, on
# THIS launch). Save each scene's DELTA since the previous snapshot instead
# of the whole file, so <scene>.log stays scoped to that scene like it was
# in the old per-scene-relaunch design.
$script:logOffset = 0
function Save-LogDelta([string]$key) {
    $text = Read-EngineLog
    if ($null -eq $text) { return }
    $delta = if ($text.Length -gt $script:logOffset) { $text.Substring($script:logOffset) } else { '' }
    Set-Content -Path (Join-Path $OutDir "$key.log") -Value $delta -NoNewline
    $script:logOffset = $text.Length
}

function Get-Avg($values) {
    $vals = @($values | Where-Object { $null -ne $_ })
    if ($vals.Count -eq 0) { return $null }
    return [math]::Round(($vals | Measure-Object -Average).Average, 2)
}

# --- launch ONE editor instance for the whole battery -------------------------
$port = 47311
$discoveryPath = Join-Path $env:TEMP "oloengine-mcp-perfbattery.json"
Remove-Item $discoveryPath -ErrorAction SilentlyContinue

$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = $exePath
$psi.WorkingDirectory = $workDir
$psi.UseShellExecute = $true
# ShellExecute children inherit this process's environment (same trick
# driver.ps1 -Action attach uses), so set the opt-ins via $env:.
$env:OLO_MCP_AUTOSTART = '1'
$env:OLO_MCP_PORT = "$port"
$env:OLO_MCP_DISCOVERY_FILE = $discoveryPath
$env:OLO_MCP_ALLOW_WRITES = '1'
$env:OLO_EDITOR_AUTOSAVE_RECOVERY = 'original'
$proc = $null
try {
    $proc = [System.Diagnostics.Process]::Start($psi)
    Write-Host "Launching $exePath (pid=$($proc.Id))..." -ForegroundColor Cyan
    $discovery = Wait-Discovery $discoveryPath $StartupTimeoutSeconds $proc
} finally {
    Remove-Item Env:OLO_MCP_AUTOSTART, Env:OLO_MCP_PORT, Env:OLO_MCP_DISCOVERY_FILE, Env:OLO_MCP_ALLOW_WRITES, Env:OLO_EDITOR_AUTOSAVE_RECOVERY -ErrorAction SilentlyContinue
}
Write-Host "MCP ready: $($discovery.url)" -ForegroundColor Cyan

$script:rpcUrl = $discovery.url
$script:rpcHeaders = @{
    'Authorization' = "Bearer $($discovery.token)"
    'Content-Type'  = 'application/json'
    'Accept'        = 'application/json, text/event-stream'
}
$null = Invoke-Rpc 'initialize' @{ protocolVersion = '2025-06-18'; capabilities = @{}; clientInfo = @{ name = 'perf-battery'; version = '2.0' } }
$null = Invoke-WebRequest -Uri $script:rpcUrl -Method Post -Headers $script:rpcHeaders -Body (@{ jsonrpc = '2.0'; method = 'notifications/initialized' } | ConvertTo-Json)
Save-LogDelta '_startup'

# --- run each scene against the live editor -----------------------------------
$results = @()

try {
    foreach ($job in $jobs) {
        $sceneRel = if ($job.File.StartsWith('../')) { "Scenes/" + $job.File.Substring(3) } else { "Scenes/PerfStress/$($job.File)" }
        Write-Host "=== $($job.Key)  ($sceneRel, entities=$($job.Entities), play=$($job.NeedsPlay)) ===" -ForegroundColor Cyan

        $row = [ordered]@{
            scene = $job.Key; entities = $job.Entities; playMode = [bool]$job.NeedsPlay
        }
        $raw = [ordered]@{ scene = $job.Key; samples = @() }

        try {
            # 1. switch scene (timed — this IS the scene-load-time metric now)
            $t0 = Get-Date
            $opened = Get-ToolJson 'olo_scene_open' @{ path = $sceneRel } $SceneOpenTimeoutSeconds
            $row.sceneOpenSeconds = [math]::Round(((Get-Date) - $t0).TotalSeconds, 1)
            if (-not $opened -or -not $opened.ok) {
                $msg = if ($opened) { $opened.message } else { 'olo_scene_open returned no result (RPC error — see warning above)' }
                throw "scene open failed: $msg"
            }
            $row.entitiesLoaded = $opened.entityCount
            Write-Host "  scene open: $($row.sceneOpenSeconds)s ($($opened.entityCount) entities)"

            # 2. enter Play mode if this axis needs runtime ticking (physics/
            #    scripts/animation don't tick in Edit mode). olo_scene_open
            #    already stopped any PREVIOUS scene's Play mode, so no
            #    explicit olo_scene_stop is needed between scenes.
            if ($job.NeedsPlay) {
                $played = Get-ToolJson 'olo_scene_play' @{}
                if (-not $played -or -not $played.ok) {
                    $msg = if ($played) { $played.message } else { 'olo_scene_play returned no result' }
                    throw "scene play failed: $msg"
                }
            }

            # 3. fixed viewport (do NOT maximize the window; override fights it)
            $null = Invoke-Tool 'olo_viewport_set_size' @{ width = 1920; height = 1080 }

            # Edit-mode scenes render through the EDITOR camera — pose it to the
            # scene's intended framing (Play mode uses the in-scene camera).
            if (-not $job.NeedsPlay -and $job.Camera) {
                $null = Invoke-Tool 'olo_camera_set_pose' @{
                    position = @($job.Camera.position); target = @($job.Camera.target)
                }
            }

            Write-Host "  settling ${SettleSeconds}s..."
            Start-Sleep -Seconds $SettleSeconds

            $snaps = @(); $passSets = @()
            for ($i = 0; $i -lt $Samples; $i++) {
                $s = Get-ToolJson 'olo_perf_snapshot' @{}
                if ($s) { $snaps += $s }
                $p = Get-ToolJson 'olo_perf_pass_timings' @{}
                if ($p) { $passSets += $p }
                Start-Sleep -Milliseconds 700
            }
            $raw.samples = $snaps
            $raw.passTimings = $passSets

            if ($snaps.Count -gt 0) {
                $row.fps        = Get-Avg ($snaps | ForEach-Object { $_.fps })
                $row.frameMs    = Get-Avg ($snaps | ForEach-Object { $_.frameTimeMs })
                $row.gpuMs      = Get-Avg ($snaps | ForEach-Object { $_.gpuMs })
                $row.gpuWaitMs  = Get-Avg ($snaps | ForEach-Object { $_.gpuWaitMs })
                $row.drawCalls  = Get-Avg ($snaps | ForEach-Object { $_.drawCalls })
                $row.triangles  = Get-Avg ($snaps | ForEach-Object { $_.triangles })
            }

            # average pass timings by pass name across samples (schema:
            # passes[].pass/gpuMs/cpuMs + frame totals incl. cpuMs)
            if ($passSets.Count -gt 0 -and $passSets[0].passes) {
                $gpuByName = [ordered]@{}; $cpuByName = @{}
                foreach ($set in $passSets) {
                    foreach ($p in $set.passes) {
                        if (-not $gpuByName.Contains($p.pass)) { $gpuByName[$p.pass] = @(); $cpuByName[$p.pass] = @() }
                        $gpuByName[$p.pass] += $p.gpuMs
                        $cpuByName[$p.pass] += $p.cpuMs
                    }
                }
                $row.cpuMs = Get-Avg ($passSets | ForEach-Object { $_.frame.cpuMs })
                $row.unattributedGpuMs = Get-Avg ($passSets | ForEach-Object { $_.unattributedGpuMs })
                $row.passes = (@($gpuByName.Keys | ForEach-Object { "$($_)=$(Get-Avg $gpuByName[$_])" }) -join ' ')
                $raw.passAverages = @($gpuByName.Keys | ForEach-Object {
                    @{ pass = $_; gpuMs = (Get-Avg $gpuByName[$_]); cpuMs = (Get-Avg $cpuByName[$_]) } })
            }

            $hist = Get-ToolJson 'olo_perf_frame_history' @{ points = 120 }
            if ($hist -and $hist.series) {
                $raw.frameHistory = $hist
                $row.maxFrameMs = ($hist.series | ForEach-Object { $_.frameTimeMs } | Measure-Object -Maximum).Maximum
            }

            # Workload probe: sample a designated moving entity twice — a
            # workload that silently isn't running reads as a great fps number.
            if ($job.ProbeId) {
                $getTranslation = {
                    $e = Get-ToolJson 'olo_scene_get_entity' @{ id = $job.ProbeId }
                    if ($e -and $e.componentsYaml -match 'Translation: \[([^\]]+)\]') { $Matches[1] } else { $null }
                }
                $p1 = & $getTranslation
                Start-Sleep -Seconds 2
                $p2 = & $getTranslation
                $row.workloadActive = ($null -ne $p1 -and $null -ne $p2 -and $p1 -ne $p2)
                $raw.probe = @{ id = $job.ProbeId; before = $p1; after = $p2 }
            }

            $mem = Get-ToolJson 'olo_memory_report' @{}
            if ($mem) { $raw.memory = $mem }

            $raw.shaderErrors = Get-ToolJson 'olo_shader_errors' @{}
            $raw.scriptErrors = Get-ToolJson 'olo_script_get_last_errors' @{ count = 10 }

            # screenshot evidence
            $shot = Invoke-Tool 'olo_screenshot' @{ maxWidth = 960 } 120
            if ($shot) {
                $img = $shot.result.content | Where-Object { $_.type -eq 'image' } | Select-Object -First 1
                if ($img -and $img.data) {
                    [IO.File]::WriteAllBytes((Join-Path $OutDir "$($job.Key).png"), [Convert]::FromBase64String($img.data))
                }
            }
        }
        catch {
            # A scene that fails to open/play must not abort the rest of the
            # battery — unlike the old per-scene-process design there is no
            # relaunch to fall back to, so just record the error and move on.
            Write-Warning "  $($job.Key): $($_.Exception.Message)"
            $row.error = $_.Exception.Message
        }
        finally {
            Save-LogDelta $job.Key
        }

        $results += [pscustomobject]$row
        ($raw | ConvertTo-Json -Depth 8) | Set-Content -Path (Join-Path $OutDir "$($job.Key).json")
    }
}
finally {
    # Best-effort: leave the editor in Edit mode before killing it.
    try { $null = Invoke-Tool 'olo_scene_stop' @{} } catch { }
    if ($proc -and -not $proc.HasExited) { $proc.Kill(); $proc.WaitForExit(5000) | Out-Null }
    Remove-Item $discoveryPath -ErrorAction SilentlyContinue
    Write-Host "Stopped editor (pid=$($proc.Id))"
}

# --- results -------------------------------------------------------------------
$results | ConvertTo-Json -Depth 4 | Set-Content -Path (Join-Path $OutDir 'results.json')

$md = @()
$md += "# Perf stress battery — $Config, $(Get-Date -Format 'yyyy-MM-dd HH:mm')"
$md += ""
$md += "One long-lived editor instance, scenes switched via olo_scene_open/olo_scene_play " +
       "(#316 Part 5) — no relaunch per scene. Settle ${SettleSeconds}s, $Samples samples averaged. " +
       "Viewport 1920x1080 via olo_viewport_set_size."
$md += ""
$md += "| scene | entities | play | scene open s | fps | frame ms | cpu ms | gpu ms | gpuWait ms | draws | tris | max ms | probe | error |"
$md += "|---|---|---|---|---|---|---|---|---|---|---|---|---|---|"
foreach ($r in $results) {
    $probe = if ($null -eq $r.workloadActive) { '' } elseif ($r.workloadActive) { 'moving' } else { 'STATIC!' }
    $md += "| $($r.scene) | $($r.entities) | $($r.playMode) | $($r.sceneOpenSeconds) | $($r.fps) | $($r.frameMs) | $($r.cpuMs) | $($r.gpuMs) | $($r.gpuWaitMs) | $($r.drawCalls) | $($r.triangles) | $($r.maxFrameMs) | $probe | $($r.error) |"
}
$md += ""
$md += "## GPU pass timings (avg ms)"
$md += ""
foreach ($r in $results) {
    if ($r.passes) { $md += "- **$($r.scene)**: $($r.passes)" }
}
$md -join "`n" | Set-Content -Path (Join-Path $OutDir 'results.md')

Write-Host "`nResults: $OutDir\results.md" -ForegroundColor Green
$results | Format-Table scene, entities, sceneOpenSeconds, fps, frameMs, gpuMs, gpuWaitMs, drawCalls -AutoSize
