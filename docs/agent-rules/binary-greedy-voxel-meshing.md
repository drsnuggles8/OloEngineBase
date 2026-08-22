# Binary greedy meshing + packed quads

Issue #727. The cubic voxel mesher that runs alongside marching cubes:
`Terrain/Voxel/VoxelGreedyMesher.{h,cpp}` (the merge), `VoxelQuad.h` (the encoding),
`VoxelGreedyMeshBuilder.{h,cpp}` (async rebuild + GPU upload), and the three
`Terrain_VoxelGreedy*.glsl` shaders that rebuild the quads on the GPU.

Read this before touching any of them. Every item below is a way to get a frame that looks
plausible and is wrong, or a change that compiles and silently drops geometry.

---

## 1. The encoding is a contract with the GPU, mirrored in a second file

`OloEngine/src/OloEngine/Terrain/Voxel/VoxelQuad.h` and
`OloEditor/assets/shaders/include/VoxelQuadUnpack.glsl` are the same contract written twice:
bit layout, face numbering, and the per-face U/V basis. Nothing links them. Change one and the
other keeps compiling, every CPU test keeps passing, and the frame comes out transposed, offset,
or inside-out.

The bit layout is derived from `VoxelChunk::CHUNK_SIZE` in the C++ header and **hard-coded** in
the GLSL, because GLSL has no `std::bit_width`. The C++ side carries a `static_assert` that fires
if the derived layout stops fitting in 32 bits — that assert is the only thing that will tell you
the two have parted company, and it only fires on a chunk-size change, not on a hand edit.

**Extents are stored biased by one.** The reference implementation this was ported from
(`D:\repos\VoxelEngine`, `voxel_mesher.tpp::_CompressQuadData`) masks width/height with `0x1F`
unbiased, which is why it documents "cannot express 32³ chunks": at a 32-voxel chunk a full-span
run encodes as 0 and the quad vanishes. `VoxelQuadEncoding.FullSpanExtentSurvivesTheRoundTrip`
exists specifically to fail if someone "simplifies" the bias away. Our chunk **is** 32, so the
quads that would disappear are the biggest ones — a flat chunk face is the best case greedy
meshing has.

## 2. Swapping a face's U and V does not lose a quad — it inverts it

`VoxelFaceBasis` orders each face's U and V so `cross(U, V) == normal`. Combined with the shared
unit quad's fixed `{0,1,2, 2,3,0}` index order, that is what makes all six directions front-face
outward under `GL_CCW`.

Swap one row and that single direction renders inside-out. With backface culling on, the symptom
is a hole you can only see from specific angles — the classic "looks fine until you fly around
it". `VoxelFaceBasisContract.CrossOfUAndVIsTheOutwardNormal` pins the C++ half;
`VoxelGreedyMeshVisualEvidenceTest` shoots four angles including one from *below* precisely
because five of the six directions can be right.

Which plane extent becomes `Width` and which becomes `Height` follows from the same table and is
**not** uniform across the six faces (`kWidthIsInnerExtent` in `VoxelGreedyMesher.cpp`). Getting
it wrong transposes the quad: a 1×8 strip renders as 8×1, which still merges, still passes a
count comparison, and still draws *something*.

## 2b. An exact axis-aligned normal breaks two idioms copied from the other terrain shaders

Cubic voxel normals are *exactly* `(±1,0,0)`, `(0,±1,0)`, `(0,0,±1)`. Two patterns that are safe
on a marching-cubes isosurface — where an exactly-axis-aligned normal is measure-zero — are
common-case bugs here:

1. **`normalize(cross(N, vec3(0,0,1)))` is NaN for every ±Z face**, and that is a third of the
   mesh. The usual rescue, `if (length(T) < 0.001) T = cross(N, vec3(1,0,0));`, **cannot fire**:
   every comparison against NaN is false. Choose the reference axis *before* crossing
   (`abs(N.z) < 0.99 ? +Z : +X`) instead of crossing and testing the result.
