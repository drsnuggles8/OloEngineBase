# The raw bindless route is compiled by the driver's own GLSL front end

**Rule.** A shader on the OpenGL bindless route (`OLO_RHI_BINDLESS=1`, any program whose
include-resolved source names `OLO_BINDLESS`) reaches the driver as GLSL text through
`OpenGLShader::CreateProgramFromRawGLSL`. No shaderc, no SPIR-V and no SPIRV-Cross stand between
your source and the driver, so a shader that compiles everywhere else can still be rejected there.
Two limits apply that the slotted route never shows you:

1. **Do not name an identifier `packed`, `row_major`, `register` or `char`.** NVIDIA's front end
   reserves them; glslang does not. As a parameter or local name they fail with `C1012: abstract
   parameters not allowed` followed by `C1503: undefined variable` at every use. Inside
   `layout(...)` they are qualifiers and fine.
2. **Keep every stage at 14 uniform blocks or fewer, counting `OloHeapOffsetBlock`.** The driver
   counts every active block against `GL_MAX_<stage>_UNIFORM_BLOCKS`. NVIDIA reports 14, the GL 4.6
   minimum; Mesa radeonsi reports 15. The slotted route prunes inactive blocks in SPIRV-Cross and has
   no heap block, so a stage can sit at the limit there and be over it here (`C5058: no buffers
   available for bindable uniform`, at link time).

A program the driver rejects **falls back to the slotted route per program** and still renders, so
the only symptom is a log line. Since #1565/#1567 that line is
`[Bindless] '<file>' FELL BACK to the slot-based route: …` at error level. Grep the log for it after
any bindless run.

Two heap rules surfaced while verifying the above, both on the raw route only:

- **A released heap slot must publish its typed null in every build.** Poison-on-free was Debug-only;
  in Release a freed slot kept its now non-resident handle, and a draw reading the stale offset
  crashed NVIDIA's driver (`0xc0000409` in `nvoglv64.dll`) after a scene's textures were freed. A
  crash that reproduces in Release and not in Debug on the raw route is this class first.
- **An inherited sampler must survive the seam.** `HeapBinding` passes `DepthCompare = false` for
  every ordinary bind; the GL backend used to inherit the texture's own sampler only for `true`, so
  every seam bind sampled REPEAT + linear-mip whatever the texture said. Pinned by
  `HeapGpuFixture.AClampToEdgeTexture3DKeepsItsWrapThroughTheHeap`, which goes through the real seam
  rather than `CreateView(…, ViewDesc{}, …)`.

## How it is guarded

`OloEngine/tests/Rendering/PropertyTests/BindlessRawRouteTest.cpp`:

- `GlslDriverPortability.NoShaderUsesAnIdentifierTheNvidiaFrontEndReserves` scans every shader file
  (CPU, runs in CI). Extend `kNvidiaReservedIdentifiers` if a driver rejects a new word.
- `BindlessRawRoute.EveryStageFitsTheGlMinimumUniformBlockBudget` preprocesses every raw-route GL
  program with `OLO_BINDLESS` defined and counts declared blocks per stage (CPU, runs in CI). It
  logs the fullest stages: `PBR_MultiLight`'s fragment stage sits at exactly 14 — one more shared
  lighting block breaks the main mesh shader on NVIDIA.
- `BindlessRawRoute.EveryOptedInProgramIsBuiltOnTheRawRouteByThisDriver` builds every opted-in
  graphics and compute program on the real driver and fails on any fallback. It skips without
  `GL_ARB_bindless_texture`, so it only runs on a developer box. Run it after touching a shared
  include.

To run any headless evidence test on the raw route, set `OLO_RHI_BINDLESS=1` in the environment of
the test binary; the heap reads the lever at initialisation.

## The two incidents

**#1567.** `oloGroomUnpackTint(float packed)` in `include/GroomStrandCommon.glsl` took
`GroomStrand` and `VSM_GroomDepth` off the route in every bindless editor run on NVIDIA. A
one-function probe on an RTX 4090 (driver 617.14, `#version 460 core`) reproduced it and showed
that renaming the parameter alone fixes it. The same probe, over every layout-qualifier name and
the C/Cg/HLSL keyword lists, found the other three words; every other reserved word is rejected by
glslang too, so the slotted route already catches it.

**#1565.** `Terrain_PBR`'s fragment stage declared 14 ordinary blocks plus the heap block. Two of
them were terrain-only per-frame constants: the editor brush preview (binding 11) and the snow
accumulation clipmap (binding 16), which the tessellation-evaluation stage of the same program
already reads. Both now travel as `flat` varyings from that stage (locations 3–6, same in
`Terrain_GBuffer`), which leaves 13 of 14 and needs no CPU or layout change. Moving
`OloHeapOffsetBlock` to an SSBO was the alternative; it would have pushed the fragment storage-block
count toward its own limit of 16 and touched every bindless consumer.

## How to measure, not guess

A standalone WGL program that compiles and links files with `glShaderSource` and prints the
info log and `GL_UNIFORM_BLOCK_REFERENCED_BY_FRAGMENT_SHADER` counts answers both questions in
seconds. Feed it the `.bindless.failed.glsl` dump from `%LOCALAPPDATA%\OloEngine\ShaderCache\opengl\`
(the exact text the driver saw), or a flattened stage with the route's two-line prologue
(`#extension GL_ARB_bindless_texture : require`, `#define OLO_BINDLESS 1`) inserted after
`#version`.
