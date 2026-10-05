# Attachment feedback verification (#1554)

Validate aliases against physical storage, then sample independent snapshots before
writing the scene framebuffer. The [implementation guide](../../../agent-rules/render-graph-attachment-feedback.md)
explains the attachment/aspect/range policy and copy placement.

The full requested matrix is 144 cells: four features x two backends x three paths x
requested MSAA {1, 4} x {Native, Bilinear, FSR2 Quality}. Each rendered live cell has
two captured camera poses. Captures read the active target region, so a bilinear
raw target PNG can be 480x270 while the display is 640x360. `matrix.json` records
both renderer dimensions and each attachment's captured dimensions. It also contains
the actual state, copy comparison, snapshot allocation/lifetime, shader draw
observations, pose and artifact hashes.
The captures use an RTX 4090 on Windows, driver 617.14. These are functional checks;
no timing or performance conclusion is drawn.

## Requested and actual support

| Request | Actual contract |
|---|---|
| Native | Render/display 640x360 |
| Bilinear | Render 480x270, display 640x360 |
| Deferred Bilinear | Explicit capture rejection under #1537, for both requested sample counts |
| Forward/Forward+ 4x | Requested 4x, actual single-sample scene target |
| Deferred 4x | Four-sample G-buffer; resolved lighting feeds single-sample SceneColor and snapshot consumers |
| OpenGL FSR2 Quality, single sample | Temporal FSR2 when available; record the resolved technique |
| Vulkan FSR2 Quality | Spatial fallback `backendNotOpenGL` |
| Deferred 4x FSR2 Quality | Spatial fallback `msaaResolved` (takes precedence over the backend fallback) |

The eight rejected cells per backend are verified rejections. Their GL fixture PNGs
have the `_Rejected1537` suffix and are diagnostic frames from that request, followed
by the explicit rejection check. They do not establish a supported deferred
dynamic-resolution render.

## Oracles

- Contract tests cover the historical water normal feedback, transitive framebuffer
  versions/views, canonical colour/depth exports, precise aspects and ranges,
  independent resolve backing, whole-intersection allowances, and transfer-only
  ordering. Ordinary attachment loads/blends are legal; shader sampling needs its
  own legal storage contract.
- The GL matrix fixtures execute the real production pass graph and compare the
  source/snapshot sampler state. All 144 new enabled/off PNGs are under
  `OloEditor/assets/tests/visual/`, named `Attachment{Skin,Snow,Fluid}[Off]_GL_...`
  and `Water_Attachment[Off]_GL_...`. Existing native multi-angle fixtures and
  analytic controls remain in place. No existing noisy golden was recooked.
- Live audits require the affected pass to remain reachable, zero feedback hazards,
  zero consumed-but-unbacked resources, zero resolve failures, and zero missing-producer
  diagnostics for snapshot parents. Each allocated
  snapshot backing begins at its named copy node and survives its affected consumer.
- Snapshot equality is a **channel-0 bit-pattern comparison at the copy node**.
  It does not claim a full-channel bitwise texture comparison. Enabled/off image
  checks and analytic tests independently exercise the visual result.
- Vulkan audits require fresh prepared draws with zero dropped draws for the
  affected shaders. Fluid additionally requires fresh recorded `FluidSmooth`
  dispatches, with zero dropped/refused dispatches. Release internal counters are
  supplemented by an externally loaded Khronos validation layer with synchronization
  validation enabled.
- A/B images use native 1x, two angles, and repeated-enabled captures. Unrelated AO,
  bloom, TAA/FXAA, motion blur, auto-exposure and SSR are disabled. Deferred's SSR
  introduced variation around the reflective helmet; disabling SSR made the repeated
  skin frame identical while preserving the diffusion enabled/off difference.
  The editor selection gizmo is cleared. A/B difference must exceed five times the
  repeated-enabled RMS variation. Water A/B additionally sets wave speed and both
  normal-map scroll speeds to zero transiently, with undo restoration. Residual
  variation is reported rather than assumed to be zero; the full matrix retains
  the scene's original animated water.
- Generic fluid component writes are unavailable during Simulate because there is
  no editor command history. No live fluid enabled/off result is claimed. Its real
  live splat/thickness/composite draws, smoothing dispatches, snapshot equality and
  two views complement the GL fluid enabled/off fixtures and Vulkan analytic test.