2. **Decoding a normal-map texel before checking whether the array is bound.** An unbound
   sampler reads solid black, and `black * 2 - 1` is `(-1,-1,-1)` — unit length, and therefore
   indistinguishable from a real perturbation. It rotates the exact face normal ~55° off true, so
   most faces point away from the light and the whole mesh renders black while geometry,
   materials and lighting are all provably fine. Blend in 0..1 texel space and decode after.

Both were found by *looking at a PNG*, not by a test: (2) rendered a black silhouette that every
CPU test passed through, and (1) is invisible in any scene that binds no terrain normal array —
which includes the visual test. If you add a normal-map-bound cubic scene, that is the case to
shoot.

## 3. Padding is uniform on all three axes here — deliberately

The reference pads X and Z in the array dimensions and special-cases ±Y against the neighbour
chunk's edge bit, because its column type is a `uint32_t` sized to its 32-voxel chunk and there is
no room for the two Y padding bits.

`VoxelNeighbourhood` uses **u64 columns for a 34-bit padded span**, so ±Y is not special and all
six directions run identical code. Keep it that way. The moment one axis gets its own path, the
chunk-boundary cases become a separate bug surface, and boundary bugs are the ones that render as
a plausible world with a seam you have to go looking for.

`VoxelGreedyMesherBoundary.EveryAxisCullsAgainstItsNeighbour` is the test that would catch a
regression here; `PartialNeighbourCullsOnlyTheCoveredFaces` is the one that catches an
all-or-nothing padding bug, which the fully-solid cases pass.

Edge and corner padded cells are never written and never read — a face of a centre voxel only ever
queries one step along **one** axis, which is always a face-neighbour cell. If you find yourself
gathering 26 neighbours, you have misread the algorithm.

## 4. Editing a chunk invalidates its six neighbours' meshes, not just its own

Boundary faces are culled against the neighbour, so a carve in chunk C changes what C's neighbours
draw. `VoxelGreedyMeshBuilder::DispatchDirty` rebuilds the ring around the dirty set **without**
setting those neighbours' `Dirty` flags — setting the flag would cascade the ring outward every
frame until the whole volume rebuilt.

Skipping the ring does not crash and does not fail a test: you get a stale wall standing in the
air where two chunks meet, which reads as a lighting artefact until you fly through it.

## 5. The mesher is a pure function of an immutable snapshot — that is the thread-safety design

`VoxelGreedyMesher::Gather` runs on the thread that owns the `VoxelOverride` and produces a
self-contained `VoxelNeighbourhood`. Everything downstream is a pure function of that blob, which
is what makes it safe to hand to a worker while the game thread keeps carving.

Do not "optimise" the gather onto the worker by capturing the `VoxelOverride` — a concurrent
`CarveSphere` then races the mesher over live `std::vector<f32>` data, and the failure is a torn
read that produces a *plausible* chunk.

The game thread keeps two jobs and only two: the gather, and the GPU upload of a finished quad
buffer (`VertexArray`/`VertexBuffer` creation is not thread-safe and must not move).

**Where that upload happens is load-bearing.** `OpenGLVertexArray::AddInstanceBuffer` is one of the
few remaining non-DSA paths — it calls `glBindVertexArray` and leaves a VAO plus a
`GL_ARRAY_BUFFER` bound. That is harmless *only* because uploads run during
`Scene::ProcessScene3DSharedLogic`, i.e. before `RenderPipeline::PrepareFrame` zeroes
`CommandDispatch`'s `CurrentBoundVAO` and long before any bucket executes. Move an upload so it
interleaves with bucket execution and the dispatcher's "this VAO is already bound" cache goes
stale against real GL state, and draws start reading whichever VAO the upload left behind. Foliage
sits on the same invariant for the same reason.

## 6. `gl_InstanceIndex` is the QUAD index, not an InstanceData index

The three greedy shaders define `OLO_INSTANCE_SINGLE` before including
`InstanceBlock_Vertex.glsl`, exactly like `Foliage_Instance.glsl`, because the chunk uploads
**one** `InstanceData` entry (its terrain transform × chunk placement) while `gl_InstanceIndex`
counts merged quads.

