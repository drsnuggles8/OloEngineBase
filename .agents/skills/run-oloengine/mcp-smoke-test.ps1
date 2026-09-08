# Smoke test for the OloEditor read-only MCP diagnostics server (#285).
#
# Drives the full Streamable-HTTP + JSON-RPC + MCP loop against a RUNNING editor:
#   initialize -> tools/list -> tools/call(olo_log_tail) -> tools/call(olo_scene_summary)
#
# The editor must be running with the server started. Easiest: launch OloEditor with
# the env var OLO_MCP_AUTOSTART=1 (optionally OLO_MCP_PORT=<port>); on start it writes
# a discovery file to %TEMP%\oloengine-mcp.json with the port + auth token, which this
# script reads. Not committed — a developer convenience for verifying the round trip.
#
# Usage:  pwsh -File smoke_test.ps1

param(
    # Path to the editor's MCP discovery JSON. Precedence: this flag, then the
    # OLO_MCP_DISCOVERY_FILE env var (the per-worktree path the run-oloengine
    # `attach` action uses), then the legacy %TEMP%\oloengine-mcp.json (the
    # default-port / manual-start location).
    [string]$DiscoveryPath
)

$ErrorActionPreference = 'Stop'

if (-not $DiscoveryPath) {
    $DiscoveryPath = if ($env:OLO_MCP_DISCOVERY_FILE) { $env:OLO_MCP_DISCOVERY_FILE } else { Join-Path $env:TEMP 'oloengine-mcp.json' }
}
$discoveryPath = $DiscoveryPath
if (-not (Test-Path $discoveryPath)) {
    Write-Error "Discovery file not found at $discoveryPath. Is the editor running with OLO_MCP_AUTOSTART=1 (pass -DiscoveryPath or set OLO_MCP_DISCOVERY_FILE for a per-worktree attach)?"
}

$discovery = Get-Content $discoveryPath -Raw | ConvertFrom-Json
$url = $discovery.url
$token = $discovery.token
Write-Host "Discovered MCP server: $url" -ForegroundColor Cyan

$headers = @{
    'Authorization' = "Bearer $token"
    'Content-Type'  = 'application/json'
    'Accept'        = 'application/json, text/event-stream'
}

$script:nextId = 0
function Invoke-Rpc([string]$method, $params) {
    $script:nextId++
    $body = @{ jsonrpc = '2.0'; id = $script:nextId; method = $method }
    if ($null -ne $params) { $body.params = $params }
    $json = $body | ConvertTo-Json -Depth 10
    $resp = Invoke-WebRequest -Uri $url -Method Post -Headers $headers -Body $json
    $sid = $resp.Headers['Mcp-Session-Id']
    if ($sid) {
        if ($sid -is [System.Array]) { $sid = $sid[0] }
        $script:sessionId = [string]$sid
    }
    return ($resp.Content | ConvertFrom-Json)
}

# 1. initialize
$init = Invoke-Rpc 'initialize' @{ protocolVersion = '2025-06-18'; capabilities = @{}; clientInfo = @{ name = 'smoke-test'; version = '0.0.1' } }
Write-Host "initialize -> protocolVersion=$($init.result.protocolVersion), server=$($init.result.serverInfo.name)" -ForegroundColor Green
if ($script:sessionId) { $headers['Mcp-Session-Id'] = $script:sessionId; Write-Host "  session: $($script:sessionId)" }

# 2. notifications/initialized (no id -> 202, no body)
$null = Invoke-WebRequest -Uri $url -Method Post -Headers $headers -Body (@{ jsonrpc='2.0'; method='notifications/initialized' } | ConvertTo-Json)

# 3. tools/list
$tools = Invoke-Rpc 'tools/list' $null
Write-Host "tools/list -> $($tools.result.tools.Count) tools:" -ForegroundColor Green
$tools.result.tools | ForEach-Object { Write-Host "  - $($_.name): $($_.description.Substring(0,[Math]::Min(70,$_.description.Length)))..." }

