# MCP diagnostics server (read-only)

OloEditor can host a **localhost-only, read-only [MCP](https://modelcontextprotocol.io)
server** so you can point your own LLM agent (Claude Code, Claude Desktop, …) at the
*running editor* and get grounded help debugging your game — using the diagnostics the
engine already collects (logs, scene/ECS state, scripting errors and API, performance,
memory, shaders, assets, crash reports, and a live screenshot) — plus a Tier-0
rendering-dev harness (camera control, viewport sizing, intermediate render-target
capture; issue #316).

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
