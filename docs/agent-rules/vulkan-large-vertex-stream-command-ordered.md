# Vulkan: a vertex stream too large for the frame arena is written in command order (#1446)

Read before changing `VulkanVertexBuffer::SetData` / `GetPullAddress`, or `VulkanFrameArena`'s slot size.

## The rule

**A mutable vertex stream whose written range exceeds half an arena slot (8 MiB of 16) does not
snapshot. `SetData` records a staged `vkCmdCopyBuffer` into its persistent allocation, and its draws
read the persistent address.** The copy goes through `VulkanRendererAPI::UploadBufferSubData`: a
transfer in the frame command buffer between two global barriers. It keeps both guarantees the
snapshot exists for: a draw recorded between two writes sees the first write (command order), and
a write cannot overtake the previous frame's reads (queue order). It uses no arena space. The
switch is sticky, keyed on the written size, not the capacity, and it frees the stream's CPU shadow.

Below the threshold nothing changed: particle batches and the 3.8 MB and 5.8 MB precipitation
streams keep their arena snapshots, which cost no transfer and no rendering-scope break per write.

## The failure

#1433 (b3d66600a) made a stream switch to snapshots on its **first** rewrite; before, it took a
second rewrite in one frame. That was right for ordering and fatal for size. A bound groom refills
its 125-219 MB stream once per frame, so every coat became a snapshot that a 16 MiB slot can never
hold. `GetPullAddress` returned 0, root assembly dropped the draw, and `GroomAnimals.olo` rendered
bald on Vulkan with three errors a frame. OpenGL, and every scene without a deformed groom, were
fine, so the suite stayed green.

## What still refuses

A command-ordered write from a parallel-recording worker is dropped with a named error:
`UploadBufferSubData` cannot record on a worker. Today the only large stream, the groom pass's, is
written on the render thread.

## Evidence

`VulkanPassSuite.VertexStreamLargerThanTheArenaKeepsCommandOrderWithoutDroppingDraws` writes a
slot-plus-a-page stream twice with a draw after each, and checks: both draws land with their own
bytes, 0 dropped, 0 arena overflows, 0 vertex-snapshot bytes. With the switch disabled it fails
exactly as #1446 did: 2 overflows, 0 prepared draws, 2 dropped. It also checks that a small stream
keeps snapshotting.
