# =============================================================================
# run-perf-battery.ps1 — measurement driver for the perf stress scene battery.
#
# For every generated scene in Assets/Scenes/PerfStress/manifest.json (see
# OloEngine/tests/scripts/generate_perf_scenes.py) this script:
#
#   1. points Sandbox.oloproj's StartScene at the scene (RESTORED afterwards —
#      no olo_scene_open MCP tool exists yet; logged on #306),
#   2. launches OloEditor (default -Config Dist) with OLO_MCP_AUTOSTART=1 on a
#      dedicated port, and OLO_EDITOR_AUTOPLAY=1 when the scene needs Play mode
#      (physics/scripts/animation don't tick in Edit mode),
#   3. measures startup+scene-load wall time (launch -> MCP discovery file),
#   4. over MCP JSON-RPC: olo_viewport_set_size 1920x1080 (window is NOT
#      maximized — the override fights a maximized panel, known issue), waits
#      -SettleSeconds, then records olo_perf_snapshot xN, olo_perf_pass_timings
#      xN (averaged), olo_perf_frame_history, olo_memory_report, a screenshot,
#      and shader/script error probes,
#   5. kills the editor and writes results.md (markdown table) + results.json
#      (raw samples) + per-scene PNGs into -OutDir.
#
# Usage (repo root):
#   pwsh -File scripts/perf/run-perf-battery.ps1                       # all scenes
#   pwsh -File scripts/perf/run-perf-battery.ps1 -Scenes physics_pile_10000,lights_many_512
#   pwsh -File scripts/perf/run-perf-battery.ps1 -Config Release -SettleSeconds 30
#
# Prereqs: scenes generated (generate_perf_scenes.py --scene all), OloEditor
# built in the chosen config, interactive desktop session (GL context).
# =============================================================================
[CmdletBinding()]
param(
    [string[]]$Scenes,                    # manifest keys; default = all in manifest
    [ValidateSet('Debug', 'Release', 'Dist')]
    [string]$Config = 'Dist',
    [int]$SettleSeconds = 20,             # steady-state wait before sampling
    [int]$Samples = 3,                    # perf_snapshot / pass_timings repeats
    [int]$LoadTimeoutSeconds = 600,       # 50k-entity scenes can load for minutes
    [switch]$SkipBaseline,                # baseline = PinkCube.olo reference row
    [string]$OutDir
)

$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$workDir = Join-Path $repo 'OloEditor'
$projFile = Join-Path $workDir 'SandboxProject\Sandbox.oloproj'
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

# The editor occasionally still holds Sandbox.oloproj briefly after a kill
# (shutdown-time writes) — retry instead of aborting the whole battery.
function Set-ContentRetry([string]$path, [string]$value) {
    for ($attempt = 1; $attempt -le 5; $attempt++) {
        try { Set-Content -Path $path -Value $value -NoNewline; return }
        catch { if ($attempt -eq 5) { throw }; Start-Sleep -Seconds 2 }
    }
}

function Get-Avg($values) {
    $vals = @($values | Where-Object { $null -ne $_ })
    if ($vals.Count -eq 0) { return $null }
    return [math]::Round(($vals | Measure-Object -Average).Average, 2)
}

# --- run ---------------------------------------------------------------------
$projBackup = Get-Content $projFile -Raw
$port = 47311   # fixed battery port; per-run discovery file below
$discoveryPath = Join-Path $env:TEMP "oloengine-mcp-perfbattery.json"
$results = @()

