# Unified image layouts validation (#1181)

Compare both the clean git baseline and the runtime-disabled policy before
attributing a rendering or timing change to unified layouts. The optional policy
uses `GENERAL` for ordinary sampled/storage access; attachment, transfer and
presentation layouts, memory dependencies, and layout tracking remain necessary.
See [ADR 0011 amendment (100)](../adr/0011-amendments.md#100-sampled-and-storage-images-share-general-when-the-device-enables-unified-layouts).

## Setup

Measured on 2026-09-13, Windows, RTX 4090, NVIDIA 616.64, Vulkan 1.4.351,
SDK and Khronos validation 1.4.357.0. `vulkaninfoSDK` reported extension revision 1
and `unifiedImageLayouts = true`. Both builds used Clang 23.1.0, the `dev-cached`
CMake preset, and the same assets. The clean control was a separate detached
worktree at `433c3e67d0efbc966d922242f15ed1d75279bdfa`.
The image-layout implementation is commit `60f51319b`.

- Control: original git implementation, Release editor.
- Unified: changed Release editor, `OLO_VULKAN_NO_UNIFIED_IMAGE_LAYOUTS=0`.
- Optimal: changed Release editor, `OLO_VULKAN_NO_UNIFIED_IMAGE_LAYOUTS=1`.

Each arm used a new editor process. Startup logs confirmed the selected policy.
The viewport was 1280 x 720, soft shadows were PCSS, and normal async compute and
Release parallel recording remained enabled. Drift used paused time of day 5.2.
VirtualGeometryStress used the fetched, checksum-verified Stanford xyzrgb dragon
and Deferred rendering: 24 instances, 2,834,448 tested clusters and 593 drawn
clusters at the timing pose, with no unresolved virtual-mesh registrations.

| Scene | Paths | Camera position | Target | FOV |
| --- | --- | --- | --- | --- |
| Drift | Forward, Forward+, Deferred | (0, 8, -274) | (0, 2, -250) | 60 |
| VirtualGeometryStress | Deferred | (0, 5, 18) | (0, 1, -20) | 60 |

## Correctness

Release OloEditor and Debug OloEditor, OloEngine-Tests, OloRuntime and OloServer
builds exited 0; expected executables existed and dependency checks passed.

- Full Debug suite: exit 0; 8,278 ran, 8,238 passed, 40 skipped, 0 failed
  (1,887.086 seconds). Twelve disabled tests were not run. Skips included optional
  fixtures/tool modes, Linux-only behavior, unavailable Steam test SDK fixtures,
  and the Vulkan frustum-cull tenant after earlier OpenGL initialization.
- Barrier-lowering/debug-lever contracts: exit 0; 23 passed, 0 skipped, 0 failed.
- Device-policy and storage-to-sampled GPU contracts: exit 0 in each mode;
  2 passed, 0 skipped, 0 failed per mode.
- Fresh Vulkan-only suite after the fixture cleanup: exit 0 in each mode;
  173 passed, 0 skipped, 0 failed per mode (52.290 seconds unified,
  48.523 seconds optimal). This includes the full-suite-skipped frustum-cull case.

The first isolated Vulkan run exposed pre-existing fixture cleanup omissions:
`VulkanParallelRecordingDevice.BucketsReplayWithItemOwnedMaterialAndInstanceUploads`
and `VulkanPassSuite.OverdrawRunsTheEmptyReplayAndMapsCountsToHeatColours` each
left the process-static VSM's two inert storage buffers alive at device teardown.
Both failures reproduced on the clean git control (exit 3, two 16-byte VMA
allocations), and the bucket failure also reproduced with unified layouts
disabled. Test-only cleanup now releases these lazily created buffers in a
standalone run and preserves them when an initialized renderer owns them.
Assertions were retained. A four-test sequence ran twice in one process: the two
Vulkan fixtures, then OpenGL renderer initialization and a full pipeline tick.
All eight invocations passed, with no skips, exit 0. Renderer initialization
occurred once, so the second iteration exercised preservation of the live GL
renderer. The full-suite result above precedes these test-only cleanup edits;
the isolated and mixed sequences validate both affected ownership cases.

The new `VulkanDrawPath.ComputeDispatchWritesStorageImageThroughRootData` coverage
dispatches a storage write, binds its output through the production sampled
descriptor path, samples it in a second compute dispatch, and checks every output
pixel after readback. The policy test checks that the extension is optional and
the active device flag matches feature support and the startup override. Headless
lowering tests assert equal-layout memory masks, queue-transfer halves, first use,
and unchanged attachment/transfer layouts.

Live Vulkan and OpenGL sessions covered all four scene/path rows. The candidate
reported zero shader errors, zero render hazards, no consumed-but-unbacked
resources, and no resource resolve failures. Debug Vulkan repeated the four
scenarios with the Khronos validation DLL confirmed loaded and synchronization
validation enabled. IBL cache entries were moved aside before launch, forcing a
fresh bake. Its log contained no VUID, SYNC-HAZARD or device-loss errors.

Existing render-graph MissingProducer diagnostics named externally supplied IBL
textures, CloudsHistory, GBufferResolved and GBufferBakedGI. The baseline Deferred
terrain binding diagnostic and VirtualGeometry's
`CopyBufferSubData(outside recording bracket)` warning were also present. These
are distinct from Vulkan validation errors and remained outside this change.

An independent whole-tree shader-toolchain comparison compiled 385 stages per
arm with zero failures and zero SPIR-V disassembly differences: 277 GL graphics
first stages, 83 GL compute approximations, and 25 Vulkan-only stages. Options
matched `ShaderCompilationTest`; GL compute is native driver GLSL at runtime, so
its SPIR-V check is a headless approximation. No shader or OpenGL source changed.

## GPU timing

Consult the PR discussion for timing results and the current acceptance status.
The collection protocol uses three interleaved clean git-control/unified pairs per
scene, plus the runtime-disabled comparison, with 60 seconds of warm-up and 30
samples per scene. Other editor, test and compiler processes are monitored. An
overlap from scene loading through final diagnostics rejects that scene; clean
scenes are retained and missing pairs are retried. The first collection attempts
overlapped sibling worktree activity and do not support a performance conclusion.
GPU clock/power-state telemetry accompanies the subsequent runs because clocks
varied on this machine. No speedup is assumed.

Whole-frame GPU timestamps intermittently report zero even in the clean control
while pass timestamps are fresh and positive. A diagnostic control with async
compute disabled also returned 5 zero frame readings in 30 samples. Zero readings
are retained in the raw data and excluded from the valid-frame median, with the
remaining sample count reported. Summed pass GPU durations are reported separately;
they do not measure elapsed frame time when queues overlap or CPU submission gaps
exist. Neither a zero reading nor a pass-duration sum is substituted for frame time.

## Visual evidence

The [capture gallery](../../OloEditor/assets/tests/visual/unified-layouts-1181/README.md)
contains 22 selected original PNGs, including matched Vulkan views of every tested
path, OpenGL controls, and Debug IBL-bake evidence. Metadata records fresh frame
indices and image hashes. The pilot pair's images remain valid visual evidence;
its timing samples were excluded because sibling builds/tests overlapped them.

Captures use explicit camera poses, 30 settle frames and forced rendering;
liveness confirmed a visible, ticking, non-minimized editor. Fourteen metadata
records retain `captureUnready=true` at the later capture marshal, after the
settling wait completed; all captures report `stale=false`, and the actual images
were inspected. In addition to the
timing pose, Drift was inspected at (30, 24, -290) looking at (0, 2, -250), and
(0, 65, -260) looking at (0, 0, -220). The dragon scene was inspected from
(23, 16, 8) toward (0, 0, -30), (0, 45, 10) toward (0, 0, -45), and a close view
at (-5.5, 1, 0) toward (-5.5, 0, -6), FOV 45.

Matched static dragon captures from the initial control/candidate live sessions
were pixel-identical on both backends. Drift contains animated water and clouds;
repeat captures from the same binary also differ, so visual inspection and
same-binary repeat measurements supplement image differences. No new visual
regression was observed. Existing backend differences in water appearance and
the scene's dragon/ground intersections were visible in both builds.

A physical device without the extension was unavailable locally. The forced-off
arm covers the old policy on the available GPU; it does not replace testing an
unsupported driver. This change does not add same-pass attachment feedback or
remove all possible image-layout mismatches.
