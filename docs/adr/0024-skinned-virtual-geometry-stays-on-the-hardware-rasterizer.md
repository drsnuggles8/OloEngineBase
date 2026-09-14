# Skinned virtual geometry stays on the hardware rasterizer

Issue [#1150](https://github.com/drsnuggles8/OloEngineBase/issues/1150) lifted
`VirtualMeshBuilder`'s build-time rejection of skinned sources, so a character can now be cooked
into a cluster LOD DAG and drawn through the virtualized path. That issue asks for one decision to
be made and **written down** rather than left implicit: what happens to the **software rasterizer**
for skinned clusters.

**Decision: a skinned cluster is never routed to the compute software rasterizer. It always takes
a hardware path — the MDI arm or the mesh-shader arm.** `VirtualClusterCull.comp` excludes it in
`EmitSurvivor`, beside the existing exclusion for alpha-masked materials.

## Why

The virtualized path has **four** places that turn a cooked vertex into a position on screen:

| Consumer | What it does with a vertex |
|---|---|
| `VirtualMeshGBuffer.glsl` (MDI) | vertex stage transforms it |
| `VirtualMeshletGBuffer.glsl` (mesh shader) | mesh stage transforms it |
| `VirtualClusterRaster.comp` (software raster) | compute fetches and transforms it to find coverage |
| `VirtualVisibilityResolve.glsl` | fetches the triangle's three vertices **again** and interpolates attributes from them |

The first two already share one spelling — `TransformVirtualVertex` in
`include/VirtualGBufferVertexStage.glsl` — and the mesh-vs-MDI parity contract
(`VulkanPassSuite.VirtualGeometryMeshTasksMatchTheMdiPath`, zero differing pixels) rests on that.
Posing a vertex is now part of that shared spelling: `SkinVirtualVertex` in
`include/VirtualSkinnedVertexFetch.glsl`, which the two shadow depth stages include as well, so
"the shadow rasterizes the pose the G-Buffer drew" is structural rather than a thing to keep
checking.

The last two are different code with different inputs, and — this is the part that decides it —
**they must agree with each other, not merely with the hardware arms.** The software rasterizer
resolves depth with an atomic min over a packed `uint64` and writes only a cluster id and a
triangle id; the resolve pass later re-fetches that triangle and reconstructs its world position,
normal and UVs from the vertices. If the two poses disagree at all, the fragment that won the depth
race is shaded from a surface that is somewhere else — wrong normal, wrong UV, wrong lighting, at a
depth that came from a third place. That is not a visible artefact with an obvious cause; it is a
character that shades subtly wrong only where the software rasterizer happened to take clusters.

So the cost of the software arm is not "write the skin twice". It is **two more independent skin
implementations that must produce bit-identical results**, in two shaders that have no parity test
between them, to buy the fast raster path for clusters under the 24-pixel routing threshold — on
character-sized meshes, which are the *least* likely content in a scene to be dominated by
sub-pixel clusters. Nanite's own software raster exists because a Nanite scene is mostly distant,
tiny, static clusters; a skinned character is neither distant nor static when it is worth drawing
at all.

## What this costs, concretely

A skinned instance's small clusters take the hardware MDI path instead of the compute rasterizer.
That is a routing change, not a loss: `EmitSurvivor` already falls through to the hardware
compaction path whenever the software route is unavailable (the software work list being over
capacity takes exactly this fallthrough today), so the geometry draws either way.

`VirtualDrawInfoGpu::SkinningBase` is still uploaded by the resolve pass even though the resolve
never poses anything. The block is uploaded whole by every pass that binds it, and a field that
means one thing in one pipeline and something else two pipelines away is the exact failure
`include/VirtualDrawInfo.glsl` was written to make impossible.

## What would change the decision

A parity test between `VirtualClusterRaster.comp` and `VirtualVisibilityResolve.glsl` — one that
fails when their reconstructed positions diverge — would remove the argument above, because the
thing that makes the software arm expensive is that its two halves can silently disagree. If that
test exists and a measured frame shows skinned clusters spending real time in the hardware arm,
revisit this.

## Consequences

- `VirtualInstanceGpuRecord::kFlagSkinned` gates the software-raster routing in
  `VirtualClusterCull.comp`, alongside `kFlagAlphaMasked`.
- `VirtualClusterRaster.comp` and `VirtualVisibilityResolve.glsl` need no skinning code, and the
  fact that they contain none is a decision recorded here rather than an omission.
- The `ForceSoftware` software-raster mode (`VirtualSwRasterMode::ForceSoftware`, used by the
  SW-vs-HW parity test) does **not** force a skinned cluster into the software path. A parity test
  run on a skinned mesh therefore compares the hardware path with itself, which is vacuous — so
  that test keeps using rigid fixtures, and this sentence exists so the next person to point it at
  a character knows why the result is meaningless.
