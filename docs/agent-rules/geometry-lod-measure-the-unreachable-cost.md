# Count the invocations a cull cannot reach before redesigning how geometry is distributed

Applies to: any LOD or culling work on `WaterRenderPass`, `TerrainChunkManager`,
the virtual-geometry cluster path, and every issue of the form "tessellate
adaptively by X instead of by Y".

Written from issue #1035 (water surface geometry LOD). The issue posed two
designs — gradient-adaptive tessellation versus a projected grid — and made a
profile the gate before either was built. The profile changed the answer, and
the two things it measured are the two things this kind of question keeps
turning on.

---

## 1. A cull inside a shader stage does not save the stages before it

The water tess-control stage rejects off-screen patches by setting
`gl_TessLevelOuter[*]` to 0, and it is good at it: 53–95% of patches, depending
on the camera. It is easy to read that as "we only pay for 5–47% of the
surface". That is wrong, and the census in
[`WaterGeometryLodProfileTest`](../../OloEngine/tests/Rendering/PropertyTests/WaterGeometryLodProfileTest.cpp)
is what made it visible:

| scene | grid | VS invocations per frame | patches culled |
|---|---|---|---|
| WaterShowcase | 512×512 over 1 km | 1,572,864 | 53–86% |
| Drift | 640×640 over 1.6 km | 2,457,600 | 71–95% |

The invocation count does not move with the camera at all. Every base patch
runs its vertex shader and its tess-control shader *in order to be rejected*.
That cost is a function of world size and grid resolution and of nothing else,
so **no tessellation rule can reach it** — not the distance rule that shipped,
and not the gradient rule that was proposed.

**The rule:** before designing a scheme that redistributes work, count the work
that happens *upstream* of the stage that scheme runs in. Separate the census
into "what the cull can reach" and "what it cannot", and state both. A design
that only improves the reachable half is competing for a minority of the cost.

The fix for the unreachable half is always structural — fewer primitives
submitted, not better decisions per primitive. For water that is the projected
grid (vertex count set by screen resolution); for terrain it is chunk
granularity; for meshes it is the cluster/LOD selection upstream of the draw.

## 2. Screen-space size, not distance, is what says whether geometry is wasted

The same census bucketed the *surviving* geometry by its screen-space area. At a
low grazing angle, 80% (WaterShowcase) and 94% (Drift) of the triangles the
tessellator generated covered less than one pixel each. Looking down from
overhead the figure was 0%.

Distance had not changed. Incidence had. A world-space lattice foreshortens with
the cosine of the grazing angle, and a rule that reads `distance(camera, patch)`
cannot see that — it hands a patch at 150 m the same subdivision whether the
camera is level with it or above it, while the raster's ability to show that
subdivision differs by two orders of magnitude between the two.

**The rule:** when profiling geometry cost, project the primitive and measure its
pixel area. "How far away is it" ranks nothing on a surface the camera can view
edge-on, which is every ground plane, water plane and terrain in the engine.

Sub-pixel geometry is not merely wasted, it is actively worse: sampling below
the raster rate is the same aliasing failure
[water-shading-nyquist.md](water-shading-nyquist.md) describes for normals, so
the cheaper scheme can also be the better-looking one.

## 3. A band-limited surface cannot be rougher than the mesh it is drawn on

The case *for* gradient-adaptive tessellation is that calm water is
over-subdivided while crests are under-subdivided. The census found no such
spread: `|grad h|` ran p5–p95 of 0.027–0.125 (WaterShowcase) and 0.004–0.024
(Drift), and the existing distance rule already landed 9.7–10.0% of its geometry
on the roughest decile of patches — the share a rule *blind* to roughness lands
there.

The reason is a mechanism that was added for an unrelated purpose. #943's
`octaveMeshWeight` band-limits the Gerstner octave ladder to what the mesh
spacing can sample, fading out any octave with fewer than 3–6 samples per
wavelength. So the rendered surface is, by construction, never rougher than the
grid under it. Adaptive subdivision by roughness would be redistributing a
signal that an earlier decision had already flattened.

**The rule:** before proposing to adapt to a signal, measure the signal's
dynamic range on the data that actually reaches the renderer — after every
filter, clamp and band-limit in the chain. A feature that flattens a signal for
good reasons in one place silently removes the payoff of adapting to it in
another, and neither end of that has a test that fails.

## 4. Invert the matrix in the shader rather than uploading an inverse

