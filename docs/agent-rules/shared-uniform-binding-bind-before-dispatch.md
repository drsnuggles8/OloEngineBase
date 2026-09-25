# A shared uniform binding needs `Bind()` before every dispatch

**Rule:** a producer that writes a uniform buffer at a binding point it shares with anyone else
calls `Bind()` on it before every dispatch or draw that reads it. `SetData` writes the buffer; it
does not make that buffer the one the shader reads. The shader reads the binding point's current
**occupant**, and the occupant is whoever bound last.

This holds on both backends. On OpenGL `Bind()` is `glBindBufferBase`. On Vulkan it publishes the
buffer into `VulkanBindingState`, which the root-data writer consults when it records the dispatch.
In both, a `UniformBuffer` claims its binding once, at construction, and keeps it only until another
buffer at the same binding is constructed or bound.

## Which bindings are shared

`UBO_RAY_TRACING` (65) has nine buffers: the RT shadow, reflection, path-tracer and ReSTIR DI/GI
blocks, ReSTIR PT, the RT probe, and the two deform producers (`DeformedSurfaceCache`,
`VegetationSurfaceCache`). The binding namespace is full (see the comment on `UBO_RAY_TRACING`),
so any new ray-tracing block also lands here. `UBO_FOLIAGE` is shared between the raster foliage
path and the vegetation deformer. Treat every binding as shared unless its header comment says
one buffer owns it.

## Why it fails silently until it faults

The failure depends on **order**, not on the code that breaks. A tenant that never binds works
for as long as it is the most recent buffer created at that binding, because construction claims
the slot. It breaks on the first frame some other tenant binds earlier in the frame, and that can
depend on scene content that has nothing to do with the broken tenant.

## The case that taught it (issue #1437)

`VegetationSurfaceCache::Dispatch` called `SetData` on its 96-byte params block and never
`Bind()`. It ran in `RayTracingScenePass`, and `SkeletalDeformPass` ran one pass earlier. Whenever
an animated surface deformed, `DeformedSurfaceCache` bound its own 48-byte block at 65. The
vegetation shader then read the skeletal block as its own: `Rest`, `Jobs` and `Tasks` are at
offsets 64 to 87, past the end of the 48 bytes, so it chased garbage device addresses.

- **Symptom:** `READ of invalid address 0x10000000000`, checkpoint `RayTracingScenePass`,
  `vkQueueSubmit2` → `VK_ERROR_DEVICE_LOST`. It reproduced every time (5 of 5 runs), including
  with `OLO_VK_ASYNC_COMPUTE=0`.
- **Why only Deferred:** RT vegetation and RT grooms are enabled only on Deferred
  (`WantsRayTracingVegetation`).
- **Why only with an animal:** without a skinned surface, nothing binds slot 65 before the
  vegetation dispatch. So the #1338 reduced scene with foliage and no fox completed 100/100
  frames, and the fault read as a property of the animal path.

## How it was found, and what to reuse

1. **An address no recorded binding covers is computed, not freed.**
   `OLO_VULKAN_ADDRESS_BINDING_REPORT=1` said `no recorded binding covers 0x10000000000 (2495
   ranges tracked)`. So the question was "which shader built this address", not "who freed
   this buffer" ([vulkan-device-fault-address-ownership.md](vulkan-device-fault-address-ownership.md)).
2. **Dump the inputs of the faulting pass per frame.** An env-gated log of every BLAS request, TLAS
   instance reference and deform dispatch showed all of them sane. That left the one shader in the
   pass whose inputs were not logged, the vegetation deform.
3. **Assert the prediction before fixing.** A capture of
   `VulkanBindingState::Get().GetUniformBuffer(65)` at the vegetation dispatch showed the skeletal
   deformer's buffer, not the vegetation one. Then an A/B on one binary with the fix behind an env
   lever: 3 of 3 completed, 5 of 5 faulted without it.

## Testing it without faulting the device

`RayTracingDevice.VegetationDispatchReadsItsOwnParamsAfterAnotherProducerTakesTheSharedBinding`
binds **zero-filled** stand-in tenants at both slots, then compares the dispatch bit-for-bit
against a cache nothing displaced. A zero block reads `TaskCount == 0`, and the shader returns on
its first line. So a regression writes nothing and fails the comparison instead of losing the
device. Pick the stand-in's contents so the shader's own guard turns a regression into a no-op.
