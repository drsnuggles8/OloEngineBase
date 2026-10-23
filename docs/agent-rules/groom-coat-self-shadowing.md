# Dense-coat self-shadowing (#1248)

Read before touching `OloEngine/src/OloEngine/Groom/GroomCoatShadow.{h,cpp}`, `GroomCoatShadowTechnique.h`, `OloEditor/assets/shaders/include/GroomCoatShadowCommon.glsl`, the coat half of `GroomStrand.glsl`, or `GroomCoatShadowComponent`.

## Rules

1. **Keep the coat term geometric and colourless.** `tau` is fibre length density times diameter times the sine of the angle to the fibre; deriving `kappa` from pigment applies pigment twice. Dual scattering (#1533) adds crossed fibres' colour separately: [groom-dual-scattering.md](groom-dual-scattering.md). [Details](groom-coat-self-shadowing-details.md#the-rules)
2. **Bypass the root-to-tip ramp (#1246) when coat shadowing is active; a coat with no volume keeps it.** Keeping both darkens roots twice. [Details](groom-coat-self-shadowing-details.md#the-rules)
3. **Do not "fix" a sparse coat getting brighter by restoring the ramp.** Pinned by `ADenserCoatIsDarkenedMoreThanASparseOne` (+380 857 summed luma at 1500 strands, -487 404 at 6000). [Details](groom-coat-self-shadowing-details.md#the-rules)
4. **Derive resolution from the coat's spacing, never "more is better".** Volume and deep opacity map optimise near two strand spacings and alias below it (128^3 scored worse than 64^3). [Details](groom-coat-self-shadowing-details.md#the-rules)
5. **Keep the march at about three voxels per step.** Error is systematic bias, so refining buys nothing; past about four voxels it degrades sharply on a dense coat. [Details](groom-coat-self-shadowing-details.md#the-rules)
6. **Keep the representation light-independent.** One bake serves every light; the deep opacity map was rejected because it must rebuild when light or coat moves. `GroomCoatShadowVisualEvidence.AnAnimatedLightCausesNoVolumeRebuilds` asserts zero rebuilds. [Details](groom-coat-self-shadowing-details.md#the-rules)
7. **Bake in groom object space, and put `WidthScale` in the rebuild condition.** Resolution change, new geometry and `WidthScale` invalidate the bake; `u_GroomCoatWorldToObject` must be rigid (no scale). [Details](groom-coat-self-shadowing-details.md#the-rules)
8. **Build `CoatWorldToObject` from the render-relative model matrix.** `v_WorldPos` is render-relative (#429); the absolute transform is wrong away from the world origin. [Details](groom-coat-self-shadowing-details.md#the-rules)
9. **Always bind a real 3D texture.** `NullSamplerKind` has no `Texture3D` arm; keep the 1x1x1 zero volume as a precondition, and draw nothing (with a log) if the placeholder is missing. [Details](groom-coat-self-shadowing-details.md#the-rules)
10. **Make a failed or missing representation read fully lit.** Covers non-finite optical depth, a point outside the volume and a corrupt `kappa`. [Details](groom-coat-self-shadowing-details.md#the-rules)
11. **Validate the mode with `Reject`, not `Clamp`.** It is a discriminated index; clamping turns corruption into a different valid representation (as `GroomComponent::m_CompositionMode`). [Details](groom-coat-self-shadowing-details.md#the-rules)
12. **Use the mean of transmittances, `exp(-tau * (1 - exp(-kappa)))` (#1360).** `kappa` describes one fibre; transmittance floors at `exp(-tau)`, so do not assert black at large `kappa`. Default `kappa` is 4.0 (knee of a sweep: 94.9% of the darkening); scenes authored at 1.0 render brighter and a dense coat should be re-judged. [Details](groom-coat-self-shadowing-details.md#the-rules)
13. **Bake a bound coat from its drawn strands in groom object space, never from rest curves.** See [groom-deformed-coat-self-shadowing.md](groom-deformed-coat-self-shadowing.md). [Details](groom-coat-self-shadowing-details.md#the-rules)
14. **Rebake a bound coat on drift, not frame count.** `CoatRebakePolicy`: rebake past 0.5 voxels, refuse (`RepresentationStale`, fully lit) past 2 (#1426). [Details](groom-coat-self-shadowing-details.md#what-this-slice-does-not-do)
15. **Make a reference's footprint span several strand spacings.** At 0.1 pitch the reference reports tau 0.00 on a lattice whose answer is 9.36, and more rays do not help; judge against spacing, not radius. `CoatSegmentGrid` must not change the answer (`TheGridChangesTheCostAndNotTheAnswer` asserts exact equality). [Details](groom-coat-self-shadowing-details.md#the-references-own-trap-the-footprint)
16. **Compare in a discriminating range, not at black.** `TheComparisonRunsInADiscriminatingRangeRatherThanAtBlack` guards the file; check it first when a case passes too easily. Sweep each bake-off candidate's own parameters. [Details](groom-coat-self-shadowing-details.md#things-that-will-bite)
17. **Deposit to the nearest voxel, not a trilinear splat.** Nearest conserves fibre area exactly (`BinningConservesTheCoatsFibreAreaExactly`). [Details](groom-coat-self-shadowing-details.md#things-that-will-bite)
18. **Update the hand-mirrored heap base when adding a sampler slot.** It moved `HEAP_IMAGE_SLOT_BASE` 78 to 79; mirror `OLO_HEAP_IMAGE_BASE` in `include/BindlessHeap.glsl` and the copy in `BindlessHeapGpuTest.cpp`. `TEX_GROOM_COAT_VOLUME = 75` was the last unclaimed index; one remains below the GL 4.6 minimum of 80. [Details](groom-coat-self-shadowing-details.md#things-that-will-bite)
19. **Change the march on both sides together.** `GroomCoatTransmittanceParityTest` (#1360) covers transmittance only; `oloGroomCoatOpticalDepth` vs `SampleDensityVolume` has no GPU parity test. [Details](groom-coat-self-shadowing-details.md#things-that-will-bite)
20. **Do not validate the bindless arm with raw `glslc`.** It lacks the `GL_ARB_bindless_texture` prologue; A/B against `git show HEAD:<shader>` first. [Details](groom-coat-self-shadowing-details.md#things-that-will-bite)
21. **Keep coat-to-scene shadows outside this volume.** They are `ShadowRenderPass` caster families via `GroomSceneShadowComponent` (#1323, #1523): [groom-into-the-shadow-techniques.md](groom-into-the-shadow-techniques.md). [Details](groom-coat-self-shadowing-details.md#what-this-slice-does-not-do)

## Commands and checks

Evidence: `OloEngine/tests/Rendering/PropertyTests/GroomCoatShadowPropertyTests.cpp`, `OloEngine/tests/Groom/GroomCoatShadowSelectionTest.cpp`, and `GroomCoatShadowVisualEvidenceTest.cpp` (writes `OloEditor/assets/tests/visual/GroomCoatShadow[Off]_GL_<Path>[_<Case>].png`). Numbers: [groom-coat-self-shadowing-1248.md](../analysis/groom-coat-self-shadowing-1248.md).

---

Full text, with the failures that taught these rules, the measurements and the derivations:
[groom-coat-self-shadowing-details.md](groom-coat-self-shadowing-details.md).
