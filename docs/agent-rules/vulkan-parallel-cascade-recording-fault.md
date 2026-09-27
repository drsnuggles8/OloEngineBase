# A parallel-region bisect finds the trigger, not the cause: the #1504 cascade fault

**Rule:** bisect a Vulkan parallel-recording fault by forcing one region at a time inline, not by
reading the checkpoint: a checkpoint names the pass the GPU was in when it faulted, not the pass that
set the fault up. Then treat the region the bisect finds as the **trigger** until a crash dump names
the reader. Here it found the shadow cascades, and the cascades were not the cause.

**What the cause was:** the descriptor heaps were unbound after `vkCmdExecuteCommands`, so the
fixed-function commands that followed a forked region read a stale driver descriptor from the heap's
reserved range. Forking the cascades added an execute before the shadow pass's depth clears and the
scene's depth export copy. See
[vulkan-descriptor-heap-rebind-after-execute-commands.md](vulkan-descriptor-heap-rebind-after-execute-commands.md)
(#1511). The cascades fork again. #1504 recorded them inline behind `OLO_VULKAN_PARALLEL_CSM`; the
inline branch, its lever and its policy test were removed on #1511.

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
runs, once an `R16G16_SFLOAT` colour attachment.

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
still faults, so the cascade arm is not a timing mask. It was a real trigger. It was not the cause.

## Why every engine-side reference came back absent

The #1198 audit, extended, ran on faulting runs:
- validation;
- a record-time use-after-retirement set;
- a slot → image map at the free;
- lifetime holds of 16 generations;
- secondary-pool resets;
- GPU-AV.

All six were empty, correctly. The stale reference was never an engine command or an engine heap
slot. It was a descriptor the driver had written into the reserved range for an earlier copy, read
by a copy recorded while no heap was bound. Only two things could see it: Aftermath's marker call
stacks, and a dump of the reserved range (#1511).

## What the visual check found

Recording the cascades inline left the four cascade layers byte-identical, yet one lit Vulkan
forward frame came out darker than a forked one. The difference was entirely in GTAO's `AOBuffer`,
which alternated frame by frame between all-255 and a real AO term. That was #1512, the same unbound
heap, read by GTAO's clear. Two single captures had landed on different phases. Diff the
intermediate targets (`olo_render_capture_target`) before blaming the pass you changed, and take
more than one capture of anything that could alternate.

## Two traps

- **The checkpoint pointed at a bystander.** `ScenePrepassPass` was the last marker in every
  report; inlining its region changed nothing. The pass that mattered recorded earlier in the frame.
- **A workaround that removes the trigger passes every test.** Inline cascades took the fault from
  7/8 to 0/8, and the unbound copies after the scene fork were still there. Keep a re-test lever on
  such a workaround, as #1504 did, and remove the workaround once the cause is fixed.

Instruments: the per-region inline switch was a temporary `std::getenv` in
`VulkanRendererAPI::RecordParallelOrdered` keyed on the region's debug label and item count. It is
the fastest bisect for this class of fault.

Related: [vulkan-device-fault-address-ownership.md](vulkan-device-fault-address-ownership.md),
[vulkan-parallel-recording.md](vulkan-parallel-recording.md),
[vulkan-parallel-pass-audit.md](vulkan-parallel-pass-audit.md).
