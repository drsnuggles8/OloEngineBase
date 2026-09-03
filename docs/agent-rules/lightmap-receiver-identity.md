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
  therefore one unwrap and one region). See below for what actually blocks it.

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

## VirtualGeometry is NOT wired, on purpose — and neither is its fallback

The cook-side half is done and tested: `MeshSource` UV2 now survives cluster building and the LOD
simplifier's boundary locks (`VirtualMeshBuilder`'s attribute set grew from 5 floats to 7 and its
protect window widened to cover the UV2 pair, so a chart seam is a wedge the simplifier may not
collapse across), it rides the versioned blob (`.OVGM` v3), and `PackVirtualMeshForGpu` expands it
as a parallel stream. `kVirtualMeshBuilderVersion` moved to 3 accordingly.

What remains is a **GPU buffer binding**, and it is an issue-level decision rather than a task:

- The cluster vertex is `VirtualGpuVertex`, 32 bytes with both `.w` lanes spent on UV0, so UV2
  needs a second per-vertex buffer.
- `ShaderBindingLayout.h` states the SSBO namespace is FULL — every value below the hard Mesa
  ceiling of 80 is claimed, and the header asks for a new claim to be raised in an issue first.
  The one reusable slot, `SSBO_BONE_PULL` (63), is the Vulkan vertex-pull stream; a virtual mesh
  can never be skinned (the builder rejects skinned sources), so the reuse argument #866 made for
  the classic path applies — but it needs a Vulkan RHI change and an ADR amendment.

**Do not wire only the classic fallback in the meantime.** When
`RendererSettings::VirtualGeometryEnabled` is off, `Scene.cpp` re-routes the same `MeshSource`
through `SubmitMeshSourceClassic` — the very renderer that *does* support lightmaps. Passing a
region only there would make baked GI appear and disappear with the master switch, destroying that
toggle's whole purpose as a clean A/B ("same geometry, same material-resolution rule, the only
difference is the renderer"). Either both sides sample the lightmap or neither does, so for now
`GatherLightmapReceivers` skips `VirtualMeshComponent` entirely — baking regions nothing reads
would spend atlas space and raise page pressure for a frame that looks identical.
