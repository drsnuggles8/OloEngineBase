# MCP material, Box2D and pack validation (#607)

The live checks use the Release editor on Windows with an RTX 4090, on 2026-10-07.
Both backends report **Forward** through `olo_renderer_support`. This is the single
renderer path requested by the handover. The scene is the repository's
`MaterialSpheres.olo`, copied into an isolated project together with `Physics2D.olo`
and the real 64x64 `Checkerboard.png` texture.

## Material fields

The subject is entity `780000000000100`. The camera is at `[-4.5,1.4,-6.8]`, looking
at `[-4.5,0.5,-4.5]`, with a 45-degree FOV and a 640x360 viewport.
`olo_entity_list_fields` discovers the generated MaterialComponent fields on both
backends. All edits below use `olo_entity_set_field`, with Metallic fixed at 0.8:

1. Red `[0.9,0.04,0.02,1]`, Roughness 0.05.
2. Blue `[0.02,0.2,0.9,1]`, Roughness 0.05.
3. Blue, Roughness 0.9.
4. `olo_editor_undo`, restoring Roughness 0.05.

These are full-frame mean absolute differences in 8-bit PNG channels. Changed
pixels use a maximum channel difference greater than 3.

| Backend | Color change: mean / pixels | Roughness change: mean / pixels | Undo versus blue smooth |
|---|---|---|---|
| OpenGL | 5.238640 / 33,940 | 2.936571 / 31,707 | identical pixels |
| Vulkan | 5.232815 / 34,225 | 2.935743 / 31,726 | identical pixels |

OpenGL captures: [red smooth](evidence/mcp-capability-gaps-607/opengl-red-smooth.png),
[blue smooth](evidence/mcp-capability-gaps-607/opengl-blue-smooth.png),
[blue rough](evidence/mcp-capability-gaps-607/opengl-blue-rough.png),
[undo](evidence/mcp-capability-gaps-607/opengl-undo-roughness.png).

Vulkan captures: [red smooth](evidence/mcp-capability-gaps-607/vulkan-red-smooth.png),
[blue smooth](evidence/mcp-capability-gaps-607/vulkan-blue-smooth.png),
[blue rough](evidence/mcp-capability-gaps-607/vulkan-blue-rough.png),
[undo](evidence/mcp-capability-gaps-607/vulkan-undo-roughness.png).

Release builds compile out shader registration: `olo_shader_errors` truthfully
returns `unavailableInThisBuild`, rather than a valid zero count. The application
logs contain no shader compilation errors or Vulkan VUID errors during these checks.

## Box2D and real packs

The OpenGL GUI session opens the real `Physics2D.olo`. Body and collider lists each
return five entities in Edit, then five with live Box2D state in Simulate. A ray
from `[0,4]` with translation `[0,-8]` hits entity `8254140920860137284`.
The Jolt list remains explicitly 3D, returns zero entities, and reports
`hasAuthored2D:true` and the Box2D alternatives in both states.

Both asynchronous builds of the isolated project succeed, each containing three
registered assets including two scenes. The returned texture records report:

| Compression | Format | sRGB | Dimensions |
|---|---|---|---|
| Off | RGB8 | false | 64x64 |
| On | BC4 | false | 64x64 |

BC4 is the actual cooked format for this grayscale checkerboard. Both builds
report available warning logs with zero warnings. Parent traversal and absolute
output paths and an actual junction pointing outside the project are rejected
through the live tool. The regression test deliberately
includes a missing skin profile to verify that nonempty build warnings and the
failed-asset count are returned too.

The [structured observations](evidence/mcp-capability-gaps-607/observations.json)
preserve field discovery, setter readbacks, measured image deltas, actual pack
records, physics results and sandbox errors. Discovery credentials are excluded.

## Regression coverage and scope

The final Release regression run executes 97 cases: **96 pass, one skips**.
The skipped symlink case requires Windows developer mode or symlink privilege;
its lexical path checks run before the skip, and the live NTFS junction escape
check above exercises final containment on this machine. The
[JUnit report](evidence/mcp-capability-gaps-607/regression.xml) records the cases.
The run requires a real GL context (`--olo-require-gpu --olo-gl-backend=glfw`);
texture serialization and the real headless MCP screenshot/dispatch check pass.

The initial Vulkan run exposed an early memory census: it reported 16 allocations
(33,998,848 bytes) before the context released its legitimate heaps/frame buffers.
The Vulkan census now runs after device teardown and its final reclaim drain;
only `Renderer::Shutdown` requests that deferred census. Temporary Vulkan contexts
leave the process-wide tracker active. A real OpenGL renderer followed by the
temporary Vulkan context runs in one process with no skips: the tracked sentinel
survives context teardown and normal deallocation removes it. The
[two-case JUnit report](evidence/mcp-capability-gaps-607/vulkan-context-tracker.xml)
records this regression check. OpenGL retains its existing census location.
The fresh session exits normally with no
survivor or VMA diagnostics. See the [before summary](evidence/mcp-capability-gaps-607/vulkan-teardown-before.txt)
and [complete post-fix teardown](evidence/mcp-capability-gaps-607/vulkan-teardown-after.log).

`McpFieldRegistry.MaterialFactorsAndSkinProfileUseLiveSettersAndUndo` covers setters,
undo/redo, roughness clamping, exact u64 skin handles and the YAML round trip.
`McpPhysics2D.*` covers real Box2D/Jolt worlds for 2D-only, 3D-only, mixed and empty
scenes in Edit and live physics, live shape/velocity/ray results and input rejection.
`McpAssetPackBuild.*` covers the real command, consent, sandbox containment including
final symlinks, headless refusal, both compression settings, warning reporting and
just-saved scene bytes despite a cached earlier scene. `TextureAutoCookPackTest.*`
compares reported texture metadata against the emitted record header and the
successfully loaded texture. Its deliberate missing-source fallback checks the
header because the reconstructed texture remains unloaded.

Material field discovery and Box2D inspection support GUI and headless hosts with
an active scene. Material field writes require the GUI's command-history hooks;
the headless host explicitly refuses these field writes. Pack
starts require the GUI's scene save and project/shutdown lease hooks; a headless host
receives an explicit error. Polling and cancellation retain the latest operation.

Packed-runtime launch is deliberately outside this tool's scope: an asset pack does
not select a start scene or create the distribution manifest, shaders, script module
and runtime binaries. Build Game assembles that distribution. Neither runtime
backend was launched as part of this pack-only feature.
