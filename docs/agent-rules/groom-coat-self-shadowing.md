# Dense-coat self-shadowing (#1248)

Read before touching `OloEngine/src/OloEngine/Groom/GroomCoatShadow.{h,cpp}`,
`GroomCoatShadowTechnique.h`, `OloEditor/assets/shaders/include/GroomCoatShadowCommon.glsl`, the
coat half of `GroomStrand.glsl`, or `GroomCoatShadowComponent`.

## The rules

1. **The coat term is GEOMETRIC and sees no colour.** There are three attenuations in play and
   they are disjoint by construction: inside one fibre (`exp(-sigma_a * chord)`, #1247), between
   the fibres of one coat (`tau`, this slice), and everything else in the scene (the shadow map).
   `tau` is fibre length density times diameter times the sine of the angle to the fibre, and
   nothing else. Deriving `kappa` from the pigment — which looks like a tidy unification — applies
   the pigment twice and darkens every coloured coat. That is the double-count the issue's own
   scope note forbids.

2. **The root-to-tip ramp is bypassed when coat shadowing is active, and that is not an
   optimisation.** `#1246`'s ramp exists because "a strand is darker near the root because it is
   deeper in the coat" — which is exactly what the density volume now measures per fragment and
   per light instead of assuming. Keeping both darkens the roots twice. A coat with no volume keeps
   the ramp, which is what makes every capture #1246 and #1247 committed still mean what it meant.

3. **Turning coat shadowing on can make a SPARSE coat brighter, and that is correct.** It does two
   things at once: it adds the measured occlusion *and* it bypasses the ramp (rule 2). On a coat too
   sparse to actually self-shadow, the fake darkening it replaced is the stronger of the two, so the
   net result is brighter. Measured on the evidence coat: **+338 020** summed luma at 1500 strands
   against **−510 380** at 6000. Do not "fix" this by restoring the ramp — that is the double count.
   Both ends are pinned by `ADenserCoatIsDarkenedMoreThanASparseOne`.

4. **FINER IS WORSE, for every representation here.** Both the density volume and the deep opacity
   map optimise at roughly **two strand spacings** and alias below it: a cell holding one strand or
   none stores a sample rather than an average. Measured, 128³ scored worse than 64³ while costing
   eight times the memory, and a 512² deep map scored three times worse than the same map at 128².
   Spending more memory past the optimum does not waste it — it makes the shadow worse. Derive a
   resolution from the coat's spacing, never from "more is better".

5. **A coarser march is free, and the reason matters.** Three voxels per step was at or within
   noise of the error minimum on both reference coats and cost a third of the taps of a one-voxel
   march, because the volume's error is a systematic **bias** rather than quadrature. Refining the
   step therefore buys nothing. Past about four voxels it degrades sharply on a dense coat, so the
   window is real rather than "as coarse as you like".

6. **The selected representation is LIGHT-INDEPENDENT, and that is what answers the flicker
   criterion.** One bake serves every light, so an animated light costs exactly zero rebuilds —
   structurally, not by tuning. A deep opacity map was measured, is cheaper per query, and was
   **rejected** for precisely this: it must be rebuilt whenever the light or the coat moves.
   `GroomCoatShadowVisualEvidence.AnAnimatedLightCausesNoVolumeRebuilds` asserts the zero.

7. **The bake is in GROOM OBJECT SPACE.** A coat that merely moves reuses it; only a resolution
   change or new geometry invalidates it. The consequence is that `u_GroomCoatWorldToObject` must
   be **rigid**: the march compares angles in that space against each voxel's mean fibre direction,
   and a scale in the rotation tilts every fibre by an amount that depends on which way the ray
   points.

8. **`CoatWorldToObject` is built from the RENDER-RELATIVE model matrix**, not the absolute one.
   `v_WorldPos` is render-relative (#429), so inverting the absolute transform marches from a point
   offset by the render origin — invisible near the world origin, which is where every test scene
   sits, and wrong everywhere else. Same trap, same place, as #1247's model matrix.

9. **Bind a real 3D texture ALWAYS; let the routing lane decide whether it is sampled.**
   `NullSamplerKind` has no `Texture3D` arm, so a null handle would hand a 2D null descriptor to a
   `sampler3D` declaration — undefined behaviour, not a black read. The pass keeps a 1×1×1 zero
   volume and treats it as a **precondition**: with no placeholder it logs and draws nothing rather
   than binding something of the wrong shape.

10. **A failed or missing representation reads FULLY LIT, never fully shadowed.** That includes a
   non-finite optical depth, a point outside the volume, and a corrupt `kappa`. A bright coat is
   visibly "this did not run"; a black one is indistinguishable from a correct silhouette.

11. **The mode is `Reject`, not `Clamp`.** It is a discriminated index, so saturating a corrupt
    value turns it into a *different valid* one and the coat is shadowed by a representation nobody
    authored — which looks entirely plausible. Same reasoning as
    `GroomComponent::m_CompositionMode`.

## The reference's own trap: the footprint

A single ray's crossing count is an integer, so the ground truth is an expectation over a bundle.
**The bundle's footprint must span several strand spacings**, and if it does not the reference
fails *silently*. On a lattice whose analytic answer is 9.36 crossings:

| footprint | reference tau |
|---|---|
| 0.1 pitch | **0.00** |
| 0.4 pitch | 11.55 |
| 0.8 pitch | 8.93 |
| 4 pitches | 9.54 |

Below one pitch the disc fits between the strands and every ray in it misses the same way, so the
reference reports a coat that casts no shadow at all — and **raising the ray count does not help**.
Judge a footprint against the coat's *spacing*, never against its radius.

The brute-force reference is `O(probes × rays × segments)` and does not finish on a real coat
(8×10⁹ intersection tests). `CoatSegmentGrid` changes the cost and not the answer — it can only add
candidates — which is why `TheGridChangesTheCostAndNotTheAnswer` asserts **exact** equality rather
than a tolerance.

## Things that will bite

- **The comparison saturates at black.** At a high enough density every candidate agrees at T = 0,
  and a comparison taken there measures nothing while looking like it measured everything — the
  same shape as #1246's alpha-to-coverage case failing with two identical numbers.
  `TheComparisonRunsInADiscriminatingRangeRatherThanAtBlack` guards every other case in that file,
  and it is the first thing to check when a new case passes suspiciously easily.
- **Under-tuning the rejected candidate.** The first run of the bake-off scored the deep opacity map
  at 512², four times past its own optimum, and therefore rated it a third as good as it is. A
  bake-off that does not sweep each candidate's own parameters is a bake-off that flatters whatever
  it started with. The resolution sweep is the part that makes the selection evidence.
- **Nearest-voxel deposit, not a trilinear splat.** Nearest conserves the coat's fibre area
  *exactly*, which is what `BinningConservesTheCoatsFibreAreaExactly` asserts as an identity. A
  splat leaks mass out of the boundary voxels and loses shadow without saying so.
- **Adding the sampler slot moved `HEAP_IMAGE_SLOT_BASE` 78 → 79**, and that base is mirrored in
  GLSL **by hand** (`OLO_HEAP_IMAGE_BASE` in `include/BindlessHeap.glsl`, plus the copy in
  `BindlessHeapGpuTest.cpp`). This is the seventh slot to move it and, like five of the six before
  it, it did **not** move the offset table's size — so the array size, the more obvious mirror,
  still matched while the base did not. `TEX_GROOM_COAT_VOLUME = 75` was the last unclaimed sampler
  index; exactly one remains below the GL 4.6 minimum of 80.
- **The bindless arm cannot be validated with raw `glslc`.** It fails on the pre-existing
  `samplerCube` too, because the engine supplies the `GL_ARB_bindless_texture` prologue that raw
  `glslc` does not. A/B against `git show HEAD:<shader>` before concluding a bindless error is
  yours — it takes seconds and it exonerates the change.

## What this slice does NOT do

**A bound, deforming groom is REFUSED, not approximated.** The bake reads the `GroomAsset`'s
rest-pose curves, so on a character whose body animates the drawn strands move and the volume does
not — the coat would carry its bind-pose shadow around, which reads as a shading bug rather than as
the missing feature it is. `GroomRenderPass::AcquireCoatVolume` therefore reports
`GroomIsDeformed` and renders the coat unshadowed. Following a deformation means baking from the
deformed strand positions the pass already builds, which is the natural next slice.


**Neither direction of coat-to-scene shadow integration.** The volume contains the groom's own
strands and nothing else, so:

- hair does not cast onto the body or the scene, and
- the body does not cast onto the hair.

That is criterion 3's other half and it is a **separate slice**, not an oversight, for a structural
reason worth recording: the strand geometry is built inside `GroomRenderPass::Execute`, which runs
*after* `ShadowRenderPass`. Making a groom a shadow caster therefore means lifting the strand
geometry cache somewhere both passes can reach — a refactor of #1246's cache, not a new caster list.
Note also that once grooms *are* casters, a strand reading the cascade map would be shadowed by its
own coat twice; the intended fix is to offset the receiver to the coat's light-exit point, which the
march already computes.

## Where the evidence lives

- **The decision and its numbers:** [docs/analysis/groom-coat-self-shadowing-1248.md](../analysis/groom-coat-self-shadowing-1248.md).
- **The comparison, re-run by CI:** `OloEngine/tests/Rendering/PropertyTests/GroomCoatShadowPropertyTests.cpp`.
- **The seam, one reason at a time:** `OloEngine/tests/Groom/GroomCoatShadowSelectionTest.cpp`.
- **The pixels:** `OloEngine/tests/Rendering/PropertyTests/GroomCoatShadowVisualEvidenceTest.cpp`
  writes `OloEditor/assets/tests/visual/GroomCoatShadow[Off]_GL_<Path>[_<Case>].png`.