# 4. tools/call olo_log_tail
$logs = Invoke-Rpc 'tools/call' @{ name = 'olo_log_tail'; arguments = @{ count = 10 } }
Write-Host "tools/call olo_log_tail (isError=$($logs.result.isError)) ->" -ForegroundColor Green
Write-Host $logs.result.content[0].text

# 5. tools/call olo_scene_summary  (exercises the main-thread marshal primitive)
$scene = Invoke-Rpc 'tools/call' @{ name = 'olo_scene_summary'; arguments = @{} }
Write-Host "tools/call olo_scene_summary (isError=$($scene.result.isError)) ->" -ForegroundColor Green
Write-Host $scene.result.content[0].text

# 6. tools/call olo_scene_list_entities  (main-marshaled, paginated)
$list = Invoke-Rpc 'tools/call' @{ name = 'olo_scene_list_entities'; arguments = @{ pageSize = 5 } }
Write-Host "tools/call olo_scene_list_entities (isError=$($list.result.isError)) ->" -ForegroundColor Green
Write-Host $list.result.content[0].text
$listObj = $list.result.content[0].text | ConvertFrom-Json

# 7. tools/call olo_scene_get_entity  (drill-down using the first listed id)
if ($listObj.entities.Count -gt 0) {
    $firstId = $listObj.entities[0].id
    $ent = Invoke-Rpc 'tools/call' @{ name = 'olo_scene_get_entity'; arguments = @{ id = $firstId } }
    Write-Host "tools/call olo_scene_get_entity(id=$firstId) (isError=$($ent.result.isError)) ->" -ForegroundColor Green
    Write-Host $ent.result.content[0].text
} else {
    Write-Host "(no entities to drill into)" -ForegroundColor Yellow
}

# 8. tools/call olo_memory_report  (lock-safe)
$mem = Invoke-Rpc 'tools/call' @{ name = 'olo_memory_report'; arguments = @{} }
Write-Host "tools/call olo_memory_report (isError=$($mem.result.isError)) ->" -ForegroundColor Green
Write-Host $mem.result.content[0].text

# 9. tools/call olo_script_get_api csharp (typeFilter=Input)
$csapi = Invoke-Rpc 'tools/call' @{ name = 'olo_script_get_api'; arguments = @{ language = 'csharp'; typeFilter = 'Input' } }
Write-Host "tools/call olo_script_get_api(csharp, Input) (isError=$($csapi.result.isError)) ->" -ForegroundColor Green
$csObj = $csapi.result.content[0].text | ConvertFrom-Json
Write-Host "  typeCount=$($csObj.typeCount) matched=$($csObj.matched); first match: $($csObj.types[0].name) with $($csObj.types[0].members.Count) members"

# 10. tools/call olo_script_get_api lua (index)
$luaapi = Invoke-Rpc 'tools/call' @{ name = 'olo_script_get_api'; arguments = @{ language = 'lua' } }
$luaObj = $luaapi.result.content[0].text | ConvertFrom-Json
Write-Host "tools/call olo_script_get_api(lua) -> typeCount=$($luaObj.typeCount); e.g. $(@($luaObj.types | Select-Object -First 5 | ForEach-Object { $_.name }) -join ', ')" -ForegroundColor Green

# 11. resources/list
$reslist = Invoke-Rpc 'resources/list' $null
Write-Host "resources/list -> $($reslist.result.resources.Count) resources:" -ForegroundColor Green
$reslist.result.resources | ForEach-Object { Write-Host "  - $($_.uri) ($($_.mimeType))" }

