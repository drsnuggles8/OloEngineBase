# Count the invocations a cull cannot reach before redesigning how geometry is distributed

Applies to: any LOD or culling work on `WaterRenderPass`, `TerrainChunkManager`,
the virtual-geometry cluster path, and every issue of the form "tessellate
adaptively by X instead of by Y" — and to any geometry laid out in screen space.

Written from issue #1035 (water surface geometry LOD). The issue posed two
designs — gradient-adaptive tessellation versus a projected grid — and made a
profile the gate before either was built. The profile changed the answer; the
build then found four defects that every CPU test had passed over.

---

## 1. A cull inside a shader stage does not save the stages before it

The water tess-control stage rejects off-screen patches by setting
`gl_TessLevelOuter[*]` to 0, and it is good at it: 53–95% of patches. It is easy
to read that as "we only pay for 5–47% of the surface". The census in
[`WaterGeometryLodProfileTest`](../../OloEngine/tests/Rendering/PropertyTests/WaterGeometryLodProfileTest.cpp)
says otherwise: WaterShowcase pays 1,572,864 vertex invocations a frame and
Drift 2,457,600, at every camera pose, because every base patch runs its vertex
and tess-control shader *in order to be rejected*. That cost is a function of
world size and grid resolution only, so **no tessellation rule can reach it**.

**The rule:** before designing a scheme that redistributes work, count the work
upstream of the stage the scheme runs in. Separate the census into "what the
cull can reach" and "what it cannot". The fix for the unreachable half is always
structural — fewer primitives submitted — never a better decision per primitive.

## 2. Screen-space size, not distance, says whether geometry is wasted

The same census bucketed surviving geometry by pixel area. At a low grazing
angle 80% (WaterShowcase) and 94% (Drift) of generated triangles covered less
than a pixel; from overhead, 0%. Distance had not changed — incidence had, and
`calcTessLevel` reads distance.

**The rule:** project the primitive and measure its pixel area. "How far away is
it" ranks nothing on a surface the camera can view edge-on. Sub-pixel geometry
is not merely wasted, it aliases ([water-shading-nyquist.md](water-shading-nyquist.md)),
so the cheaper scheme can also be the better-looking one.

## 3. A band-limited surface cannot be rougher than the mesh it is drawn on

The case *for* gradient-adaptive tessellation is calm water over-subdivided
next to under-subdivided crests. `|grad h|` ran p5–p95 of 0.027–0.125
(WaterShowcase) and 0.004–0.024 (Drift), and the distance rule already spent
9.7–10.0% of its geometry on the roughest decile — the share a rule blind to
roughness spends. #943's `octaveMeshWeight` had already removed every octave the
grid cannot sample.

**The rule:** measure a signal's dynamic range on the data that actually reaches
the renderer — after every filter, clamp and band-limit — before adapting to it.

## 4. A screen-space layout must cover where geometry ENDS UP, not where it starts

A projected grid is placed on the resting plane and displaced afterwards, so the
layout has to reach past the screen edge by however far a crest can move a
vertex there. The band scales with proximity, not displacement: at a 3 m eye a
1.5 m crest lifts the bottom row by a quarter of the vertical field of view.

**The rule:** intersect the frustum with the *volume the surface can occupy*
(Johanson 2004: the slab `plane ± maxDisplacement`), flatten it onto the layout
surface, take its bounds. Expect the margin to be the expensive part — 61% of
rows sit below the screen at that pose — and measure it.

## 5. The same quantity can need two bounds; the loose one must be the cull's

The cull's `MaxWaveDisplacement` and the grid's `MaxSurfaceDisplacement` differ
by 2.8×. Over-estimating is free for a cull and a cliff for a layout: once the
margin approaches the camera's height above the surface the rectangle runs
away, and the cull's bound put 92% of the grid off screen where the tight one
put 61%. **The rule:** derive one bound per consumer and pin their ORDER with a
test — a cull tighter than the layout discards geometry the layout placed.

## 6. A mirror test that builds its own matrix is testing itself

Eleven contract tests were green against `glm::perspective * glm::lookAt`
while the shader was wrong against `EditorCamera`. **The rule:** build the
camera the engine renders with and take its matrix; where impractical, assert
the assumption — the suite now prints which NDC depth unprojects in front.

## 7. Screen-space remapping can flip the winding of every triangle

The bug that produced no water at all. `CreateWaterGrid` winds triangles
counter-clockwise from above for its (u → +x, v → +z) frame, and +z is TOWARD a
camera looking down −z. Screen-up is AWAY from the camera, so mapping v onto
NDC y hands the same index order a frame of the opposite handedness: every
triangle is back-facing from above, and the fragment stage's waterline rule
(keep the face the camera is on) discards the surface. The frame still looked
almost like water — the skybox's painted sea and the editor's y=0 grid showed
through — and every intersection formulation produced it identically.

**The rule:** when a shader repositions vertices by a mapping, check the
mapping's handedness against the index buffer's, and know which fragment-side
rule depends on facing. A one-line axis flip (`v = 1` at the near edge) is the
fix; twelve bisection arms was the price of not knowing to look for it.

## 8. Anything that feeds displacement must be per VERTEX, never per patch

Deriving the band-limit spacing from each patch's own edge length was the right
magnitude and tore the surface: a vertex shared by two patches got two octave
weights, the two displacements disagreed, and 16 pinholes of seabed showed per
overhead frame (0 with the per-vertex form). **The rule:** compute it in the
vertex stage from the vertex's own ray and interpolate; a shared vertex then
gets the same number on both sides by construction.

## 9. Bisect a wrong frame by arms — and instrument, don't infer

What worked, in order: **look at the control** (the world-grid capture proved
the seabed should be hidden); **replace the suspect's output with something
known good** (placing vertices where the world grid would separated "the branch
is wrong" from "the placement is wrong"); **make the geometry self-describing**
(an opaque marker colour showed the surface covered only a horizon sliver; a
world-position colour showed where the rest had gone). The tell that saves the
most time: **two arms with numerically identical output means the change is not
on the path being exercised** — several arms returned the same mean to seven
digits, which said the hit test was rejecting everything in all of them, and
was worth more than any of the hypotheses. And design the instrument so it can
fail: a fixed-radius placement flattened onto the plane *is* a narrow band, and
two such captures "confirmed" nothing.

## 10. Keep the census in the suite, not in the PR body

The measurement that picks between designs is the part most likely to be
re-litigated, and a PR-body table cannot be re-run. #1035's census is an L1
test that replays the real tess-control stage over the real shipped grids and
asserts the *shape* of the answer. **If a measurement justified a design, ship
the measurement.**
