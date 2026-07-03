# MCP diagnostics server (read-only)

OloEditor can host a **localhost-only, read-only [MCP](https://modelcontextprotocol.io)
server** so you can point your own LLM agent (Claude Code, Claude Desktop, …) at the
*running editor* and get grounded help debugging your game — using the diagnostics the
engine already collects (logs, scene/ECS state, scripting errors and API, performance,
memory, shaders, assets, crash reports, and a live screenshot) — plus a Tier-0
rendering-dev harness (camera control, viewport sizing, intermediate render-target
capture, golden-image comparison, and ephemeral render-override A/B; issue #316).

Strategy is **"expose, don't embed"**: OloEditor does not ship a chat panel, an API key,
or a model. It exposes data over a standard protocol; you bring your own agent. (Issue #285.)

## Security model

- Binds **`127.0.0.1` only** — never a routable interface.
- **Off by default**; you start it explicitly (panel button or an env var).
- Every request must carry a **bearer token** the editor generates and displays. A fresh
  token is minted each time you start the server.
- The `Origin` header is validated (DNS-rebinding defence) and the dispatch layer is
  **read-only with respect to your project** — no tool writes scenes, assets, or files.
  The Tier-0 inspection tools (issue #316) may adjust *editor-only viewport state* — the
  editor camera pose and the viewport capture size — which is never persisted. The
  render-override tools (`olo_render_toggle_pass` / `olo_render_set_debug_view`) likewise
  edit only the renderer's *session-global* post-process / fog settings, never the loaded
  scene's own copy, so the change is ephemeral (a scene reload restores it) and never
  written to disk.
- Optional **path redaction** scrubs absolute filesystem paths from text output (toggle in
  the panel) for when you don't want project layout / usernames leaving the process.

## Enabling it

**From the editor UI:** `Window ▸ MCP Server`, set a port (default **7345**), click
**Start server**. The panel then shows the endpoint URL, the auth token, and a ready-to-paste
`claude mcp add` command (with copy buttons).

**For automation / headless:** set `OLO_MCP_AUTOSTART=1` (optionally `OLO_MCP_PORT=<port>`)
before launching OloEditor; the server starts during editor init.

When running, the server writes a **discovery file** containing the host, port, token, and
URL — handy for scripts/agents that read it instead of copy-paste. It's removed when the
server stops. The path is resolved in this order:

1. **`OLO_MCP_DISCOVERY_FILE`** (verbatim, when set & non-empty) — the launching tool picks
   the exact path it will read back, so **parallel worktree editors never collide** even
   when several run at once.
2. Otherwise the OS temp dir: the **default port (7345)** keeps the legacy single name
   `%TEMP%/oloengine-mcp.json` (`$TMPDIR` on POSIX); **any other port** namespaces the file
   as `oloengine-mcp-<port>.json` so two editors on distinct ports don't overwrite each
   other's host/token.

The `run-oloengine` skill's `attach` action automates this end-to-end: it picks a stable
per-worktree port + `OLO_MCP_DISCOVERY_FILE`, launches the editor with the server
auto-started, then runs `claude mcp add` so the `olo_*` tools attach to the session.

## Headless attach — the offscreen framebuffer (issue #316)

The editor host above reads a **live viewport**. But the renderer's real verification
happens **headless**: the test harness renders into an *offscreen* render-graph
framebuffer (`RendererAttachedTest::RunEditorFrames` → the `UIComposite` RT the visual
evidence tests read back — `WaterVisualEvidenceTest`, `SceneRenderEvidenceTest`). CLAUDE.md
requires every rendering change to be *visually* verified, and a renderer worktree often has
no editor running — just the test binary. The **headless attach** path makes the read-only
visual tools (`olo_screenshot`, `olo_shader_errors`, and `olo_render_capture_target`) work
against that offscreen framebuffer, so an agent can screenshot exactly what the headless
pipeline drew.

The seam is that `McpServer` is already decoupled from the editor — it only reads an
`EditorMcpContext` (a struct of hooks) and marshals main-thread work onto the engine
**GameThread** task queue. The test-side host
([`OloEngine/tests/Rendering/PropertyTests/McpHeadlessHost.h`](../../OloEngine/tests/Rendering/PropertyTests/McpHeadlessHost.h))
wires that context to the offscreen composite framebuffer + an `EditorCamera`, starts the
server, and **pumps the GameThread queue itself** (`FNamedThreadManager::ProcessTasks`) from
the GL-context thread while a tool call runs on a worker thread — preserving the
`MarshalRead` contract (handlers run on a non-game thread; the pumping thread services their
jobs). It captures the same `UIComposite` RT0 the editor viewport samples, so the screenshot
is byte-for-byte the headless frame.

Two entry points, both in
[`McpHeadlessAttachTest.cpp`](../../OloEngine/tests/Rendering/PropertyTests/McpHeadlessAttachTest.cpp):

- **`ScreenshotAndShaderErrorsOverMcp`** — the CI regression guard. Renders a lit cube into
  the offscreen FB, hosts the server in-process, issues real JSON-RPC `tools/call`
  dispatches, and asserts `olo_screenshot` returns a decodable PNG of the offscreen frame
  and `olo_shader_errors` a well-formed report. It **SKIPs** (not fails) without a GL 4.6
  context, exactly like the other visual-evidence tests.
- **`HostUntilDetached`** — the interactive headless-attach mode. It **SKIPs** in normal CI;
  set `OLO_MCP_HEADLESS_ATTACH=1` (plus the usual `OLO_MCP_PORT` / `OLO_MCP_DISCOVERY_FILE`)
  and it renders + hosts the server live, pumping until a stop signal, so an external agent
  attached over the bound socket can screenshot the offscreen FB from a renderer worktree's
  headless loop:

  ```bash
  # Host the offscreen frame over MCP from the test binary (port + discovery file
  # land where `claude mcp add` expects them):
  OLO_MCP_HEADLESS_ATTACH=1 OLO_MCP_PORT=21000 \
  OLO_MCP_DISCOVERY_FILE="$TMPDIR/oloengine-mcp-21000.json" \
    build/OloEngine/tests/Debug/OloEngine-Tests.exe \
    --gtest_filter='McpHeadlessAttachTest.HostUntilDetached'
  # …then `claude mcp add` with the token from the discovery file (see "Attaching an agent").
  ```

  The session runs for `OLO_MCP_ATTACH_SECONDS` (default 600, capped 7200), or stops early
  when the sentinel file `<discovery-file>.stop` is created and then removed.

Honesty boundary: the headless host wires the read-only/inspection hooks only —
`olo_camera_frame_entity`, `olo_viewport_set_size`, and the consented project-write tools
return a clean "not available" because the test owns no editor undo stack / panel viewport.
The screenshot, shader, scene, perf, physics, and render-capture/override tools all work.

## Attaching an agent

### Claude Code

```bash
claude mcp add --transport http oloeditor http://127.0.0.1:7345/mcp \
  --header "Authorization: Bearer <TOKEN-FROM-THE-PANEL>"
```

(The panel's "Copy command" button produces this exact line with the live token/port.)
Then in a Claude Code session the `olo_*` tools, the `olo://…` resources, and the prompts
are available.

### Claude Desktop

Add the server to `claude_desktop_config.json` (Settings ▸ Developer ▸ Edit Config). For a
build with native Streamable-HTTP MCP support:

```json
{
  "mcpServers": {
    "oloeditor": {
      "url": "http://127.0.0.1:7345/mcp",
      "headers": { "Authorization": "Bearer <TOKEN-FROM-THE-PANEL>" }
    }
  }
}
```

If your Claude Desktop build only speaks stdio, bridge with
[`mcp-remote`](https://www.npmjs.com/package/mcp-remote):

```json
{
  "mcpServers": {
    "oloeditor": {
      "command": "npx",
      "args": ["-y", "mcp-remote", "http://127.0.0.1:7345/mcp",
               "--header", "Authorization: Bearer <TOKEN-FROM-THE-PANEL>"]
    }
  }
}
```

Restart Claude Desktop after editing the config. The token changes every time you restart
the server, so update the config (or re-copy from the panel) accordingly.

## What's exposed

### Tools

| Tool | What it returns |
|---|---|
| `olo_log_tail` | recent engine log lines, filterable by `minLevel` and `tag` |
| `olo_events_tail` | unified "what just happened?" timeline — scene load, play/stop, entity spawn/destroy, asset reload, script error — newest last with a monotonic `id`; incremental polling via `sinceId`, plus a `categories` filter |
| `olo_scene_summary` | active scene name, play state, entity count |
| `olo_scene_open` | **(consented write)** open / switch the active scene by `path` (a `.olo`/`.scene` file, relative paths resolve against the project asset directory) — the scriptable scene switch. Loads directly, bypassing the auto-save recovery modal a remote agent can't click; stops Play mode first. Reports the loaded scene name + entity count. Gated behind **Agent writes** |
| `olo_scene_play` / `olo_scene_stop` | **(consented write)** enter / leave Play mode — the same as the editor's Play/Stop buttons, so an agent can verify anything that only runs in Play (physics, cloth, scripts). Transient + fully reversible (stop restores the authored scene); idempotent (`changed:false` when already in that state); `olo_scene_summary` reports `isPlaying` to confirm. Gated behind **Agent writes** |
| `olo_scene_list_entities` | paginated entity list (id, name, parent, child count) + name filter |
| `olo_scene_get_entity` | one entity's full component data (YAML) by UUID |
| `olo_perf_snapshot` | fps, frame/CPU/GPU time (real whole-frame GPU timer), `gpuWaitMs` (CPU blocked on the GPU fence — the direct GPU-bound signal), draw calls, instancing, triangles, plus `renderWidth`/`renderHeight` — the ACTUAL SceneColor render resolution; cross-check it against any `olo_viewport_set_size` override before trusting timings |
| `olo_perf_bottlenecks` | CPU/GPU/Memory/IO bottleneck + confidence + recommendations (uses real cpu/gpu/gpuWait numbers) |
| `olo_perf_frame_history` | downsampled recent-frame time series |
| `olo_perf_capture_frame` | triggers a real frame capture: stats + top-K draw commands by GPU time (per-draw times resolve via a deferred commit one-plus frames after the capture; draws carry their submesh debug names) |
| `olo_perf_pass_timings` | whole-frame GPU time split by render-graph pass (Shadow vs Scene vs GTAO vs Bloom vs ToneMap…): per-pass GPU (always-on timestamp queries) + CPU dispatch ms, frame totals incl. `gpuWaitMs`, and `unattributedGpuMs`. `ScenePass` carries `subPasses` splitting its GPU time into `DepthPrepass` vs `Color` (no DepthPrepass entry = prepass off; sub times are inside the parent's `gpuMs`, not additional) |
| `olo_render_frame_breakdown` | triggers a real frame capture and returns its **per-command / per-pipeline-stage** structural breakdown (the granularity `olo_perf_capture_frame` omits): pipeline stats + the ordered command list (type, debug-name pass label, draw key shader/material/depth, group, execution order, static flag, GPU time) + a command-type histogram, at the chosen `viewMode` (`presort`/`postsort`/`postbatch`); `format:"markdown"` returns the Command Bucket Inspector's LLM-analysis report (sort/state-change/batching analysis + optimization hints) |
| `olo_memory_report` | GPU/CPU memory total + per-type breakdown + suspected leaks |
| `olo_shader_list` | inventory of all registered shaders (id, name, hasErrors) |
| `olo_shader_errors` | shaders with compile/link errors |
| `olo_shader_get` | one shader's uniforms/buffers/samplers/instructions (+ optional GLSL) |
| `olo_shader_reload` | reload + recompile one shader from disk by name; returns post-reload status + the compile/link log (the shader inner loop) |
| `olo_assets_list` | paginated registered assets (handle, type, path) + type filter |
| `olo_assets_problems` | assets that failed to load or are missing/invalid |
| `olo_script_get_api` | C# / Lua scripting API digest (types + members), with a type filter |
| `olo_script_get_last_errors` | recent C# (Mono) / Lua (Sol2) script exceptions |
| `olo_reload_script` | **(consented write)** reload the C# script assembly — the editor's *Script ▸ Reload assembly* (Ctrl+R) path — so a rebuilt game assembly is picked up without restarting the editor; reports whether scripting is available, whether the reload ran, and the post-reload script-class count. Gated behind **Agent writes** (Disabled/Prompt/Allow all) |
| `olo_crash_list` / `olo_crash_get` | crash reports under `CrashReports/` |
| `olo_screenshot` | the viewport rendered to a PNG image block; optional one-shot camera pose (`camera`/`orbit` + `settleFrames`) with automatic save/restore of the user's camera |
| `olo_camera_get` | the editor camera's pose (position, focal point, yaw/pitch, FOV, clips, viewport size) |
| `olo_camera_set_pose` | move the editor camera: `position` + (`target` \| `yaw`/`pitch`), optional `fov` |
| `olo_camera_orbit` | orbit-frame the camera around a world point: `target`, `yaw`, `pitch`, `distance` |
| `olo_camera_frame_entity` | point the camera at an entity (by UUID) and fit it in view |
| `olo_viewport_set_size` | override the viewport's logical render size for deterministic captures (`reset` to clear). The override wins over window/panel resizes (the editor reasserts it after any OS window-resize event); verify with `olo_perf_snapshot`'s `renderWidth`/`renderHeight` before perf measurements |
| `olo_render_list_targets` | the render graph's live texture/framebuffer resources (name, kind, format, size, producers) |
| `olo_render_capture_target` | read back one intermediate render target (depth, normals, G-buffer, shadow map, AO, post-process stages, …) as a PNG image block; depth is min-max normalised by default |
| `olo_render_compare_golden` | capture the viewport (optional `camera`/`orbit` pose) and diff it against a golden PNG (`goldenPath`): returns a numeric `similarity`/`rmse`/`ssim` + `pass` verdict; missing golden or `rebase`:true writes the capture as the new baseline (the `OLOENGINE_GOLDEN_REBASE` workflow) |
| `olo_render_toggle_pass` | flip a post-process / fog feature on/off (`name` + optional `enabled`) — the ephemeral A/B loop: toggle off → `olo_screenshot` → toggle on → `olo_screenshot`. No `name` lists every pass + its live state |
| `olo_render_set_debug_view` | switch the viewport to a raw AO/SSR/SSGI buffer (`mode`: none/ssao/gtao/ssr/ssgi); reports whether the backing pass is actually running. No `mode` lists the modes + current state |
| `olo_renderer_settings_set` | **(consented write)** set a multi-valued, session-global renderer / post-process setting — `upscale` (FSR1 spatial-upscale mode), `tonemap` (operator), `renderpath` (forward/forward+/deferred), `depthprepass` (off/on/auto — the #316 perf lever), `softshadows` (pcf/pcss — THE ScenePass shadow-cost lever) — to verify a rendering feature live at each value. The enum-valued sibling of `olo_render_toggle_pass`; reports `previousValue` for restore-prior-value (no undo stack). No args lists every setting + current value + allowed values. Gated behind **Agent writes** (Disabled/Prompt/Allow all) |
| `olo_scene_set_time_of_day` | move the procedural sky's sun to a 24-hour clock time (`hours` 0–24) for lighting iteration — ephemeral session override of the sun direction, never written to the scene. `clear`:true restores the authored sun; no args reports the current override |
| `olo_scene_set_sun_angle` | aim the procedural sky's sun directly from a `yaw` (azimuth) / `pitch` (elevation) pair — the precise sibling of `olo_scene_set_time_of_day`, same ephemeral session override. `clear`:true restores; no args reports state |
| `olo_render_why_not_visible` | explain why one entity (`entity`) is NOT on screen — the "why can't I see my mesh?" debugger: root-cause `reasonCode`, summary, ordered checks, and the raw render facts |
| `olo_physics_layer_matrix` | the collision-layer matrix the sim uses: built-in object layers + user-defined layers, with pairwise collide/no-collide (works in Edit mode) |
| `olo_physics_list_colliders` | paginated entities with a rigidbody: authored body type / layer / trigger / collider shapes, plus live object layer, position, awake/asleep when playing |
| `olo_physics_contacts` | entity pairs whose bodies are touching right now (live active-contact set, deduplicated); requires Play mode |
| `olo_physics_raycast` | cast a ray (`origin` + `direction`\|`to`) through the live physics world: closest hit, or up to `maxHits` ordered hits (entity, position, normal, distance) |
| `olo_physics_overlap` | bodies overlapping a sphere (`radius`) or box (`halfExtents`) at `origin`; requires Play mode |
| `olo_physics_why_no_collision` | explain why two entities (`a`, `b`) are NOT colliding — the "player falls through the floor" debugger: root-cause `reasonCode`, summary, ordered checks, and per-entity facts |

### Write consent — Disabled / Prompt / Allow all (issue #306 item C)

Every project-mutating tool (`olo_set_collision_layer`, `olo_entity_set_field`,
`olo_reload_script`, `olo_renderer_settings_set`, `olo_scene_open`,
`olo_scene_play`, `olo_scene_stop` — anything marked **(consented write)** above)
is gated in the MCP panel by a three-way **Agent writes** control.
It is **off by default and never persisted**, so every editor launch starts read-only
and the human at the editor opts in for the session:

- **Disabled** (default) — a write tool is refused at dispatch with a clean JSON-RPC
  error; the server is read-only with respect to your project. The read-only
  diagnostics and the ephemeral editor-only tools (camera / viewport / render
  overrides) are unaffected by this control.
- **Prompt** — each write pops a **per-action consent modal** in the editor showing the
  tool and the exact arguments the agent wants to apply. The agent's `tools/call`
  **blocks** until you click **Approve** (apply this one), **Deny** (reject it — the
  call returns a "denied by the editor user" error), or **Approve all this session**
  (apply it and switch to *Allow all* for the rest of the session). An unanswered
  prompt times out (120 s) and the call returns a timeout error. The modal renders even
  when the MCP panel window is closed, so a write is never left silently blocked.
- **Allow all** — writes auto-apply for the session with no prompt (the legacy
  "Allow writes" behaviour). Use it when you're actively driving a batch of edits and
  don't want to click through each one.

Every entity/component write still routes through the editor's **undo stack** — an
approved change is a single **Ctrl-Z** — so *Prompt* and *Allow all* differ only in
whether you confirm each action up front, not in reversibility. (The renderer-settings
and sun/time-of-day writes are session-global and restore-prior-value instead; see
those sections.)

Threading: the write handler runs on a cpp-httplib worker thread and blocks there
while the main (UI) thread renders the modal and records your decision — the same
main-thread-marshal discipline the read tools use, so the editor's render loop never
blocks on an agent.

### Toolsets & on-demand tool discovery (`tools/search`)

The tool surface is large enough (51 tools) that paging the whole flat `tools/list`
to find the right one is wasteful. Every tool is tagged with a **toolset** (grouping
category), and a custom `tools/search` JSON-RPC method lets an agent discover tools by
keyword and/or category instead of pulling the entire list:

| Toolset | Tools |
|---|---|
| `diagnostics` | `olo_log_tail`, `olo_events_tail`, `olo_crash_list`, `olo_crash_get` |
| `scene` | `olo_scene_summary`, `olo_scene_list_entities`, `olo_scene_get_entity`, `olo_entity_list_fields`, `olo_entity_set_field`, `olo_scene_open`, `olo_scene_play`, `olo_scene_stop` |
| `perf` | `olo_memory_report`, `olo_perf_snapshot`, `olo_perf_bottlenecks`, `olo_perf_frame_history`, `olo_perf_capture_frame`, `olo_perf_pass_timings` |
| `render` | `olo_render_frame_breakdown`, `olo_render_list_targets`, `olo_render_graph_topology_export`, `olo_render_capture_target`, `olo_render_toggle_pass`, `olo_render_set_debug_view`, `olo_renderer_settings_set`, `olo_scene_set_time_of_day`, `olo_scene_set_sun_angle`, `olo_render_compare_golden`, `olo_render_why_not_visible` |
| `shader` | `olo_shader_list`, `olo_shader_errors`, `olo_shader_get`, `olo_shader_reload` |
| `assets` | `olo_assets_list`, `olo_assets_problems` |
| `scripting` | `olo_script_get_api`, `olo_script_get_last_errors`, `olo_reload_script` |
| `camera` | `olo_screenshot`, `olo_camera_get`, `olo_camera_set_pose`, `olo_camera_orbit`, `olo_camera_frame_entity`, `olo_viewport_set_size` |
| `physics` | `olo_physics_layer_matrix`, `olo_physics_list_colliders`, `olo_physics_contacts`, `olo_physics_raycast`, `olo_physics_overlap`, `olo_physics_why_no_collision` |

`tools/search` params (both optional):

- `query` — free text; whitespace-separated terms are ANDed and matched
  case-insensitively against each tool's name, title, description, and toolset.
- `toolset` — restrict to one category (case-insensitive exact match).

It returns `{ "tools": [...], "toolsets": [{ "name", "count" }, ...] }`. The `tools`
entries are the same shape as `tools/list` (so a tool can be called straight from a
search hit), plus a convenience top-level `toolset` field. The `toolsets` array is the
full category catalogue (every toolset + its tool count, regardless of the active
filter) so an agent can discover categories and refine. With no `query` and no
`toolset`, it returns every tool plus the catalogue.

This is **additive**: `tools/list` is unchanged (a standard MCP client that never calls
`tools/search` keeps working) — it now also carries each tool's toolset under the
spec's `_meta` extension key `io.oloengine/toolset`, which strict clients ignore.

### Multi-angle visual verification (the CLAUDE.md water pattern)

The camera tools exist so an agent can verify a rendering change from the angles where
it is most likely to break, without touching the user's viewport. The intended loop:

1. `olo_render_list_targets` → discover what the frame graph produced this frame.
2. `olo_screenshot { camera: { position, target } }` (or `orbit`) per angle — e.g. for
   water: from the side, straddling the waterline, fully submerged, top-down. Each call
   saves the user's camera, renders the pose for `settleFrames` frames, captures, and
   restores.
3. `olo_render_capture_target { name: "SceneDepth" }` (or `GBufferNormal`,
   `ShadowMapCSM`, `AOBuffer`, `BloomColor`, …) when the *final* frame looks wrong and
   you need to see which intermediate buffer broke.
4. `olo_viewport_set_size { width, height }` first when a deterministic resolution
   matters (golden comparisons); `{ reset: true }` when done.
5. `olo_render_compare_golden { goldenPath, camera }` to turn the eyeball check into a
   **numeric** pass/fail against a saved baseline (see below).

### Golden-image comparison (`olo_render_compare_golden`)

This is the numeric half of CLAUDE.md's "rendering changes MUST be visually verified"
rule — instead of eyeballing a screenshot, an agent gets a deterministic
`similarity` + `pass` verdict against a baseline PNG. It captures the viewport (from
an optional fixed `camera`/`orbit` pose, with the same save/restore as
`olo_screenshot`), then diffs the result against `goldenPath` using the **same
RMSE→SSIM metric as the `GoldenImageTests` suite**, so the MCP verdict agrees with the
`OLOENGINE_GOLDEN_REBASE` test workflow.

- **`goldenPath`** is a PNG under `assets/tests/visual/` (a bare name like
  `water_side.png` lands there; absolute paths and `..` traversal are rejected — the
  server stays read-only w.r.t. your project, only ever writing test artifacts).
- **Missing golden, or `rebase`:true → it *writes* the capture as the new baseline**
  and reports `created` instead of failing — the same "first run bootstraps, then you
  compare" loop as the suite. Re-baseline deliberately with `rebase`:true after an
  intended visual change.
- **Verdict:** by default the suite cascade decides (RMSE ≤ 0.004 → pass, ≥ 0.02 →
  fail, in-between → SSIM ≥ 0.985). Pass an explicit **`threshold`** (a minimum SSIM
  similarity in `[0,1]`) to override it with a single gate you control.
- Use the **same capture size** when creating and comparing (pin one with
  `olo_viewport_set_size`); otherwise the dimensions mismatch and the tool says so.

```jsonc
// First run (no golden yet) — bootstraps the baseline:
// olo_render_compare_golden { "goldenPath": "water_side.png",
//                             "camera": { "position": [12,3,0], "target": [0,0,0] } }
{ "goldenPath": "assets/tests/visual/water_side.png", "created": true, "rebased": false,
  "bytes": 48213, "message": "Golden created at assets/tests/visual/water_side.png …" }

// Later run after a shader change — compares against it:
// olo_render_compare_golden { "goldenPath": "water_side.png",
//                             "camera": { "position": [12,3,0], "target": [0,0,0] } }
{
  "goldenPath": "assets/tests/visual/water_side.png", "created": false,
  "pass": false, "dimensionsMatch": true,
  "actual": { "width": 1024, "height": 576 }, "golden": { "width": 1024, "height": 576 },
  "similarity": 0.913, "ssim": 0.913, "rmse": 0.071, "mse": 0.005,
  "threshold": 0.985, "thresholdMode": "suite-cascade",
  "mismatchPixels": 142880, "totalPixels": 589824, "maxChannelDelta": 203,
  "worstPixel": { "x": 511, "y": 300 },
  "message": "RMSE 0.071 >= 0.02 (hard fail); SSIM 0.913. Worst pixel (511,300) …"
}
```

The response also includes the captured frame as an image block, so the agent can SEE
what it just verdicted on alongside the numbers.

### The shader inner loop (`olo_shader_reload`)

`olo_shader_reload { name }` recompiles one shader from disk by name without restarting
the editor, so an agent can run the tight rendering inner loop: **edit the `.glsl` →
reload → read the compile/link log → screenshot → repeat.** It re-reads the file and
recompiles+links synchronously (force-finishing any async link) in both the Renderer3D
and Renderer2D shader libraries — the same path the editor's own *Shader ▸ Recompile*
action uses — and returns the post-reload `status`
(`ready`/`failed`/`compiling`/`pending`), the GL program `id`, which `libraries` held the
name, and the `log` (compile/link errors; empty on a clean reload). The status comes
straight off the shader object so it is correct in release builds too; the `log` text is
read from the shader debugger and is richest in debug builds. A worked loop:

```jsonc
// 1) olo_shader_list                          -> find the shader name
// 2) … edit OloEditor/assets/shaders/PBR_MultiLight.glsl on disk …
// 3) olo_shader_reload { "name": "PBR_MultiLight" }
{ "name": "PBR_MultiLight", "found": true, "libraries": ["Renderer3D"],
  "status": "ready", "ok": true, "rendererId": 195, "log": "" }
// 4) olo_screenshot { … }                     -> confirm the pixels changed as intended
```

A clean recompile returns `status: "ready"` with an empty `log`; on failure you get
`status: "failed"` and the compiler diagnostics in `log`.

**Which shaders are reloadable.** `olo_shader_list` reports *every* GL program the shader
debugger knows about, but only shaders owned by the Renderer3D / Renderer2D shader libraries
(the main scene shaders — `PBR_MultiLight`, `Water`, `Terrain_PBR`, `InfiniteGrid`, `Decal`,
`LightCube`, the `Renderer2D_*` shaders, …) can be hot-reloaded by name. Post-process and
compute shaders (`GTAO`, `SSAO`, `SSR`, bloom, …) are owned by their render pass and the
engine keeps no name-to-shader registry for them, so they are **not** reloadable; asking for
one returns an error that lists the names that *are* reloadable. To inspect a shader's
*existing* errors without recompiling, use `olo_shader_errors` / `olo_shader_get` instead.

**Debug-build caveat (verified).** In a Debug build, recompiling a shader that contains a
GLSL *syntax* error trips an engine debug assert (`OLO_CORE_VERIFY` → `__debugbreak`) on the
render/main thread — the same behaviour as the editor's own *Shader ▸ Recompile* button. The
reload then doesn't return a clean `status: "failed"`; instead the main-thread marshal times
out (~5 s) and the tool returns *"Timed out waiting for the editor main thread"*, and the
editor can crash. So reserve `olo_shader_reload` for applying an edit you **expect to
compile** (the normal inner-loop case — confirm the result `status` is `ready`, then
screenshot); to inspect a shader that you know is broken, read `olo_shader_errors` /
`olo_shader_get` rather than recompiling it.

### The scripting inner loop (`olo_reload_script`)

The scripting counterpart of `olo_shader_reload`: reload the **C# script assembly**
without restarting the editor, so an agent can run the tight scripting loop — **edit a
C# script → rebuild the game assembly → `olo_reload_script` → observe the new behaviour
→ repeat.** It drives the exact `ScriptEngine::ReloadAssembly()` path the editor's
*Script ▸ Reload assembly* (Ctrl+R) menu uses: it unloads the app domain and re-loads
the core + app assemblies, rediscovers the entity-script classes, and re-registers the
component bindings.

```jsonc
// 1) … edit your C# script + rebuild the game assembly (the .dll the editor loads) …
// 2) olo_reload_script {}
{ "language": "csharp", "available": true, "ok": true,
  "scriptClassCount": 7, "message": "Reloaded the C# app assembly (7 script class(es) registered)." }
```

- **It is a consented WRITE tool** (issue #306 item C): like the other writes it is
  refused while **Agent writes** is *Disabled* in the editor's MCP panel (the default),
  prompts for per-action consent in *Prompt* mode, and applies directly in *Allow all*
  — see [Write consent](#write-consent--disabled--prompt--allow-all-issue-306-item-c).
  Reloading runs the user's freshly-built assembly code, so it deliberately crosses the
  read-only line — hence the gate.
- **Whole-assembly, no arguments.** C# reload has no per-script granularity (the editor
  reloads the entire app assembly), so the tool takes no parameters — exactly mirroring
  the parameterless Ctrl+R.
- **Honest about availability.** When C# scripting is **disabled in the build** (no Mono
  on this platform) or **not yet initialized** (no core assembly loaded), the call still
  *succeeds* but reports `available: false` with an explanatory `message`, rather than
  pretending a reload happened. `scriptClassCount` is the number of entity-script classes
  registered after the reload — a non-zero value confirms the app assembly loaded.
- **Honest about failure.** `ok` reports whether the reload actually **succeeded**: if the
  freshly-built app assembly fails to load (e.g. a C# compile error, or a missing/locked
  `.dll`), the tool returns `available: true, ok: false` with a `message` pointing you at
  the engine log — the entity-class registry then keeps its stale pre-reload contents, so
  don't trust `scriptClassCount` on a failed reload. Rebuild the game assembly and retry.
- **Lua is not reloaded here.** This tool targets the C# (Mono) app assembly; Lua scripts
  re-execute per entity on play and have no single global reload entry point.

### Scriptable scene control (`olo_scene_open` / `olo_scene_play` / `olo_scene_stop`)

The **scriptable repro setup** (issue #316 Part 5): switch which scene is loaded and
toggle the runtime, so an agent can set up and drive a repro over MCP instead of the
old manual dance — editing `Sandbox.oloproj`'s `StartScene` + relaunching the editor
per target scene, and `OLO_EDITOR_AUTOPLAY=1` + a relaunch to reach Play.

- **`olo_scene_open { path }`** opens / switches the active scene. `path` is a
  `.olo` / `.scene` file; a relative path resolves against the project's asset
  directory (e.g. `"Scenes/Sandbox.olo"`), an absolute path also works, and `..`
  traversal is rejected. It loads the scene **directly** — the same install path as
  the editor's *File ▸ Open Scene*, but **without the auto-save recovery modal** (a
  remote agent can't click it; see below) and without a file dialog. If Play mode is
  running it is stopped first. The response reports whether the scene loaded (`ok`),
  the resolved `path`, and the new `sceneName` + `entityCount`.
- **`olo_scene_play {}` / `olo_scene_stop {}`** enter / leave Play mode — the same
  `OnScenePlay` / `OnSceneStop` the editor's Play / Stop toolbar buttons drive. This
  is what lets an agent verify anything that **only runs in Play**: physics,
  cloth/soft-body, scripts, animation (an edit-mode `olo_screenshot` shows none of
  it). Entering Play copies the authored scene and starts the runtime; **stopping
  restores the authored scene**, so the toggle is transient and fully reversible.
  Both are **idempotent** — a redundant call is a no-op reported as `changed:false`.
  Entering Play can fail if the scene has no primary `CameraComponent`; then
  `ok:false` and the editor stays in Edit (see the `message`).

All three are **consented WRITE tools** (issue #306 item C): refused while **Agent
writes** is *Disabled* in the MCP panel (the default), prompted per-action in
*Prompt* mode, applied directly in *Allow all*. They cross the read-only line
because `olo_scene_open` discards the current in-memory scene and `olo_scene_play`
executes the user's game scripts — but neither writes a project file (the scene
`.olo` on disk is never modified).

```jsonc
// 1) olo_scene_open { "path": "Scenes/Sandbox.olo" }
{ "available": true, "ok": true, "path": ".../Assets/Scenes/Sandbox.olo",
  "sceneName": "Sandbox", "entityCount": 42, "message": "Opened scene 'Sandbox' (42 entities)." }
// 2) olo_scene_play {}
{ "available": true, "ok": true, "playing": true, "changed": true,
  "sceneName": "Sandbox", "message": "Entered Play mode." }
// 3) olo_scene_summary {}          -> confirm "isPlaying": true
// 4) olo_screenshot { … }          -> see the simulating frame (cloth settling, physics, …)
// 5) olo_scene_stop {}
{ "available": true, "ok": true, "playing": false, "changed": true,
  "sceneName": "Sandbox", "message": "Stopped Play mode; restored the authored scene." }
```

The scene-control actions run on the editor's main thread (marshaled at a frame
boundary, like the other scene tools); a headless test host that owns no editor
scene reports a clean *"not available"*.

#### The auto-save recovery modal — headless dismissal (`OLO_EDITOR_AUTOSAVE_RECOVERY`)

When the editor opens a scene whose `.olo.auto` auto-save is newer than the saved
scene, it raises a **"Recover Auto-Save?"** modal. That modal is un-dismissible
remotely — synthetic Win32 clicks don't reach ImGui — so a headless / agent launch
with a stray `.auto` file would wedge at a popup it can never answer, blocking
`olo_scene_open` (and everything else) from ever reaching a usable state.

Set **`OLO_EDITOR_AUTOSAVE_RECOVERY`** before launching the editor to pre-answer it:

- `autosave` (aliases `recover` / `auto`) — load the newer `.auto` (the recovered work).
- `original` (aliases `keep` / `saved`) — load the saved scene, leaving the `.auto`.
- `discard` (aliases `delete`) — delete the `.auto`, then load the saved scene.

Unset / empty / unrecognized keeps the interactive modal, so this never changes a
human's editor. The `run-oloengine` skill's `attach` action sets it (typically
`original`) so an agent session always boots straight to a usable editor.
`olo_scene_open` itself never raises the modal — it loads the requested file
directly, since an agent explicitly asked for that scene.

### Render override A/B (`olo_render_toggle_pass` / `olo_render_set_debug_view`)

These are the **ephemeral render-override** tools — the rendering counterpart of the
shader inner loop. They let an agent A/B a rendering feature and inspect intermediate
visualizations without restarting the editor or touching the user's project.

**Crucially, the change is ephemeral.** Both tools mutate only the renderer's
*session-global* settings (`Renderer3D::GetPostProcessSettings()` /
`GetFogSettings()`), **not the loaded scene's own copy** — so a flip is visible on the
next rendered frame, is never written to disk, and a scene reload restores it. That
keeps the server read-only with respect to your project, the same boundary the
camera/viewport tools respect.

`olo_render_toggle_pass { name, enabled? }` flips one post-process / fog feature. The
core A/B loop:

```jsonc
// 1) olo_render_toggle_pass { "name": "bloom", "enabled": false }
{ "pass": "bloom", "enabled": false, "previous": true, "changed": true }
// 2) olo_screenshot { … }                       -> the "off" reference
// 3) olo_render_toggle_pass { "name": "bloom", "enabled": true }
{ "pass": "bloom", "enabled": true, "previous": false, "changed": true }
// 4) olo_screenshot { … }                       -> compare against the "off" frame
```

`name` is one of **bloom, ssao, gtao, ssr, ssgi, fxaa, taa, vignette,
chromaticaberration (alias ca), depthoffield (alias dof), motionblur, colorgrading,
autoexposure, fog, fogscattering, fogvolumetric, godrays** (case- and
separator-insensitive). Omit `enabled` to flip the current value (the quick A/B form);
pass it to set the state explicitly. Calling the tool **with no `name`** returns the
full list with each pass's live `enabled` state plus the `activeAOTechnique`.

Some toggles carry preconditions, surfaced as a `note` so an A/B that shows no change
isn't a mystery:

- **ssao / gtao** share one AO slot (`ActiveAOTechnique`); enabling either also selects
  that technique so it actually renders.
- **ssr / ssgi** render only in the **Deferred** rendering path; enabling one in
  Forward / Forward+ returns a `note` saying so.
- **fogscattering / fogvolumetric / godrays** need the master **fog** pass enabled
  first; the `note` reminds you.

`olo_render_set_debug_view { mode }` switches the viewport to a single raw intermediate
buffer for AO / reflection / GI debugging. `mode` is one of **none, ssao, gtao, ssr,
ssgi** (exactly one is shown at a time; `none`, or `enabled:false`, clears them all). It
reports the four `*DebugView` flag states and **`passEnabled`** — whether the pass that
produces the chosen buffer is actually running this frame — with an actionable `note`
when it is not:

```jsonc
// olo_render_set_debug_view { "mode": "ssao" }   (with SSAO not yet enabled)
{ "mode": "ssao", "ssaoDebugView": true, "gtaoDebugView": false,
  "ssrDebugView": false, "ssgiDebugView": false, "passEnabled": false,
  "note": "SSAO is not active; enable it with olo_render_toggle_pass { name: 'ssao' }." }
```

So the usual debug-view flow is two steps: `olo_render_toggle_pass { name: "ssao",
enabled: true }` then `olo_render_set_debug_view { mode: "ssao" }`. Calling the tool
with no `mode` lists the modes + the current state.

### Multi-valued renderer settings (`olo_renderer_settings_set`)

The **enum-valued** counterpart of `olo_render_toggle_pass`: where the toggle flips
an on/off pass, this selects one of several named values a boolean can't express, so
an agent can verify a rendering feature **live at each setting** over MCP. It exists
because the read-only server couldn't drive #480's FSR1 "Spatial Upscale" dropdown —
so multi-setting / multi-angle rendering verification of a new feature wasn't
possible from a session.

It is a **consented WRITE tool** (issue #306 item C): like the other writes it is
refused while **Agent writes** is *Disabled* in the MCP panel (the default), prompts
per-action in *Prompt* mode, and applies directly in *Allow all*. A
settings change crosses the read-only line — but it is **session-scoped and
restorable, never a project mutation**: the tool mutates only the renderer's
session-global `PostProcessSettings` / `RendererSettings` (the same structs
`olo_render_toggle_pass` edits), so the change is visible next frame, is never
written to disk, and a **scene reload restores it**.

The settings:

- **`upscale`** — FSR1 spatial-upscale mode: `off` | `quality` | `balanced` |
  `performance` | `ultraperformance` (the #480 motivating case).
- **`tonemap`** — tone-map operator: `none` | `reinhard` | `aces` | `uncharted2`.
- **`renderpath`** — rendering path: `forward` | `forwardplus` | `deferred`.
  Switching **rebuilds the render-graph topology**, and `deferred` is required for
  SSR / SSGI.
- **`depthprepass`** — the depth-prepass perf lever (#316): `off` | `on` | `auto`.
  `on`/`off` force the **live** Renderer3D toggle for this session; `auto` restores
  the settings-derived value (the response then carries `"requested": "auto"` plus
  the resolved `off`/`on` it landed on). Forward+/Deferred derive it **on** because
  their tile culling reads the prepass depth — forcing `off` there is a legitimate
  perf experiment but degrades tiled lighting until restored. Note a later settings
  apply (e.g. a `renderpath` switch) re-derives it.
- **`softshadows`** — directional-shadow filtering (#316): `pcf` | `pcss`. PCSS
  (contact-hardening penumbra) is **the** dominant ScenePass cost in shadowed scenes
  — the second live perf session measured it at ~93 % of a 46.6 ms ScenePass at
  1080p Sponza. This lever turns that A/B into one call instead of a shader-source
  edit + `olo_shader_reload`.

**Restore is restore-PRIOR-VALUE, not CommandHistory.** Unlike the entity field
writes (`olo_set_collision_layer` / `olo_entity_set_field`), which push an undoable
`ComponentChangeCommand` onto the editor's undo stack, these are **global renderer
settings, not scene/ECS data** — an undo-stack entry would be wrong. So the response
reports `previousValue` (and a convenience `restoreWith`) and you revert by calling
again with that token. The A/B loop:

```jsonc
// 1) olo_renderer_settings_set { "setting": "upscale", "value": "performance" }
{ "setting": "upscale", "previousValue": "off", "value": "performance",
  "changed": true, "restoreWith": "off" }
// 2) olo_screenshot { … }                          -> the upscaled frame
// 3) olo_renderer_settings_set { "setting": "upscale", "value": "off" }
{ "setting": "upscale", "previousValue": "performance", "value": "off", … }
// 4) olo_screenshot { … }                          -> the native-res reference
```

Calling the tool with **no arguments** lists every setting with its live
`currentValue` and the full allowed-value catalogue (still behind the write gate,
since the whole tool is a write tool).

### Sun / time-of-day override (`olo_scene_set_time_of_day` / `olo_scene_set_sun_angle`)

The **lighting** counterpart of the render-override A/B loop: move the procedural
sky's sun from the editor so an agent can iterate lighting for any rendering / water
/ GI / god-ray work, without restarting the editor or touching the user's project.

**Like the toggle/debug-view tools, the change is ephemeral.** Both tools edit only a
session-global Renderer3D sun-direction override that `Scene::LoadAndRenderSkybox`
bakes the **`ProceduralSkyComponent`** with — instead of the component's serialized
`m_SunDirection` — **without ever writing the component**. So a move is visible on the
next baked frame, is never saved, and the authored sun is restored on **scene reload,
play-stop, server-stop, or an explicit `clear`**. The sky is hash-gated, so a changed
(or cleared) sun triggers exactly one re-bake, not a per-frame one.

`olo_scene_set_time_of_day { hours }` maps a 24-hour clock time to the sun direction —
`0` = midnight, `6` = sunrise (sun on the east horizon, +X), `12` = noon (overhead),
`18` = sunset (west horizon, −X); before 06:00 / after 18:00 the sun is below the
horizon (night). The lighting inner loop:

```jsonc
// 1) olo_scene_set_time_of_day { "hours": 8 }
{ "active": true, "source": "timeOfDay", "sunDirection": [0.61, 0.71, -0.35],
  "elevationDegrees": 45.0, "azimuthDegrees": 120.0, "hours": 8 }
// 2) olo_screenshot { … }                       -> the morning reference
// 3) olo_scene_set_time_of_day { "hours": 17 }   -> low evening sun
// 4) olo_screenshot { … }                        -> compare
// 5) olo_scene_set_time_of_day { "clear": true } -> restore the authored sun
{ "active": false, "cleared": true, "source": "cleared" }
```

`olo_scene_set_sun_angle { yaw, pitch }` aims the sun directly: `yaw` is the azimuth in
degrees (measured from +Z toward +X: `0` = +Z, `90` = +X/east, `270` = −X/west) and
`pitch` is the elevation in degrees in `[-90, 90]` (`90` = straight up, `0` = horizon,
negative = below). **Both are required to set** — a half-specified direction is
rejected with guidance rather than guessed.

Both validate every numeric input with `std::isfinite`, report the resulting
`sunDirection` with its `elevationDegrees` / `azimuthDegrees`, and surface a **`note`**
when the override can't be seen — there is **no `ProceduralSkyComponent` in the active
scene** (nothing to bake), or the **sun is below the horizon** (the sky bakes dark).
Calling either tool with no arguments reports the current override state; `clear`:true
removes it.

### Physics introspection (the `olo_physics_*` family)

These expose Jolt's read-only query surface so an agent can debug collision/physics
problems without guessing. They follow the same "expose, don't embed" rule — no tool
moves a body, edits a layer, or steps the simulation.

`olo_physics_layer_matrix` reads static registry data and works in **Edit mode**; the
others read the live simulation, so they need **Play mode** (`olo_physics_list_colliders`
still lists authored bodies in Edit mode, just without the live fields). Every live read
is marshaled onto the editor's main thread at a frame boundary, like the `olo_scene_*`
tools.

The headline tool is **`olo_physics_why_no_collision`** — the "player falls through the
floor" debugger. Given two entity UUIDs it walks a fixed cascade (physics running → both
entities exist → each has a rigidbody + collider + live body → not both Static → layers
allowed to collide → neither is a trigger → bounds overlap) and reports the **root cause**,
not a downstream symptom. A worked example:

```jsonc
// olo_physics_why_no_collision { "a": "12758…", "b": "98321…" }
{
  "a": "12758…", "b": "98321…",
  "reasonCode": "both_static",
  "summary": "Both bodies are Static. Two static bodies never collide — at least one must be Dynamic …",
  "canCollide": false,
  "checks": [
    "[ok] A and B are distinct entities",
    "[ok] the 3D physics simulation is running",
    "[ok] both entities exist in the active scene",
    "[ok] A has a Rigidbody3DComponent",
    "[ok] A has a collider component",
    "[ok] A has a live physics body",
    "[ok] B has a Rigidbody3DComponent",
    "[ok] B has a collider component",
    "[ok] B has a live physics body",
    "[fail] both bodies are Static"
  ],
  "facts": {
    "a": { "entityExists": true, "hasRigidbody": true, "hasCollider": true, "hasLiveBody": true,
           "bodyType": "Static", "isTrigger": false, "layerId": 0, "layerName": "NON_MOVING" },
    "b": { "…": "…", "bodyType": "Static" },
    "layersCollide": false, "boundsOverlap": true
  }
}
```

Typical flow: `olo_physics_list_colliders` to find the two entities and their layers →
`olo_physics_why_no_collision` for the verdict → `olo_physics_layer_matrix` if it blames
the layer filter → `olo_physics_contacts` to confirm a contact actually fires once fixed.

### Rendering introspection (`olo_render_why_not_visible`)

The rendering counterpart of `olo_physics_why_no_collision` — the "why can't I see my
mesh?" debugger that promotes the `why-cant-i-see-my-object` prompt's manual multi-tool
flow into one call. Given one entity UUID it walks a fixed root-cause cascade (scene
loaded → entity exists → has a renderable component → geometry asset present → visibility
flag on → transform scale non-degenerate → material's shader compiled → in front of the
editor camera → inside the view frustum) and reports the **root cause**, not a downstream
symptom. It is read-only and works in **Edit mode** (it reads the live scene and the
editor camera, marshaled onto the main thread at a frame boundary).

**Honesty boundary:** the per-frame occlusion (HZB) cull result and the selected LOD
level are private renderer state with no editor-side query, so the tool does **not** claim
a verdict about them. When every observable check passes it returns `should_be_visible`
and points you at `olo_screenshot` / `olo_render_capture_target` (and `olo_camera_frame_entity`
to rule out the camera) for the causes it cannot observe. Camera-relative checks
(behind-camera / frustum) are only evaluated when the entity has resolvable world-space
bounds (3D meshes/models); 2D renderables and instanced batches skip them honestly. A
worked example:

```jsonc
// olo_render_why_not_visible { "entity": "44120…" }
{
  "entity": "44120…",
  "reasonCode": "degenerate_scale",
  "summary": "The entity's transform scale has a zero (or near-zero) component, which collapses its geometry to nothing …",
  "renderableConfigOk": false,
  "visible": false,
  "checks": [
    "[ok] an active scene is loaded",
    "[ok] the entity exists in the active scene",
    "[ok] the entity has a renderable component (MeshComponent)",
    "[ok] MeshComponent has geometry to draw",
    "[fail] the entity's transform scale has a zero (or near-zero) component"
  ],
  "facts": {
    "entityExists": true, "hasRenderable": true, "renderableKind": "MeshComponent",
    "geometryRequired": true, "geometryPresent": true, "scaleDegenerate": true,
    "hasMaterialShader": false, "boundsKnown": true, "behindCamera": false, "inFrustum": true
  },
  "sceneLoaded": true, "cameraKnown": true, "anyShaderHasErrors": false, "shaderErrorCount": 0
}
```

Typical flow: `olo_scene_list_entities` to find the entity → `olo_render_why_not_visible`
for the verdict → `olo_camera_frame_entity` + `olo_screenshot` when it returns
`behind_camera` / `outside_frustum` / `should_be_visible`, or `olo_shader_get` when it
blames a shader.

### The event timeline (`olo_events_tail`)

`olo_events_tail` is the "what just happened?" tool. The engine keeps a small
mutex-guarded ring buffer (`Debug/DiagnosticsEventLog.h`) that real seams push
structured records into: **scene load** (editor `OpenScene`), **play / stop**
(`Scene::OnRuntimeStart` / `OnRuntimeStop`), runtime **entity spawn / destroy**
(`Scene::CreateEntityWithUUID` / `DestroyEntity`), **asset reload** (the filewatch
`AssetReloadedEvent`), and **script error** (folded in from the same buffer behind
`olo_script_get_last_errors`, so the timeline is unified). Each record carries a
monotonic `id`, a `category`, a UTC `time`, a `message`, and — when applicable — the
`entity` UUID and a `context` string (scene name / asset path / script name).

The headline feature is **incremental polling**. Every response includes a `lastId`
cursor; pass it back as the next call's `sinceId` to get *only what happened since* —
the agent-loop win over re-dumping the whole buffer:

```jsonc
// 1) baseline — note lastId
// olo_events_tail { "count": 1 }            -> { "lastId": 312, "events": [ … ] }
// 2) … do something in the editor: hit Play, spawn an entity, Stop …
// 3) olo_events_tail { "sinceId": 312 }
{
  "count": 3, "lastId": 315,
  "events": [
    { "id": 313, "category": "play",           "time": "14:02:11.418", "message": "Entered Play mode", "context": "MyScene" },
    { "id": 314, "category": "entity_spawn",   "time": "14:02:13.902", "message": "Spawned entity 'Bullet'", "entity": "57…" },
    { "id": 315, "category": "stop",           "time": "14:02:15.110", "message": "Left Play mode", "context": "MyScene" }
  ]
}
```

Bulk churn is deliberately collapsed: a whole-scene copy on Play and entity
deserialization on load would otherwise flood the ring with hundreds of
`entity_spawn` records, so those paths are suppressed and represented by the single
`play` / `scene_load` event instead. Filter with `categories` (e.g.
`["script_error", "asset_reload"]`) to narrow further. The ring holds the most recent
512 events; older ones are evicted, but `sinceId` polling means an agent that checks
in regularly never misses anything between checks.

> **Companion (issue #306 item B, server-push half — done):** `olo_events_tail` is the
> *poll-based* read of the ring buffer; the same buffer is now also **pushed live** over a
> persistent SSE stream on `GET /mcp` (previously `405`). See **Live event push** below.
> `resources/subscribe` remains the one unimplemented sub-item.

### Live event push (server-initiated SSE stream)

`GET /mcp` opens a persistent **`text/event-stream`** (Server-Sent Events) — the
server-*initiated* counterpart of `olo_events_tail`. Instead of polling, an agent
holds the stream open and is **pushed** each new diagnostics event shortly after it is
recorded (within ~250 ms — see the keep-alive note below), so a session watching a
playtest sees script errors, scene loads, asset hot-reloads, and play/stop transitions
arrive on their own. It is the same ring buffer
(`Debug/DiagnosticsEventLog.h`) behind `olo_events_tail`, so the push and the poll
surface byte-identical event records.

- **Same gate as everything else.** The stream is behind the identical
  `127.0.0.1`-bind + `Origin` check + **bearer token** (+ `Mcp-Session-Id` validation
  when presented) as `POST /mcp`. It stays read-only: it only *reads* the event log.
- **Each event is an MCP `notifications/message`** — the spec's logging notification —
  carrying the structured event (the same `id` / `category` / `time` / `message` /
  `entity` / `context` fields `olo_events_tail` returns) under `params.data`, with a
  severity `level` (a script error is `error`, entity spawn/destroy are `debug`, the
  rest `info`) and `logger: "olo.events"`. Each SSE frame's `id:` line is the event's
  monotonic id. The server advertises the `logging` capability in `initialize`.
- **No backlog flood.** A fresh subscriber starts at the *current* head of the ring, so
  it receives only events recorded *after* it connects — not a 512-event dump. A
  reconnecting client that sends the standard SSE **`Last-Event-ID`** header resumes
  from that id while the requested history is still retained in the ring (it maps
  straight onto the ring's `sinceId` cursor). If more than 512 events arrive while the
  client is away, the oldest may already have been evicted, so a long gap can still drop
  the events in between.
- **Keep-alive.** When idle the stream emits an SSE comment (`: keep-alive`) every ~15 s,
  which also detects a vanished client. New events carry a worst-case latency of ~250 ms
  (the stream's internal poll cadence). The MCP panel shows how many push streams are
  connected.

```jsonc
// Wire format (one frame per event), pushed over GET /mcp:
// id: 313
// data: {"jsonrpc":"2.0","method":"notifications/message","params":{
//          "level":"info","logger":"olo.events",
//          "data":{"id":313,"category":"play","time":"14:02:11.418",
//                  "message":"Entered Play mode","context":"MyScene"}}}
```

Attach with a streaming-capable client (the same `claude mcp add` line works — Claude
Code opens the GET stream automatically alongside the POST channel). The threading is
the same lock-safe path as `olo_events_tail`: the stream runs on an httplib worker
thread and only touches the mutex-guarded event log, so it never marshals to or blocks
the editor's main thread.

### Resources

- `olo://scene/current` — the whole active scene serialized to YAML.
- `olo://logs/recent` — the recent engine log lines.

### Prompts (canned workflows)

- `diagnose-performance` — why is the scene slow, and what to fix.
- `explain-last-script-error` — explain the most recent script exception and fix it.
- `why-cant-i-see-my-object` — figure out why an entity isn't visible.

## Threading note

Diagnostics that are already mutex-guarded (logs, memory tracker) are read directly. Reads of
the EnTT registry / scene / renderer profiler are **marshaled onto the editor's main thread at
a frame boundary** (the registry is not thread-safe), so every scene/perf read returns a
consistent frame snapshot. If the editor is unresponsive, such a tool times out (~5 s) rather
than blocking it.
