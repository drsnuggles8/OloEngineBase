# GPU Scene records: one key, one generation, one layout on both sides

**Change a GPU Scene record on the C++ side and the GLSL side in the same commit, address it only by
its key, and advance its generation only for an incompatible edit or a removal.** The rules below
are the contract behind the material, light and environment records (issues #992 and #993, children
of #977; #994 adds the raster consumer). Read this before adding a field to any `GPUScene*` record,
before changing what counts as a compatible edit, and before binding a GPU Scene buffer in a pass.

Code: `Renderer/GPUScene/GPUSceneTypes.h` (records, encoders, identity rules in the header
comment), `GPUScene.h` / `GPUScene.cpp` (the registry), `GPUSceneLightAdapter.h`,
`Renderer3DGPUScene.cpp` (extraction), `HeapBindingSeam.h`, and the GLSL mirror
`include/GPUScene.glsl`.

## 1. Identity: a stable key, a generation that moves only for a reason

The full rule set is the comment at the top of `GPUSceneTypes.h`; it is not repeated here. The
parts that decide behaviour:

- Material key: (owner, slot, source). `Imported` is owned by the mesh source's asset handle, or
  by its vertex-buffer identity for a procedural source; `EntityOverride` is owned by the stable
  entity id and its slot is the override lane, so an entity that carries both a `MaterialComponent`
  and an instanced override keeps two records. A source with neither an asset handle nor GPU
  buffers gets `Unresolvable`, which extracts nothing. Light key: (entity id, light type).
  Environment key: owner, where 0 is the global IBL.
- Compatible edits keep the generation. Incompatible edits advance it in place: a material's
  closure version, alpha mode, PBR flag or any texture identity (`GPUSceneMaterialTextureIdentity`,
  compared by `IsCompatibleGPUSceneMaterialEdit`); an environment texture swap. A light type change
  is a new key. Removal always advances and retires the slot for `RetirementFrameCount` frames.
- A slot whose generation would wrap keeps its last generation on the tombstone and is never
  reused, so a consumer must test the record's `Active` flag as well as its generation.
- An owner-token change (scene reload, backend switch) resets every kind at once and drops every
  resolved heap offset; the next extraction re-resolves.

Pinned by `GPUSceneMaterialRecordPropertyTests.cpp`, `GPUSceneLightRecordPropertyTests.cpp` and
`GPUSceneEnvironmentAndLifecyclePropertyTests.cpp`; the instance and geometry half by
`GPUScenePropertyTests.cpp`.

## 2. Layout parity: two mechanisms

**Update the struct, its `static_assert` lines, the GLSL struct and the pin table in
`GPUSceneLayoutTest.cpp` together.** Mechanism A is the `static_assert` block in `GPUSceneTypes.h`
(size, alignment, standard layout, trivial copyability, the offset of every lane boundary).
Mechanism B is `GPUSceneLayoutTest.ShaderStorageLayoutsMatchCppRecords`: it compiles a kernel that
includes `include/GPUScene.glsl`, reflects it through SPIRV-Cross, and compares member count,
names, offsets, struct size and the std430 runtime-array stride against the C++ `offsetof` table for
every record. A rename on one side fails B; a pad on one side fails A and B.

## 3. The binding namespace is full: bind pass-locally

**Bind a GPU Scene buffer immediately before its consumer, never as global sticky state.** The GL
4.6 SSBO namespace has no free number (`gpu-readback-stats-channel.md` §8), so
`GPUSceneBindingLayout` aliases pass-local slots: instances, geometries and materials take 15, 16
and 17 (the instance-cull trio); lights and environments take 9 and 10 (the Forward+ per-type light
buffers that #994 retires). Buffer growth rebinds and unbinds the aliased slot inside `EndScene`,
which is why every consumer of those numbers already binds per pass.

Inside one shader the alias is a real collision on both backends.
`GPUSceneLayoutTest.NoShaderDeclaresAnAliasedStorageBindingTogetherWithGPUScene` follows every
shader's include closure and fails when a shader that reaches `GPUScene.glsl` also declares a
storage block at one of the aliased numbers. If it fails, split the includes or renumber; do not
delete the test. The layout test's own output block uses 19, a leaf the include does not alias.

## 4. Textures: an RHI handle plus a persistent heap offset, no bindless promise

**A record carries each texture as an RHI handle (index and generation) and a heap offset; a
consumer that reads `GPUSceneHeapOffsetUnresolved` binds through the slot path.** The offset comes
from `HeapBinding::ResolveRecordTextureOffset`, which forks on heap enablement alone because no
consumer is bound at extraction time. It is persistent, so a re-resolve after a reset lands on the
same descriptor; it is skipped entirely when the heap is disabled. Compatibility compares texture
identity, never the offset. Material 2D textures are minted with
`HeapBinding::MaterialTexture2DSampler`, the same state the per-draw material UBO uses, so one
texture never has two descriptors. With the heap enabled, every extracted material texture becomes
resident, not only the drawn ones. #805 (shader-side bindless) is not assumed anywhere.

## 5. Extraction: each record kind has one producer

**Do not add a second extraction site for a kind that already has one.** Materials are visited from
the mesh submission path: `Renderer3D::ResolveGPUSceneMaterialKey` derives the key from the same
`ResolveSubmeshMaterialOrigin` decision that picks the drawn material, and
`Renderer3D::ExtractGPUSceneMaterial` returns early once `GPUScene::IsMaterialStaged` is true, so a
material shared by many submeshes is encoded once per frame. Instances carry `MaterialIndex` and
`MaterialGeneration` of the canonical slot; materials commit before instances. Lights come from
`Scene::ProcessScene3DSharedLogic` through one `MakeGPUSceneLightInput` per component type, every
light regardless of `MultiLightUBO::MAX_LIGHTS`. The environment comes from
`Renderer3D::ExtractGPUSceneEnvironment` at `EndScene`, read from the published global IBL;
no published environment means no record. Every record's comment in `GPUSceneTypes.h` is a field
inventory, carried and excluded; a new field extends that comment.

## 6. The raster light adapter

**Read the authored light once into `GPUSceneLight`, and derive every raster struct from it through
`GPUSceneLightAdapter`.** Scene encodes with a zero render origin, so the positions are still
world-space for the two raster upload sites, which shift them as before; the registry's stored
record is render-relative like instance transforms. `ToMultiLightData` and the three
`ToForwardPlus*` functions return the same bytes the hand-packing produced, including the
`kNoShadowEntry` sentinel; `GPUSceneLightAdapterTest.cpp` pins that per type. The shadow atlas / VSM
layer base is a per-frame allocation, patched into the raster structs afterwards as before. A new
light field goes into `GPUSceneLightInput`, `EncodeGPUSceneLight`, the record, the GLSL mirror and
the adapter, never into the raster packing alone (`light-path-photometric-parity.md`, rule 2).
#994 removes the adapter by reading the SSBO directly.
