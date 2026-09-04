# A baked lightmap region is addressed by `(entity UUID, sub-key)` — never by the entity alone

Issue #867 extended baked GI (#439) past the classic `MeshComponent`. Read this before touching
`Scene/SceneLightmapGather.*`, `LightmapEntityEntry`, `SceneLightmapRuntime`, the instanced draw
path, or `Model::DrawParallel`.

## The rules

1. **Every region is keyed by a pair.** `LightmapEntityEntry` carries `EntityUUID` **and**
   `SubKey`. `SubKey == 0` means "the whole entity", which is what the classic path emits — so
   nothing about #439's behaviour changed, only what else is now expressible.

   | receiver | sub-key |
   |---|---|
   | `MeshComponent` | `0` |
   | `VirtualMeshComponent` | `0` — one component is one `MeshSource`, so one unwrap and one region |
   | `InstancedMeshComponent` | that instance's `InstanceData::StableID`, with `AssetStableIDNamespace` ORed in for placement-asset instances |
   | `ModelComponent` | the index of the FIRST `Model::GetMeshes()` entry sharing that mesh's `MeshSource` |

2. **Gather receivers through `GatherLightmapReceivers`, never with your own walk.** Four things
   have to agree on the receiver set — the editor's bake gather, the reference world the bake
   traces against, the runtime's self-healing re-unwrap, and `ComputeBakeKey` — and all four now
   read one list. Issue #629's recurring failure in this repo is two loops that were supposed to
   match and quietly did not; here the mismatch does not render wrongly, it renders with **no
   baked GI and no error at all**.

3. **The packing sort's tie-break must include the sub-key.** `LightmapBaker::Prepare` orders
   plans by (size desc, UUID asc, **SubKey asc**). Drop the sub-key and the comparator ties for
   every instance of one batch, `std::sort` may order them differently between runs, and the
   bit-identical-bake contract every lightmap test rests on is gone — on exactly the scenes this
   feature exists for.

4. **A per-instance identity must be persistent, not positional.** The sub-key is `StableID`
   because inserting or erasing one instance must not re-key every later one; a positional index
   would stale a bake nothing actually changed. That is also why the save-game format round-trips
   `StableID` from v26 rather than letting `EnsureStableIDs` re-derive it on load.

5. **`ReferenceSceneBuilder::AddScene` cannot capture the bake's world any more.** It walks
   `MeshComponent` entities, so a scene whose static geometry is instanced would bake against a
   room those surfaces are missing from — no occluders, no bounce, no error.
   `AddLightmapReceivers` takes the gather's own list instead.

6. **The instanced region is written into the buffer the draw READS**, which is `imc.Instances` on
   the inline-only fast path and `imc._MergedCache.Data` otherwise, and it is stamped with
   `SceneLightmapRuntime::GetResolveGeneration()` so it is not redone per frame. Writing the source
   lists on the merged path would only take effect after the next cache rebuild. Anything that
   invalidates the merge must zero the stamp.

## Why each receiver broke the 1:1 model differently

