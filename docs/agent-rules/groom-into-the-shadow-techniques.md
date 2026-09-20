# A groom in the scene's shadows: the cache lift and the double-count boundary (#1323)

Read before touching `OloEngine/Groom/GroomStrandCache.{h,cpp}`,
`OloEngine/Groom/GroomShadowWidening.h`, the groom half of
`Renderer/Passes/ShadowRenderPass.cpp`, `OloEditor/assets/shaders/GroomStrandDepth.glsl`,
`VSM_GroomDepth.glsl`, `include/GroomShadowWidening.glsl`, or `oloGroomSceneShadow` in
`GroomStrand.glsl`.

## The rules

1. **A caster family whose geometry is built inside a pass that runs AFTER `ShadowRenderPass`
   cannot be a caster, and no amount of list plumbing fixes it.** The other five families submit
   during Scene's entity traversal because their VAOs already exist; a groom's does not — it is
   built from cooked curves. That build lived inside `GroomRenderPass::Execute`, which runs in the
   render-stream band, long after the shadow map has been rasterised. So the sixth family took a
   **refactor of the cache into `GroomStrandCache`**, owned by `RenderPipeline` and pointed at by
   both passes, rather than a POD list and a depth shader. Check where a new family's geometry comes
   from before estimating the work.

2. **The one-texel width floor is the whole casting mechanism, and its number is measured.** A 70 µm
   hair against a 20 m cascade at 2048 texels is **0.0072 texels of half width** — rasterised
   honestly it crosses a texel centre essentially never, so an animal's entire coat casts nothing at
   all. The floor multiplies it by **139×**. That is the same argument `groom-strand-visibility.md`
   rule 2 makes for the main pass, three orders of magnitude coarser, and
   `GroomShadowWidening.AHairIsFarBelowACascadeTexelSoTheFloorIsWhatMakesItCast` is what keeps it a
   measurement instead of a slogan.

3. **There is NO compensating alpha on the shadow side, and that is a refusal rather than an
   omission.** The main pass turns the widening back into coverage by weighting the fragment's alpha
   and resolving it stochastically. A depth-only target has no alpha to weight, and a hashed discard
   there would be a stochastic technique with **nothing to converge it** — precisely the silent
   fallback `groom-strand-visibility.md` rule 6 refuses one pass over. So a widened strand casts an
   **opaque** shadow, and the consequence is bounded rather than hidden: a coat too sparse to fill a
   shadow texel is over-occluded by at most the widening factor of rule 2. A dense coat — the case
   grooms exist for — has many strands per texel and is opaque there in reality too.

