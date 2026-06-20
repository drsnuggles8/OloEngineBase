# MCP diagnostics server (read-only)

OloEditor can host a **localhost-only, read-only [MCP](https://modelcontextprotocol.io)
server** so you can point your own LLM agent (Claude Code, Claude Desktop, …) at the
*running editor* and get grounded help debugging your game — using the diagnostics the
engine already collects (logs, scene/ECS state, scripting errors and API, performance,
memory, shaders, assets, crash reports, and a live screenshot) — plus a Tier-0
rendering-dev harness (camera control, viewport sizing, intermediate render-target
capture, and golden-image comparison; issue #316).

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
  editor camera pose and the viewport capture size — which is never persisted.
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
| `olo_scene_list_entities` | paginated entity list (id, name, parent, child count) + name filter |
| `olo_scene_get_entity` | one entity's full component data (YAML) by UUID |
| `olo_perf_snapshot` | fps, frame/CPU/GPU time, draw calls, instancing, triangles |
| `olo_perf_bottlenecks` | CPU/GPU/Memory/IO bottleneck + confidence + recommendations |
| `olo_perf_frame_history` | downsampled recent-frame time series |
| `olo_perf_capture_frame` | triggers a real frame capture: stats + top-K draw commands by GPU time |
| `olo_memory_report` | GPU/CPU memory total + per-type breakdown + suspected leaks |
| `olo_shader_list` | inventory of all registered shaders (id, name, hasErrors) |
| `olo_shader_errors` | shaders with compile/link errors |
| `olo_shader_get` | one shader's uniforms/buffers/samplers/instructions (+ optional GLSL) |
| `olo_shader_reload` | reload + recompile one shader from disk by name; returns post-reload status + the compile/link log (the shader inner loop) |
| `olo_assets_list` | paginated registered assets (handle, type, path) + type filter |
| `olo_assets_problems` | assets that failed to load or are missing/invalid |
| `olo_script_get_api` | C# / Lua scripting API digest (types + members), with a type filter |
| `olo_script_get_last_errors` | recent C# (Mono) / Lua (Sol2) script exceptions |
| `olo_crash_list` / `olo_crash_get` | crash reports under `CrashReports/` |
| `olo_screenshot` | the viewport rendered to a PNG image block; optional one-shot camera pose (`camera`/`orbit` + `settleFrames`) with automatic save/restore of the user's camera |
| `olo_camera_get` | the editor camera's pose (position, focal point, yaw/pitch, FOV, clips, viewport size) |
| `olo_camera_set_pose` | move the editor camera: `position` + (`target` \| `yaw`/`pitch`), optional `fov` |
| `olo_camera_orbit` | orbit-frame the camera around a world point: `target`, `yaw`, `pitch`, `distance` |
| `olo_camera_frame_entity` | point the camera at an entity (by UUID) and fit it in view |
| `olo_viewport_set_size` | override the viewport's logical render size for deterministic captures (`reset` to clear) |
| `olo_render_list_targets` | the render graph's live texture/framebuffer resources (name, kind, format, size, producers) |
| `olo_render_capture_target` | read back one intermediate render target (depth, normals, G-buffer, shadow map, AO, post-process stages, …) as a PNG image block; depth is min-max normalised by default |
| `olo_render_compare_golden` | capture the viewport (optional `camera`/`orbit` pose) and diff it against a golden PNG (`goldenPath`): returns a numeric `similarity`/`rmse`/`ssim` + `pass` verdict; missing golden or `rebase`:true writes the capture as the new baseline (the `OLOENGINE_GOLDEN_REBASE` workflow) |
| `olo_render_why_not_visible` | explain why one entity (`entity`) is NOT on screen — the "why can't I see my mesh?" debugger: root-cause `reasonCode`, summary, ordered checks, and the raw render facts |
| `olo_physics_layer_matrix` | the collision-layer matrix the sim uses: built-in object layers + user-defined layers, with pairwise collide/no-collide (works in Edit mode) |
| `olo_physics_list_colliders` | paginated entities with a rigidbody: authored body type / layer / trigger / collider shapes, plus live object layer, position, awake/asleep when playing |
| `olo_physics_contacts` | entity pairs whose bodies are touching right now (live active-contact set, deduplicated); requires Play mode |
| `olo_physics_raycast` | cast a ray (`origin` + `direction`\|`to`) through the live physics world: closest hit, or up to `maxHits` ordered hits (entity, position, normal, distance) |
| `olo_physics_overlap` | bodies overlapping a sphere (`radius`) or box (`halfExtents`) at `origin`; requires Play mode |
| `olo_physics_why_no_collision` | explain why two entities (`a`, `b`) are NOT colliding — the "player falls through the floor" debugger: root-cause `reasonCode`, summary, ordered checks, and per-entity facts |

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

> **Follow-up (still open under issue #306 item B):** this is the *poll-based* half.
> Server-*initiated* push (MCP notifications / `resources/subscribe` / SSE — `GET /mcp`
> currently returns 405) is a larger transport change and remains open.

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
