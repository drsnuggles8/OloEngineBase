# Vulkan ray tracing: the acceleration-structure rules

Issue #978 built the first hardware ray-tracing code in this tree. These are the rules that were
not obvious before writing it, in the order they will bite.

## 1. A BLAS is per GEOMETRY; opacity is per INSTANCE

The natural shape — walk the GPU Scene instances, build a BLAS for each — produces two
`vkCmdBuildAccelerationStructuresKHR` entries writing the same destination structure the moment two
entities share a mesh, which is invalid usage. Accumulate per-geometry demand in the instance walk
and decide builds in a second pass.

The same mesh can be an opaque wall for one entity and an alpha-cutout for another, because
"is this alpha tested" is a property of the **material**, not the geometry. Do not encode that in
the BLAS geometry flags: build every geometry `VK_GEOMETRY_OPAQUE_BIT_KHR` and let
`VK_GEOMETRY_INSTANCE_FORCE_OPAQUE_BIT_KHR` / `FORCE_NO_OPAQUE_BIT_KHR` on the instance override it.
One structure, both uses.

## 2. An acceleration structure needs no descriptor

`rayQueryInitializeEXT(rq, accelerationStructureEXT(u_Params.TlasAddress), ...)` compiles to
`OpConvertUToAccelerationStructureKHR` under nothing but `OpCapability RayQueryKHR`. The TLAS
travels to the shader as a `uvec2` device address in ordinary buffer data — no
`VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR`, no descriptor-heap change.

**The conversion is only legal as a function argument.** Assigning it to a local is a compile
error (`accelerationStructureNV can only be used in uniform variables or function parameters`).

The same trick removes every other binding: with `GL_EXT_buffer_reference` +
`GL_EXT_buffer_reference_uvec2`, the ray batch, the hit batch and the GPU Scene tables are all
device addresses carried in one UBO. That is why #978 took only `UBO_RAY_TRACING` (65) — **the last
free buffer binding in the engine**. Anything after this must ride an existing block.

## 3. Builds ride the frame command buffer, never a one-shot

`VulkanOneShot` submits **ahead of** the still-recording frame. A BLAS built that way consumes
vertex data the frame has not uploaded yet — silently, with the wrong geometry, no error. Record
through `VulkanRendererAPI::BeginAccelerationStructureRecording()`, which closes the lazy
dynamic-rendering scope (every AS command is illegal inside a render-pass instance) and hands back
the frame's primary command buffer.

## 4. Compaction is a multi-frame handshake, because idling is banned

"No routine scene update calls `vkDeviceWaitIdle`" rules out the obvious compaction flow. Instead:

1. build with `ALLOW_COMPACTION`, barrier, `vkCmdWriteAccelerationStructuresPropertiesKHR` into a
   query slot — all in the build's own command buffer;
2. in a **later** frame, poll with `VK_QUERY_RESULT_WITH_AVAILABILITY_BIT` and **no** wait bit;
3. only once the result is available, allocate the compacted structure and record the copy.

A blocking read of that query on a slot whose write has not executed does not merely stall — it
never returns.

`ALLOW_COMPACTION` and `ALLOW_UPDATE` are mutually exclusive: a compacted structure cannot be
refitted. Make that structural (a class that refits never asks to compact), not a convention.

## 5. Retire the structure BEFORE its buffer

`VulkanDeferredReclaim` destroys in insertion order within a generation. An acceleration structure
and the `VkBuffer` backing it are two entries; enqueue the AS handle **first**. Freeing the memory
under a live structure is a use-after-free inside the driver, not a validation message.

## 6. Two narrow fields truncate silently

`VkAccelerationStructureInstanceKHR::instanceCustomIndex` is **24 bits** and `mask` is **8**, while
the GPU Scene lanes feeding them are `u32`. A straight assignment wraps, and a wrapped custom index
resolves to the wrong material on every hit — a wrong answer, not a crash. Range-check the index
(`FitsInstanceCustomIndex`) and *fold* the mask rather than truncating it, so an effect that only
ever sets a high bit does not become invisible to every ray.

`GPUSceneTransform` is exactly `VkTransformMatrixKHR`'s layout — three row-major `vec4`s. Copy the
twelve floats straight across; routing them via a `glm::mat4` is how a transpose gets in.

## 7. The stale-record hole is the generation counter saturating

`GPUSceneAllocationPolicy::NextGeneration` saturates at `u32` max and then leaves the generation
**unchanged**, so a tombstoned slot can keep the generation its last live handle had. A
generation-only liveness test therefore accepts a dead record. Always AND with the record's own
`Active` flag — that is what `GPUScene::GetLive*RecordBySlot` does, and it is the only reason a
removed instance cannot be hit through a stale TLAS entry.

The other half is that GPU Scene has **no explicit remove**: a record dies by not being re-staged.
So rebuild the instance list from live records every frame; never patch it.

## 8. Capability is four questions, not one

"Is ray tracing available" is: the extensions are listed, **and** the feature bits came back
`VK_TRUE`, **and** `vkCreateDevice` accepted them, **and** volk populated the entry points. Commit
the flag after `volkLoadDevice` and null-check the entry points — an enabled feature bit is not
proof the command is callable.

Keep it Tier-2 optional (never an ADR 0010 gate row), report the **reason** alongside the verdict
in one value, and give it a bisect lever — `OLO_VULKAN_NO_RAY_TRACING=1`, which is also the only
way to exercise the unsupported-hardware fallback on a machine whose GPU does support RT.

## 8b. An optional extension's USAGE BIT is invalid without that extension

