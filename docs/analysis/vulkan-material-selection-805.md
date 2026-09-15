# Vulkan per-instance material selection (#805)

Static deferred instances can now select their five material textures through a
GPU Scene material reference. This implements ADR 0011 amendment (101). It leaves
automatic scene-wide batching and the other shader families for subsequent work.

## Capability measurement

`VulkanDrawPath.GBufferGpuSelectsTexturesInSingleIndirectDraw` uses the production
frustum culler, instance buffer, indirect command, GPU-written root-data handoff,
material-table builder and `PBR_GBuffer` shader. A small compute probe supplies the
material-selection policy: it changes material references in the cull output
without CPU readback before drawing.

On an NVIDIA GeForce RTX 4090, driver 616.64, Windows Debug:

| Check | Result |
|---|---|
| Separate-material reference | 2 prepared draws |
| GPU-selected materials | 1 prepared indirect draw; 1 GPU-written-root draw |
| Output comparison | Exact byte equality across all 6 G-buffer attachments |
| Selection-change control | Swapped textures; exact equality to the swapped reference |
| Stale generation / out-of-range slot | Per-draw fallback |
| Texture reload with stable RHI identity | New texture sampled |
| Destroyed texture | Material factor used for the missing map |
| Vulkan validation errors | 0, including synchronization validation |

The new GPU test and the device-free
`BindlessShaderPipeline.MaterialShaderHeapTableMatchesCompiledRecordLayout` test
passed together: **2 passed, 0 skipped, 0 failed; process exit 0**.

The draw counters measure submitted capability, not speed. This experiment does
not establish a frame-time improvement or automatic merging in the editor. The
probe also binds material records directly. Existing instanced producers do not
carry canonical material references: the `InstanceData` overload and CPU batch
payloads drop them, and GPU culling copies that unlinked input. A future producer
must preserve or generate the references and restore the material binding after
culling aliases slot 17. The live editor checks below establish compatibility,
not GPU-selected material consumption through the current instanced dispatcher.
Depth, shadow and
non-table state also constrain which materials a future batcher may combine.
Keep #805 open.

## Shader compatibility

All 147 root shader files were expanded and compiled before/after the change on
the OpenGL route: **303 stages had identical SPIR-V disassembly**, with no changed
or rejected stages. Both Vulkan stages of `PBR_GBuffer` compiled and passed
`spirv-val`; the shader-extension-floor check passed all 14 extension probes and
their compile/reflection round trips.

The AMD RADV CI runner lacks descriptor-heap support. These local NVIDIA results
are the Vulkan device evidence; CI covers the portable contracts and builds.

## Live editor evidence

The final Debug editor ran Sponza from two fixed camera poses at 960 x 540 on
Vulkan and OpenGL, in deferred, forward and forward-plus modes. All twelve
accepted captures were live (`stale: false`) and visually inspected. The six
Vulkan captures are byte-identical to a separate run using the original
`PBR_GBuffer` shader in the same binary. Vulkan InstancingDemo also rendered its
legacy instanced cubes correctly. The final shader was restored after each A/B
experiment; Vulkan was restarted between shader variants.

A temporary magenta marker inside the successful table-read branch did not
appear in Sponza. Consequently these editor images do not prove that the new
branch is exercised by that scene; the GPU integration test is the positive
material-selection evidence. No automatic editor batching benefit is claimed.

Switching Vulkan to forward emitted missing storage-occupant diagnostics for
`PBR_MultiLight` slots 9, 10, 11, 12 and 18. The original shader run reproduced
all five diagnostics and the same visible green/white overlay lines. There were
no native VUID or device-loss messages in these runs. Vulkan's shader-error MCP
tool reported unavailable, so it is not counted as a zero-error check.

Captures, per-capture liveness JSON and logs are retained locally under
`.claude/skills/run-oloengine/shots/805/` (`*-final-*`, `vulkan-*-base-*`,
`vulkan-*-marker-*`). This directory is ignored; it is not a portable CI artifact.

## Build and suite results

The Windows Debug editor and test executable both built successfully through the
shared build lock (exit 0). The final focused run passed **19 tests, 0 skipped,
0 failed; exit 0**, in 152.902 seconds: all 13 `VulkanDrawPath` tests,
`CommandDispatchRecording`, the two-path instancing scene, the new compiled ABI
check and all three `RendererShutdown` tests. OpenGL's live shader-error query
also reported ready with zero errors.

The full suite finished with **8,495 passed, 43 skipped, 3 failed** (13 additional
disabled tests), **process exit 1**, in 2,457.587 seconds. Both new tests passed in
that run. The failures were:

- `ReSTIRPTDevice.ClosedCornellDiffuseAndGlossyIndirectMatchExactlyTruncatedReferenceSupport`
- `ReSTIRPTDevice.DiffuseAndNearSpecularEnvironmentAgreeWithTheWholeReferenceIntegral`
- `ReSTIRDIVisualEvidenceTest.ArmingTheTierOnANonRTDeviceStaysInsideTheRendererNoiseFloor`

The PT tests supply synthetic G-buffer data directly, bypassing the changed
raster shader, renderer table and dispatcher. Their unchanged production
`ReSTIR_PT` shader also has identical Vulkan SPIR-V before/after this branch.
The DI failure measured one changed pixel against a zero-pixel noise floor.
These failures are retained here; passing focused tests does not make the full
suite green.

An isolated recheck on the final binary reproduced both PT failures with the same
observed values; the DI check passed. Totals: **1 passed, 0 skipped, 2 failed;
exit 1**, in 115.479 seconds. The passing DI recheck alone does not diagnose its
one-pixel full-suite failure. No tolerances or tests were changed to hide any of
these failures.

After review, the GPU test restores the prior heap description/enabled state
against the live replacement backend, and compares both invalid-reference paths
against an explicit per-draw fallback across all six attachments. Two shuffled
iterations passed **29 tests, 1 skipped, 0 failed; exit 0**. The existing culler
fixture skipped once because an earlier OpenGL renderer test had initialized
process-wide UBOs; the new material-selection test passed in both iterations.