- **Instanced** — one entity draws N instances at N world transforms receiving N different
  bounces. Not representable at all under one region per entity; this is why the sub-key exists.
  Atlas pressure rises with the instance count, which is what the multi-page atlas (#868) absorbs.
- **Model** — one entity fans out over several `MeshSource`s, each unwrapped separately. The
  dedup is load-bearing: on the warm `.omesh` path every mesh is a submesh *view* into ONE combined
  source, so the whole model is one unwrap and one region; emitting one input per mesh would
  rasterize the same whole source into N identical regions and burn N times the atlas.
- **VirtualGeometry** — did NOT break the 1:1 model at all (one component is one `MeshSource` and
  therefore one unwrap and one region). What blocked it was never the identity; it was getting a
  per-vertex UV2 to a rasterizer with no spare buffer binding. See below.

## Known approximation: a model's BOUNCE colour comes from its MeshSource

The bake resolves a receiver's material as `ResolveSubmeshMaterial(override, MeshSource, submesh,
default)` — the same helper the classic path uses. A `ModelComponent`'s draw, though, resolves
through `Model::m_Materials`. The warm `.omesh` path stamps those onto the combined source
(`CreateCombinedMeshSource` calls `SetImportedMaterials`), so the two agree; a cold Assimp import
whose per-mesh `MeshSource`s carry no imported materials falls back to the engine default in the
bake while the frame shades with the imported ones.

The failure mode is bounded and worth stating rather than discovering: the bounce is a **less
saturated colour**, never a wrong atlas address. Threading `Model::m_Materials` into
`ReferenceSceneBuilder` is the fix, and it is a separate change.

## Known cost: a non-uniformly-scaled instance duplicates its geometry in the bake

`ReferenceSceneBuilder::AddMeshEntity` only shares one copy of a mesh between
transforms that are uniformly scaled AND positively oriented — everything else takes the
pre-transform path and gets its own full copy of the vertices, because `ReferenceScene::AddInstance`
cannot represent the rest. That predates #867, but #867 is what makes it reachable at scale:
non-uniform per-instance scale is the ordinary way scatter variety is authored, so a dense
lightmap-static batch can push millions of duplicated triangles into the reference scene before
a single ray is traced.

It is a bake-time memory and build-time cost, not a wrong result — the bake is still correct,
just fatter. Teaching `ReferenceScene` to carry a normal matrix per instance would fix it, and
that is a change to the path tracer's instancing rules rather than to this feature. Until then,
prefer uniform scale on lightmap-static batches, or expect the bake's peak memory to scale with
the instance count rather than with the mesh count.

## The plumbing hop that was missing, and how it hid

`InstanceData::LightmapScaleOffset` existed since #439, `CommandDispatch` already wrote one region
per instance, and `DrawMeshInstancedCommand::lightmapRegionBufferOffset` was already declared — but
`Renderer3D::DrawMeshInstanced` patched back only Colors, Customs and EntityIDs, so the offset
stayed `UINT32_MAX` and every instanced draw resolved to the all-zero sentinel. An author could set
the lane by hand and watch it do nothing.

The generalisable shape: **a per-draw value that survives auto-batching needs a FrameDataBuffer
stream, and a lane that has one end wired at each side and nothing in the middle looks exactly like
a lane that is not wired at all.** Nothing errors; the value is simply the default.

## VirtualGeometry: the UV2 rides the vertex arena, because no binding was available

The cook side is the easy half — `VirtualMeshBuilder`'s attribute set grew from 5 floats to 7 and
its protect window widened to cover the UV2 pair, so a chart seam is a wedge the simplifier may not
collapse across, and the stream rides the versioned blob (`.OVGM` v3).

The hard half was the GPU, and the answer is worth recording because the obvious candidate is a
dead end:

**`SSBO_BONE_PULL` (63) cannot serve this path.** It is the number ADR 0011 amendment (89) reused
for the classic path's UV2, and a virtual mesh can never be skinned, so the mutual-exclusion
argument holds — but `VulkanRendererAPI::AssembleRootData` resolves bindings 57 and 63 from **the
draw's VAO streams**, in an `else if` chain that returns before the published-buffer arm is ever
reached. A `StorageBuffer` published at 63 is silently ignored on Vulkan. Worse, the mesh-shader
route calls `PrepareDrawCommon(nullptr, meshPipeline=true)` — no VAO at all — so 63 resolves to the
frame arena's fixed 64 KiB null block whatever is done to `m_Vao`. Giving the VG VAO a stream-1
buffer would fix the MDI route and never the mesh route.

**A new number does not exist either**: every value 0..79 is claimed and 80 is Mesa's hard ceiling
([ssbo-binding-cap-is-80-on-mesa.md](ssbo-binding-cap-is-80-on-mesa.md)).

So it took the first route `ShaderBindingLayout.h` names when the namespace is out of numbers:
**ride an existing block.** The UV2 is a packed tail region of the cluster vertex arena
(`SSBO_VIRTUAL_VERTICES`, 39), four uv2 pairs to a 32-byte element:

    [ vertices:  SlotCount * SlotVertexCapacity elements ]
    [ uv2 tail:  SlotCount * SlotVertexCapacity / 4 elements ]

That is **+12.5% of the vertex arena, and only when a registered mesh carries UV2**. Widening
`VirtualGpuVertex` to 48 bytes would have needed no binding either, but at +50% of an arena that is
hundreds of MB on a Sponza-scale scene — paid by every VG scene, baked or not. Same trade §1 of
[baked-lightmap-pipeline.md](baked-lightmap-pipeline.md) already rejected for `Vertex`.

### Two invariants make the addressing exact — do not break either

1. `VirtualMeshRegistry::LoadPage` copies a page's vertices to `slotVertexBase`, so **every page
   starts at slot-local index 0**.
2. `m_SlotVertexCapacity` is rounded **up to a multiple of 4**, so `slot * capacity` is 4-aligned.

Together those make a global vertex index decompose into `(element, lane)` with no per-page fixup,
which is what lets the CPU pack a page from 0 and the shader read it back by the same index. The
math lives in `VirtualLightmapUVPacking.h` with a GLSL twin in `include/VirtualDrawInfo.glsl`
(`oloVirtualLightmapUVElement` / `oloVirtualLightmapUVLane`) — change one, change both.
`u_VirtualLightmapUVBase` rides the draw-info UBO's first spare pad word; **0 means "no tail"**.

### The fetch is guarded twice, and both guards are safety

- `u_VirtualLightmapUVBase != 0` — no tail means the element index lands past the arena, and a
  buffer-device-address read has no bounds. That is amendment (89)'s device-loss incident again.
- `LightmapScaleOffset.x > 0.0` — no region on this instance. The cook may **predate the unwrap**:
  a DAG is built when the mesh is first registered, while UV2 only exists after the bake. C++ side,
  `SubmitVirtualMesh` refuses to publish a region unless `MeshHasLightmapUVs(handle)`, and
  `EditorLayer::BakeLightmaps` calls `VirtualMeshRegistry::Invalidate` on every unwrapped mesh so
  the DAG re-cooks with the stream. Without that the registry's `IsRegistered()` fast path would
  serve the stale UV2-less DAG for the process lifetime.

### All three rasterizers sample, and so does the fallback

Virtual geometry reaches the screen three ways — hardware MDI, `VK_EXT_mesh_shader`, and the
compute software rasterizer resolved through `VirtualVisibilityResolve.glsl`. All three write RT5,
so all three sample; the resolve reconstructs UV2 from the triangle's three vertices with the same
perspective-correct barycentrics it already uses for UV0. Miss one and a cluster changes colour as
it crosses the software-raster size threshold — a per-cluster discontinuity, much harder to spot
than a whole surface going unlit.

And `Scene.cpp` passes the same region to the `virtualGeometryEnabled == false` fallback, because
#867 is explicit that baked GI must not appear and disappear with the master switch. The GPU proof
is `LightmapVirtualBleedRoom.VirtualGeometryReceivesBakedGIOnBothSidesOfTheToggle`, which captures
the room with the switch on, with it off, and unbaked, and asserts the switch changes the floor
*less* than the bake does. Wiring only the fallback passes every other contract and fails there.