A buffer usage flag from an optional extension is not free on a device that
lacks it. `VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR`
on every vertex and index buffer looks harmless — and makes **every buffer
creation** an error on hardware without ray tracing
(`VUID-VkBufferCreateInfo-None-09499`), which is precisely the
"unsupported RT keeps the raster renderer usable" criterion failing.

It is invisible to the test suite, because the device-backed tests only run
where ray tracing exists. It showed up on the first live launch with
`OLO_VULKAN_NO_RAY_TRACING=1` — which is the second reason that lever earns its
keep, beyond bisecting. Gate the bit on the enabled capability; asking
`VulkanDevice::Get()->IsRayQueryEnabled()` at buffer creation is safe, since no
buffer can exist before the device decided.

The gate has a second half: **clear the verdict in `Shutdown`**, next to the
host-image-copy fields that already do it for #809's reasons. A capability flag
that outlives its logical device is answered against a device that is gone, and
the next `Init` can be a different GPU or the same one with the lever set.

The general form: **when you add a capability, grep for every flag, stage bit
and struct you introduced and ask which of them the device must have ENABLED to
accept.** Usage flags and pipeline-stage bits both have this property, and both
fail loudly only on the hardware you are not developing on.

## 8c. `firstVertex` is an offset only for SUBMESH-LOCAL indices

A build addresses the vertex for index value `i` at
`vertexData + vertexStride * (firstVertex + i)`. This engine's shared mesh
sources hold **global** indices — absolute into the whole vertex array — and
`Submesh::m_BaseVertex` describes where a submesh's vertices *start*, not a
value the indices still need. So `VkAccelerationStructureBuildRangeInfoKHR::firstVertex`
is **0**, and `maxVertex` stays `BaseVertex + VertexCount - 1` (with global
indices that is genuinely the highest value the build can address).

`AnimatedModel`'s submesh split is the proof from the other side: extracting a
submesh into its own mesh rewrites every index as `orig - m_BaseVertex` and only
then sets `m_BaseVertex = 0`.

Passing `BaseVertex` applied the offset twice. It hid for as long as only
`MeshComponent` geometry was staged, because a single-submesh source has
`BaseVertex 0`; the moment `ModelComponent` submeshes were staged into the
canonical scene, sources with 25 submeshes and six-figure `BaseVertex` arrived
and Sponza's last submesh addressed vertex 195107 of 192492 — a device loss
reported as `READ of invalid address`.

**The crash was the loud half.** Every non-first submesh had been tracing
triangles shifted by its own `BaseVertex` wherever it did not fault. Nothing
warned: the validation layers cannot see it (the API usage is legal), and
GPU-assisted validation does not instrument fixed-function AS builds.

**How to check this class of bug at all.** Write the capture yourself: walk the
staged submeshes, take `max(index value)` over each one's index range, and
compare `BaseVertex + maxIndex` against the vertex count. A one-run log said
"13 of 26 submeshes address past the end" and named the arithmetic; four
build-and-probe hypotheses before it had said nothing.

## 9. A new `RHI::Access` write member is silently a read

`RHITypes.h`'s `IsWriteAccess` has a `default: return false;` — unlike every other `Access` switch
in the chain, which are default-less so `-Wswitch` catches a new member. A write enumerator missing
from it produces no write-after-write barrier and no diagnostic.

Worse, `RenderGraphBarrierPlanner`'s two GL flag resolvers also have defaults, and a
`MemoryBarrierFlags::None` result makes the planner log an `UnmappedTransition` and **drop the
barrier record entirely** — so the Vulkan lowering that would have understood the access is never
reached. Map a Vulkan-only access to the closest GL over-approximation, never to `None`.

## 10. Measured on the development GPU (RTX 4090, driver 610.88, SDK 1.4.357)

- `accelerationStructureHostCommands` = **false**, `accelerationStructureIndirectBuild` = **false**.
  Device-side, non-indirect builds are the only path. Record them; do not gate on these.
- `minAccelerationStructureScratchOffsetAlignment` = 128. AS storage needs 256-byte buffer
  alignment (VUID-…-offset-03734). Use `vmaCreateBufferWithAlignment`; a plain `vmaCreateBuffer`
  honours only the buffer's own memory requirement and a suballocation can land 16-aligned.
- `maxInstanceCount` = `maxGeometryCount` = 16777215, `maxPrimitiveCount` = 536870911.

## What is deliberately not solved here

The alpha helper reconstructs UVs and applies the cutoff, but does **not** fetch the texel: outside
`OLO_BINDLESS` a shader reaches a material texture through a per-draw slot binding, and one
ray-query dispatch has no per-draw scope to bind arbitrary materials into. The fetch is a
caller-supplied macro. Closing that needs the shader-visible sampler heap ADR 0011 §1.2a already
records — it is not an acceleration-structure problem.

Deformed geometry has a class, a refit heuristic and tests, but **no live producer**: skinned,
cloth and particle entities never reach the canonical GPU Scene at all (`Scene.cpp` skips them and
counts them in `GPUSceneUnsupportedCategory`). The policy is exercised by tests, not by a scene, and
that is stated rather than hidden.

Virtualized-cluster (Nanite-style) entities were on that list until issue #1144 and are not any
more, but they did **not** become a producer for the deformed class. Each virtual-mesh part is
traced through one FIXED proxy mesh built from the DAG's coarsest cut, so it classifies `Static` and
takes the build-once path — see [ADR 0023](../adr/0023-virtual-geometry-is-ray-traced-through-a-fixed-proxy.md).
Tracing the live per-frame cut instead is what would have needed a new class here, and that is the
main reason it was not done.