Two live seams make "which matrix" a real question: camera-relative rendering
(#429) makes the shader's `u_ViewProjection` render-*relative*, and the
projection seam (#691 / ADR 0011) makes it backend-*adjusted*. Uploading an
inverse means picking which flavour to invert on the CPU and being silently
wrong on one backend, or 40 km from the origin, if that pick ever drifts from
the upload site it was matched to. `inverse(u_ViewProjection)` in the stage
that also feeds `gl_Position` cannot drift.

**The rule:** when a shader needs the inverse of a matrix it already has, and
the vertex count affords it, invert it there. Send from the CPU only what is not
derivable in-stage — for the projected grid, just the NDC rectangle of §5, a
pure screen-space quantity identical in both spaces.

## 5. A screen-space layout must cover where geometry ENDS UP, not where it starts

A projected grid, a screen-space decal atlas, an impostor card — anything laid
out in screen space and then *displaced* — is placed before the displacement
runs. So the layout has to reach beyond the screen by however far the
displacement can move a vertex there, or the outermost row leaves the frame and
takes a band of the picture with it.

The size of that band scales with proximity, not with the displacement: water
at a 3 m eye meets the bottom of a 45° frame at 7.2 m, and a 1.5 m crest lifts
that point by 10.8° — a **quarter of the vertical field of view** gone. The same
1.5 m at the horizon is sub-pixel.

**The rule:** for any screen-space layout of displaced geometry, compute the
layout rectangle by intersecting the frustum with the *volume the surface can
occupy* (Johanson 2004 does exactly this for water: the slab between
`plane ± maxDisplacement`), flatten that onto the layout surface, and take its
bounds. Not the screen rectangle. The result is smaller than the screen where
the surface is not reachable and larger where displacement can carry geometry in
from outside.

Expect the margin to be the expensive part and measure it: on #1035's grazing
pose 61% of the grid's rows land below the screen, so the authored resolution
has to be roughly double what the viewport alone suggests.

## 6. The same quantity can need two bounds, and the loose one must be the cull's

`MaxWaveDisplacement` (the tess-control cull's margin) and
`MaxSurfaceDisplacement` (the projected grid's) both answer "how far can this
surface move". They differ by 2.8x, and collapsing them into one number breaks
whichever consumer gets the wrong end:

- for a **cull**, over-estimating is free — a patch wrongly kept costs one patch
  — so the bound can be loose, bound each detail octave by the largest, and
  carry a safety factor;
- for a **layout**, over-estimating is not free and the failure is a cliff
  rather than a slope. The rectangle grows by the margin; once the margin
  approaches the camera's height above the surface, the displaceable volume
  swallows the viewpoint and the rectangle runs away. Feeding the cull's bound
  to #1035's grid put 92% of it off screen where the tight bound put 61%.

**The rule:** when one physical quantity feeds both a conservative test and a
sizing decision, derive two bounds, name them for their consumers, and pin the
ORDER between them with a test (`cullBound >= layoutBound`). The ordering is the
real invariant — a cull tighter than the layout discards geometry the layout
placed, which renders as a hole that moves with the camera. Sharing one number
"to avoid drift" is the wrong instinct here: the drift that matters is the
ordering, not the equality.

## 7. A mirror test that builds its own matrix is testing itself

#1035's projected grid had eleven CPU contract tests green — uniform screen
density, world spacing growing with distance, the NDC band excluding the sky,
the rim fall-back, the rect clamp — while the shader rendered the water surface
collapsed onto the horizon line. Every one of those tests built its camera as
`glm::perspective(...) * glm::lookAt(...)`. The engine renders with
`EditorCamera`, whose matrix the shader inverts, and the two disagreed in a way
the hand-built one could not express.

**The rule:** a test that pins GPU math against a matrix it constructed itself
has validated the mirror against its own assumptions. Build the camera the
engine uses (`EditorCamera`, the scene camera, the shadow camera) and take its
matrix. Where that is impractical, assert the ASSUMPTION explicitly — #1035 now
carries a test that prints which NDC depth unprojects in front of the camera,
so the convention is visible rather than inferred.

## 8. Bisect a wrong frame by arms, and check the control first

Eight bisection arms found #1035's defects; the first three were wasted because
the hypotheses were plausible and untested. What worked, in order:

1. **Look at the control.** The world-grid capture of the same scene showed the
   seabed fully hidden, which turned "is magenta expected here?" from an
   argument into a fact. Capture the A and the B in the SAME run
   ([live-editor-pixel-ab-needs-a-same-mode-control] is the memory-side twin).
2. **Replace the suspect's OUTPUT with something known good.** Placing the
   vertices where the world grid would put them, with every other part of the
   new path still active, separated "the new branch is wrong" from "the new
   placement is wrong" in one arm.
3. **Make the geometry self-describing.** Forcing the surface to an opaque flat
   colour showed it covered only a sliver of the frame; encoding world position
   into the colour then showed the missing geometry was sitting 200 m away,
   behind the camera, projected back across the screen. Both answers were
   immediate and neither was available from the assertions.

**The tell that saves the most time:** if two arms produce numerically IDENTICAL
output, the thing you changed is not on the path being exercised. Three of
#1035's arms returned the same mean to seven digits, which meant the hit test
was rejecting everything in all of them — a fact worth more than any of the
three hypotheses.

## 9. Keep the census in the suite, not in the PR body

A measurement that picks between two designs is the part most likely to be
re-litigated, and a table in a merged PR body cannot be re-run. #1035's census
is a normal L1 test: it replays the real tess-control stage over the real
shipped grids, prints its numbers, and asserts the *shape* of the answer rather
than the exact counts, so a changed scene or wave model either still holds or
fails and says which premise moved.

**The rule:** if a measurement justified a design, ship the measurement.
