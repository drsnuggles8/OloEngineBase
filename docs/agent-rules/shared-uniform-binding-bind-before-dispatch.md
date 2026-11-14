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

## The check that enforces it (issue #1330)

Every draw and dispatch now compares each uniform block the program reads with the binding point's
occupant: its size, and its declared GLSL block name ([ADR 0025 §5a](../adr/0025-renderer-state-has-four-owners.md)).
A read of another tenant's buffer is refused, logged as `[BindingContract]` with the program, the
binding and both blocks, and the shader is given a zero block for that program's draws (the occupant
is back for the next program). These rules come with it:

- **A buffer on a multi-layout binding declares its block.** Pass the GLSL block name to
  `UniformBuffer::Create(size, binding, "BlockName")`. The multi-layout bindings are
  `ShaderBindingLayout::IsMultiLayoutUBOBinding` (0, 2, 7, 8, 14, 18, 30, 65).
  `UniformBlockContractSourceTest` fails on a creation site that does not, on a name no shader
  declares at that binding, and when a shader adds a second layout to a binding the table does not
  list.
- **Size the buffer for the whole block the shader declares, not for the members you write.** The
  particle renderer uploaded one `mat4` into a 64-byte buffer under a 384-byte camera block; the
  shader read its previous view-projection and jitter past the end. The contract found it on its
  first run.
- **A new pass that reads a pass-scoped binding** (`IsPassScopedUBOBinding`: 7, 8, 14, 18, 30, 65) is
  covered by `FramePredecessorPoison`, which leaves another layout there after every render-graph
  pass and requires an unchanged frame and no refusal. Add the feature to that test's scene if the
  pass does not run in it.
- **A shader that declares `MotionBlurMatrices` / `MotionBlurUBO` (binding 8) needs
  `Renderer3D::BindMotionMatrices()` after its `Bind()`.** The matrices are uploaded once per frame
  but nothing publishes them for the frame: the Gaussian-splat sort block is another tenant of 8
  (#1687). A scene-bucket draw gets the bind from `CommandDispatch::BindSceneResources`.
- **The camera block (binding 0) is the frame's at every pass entry, and a tenant that draws
  camera readers after its own puts it back.** The camera is frame-published: some passes read it
  without binding it. Other tenants have the same layout, so no contract check can refuse them. The
  render graph binds the frame's block again before every pass (`CommandDispatch::PublishFrameCamera`),
  so a pass may leave its own camera buffer there when it ends. Inside a pass the rule is still
  yours: declare a `CommandDispatch::ScopedFrameCameraRestore` before a bake's camera buffer is
  created, as the sky bakes and `IBLPrecompute` do. It restores on every exit, the failed ones
  included.
- **Do not cache "which buffer is bound".** Ask `RendererAPI::GetUniformBufferOccupant`. A cache
  kept by one caller sees only that caller's binds (#1688).
- **To skip a bind, ask `RendererAPI::IsUniformBufferBound`, not the occupant.** On OpenGL the
  record follows the engine's bind calls and a raw `glBindBufferBase` (a test, FSR2) goes around
  it, so the backend vouches only for a point bound through the engine since the dispatcher last
  dropped its caches.
- **A pass that replays packets needs no cache invalidation before the replay.** The dispatcher's
  redundant-bind caches (program, vertex array, textures, render state) are dropped at every
  replay entry. `CommandDispatch::InvalidateBindingCaches` is for code that dispatches without a
  replay.

`OLO_BINDING_CONTRACT=0` turns the check off for an A/B. On OpenGL the block names come from the
linked program, where SPIRV-Cross renames a later stage's copy `Name_1`: do not name a block with a
`_<digits>` suffix.
