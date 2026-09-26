# Bind the descriptor heaps for the whole of every Vulkan recording

**Rule:** a command buffer the Vulkan backend records has its resource and sampler heaps bound from
its first command, and a primary re-binds them **immediately** after every `vkCmdExecuteCommands`,
not at its next draw or dispatch. `VulkanRendererAPI::BindDescriptorHeaps` does this at
`BeginRecording`, `ResumeRecordingAfterFlush`, each forked secondary's begin and after every
execute, and `VulkanOneShot::Submit` binds at begin. These eager binds bind only a heap that exists
(`CmdBindIfCreated`): with no heap there is no reserved range to go stale, and the first draw or
dispatch still creates it. Do not add a recording path that skips them.

**Why it matters:** copies, clears, blits and resolves are not "descriptor-free". On NVIDIA they run
as driver-internal shaders whose texture descriptors the driver writes into the **reserved range**
of the bound resource heap (`minResourceHeapReservedRange`, 96 768 B here). The spec makes the
primary's heap binding undefined after `vkCmdExecuteCommands` unless every secondary names it in
`VkCommandBufferInheritanceDescriptorHeapInfoEXT` (ours do not), and the extension proposal says the
bindings "must be respecified in the primary". The validation layer does not check it for
fixed-function commands. Recorded unbound, the internal shader reads whatever descriptor the reserved
range last held.

Issues #1504, #1198 and #1512, one cause. Investigation: #1511.

## What it looked like

| symptom | what the stale reserved descriptor named |
|---|---|
| #1504: device fault seconds after a resize or path switch on `ParallelRecording.olo` | a scene target freed at the earlier size: the depth export copy's source |
| #1198: device fault on forward → forward+ with async compute | the freed depth target, read at its exact base |
| #1512: GTAO `AOBuffer` all-255 on every other frame | the other frame slot's AO scratch, via GTAO's clear on the async-compute buffer |

Before the old image is freed the same read returns the wrong image's contents instead of faulting,
so a copy "succeeds" with somebody else's texels. That is what #1512 is.

## How it was named

1. **Aftermath's automatic markers carry CPU call stacks.** `nv-aftermath-format -j -p <dir with
   OloEditor.pdb> <dump>` resolved the in-flight command to
   `SceneRenderPass::ExportSceneDepthAndNormals → CopyImageSubData → vkCmdCopyImage`, recorded right
   after the forked scene region. The active shader was `compute_01` / `fragment_01`,
   `internal=true`: the driver's own copy shader.
2. **The reserved range is host-visible, so read it.** At every fault the heap dump
   (`CrashReports/descriptor-heap-*.bin`, now written on any device loss) held a reserved-range
   header whose address range contained the faulting address: 8/8 faults on master. Nothing in the
   engine's slot region named it. A per-frame diff showed the entries unchanged for seconds while the
   export copy ran every frame: the unbound copies wrote nothing and read the stale entry.
3. **Census.** Logging every fixed op recorded with the heaps unbound listed the export copies
   (ScenePass, ScenePrepassPass), depth clears after the cascade fork (ShadowPass), and GTAO's clear
   at the start of the async-compute buffer.

NVIDIA texture header, for decoding a dump: 32 B per entry; address = `(word2 & 0xffff) << 32 |
word1`, width = `(word4 & 0xffff) + 1`, height = `(word5 & 0xffff) + 1`.

## The A/B (live editor, Debug, `--rhi=vulkan`, interleaved, one binary)

The negative control restored the old lazy binding behind a temporary env switch.

| arm | #1504 sequence | #1198 sequence (its own commit, hold off) | #1512 `AOBuffer`, consecutive frames |
|---|---|---|---|
| lazy binding (pre-fix) | 11/11 faults (3 plain master, 3 probe, 5 control) | 9/10 faults | alternates 212 / 255 |
| heaps always bound | 0/5 in the A/B, 0/10 in the final matrix | 0/10 (on master the sequence faults in neither arm, see below) | 212 on every frame |

On the fixed build the reserved entries are rewritten with the live images every frame. The final matrix
ran 37 live sessions without a fault: every path pair, parallel recording off, async compute off, the
four-scene sweep, MSAA and upscale switches, and GPU-AV.

## Why earlier audits missed it

Both #1198 and #1504 audited every engine reference: descriptor writes, rendering attachments,
barriers, copies and waits. All came back clean, and all were right: the copy named the NEW image.
The stale reference lived in the driver's part of our heap, written legitimately by an earlier,
bound copy of the then-current target. The workarounds each changed which image that entry named
when it was read, or when it was freed. #1198 held depth one generation longer. #1504 recorded the
cascades inline, so a bound clear refreshed the entry with a live image. Both masked the fault and
left the export reading the wrong image.

#1198 also needed a second engine bug, fixed on 2026-09-23 by `ecfcf9753`. A draw recorded in a
forked item took the engine-heap arm of `WriteMaterialHeapOffsets` and wrote lane value 0. Aftermath
mapped the fault to `PBR_MultiLight.glsl` `OLO_MAT_ALBEDO`, which samples by **byte** offset, and
heap byte 0 is reserved entry #0. That is why #1198 was an application fragment shader reading a
"descriptor nobody wrote": a shader-heap byte offset of 0 is never a null.

## Traps

- **"No command names the freed image" does not clear the engine.** The descriptor that named it was
  in the implementation's region of the engine's own heap. Dump the heap.
- **An application shader can read the reserved range.** Any shader-heap byte offset below
  `VulkanResourceHeap::GetSlotRegionOffset()` is the driver's, not a null.
- **A minimal device test did not reproduce it.** In a two-item fork NVIDIA still wrote a fresh
  header for the unbound copy. The pin is therefore the contract,
  `VulkanParallelRecordingDevice.DescriptorHeapsStayBoundAcrossForksAndResumes`, which fails on the
  pre-fix code, not a pixel test that passes on both.

Related: [vulkan-device-fault-address-ownership.md](vulkan-device-fault-address-ownership.md),
[vulkan-parallel-recording.md](vulkan-parallel-recording.md).