try {
    foreach ($job in $jobs) {
        $sceneRel = if ($job.File.StartsWith('../')) { "Scenes/" + $job.File.Substring(3) } else { "Scenes/PerfStress/$($job.File)" }
        Write-Host "=== $($job.Key)  ($sceneRel, entities=$($job.Entities), play=$($job.NeedsPlay)) ===" -ForegroundColor Cyan

        # 1. point StartScene at the scene (restored in finally)
        $projText = $projBackup -replace 'StartScene:\s*"[^"]*"', "StartScene: `"$sceneRel`""
        Set-ContentRetry $projFile $projText

        Remove-Item $discoveryPath -ErrorAction SilentlyContinue

        # 2. launch
        $psi = New-Object System.Diagnostics.ProcessStartInfo
        $psi.FileName = $exePath
        $psi.WorkingDirectory = $workDir
        $psi.UseShellExecute = $true
        # ShellExecute children inherit this process's environment (same trick
        # driver.ps1 -Action attach uses), so set the opt-ins via $env:.
        $env:OLO_MCP_AUTOSTART = '1'
        $env:OLO_MCP_PORT = "$port"
        $env:OLO_MCP_DISCOVERY_FILE = $discoveryPath
        if ($job.NeedsPlay) { $env:OLO_EDITOR_AUTOPLAY = '1' } else { Remove-Item Env:OLO_EDITOR_AUTOPLAY -ErrorAction SilentlyContinue }
        $t0 = Get-Date
        $proc = $null
        try {
            try {
                $proc = [System.Diagnostics.Process]::Start($psi)
                # 3. startup + scene load wall time = launch -> discovery file (MCP
                #    autostart runs at the END of editor init, after project+scene load)
                $discovery = Wait-Discovery $discoveryPath $LoadTimeoutSeconds $proc
            } finally {
                Remove-Item Env:OLO_MCP_AUTOSTART, Env:OLO_MCP_PORT, Env:OLO_MCP_DISCOVERY_FILE, Env:OLO_EDITOR_AUTOPLAY -ErrorAction SilentlyContinue
            }
        }
        catch {
            # A scene that fails to launch/load must not abort the rest of the battery.
            Write-Warning "  launch/load failed: $($_.Exception.Message)"
            $logText = Read-EngineLog
            if ($logText) { Set-Content -Path (Join-Path $OutDir "$($job.Key).log") -Value $logText -NoNewline }
            if ($proc -and -not $proc.HasExited) { $proc.Kill(); $proc.WaitForExit(5000) | Out-Null }
            $results += [pscustomobject]([ordered]@{ scene = $job.Key; entities = $job.Entities; playMode = [bool]$job.NeedsPlay; error = "load: $($_.Exception.Message)" })
            continue
        }
        $startupSeconds = [math]::Round(((Get-Date) - $t0).TotalSeconds, 1)
        Write-Host "  startup+load: ${startupSeconds}s  MCP: $($discovery.url)"

        $row = [ordered]@{
            scene = $job.Key; entities = $job.Entities; playMode = [bool]$job.NeedsPlay
            startupLoadSeconds = $startupSeconds
        }
        $raw = [ordered]@{ scene = $job.Key; discoveryUrl = $discovery.url; samples = @() }

        try {
            $script:rpcUrl = $discovery.url
            $script:rpcHeaders = @{
                'Authorization' = "Bearer $($discovery.token)"
                'Content-Type'  = 'application/json'
                'Accept'        = 'application/json, text/event-stream'
            }
            $null = Invoke-Rpc 'initialize' @{ protocolVersion = '2025-06-18'; capabilities = @{}; clientInfo = @{ name = 'perf-battery'; version = '1.0' } }
            $null = Invoke-WebRequest -Uri $script:rpcUrl -Method Post -Headers $script:rpcHeaders -Body (@{ jsonrpc = '2.0'; method = 'notifications/initialized' } | ConvertTo-Json)

            # 4. fixed viewport (do NOT maximize the window; override fights it)
            $null = Invoke-Tool 'olo_viewport_set_size' @{ width = 1920; height = 1080 }

            # Edit-mode scenes render through the EDITOR camera — pose it to the
            # scene's intended framing (Play mode uses the in-scene camera).
            if (-not $job.NeedsPlay -and $job.Camera) {
                $null = Invoke-Tool 'olo_camera_set_pose' @{
                    position = @($job.Camera.position); target = @($job.Camera.target)
                }
            }

            $summary = Get-ToolJson 'olo_scene_summary' @{}
            if ($summary) { $row.entitiesLoaded = $summary.entityCount }

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
            Write-Warning "  measurement failed: $($_.Exception.Message)"
            $row.error = $_.Exception.Message
        }
        finally {
            # 5. kill + snapshot log
            $logText = Read-EngineLog
            if ($logText) { Set-Content -Path (Join-Path $OutDir "$($job.Key).log") -Value $logText -NoNewline }
            if ($proc -and -not $proc.HasExited) { $proc.Kill(); $proc.WaitForExit(5000) | Out-Null }
            Start-Sleep -Seconds 2
        }

        $results += [pscustomobject]$row
        ($raw | ConvertTo-Json -Depth 8) | Set-Content -Path (Join-Path $OutDir "$($job.Key).json")
    }
}
finally {
    Set-ContentRetry $projFile $projBackup
    Remove-Item $discoveryPath -ErrorAction SilentlyContinue
    Write-Host "Restored $projFile"
}

# 6. results
$results | ConvertTo-Json -Depth 4 | Set-Content -Path (Join-Path $OutDir 'results.json')

$md = @()
$md += "# Perf stress battery — $Config, $(Get-Date -Format 'yyyy-MM-dd HH:mm')"
$md += ""
$md += "Settle ${SettleSeconds}s, $Samples samples averaged. Viewport 1920x1080 via olo_viewport_set_size."
$md += ""
$md += "| scene | entities | play | load s | fps | frame ms | cpu ms | gpu ms | gpuWait ms | draws | tris | max ms | probe | error |"
$md += "|---|---|---|---|---|---|---|---|---|---|---|---|---|---|"
foreach ($r in $results) {
    $probe = if ($null -eq $r.workloadActive) { '' } elseif ($r.workloadActive) { 'moving' } else { 'STATIC!' }
    $md += "| $($r.scene) | $($r.entities) | $($r.playMode) | $($r.startupLoadSeconds) | $($r.fps) | $($r.frameMs) | $($r.cpuMs) | $($r.gpuMs) | $($r.gpuWaitMs) | $($r.drawCalls) | $($r.triangles) | $($r.maxFrameMs) | $probe | $($r.error) |"
}
$md += ""
$md += "## GPU pass timings (avg ms)"
$md += ""
foreach ($r in $results) {
    if ($r.passes) { $md += "- **$($r.scene)**: $($r.passes)" }
}
$md -join "`n" | Set-Content -Path (Join-Path $OutDir 'results.md')

Write-Host "`nResults: $OutDir\results.md" -ForegroundColor Green
$results | Format-Table scene, entities, startupLoadSeconds, fps, frameMs, gpuMs, gpuWaitMs, drawCalls -AutoSize
