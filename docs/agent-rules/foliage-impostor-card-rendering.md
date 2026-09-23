# Octahedral foliage impostor cards — three silent-failure traps (issue #433)

Bringing up the runtime octahedral impostor card (`Foliage_Impostor.glsl`, the
camera-facing quad that samples the baked atlas) hit three bugs that all pass
every CPU/contract test, compile cleanly, and produce a *plausible* frame —
they only surface when you render a scattered field and **look at the pixels
from more than one angle**. Each one made "most of the impostors invisible"
look like a different problem.

## 1. The foliage InstanceData SSBO holds ONE entry — don't transform a per-instance position through `u_Model`

The foliage command path (`CommandDispatch::DrawFoliageLayer`) uploads a
**single** `ModelInstanceData` entry to the binding-15 `InstanceData[]` SSBO
(the shared whole-layer terrain transform), then issues
`glDrawElementsInstanced` with N instances. A foliage vertex shader that reads
`u_Model` (= `instances[gl_InstanceIndex].Transform`) for `gl_InstanceIndex > 0`
therefore reads **out of bounds**; on the dev NVIDIA driver that returns a
**zero matrix**, so `instWorld = (u_Model * vec4(a_PositionScale.xyz, 1.0)).xyz`
collapses **every** instance's card to the world origin. Symptom: instead of a
scattered field you get one dense clump of overlapping cards at the terrain
corner (world 0,0), invisible from most camera angles — looks exactly like
"scatter is broken" or "frustum culling ate them".

Per-instance foliage data lives in the **vertex attributes**
(`a_PositionScale` / `a_RotationHeight` / `a_ColorAlpha`, instance divisor 1),
**not** the SSBO. Build the card position straight from `a_PositionScale.xyz`
and shift to render-relative space by subtracting `u_RenderOrigin` directly
(issue #429), never by multiplying through `u_Model`:

```glsl
// WRONG — collapses to origin for instance > 0 (single-entry SSBO OOB read)
vec3 instWorld = (u_Model * vec4(a_PositionScale.xyz, 1.0)).xyz;
// RIGHT
vec3 instWorld = a_PositionScale.xyz - u_RenderOrigin;
```

Diagnostic that nails it in one run: force the fragment to a solid debug colour
with no alpha discard **and** flip the impostor render state's
`depthTestEnabled` to false. If every card is still stacked in one place with
depth off, the position is collapsed (this bug); if they spread out once depth
is off, it's an occlusion/placement problem (trap #3), not position.

## 2. Virtual-plane UV divides by the WORLD card radius, not the object atlas radius

The atlas is baked framing the mesh's **object-space** bounding sphere
(`ImpostorBaker` stores `Radius` in object units, e.g. 1.64). The runtime card
is scaled by the per-instance `scale`, so its half-size in **world** units is
`objectRadius * scale`. The virtual-plane reprojection that computes each
frame's UV works in world units (`offVec` is a world-space vector), so it must
divide by `2 * worldRadius`. Dividing by the object radius makes `uvFrame` blow
up (~5×), `clamp(uvFrame, 0, 1)` pins every sample to the tile border
(transparent), and the alpha test discards the whole card — except at grazing
angles where `offVec` happens to be tiny, so a few distant edge-on cards survive
and everything head-on vanishes. Pass the world radius (`objectRadius * scale`,
computed in the vertex) to the fragment as a varying and divide by that.

Also get the ray/plane intersection sign right: the in-plane offset from the
pivot is `pivotToCam - offLen * vertexToCam` (derive it — `X = camera -
offLen*vertexToCam`, offset `= X - pivot`), not the negation.

## 3. Anchor the card so the bounding sphere RESTS ON the ground — never trust the mesh's baked centre.y

A camera-facing card centred at the mesh's baked bounding-sphere centre is
correct only if the mesh is authored base-at-origin. Many meshes (e.g.
`DamagedHelmet`) are centred on the origin (`centre.y ≈ 0`), so the card centres
at ground level and sinks half below the terrain. When the camera looks **down**
at it (any top-down-ish gameplay pitch) the camera-facing card tilts toward
horizontal and its lower half lies inside the terrain, so it depth-culls to
nothing — only cards viewed edge-on at the horizon poke out. Anchor by the
world radius instead, which rests the sphere on the ground regardless of how the
source mesh is authored:

```glsl
vec3 cardCenter = instWorld + vec3(0.0, radius /*world*/, 0.0);
```

## 4. A layer draws TWO shapes now — read `EnumerateLayerDraws`, not the shader

Until issue #1233 a `MeshPath` never reached a draw call: it fed
`UpdateImpostorAtlas` and nothing else, so what
`FoliageImpostorSampling.glsl` cross-faded across `ImpostorStartDistance` was
one flat atlas frame against the full parallax impostor — the same card on both
sides. **That is no longer true.** A layer with `UseAuthoredMesh` and a loadable
`MeshPath` emits its real geometry up close *and* its card beyond, over one
instance stream, and `FoliageRenderer::EnumerateLayerDraws` is the single place
that decides which draws a layer contributes.

Two things follow, and both are easy to get wrong from a shader alone:

- **Never read a pass's own shader to learn what a layer draws.** The beauty,
  G-Buffer and shadow programs all walk the same list; a change made in one of
  them is a desync, not a fix. Placement lives in
  `include/FoliageInstanceGeometry.glsl` and is called from every foliage vertex
  stage, so a stage that computes its own scaling has already forked.
- **The mesh and the card scale differently on purpose.** The card is
  anisotropic (x/z by `scale`, y by `height * scale`); the mesh is scaled
  UNIFORMLY by `height * scale`, matching what
  `FoliageImpostorVertexStage.glsl` does to the impostor card
  (`radius = meshRadius * height * scale`). Stretch the mesh to the card's
  aspect to "match" it and a pine renders as a needle, and the near geometry and
  the far impostor become different trees.

The related scoping trap is still live in the other direction. Issue #1267 was
filed as "mesh/impostor layers do not render on Vulkan"; the actual split was
`UseImpostor` (impostor layers had no deferred program and fell back to the
forward `FoliagePass`). Reading a name in the scene YAML and assuming a draw is
the trap either way — grep `EnumerateLayerDraws` before believing either story.

## 5. Bake the impostor from the near mesh's own parts and materials

The bake must draw what the near mesh draws: every submesh, each with its own albedo, and the layer
`AlbedoPath` only where a part has none. `FoliageRenderer` gets both from one helper,
`ExtractPlantGeometry`. Until #1399 the bake drew `Model::GetMesh(0)`'s vertex array with the
layer's billboard as the only texture. On a cold import that vertex array is the trunk alone. With
the billboard painted over the pine's atlas UVs, 12% of the surface passed, and once #1399
corrected the OBJ UV orientation the far pines vanished. Frame the bake from bounds measured off
the vertices too: `MeshSource::GetBoundingBox()` is empty on a warm `.omesh` load. The #1399
integration test reads the atlas back: 8.7% covered as fixed, 3.9% with the billboard.

## Meta-lesson

Every one of these produced "the impostors are mostly missing" and each looked
like a *different* subsystem's fault (scatter, culling, sampling, lighting). The
only thing that separated them was **rendering a dense field, tinting the
subject a colour absent from the terrain palette (magenta) so it isolates
cleanly from a green auto-material, and reading the PNG from several azimuths** —
a terrain-green coverage check passed on a frame with zero actually-rendered
impostors. See `OloEngine/tests/Rendering/PropertyTests/ImpostorBakeEvidenceTest.cpp`
(`ImpostorCardRenderEvidenceTest`).