# 12. resources/read olo://scene/current  (main-marshaled SerializeToYAML)
$sceneRes = Invoke-Rpc 'resources/read' @{ uri = 'olo://scene/current' }
$yaml = $sceneRes.result.contents[0].text
Write-Host "resources/read olo://scene/current -> $($yaml.Length) chars of YAML; first line: $(($yaml -split "`n")[0])" -ForegroundColor Green

# 13. tools/call olo_script_get_last_errors  (lock-safe ring buffer)
$errs = Invoke-Rpc 'tools/call' @{ name = 'olo_script_get_last_errors'; arguments = @{ count = 10 } }
Write-Host "tools/call olo_script_get_last_errors (isError=$($errs.result.isError)) ->" -ForegroundColor Green
Write-Host $errs.result.content[0].text

# 14. tools/call olo_screenshot  (main-marshaled GL readback -> PNG image content)
$shot = Invoke-Rpc 'tools/call' @{ name = 'olo_screenshot'; arguments = @{ maxWidth = 512 } }
$block = $shot.result.content[0]
Write-Host "tools/call olo_screenshot (isError=$($shot.result.isError)) -> type=$($block.type), mimeType=$($block.mimeType)" -ForegroundColor Green
if ($block.type -eq 'image' -and $block.data) {
    $bytes = [Convert]::FromBase64String($block.data)
    $isPng = ($bytes.Length -ge 8 -and $bytes[0] -eq 0x89 -and $bytes[1] -eq 0x50 -and $bytes[2] -eq 0x4E -and $bytes[3] -eq 0x47)
    $outPng = Join-Path $env:TEMP 'olo-mcp-screenshot.png'
    [IO.File]::WriteAllBytes($outPng, $bytes)
    Write-Host "  decoded $($bytes.Length) bytes, valid PNG signature = $isPng, saved to $outPng" -ForegroundColor Green
    if (-not $isPng) { Write-Error 'Screenshot is not a valid PNG' }
} else {
    Write-Error 'Screenshot did not return an image content block'
}

function Show-Tool([string]$name, $arguments) {
    $r = Invoke-Rpc 'tools/call' @{ name = $name; arguments = $arguments }
    $txt = $r.result.content[0].text
    $oneLine = ($txt -replace "\s+", ' ')
    if ($oneLine.Length -gt 160) { $oneLine = $oneLine.Substring(0, 160) + ' ...' }
    Write-Host "tools/call $name (isError=$($r.result.isError)) -> $oneLine" -ForegroundColor Green
    return $r
}

# 15-18. Performance tools
Show-Tool 'olo_perf_snapshot' @{} | Out-Null
Show-Tool 'olo_perf_bottlenecks' @{} | Out-Null
Show-Tool 'olo_perf_frame_history' @{ points = 8 } | Out-Null
Show-Tool 'olo_perf_capture_frame' @{ topK = 5 } | Out-Null
$passTimings = Show-Tool 'olo_perf_pass_timings' @{}
# ScenePass sub-pass split (#316): ScenePass should carry subPasses
# (DepthPrepass and/or Color) once GPU results have resolved. Soft check — the
# first frames after attach may not have resolved timings yet.
if ($passTimings.result -and -not $passTimings.result.isError) {
    $ptObj = $passTimings.result.content[0].text | ConvertFrom-Json
    $scenePass = $ptObj.passes | Where-Object { $_.pass -eq 'ScenePass' } | Select-Object -First 1
    if ($scenePass -and $scenePass.subPasses) {
        $subNames = ($scenePass.subPasses | ForEach-Object { "$($_.name)=$($_.gpuMs)ms" }) -join ', '
        Write-Host "  ScenePass subPasses: $subNames" -ForegroundColor Green
    } else {
        Write-Host '  (no ScenePass subPasses yet — GPU timings unresolved or 2D mode)' -ForegroundColor Yellow
    }
}

# 19-20. Shader tools (olo_shader_list discovers a real name for olo_shader_get)
Show-Tool 'olo_shader_errors' @{} | Out-Null
$shaders = Show-Tool 'olo_shader_list' @{}
$shObj = $shaders.result.content[0].text | ConvertFrom-Json
if ($shObj.shaders.Count -gt 0) {
    Show-Tool 'olo_shader_get' @{ name = $shObj.shaders[0].name } | Out-Null
}

# 21-22. Assets
Show-Tool 'olo_assets_list' @{ pageSize = 5 } | Out-Null
Show-Tool 'olo_assets_problems' @{} | Out-Null

# A1 filtering: warnings+ only, and a tag filter
Show-Tool 'olo_log_tail' @{ count = 5; minLevel = 'warn' } | Out-Null

# 22. Crash reports
Show-Tool 'olo_crash_list' @{} | Out-Null

# 25-29. Tier-0 camera / viewport control + render-target capture (#316)
$camGet = Show-Tool 'olo_camera_get' @{}
$camObj = $camGet.result.content[0].text | ConvertFrom-Json
Show-Tool 'olo_camera_set_pose' @{ position = @(0, 5, 10); target = @(0, 0, 0) } | Out-Null
Show-Tool 'olo_camera_orbit' @{ target = @(0, 0, 0); yaw = 45; pitch = 30; distance = 15 } | Out-Null

# frame an entity if one exists
if ($listObj.entities.Count -gt 0) {
    Show-Tool 'olo_camera_frame_entity' @{ id = $listObj.entities[0].id } | Out-Null
}

# restore the original camera state: when it had a live orbit pivot (distance > 0)
# restore via olo_camera_orbit so focal point + distance survive; otherwise restore
# the free pose. Either way restore the FOV too.
if ($camObj.distance -gt 0) {
    Show-Tool 'olo_camera_orbit' @{ target = @($camObj.focalPoint[0], $camObj.focalPoint[1], $camObj.focalPoint[2]); yaw = $camObj.yawDegrees; pitch = $camObj.pitchDegrees; distance = $camObj.distance; fov = $camObj.fovDegrees } | Out-Null
} else {
    Show-Tool 'olo_camera_set_pose' @{ position = @($camObj.position[0], $camObj.position[1], $camObj.position[2]); yaw = $camObj.yawDegrees; pitch = $camObj.pitchDegrees; fov = $camObj.fovDegrees } | Out-Null
}

# viewport override + reset. While the override is active, olo_perf_snapshot's
# renderWidth/renderHeight must match it (#316: the render graph once silently
# stayed at window size, inflating override readings ~6x on a 4K monitor).
Show-Tool 'olo_viewport_set_size' @{ width = 1280; height = 720 } | Out-Null
Start-Sleep -Milliseconds 250 # let a couple of frames apply the override
$snapAtOverride = Show-Tool 'olo_perf_snapshot' @{}
if ($snapAtOverride.result -and -not $snapAtOverride.result.isError) {
    $snapObj = $snapAtOverride.result.content[0].text | ConvertFrom-Json
    if ($snapObj.renderWidth) {
        if ([int]$snapObj.renderWidth -eq 1280 -and [int]$snapObj.renderHeight -eq 720) {
            Write-Host "  renderWidth/Height match the 1280x720 override" -ForegroundColor Green
        } else {
            # Note: the framebuffer is viewport x HiDPI scale, so e.g. 1920x1080 at
            # 150% display scaling is expected — only a WILD mismatch (window-sized
            # target while an override is active) indicates the #316 resize fight.
            Write-Warning "renderWidth/Height ($($snapObj.renderWidth)x$($snapObj.renderHeight)) differ from the 1280x720 viewport override (HiDPI scale? resize fight?)"
        }
    } else {
        Write-Host '  (no renderWidth in snapshot — render graph not live?)' -ForegroundColor Yellow
    }
}
Show-Tool 'olo_viewport_set_size' @{ reset = $true } | Out-Null

# olo_renderer_settings_set is a consented WRITE tool: with the editor's
# "Allow writes" gate off (the default) the call must be REFUSED cleanly; with
# it on, the no-arg form lists every setting (upscale/tonemap/renderpath +
# the #316 perf levers depthprepass/softshadows). Either outcome is a pass here.
$settingsList = Invoke-Rpc 'tools/call' @{ name = 'olo_renderer_settings_set'; arguments = @{} }
if ($settingsList.error) {
    Write-Host "tools/call olo_renderer_settings_set -> refused (writes gate off): $($settingsList.error.message)" -ForegroundColor Yellow
} else {
    $settingsObj = $settingsList.result.content[0].text | ConvertFrom-Json
    $tokens = ($settingsObj.settings | ForEach-Object { "$($_.setting)=$($_.currentValue)" }) -join ', '
    Write-Host "tools/call olo_renderer_settings_set (introspect) -> $tokens" -ForegroundColor Green
}

# 30. olo_screenshot with a one-shot camera pose (save/restore path).
# Orbit the sandbox terrain's centre (terrain spans [0,256]^2) — orbiting the
# world origin puts the camera underground here and captures a black frame.
$posedShot = Invoke-Rpc 'tools/call' @{ name = 'olo_screenshot'; arguments = @{ maxWidth = 512; orbit = @{ target = @(128, 24, 128); yaw = 90; pitch = 35; distance = 300 }; settleFrames = 2 } }
$posedBlock = $posedShot.result.content | Where-Object { $_.type -eq 'image' } | Select-Object -First 1
if ($posedBlock -and $posedBlock.data) {
    $bytes = [Convert]::FromBase64String($posedBlock.data)
    $outPng = Join-Path $env:TEMP 'olo-mcp-screenshot-posed.png'
    [IO.File]::WriteAllBytes($outPng, $bytes)
    Write-Host "tools/call olo_screenshot(orbit) -> $($bytes.Length) bytes, saved to $outPng" -ForegroundColor Green
} else {
    Write-Error 'Posed screenshot did not return an image content block'
}

# 31-32. Render-target listing + capture. The tool errors (plain-text content,
# not JSON) when no render graph is active, so guard before parsing.
$targets = Show-Tool 'olo_render_list_targets' @{}
$targetsObj = $null
if (-not $targets.result -or $targets.result.isError -or -not $targets.result.content) {
    Write-Host '(no render targets — editor not in 3D mode?)' -ForegroundColor Yellow
} else {
    $targetsObj = $targets.result.content[0].text | ConvertFrom-Json
}
if ($targetsObj -and $targetsObj.count -gt 0) {
    foreach ($targetName in @('SceneColor', 'SceneDepth')) {
        if ($targetsObj.targets | Where-Object { $_.name -eq $targetName }) {
            $cap = Invoke-Rpc 'tools/call' @{ name = 'olo_render_capture_target'; arguments = @{ name = $targetName; maxWidth = 512 } }
            $img = $cap.result.content | Where-Object { $_.type -eq 'image' } | Select-Object -First 1
            $meta = $cap.result.content | Where-Object { $_.type -eq 'text' } | Select-Object -First 1
            if ($img -and $img.data) {
                $bytes = [Convert]::FromBase64String($img.data)
                $outPng = Join-Path $env:TEMP "olo-mcp-target-$targetName.png"
                [IO.File]::WriteAllBytes($outPng, $bytes)
                Write-Host "tools/call olo_render_capture_target($targetName) -> $($bytes.Length) bytes, saved to $outPng" -ForegroundColor Green
                Write-Host "  meta: $(($meta.text -replace '\s+', ' '))"
            } else {
                Write-Error "olo_render_capture_target($targetName) did not return an image"
            }
        }
    }
} elseif ($targetsObj) {
    Write-Host '(no render targets — editor not in 3D mode?)' -ForegroundColor Yellow
}

# 33-34. Prompts (third MCP primitive)
$plist = Invoke-Rpc 'prompts/list' $null
Write-Host "prompts/list -> $($plist.result.prompts.Count) prompts: $(@($plist.result.prompts | ForEach-Object { $_.name }) -join ', ')" -ForegroundColor Green
$pget = Invoke-Rpc 'prompts/get' @{ name = 'diagnose-performance' }
$msg = $pget.result.messages[0].content.text
Write-Host "prompts/get diagnose-performance -> role=$($pget.result.messages[0].role), $($msg.Length) chars" -ForegroundColor Green

Write-Host "`nAll MCP round-trips succeeded." -ForegroundColor Cyan
