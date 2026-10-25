---
paths:
  - "OloEngine/src/OloEngine/Renderer/**"
  - "OloEngine/src/Platform/OpenGL/**"
  - "OloEngine/src/Platform/Vulkan/**"
  - "OloEngine/src/OloEngine/{Groom,Terrain,Atmosphere,Particle,Fluid,Snow,Precipitation,Wind}/**"
  - "OloEditor/assets/shaders/**"
---

# Rendering changes must be visually verified

A renderer change can pass every CPU test and look broken on screen. For anything that changes what
the screen shows, do all three:

1. **Pin the math** with a CPU/contract test (runs in CI). Golden images are never the primary check:
   an L1–L5 contract test is required as well.
2. **Capture PNGs from several camera angles and look at them.** Follow
   [WaterVisualEvidenceTest.cpp](../../OloEngine/tests/Rendering/PropertyTests/WaterVisualEvidenceTest.cpp):
   render the real pipeline, read back, write to `OloEditor/assets/tests/visual/`, read the PNG.
   Such tests use `EnableRendering(w, h)` + `RunFrames` / `RunEditorFrames` and skip cleanly without
   a GL 4.6 context; never `DISABLED_`. Read back by RHI handle and name evidence with
   `TestRenderBackend::EvidenceName`, and the same test gives the Vulkan cells in
   `OloEngine-Tests --olo-rhi=vulkan` (testing-architecture.md rule 13).
3. **Run it in the editor** and check `OloEngine.log` for shader errors. The `run-oloengine` skill's
   `attach` action starts the editor with the MCP diagnostics server; `olo_screenshot`,
   `olo_camera_*`, `olo_shader_errors` and `olo_render_capture_target` inspect the live frame
   ([mcp-diagnostics-server.md](../../docs/guides/mcp-diagnostics-server.md)). In a task worktree,
   the `live-verifier` agent does the capture work.

Verify every path the change reaches: `{OpenGL, Vulkan} x {Forward, Forward+, Deferred}`, plus MSAA,
upscale and resolution where reachable (task-loop §2a). A new G-Buffer lane is wired on every path.

Do not report a rendering change done on unit tests alone. If you cannot inspect a frame, say so.
If you reach for an `olo_*` tool that does not exist, add a one-line bullet to the open MCP tracker
(#607) rather than working around it silently.
