# A caster family must be routed into every shadow technique, or it silently casts nothing

Applies to: `OloEngine/src/OloEngine/Renderer/VirtualGeometry/VirtualGeometryShadow.{h,cpp}`,
`OloEngine/src/OloEngine/Renderer/Shadow/VirtualShadowMap.{h,cpp}`,
`OloEngine/src/OloEngine/Renderer/Passes/ShadowRenderPass.cpp`,
`OloEditor/assets/shaders/compute/VirtualClusterCull.comp`

Introduced with issue #1149. Read it before adding a shadow technique, or before
adding a caster family that is not a `MeshComponent`.

The engine has **two directional shadow techniques** (CSM cascades and the
Virtual Shadow Map) and **five caster families** (mesh, skinned, terrain, voxel,
foliage, virtual geometry). A family reaches a technique only if somebody wired
it there. Nothing detects the gap: the frame renders, every other caster keeps
its shadow, and the missing one reads as a lighting or bias problem.

That is how virtual geometry stood from #702 to #1149. Turning VSM on removed
every Nanite mesh's shadow and left every classic mesh's in place — a frame that
looks entirely reasonable unless you already know what should be in it. The
matching hole one layer up, where `ShadowRenderPass` skipped a whole cascade
because its caster-presence check did not count virtual geometry, is the same
mistake and was found the same way (by deleting a ground plane, in the editor).

**So: when you add a technique, enumerate the families. When you add a family,
enumerate the techniques.** `VirtualShadowMapSettings::Enabled`'s comment carries
the current matrix; keep it true.

## 1. Per-page draws are the same cull, gated — not a second cull

The temptation with a page-table shadow map is to write a new culling path for
it. Don't: the DAG cut is the invariant that keeps virtual geometry crack-free,
and a second copy of it drifts.

What the VSM route actually does is run **the existing cluster cull once per clip
level**, with two things changed:

- the level's orthographic VP goes into the shadow camera UBO, so the frustum
  test and the LOD error are the level's;
- `VirtualClusterCullParams::u_VSMPageGate` names the level, and one extra
  rejection runs after the frustum test — a cluster whose page footprint covers
  no **dirty** page is dropped.

That gate is what makes the cost follow the page requests instead of the view
count, and it is why sixteen clip levels are cheaper than four cascades on a
static scene: with nothing dirty, every level rejects every cluster.

The gate is conservative by construction, which is what keeps the cut watertight:
a cluster's cull sphere bounds its geometry, an orthographic projection maps that
sphere to an exact NDC extent, and the fragment stage discards non-dirty pages
anyway. The gate only removes draws the raster would have thrown away.

## 2. Two ways to break it that no test notices unless you write one

**The gate's OFF value must not be zero.** It rides the cluster-cull parameter
block, and that block's documented contract is "value-initialise it and set only
the fields you care about" — the main camera pass, the CSM cascades, the atlas
faces and the parity test all rely on that. A gate whose off value were `0` would
read as "clip level 0" in every one of them and reject their geometry against a
page pyramid that has nothing to do with them. Hence `-1`, and hence
`VirtualGeometryVirtualShadow.TheDirtyPageGateIsOffInAZeroInitialisedCullBlock`.

**An external raster must re-bind the physical pool image after binding its own
program.** `VirtualShadowMap::BindPhysicalPoolImage` forks on whether the program
*currently in flight* is bindless, so it cannot be hoisted out of a shader
switch. It is not enough that the mesh raster bound the pool a moment earlier: in
a scene whose only casters are virtual, the mesh raster returns before binding
anything, so the pool is never bound and every `imageAtomicMin` in the pass is
discarded — a silently unshadowed frame with no error anywhere.

## 3. Sequential views sharing one buffer set need a write-after-read barrier

The classic cascade path gives every view its own command / args / visible
buffers, because its views record in **parallel**. The VSM route draws its clip
levels **sequentially** and shares one set, so the next level's buffer clear and
cull writes race the previous level's indirect draws still reading them.

`glMemoryBarrier(ShaderStorage | Command)` at the end of each level, not just
between the cull and the draw. The failure mode is a level drawing another
level's command counts: flickering shadow pages, not a crash, and not something
the CPU-side tests can see.

## 4. Dynamic casters need bounds the GPU-driven path never kept

A page cache redraws a page only when something dirties it. `VirtualShadowMap`
detects movers by comparing each mesh caster's pose against last frame's — but
virtual instances are not in that list at all, because the cluster cull does its
culling on the GPU and nothing on the CPU ever needed their bounds.

So #1149 added them: a mesh-local AABB computed once at registration from the
union of the cluster cull spheres, transformed per frame for the current **and**
the previous pose. The swept union of the two is what gets invalidated —
invalidating only the new pose leaves the mover's old silhouette baked into a
cached page, which is the page cache's characteristic artefact.

Those same bounds are what lets the CPU drop clip levels no instance reaches, so
an untouched level costs nothing rather than a dispatch per instance.

**Compare the TRANSFORMS, not the derived boxes.** A rotation about the centre of
a symmetric caster — a spinning turret, a 90-degree yaw snap of a crate — leaves
the world AABB bit-identical while changing the silhouette the shadow map holds.
A movement test on the boxes calls that "did not move" and freezes the shadow at
the first angle. `ShadowCasterBounds::Moved` memcmps the two matrices, which is
also what the mesh path does.

**Movement is only one of the three things that dirty a page.** A caster can also
APPEAR or DISAPPEAR, and neither is a transform question:

- a caster that is deleted, or has `CastShadows` unticked, stops appearing in the
  frame list — so nothing compares against it, and its silhouette stays in a
  cached page until something else evicts it;
- a freshly spawned one has `Transform == PrevTransform`, reads as "did not move",
  and lands on pages that are already clean.

Both halves are handled, and the departure half is why `ShadowCasterBounds` carries
a **stable key** (`FrameInstance::CasterKey`, `(entity, part)`) rather than a list
position: the frame list is rebuilt every frame and a submission dropped in the
middle shifts everything after it, so a positional index cannot say "this is the
same caster as last frame". `ShadowRenderPass` keeps the previous frame's
footprints by that key and invalidates the ones that did not come back.

The mesh path had the same departure hole and it is fixed the same way, though
positionally: `SubmitDynamicInvalidations` invalidates the tail it is about to trim
off `m_PrevCasterPoses`. Only the tail needs it — a removal from the middle shifts
every later caster, and those already read as moved.

**What is still open:** changes that alter the drawn cut without touching the
transform or the caster set — editing `ErrorThresholdPixels`, or a streaming page
arriving and refining the cut. Those change the silhouette with no page dirtied.

The test for all this is
`VirtualGeometryVisualEvidence.VirtualMeshCastsThroughTheVirtualShadowMapPages`,
and it deliberately does **not** flush the page table around its cast-flag toggle:
with the departure half missing, both captures show the cached shadow and the
differential silently measures zero. It is the flush's absence that makes it a test.

## 5. Verifying it

A CPU test can pin the gate's default, the block offsets, the shared footprint
helper and the level-reach math — `Rendering/VirtualGeometryVirtualShadowTest.cpp`
does. None of that can tell you the shadow is in the right *place*.

For that, the differential in
`VirtualGeometryVisualEvidence.VirtualMeshCastsThroughTheVirtualShadowMapPages`
captures the same scene and pose on CSM and on VSM, toggling only the virtual
mesh's cast flag, and compares the darkness-weighted **centroid** of the two
resulting shadows. A wrong clip level, a wrong page wrap and a flipped raster
origin all still produce "a shadow"; only its position tells them apart. Pixel
counts alone would pass for a system shadowing the whole floor.
