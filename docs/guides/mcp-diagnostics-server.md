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
  **read-only with respect to your project by default** — no tool writes scenes, assets,
  or files unless you explicitly enable the consent-gated write tier (below).
  The Tier-0 inspection tools (issue #316) may adjust *editor-only viewport state* — the
  editor camera pose and the viewport capture size — which is never persisted. The
  render-override tools (`olo_render_toggle_pass` / `olo_render_set_debug_view`) likewise
  edit only the renderer's *session-global* post-process / fog settings, never the loaded
  scene's own copy, so the change is ephemeral (a scene reload restores it) and never
  written to disk. The **(consented write)** tools behind the Agent-writes gate go one
  step further than those ephemeral overrides: `olo_entity_set_field`,
  `olo_scene_set_time_of_day`, `olo_scene_set_sun_angle`, and `olo_scene_set_weather`
  mutate **serialized components of the loaded scene in memory** — undoable in the
  editor, discarded on reload, and reaching disk only if you save the scene yourself;
  no tool ever writes a file. The gate is off by default and never persisted (see
  [Write consent](#write-consent--disabled--prompt--allow-all-issue-306-item-c)).
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

Honesty boundary: the headless host wires the read-only/inspection hooks, plus the two
Tier-0 camera/viewport hooks the Part-1 follow-on closed out — `olo_camera_frame_entity`
(shares `OloEngine::FrameCameraOnEntity`, the same bounds-computation + fit logic
`EditorLayer::FrameEditorCameraOnEntity` uses, pointed at the fixture's `Scene`/`EditorCamera`)
and `olo_viewport_set_size` (resizes the fixture's `Scene` viewport + the Renderer3D render
graph via `RendererAttachedTest::ResizeRenderTarget`, wired through `Hooks::SetViewportSize`).
The consented project-write tools (`GetCommandHistory` and friends) remain null by design —
no project writes in the headless host; that's Tier 2 / #306-C, out of scope. The screenshot,
shader, scene, perf, physics, and render-capture/override tools all work.

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
| `olo_scene_open` | **(consented write)** open / switch the active scene by `path` (a `.olo`/`.scene` file, relative paths resolve against the project asset directory) — the scriptable scene switch. Loads directly, bypassing the auto-save recovery modal a remote agent can't click; stops Play mode first; **cancels any pending auto-save recovery** (an armed recovery modal used to be able to swap the freshly opened scene back out when its button was clicked later, issue #607). Reports the loaded scene name + entity count and settles rendered frames before returning. Gated behind **Agent writes** |
| `olo_scene_play` / `olo_scene_stop` | **(consented write)** enter / leave Play mode — the same as the editor's Play/Stop buttons, so an agent can verify anything that only runs in Play (physics, cloth, scripts). Transient + fully reversible (stop restores the authored scene); idempotent (`changed:false` when already in that state); **settles rendered frames after a real transition** so an immediately following `olo_screenshot` shows the new state, not the last pre-transition frame (the uniform-grey trap, issue #607); `olo_scene_summary` reports `isPlaying` to confirm. Gated behind **Agent writes** |
| `olo_editor_select_entity` | **(consented write)** select (or `clear`) the entity in the editor's Scene Hierarchy / Properties panels — the only way to drive the Properties inspector onto a given entity over MCP, unblocking screenshot verification of its rendered component UI. `olo_input_inject` cannot reliably land a Scene Hierarchy row click (the OS cursor reasserts over the synthetic position between injected frames). An unknown `entity` UUID leaves the current selection untouched (`ok:false`), never silently clearing it. Not undoable (selection isn't project data). Gated behind **Agent writes** |
| `olo_scene_list_entities` | paginated entity list (id, name, parent, child count) + name filter |
| `olo_scene_get_entity` | one entity's full component data (YAML) by UUID |
| `olo_entity_list_fields` | the writable (component, field) pairs of one entity with each field's type, current value, and — for a range-validated field — its `min`/`max`. The read-only discovery half of `olo_entity_set_field`; optional `component` filter. See [Component field writes](#component-field-writes-olo_entity_set_field) |
| `olo_entity_set_field` | **(consented write)** set one component field by (`component`, `field`, `value`) — undoable (a single Ctrl-Z), UUID-keyed. The registry is **generated from every component definition** (issue #607), so it spans the whole ECS surface (meshes/materials/VirtualMesh, lights, fog/probes, physics bodies + colliders, text/UI, nav, water, terrain, …), not a curated handful. Out-of-range values are **clamped** to the serializer's own range (`clamped:true` + `requestedValue`); the result echoes `value` **read back from the component** plus `changed:true/false`. Gated behind **Agent writes**. See [Component field writes](#component-field-writes-olo_entity_set_field) |
| `olo_perf_snapshot` | fps, frame/CPU/GPU time (real whole-frame GPU timer), `gpuWaitMs` (CPU blocked on the GPU fence — the direct GPU-bound signal), draw calls, instancing, triangles, plus `renderWidth`/`renderHeight` — the ACTUAL SceneColor render resolution; cross-check it against any `olo_viewport_set_size` override before trusting timings |
| `olo_perf_bottlenecks` | CPU/GPU/Memory/IO bottleneck + confidence + recommendations (uses real cpu/gpu/gpuWait numbers) |
| `olo_perf_frame_history` | downsampled recent-frame time series |
| `olo_perf_capture_frame` | triggers a real frame capture: stats + top-K draw commands by GPU time (per-draw times resolve via a deferred commit one-plus frames after the capture; draws carry their submesh debug names) |
| `olo_perf_pass_timings` | whole-frame GPU time split by render-graph pass (Shadow vs Scene vs GTAO vs Bloom vs ToneMap…): per-pass GPU (always-on timestamp queries) + CPU dispatch ms, frame totals incl. `gpuWaitMs`, and `unattributedGpuMs`. `ScenePass` carries `subPasses` splitting its GPU time into `DepthPrepass` vs `Color` (no DepthPrepass entry = prepass off; sub times are inside the parent's `gpuMs`, not additional) |
| `olo_perf_cpu_scopes` | per-scope CPU time from `PerformanceProfiler` (every system in `Scene.cpp` wrapped in `OLO_PERF_SCOPE`/`OLO_PERF_SCOPE_AUTO`), sorted descending by time — mirrors the editor's PerformanceLayer CPU Scopes table. `OLO_PERF_SCOPE` is compiled out entirely in Distribution builds, so `status` reports `"unavailable"` there instead of a misleadingly empty list (`"ok_no_data"` = build supports it but nothing ran last frame; `"ok"` = real data). Optional `limit` truncates the returned list; `totalTimeMs`/`scopeCount` always reflect the full set |
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
| `olo_screenshot` | the viewport rendered to a PNG image block; optional one-shot camera pose (`camera`/`orbit` + `settleFrames`) with automatic save/restore of the user's camera. In **Play mode** the frame comes from the runtime's primary `CameraComponent`, so poses are refused there (they could only move the unused editor camera) and the reply's `sceneState`/`camera` meta says which camera produced the frame |
| `olo_camera_get` | the editor camera's pose (position, focal point, yaw/pitch, FOV, clips, viewport size) |
| `olo_camera_set_pose` | move the editor camera: `position` + (`target` \| `yaw`/`pitch`), optional `fov` |
| `olo_camera_orbit` | orbit-frame the camera around a world point: `target`, `yaw`, `pitch`, `distance` |
| `olo_camera_frame_entity` | point the camera at an entity (by UUID) and fit it in view |
| `olo_viewport_set_size` | override the viewport's logical render size for deterministic captures (`reset` to clear). The override wins over window/panel resizes (the editor reasserts it after any OS window-resize event); verify with `olo_perf_snapshot`'s `renderWidth`/`renderHeight` before perf measurements |
| `olo_render_list_targets` | the render graph's live texture/framebuffer resources (name, kind, format, size, producers) |
| `olo_render_capture_target` | read back one intermediate render target (depth, normals, G-buffer, shadow map, AO, the DDGI atlases, the froxel-fog volumes, post-process stages, …) as a PNG image block; depth is min-max normalised by default. `layer` picks one slice of an **array / cube / 3D** target (CSM cascade 0–3, cubemap faces, froxel z-slices); out-of-range is an error, never a silent layer-0 capture. `afterPass` captures the resource **as of that pass's execution** (mid-frame snapshot) instead of end-of-frame — see [Mid-frame state & exact texels](#mid-frame-state--exact-texels-afterpass-texel-space-stats-validate) |
| `olo_render_probe_pixel` | the exact NUMBERS under one pixel: every decoded G-Buffer channel (albedo, metallic, decoded world normal, roughness, AO, emissive, velocity, integer entityID, raw + linearized depth) plus the final presented colour — or, with `target`, the raw channels of ONE named resource. Every reply echoes `mappedCoord` (the exact texel read); `space`:"texel" + `mip` address an exact texel of a padded resource (the HZB pyramid), `layer` picks an array slice, `afterPass` probes mid-frame state |
| `olo_render_target_stats` | exact float min/max/mean + a **bit-exact unique-value histogram** over a `rect` of one target at a `mip` — the 1-ULP instrument an 8-bit PNG cannot be (1.0 and 0.99999994 both encode as 255). Per channel: finite/NaN/Inf counts, distinct-bit-pattern count, most frequent values with exact counts. Supports `layer` and `afterPass` |
| `olo_render_validate` | on-demand render-graph frame validation: the compiled resource-hazard sweep, barrier/build diagnostics, execute-path resolve failures, consumed-but-unbacked resources, and versioned-name physical-id groups; optional `compare` checks two targets **bit-exactly** (channel 0), e.g. `compare:{a:"SceneDepth", b:"HZB", afterPass:"GTAOPass"}` — both sides snapshotted in the SAME frame |
| `olo_froxel_fog_probe` | sample the volumetric-fog **froxel volume** at one cell (`froxel`:[x,y,z] or `worldPos`:[x,y,z]) — returns the RAW scatter (in-scatter + extinction) **and** the INTEGRATED values (accumulated in-scatter + transmittance) plus the cell's world bounds, so "scatter pass wrong" and "composite tap wrong" separate without a PNG round trip |
| `olo_render_compare_golden` | capture the viewport (optional `camera`/`orbit` pose) and diff it against a golden PNG (`goldenPath`): returns a numeric `similarity`/`rmse`/`ssim` + `pass` verdict; missing golden or `rebase`:true writes the capture as the new baseline (the `OLOENGINE_GOLDEN_REBASE` workflow) |
| `olo_render_toggle_pass` | flip a post-process / fog feature on/off (`name` + optional `enabled`) — the ephemeral A/B loop: toggle off → `olo_screenshot` → toggle on → `olo_screenshot`. No `name` lists every pass + its live state |
| `olo_render_set_debug_view` | switch the viewport to a raw AO/SSR/SSGI buffer, the overdraw heatmap, or a virtualized-geometry visualization (`mode`: none/ssao/gtao/ssr/ssgi/overdraw/**vgclusterid/vglod/vgoverdraw**); reports whether the backing pass is actually running, and (for the vg\* modes) the `captureTarget` to read back. No `mode` lists the modes + current state |
| `olo_renderer_settings_set` | **(consented write)** set a multi-valued, session-global renderer / post-process setting — `upscale` (FSR1 spatial-upscale mode), `tonemap` (operator), `renderpath` (forward/forward+/deferred), `depthprepass` (off/on/auto — the #316 perf lever), `softshadows` (pcf/pcss — THE ScenePass shadow-cost lever) — to verify a rendering feature live at each value. The enum-valued sibling of `olo_render_toggle_pass`; reports `previousValue` for restore-prior-value (no undo stack). No args lists every setting + current value + allowed values. Gated behind **Agent writes** (Disabled/Prompt/Allow all) |
| `olo_scene_set_time_of_day` | **(consented write)** set the scene's time-of-day clock — writes the **serialized `TimeOfDayComponent`** (the single authoritative sun source since issue #633; the old ephemeral override is retired): `hours` [0,24) and/or `dayOfYear`, `latitudeDegrees`, `timeScale`, `paused`, `enabled`. TimeOfDaySystem drives the sun/sky from it next frame, edit and play alike; returns the component state + derived sun elevation / isNight / sun+moon directions. In-memory edit (persisted on scene save); errors with guidance when the scene has no `TimeOfDayComponent`. `clear`:true is a legacy no-op (note only). Gated behind **Agent writes** |
| `olo_scene_set_sun_angle` | **(consented write)** aim the sun from a `yaw` (azimuth) / `pitch` (elevation) pair — SOLVES for the time of day whose ephemeris sun best matches and writes the solved hours into the `TimeOfDayComponent`. Pitch is matched exactly when the day/latitude can reach it (else clamped, reported via `clamped` + note); yaw is honoured for its east/west side only (east = morning, west = afternoon). Returns the component state + `achievedElevationDeg`/`clamped`. Gated behind **Agent writes** |
| `olo_scene_set_weather` | **(consented write)** drive the weather director — writes the `WeatherStateComponent`'s target `state` (Clear \| Overcast \| Rain \| Storm \| Snow \| FogBank, case-sensitive) with optional `transitionSeconds` (0–600) and `immediate`:true snap; applies the blend to the scene + renderer immediately (edit-mode preview). Returns currentState/targetState/transitionDuration/transitionProgress/wetness; errors with guidance when the scene has no `WeatherStateComponent`. Gated behind **Agent writes** |
| `olo_scene_get_atmosphere` | read the scene's atmosphere in one call — `timeOfDay` block (hours, dayOfYear, latitude, paused, derived sun elevation / isNight / sun+moon directions), `weather` block (current/target state, transitionProgress, wetness, blended cloud coverage), `cloudscape` block (enabled, coverage, layer bottom/top, castCloudShadows). Blocks for absent components are omitted; the note lists which components were found |
| `olo_render_why_not_visible` | explain why one entity (`entity`) is NOT on screen — the "why can't I see my mesh?" debugger: root-cause `reasonCode`, summary, ordered checks, and the raw render facts |
| `olo_physics_layer_matrix` | the collision-layer matrix the sim uses: built-in object layers + user-defined layers, with pairwise collide/no-collide (works in Edit mode) |
| `olo_physics_list_colliders` | paginated entities with a rigidbody: authored body type / layer / trigger / collider shapes, plus live object layer, position, awake/asleep when playing |
| `olo_physics_contacts` | entity pairs whose bodies are touching right now (live active-contact set, deduplicated); requires Play mode |
| `olo_physics_raycast` | cast a ray (`origin` + `direction`\|`to`) through the live physics world: closest hit, or up to `maxHits` ordered hits (entity, position, normal, distance) |
| `olo_physics_overlap` | bodies overlapping a sphere (`radius`) or box (`halfExtents`) at `origin`; requires Play mode |
| `olo_physics_why_no_collision` | explain why two entities (`a`, `b`) are NOT colliding — the "player falls through the floor" debugger: root-cause `reasonCode`, summary, ordered checks, and per-entity facts |
| `olo_input_inject` | **(consented write)** inject synthetic mouse/keyboard input — `click` / `move` / `drag` / `key` / `text` — into the editor's own input stream, so you can verify that an interactive handler actually FIRES (a viewport click selects the right entity; a panel button does what it claims), not merely that the editor renders. Synchronous: returns once the injected frames have been rendered, with the resulting selected/hovered entity in `after`. Gated behind **Agent writes**. See [Interactive UI verification](#interactive-ui-verification-olo_input_inject) |

### Write consent — Disabled / Prompt / Allow all (issue #306 item C)

Every tool marked **(consented write)** above (`olo_set_collision_layer`,
`olo_entity_set_field`, `olo_reload_script`, `olo_renderer_settings_set`,
`olo_scene_open`, `olo_scene_play`, `olo_scene_stop`, `olo_editor_select_entity`,
`olo_input_inject` — not every one of these mutates the *project*; some, like
`olo_editor_select_entity`, only mutate editor-only UI state, but all cross the
read-only line the same way and are gated identically)
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
  when the MCP panel window is closed, so a write is never left silently blocked. A
  `notifications/cancelled` for the call's request-id (issue #610) aborts the wait
  promptly — the parked call returns a cancelled response, runs **no** write, and drops
  its pending prompt — so an agent that gives up on a write it left waiting on the modal
  isn't stuck at the human's decision or the timeout.
- **Allow all** — writes auto-apply for the session with no prompt (the legacy
  "Allow writes" behaviour). Use it when you're actively driving a batch of edits and
  don't want to click through each one.

Every entity/component write still routes through the editor's **undo stack** — an
approved change is a single **Ctrl-Z** — so *Prompt* and *Allow all* differ only in
whether you confirm each action up front, not in reversibility. (The renderer-settings
writes are session-global and restore-prior-value instead; see that section. The
sun/time-of-day and weather tools edit serialized components since issue #633, so
they behave like any other component write.)

Threading: the write handler runs on a cpp-httplib worker thread and blocks there
while the main (UI) thread renders the modal and records your decision — the same
main-thread-marshal discipline the read tools use, so the editor's render loop never
blocks on an agent.

### Component field writes (`olo_entity_set_field`)

`olo_entity_set_field` mutates one component field on one entity, through the
editor's undo stack (`ComponentChangeCommand<T>`, UUID-keyed — a single Ctrl-Z).
`olo_entity_list_fields` is its read-only discovery half.

**The registry is generated, not curated (issue #607).** It used to be a
hand-written list of nine components, so most of the engine — `VirtualMeshComponent`,
`MeshComponent`, the physics bodies, `TextComponent`, the fog/probe volumes — was
simply unwritable, and a live debugging session had to fall back to hand-editing
scene YAML and reloading. OloHeaderTool now emits the registry
(`OloEditor/src/MCP/Generated/McpFieldRegistry.Generated.inl`) from the **same
component data-member scan that drives the scene serializer**, so a new component is
MCP-writable the moment it compiles — no touch-point to forget.

What is writable:

- **Every public data member** of every `struct *Component` whose type has a scalar
  JSON shape: `bool`, `int`/`uint`/small ints, `float`, `glm::vec2|3|4`, an `enum`
  (as its integer value), `std::string`, and `AssetHandle` (a decimal-digit **string**
  — a u64 exceeds JSON's safe-integer range).
- **Sub-object addressing**: a component whose authored surface lives inside a public
  nested struct/class member is still reachable — the field name is a dotted
  member-access chain (`System.Emitter.RateOverTime` for `ParticleSystemComponent`,
  whose entire authored surface lives inside its `System` member). The descent stops
  at a container (`std::vector`/`set`/`map`) — there is no static field name for an
  element — and respects `OLO_SERIALIZE(Skip)` on a nested runtime field the same way
  a top-level one is respected.
- **Setter-expression-based fields** (issue #607's `AudioSourceComponent` slice): a
  field behind a **private** member — reached only through an `OLO_PROPERTY`
  `Get`/`Set` expression pair, no public member/nested-member chain exists at all —
  is reachable too, via a small OloHeaderTool allowlist that reuses the SAME
  `OLO_PROPERTY` expressions Lua/C# scripting already compiles
  (`AudioSourceComponent`'s 16 parameters, all behind `private
  std::unique_ptr<AudioSourceColdData> m_Cold`). Unlike every other field above, a
  write here calls the `Set` expression **directly on the live component** instead of
  copying the whole component and swapping it in — `AudioSourceComponent::operator=`
  cannot be trusted to preserve `ActiveEventID` or to push the new value into the
  live `Ref<AudioSource> Source`, so the ordinary copy+swap path would silently
  detach a playing sound. See `McpGenericFieldWrite.h`'s `MakeSetterField` doc
  comment and OloHeaderTool's `EmitMcpSetterFields` for why this stays a narrow
  allowlist rather than "every `OLO_PROPERTY` component" (most of those already have
  their field reachable as a plain public member, so routing them through this path
  too would just double-register the same field).
- **Map-key addressing** (issue #607's `MorphTargetComponent::Weights` slice): a
  MAP-typed field (`std::unordered_map<std::string, f32> Weights` — target/bone name
  -> weight) has no compile-time-known key, so no static registry entry could ever
  name one ahead of time. ONE `MakeMapKeyField` entry is registered per map field
  (hand-written in `McpFieldRegistry.cpp`, not codegen'd — there is exactly one such
  field in the engine today), addressed by a **dotted runtime key**:
  `{ "field": "Weights.Smile", "value": 0.75 }` writes the `Smile` target's weight.
  The bare prefix (`"Weights"` with no key) is deliberately **not** writable — it
  names the container, not a value — so `olo_entity_list_fields` expands the field
  into one dotted entry **per current key** instead of listing `Weights` itself.
  The write goes through `MorphTargetComponent::SetWeight`/`GetWeight` directly on
  the live component (never a whole-map copy+swap), and the registered range mirrors
  `SetWeight`'s own `[0, 1]` clamp.
- **Not** writable, by design: per-tick runtime state (the `*StateComponent` family,
  `AnimationStateComponent`, `UIResolvedRectComponent`, `WorldTransformComponent`),
  entity identity (`IDComponent` — its UUID is the addressing key), any field marked
  `OLO_SERIALIZE(Skip)` (e.g. `NavAgentComponent`'s pathfinder state), any non-public
  member with no `OLO_PROPERTY` Get/Set pair (`TransformComponent::Rotation` — a
  derived euler/quat pair behind setters, but no OLO_PROPERTY annotation), and any
  field with no scalar JSON shape (a `Ref<T>`, a container other than a
  string-keyed scalar map, a `glm::quat`/`mat4`/`ivec*`).

**Ranges are enforced, and a clamp is reported.** A field whose scene-load path
clamps or rejects out-of-range values carries the same bounds here — from its
`OLO_SERIALIZE(Clamp, Min=…, Max=…)` annotation, or (for a component whose serializer
is hand-written) from the generator's `kMcpFieldClamps` table. So MCP can never put a
component into a state a scene load could not produce. Bounds are visible up front in
`olo_entity_list_fields` (`min`/`max` per field). `LightProbeVolumeComponent::Spacing`
and `ReflectionProbeComponent::InfluenceRadius` — previously a hand-written strict
`> 0` guard approximated by nothing here — are now `OLO_SERIALIZE(Clamp, Min=0.01f)`
annotated, so MCP inherits their range the same way any other annotated field does
(no `kMcpFieldClamps` entry needed); both components still keep their hand-written
`SceneSerializer.cpp` block for unrelated reasons (cross-field invariants a
single-field `Clamp` can't express), so the two components did not flip to
fully-generated serialization, only the two named fields' range semantics changed
from reject-and-reset-to-default to clamp-to-floor.

**The response is verifiable — do not trust "the call returned".** `value` is read
back **out of the live component after the write**, not echoed from the input, and
`changed` says whether anything actually moved:

```jsonc
// olo_entity_set_field { "entity": "12345…", "component": "VirtualMeshComponent",
//                        "field": "ErrorThresholdPixels", "value": 1000 }
{ "entity": "12345…", "component": "VirtualMeshComponent", "field": "ErrorThresholdPixels",
  "type": "float", "previousValue": 1.0,
  "value": 64.0,             // ← READ BACK from the component; the clamped result
  "requestedValue": 1000.0,  // ← what you asked for
  "clamped": true,           // ← it was out of range
  "changed": true, "undoable": true }

// A no-op write says so instead of looking like a success:
{ …, "value": 2.0, "changed": false, "undoable": false, "clamped": false }
```

An unknown component or field is a **tool error** that lists the valid alternatives
(`Editable components: …` / `Editable fields: …`), so a typo self-corrects in one
round-trip rather than looking like a write that quietly did nothing.

### Toolsets & on-demand tool discovery (`tools/search`)

The tool surface is large enough (~64 built-in tools; the full `tools/list` measures
~60 KB ≈ 15k tokens) that paging the whole flat list to find the right one is
wasteful. Every tool is tagged with a **toolset** (grouping category), and a custom
`tools/search` JSON-RPC method lets an agent discover tools by keyword and/or
category instead of pulling the entire list (project Lua script tools additionally
appear under the `script` toolset — see "Script-defined tools" below):

| Toolset | Tools |
|---|---|
| `diagnostics` | `olo_log_tail`, `olo_events_tail`, `olo_crash_list`, `olo_crash_get` |
| `scene` | `olo_scene_summary`, `olo_scene_list_entities`, `olo_scene_get_entity`, `olo_entity_list_fields`, `olo_entity_set_field`, `olo_scene_open`, `olo_scene_play`, `olo_scene_stop`, `olo_editor_select_entity` |
| `perf` | `olo_memory_report`, `olo_perf_snapshot`, `olo_perf_bottlenecks`, `olo_perf_frame_history`, `olo_perf_capture_frame`, `olo_perf_pass_timings`, `olo_perf_cpu_scopes` |
| `render` | `olo_render_frame_breakdown`, `olo_render_list_targets`, `olo_render_graph_topology_export`, `olo_render_capture_target`, `olo_render_probe_pixel`, `olo_render_target_stats`, `olo_render_validate`, `olo_render_toggle_pass`, `olo_render_set_debug_view`, `olo_renderer_settings_set`, `olo_scene_set_time_of_day`, `olo_scene_set_sun_angle`, `olo_scene_set_weather`, `olo_scene_get_atmosphere`, `olo_render_compare_golden`, `olo_render_why_not_visible`, `olo_froxel_fog_probe`, `olo_cluster_grid_stats`, `olo_shadow_atlas_layout`, `olo_virtual_geometry_set`, `olo_virtual_geometry_stats`, `olo_material_get` |
| `shader` | `olo_shader_list`, `olo_shader_errors`, `olo_shader_get`, `olo_shader_reload` |
| `assets` | `olo_assets_list`, `olo_assets_problems` |
| `scripting` | `olo_script_get_api`, `olo_script_get_last_errors`, `olo_reload_script` |
| `camera` | `olo_screenshot`, `olo_camera_get`, `olo_camera_set_pose`, `olo_camera_orbit`, `olo_camera_frame_entity`, `olo_viewport_set_size` |
| `physics` | `olo_physics_layer_matrix`, `olo_physics_list_colliders`, `olo_physics_contacts`, `olo_physics_raycast`, `olo_physics_overlap`, `olo_physics_why_no_collision`, `olo_set_collision_layer` |
| `input` | `olo_input_inject` |

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

(`olo_input_inject`, below, *can* now click that modal's buttons — it reaches ImGui
where Win32 injection cannot. But the env var stays the right answer for the
recovery modal specifically: it removes the wedge at launch, before any tool call is
possible, and needs no write consent.)

### Driving the Properties inspector (`olo_editor_select_entity`)

There was previously **no way to drive editor selection programmatically**: an
agent could read every component of an entity via `olo_scene_get_entity`, but
could not make the editor's Properties panel actually *render* that entity's
component UI (`SceneHierarchyPanel::DrawComponent<T>`) for a screenshot check.
`olo_input_inject` can click a viewport entity, but not reliably a Scene
Hierarchy *panel row* — the OS cursor reasserts over the synthetic position
between injected frames, so panel-space clicks never land a selection.
`olo_editor_select_entity` closes that gap with a direct write.

```jsonc
// olo_editor_select_entity { "entity": "12652600558176869447" }
{ "available": true, "ok": true, "changed": true, "selected": true,
  "entity": "12652600558176869447", "name": "Cube",
  "message": "Selected 'Cube'." }

// olo_screenshot { "space": "window" } (or the run-oloengine driver's full-window
// shot) now shows the Properties panel drawing Cube's components.

// olo_editor_select_entity { "clear": true }
{ "available": true, "ok": true, "changed": true, "selected": false,
  "message": "Cleared the Scene Hierarchy selection." }
```

- Exactly one of **`entity`** (a UUID, from `olo_scene_list_entities` /
  `olo_scene_get_entity` — a string or a number) or **`clear`:true** — giving
  both, or neither, is a clean argument error.
- An **unknown `entity` UUID leaves the current selection untouched** and
  reports `ok:false` — it never silently clears the selection on a typo'd id.
- `selected`/`entity`/`name` always reflect the **actual current selection**,
  read back from the panel — including on an `ok:false` result, where they
  describe whatever was already selected before the failed call (or are
  omitted if nothing was). `changed` is `true` only when this call actually
  moved the selection, so a redundant re-selection (or clearing an
  already-empty selection) is distinguishable from a real transition — the
  tool is `idempotentHint:true`.
- **Not undoable.** Unlike `olo_entity_set_field`, this does not route through
  the editor's `CommandHistory` — selection is editor UI state, not project
  data, so there is nothing to undo.
- A **consented write**, gated behind **Agent writes** for consistency with
  the other editor-state writes (`olo_scene_open`, `olo_scene_play`/`stop`),
  even though it never mutates the scene itself.

### Interactive UI verification (`olo_input_inject`)

**(consented write — gated behind Agent writes.)** The read-only tools can prove the
editor *renders*; they cannot prove a **click handler fires**. Verifying interactive UI
wiring — "does clicking that bone in the Animation panel actually select it in the
hierarchy?" — used to need a human at the keyboard (issue #607). `olo_input_inject`
closes that: it drives the editor's own input stream so an agent can click, drag, and
type, then observe the state change.

It is **not** an OS-level injector. `SendInput` / `SetCursorPos` would be worse than
useless here: the editor is normally *not* the foreground window when an agent drives it
(a background process cannot steal foreground on Windows), so OS input would land in
whatever window *is* focused — i.e. it would type into the user's real foreground app.
Instead the events are fed to the **ImGui GLFW backend callbacks** — the exact functions
GLFW calls for real input. Each one feeds `ImGuiIO` *and* chains to the engine's own
GLFW callback, which raises the corresponding engine `Event` through the layer stack. So
a single injected click reaches **both** ImGui widgets **and** the viewport entity-picking
path, exactly as a real click does. (The engine's poll-based `Input::IsKeyPressed` /
`IsMouseButtonPressed` / `GetMousePosition` read *hardware* state, which no synthetic
event can move; `OloEngine::SyntheticInput` is the overlay that makes them agree, so an
injected Ctrl+click really is a Ctrl+click.)

**Actions**

| `action` | arguments |
|---|---|
| `click` | `x`, `y`, `button?` (left/right/middle), `space?`, `modifiers?`, `doubleClick?` |
| `move` | `x`, `y`, `space?` |
| `drag` | `fromX`, `fromY`, `toX`, `toY`, `button?`, `space?`, `steps?` (1–64) |
| `key` | `key`, `modifiers?`, `keyAction?` (`tap` default / `press` / `release`) |
| `text` | `text` (printable ASCII, ≤ 256 chars — typed into the focused ImGui widget) |

`key` accepts a single printable character (`"s"`, `"5"`) or a named key: `escape`,
`enter`, `tab`, `backspace`, `delete`, `insert`, `up`/`down`/`left`/`right`, `home`,
`end`, `pageup`, `pagedown`, `space`, `f1`–`f12`, `ctrl`, `shift`, `alt`. Chords go
through `modifiers` (`{ "action":"key", "key":"s", "modifiers":["ctrl"] }` = Ctrl+S),
never through the key name. `keyAction: "press"` deliberately leaves the key **held**
until a matching `"release"` call.

**Coordinate spaces — read this before you click**

An off-by-one coordinate space makes this tool a liar, so `space` is explicit:

| `space` | meaning |
|---|---|
| `viewport` (default) | pixels of the `olo_screenshot` image **at native resolution**, origin top-left, +Y down. Note `olo_screenshot` downscales to `maxWidth` (default 1024) — if it did, its pixels are **not** these. |
| `viewportNorm` | fractional `[0, 1]` across the viewport, origin top-left. **Downscale-proof — prefer this after a screenshot** (`x = px / imageWidth`, `y = py / imageHeight`). |
| `window` | OS window client pixels, origin top-left. The space the ImGui panels live in — use it to click menus, buttons, and hierarchy rows **outside** the 3D viewport. |

A point outside the viewport (in a viewport space) or outside the window is a hard
error, never a silently clamped click. The response echoes the resolved point in both
window and native-viewport pixels plus the viewport's `pixelWidth`/`pixelHeight`, so a
mis-aimed click is visible rather than mysterious.

**Timing — why the call is synchronous**

A click is not an instant. ImGui only registers one if the button is **down across at
least one full frame** (a same-frame press+release never fires `IsItemClicked`), and the
editor's viewport entity picking is **two frames behind the cursor** (async PBO readback:
it issues the read at the ImGui mouse position one frame and reads it back the next). So
the tool expands each action into a frame-quantized plan — move → settle → press → hold →
release → settle — and the editor applies exactly **one frame of it per tick**, on the
game thread (`ImGuiIO` and the event queue are not thread-safe; nothing is injected from
the HTTP worker). The call then **blocks until those frames have been rendered**, so you
can immediately follow it with `olo_screenshot` / `olo_scene_summary` and see the result
rather than a frame from before the click.

The response's `after` block already reports the consequence — no second round-trip:

```jsonc
{
  "ok": true,
  "framesInjected": 13,
  "resolved": { "windowX": 940, "windowY": 460, "viewportPixelX": 640, "viewportPixelY": 360, "insideViewport": true },
  "viewport": { "pixelWidth": 1280, "pixelHeight": 720, "dpiScale": 1.0 },
  "after": {
    "pending": false,
    "viewportHovered": true,
    "selectedEntity": { "id": "12652600558176869447", "name": "Cube" },
    "hoveredEntity":  { "id": "12652600558176869447", "name": "Cube" }
  }
}
```

**The verification loop**

```jsonc
// 1) olo_screenshot {}                    -> look at the frame, pick a pixel on the thing
// 2) olo_input_inject { "action": "click", "space": "viewportNorm", "x": 0.5, "y": 0.55 }
//    -> "after".selectedEntity is the entity you aimed at? the click handler fired.
// 3) olo_screenshot {}                    -> the selection outline is now drawn around it
```

For an ImGui widget (a menu, a panel button, a hierarchy row), use `space: "window"` and
read the pixel off a full-window screenshot from the `run-oloengine` driver
(`driver.ps1 -Action shot`, which captures the whole window, not just the viewport).

**Caveats**

- One plan at a time: a second call while a sequence is still draining is refused
  (interleaving a press into someone else's drag means nothing). Retry once it drains —
  the calls are synchronous, so this only bites concurrent agents.
- `keyAction: "press"` leaves the key held on purpose. Send the matching `"release"`, or
  the next thing you do runs with that key down.
- The synthetic cursor override is dropped when a plan finishes, handing the mouse back
  to the human. Synthetic key/button state never masks a real physical press.

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
ssgi, overdraw** (exactly one is shown at a time; `none`, or `enabled:false`, clears them
all). It reports the `*DebugView` flag states and **`passEnabled`** — whether the pass
that produces the chosen buffer is actually running this frame — with an actionable
`note` when it is not:

```jsonc
// olo_render_set_debug_view { "mode": "ssao" }   (with SSAO not yet enabled)
{ "mode": "ssao", "ssaoDebugView": true, "gtaoDebugView": false,
  "ssrDebugView": false, "ssgiDebugView": false, "passEnabled": false,
  "note": "SSAO is not active; enable it with olo_render_toggle_pass { name: 'ssao' }." }
```

So the usual debug-view flow is two steps: `olo_render_toggle_pass { name: "ssao",
enabled: true }` then `olo_render_set_debug_view { mode: "ssao" }`. Calling the tool
with no `mode` lists the modes + the current state.

**`overdraw`** is the odd one out: unlike the AO/reflection/GI views it has no backing
effect to enable first (`passEnabled` is always `true`) and works on **every** rendering
path. It re-draws the frame's opaque geometry into a private single-channel accumulation
target with **depth testing off and additive blending**, so every fragment that *would*
be shaded — including the occluded ones an ordinary depth-tested pass overwrites — adds 1
to that pixel's counter. The per-pixel count is then heat-mapped (**black** = nothing
drew → **blue → green → yellow → red** as the layer count rises, saturating around 10
layers) and replaces the viewport, so "how many layers deep is this frame" is measurable
directly instead of inferred from A/B toggling passes. It answers questions like *is this
scene fill-bound?* and *is the depth prepass earning its keep?* at a glance. Geometry with
no depth-only shader variant (skybox / terrain / voxel / custom-shader meshes) is skipped,
so the heatmap is an honest count of the batchable opaque mesh geometry rather than a
guess for everything.

**The `vg*` modes** (`vgclusterid` / `vglod` / `vgoverdraw`, issue #607) are the
virtualized-geometry (Nanite-style cluster LOD DAG) visualizations. They are the odd ones
out in a different way: they do **not** replace the viewport, they render into the
`VirtualGeometryDebug` render-graph target — so the flow is two calls, and the response
says so via `captureTarget`:

```jsonc
// olo_render_set_debug_view { "mode": "vglod" }
{ "mode": "vglod", "virtualGeometryDebugMode": "lod", "passEnabled": true,
  "captureTarget": "VirtualGeometryDebug",
  "note": "Capture it with olo_render_capture_target { name: 'VirtualGeometryDebug' }." }
// then: olo_render_capture_target { "name": "VirtualGeometryDebug" }
```

They are the **same knob** as `olo_virtual_geometry_set { debugMode }` — one write path,
so the two tools always report the same current state (setting a mode through either is
visible in the other's `current`, and selecting any non-`vg*` mode — including `none` —
turns the virtual-geometry visualization off, because exactly one debug view is active at
a time). Virtual geometry renders on the **Deferred** path only; the `note` says so when
it isn't. The mode change gates a render-graph *declaration*, so the tool settles a few
frames before returning — an immediately-following capture resolves the target instead of
answering "Unknown render-graph resource".

### Array render targets — capturing one CSM cascade (`layer`)

Shadow maps are **2D array** textures: the CSM is a 4-layer cascade array, and
`ShadowCSMRaw` / `ShadowAtlasRaw` are the comparison-OFF raw-depth views the PCSS blocker
search actually reads (issue #607 — until now they were pass-owned raw GL ids, invisible
to both `olo_render_list_targets` and `olo_render_capture_target`). To inspect one
cascade:

```jsonc
// olo_render_list_targets  ->  the array targets report their layer count
{ "name": "ShadowMapCSM", "kind": "Texture2DArray", "format": "Depth32Float",
  "width": 2048, "height": 2048, "layers": 4 }

// olo_render_capture_target { "name": "ShadowCSMRaw", "layer": 2 }
```

`layer` (alias: the original `face`) selects the array layer or cube face. It is
**validated**, not clamped: asking for cascade 7 of a 4-cascade array is an error, because
silently returning cascade 0 is the confidently-wrong answer this whole tool family exists
to remove. Two related rules the capture path now honours:

- A **per-cascade layer view** (`ShadowMapCSMCascade3`) resolves to its *parent* array
  texture — that is what a sampler binding wants — so a readback that ignored the view's
  own layer would hand back cascade 0's pixels with no error at all. Such a resource now
  defaults to **its own** layer, and reports it in the capture meta.
- Depth targets are min-max **normalised** by default (`normalize` overrides), so a
  cascade is legible rather than a flat white field.

### Mid-frame state & exact texels (`afterPass`, texel space, stats, validate)

Four instruments added after the GTAO black-sky hunt (issue #607), where each missing one
cost hours:

**`afterPass:"<PassName>"`** (on `olo_render_capture_target`, `olo_render_probe_pixel`,
`olo_render_target_stats`, and `olo_render_validate`'s `compare`) snapshots a resource
**as of that pass's execution**, not end-of-frame. The motivating case: ParticlePass
re-exports `SceneDepth` *after* GTAOPass, so an end-of-frame read of `SceneDepth` can
never show what GTAO actually sampled mid-frame. Mechanics: the tool arms a one-shot
post-pass hook, renders a frame, and the hook clones the resolved texture bitwise
(`glCopyImageSubData`, every mip and layer) the moment the named pass finishes — so
transient-pool aliasing can't swap the contents before the readback. Pass names come from
`olo_render_graph_topology_export`'s `executionOrder`; a culled or unknown pass is an
error, never an empty image.

**`space:"texel"`** (on `olo_render_probe_pixel`, with `mip`) addresses an **exact texel**
of the target's own mip grid, top-left origin — required for padded resources like the
HZB pow2 pyramid, where the default proportional viewport mapping reads the wrong texel.
Every probe reply now carries `mappedCoord` — requested coords, the texel actually read,
the GL row, and the mip dims — so the mapping is never guesswork in either space.

**`olo_render_target_stats {name, rect?, mip?, layer?, afterPass?}`** answers the 1-ULP
questions a PNG cannot: exact min/max/mean over finite values, NaN/Inf counts, and a
**bit-exact unique-value histogram** (distinct bit patterns + the most frequent values
with counts). "Is this HZB region exactly 1.0f?" is one call:

```jsonc
// olo_render_target_stats { "name": "HZB", "mip": 0, "rect": { "x": 0, "y": 0, "w": 256, "h": 256 } }
{ "channels": [ { "channel": "r", "finiteCount": 65536, "min": 1.0, "max": 1.0, "mean": 1.0,
                  "uniqueValues": 1, "topValues": [ { "value": 1.0, "bits": 1065353216, "count": 65536 } ] } ] }
```

**`olo_render_validate`** is the on-demand frame-validation sweep: compiled resource
hazards, barrier/build diagnostics, resolve failures, resources that are *consumed but
resolve to no physical backing*, and versioned-name groups (`SceneColor@ParticlePass`)
with their resolved GL ids. The optional `compare` block is the bitwise instrument:

```jsonc
// olo_render_validate { "compare": { "a": "SceneDepth", "b": "HZB", "afterPass": "GTAOPass" } }
// -> "compare": { "bitwiseEqual": false, "differingTexels": 3,
//                 "firstDiffs": [ { "x": 511, "y": 12, "a": 1.0, "b": 0.99999994, ... } ] }
```

With `compare.afterPass`, BOTH sides are cloned by the *same* hook firing, so the verdict
describes one consistent frame. Note the format caveat: a D24 depth source quantizes on
float readback, so only same-conversion pairs (D32F/R32F) compare bit-exactly.

The **DDGI probe atlases** (`DDGIIrradianceAtlas0/1`, `DDGIVisibilityAtlas0/1`,
`DDGIProbeData`) and the **froxel-fog volumes** (`FroxelFogScatter0/1`,
`FroxelFogIntegrated`, 3D — pick a z-slice with `layer`) are registered render-graph
resources now (Setup-time imports, the FluidIntermediates pattern), so all of the above —
plus plain `olo_render_capture_target` — works on them. The atlases ping-pong: which ping
is "current" flips every blended frame, so both are listed under stable names; either
shows a black/leaking probe.

### Probing the froxel fog volume (`olo_froxel_fog_probe`)

Every volumetric-fog check we have compares **final-frame pixels**, which cannot tell
*"the scatter pass injected nothing here"* from *"fog was injected, but the composite
tapped the wrong froxel"*. `olo_froxel_fog_probe` samples the froxel volume itself
(issue #607; relates to #435) and returns **both** stages at one cell:

- `scatter` — `FroxelFogScatter.comp`'s output: per-froxel in-scattered radiance +
  extinction (what media injection and lighting produced).
- `integrated` — `FroxelFogIntegrate.comp`'s output: in-scatter accumulated from the
  camera to that slice + the transmittance — exactly what the fog composite trilinearly
  taps.

Raw right + integrated wrong isolates the integration; both right + the frame still wrong
isolates the composite tap.

```jsonc
// olo_froxel_fog_probe { "worldPos": [4, 2, -12] }
{ "volume": { "dims": [160, 90, 64], "near": 0.1, "far": 120.0,
              "depthDistribution": "exponential: viewDepth = near * exp2(log2(far/near) * (z + 0.5) / dimZ)" },
  "froxel": { "coords": [96, 51, 47], "viewDepth": 12.9, "inFrustum": true, "inDepthRange": true,
              "centerWorld": [4.01, 2.02, -11.98],
              "cellBounds": { "min": [...], "max": [...], "nearViewDepth": 12.5, "farViewDepth": 13.3 } },
  "scatter":    { "available": true, "inScatter": [0.41, 0.44, 0.52], "extinction": 0.031 },
  "integrated": { "available": true, "inScatter": [0.12, 0.13, 0.16], "transmittance": 0.78 } }
```

Address the cell either directly (`froxel: [x, y, z]`) or by world position
(`worldPos: [x, y, z]`), which is projected through the **same** mapping the shaders use —
including the **exponential** z-slice distribution and the view-ray secant scaling. A
world position outside the frustum, or beyond the fog volume's depth range (the volume
ends at `FogSettings::End`, clamped to [20, 500] — *not* at the camera far plane), is
reported as such rather than silently answered from the nearest cell. With fog or
volumetric fog disabled the froxel compute chain never runs, and the tool says so with the
toggle to flip instead of failing opaquely.

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

### Sun / time-of-day (`olo_scene_set_time_of_day` / `olo_scene_set_sun_angle`)

The **lighting** counterpart of the render-override A/B loop: move the sun from the
editor so an agent can iterate lighting for any rendering / water / GI / god-ray
work, without restarting the editor.

**Since issue #633 these are real component writes, not an ephemeral override.**
Both tools edit the scene's serialized **`TimeOfDayComponent`** — the astronomical
clock `TimeOfDaySystem` drives the directional light and the day/night
`AtmosphereSky` bake from. A change is visible on the next frame (the light moves
continuously; the baked sky follows on the component's rebake quantum), participates
in the editor undo stack like any component write, and is gated behind **Agent
writes** consent. There is nothing to "restore": the component is the authored
state. If the scene has no `TimeOfDayComponent`, both tools return an error asking
for one to be added (Add Component > Time Of Day) — they never auto-create entities.

`olo_scene_set_time_of_day { hours, dayOfYear?, latitudeDegrees?, timeScale?,
paused?, enabled? }` sets the clock directly — `0` = midnight, `12` = noon; sunrise
and sunset fall where the ephemeris puts them for the component's day-of-year and
latitude. The lighting inner loop:

```jsonc
// 1) olo_scene_set_time_of_day { "hours": 8 }     -> morning
// 2) olo_screenshot { … }                          -> the morning reference
// 3) olo_scene_set_time_of_day { "hours": 17 }     -> low evening sun
// 4) olo_screenshot { … }                          -> compare
// 5) Ctrl-Z twice (or set the original hours back) -> restore
```

`olo_scene_set_sun_angle { yaw, pitch }` aims the sun by angles: `yaw` is the azimuth
in degrees (from +Z toward +X: `0` = +Z, `90` = +X/east) and `pitch` the elevation in
`[-90, 90]`. It **solves for the clock time** that best places the sun there (given
the component's day-of-year and latitude) and writes that time; when the requested
elevation is unreachable on that day, it clamps to the closest achievable one and
says so in `note` (with `clamped: true`).

Both validate every numeric input with `std::isfinite` and report the resulting
component state plus the derived sun elevation / is-night flags. `clear: true` is
accepted for backward compatibility and reports that there is nothing to clear.

Weather has the same pair of controls: `olo_scene_set_weather { state,
transitionSeconds?, immediate? }` retargets the scene's `WeatherStateComponent`
(states: Clear, Overcast, Rain, Storm, Snow, FogBank) and `olo_scene_get_atmosphere`
(read-only) reports the time-of-day, weather, and cloudscape state in one call.

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

### Progress notifications & cancellation

Long-running tools (anything that settles/ticks frames — `olo_screenshot` and
`olo_render_compare_golden` with a `camera`/`orbit` pose, `olo_perf_capture_frame`,
`olo_render_frame_breakdown`) support the MCP **progress** and **cancellation**
utilities:

- **Progress.** Send a `progressToken` in the call's `_meta`
  (`params._meta.progressToken`, string or integer) and the server emits
  `notifications/progress` (`progressToken`, a monotonically increasing
  `progress`, `total` when known, and a human-readable `message`) while the
  tool runs. Over Streamable HTTP the POST response is then upgraded to a
  **`text/event-stream`**: the progress notifications arrive as SSE frames on
  the POST connection itself (spec-conformant: request-related messages go on
  the request's own stream, never the standalone `GET /mcp` stream), followed
  by the final JSON-RPC response frame, after which the stream closes. A call
  without a `progressToken` (or without `text/event-stream` in `Accept`) keeps
  the plain single-JSON response — nothing changes for clients that never ask.
- **Cancellation.** Send a `notifications/cancelled` notification (`params.requestId`
  = the in-flight call's JSON-RPC id, matched by exact value — `"5"` does not
  cancel `5`) on any connection; the server sets a cooperative flag the running
  tool polls between frames/steps. The tool stops promptly, any half-produced
  result is discarded per spec (the SSE stream ends **without** a response
  frame; a plain-JSON call returns JSON-RPC error `-32800` "Request cancelled",
  which the client ignores). Cancelling an unknown / already-finished request
  is a no-op, per spec. Tool authors: poll `server.IsCurrentCallCancelled()`
  in any loop that runs longer than a few hundred ms, and call
  `server.EmitProgress(progress, total, message)` as work advances — both are
  no-ops when the caller didn't opt in, so there is no cost to instrumenting.

```jsonc
// POST /mcp  (Accept: application/json, text/event-stream)
// { "jsonrpc":"2.0", "id":9, "method":"tools/call",
//   "params": { "name":"olo_render_compare_golden",
//               "arguments": { "goldenPath":"water_side.png" },
//               "_meta": { "progressToken":"golden-1" } } }
//
// <- Content-Type: text/event-stream
// data: {"jsonrpc":"2.0","method":"notifications/progress","params":{"progressToken":"golden-1","progress":1,"total":3,"message":"settling frames before capture (1/3)"}}
// data: {"jsonrpc":"2.0","method":"notifications/progress","params":{"progressToken":"golden-1","progress":3,"total":3,"message":"settling frames before capture (3/3)"}}
// data: {"jsonrpc":"2.0","id":9,"result":{ ... }}                  // final frame, then the stream closes
//
// To cancel mid-flight (any connection):
// { "jsonrpc":"2.0", "method":"notifications/cancelled",
//   "params": { "requestId": 9, "reason": "user changed their mind" } }
```

### Script-defined tools (Lua)

You can add **project-specific diagnostics tools** to the server without
recompiling the engine (issues #357 / #607; design in
[`docs/adr/0005-mcp-script-tools-lua-sandbox.md`](../adr/0005-mcp-script-tools-lua-sandbox.md)).
Drop `*.lua` files into **`<project assets>/McpTools/`**; each is executed when
the scripts are scanned (at editor start, and on every live reload) and registers
tools via the injected global:

```lua
-- Assets/McpTools/health_digest.lua
RegisterMcpTool{
    name        = "script_health_digest",          -- reserved script_* namespace
    title       = "Project health digest",
    description = "One-call digest: scene summary + shader errors + asset problems.",
    schema      = { type = "object", properties = {}, additionalProperties = false },
    handler     = function(args)
        local scene  = olo.call_tool("olo_scene_summary", {})
        local errors = olo.call_tool("olo_shader_errors", {})
        return {
            scene        = scene,
            shadersBroken = errors ~= nil and errors.count or 0,
        }
    end,
}
```

The rules (enforced, not advisory):

- **Names** must match `script_[a-z0-9_]+` — a reserved namespace so a script
  can never shadow a built-in `olo_*` tool. Default toolset: `script`, so
  `tools/search { "toolset": "script" }` lists exactly the user-authored tools.
- **The sandbox is capability-stripped.** Handlers run in a dedicated Lua
  state with `base`/`math`/`string`/`table` only; there is no `io`, `os`,
  `debug`, `package`, `require`, `dofile`, `loadfile`, or `load`, and `print`
  goes to the engine log. The only engine access is
  `olo.call_tool(name, args)` and `olo.log(message)`.
- **Read-only by default; write tier by declaration.** Without `writes = true`
  a script tool is `readOnlyHint:true` and `olo.call_tool` refuses **every**
  project-mutating tool — including another script tool that declares
  `writes = true` (it is itself a write tool), so there is no path from a
  read-only tool to a mutation. See "Write-tier script tools" below.
- **Schema-enforced like native tools.** The optional `schema` table becomes
  the tool's `inputSchema`; malformed arguments are rejected before your
  handler runs, exactly as for built-in tools.
- **Failure is contained.** A Lua error becomes a clean `isError:true` result
  with the message; a runaway handler is stopped by a per-call watchdog
  (default 10 s) and honours MCP cancellation; a handler that allocates without
  bound is stopped by a per-state **memory quota** (default 64 MiB of headroom
  — an allocation bomb fails its own call with a memory error instead of taking
  the editor down); a result that can't map to JSON (functions, userdata, mixed
  keys) is reported as an error.
- **Icons are optional** (MCP SEP-973): pass
  `icons = { { src = "data:image/svg+xml;base64,…", mimeType = "image/svg+xml",
  sizes = { "any" } } }` and the client may show it next to your tool. `src` is
  required; an `https://` URL works too. Omit the key entirely if you have none.
- **Registration failures never take the server down** (bad name, duplicate, no
  handler, malformed schema/icons, a non-boolean `writes`): they are logged, the
  tool is skipped, and the panel surfaces the count.

#### Reloading edited scripts — no restart (issue #607)

Edit a `.lua` file and pick it up **live**, with the server still running:

- call **`olo_script_tools_reload`** (an agent can iterate on its own tools), or
- click **"Reload script tools"** in the MCP panel.

Either rescans `<project assets>/McpTools/*.lua` and republishes the whole set —
additions, edits and **deletions** all take effect. Calls already in flight keep
running against the tools they started with. The server then emits a
`notifications/tools/list_changed` on every live `GET /mcp` stream (and
advertises `capabilities.tools.listChanged: true`), so **call `tools/list` again
after a reload**.

#### Write-tier script tools (`writes = true`)

A script tool that declares `writes = true` becomes a project-write tool:

```lua
RegisterMcpTool{
    name    = "script_perf_lite_mode",
    title   = "Renderer: lite mode",
    writes  = true,                     -- <- the whole opt-in
    schema  = { type = "object", properties = {}, additionalProperties = false },
    handler = function(args)
        -- Only reachable because this tool declared writes = true.
        local r = olo.call_tool("olo_renderer_settings_set",
                                { setting = "upscale", value = "performance" })
        return { applied = r }
    end,
}
```

(The full example ships at
`OloEditor/SandboxProject/Assets/McpTools/perf_lite_mode.lua`.)

What that changes — and nothing else:

- it lists with **`readOnlyHint:false`**;
- it goes through the **same write-consent gate as a native write tool**:
  refused outright while "Agent writes" is **Disabled** (the default), a modal
  in **Prompt**, straight through in **Allow all** (see
  [Write consent](#write-consent--disabled--prompt--allow-all-issue-306-item-c));
- **only then** may its handler call project-mutating tools through
  `olo.call_tool` — and each such inner call re-checks the *current* consent
  mode, so flipping writes back to Disabled stops the rest of the macro.

The rule to keep in mind: **write authority is the executing tool's own declared
tier — never inherited from a caller, never borrowed by a callee.** A write-tier
tool that calls a read-only script tool does not lend it the capability; a
read-only tool that calls a write-tier one is refused. You consent to the
**macro** (by name and arguments, in the modal), not to each write inside it —
so keep a write-tier tool's description honest about what it changes.

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
