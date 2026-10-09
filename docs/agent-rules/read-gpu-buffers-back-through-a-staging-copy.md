# Read a buffer the GPU keeps using back through a staging copy

**The rule.** A diagnostic that reads a buffer the renderer will keep writing or drawing from
copies the range into a throwaway buffer first (`RenderCommand::CopyBufferSubData`) and reads
*that* (`ReadBufferSubData`). Never `glGetNamedBufferSubData` / `StorageBuffer::GetData` on the live
buffer. And a timing that follows any readback in the same process is suspect until the frames
before and after the readback have been compared.

## What stayed green

`FoliageGPUCuller::ReadbackResult` (behind `FoliageRenderer::ReadbackCull`) read the cull's state
header, indirect args, source-row tail and compacted instance stream straight out of the buffers
the next frame's cull writes and the foliage pass draws from. Every result it returned was correct,
and `FoliageGPUCullEvidenceTest` asserts on those results, so nothing failed.

What changed was everything after it. On an RTX 4090 with GL (driver 617.14), one readback of the
main view left, for the rest of the process:

| main view, same pose, same survivor counts | before a readback | after |
|---|---:|---:|
| `FoliageCull` (group + instance kernels) | 0.09 ms | 1.9 ms |
| `FoliagePass` (forward draw) | 3.0 ms | 21 ms |
| whole frame | 14 ms | 35 ms |

The shadow views' culls and casters, which nothing read back, did not move. The likely mechanism is
the driver relocating a buffer the CPU has read into memory the CPU can read, and leaving it there;
whatever the mechanism, the staging copy removes it (0.086 / 0.093 ms before / after).

## How it was found

The #1391 foliage cost baseline recorded each cell's main-view survivor census with
`ReadbackCull`. Every cell after the first read 2.5x more expensive than a 600-frame window at the
same pose. Arm switches were cleared first (each returned to steady state within one frame). The
census counts (groups visible, visible, reserved, submitted) were then identical between a cheap
first visit and an expensive return visit to one pose; the same per-plant work had become more
expensive. Only the main view's brackets moved, and only the main view had been read back. Turning
the readback off made every visit cheap.

## The guard

`FoliageCostBaselineTest.ReadingTheCullBackLeavesLaterFramesAsCheap` measures the main view's cull
and draw medians, reads every layer back, and measures again; it fails at 2x (the defect was 13-22x
on the cull). With the staging copy reverted it fails (cull 0.085 -> 1.109 ms, draw 3.28 -> 13.71 ms).
