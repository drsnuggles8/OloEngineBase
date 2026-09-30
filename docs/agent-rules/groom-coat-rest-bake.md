# A coat baked at rest (#1533)

Read before touching `GroomCoatShadowComponent::m_BakeAtRest`, the `bakeAtRest` arm of
`GroomRenderPass::AcquireCoatVolume`, `oloGroomCoatTau` / `oloGroomCoatExitDistance` in
`GroomStrand.glsl`, or the bind frames in the deformation buffer (`GroomDeformBindRecord`,
`oloGroomDeformBindFrame`). The pose bake it replaces for a coat that opts in is
[groom-deformed-coat-self-shadowing.md](groom-deformed-coat-self-shadowing.md).

## The rules

1. **Look the rest volume up where the point RESTS, with the light turned back through its root.**
   The volume is baked once, from the coat at rest, like a static coat's. The vertex stage takes
   each GPU-deformed point back to its bind pose (`bindOrigin + bindRotation * local`, the rest
   stream's own encoding) and hands the fragment the rotation that takes a direction on the posed
   coat back to the rest coat (`bindRotation * conjugate(currentRotation)`). The march starts at the
   rest point and runs along the turned direction. For everything the root carries whole, the posed
   density is the rest density moved by that rotation, so it is the same integral.
   `ACoatBakedAtRestIsLookedUpWhereItRests` pins it: a whole coat turned 35 degrees.

2. **This is not #1248's refused bind-pose volume.** That one sampled the rest volume at the POSED
   point, which is wrong for every strand that moved. Rule 1 samples it at the point's own rest
   position. The same test keeps the refused lookup as its negative control, and it has to be
   clearly worse.

3. **It is biased where a limb moves AGAINST its neighbours, and nowhere else.** Across the hinge
   each part sees the other as it was at rest: a folded leg's fur does not shadow the belly, and
   the belly's does not shadow the leg. `ARestBakeIsBiasedOnlyWhereALimbTurnsAgainstItsNeighbours`
   measures it (Measured, below). The scene shadow map still carries the coat's large-scale
   occlusion of itself, because the strands are casters (#1323).

4. **The simulation's displacement is not in the lookup.** A swinging lock is shadowed by the
   neighbours it was groomed among. On a stiff short coat that is millimetres; on long feathering
   it is the price of the mode.

5. **Opt in per coat; the pose bake stays the default.** The pose bake is exact and is what #1426
   ships. The rest bake exists because on a dense coat the pose bake is the frame: the dog's
   ~1.8M segments cost 74 ms of CPU pose and 120 ms of bake every frame it breathed. Only a
   GPU-deformed draw carries a bind point per vertex. A CPU-deformed coat, the reference path,
   keeps the pose bake whatever the component says.

6. **The bind frames ride in the deformation buffer's static region.** `[BindBase, +2R)` sits
   between the guide weights and the per-frame roots, written by the first `PackFrame` after a
   `Reset` (which is not handed the binding) and uploaded once with the relayout. The shader derives
   `BindBase = RootCount * 2` from the root count; `GroomDeformBufferLayout::Make` writes the same
   number. Change one and the other in the same commit.

7. **The depth prepass skips the rest lookup.** The prepass draw writes depth only, so its vertex
   stage leaves the bind point and the rotation out, along with last frame's deformation. None of
   them feed `gl_Position`, which is what keeps the two draws equal at depth EQUAL.

## Measured

**The dog, two seconds into Idle, face close-up** (`TheRestBakeShadowsTheMovingCoatAsThePoseBakeDoes`,
GL Forward, 1280×720): the rest bake differs from the pose bake by 28 961 px (mean |ΔL| 1.63 of
255) against a repeat floor of 15 978 px. The self-shadow itself changes the same frame by
396 369 px (mean |ΔL| 13.0).

**The dog's frame** (`CostOfTheThreeFramings`, 1920×1080, RTX 4090): the pose bake spent 74 ms
posing segments and 123 ms baking (82 of them binning) on the CPU on every frame, so the frame was
CPU-bound at 293 ms. With the rest bake both are zero after the first frame, and the resident
volume drops from 91 MiB (the three-slot ring) to 25 MiB (one slot, never rewritten).

**The property tests** (`GroomCoatShadowDeformedPropertyTests`, CPU, 4000-strand ball, 48³, against
the exact reference): see the two `rest bake` lines those cases print.