Without it, `instances[gl_InstanceIndex]` reads `224 × quadIndex` bytes into a 224-byte upload.
Under GL that clamps to garbage and the chunk collapses; under Vulkan it is a buffer-device-address
page fault that loses the device (the #691 foliage `VK_ERROR_DEVICE_LOST`).

## 7. The chunk origin and voxel size ride the model matrix

Quads are in **chunk-local voxel units** — that is what lets the whole record fit in 32 bits. The
world placement is `terrainTransform × translate(chunkOrigin) × scale(voxelSize)`, built in
`VoxelGreedyMeshBuilder::UploadMesh` and combined at the draw site in `Scene.cpp`.

Applying the chunk transform twice, or baking the origin into the quad *and* the matrix, still
renders a complete, correctly-shaded, correctly-lit world — somewhere else. The visual test's
coverage-overlap check against the marching-cubes render is what notices.

## 8. The shadow silhouette must be rebuilt by the same code as the lit one

`Terrain_VoxelGreedyDepth.glsl` unpacks through the same `VoxelQuadUnpack.glsl` include as the lit
and G-Buffer shaders. If the depth stage ever grows its own copy of the unpack, the two drift and
the symptom is peter-panning or a shadow cast by geometry that is not on screen.

`ShadowVoxelCaster::instanceCount` is what selects between the two depth shaders. Casters from
both meshers land in one list, so the pass binds lazily and only when the shader changes — do not
hoist a single `Bind()` back out of that loop.

## 9. In cubic mode the voxels ARE the terrain

Two behaviours are gated on `m_VoxelMesher == GreedyCubic` and both are load-bearing:

- The voxel volume is **seeded from the height field** (`VoxelOverride::SeedFromHeightmap`). The
  marching-cubes path deliberately leaves the volume sparse — there it is a carve-only override on
  top of a heightmap terrain, and filling it would duplicate the surface as an isosurface.
- The heightmap surface itself **stops drawing** (`heightfieldSurfaceHidden` in `Scene.cpp`).
  Without that the smooth surface and the cubes occupy the same space and z-fight along every
  slope.

Both are one-line conditions and both are invisible in a headless test. If a cubic scene renders
with shimmering slopes, check the second one first.

## 10. Materials: the empty array is a fast path, not an absence

`VoxelChunk::MaterialData` empty means "material 0 everywhere", and the mesher then skips every
material comparison and merges purely bitwise. That fast path is a second implementation of the
merge, so
`VoxelGreedyMesher.UniformMaterialTakesTheSameResultAsAnExplicitZeroArray` compares it against an
explicitly-zeroed array. Keep that test if you touch the merge — without it, the common path is
the one nothing checks.

A merge that ignores materials does not lose or duplicate a face. It produces a *correct* face set
with the wrong material on part of it, so the face-set equality test passes and the world just
looks banded wrong.

## 11. Comparing quad counts is not comparing face sets

`VoxelGreedyMesherFaceSet.GreedyFaceSetMatchesNaive` expands every merged quad back into unit
faces and compares the **multiset** against an independently written per-voxel mesher, in both
directions, plus a per-key count check.

A size comparison passes on exactly the bug the test exists to catch: one face emitted twice and
another dropped. `MeshNaive` deliberately shares no code with `Mesh` for the same reason — if it
reused the bitwise face culling, a bug in that culling would be invisible to the comparison.

---

## Cross-references

- [gpu-scan-compaction.md](gpu-scan-compaction.md) — the same "a set comparison passes on the bug"
  archetype, in a different subsystem.
- [terrain-gpu-lod-quadtree.md](terrain-gpu-lod-quadtree.md) — the neighbouring terrain path, and
  the "a feature no scene enables has zero coverage" lesson that produced
  `SandboxProject/Assets/Scenes/VoxelGreedyMeshTest.olo`.
- [single-mesh-visual-test-lighting.md](single-mesh-visual-test-lighting.md) — why the visual test
  needs a lit ground, which here is the voxel terrain itself.
- [rhi-abstraction-boundary.md](rhi-abstraction-boundary.md) §9–§13 — the Vulkan two-stream vertex
  pull contract the greedy shaders sit on.
