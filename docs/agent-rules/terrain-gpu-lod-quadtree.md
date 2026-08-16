# Terrain GPU LOD quadtree — what breaks, and why none of it is loud

Applies to: `OloEngine/src/OloEngine/Terrain/TerrainGPUQuadtree.*`,
`OloEngine/src/OloEngine/Terrain/TerrainQuadtree.*`,
`OloEditor/assets/shaders/compute/Terrain*.comp`,
`OloEditor/assets/shaders/include/TerrainQuadtreeCommon.glsl`,
`OloEditor/assets/shaders/include/TerrainGpuDrivenVertex.glsl`,
`OloEditor/assets/shaders/Terrain_{PBR,GBuffer,Depth}.glsl`

Written from issue #714. Every defect below either shipped for months without a
red test, or would have.

---

## 1. The bug the rewrite found first: a feature with no scene, and therefore no coverage

`TerrainComponent::m_TessellationEnabled` gates the entire quadtree LOD path, and
**not one scene under `OloEditor/SandboxProject/Assets/Scenes/` set it to true**.
`AssetSceneLoadTest` loads every scene, `TerrainCameraRelativeVisualEvidenceTest`
renders terrain — and the quadtree still had no runtime coverage at all, because
the flag was false everywhere the renderer could see it.

What that hid: `TerrainChunkManager::FindChunkForNode` maps a selected quadtree
node to **the one chunk at the node's centre**. That is correct only while every
selected node is a leaf, because the tree's depth is chosen so leaf == chunk. The
moment the descent selects a node one level up — which it does as soon as the
node's screen-space error drops below `TargetTriangleSize` — the node covers 2×2
chunks and **three of them are never drawn**. Two levels up, fifteen of sixteen.

From a camera looking down, the terrain was mostly missing. Nothing failed.

Two rules out of this:

- **A gating flag that no scene sets is a feature with zero coverage**, whatever
  the unit tests say. Before trusting "the suite is green" for a subsystem, grep
  the scenes for the flag that turns it on: `grep -rn "TessellationEnabled: true"
  OloEditor/SandboxProject/Assets/Scenes/`. `TerrainGpuLodTest.olo` exists to be
  that scene; keep it enabled.