4. **The receiver is the coat's LIGHT-EXIT POINT, not the fragment, and the gate is CASTING
   rather than the density volume.** Once a groom is a caster its own strands are in the cascade
   map, so a strand sampling that map at its own position is occluded by its own coat -- on top of
   whatever the volume (#1248) already charged it. `oloGroomCoatLightExitDistance` returns the
   distance along `L` at which the ray leaves the coat's object box, and the sample is taken there.

   **The gate was measured, not reasoned.** Tied to the volume, a coat with no volume -- the
   commonest authoring -- kept the fragment as its receiver and the evidence coat fell from **44.98
   mean luma to 0.22**: black. A shadow map is a **binary** visibility test and a coat is not
   binary, so every strand behind the outermost widened ribbon reads as fully shadowed. A black coat
   is the silent failure `groom-coat-self-shadowing.md` rule 10 forbids. Replacing the map's
   occlusion with a *graded* one is the volume's job; a coat without one is #1247's unshadowed
   picture, which is a legitimate state. `ACoatThatCastsIsNotShadowedByItsOwnStrands` asserts both
   volume states, absolutely rather than as a ratio -- the ratio version died the moment the gate
   moved, because both arms then read 0.000.

   Three consequences that catch people:

   - **`CoatModes.z`, not `.x`.** The march's mode lane gates the volume; a separate lane gates the
     offset, because the two conditions are independent. A whole-vector assign to `u_GroomCoatModes`
     inside the coat block silently clears `.y` and `.z`.
   - **The direction is in WORLD metres because it is NOT normalised.** The march in the same file
     normalises `mat3(worldToObject) * L`, which makes its `t` an *object*-space distance --
     consistent there, because its step is an object-space voxel size. The exit distance is added to
     a *world* position, so it keeps the transform's 1/scale and `t` comes out in world metres by
     construction. Normalising and using the value anyway is wrong by the coat's object scale, which
     is 1.0 in every test scene.
   - **Nothing inside the coat's own box can shadow it any more, including the body it grows on.**
     That is the price of a box-exit offset and it is stated rather than discovered. The criterion's
     own case -- a character standing in shade -- is an occluder *outside* the box and is
     unaffected.

5. **A strand has no surface normal, so the receiver bias is spent along `L`.** The engine's
   receiver offset is `normalize(surfaceNormal) * shadowParams.y` in world metres, and a ribbon's
   `v_ViewNormal` faces the camera rather than the surface — it is written for SSAO. Passing `L`
   spends the same budget in the one direction that means anything for occlusion on a fibre, and it
   is also what stops the exit-point sample landing on the coat's own outermost ribbon and
   self-shadowing. Passing a zero vector is not an option: `calculateCascadedShadowFactorCSM`
   normalises it, and 0/0 is a NaN that removes the coat's shading entirely.

6. **Wire BOTH directional techniques, and count the draws per technique.**
   `virtual-geometry-into-a-second-shadow-technique.md` is explicit that a family reaches a technique
   only if somebody wired it there and that **nothing detects the gap**. Grooms reach the CSM
   cascades through `RenderCascadeOrFace` and the Virtual Shadow Map through the
   `ExternalCasterRenderer` seam #1149 opened — two shaders, because the VSM has no depth attachment
   and resolves a page table instead, sharing their vertex maths verbatim through
   `include/GroomShadowWidening.glsl`. `GroomShadowCasterStats` carries `CascadeDraws`,
   `VirtualShadowLevelDraws` and `AtlasDraws` **separately**, and the inspector warns when the
   frame's directional technique drew none of a coat that is a caster. That warning is the thing
   that detects the gap.

7. **Grooms are ITEM-SAFE in the parallel cascade region; their GEOMETRY is not.** The only object a
   groom draw writes is `ItemResources::Groom`, so the draws record inside the fork like the mesh and
   skinned families. But **acquiring from the cache can CREATE buffers**, and amendment (92) rule 7
   refuses resource creation on an item context — so `CollectGroomCasters` runs on the render thread
   at the top of `Execute`, before any region opens. This is silent on OpenGL and a fault on Vulkan.

8. **Grooms are BOUNDED casters in the cascade-skip test.** The strand build publishes the box the
   emitted centrelines occupy *in this pose* (`GroomStrandMeshStats::BoundsValid`), so they belong in
   the per-cascade frustum test rather than in the unbounded set beside terrain and foliage. Leaving
   them out skips a cascade whose only caster is a coat — the exact hole virtual geometry had from
   #702 to #1149, one family over. The box is padded by the widened half width, because the widening
   happens in clip space *after* the test.

9. **The shared cache builds each key at most once per frame.** Two consumers now acquire the same
   key, and a **deformed** groom rebuilds unconditionally — so without the `LastUsedTick == m_Tick`
   short-circuit the body's pose is re-evaluated and the vertex buffer refilled twice, at twice the
   CPU cost, with `DeformedRebuilds` reading two for a coat that is behaving.

10. **Eviction moved to the TOP of the frame, and it had to.** With one consumer there was a "last
    groom draw of the frame" to hang it on; with two there is not, and an entry freed between the
    shadow acquire and the strand draw is freed with a recorded draw still pointing at it.
    `GroomStrandCache::BeginFrame` evicts and then advances the tick, in that order, so the retention
    window keeps meaning "frames since last used".

11. **The caster's width must be the DRAWN width.** `GroomStrandCache::EffectiveWidthScale` is the
    one expression, shared by the strand pass and the caster gather, so a coat thickened to
    compensate for a spent strand budget (#1252) casts the shadow of the coat that is on screen. Two
    copies drift, and the symptom is a shadow slightly the wrong size — which nobody reads as a bug.

## Still deferred, with the reason restated

**A bound, deforming groom is still refused coat self-shadowing**
(`GroomCoatShadowFallbackReason::GroomIsDeformed`). #1323 lifted the geometry cache, which was the
plumbing half this was waiting on — and the remaining half turns out to be a **cost contract, not
plumbing**. The volume's whole update policy rests on the bake being object-space and
light-independent, so a static coat rebuilds *never* (`groom-coat-self-shadowing.md` rule 6). A
deformed coat's strands move every frame, so its bake would be per entity **and** per frame: eight
resident 64³ RGBA32F volumes is 128 MiB of re-upload per frame at the resident cap. That is a
different feature with a different budget, and rebuilding "sometimes" is exactly the stale-volume
flicker criterion 3 was written against.

## Things that will bite

- **`length()`, never the signed element, when deriving a scale from a projection.** Vulkan's clip
  space has +Y downwards, so the engine uploads a projection whose `[1][1]` is negative; a signed
  read returns a negative NDC-per-world, every half width clamps to the floor in one axis, and the
  failure is invisible on OpenGL with no validation message anywhere. Same trap, same shape, as the
  one `groom-strand-visibility.md` gives its own heading to.
  `TheScaleTakesTheMagnitudeSoAVulkanYFlipDoesNotInvertIt` is what says so.
- **The VSM route must re-bind the physical pool image after binding its own program.**
  `BindPhysicalPoolImage` forks on whether the program *currently in flight* is bindless, so it
  cannot be hoisted out of a shader switch — and it is not enough that the mesh raster bound it a
  moment ago, because in a scene whose only casters are grooms the mesh raster returns before binding
  anything. The failure is every `imageAtomicMin` being discarded: a silently unshadowed frame.
- **A whole-vector assign to `u_GroomCoatModes` clears the receive lane.** `.y` is written for every
  draw, outside the coat block; the coat block writes `.x` only. An `ivec4(...)` assign inside the
  coat block silently turns receiving off for every coat that also self-shadows — which is the
  configuration the double-count boundary is about.
- **Binding 7 is shared by three different blocks now.** `GroomStrandParamsUBO` (400 B, the strand
  pass), and `GroomShadowParamsUBO` (96 B) in two copies — one per cascade item and one for the
  sequential VSM route. UBO_USER_0's contract is that its occupant rebinds and refills it before its
  own draws, and all three do; the trap is assuming a bind survives another pass.
- **The evidence masks are DERIVED, not typed.** "The coat's shadow fell on the ground" and "the
  ground's shadow fell on the coat" are claims about two disjoint screen regions, and a hand-placed
  rectangle that drifts off the coat turns every assertion into a measurement of the background.
  `GroomSceneShadowVisualEvidenceTest` obtains the coat's mask by toggling
  `GroomComponent::m_RenderStrands` and taking the pixels that moved.

## Where the evidence lives

- **The measurement behind the floor:** `OloEngine/tests/Groom/GroomShadowWideningTest.cpp`.
- **The pixels:** `OloEngine/tests/Rendering/PropertyTests/GroomSceneShadowVisualEvidenceTest.cpp`
  writes `OloEditor/assets/tests/visual/GroomSceneShadow[Off]_GL_<Path>[_<Case>].png`.
- **The three-way split the double-count boundary rests on:**
  [groom-coat-self-shadowing.md](groom-coat-self-shadowing.md) rule 1.
- **Why a family does not reach a technique by itself:**
  [virtual-geometry-into-a-second-shadow-technique.md](virtual-geometry-into-a-second-shadow-technique.md).
