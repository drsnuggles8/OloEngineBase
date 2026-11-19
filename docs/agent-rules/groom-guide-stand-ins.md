# Guide stand-ins keep their neighbours' drape (#1533)

Read before touching `ExpandGroomGuideDisplacements`, `BuildGroomGuideStandIns`, `GroomGuideStandInSet`
or `GroomGuideExpansion` in `Groom/GroomGuideInfluence.*`, `compute/GroomExpandGuides.comp`, a coat's
per-role guide budget (`GroomSimulationComponent::m_MaxGuides*`), or the simulation steps of
`GroomLodComponent`. The solver itself is [groom-guide-simulation.md](groom-guide-simulation.md)'s.

## The rules

1. **Most of a coat is stand-ins, so the stand-in blend decides the coat's drape.** A guide slot the
   budget does not simulate is published as a weighted blend of up to four simulated guides of its
   own group and role (#1509). At a full budget the long-coated horse simulates 1,118 of its 7,815
   slots and the showcase dog 1,472 of its 16,580. So 86% and 91% of those coats move by the blend,
   not by the solver.

2. **Take the direction from the linear blend, and the length from the neighbours where they
   agree.** A linear blend of displacements that point different ways averages away the part of
   each that points its own way. Two guides that sag the same distance 90 degrees apart give a
   stand-in 0.71 of that distance, so it sags less than either of them. `KeepNeighbourLength` keeps
   the blend's direction and takes the weighted mean of the neighbours' lengths. It fades that in by
   coherence, the blend's length over that mean length, which is cos(angle / 2) for two equal
   guides. The mean length is taken in full up to ~106 degrees apart and not at all past ~150.
   Where the neighbours cancel, at a part or a whorl, no direction is right, so the short linear
   blend stays.

3. **A budget change must not change the drape.** The LOD ladder halves the simulated guides with
   distance: 1,118, then 561, then 281 on the long coat. With the linear blend, each halving moved
   the stand-ins further from their neighbours' drape. The long coat's strand tier read 14% less
   shadowed at 14 m than the full coat: seen energy 1.156 against a +-10% contract. With the length
   kept it reads 1.039.

4. **Publish the stand-ins, not their expansion, and expand where the coat is read (#1589).** The
   producer publishes the simulated guides' points and a `GroomGuideStandInSet`, shared by `Ref` and
   rebuilt only when the budget changes. `GroomExpandGuides.comp` writes every slot's displacement
   into the deformation buffer, so only the simulated points cross the bus each frame, and the
   stand-ins cross once per budget step. A CPU reader (the CPU deformation path, the caster padding,
   the coat bake's drawn pose, the ray-traced proxy) expands them through `GroomGuideExpansion`,
   which calls `ExpandGroomGuideDisplacements` and so reads what the producer used to publish, bit
   for bit. The dense upload was about 120k points a fox at any budget, about 170 MiB a frame for the
   acceptance herd.

5. **A CPU reader of a GPU-expanded deformation buffer calls `ResolveCpuGuides` first.** The kernel's
   output is never read back, so the CPU twin's displacement records stay stale until a reader asks;
   `GroomDeformBuffer::Displacements()` asserts rather than sample every slot at rest. A
   `BuildGroomStrandMesh` or `MeasureGroomCasterPosePadding` handed stand-ins expands them itself.

6. **The kernel matches the CPU expansion within 1e-6 m + 1e-5 of the displacement, on both
   backends.** `GroomExpandGuidesParityTest` (GL) and the `VulkanPassSuite` tenant
   `GroomExpandGuidesKernelMatchesTheCpuExpansion` hold it, each with a broken-weight control.
   `OLO_GROOM_GUIDES_EXPANDED_ON_CPU` is the A/B: it expands on the CPU and sends every slot.

## How it was found

- **Print the budget before blaming it.** Print `GuidesSimulated` and the slot count at each LOD
  stop. Even the full-budget arm was mostly stand-ins.
- **A/B the blend, not the solver.** Simulated guides are independent of each other, so a smaller
  budget simulates the guides it keeps exactly as before, and only the stand-ins differ. Stand-ins
  that copy their nearest guide read 1.031 at 14 m, which passes and proves the cause, but it snaps
  neighbouring stand-ins to different guides. The length-keeping blend reads 1.039 and stays
  smooth.
- **Pinned by** `GroomGuideInfluence.AStandInBetweenDivergingGuidesSagsAsFarAsTheyDo`, which reads
  0.707 with the linear blend, and `OpposedGuidesLeaveTheLinearBlend`.