Foreign editor/test/runtime GPU processes invalidate a capture; only those explicitly
identified overlaps are retried as cells. The first Vulkan 4x G-buffer transition
spanned a nine-second log gap and exceeded the MCP host's five-second unclaimed
read budget. The final harness requeues only safely cancelled read requests within
a bounded transition wait and still requires a completed in-capture audit. Failed attempts are retained separately from the
selected verified records. Shader errors and external Vulkan diagnostics are checked
in the full preserved logs.

Fluid readiness is checked after entering Simulate and before copying or auditing.
Simulate copies the scene and resets the renderer; `FluidSystem::OnUpdate` creates
the particle instance only on a positive timestep. The benchmark initially pins
the clock, so requested settings can latch before the first submitted fluid frame.
An early Forward+ FSR2 audit caught that empty schedule, while the completed image
and counters contained fluid. The failed attempt remains preserved. The harness
now waits for both fluid passes within that same capture, records initialization
frames, and still fails if readiness or the completed audit misses its deadline.

## Cameras and visible subjects

| Feature | First pose (position; yaw/pitch; FOV) | Second pose |
|---|---|---|
| Skin | (0, 1.08, 3.1); 0/1; 42 | (2.9, 1.25, 1.7); -60/4; 42 |
| Snow | (128, 60, 128); 0/24; 60 | (190, 65, 160); -63/30; 60 |
| Fluid | (7, 5, 7); -45/21; 60 | (-7, 4, 5); 54/18; 60 |
| Water | (0, 16, 38); 0/23; 60 | (0, 1.5, 35); 0/3; 60 |

The skin scene contains Suzanne with the skin material and a reflective helmet.
These establish visible diffusion and depth-boundary behaviour; they are not an
AAA head art-quality claim. Snow occupies the terrain view; the fluid dam-break
scene runs in Simulate and shows fluid around the boxes; water includes submerged
objects and a grazing surface view.

Native live contact sheets show every feature/path from both angles:

- [OpenGL first angles](opengl-native-angles-1.png), [second angles](opengl-native-angles-2.png).
- [Vulkan first angles](vulkan-native-angles-1.png), [second angles](vulkan-native-angles-2.png).

## Reproduction and checks

Build `OloEditor`, `OloRuntime`, `OloServer` and `OloEngine-Tests` in Release through
`build-lock.ps1`. Run the `RenderGraph*` and `FSR2Policy*` CPU contracts with
`--olo-gl-backend=none`. Run the real GL fixtures with `--olo-require-gpu`:

```text
RenderGraph*:FSR2Policy*:SkinDiffusionScene.*:AllPaths/SnowLayerTest.*:FluidVisualEvidenceTest.*:WaterVisualEvidenceTest.*:FSR2VisualEvidenceTest.*
```

The two actual-device Vulkan analytic checks are
`VulkanPassSuite.SnowBlurAddsTheDiffusedSnowHalfIntoSceneColorInPlace` and
`VulkanPassSuite.FluidCompositeFloorsWithoutIntermediatesAndPassesRefractionThrough`,
run with `--olo-require-vulkan`. Live Vulkan requires `-Rhi vulkan`, the backend
banner and a loaded `VkLayer_khronos_validation.dll`; passing these two tests alone
is insufficient.

Negative controls fail as intended: the original name-only validator misses the
historical water hazard, and allocating snapshots as bare `Texture2D` objects gives
repeat wrapping (10497) instead of source clamp-to-edge (33071). The production
framebuffer-view snapshot allocation passes the sampler guard. A third negative
control catches synthetic snapshot-parent shader reads inventing missing producers
and Undefined transitions; the final graph retains the real transfer-to-sample
barrier and refreshes late-created views against their earlier framebuffer writers.
A fourth control forces registry construction during Setup and exposes stale
producer/consumer metadata; final expansion invalidates that cache too.

`checks.json` records test counts, build/binary provenance, log results and the
negative controls. `ab.json` records every native live A/B measurement. Full raw
captures, failed attempts, diagnostic crash evidence and logs are retained locally
under the ignored `build-cached/feedback-evidence` directory.