- **Do not fix this by making `FindChunkForNode` smarter.** A node is not a
  chunk and never was — the mapping is the bug. The GPU path draws the node
  itself (one instance of a shared unit grid over the node's rect), so there is
  nothing to map.

---

## 2. Crack-freedom is a property of the vertex SET, not of a tessellation factor

Two adjacent terrain patches crack iff their shared edge is sampled at different
points, because Y comes from the heightmap at the interpolated UV. So the
contract is: **the two edges must produce the same set of positions**, exactly.

The edge-snapping scheme (`oloTerrainSnapEdgeIndex`) gets there by dropping the
low `delta` bits of the grid index on the finer side, where
`delta = max(0, myLevel - neighbourLevel)`. It works because of three facts that
are easy to break independently:

- **`kPatchGridResolution` must be a power of two.** Snapping to a multiple of
  `2^delta` only lands on the coarse neighbour's vertices if the edge length is
  one too. A `static_assert` pins it.
- **`delta` must be clamped to `log2(K)`.** A larger jump would collapse the edge
  to a single vertex. `TerrainSeamMap.comp` clamps; `kMaxSeamDelta` is the twin.
- **Corners must be fixed points.** Index 0 and index K are multiples of every
  `2^delta` up to K, so no delta can move them — which is what welds a patch's
  four corners regardless of the seam pattern. The snapping code runs the +X/−X
  test before the +Z/−Z one and a corner survives both; if you reorder or
  restructure those branches, re-derive that.

**Only the finer side decimates.** The coarse side sees `max(0, coarse - fine) = 0`
and does nothing. If both sides ever snapped, they would decimate to different
grids and the seam would open.

`TerrainGPUQuadtreeTest.AdjacentPatchEdgesShareExactlyTheSameVertexPositions`
enumerates every boundary in a selected set and compares the two edges as sets,
with **exact** equality — both sides are integer multiples of the same dyadic
step, so a tolerance would only hide a real crack.

**The anti-vacuous guard on that test is not decoration — it fired on the first
run.** Every camera pose used the shipped `TargetTriangleSize` of 8 px, at which
a 512-unit terrain splits to leaves from any distance a camera actually sits at,
so every boundary joined two nodes at the *same* level and the set equality was
trivially true. The test reported a green crack check while proving nothing. It
now sweeps `{8, 120, 600, 2400}` px alongside the camera poses, and asserts that
at least one boundary joined two DIFFERENT levels.

Generalise this: a test over "adjacent things agree" needs a second assertion
that the *interesting* adjacency actually occurred. Without it, the first
configuration change that flattens the input turns the test into a no-op and
nothing goes red.

---

## 3. Sampling one texel per edge is exact — do not "improve" it into an average

`TerrainSeamMap.comp` reads the LOD map at a single point just outside each edge,
at the edge midpoint. That looks like an approximation and is not.

Along a node's +X edge the neighbourhood is either **one node at level ≤ mine**
(a coarser node is at least twice as wide, so it spans the whole edge and every
texel there reports the same level), or **several nodes at a finer level** — for
which the delta is 0 anyway. A coarse/fine mix along a single edge is
geometrically impossible in a quadtree. Replacing the point sample with a min or
a max over the edge would produce the same answer at more cost, and a *mean*
would produce a wrong one.

---

## 4. A frustum-culled node is deliberately not marked as split

`TerrainNodeSelect.comp` returns early on a cull without touching the split map,
so `TerrainLODMap.comp` reports that whole subtree at the culled ancestor's
level. That is not a bug and it is not a compromise:

- the culled node **draws nothing**, so the visible node next to it has no
  neighbour to crack against;
- the visible side may over-decimate its edge toward a coarser level than the
  neighbour would really have used, which costs a few vertices at the screen
  border and nothing else.

The alternative — descending through culled nodes so their split decisions are
recorded — would defeat the culling entirely. If you ever see edge artefacts at
the exact frustum boundary, this is the place to look, but the fix is not "stop
culling".

---

## 5. Do not stack hardware tessellation on top of geometric LOD

The GPU path sets `TessFactors = vec4(1.0)` so the tessellator passes the base
grid through untouched, and the LOD comes entirely from which rect a node covers.
That is deliberate.

The pre-#714 path expressed LOD *only* as a tessellation factor over a uniform
chunk grid, where `min(myTess, neighbourTess)` is a correct seam rule because
every chunk is the same size. Once patches have **different world extents**, the
rule that makes edges line up is equal *world-space vertex density*, i.e.
`outerTess / edgeLength` must match — not `min`. Re-enabling tessellation on top
of the node grid brings that problem back on top of the snapping that already
solved it. If you want more triangles, lower `TargetTriangleSize` (more, smaller
nodes) or raise `kPatchGridResolution`.

The whole VS → TCS → TES → FS pipeline is still used, at tess level 1, purely so
the ~600-line terrain fragment stage is not duplicated into a second shader.

---

## 6. `HasDispatched()` must not latch

The submission side picks the GPU branch from
`TerrainGPUQuadtree::HasDispatched()`. If that were a "has ever dispatched" latch,
one successful GPU frame would pin the branch forever, and the CPU fallback —
including the `OLO_TERRAIN_CPU_LOD=1` A/B lever, which is the first thing anyone
reaches for when terrain looks wrong — would keep re-drawing the last GPU node
list. `TerrainChunkManager::SelectVisibleChunks` therefore calls
`ClearDispatched()`: whichever path ran last owns the frame.

Same shape as any "which producer filled this buffer" flag. If you add a third
path, it clears the other two.

---

## 7. Indirect compute dispatch did not exist in the RHI before this

`RendererAPI::DispatchComputeIndirect` is new (GL `glDispatchComputeIndirect`,
Vulkan `vkCmdDispatchIndirect`). Two things it needs that are easy to omit:

- **A `MemoryBarrierFlags::Command` barrier between the kernel that writes the
  arguments and the dispatch that reads them.** Without it the group count is
  whatever was in the buffer before — usually the previous frame's, so the
  symptom is a terrain that is one frame stale rather than a crash.
- **The argument buffer must carry the indirect usage bit on Vulkan.** The
  implementation routes through the same `ResolveIndirectBuffer` the indirect
  draws use, which is what checks it.

`DrawBoundElementsIndirect` also gained an explicit `topology` parameter rather
than a default argument — a default on a virtual is resolved from the *static*
type, so an override could silently disagree with the base declaration.

---

## 8. The node pyramid is shared on purpose, and that is what makes parity testable

`TerrainQuadtree::BuildHeightPyramid` produces the level-major min/max height
array that the CPU node bounds AND the GPU descent both read. Combined with
CPU-side precomputation of the frustum planes and `projScale`, the GLSL descent
is a *transcription* of `TerrainQuadtree::SelectNode`, so
`TerrainGPUQuadtreeTest.GpuDescentSelectsTheSameNodesAsTheCpuQuadtree` can assert
an identical selected-node set rather than a fuzzy overlap.

Keep it that way. The moment one side gets a term the other lacks — a deviation
metric, a different distance measure, a normalized error — that test starts
failing for a reason that is not a bug, and the usual response (loosen it) throws
away the only check that covers the descent.

Two consequences worth knowing:

- The pyramid replaced a per-node strided resample that stepped every
  `extent / 16` texels plus the four corners. That was *not* conservative: a
  spike between samples produced an AABB that clipped its own terrain. The
  pyramid is exact and costs one pass over the heightmap for the whole tree.
- The GPU tree is **not** clamped to `TerrainLODConfig::MAX_LOD_LEVELS`. That cap
  exists because the CPU descent is a per-frame single-threaded walk; it has
  nothing to say about a GPU worklist. Production depth therefore *can* exceed
  the CPU tree's, which is the point — but it also means the parity test only
  holds at equal depth, which is why it builds both sides itself.

---

## 9. Overflow is reported, not asserted — and the report costs a stall

The per-level node list and the visible list are fixed-capacity. The select
kernel bumps its counter before it knows whether the slot fits, so the raw
counter can exceed capacity; **every consumer reads the value
`TerrainCullArgs.comp` clamped**, and the producer's overflow bit survives for
the CPU.

Reading that bit is a `GetData` — a GPU→CPU sync, i.e. exactly the stall this
whole class exists to remove. `PollOverflow()` therefore reads at most once every
240 frames and warns once. If you need it every frame while debugging, say so in
the code rather than lowering the interval permanently.

Note also the per-child bound check in the select kernel: a partially-fitting
group of four still fills the slots below the cap, so the clamped `PendingCount`
never addresses a slot left over from an earlier level or frame.

---

## 10. Populating a `TerrainData` creates a GPU texture — so an L1 test must not

`TerrainData::CreateFlat` / `GenerateProcedural` / `SetHeights` all end in
`UploadToGPU()` → `Texture2D::Create` → `glCreateTextures`. With no GL context
that is a call through a null glad pointer: an access violation, reported by
gtest only as `SEH exception with code 0xc0000005`, with an empty stack trace.

That makes any L1 ("pure CPU, no GL context") test that builds a `TerrainData`
**order-dependent**: it passes when some earlier suite in the same process
happened to bring a context up, and faults when run alone under
`--gtest_filter`. `TerrainGPUQuadtreeTest` hit exactly this — 12 of 13 green in
a combined run, 5 hard faults in isolation.

The fix was to remove the dependency rather than to depend on the ordering:

- `TerrainQuadtree::BuildHeightPyramid` gained a `std::span<const f32>` overload
  (the `TerrainData` one now forwards to it), and
- `TerrainQuadtree::BuildFromPyramid` exposes what `Build()` was already doing
  once it had a pyramid.

The tree never needed the asset for anything but those heights, so this is a
better factoring anyway — and the selection-math tests now run on a plain array.

**`TerrainCameraRelativeLODTest` still has the latent version of this** (it calls
`CreateFlat(129, …)`); it survives today only because of where it happens to sit
in the run order. If it ever starts faulting in isolation, this section is why,
and `BuildFromPyramid` is the fix.

Recognising it: the tell is a `0xc0000005` with **no** gtest stack, in a test
that does no pointer work of its own. Get the real frame with the WinDbg-store
`cdb`, breaking on the first-chance AV rather than letting gtest swallow it:

```powershell
& "<pkg>\amd64\cdb.exe" -y "srv*;<test dir>" `
  -c "sxe -c `"kn 22; q`" av; g" OloEngine-Tests.exe --gtest_filter=Suite.Case
```

`-g` defeats this (it runs past the exception into `exit`); leave it off.

---

## 11. Shadow casters stayed on the chunk meshes

The GPU visible list is culled against the **camera** frustum. Replaying it per
shadow cascade would drop every caster that is off-screen but still shadowing.
So the GPU path emits shadow casters from `GetVisibleChunks(...)` exactly as the
non-tessellated path always has, and the chunk meshes are still built.

That is a scope line, not a design position: a correct fix is a second GPU
descent per cascade with the light's frustum. Until then, do not "simplify" by
pointing the shadow pass at the camera's node list.
