# Record Vulkan shadow cascades inline: forked, they fault a freed scene target

**Rule:** `ShadowRenderPass` records its cascade (CSM) region inline, never through
`RenderCommand::RecordParallel`. The atlas region still forks. `OLO_VULKAN_PARALLEL_CSM=1` restores
the forked cascades so the workaround can be re-tested against a new driver; on the NVIDIA driver
it was measured on, that brings the device fault back.

**And a method rule, which cost more time than the fix:** a checkpoint names the pass the GPU was
in when it faulted, not the pass that set the fault up. Bisect a parallel-recording fault by
forcing one region at a time inline, not by reading the checkpoint.

Issue #1504.

## What stayed green

Every CPU test, validation (no VUID before the fault), the render-graph hazard check, and a fresh
editor that opens `ParallelRecording.olo` a few times. The issue's own control, "opened directly,
fresh editor: clean 3/3", was a 1-in-2 fault that happened to miss three times.

## The fault

Debug editor, `--rhi=vulkan`, `Scenes/Benchmark/ParallelRecording.olo` (3600 unique casters, a
sun with four cascades, two point and two spot shadow lights). About 2–4 s after a scene-target
resize or a render-path switch (typically deferred → forward), the device is lost:

```
[Vulkan]   fault address: 0x20071fb10 — instruction pointer (faulting)
[Vulkan]   fault address: 0x4070a4000 — READ of invalid address   (sometimes WRITE)
[Vulkan]     owner: UNBOUND (freed) VkImage ... range=0x407000000..0x4080e0000
[Vulkan]   checkpoint (graphics): stage=0x1 pass='ScenePrepassPass'
```

With `OLO_VULKAN_ADDRESS_BINDING_REPORT=1` the owner is always a **scene-target attachment from an
earlier viewport size**, freed a few frames earlier: the `D32_SFLOAT_S8_UINT` depth-stencil in most
runs (already under #1198's extra-generation hold), once an `R16G16_SFLOAT` colour attachment. The
instruction pointer is constant across builds of one tree.

## How it was narrowed (live editor, 4–8 interleaved runs per arm)

| arm | faults |
|---|---|
| baseline, deferred → forward on ParallelRecording | 7/8 |
| `OLO_VK_PARALLEL_RECORDING=0` | 0/4 |
| `OLO_VK_ASYNC_COMPUTE=0` | 4/4 |
| `ScenePrepassPass` region inline (the checkpoint's pass) | 3/4 |
| `ScenePass` region inline (16 items, more CPU than the fix) | 4/4 |
| `ShadowPass` regions inline | 0/4 |
| shadow **atlas** region inline (14 items, one shared layer) | 4/4 |
| shadow **cascade** region inline (4 items, one layer each) | **0/8** |

The scene-pass arm is the timing control: it slows the frame more than inlining the cascades and
still faults, so the cascade arm is not a timing mask.

## Every engine-side reference was absent

Each of these was run on faulting runs and came back empty. It is the #1198 audit, extended:

1. **Validation:** no error before the fault. A submitted command buffer that referenced a
   destroyed image would have been reported at submit.
2. **Record-time use after retirement:** every image was put in a set when it was *enqueued* for
   destruction. No descriptor write (`VulkanResourceHeap::WriteImageDescriptor`, the one funnel,
   including the transient ring's `UploadSlots` and every null write) and no rendering-scope
   attachment named a retired image.
3. **Heap contents at the free:** a slot → image map kept in that funnel showed no heap slot still
   naming the image when `DestroyEntry` ran. The slot cache already poisons freed slots with the
   null image, so a stale *index* reads black and cannot fault.
4. **Lifetime:** `NotifyFrameCompleted` advances only after `vkWaitForFences` on that slot, and the
   fault does not care about async compute. Holding **every** image 16 generations instead of 2–3
   did not stop it; the fault followed the next forward switch after the free.
5. **Secondary pools:** resetting them with `VK_COMMAND_POOL_RESET_RELEASE_RESOURCES_BIT`, or
   resetting every pool of the slot each frame, changed nothing. Neither did recording the
   cascades' depth clear outside `vkCmdBeginRendering` instead of folding it into `loadOp`.
6. **GPU-AV** lost the device the same way without reporting an access first. Its descriptor-heap
   coverage may not reach this path, so that silence is weak evidence.

What is unique to the cascade region: its items are the only ones that each render into a
**different depth surface** (a single-layer view of the cascade array) from their own secondary
command buffer. The atlas items share one layer; the scene items share one target. A read that no
submitted command asks for fits the #1198 explanation, driver-side per-depth-surface bookkeeping.
It is not proven: this box has no Nsight Aftermath SDK, and only Aftermath could name the shader
and say whether the read was the driver's. If you have it, run the lever-on arm with
`OLO_VULKAN_AFTERMATH=1` before trusting this workaround further.

## Two traps

- **The checkpoint pointed at a bystander.** `ScenePrepassPass` was the last marker in every
  report; inlining its region changed nothing. The pass that mattered recorded earlier in the frame.
- **A one-line probe needs the destroy side too.** "Nothing writes a retired image" says nothing
  about a descriptor written *before* retirement and never overwritten. Check both halves.

Instruments for next time: the harness and probes are described in the #1504 PR. The per-region
inline switch was a temporary `std::getenv` in `VulkanRendererAPI::RecordParallelOrdered` keyed on
the region's debug label and item count; it is the fastest bisect this class of fault has.

Related: [vulkan-device-fault-address-ownership.md](vulkan-device-fault-address-ownership.md) (the
#1198 audit this extends), [vulkan-parallel-recording.md](vulkan-parallel-recording.md),
[vulkan-parallel-pass-audit.md](vulkan-parallel-pass-audit.md).
