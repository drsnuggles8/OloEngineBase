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

$ErrorActionPreference = 'Stop'

$discoveryPath = Join-Path $env:TEMP 'oloengine-mcp.json'
if (-not (Test-Path $discoveryPath)) {
    Write-Error "Discovery file not found at $discoveryPath. Is the editor running with OLO_MCP_AUTOSTART=1?"
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

# 19-20. Shader tools
Show-Tool 'olo_shader_errors' @{} | Out-Null
Show-Tool 'olo_shader_get' @{ name = 'PostProcess_SSR' } | Out-Null

# 21. Assets
Show-Tool 'olo_assets_list' @{ pageSize = 5 } | Out-Null

# 22. Crash reports
Show-Tool 'olo_crash_list' @{} | Out-Null

# 23-24. Prompts (third MCP primitive)
$plist = Invoke-Rpc 'prompts/list' $null
Write-Host "prompts/list -> $($plist.result.prompts.Count) prompts: $(@($plist.result.prompts | ForEach-Object { $_.name }) -join ', ')" -ForegroundColor Green
$pget = Invoke-Rpc 'prompts/get' @{ name = 'diagnose-performance' }
$msg = $pget.result.messages[0].content.text
Write-Host "prompts/get diagnose-performance -> role=$($pget.result.messages[0].role), $($msg.Length) chars" -ForegroundColor Green

Write-Host "`nAll MCP round-trips succeeded." -ForegroundColor Cyan
